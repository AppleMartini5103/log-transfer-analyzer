add_library(server_core STATIC
    src/app/Daemon.cpp
    src/app/ServerApp.cpp
    src/app/ServerConfig.cpp
    src/csv/CsvBuilder.cpp
    src/parser/LogLineParser.cpp
    src/session/Session.cpp
    src/session/SessionManager.cpp
    src/parser/SkipReporter.cpp
    src/stats/StatsCollector.cpp
)

# ServerApp owns the uv_loop, so libuv is part of server_core's interface:
# PUBLIC so the server executable and unit tests inherit it.
target_include_directories(server_core PUBLIC
    src
    ${THIRDPARTY_DIR}/libuv/include
)
target_link_libraries(server_core PUBLIC common ${LIBUV_LIB})

if(NOT WIN32)
    target_link_libraries(server_core PUBLIC pthread dl)
endif()

add_executable(server
    src/app/main.cpp
)
target_link_libraries(server PRIVATE server_core)
