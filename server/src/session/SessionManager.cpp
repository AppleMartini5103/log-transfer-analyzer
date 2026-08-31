#include "session/SessionManager.h"

#include "util/Logger.h"

namespace server::session {

SessionManager::SessionManager(uv_loop_t* loop, std::size_t chunkSize, std::size_t ringSlots,
                               int socketBufferSize, SessionTimeouts timeouts)
    : _loop(loop),
      _listener(loop),
      _parser(loop, ringSlots, chunkSize),
      _reaper(std::make_unique<uv_idle_t>()),
      _chunkSize(chunkSize),
      _socketBufferSize(socketBufferSize),
      _timeouts(timeouts) {
    if (uv_idle_init(loop, _reaper.get()) != 0) {
        // 유효한 루프에서는 사실상 실패하지 않지만, 규칙은 규칙이다 (컨벤션 3번).
        // reaper 없이는 지연 파괴가 불가능하므로 이 상태를 로그로 드러낸다
        common::Logger::instance().error("SessionManager: uv_idle_init failed — reaper disabled");
        _reaper.reset();
    } else {
        _reaper->data = this;
    }
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
        },
        [this] {
            // 폐기 완료 — 이제 다음 연결을 받아도 통계가 이어지지 않는다
            acceptWhenParserIdle();
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
    // 게이트를 거쳐 간다. 세션이 없더라도 직전 세션의 폐기가 아직 안 끝났을 수 있고,
    // 그 상태에서 받으면 통계가 이어진다. 미룬 연결은 백로그에 남아 있다가
    // 폐기 완료 신호가 오면 acceptPending()이 집어간다 — 유실되지 않는다.
    acceptWhenParserIdle();
}

void SessionManager::acceptWhenParserIdle() {
    // 이전 세션이 중단됐다면 파서가 통계·링을 폐기할 때까지 accept를 미룬다.
    //
    // 미루지 않으면: abortSession()은 플래그만 세우고 실제 폐기는 파서 스레드가 하는데,
    // 그 사이에 새 세션이 시작되면 이전 세션의 통계가 그대로 이어져 다음 클라이언트의
    // result.csv에 섞인다. 로그에도 남지 않아 조용히 틀린 결과가 나간다.
    //
    // 대기는 루프를 막지 않는다 — 여기서 그냥 돌아가고, 파서가 폐기를 마치면
    // onAbortDone 신호가 이 함수를 다시 부른다. 중단이 없었으면 판정은 원자 읽기 하나다.
    if (_parser.abortPending()) {
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
    if (_socketBufferSize > 0) {
        // 벤치 스윕용 고정값. 리눅스는 요청값의 2배로 보고하고 wmem_max/rmem_max에서
        // 잘리므로 실제 적용값을 로그에 남긴다 (design 네트워크 버퍼 절의 검증 절차)
        socket->applyBufferSizes(_socketBufferSize, _socketBufferSize);
        common::Logger::instance().info(
            "Socket buffers requested " + std::to_string(_socketBufferSize) + " B, actual snd=" +
            std::to_string(socket->actualSendBufferSize()) + " rcv=" +
            std::to_string(socket->actualRecvBufferSize()));
    }
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
        const int rc = uv_idle_start(_reaper.get(), onReapCb);
        if (rc != 0) {
            common::Logger::instance().error(std::string{"SessionManager: uv_idle_start: "} +
                                             uv_strerror(rc));
        }
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
    self->acceptWhenParserIdle();
}

}  // namespace server::session
