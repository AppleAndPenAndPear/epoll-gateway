#!/usr/bin/env bash
# One-stop CI script: build + unit tests + integration tests.
# Platform-independent: can be invoked directly from local dev, GitHub Actions, Gitee Go, or any other environment.
#
# Usage:
#   ./ci.sh              # All steps (build + unit + integration)
#   ./ci.sh build        # Build only
#   ./ci.sh unit         # Unit tests only (requires a prior build)
#   ./ci.sh integration  # Integration tests only (requires a prior build)
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="build"
STEP="${1:-all}"

do_build() {
    echo "==> [1/3] Build (Release, including unit tests)"
    cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
    cmake --build "$BUILD_DIR" -j"$(nproc)"
}

do_unit() {
    echo "==> [2/3] Unit tests (CTest)"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
}

do_integration() {
    echo "==> [3/3] Integration tests (starts the real server + mock upstream)"
    # certs/ is not tracked by git; the TLS-only server (and the mock TLS
    # upstream) need a dev keypair, so generate one on demand.
    if [[ ! -f certs/server.crt || ! -f certs/server.key ]]; then
        ./scripts/gen_dev_certs.sh
    fi
    python3 tests/integration/run_integration_tests.py
}

case "$STEP" in
    build)       do_build ;;
    unit)        do_unit ;;
    integration) do_integration ;;
    all)         do_build; do_unit; do_integration ;;
    *)
        echo "Unknown step: $STEP"
        echo "Usage: $0 [build|unit|integration|all]"
        exit 1
        ;;
esac

echo "==> CI step '$STEP' completed"
