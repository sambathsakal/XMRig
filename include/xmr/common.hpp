#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace xmr {

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;

inline int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool fromHex(const std::string &text, Bytes &out)
{
    if (text.size() % 2 != 0) {
        return false;
    }

    out.clear();
    out.reserve(text.size() / 2);

    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }

    return true;
}

inline std::string toHex(const std::uint8_t *data, size_t size)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

inline std::string toHex(const Bytes &data)
{
    return toHex(data.data(), data.size());
}

inline std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });
    return value;
}

inline std::uint32_t loadLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t loadLe64(const std::uint8_t *p)
{
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) |
           (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) |
           (static_cast<std::uint64_t>(p[7]) << 56);
}

inline void storeLe32(std::uint8_t *p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void storeLe64(std::uint8_t *p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (i * 8));
    }
}

inline std::uint64_t steadyMillis()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::atomic<int> &logLevel()
{
    static std::atomic<int> level{1};
    return level;
}

template<typename... Args>
inline void logLine(int level, const char *tag, Args &&... args)
{
    if (level > logLevel().load(std::memory_order_relaxed)) {
        return;
    }

    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    ::localtime_s(&tmv, &t);
#else
    ::localtime_r(&t, &tmv);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmv, "%H:%M:%S") << " ["
        << (level == 0 ? "err" : (level == 1 ? "inf" : "dbg")) << "] <" << tag << "> ";
    (oss << ... << std::forward<Args>(args));
    std::cout << oss.str() << std::endl;
}

template<typename... Args>
inline void logInfo(const char *tag, Args &&... args) { logLine(1, tag, std::forward<Args>(args)...); }
template<typename... Args>
inline void logWarn(const char *tag, Args &&... args) { logLine(0, tag, std::forward<Args>(args)...); }
template<typename... Args>
inline void logError(const char *tag, Args &&... args) { logLine(0, tag, std::forward<Args>(args)...); }
template<typename... Args>
inline void logDebug(const char *tag, Args &&... args) { logLine(2, tag, std::forward<Args>(args)...); }

inline std::string formatHashrate(double h)
{
    if (h >= 1000000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (h / 1000000.0) << " MH/s";
        return oss.str();
    }
    if (h >= 1000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (h / 1000.0) << " kH/s";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << h << " H/s";
    return oss.str();
}

} // namespace xmr
