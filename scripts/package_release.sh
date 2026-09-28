#!/usr/bin/env bash
# Build a release binary and assemble a distributable tarball.
#
# Usage: ./scripts/package_release.sh [output-dir]   (default: dist/)
#
# The tarball is self-contained for glibc-based systems: the binary is
# built with an $ORIGIN rpath and libspdlog (the only non-universal
# shared dependency) is bundled under lib/. openssl/zlib stay dynamic —
# they exist on essentially every Linux. The archive contains no
# private keys; the user generates a dev certificate with
# scripts/gen_dev_certs.sh (included) or mounts their own.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT_DIR="${1:-dist}"
VERSION="$(awk '/^project\(epollthread VERSION/ {print $3}' CMakeLists.txt | tr -d ')')"
ARCH="$(uname -m)"
NAME="epollthread-${VERSION}-${ARCH}"
STAGING="${OUT_DIR:?}/${NAME}"

echo "==> Configuring release build (${VERSION}, ${ARCH})"
cmake -S . -B build-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,\$ORIGIN/../lib"
cmake --build build-release -j"$(nproc)" --target server

echo "==> Assembling ${STAGING}"
rm -rf "${STAGING}"
mkdir -p "${STAGING}/bin" "${STAGING}/lib"

install -m 755 build-release/server "${STAGING}/bin/epollthread"
cp config.json "${STAGING}/config.json"
cp -r www examples "${STAGING}/"
# Keep the scripts/ layout: gen_dev_certs.sh and start.sh both resolve the
# repo root as $(dirname $0)/.., matching the in-repo and README usage.
mkdir -p "${STAGING}/scripts"
cp scripts/gen_dev_certs.sh "${STAGING}/scripts/"
cp start.sh "${STAGING}/"
cp README.md README.zh-CN.md LICENSE "${STAGING}/"
mkdir -p "${STAGING}/docs"
cp docs/PROJECT_STATUS.md docs/CHANGELOG.md docs/BENCHMARKS.md docs/DEPLOYMENT.md "${STAGING}/docs/" 2>/dev/null || true

# Bundle libspdlog if the binary links it dynamically; $ORIGIN/../lib
# (set at configure time above) makes the binary find it there first.
if ldd "${STAGING}/bin/epollthread" | grep -q libspdlog; then
    for so in $(ldd "${STAGING}/bin/epollthread" | awk '/libspdlog/ {print $3}'); do
        cp -L "$so" "${STAGING}/lib/"
    done
    echo "    bundled: $(ls "${STAGING}/lib" | tr '\n' ' ')"
fi

# Prove every dynamic dependency resolves (bundled lib/ or system-wide).
# Behavioral correctness is CI's job; packaging only owns linkage.
echo "==> Sanity check: all shared libraries resolve"
if ldd "${STAGING}/bin/epollthread" | grep -q "not found"; then
    ldd "${STAGING}/bin/epollthread" | grep "not found"
    echo "ERROR: unresolved libraries above" >&2
    exit 1
fi
echo "    ldd clean"

echo "==> Packing ${OUT_DIR}/${NAME}.tar.xz"
tar -cJf "${OUT_DIR}/${NAME}.tar.xz" -C "${OUT_DIR}" "${NAME}"
( cd "${OUT_DIR}" && sha256sum "${NAME}.tar.xz" > "${NAME}.tar.xz.sha256" )

echo "==> Done:"
ls -lh "${OUT_DIR}"/ | grep -v '^d' | grep -v '^total'
