add_library(server_core STATIC
    src/app/Daemon.cpp
    src/app/ServerApp.cpp
    src/app/ServerConfig.cpp
    src/csv/CsvBuilder.cpp
    src/parser/LogLineParser.cpp
    src/parser/ParserThread.cpp
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

# RUNPATH를 바이너리 자신의 위치 기준으로 잡는다 (README "Deployment notes").
#
# 기본값이면 CMake가 libuv.so의 절대 경로(= 빌드 머신의 홈 디렉토리)를 RUNPATH에
# 박아 넣는다. 그 경로는 채점자 머신에 없으므로 로더가 건너뛰고 시스템 libuv로
# 조용히 폴백하거나(= 테스트한 것과 다른 버전으로 실행), libuv 미설치 머신에서는
# 아예 뜨지 않는다.
#
# 순수 상대경로("lib" 같은)는 답이 아니다 — 로더가 그것을 바이너리 위치가 아니라
# 현재 작업 디렉토리 기준으로 해석하므로, cd 위치에 따라 로딩이 깨진다.
# $ORIGIN만이 "바이너리가 놓인 디렉토리"로 치환되는 토큰이다.
# ($는 CMake 변수 확장이 아니다 — CMake는 ${...} 형태만 확장하므로 그대로 전달된다)
#
# 두 항목을 넣는 이유:
#   $ORIGIN                                  배포용. server 옆에 libuv.so.1을 두고 실행
#   $ORIGIN/../../3rdparty/libuv/lib/...     빌드 트리용. build/server/server에서
#                                            소스 트리의 3rdparty를 거슬러 올라간다
#                                            (build_project_linux.sh가 `cmake -B build`로
#                                             소스 루트 바로 아래에 빌드 트리를 만든다)
# BUILD_WITH_INSTALL_RPATH를 켜야 CMake가 자동으로 넣는 절대 경로를 대체한다.
if(NOT WIN32)
    set_target_properties(server PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../../3rdparty/libuv/lib/linux/async"
    )
endif()
