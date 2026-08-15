#include "session/SessionManager.h"

#include "util/Logger.h"

namespace server::session {

SessionManager::SessionManager(uv_loop_t* loop, std::size_t chunkSize, std::size_t ringSlots,
                               SessionTimeouts timeouts)
    : _loop(loop),
      _listener(loop),
      _parser(loop, ringSlots, chunkSize),
      _reaper(std::make_unique<uv_idle_t>()),
      _chunkSize(chunkSize),
      _timeouts(timeouts) {
    uv_idle_init(loop, _reaper.get());
    _reaper->data = this;
    // 파서 → 루프 방향 신호를 현재 세션으로 라우팅한다. 파서는 상주이고 세션은 오가므로
    // 관리자가 중간에서 받아 넘긴다 (죽은 세션으로 콜백이 들어가지 않도록)
    _parser.setHandlers(
        [this](server::parser::AnalysisResult result) {
            if (_session) {
                _session->onAnalysisComplete(std::move(result));
            }
        },
        [this] {
            if (_session) {
                _session->resumeReading();
            }
        });
}

bool SessionManager::startParser(std::string& error) {
    return _parser.start(error);
}

SessionManager::~SessionManager() {
    _session.reset();
    if (_reaper) {
        _reaper->data = nullptr;
        uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(_reaper.release());
        if (!uv_is_closing(raw)) {
            uv_close(raw, [](uv_handle_t* handle) {
                std::unique_ptr<uv_idle_t> owned{reinterpret_cast<uv_idle_t*>(handle)};
            });
        }
    }
}

int SessionManager::listen(const std::string& ip, std::uint16_t port, int backlog) {
    return _listener.listen(ip, port, backlog, this);
}

void SessionManager::close() {
    _closing = true;
    _listener.close();  // 새 연결 차단이 종료 시퀀스의 첫 단계 (design 10번)
    if (_session) {
        _session.reset();  // 활성 세션은 소켓을 닫는다 (RAII)
    }
    _parser.stop();  // 종료 플래그 + notify → join → async 핸들 close

    // ★ idle 핸들도 여기서 닫아야 한다. 소멸자는 uv_run이 반환한 뒤에 실행되는데,
    //   열린 핸들이 하나라도 남아 있으면 uv_run이 반환하지 않아 서로를 기다리게 된다
    if (_reaper) {
        uv_idle_stop(_reaper.get());
        _reaper->data = nullptr;
        uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(_reaper.release());
        if (!uv_is_closing(raw)) {
            uv_close(raw, [](uv_handle_t* handle) {
                std::unique_ptr<uv_idle_t> owned{reinterpret_cast<uv_idle_t*>(handle)};
            });
        }
    }
}

void SessionManager::onConnection() {
    if (_closing) {
        return;
    }
    if (_session) {
        // 세션 진행 중 — accept하지 않는다. 연결은 백로그에서 대기하고,
        // 콜백도 연결당 1회뿐이므로 여기서 사실만 기록해두었다가 CLEANUP 후 받는다
        _pendingConnection = true;
        common::Logger::instance().info(
            "Connection queued in backlog (session in progress, 1:1 policy)");
        return;
    }
    acceptIfIdle();
}

void SessionManager::acceptIfIdle() {
    if (_closing || _session) {
        return;
    }
    auto socket = _listener.acceptPending();
    if (!socket) {
        return;  // 대기분 없음 — 다음 onConnection을 기다린다
    }
    _pendingConnection = false;
    _session =
        std::make_unique<Session>(_loop, std::move(socket), this, &_parser, _chunkSize, _timeouts);
    if (!_session->start()) {
        _session.reset();
    }
}

void SessionManager::onListenError(int status) {
    common::Logger::instance().error(std::string{"Listener error: "} + uv_strerror(status));
}

void SessionManager::onSessionFinished() {
    // 세션 자신의 콜백 안에서 불린다 — 여기서 파괴하면 실행 중인 객체를 없애는 것이므로
    // idle 핸들로 한 틱 미룬다
    _sessionFinished = true;
    if (_reaper && !_closing) {
        uv_idle_start(_reaper.get(), onReapCb);
    }
}

void SessionManager::onReapCb(uv_idle_t* handle) {
    auto* self = static_cast<SessionManager*>(handle->data);
    if (self == nullptr) {
        return;
    }
    uv_idle_stop(handle);
    if (!self->_sessionFinished) {
        return;
    }
    self->_sessionFinished = false;
    self->_session.reset();  // 안전한 지점에서 파괴
    ++self->_completedSessions;
    common::Logger::instance().info("Session reaped, ready to accept (completed " +
                                    std::to_string(self->_completedSessions) + ")");
    // CLEANUP 후 accept 재개 — 백로그에 대기 중인 연결이 있으면 지금 받는다
    self->acceptIfIdle();
}

}  // namespace server::session
