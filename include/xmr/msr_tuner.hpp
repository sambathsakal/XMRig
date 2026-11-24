#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xmr {

struct MsrItem {
    std::uint32_t reg = 0;
    std::uint64_t value = 0;
    std::uint64_t mask = ~0ULL;
};

struct MsrSaved {
    int core = 0;
    std::uint32_t reg = 0;
    std::uint64_t value = 0;
};

// Opt-in AMD/Intel MSR tuning. Nothing is written unless apply() is called with
// enable=true. Original values are kept in memory and written back on restore()
// when msr-restore is enabled (the default).
class MsrTuner {
public:
    static MsrTuner &instance();

    bool apply(bool enable, bool doRestore, const std::string &vendor, int family,
               int logicalCores, std::string &note);
    void restore();

    ~MsrTuner();

    MsrTuner(const MsrTuner &) = delete;
    MsrTuner &operator=(const MsrTuner &) = delete;

private:
    MsrTuner() = default;

    bool openBackend(std::string &note);
    void closeBackend();
    bool rdmsr(int core, std::uint32_t reg, std::uint64_t &value);
    bool wrmsr(int core, std::uint32_t reg, std::uint64_t value);

    std::vector<MsrItem> m_preset;
    std::vector<MsrSaved> m_saved;
    bool m_applied = false;
    bool m_restore = true;

#if defined(_WIN32)
    void *m_driver = nullptr;   // HANDLE
#else
    std::vector<int> m_fds;
#endif
};

} // namespace xmr
