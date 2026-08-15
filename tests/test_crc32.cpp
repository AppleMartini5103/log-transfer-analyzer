#include "util/Crc32.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>

using common::crc32;

TEST_CASE("crc32: mandatory design test vector") {
    // design 8번 / 컨벤션 7번 — 변형(reflected, 0xEDB88320, init/final 0xFFFFFFFF) 고정 검증
    REQUIRE(crc32(0, "123456789") == 0xCBF43926u);
}

TEST_CASE("crc32: known zlib-compatible vectors") {
    REQUIRE(crc32(0, "") == 0x00000000u);
    REQUIRE(crc32(0, "a") == 0xE8B7BE43u);
    REQUIRE(crc32(0, "abc") == 0x352441C2u);
}

TEST_CASE("crc32: incremental equals one-shot at every split point") {
    // 업로드 경로가 청크 단위 증분 계산이므로, 분할 위치와 무관하게 같은 값이어야 한다
    const std::string_view data = "123456789";
    const std::uint32_t expected = crc32(0, data);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        std::uint32_t crc = 0;
        crc = crc32(crc, data.substr(0, split));
        crc = crc32(crc, data.substr(split));
        INFO("split at " << split);
        REQUIRE(crc == expected);
    }
}

TEST_CASE("crc32: binary data with null and 0xFF bytes") {
    // 페이로드는 텍스트 보장이 없다 — 널바이트가 섞여도 길이 기반으로 전부 계산돼야 함
    const std::string data{"\x00\xFF\x00\x7F\x80", 5};
    const std::uint32_t oneShot = crc32(0, data);

    std::uint32_t crc = 0;
    crc = crc32(crc, std::string_view{data}.substr(0, 2));
    crc = crc32(crc, std::string_view{data}.substr(2));
    REQUIRE(crc == oneShot);

    // 널바이트에서 잘리면 다른 값이 나와야 정상 (strlen류 파싱이 아니라는 증거)
    REQUIRE(crc32(0, std::string_view{data}.substr(0, 1)) != oneShot);
}

TEST_CASE("crc32: empty chunk is a no-op in a stream") {
    std::uint32_t crc = crc32(0, "hello ");
    crc = crc32(crc, "");
    crc = crc32(crc, "world");
    REQUIRE(crc == crc32(0, "hello world"));
}
