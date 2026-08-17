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
    ${CMAKE_SOURCE_DIR}/client/src/service/IcmpPing.cpp
    ${CMAKE_SOURCE_DIR}/client/src/ui/UiState.cpp
)

# 서버 테스트는 Linux 전용 (서버 자체가 Linux 전용이라 Windows 빌드 대상이 아니다).
set(SERVER_TESTS
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
    # iphlpapi: IcmpPing.cpp의 IcmpSendEcho 계열 (Linux에서는 그 코드가 컴파일되지 않아 불필요)
    target_link_libraries(unit_tests PRIVATE common ${CATCH2_LIB} iphlpapi)
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

# Windows: libuv는 DLL로 빌드되므로 실행 파일 옆에 uv.dll이 있어야 한다
# (없으면 0xC0000135 STATUS_DLL_NOT_FOUND로 즉시 종료). Linux는 RPATH로 해결된다.
if(WIN32)
    add_custom_command(TARGET unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${THIRDPARTY_DIR}/libuv/lib/window/async/uv.dll"
                "$<TARGET_FILE_DIR:unit_tests>"
        COMMENT "Copying uv.dll next to unit_tests.exe")
endif()
