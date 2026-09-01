cmake_minimum_required(VERSION 3.16)
project(log_transfer_analyzer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

string(TIMESTAMP BUILD_DATE "%Y-%m-%d")

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Windows: libuv의 uv.h가 <windows.h>를 끌어오면서 min/max 매크로가 std::min/std::max를
# 깨뜨린다 (error C2589 "'(': '::' 오른쪽에 잘못된 토큰"). NOMINMAX로 매크로 자체를 없앤다.
# WIN32_LEAN_AND_MEAN은 불필요한 하위 헤더를 잘라 빌드 시간을 줄인다.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# MSVC 런타임을 정적(/MT)으로 고정 — 제출물이 VC++ 재배포 패키지 없이 실행되게 만든다.
# CMP0091(3.15+)이 NEW라 이 변수가 /MD·/MT 선택을 담당한다. 루트에서 한 번만 정하고,
# 3rdparty도 같은 설정으로 빌드해야 한다 — CRT가 둘이면 한쪽에서 할당하고 다른 쪽에서
# 해제하는 순간 힙이 손상되고, 링커는 LNK2038로 먼저 막는다 (catch2 build_window.bat 참조).
#
# Debug 표현식을 함께 쓰는 이유: 3rdparty는 Release(/MT)로만 빌드하므로 Windows Debug
# 빌드는 지원 대상이 아니지만, 설정 자체는 구성별로 올바른 값을 내야 나중에 어긋나지 않는다.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── 새니타이저 (gcc/clang 전용. MSVC에서는 조용히 무시된다) ────────────────────
#
# 두 구성은 상호 배타다. ASan과 TSan은 같은 프로세스의 메모리 접근을 서로 다른 방식으로
# 가로채므로 한 바이너리에 링크될 수 없다 (컴파일러가 직접 거부한다). 그래서 둘을 함께 켜면
# 여기서 즉시 빌드를 실패시킨다 — 한쪽을 조용히 무시하면 "돌렸는데 아무것도 안 잡혔다"가
# 되어 검증하지 않은 것을 검증했다고 착각하게 된다. 새니타이저에서 가장 위험한 결과는
# 오탐이 아니라 "아무 일도 없었음"이다.
#
# ASan + UBSan: 컨벤션 7번의 "제출 전 누수 0 확인"을 상시 실행 가능하게 만든다.
#   cmake -B build-asan -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
# 경고 없이 통과하던 dangling string_view를 실제로 잡아낸 도구라 옵션으로 고정해 둔다.
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer (excludes ENABLE_TSAN)" OFF)

# ThreadSanitizer: 데이터 레이스 전용. ASan이 잡지 못하는 유일한 부류라 별도 구성이 필요하다
#   cmake -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
#
# [왜 UBSan을 함께 붙이지 않는가]
#   TSan+UBSan은 링크는 되지만, 이 구성의 목적은 레이스 하나다. UBSan 소견은 이미 ASan 구성이
#   담당하므로 중복이고, TSan은 그 자체로 5~15배 느려 계측을 더 얹을 이유가 없다.
#
# [해석 시 반드시 알아야 할 한계 — libuv는 계측되지 않는다]
#   ASan은 malloc 인터셉트로 동작해 계측되지 않은 라이브러리를 지나도 누수를 잡는다. TSan은
#   다르다. 메모리 접근마다 삽입된 코드가 필요하므로, TSan으로 빌드되지 않은 libuv 내부의
#   접근은 보이지 않는다. 결과는 두 방향으로 갈린다:
#     · libuv가 pthread 뮤텍스·조건변수로 하는 동기화는 인터포지션으로 보이므로 정상 인식된다
#     · libuv가 내부 원자적 연산으로 하는 동기화(uv_async_send가 그렇다)는 보이지 않아
#       오탐이 날 수 있다 — 실제로는 동기화된 접근을 레이스로 보고할 수 있다
#   따라서 판정 기준을 미리 정해 둔다: 우리 두 스레드가 우리 자료구조(링버퍼·통계 맵)에
#   접근하는 스택은 진짜로 취급하고, 스택이 libuv 프레임을 통과하는 소견은 libuv의 동기화가
#   보이지 않아서일 수 있으므로 코드를 직접 읽어 확인한 뒤에만 결론을 낸다.
#   → 억제 파일(tsan.supp)은 미리 만들지 않는다. 관측하지 않은 잡음을 위해 억제를 깔면
#     나중에 진짜 소견까지 함께 지워질 수 있다. 실제로 잡음이 나오면 그때 근거와 함께 추가한다
option(ENABLE_TSAN "Build with ThreadSanitizer (excludes ENABLE_SANITIZERS)" OFF)

# ── 프레임워크 비교 (Linux 전용, 기본 OFF) ────────────────────────────────────
# 같은 crc32 테스트 5개를 GoogleTest로도 빌드한다 (tests/test_crc32_gtest.cpp).
# 스위트는 Catch2 하나로 유지한다 — 이 옵션은 README "Framework comparison" 절의
# 측정을 재현하기 위한 것이며, 기본 빌드·ctest에는 아무 영향이 없다.
# 선행 조건: 3rdparty/gtest/build_linux.sh (tests/CMakeLists.txt가 부재를 즉시 알린다)
option(ENABLE_GTEST_COMPARISON "Build the crc32 tests with GoogleTest too, for the README framework comparison (Linux only)" OFF)

if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR
        "ENABLE_SANITIZERS (ASan) and ENABLE_TSAN cannot be combined - they intercept memory "
        "access in incompatible ways. Configure two separate build directories instead: "
        "-DENABLE_SANITIZERS=ON in build-asan and -DENABLE_TSAN=ON in build-tsan.")
endif()

if(ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer ENABLED")
endif()

if(ENABLE_TSAN AND NOT MSVC)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "Sanitizers: ThreadSanitizer ENABLED")
endif()

# 켜졌을 때 한 줄을 반드시 찍는 이유 — 실제로 겪은 실패다.
# CMake는 프로젝트가 모르는 -D 변수를 "Manually-specified variables were not used"
# 경고 한 줄로만 알린다. 옵션이 적용되지 않은 빌드는 평범한 빌드로 조용히 성공하고,
# 테스트도 통과하므로 "새니타이저를 돌렸고 깨끗했다"로 오독된다. 실제로 커밋 [45]가
# 브랜치에만 있고 체크아웃에 없던 상태로 -DENABLE_TSAN=ON을 준 실행이 그렇게 지나갔다.
# 위 STATUS 줄의 부재가 곧 "적용되지 않았다"는 관측 가능한 신호가 된다.
# → 링크까지 실제로 됐는지는 빌드 후 확인한다: ldd <binary> | grep -E 'libtsan|libasan'
#   (STATUS는 configure 시점의 의도, ldd는 결과물의 사실 — 둘은 다른 것을 증명한다)

# 3rdparty: include/ and lib/ are generated from tarballs by each
# library's build_linux.sh / build_window.bat (see 3rdparty/*/).
set(THIRDPARTY_DIR ${CMAKE_SOURCE_DIR}/3rdparty)

if(WIN32)
    # 정적 링크 (design 19번) — 제출물이 client.exe 하나로 실행되게 만든다.
    #
    # 파일명이 libuv.lib인 이유: libuv CMakeLists.txt:491이 OUTPUT_NAME을 "uv"로 두고
    # 492-493의 if(WIN32)가 PREFIX를 "lib"로 붙인다. uv_a.lib도 uv.lib도 아니며, 이 이름은
    # 빌드 시스템과 무관하게 결정적이다. 종전의 uv.lib은 DLL의 임포트 라이브러리였고
    # 지금 것은 정적 아카이브라, 파일명이 달라진 것이 성격이 바뀐 신호이기도 하다.
    set(LIBUV_LIB  ${THIRDPARTY_DIR}/libuv/lib/window/async/libuv.lib)
    set(CATCH2_LIB ${THIRDPARTY_DIR}/catch2/lib/window/catch2.lib)

    # 정적 libuv를 쓰면 이 시스템 라이브러리들을 실행 파일이 직접 링크해야 한다.
    # DLL이던 시절에는 uv.dll이 이미 흡수하고 있어 우리 링크 줄에 나타나지 않았다.
    # 목록의 출처는 libuv CMakeLists.txt:190-198 (WIN32 분기의 uv_libraries)이며,
    # 우리는 libuv의 CMake 패키지가 아니라 .lib 경로를 직접 지정하므로 자동으로 따라오지
    # 않는다 — 빠뜨리면 WSAStartup 등에서 미해결 심볼이 쏟아진다.
    set(LIBUV_SYSTEM_LIBS psapi user32 advapi32 iphlpapi userenv ws2_32 dbghelp ole32 shell32)
else()
    set(LIBUV_LIB  ${THIRDPARTY_DIR}/libuv/lib/linux/async/libuv.so)
    set(CATCH2_LIB ${THIRDPARTY_DIR}/catch2/lib/linux/libcatch2.a)
    set(LIBUV_SYSTEM_LIBS "")
endif()

add_subdirectory(common)

# server/ is Linux-only: the daemon uses POSIX (fork/setsid/unistd.h) and the
# assignment specifies a Linux server. Building it on Windows serves no purpose
# and would require #ifdef noise throughout the daemon.
if(NOT WIN32)
    add_subdirectory(server)
endif()

# client/ is Windows-only (DearImGui + Win32/DX11).
if(WIN32)
    add_subdirectory(client)
endif()

enable_testing()
add_subdirectory(tests)
