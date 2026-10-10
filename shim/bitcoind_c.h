// C ABI over Bitcoin Core's node: bitcoind, in-process, as a shared library.
// Every returned `const char*` is owned by the caller; release it with BITCOIND_free().
#ifndef LOGOS_BITCOIND_C_H
#define LOGOS_BITCOIND_C_H

#ifdef __cplusplus
extern "C" {
#endif

// Hidden visibility would otherwise hide the API itself.
#ifndef BITCOIND_API
#  if defined(_WIN32) || defined(__CYGWIN__)
#    ifdef BITCOIND_C_BUILDING
#      define BITCOIND_API __declspec(dllexport)
#    else
#      define BITCOIND_API __declspec(dllimport)
#    endif
#  else
#    define BITCOIND_API __attribute__((visibility("default")))
#  endif
#endif

enum BITCOIND_State {
  BITCOIND_STOPPED  = 0,
  BITCOIND_STARTING = 1,
  BITCOIND_RUNNING  = 2,
  BITCOIND_STOPPING = 3,
  BITCOIND_FAILED   = 4
};

// Parses `argv_json`, a JSON array of bitcoind flags, then starts the node on its own thread
// and returns. -server=0 and -printtoconsole=0 are the defaults; -server=1 opens Core's HTTP
// RPC server. Non-zero means refused; see BITCOIND_last_error(). One node per process; a
// stopped node can be started again.
BITCOIND_API int BITCOIND_start(const char* argv_json);

// Signals the node, waits for its shutdown and releases it. Idempotent; any thread.
BITCOIND_API void BITCOIND_stop(void);

BITCOIND_API int BITCOIND_state(void);

// State, network, data dir, log file, uptime, version, init message and last error as JSON.
// Chain state (height, peers) comes from BITCOIND_rpc instead.
BITCOIND_API const char* BITCOIND_status_json(void);

// Runs one RPC method in-process, as bitcoin-qt's console does; there is no HTTP server.
// `params_json` is a JSON array or object, or NULL for none. Returns Core's reply:
// {"result":<value>,"error":null} or {"result":null,"error":{"code":<n>,"message":<s>}}.
BITCOIND_API const char* BITCOIND_rpc(const char* method, const char* params_json);

// Writes the UTXO cache to disk and keeps it in memory. Zero on success.
BITCOIND_API int BITCOIND_flush(void);

BITCOIND_API const char* BITCOIND_last_error(void);
BITCOIND_API void BITCOIND_free(const char* s);
BITCOIND_API const char* BITCOIND_version(void);

#ifdef __cplusplus
}
#endif

#endif // LOGOS_BITCOIND_C_H
