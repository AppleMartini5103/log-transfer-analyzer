#include "app/Daemon.h"
#include "app/ServerApp.h"
#include "app/ServerConfig.h"
#include "util/Logger.h"

#include <cstdio>

// 서버 진입점.
// design 10번 초기화 순서 고정 — 이 순서를 바꾸면 자식 프로세스에서 데드락이 난다:
//   시그널 기본값 → 인자 파싱 → 데몬화(-d) → 로거 → uv_loop·시그널 → (스레드) → 실행
// fork는 호출 스레드만 복제하므로 루프·스레드 생성보다 반드시 먼저다.
int main(int argc, char** argv) {
    // SIGPIPE/SIGHUP 무시 — 끊긴 소켓 write가 프로세스를 죽이지 않고 EPIPE로 돌아오게 한다
    server::app::installProcessSignalDefaults();

    server::app::CommandLine cli;
    const server::app::ParseResult parsed = server::app::parseCommandLine(argc, argv, cli);
    if (!parsed.ok) {
        std::fprintf(stderr, "error: %s\n\n%s", parsed.error.c_str(),
                     server::app::usageText().c_str());
        return 1;
    }
    if (cli.helpRequested) {
        std::fputs(server::app::usageText().c_str(), stdout);
        return 0;
    }

    server::app::ServerConfig config;
    const server::app::ParseResult built = server::app::buildConfig(cli, config);
    if (!built.ok) {
        std::fprintf(stderr, "error: %s\n", built.error.c_str());
        return 1;
    }

    // 데몬화는 로거·루프보다 먼저 (fork 함정)
    if (config.daemonize) {
        std::string error;
        if (!server::app::daemonize(error)) {
            std::fprintf(stderr, "error: daemonize: %s\n", error.c_str());
            return 1;
        }
        // 이 시점부터 stdout은 /dev/null — 로그는 반드시 파일로 가야 한다
        if (!common::Logger::instance().openFile(config.logPath)) {
            return 1;  // 알릴 통로가 없다 (stderr도 /dev/null) — 종료 코드로만 보고
        }
        std::string pidError;
        if (!server::app::writePidFile(server::app::kPidFilePath, pidError)) {
            common::Logger::instance().error(pidError);
            return 1;
        }
        common::Logger::instance().info("Daemonized (pid file " +
                                        std::string{server::app::kPidFilePath} + ")");
    }

    server::app::ServerApp app{config};
    std::string error;
    if (!app.init(error)) {
        common::Logger::instance().error("Initialization failed: " + error);
        if (!config.daemonize) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
        }
        if (config.daemonize) {
            server::app::removePidFile(server::app::kPidFilePath);
        }
        return 1;
    }

    const int exitCode = app.run();

    // 종료는 초기화의 역순 — PID 파일 제거 후 로그 플러시
    if (config.daemonize) {
        server::app::removePidFile(server::app::kPidFilePath);
    }
    common::Logger::instance().flush();
    common::Logger::instance().close();
    return exitCode;
}
