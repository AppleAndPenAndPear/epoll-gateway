#!/usr/bin/env python3
"""Mock upstream backend for integration tests.

Usage: mock_backend.py <port> <control_dir> [cert key]

Behavior:
- Default: returns 200 + JSON for any request (with a backend marker used to
  assert which node was hit)
- When the <control_dir>/reject file exists: accepts the connection and reads
  the request, then closes without returning a response. The TCP health probe
  still passes (connect succeeds) but actual forwarding fails, driving the
  circuit breaker's consecutive failure count.
- With a cert/key pair: serves TLS (for the gateway's https-upstream tests).
"""

import http.server
import json
import os
import socket
import sys
import time

PORT = int(sys.argv[1])
CONTROL_DIR = sys.argv[2] if len(sys.argv) > 2 else ""
CERT_PATH = sys.argv[3] if len(sys.argv) > 4 else ""
KEY_PATH = sys.argv[4] if len(sys.argv) > 4 else ""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        # Connection counting is deferred to the first request actually served
        # on this connection (see handle_any). The gateway's active health
        # check opens a bare TCP connection to every upstream every second and
        # never sends a request; counting in setup() would let that background
        # probing inflate the counter and make the connection-reuse assertion
        # flaky whenever a test run straddles a health-check tick.
        self._counted = False

    def _count_connection_once(self):
        if self._counted or not CONTROL_DIR:
            return
        self._counted = True
        try:
            with open(os.path.join(CONTROL_DIR, "conn_count"), "a") as f:
                f.write("1\n")
        except OSError:
            pass

    def _drain_body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length > 0:
            self.rfile.read(length)

    def _reject_requested(self):
        return bool(CONTROL_DIR) and os.path.exists(os.path.join(CONTROL_DIR, "reject"))

    def handle_any(self):
        self._count_connection_once()
        self._drain_body()
        if self._reject_requested():
            self.close_connection = True
            return
        if self.path.endswith("/slow"):
            # Slow endpoint: holds the request open so tests can exercise the
            # gateway's graceful-shutdown drain with a genuinely in-flight request.
            time.sleep(1.5)
        if self.path.endswith("/gzip"):
            # Already-gzipped upstream response. The gateway must pass it
            # through untouched (a second gzip pass would corrupt the body).
            import gzip
            payload = b"gateway-gzip-test-payload-" * 20
            gz = gzip.compress(payload)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(gz)))
            self.end_headers()
            self.wfile.write(gz)
            return
        if self.path.endswith("/inject"):
            # Raw response whose header value carries a bare LF. The gateway's
            # CRLF-delimited header parser preserves it, so re-emitting the
            # header verbatim would inject a second header toward the client.
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/plain\r\n"
                b"X-Inject: a\nX-Evil: 1\r\n"
                b"Content-Length: 2\r\n"
                b"\r\n"
                b"ok"
            )
            return
        if self.path.endswith("/hopbyhop"):
            # Response whose Connection header names another field, plus
            # Proxy-* and a benign header. Hop-by-hop fields must be consumed
            # by the gateway, the benign one preserved.
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/plain\r\n"
                b"Connection: X-Internal\r\n"
                b"X-Internal: secret\r\n"
                b"Proxy-Authenticate: Basic realm=x\r\n"
                b"X-Keep: visible\r\n"
                b"Content-Length: 3\r\n"
                b"\r\n"
                b"hop"
            )
            return
        if self.path.endswith("/chunked-trailer"):
            # Raw chunked response with a trailer section, to exercise the
            # gateway's chunked framing end-to-end. Keep-alive stays intact.
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/plain\r\n"
                b"Trailer: X-Sum\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"\r\n"
                b"3\r\nabc\r\n"
                b"2\r\nde\r\n"
                b"0\r\nX-Sum: 5\r\n"
                b"\r\n"
            )
            return
        if self.path.endswith("/drip"):
            # Slowly-streaming chunked response: one chunk every ~100ms for
            # ~1.5s. Each chunk is I/O progress, so a gateway exchange's idle
            # deadline keeps refreshing; only a composite route's hard overall
            # deadline can bound the wait.
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: text/plain\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"\r\n"
            )
            for _ in range(15):
                self.wfile.write(b"5\r\nhello\r\n")
                self.wfile.flush()
                time.sleep(0.1)
            self.wfile.write(b"0\r\n\r\n")
            return
        body = json.dumps({
            "backend": "127.0.0.1:%d" % PORT,
            "path": self.path,
            "method": self.command,
            # Echoed back so tests can assert the gateway injected the caller
            # identity on the upstream hop (X-Forwarded-For / X-Real-IP).
            "xff": self.headers.get("X-Forwarded-For", ""),
            "real_ip": self.headers.get("X-Real-IP", ""),
            # Full received header set (lowercased) so tests can assert which
            # headers the gateway actually forwarded on the upstream hop.
            "headers": {k.lower(): v for k, v in self.headers.items()},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = handle_any
    do_POST = handle_any
    do_PUT = handle_any
    do_DELETE = handle_any
    do_HEAD = handle_any

    def log_message(self, *args):
        pass


class _NodelayHTTPServer(http.server.ThreadingHTTPServer):
    # A response is written as two small segments (headers, then body). With
    # Nagle enabled the second segment waits for the client's delayed ACK, which
    # costs a fixed ~40 ms on every proxied response and makes any benchmark of
    # the gateway's proxy path measure this fixture, not the gateway. There is
    # no stdlib "disable_nagle_algorithm" attribute, so set TCP_NODELAY on each
    # accepted socket explicitly.
    # The stdlib default request_queue_size of 5 overflows the listen backlog
    # under benchmark concurrency (c50): SYNs are dropped, connects time out,
    # and the gateway's health probe marks a perfectly alive backend as down.
    request_queue_size = 128

    def get_request(self):
        conn, addr = super().get_request()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return conn, addr


def main():
    server = _NodelayHTTPServer(("127.0.0.1", PORT), Handler)
    server.allow_reuse_address = True
    if CERT_PATH and KEY_PATH:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(CERT_PATH, KEY_PATH)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
