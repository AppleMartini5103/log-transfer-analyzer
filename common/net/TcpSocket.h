#pragma once

#include "net/ISocket.h"

#include <uv.h>

#include <memory>
#include <vector>

// TCP 소켓 구현 (libuv 래핑).
//
// [수명 규칙 — 반드시 지킬 것]
//   uv_close는 비동기다. 핸들 메모리는 close 콜백이 불릴 때까지 살아 있어야 한다.
//   그래서 uv_tcp_t를 별도로 할당해 unique_ptr로 들고, 소켓 객체가 먼저 파괴되면
//   소유권을 close 콜백으로 넘겨 거기서 해제한다 (사용 후 해제 방지).
//   소유자는 onClosed() 이후에 파괴하는 것이 정석이며, 그전에 파괴해도 안전하도록 방어한다.
//
// [스레드 규칙]
//   모든 메서드는 루프를 돌리는 스레드에서만 호출한다. 타 스레드에서 허용되는 libuv API는
//   uv_async_send 하나뿐이다 (컨벤션 4번).

namespace common::net {

class TcpSocket : public ISocket {
public:
    explicit TcpSocket(uv_loop_t* loop);
    ~TcpSocket() override;

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    void setCallback(ISocketCallback* callback) override { _callback = callback; }

    int connect(const std::string& ip, std::uint16_t port) override;
    int startRead() override;
    int stopRead() override;
    bool isReading() const override { return _reading; }
    int send(std::string_view data) override;
    void close() override;
    bool isClosing() const override { return _closing; }
    std::string peerAddress() const override;

    // Listener가 uv_accept 대상으로 쓰는 원시 핸들 (내부용)
    uv_stream_t* stream() { return reinterpret_cast<uv_stream_t*>(_handle.get()); }
    bool valid() const { return _handle != nullptr; }

private:
    // uv_write_t와 데이터는 완료 콜백까지 살아 있어야 하므로 요청 단위로 묶어 보관한다
    struct WriteRequest {
        uv_write_t request{};
        std::vector<char> buffer;
        TcpSocket* socket = nullptr;  // 소켓이 먼저 죽으면 nullptr로 끊는다
    };

    static void onAllocCb(uv_handle_t* handle, std::size_t suggested, uv_buf_t* buf);
    static void onReadCb(uv_stream_t* stream, std::ptrdiff_t nread, const uv_buf_t* buf);
    static void onWriteCb(uv_write_t* request, int status);
    static void onConnectCb(uv_connect_t* request, int status);
    static void onCloseCb(uv_handle_t* handle);

    std::unique_ptr<uv_tcp_t> _handle;
    std::unique_ptr<uv_connect_t> _connectRequest;
    std::vector<WriteRequest*> _pendingWrites;  // 비소유 — 완료 콜백이 자기 자신을 해제
    ISocketCallback* _callback = nullptr;
    bool _reading = false;
    bool _closing = false;
    bool _closed = false;
};

// 팩토리 — 구현 교체·mock 주입 지점 (design 6번)
std::unique_ptr<ISocket> createTcpSocket(uv_loop_t* loop);

}  // namespace common::net
