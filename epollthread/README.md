[English](README.md) | [简体中文](README.zh-CN.md)

# epollthread

> Lightweight, self-contained API gateway in C++17 — a single binary built on epoll, with auth, rate limiting, circuit breaking and Prometheus metrics built in.

epollthread is a high-performance, multi-threaded HTTP/HTTPS API gateway and network server built on a **SO_REUSEPORT + epoll + One Loop Per Thread** architecture with asynchronous logging and non-blocking I/O. Out of the box it provides HTTP/1.1, TLS, keep-alive, zero-copy file serving, LRU/FD caching, config-driven routing, reverse proxying with upstream health checks, idempotent retries and circuit breaking, API-key authentication, host/tenant policies, token-bucket rate limiting, `X-Trace-Id` request tracing, dual AUDIT/CLF logging, `SIGHUP` runtime reload, upstream timeouts with error classification, Prometheus metrics and Docker deployment — plus unit tests (30/30 passing on CTest) and AddressSanitizer support.

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
- **HTTPS/TLS** — OpenSSL-based TLS using `certs/server.crt` and `certs/server.key`; handshake and encrypted I/O both run inside the non-blocking epoll loop.
- **Gzip compression** — text-like responses are gzip-compressed when negotiated via the client's `Accept-Encoding` header.
- **Chunked responses** — `Transfer-Encoding: chunked` support for dynamically generated or streamed content.
- **Status codes & error handling** — 200, 400, 403, 404, 405, 413, 429, 500, 502, 503, 504 and more; unreachable backends are distinguished from backend timeouts, path-traversal attacks are deflected, and a matched path with an unsupported method returns 405 with an `Allow` header.

### Gateway routing, auth & rate limiting
- **RESTful routing** — register handlers for any method + path pattern (e.g. `/users/{id}`) with dynamic parameter extraction and dispatch; build JSON APIs with ease.
- **Config-driven gateway routes** — `routes` entries declare method, path, host, tenant, auth, rate limiting and execution target; registration and dispatch are decoupled, with `local`, `upstream` and `static` target types.
- **API-key authentication** — keys are accepted via the `X-API-Key` header or `Authorization: Bearer`; supports route-level allow/deny lists and per-key host/tenant scopes with differentiated rate-limit quotas.
- **Token-bucket rate limiting** — a global limiter shared across workers (totals stay accurate under `SO_REUSEPORT` multi-worker mode), counted per client IP and returning `429 Too Many Requests`; fractional token refill avoids truncation errors.

### Reverse proxy & reliability
- **Reverse proxy & load balancing** — routes reference upstream groups via `UpstreamTarget`; `UpstreamManager` round-robins across healthy backends and probes them with active TCP health checks, automatically ejecting failed nodes.
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
- **External JSON configuration** — port, thread count, web root, thread-pool parameters, cache sizes, timeouts and more, making deployment and tuning easy.
- **Runtime reload** — `SIGHUP` triggers a hot reload: the signal handler only increments a generation counter, and workers read and apply the new config at safe checkpoints. Reload updates routes, upstreams, API keys, rate limiting and the keep-alive timeout, and an invalid file never overwrites the running config.
- **Companion non-blocking client** — an independent state-machine client covering connect/send/receive, demonstrating epoll from the client side.
- **Unit tests** — Google Test, currently 30/30 via CTest, covering the HTTP parser, LRU cache, response serialization, route matching, 404/405 semantics, path-traversal protection, API-key policy, rate-limit isolation, HTTP client timeouts, idempotent retries, upstream health checks and the circuit breaker.
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
    "upstreams": {                  // Upstream service definitions
        "test-service": {
            "servers": [
                {"host": "127.0.0.1", "port": 8081},
                {"host": "127.0.0.1", "port": 8082}
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

Integration tests — start the real server plus a mock upstream and verify end-to-end behavior (TLS, keep-alive, auth, rate limiting, failover, circuit breaking, reload, etc. — 14 scenario groups):

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
- **Timeouts & error classification** — connect, send and read phases each have timeout control; structured `BackendError` values distinguish failure types and map to 502/503/504.
- **Retries** — only idempotent methods and retryable transient errors are retried; `max_retries` controls the extra attempts per route.
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
- Reload updates routes, upstreams, API keys, default rate limiting and the keep-alive timeout.
- The JSON file is validated (exists and parses) before reload; an invalid config never replaces the currently active one.

## HTTPS

The server integrates OpenSSL and performs the TLS handshake automatically once the TCP connection is established, using `certs/server.crt` and `certs/server.key` as the certificate and private key. Clients must connect over HTTPS:

```bash
# -k skips self-signed certificate verification
curl -kv https://localhost:5005/

# Or test the TLS handshake directly with the openssl client
openssl s_client -connect localhost:5005
```

> The bundled certificate is a self-signed demo certificate — replace it with a CA-issued certificate in production.

## Companion Client

A standalone non-blocking TCP client lives in `src/client/` and builds alongside the server:

```bash
./build/client
```

> The client connects to `192.168.189.138:5005` by default; adjust the target address in `src/client/main.cpp` if needed.

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
9. **Unit tests** — Google Test, currently 30/30 passing, covering parsing, caching, routing, auth, rate limiting, upstream timeouts/retries/health checks and the circuit breaker; one command via `ctest`.

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

- Concurrency: handles 10,000+ concurrent connections with ease (subject to the system fd limit).
- Throughput: with caching enabled, repeated requests for small static files (e.g. index.html) reach several times the baseline QPS — tens of thousands of QPS per worker.
- Latency: request processing sits in the microsecond range; zero-copy + in-memory caching keeps CPU usage very low.
- Detailed benchmark reports are forthcoming.

## Known Limitations

- Circuit-breaker state is maintained independently by each worker — it is not a global, cross-worker shared state.
- Route/host/tenant dimensions currently expose latency sum/count only, without a dedicated histogram (so per-dimension P95/P99 cannot be derived directly).
- Runtime reload is polled by workers at checkpoints; workers do not switch config at the exact same instant.
- Config validation currently amounts to "the JSON parses"; field types, ranges and route-conflict checks are not yet covered.
- Route matching is a linear scan — fine at the current scale, with no Trie/index structure yet.
- Connection pooling, async upstream, multi-algorithm load balancing and distributed rate limiting are not implemented yet.
- OpenTelemetry `traceparent`, distributed tracing and external audit storage are not integrated yet.

## Roadmap

1. Add runtime-reload and real-HTTP end-to-end integration tests.
2. Add config field/range validation, route-conflict checks and reload-failure auditing.
3. Complete route/host/tenant latency histograms and failure-rate metrics.
4. Evaluate a cross-worker shared circuit-breaker implementation.
5. Decide on a route index, connection pooling or async upstream based on real route counts and benchmark results.
6. HTTP/2 and WebSocket support.
7. Stress testing and performance profiling reports.
8. Extend CI/CD: GitHub Actions releases and image publishing, plus Gitee CI.

## Documentation

- [Project status](docs/PROJECT_STATUS.md) — snapshot of current capabilities, limitations and next steps
- [Changelog](docs/CHANGELOG.md) — dated change history
- [Roadmap](docs/ROADMAP.md) — commercialization positioning, technical evolution and validation plan

## Project Structure

```
epollthread/
├── include/                  # Header files
│   ├── server/               # Server headers
│   │   ├── server.h          # TcpServer main class
│   │   ├── tcpworker.h       # TcpWorker thread (with SSL state machine)
│   │   ├── http_handler.h    # HTTP request handling & routing
│   │   ├── http_client.h     # Reverse-proxy forwarding & BackendError
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
│   │   ├── http_client.cpp   # Reverse-proxy forwarding
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
│   └── integration/          # Integration tests (real server + mock upstream)
│       ├── run_integration_tests.py
│       └── mock_backend.py
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
│   └── ROADMAP.md            # Commercialization roadmap (positioning, evolution, validation)
├── README.md                 # Documentation (English)
└── README.zh-CN.md           # Documentation (Simplified Chinese)
```

## License

Released under the MIT License.

## Acknowledgements

Thanks to spdlog, nlohmann/json, Google Test and the other excellent open-source projects this gateway builds on — and to Nginx for the architectural inspiration.

Stars and PRs welcome!
