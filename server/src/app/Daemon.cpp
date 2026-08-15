#include "app/Daemon.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace server::app {

namespace {

std::string errnoText(const char* what) {
    return std::string{what} + " failed: " + std::strerror(errno);
}

// stdin/stdout/stderr를 /dev/null로 재지정.
// 데몬은 터미널이 없으므로 표준 스트림에 쓰면 EBADF가 되거나, 더 나쁘게는 나중에 열리는
// 파일이 fd 0/1/2를 차지해 로그가 엉뚱한 곳으로 간다. 닫지 않고 /dev/null로 덮는 이유가 이것
bool redirectStdioToDevNull(std::string& error) {
    const int fd = ::open("/dev/null", O_RDWR);
    if (fd < 0) {
        error = errnoText("open(/dev/null)");
        return false;
    }
    const bool ok = ::dup2(fd, STDIN_FILENO) >= 0 && ::dup2(fd, STDOUT_FILENO) >= 0 &&
                    ::dup2(fd, STDERR_FILENO) >= 0;
    if (!ok) {
        error = errnoText("dup2");
    }
    if (fd > STDERR_FILENO) {
        ::close(fd);
    }
    return ok;
}

}  // namespace

void installProcessSignalDefaults() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
}

bool daemonize(std::string& error) {
    // 1차 fork: 부모가 죽으면 셸이 프롬프트를 돌려주고, 자식은 프로세스 그룹 리더가 아니게 되어
    // setsid()를 호출할 수 있게 된다 (그룹 리더는 setsid 실패)
    const pid_t first = ::fork();
    if (first < 0) {
        error = errnoText("fork");
        return false;
    }
    if (first > 0) {
        ::_exit(0);  // 부모 종료. exit()가 아닌 _exit() — 자식과 공유하는 stdio 버퍼를
                     // 두 번 flush하지 않기 위함
    }

    if (::setsid() < 0) {  // 새 세션·프로세스 그룹 — 제어 터미널에서 분리
        error = errnoText("setsid");
        return false;
    }

    // 2차 fork: 세션 리더가 아닌 프로세스는 제어 터미널을 다시 획득할 수 없다.
    // 이게 double-fork의 목적 — 터미널이 닫혀도 서버에 SIGHUP이 오지 않는다
    const pid_t second = ::fork();
    if (second < 0) {
        error = errnoText("second fork");
        return false;
    }
    if (second > 0) {
        ::_exit(0);
    }

    ::umask(0027);  // 산출물(result.csv·로그)이 group-read까지만 — world-writable 방지
    // chdir("/") 생략: 관례의 목적(마운트 포인트 점유 방지)이 과제 환경에서 무의미하고,
    // 실행 디렉토리 기준 상대 경로(./result.csv, ./skip_report.txt)의 예측 가능성이 더 중요
    return redirectStdioToDevNull(error);
}

bool writePidFile(const std::string& path, std::string& error) {
    // 반드시 최종 fork 이후에 호출할 것 — 그전 PID는 이미 죽은 부모의 것이다
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        error = "cannot open pid file '" + path + "': " + std::strerror(errno);
        return false;
    }
    const int written = std::fprintf(file, "%ld\n", static_cast<long>(::getpid()));
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (written <= 0 || !flushed || !closed) {
        error = "cannot write pid file '" + path + "'";
        return false;
    }
    return true;
}

void removePidFile(const std::string& path) {
    // 이미 없으면 조용히 무시 — 종료 경로에서 실패시킬 이유가 없다
    std::remove(path.c_str());
}

}  // namespace server::app
