#include "app/Daemon.h"

#include <catch_amalgamated.hpp>

#include <signal.h>  // sigaction — 처분을 바꾸지 않고 질의하려면 POSIX API가 필요하다
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

// 시그널 처분을 저장했다가 되돌리는 가드.
//
// 왜 RAII여야 하는가 (복원을 마지막 문장으로 두면 안 되는 이유):
//   아래 SIGPIPE 테스트는 검증을 위해 프로세스 전역 처분을 일시적으로 SIG_DFL로 되돌린다.
//   Catch2의 REQUIRE는 실패 시 예외를 던지므로, 복원을 함수 끝에 두면 실패한 순간 SIGPIPE가
//   기본값인 채로 남는다. 그러면 뒤따르는 소켓 테스트들이 전부 즉사한다 — 커밋 [46]이 고친
//   순서 의존 결함이 부호만 바뀌어 되살아난다. 복원은 어느 경로에서도 보장되어야 한다.
class SignalDispositionGuard {
public:
    explicit SignalDispositionGuard(int signo) : _signo(signo) {
        _savedOk = ::sigaction(_signo, nullptr, &_saved) == 0;
    }

    // 저장에 실패했으면 저장본을 되돌리는 대신 SIG_IGN으로 맞춘다.
    // 이유: _saved를 값 초기화하면 sa_handler가 0(SIG_DFL)이라, 실패한 저장본을 그대로
    // 복원하면 "복원했는데 기본값" — 막으려던 바로 그 상태로 프로세스를 떨어뜨린다.
    // 실패 확률은 사실상 0이지만 결과가 스위트 전멸이라 분기 두 줄이 싸다.
    ~SignalDispositionGuard() {
        if (_savedOk) {
            ::sigaction(_signo, &_saved, nullptr);
        } else {
            std::signal(_signo, SIG_IGN);
        }
    }

    SignalDispositionGuard(const SignalDispositionGuard&) = delete;
    SignalDispositionGuard& operator=(const SignalDispositionGuard&) = delete;

private:
    int _signo;
    bool _savedOk = false;
    struct sigaction _saved{};
};

// 현재 처분이 SIG_IGN인가 — 질의만 하고 아무것도 바꾸지 않는다.
// std::signal은 언제나 설정까지 하므로 "읽기만" 하려면 sigaction이어야 한다.
bool isIgnored(int signo) {
    struct sigaction current{};
    if (::sigaction(signo, nullptr, &current) != 0) {
        return false;
    }
    return current.sa_handler == SIG_IGN;
}

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
    // 이 테스트는 자기 전제조건을 스스로 세운다.
    //
    // 왜 필요한가: 커밋 [46]이 테스트 프로세스 시작 시점에 installProcessSignalDefaults()를
    // 부르는 리스너를 넣었다 (tests/test_signal_guard.cpp). 그래서 아무 준비 없이 시작하면
    // 이 테스트는 SIGPIPE가 이미 SIG_IGN인 상태에서 실행되고, installProcessSignalDefaults()의
    // 본문을 통째로 비워도 통과한다 — 검증한다고 선언한 것을 검증하지 못하는 공허한 통과다.
    // 전제를 바깥에서 받아쓰지 않고 여기서 만든다.
    const SignalDispositionGuard pipeGuard{SIGPIPE};
    const SignalDispositionGuard hupGuard{SIGHUP};

    REQUIRE(std::signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    REQUIRE(std::signal(SIGHUP, SIG_DFL) != SIG_ERR);
    REQUIRE_FALSE(isIgnored(SIGPIPE));  // 전제가 실제로 세워졌는지 먼저 확인한다
    REQUIRE_FALSE(isIgnored(SIGHUP));

    server::app::installProcessSignalDefaults();

    // ① 처분을 읽어 확인한다. raise()보다 먼저 하는 것이 핵심이다 —
    //    installProcessSignalDefaults()가 망가져 있으면 raise(SIGPIPE)는 프로세스를 즉사시켜
    //    스위트 전체가 사라지고 원인이 아무 데도 남지 않는다. 질의는 부작용도 사망 위험도
    //    없으므로 "무엇이 어긋났는지"를 실패 메시지로 남길 수 있다.
    REQUIRE(isIgnored(SIGPIPE));
    REQUIRE(isIgnored(SIGHUP));

    // ② 동작으로도 확인한다. ①을 통과했으므로 여기서 죽지 않는 것이 보장된 상태다.
    //    ①은 처분(선언)을, ②는 실제 전달 결과(동작)를 본다 — 서로 다른 것을 증명한다.
    REQUIRE(::raise(SIGPIPE) == 0);
    REQUIRE(::raise(SIGHUP) == 0);
}
