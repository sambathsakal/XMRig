#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xmr {

struct Config;

struct CpuFeatures {
    std::string vendor;
    std::string brand;
    int family = 0;
    int model = 0;
    bool aes = false;
    bool avx2 = false;
    bool avx512 = false;
    bool vaes = false;
    bool sha = false;
    bool sse2 = false;
    bool bmi2 = false;
    bool jitSupported = false;
};

struct Topology {
    int logical = 0;
    int physical = 0;
    int sockets = 1;
    int l1Bytes = 0;
    int l2Bytes = 0;
    int l3Bytes = 0;
};

struct HardwareProfile {
    CpuFeatures cpu;
    Topology topology;
    std::vector<int> physicalCoreIds;
};

struct HardwarePlan {
    int threads = 1;
    bool jit = false;
    bool hwAes = false;
    bool largePages = false;
    bool oneGbPages = false;
    bool fullDataset = true;
    bool v2 = false;
    bool secureJit = false;
    int initThreads = 1;
    int priority = 0;
    std::vector<int> coreIds;
    std::string note;
};

// Reads CPUID / OS topology, then turns the user configuration into a concrete
// launch plan: how many workers, which cores, and which RandomX flags.
class HardwareOptimizer {
public:
    static HardwareOptimizer &instance();

    bool detect(std::string &note);
    const HardwareProfile &profile() const { return m_profile; }

    HardwarePlan plan(const Config &config, bool largePagesAvailable) const;

    // Called from inside each worker thread.
    bool bindCurrentThread(int core) const;
    bool raiseCurrentThreadPriority(int level) const;

    std::string describe() const;

    HardwareOptimizer(const HardwareOptimizer &) = delete;
    HardwareOptimizer &operator=(const HardwareOptimizer &) = delete;

private:
    HardwareOptimizer() = default;

    HardwareProfile m_profile;
    bool m_detected = false;
};

} // namespace xmr
