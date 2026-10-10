# logos-bitcoin-core-nix

Bitcoin Core's node, built as a shared library with a C ABI, so a Logos module can run it
in-process instead of supervising a `bitcoind` executable.

```
packages.<target>.bitcoin-core-src   # Bitcoin Core v31.1 (tag, by hash) + patches/bitcoin
packages.<target>.libbitcoind_c      # libbitcoind_c: the node behind the C ABI in shim/bitcoind_c.h
packages.<target>.libevent           # libevent 2.1.12 as Core's depends builds it, static
packages.<target>.bitcoind-c-test    # the C test harness
checks.<system>.harness              # runs the harness offline against the library
```

Targets: `aarch64-darwin`, `x86_64-darwin`, `aarch64-linux`, `x86_64-linux`.

## The C ABI

`shim/bitcoind_c.h`, modelled on logos-monero-nix's `monerod_c.h`. Every `const char*` it
returns is owned by the caller and released with `BITCOIND_free`.

| Function | |
|---|---|
| `BITCOIND_start(argv_json)` | Parses a JSON array of bitcoind flags and starts the node on its own thread. `-server=0` and `-printtoconsole=0` always apply |
| `BITCOIND_stop()` | Signals the node and waits for its shutdown. Idempotent, any thread |
| `BITCOIND_state()` | `STOPPED`, `STARTING`, `RUNNING`, `STOPPING` or `FAILED` |
| `BITCOIND_status_json()` | State, network, data dir, log file, uptime, version, init message, last error |
| `BITCOIND_rpc(method, params_json)` | Runs `tableRPC.execute` in-process, as bitcoin-qt's console does. Returns Core's reply, `{"result":…,"error":null}` or `{"result":null,"error":{"code":…,"message":…}}` |
| `BITCOIND_flush()` | `ForceFlushStateToDisk(false)`: writes the UTXO cache and keeps it |
| `BITCOIND_last_error()`, `BITCOIND_free()`, `BITCOIND_version()` | |

There is no HTTP server and no port: RPC is a function call. Core registers every RPC
command whether or not `-server` is set, and until init finishes `BITCOIND_rpc` answers
Core's own "in warmup" error with the current init message.

## Running Core twice in one process

Upstream runs `AppInitMain` and `Shutdown` once per process; its only in-process test runs
one cycle. The wrapper (`shim/bitcoind_c.cpp`) makes a stopped node startable again:

- Its own `interfaces::Init` and `SignalInterrupt`, not `InitContext()`, whose static
  `g_shutdown` asserts on a second call, and a fresh `NodeContext` per start.
- It skips `AppInitBasicSetup`, which installs SIGTERM/SIGINT/SIGHUP handlers and a
  `new_handler` that turns any allocation failure in the process into `std::terminate`, and
  calls `SetupNetworking()` instead.
- Before each start: `gArgs.ClearArgs()`, `ClearPathCache()`, `SetupServerArgs`,
  `ParseParameters`, `InitConfig`; then `SetRPCWarmupStarting()`,
  `g_reachable_nets.Reset()`, `ClearLocal()`, `g_socks5_interrupt.reset()` (left set by the
  previous stop, it fails every SOCKS5 connection, so Tor would be dead after a restart),
  `ClearProxies()`, and the logger's categories and levels.
- Stop: signal, the init thread returns, `Interrupt`, wait for in-process RPC calls to
  return, `Shutdown`, `DisconnectTestLogger()`, `ReleaseDirectoryLocks()`. `Shutdown` renames
  its thread, so it runs on the node's own thread, never the caller's.

Two patches, each held to an allowlist that `nix/bitcoin-core-src.nix` checks at build time:

| Patch | Files | Why |
|---|---|---|
| 0001 | `src/init.cpp` | Reset `g_local_services` and `g_enabled_filter_types` at the start of `AppInitParameterInteraction`. Both only accumulate, so a node restarted with pruning would keep advertising full blocks |
| 0002 | `src/netbase.cpp`, `src/netbase.h` | `ClearProxies()`: `SetProxy` refuses an invalid proxy, so nothing could unset an earlier run's `-proxy` |

Consensus, validation, P2P and RPC code are upstream's.

## The library

Core's own CMake builds the node as the static library `bitcoin_node`. `shim/CMakeLists.txt`
adds Core's tree with `add_subdirectory(... EXCLUDE_FROM_ALL)` and builds only
`bitcoind_c`: no wallet, GUI, ZMQ, IPC, external signer or executables. Its dependencies are
Boost's headers and libevent, which is built static from Core's depends recipe.

- `-fvisibility=hidden` is set globally before `add_subdirectory`. As a target property it
  broke `dynamic_cast` in the Monero work (hidden typeinfo on one side, default on the other).
- Exports are an export list on macOS, a version script on Linux and a `.def` on Windows;
  `ci/check-exports.sh` checks that the library exports exactly the header's functions
  (cctools `nm` on macOS, `nm -D` on Linux).
- `-ffile-prefix-map` keeps source and header paths out of `__FILE__`, and so out of every
  consumer's runtime closure.

## Testing

```bash
nix build .#checks.aarch64-darwin.harness -L   # offline: 3 regtest cycles + regtest -> signet -> regtest
nix run .#bitcoind-c-test -- /tmp/bt --tor     # adds signet over Tor at 127.0.0.1:9050, if reachable
```

The harness drives only the C ABI: refusals before a start, three start/stop cycles on
regtest that mine blocks and read them back with `getblockchaininfo`, `getblockhash`,
`getblock` and `getblockfilter`, a network switch, and with `--tor[=host:port]` signet cycles
through the SOCKS proxy that report whether each cycle reached a peer.

## Licence

Bitcoin Core is MIT (`lib/COPYING.bitcoin`). LevelDB, secp256k1, crc32c, minisketch and
libevent notices are collected in `lib/NOTICES.bitcoind_c`.
