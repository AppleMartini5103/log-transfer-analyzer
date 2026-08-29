#include "net/TcpSocket.h"
#include "protocol/Codec.h"
#include "session/SessionManager.h"
#include "util/Crc32.h"

#include "TestLoop.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

namespace proto = common::protocol;

using common::net::TcpSocket;
using common::net::WritableBuffer;
using server::session::SessionManager;
using server::session::SessionTimeouts;
using testsupport::runUntil;

namespace {

// 프로토콜 대화를 주도하는 테스트 클라이언트 (실제 클라이언트의 역할을 대신한다)
class TestClient : public common::net::ISocketCallback {
public:
    explicit TestClient(uv_loop_t* loop) : socket(loop) { socket.setCallback(this); }

    WritableBuffer onAllocate(std::size_t suggested) override {
        _slot.resize(std::min<std::size_t>(suggested, 64 * 1024));
        return WritableBuffer{_slot.data(), _slot.size()};
    }
    void onRead(std::string_view data) override { received.append(data); }
    void onConnect(int status) override {
        connected = (status == 0);
        connectDone = true;
    }
    void onError(int status, std::string_view) override {
        lastError = status;
        ++errors;
    }
    void onClosed() override { ++closes; }

    TcpSocket socket;
    std::string received;
    bool connectDone = false;
    bool connected = false;
    int errors = 0;
    int closes = 0;
    int lastError = 0;

private:
    std::vector<char> _slot;
};

// 테스트용 소형 링 — 실제 기본값(64슬롯x64KB)보다 작게 잡아 backpressure를 쉽게 재현한다
constexpr std::size_t kTestChunkSize = 16 * 1024;
constexpr std::size_t kTestRingSlots = 8;

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

struct Fixture {
    Fixture() {
        REQUIRE(uv_loop_init(&loop) == 0);
        manager = std::make_unique<SessionManager>(&loop, kTestChunkSize, kTestRingSlots, 0,
                                                  timeouts);
        std::string error;
        REQUIRE(manager->startParser(error));
        REQUIRE(manager->listen("127.0.0.1", 0, 128) == 0);
        port = manager->boundPort();
        REQUIRE(port != 0);
    }
    ~Fixture() {
        manager.reset();
        drainLoop(&loop);
        uv_loop_close(&loop);
    }
    std::string send(std::vector<char> bytes) {
        return std::string{bytes.data(), bytes.size()};
    }

    uv_loop_t loop{};
    SessionTimeouts timeouts{};  // 테스트별로 생성 전에 조정 가능
    std::unique_ptr<SessionManager> manager;
    std::uint16_t port = 0;
};

std::vector<char> headerBytes(std::uint64_t fileSize, const std::string& name) {
    proto::UploadHeader header;
    header.fileSize = fileSize;
    header.filename = name;
    return proto::encode(header);
}

std::vector<char> trailerBytes(std::uint32_t crc) {
    proto::UploadTrailer trailer;
    trailer.crc32 = crc;
    return proto::encode(trailer);
}

std::string asString(const std::vector<char>& bytes) {
    return std::string{bytes.data(), bytes.size()};
}

}  // namespace

TEST_CASE("session: full protocol round trip ends in a clean session") {
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.connected);
    REQUIRE(client.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

    const std::string payload = "[2026-06-19_22:00:00.000000][7710][30482][1] BYDA::X: y\n";
    REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "test.log"))) == 0);
    REQUIRE(client.socket.send(payload) == 0);
    REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

    // Ack(OK) + ResultHeader + CSV가 순서대로 도착한다
    REQUIRE(runUntil(&fixture.loop, [&] {
        return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize;
    }));

    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::Ok);
    REQUIRE(ack.receivedBytes == payload.size());

    proto::ResultHeader result;
    const std::string_view afterAck = std::string_view{client.received}.substr(proto::kAckSize);
    REQUIRE(proto::decode(afterAck, result) == proto::DecodeStatus::Ok);
    REQUIRE(result.csvSize > 0);

    REQUIRE(runUntil(&fixture.loop, [&] {
        return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize + result.csvSize;
    }));
    const std::string csv =
        client.received.substr(proto::kAckSize + proto::kResultHeaderSize, result.csvSize);
    REQUIRE(csv.rfind("module,hour,count", 0) == 0);            // D-2 스키마
    REQUIRE(common::crc32(0, csv) == result.crc32);             // 헤더의 CRC와 일치

    // DownloadDone → 세션 정리 → 다음 연결 수락 가능
    REQUIRE(client.socket.send(asString(proto::encodeDownloadDone())) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }));
    REQUIRE_FALSE(fixture.manager->hasActiveSession());
}

TEST_CASE("session: empty file is a valid session") {
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    REQUIRE(client.socket.send(asString(headerBytes(0, "empty.log"))) == 0);
    REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, "")))) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.received.size() >= proto::kAckSize; }));

    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::Ok);
    REQUIRE(ack.receivedBytes == 0);
}

TEST_CASE("session: payload split across many chunks is reassembled") {
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    const std::string payload(256 * 1024, 'A');
    REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "big.log"))) == 0);
    for (std::size_t offset = 0; offset < payload.size(); offset += 4096) {
        REQUIRE(client.socket.send(std::string_view{payload}.substr(offset, 4096)) == 0);
    }
    REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

    REQUIRE(runUntil(&fixture.loop, [&] { return client.received.size() >= proto::kAckSize; }));
    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::Ok);
    REQUIRE(ack.receivedBytes == payload.size());  // 경계가 정확히 맞아야 CRC도 맞는다
}

TEST_CASE("session: header and payload arriving in one chunk are separated") {
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    // 헤더 + 페이로드 + 트레일러를 한 번에 보낸다 — 실제 클라이언트의 흔한 패턴
    const std::string payload = "merged-chunk-payload";
    std::string all = asString(headerBytes(payload.size(), "merged.log"));
    all += payload;
    all += asString(trailerBytes(common::crc32(0, payload)));
    REQUIRE(client.socket.send(all) == 0);

    REQUIRE(runUntil(&fixture.loop, [&] { return client.received.size() >= proto::kAckSize; }));
    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::Ok);
    REQUIRE(ack.receivedBytes == payload.size());
}

TEST_CASE("session: CRC mismatch is reported then the session closes") {
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    const std::string payload = "payload-bytes";
    REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "bad-crc.log"))) == 0);
    REQUIRE(client.socket.send(payload) == 0);
    REQUIRE(client.socket.send(asString(trailerBytes(0xDEADBEEF))) == 0);  // 틀린 CRC

    REQUIRE(runUntil(&fixture.loop, [&] { return client.received.size() >= proto::kAckSize; }));
    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    // "그냥 끊지 않고 사유를 알리고 닫는다" — 클라 로그에 원인이 남아야 한다 (design 8번)
    REQUIRE(ack.status == proto::AckStatus::CrcMismatch);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.errors > 0; }));
    REQUIRE(client.lastError == UV_EOF);
}

TEST_CASE("session: oversized fileSize gets Ack(PROTOCOL_ERROR)") {
    // 검증 실패 ②부류: 파싱은 됐으나 값이 무효 — 스트림은 멀쩡하므로 사유를 알린다
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    REQUIRE(client.socket.send(asString(headerBytes(proto::kMaxFileSize + 1, "huge.log"))) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.received.size() >= proto::kAckSize; }));
    proto::Ack ack;
    REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::ProtocolError);
}

TEST_CASE("session: bad magic closes without any response") {
    // 검증 실패 ①부류: 스트림 신뢰 불가 — 이 위로 보내는 응답도 의미가 없다
    Fixture fixture;
    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);

    auto bytes = headerBytes(10, "x.log");
    bytes[0] = 'X';  // 매직 훼손
    REQUIRE(client.socket.send(asString(bytes)) == 0);

    REQUIRE(runUntil(&fixture.loop, [&] { return client.errors > 0; }));
    REQUIRE(client.lastError == UV_EOF);
    REQUIRE(client.received.empty());  // 응답 없이 종료
}

TEST_CASE("session: WAIT_HEADER timeout releases a ghost connection") {
    // 파일만 고르고 헤더를 안 보내는 유령 연결이 1:1 서버를 영구 점유하면 안 된다
    Fixture fixture;
    fixture.manager.reset();
    SessionTimeouts fast;
    fast.responseMs = 60;  // 테스트용 짧은 ②류 타임아웃
    fast.idleMs = 60;
    fixture.manager =
        std::make_unique<SessionManager>(&fixture.loop, kTestChunkSize, kTestRingSlots, 0, fast);
    std::string parserError;
    REQUIRE(fixture.manager->startParser(parserError));
    REQUIRE(fixture.manager->listen("127.0.0.1", 0, 128) == 0);

    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.manager->boundPort()) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

    // 아무것도 보내지 않는다 — 타임아웃이 세션을 정리해야 한다
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }));
    REQUIRE_FALSE(fixture.manager->hasActiveSession());
    REQUIRE(client.received.empty());
}

TEST_CASE("session: abrupt disconnect mid-upload cleans up without crashing") {
    // 평가 기준: "전송 도중 강제 단절 시 크래시 없이 자원 반환"
    Fixture fixture;
    {
        TestClient client{&fixture.loop};
        REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
        REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
        REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

        REQUIRE(client.socket.send(asString(headerBytes(1'000'000, "cut.log"))) == 0);
        REQUIRE(client.socket.send(std::string(1024, 'Z')) == 0);  // 일부만 보내고
        REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));
        client.socket.close();  // 갑자기 끊는다
    }
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }));
    REQUIRE_FALSE(fixture.manager->hasActiveSession());  // 다음 연결을 받을 수 있는 상태로 복귀
}

TEST_CASE("session: second connection waits in backlog, then is served (1:1 policy)") {
    Fixture fixture;
    TestClient first{&fixture.loop};
    REQUIRE(first.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return first.connectDone; }));
    REQUIRE(first.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

    // 두 번째 클라이언트는 연결에 성공하지만 세션을 얻지 못한다 (백로그 대기)
    TestClient second{&fixture.loop};
    REQUIRE(second.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return second.connectDone; }));
    REQUIRE(second.connected);
    REQUIRE(second.socket.startRead() == 0);

    const std::string payload = "first-session-payload";
    REQUIRE(second.socket.send(asString(headerBytes(payload.size(), "queued.log"))) == 0);
    for (int i = 0; i < 300; ++i) {
        uv_run(&fixture.loop, UV_RUN_NOWAIT);
    }
    REQUIRE(second.received.empty());  // 아직 서비스되지 않음

    // 첫 세션 완주
    REQUIRE(first.socket.send(asString(headerBytes(payload.size(), "first.log"))) == 0);
    REQUIRE(first.socket.send(payload) == 0);
    REQUIRE(first.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return first.received.size() >= proto::kAckSize; }));
    REQUIRE(first.socket.send(asString(proto::encodeDownloadDone())) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }));

    // CLEANUP 후 대기 중이던 연결이 수락되고, 대기 중 보낸 헤더도 보존되어 처리된다
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));
    REQUIRE(second.socket.send(payload) == 0);
    REQUIRE(second.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return second.received.size() >= proto::kAckSize; }));
    proto::Ack ack;
    REQUIRE(proto::decode(second.received, ack) == proto::DecodeStatus::Ok);
    REQUIRE(ack.status == proto::AckStatus::Ok);
}

// ── 상태별 타임아웃 (design 9번) ─────────────────────────────────────────────
//
// design 9번은 네 상태에 타이머를 붙였는데, 그중 WAIT_HEADER만 검증되어 있었다.
// 나머지가 중요한 이유는 서버가 1:1이기 때문이다 — 유령 세션 하나가 정리되지 않으면
// 서버 전체가 다음 클라이언트를 영구히 받지 못한다. 즉 타이머의 실패는 "세션이 안 끝난다"가
// 아니라 "서버가 멈춘다"이고, 그래서 각 테스트는 종료뿐 아니라 accept 재개까지 확인한다.
//
// [어느 타이머가 발동했는지 어떻게 가리는가]
//   hasActiveSession()은 WAIT_HEADER와 RECEIVING에서 똑같이 참이라, 그냥 침묵하면
//   WAIT_HEADER 타임아웃을 다시 테스트하는 것과 구별되지 않는다. 그래서 두 타이머의 길이를
//   비대칭으로 주입한다: 검증 대상만 짧게, 나머지는 충분히 길게. 빠르게 닫혔다면 짧은 쪽
//   말고는 설명이 되지 않으므로 어느 타이머가 일했는지가 결정된다.
//   (커밋 [37]에서 "정지가 실제로 성립했는지"를 단정하지 않아 헤맸던 것과 같은 교훈이다)

namespace {

// 타임아웃만 바꿔 매니저를 다시 세운다 (Fixture 생성자가 기본값으로 만들기 때문).
// WAIT_HEADER 타임아웃 테스트가 쓰는 방식과 같다.
void restartWithTimeouts(Fixture& fixture, std::uint64_t idleMs, std::uint64_t responseMs) {
    fixture.manager.reset();
    SessionTimeouts tuned;
    tuned.idleMs = idleMs;
    tuned.responseMs = responseMs;
    fixture.manager =
        std::make_unique<SessionManager>(&fixture.loop, kTestChunkSize, kTestRingSlots, 0, tuned);
    std::string error;
    REQUIRE(fixture.manager->startParser(error));
    REQUIRE(fixture.manager->listen("127.0.0.1", 0, 128) == 0);
    fixture.port = fixture.manager->boundPort();
    REQUIRE(fixture.port != 0);
}

}  // namespace

TEST_CASE("session: RECEIVING idle timeout releases a stalled upload") {
    // 케이블이 뽑히거나 상대 프로세스가 멈추면 소켓 에러가 오지 않는다 — 조용히 멈출 뿐이다.
    // 강제 단절 테스트는 에러가 즉시 오는 경우를 다루므로 이 경로를 대신하지 못한다.
    Fixture fixture;
    // idle만 짧게. response가 길므로 빨리 닫혔다면 RECEIVING의 idle 타이머다.
    restartWithTimeouts(fixture, 60, 5000);

    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

    // 헤더는 보내되 페이로드는 일부만 보내고 침묵한다 — 서버는 RECEIVING에 남는다.
    const std::string payload = "[2026-06-19_22:00:00.000000][7710][30482][1] BYDA::X: y\n";
    REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "stalled.log"))) == 0);
    REQUIRE(client.socket.send(payload.substr(0, 10)) == 0);

    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }, 3000));
    REQUIRE_FALSE(fixture.manager->hasActiveSession());
    // 업로드가 완료된 것이 아니라 타이머로 끝났다는 증거 — 완주했다면 Ack가 왔을 것이다.
    REQUIRE(client.received.empty());

    // 1:1 서버의 실질 요구: 정리 후 다음 연결을 받을 수 있어야 한다.
    TestClient next{&fixture.loop};
    REQUIRE(next.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return next.connectDone; }));
    REQUIRE(next.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));
}

TEST_CASE("session: WAIT_DONE timeout releases a client that never acknowledges") {
    // 결과까지 받아간 뒤 DownloadDone을 보내지 않고 사라지는 클라이언트.
    // 서버는 CLEANUP까지 스스로 진행해야 다음 연결을 받을 수 있다 (design 11번).
    Fixture fixture;
    // WAIT_HEADER도 같은 responseMs 손잡이를 쓰므로 60ms처럼 짧게 잡으면 헤더를 보내기 전에
    // 세션이 WAIT_HEADER에서 죽을 수 있다. 500ms는 연결+헤더 송신(루프백에서 수 ms)보다
    // 충분히 길고 테스트 상한 3000ms보다 짧다.
    // 어느 상태에서 발동했는지는 타이머 길이가 아니라 관측으로 가른다: CSV를 끝까지 받았다는
    // 것이 SENDING_RESULT를 지나 WAIT_DONE에 도달했다는 증거다.
    restartWithTimeouts(fixture, 5000, 500);

    TestClient client{&fixture.loop};
    REQUIRE(client.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return client.connectDone; }));
    REQUIRE(client.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));

    const std::string payload = "[2026-06-19_22:00:00.000000][7710][30482][1] BYDA::X: y\n";
    REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "nodone.log"))) == 0);
    REQUIRE(client.socket.send(payload) == 0);
    REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

    // 결과를 끝까지 받는다 = WAIT_DONE에 도달했다는 관측 가능한 증거
    REQUIRE(runUntil(&fixture.loop, [&] {
        return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize;
    }));
    proto::ResultHeader result;
    const std::string_view afterAck = std::string_view{client.received}.substr(proto::kAckSize);
    REQUIRE(proto::decode(afterAck, result) == proto::DecodeStatus::Ok);
    REQUIRE(runUntil(&fixture.loop, [&] {
        return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize + result.csvSize;
    }));

    // DownloadDone을 보내지 않는다 — 타이머가 세션을 끝내야 한다.
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->completedSessions() == 1; }, 3000));
    REQUIRE_FALSE(fixture.manager->hasActiveSession());

    TestClient next{&fixture.loop};
    REQUIRE(next.socket.connect("127.0.0.1", fixture.port) == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return next.connectDone; }));
    REQUIRE(next.socket.startRead() == 0);
    REQUIRE(runUntil(&fixture.loop, [&] { return fixture.manager->hasActiveSession(); }));
}
