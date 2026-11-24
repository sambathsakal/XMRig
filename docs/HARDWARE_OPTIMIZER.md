# Hardware Optimizer

`HardwareOptimizer` is the bridge between detected machine capabilities and the
concrete launch plan used by `Miner`. It consolidates the CPU/topology code that
XMRig spreads across `src/hw/`, `src/backend/cpu/platform/` and config handling.

## Files

| File | Role |
|---|---|
| `include/xmr/hardware_optimizer.hpp` | `CpuFeatures`, `Topology`, `HardwareProfile`, `HardwarePlan`, public API |
| `src/hardware_optimizer.cpp` | CPUID/OS detection, thread planning, affinity and priority |

## Public API

```cpp
HardwareOptimizer &HardwareOptimizer::instance();
bool detect(std::string &note);
const HardwareProfile &profile() const;

HardwarePlan plan(const Config &config, bool largePagesAvailable) const;

bool bindCurrentThread(int core) const;
bool raiseCurrentThreadPriority(int level) const;
std::string describe() const;
```

`detect()` is memoized: the first call fills `m_profile`, later calls return a
short note. `Miner::run()` calls it once at startup and logs `note` plus
`describe()` at debug level.

## Detected data

### `CpuFeatures`

| Field | Source |
|---|---|
| `vendor`, `brand` | CPUID vendor leaf, `/proc/cpuinfo`, or `"Windows CPU"` fallback |
| `family`, `model` | CPUID version leaf or `/proc/cpuinfo` |
| `aes`, `sse2`, `avx2`, `avx512`, `vaes`, `sha`, `bmi2` | CPUID feature leaves and `/proc/cpuinfo` flags |
| `jitSupported` | Compile-time architecture check (x86-64, AArch64, RV64) |

On Windows with MSVC the code uses `__cpuid`/`__cpuidex`. On GCC/Clang x86 it
uses `<cpuid.h>`. Every path has a non-x86 fallback. Runtime detection covers
AES-NI, AVX2, AVX-512F, VAES, SHA-NI and BMI2. Actual RandomX VM dispatch is
left to the upstream RandomX JIT; this rebuild does not carry XMRig's
hand-written assembly profiles (`intel`, `ryzen`, `bulldozer`).

### `Topology`

| Field | Source |
|---|---|
| `logical` | `GetSystemInfo()` or `sysconf(_SC_NPROCESSORS_ONLN)` |
| `physical` | `GetLogicalProcessorInformation(RelationProcessorCore)` or unique `(physical id, core id)` pairs from `/proc/cpuinfo` |
| `sockets` | `logical / physical` (at least 1) |
| `l1Bytes`, `l2Bytes`, `l3Bytes` | Windows cache relationships or Linux `/sys/devices/system/cpu/cpu0/cache/indexN/size` |
| `physicalCoreIds` | First logical processor of each physical core (Windows) or `0..physical-1` (fallback) |

## Thread planning

`plan(config, largePagesAvailable)` builds a `HardwarePlan`:

1. `threads = config.threads` when positive; otherwise:
   - `light` mode -> `topo.logical`
   - `auto`/`fast` -> `topo.physical`
2. Apply `cpu.max-threads-hint` when it is between 1 and 99 percent.
3. Clamp to `topo.logical`.
4. If L3 is at least 2 MiB, cap threads to `l3Bytes / 2 MiB`. XMRig's published
   rule of thumb is 256 KiB of L2 and 2 MiB of L3 per mining thread; exceeding
   it causes cache thrashing.

Derived flags:

| `HardwarePlan` field | Value |
|---|---|
| `jit` | `config.jit && cpu.jitSupported` |
| `hwAes` | `config.hwAes && cpu.aes` |
| `largePages` | `largePagesAvailable` from `MemoryManager` |
| `oneGbPages` | `config.oneGbPages` |
| `fullDataset` | `config.mode != "light"` |
| `v2` | `config.v2` |
| `secureJit` | `config.secureJit` |
| `initThreads` | `config.initThreads` if positive, otherwise `min(8, max(1, physical / 2))`, independent of miner thread count |
| `priority` | `1` |
| `coreIds` | Round-robin selection from `physicalCoreIds` |

`note` contains a one-line human-readable summary that `Miner` logs, e.g.
`"plan: 4 thread(s), fast mode, JIT, hardware AES, large pages"`.

## Runtime tuning

`bindCurrentThread(core)` is called from inside each worker:

- Linux: `sched_setaffinity(0, ...)`
- Windows: `SetThreadAffinityMask(GetCurrentThread(), 1 << core)`

`raiseCurrentThreadPriority(level)` is also called from each worker:

- Linux: `setpriority(PRIO_PROCESS, 0, level > 0 ? -1 : 0)`
- Windows: `SetThreadPriority(..., THREAD_PRIORITY_ABOVE_NORMAL)`

Both return `bool` but the miner ignores failures: locked-down environments
often forbid affinity or priority changes, and neither affects correctness.

## Optional MSR tuning

XMRig can write AMD/Intel model-specific registers (MSRs) via WinRing0 on
Windows or `/dev/cpu/*/msr` on Linux. On Ryzen it uses preset values to disable
hardware prefetchers, which otherwise waste bandwidth on RandomX's random
access pattern; it also offers Cache QoS and custom MSR values.

This rebuild now has a small `MsrTuner` (`include/xmr/msr_tuner.hpp`,
`src/msr_tuner.cpp`) but it is **opt-in**: `Miner::run()` only calls `apply()`
when `--msr` or `"randomx": { "msr": true }` is set. It applies a Zen 2/Zen 3
AMD prefetcher preset or the Intel `0x1a4 = 0xf` preset, remembers the original
values and restores them on exit unless `--msr-no-restore` /
`"msr-restore": false` is used. Windows requires the WinRing0 device
`\\.\WinRing0_1_2_0`; Linux needs root and `msr.allow_writes=1`.

This is deliberately narrower than XMRig: there is no Cache QoS preset, no
custom MSR syntax and no automatic per-model tuning. It needs a privileged
driver, is hardware-specific and can leave the CPU modified if restore is
disabled. Measure each CPU family before assuming a gain.
