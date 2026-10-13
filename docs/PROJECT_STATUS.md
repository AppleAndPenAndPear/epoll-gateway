# Project Status

## Current Stage

Building the core capability set of a commercial API gateway. The regression safety net, config/security hardening, performance baseline, operations productization (ops endpoints, admin API, graceful shutdown, deployment docs), bounded async upstream execution, composite (fan-out) routes, W3C `traceparent` propagation, the access-log/redaction policy, the 2026-10-10 correctness/security hardening pass, resource budgets with documented overload behaviour, the supply-chain gate (SAST/SBOM/SCA), and per-route/host/tenant latency histograms with failure-rate metrics are complete. Next, in priority order: **profile the known Nginx throughput gap** (isolate log-write, buffer-copy and TLS-record costs) before further optimization, then commercial features shaped by customer feedback. mTLS, OIDC/JWT, cluster control planes, distributed quotas and AI-gateway features are explicitly deferred until a concrete customer requires them (see [ROADMAP.md](ROADMAP.md) §4).

## Current Validation

- C++17 Release build passes.
- Native Google Test suite: 144/144 passing (2026-10-13, `./build/tests/runTests`).
- Integration tests: 138 assertions passing (2026-10-13, `python3 tests/integration/run_integration_tests.py`, launching a real server + mock upstreams).
- Covered: HTTP parser (incl. smuggling vectors, bare-LF and whitespace-before-colon rejection, full chunked-trailer consumption), file cache, response serialization, routing, security policies, upstream timeouts, retries, health checks, circuit breaker basics, config schema validation (incl. admin section), TLS context hardening (incl. the policy shared with the companion client and its CA/hostname verification settings), upstream TLS (pooled-connection identity separated by verification policy), connection-pool reuse and eviction, upstream health/circuit status snapshots, and route-level metrics (cumulative histogram buckets + failure counters broken down by cause).
- Integration layer covers TLS (incl. TLS 1.1 refusal and cert hot reload), the companion C++ client (verified request + hostname-mismatch rejection), upstream TLS (verified https:// backend incl. hostname-mismatch rejection, skip-verify escape hatch, pool reuse over TLS), Keep-Alive, Trace-Id, 405+Allow, 404/403, authentication (keys loaded from a separate file), rate limiting, failover, circuit breaking, reload, corrupted-config rejection, /metrics, /healthz + /readyz + /version, probe rate-limit exemption, admin API (auth + stats + upstream status + reload), Chunked (incl. trailer section), connection-pool reuse (10 requests ≤4 backend connections), forwarded headers (`X-Forwarded-Proto`/`X-Forwarded-Host`, spoofed `X-Real-IP` overwritten), hop-by-hop stripping on both hops, upstream-response header-injection rejection, single-pass gzip of an already-encoded backend response, bounded composite fan-out, and SIGTERM graceful shutdown (in-flight request completes, new requests refused, clean exit).
- Benchmark report: [BENCHMARKS.md](BENCHMARKS.md) — P50/P99/QPS across static, proxy, and TLS-handshake scenarios, reproducible via `scripts/benchmark/run_benchmark.sh`.
- First-run experience: a 5-minute quickstart at the top of both READMEs (built-in echo routes answer with no backend; the admin auth demo uses a key the operator sets — see the note on default credentials below); runnable scenarios under [examples/](../examples/) — load balancing with health checks and a verified `https://` upstream; dev certificates are generated locally by `scripts/gen_dev_certs.sh` and never committed (`certs/` is gitignored); the server accepts an optional config path argument (`./build/server examples/load-balanced/config.json`).
- Default credentials: the committed `config.json` ships **no** API keys and `admin.enabled: false`, so a fresh deployment authenticates nobody until configured. Supply data-plane keys via `api_keys_file` (a JSON array with the same schema) or the `GW_API_KEYS` environment variable, and enable the admin listener by setting `admin.enabled: true` with a non-empty `admin.api_keys`. The `examples/*/config.json` files keep demo keys because they are sample configs referenced by the README walkthroughs; they are not packaged as production defaults.
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
- The upstream hop carries the caller identity and request-origin headers: `X-Forwarded-For` (appended), `X-Real-IP` (overwritten, never appended, so a client-supplied value cannot survive), `X-Forwarded-Proto` (`https`), and `X-Forwarded-Host`; hop-by-hop headers (RFC 7230 §6.1, incl. `Proxy-*`) are stripped on both the request and response hops, and upstream response headers carrying CR/LF are dropped to prevent header injection.

### 6. Static Files and Caching

- Static file serving with common Content-Type inference.
- `sendfile` zero-copy for large files.
- TLS static-file writes retain the active chunk across SSL WANT_READ/WANT_WRITE and continue from the exact file offset after readiness resumes.
- Per-worker LRU content cache for small files.
- FD cache based on TTL and mtime.
- Gzip content goes into a dedicated compressed cache.
- Static file paths go through security checks to prevent `..` path traversal.
- Static fallback for unmatched routes only allows GET/HEAD, preventing methods like POST from reading static resources.

### 7. Observability and Logging

- When a request carries no external trace ID, the gateway generates `trace_id`.
- A client-provided `X-Trace-Id` is preserved and echoed back.
- W3C `traceparent` is generated/propagated across the upstream hop and echoed back in responses, retaining `X-Trace-Id` compatibility.
- A composite route emits a structured dependency payload with `status`, `required`, `traceparent`, and timing for each dependency; required failures aggregate into a `partial_failure` response.
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
- `logging.access_log_enabled` controls CLF independently of AUDIT; configured sensitive headers and query parameters are redacted by default in both log types, and API keys are always fully masked.
- Request and response bodies are not emitted in CLF or AUDIT logs.
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

- Bounded async upstream boundary (2026-10-08): bounded executor, group-aware overload rejection, and route-to-upstream group semantics are implemented and validated.
- API composition MVP (2026-10-08): composite profile-route fan-out is implemented and validated end-to-end; success and partial-failure paths pass.
- TLS write path (2026-10-09): SSL WANT_READ/WANT_WRITE re-arms the correct readiness direction; TLS static-file sends retain pending chunks and continue across backpressure; verified by an 8 MiB delayed-reader integration case.
- Google Test suite currently 144/144 passing (2026-10-13).
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
	- Shared TLS hardening policy (minimum version, AEAD whitelist, compression off) for both server and client contexts
	- Companion client TLS context (CA file required, rejection of a missing/empty CA, `--insecure` bypass)
	- Connection pool reuse, bucketing, idle eviction, cap truncation, invalidation
	- Upstream timeouts, connection failures, and idempotent retries
	- Upstream health checks
	- Circuit breaker open/reject/recovery probing
	- Route-level metrics (cumulative histogram buckets + failure counters by cause)
- Integration suite (138 assertions) proves end-to-end behavior including connection reuse, TLS policy, the companion client's certificate/hostname verification, forwarded headers, hop-by-hop stripping, header-injection rejection, single-pass gzip, and the route/host/tenant metric families.
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

- Runtime reload uses periodic worker polling; workers are not guaranteed to switch configuration at exactly the same instant.
- Route matching is still a linear scan — simple and reliable at the current scale, with no Trie or index structure for large route sets yet.
- The upstream connection pool keeps only idle connections in memory (no cross-process sharing); load balancing currently supports `round_robin` only (upstream I/O is now scheduled through the bounded async executor).
- OpenTelemetry export and full distributed tracing are not integrated; W3C `traceparent` propagation and external audit storage are not yet provided.
- A dynamic thread pool interface exists, but the main HTTP path still mostly runs on worker threads.
- **Resource budgets are enforced, documented and covered by an overload test.** Every resource has an explicit limit and a defined overload behaviour, tabulated in [DEPLOYMENT.md](DEPLOYMENT.md) §11, including the process-wide `max_connections` (accept-time load shedding), the configurable `max_response_bytes` cap on the dominant memory term (with a memory model), and configurable composite fan-out limits. A sustained load test drives demand above `max_connections` and asserts shedding, survivability, recovery and a bounded fd count. It runs for a few seconds by default to keep CI quick; a longer soak (e.g. `EPOLL_LOAD_TEST_SECONDS=300`) is recommended before a production rollout to catch slow leaks the short run cannot.
- **A supply-chain gate now exists** (`./ci.sh supply-chain`, run by the CI `test` job after build/unit/integration): `cppcheck` SAST (blocking only on `error` severity), an offline CycloneDX 1.5 SBOM of `build/server`'s linked shared libraries (`build/sbom.cdx.json`), and an `osv-scanner` SCA pass over that SBOM. A missing tool is reported as SKIPPED with its install command, never a silent pass. Still manual/local: the gate is deliberately not part of `./ci.sh all` (it needs tools a dev box may lack), SCA needs network access to the OSV database, the SBOM reflects the build host's libraries (cross-distribution runtime resolution is covered separately by `verify_artifact.sh`), and the gate has not yet run on a release tag. See [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md).
- Backend mTLS (client certificates), OIDC/JWT, distributed quotas and multi-instance state sharing are deliberately not implemented; the project is single-node and assumes no external coordination service.

## Next Steps

1. Profile the known Nginx throughput gap (isolate log-write, buffer-copy and TLS-record costs) before further optimization; record evidence in [BENCHMARKS.md](BENCHMARKS.md).
2. Deferred until a concrete customer requires them: OIDC/JWT, upstream mTLS, distributed quotas, cluster control plane, and AI-gateway features; each claimed capability needs integration coverage and deployment guidance.

Two prerequisites from the previous list are now complete: the supply-chain gate (SAST/SBOM/SCA) runs in CI, and the per-route/host/tenant latency histograms plus failure-rate metrics are implemented.

## Recent Decisions

- `register_default_routes()` serves built-in demo and basic endpoints; `register_configured_routes()` generates gateway routes from configuration.
- Route matching runs once, and the result is reused for the request lifetime.
- Upstream work is scheduled through a bounded async executor with per-group concurrency isolation; the route-to-group identity is resolved once via `resolve_upstream_group_id()`.
- A `composite` route fans out named dependencies (required vs optional) and returns an explicit `partial_failure` (503) versus `ok` (200).
- CLF records completed requests when enabled; AUDIT focuses on failures and security policy events and remains enabled when CLF is disabled.
- Plain `Request:` entry logs are downgraded to debug, avoiding duplicate production info logs alongside CLF.
- API keys are always fully masked; configured sensitive headers and query parameters are redacted by default in CLF and AUDIT.
- A matched route with an unsupported method returns 405 with an `Allow` header; only a nonexistent path falls into 404 or static fallback.
- Invalid configuration fails fast at startup instead of silently running on defaults; rejected reloads keep the active config and write AUDIT.
- API keys load with precedence env (`GW_API_KEYS`) > `api_keys_file` > inline config, so secrets can stay out of the main config entirely.
- Connection-pool retries after send are restricted to idempotent methods; POSTs are never resent internally.
- API-key validation (data plane and admin) always uses a constant-time comparison that scans the full key set with no early return, so response timing cannot reveal a matched prefix.
- The shipped `config.json` intentionally contains no usable credentials and disables the admin listener; deployments must supply keys explicitly (`api_keys_file` / `GW_API_KEYS` / `admin.api_keys`). This trades a little first-run convenience for not shipping working credentials in Docker images and release tarballs.
- Parser hardening rejects bare-LF line endings and whitespace before a header colon (both request-smuggling vectors), and fully consumes chunked trailer sections so a pooled connection is never left out of sync.
