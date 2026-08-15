# Unit tests (Catch2 v3, amalgamated static lib — default main() included).
# Run on both gcc and MSVC: see coding_convention.txt §10.
add_executable(unit_tests
    test_skeleton.cpp
    test_protocol.cpp
    test_crc32.cpp
    test_codec.cpp
    test_framer.cpp
    test_ringbuffer.cpp
    test_logger.cpp
    test_line_reassembler.cpp
    test_log_line_parser.cpp
    test_stats_collector.cpp
    test_skip_reporter.cpp
    test_csv_builder.cpp
    test_server_config.cpp
)

target_include_directories(unit_tests PRIVATE
    ${THIRDPARTY_DIR}/catch2/include
    ${CMAKE_SOURCE_DIR}/server/src
)
target_link_libraries(unit_tests PRIVATE server_core common ${CATCH2_LIB})

add_test(NAME unit_tests COMMAND unit_tests)
