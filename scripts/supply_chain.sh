#!/usr/bin/env bash
# Supply-chain gate: SAST + SBOM + SCA, run before a release so it cannot ship
# a known-vulnerable dependency. Invoked by `./ci.sh supply-chain` (and thus by
# GitHub Actions); it is deliberately NOT part of `./ci.sh all`, so a local
# build never starts requiring cppcheck.
#
# Usage:
#   ./scripts/supply_chain.sh             # all three gates
#   ./scripts/supply_chain.sh --sbom-only # generate build/sbom.cdx.json, no scanning
#
# Design note — every gate is honest about its inputs:
#   * A missing tool is SKIPPED with the exact install command, never a silent
#     PASS. "We did not look" must not read as "nothing was found".
#   * SAST fails only on `error` severity findings (definite bugs). The
#     `warning`/`performance`/`portability` findings are printed and counted
#     but do not fail the build, which would otherwise block on style noise in
#     an existing codebase.
#   * SBOM generation works fully offline (ldd + dpkg-query, no network).
#   * SCA needs osv-scanner and, in practice, network access to the OSV
#     database; GitHub Actions has it, a local run may not.
set -euo pipefail
cd "$(dirname "$0")/.."

SBOM_PATH="build/sbom.cdx.json"
BINARY="build/server"
SBOM_ONLY=0

case "${1:-}" in
    --sbom-only) SBOM_ONLY=1 ;;
    -h|--help)   sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "")          ;;
    *)           echo "Unknown option: $1" >&2; echo "Usage: $0 [--sbom-only]" >&2; exit 2 ;;
esac

# Project version: the CMake-generated header is the single source of truth
# once a build has run; before that, fall back to project(... VERSION ...).
project_version() {
    if [[ -f build/generated/gateway_version.h ]]; then
        awk -F'"' '/#define GATEWAY_VERSION/{print $2; exit}' build/generated/gateway_version.h
        return
    fi
    awk '/^project\(epollthread VERSION/ {print $3}' CMakeLists.txt | tr -d ')'
}

# --------------------------------------------------------------------------
# Gate 1 — SAST (cppcheck): fail only on `error` severity.
# --------------------------------------------------------------------------
SAST_STATUS=""
run_sast() {
    if [[ $SBOM_ONLY -eq 1 ]]; then SAST_STATUS="SKIPPED (--sbom-only)"; return; fi
    if ! command -v cppcheck >/dev/null 2>&1; then
        SAST_STATUS="SKIPPED (cppcheck not installed)"
        echo "    install: sudo apt-get install -y cppcheck"
        return
    fi

    local log="build/cppcheck.log" rc
    # --enable=... adds the informative categories; `error` findings are always
    # reported regardless. --quiet drops the progress chatter so the log holds
    # findings only.
    set +e
    cppcheck --std=c++17 --enable=warning,performance,portability --quiet \
        --template='{file}:{line}: {severity}: {message} [{id}]' \
        -I include/common -I include/server -I include/client -I build/generated \
        src include 2>"$log"
    rc=$?
    set -e

    # cppcheck exits 0 even when it finds issues; a non-zero exit means the
    # scan itself did not complete, which we must not mistake for "clean".
    if [[ $rc -ne 0 ]]; then
        SAST_STATUS="FAIL (cppcheck exited $rc)"
        cat "$log" >&2 || true
        return
    fi

    local errors warnings
    errors="$(grep -c ' error: ' "$log" || true)";   errors="${errors:-0}"
    warnings="$(grep -cE ' (warning|performance|portability): ' "$log" || true)"; warnings="${warnings:-0}"

    # Report non-blocking findings so they stay visible without gating.
    if [[ "$warnings" != "0" ]]; then
        echo "    $warnings non-blocking finding(s) (warning/performance/portability), first 20:"
        grep -E ' (warning|performance|portability): ' "$log" | head -20 | sed 's/^/      /' || true
    fi

    if [[ "$errors" != "0" ]]; then
        SAST_STATUS="FAIL ($errors error-severity finding(s))"
        grep ' error: ' "$log" | head -20 | sed 's/^/      /' || true
    else
        SAST_STATUS="PASS (0 errors, $warnings non-blocking)"
    fi
}

# --------------------------------------------------------------------------
# Gate 2 — SBOM (CycloneDX 1.5), fully offline.
# --------------------------------------------------------------------------
SBOM_STATUS=""
generate_sbom() {
    local version distro deps
    version="$(project_version)"
    if [[ -z "$version" ]]; then
        SBOM_STATUS="FAIL (could not determine project version)"
        return
    fi
    # purl namespace for the deb ecosystem is the distribution id.
    distro="$(awk -F= '/^ID=/{gsub(/"/,"",$2); print $2; exit}' /etc/os-release 2>/dev/null || true)"
    distro="${distro:-linux}"

    if [[ ! -x "$BINARY" ]]; then
        echo "    note: $BINARY not found — run './ci.sh build' first; SBOM will list the project component only"
    fi

    mkdir -p build
    deps="build/.sbom-deps.tsv"
    : > "$deps"

    # Each runtime shared-library dependency, resolved from ldd. Prefer the
    # dpkg-recorded version; fall back to the soname filename when the library
    # is not owned by a package (e.g. a locally built toolchain).
    if [[ -x "$BINARY" ]]; then
        local base arrow path canon pkg ver
        while read -r base arrow path; do
            [[ "$arrow" == "=>" && -n "$path" ]] || continue
            canon="$(readlink -f "$path" 2>/dev/null || echo "$path")"
            pkg="$(dpkg-query -S "$canon" 2>/dev/null | head -1 | sed 's/:.*//' || true)"
            ver=""
            [[ -n "$pkg" ]] && ver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
            if [[ -z "$ver" ]]; then
                ver="$(printf '%s' "$base" | sed -E 's/^lib.*\.so\.//')"
                pkg=""
            fi
            printf '%s\t%s\t%s\t%s\n' "$base" "$ver" "$path" "$pkg" >> "$deps"
        done < <(ldd "$BINARY" | awk '/=> \//{print $1, $2, $3}')
    fi

    if SC_VERSION="$version" SC_DISTRO="$distro" python3 - "$deps" "$SBOM_PATH" <<'PY'
import datetime, json, os, sys, urllib.parse

deps_file, out = sys.argv[1], sys.argv[2]
version = os.environ["SC_VERSION"]
distro = os.environ.get("SC_DISTRO") or "linux"

project_ref = "pkg:generic/epollthread@" + urllib.parse.quote(version, safe=".")

components = []
dep_refs = []
with open(deps_file) as handle:
    for line in handle:
        line = line.rstrip("\n")
        if not line:
            continue
        name, lib_version, path, package = line.split("\t")
        ref = "lib:" + name
        comp = {"type": "library", "bom-ref": ref, "name": name, "version": lib_version}
        if package:
            comp["purl"] = "pkg:deb/{}/{}@{}".format(
                distro, package, urllib.parse.quote(lib_version, safe=""))
        else:
            comp["purl"] = "pkg:generic/{}@{}".format(
                urllib.parse.quote(name, safe="."), urllib.parse.quote(lib_version, safe="."))
        props = [{"name": "gateway:resolved-path", "value": path}]
        if package:
            props.append({"name": "gateway:dpkg-package", "value": package})
        comp["properties"] = props
        components.append(comp)
        dep_refs.append(ref)

sbom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.5",
    "version": 1,
    "metadata": {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tools": [{"vendor": "epollthread", "name": "supply_chain.sh", "version": "1"}],
        "component": {
            "type": "application",
            "bom-ref": project_ref,
            "name": "epollthread",
            "version": version,
        },
    },
    "components": components,
    "dependencies": [{"ref": project_ref, "dependsOn": dep_refs}],
}

with open(out, "w") as handle:
    json.dump(sbom, handle, indent=2)
    handle.write("\n")
print("    wrote {} component(s) to {}".format(len(components), out))
PY
    then
        rm -f "$deps"
        # The SBOM is consumed by osv-scanner and by humans; prove it parses.
        if python3 -m json.tool "$SBOM_PATH" >/dev/null 2>&1; then
            SBOM_STATUS="PASS ($SBOM_PATH)"
        else
            SBOM_STATUS="FAIL (emitted JSON does not parse)"
        fi
    else
        rm -f "$deps"
        SBOM_STATUS="FAIL (SBOM generation errored)"
    fi
}

# --------------------------------------------------------------------------
# Gate 3 — SCA (osv-scanner over the SBOM).
# --------------------------------------------------------------------------
SCA_STATUS=""
run_sca() {
    if [[ $SBOM_ONLY -eq 1 ]]; then SCA_STATUS="SKIPPED (--sbom-only)"; return; fi
    if [[ "$SBOM_STATUS" != PASS* ]]; then
        SCA_STATUS="SKIPPED (no SBOM from gate 2)"
        return
    fi
    if ! command -v osv-scanner >/dev/null 2>&1; then
        SCA_STATUS="SKIPPED (osv-scanner not installed)"
        echo "    install: https://github.com/google/osv-scanner/releases (single static binary)"
        return
    fi
    if osv-scanner --sbom "$SBOM_PATH"; then
        SCA_STATUS="PASS"
    else
        SCA_STATUS="FAIL (vulnerabilities reported)"
    fi
}

echo "==> Supply-chain gate"
[[ $SBOM_ONLY -eq 1 ]] && echo "    (--sbom-only: SAST and SCA are not run)"

generate_sbom
run_sast
run_sca

echo "==> Summary"
printf '    %-26s %s\n' "SAST (cppcheck)"      "$SAST_STATUS"
printf '    %-26s %s\n' "SBOM (CycloneDX 1.5)" "$SBOM_STATUS"
printf '    %-26s %s\n' "SCA (osv-scanner)"    "$SCA_STATUS"

# Any FAIL (not SKIPPED) blocks the gate: a release must not ship on an
# unverified or vulnerable dependency set.
rc=0
for status in "$SAST_STATUS" "$SBOM_STATUS" "$SCA_STATUS"; do
    [[ "$status" == FAIL* ]] && rc=1
done
if [[ $rc -ne 0 ]]; then
    echo "ERROR: supply-chain gate failed" >&2
fi
exit "$rc"
