#pragma once

#include "net/ISocket.h"
#include "parser/ParserThread.h"
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

// 산출물 경로 (design 5번 실행 인터페이스 — 실행 디렉토리 기준 상대 경로)
inline constexpr const char* kResultCsvPath = "./result.csv";
inline constexpr const char* kSkipReportPath = "./skip_report.txt";

// 상태별 타임아웃 (0 = 타이머 없음). 테스트는 짧은 값을 주입해 만료를 재현한다
struct SessionTimeouts {
    std::uint64_t idleMs = common::protocol::kIdleTimeoutMs;          // ①류 30초
    std::uint64_t responseMs = common::protocol::kResponseTimeoutMs;  // ②류 120초
};

// 마지막으로 완료된 분석의 보관본 (design 8번 ResultRequest — 다운로드 재개).
// 청구표(filename/fileSize/uploadCrc32)는 인증이 아니라 "남의 결과를 건네지 않기" 위한 대조표다:
// 같은 파일을 가진 사람은 어차피 업로드로 같은 결과를 얻으므로 노출이 늘지 않는다.
struct RetainedResult {
    std::string filename;
    std::uint64_t fileSize = 0;
    std::uint32_t uploadCrc32 = 0;  // 트레일러와 대조가 끝난 페이로드 CRC
    std::string csv;                // 완성 CSV 전체 — 재요청 시 csv[startOffset..]을 보낸다
    std::uint32_t csvCrc32 = 0;     // 완성 CSV 전체의 CRC (조각의 것이 아니다)
};

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;
    // 분석이 완료된 세션만 호출한다 — 중단된 세션은 AnalysisResult 자체를 만들지 않으므로
    // (ParserThread::finishSession은 업로드 완료 시에만 실행) 검증되지 않은 결과가
    // 보관본에 들어갈 구조적 여지가 없다. 부분 결과 우회로가 생기지 않는다는 뜻이다.
    virtual void onResultRetained(RetainedResult) {}
    // 청구표 세 필드가 모두 맞는 보관본을 돌려준다. 없으면 nullptr (→ Ack(NoSuchResult)).
    // 반환 포인터는 관찰자 소유이며 세션이 응답을 보내는 동안 유효하다 (같은 루프 스레드)
    virtual const RetainedResult* lookupRetainedResult(const std::string&, std::uint64_t,
                                                       std::uint32_t) const {
        return nullptr;
    }
    // 소켓이 완전히 닫힌 뒤 호출 — 이 시점 이후 세션 객체를 파괴해도 안전하다.
    // 호출 문맥은 세션 자신의 콜백 안이므로 여기서 즉시 파괴하지 말 것 (지연 파괴)
    virtual void onSessionFinished() {}
};

class Session : public common::net::ISocketCallback {
public:
    Session(uv_loop_t* loop, std::unique_ptr<common::net::ISocket> socket,
            ISessionObserver* observer, server::parser::ParserThread* parser,
            std::size_t chunkSize, SessionTimeouts timeouts = SessionTimeouts{});
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

    // 파서 스레드가 uv_async로 깨워 루프 스레드에서 호출한다 (SessionManager가 라우팅)
    void onAnalysisComplete(server::parser::AnalysisResult result);
    void resumeReading();  // 링에 여유 생김 → uv_read_start 재개

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
    // WAIT_HEADER에 온 결과 재요청 — 보관본을 csv[startOffset..]부터 다시 보낸다.
    // 상태는 추가하지 않는다: 기존 송신 경로(SENDING_RESULT → WAIT_DONE → CLEANUP)를 그대로 쓴다
    void handleResultRequest();
    std::size_t consumePayload(std::string_view data);
    std::size_t consumeTrailer(std::string_view data);
    std::size_t consumeDone(std::string_view data);

    void verifyAndAck();
    void analyzeAndSendResult();
    void applyBackpressure();  // 링 가득 → uv_read_stop (50MB 상한 장치)

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

    server::parser::ParserThread* _parser = nullptr;
    // 헤더·트레일러 등 링에 넣지 않는 바이트용 소형 버퍼. 페이로드는 링 슬롯에 직접 수신된다
    std::vector<char> _readBuffer;
    bool _slotAcquired = false;  // onAllocate가 링 슬롯을 내줬는지 (onRead에서 커밋 판단)
    std::uint64_t _fileSize = 0;
    std::uint64_t _receivedBytes = 0;  // 페이로드만 집계 (프리앰블·트레일러 제외)
    std::uint32_t _payloadCrc = 0;     // 루프 스레드 소유 — 트레일러 비교 주체와 같은 스레드
    std::string _filename;

    std::string _csv;  // 메모리 완성본 — 전송·CRC·디스크 기록의 공통 소스
    std::string _finishReason;
    // 실패 Ack의 송신 완료를 기다리는 중 (design 8: "Ack 송신 완료 후 세션 종료").
    // send 직후 close하면 uv_close가 미완료 write를 취소할 수 있어 느린 링크에서
    // 사유가 유실된다 — onSendComplete에서 cleanup으로 이어진다
    bool _closeAfterSend = false;
    std::string _pendingFailReason;
    bool _cleanupStarted = false;
    bool _finished = false;
};

}  // namespace server::session
