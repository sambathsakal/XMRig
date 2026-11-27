#include "config.hpp"

#include "common.hpp"
#include "json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace xmr {

Config defaultConfig()
{
    Config config;
    PoolConfig pool;
    config.pools.push_back(pool);
    return config;
}

namespace {

constexpr const char *kDefaultPoolUrl = "127.0.0.1:3334";
constexpr const char *kDefaultWallet  = "YOUR_WALLET_ADDRESS";
constexpr const char *kRealPoolUrl    = "stratum+ssl://pool.supportxmr.com:443";

bool startsWith(const std::string &text, const char *prefix)
{
    const std::string p(prefix);
    return text.compare(0, p.size(), p) == 0;
}

bool hasTlsPrefix(const std::string &url)
{
    return startsWith(url, "stratum+ssl://") || startsWith(url, "stratum+tls://") ||
           startsWith(url, "ssl://") || startsWith(url, "tls://");
}

bool hasPlainPrefix(const std::string &url)
{
    return startsWith(url, "stratum+tcp://") || startsWith(url, "stratum://") ||
           startsWith(url, "tcp://");
}

bool isLoopbackUrl(const std::string &url)
{
    const std::string u = lower(url);
    return u.find("127.0.0.1") != std::string::npos ||
           u.find("localhost") != std::string::npos ||
           u.find("[::1]") != std::string::npos;
}

// Structural check only; the pool performs the authoritative validation.
// A worker suffix such as wallet.worker1 is allowed.
bool looksLikeMoneroAddress(const std::string &user)
{
    const std::size_t dot = user.find('.');
    const std::string wallet = dot == std::string::npos ? user : user.substr(0, dot);
    if (wallet.size() != 95) return false;
    if (wallet.front() != '4' && wallet.front() != '8') return false;

    const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    for (const char c : wallet) {
        if (alphabet.find(c) == std::string::npos) return false;
    }
    return true;
}

void applyRealPreset(Config &config)
{
    config.realMode = true;
    if (config.pools.empty()) config.pools.emplace_back();

    for (PoolConfig &pool : config.pools) {
        if (pool.url.empty() || pool.url == kDefaultPoolUrl) {
            pool.url = kRealPoolUrl;
        }
        if (!hasTlsPrefix(pool.url) && !hasPlainPrefix(pool.url)) {
            pool.url = std::string("stratum+ssl://") + pool.url;
        }
        pool.tls = true;
        pool.tlsVerify = true;
    }

    if (config.mode == "auto") config.mode = "fast";
    config.hugePages = true;
}

const json::Value *member(const json::Value &v, const char *key)
{
    return v.find(key);
}

template<typename T>
T getOr(const json::Value &v, const char *key, T fallback)
{
    const json::Value *p = v.find(key);
    if (!p) return fallback;
    return static_cast<T>(p->uintValue(static_cast<std::uint64_t>(fallback)));
}

bool getBoolOr(const json::Value &v, const char *key, bool fallback)
{
    const json::Value *p = v.find(key);
    return p ? p->boolValue(fallback) : fallback;
}

std::string getStringOr(const json::Value &v, const char *key, const std::string &fallback)
{
    const json::Value *p = v.find(key);
    return p ? p->stringValue(fallback) : fallback;
}

int getIntOr(const json::Value &v, const char *key, int fallback)
{
    const json::Value *p = v.find(key);
    return p ? static_cast<int>(p->intValue(fallback)) : fallback;
}

PoolConfig parsePool(const json::Value &v, const PoolConfig &base)
{
    PoolConfig pool = base;
    pool.url = getStringOr(v, "url", pool.url);
    pool.user = getStringOr(v, "user", pool.user);
    pool.pass = getStringOr(v, "pass", pool.pass);
    pool.agent = getStringOr(v, "agent", pool.agent);
    pool.rigId = getStringOr(v, "rig-id", pool.rigId);
    pool.tls = getBoolOr(v, "tls", pool.tls);
    pool.tlsVerify = getBoolOr(v, "tls-verify", pool.tlsVerify);
    pool.keepalive = getBoolOr(v, "keepalive", pool.keepalive);
    return pool;
}

} // namespace

bool loadConfigFile(const std::string &path, Config &config, std::string &error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file: " + path;
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();

    std::string parseError;
    const json::Value root = json::parse(buffer.str(), parseError);
    if (!parseError.empty()) {
        error = "config JSON: " + parseError;
        return false;
    }
    if (!root.isObject()) {
        error = "config root must be an object";
        return false;
    }

    Config result;

    if (const json::Value *pools = root.find("pools"); pools && pools->isArray()) {
        result.pools.clear();
        for (const json::Value &entry : pools->items) {
            if (entry.isObject()) {
                result.pools.push_back(parsePool(entry, PoolConfig{}));
            }
        }
    }

    if (const json::Value *cpu = root.find("cpu"); cpu && cpu->isObject()) {
        result.cpuEnabled = getBoolOr(*cpu, "enabled", result.cpuEnabled);
        result.threads = getIntOr(*cpu, "threads", result.threads);
        result.maxThreadsHint = getIntOr(*cpu, "max-threads-hint", result.maxThreadsHint);
        result.mode = getStringOr(*cpu, "mode", result.mode);
        result.hugePages = getBoolOr(*cpu, "huge-pages", result.hugePages);
        result.jit = getBoolOr(*cpu, "jit", result.jit);
        result.hwAes = getBoolOr(*cpu, "hw-aes", result.hwAes);
    }

    if (const json::Value *rx = root.find("randomx"); rx && rx->isObject()) {
        result.initThreads = getIntOr(*rx, "init-threads", result.initThreads);
        result.oneGbPages = getBoolOr(*rx, "1gb-pages", result.oneGbPages);
        result.v2 = getBoolOr(*rx, "v2", result.v2);
        result.secureJit = getBoolOr(*rx, "secure-jit", result.secureJit);
        result.msr = getBoolOr(*rx, "msr", result.msr);
        result.msrRestore = getBoolOr(*rx, "msr-restore", result.msrRestore);
    }

    result.printTime = getIntOr(root, "print-time", result.printTime);
    result.retries = getIntOr(root, "retries", result.retries);
    result.retryPause = getIntOr(root, "retry-pause", result.retryPause);
    result.verbose = getIntOr(root, "verbose", result.verbose);

    if (getBoolOr(root, "real", false)) {
        applyRealPreset(result);
    }

    if (result.mode != "auto" && result.mode != "fast" && result.mode != "light") {
        error = "mode must be auto, fast or light";
        return false;
    }

    result.configPath = path;
    config = std::move(result);
    return true;
}

bool applyCommandLine(int argc, char **argv, Config &config, std::string &error, bool &help)
{
    help = false;
    const auto needValue = [&](int &i, const char *name, std::string &out) -> bool {
        if (i + 1 >= argc) {
            error = std::string(name) + " needs a value";
            return false;
        }
        out = argv[++i];
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            help = true;
            return true;
        }
        if (arg == "--config") {
            std::string path;
            if (!needValue(i, arg.c_str(), path)) return false;
            if (!loadConfigFile(path, config, error)) return false;
        } else if (arg == "-o" || arg == "--url") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].url = value;
        } else if (arg == "-u" || arg == "--user") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].user = value;
        } else if (arg == "-p" || arg == "--pass") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].pass = value;
        } else if (arg == "-t" || arg == "--threads") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            config.threads = std::atoi(value.c_str());
        } else if (arg == "--mode") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            config.mode = lower(value);
        } else if (arg == "--huge-pages") {
            config.hugePages = true;
        } else if (arg == "--no-huge-pages") {
            config.hugePages = false;
        } else if (arg == "--1gb-pages") {
            config.oneGbPages = true;
        } else if (arg == "--no-jit") {
            config.jit = false;
        } else if (arg == "--no-hw-aes") {
            config.hwAes = false;
        } else if (arg == "--real" || arg == "--real-mode") {
            applyRealPreset(config);
        } else if (arg == "--rx-v2") {
            config.v2 = true;
        } else if (arg == "--secure-jit") {
            config.secureJit = true;
        } else if (arg == "--msr") {
            config.msr = true;
        } else if (arg == "--msr-no-restore") {
            config.msrRestore = false;
        } else if (arg == "--tls") {
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].tls = true;
        } else if (arg == "--no-tls") {
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].tls = false;
        } else if (arg == "--tls-verify") {
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].tlsVerify = true;
        } else if (arg == "--no-tls-verify") {
            if (config.pools.empty()) config.pools.emplace_back();
            config.pools[0].tlsVerify = false;
        } else if (arg == "--print-time") {
            std::string value;
            if (!needValue(i, arg.c_str(), value)) return false;
            config.printTime = std::atoi(value.c_str());
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = 2;
        } else if (arg == "-q" || arg == "--quiet") {
            config.verbose = 0;
        } else {
            error = "unknown option: " + arg;
            return false;
        }
    }

    if (config.mode != "auto" && config.mode != "fast" && config.mode != "light") {
        error = "mode must be auto, fast or light";
        return false;
    }

    return true;
}

bool finalizeConfig(Config &config, std::string &error)
{
    if (!config.realMode) {
        error.clear();
        return true;
    }

    if (config.pools.empty()) {
        error = "real mode needs at least one pool";
        return false;
    }

    for (const PoolConfig &pool : config.pools) {
        if (pool.user.empty() || pool.user == kDefaultWallet || !looksLikeMoneroAddress(pool.user)) {
            error = "real mode needs a valid Monero wallet in --user/-u (placeholder not allowed; "
                    "worker suffixes like .worker are allowed)";
            return false;
        }
        if (!pool.tls && !hasTlsPrefix(pool.url)) {
            error = "real mode requires TLS: use a stratum+ssl:// URL or --tls";
            return false;
        }
        if (!pool.tlsVerify) {
            error = "real mode requires TLS certificate verification; remove --no-tls-verify";
            return false;
        }
        if (isLoopbackUrl(pool.url)) {
            error = "real mode cannot target a local/mock pool; supply a public pool with -o";
            return false;
        }
    }

    if (config.mode == "light") {
        error = "real mode requires full-dataset RandomX (use --mode fast)";
        return false;
    }

    error.clear();
    return true;
}

void printHelp()
{
    std::cout <<
        "xmr-miner - educational XMRig reconstruction (C++17)\n"
        "\n"
        "  -o, --url HOST:PORT     Stratum pool (default 127.0.0.1:3334)\n"
        "  -u, --user WALLET       Wallet / worker name\n"
        "  -p, --pass PASS         Pool password (usually x)\n"
        "  -t, --threads N         Worker threads (0 = auto)\n"
        "      --real              Live pool preset: TLS public pool, wallet required, full dataset\n"
        "      --mode auto|fast|light   RandomX dataset mode\n"
        "      --config FILE       JSON configuration file\n"
        "      --huge-pages        Request large pages (needs OS privilege)\n"
        "      --no-huge-pages     Disable large pages\n"
        "      --1gb-pages         Request 1 GiB pages when available\n"
        "      --no-jit            Use the portable interpreter instead of JIT\n"
        "      --no-hw-aes         Disable AES-NI path\n"
        "      --rx-v2             Set RANDOMX_FLAG_V2 (only for v2 chains)\n"
        "      --secure-jit        W^X JIT pages when supported\n"
        "      --msr               Opt in to AMD/Intel MSR prefetcher tuning\n"
        "      --msr-no-restore    Do not restore original MSR values on exit\n"
        "      --tls               Use TLS for the first pool (or use a tls:// URL)\n"
        "      --no-tls            Force plain TCP for the first pool\n"
        "      --tls-verify        Verify the pool certificate and hostname (default)\n"
        "      --no-tls-verify     Disable certificate/hostname verification (testing)\n"
        "      --print-time N      Hashrate report interval in seconds\n"
        "  -v, --verbose           Debug logging\n"
        "  -q, --quiet             Errors only\n"
        "  -h, --help              This help\n";
}

} // namespace xmr
