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

// ── ResultRequest: 끊긴 결과 수신을 재업로드 없이 이어 받는 메시지 ──────────

TEST_CASE("codec: ResultRequest roundtrip") {
    proto::ResultRequest in;
    in.fileSize = 506'286'814;
    in.crc32 = 0x2889'0B24;
    in.startOffset = 4096;
    in.filename = "BYDA_Test_Log_500MB.log";

    const auto bytes = proto::encode(in);
    REQUIRE(bytes.size() == proto::kResultRequestFixedSize + in.filename.size());

    proto::ResultRequest out;
    REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::Ok);
    REQUIRE(out.fileSize == in.fileSize);
    REQUIRE(out.crc32 == in.crc32);
    REQUIRE(out.startOffset == in.startOffset);
    REQUIRE(out.filename == in.filename);
}

TEST_CASE("codec: ResultRequest roundtrip boundary values") {
    // 재개 오프셋 0 = 처음부터 다시. 상한 = 프로토콜이 허용하는 가장 큰 결과의 끝.
    // 두 끝이 통과해야 "중간만 되는" 구현을 배제할 수 있다.
    for (const std::uint64_t offset : {std::uint64_t{0}, proto::kMaxCsvSize}) {
        for (const std::uint64_t size : {std::uint64_t{0}, proto::kMaxFileSize}) {
            for (const std::string& name : {std::string{"a"}, std::string(255, 'a')}) {
                proto::ResultRequest in;
                in.fileSize = size;
                in.crc32 = 0xFFFF'FFFF;
                in.startOffset = offset;
                in.filename = name;
                proto::ResultRequest out;
                INFO("offset " << offset << " size " << size << " nameLen " << name.size());
                REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::Ok);
                REQUIRE(out.startOffset == offset);
                REQUIRE(out.fileSize == size);
                REQUIRE(out.filename == name);
            }
        }
    }
}

TEST_CASE("codec: ResultRequest wire layout is explicit") {
    // 필드 순서가 실제 바이트로 지켜지는지 — 라운드트립은 encode/decode가 같이 틀리면 통과한다
    proto::ResultRequest in;
    in.fileSize = 0x0102'0304'0506'0708ull;
    in.crc32 = 0xAABB'CCDD;
    in.startOffset = 0x1112'1314'1516'1718ull;
    in.filename = "x";
    const auto bytes = proto::encode(in);
    const auto at = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    REQUIRE(at(proto::kOffsetType) == 7);
    REQUIRE(at(proto::kPreambleSize + 0) == 0x01);   // fileSize u64 BE
    REQUIRE(at(proto::kPreambleSize + 7) == 0x08);
    REQUIRE(at(proto::kPreambleSize + 8) == 0xAA);   // crc32 u32 BE
    REQUIRE(at(proto::kPreambleSize + 11) == 0xDD);
    REQUIRE(at(proto::kPreambleSize + 12) == 0x11);  // startOffset u64 BE
    REQUIRE(at(proto::kPreambleSize + 19) == 0x18);
    REQUIRE(at(proto::kPreambleSize + 20) == 0x00);  // filenameLen u16 BE = 1
    REQUIRE(at(proto::kPreambleSize + 21) == 0x01);
    REQUIRE(bytes[proto::kResultRequestFixedSize] == 'x');
}

TEST_CASE("codec: ResultRequest truncation and framing") {
    proto::ResultRequest in;
    in.fileSize = 42;
    in.crc32 = 7;
    in.startOffset = 8;
    in.filename = "cut.log";
    const auto bytes = proto::encode(in);

    for (std::size_t len = 0; len < bytes.size(); ++len) {
        proto::ResultRequest out;
        INFO("truncated to " << len << " of " << bytes.size());
        REQUIRE(proto::decode(proto::ByteView{bytes.data(), len}, out) ==
                DecodeStatus::NeedMoreData);
    }

    // 프레이머 계약: filenameLen(오프셋 28~29)까지 모여야 전체 크기가 확정된다
    std::size_t size = 0;
    REQUIRE(proto::expectedMessageSize(
                proto::ByteView{bytes.data(), proto::kResultRequestFixedSize - 1}, size) ==
            DecodeStatus::NeedMoreData);
    REQUIRE(proto::expectedMessageSize(
                proto::ByteView{bytes.data(), proto::kResultRequestFixedSize}, size) ==
            DecodeStatus::Ok);
    REQUIRE(size == bytes.size());
}

TEST_CASE("codec: ResultRequest BadValue rejections") {
    const auto base = [] {
        proto::ResultRequest in;
        in.fileSize = 1;
        in.crc32 = 1;
        in.startOffset = 0;
        in.filename = "a.log";
        return in;
    };

    SECTION("fileSize just above 8GiB") {
        auto in = base();
        in.fileSize = proto::kMaxFileSize + 1;
        proto::ResultRequest out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("startOffset beyond any result the server can build") {
        // 세션이 csvSize와 대조하기 전에, 프로토콜 상한 초과는 코덱이 끊는다
        auto in = base();
        in.startOffset = proto::kMaxCsvSize + 1;
        proto::ResultRequest out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("empty filename") {
        auto in = base();
        in.filename = "";
        proto::ResultRequest out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("filename over 255 bytes") {
        auto in = base();
        in.filename = std::string(256, 'a');
        proto::ResultRequest out;
        REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
    }
    SECTION("path separators and control chars in filename") {
        for (const std::string& bad :
             {std::string{"a/b.log"}, std::string{"a\b.log"}, std::string{"a	b.log"},
              std::string{"a m.log", 7}}) {
            auto in = base();
            in.filename = bad;
            proto::ResultRequest out;
            INFO("filename: " << bad);
            REQUIRE(proto::decode(view(proto::encode(in)), out) == DecodeStatus::BadValue);
        }
    }
}

TEST_CASE("codec: adding ResultRequest did not open the reserved type 6") {
    // ResultRequest를 7로 둔 이유가 지켜지는지 고정한다 — 6은 Heartbeat 예약이고
    // 미구현이므로 여전히 거부되어야 한다 (design 12번 973행 / protocol.h 주석).
    proto::ResultRequest in;
    in.fileSize = 1;
    in.crc32 = 1;
    in.startOffset = 0;
    in.filename = "a.log";
    auto bytes = proto::encode(in);
    bytes[proto::kOffsetType] = 6;

    std::size_t size = 0;
    REQUIRE(proto::expectedMessageSize(view(bytes), size) == DecodeStatus::BadType);
    proto::ResultRequest out;
    REQUIRE(proto::decode(view(bytes), out) == DecodeStatus::BadType);
}
