# Writing an API gateway from scratch in C++17: the threading model, control-plane separation, and three real bugs

> A architecture retrospective, not a tutorial. Every number, bug and conclusion below comes from one real project ([epollthread](https://github.com/AppleAndPenAndPear/epoll-gateway)): a single-binary API gateway in C++17 with no etcd/Postgres/Redis runtime dependency. Code references point at real files, benchmark numbers come from `docs/BENCHMARKS.md`, and test counts are actual CI output (101 unit tests / 77 integration assertions).

## 1. Why write another gateway

The scope, up front, so this doesn't read as "yet another Nginx":

- **Goal**: for small-to-mid API traffic, a gateway you start with one binary and one JSON file. Auth, rate limiting, circuit breaking, health checks, Prometheus metrics, hot reload and graceful shutdown are table stakes; a control-plane cluster, Lua and a plugin ABI are not.
- **Non-goals**: general-purpose L7 load balancing (front it with Nginx/CDN for bulk static), service mesh (no sidecar), dynamic service discovery (upstreams live in config, not in etcd).
- **Constraint**: C++17, Linux, epoll, and a codebase one person can read. That constraint drove many of the decisions that follow.

## 2. Threading model: SO_REUSEPORT + One Loop Per Thread

There are three common shapes: single reactor plus a thread pool, many preforked processes, and what this project uses — **SO_REUSEPORT + One Loop Per Thread**.

```text
        ┌────────────┐   ┌────────────┐        ┌────────────┐
        │ worker 0   │   │ worker 1   │  ...   │ worker N-1 │
        │ listen fd0 │   │ listen fd1 │        │ listen fdN │
        │ epoll loop │   │ epoll loop │        │ epoll loop │
        └────────────┘   └────────────┘        └────────────┘
              ▲                ▲                     ▲
              └── kernel hashes each connection (SO_REUSEPORT) ──┘
```

Each `TcpWorker` creates its own `SO_REUSEPORT` listen socket and runs its own epoll loop ([server.cpp](../../src/server/server.cpp#L28), [tcpworker.cpp](../../src/server/tcpworker.cpp)). The payoff:

- No shared accept queue, therefore no accept thundering herd — the kernel hashes the connection onto one listen socket.
- Once a connection lands on a worker, all of its I/O happens on that thread. **No two threads ever touch the same fd**, so connections need no locking.
- Shared state between workers shrinks to almost nothing: upstream health/circuit state, the global rate limiter, the logger. The first is a process-wide `shared_ptr<UpstreamManager>` with mutex-protected snapshots; the last is spdlog's async queue.

The costs are worth stating too: `num_workers` has to be explicit config (one worker is one thread with one event loop), and cross-worker quotas (say, a global per-IP rate limit) must live in a shared object rather than per-thread counters.

### The price of EPOLLET + EPOLLONESHOT

All events are edge-triggered and one-shot:

```cpp
epoll_.add(clientsock, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET | EPOLLONESHOT);
// ...
if (has_pending_send) epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
else                  epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
```

(from [tcpworker.cpp:247-267](../../src/server/tcpworker.cpp#L247-L267))

You get "never woken twice for the same event", but two rules become law:

1. **Read until `EAGAIN` inside the callback.** With edge triggering, a partial read means no further notification — the connection silently hangs.
2. **Re-arm after every event.** `EPOLLONESHOT` has already cleared the interest mask.

That is exactly why request handling runs to completion inside one handler instead of yielding to another thread, and why a half-drained send queue has to explicitly re-arm `EPOLLOUT`. The most intricate, bug-prone code in the project lives under this constraint.

## 3. The life of one request

```text
accept → non-blocking read → HTTP state-machine parse → route match (once)
      → auth (X-API-Key / Bearer, key-scoped host/tenant)
      → rate limit (token bucket, per key or per IP)
      → target:
          static   → response cache hit / sendfile zero-copy for large files
          upstream → health-checked node → pooled connection → precisely framed read
      → write back (TLS via the OpenSSL write queue, plaintext via sendfile)
      → trace_id throughout + AUDIT/CLF logging
```

Two implementation details deserve their own paragraph:

- **Route matched exactly once.** The `ResolvedRoute` is reused across auth, rate limiting and dispatch, so the three stages can't drift apart semantically.
- **`sendfile` is only usable on the plaintext path.** `sendfile(2)` cannot bypass the OpenSSL encryption layer, so TLS connections must go through OpenSSL's write queue; the two paths are separate in code ([http_handler.cpp:1008-1048](../../src/server/http_handler.cpp#L1008-L1048)). That's the honest boundary of zero-copy in an HTTPS world, and it rarely makes it into blog posts.

## 4. Three reliability details that are easy to skip

All three fail in production in the ugliest possible way rather than at the moment you write them.

### 4.1 Frame every response precisely

Reusing a backend connection requires knowing **where a response ends in the byte stream**. So a response may only terminate in one of three ways: `Content-Length`, `chunked` (including trailers), or `Connection: close` close-delimited. Any implementation that treats "this `recv` returned" as "the response is done" will poison the pool — the next request parses leftover bytes as its own status line.

### 4.2 Retry only idempotent methods

A connection that fails after the request was written is the awkward case: the backend may already have executed it. So retries are a whitelist: only idempotent methods (GET/HEAD, ...) and only transient failure classes; **POST is never retried automatically**, and routes that want extra attempts declare them with `max_retries`. This is where correctness has to beat availability.

### 4.3 Liveness-check before reuse

An idle connection pulled from the pool may already have been closed by the peer. Writing to it returns RST and surfaces as a mysterious 502. So before handing one out, the pool probes with a zero-timeout `MSG_PEEK`: a 0-byte result means the peer is gone, the connection is dropped and a fresh one is dialed — transparently to the caller.

## 5. Separating the control plane from the data plane

The operations API (`/admin/*`) does not share a listener with business traffic:

- **Separate port, separate thread** (default `127.0.0.1:8105`, loopback, started only when `admin.enabled: true`).
- The thread serves a small **blocking** HTTP loop (`accept` + 1-second `poll`), deliberately not wired into the epoll event loop — admin traffic is negligible, the blocking model is the simplest correct thing, and an ops request can never disturb the business event loop.
- Every request is authenticated with a **constant-time comparison** (XOR accumulation over the longer length) to avoid a timing side channel.
- Endpoints: `GET /admin/stats` (counters/latency/uptime/version), `GET /admin/upstreams` (per-backend health + circuit state), `POST /admin/reload` (validates config first; invalid ⇒ 400 with nothing applied).

### One signal source for reload

The worst hot-reload failure is two paths that mean different things. Here `SIGHUP` and `POST /admin/reload` collapse into one atomic counter (the reload generation): the signal handler only does `fetch_add`, and each worker reads, validates and applies the config at its own safe checkpoint when it notices the generation changed. Consequences:

- `systemctl reload` and the ops API behave identically;
- an invalid config is never applied, an AUDIT line records why it was rejected, and the old config keeps serving;
- a failed TLS cert reload keeps the previous `SSL_CTX`, so in-flight connections are unaffected.

The admin listener's own keys are hot-rotatable (≤1 s), while its **topology (enabled/port/bind) is fixed at startup** — rebuilding a listener at runtime brings failure modes (port in use, dropped connections, permissions) far larger than its value. That's an explicit "won't do", not an oversight.

## 6. Three real bugs

### Bug 1: a 43 ms latency floor on every request

The first benchmark run looked suspiciously tidy: P50 pinned at 43 ms at light load, QPS stuck near 170. Too tidy for noise — it looked like a timer.

```text
                      before            after (TCP_NODELAY)
static-light (c8)     176 QPS / 43.04ms → 949 QPS / 7.99ms
proxy-light  (c8)     169 QPS / 43.40ms → 1000 QPS / 7.54ms
handshake    (c10)    224 QPS / 42.79ms → 956 QPS / 10.09ms
proxy        (c50)    836 QPS / 53.64ms → 1083 QPS / 42.22ms
```

The cause is the classic Nagle × delayed-ACK interaction: the server writes a response as several small segments (especially under TLS — one record for headers, one for the body), Nagle holds the second small write waiting for an ACK, and the peer is waiting on its 40 ms delayed-ACK timer because there is only one outstanding segment. Both sides wait for the timer.

The fix is `TCP_NODELAY` on accepted sockets and upstream sockets (`setnodelay()` in [mysocket.cpp](../../src/common/mysocket.cpp)). **Light-load latency improved ~5.4x, handshake throughput ~4.3x.**

The lesson isn't "enable TCP_NODELAY" — it's that **when a latency figure is too stable to be plausible, it's protocol behavior, not your code being slow**.

### Bug 2: `close()` sends RST and can destroy the response you just wrote

The admin `POST /admin/reload` deliberately never parses a request body (no admin endpoint takes one). That produced this scenario: a client sends a POST with a body, the gateway reads the head, authenticates, writes its 200, closes — and the client sees a connection reset with an empty response.

The kernel is the culprit: **if a socket is closed while its receive buffer still holds unread data, Linux sends RST instead of FIN** — and an RST lets the peer discard data it has received but not yet delivered to the application, including the response we just wrote.

The fix drains the remaining body (per `Content-Length`, bounded, with `SO_RCVTIMEO` tightened to 1 s for that phase so a stalled client can't hold the serialized admin thread):

```cpp
// Discard the rest of the request body before the socket is closed.
// Closing a socket that still has unread received data makes the kernel send
// RST instead of FIN — and an RST can destroy a response the client has not
// read yet.
```

What makes this one interesting: the trigger is "the client sent data you don't care about", and **nothing covered it** — every integration test and documentation example sent POSTs without a body. There is now an assertion for exactly this combination.

### Bug 3: an unconsumed chunked trailer corrupts connection reuse

When a backend replies with `Transfer-Encoding: chunked` plus a trailer (`Trailer: X-Sum`), the full byte sequence is: chunks, `0\r\n`, trailer fields, terminating `\r\n`.

Treat the last chunk as the end of the response and return the connection to the pool, and the leftover trailer plus CRLF becomes unread data that the *next* request on that connection parses as the start of its status line — random parse errors or 502s. The fix reads the trailer through its terminating CRLF before considering the response complete; the integration suite verifies it with a chunked response carrying a trailer followed by a second request on the same connection (`test_chunked_trailer`).

The request direction gets the same treatment: duplicate `Content-Length`, `Content-Length` combined with `Transfer-Encoding`, non-chunked `Transfer-Encoding`, non-numeric lengths, invalid header characters, oversized request lines/header blocks are all answered with 400 + `Connection: close`. Those are the classic HTTP request-smuggling doors — reject rather than guess.

## 7. Testing: why the integration suite boots a real server

Unit tests cover pure logic: the HTTP parser (including smuggling vectors), the LRU cache, route matching, auth policy, rate-limit isolation, config schema validation, TLS context construction, connection-pool semantics, client timeouts and idempotent retries, health checks and the circuit breaker.

But none of the three bugs above is unit-testable — they are products of real TCP behavior and real kernel semantics. So the integration suite starts a **real gateway process plus real mock upstreams** (Python), drives end-to-end scenarios over real TLS, and asserts observable behavior rather than internals:

- 10 proxied requests must open ≤4 new backend connections (proof the pool actually reuses);
- a connection stays usable after a chunked response with a trailer;
- after one backend node is killed, six *consecutive* 200s are required before declaring the node ejected (so a lucky round-robin turn can't produce a false pass);
- graceful shutdown is exercised with a connection mid-way through a 1.5 s request, which must still be served inside the drain window;
- after admin key rotation, the old key gets 401 and the new key 200.

Current state: **101 unit tests (CTest) + 77 integration assertions**, all through one `./ci.sh` that builds, runs unit tests and then integration; an AddressSanitizer build is also supported.

## 8. What is intentionally not done yet

- Single-node architecture; no distributed config, and rate-limit quotas / circuit state are not shared across instances.
- No plugin system or Lua — extend by changing code or fronting another upstream.
- The admin listener's topology (enabled/port/bind) needs a process restart (keys rotate hot).
- Benchmark numbers come from a 2 vCPU box where client, gateway and Python mock share the same machine: absolute values mean little, **ratios** carry the conclusion.
- The connection pool's throughput win on loopback is within noise (a local TCP connect costs tens of microseconds). Its value scales with backend distance — it removes one RTT per request.

## 9. Reproducing

```bash
git clone https://github.com/AppleAndPenAndPear/epoll-gateway
cd epoll-gateway
./ci.sh                              # build + 101 unit tests + 77 integration assertions
scripts/benchmark/run_benchmark.sh   # wrk benchmark (15 s per scenario)
```

Further reading: [README.md](../../README.md) (capability list), [docs/BENCHMARKS.md](../BENCHMARKS.md) (full benchmark report), [docs/DEPLOYMENT.md](../DEPLOYMENT.md) (systemd deployment and rolling upgrade), [docs/PROJECT_STATUS.md](../PROJECT_STATUS.md) (current state snapshot).