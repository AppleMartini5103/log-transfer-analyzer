#!/bin/bash

# Project build script (Linux) — README "Build" section entry point.
# 1) builds 3rdparty libraries from tarballs if missing
# 2) configures + builds with CMake
# 3) runs unit tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "log-transfer-analyzer Build (Linux)"
echo "========================================"

echo "[1/3] Checking 3rdparty libraries..."
for lib in libuv catch2; do
    if [ ! -d "3rdparty/$lib/include" ]; then
        echo "  3rdparty/$lib not built yet — running its build script..."
        (cd "3rdparty/$lib" && ./build_linux.sh)
    else
        echo "  3rdparty/$lib: OK"
    fi
done

echo "[2/3] Configuring and building (CMake, Release)..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "[3/3] Running unit tests..."
ctest --test-dir build --output-on-failure

echo ""
echo "========================================"
echo "[SUCCESS] Build and tests completed!"
echo "========================================"
echo "  Server binary: build/server/server"
echo "  Unit tests:    build/tests/unit_tests"
