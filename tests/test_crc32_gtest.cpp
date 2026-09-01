// GoogleTest mirror of test_crc32.cpp — built only with -DENABLE_GTEST_COMPARISON=ON.
//
// The suite itself stays on Catch2; this file exists for the README "Framework
// comparison" section, which measures the same five cases in both frameworks.
// Keep the cases in lockstep with test_crc32.cpp: if a case is added there, add
// its mirror here, or the comparison stops being a comparison.
#include "util/Crc32.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using common::crc32;

// gtest naming is Suite.Name identifiers; Catch2 uses free-form strings.
TEST(Crc32, MandatoryDesignTestVector) {
    // EXPECT_EQ(expected, actual) - gtest asserts via dedicated macros, while
    // Catch2's REQUIRE(a == b) decomposes the expression itself.
    EXPECT_EQ(0xCBF43926u, crc32(0, "123456789"));
}

TEST(Crc32, KnownZlibCompatibleVectors) {
    EXPECT_EQ(0x00000000u, crc32(0, ""));
    EXPECT_EQ(0xE8B7BE43u, crc32(0, "a"));
    EXPECT_EQ(0x352441C2u, crc32(0, "abc"));
}

TEST(Crc32, IncrementalEqualsOneShotAtEverySplitPoint) {
    const std::string_view data = "123456789";
    const std::uint32_t expected = crc32(0, data);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        std::uint32_t crc = 0;
        crc = crc32(crc, data.substr(0, split));
        crc = crc32(crc, data.substr(split));
        SCOPED_TRACE("split at " + std::to_string(split));  // Catch2 INFO equivalent
        EXPECT_EQ(expected, crc);
    }
}

TEST(Crc32, BinaryDataWithNullAnd0xFFBytes) {
    const std::string data{"\x00\xFF\x00\x7F\x80", 5};
    const std::uint32_t oneShot = crc32(0, data);

    std::uint32_t crc = 0;
    crc = crc32(crc, std::string_view{data}.substr(0, 2));
    crc = crc32(crc, std::string_view{data}.substr(2));
    EXPECT_EQ(oneShot, crc);

    EXPECT_NE(oneShot, crc32(0, std::string_view{data}.substr(0, 1)));
}

TEST(Crc32, EmptyChunkIsANoOpInAStream) {
    std::uint32_t crc = crc32(0, "hello ");
    crc = crc32(crc, "");
    crc = crc32(crc, "world");
    EXPECT_EQ(crc32(0, "hello world"), crc);
}
