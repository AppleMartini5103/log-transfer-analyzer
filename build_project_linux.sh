#!/bin/bash

# Project build script (Linux) — README "Build" section entry point.
# 1) builds 3rdparty libraries from tarballs if missing
# 2) configures + builds with CMake
# 3) runs unit tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 환경 가드: 이 스크립트는 Linux 서버를 빌드한다. Windows의 Git Bash에서도
# 실행 자체는 되기 때문에, 엉뚱한 컴파일 에러 대신 원인을 한 줄로 알려준다.
OS_NAME="$(uname -s)"
if [ "$OS_NAME" != "Linux" ]; then
    echo "[ERROR] This script builds the Linux server and requires Linux."
    echo "        Detected OS: $OS_NAME"
    echo "        On Windows, build the client instead:"
    echo "          build_project_window.bat  (from \"x64 Native Tools Command Prompt for VS\")"
    exit 1
fi

# 아키텍처는 경고만 — libuv/Catch2를 소스에서 빌드하므로 x86_64 외에서도 동작 가능하다.
# 다만 검증된 구성이 x86_64뿐이므로 알린다.
ARCH_NAME="$(uname -m)"
if [ "$ARCH_NAME" != "x86_64" ]; then
    echo "[WARN] Only x86_64 has been verified; detected $ARCH_NAME. Continuing..."
fi

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
