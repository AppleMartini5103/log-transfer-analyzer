#include "protocol/protocol.h"

#include <catch_amalgamated.hpp>

#include <type_traits>

namespace proto = common::protocol;

// design 8번에 문서화된 수치가 코드에서 어긋나지 않는지 고정하는 테스트.
// 이 테스트가 깨졌다면 코드가 아니라 "와이어 포맷 자체"를 바꾼 것 — design.txt 갱신이 선행돼야 한다.

TEST_CASE("protocol: magic spells BYDA") {
    REQUIRE(proto::kMagic[0] == 'B');
    REQUIRE(proto::kMagic[1] == 'Y');
    REQUIRE(proto::kMagic[2] == 'D');
    REQUIRE(proto::kMagic[3] == 'A');
}

TEST_CASE("protocol: preamble layout is 8 bytes fixed") {
    REQUIRE(proto::kOffsetMagic == 0);
    REQUIRE(proto::kOffsetVersion == 4);
    REQUIRE(proto::kOffsetType == 5);
    REQUIRE(proto::kOffsetReserved == 6);
    REQUIRE(proto::kPreambleSize == 8);
}

TEST_CASE("protocol: message sizes match design section 8") {
    REQUIRE(proto::kUploadHeaderMaxSize == 273);  // 프레이머 누적 버퍼 상한
    REQUIRE(proto::kUploadTrailerSize == 12);     // RECEIVING 상태의 트레일러 수신 크기
    REQUIRE(proto::kAckSize == 17);
    REQUIRE(proto::kResultHeaderSize == 20);
    REQUIRE(proto::kDownloadDoneSize == 8);
    REQUIRE(proto::kResultRequestFixedSize == 30);  // 8 + 8 + 4 + 8 + 2
    REQUIRE(proto::kResultRequestMaxSize == 285);   // 30 + filename 255
}

TEST_CASE("protocol: wire enum values are fixed") {
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::UploadHeader) == 1);
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::UploadTrailer) == 2);
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::Ack) == 3);
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::ResultHeader) == 4);
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::DownloadDone) == 5);
    // 6은 Heartbeat 예약 (design 12번 973행) — ResultRequest가 그 자리를 쓰지 않는다.
    // 하트비트 전환 조건(design 9번)이 발동할 때 버전을 올리지 않고 추가하기 위한 자리다.
    REQUIRE(static_cast<std::uint8_t>(proto::MessageType::ResultRequest) == 7);

    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::Ok) == 0);
    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::CrcMismatch) == 1);
    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::SizeMismatch) == 2);
    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::ProtocolError) == 3);
    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::ServerError) == 4);
    REQUIRE(static_cast<std::uint8_t>(proto::AckStatus::NoSuchResult) == 5);

    // 와이어에 1바이트로 실리는 타입이 실제로 u8인지 (암묵 확장 방지)
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<proto::MessageType>, std::uint8_t>);
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<proto::AckStatus>, std::uint8_t>);
}

TEST_CASE("protocol: confirmed limits and timeouts") {
    REQUIRE(proto::kMaxFilenameLen == 255);
    REQUIRE(proto::kMaxFileSize == 8ULL * 1024 * 1024 * 1024);
    REQUIRE(proto::kIdleTimeoutMs == 30'000);
    REQUIRE(proto::kResponseTimeoutMs == 120'000);
    REQUIRE(proto::kDefaultPort == 23507);
}
