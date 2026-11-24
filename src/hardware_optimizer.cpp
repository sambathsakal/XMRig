#include "hardware_optimizer.hpp"

#include "common.hpp"
#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#   include <windows.h>
#   if defined(_MSC_VER)
#       include <intrin.h>
#   elif defined(__x86_64__) || defined(__i386__)
#       include <cpuid.h>
#   endif
#else
#   include <sched.h>
#   include <sys/resource.h>
#   include <unistd.h>
#   if defined(__x86_64__) || defined(__i386__)
#       include <cpuid.h>
#   endif
#endif

namespace xmr {

namespace {

bool jitAvailable()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64) || defined(__riscv)
    return true;
#else
    return false;
#endif
}

int cacheSizeLinux(int level)
{
    const std::string path = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(level) + "/size";
    std::ifstream in(path);
    std::string value;
    in >> value;
    if (value.empty()) return 0;
    const std::size_t k = value.find('K');
    if (k != std::string::npos) {
        return static_cast<int>(std::atoi(value.substr(0, k).c_str())) * 1024;
    }
    const std::size_t m = value.find('M');
    if (m != std::string::npos) {
        return static_cast<int>(std::atoi(value.substr(0, m).c_str())) * 1024 * 1024;
    }
    return std::atoi(value.c_str());
}

std::string valueAfterColon(const std::string &line)
{
    const std::size_t pos = line.find(':');
    if (pos == std::string::npos) return {};
    std::string value = line.substr(pos + 1);
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

} // namespace

HardwareOptimizer &HardwareOptimizer::instance()
{
    static HardwareOptimizer optimizer;
    return optimizer;
}

bool HardwareOptimizer::detect(std::string &note)
{
    if (m_detected) {
        note = "hardware already detected";
        return true;
    }

    HardwareProfile profile;
    profile.cpu.jitSupported = jitAvailable();

#if defined(_WIN32)
#   if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    const int maxLeaf = registers[0];
    char vendor[13]{};
    if (maxLeaf >= 1) {
        __cpuid(registers, 0);
        std::memcpy(vendor,      &registers[1], 4);
        std::memcpy(vendor + 4,  &registers[3], 4);
        std::memcpy(vendor + 8,  &registers[2], 4);
        profile.cpu.vendor = vendor;
        __cpuid(registers, 1);
        profile.cpu.family = ((registers[0] >> 8) & 0x0f) + ((registers[0] >> 20) & 0xff);
        profile.cpu.model  = ((registers[0] >> 4) & 0x0f) + (((registers[0] >> 12) & 0x0f) << 4);
        profile.cpu.sse2  = (registers[3] & (1 << 26)) != 0;
        profile.cpu.aes   = (registers[2] & (1 << 25)) != 0;
        if (maxLeaf >= 7) {
            int leaf7[4]{};
            __cpuidex(leaf7, 7, 0);
            profile.cpu.avx2  = (leaf7[1] & (1 << 5)) != 0;
            profile.cpu.bmi2  = (leaf7[1] & (1 << 8)) != 0;
            profile.cpu.avx512 = (leaf7[1] & (1 << 16)) != 0;
            profile.cpu.sha    = (leaf7[1] & (1 << 29)) != 0;
            if (leaf7[0] >= 1) {
                int leaf7_1[4]{};
                __cpuidex(leaf7_1, 7, 1);
                profile.cpu.vaes = (leaf7_1[3] & (1 << 9)) != 0;
            }
        }
    }
#   elif defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    const unsigned int maxLeaf = __get_cpuid_max(0, nullptr);
    if (maxLeaf >= 1) {
        char vendor[13]{};
        __cpuid(0, eax, ebx, ecx, edx);
        std::memcpy(vendor,      &ebx, 4);
        std::memcpy(vendor + 4,  &edx, 4);
        std::memcpy(vendor + 8,  &ecx, 4);
        profile.cpu.vendor = vendor;

        __cpuid(1, eax, ebx, ecx, edx);
        profile.cpu.family = static_cast<int>(((eax >> 8) & 0x0f) + ((eax >> 20) & 0xff));
        profile.cpu.model  = static_cast<int>(((eax >> 4) & 0x0f) + (((eax >> 12) & 0x0f) << 4));
        profile.cpu.sse2  = (edx & (1u << 26)) != 0;
        profile.cpu.aes   = (ecx & (1u << 25)) != 0;

        if (maxLeaf >= 7) {
            unsigned int b7 = 0, c7 = 0, d7 = 0;
            __cpuid_count(7, 0, eax, b7, c7, d7);
            profile.cpu.avx2   = (b7 & (1u << 5)) != 0;
            profile.cpu.bmi2   = (b7 & (1u << 8)) != 0;
            profile.cpu.avx512 = (b7 & (1u << 16)) != 0;
            profile.cpu.sha    = (b7 & (1u << 29)) != 0;
            if (eax >= 1) {
                unsigned int a71 = 0, b71 = 0, c71 = 0, d71 = 0;
                __cpuid_count(7, 1, a71, b71, c71, d71);
                profile.cpu.vaes = (d71 & (1u << 9)) != 0;
            }
        }
    }
#   endif
    profile.cpu.brand = "Windows CPU";

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    profile.topology.logical = static_cast<int>(systemInfo.dwNumberOfProcessors);

    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (length) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(buffer.data(), &length)) {
            for (const auto &entry : buffer) {
                if (entry.Relationship == RelationProcessorCore) {
                    ++profile.topology.physical;
                    for (int bit = 0; bit < static_cast<int>(sizeof(KAFFINITY) * 8); ++bit) {
                        if (entry.ProcessorMask & (static_cast<KAFFINITY>(1) << bit)) {
                            profile.physicalCoreIds.push_back(bit);
                            break;
                        }
                    }
                } else if (entry.Relationship == RelationCache) {
                    if (entry.Cache.Level == 1) profile.topology.l1Bytes = static_cast<int>(entry.Cache.Size);
                    if (entry.Cache.Level == 2) profile.topology.l2Bytes = static_cast<int>(entry.Cache.Size);
                    if (entry.Cache.Level == 3) profile.topology.l3Bytes = static_cast<int>(entry.Cache.Size);
                }
            }
        }
    }
#else
#   if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) >= 1) {
        char vendor[13]{};
        __cpuid(0, eax, ebx, ecx, edx);
        std::memcpy(vendor,      &ebx, 4);
        std::memcpy(vendor + 4,  &edx, 4);
        std::memcpy(vendor + 8,  &ecx, 4);
        profile.cpu.vendor = vendor;

        __cpuid(1, eax, ebx, ecx, edx);
        profile.cpu.family = static_cast<int>(((eax >> 8) & 0x0f) + ((eax >> 20) & 0xff));
        profile.cpu.model  = static_cast<int>(((eax >> 4) & 0x0f) + (((eax >> 12) & 0x0f) << 4));
        profile.cpu.sse2  = (edx & (1u << 26)) != 0;
        profile.cpu.aes   = (ecx & (1u << 25)) != 0;

        if (__get_cpuid_max(0, nullptr) >= 7) {
            unsigned int b7 = 0, c7 = 0, d7 = 0;
            __cpuid_count(7, 0, eax, b7, c7, d7);
            profile.cpu.avx2   = (b7 & (1u << 5)) != 0;
            profile.cpu.bmi2   = (b7 & (1u << 8)) != 0;
            profile.cpu.avx512 = (b7 & (1u << 16)) != 0;
            profile.cpu.sha    = (b7 & (1u << 29)) != 0;
            if (eax >= 1) {
                unsigned int a71 = 0, b71 = 0, c71 = 0, d71 = 0;
                __cpuid_count(7, 1, a71, b71, c71, d71);
                profile.cpu.vaes = (d71 & (1u << 9)) != 0;
            }
        }
    }
#   endif

    profile.topology.logical = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    profile.topology.l1Bytes = cacheSizeLinux(1);
    profile.topology.l2Bytes = cacheSizeLinux(2);
    profile.topology.l3Bytes = cacheSizeLinux(3);

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::set<std::pair<int, int>> uniqueCores;
    int physicalId = 0;
    int coreId = 0;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0 && profile.cpu.brand.empty()) {
            profile.cpu.brand = valueAfterColon(line);
        } else if (line.find("vendor_id") == 0 && profile.cpu.vendor.empty()) {
            profile.cpu.vendor = valueAfterColon(line);
        } else if (line.find("cpu family") == 0) {
            profile.cpu.family = std::atoi(valueAfterColon(line).c_str());
        } else if (line.find("model") == 0 && line.find("model name") != 0) {
            profile.cpu.model = std::atoi(valueAfterColon(line).c_str());
        } else if (line.find("physical id") == 0) {
            physicalId = std::atoi(valueAfterColon(line).c_str());
        } else if (line.find("core id") == 0) {
            coreId = std::atoi(valueAfterColon(line).c_str());
            uniqueCores.emplace(physicalId, coreId);
        } else if (line.find("flags") == 0 || line.find("Features") == 0) {
            const std::string flags = valueAfterColon(line);
            if (flags.find(" aes ") != std::string::npos || flags.find(" aes") == 0) profile.cpu.aes = true;
            if (flags.find("avx2") != std::string::npos) profile.cpu.avx2 = true;
            if (flags.find("avx512f") != std::string::npos) profile.cpu.avx512 = true;
            if (flags.find("vaes") != std::string::npos) profile.cpu.vaes = true;
            if (flags.find("sha_ni") != std::string::npos || flags.find(" sha ") != std::string::npos) profile.cpu.sha = true;
            if (flags.find("bmi2") != std::string::npos) profile.cpu.bmi2 = true;
        }
    }
    if (!uniqueCores.empty()) {
        profile.topology.physical = static_cast<int>(uniqueCores.size());
    }
#endif

    if (profile.topology.physical <= 0) {
        profile.topology.physical = profile.topology.logical > 0 ? profile.topology.logical : 1;
    }
    if (profile.physicalCoreIds.empty()) {
        for (int i = 0; i < profile.topology.physical; ++i) profile.physicalCoreIds.push_back(i);
    }
    profile.topology.sockets = std::max(1, profile.topology.logical / profile.topology.physical);

    {
        std::ostringstream oss;
        oss << "CPU " << (profile.cpu.brand.empty() ? profile.cpu.vendor : profile.cpu.brand)
            << ", " << profile.topology.physical << " physical / "
            << profile.topology.logical << " logical cores, "
            << (profile.cpu.aes ? "AES-NI " : "no AES-NI ")
            << (profile.cpu.avx2 ? "AVX2 " : "")
            << (profile.cpu.avx512 ? "AVX-512F " : "")
            << (profile.cpu.vaes ? "VAES " : "")
            << (profile.cpu.sha ? "SHA-NI " : "")
            << (profile.cpu.bmi2 ? "BMI2 " : "")
            << (profile.cpu.jitSupported ? "JIT-capable" : "interpreter only");
        note = oss.str();
    }

    m_profile = std::move(profile);
    m_detected = true;
    return true;
}

HardwarePlan HardwareOptimizer::plan(const Config &config, bool largePagesAvailable) const
{
    HardwarePlan result;
    const Topology &topo = m_profile.topology;

    int threads = config.threads;
    if (threads <= 0) {
        threads = config.mode == "light" ? topo.logical : topo.physical;
    }
    if (threads <= 0) threads = 1;

    if (config.maxThreadsHint > 0 && config.maxThreadsHint < 100) {
        const int cap = std::max(1, topo.physical * config.maxThreadsHint / 100);
        threads = std::min(threads, cap);
    }
    threads = std::min(threads, std::max(1, topo.logical));

    // Each RandomX VM needs its own 2 MiB L3 scratchpad working set.
    if (topo.l3Bytes >= (2 << 20)) {
        threads = std::min(threads, std::max(1, topo.l3Bytes / (2 << 20)));
    }

    result.threads = threads;
    result.jit = config.jit && m_profile.cpu.jitSupported;
    result.hwAes = config.hwAes && m_profile.cpu.aes;
    result.largePages = largePagesAvailable;
    result.oneGbPages = config.oneGbPages;
    result.fullDataset = config.mode != "light";
    result.v2 = config.v2;
    result.secureJit = config.secureJit;
    // Dataset generation is memory-bandwidth bound. Use about half the physical
    // cores, capped at 8, independent of the number of mining threads.
    result.initThreads = config.initThreads > 0
        ? config.initThreads
        : std::max(1, std::min(8, topo.physical / 2));
    result.priority = 1;

    const auto &cores = m_profile.physicalCoreIds;
    result.coreIds.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        result.coreIds.push_back(cores[static_cast<std::size_t>(i % cores.size())]);
    }

    std::ostringstream oss;
    oss << "plan: " << threads << " thread(s), "
        << (result.fullDataset ? "fast" : "light") << " mode, "
        << (result.jit ? "JIT" : "interpreter") << ", "
        << (result.hwAes ? "hardware AES" : "soft AES") << ", "
        << (result.largePages ? "large pages" : "normal pages");
    result.note = oss.str();
    return result;
}

bool HardwareOptimizer::bindCurrentThread(int core) const
{
    if (core < 0) {
        return false;
    }

#if defined(_WIN32)
    if (core >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << core) != 0;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core % 1024, &set);
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
#endif
}

bool HardwareOptimizer::raiseCurrentThreadPriority(int level) const
{
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), level > 0 ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL) != 0;
#else
    return ::setpriority(PRIO_PROCESS, 0, level > 0 ? -1 : 0) == 0;
#endif
}

std::string HardwareOptimizer::describe() const
{
    std::ostringstream oss;
    oss << m_profile.cpu.vendor << " " << m_profile.cpu.brand
        << " family " << m_profile.cpu.family << " model " << m_profile.cpu.model
        << " | logical=" << m_profile.topology.logical
        << " physical=" << m_profile.topology.physical
        << " L2=" << (m_profile.topology.l2Bytes >> 10) << " KiB"
        << " L3=" << (m_profile.topology.l3Bytes >> 20) << " MiB"
        << " aes=" << m_profile.cpu.aes
        << " avx2=" << m_profile.cpu.avx2
        << " avx512=" << m_profile.cpu.avx512
        << " vaes=" << m_profile.cpu.vaes
        << " sha=" << m_profile.cpu.sha
        << " bmi2=" << m_profile.cpu.bmi2;
    return oss.str();
}

} // namespace xmr
