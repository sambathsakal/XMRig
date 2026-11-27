# Network / Stratum Protocol

This module implements the Monero Stratum protocol used by XMRig. It is
newline-delimited JSON-RPC 2.0 over plain TCP or TLS; it is not Bitcoin's
`mining.subscribe` protocol. All protocol messages are serialized with
`json::dump(value, -1)` so each request is exactly one line.

## Files

| File | Role |
|---|---|
| `include/xmr/stratum.hpp` | `Job`, target helpers, `StratumClient` API |
| `src/stratum.cpp` | Login, job parsing, share submission, keepalive, reconnect, TLS |
| `tools/mock_pool.cpp` | Local difficulty-1 pool; optional TLS mode for smoke tests |

## Public API

```cpp
bool parseTargetHex(const std::string &hex, std::uint64_t &target);
std::uint64_t difficultyFromTarget(std::uint64_t target);
bool hashMeetsTarget(const std::uint8_t hash[32], std::uint64_t target);

struct Job { /* id, blob, target, seed, height, algo */ };

class StratumClient {
    using JobCallback = std::function<void(std::shared_ptr<const Job>)>;

    StratumClient(std::vector<PoolConfig> pools, int retries, int retryPause,
                  JobCallback callback);
    bool start();
    void stop();
    bool submit(const std::string &jobId, std::uint32_t nonce,
                const std::uint8_t hash[32]);
    bool connected() const;
    std::string rpcId() const;
};
```

## Connection lifecycle

```text
start()
   |
   +-- receiver thread: openSocket(current pool)
   |       TCP connect, TCP_NODELAY
   |       plain TCP: 1 s SO_RCVTIMEO
   |       TLS: OpenSSL handshake, SNI, nonblocking socket + 1 s select()
   |
   +-- send login request (id = 1)
   |
   +-- read JSON lines
          |
          +-- result for id 1 -> client id + job, reset failure counter
          |
          +-- method:"job" -> replace job, invoke callback
          |
          +-- method:"client.reconnect" -> change URL and reconnect
          |
          +-- error/disconnect -> count failure, rotate pool, retry pause
   |
   +-- drainOutbox() sends queued share JSON from the receiver thread
   |
   +-- stop() -> clear running flag, close socket, drop outbox, join receiver
```

Failures are counted per pool. After `retries` failures the client rotates to
the next entry in `config.pools`; each attempt waits `retry-pause` seconds
(capped at 60) and stops early on shutdown. This mirrors XMRig's
`FailoverStrategy` but uses one std::thread and direct sockets/OpenSSL rather
than libuv.

## Login request

```json
{"id":1,"jsonrpc":"2.0","method":"login","params":{"login":"WALLET","pass":"x","agent":"xmr-rebuild/0.1","rigid":"rig-01"}}
```

`agent` identifies this rebuild rather than impersonating XMRig. `rigid` is
added only when `rig-id` is configured.

A valid login response must contain an object `result` with a non-empty string
`id` (the client id used in submissions). An optional `extensions` array may
contain `"keepalive"`; that enables the keepalive extension.

## Job fields

| Field | Use |
|---|---|
| `job_id` | Identifies the job; stored in `Job::id` |
| `blob` | Hex-encoded RandomX input; nonce at byte offset 39, 4 bytes |
| `target` | Little-endian hex difficulty target (4 or 8 bytes) |
| `seed_hash` | 32-byte hex seed for the RandomX epoch; optional but required to mine in this port |
| `height` | Block height, logged for visibility |
| `algo` | Stored in `Job::algo`; loose compatibility only |

`Job::valid()` requires a non-empty id, a non-empty blob, a non-zero target and
`blob.size() >= 43`. `parseJob()` also rejects blobs larger than 408 bytes.

## Target and difficulty

`parseTargetHex()` accepts:

- **8 bytes:** interpreted directly as a little-endian 64-bit target.
- **4 bytes:** XMRig-style compact target converted with
  `0xFFFFFFFFFFFFFFFF / (0xFFFFFFFF / compact)`.

`difficultyFromTarget(target) = 0xFFFFFFFFFFFFFFFF / target`.

`hashMeetsTarget()` compares bytes 24..31 of the 32-byte RandomX result,
interpreted as a little-endian 64-bit integer, with the target:

```cpp
return loadLe64(hash + 24) <= target;
```

## Share submission

```json
{"id":7,"jsonrpc":"2.0","method":"submit","params":{"id":"CLIENT_ID","job_id":"abc","nonce":"0001a2f3","result":"0123...cdef"}}
```

`nonce` is the 4-byte little-endian nonce hex. `result` is the full 32-byte
RandomX hash hex. `m_nextId` is an atomic counter shared by `submit()` and
keepalive requests, so worker threads and the receiver thread can allocate ids
safely. `submit()` only appends the serialized line to `m_outbox`; the receiver
thread drains it, so hashing threads never block on `send()`. The outbox is
capped at 256 entries to avoid unbounded growth during a long outage. Share
accept/reject responses are logged but do not stop mining.

## Threading and synchronization

- One receiver thread owns the socket, `m_readBuffer`, `m_lastKeepalive`,
  `m_keepaliveExtension`, `m_poolIndex` and the failure counter.
- `m_socket` is atomic so `stop()` can close it from another thread.
- `SSL`/`SSL_CTX` are owned by the receiver thread. `stop()` closes the raw
  socket to unblock `SSL_read()`, joins the thread and only then frees the TLS
  objects; `openSocket()` and pool rotation also release the previous context.
- `m_rpcId` is guarded by `m_stateMutex`; `submit()` calls `rpcId()` to take a
  safe copy. Failover and pool-requested reconnect clear it.
- `m_outbox` is guarded by `m_outboxMutex`; only the receiver calls `sendLine()`.
  `m_sendMutex` is kept as a defensive guard for login/keepalive sends.
- A failover clears the outbox because queued shares belong to the previous
  client id.
- Jobs are delivered as `shared_ptr<const Job>`; `Miner` copies them into its
  own synchronized state and signals workers via its generation counter.

## Mock pool

`tools/mock_pool.cpp` listens on `127.0.0.1:3334` by default, accepts one client
at a time, sends a single difficulty-1 job (target `ffffffffffffffff`), and logs
every share as accepted. Because difficulty 1 accepts all hashes, it is useful
only for plumbing/smoke tests, not for performance measurement.

## Live public-pool preset

`--real` (or `"real": true` in config.json) is applied while parsing, so options
that follow it can still override the pool URL or thread count. It:

- selects `stratum+ssl://pool.supportxmr.com:443` if the pool is still the local
  default;
- adds a `stratum+ssl://` scheme to a bare custom hostname;
- enables verified TLS on every configured pool;
- switches RandomX to `fast` (full dataset) and enables huge pages;
- is finalized after parsing by `finalizeConfig()`, which requires a
  structurally valid Monero address in `login`, rejects the placeholder wallet,
  rejects localhost/mock endpoints, rejects `--no-tls-verify`, and rejects
  `--mode light`.

The address check is structural (95 base58 characters starting with `4` or `8`,
with an optional `.worker` suffix); the pool performs authoritative validation.

## TLS

TLS is provided by OpenSSL and is opt-in. Plain TCP stays the default. A pool
uses TLS when its URL starts with `stratum+ssl://`, `stratum+tls://`, `ssl://` or
`tls://`, or when the config/CLI sets `"tls": true` / `--tls`. The URL prefix is
stripped before DNS resolution; the remaining host is used for SNI and for the
certificate hostname check. IP literals use an IP-address check instead of SNI.

`tls-verify` defaults to `true`: the client loads the system CA paths, requires
a peer certificate and verifies the hostname. `--no-tls-verify` or
`"tls-verify": false` disables verification and logs a warning; this is only
appropriate for self-signed local pools. TLS sockets are switched to nonblocking
after the handshake and polled with a 1-second `select()` so the receiver can
drain the share outbox even when the pool is quiet.

On Windows the OpenSSL DLLs are copied next to `xmr-miner.exe` at build time
when they are found under `OPENSSL_ROOT_DIR/bin`.

## Mock pool TLS mode

`tools/mock_pool.cpp` accepts optional certificate and key paths:

```sh
./build/mock-pool 3334 cert.pem key.pem
./build/xmr-miner -o tls://localhost:3334 -u test-wallet --no-tls-verify
```

The self-signed certificate should include `subjectAltName=DNS:localhost` (and
`IP:127.0.0.1` if using an IP URL). With verification left on, the handshake is
expected to fail with `certificate verify failed`.
