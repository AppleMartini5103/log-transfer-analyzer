#!/bin/bash

# 두 이미지를 순서대로 빌드한다 — base 다음 app.
#
#   ./docker/build_images.sh                 base가 없으면 만들고, app을 빌드
#   ./docker/build_images.sh --rebuild-base  base까지 강제로 다시 빌드
#
# 이 래퍼가 있는 이유는 순서가 강제되기 때문이다. app 이미지는 FROM log-server-base로
# base를 참조하므로, 모르고 app부터 빌드하면 "log-server-base:latest not found"로
# 죽는다. README를 먼저 읽지 않은 사람이 정확히 밟는 함정이라 명령으로 감싼다.
#
# base를 기본적으로 재사용하는 이유: base에는 툴체인과 3rdparty(libuv·Catch2를
# tarball에서 빌드) 가 들어 있어 몇 분이 걸리지만 거의 바뀌지 않는다. 서버 코드를
# 고쳤을 때 다시 빌드해야 하는 것은 app뿐이다 — 2단 구성의 이유가 이것이다.
#
# 빌드가 끝나면 실행은 compose가 맡는다:  docker compose up

set -eo pipefail

BASE_IMAGE="log-server-base:latest"
APP_IMAGE="log-server-app:latest"

REBUILD_BASE=0
for arg in "$@"; do
    case "$arg" in
        --rebuild-base) REBUILD_BASE=1 ;;
        -h|--help)
            echo "Usage: ./docker/build_images.sh [--rebuild-base]"
            echo "  Builds ${BASE_IMAGE} (if absent) then ${APP_IMAGE}."
            echo "  --rebuild-base  rebuild the base image even if it exists"
            exit 0 ;;
        *)
            echo "[ERROR] Unknown argument: $arg (try --help)" >&2
            exit 1 ;;
    esac
done

# 리포지토리 루트에서 실행되어야 한다: 두 dockerfile 모두 빌드 컨텍스트를 루트로 잡고
# COPY 3rdparty/... 처럼 루트 기준 경로를 쓴다. 호출 위치에 의존하지 않도록 직접 옮긴다.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

if ! command -v docker > /dev/null 2>&1; then
    echo "[ERROR] docker not found in PATH."
    echo "        Install Docker Engine, or build on the host instead:"
    echo "          ./install_deps_linux.sh && ./build_project_linux.sh"
    exit 1
fi

echo "========================================"
echo "log-transfer-analyzer Docker Images"
echo "========================================"

# --- base ---------------------------------------------------------------
# 이미지 존재 확인은 inspect로 한다. `docker images -q`는 없을 때도 0으로 끝나
# 조건문에서 구분이 되지 않는다.
if [ "$REBUILD_BASE" -eq 1 ] || ! docker image inspect "$BASE_IMAGE" > /dev/null 2>&1; then
    echo "[1/2] Building ${BASE_IMAGE} (toolchain + 3rdparty; takes a few minutes)..."
    docker build -t "$BASE_IMAGE" -f docker/log-server-base.dockerfile .
else
    echo "[1/2] ${BASE_IMAGE}: already present (pass --rebuild-base to force)"
fi

# --- app ----------------------------------------------------------------
# 이 단계가 build_project_linux.sh를 돌리므로 유닛 테스트가 함께 실행된다.
# 테스트가 실패하면 이미지가 만들어지지 않는다.
echo "[2/2] Building ${APP_IMAGE} (server build + unit tests)..."
docker build -t "$APP_IMAGE" -f docker/log-server-app.dockerfile .

echo ""
echo "========================================"
echo "[SUCCESS] Images built."
echo "========================================"
echo "  Run:  docker compose up"
echo "  Artifacts (result.csv, skip_report.txt) will appear in ./out"
