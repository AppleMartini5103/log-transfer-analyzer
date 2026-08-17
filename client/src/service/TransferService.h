#pragma once

#include <uv.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <optional>

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

    // 연결 시작 공통부 (사용자 Connect / Send 시점 재연결이 같은 코드를 쓴다)
    bool beginConnect(const std::string& ip, std::uint16_t port);
    // 재연결이 끝난 뒤 보류해 둔 업로드를 이어서 실행
    void resumePendingUpload();
    void startUpload(const StartUploadCommand& command);
    void handle(const CancelUploadCommand& command);
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

    // 마지막으로 연결한 주소 — Send 시점 재연결이 재사용한다 (사용자에게 다시 묻지 않는다)
    std::string _lastIp;
    std::uint16_t _lastPort = 0;

    // 재연결 중 보류해 둔 업로드. 값 하나뿐이라 optional로 충분하다.
    // ★ 재연결은 "아직 아무것도 안 보낸 상태"에서만 안전하다 — 전송 도중 끊김은 재개
    //   프로토콜이 없어 처음부터 다시가 맞고, 그 판단은 사용자 몫이다 (design 12번 금지선).
    std::optional<StartUploadCommand> _pendingUpload;
    bool _reconnecting = false;

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
    static constexpr std::size_t kMaxInFlightWrites = 8;  // 8 × 64KB = 0.5MB

    // 업로드 진행 상태 (루프 스레드 전용)
    bool _uploading = false;
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
    enum class Expecting : std::uint8_t { Nothing, Ack, ResultHeader, CsvPayload };
    Expecting _expecting = Expecting::Nothing;
    common::protocol::Framer _framer{common::protocol::MessageType::Ack};

    std::string _csv;             // 수신 중인 result.csv (수 KB — 메모리 보관이 정상)
    std::uint64_t _csvSize = 0;
    std::uint32_t _csvExpectedCrc = 0;
    float _lastReportedDownload = -1.0f;

    std::mutex _resultMutex;      // _resultCsv만 보호 (UI가 가져가므로 스레드 경계)
    std::string _resultCsv;
};

}  // namespace client
