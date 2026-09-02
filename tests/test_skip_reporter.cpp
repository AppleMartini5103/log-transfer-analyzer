#include "parser/SkipReporter.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using server::parser::SkipReason;
using server::parser::SkipReporter;

namespace {

constexpr const char* kReportPath = "test_skip_report.txt";

std::string readAll(const char* path) {
    std::ifstream in{path};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct ReportGuard {
    ReportGuard() { std::remove(kReportPath); }
    ~ReportGuard() { std::remove(kReportPath); }
};

}  // namespace

TEST_CASE("skip reporter: counts by reason and total") {
    SkipReporter reporter;
    reporter.record(SkipReason::BadFrame, 0, "garbage");
    reporter.record(SkipReason::BadFrame, 100, "garbage again");
    reporter.record(SkipReason::UnknownModule, 200, "BYDA::BeyondLimit: spd[9e20]");

    REQUIRE(reporter.total() == 3);
    REQUIRE(reporter.count(SkipReason::BadFrame) == 2);
    REQUIRE(reporter.count(SkipReason::UnknownModule) == 1);
    REQUIRE(reporter.count(SkipReason::CtrlChar) == 0);
}

TEST_CASE("skip reporter: raw samples are bounded to 100 entries") {
    SkipReporter reporter;
    for (std::uint64_t i = 0; i < 5000; ++i) {
        reporter.record(SkipReason::BadNumber, i * 10, "poison line");
    }
    REQUIRE(reporter.total() == 5000);                                    // 카운터는 전량
    REQUIRE(reporter.sampleCount() == server::parser::kMaxSkipSamples);   // 원문은 100개까지
}

TEST_CASE("skip reporter: each sample is truncated to 200 bytes") {
    const ReportGuard guard;
    SkipReporter reporter;
    reporter.record(SkipReason::LineTooLong, 42, std::string(1000, 'x'));
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    REQUIRE(report.find(std::string(server::parser::kMaxSkipSampleBytes, 'x')) !=
            std::string::npos);
    REQUIRE(report.find(std::string(server::parser::kMaxSkipSampleBytes + 1, 'x')) ==
            std::string::npos);
}

TEST_CASE("skip reporter: control characters are escaped, UTF-8 preserved") {
    const ReportGuard guard;
    SkipReporter reporter;
    reporter.record(SkipReason::CtrlChar, 7, std::string{"bad\0byte\ttab", 12});
    reporter.record(SkipReason::BadFrame, 8, "\xED\x95\x9C\xEA\xB8\x80");  // "한글"
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    REQUIRE(report.find("bad\\x00byte\\x09tab") != std::string::npos);
    REQUIRE(report.find('\0') == std::string::npos);  // 원시 널바이트가 파일에 들어가지 않음
    REQUIRE(report.find("\xED\x95\x9C\xEA\xB8\x80") != std::string::npos);  // 한글은 보존
}

TEST_CASE("skip reporter: report contains reason code, offset and raw prefix") {
    const ReportGuard guard;
    SkipReporter reporter;
    reporter.record(SkipReason::BadTimestamp, 123456, "[2026-13-19_22:00:00.000000][7710]");
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    // design 4-1 포맷: [사유코드] 바이트 오프셋 + 원문
    REQUIRE(report.find("[BAD_TIMESTAMP] offset=123456 | [2026-13-19_22:00:00.000000][7710]") !=
            std::string::npos);
    REQUIRE(report.find("Total skipped lines: 1") != std::string::npos);
}

TEST_CASE("skip reporter: report lists all 11 reason codes including zeros") {
    const ReportGuard guard;
    SkipReporter reporter;
    reporter.record(SkipReason::BadFrame, 0, "x");
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    for (const char* code :
         {"LINE_TOO_LONG", "EMPTY", "CTRL_CHAR", "BAD_FRAME", "BAD_BRACKET", "BAD_TIMESTAMP",
          "TS_OUT_OF_RANGE", "BAD_NUMBER", "NUM_OUT_OF_RANGE", "UNKNOWN_MODULE", "MAP_LIMIT"}) {
        INFO("reason code: " << code);
        REQUIRE(report.find(code) != std::string::npos);
    }
}

TEST_CASE("skip reporter: truncation is disclosed, never silent") {
    const ReportGuard guard;
    SkipReporter reporter;
    for (std::uint64_t i = 0; i < 150; ++i) {
        reporter.record(SkipReason::BadNumber, i, "poison");
    }
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    REQUIRE(report.find("50 more skipped lines not shown") != std::string::npos);
}

TEST_CASE("skip reporter: write failure returns false") {
    SkipReporter reporter;
    reporter.record(SkipReason::Empty, 0, "");
    REQUIRE_FALSE(reporter.writeReport("no_such_dir/skip_report.txt"));
}

TEST_CASE("skip reporter: empty report is still valid output") {
    const ReportGuard guard;
    SkipReporter reporter;
    REQUIRE(reporter.writeReport(kReportPath));  // 손상 0건인 정상 로그
    const std::string report = readAll(kReportPath);
    REQUIRE(report.find("Total skipped lines: 0") != std::string::npos);
}

TEST_CASE("skip reporter: reset clears counters and samples") {
    SkipReporter reporter;
    reporter.record(SkipReason::BadFrame, 0, "x");
    reporter.reset();
    REQUIRE(reporter.total() == 0);
    REQUIRE(reporter.count(SkipReason::BadFrame) == 0);
    REQUIRE(reporter.sampleCount() == 0);
}

// ── 미지 모듈명 집계 (리뷰어 2차 지적 2번 — design 4-1 [4단계 보강 2]) ──

namespace {

// UNKNOWN_MODULE 라인의 형태: 1단계 프레임 검사를 통과했으므로 헤더와 ": "가 온전하다
std::string unknownLine(const std::string& module, const std::string& payload = "field[1]") {
    return "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::" + module + ": " +
           payload;
}

}  // namespace

TEST_CASE("skip reporter: unknown module names are counted per name") {
    SkipReporter reporter;
    reporter.record(SkipReason::UnknownModule, 0, unknownLine("CorruptPayload"));
    reporter.record(SkipReason::UnknownModule, 1, unknownLine("CorruptPayload"));
    reporter.record(SkipReason::UnknownModule, 2, unknownLine("BeyondLimit"));
    // 다른 사유는 집계에 들어가지 않는다 — 모듈명이 있어도 UNKNOWN_MODULE이 아니면 무관
    reporter.record(SkipReason::BadNumber, 3, unknownLine("RadarTrackNodeState"));

    REQUIRE(reporter.distinctUnknownModules() == 2);
    REQUIRE(reporter.unknownModuleCount("CorruptPayload") == 2);
    REQUIRE(reporter.unknownModuleCount("BeyondLimit") == 1);
    REQUIRE(reporter.unknownModuleCount("RadarTrackNodeState") == 0);
    // 불변식 (절단 없음): 집계 합 == count(UNKNOWN_MODULE)
    REQUIRE(reporter.unknownModuleCount("CorruptPayload") +
                reporter.unknownModuleCount("BeyondLimit") ==
            reporter.count(SkipReason::UnknownModule));
    REQUIRE(reporter.unlistedUnknownModuleLines() == 0);
}

TEST_CASE("skip reporter: unknown module block orders by count then name") {
    ReportGuard guard;
    SkipReporter reporter;
    for (int i = 0; i < 3; ++i) {
        reporter.record(SkipReason::UnknownModule, 0, unknownLine("Zulu"));
    }
    // Alpha와 Bravo는 동수 — 이름 오름차순이어야 한다
    reporter.record(SkipReason::UnknownModule, 1, unknownLine("Bravo"));
    reporter.record(SkipReason::UnknownModule, 2, unknownLine("Alpha"));
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    const std::size_t block = report.find("[Unknown module names]");
    REQUIRE(block != std::string::npos);
    const std::size_t zulu = report.find("Zulu 3", block);
    const std::size_t alpha = report.find("Alpha 1", block);
    const std::size_t bravo = report.find("Bravo 1", block);
    REQUIRE(zulu != std::string::npos);
    REQUIRE(alpha != std::string::npos);
    REQUIRE(bravo != std::string::npos);
    REQUIRE(zulu < alpha);   // 건수 내림차순
    REQUIRE(alpha < bravo);  // 동수는 이름 오름차순
    // 블록은 사유 집계 뒤, 표본 목록 앞에 온다
    REQUIRE(report.find("[Counts by reason]") < block);
    REQUIRE(block < report.find("skipped lines: reason, byte offset"));
}

TEST_CASE("skip reporter: no unknown module block when none were skipped") {
    ReportGuard guard;
    SkipReporter reporter;
    reporter.record(SkipReason::BadFrame, 0, "garbage");
    REQUIRE(reporter.writeReport(kReportPath));
    REQUIRE(readAll(kReportPath).find("[Unknown module names]") == std::string::npos);
}

TEST_CASE("skip reporter: distinct unknown names are capped, disclosed, and keep counting") {
    ReportGuard guard;
    SkipReporter reporter;
    const std::size_t over = server::parser::kMaxUnknownModules + 10;
    for (std::size_t i = 0; i < over; ++i) {
        reporter.record(SkipReason::UnknownModule, i, unknownLine("Mod" + std::to_string(i)));
    }
    // 상한 도달 후에도 기존 이름은 계속 누적된다 (StatsCollector의 MAP_LIMIT과 같은 규칙)
    reporter.record(SkipReason::UnknownModule, 999, unknownLine("Mod0"));

    REQUIRE(reporter.distinctUnknownModules() == server::parser::kMaxUnknownModules);
    REQUIRE(reporter.unknownModuleCount("Mod0") == 2);
    REQUIRE(reporter.unlistedUnknownModuleLines() == 10);
    // 불변식 (절단 있음): 집계 합 + 미표시분 == count(UNKNOWN_MODULE)
    std::uint64_t listed = 0;
    for (std::size_t i = 0; i < over; ++i) {
        listed += reporter.unknownModuleCount("Mod" + std::to_string(i));
    }
    REQUIRE(listed + reporter.unlistedUnknownModuleLines() ==
            reporter.count(SkipReason::UnknownModule));

    REQUIRE(reporter.writeReport(kReportPath));
    const std::string report = readAll(kReportPath);
    // 절단 문구의 숫자가 미표시분과 일치해야 한다 — 산출물이 스스로 절단을 밝힌다
    REQUIRE(report.find("... 10 more skipped lines whose module name is not listed") !=
            std::string::npos);
}

TEST_CASE("skip reporter: unknown module names are escaped and length-capped") {
    ReportGuard guard;
    SkipReporter reporter;
    // 제어문자가 섞인 이름 — 보고서 형식을 깨뜨리면 안 된다
    reporter.record(SkipReason::UnknownModule, 0,
                    unknownLine(std::string{"Evil\nName"}));
    // 초장문 이름 — 원문 바이트 상한으로 절단
    const std::string longName(server::parser::kMaxModuleNameBytes + 40, 'L');
    reporter.record(SkipReason::UnknownModule, 1, unknownLine(longName));
    REQUIRE(reporter.writeReport(kReportPath));

    const std::string report = readAll(kReportPath);
    REQUIRE(report.find("Evil\\x0AName 1") != std::string::npos);  // 개행이 이스케이프됨
    REQUIRE(reporter.unknownModuleCount(longName) == 0);           // 원문 그대로는 없다
    REQUIRE(reporter.unknownModuleCount(longName.substr(0, server::parser::kMaxModuleNameBytes)) ==
            1);
}
