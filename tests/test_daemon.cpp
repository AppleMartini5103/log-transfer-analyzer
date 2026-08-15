#include "app/Daemon.h"

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <fstream>
#include <string>

// 데몬화 자체(fork)는 프로세스를 갈라놓아 단위 테스트로 검증할 수 없다 —
// 실제 데몬 동작은 바이너리를 띄워 PID 파일·로그·SIGTERM 종료로 확인한다.
// 여기서는 fork 없이 검증 가능한 조각(PID 파일, 시그널 기본값)만 다룬다.

namespace {

constexpr const char* kTestPidPath = "test_server.pid";

struct PidFileGuard {
    PidFileGuard() { std::remove(kTestPidPath); }
    ~PidFileGuard() { std::remove(kTestPidPath); }
};

}  // namespace

TEST_CASE("daemon: pid file contains the current process id") {
    const PidFileGuard guard;
    std::string error;
    REQUIRE(server::app::writePidFile(kTestPidPath, error));
    REQUIRE(error.empty());

    std::ifstream in{kTestPidPath};
    long pid = 0;
    REQUIRE(in >> pid);
    REQUIRE(pid == static_cast<long>(::getpid()));  // kill $(cat server.pid)가 동작하려면 필수
}

TEST_CASE("daemon: pid file is overwritten, not appended") {
    const PidFileGuard guard;
    std::string error;
    REQUIRE(server::app::writePidFile(kTestPidPath, error));
    REQUIRE(server::app::writePidFile(kTestPidPath, error));

    std::ifstream in{kTestPidPath};
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) {
        ++lines;
    }
    REQUIRE(lines == 1);  // 재시작 시 옛 PID가 남아 있으면 엉뚱한 프로세스를 kill하게 된다
}

TEST_CASE("daemon: pid file failure is reported, not thrown") {
    std::string error;
    REQUIRE_FALSE(server::app::writePidFile("no_such_dir/server.pid", error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.find("no_such_dir/server.pid") != std::string::npos);
}

TEST_CASE("daemon: removing the pid file is idempotent") {
    const PidFileGuard guard;
    std::string error;
    REQUIRE(server::app::writePidFile(kTestPidPath, error));
    server::app::removePidFile(kTestPidPath);
    REQUIRE_FALSE(std::ifstream{kTestPidPath}.good());
    server::app::removePidFile(kTestPidPath);  // 없어도 조용히 성공 — 종료 경로를 막지 않는다
}

TEST_CASE("daemon: SIGPIPE and SIGHUP are ignored after installing defaults") {
    server::app::installProcessSignalDefaults();
    // 자신에게 보내도 죽지 않아야 한다. SIGPIPE가 기본값이면 여기서 프로세스가 즉사하고
    // 테스트 스위트 전체가 사라진다 — "전송 중 강제 단절" 크래시 경로와 같은 메커니즘
    REQUIRE(::raise(SIGPIPE) == 0);
    REQUIRE(::raise(SIGHUP) == 0);
    SUCCEED("process survived SIGPIPE and SIGHUP");
}
