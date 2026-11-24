#include "memory_manager.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#   include <windows.h>
#   include <malloc.h>
#else
#   include <sys/sysinfo.h>
#   include <unistd.h>
#endif

namespace xmr {

namespace {

std::vector<int> parseCpuList(const std::string &text)
{
    std::vector<int> result;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || text[i] == ' ')) ++i;
        if (i >= text.size()) break;

        const std::size_t dash = text.find('-', i);
        const std::size_t comma = text.find(',', i);
        const std::size_t end = std::min(dash, comma);
        const int first = std::atoi(text.substr(i, end == std::string::npos ? std::string::npos : end - i).c_str());

        if (dash != std::string::npos && (comma == std::string::npos || dash < comma)) {
            const std::size_t lastEnd = (comma == std::string::npos) ? text.size() : comma;
            const int last = std::atoi(text.substr(dash + 1, lastEnd - dash - 1).c_str());
            for (int cpu = first; cpu <= last && cpu < 4096; ++cpu) result.push_back(cpu);
            i = lastEnd;
        } else {
            result.push_back(first);
            i = (comma == std::string::npos) ? text.size() : comma;
        }
    }
    return result;
}

std::string readFileFirstLine(const char *path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

} // namespace

MemoryManager &MemoryManager::instance()
{
    static MemoryManager manager;
    return manager;
}

bool MemoryManager::initialize(bool wantLargePages, std::string &note)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    detectRam();
    detectPages();
    detectHugePages(wantLargePages, note);
    detectNuma();

    std::ostringstream oss;
    oss << "RAM " << (m_stats.totalRam >> 30) << " GiB total / "
        << (m_stats.availableRam >> 30) << " GiB available, NUMA nodes "
        << m_stats.numaNodes << ", base page " << m_stats.basePageSize << " B";
    if (m_stats.largePageSize) oss << ", large page " << (m_stats.largePageSize >> 20) << " MiB";
    if (m_stats.largePagePrivilege) oss << ", large pages enabled";
    if (m_stats.transparentHugePages) oss << ", THP active";
    note = oss.str();
    return true;
}

void MemoryManager::detectRam()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        m_stats.totalRam = status.ullTotalPhys;
        m_stats.availableRam = status.ullAvailPhys;
    }
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    while (meminfo >> key) {
        unsigned long long kb = 0;
        meminfo >> kb;
        if (key == "MemTotal:")       m_stats.totalRam = kb * 1024ULL;
        else if (key == "MemAvailable:") m_stats.availableRam = kb * 1024ULL;
        if (m_stats.totalRam && m_stats.availableRam) break;
        std::string unit;
        meminfo >> unit;
    }
    if (!m_stats.totalRam) {
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0) {
            m_stats.totalRam = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
        }
    }
#endif
}

void MemoryManager::detectPages()
{
#if defined(_WIN32)
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    if (systemInfo.dwPageSize > 0) {
        m_stats.basePageSize = systemInfo.dwPageSize;
    }
    const SIZE_T large = GetLargePageMinimum();
    if (large > 0 && large <= 0xffffffffULL) {
        m_stats.largePageSize = static_cast<std::uint32_t>(large);
    }
#else
    const long base = sysconf(_SC_PAGESIZE);
    if (base > 0) {
        m_stats.basePageSize = static_cast<std::uint32_t>(base);
    }

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    while (meminfo >> key) {
        if (key == "Hugepagesize:") {
            unsigned long long kb = 0;
            meminfo >> kb;
            if (kb > 0) {
                m_stats.largePageSize = static_cast<std::uint32_t>(kb * 1024ULL);
            }
            break;
        }
        std::string rest;
        std::getline(meminfo, rest);
    }
#endif
}

void MemoryManager::detectHugePages(bool want, std::string &note)
{
    if (!want) {
        note = "large pages disabled by configuration";
        return;
    }

#if defined(_WIN32)
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES privileges{};
        LUID luid{};
        if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid)) {
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Luid = luid;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
                GetLastError() == ERROR_SUCCESS) {
                m_stats.largePagePrivilege = true;
            }
        }
        CloseHandle(token);
    }
    if (!m_stats.largePagePrivilege) {
        note = "SeLockMemoryPrivilege unavailable (run as admin or use gpedit.msc -> Lock pages in memory)";
    }
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    unsigned long long hugeTotal = 0;
    while (meminfo >> key) {
        if (key == "HugePages_Total:") {
            meminfo >> hugeTotal;
            break;
        }
        std::string rest;
        std::getline(meminfo, rest);
    }
    m_stats.largePagePrivilege = hugeTotal > 0;

    const std::string thp = readFileFirstLine("/sys/kernel/mm/transparent_hugepage/enabled");
    m_stats.transparentHugePages = thp.find("[always]") != std::string::npos ||
                                   thp.find("[madvise]") != std::string::npos;
    if (!m_stats.largePagePrivilege && !m_stats.transparentHugePages) {
        note = "no preallocated hugepages and THP not active; RandomX will use normal pages";
    }
#endif
}

void MemoryManager::detectNuma()
{
    m_nodeCores.clear();
#if defined(_WIN32)
    DWORD highest = 0;
    if (GetNumaHighestNodeNumber(&highest) && highest < 64) {
        m_nodeCores.resize(static_cast<std::size_t>(highest) + 1);
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const int logical = static_cast<int>(systemInfo.dwNumberOfProcessors);
        for (int cpu = 0; cpu < logical; ++cpu) {
            UCHAR node = 0;
            if (GetNumaProcessorNode(static_cast<UCHAR>(cpu), &node) &&
                node != 0xff && static_cast<std::size_t>(node) < m_nodeCores.size()) {
                m_nodeCores[node].push_back(cpu);
            }
        }
    }
#else
    std::size_t node = 0;
    while (node < 64) {
        const std::string path = "/sys/devices/system/node/node" + std::to_string(node) + "/cpulist";
        std::ifstream in(path);
        if (!in) break;
        std::string list;
        std::getline(in, list);
        m_nodeCores.push_back(parseCpuList(list));
        ++node;
    }
#endif
    if (m_nodeCores.empty()) {
        m_nodeCores.push_back({});
    }
    m_stats.numaNodes = static_cast<int>(m_nodeCores.size());
}

bool MemoryManager::canReserve(std::size_t bytes) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stats.availableRam == 0) {
        return true;
    }
    const std::uint64_t margin = 512ULL << 20; // leave room for the OS and page tables
    const std::uint64_t need = static_cast<std::uint64_t>(bytes) + margin + m_stats.reserved;
    return need <= m_stats.availableRam;
}

void *MemoryManager::alignedAlloc(std::size_t bytes, std::size_t alignment)
{
    if (bytes == 0) {
        return nullptr;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }

    void *ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(bytes, alignment);
#else
    if (::posix_memalign(&ptr, alignment, bytes) != 0) {
        ptr = nullptr;
    }
#endif
    if (!ptr) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_allocations[ptr] = {bytes, alignment};
    m_stats.reserved += bytes;
    m_stats.peakReserved = std::max(m_stats.peakReserved, m_stats.reserved);
    return ptr;
}

void MemoryManager::alignedFree(void *ptr)
{
    if (!ptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_allocations.find(ptr);
    if (it == m_allocations.end()) {
        return;
    }
    m_stats.reserved -= it->second.first;
    m_allocations.erase(it);

#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

int MemoryManager::nodeForCore(int core) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t node = 0; node < m_nodeCores.size(); ++node) {
        if (std::find(m_nodeCores[node].begin(), m_nodeCores[node].end(), core) != m_nodeCores[node].end()) {
            return static_cast<int>(node);
        }
    }
    return 0;
}

void MemoryManager::prefetchPages(void *ptr, std::size_t bytes) const
{
    const std::size_t step = m_stats.basePageSize ? m_stats.basePageSize : 4096;
    auto *p = static_cast<volatile std::uint8_t *>(ptr);
    for (std::size_t i = 0; i < bytes; i += step) {
        (void) p[i];
    }
}

std::size_t MemoryManager::reservationCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_allocations.size();
}

} // namespace xmr
