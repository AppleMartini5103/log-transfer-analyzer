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
#include "service/Command.h"
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

private:
    class SocketCallback;

    void runLoop();
    void drainCommands();
    void handle(const ConnectCommand& command);
    void handle(const DisconnectCommand& command);
    void handle(const QuitCommand& command);
    void closeSocket();
    void pushEvent(Event event);
    void pushLog(common::LogLevel level, std::string message);
    void pushLink(LinkState link);

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
};

}  // namespace client
