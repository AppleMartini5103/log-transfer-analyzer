#!/bin/bash

# 의존성 설치 스크립트 (Linux) — README "Build" 섹션의 선행 단계.
#
# 필요한 것은 두 갈래이고, 무게가 다르다.
#   빌드용: g++ · make · cmake(3.16+) · tar  ← 깨끗한 머신에 실제로 없는 쪽
#   실행용: libstdc++.so.6                   ← 대부분의 배포판에 이미 있다
#           libuv는 3rdparty 번들 tarball에서 빌드되므로 시스템 패키지가 필요 없다.
#           (루트 CMakeLists.txt가 번들 경로를 하드코딩한다 — 시스템 libuv를 깔아도
#            빌드에 쓰이지 않으므로 여기서 설치하지 않는다.)
#
# 설치할 게 없으면 조용히 0으로 끝난다 — 여러 번 돌려도 안전하다.
# 남의 머신을 말없이 건드리지 않는 것을 원칙으로 한다: 기본 동작은 "명령을 보여주고
# 확인받기"이고, 거절하거나 배포판을 못 알아보면 아무것도 하지 않고 물러난다.
#
# set -u는 일부러 켜지 않는다. 빈 배열에 대한 ${arr[@]} 전개가 bash 4.4 미만에서
# 에러가 되므로(CentOS 7 등), 넓은 배포판을 상대하는 스크립트에서는 위험이 이득보다 크다.

set -eo pipefail

MIN_CMAKE="3.16"

usage() {
    cat <<'USAGE'
Usage: ./install_deps_linux.sh [--check | --yes]

  (no option)  Check what is missing, print the install commands, ask before running them
  --yes, -y    Install without asking (non-interactive / CI)
  --check      Check only; never installs anything.
               Exits 1 if something is missing, 0 if nothing is - so a build
               script can branch on it without duplicating the detection.
  -h, --help   Show this message

Prefer not to touch the system at all? Use Docker instead: docker compose up --build
USAGE
}

ASSUME_YES=0
CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y)  ASSUME_YES=1 ;;
        --check)   CHECK_ONLY=1 ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "[ERROR] Unknown argument: $arg" >&2
            usage >&2
            exit 1 ;;
    esac
done

if [ "$(uname -s)" != "Linux" ]; then
    echo "[ERROR] This script installs the Linux server's dependencies and requires Linux."
    echo "        Detected OS: $(uname -s)"
    echo "        On Windows the client needs Visual Studio 2019+ with the C++ workload;"
    echo "        build_project_window.bat enters the MSVC environment on its own."
    exit 1
fi

echo "========================================"
echo "log-transfer-analyzer Dependencies (Linux)"
echo "========================================"

# ── 1. 무엇이 없는지 검사 ──────────────────────────────────────────────────
# 패키지 관리자에게 묻지 않고 "실제로 쓸 수 있는가"로 판정한다. 소스에서 직접 깔았거나
# 배포판마다 패키지 이름이 다른 경우까지 통과시키기 위함이다.

missing_list=""
need_compiler=0
need_make=0
need_cmake=0
need_tar=0
need_libstdcxx=0

note_missing() {
    missing_list="${missing_list}         - $1
"
}

if ! command -v g++ >/dev/null 2>&1; then
    need_compiler=1
    note_missing "g++ (C++17 compiler)"
fi
if ! command -v make >/dev/null 2>&1; then
    need_make=1
    note_missing "make"
fi
if ! command -v tar >/dev/null 2>&1; then
    need_tar=1
    note_missing "tar (the 3rdparty libraries ship as tarballs)"
fi

if command -v cmake >/dev/null 2>&1; then
    # "있다"로 끝내지 않는다 — 버전이 낮으면 configure 단계에서 실패한다.
    #
    # ★ head로 파이프하지 않는다. head는 첫 줄을 읽고 즉시 종료하므로 앞쪽 명령이
    #   SIGPIPE로 죽고, set -o pipefail이 그 실패를 파이프라인 전체의 실패로 올려
    #   set -e가 스크립트를 끝내 버린다. 먼저 통째로 받아 놓고 셸 확장으로 자른다.
    cmake_raw="$(cmake --version 2>/dev/null || true)"
    cmake_line="${cmake_raw%%$'\n'*}"
    have_cmake="$(printf '%s' "$cmake_line" | awk '{print $3}')"

    # sort -V로 비교해야 3.9 < 3.16 을 올바로 판정한다 (문자열 비교는 반대로 나온다)
    sorted="$(printf '%s\n%s\n' "$MIN_CMAKE" "$have_cmake" | sort -V)"
    lowest="${sorted%%$'\n'*}"
    if [ "$lowest" != "$MIN_CMAKE" ]; then
        need_cmake=1
        note_missing "cmake >= $MIN_CMAKE (found $have_cmake)"
    fi
else
    need_cmake=1
    note_missing "cmake >= $MIN_CMAKE"
fi

# 실행용. 헤더가 아니라 런타임 .so가 캐시에 있는지가 기준이다.
# 위와 같은 이유로 grep -q에 파이프하지 않는다 — 캐시를 통째로 받아 문자열로 본다
# (실측: `ldconfig -p | grep -q` 는 libstdc++이 멀쩡히 있는 머신에서도 실패했다).
if command -v ldconfig >/dev/null 2>&1; then
    ld_cache="$(ldconfig -p 2>/dev/null || true)"
    case "$ld_cache" in
        *"libstdc++.so.6"*) ;;
        *)
            need_libstdcxx=1
            note_missing "libstdc++.so.6 (needed to run the server)"
            ;;
    esac
fi

if [ -z "$missing_list" ]; then
    echo "[OK] Everything needed is already installed."
    echo "     Next: ./build_project_linux.sh"
    exit 0
fi

echo "[INFO] Missing:"
printf '%s' "$missing_list"
echo ""

# ── 2. 배포판별 설치 명령 조립 ─────────────────────────────────────────────
# ID로 먼저 맞추고, 못 맞추면 ID_LIKE로 계열을 본다 (Linux Mint → ubuntu,
# Rocky/Alma → rhel fedora 등). /etc/os-release는 systemd 계열의 표준 위치다.

distro_id=""
distro_like=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    distro_id="${ID}"
    distro_like="${ID_LIKE}"
fi

pkgs=""
install_cmd=""
update_cmd=""

case " $distro_id $distro_like " in
    *" debian "*|*" ubuntu "*)
        update_cmd="apt-get update"
        install_cmd="apt-get install -y"
        # build-essential 하나가 g++·make·libc 개발 파일을 모두 끌어온다
        if [ "$need_compiler" -eq 1 ] || [ "$need_make" -eq 1 ]; then pkgs="$pkgs build-essential"; fi
        if [ "$need_cmake"     -eq 1 ]; then pkgs="$pkgs cmake"; fi
        if [ "$need_tar"       -eq 1 ]; then pkgs="$pkgs tar"; fi
        if [ "$need_libstdcxx" -eq 1 ]; then pkgs="$pkgs libstdc++6"; fi
        ;;
    *" fedora "*|*" rhel "*|*" centos "*)
        if command -v dnf >/dev/null 2>&1; then
            install_cmd="dnf install -y"
        else
            install_cmd="yum install -y"
        fi
        if [ "$need_compiler"  -eq 1 ]; then pkgs="$pkgs gcc-c++"; fi
        if [ "$need_make"      -eq 1 ]; then pkgs="$pkgs make"; fi
        if [ "$need_cmake"     -eq 1 ]; then pkgs="$pkgs cmake"; fi
        if [ "$need_tar"       -eq 1 ]; then pkgs="$pkgs tar"; fi
        if [ "$need_libstdcxx" -eq 1 ]; then pkgs="$pkgs libstdc++"; fi
        ;;
    *" arch "*)
        install_cmd="pacman -S --needed --noconfirm"
        if [ "$need_compiler" -eq 1 ]; then pkgs="$pkgs gcc"; fi
        if [ "$need_make"     -eq 1 ]; then pkgs="$pkgs make"; fi
        if [ "$need_cmake"    -eq 1 ]; then pkgs="$pkgs cmake"; fi
        if [ "$need_tar"      -eq 1 ]; then pkgs="$pkgs tar"; fi
        ;;
    *" suse "*|*" opensuse "*)
        install_cmd="zypper install -y"
        if [ "$need_compiler"  -eq 1 ]; then pkgs="$pkgs gcc-c++"; fi
        if [ "$need_make"      -eq 1 ]; then pkgs="$pkgs make"; fi
        if [ "$need_cmake"     -eq 1 ]; then pkgs="$pkgs cmake"; fi
        if [ "$need_tar"       -eq 1 ]; then pkgs="$pkgs tar"; fi
        if [ "$need_libstdcxx" -eq 1 ]; then pkgs="$pkgs libstdc++6"; fi
        ;;
esac

pkgs="${pkgs# }"

# 모르는 배포판에서 추측으로 설치하지 않는다 — 필요한 것만 알려주고 물러난다
if [ -z "$install_cmd" ] || [ -z "$pkgs" ]; then
    echo "[WARN] Unrecognized distribution (ID='${distro_id}', ID_LIKE='${distro_like}')."
    echo "       Install these with your package manager, then re-run this script:"
    echo "         a C++17 compiler (GCC 9+ / Clang 10+), make, cmake >= $MIN_CMAKE, tar"
    echo "       Or skip all of this and use Docker: docker compose up --build"
    exit 1
fi

# ── 3. 권한 확인 후 실행 ───────────────────────────────────────────────────
# root가 아니면 sudo를 앞에 붙인다. sudo조차 없으면 시스템을 건드리지 않고
# 명령만 보여준 뒤 종료한다 — 권한 없는 채점자 계정에서 알 수 없는 에러로 죽지 않게.

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo "
    else
        echo "[ERROR] Root privileges are required and sudo is not available."
        echo "        Run these as root:"
        if [ -n "$update_cmd" ]; then echo "          $update_cmd"; fi
        echo "          $install_cmd $pkgs"
        exit 1
    fi
fi

echo "[INFO] Planned commands:"
if [ -n "$update_cmd" ]; then echo "         ${SUDO}${update_cmd}"; fi
echo "         ${SUDO}${install_cmd} ${pkgs}"
echo ""

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "[INFO] --check given; nothing was installed."
    # 여기까지 왔다는 것은 빠진 것이 있다는 뜻이다 (없으면 위에서 이미 exit 0 했다).
    # 호출자가 종료 코드만으로 판단할 수 있게 1을 돌려준다.
    exit 1
fi

if [ "$ASSUME_YES" -eq 0 ]; then
    # 터미널이 없으면(파이프·CI) 묻지 않고 멈춘다. 확인 없는 자동 설치는 --yes의 몫이다
    if [ ! -t 0 ]; then
        echo "[ERROR] Not an interactive terminal. Re-run with --yes to install without asking."
        exit 1
    fi
    read -r -p "Proceed? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "[INFO] Aborted. Nothing was installed."; exit 1 ;;
    esac
fi

if [ -n "$update_cmd" ]; then
    ${SUDO}${update_cmd}
fi
# shellcheck disable=SC2086
${SUDO}${install_cmd} ${pkgs}

echo ""
echo "========================================"
echo "[SUCCESS] Dependencies installed."
echo "========================================"
echo "  Next: ./build_project_linux.sh"
