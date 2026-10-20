#!/usr/bin/env bash
# Clean-window comparison harness: epoll-gateway vs nginx vs direct backend.
#
# Why this script exists (2026-10-19/20 findings):
#   * On a shared 2-vCPU box the gateway was the only stable target (+/-3% across
#     rounds) while nginx and direct swung +/-30-90%, so a single-pass comparison
#     could not separate a real gap from host noise.
#   * Three stray gateway instances once stayed SO_REUSEPORT-bound to 8443 and
#     silently served half the requests, polluting every round.
#   * The development IDE's own processes (ai-agent/node/ckg_server) can consume
#     more CPU than the system under test.
# So this harness (1) fails fast when the window is contaminated, (2) pins each
# role to its own core when the box has them, and (3) runs an A/A calibration of
# the SAME target first: if two identical passes disagree beyond AA_TOL_PCT, the
# host is declared unmeasurable for this window and no A/B ratio is reported.
#
# Usage: scripts/benchmark/run_comparison.sh [mode] [duration]
#   mode      aa | ab | both     (default: both)
#   duration  measured seconds per run (default: 10)
#
# Env knobs:
#   ROUNDS=3              interleaved A/B rounds (medians are reported)
#   AA_ROUNDS=2           identical passes used for the A/A calibration
#   AA_TOL_PCT=10         max RPS spread between A/A passes before refusing A/B
#   BENCH_RENICE=1        renice local dev-IDE helper processes for the window
#   NGINX_BIN=<path>      nginx binary; if missing the nginx rows are skipped
#   CORES_BACKEND / CORES_GATEWAY / CORES_LOAD   taskset -c masks
#
# c50 numbers are backend-bound on this fixture (the Python mock saturates
# first). Only c8 tails are quotable; the script marks c50 as such.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER_BIN="$REPO/build/server"
NGINX_BIN="${NGINX_BIN:-/home/liu/nginx-1.24/root/usr/sbin/nginx}"

MODE="${1:-both}"
DURATION="${2:-10}"
ROUNDS="${ROUNDS:-3}"
AA_ROUNDS="${AA_ROUNDS:-2}"
AA_TOL_PCT="${AA_TOL_PCT:-10}"
BENCH_RENICE="${BENCH_RENICE:-1}"

GW_PORT=8443
NGINX_PORT=8444
BACKEND_PORTS=(9001 9002 9003 9004)
DIRECT_PORT=9001
WRK_THREADS=2

# ─── Core assignment ───
# Roles: 4 Python mock backends, 2 gateway workers, 2 nginx workers, wrk.
# The Python backends share one aggregate ~2000 RPS per CORE regardless of the
# process count, so they get TWO cores to keep the fixture cap well above both
# gateways; the gateways are then measured symmetrically on one core each.
nproc_now="$(nproc)"
if [ "$nproc_now" -ge 4 ]; then
  CORES_BACKEND="${CORES_BACKEND:-0-1}"
  CORES_GATEWAY="${CORES_GATEWAY:-2}"
  CORES_LOAD="${CORES_LOAD:-3}"
elif [ "$nproc_now" -eq 2 ]; then
  # Degraded split: gateway on one core, backends + load generator on the other.
  CORES_BACKEND="${CORES_BACKEND:-1}"
  CORES_GATEWAY="${CORES_GATEWAY:-0}"
  CORES_LOAD="${CORES_LOAD:-1}"
else
  CORES_BACKEND="${CORES_BACKEND:-0}"
  CORES_GATEWAY="${CORES_GATEWAY:-0}"
  CORES_LOAD="${CORES_LOAD:-0}"
fi

if [ ! -x "$SERVER_BIN" ]; then
  echo "server not built: $SERVER_BIN (run ./ci.sh build)" >&2
  exit 1
fi
command -v wrk >/dev/null || { echo "wrk is not installed" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is not installed" >&2; exit 1; }
command -v taskset >/dev/null || { echo "taskset is not installed" >&2; exit 1; }

HAVE_NGINX=0
[ -x "$NGINX_BIN" ] && HAVE_NGINX=1

OUT="$(mktemp -d /tmp/gw-compare-XXXXXX)"
WS="$OUT/fixture"
mkdir -p "$WS/www" "$WS/logs" "$WS/nginx/tmp/client_body" "$WS/nginx/tmp/proxy" \
         "$WS/nginx/tmp/fastcgi" "$WS/nginx/tmp/uwsgi" "$WS/nginx/tmp/scgi"
ln -s "$REPO/certs" "$WS/certs"
printf '<html><body>BENCHMARK-INDEX</body></html>\n' > "$WS/www/index.html"

PIDS=()
NICED=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done
  sleep 0.3
  for pid in "${PIDS[@]:-}"; do [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true; done
  for entry in "${NICED[@]:-}"; do
    [ -n "$entry" ] && renice -n "${entry%%:*}" -p "${entry##*:}" >/dev/null 2>&1 || true
  done
  echo "raw wrk output: $OUT"
}
trap cleanup EXIT

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

# ─── Gate 1: no residual listeners on the benchmark ports ───
gate_ports() {
  local bad=""
  for p in "$GW_PORT" "$NGINX_PORT" "${BACKEND_PORTS[@]}" 8105; do
    if ss -tln 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$p\$"; then
      bad="$bad $p"
    fi
  done
  if [ -n "$bad" ]; then
    echo "GATE FAILED: port(s) already listening:$bad" >&2
    echo "A stale instance bound to the same port (SO_REUSEPORT) silently serves" >&2
    echo "half the requests. Identify and stop it before measuring:" >&2
    ss -tlnp 2>/dev/null | grep -E ":(8443|8444|9001|9002|9003|9004|8105)\b" >&2 || true
    return 1
  fi
}

# ─── Gate 2: the box is idle ───
# loadavg has a 1-minute decay constant, so a build or index burst that just
# ended keeps it high for a while even after the box is quiet again. Poll for up
# to GATE_WAIT seconds instead of failing on the first read.
GATE_WAIT="${GATE_WAIT:-180}"
LOAD_LIMIT="${LOAD_LIMIT:-1.0}"
gate_load() {
  local waited=0 l1
  while :; do
    read -r l1 _ < /proc/loadavg
    if awk -v l="$l1" -v m="$LOAD_LIMIT" 'BEGIN { exit (l < m) ? 0 : 1 }'; then
      log "loadavg(1m)=$l1 (< $LOAD_LIMIT): window is idle"
      return 0
    fi
    if [ "$waited" -ge "$GATE_WAIT" ]; then
      echo "GATE FAILED: /proc/loadavg 1-min still $l1 (>= $LOAD_LIMIT) after ${GATE_WAIT}s" >&2
      echo "Something is actively burning CPU; find it before measuring:" >&2
      ps -eo pcpu,pid,comm --sort=-pcpu 2>/dev/null | head -8 >&2 || true
      return 1
    fi
    log "loadavg(1m)=$l1 (>= $LOAD_LIMIT), waiting for the box to settle..."
    sleep 5
    waited=$((waited + 5))
  done
}

# ─── Gate 3: quiet the local dev-IDE helpers (cannot be killed) ───
renice_helpers() {
  [ "$BENCH_RENICE" = "1" ] || return 0
  local names=(ai-agent ckg_server node trae-sandbox)
  for name in "${names[@]}"; do
    while read -r pid; do
      [ -n "$pid" ] || continue
      local orig
      orig="$(awk '{print $19}' "/proc/$pid/stat" 2>/dev/null || echo "")"
      [ -n "$orig" ] && NICED+=("$orig:$pid")
      renice -n 19 -p "$pid" >/dev/null 2>&1 || true
    done < <(pgrep -x "$name" 2>/dev/null || true)
  done
  log "renice'd ${#NICED[@]} dev-IDE helper process(es) to 19 for this window"
}

# ─── Fixture: 4 mock backends + gateway (+ nginx) ───
start_backends() {
  for p in "${BACKEND_PORTS[@]}"; do
    taskset -c "$CORES_BACKEND" python3 "$REPO/tests/integration/mock_backend.py" "$p" \
      > "$OUT/backend-$p.log" 2>&1 &
    PIDS+=($!)
  done
}

wait_port() { # $1 port, $2 tls(bool)
  local port="$1" tls="$2"
  for _ in $(seq 1 60); do
    if python3 - "$port" "$tls" <<'PY' 2>/dev/null
import socket, ssl, sys
port = int(sys.argv[1]); tls = sys.argv[2] == "1"
s = socket.create_connection(("127.0.0.1", port), timeout=1)
if tls:
    ctx = ssl._create_unverified_context()
    ctx.wrap_socket(s, server_hostname="127.0.0.1").close()
else:
    s.close()
PY
    then return 0; fi
    sleep 0.5
  done
  return 1
}

write_gateway_config() {
  cat > "$WS/config.json" <<EOF
{
  "port": $GW_PORT,
  "backlog": 2048,
  "num_workers": 2,
  "www_root": "www",
  "keepalive_timeout": 5,
  "tls": {"cert_path": "certs/server.crt", "key_path": "certs/server.key"},
  "logging": {"access_log_enabled": true},
  "rate_limit": {"capacity": 100000000, "refill_per_second": 50000000},
  "upstreams": {
    "bench": {
      "servers": [
        {"host": "127.0.0.1", "port": 9001},
        {"host": "127.0.0.1", "port": 9002},
        {"host": "127.0.0.1", "port": 9003},
        {"host": "127.0.0.1", "port": 9004}
      ],
      "algorithm": "round_robin"
    }
  },
  "routes": [
    {"name": "bench", "method": "GET", "path": "/bench",
     "target_type": "upstream", "auth_required": false, "allow_anonymous": true,
     "upstream_target": {"name": "bench", "timeout_ms": 10000, "max_retries": 0}}
  ]
}
EOF
}

write_nginx_config() {
  cat > "$WS/nginx/nginx.conf" <<EOF
daemon off;
worker_processes 2;
error_log $WS/nginx/error.log warn;
pid $WS/nginx/nginx.pid;

events { worker_connections 8192; }

http {
    access_log $WS/nginx/access.log;
    # The sandbox forbids writes under /var/lib/nginx, so every temp path is
    # relocated into the workspace.
    client_body_temp_path $WS/nginx/tmp/client_body;
    proxy_temp_path $WS/nginx/tmp/proxy;
    fastcgi_temp_path $WS/nginx/tmp/fastcgi;
    uwsgi_temp_path $WS/nginx/tmp/uwsgi;
    scgi_temp_path $WS/nginx/tmp/scgi;
    upstream bench {
        server 127.0.0.1:9001;
        server 127.0.0.1:9002;
        server 127.0.0.1:9003;
        server 127.0.0.1:9004;
        keepalive 16;
    }
    server {
        listen $NGINX_PORT ssl;
        ssl_certificate $WS/certs/server.crt;
        ssl_certificate_key $WS/certs/server.key;
        ssl_protocols TLSv1.2 TLSv1.3;
        location /bench {
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_pass http://bench;
        }
    }
}
EOF
}

preflight() { # $1 url
  python3 - "$1" <<'PY' || { echo "preflight failed for $1" >&2; return 1; }
import http.client, ssl, sys, urllib.parse
url = urllib.parse.urlparse(sys.argv[1])
if url.scheme == "https":
    conn = http.client.HTTPSConnection(url.netloc, context=ssl._create_unverified_context(), timeout=5)
else:
    conn = http.client.HTTPConnection(url.netloc, timeout=5)
try:
    conn.request("GET", url.path)
    r = conn.getresponse(); r.read()
    if r.status != 200:
        print("preflight %s -> %s" % (sys.argv[1], r.status), file=sys.stderr); sys.exit(1)
finally:
    conn.close()
PY
}

url_for() { # $1 target
  case "$1" in
    gateway) echo "https://127.0.0.1:$GW_PORT/bench" ;;
    nginx)   echo "https://127.0.0.1:$NGINX_PORT/bench" ;;
    direct)  echo "http://127.0.0.1:$DIRECT_PORT/bench" ;;
    *) echo "unknown target $1" >&2; return 1 ;;
  esac
}

# ─── One measured wrk run; appends "target conn round rps p50 p99" to raw.tsv ───
# A failed cell is retried once and then recorded as FAIL — a single bad cell
# (e.g. a c50 timeout burst on the saturated mock) must not abort the whole
# matrix, it only costs that cell's sample.
raw_tsv="$OUT/raw.tsv"
run_one() { # $1 target, $2 conns, $3 round-label
  local target="$1" conns="$2" round="$3" attempt out url f
  url="$(url_for "$target")"

  for attempt in 1 2; do
    out="$OUT/${target}-c${conns}-${round}${attempt:+-$attempt}.out"
    taskset -c "$CORES_LOAD" wrk -t"$WRK_THREADS" -c"$conns" -d3s "$url" >/dev/null 2>&1 || true
    taskset -c "$CORES_LOAD" wrk -t"$WRK_THREADS" -c"$conns" -d"${DURATION}s" --latency "$url" > "$out" 2>&1 || true

    if ! grep -Eq 'Non-2xx or 3xx responses:|Socket errors:' "$out" &&
       grep -Eq 'Requests/sec:[[:space:]]+[1-9]' "$out"; then
      f="$(awk '
        /Requests\/sec/ { rps = $2 }
        /Latency Distribution/ { inlat = 1 }
        inlat && /^ *50%/ { p50 = $2 }
        inlat && /^ *99%/ { p99 = $2; inlat = 0 }
        END { printf "%s %s %s", rps, p50, p99 }
      ' "$out")"
      echo "$target c$conns $round $f" >> "$raw_tsv"
      log "$target c$conns $round: $f"
      return 0
    fi
    log "$target c$conns $round attempt $attempt failed, retrying..."
  done

  echo "RUN FAILED: $target c$conns $round failed twice; cell recorded as FAIL, matrix continues" >&2
  echo "$target c$conns $round FAIL - -" >> "$raw_tsv"
  return 0
}

median() { sort -n | awk '{a[NR]=$1} END { if (NR==0) {print "NA"; exit}
  if (NR%2) printf "%.2f", a[(NR+1)/2]; else printf "%.2f", (a[NR/2]+a[NR/2+1])/2 }'; }
spread_pct() { sort -n | awk '{a[NR]=$1} END { if (NR<2 || a[1]<=0) {print "0"; exit}
  printf "%.1f", (a[NR]-a[1])*100.0/a[1] }'; }
col() { awk -v t="$1" -v c="$2" -v i="$3" '$1==t && $2=="c"c && $3!="FAIL" {print $i}' "$raw_tsv"; }

# ─── A/A calibration: identical passes must agree ───
aa_calibrate() {
  local target="gateway" conns=8
  log "A/A calibration: $target at c$conns, $AA_ROUNDS identical passes"
  for r in $(seq 1 "$AA_ROUNDS"); do
    run_one "$target" "$conns" "aa$r"
  done
  local spread
  spread="$(col "$target" "$conns" 4 | tail -n "$AA_ROUNDS" | spread_pct)"
  local med
  med="$(col "$target" "$conns" 4 | tail -n "$AA_ROUNDS" | median)"
  echo
  echo "### A/A calibration (same binary, same target)"
  echo
  echo "- passes: $(col "$target" "$conns" 4 | tail -n "$AA_ROUNDS" | tr '\n' ' ')"
  echo "- median RPS: $med, spread: ${spread}% (tolerance ${AA_TOL_PCT}%)"
  if awk -v s="$spread" -v t="$AA_TOL_PCT" 'BEGIN { exit (s <= t) ? 0 : 1 }'; then
    echo "- verdict: PASSED — the host is stable enough for an A/B in this window"
    return 0
  fi
  echo "- verdict: FAILED — the host is not measurable in this window; no A/B ratio is reported"
  echo "  (rise the A/A spread means host noise, not a gateway regression; re-run when quieter)"
  return 1
}

# ─── A/B: interleaved rounds, medians ───
run_ab() {
  local targets=(gateway direct)
  [ "$HAVE_NGINX" = "1" ] && targets=(gateway nginx direct)
  local conns=(8 50)
  log "A/B: ${targets[*]} at c8/c50, $ROUNDS interleaved rounds, ${DURATION}s each"
  for r in $(seq 1 "$ROUNDS"); do
    for t in "${targets[@]}"; do
      for c in "${conns[@]}"; do
        run_one "$t" "$c" "r$r"
      done
    done
  done

  echo
  echo "### A/B results (medians of $ROUNDS interleaved rounds, min-max spread in row notes)"
  echo
  echo "| target | conn | RPS (median) | P50 | P99 | RPS min–max |"
  echo "|---|---|---|---|---|---|"
  local gw8="" ng8=""
  for t in "${targets[@]}"; do
    for c in "${conns[@]}"; do
      local rps p50 p99 rmin rmax
      rps="$(col "$t" "$c" 4 | median)"
      p50="$(col "$t" "$c" 5 | median)"
      p99="$(col "$t" "$c" 6 | median)"
      rmin="$(col "$t" "$c" 4 | sort -n | head -1)"
      rmax="$(col "$t" "$c" 4 | sort -n | tail -1)"
      echo "| $t | c$c | $rps | $p50 | $p99 | $rmin – $rmax |"
      [ "$t" = "gateway" ] && [ "$c" = "8" ] && gw8="$rps"
      [ "$t" = "nginx" ] && [ "$c" = "8" ] && ng8="$rps"
    done
  done
  echo
  if [ "$HAVE_NGINX" = "1" ] && [ -n "$gw8" ] && [ -n "$ng8" ]; then
    awk -v g="$gw8" -v n="$ng8" 'BEGIN {
      printf "- gw:nginx at c8 = %.2fx (nginx %.0f / gateway %.0f RPS)\n", n/g, n, g
    }'
    awk -v g="$gw8" 'BEGIN { printf "- c50 rows are backend-bound on this fixture; only c8 tails are quotable\n" }'
  fi
  echo
  echo "### Fixture ceiling check (backend must NOT be the bottleneck)"
  echo
  local d8 d50 dspread
  d8="$(col direct 8 4 | median)"; d50="$(col direct 50 4 | median)"
  dspread="$(col direct 8 4 | spread_pct)"
  echo "- direct c8: $d8 RPS (min–max spread ${dspread}%), c50: $d50 RPS"
  if awk -v s="$dspread" 'BEGIN { exit (s <= 25) ? 0 : 1 }'; then
    echo "- direct-row spread ${dspread}% ≤ 25%: the fixture/load generator behaved consistently; medians are usable"
  else
    echo "- **direct-row spread ${dspread}% > 25%: the fixture itself is unstable this window — treat ALL medians as suspect**"
  fi
  if [ -n "$ng8" ] && awk -v d="$d8" -v n="$ng8" 'BEGIN { exit (d >= 1.4*n) ? 0 : 1 }'; then
    echo "- c8: direct >= 1.4x nginx — fixture has headroom, c8 rows are usable"
  else
    echo "- **c8: direct is NOT comfortably above nginx — the fixture may cap the comparison; treat c8 rows as suspect**"
  fi
  echo "- c50: the shared Python mock saturates before either gateway; do not quote c50 tails"
  return 0
}

# ─── Main ───
gate_ports
renice_helpers
gate_load
write_gateway_config
start_backends

for p in "${BACKEND_PORTS[@]}"; do
  wait_port "$p" 0 || { echo "mock backend on $p failed to start" >&2; exit 1; }
done
preflight "http://127.0.0.1:$DIRECT_PORT/bench" >/dev/null || { echo "direct preflight failed" >&2; exit 1; }

( cd "$WS" && exec taskset -c "$CORES_GATEWAY" "$SERVER_BIN" ) > "$OUT/gateway.log" 2>&1 &
PIDS+=($!)
wait_port "$GW_PORT" 1 || { echo "gateway failed to start; log:" >&2; cat "$OUT/gateway.log" >&2; exit 1; }
preflight "https://127.0.0.1:$GW_PORT/bench" >/dev/null || { echo "gateway preflight failed; log:" >&2; cat "$OUT/gateway.log" >&2; exit 1; }

if [ "$HAVE_NGINX" = "1" ]; then
  write_nginx_config
  taskset -c "$CORES_GATEWAY" "$NGINX_BIN" -p "$WS/nginx" -c "$WS/nginx/nginx.conf" > "$OUT/nginx.log" 2>&1 &
  PIDS+=($!)
  if wait_port "$NGINX_PORT" 1 && preflight "https://127.0.0.1:$NGINX_PORT/bench" >/dev/null; then
    log "nginx $("$NGINX_BIN" -v 2>&1) up on $NGINX_PORT"
  else
    echo "nginx failed to start; rows skipped" >&2
    cat "$OUT/nginx.log" >&2
    HAVE_NGINX=0
  fi
else
  echo "note: nginx not found at $NGINX_BIN (set NGINX_BIN=...); nginx rows skipped" >&2
fi

log "cores: backend=$CORES_BACKEND gateway=$CORES_GATEWAY load=$CORES_LOAD (nproc=$(nproc))"
log "HEAD: $(git -C "$REPO" rev-parse --short HEAD) $(git -C "$REPO" status --porcelain | wc -l) dirty file(s)"

echo
echo "## Clean-window comparison ($(date '+%Y-%m-%d %H:%M'))"
echo
echo "- box: $(nproc) vCPU, gateway/nginx 2 workers, wrk -t$WRK_THREADS, ${DURATION}s per run"
echo "- pinning: backends=$CORES_BACKEND, gateways=$CORES_GATEWAY, load=$CORES_LOAD"
echo "- HEAD: $(git -C "$REPO" rev-parse --short HEAD), tree $([ "$(git -C "$REPO" status --porcelain | wc -l)" = "0" ] && echo clean || echo dirty)"

case "$MODE" in
  aa)   aa_calibrate ;;
  ab)   run_ab ;;
  both)
    if aa_calibrate; then
      run_ab
    else
      echo
      echo "A/B skipped: the A/A calibration did not pass, so no gw:nginx ratio is"
      echo "reported for this window. Re-run when the host is quieter."
      exit 2
    fi
    ;;
  *) echo "unknown mode '$MODE' (use aa|ab|both)" >&2; exit 1 ;;
esac
