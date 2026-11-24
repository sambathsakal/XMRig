#pragma once

#include <randomx.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace xmr {

class MemoryManager;

struct RxEngineConfig {
    bool fullDataset = true;   // fast mode when true, light mode otherwise
    bool largePages = false;
    bool oneGbPages = false;
    bool jit = true;
    bool hwAes = true;
    bool secureJit = false;
    bool v2 = false;
    int initThreads = 0;       // 0 = single-threaded dataset init
};

class RxEpoch;
struct RxVm;

// Owns the cache/dataset for the current seed hash. A new epoch is built only
// when the seed hash changes (Monero changes it every 2048 blocks).
class RandomXEngine {
public:
    RandomXEngine(MemoryManager &memory) : m_memory(memory) {}
    ~RandomXEngine();

    bool initialize(const RxEngineConfig &config, std::string &error);
    bool setSeed(const std::uint8_t seed[32], std::string &error);

    std::unique_ptr<RxVm> createVm(std::string &error);

    void hash(RxVm &vm, const std::uint8_t *input, std::size_t size, std::uint8_t out[32]) const;

    // Pipeline: overlap the register-file finalisation of hash N with the VM
    // execution of hash N+1 (the library's first/next/last API).
    void hashBatch(RxVm &vm,
                   const std::uint8_t *inputs,
                   std::size_t count,
                   std::size_t inputSize,
                   std::uint8_t *outputs) const;

    bool selfTest(std::string &error) const;

    const RxEngineConfig &config() const { return m_config; }
    std::array<std::uint8_t, 32> seed() const;
    bool hasEpoch() const;
    std::uint64_t epochId() const;

private:
    randomx_flags vmFlags() const;

    MemoryManager &m_memory;
    RxEngineConfig m_config;
    mutable std::mutex m_mutex;
    std::shared_ptr<RxEpoch> m_epoch;
    std::uint64_t m_epochId = 0;
    std::array<std::uint8_t, 32> m_seed{};
    bool m_v2Epoch = false;
    bool m_initialized = false;
};

struct RxVm {
    randomx_vm *vm = nullptr;
    std::shared_ptr<RxEpoch> epoch;   // keeps cache/dataset alive

    RxVm() = default;
    ~RxVm();
    RxVm(const RxVm &) = delete;
    RxVm &operator=(const RxVm &) = delete;
};

} // namespace xmr
