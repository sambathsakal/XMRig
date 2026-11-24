# RandomX VM Engine

`RandomXEngine` owns the RandomX cache/dataset for the current seed hash and
hands out one VM per worker. The VM core itself is the official
[tevador/RandomX](https://github.com/tevador/RandomX) library: XMRig also uses a
vendored, tuned RandomX library rather than re-deriving the algorithm, so this
is the faithful architectural choice.

## Files

| File | Role |
|---|---|
| `include/xmr/randomx_engine.hpp` | `RxEngineConfig`, `RxVm`, public API |
| `src/randomx_engine.cpp` | Epoch lifecycle, flags, dataset init, hashing, self-test |

## Public API

```cpp
bool initialize(const RxEngineConfig &config, std::string &error);
bool setSeed(const std::uint8_t seed[32], std::string &error);
std::unique_ptr<RxVm> createVm(std::string &error);

void hash(RxVm &vm, const std::uint8_t *input, std::size_t size,
          std::uint8_t out[32]) const;

void hashBatch(RxVm &vm, const std::uint8_t *inputs, std::size_t count,
               std::size_t inputSize, std::uint8_t *outputs) const;

bool selfTest(std::string &error) const;

const RxEngineConfig &config() const;
std::array<std::uint8_t, 32> seed() const;
bool hasEpoch() const;
std::uint64_t epochId() const;
```

## `RxEngineConfig`

| Field | Effect |
|---|---|
| `fullDataset` | Fast mode (~2.1 GiB dataset) when true, 256 MiB cache-only mode when false |
| `largePages` | Add `RANDOMX_FLAG_LARGE_PAGES` |
| `oneGbPages` | Also request large pages; the OS decides whether 1 GiB pages are actually used |
| `jit` | Add `RANDOMX_FLAG_JIT` |
| `hwAes` | Add `RANDOMX_FLAG_HARD_AES` |
| `secureJit` | Add `RANDOMX_FLAG_SECURE` (W^X JIT pages) |
| `v2` | Add `RANDOMX_FLAG_V2` when the fetched library supports it |
| `initThreads` | Number of threads used for `randomx_init_dataset()`; 0 = single-threaded. `HardwarePlan` normally supplies `min(8, max(1, physical / 2))` |

`Miner::run()` fills this struct from `HardwarePlan`.

## Initialization

`initialize()` checks `MemoryManager::canReserve()` against
`256 MiB + (fullDataset ? 2.1 GiB : 0)` and stores the config. It does not
allocate the cache yet; allocation happens on the first `setSeed()`.

If `v2` is requested but the fetched RandomX header has no `RANDOMX_FLAG_V2`,
initialization fails with a clear message. CMake detects the enum from the
fetched header and defines `RANDOMX_HAS_V2` for this target.

## Epoch lifecycle

```text
setSeed(seed_hash)
    |
    +-- same seed and same v2 setting -> no work
    |
    +-- new seed -------------------> allocate cache
                                      randomx_init_cache()
                                      if fullDataset:
                                          allocate dataset
                                          randomx_init_dataset() [initThreads]
                                      store shared_ptr<RxEpoch>
                                      ++epochId
```

Monero changes the seed hash every 2048 blocks. `setSeed()` preserves the epoch
when the seed hash is unchanged, which is the common case for ordinary job
notifications.

Cache flags are `JIT | LARGE_PAGES | recommended Argon2 impl | V2`. JIT is
retried without `RANDOMX_FLAG_JIT` if allocation fails. Dataset allocation uses
only `LARGE_PAGES` since that is the only flag the RandomX API accepts for
`randomx_alloc_dataset()`.

The dataset is initialized in `initThreads` non-overlapping ranges. Each call
covers `startItem ... startItem + itemCount` and uses
`randomx_dataset_item_count()` from the library.

## `RxEpoch` and `RxVm`

`RxEpoch` is an internal RAII wrapper around `randomx_cache*` and
`randomx_dataset*`. It lives behind `std::shared_ptr`.

`RxVm` is the public per-worker wrapper:

```cpp
struct RxVm {
    randomx_vm *vm = nullptr;
    std::shared_ptr<RxEpoch> epoch;   // keeps cache/dataset alive
};
```

Its destructor calls `randomx_destroy_vm()`. Holding the epoch `shared_ptr`
means a newly built epoch cannot be freed while an older VM is still hashing.
`createVm()` takes the engine lock only long enough to copy that pointer.

VM flags are built from the config (`LARGE_PAGES`, `JIT`, `HARD_AES`,
`FULL_MEM`, `SECURE`, `V2`). If VM creation fails with JIT/AES enabled, the code
retries with a portable flag set. `fullDataset` must match the dataset that was
built for the epoch.

## Hashing

`hash()` is a thin wrapper around `randomx_calculate_hash()`.

`hashBatch()` uses the library's pipelined API:

```cpp
randomx_calculate_hash_first(vm, inputs, inputSize);
for (i = 1; i < count; ++i)
    randomx_calculate_hash_next(vm, inputs + i * inputSize, inputSize,
                                outputs + (i - 1) * 32);
randomx_calculate_hash_last(vm, outputs + (count - 1) * 32);
```

This overlaps finalization of one hash with VM execution of the next.
`Miner::worker` uses batches of four blobs with unique nonces at offset 39.

## Self-test

`selfTest()` hashes the same synthetic 76-byte blob twice on one VM and once on
an independent second VM. It fails if results differ, which catches accidental
VM state leakage or misuse of the pipelined API. It is available for tests and
future CLI diagnostics but is not run on every startup because creating a
second VM costs additional 2 MiB scratchpad memory.
