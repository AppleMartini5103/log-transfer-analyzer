#include "service/TransferService.h"

#include "net/TcpSocket.h"

namespace client {

// 소켓 이벤트를 서비스로 되돌리는 옵저버. 기본 구현이 빈 몸체라 필요한 것만 오버라이드한다.
class TransferService::SocketCallback : public common::net::ISocketCallback {
public:
    explicit SocketCallback(TransferService& service) : _service(service) {}

    void onConnect(int status) override {
        if (status < 0) {
            // 실패 사유를 그대로 보여준다 — "연결 안 됨"만으로는 방화벽인지 서버가 죽은
            // 것인지 알 수 없다 (그 구분이 Ping 버튼의 존재 이유이기도 하다).
            _service.pushLog(common::LogLevel::Error,
                             std::string("Connect failed: ") + ::uv_strerror(status));
            _service.pushLink(LinkState::Disconnected);
            _service.closeSocket();
            return;
        }
        _service.pushLog(common::LogLevel::Info, "Connected to " + _service._socket->peerAddress());
        _service.pushLink(LinkState::Connected);

        // 읽기를 걸어둬야 서버가 끊는 것을 EOF로 감지할 수 있다 (design 12번: 감지는 항상 즉시).
        const int result = _service._socket->startRead();
        if (result < 0) {
            _service.pushLog(common::LogLevel::Error,
                             std::string("startRead failed: ") + ::uv_strerror(result));
            _service.closeSocket();
        }
    }

    common::net::WritableBuffer onAllocate(std::size_t suggestedSize) override {
        // 아직 프로토콜 수신이 없으므로 작은 임시 버퍼면 충분하다. 업로드 이슈에서
        // 링버퍼 슬롯으로 교체된다 (design 추가 설계 2번: 슬롯 직결로 복사 0회).
        _scratch.resize(suggestedSize > 0 ? suggestedSize : 1);
        return common::net::WritableBuffer{_scratch.data(), _scratch.size()};
    }

    void onRead(std::string_view data) override {
        // 이 단계에서는 서버가 먼저 보내는 것이 없다 — 오면 프로토콜 위반이므로 남겨만 둔다.
        _service.pushLog(common::LogLevel::Warn,
                         "Unexpected " + std::to_string(data.size()) + " bytes from server");
    }

    void onError(int status, std::string_view where) override {
        _service.pushLog(common::LogLevel::Error, std::string(where) + ": " + ::uv_strerror(status));
        _service.pushLink(LinkState::Disconnected);
        _service.closeSocket();
    }

    void onClosed() override {
        // 소켓이 완전히 닫힌 뒤에야 객체를 버린다 (TcpSocket 수명 규칙).
        _service._socket.reset();
        _service.pushLink(LinkState::Disconnected);
    }

private:
    TransferService& _service;
    std::vector<char> _scratch;
};

TransferService::TransferService() = default;

TransferService::~TransferService() {
    stop();
}

bool TransferService::start(std::string& error) {
    if (_running.load()) {
        return true;
    }

    int result = ::uv_loop_init(&_loop);
    if (result < 0) {
        error = std::string("uv_loop_init failed: ") + ::uv_strerror(result);
        return false;
    }
    _loopInitialized = true;

    // async 핸들은 루프가 도는 동안 계속 살아 있어야 하므로 여기서 한 번만 만든다.
    result = ::uv_async_init(&_loop, &_wakeup, &TransferService::onAsyncCb);
    if (result < 0) {
        error = std::string("uv_async_init failed: ") + ::uv_strerror(result);
        ::uv_loop_close(&_loop);
        _loopInitialized = false;
        return false;
    }
    _wakeup.data = this;

    _socketCallback = std::make_unique<SocketCallback>(*this);
    _running.store(true);
    _thread = std::thread([this] { runLoop(); });
    return true;
}

void TransferService::stop() {
    if (_thread.joinable()) {
        post(QuitCommand{});
        _thread.join();
    }
    _running.store(false);

    if (_loopInitialized) {
        ::uv_loop_close(&_loop);
        _loopInitialized = false;
    }
}

void TransferService::post(Command command) {
    {
        const std::lock_guard<std::mutex> lock(_commandMutex);
        _commands.push_back(std::move(command));
    }
    // 타 스레드에서 부를 수 있는 유일한 libuv API (컨벤션 4번).
    ::uv_async_send(&_wakeup);
}

std::deque<TransferService::Event> TransferService::drainEvents() {
    std::deque<Event> events;
    const std::lock_guard<std::mutex> lock(_eventMutex);
    events.swap(_events);
    return events;
}

void TransferService::runLoop() {
    ::uv_run(&_loop, UV_RUN_DEFAULT);
}

void TransferService::onAsyncCb(uv_async_t* handle) {
    auto* self = static_cast<TransferService*>(handle->data);
    self->drainCommands();
}

void TransferService::drainCommands() {
    // 큐를 전부 비운다: uv_async_send를 N번 불러도 콜백이 1번으로 합쳐질 수 있어
    // 하나만 꺼내면 명령이 씹힌다 (design 7번 coalescing).
    for (;;) {
        Command command;
        {
            const std::lock_guard<std::mutex> lock(_commandMutex);
            if (_commands.empty()) {
                return;
            }
            command = std::move(_commands.front());
            _commands.pop_front();
        }

        std::visit([this](const auto& typed) { handle(typed); }, command);
    }
}

void TransferService::handle(const ConnectCommand& command) {
    if (_socket) {
        pushLog(common::LogLevel::Warn, "Already connected or connecting - ignoring Connect.");
        return;
    }

    _socket = common::net::createTcpSocket(&_loop);
    _socket->setCallback(_socketCallback.get());
    pushLink(LinkState::Reconnecting);  // 시도 중 = 노랑

    const int result = _socket->connect(command.ip, command.port);
    if (result < 0) {
        pushLog(common::LogLevel::Error,
                std::string("Connect request failed: ") + ::uv_strerror(result));
        pushLink(LinkState::Disconnected);
        _socket.reset();
        return;
    }
    pushLog(common::LogLevel::Info,
            "Connecting to " + command.ip + ":" + std::to_string(command.port) + "...");
}

void TransferService::handle(const DisconnectCommand&) {
    if (!_socket) {
        pushLink(LinkState::Disconnected);
        return;
    }
    pushLog(common::LogLevel::Info, "Disconnecting.");
    closeSocket();
}

void TransferService::handle(const QuitCommand&) {
    closeSocket();
    // 남은 핸들(async 등)을 모두 닫으면 uv_run이 반환한다 — uv_stop보다 정리가 확실하다.
    ::uv_walk(&_loop, &TransferService::onWalkCloseCb, nullptr);
}

void TransferService::onWalkCloseCb(uv_handle_t* handle, void* /*arg*/) {
    if (::uv_is_closing(handle) == 0) {
        ::uv_close(handle, nullptr);
    }
}

void TransferService::closeSocket() {
    if (_socket && !_socket->isClosing()) {
        _socket->close();  // 실제 파괴는 onClosed에서 (TcpSocket 수명 규칙)
    }
}

void TransferService::pushEvent(Event event) {
    const std::lock_guard<std::mutex> lock(_eventMutex);
    _events.push_back(std::move(event));
}

void TransferService::pushLog(common::LogLevel level, std::string message) {
    Event event;
    event.level = level;
    event.message = std::move(message);
    pushEvent(std::move(event));
}

void TransferService::pushLink(LinkState link) {
    Event event;
    event.link = link;
    event.hasLink = true;
    pushEvent(std::move(event));
}

}  // namespace client
