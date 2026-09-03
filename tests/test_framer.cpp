#include "protocol/Framer.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace proto = common::protocol;
using proto::DecodeStatus;
using proto::Framer;

namespace {

proto::ByteView view(const std::vector<char>& bytes) {
    return proto::ByteView{bytes.data(), bytes.size()};
}

std::vector<char> sampleHeaderBytes() {
    proto::UploadHeader msg;
    msg.fileSize = 524'288'000;
    msg.filename = "BYDA_Test_Log_500MB.log";
    return proto::encode(msg);
}

}  // namespace

TEST_CASE("framer: whole message in one feed") {
    const auto bytes = sampleHeaderBytes();
    Framer framer{proto::MessageType::UploadHeader};

    const auto result = framer.feed(view(bytes));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.consumed == bytes.size());

    proto::UploadHeader out;
    REQUIRE(proto::decode(framer.message(), out) == DecodeStatus::Ok);
    REQUIRE(out.filename == "BYDA_Test_Log_500MB.log");
}

TEST_CASE("framer: byte-by-byte drip feed") {
    const auto bytes = sampleHeaderBytes();
    Framer framer{proto::MessageType::UploadHeader};

    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        const auto r = framer.feed(proto::ByteView{bytes.data() + i, 1});
        INFO("byte " << i);
        REQUIRE(r.status == DecodeStatus::NeedMoreData);
        REQUIRE(r.consumed == 1);
    }
    const auto last = framer.feed(proto::ByteView{bytes.data() + bytes.size() - 1, 1});
    REQUIRE(last.status == DecodeStatus::Ok);
    REQUIRE(last.consumed == 1);
}

TEST_CASE("framer: every split point completes with correct totals") {
    const auto bytes = sampleHeaderBytes();
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        Framer framer{proto::MessageType::UploadHeader};
        const auto first = framer.feed(proto::ByteView{bytes.data(), split});
        const auto second =
            framer.feed(proto::ByteView{bytes.data() + split, bytes.size() - split});
        INFO("split at " << split);
        REQUIRE(first.consumed == split);
        REQUIRE(second.status == DecodeStatus::Ok);
        REQUIRE(first.consumed + second.consumed == bytes.size());
    }
}

TEST_CASE("framer: header chunk with payload tail — consumes only the message") {
    // 클라이언트는 헤더 직후 페이로드를 바로 흘림 — 한 청크에 섞여 도착하는 게 보통
    const auto header = sampleHeaderBytes();
    const std::string_view payload = "[2026-06-19_22:00:01.123456][7710]...";
    std::vector<char> chunk = header;
    chunk.insert(chunk.end(), payload.begin(), payload.end());

    Framer framer{proto::MessageType::UploadHeader};
    const auto r = framer.feed(view(chunk));
    REQUIRE(r.status == DecodeStatus::Ok);
    REQUIRE(r.consumed == header.size());  // 페이로드는 한 바이트도 건드리지 않음

    // 완료 후 추가 feed는 소비 0 — reset 전까지 안전
    const auto again = framer.feed(payload);
    REQUIRE(again.status == DecodeStatus::Ok);
    REQUIRE(again.consumed == 0);
}

TEST_CASE("framer: reset reuses the framer across session states") {
    Framer framer{proto::MessageType::UploadHeader};
    REQUIRE(framer.feed(view(sampleHeaderBytes())).status == DecodeStatus::Ok);

    // UploadHeader → UploadTrailer 로 전이 (RECEIVING 끝)
    framer.reset(proto::MessageType::UploadTrailer);
    proto::UploadTrailer trailer;
    trailer.crc32 = 0xCBF43926;
    const auto trailerBytes = proto::encode(trailer);
    REQUIRE(framer.feed(view(trailerBytes)).status == DecodeStatus::Ok);

    proto::UploadTrailer out;
    REQUIRE(proto::decode(framer.message(), out) == DecodeStatus::Ok);
    REQUIRE(out.crc32 == 0xCBF43926);
}

TEST_CASE("framer: bad magic detected during accumulation") {
    auto bytes = sampleHeaderBytes();
    bytes[2] = 'X';
    Framer framer{proto::MessageType::UploadHeader};
    // 프리앰블이 3+5로 쪼개져 와도 8바이트가 모이는 순간 판정
    const auto first = framer.feed(proto::ByteView{bytes.data(), 3});
    REQUIRE(first.status == DecodeStatus::NeedMoreData);
    const auto second = framer.feed(proto::ByteView{bytes.data() + 3, 5});
    REQUIRE(second.status == DecodeStatus::BadMagic);
}

TEST_CASE("framer: unexpected known type fails fast at preamble") {
    // Ack(17B)를 기대하는데 UploadHeader가 옴 — 본문을 기다리지 않고 8바이트 시점에 거부
    const auto bytes = sampleHeaderBytes();
    Framer framer{proto::MessageType::Ack};
    const auto r = framer.feed(proto::ByteView{bytes.data(), proto::kPreambleSize});
    REQUIRE(r.status == DecodeStatus::BadType);
}

TEST_CASE("framer: oversized filenameLen rejected without receiving the body") {
    // 와이어에 filenameLen=300을 조작 — 273B 상한 초과분은 본문 수신 없이 즉시 거부
    auto bytes = sampleHeaderBytes();
    bytes[proto::kPreambleSize + 8] = 0x01;  // filenameLen BE u16 = 0x012C = 300
    bytes[proto::kPreambleSize + 9] = 0x2C;

    Framer framer{proto::MessageType::UploadHeader};
    const auto r = framer.feed(proto::ByteView{bytes.data(), proto::kUploadHeaderFixedSize});
    REQUIRE(r.status == DecodeStatus::BadValue);
    REQUIRE(r.consumed == proto::kUploadHeaderFixedSize);  // 고정부까지만 소비하고 멈춤
}

TEST_CASE("framer: empty feed is a no-op") {
    Framer framer{proto::MessageType::Ack};
    const auto r = framer.feed(proto::ByteView{});
    REQUIRE(r.status == DecodeStatus::NeedMoreData);
    REQUIRE(r.consumed == 0);
}

TEST_CASE("framer: fixed-size messages complete at exact boundary") {
    // DownloadDone은 프리앰블 8B가 곧 전체 — WAIT_DONE 상태의 수신 단위
    Framer framer{proto::MessageType::DownloadDone};
    const auto bytes = proto::encodeDownloadDone();
    const auto r = framer.feed(view(bytes));
    REQUIRE(r.status == DecodeStatus::Ok);
    REQUIRE(r.consumed == proto::kDownloadDoneSize);
    REQUIRE(proto::decodeDownloadDone(framer.message()) == DecodeStatus::Ok);
}

// ── WAIT_HEADER의 두 타입 허용 + 가변 길이 메시지의 목표 크기 (ResultRequest) ──

TEST_CASE("framer: a ResultRequest is framed when the header slot allows both types") {
    // 회귀: 목표 크기를 UploadHeader(18B)로 고정해 두면 ResultRequest의 길이 프리픽스(28B)에
    // 닿지 못해 한 바이트도 소비하지 못한 채 같은 판정을 반복한다 — 실제로 무한 루프였다
    proto::ResultRequest request;
    request.fileSize = 506286814;
    request.crc32 = 0x1234ABCD;
    request.startOffset = 4096;
    request.filename = "BYDA_Test_Log_500MB.log";
    const auto bytes = proto::encode(request);

    proto::Framer framer{proto::MessageType::UploadHeader, proto::MessageType::ResultRequest};
    const auto fed = framer.feed(proto::ByteView{bytes.data(), bytes.size()});
    REQUIRE(fed.status == proto::DecodeStatus::Ok);
    REQUIRE(fed.consumed == bytes.size());
    REQUIRE(framer.messageType() == proto::MessageType::ResultRequest);

    proto::ResultRequest decoded;
    REQUIRE(proto::decode(framer.message(), decoded) == proto::DecodeStatus::Ok);
    REQUIRE(decoded.filename == request.filename);
    REQUIRE(decoded.startOffset == request.startOffset);
}

TEST_CASE("framer: a ResultRequest split one byte at a time is reassembled") {
    proto::ResultRequest request;
    request.fileSize = 1;
    request.crc32 = 7;
    request.startOffset = 0;
    request.filename = "a.log";
    const auto bytes = proto::encode(request);

    proto::Framer framer{proto::MessageType::UploadHeader, proto::MessageType::ResultRequest};
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        const auto fed = framer.feed(proto::ByteView{bytes.data() + i, 1});
        INFO("byte " << i);
        REQUIRE(fed.status == proto::DecodeStatus::NeedMoreData);
        REQUIRE(fed.consumed == 1);
    }
    const auto last = framer.feed(proto::ByteView{bytes.data() + bytes.size() - 1, 1});
    REQUIRE(last.status == proto::DecodeStatus::Ok);
}

TEST_CASE("framer: a maximum-length ResultRequest filename still fits the buffer") {
    // 285B — UploadHeader의 273B보다 크다. 버퍼가 273B이던 동안에는 BadValue로 잘못 거부됐다
    proto::ResultRequest request;
    request.filename = std::string(proto::kMaxFilenameLen, 'n');
    const auto bytes = proto::encode(request);
    REQUIRE(bytes.size() == proto::kResultRequestMaxSize);

    proto::Framer framer{proto::MessageType::UploadHeader, proto::MessageType::ResultRequest};
    const auto fed = framer.feed(proto::ByteView{bytes.data(), bytes.size()});
    REQUIRE(fed.status == proto::DecodeStatus::Ok);
    REQUIRE(fed.consumed == bytes.size());
}

TEST_CASE("framer: allowing two types does not admit a third") {
    proto::ResultHeader header;  // 서버→클라 메시지 — 이 자리에 올 수 없다
    header.csvSize = 10;
    header.crc32 = 0;
    const auto bytes = proto::encode(header);

    proto::Framer framer{proto::MessageType::UploadHeader, proto::MessageType::ResultRequest};
    REQUIRE(framer.feed(proto::ByteView{bytes.data(), bytes.size()}).status ==
            proto::DecodeStatus::BadType);
}

TEST_CASE("framer: the reserved Heartbeat type is still refused") {
    // type=6은 번호만 예약된 자리다 (design 12번). 실수로 열리지 않았음을 고정한다
    std::array<char, proto::kPreambleSize> preamble{};
    preamble[0] = static_cast<char>(0x42);
    preamble[1] = static_cast<char>(0x59);
    preamble[2] = static_cast<char>(0x44);
    preamble[3] = static_cast<char>(0x41);
    preamble[proto::kOffsetVersion] = static_cast<char>(proto::kProtocolVersion);
    preamble[proto::kOffsetType] = static_cast<char>(6);

    proto::Framer framer{proto::MessageType::UploadHeader, proto::MessageType::ResultRequest};
    REQUIRE(framer.feed(proto::ByteView{preamble.data(), preamble.size()}).status ==
            proto::DecodeStatus::BadType);
}
