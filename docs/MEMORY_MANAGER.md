# Memory Manager

RandomX is memory-hard, so memory policy is a first-class subsystem rather than
a detail hidden inside the allocator. Standard OS pages are 4 KiB. Touching a
2,080 MiB dataset through 4 KiB pages means roughly half a million page
translations, which overwhelms the CPU's Translation Lookaside Buffer (TLB).
Two MiB huge pages cut that by ~512x; 1 GiB pages cut it further. XMRig documents
up to ~50% higher RandomX hashrate from huge pages, with 1 GiB pages adding a
further 1-3% on Linux.

In this rebuild `MemoryManager` is a process-wide singleton that answers three questions:

1. Is there enough RAM for the requested RandomX mode?
2. Are large pages available, and should RandomX be allowed to request them?
3. Where are the NUMA nodes, and which node does a worker core belong to?

The RandomX library still performs the actual 256 MiB cache and ~2.1 GiB
dataset allocations itself; this manager owns only small aligned helper buffers
and the policy decisions passed into RandomX flags.

## Files

| File | Role |
|---|---|
| `include/xmr/memory_manager.hpp` | `MemoryStats`, public API |
| `src/memory_manager.cpp` | OS detection, hugepage detection, NUMA mapping, aligned allocation tracking |

## Public API

```cpp
MemoryManager &MemoryManager::instance();
bool initialize(bool wantLargePages, std::string &note);
bool canReserve(std::size_t bytes) const;

void *alignedAlloc(std::size_t bytes, std::size_t alignment = 64);
void alignedFree(void *ptr);

template<typename T> T *alloc(std::size_t count, std::size_t alignment = 64);

int nodeForCore(int core) const;
void prefetchPages(void *ptr, std::size_t bytes) const;

const MemoryStats &stats() const;
std::size_t reservationCount() const;
```

## `MemoryStats`

| Field | Meaning |
|---|---|
| `totalRam` | Physical RAM reported by the OS |
| `availableRam` | RAM currently available (best-effort) |
| `reserved` | Bytes currently tracked by `alignedAlloc()` |
| `peakReserved` | High-water mark of `reserved` |
| `basePageSize` | OS base page size (normally 4096 bytes) |
| `largePageSize` | OS large-page granularity (2 MiB on Windows and typical Linux; 0 if unknown) |
| `largePagePrivilege` | Linux: preallocated hugepages exist; Windows: `SeLockMemoryPrivilege` was enabled |
| `transparentHugePages` | Linux: THP is active in `always` or `madvise` mode |
| `numaNodes` | Number of discovered NUMA nodes (at least 1) |

## Detection details

### RAM

- **Windows:** `GlobalMemoryStatusEx()`.
- **Linux:** `/proc/meminfo` (`MemTotal`, `MemAvailable`), with `sysconf(_SC_PHYS_PAGES)` as a fallback.
- **Other POSIX:** `sysconf()` fallback.

### Page sizes

- **Windows:** `GetSystemInfo().dwPageSize` and `GetLargePageMinimum()`.
- **Linux:** `sysconf(_SC_PAGESIZE)` and `Hugepagesize:` from `/proc/meminfo`.

`prefetchPages()` uses `basePageSize` as its stride instead of a hardcoded
4096 bytes. The helper is still only for small aligned buffers; RandomX warms
its own cache and dataset during epoch setup.

### Large pages

- **Windows:** opens the process token, looks up `SE_LOCK_MEMORY_NAME`, and calls
  `AdjustTokenPrivileges()`. If this fails, a note tells the user how to grant
  the privilege.
- **Linux:** reads `HugePages_Total` from `/proc/meminfo` and checks
  `/sys/kernel/mm/transparent_hugepage/enabled` for `[always]` or `[madvise]`.

`Miner::run()` treats `largePagePrivilege || transparentHugePages` as
`largePagesAvailable` and passes that to `HardwareOptimizer::plan()`. The final
decision still reaches the RandomX library as `RANDOMX_FLAG_LARGE_PAGES`, which
can fail and fall back to normal pages.

### NUMA

- **Windows:** `GetNumaHighestNodeNumber()` + `GetNumaProcessorNode()`.
- **Linux:** `/sys/devices/system/node/nodeN/cpulist`, parsed by `parseCpuList()`
  (supports both `0-3` and `0,2,4` forms).

`nodeForCore(core)` returns the node index for a logical core, or 0 if unknown.
It is available for future NUMA-aware VM placement; the current miner uses it
only for diagnostics. XMRig can keep one dataset per NUMA node so its workers
stay on the memory controller local to that socket. Upstream RandomX allocates
one dataset per process here, so we do not get that cross-socket benefit; a
multi-socket user would need one miner process per socket or a RandomX fork with
per-node allocation.

## Allocation API

`alignedAlloc()` returns 64-byte aligned memory by default:

- Windows: `_aligned_malloc()`
- POSIX: `posix_memalign()`

Every allocation is recorded in an `unordered_map` together with its size and
alignment so `alignedFree()` can decrement `reserved` and free it with the
matching API (`_aligned_free()` on Windows, `std::free()` elsewhere).
`reservationCount()` exposes that map size for diagnostics.

`canReserve(bytes)` compares `bytes + 512 MiB margin + already-reserved bytes`
against `availableRam`. If RAM cannot be detected (`availableRam == 0`) it
returns `true`, avoiding false negatives on unusual systems.

`prefetchPages(ptr, bytes)` touches one byte per 4 KiB page. It is a small
helper for warming buffers; it is not called in the current mining loop because
RandomX performs its own dataset initialization.

## XMRig comparison

XMRig's `VirtualMemory` layer (`src/base/crypto/common/VirtualMemory*.cpp` in
current releases; older notes call it `src/base/kernel/Memory*.cpp`) is more
aggressive: it can preallocate huge pages at startup, manage 1 GiB pages, use
`mmap(MAP_HUGETLB)`/`VirtualAlloc(MEM_LARGE_PAGES)` directly, and place
allocations per NUMA node. This rebuild keeps the same concepts but uses
portable APIs and safe fallbacks so the code remains readable.
