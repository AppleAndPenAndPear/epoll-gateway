# Project Status

## Current Stage

Building the core capability set of a commercial API gateway. P1 (regression safety net), P2 (config + security hardening), P3 (performance baseline) and P4 (operations productization: ops endpoints, admin API, graceful shutdown, deployment docs) are complete. Next: P5, commercial features shaped by customer feedback.

## Current Validation

- C++17 Release build passes.
- CTest: 91/91 passing.
- Integration tests: 69/69 passing (`tests/integration/run_integration_tests.py`, launching a real server + mock upstreams).
- Covered: HTTP parser (incl. smuggling vectors), file cache, response serialization, routing, security policies, upstream timeouts, retries, health checks, circuit breaker basics, config schema validation (incl. admin section), TLS context hardening, connection-pool reuse and eviction, upstream health/circuit status snapshots.
- Integration layer covers TLS (incl. TLS 1.1 refusal and cert hot reload), Keep-Alive, Trace-Id, 405+Allow, 404/403, authentication (keys loaded from a separate file), rate limiting, failover, circuit breaking, reload, corrupted-config rejection, /metrics, /healthz + /readyz + /version, probe rate-limit exemption, admin API (auth + stats + upstream status + reload), Chunked (incl. trailer section), connection-pool reuse (10 requests ≤2 backend connections), and SIGTERM graceful shutdown (in-flight request completes, new requests refused, clean exit).
- Benchmark report: [BENCHMARKS.md](BENCHMARKS.md) — P50/P99/QPS across static, proxy, and TLS-handshake scenarios, reproducible via `scripts/benchmark/run_benchmark.sh`.
- Deployment: [DEPLOYMENT.md](DEPLOYMENT.md) — systemd unit, rolling upgrade with graceful drain, admin API usage.
- Debug builds support AddressSanitizer.

## Completed

### 1. Network and Concurrency Foundation

- Non-blocking Reactor based on Linux `epoll`.
- `SO_REUSEPORT + One Loop Per Worker`: each `TcpWorker` owns an independent listen socket and epoll loop.
- `EPOLLET + EPOLLONESHOT` event management, avoiding redundant triggering and thundering herd.
- Socket, Epoll, and other resources wrapped in RAII.
- Graceful shutdown (P4): SIGINT/SIGTERM stop accepting first, close idle keep-alive connections, and drain in-flight requests up to `shutdown_drain_timeout` (default 30 s) before exiting; pairs with systemd `TimeoutStopSec`.
- Keep-Alive, idle connection timeout, and serial multi-request handling on a single connection.

### 2. HTTP/HTTPS Protocol Support

- HTTP/1.1 request state-machine parsing.
- Request line, header, query, body, and Chunked transfer parsing.
- Currently supports `GET`, `HEAD`, `POST`, `PUT`, `DELETE`.
- OpenSSL non-blocking TLS handshake and encrypted read/write.
- Hardened TLS context: minimum TLS 1.2, AEAD-only cipher whitelist (ECDHE+GCM/ChaCha20), no compression, session cache + tickets for resumption; certificate/private key hot-reloadable via SIGHUP (`tls.cert_path`/`tls.key_path`).
- Request-smuggling protection: duplicate `Content-Length`, CL+TE mixing, non-chunked `Transfer-Encoding`, non-numeric `Content-Length`, invalid header characters, control chars in the request line, non-hex chunk sizes, and oversized request lines/headers are all rejected with 400 + `Connection: close`.
- Responses support Content-Length, Chunked, Content-Type, and Content-Encoding.
- Gzip response compression with `Accept-Encoding` negotiation.
- Uniform `X-Trace-Id` for request and log correlation.

### 3. Routing and Request Dispatch

- Built-in routes are registered via `register_default_routes()`.
- Configured routes are registered via `register_configured_routes()`.
- Route configuration comes from the `routes` field of `config.json`.
- Four match conditions: method, path, Host, and Tenant.
- Exact paths, wildcard paths, and path parameters, e.g. `/users/{id}`.
- Route matching runs exactly once per request; the `ResolvedRoute` is reused across authentication, rate limiting, and actual dispatch.
- Three target types:
	- `local`: local handler.
	- `upstream`: reverse proxy to a backend service.
	- `static`: serve files from a configured static root.
- Distinguished:
	- `404`: no match on path, Host, or Tenant.
	- `405`: path matches but method not allowed.
- `405` responses include an `Allow` header listing the permitted methods.
- When no gateway route matches, GET/HEAD can fall back to static file serving.

### 4. Authentication, Tenancy, and Rate Limiting

- Two API key extraction styles: `X-API-Key` and `Authorization: Bearer`.
- Route-level API key allow/deny lists.
- Host-scope and Tenant-scope binding for API keys.
- Route-level auth toggle and anonymous access configuration.
- Token Bucket based rate limiting.
- Rate limiting by client IP, API key, or route.
- RateLimiterManager is shared across workers, with periodic eviction of long-unused limiters.
- Each API key can have its own rate-limit capacity and refill rate.

### 5. Upstream Proxying and Reliability

- `UpstreamManager` maintains upstream service groups and backend nodes.
- Round-robin backend selection.
- Active TCP health checks that mark unhealthy nodes.
- Timeout control across connect, write, and read phases.
- Structured `BackendError` distinguishes:
	- Connection failure
	- Connection timeout
	- Write failure
	- Write timeout
	- Read failure
	- Read timeout
	- Invalid upstream response
- The retry policy only allows idempotent methods and retryable transient errors.
- Routes can configure extra retries via `max_retries`.
- Per-upstream keep-alive connection pool (`connection_pool.cpp`):
	- `checkout`/`checkin` with a 60 s idle timeout and a max of 16 idle connections per upstream; fd lifetime managed by `shared_ptr<Socket>` RAII.
	- Backend responses are framed precisely (Content-Length / chunked incl. trailer section / close-delimited); read-ahead `leftover` bytes travel with the connection.
	- Stale connections (closed by the backend while idle) are discarded by a 0-timeout `MSG_PEEK` probe at checkout; post-send failures are retried only for idempotent methods.
- A basic circuit breaker per upstream/backend:
	- Enters OPEN after consecutive failures reach `circuit_failure_threshold`.
	- Allows recovery probing after `circuit_recovery_timeout_ms`.
	- `half_open_probe` prevents multiple requests from hitting a recovering backend at once.
	- A successful probe closes the breaker; a failure keeps it open.
- Upstream errors map to 502/503/504 and are recorded in Metrics and audit categories.

### 6. Static Files and Caching

- Static file serving with common Content-Type inference.
- `sendfile` zero-copy for large files.
- Per-worker LRU content cache for small files.
- FD cache based on TTL and mtime.
- Gzip content goes into a dedicated compressed cache.
- Static file paths go through security checks to prevent `..` path traversal.
- Static fallback for unmatched routes only allows GET/HEAD, preventing methods like POST from reading static resources.

### 7. Observability and Logging

- When a request carries no external trace ID, the gateway generates `trace_id`.
- A client-provided `X-Trace-Id` is preserved and echoed back.
- AUDIT logs record failures and security policy events, including:
	- trace_id
	- method/path
	- Host/Tenant
	- route
	- status
	- failure reason
	- masked API key
	- User-Agent
- AUDIT is written once at response completion, avoiding duplicate writes across auth, rate-limit, and routing branches.
- CLF access logs record client IP, method, path, status code, response size, and duration for every completed request.
- Plain `Request:` entry logs are downgraded to debug, avoiding duplication with CLF at production info level.
- The Prometheus `/metrics` endpoint provides:
	- Global 2xx/3xx/4xx/5xx counters
	- Global latency histogram
	- File cache, FD cache, and Gzip cache statistics
	- Upstream error type statistics
	- Request counts and latency sum/count per route/host/tenant

### 8. Configuration and Runtime Operations

- `Config::from_file()` parses JSON configuration in one place, with type/range-checked reads.
- `Config::validate()` enforces field ranges (ports 1-65535, thresholds > 0, `min_threads <= max_threads`), duplicate route names/matches, upstream reference existence, `static` routes having `static_root`, and api key uniqueness/non-emptiness. Startup fails fast on an invalid config.
- Routes, upstreams, API keys, rate limiting, timeouts, and static root are all configurable.
- API keys can be loaded from outside the main config: `api_keys_file` (JSON array, same schema) or the `GW_API_KEYS` environment variable; precedence env > file > inline. Inline keys still work but log a security hint.
- New `tls` config section: `cert_path`/`key_path`, hot-reloaded on SIGHUP with AUDIT logging of applied/rejected.
- `SIGHUP` triggers runtime reload.
- The signal handler only increments the reload generation; it performs no file IO or JSON parsing.
- Workers read and apply the new configuration at safe checkpoints.
- Reload updates routes, upstreams, API keys, default rate-limit settings, Keep-Alive timeouts, and the TLS certificate.
- Before reload, the JSON is schema-validated; an invalid config never overwrites the currently working one and is recorded as `AUDIT config_reload_rejected`.
- Dockerfile, docker-compose, Prometheus configuration, and startup scripts are provided.
- Operations endpoints (P4): `/healthz` (liveness), `/readyz` (readiness — every configured upstream needs ≥1 healthy backend; JSON healthy/total breakdown, 503 otherwise), `/version` (build version from `project(VERSION)` via a generated header). Probe endpoints are exempt from auth and rate limiting.
- Admin API (P4, `admin` config section: enabled/port/bind/api_keys): a separate listener (loopback by default) with mandatory key auth and constant-time comparison; serves `GET /admin/stats` (counters/latency/uptime/version), `GET /admin/upstreams` (per-backend health + circuit state) and `POST /admin/reload` (validates first, 400 + AUDIT on invalid config, otherwise triggers the worker reload path). Auth failures are AUDIT logged.
- `UpstreamManager` is shared process-wide (all workers + admin) so health/circuit state has a single source of truth.
- systemd unit (`deploy/gateway.service`) and a deployment/upgrade guide (`docs/DEPLOYMENT.md`) with a zero-downtime rolling-upgrade procedure built on graceful drain.

### 9. Testing and Engineering

- Google Test suite currently 91/91 passing.
- Covered:
	- HTTP request parsing, incl. request-smuggling vectors (duplicate CL, CL+TE, header characters, line limits)
	- Query and body
	- LRU file cache
	- FD/response basics
	- Route parameters and Host/Tenant matching
	- 404/405 routing semantics basics
	- Static path traversal protection
	- API key authorization and key-source precedence (env > file > inline)
	- Rate limiter and per-key isolation
	- Config schema validation
	- TLS context builder hardening
	- Connection pool reuse, bucketing, idle eviction, cap truncation, invalidation
	- Upstream timeouts, connection failures, and idempotent retries
	- Upstream health checks
	- Circuit breaker open/reject/recovery probing
- Integration suite (42 assertions) proves end-to-end behavior including connection reuse and TLS policy.
- Benchmark harness `scripts/benchmark/run_benchmark.sh` + report in [BENCHMARKS.md](BENCHMARKS.md).

## Request Flow

```text
Client connects
	-> TLS handshake (HTTPS)
	-> epoll reads data
	-> HttpParser parses the full request
	-> trace_id generated or received
	-> Host/Tenant extracted
	-> Single route match
	-> 404/405 decision
	-> API key authentication
	-> Rate limiting
	-> Local handler / static file / upstream forwarding
	-> Upstream timeout, retry, and circuit breaking
	-> Response built
	-> Written back to client
	-> Metrics recorded
	-> AUDIT written on failure
	-> CLF written for every completed request
```

## Known Limitations

- Circuit breaker state is currently maintained independently per worker, not as a global state shared across all workers.
- The route/host/tenant dimensions currently provide latency sum/count but no dedicated histogram, so per-dimension P95/P99 is not directly available.
- Runtime reload uses periodic worker polling; workers are not guaranteed to switch configuration at exactly the same instant.
- Route matching is still a linear scan — simple and reliable at the current scale, with no Trie or index structure for large route sets yet.
- The upstream connection pool keeps only idle connections in memory (no cross-process sharing); async upstream I/O and multi-algorithm load balancing (beyond round_robin) are not implemented.
- No `/healthz`/`/readyz` endpoints or admin API yet (planned for P4).
- OpenTelemetry `traceparent`, distributed tracing, and external audit storage are not yet integrated.
- A dynamic thread pool interface exists, but the main HTTP path still mostly runs on worker threads.

## Next Steps

1. P4: add `/healthz` (liveness) and `/readyz` (readiness) endpoints.
2. P4: graceful-shutdown drain refinement (stop accepting first, wait for in-flight requests) and deployment docs.
3. P4: admin API (separate listen address, authenticated) for hot config updates and upstream/breaker status.
4. Complete per-route/host/tenant latency histograms and failure-rate metrics.
5. Evaluate an implementation for cross-worker shared circuit breaker state.

## Recent Decisions

- `register_default_routes()` serves built-in demo and basic endpoints; `register_configured_routes()` generates gateway routes from configuration.
- Route matching runs once, and the result is reused for the request lifetime.
- CLF records all access; AUDIT focuses on failures and security policy events.
- Plain `Request:` entry logs are downgraded to debug, avoiding duplicate production info logs alongside CLF.
- Only masked API keys are kept in AUDIT to prevent credential leakage.
- A matched route with an unsupported method returns 405 with an `Allow` header; only a nonexistent path falls into 404 or static fallback.
- Invalid configuration fails fast at startup instead of silently running on defaults; rejected reloads keep the active config and write AUDIT.
- API keys load with precedence env (`GW_API_KEYS`) > `api_keys_file` > inline config, so secrets can stay out of the main config entirely.
- Connection-pool retries after send are restricted to idempotent methods; POSTs are never resent internally.
