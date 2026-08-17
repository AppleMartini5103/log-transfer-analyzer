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
    add_executable(unit_tests ${COMMON_TESTS})
    target_link_libraries(unit_tests PRIVATE common ${CATCH2_LIB})
else()
    add_executable(unit_tests ${COMMON_TESTS} ${SERVER_TESTS})
    target_link_libraries(unit_tests PRIVATE server_core common ${CATCH2_LIB})
endif()

target_include_directories(unit_tests PRIVATE
    ${THIRDPARTY_DIR}/catch2/include
    ${CMAKE_SOURCE_DIR}/server/src
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
