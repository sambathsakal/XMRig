#include "common.hpp"
#include "config.hpp"
#include "json.hpp"
#include "stratum.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace xmr;

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++failures;
    }
}

} // namespace

int main()
{
    // --- hex helpers -------------------------------------------------------
    Bytes raw;
    check(fromHex("00ff10aB", raw), "fromHex should accept mixed case");
    check(raw == Bytes({0x00, 0xff, 0x10, 0xab}), "fromHex bytes");
    check(!fromHex("abc", raw), "odd-length hex should fail");
    check(toHex(raw) == "00ff10ab", "toHex round-trip");

    std::uint32_t value = 0x01020304;
    std::uint8_t le[4]{};
    storeLe32(le, value);
    check(loadLe32(le) == value, "little endian 32-bit round-trip");

    // --- JSON --------------------------------------------------------------
    std::string error;
    const json::Value doc = json::parse(
        R"({"id":1,"result":{"job":{"job_id":"abc","blob":"0c","target":"ffffffffffffffff"},"extensions":["keepalive"]}})",
        error);
    check(error.empty(), "JSON parse error");
    check(doc.isObject(), "JSON root is object");
    const json::Value *result = doc.find("result");
    check(result && result->isObject(), "result object");
    const json::Value *job = result ? result->find("job") : nullptr;
    check(job && job->find("job_id") && job->find("job_id")->stringValue() == "abc", "job_id");
    check(result->find("extensions")->items[0].stringValue() == "keepalive", "extensions array");

    json::Value made = json::Value::object();
    made.set("quote", json::Value::string("a\"b\\c\n"));
    made.set("number", json::Value::number(std::uint64_t(18446744073709551615ULL)));
    const std::string dumped = json::dump(made);
    json::Value again = json::parse(dumped, error);
    check(error.empty(), "dumped JSON parses");
    check(again.find("quote")->stringValue() == "a\"b\\c\n", "string escaping round-trip");
    check(again.find("number")->uintValue() == 18446744073709551615ULL, "uint64 precision preserved");

    // --- stratum difficulty maths -----------------------------------------
    std::uint64_t target = 0;
    check(parseTargetHex("ffffffffffffffff", target) && target == 0xffffffffffffffffULL, "8-byte target");
    check(difficultyFromTarget(target) == 1, "difficulty 1");
    check(parseTargetHex("ffffff7f", target), "4-byte target");
    check(difficultyFromTarget(target) > 0, "compact target difficulty");

    Hash hash{};
    std::memset(hash.data(), 0xff, hash.size());
    storeLe64(hash.data() + 24, 0x0000000000000010ULL);
    check(hashMeetsTarget(hash.data(), 0x0000000000000020ULL), "hash below target");
    check(!hashMeetsTarget(hash.data(), 0x0000000000000008ULL), "hash above target");

    // --- live public-pool "real" preset ------------------------------------
    Config real = defaultConfig();
    const std::string wallet(95, '4');
    std::vector<char> walletBuffer(wallet.begin(), wallet.end());
    walletBuffer.push_back('\0');
    char program[] = "xmr-miner";
    char realFlag[] = "--real";
    char userFlag[] = "-u";
    char *realArgv[] = { program, realFlag, userFlag, walletBuffer.data() };
    bool help = false;
    check(applyCommandLine(4, realArgv, real, error, help), "real mode CLI parses");
    check(real.realMode, "real mode flag is set");
    check(real.pools[0].url == "stratum+ssl://pool.supportxmr.com:443", "real mode uses public TLS pool");
    check(real.pools[0].tls && real.pools[0].tlsVerify, "real mode enables verified TLS");
    check(real.mode == "fast", "real mode uses the full dataset");
    check(finalizeConfig(real, error), "real mode config validates: " + error);

    Config placeholder = real;
    placeholder.pools[0].user = "YOUR_WALLET_ADDRESS";
    check(!finalizeConfig(placeholder, error), "real mode rejects the placeholder wallet");

    Config local = real;
    local.pools[0].url = "127.0.0.1:3334";
    local.pools[0].tls = false;
    check(!finalizeConfig(local, error), "real mode rejects a local/mock pool");

    Config unverified = real;
    unverified.pools[0].tlsVerify = false;
    check(!finalizeConfig(unverified, error), "real mode rejects disabled TLS verification");

    if (failures == 0) {
        std::cout << "test_core: all checks passed" << std::endl;
    }
    return failures == 0 ? 0 : 1;
}
