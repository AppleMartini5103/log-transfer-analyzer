#include "parser/LogLineParser.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>

using server::parser::LogLineParser;
using server::parser::ModuleId;
using server::parser::SkipReason;

namespace {

// design 4의 실측 스키마에서 가져온 정상 라인 5종 (모듈별 대표)
constexpr std::string_view kNodeState =
    "[2026-06-19_22:00:00.123456][7710][30482][1885246073] BYDA::RadarTrackNodeState: "
    "node_state_synced: nodeUID[47], rfLane[3], lockState[1->0]";
constexpr std::string_view kAntenna =
    "[2026-06-19_22:00:01.000000][7710][30482][1885246073] BYDA::AntennaProfileSpec: "
    "applyElement: sectorID[20641107], element[2][0->1][4]";
constexpr std::string_view kBeamSteer =
    "[2026-06-19_22:00:02.000000][7710][30482][1885246073] BYDA::BeamSteerCtrlUnitImpl: "
    "unitAddr[4181], spd[137500.000000], advDelta[62750.000000]";
constexpr std::string_view kDetection =
    "[2026-06-19_22:00:03.000000][7710][30482][1885246073] BYDA::DetectionTaskRunner: "
    "Sector Command: jobID[7710000000415], command[RUN], mode[AUTO]";
constexpr std::string_view kScheduler =
    "[2026-06-19_22:00:04.000000][7710][30482][1885246073] BYDA::SectorSchedulerRTS: "
    "scan started: RT_SWEEP jobID[7710000000415], pattern[SW3], gatedFlag[1]";

// 임의의 라인을 만들되 헤더는 정상으로 고정
std::string lineWith(std::string_view module, std::string_view message) {
    return std::string{"[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::"} +
           std::string{module} + ": " + std::string{message};
}

SkipReason reasonOf(LogLineParser& parser, std::string_view text) {
    const auto result = parser.parse(text);
    REQUIRE_FALSE(result.ok);
    return result.reason;
}

}  // namespace

// ── 정상 라인은 전부 통과해야 한다 (화이트리스트가 정상까지 막으면 실패) ──

TEST_CASE("parser: all five known modules parse") {
    LogLineParser parser;
    struct Case {
        std::string_view text;
        ModuleId module;
    };
    for (const auto& c : {Case{kNodeState, ModuleId::RadarTrackNodeState},
                          Case{kAntenna, ModuleId::AntennaProfileSpec},
                          Case{kBeamSteer, ModuleId::BeamSteerCtrlUnitImpl},
                          Case{kDetection, ModuleId::DetectionTaskRunner},
                          Case{kScheduler, ModuleId::SectorSchedulerRTS}}) {
        const auto result = parser.parse(c.text);
        INFO("line: " << c.text);
        REQUIRE(result.ok);
        REQUIRE(result.line.module == c.module);
        REQUIRE(result.line.year == 2026);
        REQUIRE(result.line.month == 6);
        REQUIRE(result.line.day == 19);
        REQUIRE(result.line.hour == 22);
    }
}

TEST_CASE("parser: spd extracted only from BeamSteerCtrlUnitImpl") {
    LogLineParser parser;
    const auto beam = parser.parse(kBeamSteer);
    REQUIRE(beam.ok);
    REQUIRE(beam.line.hasSpd);
    REQUIRE(beam.line.spdInRange);
    REQUIRE(beam.line.spd == Catch::Approx(137500.0));

    const auto other = parser.parse(kNodeState);
    REQUIRE(other.ok);
    REQUIRE_FALSE(other.line.hasSpd);
}

TEST_CASE("parser: event names with spaces and extra fields are accepted") {
    // "Sector Command:" 처럼 이벤트명에 공백이 있어도 통과 (design 방어적 파싱 원칙)
    LogLineParser parser;
    REQUIRE(parser.parse(kDetection).ok);
    // 통계와 무관한 워드 필드(command[RUN])는 구조만 맞으면 통과 — 과잉 거부 방지
    REQUIRE(parser.parse(lineWith("DetectionTaskRunner",
                                  "Sector Command: jobID[1], command[STOP-NOW], note[a b c]"))
                .ok);
}

// ── 관측된 독약 4종 (design 4) ──

TEST_CASE("poison: GARBAGE frame") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000] "
                             "!@#$RAW_FRAME_DECODE_FAILURE_GARBAGE_OCTETS%^&*()") ==
            SkipReason::BadFrame);
}

TEST_CASE("poison: BeyondLimit spd is parsed but excluded from the average") {
    // stod류로는 "성공"하는 8.9e20 — 라인은 유효하되 표본에서 제외 (작업2의 핵심 함정)
    LogLineParser parser;
    const auto result = parser.parse(lineWith(
        "BeamSteerCtrlUnitImpl",
        "unitAddr[4181], spd[888888888888888888888.88], advDelta[62750.000000]"));
    REQUIRE(result.ok);
    REQUIRE(result.line.hasSpd);
    REQUIRE_FALSE(result.line.spdInRange);
}

TEST_CASE("poison: HeadBraceLoss") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser,
                     "2026-06-19_22:00:00.000000][7710][30482][1885246073] "
                     "BYDA::RadarTrackNodeState: node_state_synced: nodeUID[47], rfLane[3], "
                     "lockState[1->0]") == SkipReason::BadFrame);
}

TEST_CASE("poison: CorruptPayload — letters in numeric fields") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID[NONE], rfLane[3], "
                                      "lockState[1->0]")) == SkipReason::BadNumber);
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID[47], rfLane[X], "
                                      "lockState[1->0]")) == SkipReason::BadNumber);
}

// ── 단계별 거부 케이스 (컨벤션 7번: 4-1 각 단계 픽스처) ──

TEST_CASE("stage 0: empty, whitespace-only, control characters") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, "") == SkipReason::Empty);
    REQUIRE(reasonOf(parser, "     ") == SkipReason::Empty);
    REQUIRE(reasonOf(parser, std::string{"[2026-06-19_22:00:00.000000]\0[7710]", 36}) ==
            SkipReason::CtrlChar);
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState", "node\tstate")) ==
            SkipReason::CtrlChar);
    // 재조립기가 넘긴 상한 초과 플래그
    const auto tooLong = parser.parse(kNodeState, /*tooLong=*/true);
    REQUIRE_FALSE(tooLong.ok);
    REQUIRE(tooLong.reason == SkipReason::LineTooLong);
}

TEST_CASE("stage 1: frame structure violations") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000][7710][30482] BYDA::"
                             "RadarTrackNodeState: nodeUID[47], rfLane[3], lockState[1->0]") ==
            SkipReason::BadFrame);  // 헤더 필드 부족
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000][7710][30482][1][9] BYDA::"
                             "RadarTrackNodeState: nodeUID[47]") == SkipReason::BadFrame);
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000][7710][30482][1885246073] XXXX::"
                             "RadarTrackNodeState: nodeUID[47]") ==
            SkipReason::BadFrame);  // BYDA:: 마커 없음
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000][7710][][1885246073] BYDA::"
                             "RadarTrackNodeState: nodeUID[47]") ==
            SkipReason::BadFrame);  // 빈 헤더 필드
    REQUIRE(reasonOf(parser, "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::"
                             "RadarTrackNodeState") == SkipReason::BadFrame);  // 메시지 없음
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState", "node_state_synced: rfLane[3], "
                                                             "lockState[1->0]")) ==
            SkipReason::BadFrame);  // 필수 필드(nodeUID) 부재
}

TEST_CASE("stage 2: timestamp format and calendar validity") {
    LogLineParser parser;
    const auto withTs = [](std::string_view ts) {
        return std::string{"["} + std::string{ts} +
               "][7710][30482][1885246073] BYDA::RadarTrackNodeState: node_state_synced: "
               "nodeUID[47], rfLane[3], lockState[1->0]";
    };
    REQUIRE(reasonOf(parser, withTs("2026-13-19_22:00:00.000000")) == SkipReason::BadTimestamp);
    REQUIRE(reasonOf(parser, withTs("2026-06-32_22:00:00.000000")) == SkipReason::BadTimestamp);
    REQUIRE(reasonOf(parser, withTs("2026-06-19_25:00:00.000000")) == SkipReason::BadTimestamp);
    REQUIRE(reasonOf(parser, withTs("2026-06-19_22:61:00.000000")) == SkipReason::BadTimestamp);
    REQUIRE(reasonOf(parser, withTs("2026-06-19_22:00:61.000000")) == SkipReason::BadTimestamp);
    REQUIRE(reasonOf(parser, withTs("2026-02-29_22:00:00.000000")) ==
            SkipReason::BadTimestamp);  // 평년 2/29
    REQUIRE(reasonOf(parser, withTs("2026-06-19 22:00:00.000000")) ==
            SkipReason::BadTimestamp);  // 구분자 '_' 아님
    REQUIRE(reasonOf(parser, withTs("2026-6-19_22:00:00.00000")) == SkipReason::BadTimestamp);

    // 윤년 2/29는 정상
    LogLineParser leapParser;
    REQUIRE(leapParser.parse(withTs("2024-02-29_22:00:00.000000")).ok);
}

TEST_CASE("stage 2: session timestamp range anchors on the first good line") {
    LogLineParser parser;
    REQUIRE(parser.parse(kNodeState).ok);  // 앵커 = 2026-06-19 22:00
    const auto withTs = [](std::string_view ts) {
        return std::string{"["} + std::string{ts} +
               "][7710][30482][1885246073] BYDA::RadarTrackNodeState: node_state_synced: "
               "nodeUID[47], rfLane[3], lockState[1->0]";
    };
    REQUIRE(parser.parse(withTs("2026-06-20_22:40:00.000000")).ok);  // 24.7시간 — 실제 로그 범위
    REQUIRE(reasonOf(parser, withTs("2027-01-01_00:00:00.000000")) == SkipReason::TsOutOfRange);
    REQUIRE(reasonOf(parser, withTs("2020-01-01_00:00:00.000000")) == SkipReason::TsOutOfRange);

    parser.reset();  // 세션 경계 — 앵커 해제
    REQUIRE(parser.parse(withTs("2027-01-01_00:00:00.000000")).ok);
}

TEST_CASE("stage 3: number parsing rejects partial consumption and overflow") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, lineWith("SectorSchedulerRTS",
                                      "scan started: jobID[123abc], gatedFlag[1]")) ==
            SkipReason::BadNumber);
    REQUIRE(reasonOf(parser, lineWith("SectorSchedulerRTS",
                                      "scan started: jobID[1.2.3], gatedFlag[1]")) ==
            SkipReason::BadNumber);
    REQUIRE(reasonOf(parser, lineWith("SectorSchedulerRTS",
                                      "scan started: jobID[ 12], gatedFlag[1]")) ==
            SkipReason::BadNumber);  // 앞 공백
    REQUIRE(reasonOf(parser, lineWith("SectorSchedulerRTS",
                                      "scan started: jobID[99999999999999999999], "
                                      "gatedFlag[1]")) == SkipReason::NumOutOfRange);
    // jobID 7.7조는 int64라 정상 (32비트 파싱 금지 근거)
    REQUIRE(parser.parse(lineWith("SectorSchedulerRTS",
                                  "scan started: jobID[7710000000415], gatedFlag[1]"))
                .ok);
    // double 필드: inf/nan 문자열은 from_chars가 파싱 성공하므로 isfinite로 거부
    REQUIRE(reasonOf(parser, lineWith("BeamSteerCtrlUnitImpl",
                                      "unitAddr[4181], spd[inf], advDelta[1.0]")) ==
            SkipReason::BadNumber);
    REQUIRE(reasonOf(parser, lineWith("BeamSteerCtrlUnitImpl",
                                      "unitAddr[4181], spd[nan], advDelta[1.0]")) ==
            SkipReason::BadNumber);
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID[47], rfLane[3], "
                                      "lockState[1-0]")) == SkipReason::BadNumber);  // 화살표 형식
}

TEST_CASE("stage 4: bracket pairing and module whitelist") {
    LogLineParser parser;
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID[47, rfLane[3]")) ==
            SkipReason::BadBracket);  // 미닫힘
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID[], rfLane[3]")) ==
            SkipReason::BadBracket);  // 빈 []
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeState",
                                      "node_state_synced: nodeUID47], rfLane[3]")) ==
            SkipReason::BadBracket);  // 홀로 ]
    REQUIRE(reasonOf(parser, lineWith("BeyondLimit", "whatever[1]")) ==
            SkipReason::UnknownModule);
    REQUIRE(reasonOf(parser, lineWith("RadarTrackNodeStateX", "nodeUID[1]")) ==
            SkipReason::UnknownModule);  // 접두 일치도 거부 — 정확 일치만
}

TEST_CASE("stage 4: spd domain range boundaries") {
    LogLineParser parser;
    const auto spdLine = [](std::string_view spd) {
        return lineWith("BeamSteerCtrlUnitImpl",
                        std::string{"unitAddr[4181], spd["} + std::string{spd} +
                            "], advDelta[62750.000000]");
    };
    const auto atZero = parser.parse(spdLine("0.000000"));
    REQUIRE(atZero.ok);
    REQUIRE(atZero.line.spdInRange);  // 하한 포함
    const auto atMax = parser.parse(spdLine("10000000.000000"));
    REQUIRE(atMax.ok);
    REQUIRE_FALSE(atMax.line.spdInRange);  // 상한 미포함
    const auto negative = parser.parse(spdLine("-1.0"));
    REQUIRE(negative.ok);
    REQUIRE_FALSE(negative.line.spdInRange);
}

TEST_CASE("parser: skip reason codes match the documented vocabulary") {
    // 스킵 로그·CSV가 쓰는 어휘 — design 4-1의 11종과 문자열까지 일치해야 함
    REQUIRE(server::parser::skipReasonCode(SkipReason::LineTooLong) == "LINE_TOO_LONG");
    REQUIRE(server::parser::skipReasonCode(SkipReason::Empty) == "EMPTY");
    REQUIRE(server::parser::skipReasonCode(SkipReason::CtrlChar) == "CTRL_CHAR");
    REQUIRE(server::parser::skipReasonCode(SkipReason::BadFrame) == "BAD_FRAME");
    REQUIRE(server::parser::skipReasonCode(SkipReason::BadBracket) == "BAD_BRACKET");
    REQUIRE(server::parser::skipReasonCode(SkipReason::BadTimestamp) == "BAD_TIMESTAMP");
    REQUIRE(server::parser::skipReasonCode(SkipReason::TsOutOfRange) == "TS_OUT_OF_RANGE");
    REQUIRE(server::parser::skipReasonCode(SkipReason::BadNumber) == "BAD_NUMBER");
    REQUIRE(server::parser::skipReasonCode(SkipReason::NumOutOfRange) == "NUM_OUT_OF_RANGE");
    REQUIRE(server::parser::skipReasonCode(SkipReason::UnknownModule) == "UNKNOWN_MODULE");
    REQUIRE(server::parser::skipReasonCode(SkipReason::MapLimit) == "MAP_LIMIT");
    REQUIRE(server::parser::moduleName(ModuleId::BeamSteerCtrlUnitImpl) ==
            "BeamSteerCtrlUnitImpl");
}
