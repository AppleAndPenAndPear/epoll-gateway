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
  15. Upstream TLS: https:// backend, hostname verification (positive/negative)

Only depends on the Python3 standard library and the built server binary.
"""

import http.client
import hashlib
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
CLIENT_BIN = os.path.join(REPO, "build", "client")
MOCK_BACKEND = os.path.join(HERE, "mock_backend.py")

SERVER_PORT = 5605
BACKEND_A = 5611   # Failover test node 1 (will be killed)
BACKEND_B = 5612   # Failover test node 2 (stays alive)
BACKEND_FAIL = 5613  # Circuit breaker test backend (can be switched to reject via a control file)
BACKEND_LIMIT = 5614  # Rate limit test backend
BACKEND_TLS = 5616   # TLS test backend (https upstream)
PROFILE_USER = 5617  # Composite profile user dependency
PROFILE_ORDERS = 5618  # Composite profile orders dependency
PROFILE_RECOMMENDATIONS = 5619  # Composite profile recommendations dependency

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


def make_config(with_reloaded_route=False, tls_cert=None, tls_key=None, admin_key=None,
                access_log_enabled=True):
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
        {"name": "proxy-tls", "method": "GET", "path": "/api/tls/*",
         "target_type": "upstream",
         "upstream_target": {"name": "tls_single", "timeout_ms": 3000},
         "auth_required": False, "allow_anonymous": True},
        {"name": "proxy-tls-bad", "method": "GET", "path": "/api/tls-bad/*",
         "target_type": "upstream",
         "upstream_target": {"name": "tls_bad", "timeout_ms": 3000},
         "auth_required": False, "allow_anonymous": True},
        {"name": "proxy-tls-insecure", "method": "GET", "path": "/api/tls-insecure/*",
         "target_type": "upstream",
         "upstream_target": {"name": "tls_insecure", "timeout_ms": 3000},
         "auth_required": False, "allow_anonymous": True},
        {"name": "profile-page", "method": "GET", "path": "/profile",
         "target_type": "composite",
         "composite_target": {"name": "profile-page",
                             "dependencies": [
                                 {"name": "user", "upstream_name": "user-service", "path": "/me", "method": "GET", "required": True, "timeout_ms": 2000},
                                 {"name": "orders", "upstream_name": "orders-service", "path": "/orders/42", "method": "GET", "required": False, "timeout_ms": 2000},
                                 {"name": "recommendations", "upstream_name": "recommendations-service", "path": "/recommendations/42", "method": "GET", "required": True, "timeout_ms": 2000},
                             ]},
         "auth_required": False, "allow_anonymous": True},
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
            # https:// scheme in the host enables upstream TLS. tls_single pins
            # the name to the cert's CN (localhost); tls_bad uses a name the
            # certificate does not carry and must fail verification; tls_insecure
            # exercises the documented skip-verify escape hatch with that same
            # bad name and must succeed.
            "tls_single": {"servers": [{"host": "https://" + BASE, "port": BACKEND_TLS,
                                        "tls_ca_file": "certs/server.crt",
                                        "tls_server_name": "localhost"}],
                           "algorithm": "round_robin"},
            "tls_bad": {"servers": [{"host": "https://" + BASE, "port": BACKEND_TLS,
                                     "tls_ca_file": "certs/server.crt",
                                     "tls_server_name": "wrong.example.com"}],
                        "algorithm": "round_robin"},
            "tls_insecure": {"servers": [{"host": "https://" + BASE, "port": BACKEND_TLS,
                                          "tls_skip_verify": True,
                                          "tls_server_name": "wrong.example.com"}],
                             "algorithm": "round_robin"},
            "user-service": {"servers": [{"host": BASE, "port": PROFILE_USER}],
                             "algorithm": "round_robin"},
            "orders-service": {"servers": [{"host": BASE, "port": PROFILE_ORDERS}],
                               "algorithm": "round_robin"},
            "recommendations-service": {"servers": [{"host": BASE, "port": PROFILE_RECOMMENDATIONS}],
                                       "algorithm": "round_robin"},
        },
        "upstream_health_check_timeout_ms": 300,
        "logging": {
            "access_log_enabled": access_log_enabled,
            "redact_sensitive_headers": True,
            "redact_sensitive_query_params": True,
            "sensitive_headers": [
                "authorization", "proxy-authorization", "x-api-key", "cookie",
                "set-cookie", "x-forwarded-for", "x-real-ip"
            ],
            "sensitive_query_params": [
                "access_token", "api_key", "apikey", "key", "password", "secret", "token"
            ]
        },
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


def test_upstream_tls():
    print("\n[16] Upstream TLS (https:// backend, hostname verification)")
    # Positive: the gateway completes a client-side TLS handshake with the
    # backend, verifies its certificate against tls_ca_file, and forwards.
    status, _, body = request("/api/tls/hello")
    check("https upstream 200", status == 200, "got %s %r" % (status, body[:120]))
    ok = status == 200 and ("127.0.0.1:%d" % BACKEND_TLS).encode() in body
    check("https upstream hit the TLS backend", bool(ok), body[:120])
    # A second round trip exercises the pooled TLS connection (SSL_peek probe
    # + connection reuse must work with the TLS session intact).
    status, _, body = request("/api/tls/again")
    check("https upstream 200 on pooled connection", status == 200,
          "got %s %r" % (status, body[:120]))
    # Negative: certificate does not carry the configured server name —
    # verification must fail and the gateway must answer 502, never downgrade.
    status, _, _ = request("/api/tls-bad/hello")
    check("hostname mismatch rejected with 502", status == 502, "got %s" % status)
    # Escape hatch: tls_skip_verify disables verification entirely.
    status, _, _ = request("/api/tls-insecure/hello")
    check("tls_skip_verify reaches the backend", status == 200, "got %s" % status)


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

def test_companion_client():
    print("\n[15b] Companion C++ client (TLS + certificate verification)")
    if not os.path.exists(CLIENT_BIN):
        print("  SKIP: %s not built" % CLIENT_BIN)
        return

    ca = os.path.join(REPO, "certs", "server.crt")
    # The generated dev certificate carries CN=localhost and SAN DNS:localhost,
    # so "localhost" is the only name that verifies against the repo CA
    good = subprocess.run(
        [CLIENT_BIN, "--host", "localhost", "--port", str(SERVER_PORT),
         "--path", "/", "--ca", ca],
        cwd=REPO, capture_output=True, timeout=30)
    check("client exits 0 on a verified response", good.returncode == 0,
          "rc=%s stderr=%s" % (good.returncode, good.stderr[-200:]))
    check("client received HTTP 200", b"HTTP/1.1 200" in good.stdout,
          "stdout=%s" % good.stdout[:120])

    # An IP literal can never match CN, so this proves the hostname check is
    # really enabled (SSL_set1_host is a no-op without SSL_VERIFY_PEER)
    bad = subprocess.run(
        [CLIENT_BIN, "--host", "127.0.0.1", "--port", str(SERVER_PORT),
         "--path", "/", "--ca", ca],
        cwd=REPO, capture_output=True, timeout=30)
    check("hostname mismatch is rejected", bad.returncode != 0,
          "rc=%s stdout=%s" % (bad.returncode, bad.stdout[:120]))


def test_tls_and_static():
    print("\n[1] TLS handshake + static files")
    status, hdrs, body = request("/")
    check("static index 200", status == 200, "got %s" % status)
    check("response contains test marker", b"INTEGRATION-INDEX" in body)
    check("Server header present", "server" in hdrs, "headers=%s" % hdrs)


def test_static_file_backpressure(expected_body):
    print("\n[1b] TLS static-file writes under client backpressure")
    conn = http.client.HTTPSConnection(BASE, SERVER_PORT, context=TLS_CTX, timeout=30)
    try:
        conn.request("GET", "/large.bin", headers={"Connection": "close"})
        time.sleep(0.25)
        response = conn.getresponse()
        body = response.read()
        check("large TLS static file returns 200", response.status == 200,
              "got %s" % response.status)
        check("large TLS static file is complete after partial writes",
              len(body) == len(expected_body) and
              hashlib.sha256(body).digest() == hashlib.sha256(expected_body).digest(),
              "expected=%d actual=%d" % (len(expected_body), len(body)))
    except Exception as exc:
        check("large TLS static file survives backpressure", False, str(exc))
    finally:
        conn.close()


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


def test_pipelined_requests():
    print("\n[2b] HTTP pipelined requests in one TCP write")
    raw_sock = socket.create_connection((BASE, SERVER_PORT), timeout=5)
    tls_sock = TLS_CTX.wrap_socket(raw_sock, server_hostname=BASE)
    tls_sock.settimeout(5)
    first_request = (
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
    ).encode()
    second_request = (
        "GET /version HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    ).encode()
    responses = []
    pending = b""
    try:
        tls_sock.sendall(first_request[:11])
        time.sleep(0.1)
        tls_sock.sendall(first_request[11:] + second_request)
        while len(responses) < 2:
            chunk = tls_sock.recv(4096)
            if not chunk:
                break
            pending += chunk
            while len(responses) < 2:
                header_end = pending.find(b"\r\n\r\n")
                if header_end < 0:
                    break
                header = pending[:header_end].decode("latin1")
                content_length = 0
                for line in header.split("\r\n")[1:]:
                    if line.lower().startswith("content-length:"):
                        content_length = int(line.split(":", 1)[1].strip())
                        break
                response_end = header_end + 4 + content_length
                if len(pending) < response_end:
                    break
                responses.append((header.split("\r\n", 1)[0],
                                  pending[header_end + 4:response_end]))
                pending = pending[response_end:]
    except (OSError, ssl.SSLError) as exc:
        check("both pipelined responses received", False, str(exc))
        return
    finally:
        tls_sock.close()

    check("both pipelined responses received", len(responses) == 2,
          "received %d responses" % len(responses))
    if len(responses) == 2:
        check("pipelined responses preserve request order",
              responses[0][0].startswith("HTTP/1.1 200") and
              responses[0][1].strip() == b"ok" and
              responses[1][0].startswith("HTTP/1.1 200") and
              b"version" in responses[1][1],
              "statuses/bodies=%r" % (responses,))


def test_trace_id():
    print("\n[3] X-Trace-Id + traceparent propagation and echo")
    custom_tp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"
    status, hdrs, _ = request("/api/two/trace", headers={"X-Trace-Id": "itest-trace-123", "traceparent": custom_tp})
    check("client trace_id echoed back",
          status == 200 and hdrs.get("x-trace-id") == "itest-trace-123",
          "status=%s trace=%s" % (status, hdrs.get("x-trace-id")))
    check("client traceparent echoed back",
          status == 200 and hdrs.get("traceparent") == custom_tp,
          "status=%s traceparent=%s" % (status, hdrs.get("traceparent")))
    status2, hdrs2, _ = request("/api/two/trace")
    check("gateway generates trace_id when absent",
          status2 == 200 and len(hdrs2.get("x-trace-id", "")) > 0,
          "trace=%s" % hdrs2.get("x-trace-id"))
    check("gateway generates traceparent when absent",
          status2 == 200 and len(hdrs2.get("traceparent", "")) > 0,
          "traceparent=%s" % hdrs2.get("traceparent"))


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


def test_profile_composite_route(fail_control_dir):
    print("\n[07c] Composite profile route")

    status, _, body = request("/profile")
    check("profile route success", status == 200, "got %s" % status)
    if status != 200:
        return

    payload = json.loads(body.decode())
    check("profile route status is ok", payload.get("status") == "ok", "got %s" % payload)
    check("profile user dependency succeeded", payload["dependencies"]["user"]["status"] == "ok", str(payload))
    check("profile orders dependency succeeded", payload["dependencies"]["orders"]["status"] == "ok", str(payload))
    check("profile recommendations dependency succeeded", payload["dependencies"]["recommendations"]["status"] == "ok", str(payload))
    check("user dependency includes timing and trace context", 
          payload["dependencies"]["user"].get("duration_ms") is not None and payload["dependencies"]["user"].get("traceparent"),
          str(payload["dependencies"]["user"]))

    reject_path = os.path.join(fail_control_dir, "reject")
    with open(reject_path, "w") as f:
        f.write("1")
    try:
        status, _, body = request("/profile")
        check("profile route fails required dependency deterministically", status == 503, "got %s" % status)
        if status == 503:
            payload = json.loads(body.decode())
            check("profile route reports partial failure", payload.get("status") == "partial_failure", str(payload))
            check("profile route exposes dependency detail", "user" in payload["dependencies"], str(payload))
    finally:
        try:
            os.unlink(reject_path)
        except FileNotFoundError:
            pass


def test_chunked_trailer():
    print("\n[07d] Chunked response with trailer (pool-safe framing)")
    status, _, body = request("/api/two/chunked-trailer")
    check("chunked+trailer response proxied with full body",
          status == 200 and body == b"abcde", "status=%s body=%r" % (status, body[:60]))
    # The pooled connection must survive the trailer: a second request on the
    # same upstream must still work (would time out/garble if leftover CRLF
    # from the trailer were kept on the connection).
    status2, _, body2 = request("/api/two/chunked-trailer")
    check("connection reusable after chunked trailer",
          status2 == 200 and body2 == b"abcde", "status=%s" % status2)


def test_forwarded_headers():
    print("\n[07d] Caller identity injected on the upstream hop")
    status, _, body = request("/api/two/echo-headers")
    doc = {}
    try:
        doc = json.loads(body.decode())
    except (ValueError, UnicodeDecodeError):
        pass
    check("backend sees X-Forwarded-For / X-Real-IP",
          status == 200 and doc.get("xff") == "127.0.0.1" and doc.get("real_ip") == "127.0.0.1",
          "status=%s body=%r" % (status, body[:160]))
    hop = doc.get("headers", {})
    check("backend sees X-Forwarded-Proto https", hop.get("x-forwarded-proto") == "https",
          "headers=%s" % hop)
    check("backend sees original Host in X-Forwarded-Host",
          hop.get("x-forwarded-host") == "%s:%d" % (BASE, SERVER_PORT), "headers=%s" % hop)
    # Client-supplied values for these two must not be trusted.
    status3, _, body3 = request("/api/two/echo-headers", headers={
        "X-Forwarded-Proto": "http", "X-Forwarded-Host": "evil.example.com",
        "X-Real-IP": "9.9.9.9"})
    hop3 = {}
    try:
        hop3 = json.loads(body3.decode()).get("headers", {})
    except (ValueError, UnicodeDecodeError):
        pass
    check("spoofed X-Forwarded-Proto overwritten",
          status3 == 200 and hop3.get("x-forwarded-proto") == "https", "headers=%s" % hop3)
    check("spoofed X-Forwarded-Host overwritten",
          hop3.get("x-forwarded-host") == "%s:%d" % (BASE, SERVER_PORT), "headers=%s" % hop3)
    check("spoofed X-Real-IP overwritten",
          hop3.get("x-real-ip") == "127.0.0.1", "headers=%s" % hop3)
    status2, _, body2 = request("/api/two/echo-headers",
                                headers={"X-Forwarded-For": "203.0.113.9"})
    ok2 = False
    try:
        doc2 = json.loads(body2.decode())
        ok2 = doc2.get("xff") == "203.0.113.9, 127.0.0.1"
    except (ValueError, UnicodeDecodeError):
        pass
    check("client X-Forwarded-For chain is extended, not replaced",
          status2 == 200 and ok2, "status=%s body=%r" % (status2, body2[:160]))


def test_hop_by_hop_stripping():
    print("\n[07e] Hop-by-hop header stripping (request + response side)")
    # Request side: the gateway must not forward hop-by-hop fields, nor any
    # field named in the client's Connection header.
    status, _, body = request("/api/two/echo-headers", headers={
        "Proxy-Authorization": "Basic c2VjcmV0",
        "TE": "trailers",
        "Connection": "X-Custom-Hop",
        "X-Custom-Hop": "dropme",
        "X-Keep-Me": "keepme",
    })
    hdrs = {}
    try:
        hdrs = json.loads(body.decode()).get("headers", {})
    except (ValueError, UnicodeDecodeError):
        pass
    check("request Proxy-Authorization not forwarded upstream",
          status == 200 and "proxy-authorization" not in hdrs, "hdrs=%s" % hdrs)
    check("request TE not forwarded upstream", "te" not in hdrs, "hdrs=%s" % hdrs)
    check("Connection-named request header stripped upstream",
          "x-custom-hop" not in hdrs, "hdrs=%s" % hdrs)
    check("benign request header still forwarded upstream",
          hdrs.get("x-keep-me") == "keepme", "hdrs=%s" % hdrs)

    # Response side: hop-by-hop fields from the backend (including those named
    # in its Connection header) and Proxy-* must be consumed, not relayed.
    status, rhdrs, rbody = request("/api/two/hopbyhop")
    check("backend hopbyhop response proxied", status == 200 and rbody == b"hop",
          "status=%s body=%r" % (status, rbody[:40]))
    check("backend Connection-named field stripped", "x-internal" not in rhdrs, "hdrs=%s" % rhdrs)
    check("backend Proxy-Authenticate stripped", "proxy-authenticate" not in rhdrs, "hdrs=%s" % rhdrs)
    check("backend Connection header not relayed verbatim",
          rhdrs.get("connection", "").lower() != "x-internal", "hdrs=%s" % rhdrs)
    check("benign backend header preserved", rhdrs.get("x-keep") == "visible", "hdrs=%s" % rhdrs)


def test_upstream_response_header_injection():
    print("\n[07f] Upstream response header injection (bare LF in value)")
    # The backend's X-Inject value carries a bare LF followed by "X-Evil: 1".
    # Re-emitting the header verbatim would inject X-Evil toward the client.
    status, hdrs, body = request("/api/two/inject")
    check("injecting backend response proxied", status == 200 and body == b"ok",
          "status=%s body=%r" % (status, body[:40]))
    check("injected header not present in gateway response", "x-evil" not in hdrs, "hdrs=%s" % hdrs)
    check("header carrying CR/LF dropped entirely", "x-inject" not in hdrs, "hdrs=%s" % hdrs)


def test_no_double_gzip():
    print("\n[07g] Already-compressed upstream body is not re-gzipped")
    import gzip
    status, hdrs, body = request("/api/two/gzip", headers={"Accept-Encoding": "gzip"})
    check("gzip upstream response proxied", status == 200, "got %s" % status)
    decoded = None
    try:
        decoded = gzip.decompress(body)
    except Exception:
        decoded = None
    check("client decodes exactly one gzip layer",
          decoded is not None and decoded.startswith(b"gateway-gzip-test-payload-"),
          "body_len=%d" % len(body))
    check("Content-Encoding not duplicated", hdrs.get("content-encoding") == "gzip",
          "got %r" % hdrs.get("content-encoding"))


def test_composite_fanout_bounded():
    print("\n[07h] Composite fan-out is bounded under load")
    import threading
    results = []
    lock = threading.Lock()

    def worker():
        st, _, bd = request("/profile", timeout=20)
        with lock:
            results.append((st, bd))

    threads = [threading.Thread(target=worker) for _ in range(24)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=40)
    check("all composite load requests returned", len(results) == 24, "%d/24" % len(results))

    statuses = sorted({st for st, _ in results})
    check("overload degrades to 200/503 (no hang, no crash)",
          all(st in (200, 503) for st, _ in results), "statuses=%s" % statuses)
    structured = 0
    for _, bd in results:
        try:
            doc = json.loads(bd.decode())
            if isinstance(doc.get("dependencies"), dict):
                structured += 1
        except (ValueError, UnicodeDecodeError):
            pass
    check("every response is a structured composite document",
          structured == len(results), "%d/%d structured" % (structured, len(results)))
    st, _, _ = request("/")
    check("server responsive after composite burst", st == 200, "got %s" % st)


def test_auth():
    print("\n[7] API key auth")
    status, _, _ = request("/api/limited/x")
    check("missing key returns 401", status == 401, "got %s" % status)
    status, _, _ = request("/api/limited/x", headers={"X-API-Key": "wrong-key"})
    check("wrong key returns 401", status == 401, "got %s" % status)
    status, _, _ = request("/api/limited/x", headers={"X-API-Key": "limited-key-1"})
    check("valid key returns 200", status == 200, "got %s" % status)


def test_logging_policy(ws_dir):
    print("\n[logging] access-log switch and redaction")
    log_path = os.path.join(ws_dir, "logs", "epollserver.log")

    def read_log():
        with open(log_path) as log_file:
            return log_file.read()

    status, _, _ = request("/api/two/log-policy?token=access-query-secret&view=full")
    check("request succeeds with access logging enabled", status == 200, "got %s" % status)

    access_line = '"GET /api/two/log-policy?token=[redacted]&view=full HTTP/'
    deadline = time.time() + 5
    logs = read_log()
    while access_line not in logs and time.time() < deadline:
        time.sleep(0.05)
        logs = read_log()
    check("CLF redacts sensitive query values", access_line in logs, "missing redacted request target")
    check("CLF does not contain raw query secret", "access-query-secret" not in logs)

    cfg = json.loads(make_config(access_log_enabled=False))
    cfg["logging"]["sensitive_headers"].append("user-agent")
    with open(os.path.join(ws_dir, "config.json"), "w") as config_file:
        json.dump(cfg, config_file)
    reload_marker = "AUDIT config_reload_applied path=config.json"
    reload_count = read_log().count(reload_marker)
    os.kill(server_proc.pid, signal.SIGHUP)

    deadline = time.time() + 10
    while time.time() < deadline:
        logs = read_log()
        if logs.count(reload_marker) > reload_count:
            break
        time.sleep(0.1)
    check("logging policy applies on config reload", logs.count(reload_marker) > reload_count)

    audit_target = "/missing/log-policy?token=audit-query-secret&view=full"
    status, _, _ = request(
        audit_target,
        headers={
            "X-API-Key": "audit-api-secret",
            "Authorization": "Bearer audit-auth-secret",
            "Cookie": "session=audit-cookie-secret",
            "User-Agent": "audit-agent-secret",
        })
    check("missing route produces an AUDIT event", status == 404, "got %s" % status)

    audit_marker = "path=/missing/log-policy?token=[redacted]&view=full"
    deadline = time.time() + 5
    logs = read_log()
    while audit_marker not in logs and time.time() < deadline:
        time.sleep(0.05)
        logs = read_log()
    check("AUDIT remains enabled when CLF is disabled", audit_marker in logs)
    check("AUDIT redacts configured credentials",
          "api_key=[redacted]" in logs and "authorization=[redacted]" in logs and
          "cookie=[redacted]" in logs and "user_agent=[redacted]" in logs)
    check("disabled CLF does not write the audit request line",
          '"GET %s HTTP/' % audit_target not in logs)
    check("AUDIT output does not contain raw credentials",
          all(secret not in logs for secret in (
              "audit-query-secret", "audit-api-secret", "audit-auth-secret",
              "audit-cookie-secret", "audit-agent-secret")))


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
    os.makedirs(os.path.join(ws, "logs"))
    with open(os.path.join(ws, "www", "index.html"), "w") as f:
        f.write("<html><body>INTEGRATION-INDEX</body></html>")
    large_body = bytes(range(256)) * (8 * 1024 * 1024 // 256)
    with open(os.path.join(ws, "www", "large.bin"), "wb") as f:
        f.write(large_body)
    with open(os.path.join(ws, "config.json"), "w") as f:
        f.write(make_config())
    with open(os.path.join(ws, API_KEYS_FILE), "w") as f:
        f.write(json.dumps(API_KEYS, indent=2))
    fail_control_dir = tempfile.mkdtemp(prefix="epoll-mock-fail-")

    try:
        def start_mock(port, *extra):
            p = subprocess.Popen([sys.executable, MOCK_BACKEND, str(port), fail_control_dir, *extra],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            mock_procs.append(p)
            return p

        p_a = start_mock(BACKEND_A)
        start_mock(BACKEND_B)
        start_mock(BACKEND_FAIL)
        start_mock(BACKEND_LIMIT)
        start_mock(BACKEND_TLS,
                   os.path.join(REPO, "certs", "server.crt"),
                   os.path.join(REPO, "certs", "server.key"))
        start_mock(PROFILE_USER)
        start_mock(PROFILE_ORDERS)
        start_mock(PROFILE_RECOMMENDATIONS)
        # Wait for each mock to actually bind instead of sleeping a fixed time.
        # The server starts accepting right after this, so any proxy test that
        # runs before a backend is listening sees a refused upstream connection
        # and fails with an intermittent 502 (the backend probe connection is
        # counted, but test_connection_reuse snapshots the counter later).
        for port in (BACKEND_A, BACKEND_B, BACKEND_FAIL, BACKEND_LIMIT, BACKEND_TLS,
                     PROFILE_USER, PROFILE_ORDERS, PROFILE_RECOMMENDATIONS):
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
        test_static_file_backpressure(large_body)
        test_keepalive()
        test_pipelined_requests()
        test_trace_id()
        test_405_allow()
        test_404_403()
        test_proxy()
        test_connection_reuse(fail_control_dir)
        test_profile_composite_route(fail_control_dir)
        test_chunked_trailer()
        test_forwarded_headers()
        test_hop_by_hop_stripping()
        test_upstream_response_header_injection()
        test_no_double_gzip()
        test_composite_fanout_bounded()
        test_upstream_tls()
        test_auth()
        test_logging_policy(ws)
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
        test_companion_client()
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
