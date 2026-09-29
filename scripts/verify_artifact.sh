#!/usr/bin/env bash
# Prove a packaged tarball actually runs on the distributions we support.
#
# Usage: ./scripts/verify_artifact.sh dist/epollthread-<version>-<arch>.tar.xz
#
# Why this exists: package_release.sh's ldd gate and the CI smoke test both
# run on the BUILD machine, which by definition has every library the build
# needs — a check that cannot fail for the very bug it is meant to catch.
# v0.2.0 shipped an x86_64 tarball linking libfmt.so.8 (Ubuntu 22.04) that
# died instantly on Ubuntu 24.04 (libfmt.so.9 only):
#
#     ./bin/epollthread: error while loading shared libraries:
#     libfmt.so.8: cannot open shared object file
#
# Executing the artifact in a clean container of each supported distribution
# is the only check that detects that class of failure. The images are
# deliberately minimal: they have a C library, a loader, bash and coreutils
# and nothing else, so a missing bundle cannot be papered over.
#
# Requires: docker (the container's architecture must match the tarball's).
set -euo pipefail
cd "$(dirname "$0")/.."

TARBALL="${1:?usage: $0 <tarball.tar.xz>}"
TARBALL="$(readlink -f "$TARBALL")"
[[ -f "$TARBALL" ]] || { echo "ERROR: no such tarball: ${1}" >&2; exit 1; }

NAME="$(basename "$TARBALL" .tar.xz)"
PLATFORM="$(uname -m)"

# Debian 12 covers the same glibc generation as Ubuntu 22.04 from a different
# package lineage, so it catches a bundle that only happens to exist on the
# distribution the build ran on.
IMAGES=(ubuntu:22.04 ubuntu:24.04 debian:12)

command -v docker >/dev/null || { echo "ERROR: docker is required to verify a tarball" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
tar -xJf "${TARBALL}" -C "${WORK}"
[[ -d "${WORK}/${NAME}" ]] || { echo "ERROR: ${TARBALL} did not contain ${NAME}/" >&2; exit 1; }

# The data plane is TLS-only and aborts at startup without a keypair. certs/
# is gitignored, so generate one on the host (openssl lives here, not
# necessarily in the minimal images) and mount it read-only.
if [[ ! -f certs/server.crt || ! -f certs/server.key ]]; then
    ./scripts/gen_dev_certs.sh
fi

echo "==> Verifying ${NAME} (${PLATFORM}) on: ${IMAGES[*]}"
fail=0
for img in "${IMAGES[@]}"; do
    echo "--- ${img}"
    if docker run --rm \
            -e PKG="/src/${NAME}" \
            -v "${WORK}:/src:ro" \
            -v "${PWD}/certs:/certs:ro" \
            "${img}" bash -euc '
        # Run from a writable copy: the mounted source tree is read-only and
        # the run test needs to drop a certs/ next to config.json.
        cp -a "$PKG" /tmp/run
        cd /tmp/run
        mkdir -p certs
        cp /certs/server.crt /certs/server.key certs/

        # 1. Loader resolution against THIS distribution: the decisive check.
        ldd ./bin/epollthread
        if ldd ./bin/epollthread | grep -q "not found"; then
            echo "FAIL: unresolved shared libraries on this distribution" >&2
            exit 1
        fi

        # 2. Actually start it and prove the listener comes up. A raw TCP
        #    connect is enough: it means the binary loaded, the TLS context
        #    built and the socket is accepting.
        ./bin/epollthread > /tmp/server.log 2>&1 &
        pid=$!
        ok=1
        for _ in $(seq 1 60); do
            if (exec 3<>/dev/tcp/127.0.0.1/5005) 2>/dev/null; then ok=0; break; fi
            if ! kill -0 "$pid" 2>/dev/null; then break; fi
            sleep 0.25
        done
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        if [ "$ok" -ne 0 ]; then
            echo "FAIL: server never accepted a connection; log follows" >&2
            cat /tmp/server.log >&2
            exit 1
        fi
        echo "OK: loads and serves on this distribution"
    '; then
        echo "    ${img}: OK"
    else
        echo "    ${img}: FAILED" >&2
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo "ERROR: ${NAME} does not run on every supported distribution" >&2
    exit 1
fi
echo "==> ${NAME} verified on all ${#IMAGES[@]} distributions"