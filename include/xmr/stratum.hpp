#pragma once

#include "common.hpp"
#include "config.hpp"
#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
    using SocketHandle = SOCKET;
#else
#   include <sys/socket.h>
#   include <netdb.h>
#   include <unistd.h>
    using SocketHandle = int;
#endif

#if defined(XMR_HAS_TLS)
struct ssl_ctx_st;
struct ssl_st;
#endif

namespace xmr {

// Monero hashing blob layout: 4-byte nonce starts at byte 39.
constexpr std::size_t kNonceOffset = 39;
constexpr std::size_t kNonceSize = 4;
constexpr std::size_t kMaxBlobSize = 408;

bool parseTargetHex(const std::string &hex, std::uint64_t &target);
std::uint64_t difficultyFromTarget(std::uint64_t target);
bool hashMeetsTarget(const std::uint8_t hash[32], std::uint64_t target);

struct Job {
    std::string id;
    Bytes blob;
    std::uint64_t target = 0;
    std::array<std::uint8_t, 32> seed{};
    bool hasSeed = false;
    std::uint64_t height = 0;
    std::string algo;

    bool valid() const { return !id.empty() && !blob.empty() && target != 0 && blob.size() >= kNonceOffset + kNonceSize; }

    std::uint32_t nonce() const { return loadLe32(blob.data() + kNonceOffset); }
    void setNonce(std::uint32_t value) { storeLe32(blob.data() + kNonceOffset, value); }
    std::uint64_t difficulty() const { return difficultyFromTarget(target); }
    bool accepts(const std::uint8_t hash[32]) const { return hashMeetsTarget(hash, target); }
};

// Newline-delimited JSON-RPC 2.0 Stratum client (XMRig "Client.cpp" protocol).
class StratumClient {
public:
    using JobCallback = std::function<void(std::shared_ptr<const Job>)>;

    StratumClient(std::vector<PoolConfig> pools, int retries, int retryPause, JobCallback callback);
    ~StratumClient();

    bool start();
    void stop();

    bool submit(const std::string &jobId, std::uint32_t nonce, const std::uint8_t hash[32]);

    bool connected() const { return m_connected.load(std::memory_order_relaxed); }
    std::string rpcId() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_rpcId;
    }

    StratumClient(const StratumClient &) = delete;
    StratumClient &operator=(const StratumClient &) = delete;

private:
    const PoolConfig &currentPool() const { return m_pools[m_poolIndex]; }
    PoolConfig &currentPool() { return m_pools[m_poolIndex]; }

    void run();
    bool openSocket(std::string &error);
    bool openTls(const std::string &host, std::string &error);
    void closeSocket();
    void closeTls();

    void onConnectionFailure(const std::string &reason);
    void rotatePool();
    void waitBeforeRetry();
    void drainOutbox();

    bool sendLine(const std::string &line);
    bool readLine(std::string &line);

    void handleMessage(const std::string &line);
    void handleLoginResponse(const json::Value &result);
    void handleNotification(const std::string &method, const json::Value &params);
    bool parseJob(const json::Value &object, Job &job);

    std::vector<PoolConfig> m_pools;
    std::size_t m_poolIndex = 0;
    int m_retries = 5;
    int m_retryPause = 5;
    int m_failures = 0;
    JobCallback m_jobCallback;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    mutable std::mutex m_sendMutex;
    mutable std::mutex m_stateMutex;
    std::mutex m_outboxMutex;
    std::deque<std::string> m_outbox;

    std::atomic<SocketHandle> m_socket{static_cast<SocketHandle>(-1)};
    std::string m_readBuffer;

#if defined(XMR_HAS_TLS)
    ::ssl_ctx_st *m_tlsCtx = nullptr;
    ::ssl_st *m_tls = nullptr;
#endif

    std::string m_rpcId;
    std::atomic<std::int64_t> m_nextId{2};
    std::int64_t m_lastKeepalive = 0;
    bool m_keepaliveExtension = false;
};

} // namespace xmr
