# Unit tests (Catch2 v3, amalgamated static lib — default main() included).
# Run on both gcc and MSVC: see coding_convention.txt §10.
#
# common/ 테스트는 양쪽 플랫폼에서 모두 돈다 — 클라이언트가 공유하는 계층이므로
# MSVC/gcc 차이를 여기서 잡는 것이 §10의 목적이다.
set(COMMON_TESTS
    test_skeleton.cpp
    test_protocol.cpp
    test_crc32.cpp
    test_codec.cpp
    test_framer.cpp
    test_ringbuffer.cpp
    test_logger.cpp
    test_net.cpp
    test_client_scenarios.cpp
    test_ping_console.cpp
    test_ui_state.cpp
)

# 클라이언트 시나리오 테스트가 필요로 하는 client/ 소스 — GUI 의존이 없는 3개만 가져온다.
# TransferService/FileReader/UiState는 ImGui·Win32에 의존하지 않는다 (그 의존은
# app/Application, ui/FileDialog, ui/UiRenderer 3개 파일에 갇혀 있다 — design 6번의
# "Application은 조립만" 원칙이 여기서 배당금을 준다).
#
# 왜 Linux 빌드에도 넣는가: 강제 단절·취소 경로를 Linux에서 ASan/valgrind로 돌려
# 과제 평가 기준("강제 단절 시 양쪽 모두 안전한 자원 반환", 누수 0)을 증빙할 수 있다.
# 배포 대상은 여전히 Windows client.exe이고 테스트만 양쪽에서 돈다 — common/ 단위 테스트를
# 양쪽에서 돌리는 기존 방침(coding_convention 10번)과 같은 논리다.
set(CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/client/src/service/TransferService.cpp
    ${CMAKE_SOURCE_DIR}/client/src/service/FileReader.cpp
    ${CMAKE_SOURCE_DIR}/client/src/ui/UiState.cpp
)

# 서버 테스트는 Linux 전용 (서버 자체가 Linux 전용이라 Windows 빌드 대상이 아니다).
#
# test_signal_guard.cpp가 이 목록에 있는 이유: SIGPIPE는 POSIX 개념이라 Windows에는 없고,
# 가드가 재사용하는 installProcessSignalDefaults()는 server_core에 있어 Windows 빌드에서는
# 링크되지 않는다 (아래 add_executable 분기). 즉 Linux 전용 목록이 #ifdef 없는 해답이다.
set(SERVER_TESTS
    test_signal_guard.cpp
    test_line_reassembler.cpp
    test_log_line_parser.cpp
    test_stats_collector.cpp
    test_skip_reporter.cpp
    test_csv_builder.cpp
    test_server_config.cpp
    test_daemon.cpp
    test_session.cpp
    test_parser_thread.cpp
)

if(WIN32)
    add_executable(unit_tests ${COMMON_TESTS} ${CLIENT_SOURCES})
    target_link_libraries(unit_tests PRIVATE common ${CATCH2_LIB})
else()
    add_executable(unit_tests ${COMMON_TESTS} ${SERVER_TESTS} ${CLIENT_SOURCES})
    target_link_libraries(unit_tests PRIVATE server_core common ${CATCH2_LIB})
endif()

target_include_directories(unit_tests PRIVATE
    ${THIRDPARTY_DIR}/catch2/include
    ${CMAKE_SOURCE_DIR}/server/src
    ${CMAKE_SOURCE_DIR}/client/src
)

add_test(NAME unit_tests COMMAND unit_tests)

# ── 프레임워크 비교 타깃 (기본 OFF — 루트 CMakeLists의 옵션 주석 참조) ──────
# 켜졌을 때 3rdparty가 준비되지 않았으면 링크 단계가 아니라 configure에서 바로 알린다
# (새니타이저 옵션의 "적용 안 된 빌드가 조용히 성공" 교훈과 같은 원칙).
if(ENABLE_GTEST_COMPARISON AND NOT WIN32)
    set(GTEST_DIR ${THIRDPARTY_DIR}/gtest)
    if(NOT EXISTS ${GTEST_DIR}/lib/linux/libgtest.a)
        message(FATAL_ERROR
            "ENABLE_GTEST_COMPARISON is ON but 3rdparty/gtest is not built. "
            "Run 3rdparty/gtest/build_linux.sh first.")
    endif()
    find_package(Threads REQUIRED)
    add_executable(crc32_tests_gtest test_crc32_gtest.cpp)
    target_include_directories(crc32_tests_gtest PRIVATE ${GTEST_DIR}/include)
    target_link_libraries(crc32_tests_gtest PRIVATE common
        ${GTEST_DIR}/lib/linux/libgtest_main.a
        ${GTEST_DIR}/lib/linux/libgtest.a
        Threads::Threads)
    add_test(NAME crc32_tests_gtest COMMAND crc32_tests_gtest)
    message(STATUS "Framework comparison: crc32_tests_gtest ENABLED")
endif()

# uv.dll 복사 단계는 없다 — Windows도 libuv를 정적으로 링크한다 (design 19번).
# 종전에는 여기서 uv.dll을 실행 파일 옆으로 복사했다. 빠뜨리면 0xC0000135로 즉시 죽었다.
