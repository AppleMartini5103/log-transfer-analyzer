#pragma once

#include "net/TcpSocket.h"

#include <uv.h>

#include <cstdint>
#include <memory>
#include <string>

// 리스닝 소켓 (서버 전용).
//
// [1:1 정책과 accept 미루기 — 2026-08-14 스파이크로 검증됨]
//   세션 진행 중에는 onConnection()에서 accept()를 호출하지 않는다. 그러면 연결은
//   커널 백로그에 pending으로 남고, 콜백이 다시 불리지도 않는다. CLEANUP 후
//   acceptPending()으로 늦게 받아도 성공하며, 대기 중 클라이언트가 보낸 데이터도 보존된다.
//   → 거절(accept 후 즉시 close)보다 나은 이유: 클라이언트에 재시도 로직이 불필요하다.
//   ※ 대기 클라이언트는 자기가 연결됐다고 믿으므로, 앞 세션이 길면 대기 측이
//     타임아웃될 수 있다 — 1:1 설계가 수용한 트레이드오프 (design 11번)

namespace common::net {

class IListenerCallback {
public:
    virtual ~IListenerCallback() = default;
    // 연결 대기 알림. accept할지는 호출부(세션 관리자)가 결정한다
    virtual void onConnection() {}
    virtual void onListenError(int /*status*/) {}
};

class Listener {
public:
    explicit Listener(uv_loop_t* loop);
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;

    // port=0이면 임시 포트를 커널이 배정한다 (테스트에서 포트 충돌 회피). 실패 시 libuv 에러 코드
    int listen(const std::string& ip, std::uint16_t port, int backlog,
               IListenerCallback* callback);

    // 실제 바인딩된 포트 — port=0으로 열었을 때 확인용
    std::uint16_t boundPort() const;

    // 대기 중인 연결 수락. 실패하거나 대기분이 없으면 nullptr
    std::unique_ptr<ISocket> acceptPending();

    void close();
    bool isClosing() const { return _closing; }

private:
    static void onConnectionCb(uv_stream_t* server, int status);
    static void onCloseCb(uv_handle_t* handle);

    std::unique_ptr<uv_tcp_t> _handle;
    uv_loop_t* _loop = nullptr;
    IListenerCallback* _callback = nullptr;
    bool _closing = false;
    bool _closed = false;
};

}  // namespace common::net
