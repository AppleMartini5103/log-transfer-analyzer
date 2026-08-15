#pragma once

#include "net/ISocket.h"
#include "net/Timer.h"
#include "protocol/Framer.h"
#include "protocol/protocol.h"

#include <uv.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 세션 상태 머신 (design 11번) — 한 세션의 수명 = 한 바퀴.
//
//   WAIT_HEADER → RECEIVING → VERIFYING → ANALYZING → SENDING_RESULT → WAIT_DONE → CLEANUP
//   어느 상태에서든 에러·타임아웃·강제 단절 → CLEANUP 하나로 수렴 (총괄 원칙 ③).
//
// [타이머 배정 — 상대를 기다리는 상태에만 건다]
//   WAIT_HEADER    상대(UploadHeader)      ②류 120초
//   RECEIVING      상대(페이로드+트레일러) ①류 무활동 30초 (활동마다 리셋)
//   VERIFYING      자기 자신(CRC 비교)      없음
//   ANALYZING      자기 자신(통계+CSV)      없음
//   SENDING_RESULT 상대(수신해 감)          ①류 무활동 30초
//   WAIT_DONE      상대(DownloadDone)      ②류 120초
//   자기 CPU 작업에 타이머를 걸면 느린 머신에서 자살하는 서버가 된다.
//   매핑은 timeoutForState() 한 곳에만 둔다 — 상태를 추가할 때 타이머 누락을 막기 위함.
//
// [검증 실패 2부류 — design 11번]
//   ① 매직/버전/타입 불일치 → 응답 없이 즉시 CLEANUP (스트림 자체를 신뢰할 수 없음)
//   ② 파싱은 됐으나 값 무효 → Ack(PROTOCOL_ERROR) 송신 후 CLEANUP (사유를 알리고 닫는다)
//
// 세션 상태·버퍼·CRC는 전부 루프 스레드 소유 (총괄표). 파서 스레드는 다음 이슈에서 붙는다.

namespace server::session {

enum class SessionState : std::uint8_t {
    WaitHeader,
    Receiving,
    Verifying,
    Analyzing,
    SendingResult,
    WaitDone,
    Cleanup,
};

std::string_view stateName(SessionState state);

// 상태별 타임아웃 (0 = 타이머 없음). 테스트는 짧은 값을 주입해 만료를 재현한다
struct SessionTimeouts {
    std::uint64_t idleMs = common::protocol::kIdleTimeoutMs;          // ①류 30초
    std::uint64_t responseMs = common::protocol::kResponseTimeoutMs;  // ②류 120초
};

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;
    // 소켓이 완전히 닫힌 뒤 호출 — 이 시점 이후 세션 객체를 파괴해도 안전하다.
    // 호출 문맥은 세션 자신의 콜백 안이므로 여기서 즉시 파괴하지 말 것 (지연 파괴)
    virtual void onSessionFinished() {}
};

class Session : public common::net::ISocketCallback {
public:
    Session(uv_loop_t* loop, std::unique_ptr<common::net::ISocket> socket,
            ISessionObserver* observer, std::size_t chunkSize,
            SessionTimeouts timeouts = SessionTimeouts{});
    ~Session() override;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // 수신 시작 + WAIT_HEADER 타이머 가동
    bool start();

    SessionState state() const { return _state; }
    const std::string& filename() const { return _filename; }
    std::uint64_t receivedBytes() const { return _receivedBytes; }
    // 세션이 끝난 사유 (로그·테스트 확인용)
    const std::string& finishReason() const { return _finishReason; }

private:
    // ── ISocketCallback ──
    common::net::WritableBuffer onAllocate(std::size_t suggestedSize) override;
    void onRead(std::string_view data) override;
    void onSendComplete(int status) override;
    void onError(int status, std::string_view where) override;
    void onClosed() override;

    // ── 상태 전이 ──
    void transition(SessionState next);
    static std::uint64_t timeoutForState(SessionState state, const SessionTimeouts& timeouts);
    void armTimer();
    void onTimeout();

    // ── 단계별 처리 (소비한 바이트 수를 반환) ──
    std::size_t consumeHeader(std::string_view data);
    std::size_t consumePayload(std::string_view data);
    std::size_t consumeTrailer(std::string_view data);
    std::size_t consumeDone(std::string_view data);

    void verifyAndAck();
    void analyzeAndSendResult();

    // 사유를 알리고 닫는다 (검증 실패 ②부류·CRC 불일치)
    void failWithAck(common::protocol::AckStatus status, const std::string& reason);
    // 스트림 신뢰 불가 — 응답 없이 닫는다 (검증 실패 ①부류)
    void closeSilently(const std::string& reason);
    void cleanup(const std::string& reason);

    uv_loop_t* _loop = nullptr;
    std::unique_ptr<common::net::ISocket> _socket;
    ISessionObserver* _observer = nullptr;
    common::net::Timer _timer;
    SessionTimeouts _timeouts;

    SessionState _state = SessionState::WaitHeader;
    common::protocol::Framer _framer;

    std::vector<char> _readBuffer;  // 다음 이슈에서 링버퍼 슬롯으로 교체된다
    std::uint64_t _fileSize = 0;
    std::uint64_t _receivedBytes = 0;  // 페이로드만 집계 (프리앰블·트레일러 제외)
    std::uint32_t _payloadCrc = 0;     // 루프 스레드 소유 — 트레일러 비교 주체와 같은 스레드
    std::string _filename;

    std::string _csv;  // 메모리 완성본 — 전송·CRC·디스크 기록의 공통 소스
    std::string _finishReason;
    bool _cleanupStarted = false;
    bool _finished = false;
};

}  // namespace server::session
