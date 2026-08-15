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
