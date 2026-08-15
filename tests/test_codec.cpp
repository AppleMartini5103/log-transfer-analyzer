#include "protocol/Codec.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace proto = common::protocol;
using proto::DecodeStatus;

namespace {

proto::ByteView view(const std::vector<char>& bytes) {
    return proto::ByteView{bytes.data(), bytes.size()};
}

}  // namespace

// ── 라운드트립: 직렬화 → 역직렬화 = 원본 (컨벤션 7번 필수) ──────────────────

TEST_CASE("codec: UploadHeader roundtrip") {
    proto::UploadHeader in;
    in.fileSize = 524'288'000;  // 500MB
    in.filename = "BYDA_Test_Log_500MB.log";

    const auto bytes = proto::encode(in);
    REQUIRE(bytes.size() == proto::kUploadHeaderFixedSize + in.filename.size());

    proto::UploadHeader out;
    REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::Ok);
    REQUIRE(out.fileSize == in.fileSize);
    REQUIRE(out.filename == in.filename);
}

TEST_CASE("codec: UploadHeader roundtrip boundary values") {
    SECTION("empty file is a valid session (design 8)") {
        proto::UploadHeader in;
        in.fileSize = 0;
        in.filename = "empty.log";
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::Ok);
        REQUIRE(out.fileSize == 0);
    }
    SECTION("exactly 8GiB is allowed (only above is rejected)") {
        proto::UploadHeader in;
        in.fileSize = proto::kMaxFileSize;
        in.filename = "big.log";
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::Ok);
    }
    SECTION("UTF-8 Korean filename allowed (design 8)") {
        proto::UploadHeader in;
        in.fileSize = 1;
        in.filename = "\xEB\xA1\x9C\xEA\xB7\xB8.log";  // "로그.log"
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::Ok);
        REQUIRE(out.filename == in.filename);
    }
    SECTION("255-byte filename is the max") {
        proto::UploadHeader in;
        in.fileSize = 1;
        in.filename = std::string(proto::kMaxFilenameLen, 'x');
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::Ok);
        REQUIRE(out.filename.size() == proto::kMaxFilenameLen);
    }
}

TEST_CASE("codec: UploadTrailer / Ack / ResultHeader / DownloadDone roundtrip") {
    proto::UploadTrailer trailer;
    trailer.crc32 = 0xCBF43926;
    proto::UploadTrailer trailerOut;
    REQUIRE(proto::decode(view(proto::encode(trailer)), trailerOut) == DecodeStatus::Ok);
    REQUIRE(trailerOut.crc32 == trailer.crc32);

    proto::Ack ack;
    ack.status = proto::AckStatus::CrcMismatch;
    ack.receivedBytes = 0xFFFF'FFFF'FFFF'FFFFull;  // u64 최대값도 왕복 보존
    proto::Ack ackOut;
    REQUIRE(proto::decode(view(proto::encode(ack)), ackOut) == DecodeStatus::Ok);
    REQUIRE(ackOut.status == ack.status);
    REQUIRE(ackOut.receivedBytes == ack.receivedBytes);

    proto::ResultHeader result;
    result.csvSize = 4096;
    result.crc32 = 0x12345678;
    proto::ResultHeader resultOut;
    REQUIRE(proto::decode(view(proto::encode(result)), resultOut) == DecodeStatus::Ok);
    REQUIRE(resultOut.csvSize == result.csvSize);
    REQUIRE(resultOut.crc32 == result.crc32);

    REQUIRE(proto::decodeDownloadDone(view(proto::encodeDownloadDone())) == DecodeStatus::Ok);
}

TEST_CASE("codec: big-endian wire layout is explicit") {
    // BE 규칙이 실제 바이트 배치로 지켜지는지 — 라운드트립만으론 양쪽이 같이 틀릴 수 있음
    proto::ResultHeader in;
    in.csvSize = 0x0102'0304'0506'0708ull;
    in.crc32 = 0xAABBCCDD;
    const auto bytes = proto::encode(in);
    const auto at = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    REQUIRE(at(proto::kPreambleSize + 0) == 0x01);  // u64 최상위 바이트가 먼저
    REQUIRE(at(proto::kPreambleSize + 7) == 0x08);
    REQUIRE(at(proto::kPreambleSize + 8) == 0xAA);  // u32도 BE
    REQUIRE(at(proto::kPreambleSize + 11) == 0xDD);
}

// ── 잘린 바이트열: 모든 절단 지점에서 NeedMoreData (컨벤션 7번 필수) ────────

TEST_CASE("codec: every truncation point yields NeedMoreData") {
    proto::UploadHeader in;
    in.fileSize = 42;
    in.filename = "cut.log";
    const auto bytes = proto::encode(in);

    for (std::size_t len = 0; len < bytes.size(); ++len) {
        proto::UploadHeader out;
        INFO("truncated to " << len << " of " << bytes.size());
        REQUIRE(proto::decode(proto::ByteView{bytes.data(), len}, out) ==
                DecodeStatus::NeedMoreData);
    }
}

TEST_CASE("codec: expectedMessageSize accumulation contract") {
    proto::UploadHeader in;
    in.fileSize = 42;
    in.filename = "frame.log";
    const auto bytes = proto::encode(in);
    std::size_t size = 0;

    // 프리앰블 미만 / filenameLen 미만 → NeedMoreData
    REQUIRE(proto::expectedMessageSize(proto::ByteView{bytes.data(), 7}, size) ==
            DecodeStatus::NeedMoreData);
    REQUIRE(proto::expectedMessageSize(proto::ByteView{bytes.data(), 17}, size) ==
            DecodeStatus::NeedMoreData);
    // filenameLen까지 모이면 전체 크기 확정
    REQUIRE(proto::expectedMessageSize(proto::ByteView{bytes.data(), 18}, size) == DecodeStatus::Ok);
    REQUIRE(size == bytes.size());

    // 고정 크기 메시지는 프리앰블만으로 확정
    const auto ackBytes = proto::encode(proto::Ack{});
    REQUIRE(proto::expectedMessageSize(view(ackBytes), size) == DecodeStatus::Ok);
    REQUIRE(size == proto::kAckSize);
}

// ── 프리앰블 검증: 스트림 신뢰 불가 부류 (design 11번 ①) ────────────────────

TEST_CASE("codec: preamble rejection statuses") {
    auto bytes = proto::encode(proto::Ack{});

    SECTION("bad magic") {
        bytes[0] = 'X';
        proto::Ack out;
        REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::BadMagic);
    }
    SECTION("unknown version") {
        bytes[proto::kOffsetVersion] = 2;
        proto::Ack out;
        REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::BadVersion);
    }
    SECTION("unknown type (reserved Heartbeat=6 also rejected)") {
        bytes[proto::kOffsetType] = 6;
        std::size_t size = 0;
        REQUIRE(proto::expectedMessageSize(view(bytes), size) == DecodeStatus::BadType);
    }
    SECTION("known but unexpected type — fixed message order violation") {
        proto::UploadTrailer out;  // Ack 바이트를 트레일러로 해석 시도
        REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::BadType);
    }
    SECTION("nonzero reserved bytes are ignored (design 8: receiver ignores)") {
        bytes[proto::kOffsetReserved] = 0x7F;
        proto::Ack out;
        REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::Ok);
    }
}

// ── 값 검증: 파싱됐지만 값 무효 부류 (design 11번 ②) ────────────────────────

TEST_CASE("codec: BadValue rejections") {
    SECTION("fileSize just above 8GiB") {
        proto::UploadHeader in;
        in.fileSize = proto::kMaxFileSize + 1;
        in.filename = "huge.log";
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("empty filename (length must be 1~255)") {
        proto::UploadHeader in;
        in.fileSize = 1;
        in.filename = "";
        proto::UploadHeader out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("path separators and control chars in filename") {
        for (const std::string& bad :
             {std::string{"a/b.log"}, std::string{"a\\b.log"}, std::string{"a\tb.log"},
              std::string{"a\x7F.log"}, std::string{"a\x00m.log", 7}}) {
            proto::UploadHeader in;
            in.fileSize = 1;
            in.filename = bad;
            proto::UploadHeader out;
            INFO("filename: " << bad);
            REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
        }
    }
    SECTION("unknown Ack status code") {
        auto bytes = proto::encode(proto::Ack{});
        bytes[proto::kPreambleSize] = 99;
        proto::Ack out;
        REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::BadValue);
    }
}
