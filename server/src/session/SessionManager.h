#pragma once

#include "net/Listener.h"
#include "parser/ParserThread.h"
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
    // socketBufferSize = 0이면 SO_SNDBUF/SO_RCVBUF를 건드리지 않는다 (커널 autotuning)
    SessionManager(uv_loop_t* loop, std::size_t chunkSize, std::size_t ringSlots,
                   int socketBufferSize = 0, SessionTimeouts timeouts = SessionTimeouts{});
    ~SessionManager() override;

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;

    // 파서 스레드 기동 — 반드시 데몬화(fork) 이후에 호출 (design 10번 순서)
    bool startParser(std::string& error);
    int listen(const std::string& ip, std::uint16_t port, int backlog);
    std::uint16_t boundPort() const { return _listener.boundPort(); }
    void close();

    bool hasActiveSession() const { return _session != nullptr; }
    std::uint64_t completedSessions() const { return _completedSessions; }

    // 보관본 조회 — 청구표 세 필드가 모두 일치할 때만 준다. 하나라도 어긋나면 nullptr
    // (= Ack(NoSuchResult)). "재접속하면 마지막 결과를 준다"로 하면 A의 결과가 B에게 간다.
    const RetainedResult* retainedResult(const std::string& filename, std::uint64_t fileSize,
                                         std::uint32_t uploadCrc32) const;
    bool hasRetainedResult() const { return _hasRetained; }

private:
    // ── IListenerCallback ──
    void onConnection() override;
    void onListenError(int status) override;
    // ── ISessionObserver ──
    void onSessionFinished() override;
    // 분석 완료본을 루프 스레드 소유 복사본으로 보관한다 (1개 — 1:1이라 동시 세션이 없다).
    // Session은 파괴돼도 SessionManager는 살아 있으므로 재요청 세션이 이 값을 쓸 수 있다.
    void onResultRetained(RetainedResult result) override;
    const RetainedResult* lookupRetainedResult(const std::string& filename, std::uint64_t fileSize,
                                               std::uint32_t uploadCrc32) const override;

    void acceptIfIdle();
    // 파서가 이전 세션의 폐기를 끝낸 뒤에만 accept한다 (경쟁 조건 해소)
    void acceptWhenParserIdle();
    static void onReapCb(uv_idle_t* handle);

    uv_loop_t* _loop = nullptr;
    common::net::Listener _listener;
    std::unique_ptr<Session> _session;
    server::parser::ParserThread _parser;  // 상주 1개 — 세션마다 만들지 않는다
    std::unique_ptr<uv_idle_t> _reaper;  // 지연 파괴용 — 세션 콜백 밖에서 정리한다
    std::size_t _chunkSize;
    int _socketBufferSize = 0;
    SessionTimeouts _timeouts;
    std::uint64_t _completedSessions = 0;
    RetainedResult _retained;      // 마지막 완료 분석 — 새 업로드가 완료되면 교체된다
    bool _hasRetained = false;     // 메모리 전용: 데몬 재시작이면 사라진다 (→ NoSuchResult)
    bool _pendingConnection = false;  // 세션 중에 온 연결 — CLEANUP 후 받는다
    bool _sessionFinished = false;
    bool _closing = false;
};

}  // namespace server::session
