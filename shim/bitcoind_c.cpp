#include "bitcoind_c.h"

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/init.h>
#include <common/system.h>
#include <init.h>
#include <interfaces/init.h>
#include <kernel/context.h>
#include <key.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <node/warnings.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <sync.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/signalinterrupt.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/signals2/connection.hpp>

#ifndef WIN32
#include <csignal>
#endif

// Every executable linking Core defines this; we never translate.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

// Our own Init and SignalInterrupt: InitContext() asserts its static g_shutdown is unset,
// so a second start in one process would abort.
class LogosInit : public interfaces::Init
{
public:
    explicit LogosInit(node::NodeContext& node) : m_node(node)
    {
        m_node.args = &gArgs;
        m_node.shutdown_signal = &m_interrupt;
        m_node.shutdown_request = [this] { return m_interrupt(); };
        m_node.init = this;
    }
    const char* exeName() override { return "bitcoind_c"; }

    util::SignalInterrupt m_interrupt;
    node::NodeContext& m_node;
};

std::mutex g_mutex; // serialises start and stop, and owns g_node, g_init and g_thread
std::atomic<int> g_state{BITCOIND_STOPPED};
std::thread g_thread;
std::unique_ptr<node::NodeContext> g_node;
std::unique_ptr<LogosInit> g_init;

std::mutex g_info_mutex; // short-lived: never held across Core calls
std::string g_error;
std::string g_init_message;
std::string g_network;
std::string g_data_dir;
std::string g_log_file;
std::string g_argv_json = "[]";
std::chrono::steady_clock::time_point g_started;

// RPC and flush calls hold the node open; the node thread closes it and drains them first.
std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
bool g_gate_open = false;
bool g_node_ready = false; // AppInitMain finished: node members are no longer being assigned
int g_in_flight = 0;
std::atomic<bool> g_closing{false};
std::atomic<bool> g_stop_requested{false}; // a stop we were asked for is never a failure

void set_error(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_info_mutex);
    g_error = msg;
}

// The first error of a run is the specific one; later ones are generic follow-ups.
void record_error(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_info_mutex);
    if (g_error.empty()) g_error = msg;
}

char* dup_c(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// noui's handlers, minus stderr, and recording errors for BITCOIND_last_error().
bool on_message(const bilingual_str& message, unsigned int style)
{
    const bool secure = style & CClientUIInterface::SECURE;
    style &= ~CClientUIInterface::SECURE;
    switch (style & CClientUIInterface::ICON_MASK) {
    case CClientUIInterface::ICON_ERROR:
        if (!secure) LogError("%s\n", message.original);
        record_error(message.original);
        break;
    case CClientUIInterface::ICON_WARNING:
        if (!secure) LogWarning("%s\n", message.original);
        break;
    default:
        if (!secure) LogInfo("%s\n", message.original);
    }
    return false;
}

bool on_question(const bilingual_str&, const std::string& message, unsigned int style)
{
    return on_message(Untranslated(message), style);
}

void on_init_message(const std::string& message)
{
    LogInfo("init message: %s", message);
    SetRPCWarmupStatus(message);
    std::lock_guard<std::mutex> lock(g_info_mutex);
    g_init_message = message;
}

// AppInitBasicSetup minus its signal handlers and new_handler, which would take over the host.
bool library_startup_once()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = SetupNetworking();
        uiInterface.ThreadSafeMessageBox_connect(on_message);
        uiInterface.ThreadSafeQuestion_connect(on_question);
        uiInterface.InitMessage_connect(on_init_message);
#ifndef WIN32
        // Core's sockets use MSG_NOSIGNAL/SO_NOSIGPIPE; anything else must not kill the host.
        struct sigaction old{};
        if (sigaction(SIGPIPE, nullptr, &old) == 0 && old.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
#endif
    });
    return ok;
}

class NodeCall
{
public:
    explicit NodeCall(bool need_ready)
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        if (!g_gate_open || (need_ready && !g_node_ready)) return;
        ++g_in_flight;
        m_node = g_node.get();
    }
    ~NodeCall()
    {
        if (!m_node) return;
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        --g_in_flight;
        g_gate_cv.notify_all();
    }
    NodeCall(const NodeCall&) = delete;
    NodeCall& operator=(const NodeCall&) = delete;
    node::NodeContext* get() const { return m_node; }

private:
    node::NodeContext* m_node = nullptr;
};

// Refuses new calls, wakes the ones that wait on the node, and waits for the rest.
void close_gate_and_drain(node::NodeContext& node)
{
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        g_gate_open = false;
        g_node_ready = false;
    }
    g_closing = true;
    Interrupt(node);
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    while (!g_gate_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_in_flight == 0; })) {
        LogInfo("bitcoind_c: waiting for %d in-process RPC call(s) to return", g_in_flight);
    }
}

void node_main()
{
    util::ThreadSetInternalName("init");
    node::NodeContext& node = *g_node;
    bool ok = false;
    try {
        InitLogging(gArgs);
        InitParameterInteraction(gArgs);
        {
            std::lock_guard<std::mutex> lock(g_info_mutex);
            g_log_file = LogInstance().m_print_to_file ? fs::PathToString(LogInstance().m_file_path) : "";
        }
        if (AppInitParameterInteraction(gArgs)) {
            node.warnings = std::make_unique<node::Warnings>();
            node.kernel = std::make_unique<kernel::Context>();
            node.ecc_context = std::make_unique<ECC_Context>();
            ok = AppInitSanityChecks(*node.kernel) && AppInitLockDirectories() &&
                 AppInitInterfaces(node) && AppInitMain(node);
        }
    } catch (const std::exception& e) {
        record_error(std::string("init: ") + e.what());
        ok = false;
    } catch (...) {
        record_error("init: unknown exception");
        ok = false;
    }
    if (!ok && node.exit_status == EXIT_SUCCESS) node.exit_status = EXIT_FAILURE;

    if (ok && !ShutdownRequested(node)) {
        {
            std::lock_guard<std::mutex> lock(g_gate_mutex);
            g_node_ready = true;
        }
        int expected = BITCOIND_STARTING;
        if (g_state.compare_exchange_strong(expected, BITCOIND_RUNNING)) {
            (void)node.shutdown_signal->wait();
        }
    }

    g_state = BITCOIND_STOPPING;
    close_gate_and_drain(node);
    try {
        Shutdown(node); // renames this thread, which is why it never runs on the caller's
    } catch (const std::exception& e) {
        record_error(std::string("shutdown: ") + e.what());
        node.exit_status = EXIT_FAILURE;
    }
    LogInstance().DisconnectTestLogger();
    ReleaseDirectoryLocks();
    SetRPCWarmupStarting();
    const bool failed = node.exit_status != EXIT_SUCCESS && !g_stop_requested;
    g_state = failed ? BITCOIND_FAILED : BITCOIND_STOPPED;
}

// With g_mutex held: waits for the node thread, then frees what the last run left.
void reap()
{
    if (g_thread.joinable()) g_thread.join();
    g_init.reset();
    g_node.reset();
}

// Core's -flag syntax only; a stray token would otherwise be ignored silently.
bool parse_argv(const char* argv_json, std::vector<std::string>& out, std::string& err)
{
    UniValue v;
    if (!argv_json || !v.read(argv_json) || !v.isArray()) {
        err = "argv must be a JSON array of strings";
        return false;
    }
    for (const UniValue& item : v.getValues()) {
        if (!item.isStr()) {
            err = "argv must be a JSON array of strings";
            return false;
        }
        const std::string& s = item.get_str();
        if (s.empty() || !IsSwitchChar(s[0])) {
            err = "unexpected token in argv: " + s;
            return false;
        }
        out.push_back(s);
    }
    return true;
}

UniValue error_obj(int code, const std::string& message)
{
    UniValue reply(UniValue::VOBJ);
    reply.pushKV("result", NullUniValue);
    reply.pushKV("error", JSONRPCError(code, message));
    return reply;
}

} // namespace

extern "C" {

int BITCOIND_start(const char* argv_json)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const int st = g_state.load();
    if (st == BITCOIND_STARTING || st == BITCOIND_RUNNING || st == BITCOIND_STOPPING) {
        set_error("a node is already running in this process");
        return 1;
    }
    reap();
    if (!library_startup_once()) {
        set_error("initializing networking failed");
        return 1;
    }
    {
        std::lock_guard<std::mutex> info(g_info_mutex);
        g_error.clear();
        g_init_message.clear();
        g_log_file.clear();
    }

    std::vector<std::string> args;
    std::string err;
    if (!parse_argv(argv_json, args, err)) {
        set_error(err);
        return 1;
    }

    // Process-wide state an earlier run leaves behind.
    gArgs.ClearArgs();
    gArgs.ClearPathCache();
    SetupServerArgs(gArgs, /*can_listen_ipc=*/false);
    // Defaults first: the last value wins, so a caller can still ask for Core's HTTP RPC server.
    std::vector<std::string> full{"bitcoind", "-server=0", "-printtoconsole=0"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    for (const auto& a : full) argv.push_back(a.c_str());
    if (!gArgs.ParseParameters(static_cast<int>(argv.size()), argv.data(), err)) {
        set_error("bad arguments: " + err);
        return 1;
    }
    if (auto cfg = common::InitConfig(gArgs)) {
        std::string msg = cfg->message.original;
        for (const auto& d : cfg->details) msg += "\n" + d;
        set_error(msg);
        return 1;
    }

    SetRPCWarmupStarting();
    SetRPCWarmupStatus("Starting");
    g_reachable_nets.Reset();
    ClearLocal();
    g_socks5_interrupt.reset(); // left set by the last stop, it fails every SOCKS5 connection
    ClearProxies();
    LogInstance().DisableCategory(BCLog::ALL);
    LogInstance().SetLogLevel(BCLog::DEFAULT_LOG_LEVEL);
    LogInstance().SetCategoryLogLevel({});

    g_node = std::make_unique<node::NodeContext>();
    g_init = std::make_unique<LogosInit>(*g_node);
    // -server=0 leaves this a no-op; ours lets a stop interrupt long RPC scans.
    g_node->rpc_interruption_point = [] {
        if (g_closing) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
    };
    {
        std::lock_guard<std::mutex> info(g_info_mutex);
        g_network = ChainTypeToString(gArgs.GetChainType());
        g_data_dir = fs::PathToString(gArgs.GetDataDirNet());
        UniValue a(UniValue::VARR);
        for (const auto& s : args) a.push_back(s);
        g_argv_json = a.write();
        g_started = std::chrono::steady_clock::now();
    }
    g_closing = false;
    g_stop_requested = false;
    {
        std::lock_guard<std::mutex> gate(g_gate_mutex);
        g_gate_open = true; // RPC answers "in warmup" until AppInitMain finishes
        g_node_ready = false;
    }
    g_state = BITCOIND_STARTING;
    try {
        g_thread = std::thread(node_main);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> gate(g_gate_mutex);
            g_gate_open = false;
        }
        g_state = BITCOIND_FAILED;
        set_error(std::string("cannot start the node thread: ") + e.what());
        g_init.reset();
        g_node.reset();
        return 1;
    }
    return 0;
}

void BITCOIND_stop(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    int st = g_state.load();
    if ((st == BITCOIND_STARTING || st == BITCOIND_RUNNING) && g_init) {
        g_stop_requested = true;
        while ((st == BITCOIND_STARTING || st == BITCOIND_RUNNING) &&
               !g_state.compare_exchange_weak(st, BITCOIND_STOPPING)) {}
        if (!g_init->m_interrupt()) record_error("could not signal the node to stop");
    }
    reap();
}

int BITCOIND_state(void) { return g_state.load(); }

const char* BITCOIND_status_json(void)
{
    const int st = g_state.load();
    const char* name = st == BITCOIND_STOPPED  ? "stopped"
                     : st == BITCOIND_STARTING ? "starting"
                     : st == BITCOIND_RUNNING  ? "running"
                     : st == BITCOIND_STOPPING ? "stopping"
                                               : "failed";
    UniValue o(UniValue::VOBJ);
    o.pushKV("state", name);
    std::lock_guard<std::mutex> lock(g_info_mutex);
    const bool live = st == BITCOIND_STARTING || st == BITCOIND_RUNNING || st == BITCOIND_STOPPING;
    const int64_t uptime = live ? std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now() - g_started).count()
                                : 0;
    o.pushKV("network", g_network);
    o.pushKV("dataDir", g_data_dir);
    o.pushKV("logFile", g_log_file);
    o.pushKV("uptimeSecs", uptime);
    o.pushKV("version", FormatFullVersion());
    o.pushKV("initMessage", g_init_message);
    o.pushKV("lastError", g_error);
    UniValue argv;
    if (!argv.read(g_argv_json)) argv = UniValue(UniValue::VARR);
    o.pushKV("argv", argv);
    return dup_c(o.write());
}

const char* BITCOIND_rpc(const char* method, const char* params_json)
{
    if (!method || !*method) return dup_c(error_obj(RPC_INVALID_REQUEST, "method is required").write());
    UniValue params(UniValue::VARR);
    if (params_json && *params_json) {
        if (!params.read(params_json) || !(params.isArray() || params.isObject())) {
            return dup_c(error_obj(RPC_PARSE_ERROR, "params must be a JSON array or object").write());
        }
    }
    NodeCall call(/*need_ready=*/false);
    if (!call.get()) return dup_c(error_obj(RPC_CLIENT_NOT_CONNECTED, "node is not running").write());

    JSONRPCRequest req;
    req.context = call.get();
    req.strMethod = method;
    req.params = std::move(params);
    UniValue reply(UniValue::VOBJ);
    try {
        UniValue result = tableRPC.execute(req);
        reply.pushKV("result", std::move(result));
        reply.pushKV("error", NullUniValue);
    } catch (const UniValue& e) {
        reply.pushKV("result", NullUniValue);
        reply.pushKV("error", e);
    } catch (const std::exception& e) {
        reply = error_obj(RPC_MISC_ERROR, e.what());
    }
    return dup_c(reply.write());
}

int BITCOIND_flush(void)
{
    NodeCall call(/*need_ready=*/true);
    if (!call.get() || !call.get()->chainman) {
        set_error("node is not running");
        return 1;
    }
    try {
        ChainstateManager& chainman = *call.get()->chainman;
        LOCK(cs_main);
        for (const auto& chainstate : chainman.m_chainstates) {
            if (chainstate->CanFlushToDisk()) chainstate->ForceFlushStateToDisk(/*wipe_cache=*/false);
        }
    } catch (const std::exception& e) {
        set_error(std::string("flush: ") + e.what());
        return 1;
    }
    return 0;
}

const char* BITCOIND_last_error(void)
{
    std::lock_guard<std::mutex> lock(g_info_mutex);
    return dup_c(g_error);
}

void BITCOIND_free(const char* s) { std::free(const_cast<char*>(s)); }

const char* BITCOIND_version(void) { return dup_c(FormatFullVersion()); }

} // extern "C"
