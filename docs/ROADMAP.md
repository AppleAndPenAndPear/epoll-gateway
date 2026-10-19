# ROADMAP

The execution plan for taking this project from a personal project to a commercial product. Three parts: positioning and differentiation, the technical evolution plan (P1~P6), and the commercialization validation track.

> Companion documents: [PROJECT_STATUS.md](PROJECT_STATUS.md) records the current capability snapshot, [CHANGELOG.md](CHANGELOG.md) records completed changes, and this file plans what comes next. **The active plan is §4's "Post-2026-09-29 Engineering Milestones"; §2 (P1~P6) is the original historical framing and is kept only for context.**

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
| KrakenD | Go | Single binary, stateless | The closest competitor for composite aggregation; a transformation engine, not a general auth/TLS-hardening proxy; GC runtime |
| Traefik / Caddy | Go | Single binary | Comfortable defaults, but no per-key auth / quota / composite aggregation without plugins; GC runtime |
| LiteLLM (proxy) | Python | Single process + runtime | The de-facto self-hosted AI/model proxy; Python runtime and provider-focused, not a general gateway |

**Market gap**: originally framed as "Nginx-grade resource footprint with out-of-the-box API gateway semantics". The 2026-09-29 same-conditions comparison against Nginx 1.24 ([BENCHMARKS.md](BENCHMARKS.md)) corrected this: memory is a wash (steady-state RSS 26.0 MB vs 26.4 MB) and throughput is 2.5-2.7x lower — so the gap we actually fill is **deployment and operations surface**: no competitor ships out-of-the-box gateway semantics (auth/tenancy/rate limiting/metrics/hot reload) as a single binary with one JSON config and zero runtime dependencies. APISIX grows toward the ecosystem, Nginx stays a pure proxy — the middle is still where this project sits, but the middle is defined by operational simplicity, not resource footprint.

**Named audiences (2026-10-19)** — the gap above resolves into three concrete buyer groups; interviews and the architecture article must target these, not generic "backend/ops teams":
1. **On-prem appliance / private-delivery / OEM teams** — cannot require a database, config center or K8s at the customer site; want an auditable C++ entry point they can read end-to-end and (later) embed (Differentiator #3).
2. **Nginx users who resorted to Lua/OpenResty** — they run Lua for per-key auth, rate limiting and aggregation and would rather not maintain Lua code; this is the simplest one-line pitch we have.
3. **Mobile / mini-program teams needing a BFF layer** — composite fan-out with per-dependency timeouts, a hard aggregate deadline and `partial_failure` semantics is a complete answer to "one page calls 3~5 backends".

### Head-to-Head (where we win, where we do not compete)

| Rival | Its edge | Our stance |
|---|---|---|
| KrakenD | Deepest aggregation (chaining, transformations) | Composite fan-out is table stakes, not our wedge; we win on C++ embeddability plus a general proxy/auth story beside it |
| Kong / APISIX | Plugin ecosystem, control plane, community | Do not fight the ecosystem; win "teams that don't want a control plane or a database" |
| Traefik / Caddy | Ease of use, auto-TLS | Win buyers who need explicit, readable hardening (TLS policy, audit log, request-smuggling defenses) |
| OpenResty / Nginx+Lua | Infinite flexibility | Productize the common Lua uses (key auth + rate limit + aggregation) with zero Lua to maintain |
| LiteLLM / Higress (AI) | AI ecosystem, vendor backing | Enter only via the self-hosted-inference niche signal (see §4's AI gate); never as a generic model router |

### Three Verifiable Differentiators (every action maps back to them)

1. **Single-binary deployability** — targets: single binary < 10MB, zero external dependencies (no etcd/Postgres/runtime), one JSON config covering the full production surface, runs on ARM out of the box. *Data note (2026-09-29): idle memory was measured at parity with Nginx (~26 MB each), so "lower memory" is NOT a differentiator and must not be used in marketing copy; binary size and dependency-free deployment remain.*
2. **Fully auditable** — targets: core code kept within tens of thousands of lines, readable end-to-end by one senior engineer in a week. Aimed at Xinchuang (domestic IT), classified intranets, and security-sensitive customers.
3. **Embeddable** *(aspiration — not yet implemented; nothing to sell on today)* — targets: runs as a standalone process and, in a future release, embeds into third-party products as a static library (gateway built into device firmware). **Current reality: the build produces only `server`/`client` executables plus an internal object library — there is no stable C API, no installable library target, and no ABI/versioning promise, so the project cannot yet be embedded.** This differentiator becomes real only when a device/OEM customer requires it (see P5's Edge/OEM row); until then it must not appear in marketing copy as a present capability.

### One-line Positioning

> For teams with no etcd, no K8s, and no ops department: one API gateway binary — one JSON config, nothing else to install, and a codebase you can read in a week. *(Embeddability is a roadmap target, not a shipping feature — see Differentiator #3.)*

### What We Deliberately Won't Do

- No plugin marketplace, no sprawling dashboard, no all-protocol suite (competing on ecosystem is a losing game)
- No show-off features like HTTP/3/QUIC (target customers don't care)
- No contest over extreme benchmark numbers (we sell "good enough + deployable", not peak performance — the Nginx comparison quantified the gap at 2.5-2.7x and we say so publicly)

### Licensing Strategy (current decision: stay MIT for now)

- At this stage the bottleneck is users, not revenue; MIT has the least adoption friction
- Revisit dual licensing (BSL/AGPL) or "MIT core + closed-source enterprise features" only once real paid intent appears
- Candidate enterprise features (closed-source, paid, in the future): cluster control plane, multi-tenant quotas and billing, advanced plugins, certification and compliance support

---

## 2. Technical Evolution Plan (P1~P6)

> **Historical framing.** P1~P4 and P6's first item are complete; the project outgrew the single linear P1~P6 track when async upstream execution and composite routes landed. This section is retained to explain how today's design was reached. Do **not** treat it as the active to-do list — the active sequence is §4 "Post-2026-09-29 Engineering Milestones", and current state is authoritative in [PROJECT_STATUS.md](PROJECT_STATUS.md).

| Phase | Theme | Exit Criteria (DoD) | State |
|---|---|---|---|
| **P1** | Regression safety net | One-command integration tests, CI fully green | Done (2026-09-12) |
| **P2** | Config + security hardening | Config schema validation live; pass HTTP spec/TLS self-check checklist | Done (2026-09-17) |
| **P3** | Performance baseline | Load test report across three scenarios + connection pooling landed | Done (2026-09-17/29) |
| **P4** | Operations productization | Admin API + graceful shutdown + deployment docs | Done (2026-09-18) |
| **P5** | Commercial features | Shape branch decided by customer feedback | Superseded by §4 |
| **P6** | Performance honesty follow-up | Log switch landed; throughput gap vs Nginx profiled and top costs addressed | Log switch done; profiling pending |

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
- Add a per-route latency histogram — done 2026-10-13
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

### P6: Performance Honesty Follow-up (data-driven, from the 2026-09-29 Nginx comparison)

The comparison in [BENCHMARKS.md](BENCHMARKS.md) established: memory parity, 2.5-2.7x throughput gap, and the gateway being CPU-bound in its own request path (run-to-run stable at 600-650 RPS while Nginx numbers floated with machine noise). Before any optimization, make the costs visible and controllable:

- ~~**Per-request log switch (do first)**~~ ✅ **Done (2026-10-09):** `logging.access_log_enabled` disables CLF access lines independently; AUDIT for auth/reload/security events stays on regardless. Shipped with integration tests (access-log switch + redaction) and documented in [DEPLOYMENT.md](DEPLOYMENT.md).
- **Profile before touching the hot path**: isolate the gap's sources — log I/O volume (now measurable against `access_log off`), buffer copies in the proxy path, TLS record sizing, per-request allocations — with flame graphs / syscall counts on the existing bench setup. Optimize only what profiling proves.
- **Re-run the comparison after each change**: same methodology (interleaved rounds, medians, RSS sampling) so regressions and wins are directly comparable to the recorded baseline.
- **Complete the competitor matrix when the network allows**: Traefik and KrakenD release binaries (download was blocked from this network); they add the "batteries-included gateway" reference points the Nginx comparison lacks.
- **DoD**: log switch shipped with tests; profiling report appended to BENCHMARKS.md naming the top cost; the gap re-measured after addressing it (or a written justification for why the gap is accepted).

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

> **Actual progress (2026-10-17).** The Month 1~3 *engineering* goals are met (CI green, benchmark + Nginx comparison table, English README). But the Month-3 outreach item (publish the articles) never started and the Month 4~6 measure (10+ real users, ≥1 paid intent) has not happened — the six-month validation window is now past its midpoint with **zero demand-side signal**. That is the current binding constraint, not the feature surface.

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
- [x] Release channel (2026-09-28, v0.2.0)
  - `scripts/package_release.sh`: self-contained tarball per arch (x86_64 / aarch64) — binary with `$ORIGIN` rpath, libspdlog bundled, openssl/zlib system-wide, config/www/examples/cert-script/docs inside, no keys or logs; `ldd` gate + end-to-end smoke (healthz/echo/admin) before shipping
  - `.github/workflows/release.yml`: tag `v*` → per-arch builds (ubuntu-22.04 glibc baseline for x86_64, native arm runner for aarch64) → GitHub Release with sha256 → multi-arch image on ghcr.io; Gitee receives the tag
  - README quickstarts lead with the prebuilt path; source build remains Option B — the evaluator funnel no longer starts at "install a toolchain"
- [x] Release channel hardening (2026-09-29, v0.2.1)
  - v0.2.0's tarball did not run on Ubuntu 24.04 (it linked `libfmt.so.8`; 24.04 has only `.9`) because packaging bundled `libspdlog` by name and missed its transitive dependency. Bundling is now whitelist + transitive closure, and an incomplete closure fails the build instead of shipping
  - `scripts/verify_artifact.sh` extracts the finished tarball and runs it in clean `ubuntu:22.04` / `ubuntu:24.04` / `debian:12` containers (loader resolution + real startup, listener accepting connections); called from both `ci.yml` and `release.yml`, so a release cannot publish an artifact that does not run on a supported distribution
  - `-static-libstdc++ -static-libgcc` removes the `GLIBCXX_3.4.x` floor; the supported baseline is documented as glibc ≥ 2.35 + system OpenSSL 3 (Ubuntu 22.04+ / Debian 12+) in both READMEs
  - Both architectures build on the 22.04 runner (`ubuntu-22.04` / `ubuntu-22.04-arm`) so that one documented glibc floor describes the whole release; the aarch64 job initially built on `ubuntu-24.04-arm` and its binaries referenced glibc 2.39 symbols that the 22.04 verification container does not provide
  - The check also established what "system OpenSSL 3" concretely means: Ubuntu base images ship `libssl3` (apt links it), Debian base images ship GnuTLS and no OpenSSL, so on a minimal Debian the tarball needs `libssl3` installed. The verifier provisions exactly that declared prerequisite and then keeps asserting nothing else is unresolved — a bundle gap would still fail, since no distribution package supplies `libspdlog`/`libfmt` on a minimal image
  - Decision recorded: OpenSSL stays system-wide even though it, not glibc, sets the real floor — a security product must let crypto take distribution CVE patches rather than freeze it in the tarball. Going below glibc 2.35 would mean shipping OpenSSL 3 ourselves (manylinux2014 container + source build), deferred until a concrete Xinchuang/legacy-system requirement appears
  - Process lesson: the `ldd` gate and the CI smoke test both ran on the build host, which has every library the build needs — a self-verifying check that structurally cannot detect the bug it exists for. Packaging verification must execute in an environment where the build environment is absent
- [x] Outreach: English README (2026-09-16) — `README.md` is the English edition with a language switcher, kept in sync with `README.zh-CN.md`
- [x] P3: competitor comparison baseline — Nginx (2026-09-29)
  - Same-conditions comparison against Nginx 1.24 (same certificate, same route, same 4 mock backends, 2 vCPU box, interleaved wrk rounds with medians): memory parity (steady-state RSS 26.0 MB vs 26.4 MB) and a 2.5-2.7x throughput gap (608 vs 1549 RPS at c8) — methodology, numbers and measurement traps recorded in [BENCHMARKS.md](BENCHMARKS.md)
  - Positioning corrected across docs/articles: the differentiator is single-binary deployment + one-JSON-config operations surface, NOT memory or performance (data note added to Differentiator #1)
  - The benchmark uncovered a real bug: clients disconnecting mid-TLS-handshake spun dead fds forever (`SSL_accept` returning "not finished" on a dead fd re-armed and retried each event loop turn — 225k error lines in 25s). Fix implemented: `sslAccept()` returns a four-state `SSLHandshakeStatus`, the worker tears down on FAILED/HUP before attempting a handshake, `ERR_clear_error()` per operation — pending CI verification and commit
  - Per-request access/AUDIT logging has no config switch (a fairness gap in comparisons); logged as the first item of P6
  - Pending: Traefik/KrakenD reference points (release binary downloads blocked from this network)
- [ ] Outreach: first architecture article — draft lives in [docs/articles/](articles/); publishing to Juejin/Zhihu/V2EX/HN still pending
- [ ] Signals: interview 5 potential users

## Post-2026-09-29 Engineering Milestones (current work)

These supersede the earlier P1~P6 framing above and are the active sequence; current state is the source of truth in [PROJECT_STATUS.md](PROJECT_STATUS.md).

- [x] Bounded async upstream boundary (2026-10-08)
  - `BoundedAsyncUpstreamExecutor` (`upstream_async.{h,cpp}`): worker pool + bounded queue + per-upstream-group concurrency isolation; overloaded or shutting-down submissions return a deterministic 503 instead of blocking an epoll worker (removed 2026-10-18 — composite fan-out now uses the event-loop `UpstreamExchange` state machine)
  - Route identity resolved once via `resolve_upstream_group_id()` (route bound to `upstream_target.name`, falling back to the route name) so the executor key always matches the real backend pool, not a display label
- [x] Composite route MVP (2026-10-08)
  - `composite` target type with named dependencies (required vs optional) validated at config load; dependencies fan out through the bounded executor and aggregate into an explicit `partial_failure` (503) vs `ok` (200) response (the bounded executor was later replaced by the non-blocking state machine — see the 2026-10-18 entry under the active sequence)
  - End-to-end profile-page scenario (user + orders + recommendations) passes
- [x] Composition diagnostics and logging policy (2026-10-09)
  - W3C `traceparent` generated/propagated across the upstream hop and echoed back, keeping `X-Trace-Id` compatibility; per-dependency `traceparent`, `duration_ms`, and status in the composite payload
  - `logging` config section: `access_log_enabled` (CLF toggle independent of AUDIT), `redact_sensitive_headers` / `redact_sensitive_query_params` with configurable sensitive-header/query lists; API keys always fully masked; request/response bodies never logged
- [x] Read/write boundary correctness (2026-10-09)
  - Half-packet assembly: the parser reports every consumed byte and the caller erases it each pass, so fragmented requests resume correctly instead of re-parsing old bytes
  - Coalesced requests: `handle_read` parses already-buffered bytes before `recv`, and one TCP write carrying two requests yields two ordered responses (pipelining regression added)
  - TLS partial writes: `SSL_write` WANT_READ/WANT_WRITE re-arms the correct epoll direction via `last_ssl_want()`; TLS static-file sends retain their pending chunk and continue across backpressure (8 MiB delayed-reader test added)
- [x] Proxied chunked re-frame (2026-10-09, restored)
  - Backend chunked responses are de-chunked and re-framed by `Content-Length` so the client never receives a `Transfer-Encoding: chunked` + `Content-Length` conflict; trailer fields are dropped and the pooled connection stays reusable
- [x] Commercial-readiness hardening (2026-10-10)
  - Four high-severity fixes: no double-gzip of already-encoded upstream responses, bounded composite fan-out (no unbounded `std::async`), full RFC 7230 hop-by-hop stripping both directions, and CR/LF injection guard on upstream response headers
  - Medium/Low fixes: factory default keys removed from `config.json`, constant-time API-key comparison, unified path-traversal guard, real version in the `Server` header, `std::tolower` UB, rate-limiter default-config race
  - Parser robustness: bare-LF rejection, whitespace-before-colon rejection, full chunked-trailer consumption
  - Forwarded headers: `X-Forwarded-Proto`/`X-Forwarded-Host` added, `X-Real-IP` overwritten (not appended)
  - Connection-reuse integration test de-flaked (health-probe connections no longer counted)
- [x] Resource budgets and overload defence (2026-10-10)
  - Process-wide `max_connections` enforced by a shared `ConnectionBudget`; over-limit connections are accepted and shed immediately so a flood cannot exhaust fds/memory
  - Composite fan-out limits (`upstream_max_inflight`/`upstream_max_queue`) made configurable instead of hard-coded (both keys removed 2026-10-18 with the bounded executor)
  - `epoll_connections_active` / `epoll_connections_rejected_total` metrics; full per-resource budget table in [DEPLOYMENT.md](DEPLOYMENT.md) §11
- [x] Supply-chain gate (2026-10-13)
  - `scripts/supply_chain.sh` wired into CI (`./ci.sh supply-chain`): `cppcheck` SAST (blocking only on `error` severity), an offline CycloneDX 1.5 SBOM of `build/server`'s linked libraries, and an `osv-scanner` SCA pass over that SBOM; a missing tool reports SKIPPED with its install command, never a silent pass
  - Caught a real defect on its first run: an uninitialised `UpstreamServer::port` (fixed); see [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md)
- [x] Per-route/host/tenant latency histograms + failure-rate metrics (2026-10-13)
  - The route dimension now exports a full `epoll_route_request_duration_seconds` histogram (`_bucket`/`_sum`/`_count`) with the same bucket boundaries as the global histogram, so per-dimension P95/P99 is available
  - Request counters report 2xx/3xx/4xx/5xx per dimension (failure rate derivable in PromQL) and cause-specific counters (`epoll_route_upstream_errors_total`, `epoll_route_auth_failures_total`, `epoll_route_rate_limit_hits_total`)
- [x] Fully event-driven single-upstream forwarding (2026-10-14)
  - The single-upstream round trip is now a non-blocking state machine (`UpstreamExchange`) registered in the worker's own epoll, so no executor thread and no per-request thread are involved; concurrency is bounded by connection fds, not thread count
  - The `eventfd` wake-up path and the single-upstream use of `BoundedAsyncUpstreamExecutor` are removed; the executor (and `upstream_max_inflight`/`upstream_max_queue`) then served composite fan-out only (all three removed 2026-10-18 — see below)
  - New `test_concurrent_proxy_isolation` integration case guards the correctness of the loop-driven forwarding (N simultaneous connections each get their own correct response, no cross-talk or loss)
  - The throughput before/after could not be measured on this shared 2-vCPU box (a single-threaded mock saturates near the gateway's own rate); the change is justified on structure, not a measured win
- [x] Fully event-driven composite fan-out (2026-10-18)
  - Composite routes now use the same non-blocking `UpstreamExchange` state machine as single-upstream routes (`ExchangeState` generalized to N child exchanges, one shared driver), so a fan-out no longer blocks the worker's event loop; the aggregate is framed when the last dependency finishes or the overall deadline fires
  - `composite_target.total_timeout_ms` is enforced as a hard wall-clock deadline independent of each child's idle deadline (which is refreshed on I/O progress), so a slowly-streaming backend cannot outlive the budget
  - Composite dependencies now report `record_success`/`record_failure`, so the circuit breaker accounts for composite traffic like single-upstream traffic
  - `BoundedAsyncUpstreamExecutor` and the `upstream_max_inflight`/`upstream_max_queue` keys removed (no remaining callers); the `rejected`/`executor_overloaded` dependency statuses removed from the outward JSON contract
- Current verification: 137/137 unit, 155 integration assertions (2026-10-19)

### Direction decision (2026-10-17)

Reviewed after the engineering milestones above cleared out. Decision: **keep the API gateway as the stable core and add AI-gateway capability as an optional upper layer** (already the recommendation in [COMMERCIAL_READINESS.zh-CN.md](COMMERCIAL_READINESS.zh-CN.md)); do **not** pivot the whole product to an AI gateway. The binding constraint is now the demand side, not the feature surface — every remaining unchecked box in §4 is non-engineering (publish, interview). Order: **validate first (low engineering cost, but it costs calendar time), then invest in the AI layer only if a real signal appears.**

**P0/P1/P2 are not a pre-publish gate.** By the register's own wording P2 is "selected by market signal", and much of P0/P1's remaining work is customer-gated (JWT/OIDC, mTLS, HTTP/2, HA/cluster, distributed quota — each waiting on "访谈确认需求后"). Validation is the *input* that tells us which of those gaps matter, not a reward earned after feature completeness. So the product is already credible enough to publish an architecture article and start interviews, and validation runs **in parallel** with a small, customer-independent "credibility-gap" worklist below, instead of waiting for a feature-complete P0/P1/P2.

### Next in the active sequence

- **[Now · validation first, low engineering cost] Unblock the demand side before building anything new.**
  - Publish the first architecture article (draft already in [docs/articles/](articles/)) to Juejin/Zhihu/V2EX/HN — the §3 Month-3 item that never started. **The lead demo is the composite/BFF fan-out** (a working, differentiating capability — see §1's named audience 3), not "another epoll server"; headline copy must not claim performance superiority (see the copy-sweep item below). Interviews ask about **switching costs and trust barriers** against the incumbent setup, not feature wishlists — gateways are infrastructure, an article earns interest, not adoption.
  - Interview 5 potential users (backend/ops/embedded teams, plus AI-platform teams) — the §3 Month 4~6 signal item that never happened.
  - **In parallel (not a gate): close only the customer-independent "credibility-gap" items** — the ones a reader would immediately challenge, all low-engineering and none requiring a customer decision:
    - [x] Aggregate **total deadline + cancellation propagation** (the P0 open item; correctness-facing) — done 2026-10-17 via `composite_target.total_timeout_ms`, then **fully non-blocking on 2026-10-18**: composite fan-out now runs on the same event-loop `UpstreamExchange` state machine as single-upstream routes, so the worker is never blocked while a fan-out waits, and the total deadline is a hard wall-clock bound independent of each child's progress-refreshed idle deadline.
    - **Upstream-side latency/failure plus rate-limit/circuit/retry/pool metrics** (the P1 observability residue; the first thing a high-traffic reader asks).
    - **Copy sweep for performance claims** (docs-only, do first): remove "high-performance" wording from headline copy (README slogan, article drafts) until an honest re-baseline exists — the claim to make is "predictable latency, low footprint, zero dependencies"; the current README headline contradicts [BENCHMARKS.md](BENCHMARKS.md) Finding D and would be the first thing a serious reader challenges.
    - **Capacity/soak numbers**, including an honest Nginx re-baseline (the P1 proof item; pairs with the "Later pipeline" entry below).
    - **Parser fuzz harness + a one-page vulnerability-response policy** — for infrastructure buyers this is the first trust question about a solo-maintained C++ daemon handling untrusted traffic; far cheaper than OTel and a much stronger signal for the §1 embedded/private-delivery audience.
    - **Install/upgrade compatibility matrix + log rotation/retention policy** (P1 ops-reader threshold).
    - **Document the unsupported protocols** and remove any wording that implies enterprise SSO / HTTP-2 / gRPC support (P0, docs-only).
    - Everything else waits: full OpenTelemetry, the full vulnerability-response *process* (beyond the one-page policy above), and every customer-gated item (JWT/OIDC, mTLS, HTTP/2, HA/cluster, distributed quota) needs a signal first; P2 is untouched.
  - Gate for the next step: ≥1 real deployment, or ≥1 concrete paid/customization intent. **AI signals are recorded in two classes** (2026-10-19): "self-hosted inference front-end" (teams running vLLM/SGLang wanting a C++ zero-dependency front door with SSE streaming and token quotas) counts toward the AI epic below; "SaaS model routing" does **not** — that market is occupied by LiteLLM/Higress and a solo C++ entrant has no edge there. Zero signal means the feature surface does **not** expand, and the unmet Month 4~6 measure is reported honestly rather than papered over.
- **[Conditional · only after a concrete AI signal] AI traffic layer — one epic with a dependency order, not three independent features.**
  1. **Model routing + model-level observability** — smallest change; reuses the existing upstream/circuit/metrics machinery and adds a model-name → provider mapping plus model/token/first-token metrics.
  2. **SSE / streaming passthrough** — ⚠ the deepest change: today the response path buffers the whole backend response before re-framing it (`forward_request` / `UpstreamExchange`), so low-latency streaming is genuinely not supported yet; this needs a streaming re-frame with end-to-end backpressure, client-disconnect cancellation, and idle/first-token timing.
  3. **Token / cost quota** — depends on (1) for model identity and on (2) to read usage off the stream; the existing request-level token bucket stays separate, and cost figures are labelled estimates, never billing-grade.
  The three do not conflict — they compose into one AI layer — but land them **one at a time, each triggered by a concrete user need**; a solo-maintainer "all three at once" branch is exactly the unvalidated big-bang this plan exists to avoid.
- **[Later pipeline] Re-baseline the Nginx throughput gap on a quieter host.** The structural cause behind [BENCHMARKS.md](BENCHMARKS.md) Finding D is closed (eventfd wake-up 2026-10-13, fully event-driven upstream forwarding 2026-10-14). Re-run the comparison against the corrected fixture on a box with more cores and less background load, and isolate any remaining log-write, buffer-copy and TLS-record costs; record evidence in [BENCHMARKS.md](BENCHMARKS.md). Not on the critical path for commercial validation.
- **Deferred by decision (do not start without a concrete customer signal).** mTLS to backends, OIDC/JWT, cluster control plane / distributed quotas, and prompt-guardrail / semantic-cache / MCP-gateway features. Each is a large, customer-shaped commitment; the single-node, zero-external-dependency positioning stays intact until a real user requires otherwise.
