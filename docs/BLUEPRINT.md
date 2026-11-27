# Project Blueprint

This is the visual counterpart to [ARCHITECTURE.md](ARCHITECTURE.md). All
diagrams are [Mermaid](https://mermaid.js.org/) and render directly on GitHub,
so the blueprint stays version-controlled with the code.

## 1. System context

```mermaid
flowchart LR
    Operator["Operator / CLI"] -->|flags + config.json| MinerExe["xmr-miner.exe"]
    MinerExe --> Core["xmr_core static library"]

    subgraph External["External dependencies"]
        RX["tevador/RandomX v2.0.1"]
        SSL["OpenSSL"]
        OS["Windows / Linux OS APIs"]
    end

    Core --> RX
    Core --> SSL
    Core --> OS

    Core <-->|newline-delimited JSON-RPC 2.0<br/>plain TCP or TLS| Pool["Monero Stratum pool"]
    Mock["mock-pool.exe"] -.->|local difficulty-1 stand-in| Pool
    Tests["test_core.exe"] -.->|unit checks| Core
```

## 2. Module map

| Layer | Code | Responsibility |
|---|---|---|
| Entry / config | `src/main.cpp`, `src/config.cpp`, `src/json.cpp` | Parse config/CLI, apply `--real` preset, validate, set log level |
| Orchestration | `src/miner.cpp` | Startup, job fan-out, worker pool, hashrate statistics, shutdown |
| Networking | `src/stratum.cpp` | Login, jobs, target parsing, share submission, TLS, failover, keepalive |
| RandomX | `src/randomx_engine.cpp` | Cache/dataset epochs, VM creation, batched hashing, self-test |
| Memory | `src/memory_manager.cpp` | RAM/page/NUMA detection, allocation policy, page prefetch |
| Hardware | `src/hardware_optimizer.cpp` | CPUID/topology, thread plan, affinity, priority |
| MSR | `src/msr_tuner.cpp` | Opt-in prefetcher MSR presets and restore |
| Test tooling | `tools/mock_pool.cpp`, `tests/test_core.cpp` | Local smoke pool and unit tests |

## 3. Component architecture

```mermaid
flowchart TD
    subgraph Frontend["Frontend"]
        Main["main.cpp"]
        Config["Config / JSON parser"]
    end

    subgraph Orchestration["Orchestration"]
        Miner["Miner"]
    end

    subgraph Network["Networking"]
        Stratum["StratumClient"]
        Job["Job + target helpers"]
    end

    subgraph Compute["Compute and platform"]
        Engine["RandomXEngine"]
        Memory["MemoryManager"]
        Hardware["HardwareOptimizer"]
        Msr["MsrTuner (opt-in)"]
    end

    subgraph Deps["Imported libraries"]
        RandomXLib["randomx target"]
        OpenSSL["OpenSSL SSL + Crypto"]
    end

    Main --> Config
    Config -->|validated Config| Miner
    Miner --> Stratum
    Stratum --> Job
    Job -->|shared_ptr&lt;const Job&gt;| Miner
    Miner --> Engine
    Miner --> Memory
    Miner --> Hardware
    Miner --> Msr
    Engine --> RandomXLib
    Engine --> Memory
    Stratum --> OpenSSL
    Hardware -->|affinity + priority| Miner
```

## 4. Core class relationships

```mermaid
classDiagram
    class Config {
        +vector~PoolConfig~ pools
        +bool realMode
        +string mode
        +bool hugePages
    }
    class Miner {
        -Config m_config
        -RandomXEngine m_engine
        -StratumClient m_client
        -shared_ptr~const Job~ m_job
        +run() int
        -onJob(job)
        -worker(index, core, count)
        -statsLoop()
    }
    class StratumClient {
        -vector~PoolConfig~ pools
        -SocketHandle m_socket
        -SSL m_tls
        +start() bool
        +stop()
        +submit(id, nonce, hash) bool
        -parseJob(json, job) bool
    }
    class RandomXEngine {
        -shared_ptr~RxEpoch~ m_epoch
        -mutex m_mutex
        +initialize(config) bool
        +setSeed(seed) bool
        +createVm() RxVm
        +hashBatch(vm, inputs, count)
    }
    class MemoryManager {
        +initialize(wantLargePages) bool
        +canReserve(bytes) bool
        +prefetchPages(ptr, bytes)
    }
    class HardwareOptimizer {
        +detect() bool
        +plan(config, largePages) HardwarePlan
        +bindCurrentThread(core) bool
    }
    class MsrTuner {
        +apply(enable, restore, cpu) bool
        +restore()
    }

    Miner --> Config
    Miner --> StratumClient
    Miner --> RandomXEngine
    Miner --> MemoryManager
    Miner --> HardwareOptimizer
    Miner --> MsrTuner
    RandomXEngine --> MemoryManager
    StratumClient --> Config
```

## 5. Startup and mining sequence

```mermaid
sequenceDiagram
    participant CLI as main.cpp
    participant CFG as Config
    participant M as Miner::run
    participant Mem as MemoryManager
    participant HW as HardwareOptimizer
    participant MSR as MsrTuner
    participant RX as RandomXEngine
    participant S as StratumClient
    participant Pool as Stratum pool

    CLI->>CFG: loadConfigFile / applyCommandLine
    CLI->>CFG: finalizeConfig (real-mode gates)
    CLI->>M: Miner(config).run()
    M->>Mem: initialize(hugePages)
    M->>HW: detect()
    M->>MSR: apply(msr, restore, CPU info)
    M->>HW: plan(config, largePagesAvailable)
    M->>RX: initialize(RxEngineConfig)
    M->>S: start()

    S->>Pool: TCP/TLS connect
    S->>Pool: login(id=1, wallet, pass, agent, rigid)
    Pool-->>S: result.id + optional job
    S->>M: onJob(job)
    M->>RX: setSeed(seed_hash)
    RX-->>M: epoch ready (reused or rebuilt)
    M->>M: generation++ and publish job

    par Worker loop
        M->>RX: createVm() when generation changes
        M->>RX: hashBatch(4 blobs with unique nonces)
        RX-->>M: 32-byte hashes
        M->>S: submit(jobId, nonce, hash) when target met
        S->>Pool: submit JSON-RPC
    and Stats loop
        M->>M: sample atomic hash counter every print-time
    end
```

## 6. Job and epoch lifecycle

```mermaid
flowchart TD
    A["Stratum job notification"] --> B["parseJob: blob, target, seed_hash"]
    B --> C{"seed_hash changed?"}
    C -->|no| D["Reuse existing cache/dataset"]
    C -->|yes| E["Build new cache"]
    E --> F["Build dataset in fast mode"]
    F --> G["Publish new shared_ptr&lt;RxEpoch&gt;"]
    D --> H["Increment job generation"]
    G --> H
    H --> I["Workers recreate VM on generation change"]
    I --> J["Write nonce at blob offset 39"]
    J --> K["hashBatch over 4 nonces"]
    K --> L{"hash[24..31] &lt;= target?"}
    L -->|yes| M["submit(jobId, nonce, hash)"]
    L -->|no| J
```

## 7. Stratum connection state machine

```mermaid
stateDiagram-v2
    [*] --> Disconnected
    Disconnected --> Connecting: start / retry
    Connecting --> TlsHandshake: tls URL or --tls
    Connecting --> Authenticating: plain TCP connect
    TlsHandshake --> Authenticating: verified handshake
    TlsHandshake --> Disconnected: verify or handshake failure
    Authenticating --> Mining: login OK and job received
    Authenticating --> Disconnected: login rejected
    Mining --> Mining: keepalived / new jobs / shares
    Mining --> Reconnecting: pool asked to reconnect
    Mining --> Disconnected: socket lost
    Reconnecting --> Connecting
    Disconnected --> Failover: retries exhausted
    Failover --> Connecting: rotate pool index
    Mining --> [*]: stop()
```

## 8. Threading and shared state

```mermaid
flowchart LR
    subgraph Threads["Threads"]
        Main["Main thread<br/>signal + 250 ms wait"]
        RxThread["Stratum receiver thread<br/>read JSON / drain outbox"]
        W0["Worker 0<br/>VM + core affinity"]
        W1["Worker 1<br/>VM + core affinity"]
        WN["Worker N<br/>VM + core affinity"]
        Stats["Stats thread<br/>hashrate report"]
    end

    subgraph Shared["Shared state"]
        JobState["m_job + m_generation<br/>guarded by m_stateMutex"]
        Counters["m_hashes / m_accepted / m_rejected<br/>atomic counters"]
        Outbox["share outbox<br/>guarded by m_outboxMutex"]
        Epoch["current RxEpoch<br/>shared_ptr in each VM"]
    end

    RxThread -->|publish job| JobState
    JobState --> W0
    JobState --> W1
    JobState --> WN
    W0 --> Counters
    W1 --> Counters
    WN --> Counters
    W0 --> Outbox
    W1 --> Outbox
    WN --> Outbox
    Outbox --> RxThread
    W0 --> Epoch
    W1 --> Epoch
    WN --> Epoch
    Stats --> Counters
    Main --> RxThread
    Main --> Stats
```

## 9. Build and verification pipeline

```mermaid
flowchart LR
    CMake["CMake configure"] --> RXFetch["FetchContent: tevador/RandomX v2.0.1"]
    CMake --> SSLFind["find_package(OpenSSL)"]
    CMake --> Threads["find_package(Threads)"]
    RXFetch --> Core["xmr_core.a"]
    SSLFind --> Core
    Threads --> Core
    Core --> Miner["xmr-miner.exe"]
    Core --> Mock["mock-pool.exe"]
    Core --> Test["test_core.exe"]
    Test --> CTest["ctest"]
    Miner --> SmokeTCP["plain TCP smoke"]
    Mock --> SmokeTCP
    Miner --> SmokeTLS["TLS smoke with self-signed cert"]
    Mock --> SmokeTLS
```

## 10. Real-mode decision flow

```mermaid
flowchart TD
    A["Start"] --> B{"--real or real:true?"}
    B -->|no| C["Manual mode: plain TCP default<br/>TLS and wallet are user-managed"]
    B -->|yes| D["Apply preset:<br/>public TLS pool, verified TLS,<br/>fast full dataset, huge pages"]
    D --> E{"finalizeConfig checks"}
    E -->|wallet missing or placeholder| X["Reject"]
    E -->|localhost / mock pool| X
    E -->|TLS verification disabled| X
    E -->|light mode| X
    E -->|all checks pass| F["Start live mining session"]
    C --> G["Mock-pool smoke test remains available"]
```

## 11. Source tree

```text
E:\opencode\XMRig
├── CMakeLists.txt
├── config.example.json
├── README.md
├── docs/
│   ├── ARCHITECTURE.md
│   ├── BLUEPRINT.md        <-- this file
│   ├── HARDWARE_OPTIMIZER.md
│   ├── MEMORY_MANAGER.md
│   ├── RANDOMX_VM.md
│   └── STRATUM.md
├── include/xmr/
│   ├── common.hpp
│   ├── config.hpp
│   ├── hardware_optimizer.hpp
│   ├── json.hpp
│   ├── memory_manager.hpp
│   ├── miner.hpp
│   ├── msr_tuner.hpp
│   ├── randomx_engine.hpp
│   └── stratum.hpp
├── src/
│   ├── config.cpp
│   ├── hardware_optimizer.cpp
│   ├── json.cpp
│   ├── main.cpp
│   ├── memory_manager.cpp
│   ├── miner.cpp
│   ├── msr_tuner.cpp
│   ├── randomx_engine.cpp
│   └── stratum.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── test_core.cpp
└── tools/
    └── mock_pool.cpp
```

> Build outputs under `build/` are intentionally ignored by Git.
