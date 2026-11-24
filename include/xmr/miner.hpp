#pragma once

#include "config.hpp"
#include "hardware_optimizer.hpp"
#include "memory_manager.hpp"
#include "randomx_engine.hpp"
#include "stratum.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xmr {

class Miner {
public:
    explicit Miner(Config config);
    ~Miner();

    int run();

    static void requestStop();
    static bool stopRequested();

private:
    void onJob(std::shared_ptr<const Job> job);
    void worker(int index, int core, int threadCount);
    void statsLoop();

    Config m_config;
    MemoryManager &m_memory;
    HardwareOptimizer &m_hardware;
    RandomXEngine m_engine;
    std::unique_ptr<StratumClient> m_client;

    std::mutex m_stateMutex;
    std::shared_ptr<const Job> m_job;

    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<std::uint64_t> m_hashes{0};
    std::atomic<std::uint64_t> m_accepted{0};
    std::atomic<std::uint64_t> m_rejected{0};
    std::atomic<bool> m_running{false};

    std::vector<std::thread> m_workers;
    std::thread m_statsThread;
};

} // namespace xmr
