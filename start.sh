#!/bin/bash
# Start the gateway (./start.sh [path/to/config.json], default config.json).
if [[ ! -f certs/server.crt || ! -f certs/server.key ]]; then
    ./scripts/gen_dev_certs.sh
fi
echo "==> Starting server..."
exec ./build/server "${1:-}"
