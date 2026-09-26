# Example: proxy to two backends with load balancing and health checks

Two mock backends answer on 127.0.0.1:8091 and 127.0.0.1:8092; the gateway
round-robins across them and probes them with active TCP health checks, so
killing one backend is absorbed without errors on healthy nodes.

```bash
# from the repository root; run ./scripts/gen_dev_certs.sh once if certs/ is empty
python3 tests/integration/mock_backend.py 8091 &
python3 tests/integration/mock_backend.py 8092 &
./build/server examples/load-balanced/config.json
```

Then in another terminal:

```bash
# Each response names the backend that served it — repeat and watch them alternate.
# (/demo/* avoids the built-in demo routes like /api/hello.)
curl -sk https://localhost:5005/demo/hello

# Kill one backend; round-robin skips the node the health check ejected.
kill %1
curl -sk https://localhost:5005/demo/hello   # always 127.0.0.1:8092

# Circuit-breaker and health state are visible out of band:
curl -s -H 'X-API-Key: admin-demo-key' http://127.0.0.1:8105/admin/upstreams
```

Point `servers` at your real services (any HTTP server works — Nginx, Node,
Go, Spring Boot...) to turn this into your own deployment.
