#!/usr/bin/env bash
# wrk baseline benchmark for the epoll gateway (P3).
#
# Scenarios (all HTTPS, self-signed repo certs):
#   1. static     — small file from www_root (response cache hit)
#   2. proxy      — reverse proxy to a local mock backend (keep-alive)
#   3. handshake  — TLS handshake + request per request (Connection: close)
#
# Usage: scripts/benchmark/run_benchmark.sh [duration_seconds]
# Prints a markdown table with QPS and P50/P99 latency.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER_BIN="$REPO/build/server"
PORT=5607
BACKEND_PORT=5613
THREADS=1
CONNS=50
DURATION="${1:-15}"

[ -x "$SERVER_BIN" ] || { echo "server not built: $SERVER_BIN (run ./ci.sh build)" >&2; exit 1; }
command -v wrk >/dev/null || { echo "wrk is not installed" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is not installed" >&2; exit 1; }

WS="$(mktemp -d /tmp/gw-bench-XXXXXX)"
SERVER_PID=""
MOCK_PID=""
cleanup() {
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
  [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
  rm -rf "$WS"
}
trap cleanup EXIT

# ─── Workspace: certs, static file, config ───
ln -s "$REPO/certs" "$WS/certs"
mkdir "$WS/www"
printf '<html><body>BENCHMARK-INDEX</body></html>\n' > "$WS/www/index.html"

cat > "$WS/config.json" <<EOF
{
  "port": $PORT,
  "num_workers": 1,
  "www_root": "www",
  "upstreams": {
    "bench": {
      "servers": [{"host": "127.0.0.1", "port": $BACKEND_PORT}],
      "algorithm": "round_robin"
    }
  },
  "routes": [
    {"name": "bench-proxy", "method": "GET", "path": "/api/two/*",
     "target_type": "upstream",
     "upstream_target": {"name": "bench", "timeout_ms": 3000, "max_retries": 0}}
  ],
  "rate_limit": {"capacity": 100000000, "refill_per_second": 50000000}
}
EOF

# ─── Start mock backend + gateway ───
python3 "$REPO/tests/integration/mock_backend.py" "$BACKEND_PORT" >/dev/null 2>&1 &
MOCK_PID=$!
( cd "$WS" && exec "$SERVER_BIN" ) > "$WS/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 60); do
  if python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" || {
  echo "gateway failed to start; log:" >&2; cat "$WS/server.log" >&2; exit 1;
}

# ─── Run wrk: warmup + measured, per scenario ───
run_wrk() { # $1 label, $2 conns, extra args...
  local label="$1" conns="$2"; shift 2
  # warmup (discarded)
  wrk -t"$THREADS" -c"$conns" -d3s "$@" "https://127.0.0.1:$PORT/" >/dev/null 2>&1 || true
  echo "### $label" >&2
  wrk -t"$THREADS" -c"$conns" -d"${DURATION}s" --latency "$@" "https://127.0.0.1:$PORT/" > "$WS/$label.out"
  tail -n 14 "$WS/$label.out" >&2
}

run_wrk static "$CONNS"
run_wrk proxy "$CONNS"
run_wrk static_light 8
run_wrk proxy_light 8
run_wrk handshake 10 -H 'Connection: close'

# ─── Summarize into a markdown table ───
summarize() { # $1 label, $2 out-file
  awk -v label="$1" '
    /Requests\/sec/ { qps = $2 }
    /Latency Distribution/ { inlat = 1 }
    inlat && /^ *50%/ { p50 = $2 }
    inlat && /^ *99%/ { p99 = $2; inlat = 0 }
    END { printf "| %s | %.2f | %s | %s |\n", label, qps, p50, p99 }
  ' "$2"
}

echo
echo "## Results"
echo
echo "| scenario | QPS | P50 | P99 |"
echo "|---|---|---|---|"
summarize static "$WS/static.out"
summarize proxy "$WS/proxy.out"
summarize static-light-c8 "$WS/static_light.out"
summarize proxy-light-c8 "$WS/proxy_light.out"
summarize handshake-c10 "$WS/handshake.out"
