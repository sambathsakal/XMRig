#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xmr {

struct PoolConfig {
    std::string url = "127.0.0.1:3334";
    std::string user = "YOUR_WALLET_ADDRESS";
    std::string pass = "x";
    std::string agent = "xmr-rebuild/0.1";
    std::string rigId;
    bool tls = false;
    bool tlsVerify = true;
    bool keepalive = true;
};

struct Config {
    std::vector<PoolConfig> pools;

    bool cpuEnabled = true;
    int threads = -1;                 // -1 = auto
    int maxThreadsHint = 100;         // percent of physical cores
    std::string mode = "auto";        // auto | fast | light

    bool hugePages = true;
    bool oneGbPages = false;
    bool jit = true;
    bool hwAes = true;
    bool secureJit = false;
    bool v2 = false;                  // RandomX v2 flag (Monero fork dependent)
    bool msr = false;                 // opt-in MSR prefetcher tuning
    bool msrRestore = true;           // restore original MSR values on exit
    int initThreads = -1;             // -1 = auto

    int printTime = 60;
    int retries = 5;
    int retryPause = 5;
    int verbose = 1;
    std::string configPath;
};

Config defaultConfig();
bool loadConfigFile(const std::string &path, Config &config, std::string &error);
bool applyCommandLine(int argc, char **argv, Config &config, std::string &error, bool &help);
void printHelp();

} // namespace xmr
