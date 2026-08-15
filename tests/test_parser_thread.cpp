#include "net/TcpSocket.h"
#include "protocol/Codec.h"
#include "session/SessionManager.h"
#include "util/Crc32.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace proto = common::protocol;

using common::net::TcpSocket;
using common::net::WritableBuffer;
using server::session::SessionManager;

namespace {

// 실측 스키마의 정상 라인 (design 4번) — 파서 스레드가 통계로 집계해야 한다
const std::string kGoodLine =
    "[2026-06-19_22:00:00.123456][7710][30482][1885246073] BYDA::RadarTrackNodeState: "
    "node_state_synced: nodeUID[47], rfLane[3], lockState[1->0]\n";
const std::string kSpdLine =
    "[2026-06-19_22:00:01.000000][7710][30482][1885246073] BYDA::BeamSteerCtrlUnitImpl: "
    "unitAddr[4181], spd[137500.000000], advDelta[62750.000000]\n";
// 관측된 독약 — 스킵되어야 한다
const std::string kPoisonLine =
    "[2026-06-19_22:15:00.000000] !@#$RAW_FRAME_DECODE_FAILURE_GARBAGE_OCTETS%^&*()\n";

class Client : public common::net::ISocketCallback {
public:
    explicit Client(uv_loop_t* loop) : socket(loop) { socket.setCallback(this); }
    WritableBuffer onAllocate(std::size_t suggested) override {
        _slot.resize(std::min<std::size_t>(suggested, 64 * 1024));
        return WritableBuffer{_slot.data(), _slot.size()};
    }
    void onRead(std::string_view data) override { received.append(data); }
    void onConnect(int status) override {
        connected = (status == 0);
        connectDone = true;
    }
    void onError(int, std::string_view) override { ++errors; }

    TcpSocket socket;
    std::string received;
    bool connectDone = false;
    bool connected = false;
    int errors = 0;

private:
    std::vector<char> _slot;
};

template <typename Predicate>
bool runUntil(uv_loop_t* loop, Predicate predicate, int maxIterations = 8000) {
    for (int i = 0; i < maxIterations; ++i) {
        if (predicate()) {
            return true;
        }
        uv_run(loop, UV_RUN_NOWAIT);
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

std::string asString(const std::vector<char>& bytes) {
    return std::string{bytes.data(), bytes.size()};
}

std::vector<char> headerBytes(std::uint64_t size, const std::string& name) {
    proto::UploadHeader header;
    header.fileSize = size;
    header.filename = name;
    return proto::encode(header);
}

std::vector<char> trailerBytes(std::uint32_t crc) {
    proto::UploadTrailer trailer;
    trailer.crc32 = crc;
    return proto::encode(trailer);
}

// 세션 하나를 끝까지 돌리고 서버가 돌려준 CSV를 반환한다
struct UploadOutcome {
    proto::AckStatus ackStatus = proto::AckStatus::ServerError;
    std::string csv;
    bool csvCrcOk = false;
};

UploadOutcome runUpload(std::size_t chunkSize, std::size_t ringSlots, const std::string& payload,
                        std::size_t sendChunk) {
    uv_loop_t loop{};
    REQUIRE(uv_loop_init(&loop) == 0);
    UploadOutcome outcome;
    {
        SessionManager manager{&loop, chunkSize, ringSlots};
        std::string error;
        REQUIRE(manager.startParser(error));
        REQUIRE(manager.listen("127.0.0.1", 0, 128) == 0);

        Client client{&loop};
        REQUIRE(client.socket.connect("127.0.0.1", manager.boundPort()) == 0);
        REQUIRE(runUntil(&loop, [&] { return client.connectDone; }));
        REQUIRE(client.socket.startRead() == 0);

        REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "real.log"))) == 0);
        for (std::size_t offset = 0; offset < payload.size(); offset += sendChunk) {
            REQUIRE(client.socket.send(std::string_view{payload}.substr(offset, sendChunk)) == 0);
        }
        REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

        REQUIRE(runUntil(&loop, [&] {
            return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize;
        }));
        proto::Ack ack;
        REQUIRE(proto::decode(client.received, ack) == proto::DecodeStatus::Ok);
        outcome.ackStatus = ack.status;

        proto::ResultHeader header;
        REQUIRE(proto::decode(std::string_view{client.received}.substr(proto::kAckSize), header) ==
                proto::DecodeStatus::Ok);
        REQUIRE(runUntil(&loop, [&] {
            return client.received.size() >=
                   proto::kAckSize + proto::kResultHeaderSize + header.csvSize;
        }));
        outcome.csv = client.received.substr(proto::kAckSize + proto::kResultHeaderSize,
                                             header.csvSize);
        outcome.csvCrcOk = common::crc32(0, outcome.csv) == header.crc32;

        REQUIRE(client.socket.send(asString(proto::encodeDownloadDone())) == 0);
        REQUIRE(runUntil(&loop, [&] { return manager.completedSessions() == 1; }));
    }
    drainLoop(&loop);
    uv_loop_close(&loop);
    return outcome;
}

std::string metricOf(const std::string& csv, const std::string& name) {
    std::istringstream in{csv};
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(name + ",", 0) == 0) {
            return line.substr(name.size() + 1);
        }
    }
    return "<missing>";
}

std::size_t countBucketLines(const std::string& csv) {
    std::istringstream in{csv};
    std::string line;
    std::size_t count = 0;
    while (std::getline(in, line) && !line.empty()) {
        if (line != "module,hour,count") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("parser thread: uploaded payload becomes real statistics") {
    std::string payload;
    for (int i = 0; i < 50; ++i) {
        payload += kGoodLine;
        payload += kSpdLine;
    }
    payload += kPoisonLine;  // 스킵되어야 함

    const auto outcome = runUpload(16 * 1024, 8, payload, 4096);
    REQUIRE(outcome.ackStatus == proto::AckStatus::Ok);
    REQUIRE(outcome.csvCrcOk);

    // 세션 상태머신 이슈에서는 비어 있던 통계가 이제 실제 값으로 채워진다
    REQUIRE(countBucketLines(outcome.csv) == 2);  // 두 모듈 x 같은 시간대
    REQUIRE(outcome.csv.find("RadarTrackNodeState,2026-06-19 22,50") != std::string::npos);
    REQUIRE(outcome.csv.find("BeamSteerCtrlUnitImpl,2026-06-19 22,50") != std::string::npos);
    REQUIRE(metricOf(outcome.csv, "avg_speed") == "137500.000000");
    REQUIRE(metricOf(outcome.csv, "valid_spd_samples") == "50");
    REQUIRE(metricOf(outcome.csv, "skipped_lines") == "1");  // 독약 1줄
}

TEST_CASE("parser thread: lines split across chunk boundaries are reassembled") {
    // 라인이 링 슬롯 경계에 걸쳐 쪼개져도 재조립되어야 한다 (TCP는 스트림)
    std::string payload;
    for (int i = 0; i < 400; ++i) {
        payload += kGoodLine;
    }
    // 아주 작은 전송 단위로 보내 경계를 최대한 많이 만든다
    const auto outcome = runUpload(4096, 8, payload, 97);
    REQUIRE(outcome.ackStatus == proto::AckStatus::Ok);
    REQUIRE(outcome.csv.find("RadarTrackNodeState,2026-06-19 22,400") != std::string::npos);
    REQUIRE(metricOf(outcome.csv, "skipped_lines") == "0");  // 재조립 실패가 없어야 0
}

TEST_CASE("parser thread: last line without a trailing newline is not lost") {
    std::string payload;
    for (int i = 0; i < 10; ++i) {
        payload += kGoodLine;
    }
    payload += kGoodLine.substr(0, kGoodLine.size() - 1);  // 개행 없이 끝난다

    const auto outcome = runUpload(16 * 1024, 8, payload, 4096);
    REQUIRE(outcome.ackStatus == proto::AckStatus::Ok);
    // finish()가 잔여분을 배출해야 11줄이 된다 (안 하면 10줄)
    REQUIRE(outcome.csv.find("RadarTrackNodeState,2026-06-19 22,11") != std::string::npos);
}

TEST_CASE("parser thread: backpressure keeps data intact under a tiny ring") {
    // 링 4슬롯 x 4KB = 16KB짜리 아주 작은 링으로 1MB를 밀어넣는다.
    // uv_read_stop/재개가 여러 번 발생하지만 한 줄도 유실되면 안 된다
    std::string payload;
    const std::size_t targetLines = 6000;
    payload.reserve(targetLines * kGoodLine.size());
    for (std::size_t i = 0; i < targetLines; ++i) {
        payload += kGoodLine;
    }
    REQUIRE(payload.size() > 800 * 1024);

    const auto outcome = runUpload(4096, 4, payload, 64 * 1024);
    REQUIRE(outcome.ackStatus == proto::AckStatus::Ok);  // CRC 일치 = 바이트 유실 없음
    REQUIRE(outcome.csvCrcOk);
    REQUIRE(outcome.csv.find("RadarTrackNodeState,2026-06-19 22," +
                             std::to_string(targetLines)) != std::string::npos);
    REQUIRE(metricOf(outcome.csv, "skipped_lines") == "0");
}

TEST_CASE("parser thread: state is reset between sessions") {
    // 상주 파서가 이전 세션의 통계를 다음 세션으로 흘리면 안 된다
    uv_loop_t loop{};
    REQUIRE(uv_loop_init(&loop) == 0);
    {
        SessionManager manager{&loop, 16 * 1024, 8};
        std::string error;
        REQUIRE(manager.startParser(error));
        REQUIRE(manager.listen("127.0.0.1", 0, 128) == 0);
        const std::uint16_t port = manager.boundPort();

        for (int round = 0; round < 2; ++round) {
            Client client{&loop};
            REQUIRE(client.socket.connect("127.0.0.1", port) == 0);
            REQUIRE(runUntil(&loop, [&] { return client.connectDone; }));
            REQUIRE(client.socket.startRead() == 0);

            const std::string payload = kGoodLine + kGoodLine;  // 매 세션 2줄
            REQUIRE(client.socket.send(asString(headerBytes(payload.size(), "s.log"))) == 0);
            REQUIRE(client.socket.send(payload) == 0);
            REQUIRE(client.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

            REQUIRE(runUntil(&loop, [&] {
                return client.received.size() >= proto::kAckSize + proto::kResultHeaderSize;
            }));
            proto::ResultHeader header;
            REQUIRE(proto::decode(std::string_view{client.received}.substr(proto::kAckSize),
                                  header) == proto::DecodeStatus::Ok);
            REQUIRE(runUntil(&loop, [&] {
                return client.received.size() >=
                       proto::kAckSize + proto::kResultHeaderSize + header.csvSize;
            }));
            const std::string csv = client.received.substr(
                proto::kAckSize + proto::kResultHeaderSize, header.csvSize);

            INFO("round " << round);
            // 누적됐다면 2회차에 4가 된다 — 항상 2여야 정상
            REQUIRE(csv.find("RadarTrackNodeState,2026-06-19 22,2") != std::string::npos);

            REQUIRE(client.socket.send(asString(proto::encodeDownloadDone())) == 0);
            REQUIRE(runUntil(&loop, [&] {
                return manager.completedSessions() == static_cast<std::uint64_t>(round + 1);
            }));
        }
    }
    drainLoop(&loop);
    uv_loop_close(&loop);
}

TEST_CASE("parser thread: aborted session does not leak into the next one") {
    // 강제 단절로 중단된 세션의 부분 데이터가 다음 세션 통계에 섞이면 안 된다
    uv_loop_t loop{};
    REQUIRE(uv_loop_init(&loop) == 0);
    {
        SessionManager manager{&loop, 16 * 1024, 8};
        std::string error;
        REQUIRE(manager.startParser(error));
        REQUIRE(manager.listen("127.0.0.1", 0, 128) == 0);
        const std::uint16_t port = manager.boundPort();

        {  // 1) 헤더 + 일부 페이로드만 보내고 끊는다
            Client aborted{&loop};
            REQUIRE(aborted.socket.connect("127.0.0.1", port) == 0);
            REQUIRE(runUntil(&loop, [&] { return aborted.connectDone; }));
            std::string partial;
            for (int i = 0; i < 20; ++i) {
                partial += kGoodLine;
            }
            REQUIRE(aborted.socket.send(asString(headerBytes(1'000'000, "cut.log"))) == 0);
            REQUIRE(aborted.socket.send(partial) == 0);
            REQUIRE(runUntil(&loop, [&] { return manager.hasActiveSession(); }));
            aborted.socket.close();
            REQUIRE(runUntil(&loop, [&] { return manager.completedSessions() == 1; }));
        }

        {  // 2) 정상 세션 — 통계는 이 세션 것만 있어야 한다
            Client fresh{&loop};
            REQUIRE(fresh.socket.connect("127.0.0.1", port) == 0);
            REQUIRE(runUntil(&loop, [&] { return fresh.connectDone; }));
            REQUIRE(fresh.socket.startRead() == 0);
            const std::string payload = kGoodLine + kGoodLine + kGoodLine;
            REQUIRE(fresh.socket.send(asString(headerBytes(payload.size(), "fresh.log"))) == 0);
            REQUIRE(fresh.socket.send(payload) == 0);
            REQUIRE(fresh.socket.send(asString(trailerBytes(common::crc32(0, payload)))) == 0);

            REQUIRE(runUntil(&loop, [&] {
                return fresh.received.size() >= proto::kAckSize + proto::kResultHeaderSize;
            }));
            proto::ResultHeader header;
            REQUIRE(proto::decode(std::string_view{fresh.received}.substr(proto::kAckSize),
                                  header) == proto::DecodeStatus::Ok);
            REQUIRE(runUntil(&loop, [&] {
                return fresh.received.size() >=
                       proto::kAckSize + proto::kResultHeaderSize + header.csvSize;
            }));
            const std::string csv =
                fresh.received.substr(proto::kAckSize + proto::kResultHeaderSize, header.csvSize);
            REQUIRE(csv.find("RadarTrackNodeState,2026-06-19 22,3") != std::string::npos);
        }
    }
    drainLoop(&loop);
    uv_loop_close(&loop);
}
