# Changelog

Notable changes to the project. Format follows [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/), entries sorted newest first.

> For a detailed snapshot of the current project state, see [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md). This file traces back "what was done, when, and why".

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
