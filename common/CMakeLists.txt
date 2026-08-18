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
    # 정적 libuv가 요구하는 시스템 라이브러리 (목록 출처는 루트 CMakeLists 주석).
    # common이 libuv를 PUBLIC으로 링크하므로 여기 한 곳에 두면 client와 unit_tests로
    # 자동 전파된다 — 소비자마다 적으면 새 타깃이 생길 때 반드시 하나를 빠뜨린다.
    # ws2_32가 위와 중복되지만 이유가 다르다(위: 우리 코드의 ntohs, 아래: libuv 내부).
    # 링커에는 무해하고, 각자의 근거를 남기는 편이 나중에 지울 때 안전하다.
    target_link_libraries(common PUBLIC ${LIBUV_SYSTEM_LIBS})
endif()
target_compile_definitions(common PRIVATE BUILD_DATE="${BUILD_DATE}")
