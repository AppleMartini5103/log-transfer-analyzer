#include "service/ResultSummary.h"

#include <algorithm>
#include <charconv>

namespace client {
namespace {

constexpr std::string_view kMetricHeader = "metric,value";
constexpr std::string_view kTotalLines = "total_lines";
constexpr std::string_view kSkippedLines = "skipped_lines";
constexpr std::string_view kReasonPrefix = "skip_reason_";
constexpr std::size_t kMaxReasonsShown = 3;

// 서버가 CRLF로 쓰거나 줄 끝에 공백이 붙어도 이름 비교가 어긋나지 않게 다듬는다.
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// 토큰 전체가 소비됐을 때만 성공. from_chars를 쓰는 이유는 서버 파서와 같다 —
// 예외가 없고 로케일과 무관하며 "123abc" 같은 부분 소비를 검출한다 (컨벤션 9번).
bool parseCount(std::string_view token, std::uint64_t& out) {
    token = trim(token);
    if (token.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;  // 범위 초과·비숫자·부분 소비 — 값이 없는 것으로 취급한다
    }
    out = value;
    return true;
}

}  // namespace

ResultSummary summarizeResultCsv(std::string_view csv) {
    ResultSummary summary;
    bool inMetricBlock = false;
    bool haveSkipped = false;

    std::size_t pos = 0;
    while (pos <= csv.size()) {
        const std::size_t newline = csv.find('\n', pos);
        const std::size_t end = (newline == std::string_view::npos) ? csv.size() : newline;
        const std::string_view line = trim(csv.substr(pos, end - pos));
        pos = end + 1;

        if (!inMetricBlock) {
            // 버킷 블록의 행 수는 로그마다 다르므로 위치가 아니라 헤더로 찾는다.
            inMetricBlock = (line == kMetricHeader);
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }
        if (line.empty()) {
            break;  // 블록의 끝
        }

        const std::size_t comma = line.find(',');
        if (comma == std::string_view::npos) {
            continue;  // 계약에 없는 형태 — 무시하고 나머지를 계속 읽는다
        }
        const std::string_view name = trim(line.substr(0, comma));
        const std::string_view value = line.substr(comma + 1);

        // 이름으로만 찾는다 — 행 순서에 의존하지 않으므로 서버가 순서를 바꿔도 안전하다.
        if (name == kTotalLines) {
            summary.hasTotal = parseCount(value, summary.totalLines);
        } else if (name == kSkippedLines) {
            haveSkipped = parseCount(value, summary.skippedLines);
        } else if (name.size() > kReasonPrefix.size() &&
                   name.compare(0, kReasonPrefix.size(), kReasonPrefix) == 0) {
            std::uint64_t count = 0;
            if (parseCount(value, count)) {
                summary.reasons.emplace_back(std::string{name.substr(kReasonPrefix.size())},
                                             count);
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
    }

    // skipped_lines를 못 읽었으면 사유 합으로 대신한다. 계약이 "사유 합 == skipped_lines"를
    // 보장하므로 같은 값이고, 총계 행만 이름이 바뀐 경우에도 경고가 살아남는다.
    if (!haveSkipped) {
        std::uint64_t total = 0;
        for (const auto& reason : summary.reasons) {
            total += reason.second;
        }
        summary.skippedLines = total;
    }

    // 동수(참조 로그의 BAD_FRAME 13 / UNKNOWN_MODULE 13)에서도 출력이 흔들리지 않도록
    // 코드 오름차순을 타이브레이크로 둔다.
    std::sort(summary.reasons.begin(), summary.reasons.end(),
              [](const auto& lhs, const auto& rhs) {
                  if (lhs.second != rhs.second) {
                      return lhs.second > rhs.second;
                  }
                  return lhs.first < rhs.first;
              });
    return summary;
}

std::string formatSkipWarning(const ResultSummary& summary) {
    if (summary.skippedLines == 0) {
        return {};  // 버릴 것이 없으면 알릴 것도 없다
    }

    std::string text = std::to_string(summary.skippedLines);
    if (summary.hasTotal) {
        // 분모가 있어야 "26줄"이 사소한 것인지 포맷이 통째로 깨진 것인지 구분된다.
        // 백분율은 쓰지 않는다 — 26/3483528은 0.00%로 반올림되어 오히려 정보가 준다.
        text += " of " + std::to_string(summary.totalLines);
    }
    text += " lines were skipped";

    if (summary.reasons.empty()) {
        return text;
    }
    text += " - ";
    const std::size_t shown = std::min(kMaxReasonsShown, summary.reasons.size());
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += summary.reasons[i].first + " " + std::to_string(summary.reasons[i].second);
    }
    if (summary.reasons.size() > shown) {
        text += ", +" + std::to_string(summary.reasons.size() - shown) + " more";
    }
    return text;
}

}  // namespace client
