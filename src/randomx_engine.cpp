#include "randomx_engine.hpp"

#include "common.hpp"
#include "memory_manager.hpp"

#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace xmr {

class RxEpoch {
public:
    RxEpoch(randomx_cache *cache, randomx_dataset *dataset) : cache(cache), dataset(dataset) {}

    ~RxEpoch()
    {
        if (dataset) randomx_release_dataset(dataset);
        if (cache) randomx_release_cache(cache);
    }

    randomx_cache *cache = nullptr;
    randomx_dataset *dataset = nullptr;

    RxEpoch(const RxEpoch &) = delete;
    RxEpoch &operator=(const RxEpoch &) = delete;
};

RxVm::~RxVm()
{
    if (vm) {
        randomx_destroy_vm(vm);
    }
}

RandomXEngine::~RandomXEngine() = default;

namespace {

constexpr std::uint64_t kCacheSize = 256ULL << 20;
constexpr std::uint64_t kDatasetSize = (2147483648ULL + 33554368ULL);

} // namespace

bool RandomXEngine::initialize(const RxEngineConfig &config, std::string &error)
{
#ifndef RANDOMX_HAS_V2
    if (config.v2) {
        error = "this RandomX build has no RANDOMX_FLAG_V2 support";
        return false;
    }
#endif

    const std::uint64_t needed = kCacheSize + (config.fullDataset ? kDatasetSize : 0);
    if (needed > std::numeric_limits<std::size_t>::max()) {
        error = "RandomX fast mode requires a 64-bit process";
        return false;
    }
    if (!m_memory.canReserve(static_cast<std::size_t>(needed))) {
        error = "not enough available RAM for RandomX (need about " +
                std::to_string(needed >> 20) + " MiB)";
        return false;
    }

    m_config = config;
    m_initialized = true;
    error.clear();
    return true;
}

randomx_flags RandomXEngine::vmFlags() const
{
    randomx_flags flags = RANDOMX_FLAG_DEFAULT;

    if (m_config.largePages) flags |= RANDOMX_FLAG_LARGE_PAGES;
    if (m_config.jit)       flags |= RANDOMX_FLAG_JIT;
    if (m_config.hwAes)     flags |= RANDOMX_FLAG_HARD_AES;
    if (m_config.fullDataset) flags |= RANDOMX_FLAG_FULL_MEM;
    if (m_config.secureJit) flags |= RANDOMX_FLAG_SECURE;

#ifdef RANDOMX_HAS_V2
    if (m_config.v2) flags |= RANDOMX_FLAG_V2;
#endif

    if (m_config.oneGbPages) {
        // 1 GiB pages are requested through the large-page flag in upstream
        // RandomX; the OS decides whether they are actually available.
        flags |= RANDOMX_FLAG_LARGE_PAGES;
    }
    return flags;
}

bool RandomXEngine::setSeed(const std::uint8_t seed[32], std::string &error)
{
    if (!m_initialized) {
        error = "engine not initialized";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_epoch && std::memcmp(m_seed.data(), seed, 32) == 0 && m_config.v2 == m_v2Epoch) {
        return true; // same seed hash: Monero reuses the cache for 2048 blocks
    }

    randomx_flags recommended = randomx_get_flags();
    randomx_flags cacheFlags = RANDOMX_FLAG_DEFAULT;
    if (m_config.jit) cacheFlags |= RANDOMX_FLAG_JIT;
    if (m_config.largePages) cacheFlags |= RANDOMX_FLAG_LARGE_PAGES;
    cacheFlags |= (recommended & RANDOMX_FLAG_ARGON2_SSSE3);
    cacheFlags |= (recommended & RANDOMX_FLAG_ARGON2_AVX2);
#ifdef RANDOMX_HAS_V2
    if (m_config.v2) cacheFlags |= RANDOMX_FLAG_V2;
#endif

    randomx_cache *cache = randomx_alloc_cache(cacheFlags);
    if (!cache && m_config.jit) {
        logWarn("randomx", "JIT cache allocation failed, retrying without JIT");
        cacheFlags = static_cast<randomx_flags>(static_cast<int>(cacheFlags) & ~static_cast<int>(RANDOMX_FLAG_JIT));
        cache = randomx_alloc_cache(cacheFlags);
    }
    if (!cache) {
        error = "randomx_alloc_cache failed";
        return false;
    }

    randomx_init_cache(cache, seed, 32);

    randomx_dataset *dataset = nullptr;
    if (m_config.fullDataset) {
        randomx_flags datasetFlags = m_config.largePages ? RANDOMX_FLAG_LARGE_PAGES : RANDOMX_FLAG_DEFAULT;
        dataset = randomx_alloc_dataset(datasetFlags);
        if (!dataset) {
            randomx_release_cache(cache);
            error = "randomx_alloc_dataset failed (fast mode needs ~2.1 GiB)";
            return false;
        }

        const unsigned long count = randomx_dataset_item_count();
        const int workers = m_config.initThreads > 0 ? m_config.initThreads : 1;

        if (workers <= 1) {
            randomx_init_dataset(dataset, cache, 0, count);
        } else {
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(workers));
            for (int i = 0; i < workers; ++i) {
                const unsigned long start = count * static_cast<unsigned long>(i) / static_cast<unsigned long>(workers);
                const unsigned long end = count * static_cast<unsigned long>(i + 1) / static_cast<unsigned long>(workers);
                pool.emplace_back([dataset, cache, start, end]() {
                    randomx_init_dataset(dataset, cache, start, end - start);
                });
            }
            for (auto &thread : pool) {
                thread.join();
            }
        }
    }

    m_epoch = std::make_shared<RxEpoch>(cache, dataset);
    std::memcpy(m_seed.data(), seed, 32);
    m_v2Epoch = m_config.v2;
    ++m_epochId;
    error.clear();
    return true;
}

std::unique_ptr<RxVm> RandomXEngine::createVm(std::string &error)
{
    std::shared_ptr<RxEpoch> epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = m_epoch;
    }
    if (!epoch) {
        error = "no RandomX epoch (seed hash not set yet)";
        return nullptr;
    }

    randomx_flags flags = vmFlags();
    randomx_vm *vm = randomx_create_vm(flags, epoch->cache, epoch->dataset);

    if (!vm && ((flags & RANDOMX_FLAG_JIT) || (flags & RANDOMX_FLAG_HARD_AES))) {
        logWarn("randomx", "VM creation with JIT/AES failed, retrying with portable flags");
        flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_JIT));
        flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_HARD_AES));
        flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_SECURE));
        vm = randomx_create_vm(flags, epoch->cache, epoch->dataset);
    }
    if (!vm) {
        error = "randomx_create_vm failed";
        return nullptr;
    }

    auto result = std::unique_ptr<RxVm>(new RxVm());
    result->vm = vm;
    result->epoch = std::move(epoch);
    error.clear();
    return result;
}

void RandomXEngine::hash(RxVm &vm, const std::uint8_t *input, std::size_t size, std::uint8_t out[32]) const
{
    randomx_calculate_hash(vm.vm, input, size, out);
}

void RandomXEngine::hashBatch(RxVm &vm,
                              const std::uint8_t *inputs,
                              std::size_t count,
                              std::size_t inputSize,
                              std::uint8_t *outputs) const
{
    if (count == 0) return;

    randomx_calculate_hash_first(vm.vm, inputs, inputSize);
    for (std::size_t i = 1; i < count; ++i) {
        randomx_calculate_hash_next(vm.vm, inputs + i * inputSize, inputSize, outputs + (i - 1) * 32);
    }
    randomx_calculate_hash_last(vm.vm, outputs + (count - 1) * 32);
}

bool RandomXEngine::selfTest(std::string &error) const
{
    std::string vmError;
    auto vm = const_cast<RandomXEngine *>(this)->createVm(vmError);
    if (!vm) {
        error = vmError;
        return false;
    }

    std::uint8_t blob[76]{};
    for (std::size_t i = 0; i < sizeof(blob); ++i) {
        blob[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }

    Hash a{};
    Hash b{};
    hash(*vm, blob, sizeof(blob), a.data());
    hash(*vm, blob, sizeof(blob), b.data());
    if (a != b) {
        error = "RandomX is not deterministic on the same VM";
        return false;
    }

    auto other = const_cast<RandomXEngine *>(this)->createVm(vmError);
    if (!other) {
        error = vmError;
        return false;
    }
    Hash c{};
    hash(*other, blob, sizeof(blob), c.data());
    if (a != c) {
        error = "two independent RandomX VMs produced different hashes";
        return false;
    }

    error.clear();
    return true;
}

std::array<std::uint8_t, 32> RandomXEngine::seed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_seed;
}

bool RandomXEngine::hasEpoch() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_epoch != nullptr;
}

std::uint64_t RandomXEngine::epochId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_epochId;
}

} // namespace xmr
