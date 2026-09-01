#include "csv/CsvBuilder.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using server::csv::buildResultCsv;
using server::parser::ModuleId;
using server::parser::ParsedLine;
using server::parser::SkipReason;
using server::parser::SkipReporter;
using server::stats::StatsCollector;

namespace {

ParsedLine makeLine(ModuleId module, int day, int hour) {
    ParsedLine line;
    line.module = module;
    line.year = 2026;
    line.month = 6;
    line.day = day;
    line.hour = hour;
    return line;
}

ParsedLine makeSpdLine(double spd, bool inRange) {
    ParsedLine line = makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, 22);
    line.hasSpd = true;
    line.spdInRange = inRange;
    line.spd = inRange ? spd : 0.0;
    return line;
}

std::vector<std::string> splitLines(const std::string& csv) {
    std::vector<std::string> lines;
    std::istringstream in{csv};
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

// 작업2 블록의 값 조회 — 버킷 개수에 따라 행 번호가 밀리므로 이름으로 찾는다
std::string metricOf(const std::string& csv, const std::string& name) {
    for (const auto& line : splitLines(csv)) {
        if (line.rfind(name + ",", 0) == 0) {
            return line.substr(name.size() + 1);
        }
    }
    return "<missing>";
}

}  // namespace

TEST_CASE("csv: full schema layout matches design D-2") {
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 23)));
    REQUIRE(stats.record(makeSpdLine(137500.0, true)));
    reporter.record(SkipReason::BadFrame, 0, "garbage");

    // 계수 4 + 스킵 1 = 총 5라인의 세션
    const auto lines = splitLines(buildResultCsv(stats, reporter, 5));
    REQUIRE(lines.size() == 13);
    REQUIRE(lines[0] == "module,hour,count");
    REQUIRE(lines[1] == "AntennaProfileSpec,2026-06-19 22,2");
    REQUIRE(lines[2] == "BeamSteerCtrlUnitImpl,2026-06-19 22,1");
    REQUIRE(lines[3] == "RadarTrackNodeState,2026-06-19 23,1");
    REQUIRE(lines[4].empty());  // 블록 구분 빈 줄
    REQUIRE(lines[5] == "metric,value");
    REQUIRE(lines[6] == "total_lines,5");
    REQUIRE(lines[7] == "avg_speed,137500.000000");
    REQUIRE(lines[8] == "valid_spd_samples,1");
    REQUIRE(lines[9] == "excluded_spd_samples,0");
    REQUIRE(lines[10] == "missing_spd_samples,0");
    REQUIRE(lines[11] == "skipped_lines,1");
    REQUIRE(lines[12] == "skip_reason_BAD_FRAME,1");
}

TEST_CASE("csv: task1 block is module-alphabetical then hour-ascending") {
    StatsCollector stats;
    SkipReporter reporter;
    // 역순 투입
    REQUIRE(stats.record(makeLine(ModuleId::SectorSchedulerRTS, 20, 5)));
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 20, 3)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 19, 22)));

    const auto lines = splitLines(buildResultCsv(stats, reporter, 4));
    REQUIRE(lines[1] == "AntennaProfileSpec,2026-06-19 22,1");
    REQUIRE(lines[2] == "AntennaProfileSpec,2026-06-20 03,1");
    REQUIRE(lines[3] == "RadarTrackNodeState,2026-06-19 22,1");
    REQUIRE(lines[4] == "SectorSchedulerRTS,2026-06-20 05,1");
}

TEST_CASE("csv: avg_speed always carries six decimals") {
    SkipReporter reporter;
    struct Case {
        double spd;
        std::string expected;
    };
    for (const auto& c : {Case{137500.0, "137500.000000"}, Case{0.5, "0.500000"},
                          Case{1.0 / 3.0, "0.333333"}, Case{9999999.999999, "9999999.999999"}}) {
        StatsCollector stats;
        REQUIRE(stats.record(makeSpdLine(c.spd, true)));
        INFO("spd: " << c.spd);
        REQUIRE(metricOf(buildResultCsv(stats, reporter, 1), "avg_speed") == c.expected);
    }
}

TEST_CASE("csv: rounding carries into the integer part") {
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE(stats.record(makeSpdLine(1.9999999, true)));  // 소수 6자리 반올림 → 2.000000
    REQUIRE(metricOf(buildResultCsv(stats, reporter, 1), "avg_speed") == "2.000000");
}

TEST_CASE("csv: empty session still emits a valid two-block file") {
    // 빈 파일도 정상 세션 (design 8번) — 서버는 빈 통계 CSV를 반환한다
    StatsCollector stats;
    SkipReporter reporter;
    const auto lines = splitLines(buildResultCsv(stats, reporter, 0));
    REQUIRE(lines.size() == 9);
    REQUIRE(lines[0] == "module,hour,count");  // 헤더는 유지 — 파서가 읽을 수 있는 구조
    REQUIRE(lines[1].empty());
    REQUIRE(lines[2] == "metric,value");
    REQUIRE(lines[3] == "total_lines,0");
    REQUIRE(lines[4] == "avg_speed,0.000000");  // 0으로 나누지 않음
    REQUIRE(lines[5] == "valid_spd_samples,0");
    REQUIRE(lines[6] == "excluded_spd_samples,0");
    REQUIRE(lines[7] == "missing_spd_samples,0");
    REQUIRE(lines[8] == "skipped_lines,0");  // 스킵 0 → skip_reason_ 행 없음 (계약 결정 2)
}

TEST_CASE("csv: excluded samples are reported separately from the average") {
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE(stats.record(makeSpdLine(100.0, true)));
    REQUIRE(stats.record(makeSpdLine(0.0, false)));  // BeyondLimit류 — 평균 오염 금지
    const std::string csv = buildResultCsv(stats, reporter, 2);
    REQUIRE(metricOf(csv, "avg_speed") == "100.000000");
    REQUIRE(metricOf(csv, "valid_spd_samples") == "1");
    REQUIRE(metricOf(csv, "excluded_spd_samples") == "1");
}

TEST_CASE("csv: no field ever needs quoting") {
    // 모듈명·시간키·숫자 어디에도 쉼표/따옴표/개행이 없어야 인용 없이 안전하다.
    // 작업1 블록은 3필드(쉼표 2), 작업2 블록은 2필드(쉼표 1)
    StatsCollector stats;
    SkipReporter reporter;
    for (std::size_t m = 0; m < server::parser::kModuleCount; ++m) {
        REQUIRE(stats.record(makeLine(static_cast<ModuleId>(m), 19, 22)));
    }
    bool inSecondBlock = false;
    for (const auto& line : splitLines(buildResultCsv(stats, reporter, 5))) {
        if (line.empty()) {
            inSecondBlock = true;
            continue;
        }
        INFO("line: " << line);
        REQUIRE(line.find('"') == std::string::npos);
        REQUIRE(std::count(line.begin(), line.end(), ',') == (inSecondBlock ? 1 : 2));
    }
}

TEST_CASE("csv: file write is byte-identical to the in-memory buffer") {
    // 전송본(csvSize·CRC32)과 디스크본이 어긋나면 클라이언트 CRC 검증이 깨진다
    constexpr const char* kPath = "test_result.csv";
    std::remove(kPath);
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE(stats.record(makeSpdLine(137500.0, true)));
    const std::string csv = buildResultCsv(stats, reporter, 1);
    REQUIRE(server::csv::writeCsvFile(kPath, csv));

    std::ifstream in{kPath, std::ios::binary};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    REQUIRE(buffer.str() == csv);
    REQUIRE(buffer.str().size() == csv.size());
    in.close();
    std::remove(kPath);
}

TEST_CASE("csv: write failure returns false") {
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE_FALSE(server::csv::writeCsvFile("no_such_dir/result.csv",
                                            buildResultCsv(stats, reporter, 0)));
}

// ── 리뷰 3 계약: skip_reason_ 행 규약 + spd 표본 항등식 ──

TEST_CASE("csv: skip reasons emit only non-zero codes, alphabetical, summing to the total") {
    StatsCollector stats;
    SkipReporter reporter;
    // 알파벳 역순으로 투입 — 출력이 코드 알파벳순임을 순서로 증명
    reporter.record(SkipReason::UnknownModule, 0, "BeyondLimit");
    reporter.record(SkipReason::UnknownModule, 1, "CorruptPayload");
    reporter.record(SkipReason::BadNumber, 2, "nodeUID[NONE]");
    reporter.record(SkipReason::BadNumber, 3, "rfLane[X]");
    reporter.record(SkipReason::BadNumber, 4, "spd[fast]");
    reporter.record(SkipReason::BadFrame, 5, "garbage");

    const auto lines = splitLines(buildResultCsv(stats, reporter, 6));
    // 마지막 3행이 사유 행 — 코드 알파벳순 (BAD_FRAME < BAD_NUMBER < UNKNOWN_MODULE)
    REQUIRE(lines[lines.size() - 3] == "skip_reason_BAD_FRAME,1");
    REQUIRE(lines[lines.size() - 2] == "skip_reason_BAD_NUMBER,3");
    REQUIRE(lines[lines.size() - 1] == "skip_reason_UNKNOWN_MODULE,2");
    // 0인 사유는 행 자체가 없다 (계약 결정 2 — "행이 없다 = 0")
    const std::string csv = buildResultCsv(stats, reporter, 6);
    REQUIRE(csv.find("skip_reason_EMPTY") == std::string::npos);
    REQUIRE(csv.find("skip_reason_BAD_BRACKET") == std::string::npos);
    // 사유 합 == skipped_lines 총계
    REQUIRE(metricOf(csv, "skipped_lines") == "6");
}

TEST_CASE("csv: missing_spd_samples completes the BeamSteer identity") {
    // 항등식 (계약 결정 4): BeamSteer 계수 = valid + excluded + missing
    StatsCollector stats;
    SkipReporter reporter;
    REQUIRE(stats.record(makeSpdLine(100.0, true)));   // valid
    REQUIRE(stats.record(makeSpdLine(0.0, false)));    // excluded (범위 밖)
    // spd 필드 부재 라인 (리뷰 2 관용) — 계수되되 표본 없음
    REQUIRE(stats.record(makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, 22)));

    const std::string csv = buildResultCsv(stats, reporter, 3);
    REQUIRE(metricOf(csv, "valid_spd_samples") == "1");
    REQUIRE(metricOf(csv, "excluded_spd_samples") == "1");
    REQUIRE(metricOf(csv, "missing_spd_samples") == "1");
    // 좌변: 작업1 블록의 BeamSteer 버킷 계수
    bool found = false;
    for (const auto& line : splitLines(csv)) {
        if (line.rfind("BeamSteerCtrlUnitImpl,", 0) == 0) {
            REQUIRE(line == "BeamSteerCtrlUnitImpl,2026-06-19 22,3");  // 1+1+1
            found = true;
        }
    }
    REQUIRE(found);
    // missing은 스킵이 아니다 — 사유 행이 생기지 않는다
    REQUIRE(metricOf(csv, "skipped_lines") == "0");
    REQUIRE(csv.find("skip_reason_") == std::string::npos);
}
