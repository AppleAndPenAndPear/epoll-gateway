# ROADMAP

The execution plan for taking this project from a personal project to a commercial product. Three parts: positioning and differentiation, the technical evolution plan (P1~P5), and the commercialization validation track.

> Companion documents: [PROJECT_STATUS.md](PROJECT_STATUS.md) records the current capability snapshot, [../CHANGELOG.md](../CHANGELOG.md) records completed changes, and this file plans what comes next.

---

## 1. Positioning and Differentiation

### Competitive Landscape

| Competitor | Stack | Deployment | Where its "heaviness" lies |
|---|---|---|---|
| Kong | Lua/OpenResty + Postgres | Requires a database | Postgres is a hard dependency; large memory footprint |
| APISIX | OpenResty + etcd | Requires a config center | etcd cluster + LuaJIT runtime; does not run at the edge |
| Envoy | C++ | Single binary but extremely heavy | Million-line codebase; unauditable, hard to embed |
| Higress | Envoy + Go control plane | K8s ecosystem | Deeply coupled to cloud native; unfriendly to bare metal/edge |
| Nginx | C | Very light | Not an API gateway — auth/rate limiting/tenancy/metrics all require custom extension |

**Market gap**: no competitor combines "Nginx-grade resource footprint with out-of-the-box API gateway semantics". APISIX grows toward the ecosystem, Nginx spreads as a pure proxy — the middle is where this project sits.

### Three Verifiable Differentiators (every action maps back to them)

1. **Extremely lightweight** — targets: single binary < 10MB, idle memory < 30MB, zero external dependencies (no etcd/Postgres/runtime), runs on ARM out of the box.
2. **Fully auditable** — targets: core code kept within tens of thousands of lines, readable end-to-end by one senior engineer in a week. Aimed at Xinchuang (domestic IT), classified intranets, and security-sensitive customers.
3. **Embeddable** — targets: runs as a standalone process and embeds into third-party products as a static library (gateway built into device firmware).

### One-line Positioning

> For teams with no etcd, no K8s, and no ops department: one API gateway binary that runs lean, reads in a week, and embeds cleanly.

### What We Deliberately Won't Do

- No plugin marketplace, no sprawling dashboard, no all-protocol suite (competing on ecosystem is a losing game)
- No show-off features like HTTP/3/QUIC (target customers don't care)
- No contest over extreme benchmark numbers (we sell "good enough + light", not peak performance)

### Licensing Strategy (current decision: stay MIT for now)

- At this stage the bottleneck is users, not revenue; MIT has the least adoption friction
- Revisit dual licensing (BSL/AGPL) or "MIT core + closed-source enterprise features" only once real paid intent appears
- Candidate enterprise features (closed-source, paid, in the future): cluster control plane, multi-tenant quotas and billing, advanced plugins, certification and compliance support

---

## 2. Technical Evolution Plan (P1~P5)

| Phase | Theme | Exit Criteria (DoD) |
|---|---|---|
| **P1** | Regression safety net | One-command integration tests, CI fully green |
| **P2** | Config + security hardening | Config schema validation live; pass HTTP spec/TLS self-check checklist |
| **P3** | Performance baseline | Load test report across three scenarios + connection pooling landed |
| **P4** | Operations productization | Admin API + graceful shutdown + deployment docs |
| **P5** | Commercial features | Shape branch decided by customer feedback |

### P1: Integration Tests and the Regression Safety Net

- Integration test harness: launch a real server + local mock upstreams, assert with curl or a C++ HTTP client
- Scenarios covered: TLS handshake, keep-alive multi-request reuse, chunked, 405+Allow, 429 rate limiting, upstream failover, circuit breaker open/recovery, SIGHUP reload before/after behavior, reload rejection on corrupted config
- Wire into CI (GitHub Actions): unit tests + integration tests + ASAN on every commit

### P2: Configuration and Security Hardening

- Config validation: field types/ranges (ports 1-65535, thresholds > 0), duplicate route name/path conflicts, upstream reference existence; a failed reload writes AUDIT and keeps the old config
- Protocol security: reject duplicate `Content-Length`, CL/TE conflicts, and invalid header characters (request smuggling prevention); caps on request line/header lengths
- TLS: minimum TLS 1.2, cipher suite whitelist, session ticket reuse, hot reload of certificate/private key
- Secret management: api_keys loadable from environment variables or a separate file, not mixed into the main config

### P3: Performance Baseline and Key Optimizations

- wrk baseline across three scenarios: static small files (cache hits), reverse proxy (local backend), TLS handshake + request; output a P50/P99/QPS report (usable directly as marketing material)
- Upstream connection pooling: per-request TCP connection → pooled + keep-alive; expected to be the single highest-yield optimization
- Add a per-route latency histogram
- Confirm hotspots with flame graphs before deciding on a route Trie/index (avoid premature optimization)

### P4: Operations Productization

- `/healthz` (liveness) and `/readyz` (readiness, checks upstream availability)
- Admin API (separate listen address, authentication enforced): hot config updates, upstream/circuit breaker status, stats summary
- Graceful shutdown: SIGTERM stops accepting first, waits for in-flight requests to finish, then exits (pairs with systemd `TimeoutStopSec`)
- systemd unit, deployment/upgrade docs, `/version` endpoint

### P5: Commercial Features (shape driven by customer signals)

| Shape | Trigger Signal | What to Build First |
|---|---|---|
| Edge/OEM embedding | Device vendor outreach, concentrated ARM user feedback | C API/static library form, cross-compilation, offline activation |
| Xinchuang compliance | Integrator/domestic-IT customer outreach | Kylin/UOS, Kunpeng/Phytium/Loongson adaptation and certification |
| Self-hosting for small teams | Community growth, self-hosting issues clustering | Mini control plane (SQLite instead of etcd), multi-tenant quotas |

---

## 3. Commercialization Validation Track (6 months)

### Core Principles

- **No all-in**: keep existing income, set a validation window (6 months) and exit criteria (10 real users, 1 paid intent; otherwise downgrade to a portfolio project)
- **The first goal is not making money — it is finding the first real user who is not yourself**
- Customers before form factor, not the other way around

### Phased Execution

| Timeline | Goal | Measure |
|---|---|---|
| Month 1~2 | P1~P2 done (integration tests + config/security hardening) | CI fully green |
| Month 2~3 | P3 done, load test report and competitor comparison table produced | Publicly shareable numbers |
| Month 3 | English README + technical articles published (Juejin/Zhihu/V2EX/HN) | 100+ stars or steady external issues |
| Month 4~6 | Collect feedback while building P4 | 10+ real users, ≥1 paid/customization intent |

### How to Gather Market Signals

- Proactively interview nearby companies doing backend/ops/embedded work (5 conversations give real signal)
- Issue templates ask users about: deployment environment, device specs, most-wanted features
- Watch user composition: many ARM/edge players → edge direction; many enterprise intranet ops → self-hosting direction

### Review and Exit

Review after 6 months: if the direction is clear, commit fully; otherwise gracefully convert it into a high-quality portfolio project (equally valuable for job hunting/contract work — not a failure).

---

## 4. Current Status and Next Steps

- [x] P1: integration test framework (real server + mock upstreams), 32/32 passing (2026-09-12)
  - Along the way, found and fixed a defect where `forward_request` reading the response with a single `recv` returned 200 with an empty body
- [x] P1: CI wired up (2026-09-12)
  - Added a platform-agnostic `ci.sh` (build → CTest unit tests → integration tests in one command), the single entry point shared by local and CI environments
  - GitHub Actions workflow in place (`.github/workflows/ci.yml`), activates automatically once the repo is mirrored to GitHub
  - The repo is currently hosted on Gitee: if Gitee-side CI is needed later, Gitee Go can invoke the same ci.sh
- [x] P2: config schema validation (2026-09-16)
  - Added `Config::validate()` (field ranges, duplicate route names/matches, upstream reference existence, api key checks) and type/range-checked `from_file` reads
  - Startup fails fast on invalid config; SIGHUP reload of an invalid config writes an AUDIT log and keeps the old config
  - 14 new unit tests in `test_config_validation.cpp`
- [x] P2: HTTP request smuggling protection (2026-09-17)
  - Parser now rejects duplicate `Content-Length`, CL+TE mixing, non-chunked `Transfer-Encoding`, non-numeric `Content-Length`, invalid header name/value characters, control chars in the request line, non-hex chunk sizes, and oversized request lines/headers
  - Malformed requests get 400 + `Connection: close`; also capped the previously unbounded chunked body at `MAX_BODY_SIZE`
- [x] P2: TLS hardening (2026-09-17)
  - `build_hardened_ssl_ctx` (`tls_context.cpp`): minimum TLS 1.2, AEAD-only cipher whitelist (ECDHE+GCM/ChaCha20), no compression, session cache + tickets for resumption
  - TLS setup is fail-fast at startup; SIGHUP hot-reloads cert/key from the new `tls.cert_path`/`tls.key_path` config, AUDIT logs applied/rejected, old context kept on failure
  - Integration tests: TLS 1.1 refused, cert hot reload verified via peer-certificate fingerprint; 4 unit tests for the context builder
- [x] P2: Secret management (2026-09-17)
  - `api_keys` can now live outside the main config: `"api_keys_file": "api_keys.json"` (JSON array, same schema) or the `GW_API_KEYS` environment variable; precedence env > file > inline
  - Inline keys in config.json still work (backward compat) but log a security hint at startup; a missing/invalid keys file or env JSON fails fast with a schema error
  - Integration tests now load keys via `api_keys_file` end-to-end; 6 unit tests cover the source precedence
- [x] P3: wrk baseline load test report (2026-09-17)
  - `scripts/benchmark/run_benchmark.sh` + full report in `docs/BENCHMARKS.md` (P50/P99/QPS across static, proxy, handshake scenarios)
  - Bonus fix found by the baseline: missing `TCP_NODELAY` caused a fixed ~43 ms delayed-ACK stall per request; enabling it improved light-load latency ~5.4x and handshake throughput ~4.3x
- [x] P3: upstream connection pool (2026-09-17)
  - Per-upstream keep-alive pool (`connection_pool.cpp`): checkout/checkin with idle timeout (60s) and max-idle cap (16), leftovers carried with the connection, stale connections invalidated and retried transparently
  - `forward_request` now frames responses precisely (Content-Length / chunked / close-delimited) and returns healthy connections to the pool
  - Integration test counts backend connections: 10 proxied requests open ≤2 backend connections (was 10); 5 new unit tests
- [x] P4: /healthz + graceful shutdown (2026-09-18)
  - Built-in ops endpoints: `/healthz` (liveness), `/readyz` (readiness: 200 only when every upstream has a healthy backend, JSON breakdown otherwise 503), `/version` (build version from CMake via a generated header); all probe endpoints are exempt from auth and rate limiting
  - Graceful shutdown: SIGTERM stops accepting first (listen fd removed from epoll), closes idle keep-alive connections immediately, keeps serving in-flight requests until done or `shutdown_drain_timeout` (default 30s, pairs with systemd `TimeoutStopSec`), then exits
  - Admin API: separate listener (`admin.enabled/port/bind`) with mandatory key auth (`X-API-Key`/Bearer, constant-time compare): `GET /admin/stats` (counters/latency/uptime/version), `GET /admin/upstreams` (per-backend health + circuit state), `POST /admin/reload` (validates first, rejects invalid config with 400, otherwise triggers the same worker reload path as SIGHUP)
  - `UpstreamManager` is now process-wide (shared by workers and admin) so health/circuit state has a single source of truth
  - systemd unit (`deploy/gateway.service`) + deployment/upgrade guide (`docs/DEPLOYMENT.md`)
  - Tests: 100 unit / 72 integration assertions passing
- [x] Companion client HTTPS support (2026-09-22)
  - `src/client/` now runs a non-blocking TLS handshake inside its own epoll loop, verifies the server certificate (`--ca`, default `certs/server.crt`) including the hostname, and issues a real `GET <path> HTTP/1.1`; `--insecure` skips verification, `--no-tls` falls back to plaintext, and exit code 0 means a complete response was received (so `./build/client` is a usable smoke test)
  - TLS hardening (minimum 1.2 + AEAD whitelist) extracted to `include/common/tls_utils.{h,cpp}` and shared by the server and client context builders
  - Tests: 9 unit tests and 3 integration assertions, including a hostname-mismatch rejection that proves verification is not a silent no-op (needs `SSL_VERIFY_PEER` before `SSL_set1_host`)
  - Bugs found while wiring it up: the client's event dispatch had no `TLS_HANDSHAKING` case (permanent stall under `EPOLLET | EPOLLONESHOT`), `Content-Length` parsing rejected the space after the colon, completion relied on EOF and so misfired against the 60 s keep-alive timeout, `SIGPIPE` was not ignored, TLS writes must not be half-closed with `shutdown(SHUT_WR)`, and OpenSSL 3.0's unexpected-EOF needed the lenient read path
- [x] Upstream TLS (2026-09-24)
  - Backends declared as `https://host:port` in `servers` get a client-side handshake before proxying (non-blocking, reusing the same hardening policy and `Socket`/`Poller` timeouts); per-upstream `tls_ca_file` (empty = system default trust store), `tls_server_name` (SNI + `SSL_set1_host` hostname binding) and `tls_skip_verify` (explicit escape hatch, off by default); verification defaults to on
  - The connection pool key now embeds the verification policy (`scheme` = `tls/<server_name>`, `tls/insecure` or empty), so a session verified for one hostname can never be handed to a different policy on the same host:port — the hostname check would otherwise only run during the handshake that a reused connection skips
  - Every SSL I/O call now starts with `ERR_clear_error()`: a failed upstream handshake leaves stale errors in OpenSSL's per-thread queue, and the next `SSL_get_error` on an unrelated socket in the same thread misclassified `WANT_READ` as fatal and dropped the client connection
  - Tests: 101 unit / 77 integration passing, including 5 new upstream-TLS integration assertions (verified 200, pooled TLS reuse, hostname-mismatch 502, skip-verify escape hatch) and a pool-identity unit test
  - Not yet: mTLS (client certificates to backends), TLS-layer health probes (health checks stay TCP-level)
- [x] First-run experience productization (2026-09-26)
  - 5-minute quickstart at the top of both READMEs: built-in echo routes answer with no backend, admin auth demo (401 → 200 with key), ops endpoints and /metrics; default `config.json` no longer carries phantom upstreams that made `/readyz` answer 503 out of the box
  - Runnable `examples/`: load balancing with visible round-robin and health-check ejection, and a verified `https://` upstream (upstream TLS) — both copy-paste runnable against the integration mock backend
  - Dev certificates generated locally (`scripts/gen_dev_certs.sh`, SAN DNS:localhost only so IP-literal mismatches still fail), `certs/` gitignored and the committed private key removed; Docker mounts the keypair read-only instead of baking it into the image
  - Server accepts an optional config path argument; config validation failures print a file-name + error-count summary
  - Process lesson recorded: the previous commit shipped non-compiling code because CI was not re-run right before the commit (an outside edit partially reverted `http_client.cpp` between the last green run and the commit); CI now runs immediately before every commit
- [x] Outreach: English README (2026-09-16) — `README.md` is the English edition with a language switcher, kept in sync with `README.zh-CN.md`
- [ ] Outreach: first architecture article — draft lives in [docs/articles/](articles/); publishing to Juejin/Zhihu/V2EX/HN still pending
- [ ] Signals: interview 5 potential users
