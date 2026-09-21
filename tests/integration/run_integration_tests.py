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
ADMIN_PORT = 5615   # Admin API listener (separate from the data plane)
ADMIN_KEY = "admin-secret-key"
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


# API keys live in their own file (P2 secret management), referenced from the
# main config via "api_keys_file"; the server cwd is the workspace, so a bare
# relative path resolves there.
API_KEYS_FILE = "api_keys.json"
API_KEYS = [
    {"key": "limited-key-1", "name": "limited",
     "rate_limit": {"capacity": 1000, "refill_per_second": 500},
     "allowed_hosts": ["*"], "allowed_tenants": ["*"]},
    {"key": "burst-key-1", "name": "burst",
     "rate_limit": {"capacity": 1, "refill_per_second": 1},
     "allowed_hosts": ["*"], "allowed_tenants": ["*"]},
]


def make_config(with_reloaded_route=False, tls_cert=None, tls_key=None, admin_key=None):
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
    cfg = {
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
        # Secrets stay out of the main config: keys come from api_keys.json
        "api_keys_file": API_KEYS_FILE,
        # Admin API on a separate loopback listener with its own key
        "admin": {"enabled": True, "port": ADMIN_PORT, "bind": "127.0.0.1",
                  "api_keys": [admin_key or ADMIN_KEY]},
    }
    if tls_cert and tls_key:
        cfg["tls"] = {"cert_path": tls_cert, "key_path": tls_key}
    return json.dumps(cfg, indent=2)


def test_tls_hardening():
    print("\n[15] TLS hardening (min version 1.2)")
    # A legacy client offering only TLS 1.1 or below must be refused by the server
    rejected = False
    try:
        legacy = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        legacy.check_hostname = False
        legacy.verify_mode = ssl.CERT_NONE
        legacy.minimum_version = ssl.TLSVersion.MINIMUM_SUPPORTED
        legacy.maximum_version = ssl.TLSVersion.TLSv1_1
        # avoid a client-side security-level failure masking the server refusal
        try:
            legacy.set_ciphers("DEFAULT@SECLEVEL=0")
        except ssl.SSLError:
            pass
        with socket.create_connection((BASE, SERVER_PORT), timeout=5) as raw:
            with legacy.wrap_socket(raw):
                pass
    except Exception:
        rejected = True
    check("TLS 1.1 handshake refused", rejected)

    status, _, _ = request("/")
    check("TLS 1.2+ handshake still works", status == 200, "got %s" % status)


def peer_cert_fingerprint():
    """TLS-connects and returns the DER peer certificate bytes, or None."""
    try:
        with socket.create_connection((BASE, SERVER_PORT), timeout=5) as raw:
            with TLS_CTX.wrap_socket(raw) as ss:
                return ss.getpeercert(binary_form=True)
    except Exception:
        return None


def test_tls_cert_hot_reload(ws_dir):
    print("\n[16] TLS certificate hot reload")
    if shutil.which("openssl") is None:
        print("  SKIP: openssl CLI not available")
        return

    old_fp = peer_cert_fingerprint()
    check("captured current certificate", old_fp is not None)
    if old_fp is None:
        return

    cert_path = os.path.join(ws_dir, "tls-reload.crt")
    key_path = os.path.join(ws_dir, "tls-reload.key")
    gen = subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key_path, "-out", cert_path, "-days", "1",
         "-subj", "/CN=gateway-reload-test"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    check("generated replacement certificate", gen.returncode == 0)
    if gen.returncode != 0:
        return

    cfg_path = os.path.join(ws_dir, "config.json")
    with open(cfg_path, "w") as f:
        f.write(make_config(with_reloaded_route=True, tls_cert=cert_path, tls_key=key_path))
    os.kill(server_proc.pid, signal.SIGHUP)

    new_fp = None
    deadline = time.time() + 10
    while time.time() < deadline:
        new_fp = peer_cert_fingerprint()
        if new_fp is not None and new_fp != old_fp:
            break
        time.sleep(0.3)
    check("peer certificate changed after SIGHUP", new_fp is not None and new_fp != old_fp)

    status, _, _ = request("/api/reloaded/x")
    check("traffic healthy after cert reload", status == 200, "got %s" % status)


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


def test_connection_reuse(control_dir):
    print("\n[07b] Upstream connection reuse (keep-alive pool)")
    count_path = os.path.join(control_dir, "conn_count")

    def conn_count():
        try:
            with open(count_path) as f:
                return len(f.read().split())
        except OSError:
            return 0

    before = conn_count()
    ok = 0
    for _ in range(10):
        status, _, _ = request("/api/two/pool")
        if status == 200:
            ok += 1
    after = conn_count()
    new_conns = after - before
    check("10 proxied requests succeeded", ok == 10, "%d/10 ok" % ok)
    check("backend connections reused by the pool", new_conns <= 4,
          "%d new backend connections for 10 requests" % new_conns)


def test_chunked_trailer():
    print("\n[07c] Chunked response with trailer (pool-safe framing)")
    status, _, body = request("/api/two/chunked-trailer")
    check("chunked+trailer response proxied with full body",
          status == 200 and body == b"abcde", "status=%s body=%r" % (status, body[:60]))
    # The pooled connection must survive the trailer: a second request on the
    # same upstream must still work (would time out/garble if leftover CRLF
    # from the trailer were kept on the connection).
    status2, _, body2 = request("/api/two/chunked-trailer")
    check("connection reusable after chunked trailer",
          status2 == 200 and body2 == b"abcde", "status=%s" % status2)


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


def test_ops_endpoints():
    print("\n[15] /healthz /readyz /version endpoints")
    status, hdrs, body = request("/healthz")
    check("healthz 200", status == 200, "got %s" % status)
    check("healthz body ok", body.strip() == b"ok", "body=%r" % body)

    status, _, body = request("/version")
    check("version 200", status == 200, "got %s" % status)
    ver = None
    try:
        ver = json.loads(body).get("version")
    except Exception:
        pass
    check("version reports a semver string", isinstance(ver, str) and ver.count(".") == 2,
          "body=%r" % body)

    status, _, body = request("/readyz")
    check("readyz 200 with healthy backends", status == 200, "got %s status, body=%r" % (status, body))
    try:
        doc = json.loads(body)
    except Exception:
        doc = {}
    check("readyz status=ready", doc.get("status") == "ready", "body=%r" % body)
    ups = doc.get("upstreams", {})
    check("readyz lists upstreams with healthy>0",
          bool(ups) and all(v.get("healthy", 0) > 0 for v in ups.values()), "upstreams=%r" % ups)


def test_rate_limit_exemption():
    print("\n[16] Probe endpoints are exempt from rate limiting")
    statuses = set()
    for _ in range(30):
        status, _, _ = request("/healthz")
        statuses.add(status)
    check("healthz never 429 under burst", statuses == {200}, "statuses=%r" % statuses)


def admin_request(path, method="GET", headers=None, body_bytes=None):
    """Plain-HTTP request to the admin listener; returns (status, body bytes)."""
    conn = http.client.HTTPConnection(BASE, ADMIN_PORT, timeout=5)
    try:
        conn.request(method, path, body=body_bytes, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def test_admin_api(ws_dir):
    print("\n[17] Admin API (separate listener + auth)")
    auth = {"X-API-Key": ADMIN_KEY}

    status, _ = admin_request("/admin/stats")
    check("admin without key 401", status == 401, "got %s" % status)
    status, _ = admin_request("/admin/stats", headers={"X-API-Key": "wrong-key"})
    check("admin with wrong key 401", status == 401, "got %s" % status)

    status, body = admin_request("/admin/stats", headers=auth)
    check("admin stats 200", status == 200, "got %s" % status)
    try:
        doc = json.loads(body)
    except Exception:
        doc = {}
    check("stats has request counters",
          isinstance(doc.get("requests"), dict) and "total" in doc["requests"],
          "body=%r" % body[:120])
    check("stats has uptime and version",
          isinstance(doc.get("uptime_seconds"), int) and "version" in doc, "body=%r" % body[:120])

    status, body = admin_request("/admin/upstreams", headers=auth)
    check("admin upstreams 200", status == 200, "got %s" % status)
    try:
        doc = json.loads(body)
    except Exception:
        doc = {}
    ups = {u["name"]: u for u in doc.get("upstreams", [])}
    check("upstreams snapshot lists all upstreams",
          {"two", "single", "limit"} <= set(ups), "names=%r" % sorted(ups))
    check("backend status has health and circuit fields",
          "two" in ups and len(ups["two"].get("backends", [])) == 2 and
          all("circuit_open" in b and "healthy" in b for b in ups["two"]["backends"]),
          "two=%r" % ups.get("two"))

    status, _ = admin_request("/admin/unknown", headers=auth)
    check("unknown admin endpoint 404", status == 404, "got %s" % status)

    # Corrupt config on disk must be rejected without touching the live config
    cfg_path = os.path.join(ws_dir, "config.json")
    with open(cfg_path, "w") as f:
        f.write("{ broken json !!!")
    status, body = admin_request("/admin/reload", method="POST", headers=auth)
    check("admin reload rejects corrupt config", status == 400, "got %s status, body=%r" % (status, body[:120]))

    # Restore a valid config via the admin API and confirm it applies
    with open(cfg_path, "w") as f:
        f.write(make_config(with_reloaded_route=True))
    status, body = admin_request("/admin/reload", method="POST", headers=auth)
    check("admin reload accepts valid config", status == 200, "got %s" % status)
    time.sleep(1.5)   # workers apply the reload at their next checkpoint (<=1s)
    status, _, _ = request("/api/reloaded/x")
    check("traffic healthy after admin reload", status == 200, "got %s" % status)

    # Rotate the admin key via reload: the new key must be accepted and the
    # old one rejected, without restarting the process (<=1s poll loop)
    rotated_key = "rotated-admin-key-77"
    with open(cfg_path, "w") as f:
        f.write(make_config(with_reloaded_route=True, admin_key=rotated_key))
    status, body = admin_request("/admin/reload", method="POST", headers=auth)
    check("admin reload with rotated key accepted", status == 200, "got %s" % status)
    time.sleep(1.5)   # admin loop adopts the new keys at its next poll (<=1s)
    status, _ = admin_request("/admin/stats", headers=auth)
    check("old admin key rejected after rotation", status == 401, "got %s" % status)
    status, _ = admin_request("/admin/stats", headers={"X-API-Key": rotated_key})
    check("new admin key accepted after rotation", status == 200, "got %s" % status)

    # A POST carrying a body is never parsed (no admin endpoint takes one), but
    # the response must still arrive: closing the socket with unread body bytes
    # makes the kernel send RST, which can discard the response we just wrote.
    status, resp_body = admin_request("/admin/reload", method="POST",
                                      headers={"X-API-Key": rotated_key},
                                      body_bytes=b'{"note":"ignored"}')
    check("admin reload with request body still returns its response",
          status == 200, "got %s status, body=%r" % (status, resp_body[:120]))


def test_graceful_shutdown():
    print("\n[18] SIGTERM graceful shutdown")
    # Start an in-flight request against the slow backend (sleeps 1.5s), then
    # send SIGTERM while it is being proxied: the gateway must finish it.
    import threading
    result = {}

    def slow_req():
        result['s'], result['h'], result['b'] = request("/api/two/slow", timeout=10)

    t = threading.Thread(target=slow_req)
    t.start()
    time.sleep(0.6)   # the request is now inside the gateway / backend
    os.kill(server_proc.pid, signal.SIGTERM)
    t.join(timeout=10)
    check("in-flight request completed during shutdown", result.get('s') == 200,
          "got %s" % result.get('s'))
    # After SIGTERM the gateway must stop accepting/serving new connections
    status, _, _ = request("/healthz", timeout=3)
    check("new request after SIGTERM is not served", status == 0, "got %s" % status)
    try:
        rc = server_proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        rc = None
    check("server exits cleanly on SIGTERM", rc == 0, "rc=%s" % rc)


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
    with open(os.path.join(ws, API_KEYS_FILE), "w") as f:
        f.write(json.dumps(API_KEYS, indent=2))
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
        # Wait for each mock to actually bind instead of sleeping a fixed time.
        # The server starts accepting right after this, so any proxy test that
        # runs before a backend is listening sees a refused upstream connection
        # and fails with an intermittent 502 (the backend probe connection is
        # counted, but test_connection_reuse snapshots the counter later).
        for port in (BACKEND_A, BACKEND_B, BACKEND_FAIL, BACKEND_LIMIT):
            if not wait_for_port(port):
                print("mock backend on port %d never came up" % port)
                return 1
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
        test_connection_reuse(fail_control_dir)
        test_chunked_trailer()
        test_auth()
        test_rate_limit()
        test_failover(kill_backend_a)
        test_circuit_breaker(fail_control_dir)
        test_reload(ws)
        test_corrupt_reload(ws)
        test_metrics()
        test_chunked()
        test_ops_endpoints()
        test_rate_limit_exemption()
        test_tls_hardening()
        test_tls_cert_hot_reload(ws)
        test_admin_api(ws)

        # Final check: the server stayed alive and responsive throughout
        status, _, _ = request("/")
        check("server alive throughout", status == 200, "got %s" % status)

        # Must be the last scenario: it terminates the server.
        test_graceful_shutdown()
    finally:
        cleanup()
        shutil.rmtree(ws, ignore_errors=True)
        shutil.rmtree(fail_control_dir, ignore_errors=True)

    print("\n========== Result: %d passed, %d failed ==========" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
