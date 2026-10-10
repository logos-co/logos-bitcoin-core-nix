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

Targets: `aarch64-darwin`, `x86_64-darwin`, `aarch64-linux`, `x86_64-linux`, and
`x86_64-windows`, a cross build from x86_64-linux with logos-nix's MinGW (GCC, mcf threads,
UCRT). The Windows DLL links GCC's runtime and mcfgthread statically, so it imports only
Windows' own DLLs; `packages.x86_64-windows.bitcoind-c-test` is the harness beside it.

## The C ABI

`shim/bitcoind_c.h`, modelled on logos-monero-nix's `monerod_c.h`. Every `const char*` it
returns is owned by the caller and released with `BITCOIND_free`.

| Function | |
|---|---|
| `BITCOIND_start(argv_json)` | Parses a JSON array of bitcoind flags and starts the node on its own thread. `-server=0` and `-printtoconsole=0` are the defaults; `-server=1` opens Core's HTTP RPC server |
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

Four patches, each held to an allowlist that `nix/bitcoin-core-src.nix` checks at build time:

| Patch | Files | Why |
|---|---|---|
| 0001 | `src/init.cpp` | Reset `g_local_services` and `g_enabled_filter_types` at the start of `AppInitParameterInteraction`. Both only accumulate, so a node restarted with pruning would keep advertising full blocks |
| 0002 | `src/netbase.cpp`, `src/netbase.h` | `ClearProxies()`: `SetProxy` refuses an invalid proxy, so nothing could unset an earlier run's `-proxy` |
| 0003 | `src/rpc/server.cpp` | Only for `-server=1`: `InterruptRPC`/`StopRPC` ran once per process through static `std::once_flag`s, so a second stop aborted on `StopRPC`'s assert. Now once per start |
| 0004 | `src/httprpc.cpp` | Only for `-server=1`: `g_rpcauth` and `g_rpc_whitelist` only grew, so a restarted server accepted the previous cookie and intersected the previous whitelists |

Consensus, validation, P2P code and the RPC methods are upstream's.

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
`getblock` and `getblockfilter`, a network switch, two `-server=1` cycles that call Core's HTTP
server with each start's cookie and `-rpcwhitelist` (and the previous cookie, which must fail),
and with `--tor[=host:port]` signet cycles through the SOCKS proxy that report whether each
cycle reached a peer. On Windows, copy `packages.x86_64-windows.bitcoind-c-test`'s `bin/` over
and run `bitcoind-c-test.exe <workdir>`.

## Verified, 2026-10-10

| Target | Where | Exports | Harness |
|---|---|---|---|
| aarch64-darwin | nix on an M-series Mac | 9, cctools `nm` | PASS: three regtest cycles in 72–479 ms to running and 116–285 ms to stop; regtest → signet → regtest; two `-server=1` cycles; Tor through 127.0.0.1:9050, 3 of 3 cycles reached a peer (7, 41 and 26 s) |
| x86_64-linux | nix on framework.lan, sandboxed | 9, `nm -D` | PASS: cycles in 20–141 ms to running and 55–107 ms to stop |
| x86_64-windows | cross from x86_64-linux, run on Windows 10.0.26200 | 9; imports only Windows' DLLs and the UCRT | PASS: cycles in 63–203 ms and 63–125 ms; two `-server=1` cycles; Tor via `tor.exe`, 3 of 3 cycles reached a peer (27, 55 and 7 s) |

The same Tor cycles with `g_socks5_interrupt.reset()` removed reached no peer in 2 of 2 cycles
(150 s each), the first of them already after an earlier stop in the process.

The macOS library is 10 MB, and its runtime closure is itself plus libc++; the Windows DLL is
17.5 MB with GCC's runtime inside. x86_64-darwin and aarch64-linux evaluate but have not been
built: there was no builder for them.

## Licence

Bitcoin Core is MIT (`lib/COPYING.bitcoin`). LevelDB, secp256k1, crc32c, minisketch and
libevent notices are collected in `lib/NOTICES.bitcoind_c`.
