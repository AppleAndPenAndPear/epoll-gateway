#!/usr/bin/env python3
"""P1 integration test framework: starts a real server + mock upstreams and
verifies end-to-end behavior.

Usage:  python3 tests/integration/run_integration_tests.py
Prereq: build build/server first (./build.sh or cmake --build build)

Covered scenarios (ROADMAP P1):
  1.  TLS handshake + static files
  2.  Keep-Alive connection reuse
  3.  X-Trace-Id propagation and echo
  4.  405 + Allow header
  5.  404 / path traversal 403
  6.  Reverse proxy forwarding
  7.  API key auth (401 / 200)
  8.  Token bucket rate limit 429
  9.  Upstream failover (unhealthy node removed by health check)
  10. Circuit breaker open / reject / recovery probe
  11. SIGHUP runtime reload takes effect
  12. Corrupt config reload rejected, old config kept
  13. /metrics endpoint
  14. Chunked responses

Only depends on the Python3 standard library and the built server binary.
"""

import http.client
import json
import os
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_BIN = os.path.join(REPO, "build", "server")
MOCK_BACKEND = os.path.join(HERE, "mock_backend.py")

SERVER_PORT = 5605
BACKEND_A = 5611   # Failover test node 1 (will be killed)
BACKEND_B = 5612   # Failover test node 2 (stays alive)
BACKEND_FAIL = 5613  # Circuit breaker test backend (can be switched to reject via a control file)
BACKEND_LIMIT = 5614  # Rate limit test backend

BASE = "127.0.0.1"
TLS_CTX = ssl._create_unverified_context()

PASS = 0
FAIL = 0


# ──────────────────────────── Infrastructure ────────────────────────────

def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  PASS  %s" % name)
    else:
        FAIL += 1
        print("  FAIL  %s   %s" % (name, detail))


def wait_for_port(port, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((BASE, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def request(path, method="GET", headers=None, body=None, port=SERVER_PORT, timeout=10):
    """Send an HTTPS request; returns (status, headers{lowercase keys}, body bytes), or (0, {}, b'') on failure."""
    conn = http.client.HTTPSConnection(BASE, port, context=TLS_CTX, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        hdrs = {k.lower(): v for k, v in resp.getheaders()}
        return resp.status, hdrs, data
    except Exception:
        return 0, {}, b""
    finally:
        conn.close()


def make_config(with_reloaded_route=False):
    routes = [
        {"name": "proxy-two", "method": "GET", "path": "/api/two/*",
         "target_type": "upstream",
         "upstream_target": {"name": "two", "timeout_ms": 3000,
                             "max_retries": 1,
                             "circuit_failure_threshold": 5,
                             "circuit_recovery_timeout_ms": 10000},
         "auth_required": False, "allow_anonymous": True},
        {"name": "proxy-single", "method": "GET", "path": "/api/single/*",
         "target_type": "upstream",
         "upstream_target": {"name": "single", "timeout_ms": 2000,
                             "max_retries": 0,
                             "circuit_failure_threshold": 2,
                             "circuit_recovery_timeout_ms": 2000},
         "auth_required": False, "allow_anonymous": True},
        {"name": "limited", "method": "GET", "path": "/api/limited/*",
         "target_type": "upstream",
         "upstream_target": {"name": "limit", "timeout_ms": 3000},
         "auth_required": True, "allowed_api_keys": ["limited-key-1", "burst-key-1"]},
    ]
    if with_reloaded_route:
        routes.append(
            {"name": "reloaded", "method": "GET", "path": "/api/reloaded/*",
             "target_type": "upstream",
             "upstream_target": {"name": "two", "timeout_ms": 3000},
             "auth_required": False, "allow_anonymous": True})
    return json.dumps({
        "port": SERVER_PORT,
        "backlog": 512,
        "num_workers": 1,
        "www_root": "./www",
        "cache_max_entries": 256,
        "cache_max_file_size_mb": 1,
        "keepalive_timeout": 5,
        "upstreams": {
            "two": {"servers": [
                {"host": BASE, "port": BACKEND_A},
                {"host": BASE, "port": BACKEND_B}],
                "algorithm": "round_robin"},
            "single": {"servers": [{"host": BASE, "port": BACKEND_FAIL}],
                       "algorithm": "round_robin"},
            "limit": {"servers": [{"host": BASE, "port": BACKEND_LIMIT}],
                      "algorithm": "round_robin"},
        },
        "upstream_health_check_timeout_ms": 300,
        "routes": routes,
        # Large global rate limit to avoid interfering with other cases; 429 is triggered by a dedicated api_key quota
        "rate_limit": {"capacity": 100000, "refill_per_second": 50000},
        "api_keys": [
            {"key": "limited-key-1", "name": "limited",
             "rate_limit": {"capacity": 1000, "refill_per_second": 500},
             "allowed_hosts": ["*"], "allowed_tenants": ["*"]},
            {"key": "burst-key-1", "name": "burst",
             "rate_limit": {"capacity": 1, "refill_per_second": 1},
             "allowed_hosts": ["*"], "allowed_tenants": ["*"]},
        ],
    }, indent=2)


# ──────────────────────────── Test cases ────────────────────────────

def test_tls_and_static():
    print("\n[1] TLS handshake + static files")
    status, hdrs, body = request("/")
    check("static index 200", status == 200, "got %s" % status)
    check("response contains test marker", b"INTEGRATION-INDEX" in body)
    check("Server header present", "server" in hdrs, "headers=%s" % hdrs)


def test_keepalive():
    print("\n[2] Keep-Alive connection reuse")
    try:
        conn = http.client.HTTPSConnection(BASE, SERVER_PORT, context=TLS_CTX, timeout=10)
        conn.request("GET", "/api/two/ka1")
        r1 = conn.getresponse()
        b1 = r1.read()
        conn.request("GET", "/api/two/ka2")
        r2 = conn.getresponse()
        b2 = r2.read()
        conn.close()
        check("two requests on the same connection both 200", r1.status == 200 and r2.status == 200,
              "got %s/%s" % (r1.status, r2.status))
        check("both requests hit the backend", b"backend" in b1 and b"backend" in b2)
    except Exception as e:
        check("Keep-Alive reuse", False, "exception: %s" % e)


def test_trace_id():
    print("\n[3] X-Trace-Id propagation and echo")
    status, hdrs, _ = request("/api/two/trace", headers={"X-Trace-Id": "itest-trace-123"})
    check("client trace_id echoed back",
          status == 200 and hdrs.get("x-trace-id") == "itest-trace-123",
          "status=%s trace=%s" % (status, hdrs.get("x-trace-id")))
    status2, hdrs2, _ = request("/api/two/trace")
    check("gateway generates trace_id when absent",
          status2 == 200 and len(hdrs2.get("x-trace-id", "")) > 0,
          "trace=%s" % hdrs2.get("x-trace-id"))


def test_405_allow():
    print("\n[4] 405 + Allow header")
    status, hdrs, _ = request("/api/echo")
    check("GET /api/echo returns 405", status == 405, "got %s" % status)
    allow = hdrs.get("allow", "")
    check("Allow header contains POST", "POST" in allow.upper(), "allow=%r" % allow)


def test_404_403():
    print("\n[5] 404 / path traversal 403")
    status, _, _ = request("/no/such/file.html")
    check("unknown path 404", status == 404, "got %s" % status)
    status, _, _ = request("/../etc/passwd")
    check("path traversal rejected with 403", status == 403, "got %s" % status)


def test_proxy():
    print("\n[6] Reverse proxy forwarding")
    status, _, body = request("/api/two/hello")
    check("proxied request 200", status == 200, "got %s" % status)
    ok = b"127.0.0.1:%d" % BACKEND_A in body or b"127.0.0.1:%d" % BACKEND_B in body
    check("response comes from the mock backend", ok, "body=%r" % body[:120])


def test_auth():
    print("\n[7] API key auth")
    status, _, _ = request("/api/limited/x")
    check("missing key returns 401", status == 401, "got %s" % status)
    status, _, _ = request("/api/limited/x", headers={"X-API-Key": "wrong-key"})
    check("wrong key returns 401", status == 401, "got %s" % status)
    status, _, _ = request("/api/limited/x", headers={"X-API-Key": "limited-key-1"})
    check("valid key returns 200", status == 200, "got %s" % status)


def test_rate_limit():
    print("\n[8] Token bucket rate limit 429")
    statuses = []
    for _ in range(3):
        s, _, _ = request("/api/limited/burst", headers={"X-API-Key": "burst-key-1"})
        statuses.append(s)
    # burst-key capacity=1, refill=1/s: 3 consecutive requests; the first should succeed and 429 should appear afterwards
    check("first request succeeds", statuses[0] == 200, "statuses=%s" % statuses)
    check("over-limit returns 429", 429 in statuses[1:], "statuses=%s" % statuses)


def test_failover(kill_backend_a):
    print("\n[9] Upstream failover")
    kill_backend_a()
    # Health checks run once per second; hitting the dead node returns 502, so we require
    # 6 consecutive successes to consider the node removed, avoiding false positives when
    # round-robin happens to land on the healthy node's turn.
    consecutive = 0
    last_status, last_body = 0, b""
    deadline = time.time() + 15
    while time.time() < deadline and consecutive < 6:
        status, _, body = request("/api/two/failover")
        if status == 200 and b"127.0.0.1:%d" % BACKEND_B in body:
            consecutive += 1
        else:
            consecutive = 0
            last_status, last_body = status, body
        time.sleep(0.2)
    check("6 consecutive 200s from the healthy node after removal", consecutive >= 6,
          "last_status=%s last_body=%r" % (last_status, last_body[:120]))


def test_circuit_breaker(fail_control_dir):
    print("\n[10] Circuit breaker open/reject/recover")
    reject_flag = os.path.join(fail_control_dir, "reject")
    open(reject_flag, "w").close()

    # threshold=2: the circuit opens after two failures
    s1, _, _ = request("/api/single/cb1")
    s2, _, _ = request("/api/single/cb2")
    check("backend failure returns 502", s1 == 502 and s2 == 502, "got %s,%s" % (s1, s2))
    s3, _, _ = request("/api/single/cb3")
    check("circuit returns 503 after reaching the threshold", s3 == 503, "got %s" % s3)

    # Remove the failure flag and wait out the recovery window (2000ms) before the half-open probe
    os.remove(reject_flag)
    time.sleep(2.5)
    s4, _, body = request("/api/single/cb4")
    check("half-open probe succeeds after the recovery window", s4 == 200, "got %s" % s4)
    s5, _, _ = request("/api/single/cb5")
    check("circuit closes and traffic recovers", s5 == 200, "got %s" % s5)


def test_reload(ws_dir):
    print("\n[11] SIGHUP runtime reload")
    status, _, _ = request("/api/reloaded/x")
    check("new route 404 before reload", status == 404, "got %s" % status)

    cfg_path = os.path.join(ws_dir, "config.json")
    with open(cfg_path, "w") as f:
        f.write(make_config(with_reloaded_route=True))
    os.kill(server_proc.pid, signal.SIGHUP)

    ok = False
    deadline = time.time() + 10
    while time.time() < deadline:
        status, _, body = request("/api/reloaded/x")
        if status == 200 and b"backend" in body:
            ok = True
            break
        time.sleep(0.3)
    check("new route active after SIGHUP", ok)


def test_corrupt_reload(ws_dir):
    print("\n[12] Corrupt config reload rejected")
    cfg_path = os.path.join(ws_dir, "config.json")
    with open(cfg_path, "w") as f:
        f.write("{ broken json !!!")
    os.kill(server_proc.pid, signal.SIGHUP)
    time.sleep(2)

    s1, _, _ = request("/api/reloaded/kept")
    s2, _, _ = request("/api/two/kept")
    check("invalid config does not override the old config", s1 == 200 and s2 == 200,
          "reloaded=%s two=%s" % (s1, s2))


def test_metrics():
    print("\n[13] /metrics endpoint")
    status, hdrs, body = request("/metrics")
    check("metrics 200", status == 200, "got %s" % status)
    check("request counter metric present", b"epoll_http_requests_total" in body)
    check("latency histogram metric present", b"epoll_http_request_duration_seconds" in body)


def test_chunked():
    print("\n[14] Chunked response")
    status, hdrs, _ = request("/chunked")
    check("chunked 200", status == 200, "got %s" % status)
    te = hdrs.get("transfer-encoding", "")
    check("Transfer-Encoding: chunked", te == "chunked", "te=%r" % te)


# ──────────────────────────── Main flow ────────────────────────────

server_proc = None
mock_procs = []


def cleanup():
    for p in mock_procs:
        if p.poll() is None:
            p.terminate()
    if server_proc and server_proc.poll() is None:
        server_proc.terminate()
        try:
            server_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server_proc.kill()
    for p in mock_procs:
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()


def main():
    global server_proc
    if not os.path.exists(SERVER_BIN):
        print("%s not found; build it first (./build.sh or cmake --build build)" % SERVER_BIN)
        return 1

    ws = tempfile.mkdtemp(prefix="epoll-int-")
    # The server reads certs/ and config.json via relative paths, so the cwd must be the workspace
    os.symlink(os.path.join(REPO, "certs"), os.path.join(ws, "certs"))
    os.makedirs(os.path.join(ws, "www"))
    with open(os.path.join(ws, "www", "index.html"), "w") as f:
        f.write("<html><body>INTEGRATION-INDEX</body></html>")
    with open(os.path.join(ws, "config.json"), "w") as f:
        f.write(make_config())
    fail_control_dir = tempfile.mkdtemp(prefix="epoll-mock-fail-")

    try:
        def start_mock(port):
            p = subprocess.Popen([sys.executable, MOCK_BACKEND, str(port), fail_control_dir],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            mock_procs.append(p)
            return p

        p_a = start_mock(BACKEND_A)
        start_mock(BACKEND_B)
        start_mock(BACKEND_FAIL)
        start_mock(BACKEND_LIMIT)
        time.sleep(0.5)
        if any(p.poll() is not None for p in mock_procs):
            print("failed to start mock backends")
            return 1

        log_out = open(os.path.join(ws, "server_stdout.log"), "w")
        server_proc = subprocess.Popen([SERVER_BIN], cwd=ws, stdout=log_out, stderr=log_out)
        if not wait_for_port(SERVER_PORT):
            print("failed to start server, log:")
            print(open(os.path.join(ws, "server_stdout.log")).read())
            return 1

        def kill_backend_a():
            if p_a.poll() is None:
                p_a.terminate()
                try:
                    p_a.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p_a.kill()

        test_tls_and_static()
        test_keepalive()
        test_trace_id()
        test_405_allow()
        test_404_403()
        test_proxy()
        test_auth()
        test_rate_limit()
        test_failover(kill_backend_a)
        test_circuit_breaker(fail_control_dir)
        test_reload(ws)
        test_corrupt_reload(ws)
        test_metrics()
        test_chunked()

        # Final check: the server stayed alive and responsive throughout
        status, _, _ = request("/")
        check("server alive throughout", status == 200, "got %s" % status)
    finally:
        cleanup()
        shutil.rmtree(ws, ignore_errors=True)
        shutil.rmtree(fail_control_dir, ignore_errors=True)

    print("\n========== Result: %d passed, %d failed ==========" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
