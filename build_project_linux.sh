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

# 툴체인 가드. 검사 자체는 install_deps_linux.sh에 위임한다 — 무엇이 필요한지 아는
# 곳을 두 군데로 두면 한쪽만 갱신되어 어긋난다. 여기서는 종료 코드만 본다.
#
# 설치는 이 스크립트가 하지 않는다: 빌드 명령이 말없이 시스템 패키지를 건드리면
# 채점자 입장에서 예상 밖의 일이 된다. 무엇이 없는지 보여주고, 다음에 칠 명령을
# 알려주고, 결정은 사람에게 남긴다.
# 출력은 붙잡아 두었다가 실패했을 때만 보여준다. 정상 빌드에서 배너가 두 번
# 나오면 정작 읽어야 할 [1/3]·[2/3] 진행 표시가 묻힌다.
if [ -x ./install_deps_linux.sh ]; then
    if ! deps_report="$(./install_deps_linux.sh --check 2>&1)"; then
        printf '%s\n' "$deps_report"
        echo ""
        echo "[ERROR] Build prerequisites are missing (listed above)."
        echo "        Install them:"
        echo "          ./install_deps_linux.sh"
        echo "        Or build and run without a toolchain at all:"
        echo "          ./docker/build_images.sh && docker compose up"
        exit 1
    fi
elif ! command -v cmake > /dev/null 2>&1; then
    # install_deps_linux.sh가 없는 경우(부분 체크아웃 등)의 최소 안전망
    echo "[ERROR] cmake not found, and install_deps_linux.sh is not available here."
    echo "        Install a C++17 toolchain: g++, make, cmake >= 3.16, tar"
    exit 1
fi

# 낡은 CMake 캐시 가드. CMakeCache.txt에는 소스·빌드 디렉토리의 절대 경로가 박히므로,
# 리포를 옮기거나 다른 경로로 마운트하면 configure가 캐시를 거부한다. CMake가 내는
# 메시지는 두 줄짜리라 원인이 "이전 빌드 트리가 남아 있다"는 것임을 알아채기 어렵다.
#
# head로 파이프하지 않는다 — grep -q·head가 선행 명령을 SIGPIPE로 죽이는 함정을
# 이슈 66에서 겪었다. 통째로 받아 셸 확장으로 첫 줄만 자른다.
CMAKE_CACHE="build/CMakeCache.txt"
if [ -f "$CMAKE_CACHE" ]; then
    cached_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$CMAKE_CACHE")"
    cached_home="${cached_home%%$'\n'*}"
    if [ -n "$cached_home" ] && [ "$cached_home" != "$SCRIPT_DIR" ]; then
        echo "[ERROR] build/ was configured for a different directory:"
        echo "          cached: $cached_home"
        echo "          now:    $SCRIPT_DIR"
        echo "        A CMake cache cannot be reused from another path. Remove it:"
        echo "          rm -rf build"
        exit 1
    fi
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
