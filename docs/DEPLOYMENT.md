# Deployment and Upgrade Guide

How to run the gateway as a managed service, verify it, and upgrade it without dropping requests.

> Companion documents: [PROJECT_STATUS.md](PROJECT_STATUS.md) (current capability snapshot), [CHANGELOG.md](CHANGELOG.md) (release history), [BENCHMARKS.md](BENCHMARKS.md) (capacity baseline).

---

## 1. Build and Install

```bash
./build.sh                          # Release build → build/server
sudo cp build/server /usr/local/bin/epoll-gateway
sudo useradd -r -s /usr/sbin/nologin epoll-gateway
sudo mkdir -p /var/lib/epoll-gateway/logs
```

The binary is statically self-contained apart from OpenSSL/spdlog/zlib shared libraries (install `libssl`, `zlib` via the system package manager). A single binary, no etcd/Postgres/runtime.

## 2. Files Layout

| Path | Purpose |
|---|---|
| `/var/lib/epoll-gateway/config.json` | Main config (routes, upstreams, TLS paths, admin section) |
| `/var/lib/epoll-gateway/api_keys.json` | API keys — kept out of the main config |
| `/var/lib/epoll-gateway/certs/` | TLS certificate + private key |
| `/var/lib/epoll-gateway/logs/` | Access/AUDIT/structured logs |

Secrets precedence: `GW_API_KEYS` env > `api_keys_file` > inline `api_keys`.

## 3. Minimal Production Config

```json
{
  "port": 5005,
  "num_workers": 4,
  "keepalive_timeout": 60,
  "shutdown_drain_timeout": 30,
  "upstreams": {
    "api": {"servers": [{"host": "10.0.0.1", "port": 8080},
                         {"host": "10.0.0.2", "port": 8080}]}
  },
  "routes": [
    {"name": "api", "method": "GET", "path": "/api/*", "target_type": "upstream",
     "upstream_target": {"name": "api", "timeout_ms": 5000},
     "auth_required": true, "allowed_api_keys": ["..."]}
  ],
  "tls": {"cert_path": "certs/server.crt", "key_path": "certs/server.key"},
  "api_keys_file": "api_keys.json",
  "admin": {
    "enabled": true,
    "port": 8105,
    "bind": "127.0.0.1",
    "api_keys": ["choose-a-long-random-admin-key"]
  }
}
```

Key points:

- `admin.enabled` exposes the operations API on a **separate listener** (never the data-plane port). Keep `bind` on `127.0.0.1` (or an mgmt-network address) and require a dedicated key — admin access is a full control-plane capability.
- `shutdown_drain_timeout` is the graceful-shutdown drain budget; keep it below systemd's `TimeoutStopSec`.

## 4. systemd Service

```bash
sudo cp deploy/gateway.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now epoll-gateway
```

Everyday operations:

```bash
systemctl status epoll-gateway
systemctl reload epoll-gateway      # SIGHUP: hot reload config + TLS cert
systemctl restart epoll-gateway     # graceful: SIGTERM, drains in-flight requests
journalctl -u epoll-gateway -f
```

## 5. Health, Readiness and Version

| Endpoint | Meaning | Use |
|---|---|---|
| `GET /healthz` | Liveness — process and event loop are up | k8s/systemd liveness, LB "up" checks |
| `GET /readyz` | Readiness — every configured upstream has ≥1 healthy backend | LB pool membership, rolling deploys |
| `GET /version` | Build version JSON | Fleet inventory, upgrade verification |

`/readyz` returns `200` with a per-upstream healthy/total breakdown, or `503` when an upstream has no healthy backend. Probe endpoints are exempt from rate limiting and authentication by design.

## 6. Admin API

Plain HTTP on the admin listener; every endpoint requires `X-API-Key: <admin key>` (or `Authorization: Bearer`). Auth failures are AUDIT logged.

```bash
# Request counters + latency totals + uptime + version
curl -s -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/stats

# Per-backend health, failure counts and circuit-breaker state
curl -s -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/upstreams

# Hot reload: validates config.json first; invalid config -> 400, nothing applied.
# Valid config -> 200 "reload_triggered"; workers apply it within ~1 second.
curl -s -X POST -H "X-API-Key: $ADMIN_KEY" http://127.0.0.1:8105/admin/reload
```

The admin listener binds `127.0.0.1` by default. To expose it remotely, terminate TLS in a reverse proxy bound to a management interface, or use an SSH tunnel — do not put the admin API on the public data-plane address.

Note on the `admin` section: `api_keys` **are** hot-reloadable — after a reload (SIGHUP or `POST /admin/reload`) the admin listener adopts the new keys within ~1 second, so key rotation needs no restart. The listener topology (`enabled`, `port`, `bind`) is fixed at startup and requires `systemctl restart epoll-gateway` to change: setting `enabled: false` in a reloaded config stops the data-plane reload but leaves the admin listener running with its previous keys (logged as AUDIT `admin_keys_unchanged`). A reload whose config is invalid, or whose `admin.api_keys` list is empty, keeps the previous keys (the admin listener never locks itself out). Everything else (routes, upstreams, data-plane API keys, rate limits, TLS cert) reloads live.

## 7. Rolling Upgrade (zero-downtime, single instance)

1. **Check readiness**: `GET /readyz` → 200 (a gateway that would fail probing should not be restarted).
2. **Install the new binary**: build/`scp` the new `epoll-gateway` to `/usr/local/bin/epoll-gateway.new`, verify with `epoll-gateway.new`-style smoke tests (or a second instance on another port).
3. **Drain and restart**: `systemctl restart epoll-gateway`. systemd sends SIGTERM; the gateway:
   - stops accepting new connections,
   - closes idle keep-alive connections immediately (clients reconnect),
   - finishes in-flight requests (up to `shutdown_drain_timeout` seconds),
   - exits; systemd starts the new binary.
4. **Verify**: `GET /healthz` → 200, `GET /version` shows the new version, `GET /admin/upstreams` shows all backends healthy, error-rate metrics stable.

Load balancer guidance: remove the instance from the pool (or rely on `/readyz`) before restart, and re-add it after `healthz`/`readyz` recover.

## 8. Config and Certificate Hot Reload

Both SIGHUP and `POST /admin/reload`:

- validate the new config **before** applying; an invalid config is rejected with an AUDIT log (`config_reload_rejected` / `admin_reload_rejected`) and the old config keeps running;
- apply routes, upstreams, API keys, rate-limit defaults, keep-alive timeouts and TLS cert/key atomically at worker safe points (within ~1 s);
- keep the old TLS context alive for in-flight connections when cert reload fails.

## 9. Monitoring Checklist

- Prometheus scrape: `GET /metrics` (counters, latency histogram, per-route, upstream errors, cache stats).
- Confirm `/readyz` is what your LB probes — not `/healthz` — so a gateway with all backends down leaves the pool instead of failing client traffic.
- AUDIT log lines (`AUDIT ...`) carry trace_id, route, status, failure reason, masked keys — wire into your SIEM.
- Baseline capacity numbers: [BENCHMARKS.md](BENCHMARKS.md).
