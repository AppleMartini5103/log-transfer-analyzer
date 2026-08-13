#pragma once

#include <string_view>

namespace common {

// 뼈대 검증용 최소 모듈 — 이후 이슈에서 protocol/net/util 실물이 들어온다.
constexpr std::string_view kProjectName = "log-transfer-analyzer";

std::string_view buildDate();

}  // namespace common
