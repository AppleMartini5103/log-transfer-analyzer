#include "app/ServerApp.h"

#include "app/Daemon.h"
#include "util/Logger.h"

#include <unistd.h>

#include <csignal>
#include <string>
#include <utility>

namespace server::app {

namespace {

std::string signalName(int signum) {
    switch (signum) {
        case SIGTERM:
            return "SIGTERM";
        case SIGINT:
            return "SIGINT";
        default:
            return "signal " + std::to_string(signum);
    }
}

}  // namespace

ServerApp::ServerApp(ServerConfig config) : _config(std::move(config)) {}

ServerApp::~ServerApp() {
    if (_loopReady) {
        // 핸들이 모두 닫힌 뒤에야 성공한다. run()이 정상 종료했다면 이미 닫혀 있음
        uv_loop_close(&_loop);
    }
}

bool ServerApp::init(std::string& error) {
    int rc = uv_loop_init(&_loop);
    if (rc != 0) {
        error = std::string{"uv_loop_init: "} + uv_strerror(rc);
        return false;
    }
    _loopReady = true;

    // 시그널 핸들은 번호당 하나씩 등록한다
    struct Registration {
        uv_signal_t* handle;
        int signum;
    };
    const Registration registrations[] = {{&_sigterm, SIGTERM}, {&_sigint, SIGINT}};
    for (const auto& registration : registrations) {
        rc = uv_signal_init(&_loop, registration.handle);
        if (rc != 0) {
            error = std::string{"uv_signal_init: "} + uv_strerror(rc);
            return false;
        }
        registration.handle->data = this;
        rc = uv_signal_start(
            registration.handle,
            [](uv_signal_t* handle, int signum) {
                static_cast<ServerApp*>(handle->data)->onSignal(signum);
            },
            registration.signum);
        if (rc != 0) {
            error = std::string{"uv_signal_start: "} + uv_strerror(rc);
            return false;
        }
    }
    return true;
}

int ServerApp::run() {
    common::Logger::instance().info("Server started (port " + std::to_string(_config.port) +
                                    ", pid " + std::to_string(static_cast<long>(::getpid())) +
                                    ")");
    // 아직 리스너가 없어도 시그널 핸들이 활성 핸들이라 uv_run은 블록한다.
    // 다음 이슈에서 리스너·파서 스레드가 이 자리에 붙는다
    const int rc = uv_run(&_loop, UV_RUN_DEFAULT);
    if (rc != 0) {
        // 활성 핸들이 남은 채 반환 — 정리 누락 신호이므로 조용히 넘기지 않는다
        common::Logger::instance().warn("uv_run returned with " + std::to_string(rc) +
                                        " active handle(s) remaining");
    }
    common::Logger::instance().info("Server stopped");
    common::Logger::instance().flush();
    return 0;
}

void ServerApp::onSignal(int signum) {
    if (_shuttingDown) {
        // 두 번째 Ctrl+C 등 — 이미 정리 중이므로 무시 (경로 재진입이 이중 close를 만든다)
        common::Logger::instance().warn("Ignoring " + signalName(signum) +
                                        " (shutdown already in progress)");
        return;
    }
    _shuttingDown = true;
    common::Logger::instance().info("Received " + signalName(signum) +
                                    " — starting graceful shutdown");
    shutdown();
}

void ServerApp::shutdown() {
    // ★ 정리 시작 즉시 종료 시그널을 "차단"한다 (무시가 아니라 block).
    //   uv_signal_stop은 해당 시그널의 마지막 핸들이 사라질 때 처리를 기본 동작(=프로세스
    //   즉사)으로 되돌린다. 그래서 SIG_IGN을 stop 앞에 걸면 stop이 덮어쓰고, 뒤에 걸면
    //   stop과 SIG_IGN 사이에 창이 남는다 — 실제로 시그널 폭격 테스트에서 그 창으로
    //   프로세스가 죽어 PID 파일이 남았다. 차단은 창 자체를 없앤다: 이후 도착분은
    //   pending으로 묶여 배달되지 않고, 해제 없이 종료하므로 그대로 폐기된다.
    //   ※ 파서 스레드가 추가되면 마스크는 스레드 생성 "전"에 걸어 상속시킬 것
    //     (시그널은 차단하지 않은 아무 스레드에나 배달될 수 있음)
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    ::sigprocmask(SIG_BLOCK, &mask, nullptr);

    // 초기화의 역순으로 정리한다 (총괄 원칙 ④).
    // 지금 존재하는 자원은 시그널 핸들뿐 — 리스너·세션·파서 스레드가 생기면
    // "리스닝 소켓 닫기 → 활성 세션 CLEANUP → 핸들 close" 순서로 이 앞에 추가된다.
    uv_signal_stop(&_sigterm);
    uv_signal_stop(&_sigint);

    const auto closeHandle = [](uv_handle_t* handle) {
        if (!uv_is_closing(handle)) {
            uv_close(handle, nullptr);
        }
    };
    closeHandle(reinterpret_cast<uv_handle_t*>(&_sigterm));
    closeHandle(reinterpret_cast<uv_handle_t*>(&_sigint));
    // 모든 핸들이 닫히면 uv_run이 자연스럽게 반환한다 — uv_stop으로 끊지 않는 이유는
    // 진행 중인 close 콜백까지 정상 처리되게 하기 위함
}

}  // namespace server::app
