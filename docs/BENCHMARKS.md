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

## Interpretation

- At 50 connections the single worker saturates its CPU share (~1000 QPS
  combined with wrk and the Python mock on 2 cores); P50/P99 at that point
  reflect queueing behind a saturated event loop, not per-request cost.
- Per-request cost at light load is now ~8 ms for TLS-served traffic on this
  small box, dominated by symmetric TLS crypto and epoll overhead.
- The proxy scenario still opens one TCP connection to the backend per
  request (three-way handshake each time). The planned **upstream connection
  pool** (P3) removes that cost and is expected to be the next biggest win.
- The handshake scenario shows the gateway sustains ~956 full TLS handshakes
  per second even while sharing 2 cores with the client — session resumption
  (session cache + tickets, P2) is active for repeat connections.

## Reproducing

```bash
./ci.sh build
scripts/benchmark/run_benchmark.sh          # 15s per scenario
scripts/benchmark/run_benchmark.sh 30       # longer runs for smoother numbers
```
