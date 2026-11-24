#include "stratum.hpp"

#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(XMR_HAS_TLS)
#   include <openssl/err.h>
#   include <openssl/ssl.h>
#   include <openssl/x509v3.h>
#endif

#if defined(_WIN32)
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <errno.h>
#   include <fcntl.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <arpa/inet.h>
#   include <sys/time.h>
#endif

namespace xmr {

namespace {

#if defined(_WIN32)
bool netInit()
{
    static const bool ok = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#endif

void closeHandle(SocketHandle socket)
{
    if (socket == static_cast<SocketHandle>(-1)) return;
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
    ::closesocket(socket);
#else
    ::shutdown(socket, SHUT_RDWR);
    ::close(socket);
#endif
}

bool isTimeout()
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

bool setNonBlocking(SocketHandle socket, bool enabled)
{
#if defined(_WIN32)
    u_long mode = enabled ? 1 : 0;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0) return false;
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(socket, F_SETFL, wanted) == 0;
#endif
}

bool waitReadable(SocketHandle socket, int seconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);

    timeval timeout{};
    timeout.tv_sec = seconds;
    return ::select(static_cast<int>(socket) + 1, &readable, nullptr, nullptr, &timeout) > 0;
}

struct UrlPrefix {
    const char *text;
    bool tls;
};

const UrlPrefix kUrlPrefixes[] = {
    { "stratum+ssl://", true },
    { "stratum+tls://", true },
    { "ssl://", true },
    { "tls://", true },
    { "stratum+tcp://", false },
    { "stratum://", false },
    { "tcp://", false },
};

const UrlPrefix *findUrlPrefix(const std::string &url)
{
    for (const UrlPrefix &prefix : kUrlPrefixes) {
        if (url.compare(0, std::strlen(prefix.text), prefix.text) == 0) {
            return &prefix;
        }
    }
    return nullptr;
}

bool isIpLiteral(const std::string &host)
{
    if (host.empty()) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;

    addrinfo *resolved = nullptr;
    const bool numeric = ::getaddrinfo(host.c_str(), nullptr, &hints, &resolved) == 0;
    if (resolved) ::freeaddrinfo(resolved);
    return numeric;
}

#if defined(XMR_HAS_TLS)
std::string opensslError(const char *what)
{
    std::ostringstream oss;
    oss << what;
    if (const unsigned long code = ERR_get_error()) {
        char text[256];
        ERR_error_string_n(code, text, sizeof(text));
        oss << ": " << text;
    }
    return oss.str();
}
#endif

std::string splitHost(std::string url, std::string &port)
{
    if (const UrlPrefix *prefix = findUrlPrefix(url)) {
        url.erase(0, std::strlen(prefix->text));
    }

    if (!url.empty() && url.front() == '[') {
        const std::size_t close = url.find(']');
        if (close != std::string::npos) {
            port = (close + 1 < url.size() && url[close + 1] == ':')
                 ? url.substr(close + 2) : "3333";
            if (port.empty()) port = "3333";
            return url.substr(1, close - 1);
        }
    }

    const std::size_t colon = url.rfind(':');
    if (colon != std::string::npos) {
        port = url.substr(colon + 1);
        if (port.empty()) port = "3333";
        return url.substr(0, colon);
    }

    port = "3333";
    return url;
}

} // namespace

bool parseTargetHex(const std::string &hex, std::uint64_t &target)
{
    Bytes raw;
    if (!fromHex(hex, raw)) return false;

    if (raw.size() == 8) {
        target = loadLe64(raw.data());
        return target != 0;
    }
    if (raw.size() == 4) {
        const std::uint64_t compact = loadLe32(raw.data());
        if (compact == 0) return false;
        target = 0xFFFFFFFFFFFFFFFFULL / (0xFFFFFFFFULL / compact);
        return true;
    }
    return false;
}

std::uint64_t difficultyFromTarget(std::uint64_t target)
{
    return target ? (0xFFFFFFFFFFFFFFFFULL / target) : 0;
}

bool hashMeetsTarget(const std::uint8_t hash[32], std::uint64_t target)
{
    // Monero targets cover the most significant 64 bits of the little-endian
    // 256-bit hash, i.e. bytes 24..31.
    return loadLe64(hash + 24) <= target;
}

StratumClient::StratumClient(std::vector<PoolConfig> pools, int retries, int retryPause, JobCallback callback)
    : m_pools(std::move(pools)),
      m_retries(std::max(0, retries)),
      m_retryPause(std::max(1, retryPause)),
      m_jobCallback(std::move(callback))
{
    if (m_pools.empty()) {
        m_pools.emplace_back();
    }
}

StratumClient::~StratumClient()
{
    stop();
}

bool StratumClient::start()
{
    if (m_running.exchange(true)) {
        return false;
    }
    m_thread = std::thread(&StratumClient::run, this);
    return true;
}

void StratumClient::stop()
{
    m_running.store(false);
    closeSocket();
    {
        std::lock_guard<std::mutex> lock(m_outboxMutex);
        m_outbox.clear();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    closeTls();
}

bool StratumClient::openSocket(std::string &error)
{
    const std::string &url = currentPool().url;
    const UrlPrefix *prefix = findUrlPrefix(url);
    const bool useTls = currentPool().tls || (prefix != nullptr && prefix->tls);

    // A previous connection may still own an SSL object. It is freed here on
    // the receiver thread, never from stop() while SSL_read() is in flight.
    closeTls();

#if defined(_WIN32)
    if (!netInit()) {
        error = "WSAStartup failed";
        return false;
    }
#endif

    std::string port;
    const std::string host = splitHost(url, port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *resolved = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved);
    if (rc != 0) {
        error = std::string("DNS failure: ") + gai_strerror(rc);
        return false;
    }

    SocketHandle socket = static_cast<SocketHandle>(-1);
    for (addrinfo *it = resolved; it != nullptr; it = it->ai_next) {
        socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == static_cast<SocketHandle>(-1)) continue;
        if (::connect(socket, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) break;
        closeHandle(socket);
        socket = static_cast<SocketHandle>(-1);
    }
    ::freeaddrinfo(resolved);

    if (socket == static_cast<SocketHandle>(-1)) {
        error = "cannot connect to " + url;
        return false;
    }

    const int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
// TLS uses a nonblocking socket plus select(), because a blocking read that
// times out can be reported by OpenSSL as a protocol error.
if (!useTls) {
#ifdef SO_RCVTIMEO
#   if defined(_WIN32)
    DWORD timeout = 1000; // milliseconds on Windows
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#   else
    struct timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#   endif
#endif
}

    m_socket = socket;
    m_readBuffer.clear();

    if (useTls && !openTls(host, error)) {
        closeSocket();
        return false;
    }

    m_connected.store(true);
    error.clear();
    return true;
}

bool StratumClient::openTls(const std::string &host, std::string &error)
{
#if !defined(XMR_HAS_TLS)
    (void) host;
    error = "TLS support was not compiled into this build";
    return false;
#else
    const PoolConfig &pool = currentPool();

    m_tlsCtx = SSL_CTX_new(TLS_client_method());
    if (!m_tlsCtx) {
        error = opensslError("SSL_CTX_new failed");
        return false;
    }

    SSL_CTX_set_default_verify_paths(m_tlsCtx);
    SSL_CTX_set_verify_depth(m_tlsCtx, 4);
    SSL_CTX_set_verify(m_tlsCtx, pool.tlsVerify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    if (!pool.tlsVerify) {
        logWarn("stratum", "TLS certificate verification disabled for ", host);
    }

    m_tls = SSL_new(m_tlsCtx);
    if (!m_tls) {
        error = opensslError("SSL_new failed");
        closeTls();
        return false;
    }

    if (SSL_set_fd(m_tls, static_cast<int>(m_socket.load())) != 1) {
        error = opensslError("SSL_set_fd failed");
        closeTls();
        return false;
    }

    const bool ipLiteral = isIpLiteral(host);
    X509_VERIFY_PARAM *params = SSL_get0_param(m_tls);
    const int hostSet = ipLiteral
        ? X509_VERIFY_PARAM_set1_ip_asc(params, host.c_str())
        : X509_VERIFY_PARAM_set1_host(params, host.c_str(), 0);
    if (hostSet != 1) {
        error = opensslError("cannot set TLS hostname check");
        closeTls();
        return false;
    }

    if (!ipLiteral && SSL_set_tlsext_host_name(m_tls, host.c_str()) != 1) {
        error = opensslError("cannot set TLS SNI");
        closeTls();
        return false;
    }

    if (SSL_connect(m_tls) != 1) {
        error = opensslError("TLS handshake failed");
        closeTls();
        return false;
    }

    if (!setNonBlocking(m_socket.load(), true)) {
        error = "cannot switch TLS socket to nonblocking mode";
        closeTls();
        return false;
    }

    logDebug("stratum", "TLS connected to ", host, pool.tlsVerify ? " (verified)" : " (unverified)");
    error.clear();
    return true;
#endif
}

void StratumClient::closeTls()
{
#if defined(XMR_HAS_TLS)
    if (m_tls) {
        SSL_free(m_tls);
        m_tls = nullptr;
    }
    if (m_tlsCtx) {
        SSL_CTX_free(m_tlsCtx);
        m_tlsCtx = nullptr;
    }
#endif
}

void StratumClient::closeSocket()
{
    const SocketHandle socket = m_socket.exchange(static_cast<SocketHandle>(-1));
    closeHandle(socket);
    m_connected.store(false);
}

void StratumClient::onConnectionFailure(const std::string &reason)
{
    ++m_failures;
    logWarn("stratum", reason);

    // Shares queued for the previous login may carry a stale client id.
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_rpcId.clear();
    }
    m_keepaliveExtension = false;
    {
        std::lock_guard<std::mutex> outboxLock(m_outboxMutex);
        m_outbox.clear();
    }

    const int perPool = std::max(1, m_retries);
    if (m_failures % perPool == 0 && m_pools.size() > 1) {
        rotatePool();
    }
    waitBeforeRetry();
}

void StratumClient::rotatePool()
{
    closeTls();
    closeSocket();
    m_poolIndex = (m_poolIndex + 1) % m_pools.size();
    logInfo("stratum", "failover to pool #", m_poolIndex + 1, " (", currentPool().url, ")");

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_rpcId.clear();
    }
    m_keepaliveExtension = false;
    m_lastKeepalive = 0;

    std::lock_guard<std::mutex> outboxLock(m_outboxMutex);
    m_outbox.clear();
}

void StratumClient::waitBeforeRetry()
{
    const int seconds = std::min(60, m_retryPause);
    for (int i = 0; i < seconds && m_running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void StratumClient::drainOutbox()
{
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(m_outboxMutex);
        pending.swap(m_outbox);
    }
    while (!pending.empty() && m_running.load() && m_connected.load()) {
        sendLine(pending.front());
        pending.pop_front();
    }
}

bool StratumClient::sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lock(m_sendMutex);
    const SocketHandle socket = m_socket.load();
    if (socket == static_cast<SocketHandle>(-1)) return false;

    std::string packet = line;
    packet.push_back('\n');
    std::size_t sent = 0;
    int wants = 0;
    while (sent < packet.size()) {
#if defined(XMR_HAS_TLS)
        if (m_tls) {
            const int n = SSL_write(m_tls, packet.data() + sent,
                                    static_cast<int>(packet.size() - sent));
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                wants = 0;
                continue;
            }
            const int code = SSL_get_error(m_tls, n);
            if ((code == SSL_ERROR_WANT_WRITE || code == SSL_ERROR_WANT_READ) && ++wants < 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            return false;
        }
#endif
        const int n = ::send(socket, packet.data() + sent,
                             static_cast<int>(packet.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool StratumClient::readLine(std::string &line)
{
    char buffer[4096];
    while (m_running.load()) {
        const std::size_t newline = m_readBuffer.find('\n');
        if (newline != std::string::npos) {
            line = m_readBuffer.substr(0, newline);
            m_readBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }

        int n = 0;
#if defined(XMR_HAS_TLS)
        if (m_tls) {
            if (!waitReadable(m_socket.load(), 1)) {
                continue;
            }
            n = SSL_read(m_tls, buffer, static_cast<int>(sizeof(buffer)));
            if (n <= 0) {
                // Capture socket state before SSL_get_error(), which may query
                // the same OS error. SO_RCVTIMEO surfaces as SSL_ERROR_SYSCALL
                // with an empty OpenSSL error queue; treat it like plain TCP.
                const bool timedOut = isTimeout();
                const int code = SSL_get_error(m_tls, n);
                if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                if (code == SSL_ERROR_SYSCALL && timedOut && ERR_peek_error() == 0) {
                    continue;
                }
                if (code != SSL_ERROR_ZERO_RETURN) {
                    logWarn("stratum", "TLS read failed (SSL error ", code, "): ",
                            opensslError("no OpenSSL detail"));
                }
                closeSocket();
                return false;
            }
        } else
#endif
        {
            n = ::recv(m_socket.load(), buffer, sizeof(buffer), 0);
            if (n < 0 && isTimeout()) {
                continue;
            }
            if (n <= 0) {
                closeSocket();
                return false;
            }
        }

        m_readBuffer.append(buffer, static_cast<std::size_t>(n));
        if (m_readBuffer.size() > (1 << 20)) {
            closeSocket();
            return false;
        }
    }
    return false;
}

void StratumClient::run()
{
    std::string error;
    while (m_running.load()) {
        if (!m_connected.load()) {
            if (!openSocket(error)) {
                onConnectionFailure(error);
                continue;
            }

            const PoolConfig &pool = currentPool();
            json::Value request = json::Value::object();
            json::Value params = json::Value::object();
            params.set("login", json::Value::string(pool.user));
            params.set("pass", json::Value::string(pool.pass));
            params.set("agent", json::Value::string(pool.agent));
            if (!pool.rigId.empty()) params.set("rigid", json::Value::string(pool.rigId));
            request.set("id", json::Value::number(std::int64_t{1}));
            request.set("jsonrpc", json::Value::string("2.0"));
            request.set("method", json::Value::string("login"));
            request.set("params", std::move(params));

            if (!sendLine(json::dump(request, -1))) {
                closeSocket();
                onConnectionFailure("login send failed");
                continue;
            }
            logInfo("stratum", "connected to ", pool.url, " (pool ", m_poolIndex + 1, "/", m_pools.size(), ")");
        }

        std::string line;
        if (readLine(line)) {
            handleMessage(line);
        } else if (m_running.load()) {
            onConnectionFailure("pool connection lost");
            continue;
        }

        drainOutbox();

        if (m_connected.load() && m_keepaliveExtension && currentPool().keepalive &&
            steadyMillis() - m_lastKeepalive > 60000) {
            json::Value ping = json::Value::object();
            json::Value params = json::Value::object();
            params.set("id", json::Value::string(rpcId()));
            ping.set("id", json::Value::number(m_nextId.fetch_add(1)));
            ping.set("jsonrpc", json::Value::string("2.0"));
            ping.set("method", json::Value::string("keepalived"));
            ping.set("params", std::move(params));
            sendLine(json::dump(ping, -1));
            m_lastKeepalive = steadyMillis();
        }
    }
    closeSocket();
}

void StratumClient::handleMessage(const std::string &line)
{
    logDebug("stratum", "recv ", line);

    std::string parseError;
    const json::Value message = json::parse(line, parseError);
    if (!parseError.empty() || !message.isObject()) {
        logWarn("stratum", "bad JSON from pool: ", parseError);
        return;
    }

    const json::Value *method = message.find("method");
    if (method && method->isString()) {
        const std::string name = method->stringValue();
        const json::Value *params = message.find("params");
        handleNotification(name, params ? *params : json::Value::null());
        return;
    }

    const json::Value *id = message.find("id");
    if (!id || !id->isNumber()) return;

    const std::int64_t requestId = id->intValue();
    const json::Value *error = message.find("error");
    if (error && !error->isNull()) {
        const json::Value *messageText = error->find("message");
        logWarn("stratum", "pool error ", requestId, ": ",
                messageText ? messageText->stringValue("unknown") : "unknown");
        if (requestId == 1) {
            closeSocket();
            onConnectionFailure("login rejected by pool");
        }
        return;
    }

    const json::Value *result = message.find("result");
    if (!result) return;

    if (requestId == 1) {
        handleLoginResponse(*result);
    } else if (result->find("status")) {
        const json::Value *status = result->find("status");
        logInfo("stratum", "share ", requestId, " ", status->stringValue("OK"));
    }
}

void StratumClient::handleLoginResponse(const json::Value &result)
{
    if (!result.isObject()) {
        closeSocket();
        onConnectionFailure("invalid login response");
        return;
    }

    const json::Value *rpcId = result.find("id");
    if (!rpcId || rpcId->stringValue().empty()) {
        closeSocket();
        onConnectionFailure("login response has no client id");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_rpcId = rpcId->stringValue();
    }
    m_failures = 0;

    if (const json::Value *extensions = result.find("extensions"); extensions && extensions->isArray()) {
        for (const json::Value &extension : extensions->items) {
            if (extension.stringValue() == "keepalive") m_keepaliveExtension = true;
        }
    }

    m_lastKeepalive = steadyMillis();

    if (const json::Value *job = result.find("job"); job && job->isObject()) {
        Job parsed;
        if (parseJob(*job, parsed) && m_jobCallback) {
            auto shared = std::make_shared<const Job>(std::move(parsed));
            m_jobCallback(shared);
        } else {
            closeSocket();
            onConnectionFailure("invalid job in login response");
        }
    }
}

void StratumClient::handleNotification(const std::string &method, const json::Value &params)
{
    if (method == "job") {
        Job job;
        if (parseJob(params, job) && m_jobCallback) {
            m_jobCallback(std::make_shared<const Job>(std::move(job)));
        } else {
            closeSocket();
            onConnectionFailure("invalid job notification");
        }
        return;
    }

    if (method == "client.reconnect" && params.isArray() && params.items.size() >= 2) {
        const std::string host = params.items[0].stringValue();
        const std::string port = params.items[1].stringValue();
        if (!host.empty() && !port.empty()) {
            logInfo("stratum", "pool asked us to reconnect to ", host, ":", port);
            currentPool().url = host + ":" + port;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_rpcId.clear();
            }
            m_keepaliveExtension = false;
            {
                std::lock_guard<std::mutex> lock(m_outboxMutex);
                m_outbox.clear();
            }
            closeSocket();
        }
        return;
    }

    logDebug("stratum", "unhandled notification ", method);
}

bool StratumClient::parseJob(const json::Value &object, Job &job)
{
    const json::Value *id = object.find("job_id");
    const json::Value *blob = object.find("blob");
    const json::Value *target = object.find("target");
    if (!id || !blob || !target || !id->isString() || !blob->isString() || !target->isString()) {
        return false;
    }

    job = Job{};
    job.id = id->stringValue();
    if (!fromHex(blob->stringValue(), job.blob)) return false;
    if (job.blob.size() < kNonceOffset + kNonceSize || job.blob.size() > kMaxBlobSize) return false;
    if (!parseTargetHex(target->stringValue(), job.target)) return false;

    if (const json::Value *seed = object.find("seed_hash"); seed && seed->isString()) {
        Bytes seedBytes;
        if (fromHex(seed->stringValue(), seedBytes) && seedBytes.size() == 32) {
            std::memcpy(job.seed.data(), seedBytes.data(), 32);
            job.hasSeed = true;
        }
    }

    if (const json::Value *height = object.find("height"); height) {
        job.height = height->isNumber() ? height->uintValue()
                                        : static_cast<std::uint64_t>(std::strtoull(height->stringValue().c_str(), nullptr, 10));
    }
    if (const json::Value *algo = object.find("algo"); algo) {
        job.algo = algo->stringValue();
    }

    return job.valid();
}

bool StratumClient::submit(const std::string &jobId, std::uint32_t nonce, const std::uint8_t hash[32])
{
    const std::string clientId = rpcId();
    if (!connected() || clientId.empty()) return false;

    std::uint8_t nonceBytes[4]{};
    storeLe32(nonceBytes, nonce);

    json::Value request = json::Value::object();
    json::Value params = json::Value::object();
    params.set("id", json::Value::string(clientId));
    params.set("job_id", json::Value::string(jobId));
    params.set("nonce", json::Value::string(toHex(nonceBytes, sizeof(nonceBytes))));
    params.set("result", json::Value::string(toHex(hash, 32)));

    request.set("id", json::Value::number(m_nextId.fetch_add(1)));
    request.set("jsonrpc", json::Value::string("2.0"));
    request.set("method", json::Value::string("submit"));
    request.set("params", std::move(params));

    std::lock_guard<std::mutex> lock(m_outboxMutex);
    if (m_outbox.size() >= 256) {
        return false;
    }
    m_outbox.push_back(json::dump(request, -1));
    return true;
}

} // namespace xmr
