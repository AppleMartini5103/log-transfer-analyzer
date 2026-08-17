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

# AddressSanitizer + UBSan (gcc/clang). 컨벤션 7번의 "제출 전 누수 0 확인"을 상시 실행
# 가능하게 만든다: cmake -B build-asan -DENABLE_SANITIZERS=ON && ctest --test-dir build-asan
# 경고 없이 통과하던 dangling string_view를 실제로 잡아낸 도구라 옵션으로 고정해 둔다.
option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# 3rdparty: include/ and lib/ are generated from tarballs by each
# library's build_linux.sh / build_window.bat (see 3rdparty/*/).
set(THIRDPARTY_DIR ${CMAKE_SOURCE_DIR}/3rdparty)
if(WIN32)
    set(LIBUV_LIB  ${THIRDPARTY_DIR}/libuv/lib/window/async/uv.lib)
    set(CATCH2_LIB ${THIRDPARTY_DIR}/catch2/lib/window/catch2.lib)
else()
    set(LIBUV_LIB  ${THIRDPARTY_DIR}/libuv/lib/linux/async/libuv.so)
    set(CATCH2_LIB ${THIRDPARTY_DIR}/catch2/lib/linux/libcatch2.a)
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
