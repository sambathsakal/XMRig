#include "common.hpp"
#include "config.hpp"
#include "miner.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char **argv)
{
    using namespace xmr;

    Config config = defaultConfig();
    std::string error;
    bool help = false;

    {
        std::ifstream probe("config.json");
        if (probe.good()) {
            if (!loadConfigFile("config.json", config, error)) {
                std::cerr << "config error: " << error << std::endl;
                return 1;
            }
        }
    }

    if (!applyCommandLine(argc, argv, config, error, help)) {
        std::cerr << "argument error: " << error << std::endl;
        printHelp();
        return 1;
    }
    if (help) {
        printHelp();
        return 0;
    }
    if (!finalizeConfig(config, error)) {
        std::cerr << "config error: " << error << std::endl;
        return 1;
    }

    logLevel().store(config.verbose);

    logInfo("main", "xmr-rebuild 0.1 (educational XMRig reconstruction)");
    Miner miner(std::move(config));
    return miner.run();
}
