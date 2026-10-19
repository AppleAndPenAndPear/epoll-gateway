# Changelog

Notable changes to the project. Format follows [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/), entries sorted newest first.

> For a detailed snapshot of the current project state, see [PROJECT_STATUS.md](PROJECT_STATUS.md). This file traces back "what was done, when, and why".

## 2026-10-19

### Performance (profile-driven hot-path optimization)

- **Proxy hot path optimization after a `perf` profile under c50 load.** The profile attributed the gap to nginx to four concrete costs, each addressed with a bounded change in [http_handler.cpp](../src/server/http_handler.cpp) / [mylogger.cpp](../src/common/mylogger.cpp):
  - **Response coalescing**: the send queue received the serialized header and the body as two separate chunks, so every proxied response paid two `SSL_write` calls (two TLS records) and two syscalls. Header and body are now coalesced into a single buffer when the body is ≤ 64 KB (large bodies keep the two-chunk layout to bound peak memory), so a small response is one TLS record and one syscall.
  - **`headers_to_string` rewritten from `std::ostringstream` to direct string building with a size pre-pass**, and `generate_trace_id` / `generate_traceparent` from ostringstream stream formatting to `snprintf`. All three run once per request.
  - **Allocation-free response-header filtering**: the per-response scan used to lowercase a copy of *every* upstream header name (one allocation + libc `tolower` per character each) and parsed the `Connection` header through `std::istringstream`. It now uses a case-insensitive ASCII compare against the literal, one reusable lowercase buffer for the whole loop, and manual comma splitting. `ascii_to_lower` keeps the project's unsigned-char convention.
  - **Log flush policy: `flush_on(info)` → `flush_on(err)` + 1 s periodic flush.** AUDIT and the access log are info-level, so `flush_on(info)` put an `fflush` + write syscall on the hot path *twice per request*; this was the largest fixed per-request latency term found under load. Errors and above still flush immediately, `spdlog::flush_every(1s)` keeps `tail -f` near-real-time, and graceful shutdown flushes everything via `spdlog::shutdown()`. **Behavior change**: a hard crash can now lose up to one second of info-level (AUDIT/access) log lines — previously only the un-flushed stdio buffer could be lost. Request-path AUDIT entries are info-level and rely on the periodic flush; admin AUDIT failure paths are warn/error-level and still flush per message.
- **Quantitative validation status — sprint stopped, candidates exhausted.** A clean-window re-baseline (load 0.32, IDE helpers renice'd, stray instances removed) measured the optimized gateway at 744 RPS (c8) / 893 RPS (c50) vs nginx 1491 / 1775 → **`gw:nginx ≈ 2.0x`, which does not reach the ≤1.5x stop rule.** The optimization lifted the gateway ~20-45% over the pre-optimization baseline (608/604) but barely moved the ratio. The capture is read as real (the gateway held ±3% across rounds and was verified to be the optimized binary, while `direct`/`nginx` still swung ±30-90%), so the stable ~745 RPS ceiling is a structural per-request limit, not contention noise. All profile-driven candidates are implemented and the last one (jemalloc) is blocked by the zero-dependency constraint, so per the pre-agreed rule the sprint stops on the "candidates exhausted" prong with a **negative quantitative verdict**. Closing the remaining gap needs a less-contended box and is logged as future work in [ROADMAP.md](ROADMAP.md), not a gate for validation. Details and the full matrix: [BENCHMARKS.md](BENCHMARKS.md).
- **Structural (nginx-level) send-path change, continued deliberately past the "candidates exhausted" verdict.** The earlier sprint had already reduced the TLS client path to one `SSL_write` per response by coalescing header+body; two remaining per-response costs in the same path were removed next, in [http_handler.cpp](../src/server/http_handler.cpp):
  - **The send queue now holds `std::string` instead of `std::vector<char>`**, so a fully-built header buffer is *moved* into the queue instead of copied. Previously every response copied the serialized header — and, on plaintext, the body — once more into a fresh `vector<char>`.
  - **Plaintext responses are now flushed with a single `writev`** that gathers the queued chunks (header + body) into one syscall, replacing one `send()` call per chunk.
  - **TLS keeps the coalesced single buffer** (only the redundant second copy is gone). `SSL_write` requires contiguous plaintext and **OpenSSL has no `SSL_writev`** — verified against this build's 3.0.13 `ssl.h`, where the symbol does not exist — so header and body are still concatenated into one buffer to keep one record / one call. The originally-planned "zero-copy `SSL_writev`" is therefore **not applicable to the TLS path**.
  - Verified functionally only: build OK, unit 137/137, integration 155/155. **No RPS number is claimed** — on the shared 2-vCPU box the noise exceeds the expected increment, so this is logged as a structural change pending re-measure on a quiet machine, not as a measured win.
- **Third structural change: route matching and the proxy request-header path no longer allocate or use iostreams per request.** A fresh `perf` profile under c50 TLS load put ~4% of process CPU into route resolution plus the per-header string churn, from two places:
  - `route_utils.h`'s `matchRoute` split *every* route pattern and the request path with `std::istringstream` + `std::getline` into a `std::vector<std::string>` (one stream + one vector alloc per segment) — **for every route, on every request** — and `resolve_route` ran the whole match **twice per route** (once to build the 405 `Allow` set, once for the actual match), with the `host` / `x-tenant-id` lookups and the allocating `normalize_header_value(host)` copies inside the loop. `matchRoute` now walks both strings segment-by-segment in place (`next_path_segment`), `splitPath` is removed, the header lookups are hoisted out of the loop, the path match is evaluated once per route, and `routeMatchesPath` skips host/tenant normalization entirely when the field is `"*"` (the default).
  - `http_client.cpp`'s request-side `connection_tokens` still split the `Connection` header through `istringstream` + `getline`, and `find_header_value` built a lowercase substring of every header's field name on every lookup; both now compare/scan in place with the project's unsigned-char `tolower` convention (`connection_tokens` mirrors the already-optimized `response_connection_tokens` in `http_handler.cpp`, so the two sides cannot drift).
  - Verified structurally on the profile: `basic_istream` / `basic_ios`, `resolve_route`, `matchRoute` and `memchr` leave the top symbols and `tolower` self-time roughly halves (1.45% → 0.66%). Functional gate: build OK, unit 137/137, integration 155/155. **No RPS number is claimed** — same shared-box caveat as above.
- **Fourth change: the access/audit logger is now synchronous (nginx-style), removing the per-message async machinery.** Profiling the c50 TLS load attributed ~11% of the worker process's CPU to the spdlog async thread, plus ~3% of worker-side CPU to the cross-thread handoff itself (`pthread_cond_signal` / futex wake / mutex). Measuring the logging cost directly, by running the same load with `access_log_enabled: true` vs `false` interleaved, put it at ~16% of CPU per request. [mylogger.cpp](../src/common/mylogger.cpp) previously created an `spdlog::async_logger` on a 2-thread `spdlog::thread_pool`, so every access line paid a queue handoff, a heap copy of the formatted line (`log_msg_buffer`), two extra logger threads and a periodic flusher — per worker process, for one access-log line per request.
  - It is now a plain synchronous `spdlog::logger` over the same two sinks. Writing a line costs a `memcpy` into the stdio buffer of the rotating file sink (actual file I/O is bounded by the unchanged 1 s periodic flush) — the same model nginx uses for its access log. `flush_on(err)`, `flush_every(1s)`, the pattern, the 50 MB × 5 rotation and all log content are unchanged, so no log consumer sees a difference. **Behavior note**: log lines are now written on the worker's event-loop thread; a pathologically slow disk could briefly delay that worker, which the stdio buffering + 1 s flush bound, and which nginx accepts too.
  - **Why `localtime`/time-zone was *not* the target**: a micro-benchmark (300 k × `localtime_r` + `strftime` with `%z`) measured ~0.5 µs and confirmed the ~4.8% `__tz_convert` figure perf reported was a call-graph accounting artifact, not real per-request time — so time caching was rejected as a non-fix.
  - A/B (async vs sync, interleaved, 3 rounds, c8, same config): sync beat async in **every** round on both throughput and worker CPU-per-request (e.g. RPS 161/168/153 vs 113/94/135; CPU/req ~2.7 ms vs ~3.9 ms). The box was heavily loaded during the run (load avg 7.6 on 2 vCPU), so the absolute figures are not quotable — the **unanimous direction** is the claim; re-measure on a quiet box. Functional gate: build OK, unit 137/137, integration 155/155.
- **Fifth change: the per-dispatch params are moved into the owning exchange instead of being held twice (allocator churn, profile-driven).** A fresh `perf` profile of the *post-logging* build under c50 TLS load put the allocator family at ~6.5% of flat self-time (`malloc` 2.10 / `_int_malloc` 1.89 / `cfree` 1.34 / `_int_free` 1.12), and the call graph attributed almost all of it to `std::unordered_map<string,string>` churn — the request/response **header maps** (`_Hashtable::_M_allocate_node` copying a node, `_Hashtable::clear()` destroying one, `_Map_base::operator[]` inserting one) — not to generic allocation.
  - The proxy path built the backend request's params **twice**: `ChildExchange` kept a full `UpstreamExchange::Params params` snapshot *and* `make_unique<UpstreamExchange>(child->params)` copied it again into the exchange's by-value constructor parameter (then moved it to `params_`). So the per-request header map was deep-copied one extra time per dispatch — and once **per dependency** on a composite route.
  - `ChildExchange::params` is removed; the params are now built in a local and `std::move`d into the exchange, which owns them for its whole lifetime and exposes them via `ex->params()`. The route-level **retry** path still needs the params, so it copies them out of the outgoing exchange before replacing it — paying one deep copy only on the rare retry, never on the steady state.
  - **Why not the planned pmr `monotonic_buffer_resource` arena:** the profile shows the cost is the `std::unordered_map<string,string>` header maps themselves, so an arena only pays off if those maps become `std::pmr` containers — which would ripple through `HttpRequest`/`HttpResponse`/`UpstreamExchange::Params`/`BackendResponse` and every function taking a `const std::unordered_map<string,string>&` (parser, `forward_request`, `inject_forwarded_headers`, hop-by-hop filters, …). That is a large, lifetime-sensitive blast radius for a bucket that is only ~6% and mostly shared with nginx; the targeted copy elimination removes one of the four header-map copies per request with no type changes. The nginx-style "flat header list in a pool" that would remove the remaining churn is logged as architectural future work in [ROADMAP.md](ROADMAP.md), not attempted here.
  - Verified structurally: the header-map node copy for the params is gone (one fewer deep copy per dispatch). Functional gate: build OK, unit 137/137, integration 155/155. **No RPS number is claimed** — same shared-box caveat as above; re-measure on a quiet machine.

### Tests

- Unit 137/137, integration 155/155 (2026-10-19, after the optimization changes).

### Changed

- **`composite_target.name` now has a runtime effect: it is surfaced as the `aggregate` field of the composite JSON response** (`{"route", "aggregate", "status", "dependencies"}`). Previously the field was required by validation but never read at runtime — the sample configs happened to set it equal to the route name, which masked that it did nothing. `route` continues to identify the matched route (the same value used by metrics and the audit log); `aggregate` identifies the composition itself, so the two can legitimately differ. The non-empty validation is unchanged and is now meaningful.

### Tests

- One new integration assertion (the `/profile` composite reports `aggregate: "profile-page"`): unit 137/137, integration 155/155 (2026-10-19).

## 2026-10-18

### Changed

- **Composite routes are now driven by the worker's own epoll, like single-upstream routes — a fan-out no longer blocks the event loop.** Previously the composite path submitted each dependency to the process-level `BoundedAsyncUpstreamExecutor` and blocked the worker's epoll on `std::future::wait_for` for up to `total_timeout_ms`, so one worker could not serve any other connection (health checks, static files, other requests) while a fan-out was in flight. Composite dispatch now reuses the same non-blocking `UpstreamExchange` state machine as the single-upstream path: `ExchangeState` was generalized to hold **N** child exchanges (single-upstream is N=1) with one shared driver, each dependency becomes its own exchange registered on the worker's epoll, and the aggregate is framed when the last child finishes or the overall deadline fires. No thread waits on a fan-out any more.
  - The overall `total_timeout_ms` is enforced as a **hard wall-clock deadline independent of each child's idle deadline**: `UpstreamExchange` refreshes its idle deadline on every byte of progress, so a slowly-streaming backend would otherwise outlive the aggregate budget forever. The loop checks the composite deadline and abandons every still-in-flight child (`force_finish_child`, synthesized `timed_out`/504 item) before finalizing. Reason: the removed blocking call could not be interrupted mid-syscall, so the deadline's job is to stop the *loop* waiting, which the state machine does at the socket level.
  - Composite children now report `record_success` / `record_failure` to the circuit breaker (previously composite only *queried* `allow_request` and never reported an outcome), so the breaker now accounts for composite traffic exactly as it does for single-upstream traffic.
  - **Removed `BoundedAsyncUpstreamExecutor`** (`include/server/upstream_async.h`, `src/server/upstream_async.cpp`) and its configuration keys `upstream_max_inflight` / `upstream_max_queue`: after the composite path moved onto the event loop the executor had no remaining callers. Composite fan-out is no longer bounded by a process-level in-flight cap; it is bounded by the connection/fd budget like every other route ([DEPLOYMENT.md](DEPLOYMENT.md) §11 updated). A per-composite fan-out cap, if wanted, is a separate item.
  - **Outward JSON contract change**: the `rejected` / `executor_overloaded` dependency statuses are gone (they only existed to surface executor overload), since there is no longer a bounded executor to reject a dependency. The aggregate shape (`route` / `status` / `dependencies.<name>`) and the remaining item fields are unchanged.
  - Known inconsistency kept as-is for parity: the composite path's `allow_request` / `record_failure` recovery timeout still uses the dependency's own `timeout_ms` (not `circuit_recovery_timeout_ms`); unchanged here, tracked separately.

### Tests

- Unit 137/137 (the six `BoundedAsyncUpstreamExecutorTest` cases and the `upstream_max_inflight` config-validation case were deleted with the executor; the file-schema test dropped the two removed keys), integration 154/154 assertions (2026-10-18). New composite integration cases: a **static request is served while a composite is in flight on the single-worker server** (direct proof the loop is not blocked); a hard aggregate deadline is enforced **against a backend that streams progress** (proving the deadline is independent of the child's refreshed idle deadline); composite dependencies **drive the circuit breaker open** after recorded failures; and a **client disconnect mid-composite** leaves the server responsive with no fd leak.

## 2026-10-17

### Added

- **Aggregate total deadline + cancellation propagation for composite routes.** New `composite_target.total_timeout_ms` (0 = disabled, the previous behaviour) bounds the whole fan-out. It is propagated two ways so it cannot be evaded: each dependency's backend call is given `min(dep.timeout_ms, total_timeout_ms)`, so a slow call cannot outlive the budget, and the caller waits on each dependency's future only up to the shared deadline (a bounded `wait_for` instead of an unbounded `get()`). A dependency that misses the deadline is reported as `timed_out` (504) with `error: upstream_timeout`, so a required miss still aggregates to `partial_failure` (503). Reason this is the *only* form of cancellation available: `forward_request` is a blocking call that cannot be interrupted mid-syscall, so "cancel" here is cooperative — the caller stops waiting and each backend call self-terminates at its (deadline-bounded) effective timeout, releasing its executor slot when it returns. Because a bounded wait can let the caller return before a dependency task finishes, the task now captures the request (headers + body) and client IP **by value** (it previously borrowed the request-scoped objects through references); this is required for correctness, since `requests_[sock_ptr]` can be overwritten or erased once dispatch returns.
  - ~~Known limit~~ **Resolved on 2026-10-18**: the synchronous wait described here was replaced by a fully non-blocking, event-loop-driven fan-out (see the 2026-10-18 entry). The bounded `wait_for` no longer exists.

### Tests

- Unit 146/146, integration 144/144 assertions (2026-10-17). Two new `ConfigValidationTest` cases cover the new field (accept + reject-negative); `test_composite_total_deadline` proves a dependency whose own timeout is 5 s is cut off by a 300 ms route deadline well before the backend's 1.5 s sleep.

## 2026-10-17

### Fixed

- **The pooled-connection liveness probe was silently a no-op.** `Poller::wait(fd, event, 0)` returned `Timeout` before calling `poll` at all — the elapsed-budget guard `remaining <= 0` fired first for a zero budget — so the 0-timeout readiness check on checkout in both `forward_request` and `UpstreamExchange::open_connection` always concluded "not ready" and used the pooled socket unconditionally. A connection the backend had closed while idle was therefore only discovered at send/read time (surfacing as a wasted round trip or an RST/502) instead of being discarded at checkout, which is what [PROJECT_STATUS.md](PROJECT_STATUS.md) and the architecture note describe. `timeout_ms == 0` now performs exactly one non-blocking `poll` and reports readiness immediately; a non-zero budget that has already elapsed still short-circuits to `Timeout`, and a zero timeout no longer spins on repeated `EINTR`.
- **Two undefined-behaviour `::tolower` sites.** `std::transform(..., ::tolower)` on a raw `char` is UB for bytes ≥ 0x80. The live `handle_read` connection-header normalization (keep-alive decision) now goes through `unsigned char`, matching the convention already used in `http_parser.cpp`; the second site was in dead code (removed below).
- **Upstream pool identity now includes the CA file.** The pool key was `tls/<server_name>|` (or `tls/insecure|`), so two upstreams sharing host:port and server name but trusting **different** CA bundles could hand each other a pooled session that had been verified against the other's trust store — a verification-policy mismatch. The key is now `tls/<ca_file>|<server_name>|`, matching the SSL_CTX cache key in `get_upstream_ssl_ctx` (which already partitions by CA file). Narrow in practice (needs a duplicated host:port with a divergent CA), but it restores the documented invariant that pool identity carries the full verification policy.

### Changed

- Removed dead code with no callers in the tree: `HttpHandler::process_request` (a stale duplicate of the `handle_read` keep-alive logic, and the second UB `::tolower` site), `HttpHandler::close_connection` (a one-line `cleanup` wrapper), and the file-local `split()` helper. The redundant `error == nullptr ? "upstream_unavailable" : "upstream_unavailable"` ternary in the 502 code mapper collapsed to a single string literal. No behaviour change.

### Tests

- Unit 144/144, integration 140/140 assertions (2026-10-17). The pool/probe fix is exercised by the existing connection-reuse integration cases.

## 2026-10-14

### Changed

- **Single-upstream forwarding is now driven by the worker's own epoll, not a per-request executor thread.** The 2026-10-13 `eventfd` fix removed the *event-loop block* but still ran the round trip on a blocking `BoundedAsyncUpstreamExecutor` thread, so proxy concurrency was bounded by the executor's thread count. The round trip is now a non-blocking state machine (`UpstreamExchange` in `src/server/http_client.cpp`): the upstream socket is registered in the same epoll as the client sockets and the loop advances it through `Connect -> Connecting -> TlsHandshake -> Send -> Read -> Done` on readiness, so there is no thread per request and the "thread bound" is gone (the full Finding D design; see [BENCHMARKS.md](BENCHMARKS.md)).
  - Reasons for the design choices: re-arming uses `del()` + `add()` rather than `mod()`, because an internal retry `close()`s the old fd (which the kernel auto-removes from epoll) and the new socket can reuse the same fd number — `mod()` would then fail with `ENOENT` or silently re-arm a stale registration. Readiness is only consulted after a real event (never a bare `SO_ERROR` read) so an in-progress connect is not mistaken for success. Each phase transition and successful I/O refreshes an idle deadline mapped to the route's Connect/Write/Read timeout (504 on expiry), and both the `epoll_wait` timeout and the reaper call `check_upstream_timeouts()` so a stalled round trip is never left to an unbounded wait.
  - The safety invariants are unchanged: a pooled connection that the backend closed before send is retried **once** on a fresh connection and only for idempotent methods; a request whose bytes reached the backend is never retried. Bytes past the framed response end are re-checked into the pool as leftover, as before. TLS policy stays part of the pool key.
  - The `eventfd`, `async_fd`, and the single-upstream use of `BoundedAsyncUpstreamExecutor` are removed; the executor is retained only for composite dependency fan-out, so `upstream_max_inflight` / `upstream_max_queue` now bound composite routes only, not single-upstream routes ([DEPLOYMENT.md](DEPLOYMENT.md) §11 updated).

### Added

- **Concurrent proxy isolation integration case** (`test_concurrent_proxy_isolation` in `tests/integration/run_integration_tests.py`): multiple connections to one upstream route at once each receive their own correct response, asserted for no cross-talk (responses not mis-delivered between connections) and no loss. This is a correctness regression guard for the new loop-driven forwarding, which moved per-connection state onto the same thread.

### Tests

- Unit 144/144, integration 140/140 assertions (2026-10-14). The benchmark comparison for this change is **not** conclusive on this host — see the honest caveat in [BENCHMARKS.md](BENCHMARKS.md); the box is 2 vCPU with a single-threaded Python mock saturating at ~800 RPS and background load ~2.3.

## 2026-10-13

### Fixed

- **The event loop no longer blocks on an upstream round trip.** `handle_read` runs on the TcpWorker event-loop thread and, for an upstream route, called `run_upstream_task`, which submitted the backend call to `BoundedAsyncUpstreamExecutor` and then **blocked the event-loop thread on `future.get()`** — one worker served proxied requests strictly one at a time, so proxy throughput was pinned to `1/upstream_latency` regardless of client concurrency. Each `HttpHandler` now owns an `eventfd`; the loop hands the round trip to the executor and returns immediately, the worker pushes its finished `AsyncUpstreamResult` under a mutex and bumps the eventfd, and the loop frames and sends it from `drain_async()` (dispatched by `TcpWorker` when the registered `async_fd` becomes readable). Every per-connection map stays single-threaded on the loop; the worker snapshots `config_`/route fields by value (a concurrent SIGHUP reload reassigns them in place) and touches only the mutex-guarded result vector plus the eventfd. A connection with a round trip in flight is left disarmed (`EPOLLONESHOT` already fired), so a pipelined second request cannot run concurrently with the first. Rationale: proxy throughput now grows with client concurrency like the static path (see [BENCHMARKS.md](BENCHMARKS.md), measured ≈1.5-2x at c8/c50 on the shared box).
- **Default upstream concurrency raised 4 -> 64.** `BoundedAsyncUpstreamExecutor` caps each upstream *group* at `max_group_inflight_ == upstream_max_inflight`, counting running **and** queued tasks. With the pre-fix default of 4 that cap was invisible (the blocking loop serialized requests); once the loop stopped blocking, a single upstream could serve at most 4 concurrent requests and answered `503 Upstream executor is overloaded` for the rest. The default is now 64 so ordinary concurrency is served instead of rejected, at the cost of up to 64 executor threads per worker process (sized once at startup; changing it requires a restart).

### Added

- **Per-route/host/tenant latency histograms.** The route dimension exposed only latency `sum`/`count` (`epoll_route_latency_seconds_*`), so per-route P95/P99 could not be computed — operators could see *that* a route was slow but not the tail. `Metrics::RouteMetricSample` now keeps a cumulative bucket array per dimension, using the same boundaries as the global histogram (so the two are directly comparable), and `to_string()` exports a proper `epoll_route_request_duration_seconds` histogram (`_bucket`/`_sum`/`_count`, with `le` labels incl. `+Inf`). The old `epoll_route_latency_seconds_sum/_count` series are replaced by this family.
- **Per-dimension failure-rate metrics.** `epoll_route_requests_total` now reports 2xx/3xx/4xx/5xx (3xx was recorded internally but never exported), so the failure rate is derivable in PromQL as `rate(...{code="5xx"}) / rate(...)`. Three cause-specific counters are now emitted for the first time — `epoll_route_upstream_errors_total`, `epoll_route_auth_failures_total`, `epoll_route_rate_limit_hits_total` — the underlying counts already existed but had no exposition line.

### Tests

- 3 unit tests (bucket values against the documented boundaries, cause breakdown, and bucket monotonicity so `histogram_quantile` stays valid) and 4 integration assertions on the new `/metrics` route families. Totals: 144 unit / 138 integration assertions.

### Changed

- Both READMEs' `/metrics` metric tables and PromQL examples updated to the new `epoll_route_request_duration_seconds` family; [PROJECT_STATUS.md](PROJECT_STATUS.md) Known Limitations drops the "no per-dimension histogram" entry, and its unit/integration counts plus the ROADMAP milestone list are synced to the shipped code.

## 2026-10-10

Commercial-readiness hardening pass: a correctness/security audit produced four high-severity fixes (already committed) followed by a batch of Medium/Low consistency fixes, a parser-robustness batch, and forwarded-header completion. Every change below is covered by a regression test.

### Security

- **Factory default credentials removed.** `config.json` shipped three usable key shapes (`test-key-123`, `premium-key-456`, the data-plane `api_keys` array, and a working `admin-demo-key`) and both the `Dockerfile` (`COPY config.json .`) and `scripts/package_release.sh` bundled the file into release artifacts — a real out-of-the-box exposure. The data-plane `api_keys` array is gone and `admin.enabled` now defaults to `false` with no keys, so a fresh deployment authenticates nobody until an operator configures it. `admin.port`/`admin.bind` are kept so enabling the listener is a one-line change. Keys should be supplied via `api_keys_file` or the `GW_API_KEYS` environment variable, never inline.
- **API-key validation is now constant-time.** `ApiKeyManager::validate()`/`get()` used `unordered_map::find`, whose hash-then-byte-compare exits at the first mismatch and leaks the matched prefix through response timing. Both now scan every key through a `volatile`-accumulator `constant_time_equals` with no early return, matching the pattern already used by the admin API.
- **Static-fallback path traversal guard unified.** The static-fallback branch used a naive `path.find("..")` while the `static` route type used `is_safe_static_path()` (canonicalize + require the result to stay under the document root). Encodings the naive check missed on one path were therefore accepted on the other; both now call `is_safe_static_path()`.
- **`Server` header reports the real version.** It was a hard-coded `EpollHTTP/0.2` while the project is `0.2.1`; it now reads the CMake-generated `GATEWAY_VERSION`, so the banner cannot drift from `/version` again.

### Fixed

- **Already-compressed upstream responses were gzipped a second time.** The upstream `Content-Encoding` check was case-sensitive, so a backend sending `content-encoding: gzip` was treated as uncompressed and the gateway double-gzipped the body, corrupting it. The check is now case-insensitive.
- **Composite fan-out could exhaust threads.** Dependency fan-out used unbounded `std::async` (one thread per dependency per request), so load scaled thread count without limit; it now submits through the shared bounded upstream executor, which returns a deterministic 503 on overload instead of spawning. The same change fixed a dangling-reference bug where a loop variable was captured by reference by the async tasks.
- **Incomplete hop-by-hop header stripping.** Request-side and response-side stripping missed several RFC 7230 §6.1 fields (`Proxy-Authorization`, `Proxy-Authenticate`, `Proxy-Connection`, bare `Connection` on the response), so proxy credentials could leak to a backend and backend hop-by-hop fields could reach the client. Both directions now strip the full set plus the tokens named in `Connection`.
- **Upstream response headers could inject headers toward the client.** A backend header value containing a bare CR or LF was re-emitted verbatim, splitting the response and allowing header injection. Any response header whose name or value contains CR/LF is now discarded entirely.
- **Bare LF line endings were accepted as CRLF.** A bare-LF request line, header terminator, or end-of-headers was treated as a valid line break — a request-smuggling vector when a front-end proxy disagrees. The parser now tracks whether a CR preceded the LF and rejects bare LF with 400 + `Connection: close`.
- **Whitespace before a header colon was accepted.** `Foo : bar` was parsed by trimming the field name; RFC 7230 forbids the space before the colon (another smuggling vector). The field name is no longer trimmed, so its raw characters fail the `tchar` check and the request is rejected.
- **Chunked trailers were not fully consumed.** Only the first CRLF after the last-chunk was consumed, leaving the remainder of the trailer section (and its terminating CRLF) in the buffer, which garbled the next request on a reused connection. The trailer section is now read line by line until the empty line, so the connection stays in sync.
- **`std::tolower` invoked with a negative `char`.** Header names were lowercased by passing a `char` directly to `std::tolower`, which is undefined behaviour for bytes ≥ 0x80. Both call sites now cast through `unsigned char`.
- **Rate limiter read its default config outside the lock.** `RateLimiterManager::try_acquire(key)` read `default_config_` without holding the mutex while `update_default_config()` wrote it under the mutex — a data race on reload. Both overloads now take the lock and delegate to a `try_acquire_locked` helper.

### Added

- **Forwarded-request headers completed.** Upstream requests now carry `X-Forwarded-Proto` (always `https`, since the client-facing listener is TLS-only by configuration) and `X-Forwarded-Host` (from the client's `Host`), and `X-Real-IP` is overwritten rather than appended so a client-supplied value cannot survive. All these names are written lowercase to match the parser's lowercased header keys — writing mixed case would create a second map entry alongside a client-supplied name and let the client spoof the value (this exact bug was caught by the new integration assertion).
- 6 parser unit tests (bare-LF request line / header terminator / end-of-headers, whitespace before colon, full chunked-trailer consumption, keep-alive after a trailer request) and 5 integration groups (forwarded headers incl. spoof overwrite, hop-by-hop stripping, upstream response-header injection, no double gzip, bounded composite fan-out). Totals: 130 unit / 127 integration before the resource-budget work below.

### Resource budgets and overload behaviour

The remaining commercial-readiness gap was that capacity was unbounded and its overload behaviour undefined. Each resource now has an explicit limit with a defined response, tabulated in [DEPLOYMENT.md](DEPLOYMENT.md) §11.

- **`max_response_bytes` (default 64 MB) makes the dominant memory term explicit.** The cap on a single buffered upstream response was a hard-coded `constexpr` in the proxy path, invisible to operators — yet multiplied by concurrent proxied requests it dominates memory use. It is now a validated config key (1 byte .. 4 GiB), threaded through `forward_request`; a response above it is rejected as a framing error (502) instead of buffered unbounded. DEPLOYMENT.md §11 gains a **memory model** with the worst-case formula and sizing guidance.

- **`max_connections` (default 10000) caps concurrent client connections process-wide.** It is enforced through a new `ConnectionBudget` shared by all workers (a per-worker counter would let the process hold `num_workers × limit`). A slot is reserved at accept and held by an RAII `Ticket` stored in the connection-table entry, so it is released exactly once on any teardown path (idle timeout, peer close, handshake failure, forced drain) — the accounting cannot drift from the real connection set.
- **Overload sheds, it does not queue.** When the ceiling is reached, the connection is accepted and closed immediately (no TLS handshake, no epoll registration). This deliberately drains the kernel backlog — the listen fd is level-triggered, so leaving connections unaccepted would spin the worker — and keeps the fd/memory cost of a flood bounded.
- **Composite fan-out limits are now configurable** (`upstream_max_inflight` default 4, `upstream_max_queue` default 64) instead of hard-coded constants, so the async upstream budget is tunable per deployment. Exceeding them returns a deterministic 503. These are fixed at startup because the executor's threads are sized once.
- **Observability:** `epoll_connections_active` (gauge) and `epoll_connections_rejected_total` (counter) in `/metrics`; the gauge is sampled by the 1-second reaper so the accept/release hot path stays free of metrics work.
- `max_connections` is validated (`must be > 0`) and applies live on reload; lowering it never evicts existing connections, it only refuses new ones until usage drops below the new ceiling.
- Tests: 9 unit tests (admission, release, move semantics, live limit changes, and a concurrent-acquire test proving the limit is never exceeded) plus an integration case that runs a dedicated server with `max_connections: 1` and asserts the second connection is shed while the first is kept.
- **Sustained load/overload test.** A dedicated server runs with `max_connections: 32` while 100 concurrent clients connect-and-hold for a few seconds, asserting the gateway sheds connections (`epoll_connections_rejected_total > 0`), survives, keeps serving afterwards, and keeps its fd count bounded. Short by default; `EPOLL_LOAD_TEST_SECONDS` extends it for a soak. Integration total: 134.

### Changed

- **The connection-reuse integration test was de-flaked.** The mock backend counted every TCP accept, but the gateway's 1-second active health check opens a bare connection to each upstream and never sends a request, so the shared counter was polluted whenever a run straddled a health-check tick. The mock now counts a connection only on the first request it actually serves, which removes the background noise instead of loosening the assertion.
- Documentation synced to the shipped code: this file's cross-doc links repaired (`docs/…` paths that resolved to `docs/docs/…`), test counts updated across [PROJECT_STATUS.md](PROJECT_STATUS.md) and [ROADMAP.md](ROADMAP.md), the ROADMAP's stale `../CHANGELOG.md` link fixed, and the API-composition rows in [COMMERCIAL_READINESS.zh-CN.md](COMMERCIAL_READINESS.zh-CN.md) moved from "待做" to their actual implemented state.

### Supply-chain gate

- **SAST + SBOM + SCA now gate a release.** New `scripts/supply_chain.sh` (invoked as `./ci.sh supply-chain`, deliberately not part of `all` so local builds need not install the tools) runs `cppcheck` SAST over `src/` and `include/` failing only on `error` severity, generates an offline CycloneDX 1.5 SBOM of `build/server`'s linked shared libraries to `build/sbom.cdx.json`, and scans it with `osv-scanner`. A missing tool is reported as SKIPPED with its install command, never a silent pass; GitHub Actions installs `cppcheck` and the `osv-scanner` release binary and fails the job if any gate fails. See [SUPPLY_CHAIN.md](SUPPLY_CHAIN.md).

## 2026-10-09

### Added

- **Composition diagnostics and logging policy.** W3C `traceparent` is generated at the request boundary, propagated on the upstream hop, and echoed back, while `X-Trace-Id` stays compatible (a `traceparent` from the caller supplies the trace id). Composite routes now return per-dependency `traceparent`, `duration_ms`, and status, and a `COMPOSITE` structured log line per dependency. A `logging` config section was added: `access_log_enabled` toggles CLF access lines independently of security AUDIT logging, and `redact_sensitive_headers` / `redact_sensitive_query_params` (with configurable `sensitive_headers` / `sensitive_query_params`) redact credential-bearing headers and query parameters in both CLF and AUDIT output. API keys are always fully masked; request/response bodies are never logged.
- Pipelined-request integration coverage: one TCP write carrying two requests must yield two ordered responses, plus a case that sends the first request in two fragmented writes.

### Fixed

- **Half-packet request assembly could re-parse already-consumed bytes.** `HttpParser::parse` now reports the number of bytes consumed on every call (including inside the request line, headers, and body), and the read loop erases that prefix after each pass. Previously an incomplete request that had advanced the parser state left the original bytes in the buffer, so the next read re-parsed stale data.
- **Coalesced requests already in the buffer were not drained before reading the socket.** `handle_read` now parses buffered bytes first and only calls `recv` when the buffer is empty, so a second request that arrived in the same TCP segment (pipelining) is handled instead of waiting for a new socket event that may never come.
- **TLS partial writes did not resume in the correct direction.** A pending `SSL_write` returning `SSL_ERROR_WANT_READ` re-arms `EPOLLIN` (not just `EPOLLOUT`) via `Socket::last_ssl_want()`, and the worker routes that readiness to `handle_write` rather than treating it as new request data.
- **TLS static-file responses stopped after the first chunk.** The active user-space chunk is now retained across `WANT_READ`/`WANT_WRITE` retries and the send continues from the exact file offset until the whole file is written; previously a single partial write could be mistaken for completion (an 8 MiB delayed-reader integration case reproduces and now guards this).

## 2026-10-08

### Added

- **Bounded asynchronous upstream foundation.** `BoundedAsyncUpstreamExecutor` (`upstream_async.{h,cpp}`) schedules upstream work off the epoll worker with a bounded worker pool, a bounded queue, and per-upstream-group concurrency isolation; overload or shutdown returns a deterministic 503 instead of blocking. Routes resolve their backend identity once through `resolve_upstream_group_id()` (`upstream_target.name`, falling back to the route name) so the executor key always matches the real backend pool.
- **Composite route MVP.** A `composite` target type with named dependencies (required vs optional) is validated at config load, fans out through the bounded executor, and aggregates into an explicit `ok` (200) or `partial_failure` (503) response. A runnable profile-page example and an end-to-end integration scenario (user + orders + recommendations) validate both paths.

## 2026-09-29

### Fixed

- **The v0.2.0 tarball could not run on Ubuntu 24.04.** The binary linked `libfmt.so.8` (what Ubuntu 22.04 provides) while 24.04 ships only `libfmt.so.9`, so it died immediately with `error while loading shared libraries: libfmt.so.8: cannot open shared object file`. The cause: packaging bundled `libspdlog` by name and therefore missed its own transitive dependency `libfmt`. Bundling is now a **whitelist plus transitive closure** — every library the binary needs that is not part of the per-distribution baseline gets bundled, so a dependency of a bundled dependency can no longer be forgotten. A missing bundle fails packaging instead of shipping.
- The `ldd` gate and the CI smoke test could not have caught this: both ran on the build machine, which by definition has every library the build needs. Added `scripts/verify_artifact.sh`, wired into `ci.yml` and `release.yml`, which extracts the finished tarball and runs it inside clean `ubuntu:22.04`, `ubuntu:24.04` and `debian:12` containers — loader resolution plus a real start-up (listener accepting connections). Failing there now blocks the release.
- The binary is linked with `-static-libstdc++ -static-libgcc`, removing the `GLIBCXX_3.4.x` ABI floor that an Ubuntu 22.04 build would otherwise impose on older `libstdc++`.
- The aarch64 job built on `ubuntu-24.04-arm` and failed verification on `ubuntu:22.04`: its binaries referenced glibc 2.39 version symbols while the container has 2.35. Both architectures now build on 22.04 (`ubuntu-22.04` / `ubuntu-22.04-arm`), so the single documented "glibc ≥ 2.35" floor holds for the whole release rather than one floor per architecture.
- The cross-distribution check also pinned down the real runtime prerequisite: Ubuntu's base image carries `libssl3` (apt depends on it there), while Debian's ships GnuTLS instead and has no OpenSSL at all — `libssl.so.3` is simply absent until `libssl3` is installed. Since the tarball deliberately leaves OpenSSL dynamic, the verifier installs exactly the declared prerequisite (`libssl3`, `zlib1g`) inside the container and then still asserts that nothing else is unresolved. A gap in our own bundle cannot be papered over that way, because no distribution package provides `libspdlog`/`libfmt` on a minimal image.

### Changed

- Bundling policy is now explicit in `package_release.sh` and stated in both READMEs: `libspdlog`/`libfmt` (unstable sonames across distributions) are bundled, while the glibc core and `libssl`/`libcrypto`/`libz` stay system-wide on purpose — this is a security product, so crypto must keep taking CVE patches from the distribution rather than be frozen inside the tarball, and `libssl.so.3` is the stable soname of the entire OpenSSL 3.x line. The supported floor is glibc ≥ 2.35 with a system OpenSSL 3 (Ubuntu 22.04+ / Debian 12+).
- Version bumped 0.2.0 → 0.2.1.

### Notes

- Process lesson: a check that runs in the same environment as the build verifies itself and structurally cannot fail for the class of bug it targets. "It worked on the machine that produced it" is not verification — a packaging check only means something when it runs somewhere the build environment is absent.

## 2026-09-28

### Added

- **Release channel (v0.2.0)**: `scripts/package_release.sh` builds a Release binary and assembles a self-contained tarball — `bin/epollthread` with an `$ORIGIN` rpath, `lib/libspdlog` bundled (the only non-universal shared dependency; openssl/zlib stay system-wide), plus `config.json`, `www/`, `examples/`, `scripts/gen_dev_certs.sh`, `start.sh`, README×2, LICENSE and the core docs. The archive carries no private keys and no logs. An `ldd` sanity check fails packaging if anything is unresolved; the tarball is smoke-tested end to end (healthz / echo / admin 200) before shipping.
- `.github/workflows/release.yml`: pushing a `v*` tag builds per-arch tarballs (x86_64 on ubuntu-22.04 for the older glibc baseline, aarch64 on the native arm runner), smoke-tests each, publishes a GitHub Release with checksums, and pushes a multi-arch image to ghcr.io. Gitee gets the tag; artifacts are attached manually there.
- Version bumped 0.1.0 → 0.2.0 (upstream TLS, first-run experience, release channel). Both README quickstarts now lead with the prebuilt-download path — the "clone and compile" step stays available but is no longer the front door.

## 2026-09-26

### Added

- First-run experience: a **5-minute quickstart** at the top of both READMEs — clone, `gen_dev_certs.sh`, `build.sh`, `start.sh`, then curl the built-in echo route, the ops endpoints, the key-protected admin API (401 without / 200 with `X-API-Key: admin-demo-key`) and `/metrics`, with no backend to start. The default `config.json` now ships an explicit `tls` section, an enabled admin listener for the auth demo, and no phantom upstreams (`/readyz` used to answer 503 out of the box because its upstream pointed at ports nothing listened on).
- Runnable example scenarios under `examples/`: `load-balanced/` (two mock backends, round-robin alternating responses visible per request, health-check ejection on kill) and `tls-backend/` (verified `https://` upstream with `tls_ca_file` + `tls_server_name`, reusing the dev certificate). Both are copy-paste runnable against the mock backend from the integration suite.
- `scripts/gen_dev_certs.sh`: generates the self-signed dev certificate (CN=localhost, SAN DNS:localhost only — deliberately no IP SAN so IP-literal connections still fail hostname verification, which the integration suite relies on to prove the check is enabled). `certs/` is now gitignored and the previously committed private key is out of the repository; `ci.sh` generates a keypair on demand, and the Docker quickstart mounts `./certs` read-only instead of baking keys into the image.
- The server accepts an optional config path argument (`./build/server examples/load-balanced/config.json`) in addition to the cwd default. A failed config validation now also prints a summary line with the file name and error count.

### Fixed

- The previous release commit was cut without a final CI run and shipped code that did not compile: `http_client.cpp` still passed the old `bool` TLS flag to the connection pool whose signature had already moved to a `scheme` string (the file had been partially reverted by an outside edit between the last green run and the commit). All call sites now pass `pool_scheme`; the lesson is recorded — `./ci.sh` right before every commit, not just before the last one of a session.

## 2026-09-24

### Added

- Upstream TLS: backends can now be declared as `https://host:port` in the `servers` config, and the gateway runs the client-side handshake (non-blocking, integrated with the same `Socket`/`Poller` timeouts) before proxying. Per-upstream options: `tls_ca_file` (empty = the system default trust store, so public-CA backends need no extra config), `tls_server_name` (SNI + hostname binding via `SSL_set1_host`; empty = verify the chain only), and `tls_skip_verify` (an explicit escape hatch for experiments, off by default). Certificate verification is always on unless `tls_skip_verify` is set — the hardening policy (min TLS 1.2, AEAD-only suites) is shared with the server and companion-client contexts. Configuration rejects `tls_*` fields on a plaintext upstream (fail-fast). The connection pool is shared with plaintext upstreams; the client-side `SSL_CTX`s are cached per (CA, server-name, skip-verify) tuple. mTLS (client certificates) and TLS health probes remain future work.
- 5 integration assertions (`test_upstream_tls`): a verified `https://` backend returns 200 and reaches the mock, a second request reuses the pooled TLS connection, a wrong `tls_server_name` is rejected with 502 (proving hostname verification is not a silent no-op), and `tls_skip_verify` connects where verification would fail. 1 new unit test pins the pool-identity split. Test totals: 101 unit / 77 integration.

### Fixed

- Two defects surfaced only once a second, independent TLS client lived in the same process:
  - The connection pool key was `host:port` only, so a pooled connection verified for one hostname/verification policy could be handed to a different upstream on the same host:port — the hostname check happened only during the handshake, and a reused connection never handshakes. The pool key is now `scheme + host + port`, where the scheme embeds the verification policy (`tls/<server_name>` vs `tls/insecure` vs empty for plaintext), so a session verified for one name can never be reused under another.
  - OpenSSL keeps a per-thread error queue: a failed `SSL_connect`/`SSL_shutdown` on an upstream socket left errors behind, and the next `SSL_get_error` on an unrelated client socket in the same thread read that stale error, misclassifying an ordinary `WANT_READ` as fatal and dropping a perfectly good client connection. `sslRead`/`sslWrite` now call `ERR_clear_error()` before every SSL I/O call (the handshake/peek helpers already did).
- `sslRead` lost its `lenient_eof` parameter entirely: both call sites passed `true`, so the lenient-EOF policy moved into the function body and its documentation on `Socket::sslRead` instead of being a knob every caller had to remember to set.

## 2026-09-22

### Added

- The companion client (`src/client/`) now speaks TLS and works as a real smoke test. It resolves its target through `getaddrinfo` (the default host is `localhost`, which the old `inet_pton` call could not accept), runs a non-blocking `SSL_connect` inside its own epoll state machine, verifies the server certificate against `certs/server.crt` (hostname check included) and sends a real `GET <path> HTTP/1.1` request with `Connection: close`. New flags: `--host`, `--port`, `--path`, `--ca`, `--insecure` (skip verification — debugging only) and `--no-tls` (plaintext, for targets that are not this gateway); the exit code is 0 only when a complete HTTP response was received. Before this the client sent the literal string `Hello, server!` over plaintext and could not talk to the TLS-only data plane at all, which made the `./build/client` line in both READMEs incorrect.
- `include/common/tls_utils.{h,cpp}`: the hardening policy (minimum TLS 1.2, AEAD-only cipher whitelist, compression off) is now shared by the server and client context builders instead of living inside `tls_context.cpp`, so both sides of the wire are pinned to the same suites.
- 9 unit tests (`test_tls_utils.cpp`: the shared policy on server and client contexts; `test_client_tls.cpp`: CA required, empty/missing CA rejected, `--insecure` bypass) and 3 integration assertions that run the built client against the real server, including a hostname-mismatch rejection that proves verification is not a silent no-op.

### Fixed

- The client never dispatched the handshake state: `TLS_HANDSHAKING` had no `switch` case, so after the first wait-for-read re-arm the fd stayed disarmed under `EPOLLET | EPOLLONESHOT`. Every run exchanged zero bytes and died on the 10 s idle timeout, even though the TCP connection and the request were fine.
- Client response framing read `Content-Length` without skipping the space after the colon, so an ordinary `Content-Length: 226` parsed as "no digits" and was reported as an incomplete body — the client printed a perfectly good 200 and exited non-zero.
- Client completion depended only on EOF, which needs the peer to close: with the documented default `keepalive_timeout` of 60 s, a valid response would have been reported as a receive timeout (and a truncated one as a failure). The client now finishes as soon as the declared body is complete, keeping the EOF + completeness check as the fallback for close-delimited and chunked responses.
- `sslRead` treated a peer that closes the TCP connection without `close_notify` as a fatal error. OpenSSL 3.0 reports that as `SSL_ERROR_SSL` + `SSL_R_UNEXPECTED_EOF_WHILE_READING` rather than `SSL_ERROR_SYSCALL` with a 0 return, so a response already on the wire was thrown away. The read path now maps both spellings to end-of-stream, because completeness is decided by HTTP framing rather than by the peer's close style.
- The client did not ignore `SIGPIPE`: `SSL_write`/`SSL_shutdown` take no `MSG_NOSIGNAL`, so writing to an already-closed socket during teardown would have killed the process instead of returning an error.
- A self-review pass over the new client code found three more defects before they could bite:
  - `do_send`/`do_receive` assumed their read/write wrappers throw on fatal errors, but `Socket::send` maps `EPIPE` to a -1 return and the SSL wrappers return -1 with `errno=EIO`. The loops treated any non-EAGAIN -1 as "keep going" and would spin at 100% CPU until SIGINT. Both loops now treat an unexpected -1 as fatal.
  - `handle_event` bailed out on `EPOLLHUP`/`EPOLLRDHUP` before reading. A plaintext peer that closes right after responding reports HUP together with `EPOLLIN`, and the still-buffered response was being discarded and reported as a failure. HUP now falls through to the read path, which drains the bytes and lets the framing check decide DONE vs ERROR; only `EPOLLERR` is immediately fatal.
  - The idle timeout counter was never reset when events arrived (its reset sat on an unreachable branch), so a transfer that went silent once for over 10 seconds mid-way was aborted even if it resumed. The budget now restarts on every event batch.
- The client no longer mirrors log warnings to stdout (the shared logger's console sink is removed in the client process only), so `./build/client | <filter>` sees the HTTP response and nothing else; logs still go to `logs/client.log`.
- The client's send path no longer calls `shutdown(SHUT_WR)` when the connection is TLS. It emits a bare TCP FIN instead of `close_notify` and permanently closes the write side, so the `SSL_shutdown` in teardown always failed with `EPIPE`. Plaintext keeps the half-close.
- A pre-publication review of the server data plane found and fixed three issues:
  - The server-side `sslRead` kept the strict EOF policy, so clients that close TCP without `close_notify` (some Java/PHP HTTP stacks) generated error-level SSL logs on every disconnect even though their request was fully handled and the response already sent. The server read path now uses the same lenient-EOF mode as the client — request completeness is driven by framing, not by the peer's close style. With both callers wanting the same policy, the `lenient_eof` parameter was removed as a knob nobody turned and the rule now lives as documentation on `Socket::sslRead`; genuine protocol errors (bad record MAC, handshake failure) stay fatal. The diagnostic for the bare-FIN case also moved from warn to debug, since it fires on every disconnect from a client that never sends `close_notify`.
  - After rejecting a malformed request (400/413) the read loop `continue`d parsing the rest of the buffer. `send_error_response` already queues `Connection: close`, but the loop could still process (and queue responses for) further pipelined requests on a connection that is about to die — and after a smuggling-class rejection the remaining bytes are untrusted anyway. The loop now stops parsing immediately on rejection.
  - `TcpWorker::check_timeout` probed `last_active_` records with `fcntl(fd)`: a closed fd number can be reused by a new connection, so a stale record could in theory pass the probe and kill an innocent connection. All lifecycle paths already insert/erase `conns_` and `last_active_` together, so membership in `conns_` is now the source of truth and the fd probe is gone.

### Changed

- The client owns its `SSL_CTX` through a `unique_ptr` declared before the socket, so the session is released before the context it was created from, and the defaulted move operations are gone — they would have copied a raw context pointer.
- Client CLI errors (unknown flag, missing value, port out of range) now print usage and exit 2, distinct from the 1 returned when no complete response arrived.
- README test counts (both languages), the companion-client sections, PROJECT_STATUS and ROADMAP counts now say 100 unit / 72 integration assertions; ROADMAP's P2 note claiming "6 unit tests for the context builder" matched neither the old nor the new file (it has 4) and is corrected.

## 2026-09-21

### Added

- First architecture article draft in `docs/articles/` — `01-architecture.md` (English) and `01-architecture.zh-CN.md` (中文), linked from both READMEs. It covers the threading model and the cost of `EPOLLET + EPOLLONESHOT`, the control-plane/data-plane split, the three reliability rules (precise response framing, idempotent-only retries, liveness check before pool reuse) and three real bugs with their measurements: the 43 ms Nagle/delayed-ACK floor, the RST-on-close that can destroy a just-written response, and connection reuse corrupted by an unconsumed chunked trailer. Publishing to Juejin/Zhihu/V2EX/HN is still pending.
- Admin key hot rotation: the admin listener watches the reload generation and adopts a new `admin.api_keys` list from `config.json` within ~1 second of any accepted reload (SIGHUP or `POST /admin/reload`), so rotating the admin key no longer requires a restart. A refresh never adopts an invalid config or an empty key list — the previous keys are kept and an AUDIT `admin_keys_refresh_failed` line is written; success logs AUDIT `admin_keys_rotated`. `enabled`/`port`/`bind` remain startup-fixed (rebuilding a listener at runtime has messy failure modes for near-zero value). Integration tests cover rotation: old key 401, new key 200.

### Fixed

- Admin accept loop had no exception guard: an uncaught exception in the thread calls `std::terminate`, so a failure in the (file-reading, JSON-parsing) key refresh or in JSON response building would have killed the whole gateway, data plane included. The loop body is now wrapped in try/catch and logs AUDIT `admin_request_failed` — enforcing the guarantee the surrounding comment already claimed.
- `POST` to an admin endpoint with a request body could lose its response: the body is deliberately never parsed, and closing a socket with unread received data makes the kernel send RST instead of FIN, which can discard the response just written. The rest of the body (per `Content-Length`, bounded, 1 s cap) is now drained after the response and before the socket closes; covered by an integration assertion.
- `admin.api_keys` parsing did not clear the target list first, unlike the data-plane `api_keys` parsing; a reused `Config` would accumulate keys, keeping revoked ones valid. Defensive today (a fresh `Config` per parse) but a security footgun if the parse path is ever reused.
- `Config::validate()` did not check `admin.bind`, so a non-IPv4-literal bind passed validation and only failed later inside `AdminServer::start()` as a generic startup error. Now reported as a field-level `admin.bind` error.
- `deploy/gateway.service`: the install notes (unlike DEPLOYMENT.md §1) omitted creating the `epoll-gateway` user and chowning the working directory, so following the unit's own instructions produced a service that cannot resolve `User=` or write its logs; the steps are now listed in the right order before the unit is enabled. The `Documentation=` placeholder also pointed at a different repository name, and no `LimitNOFILE` was set, leaving the service on systemd's default soft limit of 1024 fds — below what a gateway with a 1024-deep accept backlog, cached static files and pooled upstream connections can hold — where exhaustion surfaces as `EMFILE` on accept. The unit now sets `LimitNOFILE=65535`.
- Integration harness started upstream mocks and then slept a fixed 0.5 s before starting the gateway, so a slow-to-bind mock could still be absent when the first proxied test ran, producing an intermittent 502 in unrelated scenarios. The harness now waits for each mock port to accept connections (and still fails fast if a mock process died).
- README test counts were only partially synced: the intro paragraph of both READMEs and item 9 of the English README's tech-stack section still claimed 84/84 unit and 42 integration assertions while other lines in the same files already said 91/91 and 69. All spots now agree.
- `docs/BENCHMARKS.md` described the upstream connection pool as "planned (P3)" in its interpretation section while the report's own section above documents it as shipped; the bullet now states that the proxy rows predate the pool and records the current reuse bound (10 proxied requests open ≤4 backend connections instead of 10).

### Changed

- Admin key refresh reuses the config revision already parsed and validated by `POST /admin/reload` instead of re-reading the file, removing a window where the validated revision and the adopted keys could differ. The SIGHUP path still reads the file.
- The refresh log distinguishes "admin disabled in the new config" (AUDIT `admin_keys_unchanged`, informational — the listener keeps serving with its previous keys since it cannot be stopped at runtime) from a genuine failure, instead of reporting both as a refresh error.
- ROADMAP's outreach item bundled two deliverables ("English README, first architecture article") behind one checkbox, leaving the finished English README unticked. Split into two entries: English README checked (2026-09-16) and the article/publishing work tracked on its own.

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

- Added [ROADMAP.md](ROADMAP.md): settled the commercialization positioning (three differentiators — lightweight/auditable/embeddable, see the doc for the competitor analysis) plus the P1~P5 technical evolution plan and the 6-month commercialization validation track. Licensing decision: stay MIT for now; evaluate dual licensing once paid intent appears.
- Added [CHANGELOG.md](CHANGELOG.md): records changes and decision rationale by date, backfilling the project timeline since 2026-05-08.
- Cross-checked [PROJECT_STATUS.md](PROJECT_STATUS.md) against the actual code and fully updated the README:
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
