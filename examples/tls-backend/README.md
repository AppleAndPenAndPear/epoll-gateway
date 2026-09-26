# Example: proxy to a TLS backend (upstream https://)

The gateway encrypts the hop to the backend too. This example declares an
`https://` upstream with certificate and hostname verification on, and a
second entry showing what a plaintext upstream looks like for comparison.
The mock backend runs in TLS mode with the same self-signed dev certificate.

```bash
# from the repository root; run ./scripts/gen_dev_certs.sh once if certs/ is empty
python3 tests/integration/mock_backend.py 8443 "" certs/server.crt certs/server.key &
./build/server examples/tls-backend/config.json
```

Then in another terminal:

```bash
# Verified upstream TLS: the gateway checks the backend's chain AND that the
# certificate was issued for "localhost" (tls_server_name in the config).
curl -sk https://localhost:5005/api/tls/hello

# Watch the state: backend healthy, TLS pooled connections reused across requests.
curl -s -H 'X-API-Key: admin-demo-key' http://127.0.0.1:8105/admin/upstreams
```

Verification knobs, per upstream server:

- `tls_ca_file` — CA bundle verifying the backend; empty = the system default trust store
- `tls_server_name` — SNI + hostname check; empty = verify the chain only
- `tls_skip_verify` — disable verification (lab use only)

Real backends with a public CA certificate need none of these: just write
`"host": "https://api.example.com"` and the system trust store does the rest.
