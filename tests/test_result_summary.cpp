#include "service/ResultSummary.h"

#include <catch_amalgamated.hpp>

#include <string>

using client::formatSkipWarning;
using client::summarizeResultCsv;

namespace {

// 서버가 실제로 내보낸 result.csv(2026-09-02, 483MB 참조 로그, 커밋 95dc45a)에서 옮겨왔다.
// 버킷 행은 125개지만 블록 인식만 확인하면 되므로 앞의 3개만 남겼고, 값은 실물 그대로다.
// metric 블록은 한 글자도 바꾸지 않았다 — 계약 위반을 놓치지 않으려면 실물이어야 한다.
const std::string kRealCsv =
    "module,hour,count\n"
    "AntennaProfileSpec,2026-06-19 22,23502\n"
    "BeamSteerCtrlUnitImpl,2026-06-19 22,23531\n"
    "RadarTrackNodeState,2026-06-19 22,47315\n"
    "\n"
    "metric,value\n"
    "total_lines,3483528\n"
    "avg_speed,137500.000000\n"
    "valid_spd_samples,580661\n"
    "excluded_spd_samples,0\n"
    "missing_spd_samples,0\n"
    "skipped_lines,26\n"
    "skip_reason_BAD_FRAME,13\n"
    "skip_reason_UNKNOWN_MODULE,13\n";

std::string warningOf(const std::string& csv) {
    return formatSkipWarning(summarizeResultCsv(csv));
}

}  // namespace

TEST_CASE("result summary: the contracted csv yields the full warning") {
    const auto summary = summarizeResultCsv(kRealCsv);
    REQUIRE(summary.hasTotal);
    REQUIRE(summary.totalLines == 3483528);
    REQUIRE(summary.skippedLines == 26);
    REQUIRE(summary.reasons.size() == 2);
    // 두 사유가 동수(13)라 정렬이 흔들릴 수 있는 자리 — 코드 오름차순으로 고정된다
    REQUIRE(summary.reasons[0].first == "BAD_FRAME");
    REQUIRE(summary.reasons[1].first == "UNKNOWN_MODULE");

    REQUIRE(warningOf(kRealCsv) ==
            "26 of 3483528 lines were skipped - BAD_FRAME 13, UNKNOWN_MODULE 13");
}

TEST_CASE("result summary: a format change shows up as an unmissable ratio") {
    // 리뷰 3이 지적한 상황 — 포맷이 어긋나 거의 전량이 스킵되면 분모 덕에 즉시 보인다
    const std::string broken =
        "module,hour,count\n"
        "\n"
        "metric,value\n"
        "total_lines,3483528\n"
        "skipped_lines,3483502\n"
        "skip_reason_UNKNOWN_MODULE,3483502\n";
    REQUIRE(warningOf(broken) ==
            "3483502 of 3483528 lines were skipped - UNKNOWN_MODULE 3483502");
}

TEST_CASE("result summary: nothing skipped means no warning at all") {
    const std::string clean =
        "module,hour,count\n"
        "\n"
        "metric,value\n"
        "total_lines,400\n"
        "skipped_lines,0\n";
    REQUIRE(warningOf(clean).empty());
}

// ── 관대한 파서: 서버가 계약을 덜 지켜도 경고만 줄어들 뿐 실패하지 않는다 ──

TEST_CASE("result summary: a missing total_lines drops the denominator, not the warning") {
    // 클라이언트가 서버보다 먼저 배포되는 경우 — total_lines 행이 아직 없다
    const std::string old =
        "metric,value\n"
        "skipped_lines,26\n"
        "skip_reason_BAD_FRAME,13\n";
    const auto summary = summarizeResultCsv(old);
    REQUIRE_FALSE(summary.hasTotal);
    REQUIRE(warningOf(old) == "26 lines were skipped - BAD_FRAME 13");
}

TEST_CASE("result summary: without per-reason rows the count still reaches the user") {
    // 서버가 사유 행을 내보내기 전 버전 — 총계만으로도 리뷰 3의 요구는 충족된다
    const std::string old =
        "metric,value\n"
        "total_lines,3483528\n"
        "skipped_lines,26\n";
    REQUIRE(warningOf(old) == "26 of 3483528 lines were skipped");
}

TEST_CASE("result summary: an absent skipped_lines falls back to the reason total") {
    // 계약이 "사유 합 == skipped_lines"를 보장하므로 총계 행 이름이 바뀌어도 살아남는다
    const std::string renamed =
        "metric,value\n"
        "total_lines,100\n"
        "skip_reason_BAD_FRAME,7\n"
        "skip_reason_CTRL_CHAR,5\n";
    REQUIRE(warningOf(renamed) == "12 of 100 lines were skipped - BAD_FRAME 7, CTRL_CHAR 5");
}

TEST_CASE("result summary: row order is never assumed") {
    const std::string shuffled =
        "metric,value\n"
        "skip_reason_UNKNOWN_MODULE,13\n"
        "skipped_lines,26\n"
        "skip_reason_BAD_FRAME,13\n"
        "total_lines,3483528\n";
    REQUIRE(warningOf(shuffled) == warningOf(kRealCsv));
}

TEST_CASE("result summary: reason codes are not hardcoded and the list is capped") {
    // 서버가 새 사유를 추가해도 클라이언트 수정 없이 그대로 표시된다
    const std::string many =
        "metric,value\n"
        "total_lines,100\n"
        "skipped_lines,40\n"
        "skip_reason_BRAND_NEW_REASON,20\n"
        "skip_reason_BAD_FRAME,10\n"
        "skip_reason_CTRL_CHAR,6\n"
        "skip_reason_EMPTY,3\n"
        "skip_reason_MAP_LIMIT,1\n";
    REQUIRE(warningOf(many) ==
            "40 of 100 lines were skipped - BRAND_NEW_REASON 20, BAD_FRAME 10, CTRL_CHAR 6, "
            "+2 more");
}

TEST_CASE("result summary: malformed input is survived, never thrown on") {
    // 어느 것도 예외를 던지지 않고, 읽을 수 없으면 조용히 경고를 접는다.
    // 여기서 엄격해지면 "서버 CSV가 조금 바뀌면 클라이언트가 실패한다"가 되어
    // 리뷰 3이 지적한 결함을 클라이언트에 그대로 옮기게 된다.
    REQUIRE(warningOf("").empty());
    REQUIRE(warningOf("module,hour,count\n").empty());          // metric 블록 자체가 없다
    REQUIRE(warningOf("metric,value").empty());                 // 헤더만 있고 잘렸다
    REQUIRE(warningOf("metric,value\nskipped_lines,").empty());  // 값이 비었다
    REQUIRE(warningOf("metric,value\nskipped_lines,not_a_number\n").empty());
    REQUIRE(warningOf("metric,value\nskipped_lines,26abc\n").empty());  // 부분 소비 거부
    REQUIRE(warningOf("metric,value\nskipped_lines,99999999999999999999999\n").empty());
    std::string binary(3, '\0');  // 널바이트가 섞여도 길이 기반 파싱이라 죽지 않는다
    binary += " garbage ";
    binary.push_back('\0');
    REQUIRE(warningOf(binary).empty());

    // 콤마 없는 줄이 섞여도 나머지 행은 계속 읽는다
    REQUIRE(warningOf("metric,value\ngarbage line\nskipped_lines,26\n") ==
            "26 lines were skipped");
}

TEST_CASE("result summary: CRLF and stray spaces do not break name matching") {
    const std::string crlf =
        "module,hour,count\r\n"
        "\r\n"
        "metric,value\r\n"
        "total_lines,3483528\r\n"
        " skipped_lines ,26\r\n"
        "skip_reason_BAD_FRAME, 13 \r\n";
    REQUIRE(warningOf(crlf) == "26 of 3483528 lines were skipped - BAD_FRAME 13");
}
