# xmr-rebuild — an XMRig reconstruction study

This folder is a **clean-room-style reconstruction** of the four subsystems that
matter most in XMRig. It is independent of the JumpTask checklist app that was
in the workspace root; nothing there was modified.

The goal is to understand, document and re-implement the *structure and data
flow* of a RandomX CPU miner, not to copy XMRig's source. The RandomX VM core
itself is the official [tevador/RandomX](https://github.com/tevador/RandomX)
library — XMRig also consumes an (optimized, vendored) RandomX library rather
than re-deriving the algorithm, so this is the faithful architectural choice.

## The four modules

| Module | Code | Research notes |
|---|---|---|
| RandomX VM engine | `include/xmr/randomx_engine.hpp`, `src/randomx_engine.cpp` | [docs/RANDOMX_VM.md](docs/RANDOMX_VM.md) |
| Memory manager | `include/xmr/memory_manager.hpp`, `src/memory_manager.cpp` | [docs/MEMORY_MANAGER.md](docs/MEMORY_MANAGER.md) |
| Hardware optimizer | `include/xmr/hardware_optimizer.hpp`, `src/hardware_optimizer.cpp` | [docs/HARDWARE_OPTIMIZER.md](docs/HARDWARE_OPTIMIZER.md) |
| Network / Stratum | `include/xmr/stratum.hpp`, `src/stratum.cpp` | [docs/STRATUM.md](docs/STRATUM.md) |

Glue: `src/miner.cpp` (worker pool, jobs, hashrate), `src/config.cpp`,
`src/json.cpp` (dependency-free JSON), `src/main.cpp`.
Optional additions: `include/xmr/msr_tuner.hpp` and `src/msr_tuner.cpp` for
opt-in MSR prefetcher tuning.
Overall design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
Visual blueprint: [docs/BLUEPRINT.md](docs/BLUEPRINT.md).

## Build

Requires CMake 3.16+, a C++17 compiler and OpenSSL development headers/libraries.
CMake downloads RandomX with `FetchContent` (tag configurable with
`-DRANDOMX_TAG=...`). If OpenSSL is in a nonstandard location, pass
`-DOPENSSL_ROOT_DIR=<prefix>`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

With a multi-config generator on Windows, the executables are in
`build/Release/` (for example `build\Release\xmr-miner.exe`).

## Run in live public-pool mode

`--real` is the live-pool preset. It selects
`stratum+ssl://pool.supportxmr.com:443`, forces certificate/hostname verified
TLS, requires your own Monero wallet address, and uses full-dataset RandomX
with huge pages. You must supply the wallet with `-u`:

```sh
./build/xmr-miner --real -u YOUR_MONERO_ADDRESS -t 4 --print-time 60
```

A worker suffix is allowed, for example `-u YOUR_MONERO_ADDRESS.rig1`. Put `-o`
after `--real` to choose another public TLS pool; local/mock endpoints are
rejected. The same preset is available in JSON as `"real": true`. Keep the
mock-pool smoke test below for safe local experiments.

## Run a local smoke test

Terminal 1 — start the mock pool (it hands out difficulty-1 jobs, so every
hash is a valid share):

```sh
./build/mock-pool 3334
```

Optionally give it a certificate and key for a TLS smoke test:
`./build/mock-pool 3334 cert.pem key.pem`.

Terminal 2 — mine against it:

```sh
./build/xmr-miner -o 127.0.0.1:3334 -u test-wallet -t 2 --no-huge-pages --print-time 5
```

Outside `--real`, plain TCP (`stratum+tcp://host:port`) remains the default.
TLS is enabled with a `stratum+ssl://`, `stratum+tls://`, `ssl://` or `tls://`
URL, or with `--tls` / `"tls": true`. Certificate and hostname verification are
on by default; `--no-tls-verify` is only for self-signed pools and local tests.

## Command line

```
-o, --url HOST:PORT       Stratum pool
-u, --user WALLET         Wallet / worker name
-p, --pass PASS           Pool password
-t, --threads N           Workers (0/-1 = auto)
    --real                Live public-pool preset (TLS + wallet + full dataset)
    --mode auto|fast|light   RandomX mode
    --config FILE         JSON config (see config.example.json)
    --huge-pages / --no-huge-pages
    --1gb-pages
    --no-jit / --no-hw-aes
    --rx-v2               Set RANDOMX_FLAG_V2 (only if the chain uses v2)
    --secure-jit
    --msr                 Opt in to AMD/Intel MSR prefetcher tuning
    --msr-no-restore      Leave MSRs modified on exit
    --tls                 Use TLS for the first pool (or use a tls:// URL)
    --no-tls              Force plain TCP for the first pool
    --tls-verify          Verify certificate + hostname (default)
    --no-tls-verify       Disable verification (local/self-signed tests only)
    --print-time N
-v, --verbose / -q, --quiet
-h, --help
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

## Scope limits and opt-ins

- **TLS is opt-in outside `--real`.** Plain TCP remains the manual default.
  OpenSSL is linked directly; SNI and hostname verification are enabled by
  default. `--real` requires verified TLS and rejects `--no-tls-verify`.
  Disable verification only for self-signed local pools. Pool failover and
  retry pause work on both plain and TLS connections. This is not a
  production-audited networking stack.
- **MSR tuning is opt-in.** `--msr` / `"randomx.msr": true` applies XMRig-style
  prefetcher presets through WinRing0 on Windows or `/dev/cpu/N/msr` on Linux.
  Original values are restored on exit by default; `--msr-no-restore` disables
  that. It needs a privileged driver/root and should be measured per CPU family.
- **No stealth, persistence, autostart or evasion.** This is an interactive
  command-line miner. Only run it on machines you are authorized to use.
- **No donation/proxy/daemon modes, no OpenCL/CUDA backends, no HTTP API.**
- RandomX v1 is the default. Use `--rx-v2` only for a chain that actually
  uses the v2 configuration.

## Provenance

Research sources are MIT/X11 RandomX spec, tevador/RandomX README and API
header, and XMRig's public repository (`src/backend/cpu`, `src/crypto/rx`,
`src/base/net/stratum`, `src/hw`, `src/config.json`). All code here is original
and written for study; RandomX remains BSD-3-Clause and XMRig GPLv3.
