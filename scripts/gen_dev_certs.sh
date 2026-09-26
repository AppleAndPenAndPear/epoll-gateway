#!/usr/bin/env bash
# Generate a self-signed development certificate for the gateway.
#
# The data plane is TLS-only and aborts at startup without a keypair, so
# both `./ci.sh` (integration tests) and the Docker quickstart need one.
# These certificates are NOT tracked by git (certs/ is in .gitignore) and
# are for development only — never use them in production.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p certs
# SAN is DNS:localhost only — deliberately no IP SANs, so IP-literal
# connections still fail hostname verification (the integration suite
# relies on that to prove the check is really enabled).
openssl req -x509 -newkey rsa:2048 -sha256 -days 825 -nodes \
    -keyout certs/server.key -out certs/server.crt \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" \
    >/dev/null 2>&1
chmod 600 certs/server.key

echo "==> Development certificate written to certs/server.crt"
echo "    (CN=localhost, SAN: localhost; valid 825 days)"
