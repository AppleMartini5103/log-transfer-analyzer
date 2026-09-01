#include "stats/StatsCollector.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

using server::parser::ModuleId;
using server::parser::ParsedLine;
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

ParsedLine makeSpdLine(double spd, bool inRange, int hour = 22) {
    ParsedLine line = makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, hour);
    line.hasSpd = true;
    line.spdInRange = inRange;
    line.spd = inRange ? spd : 0.0;
    return line;
}

struct Bucket {
    ModuleId module;
    std::string hour;
    std::uint64_t count;
};

std::vector<Bucket> dump(const StatsCollector& stats) {
    std::vector<Bucket> out;
    stats.forEachBucket([&out](ModuleId module, server::stats::HourKey key, std::uint64_t count) {
        out.push_back({module, server::stats::formatHourKey(key), count});
    });
    return out;
}

}  // namespace

TEST_CASE("stats: hour key formats and orders chronologically") {
    using server::stats::formatHourKey;
    using server::stats::makeHourKey;
    REQUIRE(formatHourKey(makeHourKey(2026, 6, 19, 22)) == "2026-06-19 22");
    REQUIRE(formatHourKey(makeHourKey(2026, 6, 20, 0)) == "2026-06-20 00");
    // 22시가 이틀에 걸쳐도 날짜가 포함되어 충돌하지 않는다 (design 근거)
    REQUIRE(makeHourKey(2026, 6, 19, 22) < makeHourKey(2026, 6, 20, 22));
    REQUIRE(makeHourKey(2026, 6, 19, 23) < makeHourKey(2026, 6, 20, 0));
    REQUIRE(makeHourKey(2026, 12, 31, 23) < makeHourKey(2027, 1, 1, 0));
}

TEST_CASE("stats: counts accumulate per module and hour") {
    StatsCollector stats;
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 23)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 19, 22)));

    const auto buckets = dump(stats);
    REQUIRE(buckets.size() == 3);
    REQUIRE(stats.totalEntries() == 3);
    // 알파벳순: AntennaProfileSpec이 RadarTrackNodeState보다 먼저
    REQUIRE(buckets[0].module == ModuleId::AntennaProfileSpec);
    REQUIRE(buckets[0].count == 1);
    REQUIRE(buckets[1].module == ModuleId::RadarTrackNodeState);
    REQUIRE(buckets[1].hour == "2026-06-19 22");
    REQUIRE(buckets[1].count == 2);
    REQUIRE(buckets[2].hour == "2026-06-19 23");
}

TEST_CASE("stats: CSV order is module alphabetical then hour ascending") {
    StatsCollector stats;
    // 일부러 역순으로 투입 — 출력 정렬이 입력 순서와 무관함을 확인
    REQUIRE(stats.record(makeLine(ModuleId::SectorSchedulerRTS, 20, 5)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 20, 3)));
    REQUIRE(stats.record(makeLine(ModuleId::AntennaProfileSpec, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::DetectionTaskRunner, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));
    REQUIRE(stats.record(makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, 22)));

    const auto buckets = dump(stats);
    REQUIRE(buckets.size() == 6);
    std::vector<std::string> names;
    for (const auto& bucket : buckets) {
        names.push_back(std::string{server::parser::moduleName(bucket.module)});
    }
    REQUIRE(names == std::vector<std::string>{"AntennaProfileSpec", "AntennaProfileSpec",
                                              "BeamSteerCtrlUnitImpl", "DetectionTaskRunner",
                                              "RadarTrackNodeState", "SectorSchedulerRTS"});
    // 같은 모듈 안에서는 시간 오름차순
    REQUIRE(buckets[0].hour == "2026-06-19 22");
    REQUIRE(buckets[1].hour == "2026-06-20 03");
}

TEST_CASE("stats: spd average uses in-range samples only") {
    StatsCollector stats;
    REQUIRE(stats.record(makeSpdLine(100.0, true)));
    REQUIRE(stats.record(makeSpdLine(200.0, true)));
    REQUIRE(stats.record(makeSpdLine(0.0, false)));  // BeyondLimit류 — 제외 표본

    REQUIRE(stats.validSpdSamples() == 2);
    REQUIRE(stats.excludedSpdSamples() == 1);
    REQUIRE(stats.averageSpeed() == Catch::Approx(150.0));
}

TEST_CASE("stats: excluded samples still count toward module totals") {
    // 범위 밖 spd라도 라인 자체는 유효 — 작업1 카운트에는 들어간다
    StatsCollector stats;
    REQUIRE(stats.record(makeSpdLine(0.0, false)));
    const auto buckets = dump(stats);
    REQUIRE(buckets.size() == 1);
    REQUIRE(buckets[0].count == 1);
    REQUIRE(stats.validSpdSamples() == 0);
}

TEST_CASE("stats: missing spd counts only for BeamSteer lines without the field") {
    // 항등식 재료 (리뷰 3 계약 결정 4): BeamSteer 계수 = valid + excluded + missing
    StatsCollector stats;
    REQUIRE(stats.record(makeSpdLine(137500.0, true)));                          // valid
    REQUIRE(stats.record(makeSpdLine(0.0, false)));                              // excluded
    REQUIRE(stats.record(makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, 22)));    // missing
    // 다른 모듈은 hasSpd=false여도 missing이 아니다 — spd는 BeamSteer의 필드
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));

    REQUIRE(stats.validSpdSamples() == 1);
    REQUIRE(stats.excludedSpdSamples() == 1);
    REQUIRE(stats.missingSpdSamples() == 1);
    // 좌변 = 3 (valid+excluded+missing 라인이 전부 같은 버킷에 계수됨)
    const auto buckets = dump(stats);
    for (const auto& bucket : buckets) {
        if (bucket.module == ModuleId::BeamSteerCtrlUnitImpl) {
            REQUIRE(bucket.count == 3);
        }
    }
}

TEST_CASE("stats: empty session yields zero average without dividing by zero") {
    StatsCollector stats;
    REQUIRE(stats.averageSpeed() == Catch::Approx(0.0));  // 빈 파일도 정상 세션
    REQUIRE(stats.totalEntries() == 0);
    REQUIRE(dump(stats).empty());
}

TEST_CASE("stats: uniform spd values average exactly (real-log shape)") {
    // 실물 로그는 spd가 137500.000000 고정 — 누적 오차 없이 정확히 나와야 함
    StatsCollector stats;
    bool allAccepted = true;
    for (int i = 0; i < 600'000; ++i) {  // 실물 표본 수(58만)와 같은 규모
        allAccepted = stats.record(makeSpdLine(137500.0, true)) && allAccepted;
    }
    REQUIRE(allAccepted);
    REQUIRE(stats.validSpdSamples() == 600'000);
    REQUIRE(stats.averageSpeed() == Catch::Approx(137500.0).epsilon(1e-12));
}

TEST_CASE("stats: entry limit rejects new keys but keeps counting known ones") {
    StatsCollector stats;
    constexpr int kLimit = static_cast<int>(server::stats::kMaxStatsEntries);
    // 상한까지 서로 다른 키를 채운다 (모듈 × 월 × 일 × 시 조합 = 충분히 여유)
    int filled = 0;
    bool allAccepted = true;
    for (int month = 1; month <= 12 && filled < kLimit; ++month) {
        for (int day = 1; day <= 28 && filled < kLimit; ++day) {
            for (int hour = 0; hour < 24 && filled < kLimit; ++hour) {
                for (std::size_t m = 0;
                     m < server::parser::kModuleCount && filled < kLimit; ++m) {
                    ParsedLine line = makeLine(static_cast<ModuleId>(m), day, hour);
                    line.month = month;
                    allAccepted = stats.record(line) && allAccepted;
                    ++filled;
                }
            }
        }
    }
    REQUIRE(allAccepted);
    REQUIRE(stats.totalEntries() == server::stats::kMaxStatsEntries);

    // 신규 키는 거부 (MAP_LIMIT)
    ParsedLine fresh = makeLine(ModuleId::RadarTrackNodeState, 28, 23);
    fresh.month = 12;  // 루프가 도달하지 못한 조합
    REQUIRE_FALSE(stats.record(fresh));

    // 기존 키는 계속 누적 — 상한이 정상 동작을 멈추게 하지 않는다
    ParsedLine known = makeLine(ModuleId::RadarTrackNodeState, 1, 0);
    known.month = 1;  // 루프가 가장 먼저 채운 조합
    REQUIRE(stats.record(known));
    REQUIRE(stats.totalEntries() == server::stats::kMaxStatsEntries);
}

TEST_CASE("stats: reset clears everything for the next session") {
    StatsCollector stats;
    REQUIRE(stats.record(makeSpdLine(137500.0, true)));
    REQUIRE(stats.record(makeLine(ModuleId::BeamSteerCtrlUnitImpl, 19, 22)));  // missing 표본
    REQUIRE(stats.record(makeLine(ModuleId::RadarTrackNodeState, 19, 22)));
    stats.reset();

    REQUIRE(stats.totalEntries() == 0);
    REQUIRE(stats.validSpdSamples() == 0);
    REQUIRE(stats.excludedSpdSamples() == 0);
    REQUIRE(stats.missingSpdSamples() == 0);
    REQUIRE(stats.averageSpeed() == Catch::Approx(0.0));
    REQUIRE(dump(stats).empty());
}
