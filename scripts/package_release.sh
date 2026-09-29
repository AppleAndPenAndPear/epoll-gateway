#!/usr/bin/env bash
# Build a release binary and assemble a distributable tarball.
#
# Usage: ./scripts/package_release.sh [output-dir]   (default: dist/)
#
# Dependency policy — what is bundled and what stays system-wide:
#
#   bundled   libspdlog/libfmt and, by transitive closure, everything else
#             the binary needs that is not in the system baseline below.
#             These are the libraries whose soname is NOT stable across
#             distributions: Ubuntu 22.04 ships libfmt.so.8, 24.04 ships
#             libfmt.so.9. Bundling both from the same build host also
#             guarantees they stay ABI-compatible with each other.
#
#   system    glibc core (libc/libm/libpthread/.../ld-linux) — must match
#             the running kernel and NSS stack; bundling it is not
#             portable.
#             libssl/libcrypto/libz — this is a security product: crypto
#             must take CVE patches from the distribution rather than be
#             frozen inside the tarball. libssl.so.3 is the stable soname
#             of the whole OpenSSL 3.x line, so it is a safe dynamic
#             dependency.
#             libstdc++/libgcc_s — eliminated at link time by
#             -static-libstdc++ -static-libgcc (removes the GLIBCXX_3.4.x
#             floor); listed here only as a safety net.
#
# The archive contains no private keys and no logs; the user generates a
# dev certificate with scripts/gen_dev_certs.sh (included) or mounts their
# own.
#
# NOTE: the checks at the bottom run on the BUILD machine, which by
# definition has every library the build needs. They can prove the closure
# is complete and that the C++ runtime is static, but they cannot prove the
# result runs on a target distribution. That is what
# scripts/verify_artifact.sh (run from CI) is for.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT_DIR="${1:-dist}"
VERSION="$(awk '/^project\(epollthread VERSION/ {print $3}' CMakeLists.txt | tr -d ')')"
ARCH="$(uname -m)"
NAME="epollthread-${VERSION}-${ARCH}"
STAGING="${OUT_DIR:?}/${NAME}"
BINARY="build-release/server"

echo "==> Configuring release build (${VERSION}, ${ARCH})"
# -static-libstdc++ / -static-libgcc go through the linker flags so the
# C++ runtime is baked into the binary: an Ubuntu 22.04 build otherwise
# requires GLIBCXX_3.4.30, which 20.04's libstdc++ (3.4.28) does not have.
cmake -S . -B build-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,\$ORIGIN/../lib -static-libstdc++ -static-libgcc"
cmake --build build-release -j"$(nproc)" --target server

# True when $1 is part of the per-distribution baseline (see header).
is_system_lib() {
    case "$(basename "$1")" in
        linux-vdso.so.*|ld-linux*.so.*)                                   return 0 ;;
        libc.so.*|libm.so.*|libmvec.so.*|libpthread.so.*|libdl.so.*)      return 0 ;;
        librt.so.*|libresolv.so.*|libnsl.so.*|libutil.so.*|libcrypt.so.*) return 0 ;;
        libssl.so.*|libcrypto.so.*|libz.so.*)                             return 0 ;;
        libstdc++.so.*|libgcc_s.so.*)                                     return 0 ;;
    esac
    return 1
}

# Echo the transitive closure of non-system shared libraries required by $1,
# as absolute paths on this host. Walked breadth-first so a dependency of a
# dependency (libspdlog -> libfmt) is never missed.
library_closure() {
    declare -A seen=()
    local -a queue=("$1") out=()
    local cur lib
    while ((${#queue[@]})); do
        cur="${queue[0]}"
        queue=("${queue[@]:1}")
        while read -r lib; do
            [[ -n "$lib" ]] || continue
            is_system_lib "$lib" && continue
            [[ -n "${seen[$lib]:-}" ]] && continue
            seen["$lib"]=1
            out+=("$lib")
            queue+=("$lib")
        done < <(ldd "$cur" 2>/dev/null | awk '/=> \//{print $3}')
    done
    if ((${#out[@]})); then printf '%s\n' "${out[@]}"; fi
}

# Resolve the closure BEFORE staging: once the libraries sit in $STAGING/lib,
# the binary's $ORIGIN rpath would make ldd find them there and the checks
# below would validate themselves.
mapfile -t DEPS < <(library_closure "$BINARY")

echo "==> Assembling ${STAGING}"
rm -rf "${STAGING}"
mkdir -p "${STAGING}/bin" "${STAGING}/lib"

install -m 755 "$BINARY" "${STAGING}/bin/epollthread"
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

echo "==> Bundling non-portable shared libraries"
for dep in ${DEPS[@]+"${DEPS[@]}"}; do
    # cp -L dereferences the soname symlink, so the file lands under the
    # exact name the binary's DT_NEEDED entry asks for.
    cp -L "$dep" "${STAGING}/lib/$(basename "$dep")"
done
echo "    bundled: $(ls "${STAGING}/lib" | tr '\n' ' ')"

echo "==> Sanity check: dependency closure is complete"
fail=0

# 1. Nothing may be unresolved for the loader.
if ldd "${STAGING}/bin/epollthread" | grep -q "not found"; then
    ldd "${STAGING}/bin/epollthread" | grep "not found"
    fail=1
fi

# 2. Every non-system library of the closure must exist in lib/. This is the
#    assertion v0.2.0 lacked: it bundled libspdlog but not the libfmt that
#    libspdlog itself needs, and the packaged binary died on any
#    distribution without libfmt.so.8.
for dep in ${DEPS[@]+"${DEPS[@]}"}; do
    base="$(basename "$dep")"
    if [[ ! -e "${STAGING}/lib/${base}" ]]; then
        echo "    MISSING from lib/: ${base} (required at ${dep})"
        fail=1
    fi
done

# 3. -static-libstdc++ must really have taken effect, otherwise the binary
#    silently re-introduces the GLIBCXX_3.4.x floor the flag exists to remove.
if readelf -d "${STAGING}/bin/epollthread" | grep -q 'libstdc++'; then
    echo "    libstdc++ is still linked dynamically (GLIBCXX floor not removed)"
    fail=1
fi

# 4. The rpath that makes lib/ reachable must be embedded; without it a
#    bundled library is present but never found.
if ! readelf -d "${STAGING}/bin/epollthread" | grep -q 'ORIGIN/../lib'; then
    echo "    \$ORIGIN/../lib rpath missing from the binary"
    fail=1
fi

if [[ $fail -ne 0 ]]; then
    echo "ERROR: packaging checks failed (see above)" >&2
    exit 1
fi
echo "    closure fully bundled; C++ runtime static; rpath embedded"
echo "    (cross-distribution execution is verified by scripts/verify_artifact.sh)"

echo "==> Packing ${OUT_DIR}/${NAME}.tar.xz"
tar -cJf "${OUT_DIR}/${NAME}.tar.xz" -C "${OUT_DIR}" "${NAME}"
( cd "${OUT_DIR}" && sha256sum "${NAME}.tar.xz" > "${NAME}.tar.xz.sha256" )

echo "==> Done:"
ls -lh "${OUT_DIR}"/ | grep -v '^d' | grep -v '^total'