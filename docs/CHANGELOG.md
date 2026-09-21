# Changelog

Notable changes to the project. Format follows [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/), entries sorted newest first.

> For a detailed snapshot of the current project state, see [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md). This file traces back "what was done, when, and why".

## 2026-09-21

### Added

- Admin key hot rotation: the admin listener watches the reload generation and adopts a new `admin.api_keys` list from `config.json` within ~1 second of any accepted reload (SIGHUP or `POST /admin/reload`), so rotating the admin key no longer requires a restart. A refresh never adopts an invalid config or an empty key list — the previous keys are kept and an AUDIT `admin_keys_refresh_failed` line is written; success logs AUDIT `admin_keys_rotated`. `enabled`/`port`/`bind` remain startup-fixed (rebuilding a listener at runtime has messy failure modes for near-zero value). Integration tests cover rotation: old key 401, new key 200.

### Fixed

- Admin accept loop had no exception guard: an uncaught exception in the thread calls `std::terminate`, so a failure in the (file-reading, JSON-parsing) key refresh or in JSON response building would have killed the whole gateway, data plane included. The loop body is now wrapped in try/catch and logs AUDIT `admin_request_failed` — enforcing the guarantee the surrounding comment already claimed.
- `POST` to an admin endpoint with a request body could lose its response: the body is deliberately never parsed, and closing a socket with unread received data makes the kernel send RST instead of FIN, which can discard the response just written. The rest of the body (per `Content-Length`, bounded, 1 s cap) is now drained after the response and before the socket closes; covered by an integration assertion.
- `admin.api_keys` parsing did not clear the target list first, unlike the data-plane `api_keys` parsing; a reused `Config` would accumulate keys, keeping revoked ones valid. Defensive today (a fresh `Config` per parse) but a security footgun if the parse path is ever reused.
- `Config::validate()` did not check `admin.bind`, so a non-IPv4-literal bind passed validation and only failed later inside `AdminServer::start()` as a generic startup error. Now reported as a field-level `admin.bind` error.
- `deploy/gateway.service`: the install notes (unlike DEPLOYMENT.md §1) omitted creating the `epoll-gateway` user and chowning the working directory, so following the unit's own instructions produced a service that cannot resolve `User=` or write its logs; the steps are now listed in the right order before the unit is enabled. The `Documentation=` placeholder also pointed at a different repository name, and no `LimitNOFILE` was set, leaving the service on systemd's default soft limit of 1024 fds — below what a gateway with a 1024-deep accept backlog, cached static files and pooled upstream connections can hold — where exhaustion surfaces as `EMFILE` on accept. The unit now sets `LimitNOFILE=65535`.
- Integration harness started upstream mocks and then slept a fixed 0.5 s before starting the gateway, so a slow-to-bind mock could still be absent when the first proxied test ran, producing an intermittent 502 in unrelated scenarios. The harness now waits for each mock port to accept connections (and still fails fast if a mock process died).

### Changed

- Admin key refresh reuses the config revision already parsed and validated by `POST /admin/reload` instead of re-reading the file, removing a window where the validated revision and the adopted keys could differ. The SIGHUP path still reads the file.
- The refresh log distinguishes "admin disabled in the new config" (AUDIT `admin_keys_unchanged`, informational — the listener keeps serving with its previous keys since it cannot be stopped at runtime) from a genuine failure, instead of reporting both as a refresh error.

## 2026-09-18

### Added

- P4 operations endpoints: `/healthz` (liveness, always 200 while the event loop serves), `/readyz` (readiness — 200 with a per-upstream healthy/total JSON breakdown only when every configured upstream has a healthy backend, 503 otherwise), and `/version` (build version sourced from `project(VERSION)` via a CMake-generated header). All probe endpoints are exempt from authentication and rate limiting so load balancers and Prometheus can never trip a limiter.
- P4 graceful shutdown on SIGTERM/SIGINT: the gateway first stops accepting (listen fd removed from epoll), immediately closes idle keep-alive connections and unfinished TLS handshakes, then keeps serving in-flight requests until they complete or the new `shutdown_drain_timeout` budget (default 30 s) expires — pairing with systemd `TimeoutStopSec`. Covered by an integration test that kills the server mid-proxy of a 1.5 s backend request.
- P4 Admin API on a separate listener (`admin` config section: `enabled`/`port`/`bind`/`api_keys`, loopback by default, key mandatory):
  - `GET /admin/stats` — request counters, latency totals, uptime, version
  - `GET /admin/upstreams` — per-backend health, consecutive failures and circuit-breaker state
  - `POST /admin/reload` — validates `config.json` first (invalid config ⇒ 400, nothing applied), then triggers the same worker reload path as SIGHUP (applied within ~1 s)
  - Auth uses `X-API-Key` or `Authorization: Bearer` with a constant-time comparison; failures are AUDIT logged.
- P4 systemd unit (`deploy/gateway.service`, with hardening options and `TimeoutStopSec=45`) and a deployment/upgrade guide (`docs/DEPLOYMENT.md`: install, config, health probes, rolling upgrade with drain, hot reload, monitoring checklist).

### Changed

- `UpstreamManager` is now shared process-wide (one instance across all workers and the admin API, mutex-protected as before) so health and circuit state have a single source of truth; previously every worker held its own copy and the admin API could not see real state.
- Admin-facing metrics gained `Metrics::snapshot()` (atomic counter copy) and `UpstreamManager::status_snapshot()` (full per-backend status under lock).

### Decisions

- The admin API is plain HTTP on a loopback/management listener instead of TLS-on-data-plane: TLS on the admin port can be terminated by a reverse proxy or SSH tunnel, keeping the gateway binary simple; the alternative (a second hardened TLS listener) doubles cert/key handling for little gain at this stage.
- Readiness is defined per-upstream ("every upstream has ≥1 healthy backend") rather than global: a single degraded backend must not remove a healthy gateway from the pool, but a fully unreachable upstream means client traffic would fail anyway.

## 2026-09-17

### Added

- P3 upstream connection pool (`src/server/connection_pool.cpp`):
  - Per-upstream keep-alive pool: `checkout`/`checkin` with 60 s idle timeout, max 16 idle connections per upstream, RAII fd ownership via `shared_ptr<Socket>`. Lazy eviction at checkout (FIFO ⇒ LRU, oldest idle connection probed first).
  - `forward_request` rewritten to frame backend responses precisely (Content-Length / chunked incl. trailer section / close-delimited), and to carry read-ahead `leftover` bytes with the connection so subsequent responses never garble.
  - Stale-connection race covered two ways: a 0-timeout `MSG_PEEK` probe on checkout discards connections the backend closed while idle (safe for every method), and post-send failures auto-retry only for idempotent methods so POSTs are never duplicated.
  - Integration tests prove reuse (10 proxied requests open ≤2 backend connections, was 10) and trailer-safe framing; 5 new unit tests cover pool logic.
- P3 wrk baseline load test report (`docs/BENCHMARKS.md`): `scripts/benchmark/run_benchmark.sh` produces P50/P99/QPS across three scenarios (static cache hit, reverse proxy, TLS handshake). Bonus finding: missing `TCP_NODELAY` caused a fixed ~43 ms delayed-ACK stall per request; enabling it on both client and upstream sockets improved light-load latency ~5.4× and handshake throughput ~4.3×.
- P2 secret management: `api_keys` can now live outside the main config — `"api_keys_file": "api_keys.json"` (JSON array, same schema) or the `GW_API_KEYS` environment variable; precedence env > file > inline. Inline keys still work (backward compat) but log a security hint at startup. Missing/invalid file or env JSON fails fast with a schema error.
- P2 TLS hardening (`src/server/tls_context.cpp`): `build_hardened_ssl_ctx` enforces minimum TLS 1.2, AEAD-only cipher whitelist (ECDHE+GCM/ChaCha20), no compression, session cache + tickets for resumption. TLS setup is fail-fast at startup; SIGHUP hot-reloads cert/key from the new `tls.cert_path`/`tls.key_path` config with AUDIT log on apply/reject; the old context is kept on failure. Integration tests: TLS 1.1 refused, cert hot reload verified via peer-certificate fingerprint; 6 unit tests for the context builder.
- P2 HTTP request smuggling protection: parser now rejects duplicate `Content-Length`, CL+TE mixing, non-chunked `Transfer-Encoding`, non-numeric `Content-Length`, invalid header name/value characters, control chars in the request line, non-hex chunk sizes, and oversized request lines/headers. Malformed requests get 400 + `Connection: close`; also capped the previously unbounded chunked body at `MAX_BODY_SIZE` (16 unit tests).
- P2 config schema validation: `Config::validate()` checks field ranges (ports 1-65535, thresholds > 0, `min_threads <= max_threads`), duplicate route names/matches, upstream reference existence, `static` routes have `static_root`, and api key uniqueness; `from_file` does type/range-checked reads. Startup fails fast on invalid config; SIGHUP reload of an invalid config writes an AUDIT log and keeps the old config (14 unit tests).

### Changed

- Default gateway behavior change: an invalid config file at startup now causes the server to exit instead of silently running with default config (fail-fast). SIGHUP reload of an invalid config keeps the currently active config and logs `AUDIT config_reload_rejected`.
- Header-value parsing in `forward_request` now trims OWS per RFC 7230 instead of assuming `": "` (a single space after the colon); accepts `Header:value`, `Header:  value`, etc.
- Chunked response scanning now consumes the trailer section line by line until the terminating empty line (previously only one `\r\n` was consumed after the last-chunk, which dropped the terminator into `leftover` and garbled subsequent responses when a trailer was present).

### Decisions

- The baseline benchmark is the first piece of public marketing material. The TCP_NODELAY fix is surfaced as a "found and fixed" story rather than a quiet patch — concrete, measurable, reproducible by anyone running `scripts/benchmark/run_benchmark.sh`.
- Honest result reporting: the connection pool showed no measurable QPS gain on loopback (the saved `connect()` is microseconds against a CPU-bound loopback backend). The real value is proportional to backend RTT, so correctness is proven via integration tests (backend connection count) rather than local QPS. The BENCHMARKS report says this plainly instead of inflating numbers.
- POST safety: the original pool retry could resend a POST after a stale-connection reset; the new policy only auto-retries idempotent methods once the request has been sent. This is deliberately stricter than what `should_retry_backend_request` enforces at the higher layer — the in-flight retry happens before the gateway considers the request "sent", but POSTs still aren't retried because the backend may have already acted.

## 2026-09-15

### Changed

- Full i18n conversion to English in preparation for open-sourcing on GitHub:
  - All code comments, log/error messages, test descriptions, and build-script echoes translated to English across `include/`, `src/`, `tests/`, and build files (~45 files). No code semantics touched; verified by full CI (build + 30 unit tests + 32 integration assertions, all green).
  - README split into bilingual versions: new English `README.md` (reorganized for open-source conventions: Features grouped by theme, Quick Start up front, Documentation section added) and the original Chinese preserved as `README.zh-CN.md`, with a language switcher at the top of both.
  - `docs/PROJECT_STATUS.md`, `docs/ROADMAP.md`, `docs/CHANGELOG.md` translated in place to English; this changelog is written in English from now on.
- Config: `api_keys.name` display values translated ("Test Client" / "Premium Client"); keys, ports, and structure unchanged.

### Decisions

- Wording correction: the slogan "zero-dependency" was replaced by "self-contained" — the project has library dependencies (spdlog, nlohmann/json, OpenSSL, zlib); "zero-dependency" now refers only to the absence of external services (no etcd/Postgres), matching the ROADMAP positioning.
- Binary test fixtures (`www/big.html`, `test.gz`) intentionally left untouched — they are random-byte fixtures, not text.
- Dual remote configured (environment, not repo content): `origin` = Gitee, `gh` = GitHub (`AppleAndPenAndPear/multithread_epoll`), with a `git pushall` alias pushing the current branch to both. GitHub repo was created manually (no CI token available in the dev environment).

## 2026-09-12

### Added

- CI integration:
  - Added a platform-agnostic `ci.sh` (build → CTest unit tests → integration tests in one command); the same entry point serves both local and CI environments, fully verified locally.
  - Added a GitHub Actions workflow (`.github/workflows/ci.yml`, ubuntu-latest + apt dependency install), which activates automatically once the repo is mirrored to GitHub; the Gitee side is deferred — if needed, Gitee Go can invoke the same ci.sh.
- P1 integration test framework (`tests/integration/`): launches a real server + Python mock upstream backends, covering 14 scenario categories — TLS, Keep-Alive, Trace-Id, 405+Allow, 404/403, reverse proxy, API key authentication, 429 rate limiting, failover, circuit breaker open/reject/recovery, SIGHUP reload, corrupted-config rejection, /metrics, and Chunked — with all 32 assertions passing.
- The mock backend supports switching into a "close immediately after accepting" mode via a control file (the TCP health probe still passes but forwarding fails), used to drive circuit breaker counting.

### Fixed

- `forward_request` read the backend response with a single `recv()`; when headers and body arrived across multiple TCP segments it returned 200 with an empty body (or InvalidResponse) — caught on the very first integration test run. Fixed to loop until Content-Length is satisfied or the backend closes with `Connection: close`.

### Decisions

- Integration tests depend only on the Python3 standard library plus `http.client` outside curl scenarios — no third-party dependencies, preserving the "zero-dependency, auditable" positioning.
- CI platform pending: the repo is hosted on Gitee; GitHub Actions requires solving mirroring first, so no workflow was created yet.

## 2026-09-11

### Documentation

- Added [docs/ROADMAP.md](docs/ROADMAP.md): settled the commercialization positioning (three differentiators — lightweight/auditable/embeddable, see the doc for the competitor analysis) plus the P1~P5 technical evolution plan and the 6-month commercialization validation track. Licensing decision: stay MIT for now; evaluate dual licensing once paid intent appears.
- Added [docs/CHANGELOG.md](docs/CHANGELOG.md): records changes and decision rationale by date, backfilling the project timeline since 2026-05-08.
- Cross-checked [PROJECT_STATUS.md](docs/PROJECT_STATUS.md) against the actual code and fully updated the README:
  - Corrected `/metrics` metric names to the actual output (the `epoll_http_*` series; the `epoll_server_*` names in the previous text do not exist in code).
  - Added route/host/tenant dimension metrics (`epoll_route_requests_total`, `epoll_route_latency_seconds_*`) and upstream error classification metrics.
  - Feature list gained idempotent retries, the basic circuit breaker, `X-Trace-Id`, AUDIT/CLF dual-channel logging, and `SIGHUP` runtime reload.
  - Added "Observability Logging", "Runtime Reload", and "Known Limitations" sections.
  - Config examples gained `max_retries`, `circuit_failure_threshold`, `circuit_recovery_timeout_ms`.
  - Fixed markdown formatting lost in the second half (tech stack, core design details, etc. restructured into proper tables/lists).

## 2026-09-08

### Added

- Circuit breaker: maintained per upstream/backend; enters OPEN after consecutive failures reach `circuit_failure_threshold`, allows half-open probing after `circuit_recovery_timeout_ms`, and closes on a successful probe (decision: per-worker independent breakers first; shared cross-worker state to be evaluated).
- Idempotent retries: only idempotent methods and retryable transient errors are retried; routes can configure `max_retries`.
- `X-Trace-Id` request tracing: reused and echoed back when provided by the client, otherwise generated by the gateway, spanning requests and audit logs.
- AUDIT/CLF dual-channel logging: CLF records every completed request; AUDIT records failures and security policy events once at response completion, keeping only masked API key values.
- 405 semantics finalized: a matched path with an unsupported method returns 405 with an `Allow` header; when no gateway route matches, only GET/HEAD fall back to static files.
- Runtime reload: triggered by `SIGHUP`; the signal handler only increments the generation, and workers apply the new config at safe checkpoints; JSON validity is verified before reload.
- Upstream errors are uniformly mapped to 502/503/504 and written to Metrics and audit categories.

### Changed

- Plain `Request:` entry logs downgraded to debug, avoiding duplication with CLF at production info level.
- Route matching runs exactly once per request; `ResolvedRoute` is reused across authentication, rate limiting, and dispatch.

### Tests

- Unit tests expanded to 30/30 passing, adding circuit breaker open/reject/recovery probing, idempotent retries, upstream timeouts, and related cases.

## 2026-08-30

### Added

- API key management: `X-API-Key` and `Authorization: Bearer` extraction, route allow/deny lists, host/tenant scope binding for API keys, and independent rate-limit quotas per API key.

## 2026-08-13 ~ 2026-08-14

### Added

- Reverse proxy core: `UpstreamManager` round-robin selection + active TCP health checks (automatic removal/recovery of failed nodes), `HttpClient` single-backend forwarding with timeouts, structured `BackendError` classification.

## 2026-08-11

### Added

- HTTPS support: OpenSSL non-blocking TLS handshake and encrypted read/write, running inside the epoll event loop.

## 2026-08-04

### Added

- Prometheus metrics endpoint `/metrics`; docker-compose integration with a Prometheus + Grafana monitoring stack.

## 2026-07-21 ~ 2026-07-23

### Added

- FD cache (TTL), multi-level cache hit-rate metrics, P99 latency histogram.

### Changed

- `http_parser` moved into the common library; adjusted the log queue, async thread count, and rotation policy.

## 2026-06-11

### Added

- Google Test unit test framework integration, Docker multi-stage build, `build.sh`/`start.sh` scripts, RESTful routing, and static file serving.

## 2026-05-08 ~ 2026-05-27

### Added

- Project init: SO_REUSEPORT + One Loop Per Thread multi-threaded epoll server, HTTP/1.1 state-machine parser, Keep-Alive, zero-copy sendfile, LRU file cache, spdlog async logging, MIT License.
