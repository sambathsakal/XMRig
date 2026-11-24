#include "miner.hpp"

#include "common.hpp"
#include "msr_tuner.hpp"

#include <chrono>
#include <csignal>
#include <random>

namespace xmr {

namespace {

volatile sig_atomic_t g_stopSignal = 0;

void signalHandler(int)
{
    g_stopSignal = 1;
}

} // namespace

void Miner::requestStop()
{
    g_stopSignal = 1;
}

bool Miner::stopRequested()
{
    return g_stopSignal != 0;
}

Miner::Miner(Config config)
    : m_config(std::move(config)),
      m_memory(MemoryManager::instance()),
      m_hardware(HardwareOptimizer::instance()),
      m_engine(m_memory)
{
}

Miner::~Miner()
{
    m_running.store(false);
    if (m_client) m_client->stop();
    for (auto &worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
    if (m_statsThread.joinable()) m_statsThread.join();
    MsrTuner::instance().restore();
}

int Miner::run()
{
    std::signal(SIGINT, signalHandler);
#ifndef _WIN32
    std::signal(SIGTERM, signalHandler);
#endif

    std::string note;
    m_memory.initialize(m_config.hugePages, note);
    logInfo("memory", note);

    m_hardware.detect(note);
    logInfo("hw", note);
    logDebug("hw", m_hardware.describe());

    const HardwareProfile &hw = m_hardware.profile();
    std::string msrNote;
    MsrTuner::instance().apply(m_config.msr, m_config.msrRestore,
                               hw.cpu.vendor, hw.cpu.family,
                               hw.topology.logical, msrNote);
    if (m_config.msr) {
        logInfo("msr", msrNote);
    } else {
        logDebug("msr", msrNote);
    }

    const bool largePages = m_memory.stats().largePagePrivilege || m_memory.stats().transparentHugePages;
    const HardwarePlan plan = m_hardware.plan(m_config, largePages);
    logInfo("hw", plan.note);

    RxEngineConfig rx;
    rx.fullDataset = plan.fullDataset;
    rx.largePages = plan.largePages;
    rx.oneGbPages = plan.oneGbPages;
    rx.jit = plan.jit;
    rx.hwAes = plan.hwAes;
    rx.secureJit = plan.secureJit;
    rx.v2 = plan.v2;
    rx.initThreads = plan.initThreads;

    std::string error;
    if (!m_engine.initialize(rx, error)) {
        logError("randomx", error);
        return 1;
    }

    if (!m_config.cpuEnabled) {
        logError("config", "CPU backend disabled in configuration");
        return 1;
    }
    if (m_config.pools.empty()) {
        logError("config", "no pool configured");
        return 1;
    }

    m_running.store(true);
    m_client.reset(new StratumClient(m_config.pools,
                                     m_config.retries,
                                     m_config.retryPause,
                                     [this](std::shared_ptr<const Job> job) {
                                         onJob(std::move(job));
                                     }));
    m_client->start();

    for (int i = 0; i < plan.threads; ++i) {
        const int core = plan.coreIds[static_cast<std::size_t>(i % plan.coreIds.size())];
        m_workers.emplace_back([this, i, core, plan]() { worker(i, core, plan.threads); });
    }
    m_statsThread = std::thread(&Miner::statsLoop, this);

    while (m_running.load() && !stopRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    logInfo("miner", "shutting down");
    m_running.store(false);
    m_client->stop();
    for (auto &worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
    if (m_statsThread.joinable()) m_statsThread.join();
    MsrTuner::instance().restore();
    return 0;
}

void Miner::onJob(std::shared_ptr<const Job> job)
{
    if (!job || !job->valid()) {
        logWarn("stratum", "ignoring invalid job");
        return;
    }

    if (job->hasSeed) {
        std::string error;
        if (!m_engine.setSeed(job->seed.data(), error)) {
            logError("randomx", "cannot initialize epoch: ", error);
            return;
        }
    }
    if (!m_engine.hasEpoch()) {
        logWarn("randomx", "job received without a seed hash; cannot mine it");
        return;
    }

    std::shared_ptr<const Job> current;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_job = std::move(job);
        current = m_job;
        ++m_generation;
    }

    logInfo("stratum", "job ", current->id, " height ", current->height,
            " diff ", current->difficulty(), " blob ", current->blob.size(), " bytes");
}

void Miner::worker(int index, int core, int threadCount)
{
    m_hardware.bindCurrentThread(core);
    m_hardware.raiseCurrentThreadPriority(1);

    std::string error;
    std::unique_ptr<RxVm> vm;
    std::uint64_t myGeneration = 0;

    Bytes blob;
    std::uint8_t hash[32]{};

    constexpr std::size_t kBatch = 4;
    std::uint8_t inputs[kBatch][kMaxBlobSize]{};
    std::uint8_t outputs[kBatch][32]{};
    std::uint32_t nonces[kBatch]{};

    const int threads = std::max(1, threadCount);
    std::uint32_t nonce = static_cast<std::uint32_t>(steadyMillis()) ^
                          (static_cast<std::uint32_t>(index) * 0x9e3779b9u);

    while (m_running.load() && !stopRequested()) {
        std::shared_ptr<const Job> job;
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            job = m_job;
            generation = m_generation.load();
        }

        if (!job) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!vm || myGeneration != generation) {
            vm = m_engine.createVm(error);
            if (!vm) {
                logError("worker", index, ": ", error);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            myGeneration = generation;
        }

        blob = job->blob;

        for (std::size_t k = 0; k < kBatch; ++k) {
            std::memcpy(inputs[k], blob.data(), blob.size());
            storeLe32(inputs[k] + kNonceOffset, nonce);
            nonces[k] = nonce;
            nonce = (nonce + static_cast<std::uint32_t>(threads)) & 0xffffffffu;
        }

        m_engine.hashBatch(*vm, &inputs[0][0], kBatch, blob.size(), &outputs[0][0]);
        m_hashes.fetch_add(kBatch, std::memory_order_relaxed);

        for (std::size_t k = 0; k < kBatch; ++k) {
            if (!job->accepts(outputs[k])) continue;

            logInfo("worker", index, ": share found nonce=", nonces[k],
                    " hash=", toHex(outputs[k], 32));
            if (m_client && m_client->submit(job->id, nonces[k], outputs[k])) {
                m_accepted.fetch_add(1, std::memory_order_relaxed);
            } else {
                m_rejected.fetch_add(1, std::memory_order_relaxed);
            }

            // Refresh the job snapshot so a newly received job is picked up.
            break;
        }
    }
}

void Miner::statsLoop()
{
    const int interval = std::max(1, m_config.printTime);
    std::uint64_t previous = m_hashes.load(std::memory_order_relaxed);
    std::uint64_t last = steadyMillis();

    while (m_running.load() && !stopRequested()) {
        for (int i = 0; i < interval * 4 && m_running.load() && !stopRequested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!m_running.load()) break;

        const std::uint64_t now = steadyMillis();
        const std::uint64_t current = m_hashes.load(std::memory_order_relaxed);
        const double seconds = static_cast<double>(now - last) / 1000.0;
        if (seconds > 0.0) {
            const double rate = static_cast<double>(current - previous) / seconds;
            logInfo("miner", "hashrate ", formatHashrate(rate),
                    " submitted ", m_accepted.load(),
                    " submit-failed ", m_rejected.load(),
                    " epoch ", m_engine.epochId());
        }
        previous = current;
        last = now;
    }
}

} // namespace xmr
