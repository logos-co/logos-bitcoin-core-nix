// Drives libbitcoind_c through its C ABI only: start/stop cycles in one process, a network
// switch, in-process RPC, and optionally signet over a Tor SOCKS proxy.
// Usage: bitcoind-c-test <workdir> [--tor[=host:port]] [--tor-cycles n] [--peer-timeout secs]
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L // getaddrinfo, clock_gettime and nanosleep under -std=c11
#endif

#include "bitcoind_c.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++g_failures;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            fflush(stdout);                                    \
        }                                                      \
    } while (0)

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static void mkdir_p(const char* path)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(buf);
#else
        mkdir(buf, 0700);
#endif
        *p = c;
    }
#ifdef _WIN32
    _mkdir(buf);
#else
    mkdir(buf, 0700);
#endif
}

static char* take(const char* s)
{
    const char* v = s ? s : "";
    char* out = malloc(strlen(v) + 1);
    strcpy(out, v);
    BITCOIND_free(s);
    return out;
}

static char* rpc(const char* method, const char* params) { return take(BITCOIND_rpc(method, params)); }

static int ok_reply(const char* reply) { return strstr(reply, "\"error\":null") != NULL; }

// Core writes compact JSON, so "key": is followed directly by the value.
static long long json_int(const char* json, const char* key, long long def)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(json, pat);
    if (!p) return def;
    p += strlen(pat);
    char* end;
    const long long v = strtoll(p, &end, 10);
    return end == p ? def : v;
}

static int json_str(const char* json, const char* key, char* out, size_t n)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    const char* e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n) return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}

// Paths go into a JSON string; Windows paths carry backslashes.
static void json_escape(const char* in, char* out, size_t n)
{
    size_t j = 0;
    for (; *in && j + 2 < n; ++in) {
        if (*in == '\\' || *in == '"') out[j++] = '\\';
        out[j++] = *in;
    }
    out[j] = 0;
}

static const char* state_name(int st)
{
    switch (st) {
    case BITCOIND_STOPPED: return "stopped";
    case BITCOIND_STARTING: return "starting";
    case BITCOIND_RUNNING: return "running";
    case BITCOIND_STOPPING: return "stopping";
    default: return "failed";
    }
}

static void print_last_error(void)
{
    char* e = take(BITCOIND_last_error());
    printf("     last error: %s\n", e);
    free(e);
}

// Starts the node and waits for RUNNING. Returns the milliseconds taken, or -1.
static long long start_and_wait(const char* argv_json)
{
    const int64_t t0 = now_ms();
    if (BITCOIND_start(argv_json) != 0) {
        printf("     start refused\n");
        print_last_error();
        return -1;
    }
    while (now_ms() - t0 < 300000) {
        const int st = BITCOIND_state();
        if (st == BITCOIND_RUNNING) return now_ms() - t0;
        if (st == BITCOIND_FAILED || st == BITCOIND_STOPPED) {
            printf("     node went %s while starting\n", state_name(st));
            print_last_error();
            return -1;
        }
        sleep_ms(20);
    }
    printf("     node not running after 300 s\n");
    return -1;
}

static long long stop_timed(void)
{
    const int64_t t0 = now_ms();
    BITCOIND_stop();
    return now_ms() - t0;
}

static void regtest_argv(const char* dir, char* out, size_t n)
{
    char esc[4096];
    json_escape(dir, esc, sizeof esc);
    // The module's defaults that apply offline: pruned, filters, no wallet, no listener.
    snprintf(out, n,
             "[\"-regtest\",\"-datadir=%s\",\"-prune=550\",\"-blockfilterindex=1\",\"-disablewallet\","
             "\"-persistmempool=0\",\"-listen=0\",\"-connect=0\"]",
             esc);
}

// One regtest cycle: mine 10 blocks on top of what the last cycle left, then read them back.
static void regtest_cycle(int cycle, const char* argv)
{
    const long long up = start_and_wait(argv);
    CHECK(up >= 0, "cycle %d: node did not reach running", cycle);
    if (up < 0) {
        BITCOIND_stop();
        return;
    }
    char* info = rpc("getblockchaininfo", NULL);
    char chain[32] = "";
    json_str(info, "chain", chain, sizeof chain);
    const long long before = json_int(info, "blocks", -1);
    CHECK(ok_reply(info) && strcmp(chain, "regtest") == 0, "cycle %d: getblockchaininfo: %.300s", cycle, info);
    CHECK(before == 10LL * (cycle - 1), "cycle %d: started at height %lld, want %d", cycle, before, 10 * (cycle - 1));
    free(info);

    char* mined = rpc("generatetodescriptor", "[10,\"raw(51)\"]");
    CHECK(ok_reply(mined), "cycle %d: generatetodescriptor: %.300s", cycle, mined);
    free(mined);

    char params[64];
    snprintf(params, sizeof params, "[%d]", 10 * cycle);
    char* hash_reply = rpc("getblockhash", params);
    char hash[80] = "";
    json_str(hash_reply, "result", hash, sizeof hash);
    CHECK(ok_reply(hash_reply) && strlen(hash) == 64, "cycle %d: getblockhash: %.300s", cycle, hash_reply);
    free(hash_reply);

    char bparams[128];
    snprintf(bparams, sizeof bparams, "[\"%s\",1]", hash);
    char* block = rpc("getblock", bparams);
    CHECK(ok_reply(block) && json_int(block, "height", -1) == 10LL * cycle, "cycle %d: getblock: %.300s", cycle, block);
    free(block);

    // The filter index catches up in the background.
    int filtered = 0;
    char fparams[128];
    snprintf(fparams, sizeof fparams, "[\"%s\"]", hash);
    for (int i = 0; i < 100 && !filtered; ++i) {
        char* f = rpc("getblockfilter", fparams);
        filtered = ok_reply(f) && strstr(f, "\"filter\":\"") != NULL;
        free(f);
        if (!filtered) sleep_ms(50);
    }
    CHECK(filtered, "cycle %d: getblockfilter never answered for height %d", cycle, 10 * cycle);

    char* bad = rpc("getblockhash", "[100000]");
    CHECK(!ok_reply(bad) && json_int(bad, "code", 0) == -8, "cycle %d: out-of-range getblockhash: %.200s", cycle, bad);
    free(bad);

    CHECK(BITCOIND_flush() == 0, "cycle %d: flush failed", cycle);
    CHECK(BITCOIND_start(argv) != 0, "cycle %d: a second start while running was accepted", cycle);

    const long long down = stop_timed();
    CHECK(BITCOIND_state() == BITCOIND_STOPPED, "cycle %d: state after stop is %s", cycle, state_name(BITCOIND_state()));
    printf("ok   regtest cycle %d: running in %lld ms, height %lld -> %d, getblock/getblockfilter ok, stopped in %lld ms\n",
           cycle, up, before, 10 * cycle, down);
    fflush(stdout);
}

static int tcp_reachable(const char* host, const char* port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return 0;
    int ok = 0;
    for (struct addrinfo* a = res; a && !ok; a = a->ai_next) {
#ifdef _WIN32
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        ok = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
        closesocket(s);
#else
        int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        ok = connect(s, a->ai_addr, a->ai_addrlen) == 0;
        close(s);
#endif
    }
    freeaddrinfo(res);
    return ok;
}

static int free_port(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001UL);
    socklen_t len = sizeof a;
    int port = 0;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    if (bind(s, (struct sockaddr*)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
    closesocket(s);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    if (bind(s, (struct sockaddr*)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
    close(s);
#endif
    return port;
}

static void base64(const char* in, char* out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t n = strlen(in);
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | (i + 1 < n ? (unsigned char)in[i + 1] << 8 : 0) |
                           (i + 2 < n ? (unsigned char)in[i + 2] : 0);
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[j] = 0;
}

// One JSON-RPC call to Core's HTTP server. Returns the HTTP status, or -1.
static int http_rpc(int port, const char* user_pass, const char* method, char* body, size_t n)
{
    char auth[512], payload[256], req[2048];
    base64(user_pass, auth);
    snprintf(payload, sizeof payload, "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"%s\",\"params\":[]}", method);
    const int len = snprintf(req, sizeof req,
                             "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic %s\r\n"
                             "Content-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                             auth, (unsigned)strlen(payload), payload);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001UL);
    a.sin_port = htons((unsigned short)port);
    char buf[65536];
    int got = 0;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr*)&a, sizeof a) != 0) { if (s != INVALID_SOCKET) closesocket(s); return -1; }
    send(s, req, len, 0);
    for (int r; got < (int)sizeof buf - 1 && (r = recv(s, buf + got, (int)sizeof buf - 1 - got, 0)) > 0;) got += r;
    closesocket(s);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr*)&a, sizeof a) != 0) { if (s >= 0) close(s); return -1; }
    if (send(s, req, (size_t)len, 0) != len) { close(s); return -1; }
    for (ssize_t r; got < (int)sizeof buf - 1 && (r = recv(s, buf + got, sizeof buf - 1 - (size_t)got, 0)) > 0;) got += (int)r;
    close(s);
#endif
    buf[got] = 0;
    int status = -1;
    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
    const char* b = strstr(buf, "\r\n\r\n");
    snprintf(body, n, "%s", b ? b + 4 : "");
    return status;
}

static int read_cookie(const char* path, char* out, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t r = fread(out, 1, n - 1, f);
    fclose(f);
    out[r] = 0;
    while (r && (out[strlen(out) - 1] == '\n' || out[strlen(out) - 1] == '\r')) out[strlen(out) - 1] = 0;
    return out[0] != 0;
}

// A stop while Core's addnode thread waits on connect() to unreachable addresses (patch 0005):
// without it, Shutdown waited out -timeout (5 s). Offline in a sandbox the connects fail at once.
static void stop_with_connect_in_flight(const char* workdir)
{
    char dir[4096], esc[4096], argv[8192];
    snprintf(dir, sizeof dir, "%s/regtest-blackhole", workdir);
    mkdir_p(dir);
    json_escape(dir, esc, sizeof esc);
    snprintf(argv, sizeof argv,
             "[\"-regtest\",\"-datadir=%s\",\"-listen=0\",\"-connect=0\",\"-addnode=192.0.2.1:18444\","
             "\"-addnode=198.51.100.1:18444\",\"-addnode=203.0.113.1:18444\"]",
             esc);
    for (int c = 1; c <= 3; ++c) {
        const long long up = start_and_wait(argv);
        CHECK(up >= 0, "blackhole cycle %d: node did not reach running", c);
        if (up < 0) { BITCOIND_stop(); continue; }
        sleep_ms(1500);
        const long long down = stop_timed();
        CHECK(down < 2000, "blackhole cycle %d: stop with a connect in flight took %lld ms", c, down);
        printf("%s stop with connects to blackholed addnodes in flight, cycle %d: %lld ms\n", down < 2000 ? "ok  " : "slow", c, down);
    }
}

// Core's HTTP RPC server twice in one process: each start must issue its own cookie and apply
// only its own -rpcwhitelist (patches 0003 and 0004).
static void rpc_server_cycles(const char* workdir)
{
    char dir[4096], esc[4096], argv[8192], cookie_path[4200], first_cookie[512] = "";
    snprintf(dir, sizeof dir, "%s/regtest-rpc", workdir);
    mkdir_p(dir);
    json_escape(dir, esc, sizeof esc);
    snprintf(cookie_path, sizeof cookie_path, "%s/regtest/.cookie", dir);
    const char* allowed[2] = {"getblockchaininfo", "getblockcount"};
    for (int c = 0; c < 2; ++c) {
        const int port = free_port();
        snprintf(argv, sizeof argv,
                 "[\"-regtest\",\"-datadir=%s\",\"-listen=0\",\"-connect=0\",\"-server=1\",\"-rpcbind=127.0.0.1\","
                 "\"-rpcallowip=127.0.0.1\",\"-rpcport=%d\",\"-rpcwhitelist=__cookie__:%s\",\"-rpcwhitelistdefault=1\"]",
                 esc, port, allowed[c]);
        const long long up = start_and_wait(argv);
        CHECK(up >= 0, "rpc server cycle %d: node did not reach running", c + 1);
        if (up < 0) { BITCOIND_stop(); continue; }
        char cookie[512] = "", body[65536];
        CHECK(read_cookie(cookie_path, cookie, sizeof cookie), "rpc server cycle %d: no cookie at %s", c + 1, cookie_path);
        const int ok_status = http_rpc(port, cookie, allowed[c], body, sizeof body);
        CHECK(ok_status == 200 && strstr(body, "\"error\":null") != NULL, "rpc server cycle %d: %s -> %d %.200s", c + 1, allowed[c], ok_status, body);
        const int denied = http_rpc(port, cookie, allowed[1 - c], body, sizeof body);
        CHECK(denied == 403, "rpc server cycle %d: %s outside the whitelist -> %d", c + 1, allowed[1 - c], denied);
        int stale = 0;
        if (c == 0) {
            snprintf(first_cookie, sizeof first_cookie, "%s", cookie);
        } else {
            stale = http_rpc(port, first_cookie, allowed[c], body, sizeof body);
            CHECK(stale == 401 && strcmp(first_cookie, cookie) != 0, "rpc server cycle 2: the first cycle's cookie -> %d", stale);
        }
        const long long down = stop_timed();
        printf("ok   rpc server cycle %d: port %d, %s -> %d, %s -> %d%s, stopped in %lld ms\n", c + 1, port, allowed[c],
               ok_status, allowed[1 - c], denied, c ? (stale == 401 ? ", old cookie -> 401" : ", old cookie accepted") : "", down);
    }
}

// Signet over Tor only: after a restart, a stale SOCKS5 interrupt would fail every connection.
static void tor_cycles(const char* workdir, const char* proxy, int cycles, int peer_timeout_s)
{
    char host[256], port[16] = "9050";
    snprintf(host, sizeof host, "%s", proxy);
    char* colon = strrchr(host, ':');
    if (colon) {
        snprintf(port, sizeof port, "%s", colon + 1);
        *colon = 0;
    }
    if (!tcp_reachable(host, port)) {
        printf("skip tor: no SOCKS proxy reachable at %s:%s\n", host, port);
        return;
    }
    char dir[4096], esc[4096], argv[8192];
    snprintf(dir, sizeof dir, "%s/signet-tor", workdir);
    mkdir_p(dir);
    json_escape(dir, esc, sizeof esc);
    snprintf(argv, sizeof argv,
             "[\"-signet\",\"-datadir=%s\",\"-prune=550\",\"-blockfilterindex=1\",\"-disablewallet\","
             "\"-persistmempool=0\",\"-listen=0\",\"-onlynet=onion\",\"-proxy=%s:%s\",\"-privatebroadcast\"]",
             esc, host, port);
    int reached = 0;
    for (int c = 1; c <= cycles; ++c) {
        const long long up = start_and_wait(argv);
        CHECK(up >= 0, "tor cycle %d: node did not reach running", c);
        if (up < 0) {
            BITCOIND_stop();
            continue;
        }
        const int64_t t0 = now_ms();
        long long peers = 0, height = -1;
        while (now_ms() - t0 < (int64_t)peer_timeout_s * 1000) {
            char* net = rpc("getnetworkinfo", NULL);
            peers = json_int(net, "connections_out", 0);
            free(net);
            if (peers > 0) break;
            sleep_ms(500);
        }
        const long long waited = now_ms() - t0;
        char* info = rpc("getblockchaininfo", NULL);
        height = json_int(info, "headers", -1);
        free(info);
        const long long down = stop_timed();
        if (peers > 0) ++reached;
        printf("%s tor cycle %d: %s after %lld ms (%lld outbound, headers %lld), stopped in %lld ms\n",
               peers > 0 ? "ok  " : "warn", c, peers > 0 ? "reached a peer" : "no peer", waited, peers, height, down);
        fflush(stdout);
    }
    printf("tor: %d of %d cycles reached a peer through %s:%s\n", reached, cycles, host, port);
    CHECK(reached == cycles, "tor: only %d of %d cycles reached a peer", reached, cycles);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <workdir> [--tor[=host:port]] [--tor-cycles n] [--peer-timeout secs]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char* workdir = argv[1];
    const char* tor = NULL;
    int tor_cycles_n = 3, peer_timeout = 240;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--tor") == 0) tor = "127.0.0.1:9050";
        else if (strncmp(argv[i], "--tor=", 6) == 0) tor = argv[i] + 6;
        else if (strcmp(argv[i], "--tor-cycles") == 0 && i + 1 < argc) tor_cycles_n = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peer-timeout") == 0 && i + 1 < argc) peer_timeout = atoi(argv[++i]);
    }
    mkdir_p(workdir);

    char* version = take(BITCOIND_version());
    printf("libbitcoind_c %s\n", version);
    free(version);

    // Before any start: refusals, not crashes.
    CHECK(BITCOIND_state() == BITCOIND_STOPPED, "fresh library state is %s", state_name(BITCOIND_state()));
    char* early = rpc("getblockchaininfo", NULL);
    CHECK(!ok_reply(early) && json_int(early, "code", 0) == -9, "rpc before start: %.200s", early);
    free(early);
    CHECK(BITCOIND_flush() != 0, "flush before start succeeded");
    CHECK(BITCOIND_start("{\"not\":\"an array\"}") != 0, "a non-array argv was accepted");
    CHECK(BITCOIND_start("[\"-regtest\",\"stray\"]") != 0, "a loose token in argv was accepted");
    CHECK(BITCOIND_start("[\"-regtest\",\"-datadir=/nonexistent/bitcoind-c-test\"]") != 0, "a missing datadir was accepted");
    CHECK(BITCOIND_state() == BITCOIND_STOPPED, "state after refusals is %s", state_name(BITCOIND_state()));
    char* status = take(BITCOIND_status_json());
    CHECK(strstr(status, "\"state\":\"stopped\"") != NULL, "status_json: %.300s", status);
    free(status);
    printf("ok   refusals before start\n");

    char rdir[4096], rargv[8192];
    snprintf(rdir, sizeof rdir, "%s/regtest", workdir);
    mkdir_p(rdir);
    regtest_argv(rdir, rargv, sizeof rargv);
    for (int c = 1; c <= 3; ++c) regtest_cycle(c, rargv);

    // Chain parameters are replaced only between runs: regtest -> signet -> regtest.
    char sdir[4096], sesc[4096], sargv[8192];
    snprintf(sdir, sizeof sdir, "%s/signet", workdir);
    mkdir_p(sdir);
    json_escape(sdir, sesc, sizeof sesc);
    snprintf(sargv, sizeof sargv,
             "[\"-signet\",\"-datadir=%s\",\"-prune=550\",\"-blockfilterindex=1\",\"-disablewallet\","
             "\"-persistmempool=0\",\"-listen=0\",\"-connect=0\"]",
             sesc);
    const long long sup = start_and_wait(sargv);
    CHECK(sup >= 0, "signet: node did not reach running");
    if (sup >= 0) {
        char* info = rpc("getblockchaininfo", NULL);
        char chain[32] = "";
        json_str(info, "chain", chain, sizeof chain);
        CHECK(strcmp(chain, "signet") == 0, "signet: getblockchaininfo: %.300s", info);
        free(info);
        char* genesis = rpc("getblockhash", "[0]");
        CHECK(strstr(genesis, "00000008819873e925422c1ff0f99f7cc9bbb232af63a077a480a3633bee1ef6") != NULL,
              "signet: genesis hash: %.200s", genesis);
        free(genesis);
    }
    const long long sdown = stop_timed();
    const long long rup = start_and_wait(rargv);
    CHECK(rup >= 0, "regtest after signet: node did not reach running");
    long long rheight = -1;
    if (rup >= 0) {
        char* info = rpc("getblockchaininfo", NULL);
        char chain[32] = "";
        json_str(info, "chain", chain, sizeof chain);
        rheight = json_int(info, "blocks", -1);
        CHECK(strcmp(chain, "regtest") == 0 && rheight == 30, "regtest after signet: %.300s", info);
        free(info);
    }
    const long long rdown = stop_timed();
    printf("ok   switch regtest -> signet (running in %lld ms, stopped in %lld ms) -> regtest (height %lld, running in %lld ms, stopped in %lld ms)\n",
           sup, sdown, rheight, rup, rdown);

    rpc_server_cycles(workdir);
    stop_with_connect_in_flight(workdir);

    if (tor) tor_cycles(workdir, tor, tor_cycles_n, peer_timeout);

    char* after = rpc("getblockchaininfo", NULL);
    CHECK(json_int(after, "code", 0) == -9, "rpc after the last stop: %.200s", after);
    free(after);

    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
