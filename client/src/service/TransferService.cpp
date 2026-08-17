#include "service/TransferService.h"

#include "net/TcpSocket.h"
#include "protocol/Codec.h"
#include "util/Crc32.h"

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

    // 쓰기 완료 = libuv가 그 청크의 복사본을 놓았다는 뜻. 이때만 다음 청크를 밀어 넣어야
    // 메모리에 쌓이는 양이 상한 안에 묶인다 (서버의 uv_read_stop과 대칭).
    void onSendComplete(int status) override {
        if (_service._inFlightWrites > 0) {
            --_service._inFlightWrites;
        }
        if (status < 0) {
            if (_service._uploading) {
                _service.abortUpload(std::string("Send failed: ") + ::uv_strerror(status),
                                     common::LogLevel::Error);
            }
            return;
        }
        _service.pumpUpload();
    }

    void onRead(std::string_view data) override {
        // 이 단계에서는 서버가 먼저 보내는 것이 없다 — 오면 프로토콜 위반이므로 남겨만 둔다.
        _service.pushLog(common::LogLevel::Warn,
                         "Unexpected " + std::to_string(data.size()) + " bytes from server");
    }

    void onError(int status, std::string_view where) override {
        // 서버의 정상 종료(EOF)와 예기치 않은 단절을 구분해 표기한다 (design 12번:
        // 정상 종료는 Info, 그 외는 Warn/Error). 업로드 중이라면 어느 쪽이든 실패다.
        const bool eof = (status == UV_EOF);
        const auto level = eof ? common::LogLevel::Info : common::LogLevel::Error;
        _service.pushLog(level, eof ? "Server closed the connection."
                                    : std::string(where) + ": " + ::uv_strerror(status));

        if (_service._uploading) {
            _service.abortUpload("Upload aborted: the connection was lost.",
                                 common::LogLevel::Error);
        }
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

    // 리더가 링을 채우면 루프를 깨워 전송하게 한다. 리더 스레드에서 불리므로
    // uv_async_send만 쓴다 (타 스레드에서 안전한 유일한 libuv API).
    _reader.start([this] { ::uv_async_send(&_wakeup); });

    _running.store(true);
    _thread = std::thread([this] { runLoop(); });
    return true;
}

void TransferService::stop() {
    if (_thread.joinable()) {
        post(QuitCommand{});
        _thread.join();
    }
    // 루프가 멈춘 뒤 리더를 세운다 — 순서가 반대면 리더가 이미 죽은 루프를 깨우려 한다.
    _reader.stop();
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
    // 이 async는 두 가지 신호를 겸한다: UI의 명령과, 리더가 "링에 데이터 넣었다"는 알림.
    // coalescing 때문에 어느 쪽인지 구분할 수 없으므로 매번 둘 다 처리한다.
    self->drainCommands();
    self->pumpUpload();
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

void TransferService::handle(const StartUploadCommand& command) {
    if (!_socket || _socket->isClosing()) {
        pushLog(common::LogLevel::Error, "Not connected - cannot start the upload.");
        return;
    }
    if (_uploading) {
        pushLog(common::LogLevel::Warn, "An upload is already in progress.");
        return;
    }

    // 이전 세션의 잔여 슬롯을 여기서 비운다 — release는 consumer(루프 스레드)만 부를 수 있다.
    drainRing();

    std::string error;
    if (!_reader.beginUpload(command.path, command.size, error)) {
        pushLog(common::LogLevel::Error, error);
        return;
    }

    _uploading = true;
    _uploadTotal = command.size;
    _uploadSent = 0;
    _uploadCrc = 0;
    _lastReportedProgress = -1.0f;
    _inFlightWrites = 0;
    _trailerSent = false;

    common::protocol::UploadHeader header;
    header.fileSize = command.size;
    header.filename = command.filename;
    const std::vector<char> encoded = common::protocol::encode(header);

    pushSession(SessionState::SendingHeader);
    const int result = _socket->send(std::string_view(encoded.data(), encoded.size()));
    if (result < 0) {
        abortUpload(std::string("Failed to send the upload header: ") + ::uv_strerror(result),
                    common::LogLevel::Error);
        return;
    }

    pushLog(common::LogLevel::Info, "Uploading " + command.filename + " (" +
                                        std::to_string(command.size) + " bytes)");
    pushSession(SessionState::Streaming);
    pumpUpload();
}

void TransferService::handle(const CancelUploadCommand&) {
    if (!_uploading) {
        return;
    }
    // 취소는 프로토콜상 재개가 없다 — 스트림이 중간에 끊기므로 연결을 닫는 것이
    // 서버 입장에서도 명확하다 (design 12번: 전송 도중 끊김은 처음부터 다시).
    abortUpload("Upload cancelled by user.", common::LogLevel::Warn);
}

// 링에 쌓인 청크를 소켓으로 흘려보낸다. 한 번에 다 비우지 않고 링이 빌 때까지만 돌며,
// 리더가 다시 채우면 async 콜백으로 재진입한다.
void TransferService::pumpUpload() {
    if (!_uploading || !_socket || _socket->isClosing()) {
        return;
    }

    if (_reader.failed()) {
        abortUpload(_reader.takeError(), common::LogLevel::Error);
        return;
    }

    while (_inFlightWrites < kMaxInFlightWrites) {
        common::SpscRingBuffer::ReadView view;
        if (!_reader.ring().tryPeek(view)) {
            break;
        }

        const std::string_view chunk(view.data, view.size);
        // CRC는 보내기 직전에 누적한다 — 트레일러를 만드는 주체와 같은 스레드 (design 8번)
        _uploadCrc = common::crc32(_uploadCrc, chunk);

        const int result = _socket->send(chunk);
        ++_inFlightWrites;
        // send()가 데이터를 자체 버퍼로 복사하므로 슬롯은 여기서 반납해도 안전하다.
        // 다만 "다음 청크를 읽어도 되는가"는 링이 아니라 미완료 쓰기 수가 정한다.
        _reader.ring().release();
        _reader.notifySpaceAvailable();

        if (result < 0) {
            abortUpload(std::string("Send failed: ") + ::uv_strerror(result),
                        common::LogLevel::Error);
            return;
        }

        _uploadSent += view.size;

        // 진행률은 1% 단위로만 올린다 — 8000개 청크마다 이벤트를 만들면 UI 큐가 넘친다
        // (로그·이벤트는 핫 패스에서 억제한다는 컨벤션 8번과 같은 취지).
        const float progress =
            _uploadTotal > 0 ? static_cast<float>(static_cast<double>(_uploadSent) /
                                                  static_cast<double>(_uploadTotal))
                             : 1.0f;
        if (progress - _lastReportedProgress >= 0.01f || progress >= 1.0f) {
            _lastReportedProgress = progress;
            Event event;
            event.uploadProgress = progress;
            event.hasUploadProgress = true;
            pushEvent(std::move(event));
        }
    }

    // 읽기가 끝났고 링도 비었다면 트레일러를 보낸다.
    if (!_trailerSent && _reader.readComplete() && _reader.ring().empty() &&
        _uploadSent >= _uploadTotal) {
        finishUpload();
    }
}

void TransferService::finishUpload() {
    _trailerSent = true;
    common::protocol::UploadTrailer trailer;
    trailer.crc32 = _uploadCrc;
    const std::vector<char> encoded = common::protocol::encode(trailer);

    const int result = _socket->send(std::string_view(encoded.data(), encoded.size()));
    if (result < 0) {
        abortUpload(std::string("Failed to send the upload trailer: ") + ::uv_strerror(result),
                    common::LogLevel::Error);
        return;
    }

    _uploading = false;
    pushLog(common::LogLevel::Info,
            "Upload finished (" + std::to_string(_uploadSent) + " bytes, crc32=" +
                std::to_string(_uploadCrc) + ") - waiting for ack");
    pushSession(SessionState::WaitAck);
}

void TransferService::abortUpload(const std::string& reason, common::LogLevel level) {
    _reader.abortUpload();
    _uploading = false;
    drainRing();

    pushLog(level, reason);
    // 스트림이 헤더와 어긋난 상태이므로 연결을 닫는다 — 취소·에러가 같은 경로로 수렴한다.
    closeSocket();
    pushSession(SessionState::Idle);

    Event event;
    event.uploadProgress = 0.0f;
    event.hasUploadProgress = true;
    pushEvent(std::move(event));
}

void TransferService::drainRing() {
    common::SpscRingBuffer::ReadView view;
    while (_reader.ring().tryPeek(view)) {
        _reader.ring().release();
    }
    _reader.notifySpaceAvailable();
}

void TransferService::handle(const QuitCommand&) {
    _reader.abortUpload();
    _uploading = false;
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

void TransferService::pushSession(SessionState session) {
    Event event;
    event.session = session;
    event.hasSession = true;
    pushEvent(std::move(event));
}

}  // namespace client
