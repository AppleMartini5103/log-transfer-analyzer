# common/: shared protocol/net/util layer (libuv + standard library only —
# no OS headers; see coding_convention.txt §10)
add_library(common STATIC
    util/Version.cpp
)

target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(common PRIVATE BUILD_DATE="${BUILD_DATE}")
