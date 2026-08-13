#include "util/Version.h"

#include <catch_amalgamated.hpp>

TEST_CASE("skeleton: common library links and constants are visible") {
    REQUIRE(common::kProjectName == "log-transfer-analyzer");
    REQUIRE_FALSE(common::buildDate().empty());
}
