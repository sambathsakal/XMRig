#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xmr {

struct MemoryStats {
    std::uint64_t totalRam = 0;
    std::uint64_t availableRam = 0;
    std::size_t reserved = 0;
    std::size_t peakReserved = 0;
    std::uint32_t basePageSize = 4096;
    std::uint32_t largePageSize = 0;
    bool largePagePrivilege = false;
    bool transparentHugePages = false;
    int numaNodes = 1;
};

// Owns the process-wide allocation policy and the small aligned buffers used
// by the miner. The RandomX library allocates its own cache/dataset/VM memory;
// this class decides whether those allocations are allowed and on which NUMA
// node the calling thread should run.
class MemoryManager {
public:
    static MemoryManager &instance();

    bool initialize(bool wantLargePages, std::string &note);

    bool canReserve(std::size_t bytes) const;

    void *alignedAlloc(std::size_t bytes, std::size_t alignment = 64);
    void alignedFree(void *ptr);

    template<typename T>
    T *alloc(std::size_t count, std::size_t alignment = 64)
    {
        return static_cast<T *>(alignedAlloc(count * sizeof(T), alignment));
    }

    int nodeForCore(int core) const;
    void prefetchPages(void *ptr, std::size_t bytes) const;

    const MemoryStats &stats() const { return m_stats; }
    std::size_t reservationCount() const;

    MemoryManager(const MemoryManager &) = delete;
    MemoryManager &operator=(const MemoryManager &) = delete;

private:
    MemoryManager() = default;

    void detectRam();
    void detectPages();
    void detectHugePages(bool want, std::string &note);
    void detectNuma();

    mutable std::mutex m_mutex;
    MemoryStats m_stats;
    std::unordered_map<void *, std::pair<std::size_t, std::size_t>> m_allocations;
    std::vector<std::vector<int>> m_nodeCores;
};

} // namespace xmr
