#include "net/Listener.h"
#include "net/TcpSocket.h"

#include "TestLoop.h"

#include <catch_amalgamated.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

using common::net::IListenerCallback;
using common::net::ISocket;
using common::net::ISocketCallback;
using common::net::Listener;
using common::net::TcpSocket;
using common::net::WritableBuffer;
using testsupport::runUntil;

namespace {

// 테스트용 수신 측: 고정 버퍼를 alloc 콜백에 물려 malloc 없이 받는다
// (실제 서버는 여기에 링버퍼 슬롯을 물린다)
class Recorder : public ISocketCallback {
public:
    WritableBuffer onAllocate(std::size_t suggested) override {
        _slot.resize(std::min<std::size_t>(suggested, 64 * 1024));
        return WritableBuffer{_slot.data(), _slot.size()};
    }
    void onRead(std::string_view data) override { received.append(data); }
    void onSendComplete(int status) override {
        ++sendCompletions;
        if (status != 0) {
            lastError = status;
        }
    }
    void onError(int status, std::string_view where) override {
        lastError = status;
        lastErrorWhere.assign(where);
        ++errors;
    }
    void onClosed() override { ++closes; }
    void onConnect(int status) override {
        connectStatus = status;
        connected = true;
    }

    std::string received;
    int sendCompletions = 0;
    int errors = 0;
    int closes = 0;
    int lastError = 0;
    std::string lastErrorWhere;
    bool connected = false;
    int connectStatus = -1;

private:
    std::vector<char> _slot;
};

// accept 여부를 테스트가 제어하는 리스너 콜백
class AcceptController : public IListenerCallback {
public:
    AcceptController(Listener& listener, ISocketCallback* serverCallback)
        : _listener(listener), _serverCallback(serverCallback) {}

    void onConnection() override {
        ++connectionEvents;
        if (!acceptEnabled) {
            return;  // 1:1 정책: 세션 중이면 받지 않고 백로그에 둔다
        }
        accepted = _listener.acceptPending();
        if (accepted) {
            accepted->setCallback(_serverCallback);
            accepted->startRead();
        }
    }
    void onListenError(int status) override { lastError = status; }

    bool acceptEnabled = true;
    int connectionEvents = 0;
    int lastError = 0;
    std::unique_ptr<ISocket> accepted;

private:
    Listener& _listener;
    ISocketCallback* _serverCallback;
};

// 루프에 남은 핸들을 모두 닫고 정리 (테스트 간 자원 누수 방지)
void drainLoop(uv_loop_t* loop) {
    uv_walk(
        loop,
        [](uv_handle_t* handle, void*) {
            if (!uv_is_closing(handle)) {
                uv_close(handle, nullptr);
            }
        },
        nullptr);
    uv_run(loop, UV_RUN_DEFAULT);
}

struct LoopFixture {
    LoopFixture() { REQUIRE(uv_loop_init(&loop) == 0); }
    ~LoopFixture() {
        drainLoop(&loop);
        uv_loop_close(&loop);
    }
    uv_loop_t loop{};
};

}  // namespace

TEST_CASE("net: connect, send, receive over a real loopback socket") {
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};

    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);
    const std::uint16_t port = listener.boundPort();
    REQUIRE(port != 0);  // 커널이 임시 포트를 배정 — 테스트 간 포트 충돌 없음

    Recorder clientSide;
    TcpSocket client{&fixture.loop};
    client.setCallback(&clientSide);
    REQUIRE(client.connect("127.0.0.1", port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return clientSide.connected; }));
    REQUIRE(clientSide.connectStatus == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.accepted != nullptr; }));

    REQUIRE(client.send("UploadHeader-bytes") == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return serverSide.received.size() >= 18; }));
    REQUIRE(serverSide.received == "UploadHeader-bytes");
    REQUIRE(clientSide.sendCompletions == 1);

    // 역방향 (서버 → 클라이언트: Ack·ResultHeader 경로)
    REQUIRE(controller.accepted->send("Ack-OK") == 0);
    REQUIRE(client.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return clientSide.received.size() >= 6; }));
    REQUIRE(clientSide.received == "Ack-OK");
}

TEST_CASE("net: peer address is reported for logging") {
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    Recorder clientSide;
    TcpSocket client{&fixture.loop};
    client.setCallback(&clientSide);
    REQUIRE(client.connect("127.0.0.1", listener.boundPort()) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.accepted != nullptr; }));

    const std::string peer = controller.accepted->peerAddress();
    INFO("peer: " << peer);
    REQUIRE(peer.rfind("127.0.0.1:", 0) == 0);
}

TEST_CASE("net: large payload arrives complete across many chunks") {
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    Recorder clientSide;
    TcpSocket client{&fixture.loop};
    client.setCallback(&clientSide);
    REQUIRE(client.connect("127.0.0.1", listener.boundPort()) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.accepted != nullptr; }));

    // 1MB — 스트림이 여러 조각으로 쪼개져 도착함을 확인 (프레이머·재조립의 전제)
    const std::string payload(1024 * 1024, 'X');
    REQUIRE(client.send(payload) == 0);
    // 1MB는 루프백에서 1초도 걸리지 않지만, 상한을 넉넉히 둔다 — 조건이 만족되면 즉시
    // 빠져나오므로 큰 상한은 비용이 없고, 부하가 걸린 머신에서의 헛된 실패만 막아준다.
    REQUIRE(runUntil(&fixture.loop, [&] { return serverSide.received.size() >= payload.size(); },
                     20000 /* ms */));
    REQUIRE(serverSide.received.size() == payload.size());
    REQUIRE(serverSide.received == payload);
}

TEST_CASE("net: stopRead applies backpressure, startRead resumes") {
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    Recorder clientSide;
    TcpSocket client{&fixture.loop};
    client.setCallback(&clientSide);
    REQUIRE(client.connect("127.0.0.1", listener.boundPort()) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.accepted != nullptr; }));

    REQUIRE(controller.accepted->isReading());
    REQUIRE(controller.accepted->stopRead() == 0);
    REQUIRE_FALSE(controller.accepted->isReading());

    REQUIRE(client.send("while-stopped") == 0);
    for (int i = 0; i < 200; ++i) {
        uv_run(&fixture.loop, UV_RUN_NOWAIT);
    }
    REQUIRE(serverSide.received.empty());  // 읽지 않는 동안 앱까지 올라오지 않는다

    REQUIRE(controller.accepted->startRead() == 0);  // 링이 비면 재개 (uv_read_start)
    REQUIRE(runUntil(&fixture.loop, [&] { return !serverSide.received.empty(); }));
    REQUIRE(serverSide.received == "while-stopped");  // 커널 버퍼에 보존되어 유실 없음
}

TEST_CASE("net: peer disconnect surfaces as EOF, not a crash") {
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    auto client = std::make_unique<TcpSocket>(&fixture.loop);
    Recorder clientSide;
    client->setCallback(&clientSide);
    REQUIRE(client->connect("127.0.0.1", listener.boundPort()) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.accepted != nullptr; }));

    // 클라이언트가 갑자기 끊는다 — "전송 중 강제 단절" 시나리오
    client->close();
    REQUIRE(runUntil(&fixture.loop, [&] { return serverSide.errors > 0; }));
    REQUIRE(serverSide.lastError == UV_EOF);
    REQUIRE(serverSide.lastErrorWhere == "read(eof)");
    REQUIRE(runUntil(&fixture.loop, [&] { return clientSide.closes == 1; }));
}

TEST_CASE("net: deferred accept keeps the connection pending (1:1 policy)") {
    // 2026-08-14 스파이크 결과를 회귀 테스트로 고정한다:
    // accept를 미뤄도 연결은 유지되고, 나중에 받아도 그때까지 보낸 데이터가 보존된다
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    controller.acceptEnabled = false;  // 세션 진행 중이라고 가정

    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    Recorder clientSide;
    TcpSocket client{&fixture.loop};
    client.setCallback(&clientSide);
    REQUIRE(client.connect("127.0.0.1", listener.boundPort()) == 0);

    // 클라이언트의 TCP 연결은 성공한다 (커널이 핸드셰이크를 완료하므로 거절이 아니라 대기)
    REQUIRE(runUntil(&fixture.loop, [&] { return clientSide.connected; }));
    REQUIRE(clientSide.connectStatus == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return controller.connectionEvents == 1; }));

    // 대기 중에 보낸 데이터도 커널 버퍼에 남는다
    REQUIRE(client.send("sent-while-pending") == 0);
    for (int i = 0; i < 200; ++i) {
        uv_run(&fixture.loop, UV_RUN_NOWAIT);
    }
    REQUIRE(controller.connectionEvents == 1);  // 콜백 폭주 없음 — 연결당 1회
    REQUIRE(serverSide.received.empty());

    // CLEANUP 후 늦게 수락 — 성공해야 하고, 밀린 데이터가 그대로 올라와야 한다
    auto late = listener.acceptPending();
    REQUIRE(late != nullptr);
    late->setCallback(&serverSide);
    REQUIRE(late->startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return !serverSide.received.empty(); }));
    REQUIRE(serverSide.received == "sent-while-pending");
}

TEST_CASE("net: socket destroyed before close callback does not crash") {
    // 소유자가 onClosed()를 기다리지 않고 파괴하는 경우 — 사용 후 해제가 나면 안 된다
    LoopFixture fixture;
    Recorder serverSide;
    Listener listener{&fixture.loop};
    AcceptController controller{listener, &serverSide};
    REQUIRE(listener.listen("127.0.0.1", 0, 128, &controller) == 0);

    {
        auto client = std::make_unique<TcpSocket>(&fixture.loop);
        Recorder clientSide;
        client->setCallback(&clientSide);
        REQUIRE(client->connect("127.0.0.1", listener.boundPort()) == 0);
        REQUIRE(runUntil(&fixture.loop, [&] { return clientSide.connected; }));
        REQUIRE(client->send("in-flight") == 0);
        client.reset();  // 전송 완료 콜백·close 콜백 전에 파괴
    }
    for (int i = 0; i < 200; ++i) {
        uv_run(&fixture.loop, UV_RUN_NOWAIT);
    }
    SUCCEED("no use-after-free when destroyed with callbacks in flight");
}

TEST_CASE("net: send on a closing socket is rejected, not silently dropped") {
    LoopFixture fixture;
    Recorder clientSide;
    TcpSocket socket{&fixture.loop};
    socket.setCallback(&clientSide);
    socket.close();
    REQUIRE(socket.isClosing());
    REQUIRE(socket.send("nope") == UV_EINVAL);
    REQUIRE(socket.startRead() == UV_EINVAL);
}
