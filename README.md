[English](README.md) | [简体中文](README.zh-CN.md)

# epollthread

> Lightweight, self-contained API gateway in C++17 — a single binary built on epoll, with auth, rate limiting, circuit breaking and Prometheus metrics built in.

epollthread is a high-performance, multi-threaded HTTP/HTTPS API gateway and network server built on a **SO_REUSEPORT + epoll + One Loop Per Thread** architecture with asynchronous logging and non-blocking I/O. Out of the box it provides HTTP/1.1, hardened TLS (minimum 1.2, AEAD ciphers, cert hot reload) on both the client-facing and upstream hops, keep-alive with upstream connection pooling, zero-copy file serving, LRU/FD caching, config-driven routing with schema validation, reverse proxying with upstream health checks, request-smuggling protection, idempotent retries and circuit breaking, API-key authentication (keys loadable from a file or environment variable), host/tenant policies, token-bucket rate limiting, `X-Trace-Id` request tracing, dual AUDIT/CLF logging, `SIGHUP` runtime reload, upstream timeouts with error classification, Prometheus metrics and Docker deployment — plus unit tests (101/101 passing on CTest), integration tests (77/77 assertions) and AddressSanitizer support.

## 5-Minute Quickstart

```bash
git clone https://github.com/AppleAndPenAndPear/epoll-gateway
cd epoll-gateway
./scripts/gen_dev_certs.sh   # self-signed dev certificate (the data plane is TLS-only)
./build.sh                   # or ./ci.sh to build AND run the full test suite
./start.sh                   # gateway on https://localhost:5005 (Ctrl+C to stop)
```

No backend needed — built-in routes answer immediately:

```bash
# Echo (built-in, no upstream): POST any JSON and get it reflected back
curl -sk -X POST https://localhost:5005/api/echo -H 'Content-Type: application/json' -d '{"hello":"gateway"}'
# {"echo":{"hello":"gateway"}}

# Ops endpoints (always auth-exempt)
curl -sk https://localhost:5005/healthz
curl -sk https://localhost:5005/version

# Admin API on its own loopback port, behind key auth:
curl -s http://127.0.0.1:8105/admin/stats                                   # 401
curl -s -H 'X-API-Key: admin-demo-key' http://127.0.0.1:8105/admin/stats    # 200

# Prometheus metrics
curl -sk https://localhost:5005/metrics | head
```

Or with Docker (the keypair stays on the host, never baked into the image):

```bash
./scripts/gen_dev_certs.sh
docker compose up -d
```

To proxy to real backends — load balancing with health checks, or verified
`https://` upstreams — see the runnable scenarios in [examples/](examples/).

## Features

### Networking & concurrency
- **Multi-threaded Reactor model** — each worker thread runs its own epoll event loop with its own listen socket (`SO_REUSEPORT`), so the kernel load-balances connections and there is no lock contention.
- **Non-blocking I/O + edge triggering** — every socket is non-blocking, combined with `EPOLLET` and `EPOLLONESHOT` for precise event control, avoiding thundering herds and duplicate events.
- **Connection reuse** — correct `Connection: keep-alive` handling; multiple requests are processed serially on a single TCP connection (HTTP pipelining) with strict response ordering.
- **Idle connection timeout** — configurable timeout that automatically closes inactive connections to prevent resource leaks.
- **Graceful shutdown** — `SIGINT`/`SIGTERM` are caught, all worker threads are safely asked to exit, and logs and resources are reclaimed intact.
- **RAII resource management** — `Socket`, `Epoll` and friends are RAII wrappers with move semantics; descriptor leaks are eliminated by construction.
- **Clean waiter separation** — `Socket` owns fd lifetime and I/O only, `Poller` wraps a single-fd `poll` wait, and `Epoll` manages large numbers of long-lived connections.
- **Dynamic thread pool** — configurable min/max threads with load-based scaling (interface reserved for future async business logic).

### Protocol support
- **HTTP/1.1** — built-in state-machine parser covering the request line, headers, query string and body, supporting `GET / HEAD / POST / PUT / DELETE` and chunked transfer encoding.
- **HTTPS/TLS, hardened** — OpenSSL-based TLS with a minimum version of TLS 1.2 and an AEAD-only cipher whitelist (ECDHE + GCM/ChaCha20, no CBC/3DES/RC4), session resumption via cache + tickets, and fail-fast setup. The certificate/private key can be hot-reloaded with `SIGHUP` (`tls.cert_path`/`tls.key_path`), keeping the old context if the new one fails to load.
- **Request-smuggling protection** — the parser rejects duplicate `Content-Length`, CL+TE mixing, non-chunked `Transfer-Encoding`, non-numeric `Content-Length`, invalid header characters, control characters in the request line, non-hex chunk sizes and oversized request lines/headers, answering malformed requests with 400 + `Connection: close`.
- **Gzip compression** — text-like responses are gzip-compressed when negotiated via the client's `Accept-Encoding` header.
- **Chunked responses** — `Transfer-Encoding: chunked` support for dynamically generated or streamed content.
- **Status codes & error handling** — 200, 400, 403, 404, 405, 413, 429, 500, 502, 503, 504 and more; unreachable backends are distinguished from backend timeouts, path-traversal attacks are deflected, and a matched path with an unsupported method returns 405 with an `Allow` header.

### Gateway routing, auth & rate limiting
- **RESTful routing** — register handlers for any method + path pattern (e.g. `/users/{id}`) with dynamic parameter extraction and dispatch; build JSON APIs with ease.
- **Config-driven gateway routes** — `routes` entries declare method, path, host, tenant, auth, rate limiting and execution target; registration and dispatch are decoupled, with `local`, `upstream` and `static` target types.
- **API-key authentication** — keys are accepted via the `X-API-Key` header or `Authorization: Bearer`; supports route-level allow/deny lists and per-key host/tenant scopes with differentiated rate-limit quotas. Keys can live inline in `config.json`, in a separate file (`api_keys_file`), or come from the `GW_API_KEYS` environment variable (precedence env > file > inline) so secrets stay out of the main config.
- **Token-bucket rate limiting** — a global limiter shared across workers (totals stay accurate under `SO_REUSEPORT` multi-worker mode), counted per client IP and returning `429 Too Many Requests`; fractional token refill avoids truncation errors.

### Reverse proxy & reliability
- **Reverse proxy & load balancing** — routes reference upstream groups via `UpstreamTarget`; `UpstreamManager` round-robins across healthy backends and probes them with active TCP health checks, automatically ejecting failed nodes.
- **Upstream connection pooling** — keep-alive connections to backends are pooled and reused across requests (60 s idle timeout, max 16 idle per upstream), eliminating one TCP handshake per request; backend responses are framed precisely (Content-Length / chunked incl. trailers / close-delimited) and connections closed by the backend while idle are detected up front and transparently replaced. Post-send failures are retried only for idempotent methods, so POSTs are never duplicated.
- **Upstream access control & timeouts** — host and tenant binding per route/API key; connect, send and read phases each have timeout control, with distinct 502 vs 504 semantics.
- **Upstream error classification** — connect failure/timeout, send failure/timeout, read failure/timeout and invalid responses are distinguished, exposed as Prometheus error counters and mapped to 502/503/504.
- **Idempotent retries** — only idempotent methods (GET/HEAD, etc.) and retryable transient errors are retried; routes can add extra attempts via `max_retries`, so non-idempotent requests are never duplicated.
- **Circuit breaker** — per-upstream/per-backend state: consecutive failures past a threshold open the circuit, half-open probes run after a recovery timeout and auto-close it on success, keeping a failing backend from dragging the whole gateway down.

### Static files & caching
- **Static file serving** — URL-to-file mapping with **zero-copy** `sendfile` transfer and automatic `Content-Type` detection (HTML, CSS, JS, JSON, images, fonts and other common formats).
- **Two-layer caching** — a per-worker LRU in-memory file cache for hot small files (less disk I/O, dramatically higher repeat-request throughput) and a TTL-based file-descriptor cache that avoids repeated `open()`/`stat()` syscalls.

### Observability
- **`X-Trace-Id` tracing** — echoed back when the client sends one, otherwise generated by the gateway; flows through request logs and audit logs.
- **AUDIT + CLF dual-channel logging** — the CLF access log captures IP, method, path, status code, response size and latency for every completed request; the AUDIT log records failures and security-policy events once per completed response (trace_id, route, Host/Tenant, failure reason, masked API key) without duplicate writes.
- **Prometheus metrics** — built-in `/metrics` endpoint covering request counts by status class, a request-latency histogram, cache hit ratios and upstream error-type counters.
- **Grafana dashboards** — bundled Grafana + Prometheus monitoring stack, deployed with a single `docker-compose` command for out-of-the-box dashboards.

### Operations & engineering
- **External JSON configuration with schema validation** — port, thread count, web root, thread-pool parameters, cache sizes, timeouts and more, making deployment and tuning easy. Field types/ranges, duplicate route names/matches and upstream references are validated; startup fails fast on an invalid config.
- **Runtime reload** — `SIGHUP` triggers a hot reload: the signal handler only increments a generation counter, and workers read and apply the new config at safe checkpoints. Reload updates routes, upstreams, API keys, rate limiting, the keep-alive timeout and the TLS certificate; an invalid file never overwrites the running config and is recorded as an AUDIT event.
- **Ops endpoints + Admin API** — `/healthz`, `/readyz`, `/version` built in; a separate authenticated admin listener serves `/admin/stats`, `/admin/upstreams` (health + circuit-breaker state) and `POST /admin/reload`. See [Operations](#operations--health).
- **Graceful shutdown** — SIGTERM stops accepting first, closes idle keep-alive connections, and drains in-flight requests (up to `shutdown_drain_timeout`, default 30 s) before exiting; pairs with systemd `TimeoutStopSec`.
- **Companion non-blocking client** — an independent state-machine HTTPS client (TLS handshake, certificate and hostname verification, connect/send/receive), demonstrating epoll from the client side.
- **Unit tests** — Google Test, currently 101/101 via CTest, covering the HTTP parser (incl. smuggling vectors), LRU cache, response serialization, route matching, 404/405 semantics, path-traversal protection, API-key policy (incl. key-source precedence), rate-limit isolation, config schema validation, TLS context hardening (incl. the shared client/server policy and the client trust settings), connection-pool semantics (incl. scheme/verification-identity separation), HTTP client timeouts, idempotent retries, upstream health checks, status snapshots and the circuit breaker.
- **Integration tests** — 77 end-to-end assertions against a real server + mock upstreams (TLS policy, cert hot reload, the companion client with hostname verification, upstream TLS with certificate/hostname verification incl. a mismatch-rejection case, auth from a keys file, rate limiting, failover, circuit breaking, reload, ops endpoints, admin API, admin key rotation, connection reuse, chunked trailers, graceful shutdown), runnable with nothing but the Python 3 standard library.
- **AddressSanitizer** — enabled automatically in Debug builds to catch memory leaks and out-of-bounds accesses.
- **Docker** — multi-stage `Dockerfile` builds a lean image for one-command deployment anywhere.

## Architecture

```
                    Master Thread
                         |
         ┌───────────────┼───────────────┐
         │               │               │
     TcpWorker 1    TcpWorker 2    ... TcpWorker N
    (epoll loop)   (epoll loop)       (epoll loop)
         │               │               │
    listen fd 1     listen fd 2      listen fd N   (SO_REUSEPORT)
         │               │               │
    ┌────┴────┐    ┌────┴────┐      ┌────┴────┐
    │ client  │    │ client  │      │ client  │
    │ conns   │    │ conns   │      │ conns   │
    └────┬────┘    └────┬────┘      └────┬────┘
         │               │               │
    HttpHandler    HttpHandler      HttpHandler
    (parse, route, (parse, route,   (parse, route,
     policy)        policy)          policy)
    (files, cache)  (files, cache)   (files, cache)
         │               │               │
         └───────────────┼───────────────┘
                         │
              UpstreamManager + HttpClient
              (health checks, backend
               selection, timeouts, proxying)
         │               │               │
         └───────────────┴───────────────┘
                         │
              DynamicThreadPool (shared pool)
                         │
              ┌──────────┴──────────┐
              │                     │
         Prometheus              Grafana
      (scrapes /metrics     (dashboards
       every 5s) @ :9090      @ :3000)
```

- **TcpServer** — creates the N listen sockets and starts the matching `TcpWorker` threads.
- **TcpWorker** — each worker owns an epoll instance, a connection table, an `HttpHandler` and an SSL context, and handles all I/O events for its connections (including TLS handshakes), plus idle-connection cleanup and upstream health checks.
- **HttpHandler** — the HTTP/1.1 core: request parsing, config-driven route matching, host/tenant policies, API-key auth, rate limiting, keep-alive, static file serving and error responses.
- **UpstreamManager** — manages upstream server groups; performs TCP health checks with timeouts using non-blocking `Socket + Poller`, and round-robin backend selection.
- **HttpClient** — performs synchronous single-backend HTTP forwarding with connect, send and read timeouts via `Socket + Poller`, returning a structured `BackendError`.
- **Poller** — wraps a single `poll` wait; unlike `Epoll` it waits on individual backend connections rather than serving as the worker event loop.
- **Metrics** — thread-safe singleton collector recording request totals, status-code distribution, latency histograms and multi-layer cache hit rates, exposed in Prometheus text format at `/metrics`.
- **Prometheus** — periodically scrapes `server:5005/metrics` and stores the time series.
- **Grafana** — uses Prometheus as a data source to provide live visualization dashboards.
- **DynamicThreadPool** — optional shared pool for offloading slow tasks from I/O threads (reserved for extension).
- **Logger** — global asynchronous logger running on the spdlog global thread pool for high-performance logging.

## Quick Start

### Requirements
- Linux (kernel 3.9+, for `SO_REUSEPORT`)
- GCC 7+ or Clang 5+ (C++17 required)
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) (recommended) or manually installed dependencies

### Dependencies

| Library | Purpose | Install |
|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | Async logging | vcpkg / apt |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON config & API | vcpkg / apt |
| [OpenSSL](https://www.openssl.org/) | TLS/SSL encrypted transport | system / apt |
| [zlib](https://zlib.net/) | Gzip compression | system / apt |
| [Google Test](https://github.com/google/googletest) | Unit tests | vcpkg / apt |

### Build

Option 1 — one-command build script (recommended):

```bash
# Release build
./build.sh

# Debug build (enables AddressSanitizer)
./build.sh Debug
```

Option 2 — plain CMake:

```bash
# Using the vcpkg toolchain (recommended)
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Or install dependencies with the system package manager and build directly
sudo apt install g++ cmake make libspdlog-dev nlohmann-json3-dev zlib1g-dev
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Configure

Edit `config.json` to suit your deployment (every field has a default; if the file is missing, defaults apply):

```json
{
    "port": 5005,                   // Listen port
    "backlog": 1024,                // listen backlog size
    "num_workers": 2,               // Worker thread count (0 = auto, CPU core count)
    "www_root": "./www",            // Static file root directory
    "thread_pool": {
        "min": 2,                   // Min threads
        "max": 10,                  // Max threads
        "scale_up": 2,              // Scale-up threshold (tasks per thread)
        "scale_down": 1             // Scale-down threshold
    },
    "cache_max_entries": 1024,      // Max LRU file-cache entries
    "cache_max_file_size_mb": 1,    // Max cacheable file size (MB)
    "keepalive_timeout": 60,        // Keep-Alive idle timeout (seconds)
    "tls": {                        // Optional TLS paths (defaults shown)
        "cert_path": "certs/server.crt",
        "key_path": "certs/server.key"
    },
    "upstreams": {                  // Upstream service definitions
        "test-service": {
            "servers": [
                {"host": "127.0.0.1", "port": 8081},        // plaintext (fine on loopback)
                {"host": "https://10.0.0.8", "port": 8082,  // https:// enables upstream TLS
                 "tls_ca_file": "certs/backend-ca.crt",     // CA that signed the backend cert (empty = system store)
                 "tls_server_name": "backend.internal"}     // SNI + hostname check (empty = chain only)
            ],
            "algorithm": "round_robin"
        }
    },
    "upstream_health_check_timeout_ms": 500, // Upstream TCP health-check timeout (ms)
    "routes": [                     // Config-driven routes
        {
            "name": "test-service-api",
            "method": "GET",
            "path": "/api/test/*",
            "host": "*",            // Optional, defaults to *
            "tenant": "*",          // Optional, defaults to *
            "target_type": "upstream",
            "upstream_target": {
                "name": "test-service",
                "timeout_ms": 5000,
                "max_retries": 1,               // Extra retries (idempotent methods only)
                "circuit_failure_threshold": 5, // Consecutive failures that open the circuit
                "circuit_recovery_timeout_ms": 10000 // Time before half-open probes are allowed
            },
            "auth_required": true,
            "allowed_api_keys": ["test-key-123", "premium-key-456"]
        }
    ],
    "rate_limit": {                 // Default rate limit (token bucket)
        "capacity": 20,
        "refill_per_second": 5
    },
    "api_keys_file": "api_keys.json", // Optional: load keys from a separate file (or GW_API_KEYS env) instead of inline
    "api_keys": [                   // API keys, rate-limit quotas and host/tenant scopes
        {
            "key": "test-key-123",
            "name": "Test Client",
            "rate_limit": {"capacity": 200, "refill_per_second": 100},
            "allowed_hosts": ["*"],
            "allowed_tenants": ["*"]
        },
        {
            "key": "premium-key-456",
            "name": "Premium Client",
            "rate_limit": {"capacity": 1000, "refill_per_second": 500}
        }
    ]
}
```

### Run

```bash
# Start directly
./build/server

# Or use the start script
./start.sh
```

Accessing the server:
- HTTPS is enabled by default — open https://localhost:5005 for the default page (trust the self-signed certificate manually).
- Inspect request/response headers with `curl -kv https://localhost:5005/`.
- Try HEAD: `curl -kI https://localhost:5005/index.html`.
- Server identification: responses carry `Server: EpollHTTP/0.2`.
- Press `Ctrl+C` for a graceful shutdown; the full log is preserved in `logs/epollserver.log`.

### Test

Unit tests:

```bash
cd build
cmake .. -DBUILD_TESTS=ON
cmake --build . -j$(nproc)
./tests/runTests

# Or via CTest
ctest --test-dir build --output-on-failure
```

Integration tests — start the real server plus a mock upstream and verify end-to-end behavior (TLS policy, cert hot reload, auth, rate limiting, failover, circuit breaking, reload, connection reuse, chunked trailers — 16 scenario groups):

```bash
python3 tests/integration/run_integration_tests.py
```

Only the Python 3 standard library is required — nothing extra to install.

### One-command CI

```bash
./ci.sh              # Build + unit tests + integration tests
./ci.sh build        # Build only
./ci.sh integration  # Integration tests only
```

The GitHub Actions workflow lives in `.github/workflows/ci.yml` and runs build, unit and integration tests on every push and pull request.

## Docker

### Option 1 — Docker Compose (recommended, includes the monitoring stack)

```bash
# Start all services (server + Prometheus + Grafana)
sudo docker-compose up -d

# Check service status
sudo docker ps -a

# Tail logs
sudo docker-compose logs -f

# Stop all services
sudo docker-compose down
```

Access points:
- Web home: https://localhost:5005 (self-signed certificate; trust it manually)
- Metrics: https://localhost:5005/metrics
- Prometheus: http://localhost:9090
- Grafana: http://localhost:3000 (default credentials: `admin`/`admin`)

### Option 2 — build the image standalone

```bash
# Build the image
docker build -t epoll-server .

# Run the container
docker run -d -p 5005:5005 --name my-server epoll-server

# View logs
docker logs my-server

# Stop and remove
docker stop my-server && docker rm my-server
```

## Routing & Reverse Proxy

The server registers the following RESTful routes by default:

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/hello` | Returns `{"message": "Hello, World!"}` |
| `POST` | `/api/echo` | Echoes the JSON request body |
| `PUT` | `/api/echo` | Returns `PUT received: <body>` |
| `DELETE` | `/api/resource` | Returns `{"status": "deleted", ...}` |
| `GET` | `/users/{id}` | Dynamic route returning mock user data |
| `GET` | `/chunked` | Chunked transfer demo |
| `GET` | `/metrics` | Prometheus metrics endpoint (text format) |
| `GET` | `/api/test/*` | Config-driven reverse proxy, forwarded via `upstream_target.name` to `test-service` |
| `GET` | `/<path>` | Static file serving (default behavior) |

Reverse proxying is configured through the `upstreams` and `routes` sections of `config.json`, transparently forwarding requests to backend services:

- **`upstreams`** — defines a group of backend servers and their load-balancing algorithm (`round_robin` is currently supported).
- **`routes`** — forwards matching "method + path" pairs to a named upstream (paths support `*` wildcards).
- **Health checks** — every worker actively probes upstream nodes over TCP once per second, ejecting failed nodes and re-admitting them after recovery.
- **Load balancing** — `UpstreamManager` round-robins across healthy nodes; forwarding is implemented in `http_client::forward_request`.
- **Connection pooling** — keep-alive connections to each upstream are pooled (60 s idle timeout, max 16 idle per upstream) and reused across requests, saving one TCP handshake per request; responses are framed precisely (Content-Length / chunked incl. trailers / close-delimited) and connections closed by the backend while idle are detected before use and transparently replaced. Pool identity includes the TLS verification policy, so a session verified for one backend name is never handed to an upstream with a different policy.
- **Upstream TLS** — a server address written as `https://host` makes the gateway complete a client-side TLS handshake (same hardening as the data plane: minimum TLS 1.2, AEAD-only ciphers) before forwarding. The backend certificate is verified against `tls_ca_file` (empty = system default trust store) with `tls_server_name` as SNI + hostname check; `tls_skip_verify: true` disables verification for lab setups. Plaintext `http://` upstreams remain supported and are the sensible choice on loopback.
- **Timeouts & error classification** — connect (incl. the TLS handshake), send and read phases each have timeout control; structured `BackendError` values distinguish failure types and map to 502/503/504.
- **Retries** — only idempotent methods and retryable transient errors are retried; `max_retries` controls the extra attempts per route, and the pool never resends POSTs internally.
- **Circuit breaking** — consecutive failures are tracked per upstream/backend; `circuit_failure_threshold` opens the circuit, half-open probes are allowed after `circuit_recovery_timeout_ms`, and success closes it.

## Auth & Rate Limiting

- **Token-bucket rate limiting** — `RateLimiter` implements a global limiter shared across workers (keeping totals accurate under `SO_REUSEPORT` multi-worker mode), counted per client IP; over-limit requests receive `429 Too Many Requests`.
- **API-key authentication** — keys are extracted from the `X-API-Key` header or `Authorization: Bearer <key>`, and the `api_keys` config assigns differentiated rate-limit quotas per client.

```bash
# Returns 429 once the rate limit trips
for i in $(seq 1 30); do curl -sk -o /dev/null -w "%{http_code}\n" https://localhost:5005/api/hello; done
```

## Observability

The gateway exposes Prometheus metrics at the built-in `/metrics` endpoint, and `docker-compose` wires up the complete monitoring stack in one step.

### Monitoring architecture

```
server:5005/metrics
       │
       ▼ (scraped every 5s)
 Prometheus :9090 ─────▶ Grafana :3000
 (time series)           (dashboards)
```

### Exposed metrics

| Metric | Type | Description |
|---|---|---|
| `epoll_http_requests_total{code="2xx|3xx|4xx|5xx"}` | Counter | Requests by status class |
| `epoll_http_requests_total_total` | Counter | Total requests |
| `epoll_http_request_duration_seconds_bucket{le="..."}` | Histogram | Request latency distribution (11 buckets) |
| `epoll_http_request_duration_seconds_sum/_count` | Histogram | Cumulative latency and request count |
| `epoll_cache_hits_total` / `epoll_cache_misses_total` | Counter | LRU file-cache hits/misses |
| `epoll_fd_cache_hits_total` / `epoll_fd_cache_misses_total` | Counter | FD-cache hits/misses |
| `epoll_gzip_cache_hits_total` / `epoll_gzip_cache_misses_total` | Counter | Gzip compression-cache hits/misses |
| `epoll_upstream_errors_total{type="..."}` | Counter | Upstream error counts (connect_failed, connect_timeout, write_failed, write_timeout, read_failed, read_timeout, invalid_response) |
| `epoll_route_requests_total{route,host,tenant,code}` | Counter | 2xx/4xx/5xx requests per route/host/tenant |
| `epoll_route_latency_seconds_sum/_count{route,host,tenant}` | - | Latency sum/count per route/host/tenant (no dedicated histogram yet) |

### Adding the Grafana data source

1. Open http://localhost:3000 in a browser and sign in with `admin`/`admin`
2. Left sidebar → **Connections** → **Data sources** → **Add data source**
3. Choose **Prometheus**
4. Set **Prometheus server URL** to `http://prometheus:9090` (containers communicate over the Docker network)
5. Click **Save & test**; you should see "Successfully queried the Prometheus API"

### Useful PromQL

```promql
# QPS (requests per second)
rate(epoll_http_requests_total_total[1m])

# Error rate
sum(rate(epoll_http_requests_total{code="4xx"}[1m]) + rate(epoll_http_requests_total{code="5xx"}[1m])) /
sum(rate(epoll_http_requests_total_total[1m]))

# P99 latency
histogram_quantile(0.99, rate(epoll_http_request_duration_seconds_bucket[1m]))

# File-cache hit ratio
sum(rate(epoll_cache_hits_total[1m])) /
sum(rate(epoll_cache_hits_total[1m]) + rate(epoll_cache_misses_total[1m]))

# Per-route average latency
rate(epoll_route_latency_seconds_sum[1m]) / rate(epoll_route_latency_seconds_count[1m])

# Upstream connect-timeout error rate
rate(epoll_upstream_errors_total{type="connect_timeout"}[1m])
```

### Logging

- **`X-Trace-Id`** — echoed back when the client provides it, otherwise generated by the gateway; present in both request and AUDIT logs.
- **CLF access log** — client IP, method, path, status code, response size and latency for every completed request.
- **AUDIT log** — records failures and security-policy events once when the response completes: trace_id, method/path, Host/Tenant, route, status, failure reason, masked API key and User-Agent — no duplicate writes from the auth/rate-limit/routing branches.

## Runtime Reload

Send `SIGHUP` to the server process to trigger a hot reload:

```bash
kill -HUP $(pgrep server)
```

- The signal handler only increments the reload generation — no file I/O or JSON parsing happens there.
- Workers check the generation at safe checkpoints and apply the new config; workers do not switch at the exact same instant.
- Reload updates routes, upstreams, API keys, default rate limiting, the keep-alive timeout and the TLS certificate.
- The JSON file is schema-validated before reload; an invalid config never replaces the currently active one and is recorded as `AUDIT config_reload_rejected`.

Reloads can also be triggered through the admin API — see below.

## Operations & Health

### Probe endpoints (built in, no auth, exempt from rate limiting)

```bash
curl -sk https://localhost:5005/healthz    # liveness: "ok" while the event loop serves
curl -sk https://localhost:5005/readyz     # readiness: 200 + per-upstream healthy/total JSON,
                                           # 503 when an upstream has no healthy backend
curl -sk https://localhost:5005/version    # {"version":"0.1.0"}
```

Point your load balancer at `/readyz` (not `/healthz`) so a gateway with all backends down leaves the pool instead of failing client traffic.

### Admin API (separate listener, key required)

Enable it in the config (bind stays on loopback by default; every endpoint requires the key):

```json
"admin": {"enabled": true, "port": 8105, "bind": "127.0.0.1",
          "api_keys": ["choose-a-long-random-admin-key"]}
```

```bash
# Request counters, latency totals, uptime, version
curl -s -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/stats

# Per-backend health, failure counts and circuit-breaker state
curl -s -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/upstreams

# Hot reload with pre-validation: invalid config -> 400 and nothing applied,
# valid config -> 200 and workers apply it within ~1 second
curl -s -X POST -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/reload
```

### Graceful shutdown

```bash
kill -TERM $(pgrep server)   # or: systemctl restart epoll-gateway
```

The gateway stops accepting new connections, closes idle keep-alive connections immediately, then waits for in-flight requests to finish (up to `shutdown_drain_timeout` seconds, default 30) before exiting. Set systemd's `TimeoutStopSec` above that value; a ready-made unit and a full rolling-upgrade walkthrough live in [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

## HTTPS

The server integrates OpenSSL and performs the TLS handshake automatically once the TCP connection is established, using `certs/server.crt` and `certs/server.key` as the certificate and private key (configurable via the `tls` section). TLS 1.0/1.1 are refused — the minimum version is TLS 1.2, restricted to an AEAD cipher whitelist (ECDHE + GCM/ChaCha20) with session resumption. Clients must connect over HTTPS:

```bash
# -k skips self-signed certificate verification
curl -kv https://localhost:5005/

# Or test the TLS handshake directly with the openssl client
openssl s_client -connect localhost:5005

# Verify the TLS 1.1 handshake is refused
openssl s_client -connect localhost:5005 -tls1_1
```

The certificate can be swapped at runtime: update the files (or point `tls.cert_path`/`tls.key_path` elsewhere in `config.json`) and send `SIGHUP` — the new context is validated before activation and the old one is kept on failure.

> The bundled certificate is a self-signed demo certificate — replace it with a CA-issued certificate in production.

## Companion Client

A standalone non-blocking HTTPS client lives in `src/client/` and builds alongside the server. It verifies the server certificate against the bundled CA and sends a real HTTP/1.1 request:

```bash
./build/client                        # GET / on localhost:5005, verified against certs/server.crt
./build/client --path /api/two/hello  # any path
```

| Flag | Meaning |
| --- | --- |
| `--host <name>` | Hostname or IPv4 literal (default `localhost`) |
| `--port <n>` | Server port (default `5005`) |
| `--path <path>` | Request path (default `/`) |
| `--ca <file>` | Trust anchor used to verify the server (default `certs/server.crt`) |
| `--insecure` | Skip certificate verification — debugging only, never against a real server |
| `--no-tls` | Send plaintext HTTP instead of HTTPS (debugging/comparison only) |
| `-h`, `--help` | Usage |

The exit code is `0` only when a complete HTTP response was received, so the client doubles as a smoke test:

```bash
./build/client --host localhost || echo "unreachable"
```

> The bundled certificate has `CN=localhost` and no SAN, so `./build/client --host 127.0.0.1` fails the name check by design — that is hostname verification working, not a bug. The gateway is TLS-only, so `--no-tls` cannot talk to it (use it against plaintext servers).

## Debugging & Diagnostics

### AddressSanitizer (ASAN)

Debug builds enable AddressSanitizer automatically, detecting:
- Heap/stack buffer overflows
- Use-after-free
- Memory leaks

```bash
./build.sh Debug
./build/server   # ASAN reports are printed to stderr
```

### Logs

- Server log: `logs/epollserver.log`
- Client log: `logs/client.log`
- Colored console output synchronized with the log (handy during development)

## Design Notes

1. **HTTP parsing & pipelining** — a state machine parses the request line and headers incrementally as fragments arrive, without needing the full message; `Connection: keep-alive`/`close` are handled correctly, and pipelined requests on the same connection are processed strictly in request order.
2. **Zero-copy file sending & LRU cache** — static files first hit the in-memory LRU cache; on a miss they are sent via zero-copy `open + fstat + sendfile`; small files (≤ 1 MB by default) are read into the cache after their first send, and cache entries expire automatically based on the file's modification time.
3. **Send queue + EPOLLONESHOT cooperation** — the send queue is a `std::deque<std::vector<char>>` to keep front-deletion cheap; after each event the worker re-arms EPOLLIN/EPOLLOUT according to queue state and re-applies `EPOLLET | EPOLLONESHOT`, ensuring only one thread ever touches an fd at a time.
4. **Errors & path safety** — requests containing `..` are rejected with 403; unsupported methods on matched paths return 405 with an `Allow` header; missing files return 404 and internal errors 500; error responses automatically set Content-Length and Content-Type and close the connection.
5. **Logging & monitoring** — the CLF access log records every completed request and the AUDIT log records failures and security-policy events, both correlated by trace_id; timeouts and debug output can be lowered to trace level so normal runs stay quiet.
6. **Signals & graceful shutdown** — `SIGINT`/`SIGTERM` set a global atomic flag; workers check it on every epoll_wait timeout and exit their event loop; `SIGHUP` triggers runtime reload; destruction order guarantees Tcpserver → DynamicThreadPool → Logger::Guard, with logs closed last.
7. **Routing** — built-in routes are registered by `register_default_routes()` and config-driven routes by `register_configured_routes()`; each request is matched exactly once and the `ResolvedRoute` is reused across auth, rate limiting and dispatch; matching covers method/path/Host/Tenant with `local`/`upstream`/`static` targets, and GET/HEAD fall back to static file serving when no gateway route matches.
8. **Idle timeout** — each connection tracks its last-active time; when epoll_wait times out, expired connections are swept and closed, with a configurable threshold.
9. **Unit & integration tests** — Google Test, currently 101/101 passing, covering parsing (incl. smuggling vectors), caching, routing, auth, rate limiting, config schema validation, TLS hardening (shared across server and client), connection-pool semantics, upstream timeouts/retries/health checks, status snapshots and the circuit breaker; one command via `ctest`. Integration tests (77 assertions) cover end-to-end behavior with a real server + mock upstreams, including upstream TLS verification.

## Tech Stack

| Technology | Role |
|---|---|
| C++17 | Core language: RAII, move semantics, std::atomic, std::call_once, etc. |
| epoll | Linux I/O multiplexing, edge-triggered (ET) + ONESHOT |
| SO_REUSEPORT | Multi-worker load balancing |
| spdlog | High-performance async logging |
| CMake | Cross-platform build system |
| sendfile | Zero-copy file transfer |
| nlohmann/json | JSON parsing (header-only) |
| Google Test | Unit test framework |
| Docker | Containerized deployment |
| Docker Compose | Multi-container service orchestration |
| Prometheus | Metrics collection & time-series storage |
| Grafana | Metrics visualization dashboards |
| zlib | Gzip compression |
| OpenSSL | TLS/SSL encrypted transport |

## Performance

Measured on a 2-core dev box with wrk; the full report (environment, methodology, per-scenario P50/P99/QPS) lives in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) and is reproducible via `scripts/benchmark/run_benchmark.sh`.

| Scenario (light load, 8 connections) | Baseline | After TCP_NODELAY fix |
|---|---|---|
| Static cache-hit P50 latency | 43.0 ms | **8.0 ms (~5.4×)** |
| TLS handshake throughput (10 conn) | 224 QPS | **956 QPS (~4.3×)** |
| Reverse-proxy throughput (8 conn) | 169 QPS | **1000 QPS (~5.9×)** |

- The baseline famously caught a missing `TCP_NODELAY`: every request stalled ~43 ms on the Nagle + delayed-ACK interaction.
- Concurrency: handles 10,000+ concurrent connections with ease (subject to the system fd limit).
- Upstream connection pooling removes one TCP handshake per proxied request — the gain scales with backend RTT (invisible on loopback, significant cross-host).

## Known Limitations

- Circuit-breaker and health state is now shared process-wide (all workers + the admin API read the same state); load balancing beyond `round_robin` is still not implemented.
- Route/host/tenant dimensions currently expose latency sum/count only, without a dedicated histogram (so per-dimension P95/P99 cannot be derived directly).
- Runtime reload is polled by workers at checkpoints; workers do not switch config at the exact same instant.
- Route matching is a linear scan — fine at the current scale, with no Trie/index structure yet.
- The connection pool keeps only in-process idle connections; async upstream I/O and multi-algorithm load balancing (beyond `round_robin`) are not implemented yet.
- Upstream TLS verifies the backend certificate (chain + hostname) but does not yet present a client certificate, so mTLS-style mutual authentication with backends is not available; upstream health probes stay at the TCP level (reachability only, not TLS-layer checks).
- The admin API is a separate plain-HTTP listener (loopback by default) without built-in TLS (terminate TLS in a reverse proxy if it must be exposed).
- OpenTelemetry `traceparent`, distributed tracing and external audit storage are not integrated yet.

## Roadmap

1. ~~Add runtime-reload and real-HTTP end-to-end integration tests.~~ ✅ P1
2. ~~Add config field/range validation, route-conflict checks and reload-failure auditing.~~ ✅ P2 (with request-smuggling protection, TLS hardening and secret management)
3. Complete route/host/tenant latency histograms and failure-rate metrics.
4. ~~Evaluate a cross-worker shared circuit-breaker implementation.~~ ✅ P4 (`UpstreamManager` is now process-wide)
5. ~~Connection pooling~~ ✅ P3 (with the wrk baseline report); async upstream still open.
6. ~~P4: `/healthz` + `/readyz`, admin API, graceful-shutdown drain, deployment docs.~~ ✅ P4 (plus `/version`, a systemd unit and an upgrade guide)
7. P5: commercial features shaped by customer feedback.
8. Extend CI/CD: GitHub Actions releases and image publishing, plus Gitee CI.

## Documentation

- [Project status](docs/PROJECT_STATUS.md) — snapshot of current capabilities, limitations and next steps
- [Changelog](docs/CHANGELOG.md) — dated change history
- [Roadmap](docs/ROADMAP.md) — commercialization positioning, technical evolution and validation plan
- [Benchmarks](docs/BENCHMARKS.md) — wrk baseline report (P50/P99/QPS across three scenarios)
- [Deployment](docs/DEPLOYMENT.md) — systemd unit, admin API, rolling upgrade with graceful drain
- [Architecture article](docs/articles/01-architecture.md) — design decisions, threading model and three real bugs ([中文版](docs/articles/01-architecture.zh-CN.md))

## Project Structure

```
epollthread/
├── include/                  # Header files
│   ├── server/               # Server headers
│   │   ├── server.h          # TcpServer main class
│   │   ├── tcpworker.h       # TcpWorker thread (with SSL state machine)
│   │   ├── http_handler.h    # HTTP request handling & routing
│   │   ├── http_client.h     # Reverse-proxy forwarding & BackendError
│   │   ├── connection_pool.h # Per-upstream keep-alive connection pool
│   │   ├── tls_context.h     # Hardened TLS context builder
│   │   ├── upstream_manager.h# Upstream management & health checks
│   │   ├── rate_limiter.h    # Token-bucket rate limiter
│   │   ├── rate_limiter_manager.h # Rate-limiter manager
│   │   ├── api_key_manager.h # API key validation
│   │   ├── metrics.h         # Prometheus metrics collector (singleton, thread-safe)
│   │   ├── pool.h            # DynamicThreadPool
│   │   ├── config.h          # JSON config loading
│   │   ├── gzip_utils.h      # Gzip compression utilities
│   │   ├── content_type.h    # Content-Type mapping
│   │   ├── route_utils.h     # Route matching & parameter extraction
│   │   └── echohandler.h     # Echo handler (early demo)
│   ├── client/               # Client headers
│   │   ├── client.h          # Non-blocking client
│   │   └── clienthandler.h   # Client handler
│   └── common/               # Shared headers
│       ├── mysocket.h        # Socket RAII wrapper (with SSL support)
│       ├── poller.h          # Single-fd poll wait wrapper
│       ├── myepoll.h         # Epoll RAII wrapper
│       ├── mylogger.h        # Async logging wrapper
│       ├── http_parser.h     # HTTP/1.1 protocol parser (state machine)
│       ├── file_cache.h      # LRU in-memory file cache
│       ├── fd_cache.h        # FD cache (TTL-based)
│       └── error_utils.h     # Error handling utilities
├── src/                      # Source files
│   ├── server/               # Server sources
│   │   ├── main.cpp          # Server entry point
│   │   ├── server.cpp        # TcpServer implementation
│   │   ├── tcpworker.cpp     # TcpWorker implementation (incl. TLS handshake)
│   │   ├── http_handler.cpp  # HttpHandler implementation (route registration, rate limiting)
│   │   ├── http_client.cpp   # Reverse-proxy forwarding (precise response framing)
│   │   ├── connection_pool.cpp # Upstream connection pool implementation
│   │   ├── tls_context.cpp   # TLS hardening (min version, cipher whitelist)
│   │   ├── upstream_manager.cpp # Upstream management & health checks
│   │   ├── rate_limiter.cpp  # Token-bucket rate limiter
│   │   ├── metrics.cpp       # Metrics collector implementation
│   │   ├── pool.cpp          # Dynamic thread pool
│   │   ├── content_type.cpp  # Content-Type implementation
│   │   └── gzip_utils.cpp    # Gzip compression
│   ├── client/               # Client sources
│   │   ├── main.cpp          # Client entry point
│   │   ├── client.cpp        # Client implementation
│   │   └── clienthandler.cpp # ClientHandler implementation
│   └── common/               # Shared module sources
│       ├── poller.cpp        # Poller implementation
│       ├── mysocket.cpp      # Socket implementation (with SSL)
│       ├── myepoll.cpp       # Epoll implementation
│       ├── mylogger.cpp      # Logger implementation
│       ├── error_utils.cpp   # Error handling implementation
│       ├── fd_cache.cpp      # FD cache implementation
│       ├── file_cache.cpp    # File cache implementation
│       └── http_parser.cpp   # HTTP parser implementation
├── certs/                    # TLS certificate & private key
│   ├── server.crt
│   └── server.key
├── tests/                    # Unit tests
│   ├── CMakeLists.txt
│   ├── test_http_parser.cpp  # HTTP parser tests
│   ├── test_file_cache.cpp   # LRU cache tests
│   ├── test_http_response.cpp# HTTP response tests
│   ├── test_route_utils.cpp  # Route matching tests
│   ├── test_auth_and_rate_limit.cpp # API key & rate limiting tests
│   ├── test_http_client.cpp  # Upstream timeout & error classification tests
│   ├── test_upstream_manager.cpp # Upstream health-check tests
│   ├── test_config_validation.cpp # Config schema validation tests
│   ├── test_tls_context.cpp  # TLS context hardening tests
│   ├── test_connection_pool.cpp # Connection pool tests
│   └── integration/          # Integration tests (real server + mock upstream)
│       ├── run_integration_tests.py
│       └── mock_backend.py
├── scripts/
│   └── benchmark/
│       └── run_benchmark.sh  # One-command wrk baseline (static/proxy/handshake)
├── www/                      # Static file root
│   ├── index.html
│   └── big.html
├── logs/                     # Log output directory
├── build/                    # Build output directory (generated by cmake)
├── CMakeLists.txt            # CMake build configuration
├── build.sh                  # One-command build script
├── ci.sh                     # One-command CI (build + unit + integration tests)
├── start.sh                  # Quick start script
├── config.json               # Server configuration file
├── vcpkg.json                # vcpkg dependency manifest
├── Dockerfile                # Docker multi-stage build
├── docker-compose.yml        # Docker Compose stack (server + Prometheus + Grafana)
├── prometheus.yml            # Prometheus scrape configuration
├── docs/
│   ├── PROJECT_STATUS.md     # Project status snapshot (capabilities, limits, next steps)
│   ├── CHANGELOG.md          # Change history (reverse-chronological)
│   ├── BENCHMARKS.md         # wrk baseline report (P50/P99/QPS)
│   └── ROADMAP.md            # Commercialization roadmap (positioning, evolution, validation)
├── README.md                 # Documentation (English)
└── README.zh-CN.md           # Documentation (Simplified Chinese)
```

## License

Released under the MIT License.

## Acknowledgements

Thanks to spdlog, nlohmann/json, Google Test and the other excellent open-source projects this gateway builds on — and to Nginx for the architectural inspiration.

Stars and PRs welcome!
