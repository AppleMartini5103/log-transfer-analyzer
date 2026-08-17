# common/: shared protocol/net/util layer (libuv + standard library only —
# no OS headers; see coding_convention.txt §10)
add_library(common STATIC
    net/Listener.cpp
    net/TcpSocket.cpp
    net/Timer.cpp
    protocol/Codec.cpp
    protocol/Framer.cpp
    util/Crc32.cpp
    util/Logger.cpp
    util/SpscRingBuffer.cpp
    util/Version.cpp
)

target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${THIRDPARTY_DIR}/libuv/include
)
target_link_libraries(common PUBLIC ${LIBUV_LIB})

# Windows: ntohs/htons 등 바이트 오더 변환이 libc가 아니라 Winsock에 있다
# (TcpSocket::peerAddress). Linux에서 pthread/dl을 링크하는 것과 같은 성격의 플랫폼 의존.
if(WIN32)
    target_link_libraries(common PUBLIC ws2_32)
endif()
target_compile_definitions(common PRIVATE BUILD_DATE="${BUILD_DATE}")
