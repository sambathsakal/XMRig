#include "msr_tuner.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   include <winioctl.h>
#else
#   include <fcntl.h>
#   include <unistd.h>
#endif

namespace xmr {

namespace {

#if defined(_WIN32)
#   ifndef METHOD_BUFFERED
#       define METHOD_BUFFERED 0
#   endif
#   ifndef FILE_ANY_ACCESS
#       define FILE_ANY_ACCESS 0
#   endif
#   ifndef CTL_CODE
#       define CTL_CODE(DeviceType, Function, Method, Access) \
            (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#   endif

constexpr DWORD kIoctlReadMsr  = CTL_CODE(40000, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlWriteMsr = CTL_CODE(40000, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS);

#pragma pack(push, 1)
struct WinRing0WriteInput {
    std::uint32_t reg;
    std::uint32_t lo;
    std::uint32_t hi;
};
#pragma pack(pop)
static_assert(sizeof(WinRing0WriteInput) == 12, "WinRing0 write layout must be 12 bytes");
#endif

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::vector<MsrItem> zen2Preset()
{
    return {
        { 0xC0011020ULL, 0x0004480000000000ULL, ~0ULL },
        { 0xC0011021ULL, 0x001c000200000040ULL, ~0x20ULL },
        { 0xC0011022ULL, 0xc000000401570000ULL, ~0ULL },
        { 0xC001102bULL, 0x2000cc10ULL, ~0ULL },
    };
}

std::vector<MsrItem> zen3Preset()
{
    return {
        { 0xC0011020ULL, 0x0004400000000000ULL, ~0ULL },
        { 0xC0011021ULL, 0x0004000000000040ULL, ~0x20ULL },
        { 0xC0011022ULL, 0x8680000401570000ULL, ~0ULL },
        { 0xC001102bULL, 0x2040cc10ULL, ~0ULL },
    };
}

std::vector<MsrItem> presetFor(const std::string &vendor, int family)
{
    const std::string v = lower(vendor);
    if (v.find("amd") != std::string::npos || v.find("authenticamd") != std::string::npos) {
        if (family >= 0x19) return zen3Preset();
        if (family >= 0x17) return zen2Preset();
        return {};
    }
    if (v.find("intel") != std::string::npos || v.find("genuineintel") != std::string::npos) {
        return { { 0x1a4ULL, 0xfULL, ~0ULL } };
    }
    return {};
}

} // namespace

MsrTuner &MsrTuner::instance()
{
    static MsrTuner tuner;
    return tuner;
}

MsrTuner::~MsrTuner()
{
    restore();
}

bool MsrTuner::openBackend(std::string &note)
{
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(L"\\\\.\\WinRing0_1_2_0",
                                  GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        note = "MSR tuning needs the WinRing0 driver (\\\\.\\WinRing0_1_2_0) to be installed and running";
        return false;
    }
    m_driver = handle;
#else
    // /dev/cpu/N/msr files are opened in apply() once the core count is known.
#endif
    return true;
}

void MsrTuner::closeBackend()
{
#if defined(_WIN32)
    if (m_driver && m_driver != reinterpret_cast<void *>(INVALID_HANDLE_VALUE)) {
        ::CloseHandle(static_cast<HANDLE>(m_driver));
    }
    m_driver = nullptr;
#else
    for (int fd : m_fds) {
        if (fd >= 0) ::close(fd);
    }
    m_fds.clear();
#endif
}

bool MsrTuner::rdmsr(int core, std::uint32_t reg, std::uint64_t &value)
{
    value = 0;
#if defined(_WIN32)
    if (!m_driver || core < 0 || core >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;

    HANDLE thread = ::GetCurrentThread();
    const DWORD_PTR previous = ::SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(1) << core);
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(static_cast<HANDLE>(m_driver), kIoctlReadMsr,
                                      &reg, sizeof(reg), &value, sizeof(value),
                                      &returned, nullptr);
    if (previous != 0) ::SetThreadAffinityMask(thread, previous);
    return ok != 0;
#else
    if (core < 0 || static_cast<std::size_t>(core) >= m_fds.size() || m_fds[core] < 0) return false;
    return ::pread(m_fds[static_cast<std::size_t>(core)], &value, sizeof(value), reg) == static_cast<ssize_t>(sizeof(value));
#endif
}

bool MsrTuner::wrmsr(int core, std::uint32_t reg, std::uint64_t value)
{
#if defined(_WIN32)
    if (!m_driver || core < 0 || core >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;

    WinRing0WriteInput input{};
    input.reg = reg;
    input.lo = static_cast<std::uint32_t>(value & 0xffffffffULL);
    input.hi = static_cast<std::uint32_t>(value >> 32);

    HANDLE thread = ::GetCurrentThread();
    const DWORD_PTR previous = ::SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(1) << core);
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(static_cast<HANDLE>(m_driver), kIoctlWriteMsr,
                                      &input, sizeof(input), nullptr, 0,
                                      &returned, nullptr);
    if (previous != 0) ::SetThreadAffinityMask(thread, previous);
    return ok != 0;
#else
    if (core < 0 || static_cast<std::size_t>(core) >= m_fds.size() || m_fds[core] < 0) return false;
    return ::pwrite(m_fds[static_cast<std::size_t>(core)], &value, sizeof(value), reg) == static_cast<ssize_t>(sizeof(value));
#endif
}

bool MsrTuner::apply(bool enable, bool doRestore, const std::string &vendor, int family,
                     int logicalCores, std::string &note)
{
    if (!enable) {
        note = "MSR tuning disabled (opt in with --msr or randomx.msr)";
        return true;
    }
    if (m_applied) {
        note = "MSR tuning already applied";
        return true;
    }

    m_preset = presetFor(vendor, family);
    if (m_preset.empty()) {
        note = "no MSR prefetcher preset for this CPU family";
        return false;
    }
    if (logicalCores <= 0) {
        note = "cannot apply MSR tuning: logical core count is unknown";
        return false;
    }

    std::string backendNote;
    if (!openBackend(backendNote)) {
        note = backendNote;
        return false;
    }

#if !defined(_WIN32)
    m_fds.assign(static_cast<std::size_t>(logicalCores), -1);
    for (int core = 0; core < logicalCores; ++core) {
        const std::string path = "/dev/cpu/" + std::to_string(core) + "/msr";
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            note = "cannot open " + path + " (needs root and msr.allow_writes=1)";
            closeBackend();
            return false;
        }
        m_fds[static_cast<std::size_t>(core)] = fd;
    }
#endif

    m_restore = doRestore;
    m_saved.clear();
    // Mark as applied before the first write so restore() can undo a partial
    // preset if a later MSR write fails.
    m_applied = true;

    for (int core = 0; core < logicalCores; ++core) {
        for (const MsrItem &item : m_preset) {
            std::uint64_t oldValue = 0;
            if (m_restore && rdmsr(core, item.reg, oldValue)) {
                m_saved.push_back({ core, item.reg, oldValue });
            }

            std::uint64_t newValue = item.value;
            if (item.mask != ~0ULL) {
                newValue = (oldValue & item.mask) | (item.value & ~item.mask);
            }

            if (!wrmsr(core, item.reg, newValue)) {
                std::ostringstream oss;
                oss << "cannot set MSR 0x" << std::hex << item.reg << " on core " << core;
                note = oss.str();
                restore();
                return false;
            }
        }
    }

    m_applied = true;
    std::ostringstream oss;
    oss << "MSR preset applied to " << logicalCores << " logical core(s), " << m_preset.size()
        << " register(s) per core";
    if (m_restore && m_saved.size() != static_cast<std::size_t>(logicalCores) * m_preset.size()) {
        oss << " (partial restore data: rdmsr failed for some registers)";
    }
    note = oss.str();
    return true;
}

void MsrTuner::restore()
{
    if (m_applied && m_restore) {
        for (auto it = m_saved.rbegin(); it != m_saved.rend(); ++it) {
            wrmsr(it->core, it->reg, it->value);
        }
    }
    m_saved.clear();
    m_preset.clear();
    m_applied = false;
    closeBackend();
}

} // namespace xmr
