# Architecture

## Data flow

```
                         +----------------------+
                         |  Stratum pool / mock |
                         +----------+-----------+
                                    | newline-delimited JSON-RPC
                          +---------v----------+
                          |  StratumClient     |  login -> job -> submit
                          +---------+----------+
                                    | shared_ptr<const Job>
                    +---------------v----------------+
                    | Miner::onJob                   |
                    |  - engine.setSeed(seed_hash)   |
                    |  - publish job + generation++  |
                    +---------------+----------------+
                                    |
       +----------------+-----------+-----------+----------------+
       |                |                       |                |
  +----v-----+     +----v-----+            +----v-----+     +----v-----+
  | worker 0 |     | worker 1 |    ...     | worker N |     | stats    |
  | VM       |     | VM       |            | VM       |     | thread   |
  +----+-----+     +----+-----+            +----+-----+     +----------+
       |                |                       |
       +---------> RandomXEngine <---------------+
                 cache / dataset / hashBatch()

  MemoryManager    -> RAM checks, aligned buffers, NUMA mapping
  HardwareOptimizer-> CPUID/OS topology, thread plan, affinity, priority
```

## Lifetime of a job

1. `StratumClient` logs in (`login`) and receives a `job` object, or receives
   later `job` notifications.
2. `parseJob` decodes the blob hex, the compact/little-endian target and the
   `seed_hash`.
3. `Miner::onJob` calls `RandomXEngine::setSeed`. If the 32-byte seed hash is
   unchanged, the existing cache/dataset is kept (Monero rotates it every
   2048 blocks). Otherwise a new epoch is built.
4. A monotonic `generation` counter is incremented. Workers notice the change
   and recreate their VM with `createVm()` so the VM points at the new epoch.
5. Workers copy the job blob into a local buffer, write a unique nonce at
   offset 39, hash with `hashBatch()`, and test `hash[24..31] <= target`.
6. Winning nonces are submitted with `submit`.

## Threading model

- One receiver thread in `StratumClient` (plain TCP uses a 1 s receive timeout;
  TLS uses a nonblocking socket with a 1 s `select()` wait).
- N miner threads, each owning one `randomx_vm*` and pinned to a physical core.
- One statistics thread.
- `RandomXEngine` guards cache/dataset replacement with a mutex; workers hold
  a `shared_ptr<RxEpoch>` through their VM, so an old epoch can never be freed
  while a worker is still hashing with it.

## Why a separate epoch object?

XMRig's `RxDataset`/`RxCache` are rebuilt only when the seed hash changes.
Keeping the epoch behind a `shared_ptr` gives the same semantics with very
little synchronization: job updates are cheap, and VM creation is the only
operation that needs the engine lock.

## Mapping to XMRig source

| Our module | XMRig equivalent |
|---|---|
| `RandomXEngine` | `src/crypto/rx/Rx.cpp`, `src/backend/cpu/CpuBackend.cpp` |
| `MemoryManager` | `src/crypto/common/VirtualMemory.cpp`, `src/base/kernel/Platform.cpp` |
| `HardwareOptimizer` | `src/backend/cpu/platform/`, `src/hw/` |
| `StratumClient`, `Job` | `src/base/net/stratum/Client.cpp`, `Job.cpp` |
| `Miner` | `src/core/Controller.cpp`, `src/core/config/Config.cpp` |
