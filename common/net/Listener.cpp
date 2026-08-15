#include "net/Listener.h"

namespace common::net {

Listener::Listener(uv_loop_t* loop) : _handle(std::make_unique<uv_tcp_t>()), _loop(loop) {
    if (uv_tcp_init(loop, _handle.get()) != 0) {
        _handle.reset();
        return;
    }
    _handle->data = this;
}

Listener::~Listener() {
    if (!_handle || _closed) {
        return;
    }
    _handle->data = nullptr;  // 죽은 this로 돌아오는 통로 차단 (TcpSocket과 동일 규칙)
    uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(_handle.release());
    if (uv_is_closing(raw)) {
        return;
    }
    uv_close(raw, onCloseCb);
}

int Listener::listen(const std::string& ip, std::uint16_t port, int backlog,
                     IListenerCallback* callback) {
    if (!_handle) {
        return UV_EINVAL;
    }
    _callback = callback;

    struct sockaddr_in address {};
    int rc = uv_ip4_addr(ip.c_str(), static_cast<int>(port), &address);
    if (rc != 0) {
        return rc;
    }
    // SO_REUSEADDR 상당은 libuv가 기본 적용 — 재시작 시 TIME_WAIT로 bind가 막히지 않는다
    rc = uv_tcp_bind(_handle.get(), reinterpret_cast<const struct sockaddr*>(&address), 0);
    if (rc != 0) {
        return rc;
    }
    return uv_listen(reinterpret_cast<uv_stream_t*>(_handle.get()), backlog, onConnectionCb);
}

std::uint16_t Listener::boundPort() const {
    if (!_handle) {
        return 0;
    }
    struct sockaddr_storage storage {};
    int length = static_cast<int>(sizeof(storage));
    if (uv_tcp_getsockname(_handle.get(), reinterpret_cast<struct sockaddr*>(&storage), &length) !=
        0) {
        return 0;
    }
    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const struct sockaddr_in*>(&storage)->sin_port);
    }
    return ntohs(reinterpret_cast<const struct sockaddr_in6*>(&storage)->sin6_port);
}

std::unique_ptr<ISocket> Listener::acceptPending() {
    if (!_handle || _closing) {
        return nullptr;
    }
    auto socket = std::make_unique<TcpSocket>(_loop);
    if (!socket->valid()) {
        return nullptr;
    }
    // 대기분이 없으면 UV_EAGAIN — 에러가 아니라 "지금은 없음"
    if (uv_accept(reinterpret_cast<uv_stream_t*>(_handle.get()), socket->stream()) != 0) {
        return nullptr;
    }
    return socket;
}

void Listener::close() {
    if (!_handle || _closing) {
        return;
    }
    _closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(_handle.get()), onCloseCb);
}

void Listener::onConnectionCb(uv_stream_t* server, int status) {
    auto* self = static_cast<Listener*>(server->data);
    if (self == nullptr || self->_callback == nullptr) {
        return;
    }
    if (status != 0) {
        self->_callback->onListenError(status);
        return;
    }
    // 여기서 accept를 호출하지 않아도 연결은 백로그에 남는다 (스파이크 검증 완료).
    // 세션이 진행 중이면 호출부가 그냥 무시하고, CLEANUP 후 acceptPending()으로 받는다
    self->_callback->onConnection();
}

void Listener::onCloseCb(uv_handle_t* handle) {
    auto* self = static_cast<Listener*>(handle->data);
    if (self == nullptr) {
        std::unique_ptr<uv_tcp_t> owned{reinterpret_cast<uv_tcp_t*>(handle)};
        return;
    }
    self->_closed = true;
}

}  // namespace common::net
