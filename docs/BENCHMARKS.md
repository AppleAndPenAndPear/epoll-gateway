# Benchmark Report: epoll-gateway (wrk baseline)

Baseline throughput and latency of the gateway, measured with
[wrk](https://github.com/wg/wrk) via [scripts/benchmark/run_benchmark.sh](../scripts/benchmark/run_benchmark.sh).
All numbers below are reproducible on any 2+ core Linux box; absolute values
matter less than the ratios between configurations.

## Environment

| item | value |
|---|---|
| CPU | 2 vCPU (client, gateway, and mock backend share the same box) |
| OS | Linux (Debian) |
| TLS | OpenSSL server, min TLS 1.2, ECDHE + AEAD cipher whitelist |
| Gateway | 1 worker process, epoll (edge-triggered, one-shot), default thread pool |
| Client | wrk 4.1.0, 1 thread, warmup 3s + measured 15s per scenario |
| Backend | Python `ThreadingHTTPServer` mock returning a small JSON body |

## Scenarios

1. **static** — small HTML file served from `www_root` (response cache hit),
   50 keep-alive connections (saturation load).
2. **proxy** — reverse proxy to the local mock backend over a fresh TCP
   connection per request, 50 keep-alive connections (saturation load).
3. **static-light / proxy-light** — same scenarios at 8 connections (light
   load, measures per-request latency rather than max throughput).
4. **handshake** — static file with `Connection: close`, i.e. a full TLS
   handshake for every request, 10 connections.

## Results

| scenario | QPS | P50 | P99 |
|---|---|---|---|
| static (c50) | 994.00 | 44.23ms | 291.42ms |
| proxy (c50) | 1083.41 | 42.22ms | 112.63ms |
| static-light (c8) | 948.61 | 7.99ms | 25.07ms |
| proxy-light (c8) | 999.95 | 7.54ms | 22.49ms |
| handshake (c10) | 956.17 | 10.09ms | 27.38ms |

## Finding: TCP_NODELAY (the 40 ms delayed-ACK stall)

The first benchmark run exposed a classic TCP pathology: **every request paid
a fixed ~43 ms latency floor**, even at light load (P50 43ms at 8 connections).
QPS at light load was stuck around 170, and TLS handshakes around 220/s.

Root cause: accepted sockets (and upstream sockets) did not set
`TCP_NODELAY`. The server writes responses in several small TLS records
(header record + body record, handshake flights), so with Nagle's algorithm
enabled the second small write stalled on the peer's 40 ms delayed-ACK timer
on every request.

After enabling `TCP_NODELAY` on client and upstream sockets
(`Socket::setnodelay()` in `src/common/mysocket.cpp`, called from
`tcpworker.cpp` and `http_client.cpp`):

| scenario | before QPS | after QPS | before P50 | after P50 |
|---|---|---|---|---|
| static-light (c8) | 176 | 949 | 43.04ms | 7.99ms |
| proxy-light (c8) | 169 | 1000 | 43.40ms | 7.54ms |
| handshake (c10) | 224 | 956 | 42.79ms | 10.09ms |
| proxy (c50) | 836 | 1083 | 53.64ms | 42.22ms |

**Light-load latency improved ~5.4x, handshake throughput ~4.3x.**

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

Throughput impact on this box is within noise (proxy-light 965 vs 1000 QPS
before the pool, proxy c50 1098 vs 1083): on loopback a TCP connect costs
tens of microseconds, so with the client, gateway, and Python backend all
sharing 2 CPU cores the bottleneck remains CPU, not connections. The win
scales with backend distance — for a remote backend one connect round-trip
(RTT) per request is removed, which on any real network dominates the
request budget.

## Interpretation

- At 50 connections the single worker saturates its CPU share (~1000 QPS
  combined with wrk and the Python mock on 2 cores); P50/P99 at that point
  reflect queueing behind a saturated event loop, not per-request cost.
- Per-request cost at light load is now ~8 ms for TLS-served traffic on this
  small box, dominated by symmetric TLS crypto and epoll overhead.
- The proxy rows above were measured **before** the upstream connection pool
  (P3); the pool's own finding below records the comparison (throughput within
  noise on loopback). Functionally, 10 proxied requests now open ≤4 backend
  connections instead of 10, and the win grows with backend network distance.
- The handshake scenario shows the gateway sustains ~956 full TLS handshakes
  per second even while sharing 2 cores with the client — session resumption
  (session cache + tickets, P2) is active for repeat connections.

## Reproducing

```bash
./ci.sh build
scripts/benchmark/run_benchmark.sh          # 15s per scenario
scripts/benchmark/run_benchmark.sh 30       # longer runs for smoother numbers
```

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
| epoll-gateway | release build, 2 workers, same cert, access+audit logging on (not configurable off yet) |
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
- A config option to disable per-request access/audit logging (fairness gap
  vs `access_log off`; also a real feature request).
