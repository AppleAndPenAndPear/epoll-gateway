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
THREADS="${BENCH_THREADS:-1}"
CONNS="${BENCH_CONNS:-50}"
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
      "auth_required": false, "allow_anonymous": true,
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
  if python3 -c "import socket; socket.create_connection(('127.0.0.1', $BACKEND_PORT), timeout=1).close()" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
python3 -c "import socket; socket.create_connection(('127.0.0.1', $BACKEND_PORT), timeout=1).close()" || {
  echo "mock backend failed to start" >&2; exit 1;
}

for _ in $(seq 1 60); do
  if python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" || {
  echo "gateway failed to start; log:" >&2; cat "$WS/server.log" >&2; exit 1;
}

if ! python3 - "$PORT" <<'PY'
import http.client
import ssl
import sys

port = int(sys.argv[1])
context = ssl._create_unverified_context()
for path in ("/", "/api/two/benchmark"):
    conn = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=5)
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        response.read()
        print("preflight %s -> %s" % (path, response.status), file=sys.stderr)
        if response.status != 200:
            sys.exit(1)
    finally:
        conn.close()
PY
then
  echo "benchmark preflight failed; gateway log:" >&2
  cat "$WS/server.log" >&2
  exit 1
fi

# ─── Run wrk: warmup + measured, per scenario ───
run_wrk() { # $1 label, $2 conns, $3 path, extra args...
  local label="$1" conns="$2" path="$3"; shift 3
  # warmup (discarded)
  wrk -t"$THREADS" -c"$conns" -d3s "$@" "https://127.0.0.1:$PORT$path" >/dev/null 2>&1 || true
  echo "### $label" >&2
  wrk -t"$THREADS" -c"$conns" -d"${DURATION}s" --latency "$@" "https://127.0.0.1:$PORT$path" > "$WS/$label.out"
  tail -n 14 "$WS/$label.out" >&2
  if grep -Eq 'Non-2xx or 3xx responses:|Socket errors:' "$WS/$label.out" ||
     ! grep -Eq 'Requests/sec:[[:space:]]+[1-9]' "$WS/$label.out"; then
    echo "benchmark scenario '$label' had request errors or completed no requests; gateway log:" >&2
    cat "$WS/server.log" >&2
    return 1
  fi
}

run_wrk static "$CONNS" /
run_wrk proxy "$CONNS" /api/two/benchmark
run_wrk static_light 8 /
run_wrk proxy_light 8 /api/two/benchmark
run_wrk handshake 10 / -H 'Connection: close'

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
