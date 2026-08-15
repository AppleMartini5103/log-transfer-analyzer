#pragma once

#include "net/Listener.h"
#include "session/Session.h"

#include <uv.h>

#include <cstdint>
#include <memory>
#include <string>

// 1:1 순차 세션 관리 (design 11번 [1:1 정책 구체화]).
//
// 세션 진행 중 새 연결은 accept하지 않는다 — 커널 백로그에 대기시킨다.
// 거절(accept 후 즉시 close)보다 나은 이유: 클라이언트에 재시도 로직이 불필요하다.
// 2026-08-14 스파이크로 검증됨: 미수락 연결은 pending 유지되고, 연결당 콜백은 1회이며,
// CLEANUP 후 늦게 accept해도 성공한다 (대기 중 도착한 데이터도 보존).
//
// [지연 파괴] 세션은 자기 콜백 안에서 onSessionFinished()를 부른다. 그 문맥에서 바로
// 파괴하면 실행 중인 객체를 없애는 셈이라, uv_idle로 한 틱 미뤄 안전한 지점에서 파괴한다.

namespace server::session {

class SessionManager : public common::net::IListenerCallback, public ISessionObserver {
public:
    SessionManager(uv_loop_t* loop, std::size_t chunkSize,
                   SessionTimeouts timeouts = SessionTimeouts{});
    ~SessionManager() override;

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;

    int listen(const std::string& ip, std::uint16_t port, int backlog);
    std::uint16_t boundPort() const { return _listener.boundPort(); }
    void close();

    bool hasActiveSession() const { return _session != nullptr; }
    std::uint64_t completedSessions() const { return _completedSessions; }

private:
    // ── IListenerCallback ──
    void onConnection() override;
    void onListenError(int status) override;
    // ── ISessionObserver ──
    void onSessionFinished() override;

    void acceptIfIdle();
    static void onReapCb(uv_idle_t* handle);

    uv_loop_t* _loop = nullptr;
    common::net::Listener _listener;
    std::unique_ptr<Session> _session;
    std::unique_ptr<uv_idle_t> _reaper;  // 지연 파괴용 — 세션 콜백 밖에서 정리한다
    std::size_t _chunkSize;
    SessionTimeouts _timeouts;
    std::uint64_t _completedSessions = 0;
    bool _pendingConnection = false;  // 세션 중에 온 연결 — CLEANUP 후 받는다
    bool _sessionFinished = false;
    bool _closing = false;
};

}  // namespace server::session
