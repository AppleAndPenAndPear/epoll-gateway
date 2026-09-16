#!/bin/bash
set -e  # Exit immediately on error

BUILD_DIR="build"
BUILD_TYPE="${1:-Release}"   # The first argument is the build type, defaults to Release; for Debug run: ./build.sh Debug

echo "==> Cleaning old build files..."
if [ -d "$BUILD_DIR" ]; then
    rm -rf "${BUILD_DIR:?}"/*           # Remove all visible files/directories
    find "$BUILD_DIR" -mindepth 1 -maxdepth 1 -name '.*' ! -name '.' ! -name '..' -exec rm -rf {} +  2>/dev/null  # Remove hidden files (e.g. .cache)
fi
mkdir -p "$BUILD_DIR"

echo "==> Configuring CMake (${BUILD_TYPE}) ..."
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "==> Compiling (using $(nproc) cores) ..."
cmake --build . -j$(nproc)

echo "==> Build finished! Executables are located at $BUILD_DIR/server and $BUILD_DIR/client"
cd ..