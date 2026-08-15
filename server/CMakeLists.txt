add_library(server_core STATIC
    src/parser/LogLineParser.cpp
    src/parser/SkipReporter.cpp
    src/stats/StatsCollector.cpp
)
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC common)

add_executable(server
    src/app/main.cpp
)

target_include_directories(server PRIVATE
    src
    ${THIRDPARTY_DIR}/libuv/include
)
target_link_libraries(server PRIVATE server_core common ${LIBUV_LIB})

if(NOT WIN32)
    target_link_libraries(server PRIVATE pthread dl)
endif()
