#include "net/TcpSocket.h"
#include "protocol/Codec.h"
#include "session/SessionManager.h"
#include "util/Crc32.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

namespace proto = common::protocol;

using common::net::TcpSocket;
using common::net::WritableBuffer;
using server::session::SessionManager;
using server::session::SessionTimeouts;

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

template <typename Predicate>
bool runUntil(uv_loop_t* loop, Predicate predicate, int maxIterations = 5000) {
    for (int i = 0; i < maxIterations; ++i) {
        if (predicate()) {
            return true;
        }
        uv_run(loop, UV_RUN_NOWAIT);
        // NOWAIT로만 돌리면 벽시계가 거의 진행되지 않아 타이머가 만료되지 못한다.
        // 주기적으로 잠깐 재워 실제 시간이 흐르게 한다 (타임아웃 테스트의 전제)
        if ((i % 4) == 3 && !predicate()) {
            uv_sleep(1);
        }
    }
    return predicate();
}

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
        manager = std::make_unique<SessionManager>(&loop, kTestChunkSize, kTestRingSlots, timeouts);
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
        std::make_unique<SessionManager>(&fixture.loop, kTestChunkSize, kTestRingSlots, fast);
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
