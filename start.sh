#!/bin/bash
# Start the gateway (./start.sh [path/to/config.json], default config.json).
SERVER_BIN="./build/server"
if [[ ! -x "$SERVER_BIN" ]]; then
    SERVER_BIN="./bin/epollthread"
fi
if [[ ! -x "$SERVER_BIN" ]]; then
    echo "server binary not found; run ./build.sh or use a release package" >&2
    exit 1
fi

if [[ ! -f certs/server.crt || ! -f certs/server.key ]]; then
    ./scripts/gen_dev_certs.sh
fi
echo "==> Starting server..."
exec "$SERVER_BIN" "$@"
