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
    # Keep the full output: failures are mirrored into the GitHub step
    # summary so they stay readable on the public run page.
    # Temporarily disable set -e: a failing pipeline here must not abort
    # the script before the failure-extraction block below runs.
    set +e
    ctest --test-dir "$BUILD_DIR" --output-on-failure 2>&1 | tee /tmp/ctest.log
    local rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        # Surface each failed test as a check-run annotation (::error::
        # workflow command) — readable via the API without authentication.
        # NOTE: grep may legitimately match nothing here; guard every one
        # with `|| true` because of `set -e`.
        awk '/The following tests FAILED/{f=1;next} f&&NF{print $1}' /tmp/ctest.log | head -20 | \
            while read -r t; do echo "::error title=CTest failed::$t"; done || true
        grep -E "\[  FAILED  \]" /tmp/ctest.log | head -20 | \
            while read -r line; do echo "::error title=GoogleTest::$line"; done || true
        grep -B 2 -A 12 "The following tests FAILED" /tmp/ctest.log | tail -60 > /tmp/ctest-fail.txt || true
        if [[ -s /tmp/ctest-fail.txt && -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
            {
                echo "## Unit test failures"
                echo '```'
                cat /tmp/ctest-fail.txt
                echo '```'
            } >> "$GITHUB_STEP_SUMMARY"
        fi
    fi
    return "$rc"
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
