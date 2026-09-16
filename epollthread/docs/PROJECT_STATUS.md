# Project Status

## Current Stage

Building the core capability set of a commercial API gateway. The evolution from an epoll HTTP server to a configuration-driven gateway is complete; work is now closing out maintainability, protocol semantics, and production operations.

## Current Validation

- C++17 Release build passes.
- CTest: 30/30 passing.
- Integration tests: 32/32 passing (`tests/integration/run_integration_tests.py`, launching a real server + mock upstreams).
- Covered: HTTP parser, file cache, response serialization, routing, security policies, upstream timeouts, retries, health checks, and circuit breaker basics.
- Integration layer covers TLS, Keep-Alive, Trace-Id, 405+Allow, authentication, rate limiting, failover, circuit breaking, reload, corrupted-config rejection, /metrics, and Chunked.
- Debug builds support AddressSanitizer.

## Completed

### 1. Network and Concurrency Foundation

- Non-blocking Reactor based on Linux `epoll`.
- `SO_REUSEPORT + One Loop Per Worker`: each `TcpWorker` owns an independent listen socket and epoll loop.
- `EPOLLET + EPOLLONESHOT` event management, avoiding redundant triggering and thundering herd.
- Socket, Epoll, and other resources wrapped in RAII.
- Graceful shutdown: `SIGINT`/`SIGTERM` notify workers to exit and drain connections.
- Keep-Alive, idle connection timeout, and serial multi-request handling on a single connection.

### 2. HTTP/HTTPS Protocol Support

- HTTP/1.1 request state-machine parsing.
- Request line, header, query, body, and Chunked transfer parsing.
- Currently supports `GET`, `HEAD`, `POST`, `PUT`, `DELETE`.
- OpenSSL non-blocking TLS handshake and encrypted read/write.
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

- `Config::from_file()` parses JSON configuration in one place.
- Routes, upstreams, API keys, rate limiting, timeouts, and static root are all configurable.
- `SIGHUP` triggers runtime reload.
- The signal handler only increments the reload generation; it performs no file IO or JSON parsing.
- Workers read and apply the new configuration at safe checkpoints.
- Reload updates routes, upstreams, API keys, default rate-limit settings, and Keep-Alive timeouts.
- Before reload, the JSON file is checked for existence and validity, preventing a corrupted config from overwriting the currently working one.
- Dockerfile, docker-compose, Prometheus configuration, and startup scripts are provided.

### 9. Testing and Engineering

- Google Test suite currently 30/30 passing.
- Covered:
	- HTTP request parsing
	- Query and body
	- LRU file cache
	- FD/response basics
	- Route parameters and Host/Tenant matching
	- 404/405 routing semantics basics
	- Static path traversal protection
	- API key authorization
	- Rate limiter and per-key isolation
	- Upstream timeouts, connection failures, and idempotent retries
	- Upstream health checks
	- Circuit breaker open/reject/recovery probing

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
- `forward_request` reads backend responses with `Connection: close` semantics (satisfied by Content-Length or EOF); forwarding of Chunked upstream responses is not handled yet.
- The route/host/tenant dimensions currently provide latency sum/count but no dedicated histogram, so per-dimension P95/P99 is not directly available.
- Runtime reload uses periodic worker polling; workers are not guaranteed to switch configuration at exactly the same instant.
- Current config validation mostly verifies that the JSON is parseable; full field type, range, and route-conflict validation is not yet provided.
- Route matching is still a linear scan — simple and reliable at the current scale, with no Trie or index structure for large route sets yet.
- Connection pooling, async upstream, multi-level load balancing algorithms, and distributed rate limiting are not implemented.
- OpenTelemetry `traceparent`, distributed tracing, and external audit storage are not yet integrated.
- Testing is mostly unit tests; real TLS, HTTP/1.1 long connections, reload, and end-to-end upstream scenarios still need integration tests.
- A dynamic thread pool interface exists, but the main HTTP path still mostly runs on worker threads.

## Next Steps

1. Add runtime reload and real HTTP end-to-end tests.
2. Add config field range validation, route conflict checks, and reload failure auditing.
3. Complete per-route/host/tenant latency histograms and failure-rate metrics.
4. Evaluate an implementation for cross-worker shared circuit breaker state.
5. Decide on route indexing, connection pooling, or async upstream based on real route scale and load test results.

## Recent Decisions

- `register_default_routes()` serves built-in demo and basic endpoints; `register_configured_routes()` generates gateway routes from configuration.
- Route matching runs once, and the result is reused for the request lifetime.
- CLF records all access; AUDIT focuses on failures and security policy events.
- Plain `Request:` entry logs are downgraded to debug, avoiding duplicate production info logs alongside CLF.
- Only masked API keys are kept in AUDIT to prevent credential leakage.
- A matched route with an unsupported method returns 405 with an `Allow` header; only a nonexistent path falls into 404 or static fallback.
