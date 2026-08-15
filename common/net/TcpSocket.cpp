#include "net/TcpSocket.h"

#include <algorithm>
#include <cstring>

namespace common::net {

TcpSocket::TcpSocket(uv_loop_t* loop) : _handle(std::make_unique<uv_tcp_t>()) {
    if (uv_tcp_init(loop, _handle.get()) != 0) {
        _handle.reset();  // 초기화 실패한 핸들은 close 대상이 아니다 — 그냥 버린다
        return;
    }
    _handle->data = this;
}

TcpSocket::~TcpSocket() {
    // 소유자가 onClosed()를 기다리지 않고 파괴한 경우에도 사용 후 해제가 나지 않게 한다:
    // 죽은 this로 돌아오는 통로를 끊고, 핸들 메모리 소유권은 close 콜백으로 넘긴다
    for (WriteRequest* pending : _pendingWrites) {
        pending->socket = nullptr;
    }
    if (!_handle) {
        return;
    }
    if (_closed) {
        return;  // 이미 닫힘 — unique_ptr가 정상 해제
    }
    _handle->data = nullptr;
    uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(_handle.release());
    if (uv_is_closing(raw)) {
        return;  // 진행 중인 close 콜백이 data==nullptr을 보고 해제한다
    }
    uv_close(raw, onCloseCb);
}

int TcpSocket::connect(const std::string& ip, std::uint16_t port) {
    if (!_handle) {
        return UV_EINVAL;
    }
    struct sockaddr_in address {};
    int rc = uv_ip4_addr(ip.c_str(), static_cast<int>(port), &address);
    if (rc != 0) {
        return rc;
    }
    _connectRequest = std::make_unique<uv_connect_t>();
    _connectRequest->data = this;
    rc = uv_tcp_connect(_connectRequest.get(), _handle.get(),
                        reinterpret_cast<const struct sockaddr*>(&address), onConnectCb);
    if (rc != 0) {
        _connectRequest.reset();
    }
    return rc;
}

int TcpSocket::startRead() {
    if (!_handle || _closing) {
        return UV_EINVAL;
    }
    if (_reading) {
        return 0;  // 멱등 — backpressure 해제 경로에서 중복 호출될 수 있다
    }
    const int rc = uv_read_start(stream(), onAllocCb, onReadCb);
    if (rc == 0) {
        _reading = true;
    }
    return rc;
}

int TcpSocket::stopRead() {
    if (!_handle || !_reading) {
        return 0;
    }
    const int rc = uv_read_stop(stream());
    if (rc == 0) {
        _reading = false;
    }
    return rc;
}

int TcpSocket::send(std::string_view data) {
    if (!_handle || _closing) {
        return UV_EINVAL;
    }
    // 요청 버퍼로 복사한다 — 호출부가 완료 시점까지 수명을 관리하지 않아도 되게.
    // 메시지는 수 KB(Ack·ResultHeader·CSV)라 복사 비용이 무시 가능하다
    auto request = std::make_unique<WriteRequest>();
    request->buffer.assign(data.begin(), data.end());
    request->socket = this;
    request->request.data = request.get();

    uv_buf_t buf = uv_buf_init(request->buffer.data(),
                               static_cast<unsigned int>(request->buffer.size()));
    const int rc = uv_write(&request->request, stream(), &buf, 1, onWriteCb);
    if (rc != 0) {
        return rc;  // unique_ptr가 여기서 해제 — 콜백은 오지 않는다
    }
    _pendingWrites.push_back(request.get());
    request.release();  // 소유권을 완료 콜백으로 이동
    return 0;
}

void TcpSocket::close() {
    if (!_handle || _closing) {
        return;
    }
    _closing = true;
    stopRead();
    uv_close(reinterpret_cast<uv_handle_t*>(_handle.get()), onCloseCb);
    // 핸들 메모리는 _handle이 계속 소유한다 — 콜백에서 onClosed()만 알리고,
    // 실제 해제는 소유자가 이 객체를 파괴할 때 일어난다
}

std::string TcpSocket::peerAddress() const {
    if (!_handle) {
        return {};
    }
    struct sockaddr_storage storage {};
    int length = static_cast<int>(sizeof(storage));
    if (uv_tcp_getpeername(_handle.get(), reinterpret_cast<struct sockaddr*>(&storage), &length) !=
        0) {
        return {};
    }
    char ip[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const struct sockaddr_in*>(&storage);
        if (uv_ip4_name(v4, ip, sizeof(ip)) != 0) {
            return {};
        }
        port = ntohs(v4->sin_port);
    } else {
        const auto* v6 = reinterpret_cast<const struct sockaddr_in6*>(&storage);
        if (uv_ip6_name(v6, ip, sizeof(ip)) != 0) {
            return {};
        }
        port = ntohs(v6->sin6_port);
    }
    return std::string{ip} + ":" + std::to_string(port);
}

int TcpSocket::applyBufferSizes(int sendSize, int recvSize) {
    if (!_handle) {
        return UV_EINVAL;
    }
    auto* handle = reinterpret_cast<uv_handle_t*>(_handle.get());
    if (sendSize > 0) {
        int value = sendSize;
        const int rc = uv_send_buffer_size(handle, &value);  // value > 0 → 설정
        if (rc != 0) {
            return rc;
        }
    }
    if (recvSize > 0) {
        int value = recvSize;
        const int rc = uv_recv_buffer_size(handle, &value);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int TcpSocket::actualSendBufferSize() const {
    if (!_handle) {
        return 0;
    }
    int value = 0;  // value == 0 → 조회 (실제 적용값)
    auto* handle = const_cast<uv_handle_t*>(reinterpret_cast<const uv_handle_t*>(_handle.get()));
    return uv_send_buffer_size(handle, &value) == 0 ? value : 0;
}

int TcpSocket::actualRecvBufferSize() const {
    if (!_handle) {
        return 0;
    }
    int value = 0;
    auto* handle = const_cast<uv_handle_t*>(reinterpret_cast<const uv_handle_t*>(_handle.get()));
    return uv_recv_buffer_size(handle, &value) == 0 ? value : 0;
}

void TcpSocket::onAllocCb(uv_handle_t* handle, std::size_t suggested, uv_buf_t* buf) {
    auto* self = static_cast<TcpSocket*>(handle->data);
    if (self == nullptr || self->_callback == nullptr) {
        *buf = uv_buf_init(nullptr, 0);  // UV_ENOBUFS로 이어져 onError에서 드러난다
        return;
    }
    const WritableBuffer slot = self->_callback->onAllocate(suggested);
    *buf = uv_buf_init(slot.data, static_cast<unsigned int>(slot.size));
}

void TcpSocket::onReadCb(uv_stream_t* stream, std::ptrdiff_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<TcpSocket*>(stream->data);
    if (self == nullptr || self->_callback == nullptr) {
        return;
    }
    if (nread > 0) {
        self->_callback->onRead(std::string_view{buf->base, static_cast<std::size_t>(nread)});
        return;
    }
    if (nread == 0) {
        return;  // EAGAIN 상당 — 데이터 없음, 에러 아님
    }
    // EOF도 에러도 같은 정리 경로로 수렴한다 (총괄 원칙 ③)
    self->_callback->onError(static_cast<int>(nread),
                             nread == UV_EOF ? "read(eof)" : "read");
}

void TcpSocket::onWriteCb(uv_write_t* request, int status) {
    std::unique_ptr<WriteRequest> owned{static_cast<WriteRequest*>(request->data)};
    TcpSocket* self = owned->socket;
    if (self == nullptr) {
        return;  // 소켓이 먼저 파괴됨 — 버퍼만 해제하고 조용히 끝낸다
    }
    auto& pending = self->_pendingWrites;
    pending.erase(std::remove(pending.begin(), pending.end(), owned.get()), pending.end());
    if (self->_callback != nullptr) {
        self->_callback->onSendComplete(status);
        if (status != 0) {
            self->_callback->onError(status, "write");
        }
    }
}

void TcpSocket::onConnectCb(uv_connect_t* request, int status) {
    auto* self = static_cast<TcpSocket*>(request->data);
    if (self != nullptr && self->_callback != nullptr) {
        self->_callback->onConnect(status);
    }
}

void TcpSocket::onCloseCb(uv_handle_t* handle) {
    auto* self = static_cast<TcpSocket*>(handle->data);
    if (self == nullptr) {
        // 소유자가 이미 파괴됨 — 핸들 메모리 해제가 이 콜백의 유일한 책임
        std::unique_ptr<uv_tcp_t> owned{reinterpret_cast<uv_tcp_t*>(handle)};
        return;
    }
    self->_closed = true;
    self->_reading = false;
    if (self->_callback != nullptr) {
        self->_callback->onClosed();
    }
}

std::unique_ptr<ISocket> createTcpSocket(uv_loop_t* loop) {
    return std::make_unique<TcpSocket>(loop);
}

}  // namespace common::net
