# Benchmark Report: epoll-gateway (wrk baseline)

Baseline throughput and latency of the gateway, measured with
[wrk](https://github.com/wg/wrk) via [scripts/benchmark/run_benchmark.sh](../scripts/benchmark/run_benchmark.sh).
Results are machine-specific; treat them as reproducible observations, not
portable capacity promises.

## 2026-10-02 Benchmark Correction

The original script sent every scenario to `/`, although its proxy route was
configured for `/api/two/*`; that route also inherited API-key authentication
and the benchmark supplied no key. The historical proxy rows and
proxy-specific throughput claims derived from that script were therefore invalid
(they measured static fallback or 401s, not successful upstream forwarding).

The benchmark harness now uses an explicit path per scenario, makes only its
proxy test route anonymous, waits for the mock backend, checks the static/proxy
preflight responses are 200, and refuses to report runs with non-2xx responses,
socket errors or zero completed requests.

The real stall proved to be a TCP issue in the benchmark fixture itself: the
Python mock backend was not disabling Nagle (`TCP_NODELAY`), so every tiny
proxied response paid a 40 ms delayed-ACK penalty. The mock now sets
`TCP_NODELAY` on accepted sockets. The results below are from the corrected
benchmark fixture and reflect the gateway's actual proxy path.

## Environment

| item | value |
|---|---|
| CPU | 2 vCPU (client, gateway, and mock backend share the same box) |
| OS | Linux 6.8.0-142-generic, x86_64 |
| CPU | 2 logical CPUs visible to the process |
| TLS | OpenSSL 3.0.13 server, min TLS 1.2, ECDHE + AEAD cipher whitelist |
| Gateway | 1 worker process, epoll (edge-triggered, one-shot), default thread pool |
| Client | wrk 4.1.0, 1 thread, warmup 3s + measured 15s per scenario |
| Backend | Python `ThreadingHTTPServer` mock returning a small JSON body |

## Scenarios

1. **static** — small HTML file served from `www_root` (response cache hit),
   50 keep-alive connections (saturation load).
2. **proxy** — reverse proxy through `/api/two/benchmark` to the local mock
   backend; connection count is configurable with `BENCH_CONNS` (default 50).
3. **static-light / proxy-light** — same scenarios at 8 connections (light
   load, measures per-request latency rather than max throughput).
4. **handshake** — static file with `Connection: close`, i.e. a full TLS
   handshake for every request, 10 connections.

## Results

| corrected scenario (8 connections) | QPS | P50 | P99 |
|---|---|---|---|
| static | 2055.37 | 3.57ms | 10.74ms |
| proxy | 781.46 | 10.35ms | 65.29ms |
| static-light | 1973.16 | 3.70ms | 17.01ms |
| proxy-light | 999.25 | 8.13ms | 26.80ms |
| handshake (c10) | 2226.76 | 4.12ms | 52.29ms |

| corrected scenario (50 connections) | QPS | P50 | P99 |
|---|---|---|---|
| proxy | 859.28 | 64.57ms | 150.69ms |

## Finding: TCP_NODELAY (the 40 ms delayed-ACK stall)

The first benchmark run exposed a classic TCP pathology: **every request paid
a fixed ~43 ms latency floor**, even at light load (P50 43ms at 8 connections).
QPS at light load was stuck around 170, and TLS handshakes around 220/s.

Root cause: accepted sockets (and upstream sockets) did not set
`TCP_NODELAY`. The server writes responses in several small TLS records
(header record + body record, handshake flights), so with Nagle's algorithm
enabled the second small write stalled on the peer's 40 ms delayed-ACK timer
on every request. In the benchmark fixture, the Python mock backend also needed
`TCP_NODELAY` on accepted sockets so the loopback proxy path did not sit behind
the same delayed-ACK behavior.

After enabling `TCP_NODELAY` on client, upstream and benchmark-backend sockets
(`Socket::setnodelay()` in `src/common/mysocket.cpp`, plus the mock backend in
`tests/integration/mock_backend.py`), the gateway shows healthy proxy numbers:

| scenario | before QPS | after QPS | before P50 | after P50 |
|---|---|---|---|---|
| static-light (c8) | 176 | 949 | 43.04ms | 7.99ms |
| handshake (c10) | 224 | 956 | 42.79ms | 10.09ms |

**Light-load latency improved ~5.4x, handshake throughput ~4.3x, and the proxy path improved from ~23 RPS to ~780-1000 RPS with no 50-connection timeouts.**

## Finding: upstream connection pooling (P3)

Originally the gateway opened one TCP connection to the backend per proxied
request (`Connection: close`, read to EOF). It now maintains a per-upstream
pool of keep-alive connections (`src/server/connection_pool.cpp`): responses
are framed precisely (Content-Length or chunked), any bytes past the end of a
response are kept with the pooled connection, and a pooled connection that the
backend closed while idle is detected and transparently retried on a fresh
connection.

Functional proof: the integration suite counts backend connections — 10
consecutive proxied requests now open at most 2 backend connections instead
of 10.

Connection reuse is functionally proven by integration tests. The earlier
proxy throughput comparison used the wrong path and is invalid; the corrected
benchmark above measures the actual route with the mock backend's TCP delay
removed. These loopback results are a reproducible local baseline, not a
production capacity guarantee.

## Interpretation

- The benchmark harness and Python mock are now corrected; the proxy path no
  longer stalls behind delayed ACKs and no longer times out at 50 connections in
  the default 15 s run.
- The corrected proxy path is roughly 800-1000 RPS under light load on this
  shared 2-CPU box, with a c50 P50 around 65 ms and no request errors.
- Static and handshake results are from the same 2-CPU shared test host and
  should not be compared directly with results from a different machine.
- Connection-pool functional reuse remains proven by the integration suite.

## Reproducing

```bash
./ci.sh build
scripts/benchmark/run_benchmark.sh          # 15s per scenario
scripts/benchmark/run_benchmark.sh 30       # longer runs for smoother numbers
BENCH_CONNS=8 scripts/benchmark/run_benchmark.sh  # lower-concurrency diagnostic run
```

The default 50-connection proxy scenario currently reports an error and exits
when requests time out; this is intentional so a failed load run cannot be
mistaken for a valid throughput report.

---

# Comparison: epoll-gateway vs nginx (in-place baseline)

A competitive comparison against an incumbent gateway, run to validate the
"low resource footprint" positioning with data instead of claims.

## Environment

Same box as the baseline above: 2 vCPU, client (wrk), gateway, and backends
all sharing it. Absolute numbers are meaningless on a saturated shared box —
only ratios between configs are comparable.

| item | value |
|---|---|
| Backend | 4 × Python `ThreadingHTTPServer` (ports 9001-9004), HTTP/1.1 keep-alive, `TCP_NODELAY` |
| Route | single `GET /bench` returning a small JSON body, TLS terminated at the gateway |
| epoll-gateway | release build, 2 workers, same cert, access+audit logging on (access log can now be disabled; AUDIT remains enabled) |
| nginx 1.24 | 2 workers, `keepalive 16` upstream pool, same cert, access log enabled with the same CLF fields for fairness |
| direct | wrk hitting backend 9001 directly (single-backend reference) |
| Load | wrk 4.1.0, `-t2`, c8 (light) and c50 (saturation), 10-12s per run, 3 interleaved rounds, medians below |

Traefik and KrakenD were planned but their release binaries could not be
downloaded from this network (blocked CDN); they are pending.

## Results (medians of 3 interleaved rounds)

| target | conn | RPS | P50 | P99 |
|---|---|---|---|---|
| direct (1 backend) | 8 | 1210 | 6.0ms | 18.7ms |
| direct (1 backend) | 50 | 1239 | 29.0ms | 988ms |
| nginx (4 backends) | 8 | 1549 | 4.7ms | 15.1ms |
| nginx (4 backends) | 50 | 1616 | 29.1ms | 138.8ms |
| **epoll-gateway (4 backends)** | 8 | **608** | 12.7ms | 29.0ms |
| **epoll-gateway (4 backends)** | 50 | **604** | 78.2ms | 227.1ms |

Idle RSS (steady state, after warm traffic):

| process | RSS |
|---|---|
| epoll-gateway (1 process, 2 workers) | 26.0 MB |
| nginx (master 3.8 + 2 workers ≈ 22.6) | 26.4 MB total |

## Findings

1. **Memory parity, not advantage, vs nginx on this workload.** The claim
   "lighter than nginx" does not hold for a single tiny route: both sit at
   ~26 MB. The gateway's edge, if any, shows in single-binary deployment and
   config surface, not RSS.
2. **Throughput gap is ~2.5-2.7x.** epoll-gateway's numbers were extremely
   stable across rounds (600-650 at c8) — it is CPU-bound inside its own
   request path, while nginx/direct numbers bounced with background machine
   noise. Candidates for the gap (unmeasured): per-request log writes, extra
   copies in the proxy buffer path, TLS record sizing.
3. **Measurement traps worth remembering:**
   - `ps` %CPU is a lifetime average, useless for idle checks; sample
     `/proc/PID/task/*/stat` over 3s instead.
   - A stale pre-fix binary was still listening via `SO_REUSEPORT`, silently
     serving half the requests and polluting every round until killed. Always
     `ss -tlnp` before trusting a number.
   - The Python mock needs `disable_nagle_algorithm = True` (see the 43 ms
     finding above) — a regression here shows up as a mysterious 40 ms p50.

## Bug found by the benchmark: handshake-phase spin on dead connections

Under c50 load, a client that disconnects mid-handshake left the connection
in the HANDSHAKING state spinning: `SSL_accept` on a dead fd kept returning
"not finished", the fd was re-armed and retried forever, one error log per
turn — 225k error lines in a 25s window.

Root cause chain (all three required):

1. `Socket::sslAccept()` returned `bool`, collapsing WANT_READ/WANT_WRITE
   (retry) and FAILED (tear down) into the same `false`.
2. The worker's HANDSHAKING branch checked EPOLLHUP/EPOLLERR *after* the
   handshake attempt, so a dead fd never reached the cleanup path.
3. No `ERR_clear_error()` before `SSL_accept`, so the error string was
   `error:00000000` (per-thread error queue pollution).

Fix: `sslAccept()` now returns the four-state `SSLHandshakeStatus`; the
worker tears the connection down on FAILED/HUP before attempting a
handshake; error queue is cleared per operation. Post-fix, the same load
produces 11 log lines instead of 225k.

## Profiling pass: locating the proxy-path gap (2026-10-13)

Profiling method, on the same shared 2-vCPU box. `perf` is unusable here
(`perf_event_paranoid=4`, `ptrace_scope=1`, no passwordless sudo), and no
valgrind/callgrind/flamegraph is installed, so the toolset was: `strace -c -f`
started **as the parent** of the gateway (required: `ptrace_scope=1` forbids
attaching to a non-descendant), `/proc/<pid>/task/*/stat` sampling for the
user/kernel CPU split, wrk, and config A/B toggles.

### Finding A — the benchmark fixture was still measuring a 40 ms Nagle stall

The comparison section above claims the mock "now sets `TCP_NODELAY`". That was
never true: `tests/integration/mock_backend.py` set **no** socket option, and
the `disable_nagle_algorithm` attribute the old text referred to does not exist
in the Python standard library (`socketserver`/`http.server` expose no such
attribute — verified). So the fixture wrote each response as two small segments
(headers, then body) with Nagle on, and every proxied response paid the
client's delayed-ACK timer.

Evidence (single request path, mock on loopback):

| measurement | p50 | RPS |
|---|---|---|
| direct to mock, c4, Nagle (as shipped) | 43.5 ms | 86 |
| direct to mock, c4, TCP_NODELAY | 3.1 ms | 1074 |
| `run_benchmark.sh` proxy c50, Nagle | 0 (all timed out) | 19.9, 200 socket timeouts |
| `run_benchmark.sh` proxy c50, TCP_NODELAY | 74.5 ms | 536, 0 errors |

The fix is in `mock_backend.py` (`_NodelayHTTPServer.get_request` sets
`TCP_NODELAY` on every accepted socket; the same intent the old doc text
described, done for real). **Every proxy number taken before this fix — the
`proxy`/`proxy-light` rows and the whole nginx proxy comparison — is
contaminated by this stall and should be re-measured.**

### Finding B — the gateway's own HTTP/TLS path is fine; the proxy path is the gap

With the fixture corrected, the gateway's static path (its own request path, no
upstream) runs at 1580-3290 RPS depending on machine noise, while the proxy
path sits at ~535-860 RPS — roughly **4-5x slower than static** even though
proxying does strictly less TLS work. So the throughput gap vs nginx
(1549-1616 proxy on the same box) localises to the upstream-forwarding path,
not to the TLS/HTTP front end.

### Finding C — it is not CPU-bound, and not the fan-out / pool caps

CPU sampling (`/proc/<pid>/task/*/stat` deltas over the load window):

| scenario | RPS | CPU of one core | user | sys |
|---|---|---|---|---|
| static c50 | 3677 | 0.58 | 29.5% | 28.9% |
| proxy c50 | 733 | 0.31 | 15.0% | 15.9% |

The proxy path uses **0.31 core** at 733 RPS: it is waiting, not computing
(~50% of the CPU it does use is kernel time — syscalls, not a user-space hot
loop). Config A/B confirms the obvious knobs are not the limiter:

| change (proxy c50) | RPS |
|---|---|
| default (`upstream_max_inflight` 4) | 441-587 |
| `upstream_max_inflight` 64 | 515-658 |
| `upstream_max_inflight` 64 + `thread_pool` 2/10 → 16/64 | 667 |
| `num_workers` 1 → 2 | 604 → 855 |

Raising the fan-out and pool sizes buys ~15%; doubling the event-loop workers
buys ~40%. The path scales with event-loop threads and barely responds to
worker/thread-pool size, which points at the single forwarded-request
round-trip, not at a queue-depth limit.

> The rows above were measured **with the blocking loop still in place**, so the
> knobs looked inert. With the loop unblocked (the fix below) they do matter, and
> the `upstream_max_inflight` default has since been raised to 64; the "default
> 4" row describes the state at profiling time.

### Finding D — structural cause: the event loop blocks on the upstream round trip

`handle_read` runs on the TcpWorker event-loop thread and processes each
request inline (`http_handler.cpp`). For an upstream route it calls
`run_upstream_task`, which submits to `BoundedAsyncUpstreamExecutor` and then
**blocks the calling thread on `future.get()`** — i.e. it blocks the *event
loop* for the whole upstream round trip. One worker therefore serves proxied
requests strictly one at a time: throughput is ~`1/upstream_latency` and does
not grow with client concurrency. That is exactly the flat 441-667 RPS across
c8/c50, the latency rising as `concurrency x ~1.4 ms`, and the near-zero
response to `upstream_max_inflight`. The upstream socket itself is
non-blocking (`Socket::setnonblocking` plus `Poller::wait` in
`http_client.cpp`), so the cost is not a blocking syscall: it is one thread
pinned polling a single round trip while the loop stalls, and a second thread
(the executor) doing the I/O the loop is waiting on — two threads, no added
concurrency. `strace` shows the resulting churn under load (~83 `clone3`/
`set_robust_list` thread creations per 6 s as the dynamic pool scales up and
down, plus `madvise`/`mmap`/`munmap` for the stacks). `num_workers` raises
throughput only by multiplying blocked threads (1→2 gave +42%). nginx never
pays this: it registers the upstream socket in the same epoll and keeps
serving other clients while the upstream is in flight.

This is the leading explanation for the residual gap and the reason the knobs
in Finding C saturate early. Closing it is a design change (drive upstream
sockets from the event loop with an `eventfd` wake-up, instead of a blocking
worker-thread pool), not a tuning flag, so it is listed as follow-up rather
than applied here.

### Fix — the event loop no longer blocks on the upstream round trip (2026-10-13)

Finding D's minimal fix is applied. Previously `run_upstream_task` submitted the
backend call to `BoundedAsyncUpstreamExecutor` and then blocked the **event-loop
thread** on `future.get()`, so a worker served proxied requests strictly one at a
time. Now the loop hands the round trip to the executor and returns immediately:
each `HttpHandler` owns an `eventfd`; the worker pushes its finished
`AsyncUpstreamResult` under a mutex and bumps the eventfd, and the loop drains it
in `drain_async()` (dispatched by `TcpWorker` when the registered `async_fd`
becomes readable) to frame and send the response. Every per-connection map stays
single-threaded on the loop; the worker touches only the mutex-guarded result
vector and the eventfd, and snapshots `config_`/route fields by value so a
concurrent `SIGHUP` reload cannot dangle underneath it. While a round trip is in
flight the client fd is intentionally left disarmed (`EPOLLONESHOT` already
fired), so a pipelined second request cannot run concurrently with the first.

Measured on the same shared 2-vCPU box (1 worker, single mock backend, `wrk -t1`,
~4-6 s per run, repeated because the box is noisy — absolute numbers are not
portable, only the before/after ratio is):

| scenario | before (blocking loop) | after (async loop) |
|---|---|---|
| proxy c8 | 589 RPS, p50 9.3 ms, 0 err | ~820-1110 RPS, p50 ~8 ms |
| proxy c50 | 389 RPS, p50 126 ms, 0 err | ~680-1250 RPS |

Proxy throughput is no longer pinned to `1/upstream_latency`: it grows with client
concurrency, like the static path. At c50 the residual timeouts/non-2xx come from
the **single-threaded Python mock** saturating, not the gateway — the mock alone
reports the same timeouts at c50 (`wrk -c50 http://127.0.0.1:<mock>`), and several
of them cascade into the gateway's circuit breaker (503).

#### Side effect: the default fan-out budget had to be raised 4 -> 64

`BoundedAsyncUpstreamExecutor` caps each upstream *group* at
`max_group_inflight_ == upstream_max_inflight`, counting running **and** queued
tasks. With the old default of 4, unblocking the loop turned "slow under
concurrency" into "rejected under concurrency": a single upstream served at most
4 concurrent requests and answered `503 Upstream executor is overloaded` for the
rest (35% of responses at c8, 54% at c50). The default is now **64** so ordinary
concurrency is served instead of rejected, at the cost of up to 64 executor
threads per worker process (fixed at startup).

> Superseded on 2026-10-14: the single-upstream path no longer goes through the
> executor at all (see the next section), so `upstream_max_inflight` /
> `upstream_max_queue` now bound **composite** fan-out only. The 64 default is
> kept; it constrains composite dependency fan-out, not a single upstream.
>
> **Superseded again on 2026-10-18**: composite fan-out now runs on the same
> event-loop-driven `UpstreamExchange` state machine (see the composite entry in
> [CHANGELOG.md](CHANGELOG.md)), so `BoundedAsyncUpstreamExecutor` and the
> `upstream_max_inflight` / `upstream_max_queue` keys were **removed**. The
> "raised 4 -> 64" history above is retained as a record; neither key exists now.
> Fan-out is bounded by the connection/fd budget, like single-upstream forwarding.

#### Fixed: the upstream socket is registered in the same epoll (full Finding D, 2026-10-14)

The follow-up above is implemented. The single-upstream round trip is now a
non-blocking state machine (`UpstreamExchange`, `src/server/http_client.cpp`)
driven by the worker's own `epoll`. The upstream socket is registered with
`EPOLLONESHOT` and the loop advances it through connect / TLS handshake / send /
read on readiness, so **no executor thread is involved and there is no
per-request thread bound**; concurrency is bounded only by connection fds, not by
a thread count. Re-arming uses `del()` + `add()` rather than `mod()` (an internal
retry `close()`s the old fd, which the kernel auto-removes, and the new socket may
reuse the same fd number). Idle deadlines map to the route's Connect/Write/Read
timeouts (504 on expiry). The retry rules are unchanged: a pooled connection the
backend closed before send is retried once on a fresh connection for idempotent
methods only, and a request whose bytes reached the backend is never retried. The
`BoundedAsyncUpstreamExecutor` — and with it `upstream_max_inflight` /
`upstream_max_queue` — now serves composite dependency fan-out only.
**Removed on 2026-10-18:** composite fan-out is now driven by the same event-loop
`UpstreamExchange` state machine, so the executor and both keys were deleted; see
the composite entry in [CHANGELOG.md](CHANGELOG.md).

**Honest caveat — this box cannot measure the change.** The follow-up is justified
on structure (one fewer thread per round trip, all per-connection state on the
loop), not on a throughput win measured here. This is a 2-vCPU shared box with the
client, gateway and a single-threaded Python mock backend all on it; the mock
saturates at ~800 RPS on its own (`wrk` direct, no gateway), which is at or below
the numbers the gateway already reached, and the IDE background load sits around
2.3. `scripts/benchmark/run_benchmark.sh` runs in that state land inside the
mock's own noise band (e.g. proxy c50 ~328 RPS, proxy-light c8 ~553 RPS — *below*
the earlier eventfd rows, consistent with a busy box, not a regression signal). A
known trade-off: with `num_workers: 1` all upstream I/O now runs on the one loop
thread, whereas the eventfd path could spill I/O onto a second core; on a 2-core
box under external load that can lower throughput. The deliverables are the
removal of the thread bound and the single-threaded-per-connection model; a
defensible before/after needs a quieter, higher-core host.

### Reproducing this pass

```bash
./ci.sh build
# fixture-corrected proxy baseline; the box is noisy and the Python mock
# saturates at c50, so the strict harness may flag c50 proxy timeouts
# (environment, not a gateway error):
scripts/benchmark/run_benchmark.sh 8
# CPU split of a running gateway while wrk drives it:
#   sum /proc/<pid>/task/*/stat fields 14/15 before and after the load window
# thread-creation churn (start the gateway as a child, ptrace_scope=1):
#   strace -c -f -o /tmp/st.txt build/server   # run wrk, then SIGTERM the server
```

## Re-baseline after the event-driven rewrite (2026-10-19)

The structural cause in Finding D (event loop blocking per upstream round
trip) was fully removed on 2026-10-14/10-18, so the 2.5-2.7x gap below was a
pre-fix number. Same box, same methodology as the 2026-09-29 comparison —
4 local mock backends (ports 9001-9004, HTTP/1.1 keep-alive, TCP_NODELAY),
`GET /bench` small JSON, TLS terminated at the gateway, 2 workers each side,
access logging on both, wrk 4.1.0 `-t2` at c8/c50, 10 s per run, 3 interleaved
rounds, medians. nginx 1.24.0 (same version as the original comparison) was
installed from the Ubuntu deb into a user prefix — no `SO_REUSEPORT` stale
binary this time (`ss -tlnp` verified).

| target | conn | RPS (median) | P50 | P99 |
|---|---|---|---|---|
| direct (1 backend) | 8 | 1203 | 5.8ms | 26.6ms |
| direct (1 backend) | 50 | 1252 | 29.0ms | 1.18s |
| nginx (4 backends) | 8 | 1335 | 5.2ms | 25.4ms |
| nginx (4 backends) | 50 | 1453 | 29.4ms | 541ms |
| **epoll-gateway (4 backends)** | 8 | **655** | 11.6ms | 47.8ms |
| **epoll-gateway (4 backends)** | 50 | **673** | 75.2ms | 257.8ms |

RSS, sampled under simultaneous c50 load against both gateways (not idle —
not directly comparable to the idle-RSS table above): nginx master + 2
workers ≈ 28.3 MB; epoll-gateway main + 2 workers ≈ 44-48 MB (the sample
counted one extra pid; treat as order-of-magnitude). The gateway's steady
footprint grows with in-flight connection buffers; worth a dedicated idle-vs-
loaded pass before quoting.

### Findings

1. **The gap narrowed to ~2.0-2.2x** (c8: 1335/655, c50: 1453/673), from
   2.5-2.7x before the rewrite. The event-driven fix bought roughly 20-30%
   relative throughput, but the gateway still sits **below the single-backend
   direct reference** (655 vs 1203 at c8): the backend is not the bottleneck —
   the gateway's own request path is.
2. **Per the positioning decision fork, ≥2x after re-baseline means: profile
   before touching anything.** Candidates ranked by expected value for the
   *proxy* path (sendfile does not apply here — this benchmark is proxied):
   per-request allocations/copies (header re-serialization, body transit
   through `send_queues_`), synchronous AUDIT/CLF log writes on the hot path,
   TLS record sizing, allocator choice (jemalloc). Target: converge to ≤1.5x
   of nginx, then stop — beyond that the operations-surface positioning does
   not benefit.
3. **p99 noise caveat**: the gateway's c50 p99 (258ms) beat nginx's (541ms) in
   these runs, but both bounce heavily on the shared box — not a claimable
   win.

## Pending

- ~~Re-run the nginx proxy comparison with the corrected mock fixture~~ and
  ~~re-run it after the fully event-driven upstream change~~ — **done
  2026-10-19**, see the re-baseline section above (both stale rows replaced).
- ~~**Profile before optimizing** (new, from the re-baseline fork)~~ — **profiled
  2026-10-19**, see the optimization-sprint section below; the quantitative
  ≤1.5x stop-rule check is still pending a quiet benchmark window.
- **Idle-vs-loaded RSS pass** — the 2026-10-19 RSS sample was taken under
  load and counted one extra pid; re-measure cleanly before quoting either
  number.
- Per-request log-write share via `logging.access_log_enabled` (access log can
  be turned off independently of AUDIT); not yet isolated with data.
- Traefik / KrakenD comparison (binary download blocked on this network).

---

# Profile-driven hot-path optimization sprint (2026-10-19)

A `perf record -F 999 -g -p <worker>` capture under the c50 proxy load
(12,989 samples) attributed the request path's CPU and fixed latency costs.
Findings and the bounded changes made in response:

| profile evidence | change |
|---|---|
| `handle_write` 22% children: every response issued **two `SSL_write` calls** (header chunk + body chunk = two TLS records, two syscalls) | header + body coalesced into one send chunk for bodies ≤ 64 KB ([http_handler.cpp](../src/server/http_handler.cpp) `send_response`) |
| `malloc`/`_int_free` ≈ 3.5% + `std::tolower` 0.77% + `getline` 0.67%: the per-response header filter allocated a lowercase copy of **every** upstream header name and parsed `Connection` via `istringstream`; `headers_to_string`, `generate_trace_id`, `generate_traceparent` used `ostringstream` | case-insensitive ASCII compare against literals, one reusable lowercase buffer, manual comma split; string building with reserve; `snprintf` for trace IDs |
| `pthread_cond_signal`/futex + `__GI___write` cluster under spdlog; **`flush_on(info)` meant an `fflush`+write syscall twice per request** (AUDIT + CLF are both info) | flush policy `flush_on(err)` + `spdlog::flush_every(1s)` ([mylogger.cpp](../src/common/mylogger.cpp)) |

TLS itself (~21% children in `SSL_write`+`SSL_read`) and kernel network time
(~17% in unsymbolized `[k]` frames) are paid by nginx equally and were left
alone, as was jemalloc (not installed on the box; the project is zero-dependency).

## Validation status: functional yes, quantitative pending

- Unit 137/137, integration 155/155 after all changes.
- **The planned ≤1.5x-of-nginx stop-rule check did not produce a trustworthy
  number this sprint.** The shared 2-vCPU box was contended by background IDE
  tooling (steady ~30% CPU before the sprint; transient load spikes 6-10
  during runs). The *same binary* measured 414-1015 RPS at c8 across windows;
  interleaved old-vs-new rounds flipped sign with background load. Absolute
  numbers from this window must not be compared with the 2026-09-29 baseline.
- What remains valid: the profile attributions (taken at load 3-4, consistent
  with the earlier 43 ms / Nagle findings), the removal of the per-request
  double TLS record and double flush — both are structural per-request costs,
  not statistical artifacts.
- Re-run `run_matrix.sh` (same methodology) in a quiet window
  (`/proc/loadavg` 1-min < 1.5 sustained) to get the stop-rule number.

## Measurement trap (new): self-induced benchmark noise

On a 2-vCPU box, the development IDE's own agent processes and the file
watcher triggered by large rebuilds (a `-g` build emits a 34 MB binary plus
hundreds of objects) can consume more CPU than the system under test. Signs:
`vmstat 1` shows 20%+ sys time with no benchmark running; same-binary RPS
varies >2x between minutes. Run `vmstat 1` alongside any benchmark and treat
numbers from contended windows as invalid.
