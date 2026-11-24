// Minimal local Stratum pool used to smoke-test xmr-miner without a real pool.
// It sends one job with difficulty 1, so every hash is accepted as a share.

#include "common.hpp"
#include "json.hpp"
#include "stratum.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#if defined(XMR_HAS_TLS)
#   include <openssl/err.h>
#   include <openssl/ssl.h>
#endif

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif

using namespace xmr;

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket s) { if (s != kInvalid) { shutdown(s, SD_BOTH); closesocket(s); } }
bool netInit()
{
    static const bool ok = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(Socket s) { if (s != kInvalid) { shutdown(s, SHUT_RDWR); close(s); } }
bool netInit() { return true; }
#endif

#if defined(XMR_HAS_TLS)
SSL_CTX *g_tlsCtx = nullptr;
SSL *g_tls = nullptr;
#endif

bool sendAll(Socket socket, const std::string &text)
{
    std::string line = text;
    line.push_back('\n');
    std::size_t sent = 0;
    while (sent < line.size()) {
#if defined(XMR_HAS_TLS)
        if (g_tls) {
            const int n = SSL_write(g_tls, line.data() + sent, static_cast<int>(line.size() - sent));
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            const int code = SSL_get_error(g_tls, n);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            return false;
        }
#endif
        const int n = ::send(socket, line.data() + sent, static_cast<int>(line.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool readLine(Socket socket, std::string &buffer, std::string &line)
{
    char chunk[4096];
    while (true) {
        const std::size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }

        int n = 0;
#if defined(XMR_HAS_TLS)
        if (g_tls) {
            n = SSL_read(g_tls, chunk, static_cast<int>(sizeof(chunk)));
            if (n <= 0) {
                const int code = SSL_get_error(g_tls, n);
                if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                return false;
            }
        } else
#endif
        {
            n = ::recv(socket, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
}

json::Value jobObject()
{
    Bytes blob(76, 0);
    blob[0] = 0x0c; // Monero major version 12
    for (std::size_t i = 1; i < blob.size(); ++i) {
        blob[i] = static_cast<std::uint8_t>((i * 53 + 7) & 0xff);
    }
    for (std::size_t i = kNonceOffset; i < kNonceOffset + kNonceSize; ++i) {
        blob[i] = 0;
    }

    Bytes seed(32, 0xab);

    json::Value job = json::Value::object();
    job.set("job_id", json::Value::string("mock-1"));
    job.set("blob", json::Value::string(toHex(blob)));
    job.set("target", json::Value::string("ffffffffffffffff"));
    job.set("seed_hash", json::Value::string(toHex(seed)));
    job.set("height", json::Value::number(std::uint64_t(1234567)));
    job.set("algo", json::Value::string("rx/0"));
    return job;
}

void handleClient(Socket socket)
{
    std::string buffer;
    std::string line;
    bool loggedIn = false;

    while (readLine(socket, buffer, line)) {
        std::string error;
        const json::Value message = json::parse(line, error);
        if (!error.empty() || !message.isObject()) {
            continue;
        }

        const json::Value *method = message.find("method");
        if (!method || !method->isString()) continue;

        if (method->stringValue() == "login" && !loggedIn) {
            loggedIn = true;
            json::Value result = json::Value::object();
            result.set("id", json::Value::string("mock-client-id"));
            result.set("job", jobObject());
            result.set("extensions", json::Value::array());
            result.set("status", json::Value::string("OK"));

            json::Value reply = json::Value::object();
            reply.set("id", json::Value::number(std::int64_t{1}));
            reply.set("jsonrpc", json::Value::string("2.0"));
            reply.set("result", std::move(result));
            sendAll(socket, json::dump(reply, -1));
            logInfo("pool", "client logged in, job sent");
        } else if (method->stringValue() == "submit") {
            const json::Value *id = message.find("id");
            const json::Value *params = message.find("params");
            const json::Value *nonce = params ? params->find("nonce") : nullptr;
            const json::Value *hash = params ? params->find("result") : nullptr;
            logInfo("pool", "share accepted nonce=", (nonce ? nonce->stringValue() : "?"),
                    " hash=", (hash ? hash->stringValue() : "?"));

            json::Value result = json::Value::object();
            result.set("status", json::Value::string("OK"));
            json::Value reply = json::Value::object();
            reply.set("id", id ? *id : json::Value::number(std::int64_t{0}));
            reply.set("jsonrpc", json::Value::string("2.0"));
            reply.set("result", std::move(result));
            sendAll(socket, json::dump(reply, -1));
        } else if (method->stringValue() == "keepalived") {
            json::Value reply = json::Value::object();
            reply.set("id", message.find("id") ? *message.find("id") : json::Value::number(std::int64_t{0}));
            reply.set("jsonrpc", json::Value::string("2.0"));
            reply.set("result", json::Value::object());
            sendAll(socket, json::dump(reply, -1));
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (!netInit()) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    const unsigned short port = argc > 1 ? static_cast<unsigned short>(std::atoi(argv[1])) : 3334;
    const char *certPath = argc > 2 ? argv[2] : nullptr;
    const char *keyPath  = argc > 3 ? argv[3] : nullptr;

    Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalid) {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }

    const int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 4) != 0) {
        std::cerr << "cannot listen on 127.0.0.1:" << port << std::endl;
        closeSocket(listener);
        return 1;
    }

#if defined(XMR_HAS_TLS)
    if (certPath && keyPath) {
        g_tlsCtx = SSL_CTX_new(TLS_server_method());
        if (!g_tlsCtx ||
            SSL_CTX_use_certificate_file(g_tlsCtx, certPath, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(g_tlsCtx, keyPath, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(g_tlsCtx) != 1) {
            std::cerr << "cannot load TLS certificate/key" << std::endl;
            closeSocket(listener);
            return 1;
        }
    } else if (certPath || keyPath) {
        std::cerr << "TLS mode needs both certificate and key paths" << std::endl;
        closeSocket(listener);
        return 1;
    }
#else
    if (certPath || keyPath) {
        std::cerr << "mock-pool was built without TLS support" << std::endl;
        closeSocket(listener);
        return 1;
    }
#endif

#if defined(XMR_HAS_TLS)
    logInfo("pool", "mock Stratum pool on 127.0.0.1:", port,
            g_tlsCtx ? " (TLS, difficulty 1)" : " (difficulty 1)");
#else
    logInfo("pool", "mock Stratum pool on 127.0.0.1:", port, " (difficulty 1)");
#endif
    while (true) {
        Socket client = ::accept(listener, nullptr, nullptr);
        if (client == kInvalid) break;

#if defined(XMR_HAS_TLS)
        if (g_tlsCtx) {
            g_tls = SSL_new(g_tlsCtx);
            if (!g_tls || SSL_set_fd(g_tls, static_cast<int>(client)) != 1 || SSL_accept(g_tls) != 1) {
                logWarn("pool", "TLS handshake failed");
                if (g_tls) SSL_free(g_tls);
                g_tls = nullptr;
                closeSocket(client);
                continue;
            }
        }
#endif

        handleClient(client);

#if defined(XMR_HAS_TLS)
        if (g_tls) {
            SSL_free(g_tls);
            g_tls = nullptr;
        }
#endif
        closeSocket(client);
        logInfo("pool", "client disconnected");
    }

#if defined(XMR_HAS_TLS)
    if (g_tlsCtx) {
        SSL_CTX_free(g_tlsCtx);
        g_tlsCtx = nullptr;
    }
#endif
    closeSocket(listener);
    return 0;
}
