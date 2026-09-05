#pragma once

#include <uv.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "net/ISocket.h"
#include "net/Timer.h"
#include "protocol/Framer.h"
#include "service/Command.h"
#include "service/FileReader.h"
#include "ui/UiState.h"

namespace client {

// 워커: uv 루프를 소유하고 실제 소켓 조작을 수행한다.
//
// [스레드 수명·소유권] design 7번 총괄표의 "uv 루프(클라)" 행에 해당한다.
//   생성    : start() 시 1개, 종료까지 상주
//   소유    : 소켓, 커맨드 큐(소비), uv 핸들(async/타이머)
//   잠/깨움 : epoll/IOCP에서 대기 <- uv_async_send
//   종료    : QuitCommand -> 핸들 close -> uv_run 반환 -> join
//   중단    : 소켓 close 후 IDLE 복귀 (에러·취소·타임아웃이 모두 같은 경로)
//
// [UI와의 경계]
//   UI -> 워커 : post()로 커맨드 큐에 넣고 uv_async_send (이 클래스에서 유일하게
//                타 스레드에서 호출해도 되는 메서드)
//   워커 -> UI : 이벤트를 뮤텍스 보호 큐에 쌓고, UI가 매 프레임 drainEvents()로 가져간다.
//                워커가 UiState를 직접 만지면 ImGui 렌더 도중 상태가 바뀌어 화면이
//                프레임 안에서 어긋난다 (design 7번: 진행률/로그는 폴링으로 전달).
class TransferService {
public:
    // 워커가 UI에 알리는 사건. 값 타입으로 복사해 넘기므로 소유권 문제가 없다.
    struct Event {
        LinkState link = LinkState::Disconnected;
        bool hasLink = false;

        SessionState session = SessionState::Idle;
        bool hasSession = false;

        float uploadProgress = 0.0f;
        bool hasUploadProgress = false;

        float downloadProgress = 0.0f;
        bool hasDownloadProgress = false;

        // 서버가 이 클라이언트의 결과를 아직 들고 있을 가능성 — Get result의 근거.
        // 워커만이 청구표(파일명·크기·CRC)와 재개 지점을 알고 있으므로 UI에 통보한다.
        bool resultClaimAvailable = false;
        bool hasResultClaim = false;

        common::LogLevel level = common::LogLevel::Info;
        std::string message;  // 비어 있으면 로그 없음
    };

    // 생성자·소멸자 모두 .cpp에 정의한다: _socketCallback이 불완전 타입(SocketCallback)을
    // 가리키는 unique_ptr이라, 인라인 = default면 예외 정리 경로에서 멤버 소멸자를
    // 인스턴스화하며 "can't delete an incomplete type"로 깨진다 (pimpl 규칙).
    TransferService();
    ~TransferService();

    TransferService(const TransferService&) = delete;
    TransferService& operator=(const TransferService&) = delete;

    // 루프·핸들을 초기화하고 워커 스레드를 띄운다. 실패 시 false + error.
    bool start(std::string& error);

    // QuitCommand를 보내고 스레드를 join한다. 여러 번 불러도 안전하다.
    void stop();

    // 타 스레드(UI)에서 호출 가능한 유일한 지점.
    void post(Command command);

    // UI 스레드가 매 프레임 호출해 쌓인 이벤트를 가져간다.
    std::deque<Event> drainEvents();

    // 수신한 result.csv를 UI가 가져가 저장한다 (Save 버튼). 세션당 한 번만 채워지고,
    // 다음 업로드 시작 시 비워진다. 수 KB라 값 복사로 넘겨도 부담이 없다.
    std::string takeResultCsv();

private:
    class SocketCallback;

    void runLoop();
    void drainCommands();
    void handle(const ConnectCommand& command);
    void handle(const DisconnectCommand& command);
    void handle(const StartUploadCommand& command);

    bool beginConnect(const std::string& ip, std::uint16_t port);
    void handle(const CancelUploadCommand& command);
    void handle(const RequestResultCommand& command);
    void handle(const QuitCommand& command);
    void closeSocket();
    void pushEvent(Event event);
    void pushLog(common::LogLevel level, std::string message);
    void pushLink(LinkState link);
    void pushSession(SessionState session);

    // 링에 쌓인 청크를 소켓으로 흘려보낸다 (루프 스레드 전용).
    void pumpUpload();
    void finishUpload();
    void abortUpload(const std::string& reason, common::LogLevel level);
    void drainRing();

    // 수신 경로 (루프 스레드 전용): 스트림을 메시지·페이로드로 갈라 처리한다.
    void onBytes(std::string_view data);
    std::size_t handleAckBytes(std::string_view data);
    std::size_t handleResultHeaderBytes(std::string_view data);
    std::size_t handleCsvBytes(std::string_view data);
    // 재요청의 응답은 Ack(거절) 또는 ResultHeader(수락) 둘 중 하나다.
    std::size_t handleResumeReplyBytes(std::string_view data);
    bool beginCsvReceive(const common::protocol::ResultHeader& header, bool resuming);
    void dropResultClaim();
    void finishDownload();
    void failSession(const std::string& reason, common::LogLevel level);

    static void onAsyncCb(uv_async_t* handle);
    static void onWalkCloseCb(uv_handle_t* handle, void* arg);

    uv_loop_t _loop{};
    uv_async_t _wakeup{};
    bool _loopInitialized = false;

    std::thread _thread;
    std::atomic<bool> _running{false};

    std::mutex _commandMutex;
    std::deque<Command> _commands;

    std::mutex _eventMutex;
    std::deque<Event> _events;

    // 루프 스레드 전용 — 다른 스레드에서 접근 금지
    std::unique_ptr<common::net::ISocket> _socket;
    std::unique_ptr<SocketCallback> _socketCallback;
    std::unique_ptr<common::net::Timer> _connectTimer;

    // 직전 세션이 정상 완료(DownloadDone 송신)됐는가 — EOF를 Info로 볼지 Warn으로 볼지의 기준.
    // 완료 후 서버가 닫는 것은 프로토콜의 정상 종료이고, 그 전에 닫히는 것은 유휴 정리다.
    bool _sessionCompleted = false;

    FileReader _reader;

    // 동시에 libuv에 맡겨 둘 수 있는 쓰기 요청 수.
    // ★ 이 상한이 클라이언트 측 메모리 상한 장치다. uv_write는 비동기라 send()가 성공해도
    //   "큐에 넣었다"는 뜻일 뿐이고, libuv가 요청마다 데이터를 복사해 들고 있는다.
    //   완료를 기다리지 않고 계속 밀어 넣으면 링버퍼(4MB)와 무관하게 파일 전체가 쓰기 큐로
    //   쌓인다 — 실측에서 483MB 업로드 시 peak RSS 572MB로 50MB 제약을 위반했다.
    //   서버가 링이 차면 uv_read_stop 하는 것과 대칭으로, 클라는 미완료 쓰기가 상한에
    //   도달하면 송신을 멈추고 onSendComplete에서 재개한다.
    //
    // [8이 처리량을 제한하지 않음 — 실측 확인, design "실물 클라이언트 실측" 절]
    //   이 값을 "보수적으로 잡은 자리표시자"로 적어 두고 11MB/s의 원인으로 의심했으나 틀렸다.
    //   코드를 그대로 두고 링크만 100Mbps → 1Gbps로 바꿔 11 → 97MB/s(8.8배)가 나왔다.
    //   uv_write 완료는 상대 ACK가 아니라 커널 송신 버퍼에 들어간 시점이므로, LAN의 sub-ms
    //   RTT에서 0.5MB in-flight면 수백 MB/s를 낼 수 있다. 올릴 이유가 없다 — 올리면 처리량은
    //   그대로인 채 메모리 상한만 0.5MB에서 커진다.
    static constexpr std::size_t kMaxInFlightWrites = 8;  // 8 × 64KB = 0.5MB

    // 업로드 진행 상태 (루프 스레드 전용)
    bool _uploading = false;

    // 업로드 시작 시각 (uv_hrtime, 나노초). 완료 로그에 소요 시간·처리량을 함께 남기기 위한 값.
    // → 왜 벽시계가 아니라 uv_hrtime인가: 로그 타임스탬프는 1초 해상도라 4초와 4.9초를 구분할
    //   수 없다. 실제로 [39] 처리량 측정에서 그 때문에 "80~120MB/s 구간"으로밖에 기록하지
    //   못했다. 단조 시계로 재면 그 폭이 사라지고, NTP 시각 보정에도 영향받지 않는다.
    std::uint64_t _uploadStartedAt = 0;
    std::size_t _inFlightWrites = 0;
    bool _trailerSent = false;
    std::uint64_t _uploadTotal = 0;
    std::uint64_t _uploadSent = 0;
    // CRC는 루프 스레드가 송신 시점에 증분 계산한다 — 트레일러를 만드는 주체와 같은
    // 스레드여야 파서/리더의 진행을 기다릴 필요가 없다 (design 8번 확정).
    std::uint32_t _uploadCrc = 0;
    float _lastReportedProgress = -1.0f;

    // 수신 상태 (루프 스레드 전용). 세션 순서가 고정이라 "지금 기대하는 것"이 하나로 정해진다:
    // Ack -> ResultHeader -> CSV 페이로드 -> (DownloadDone 송신) -> 완료
    enum class Expecting : std::uint8_t { Nothing, Ack, ResultHeader, CsvPayload, ResumeReply };
    Expecting _expecting = Expecting::Nothing;
    common::protocol::Framer _framer{common::protocol::MessageType::Ack};

    std::string _csv;             // 수신 중인 result.csv (수 KB — 메모리 보관이 정상)
    std::uint64_t _csvSize = 0;
    std::uint32_t _csvExpectedCrc = 0;
    float _lastReportedDownload = -1.0f;

    // ── 결과 청구권 ─────────────────────────────────────────────────────────
    // 끊긴 결과 수신이 잃는 것은 CSV 몇 KB가 아니라 500MB 재업로드다. 서버는 마지막
    // 완료 분석을 보관하고 있으므로, 그것을 만든 업로드의 파일명·크기·CRC를 들고 있으면
    // 재업로드 없이 이어 받을 수 있다. 세 값은 이미 업로드에 실어 보낸 것들이라 새로
    // 만들어 낼 상태가 아니다.
    //
    // 켜는 시점이 Ack(Ok) 수신인 이유: 그때가 "서버가 이 업로드를 받아들였고 결과를
    // 만들 것"이 확정되는 지점이다. 그전에 켜면 CRC 불일치로 거절된 업로드에 대해서도
    // 있지도 않은 결과를 청구하게 된다.
    struct ResultClaim {
        std::string filename;
        std::uint64_t fileSize = 0;
        std::uint32_t crc32 = 0;
        std::uint64_t received = 0;  // 이미 받은 CSV 바이트 수 = 다음 재개 지점
    };
    bool _hasClaim = false;
    ResultClaim _claim;
    std::string _uploadFilename;  // 헤더에 실어 보낸 이름 — 청구표를 만들 때 필요하다

    std::mutex _resultMutex;      // _resultCsv만 보호 (UI가 가져가므로 스레드 경계)
    std::string _resultCsv;
};

}  // namespace client
