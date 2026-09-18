#!/usr/bin/env python3
"""Mock upstream backend for integration tests.

Usage: mock_backend.py <port> <control_dir>

Behavior:
- Default: returns 200 + JSON for any request (with a backend marker used to
  assert which node was hit)
- When the <control_dir>/reject file exists: accepts the connection and reads
  the request, then closes without returning a response. The TCP health probe
  still passes (connect succeeds) but actual forwarding fails, driving the
  circuit breaker's consecutive failure count.
"""

import http.server
import json
import os
import sys
import time

PORT = int(sys.argv[1])
CONTROL_DIR = sys.argv[2] if len(sys.argv) > 2 else ""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        # Connection counter: lets integration tests assert that the gateway
        # reuses pooled keep-alive connections instead of opening one per request.
        if CONTROL_DIR:
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
        self._drain_body()
        if self._reject_requested():
            self.close_connection = True
            return
        if self.path.endswith("/slow"):
            # Slow endpoint: holds the request open so tests can exercise the
            # gateway's graceful-shutdown drain with a genuinely in-flight request.
            time.sleep(1.5)
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
        body = json.dumps({
            "backend": "127.0.0.1:%d" % PORT,
            "path": self.path,
            "method": self.command,
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


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.allow_reuse_address = True
    server.serve_forever()


if __name__ == "__main__":
    main()
