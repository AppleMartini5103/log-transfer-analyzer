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
add_subdirectory(server)

# client/ is Windows-only (DearImGui + Win32); added when client work starts.
# if(WIN32)
#     add_subdirectory(client)
# endif()

enable_testing()
add_subdirectory(tests)
