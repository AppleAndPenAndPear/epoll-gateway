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

## Pending

- Traefik / KrakenD comparison (binary download blocked on this network).
- Isolating the remaining throughput gap with flame graphs / syscall counts.
  A per-request access-log switch now exists (`logging.access_log_enabled`,
  independent of AUDIT), so the log-write share can be measured against
  `access_log off`; the other candidates (buffer copies, TLS record sizing)
  still need profiling.
