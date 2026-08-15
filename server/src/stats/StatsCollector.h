#pragma once

#include "parser/LogLineParser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>

// 통계 수집기 — 작업1(모듈별 시간대 카운트) + 작업2(spd 평균)의 누적 상태.
//
// 스트림 파싱하며 맵만 갱신한다 (원본 500MB는 저장하지 않음 — design 적재·분석).
// 실측 기준 5모듈 × 25시간 = 125엔트리, 수 KB 수준이라 메모리 부담 없음.
//
// 자원 고갈 방어 (4-1 5단계): 엔트리 상한 초과 시 신규 키 거부 + MAP_LIMIT 스킵.
// 2단계의 타임스탬프 ±48h 검증이 1차 방어(최대 5×96=480엔트리)이고, 이 상한은
// 그마저 뚫렸을 때를 위한 심층 방어다.
//
// 파서 스레드 전용 (총괄표: 통계 맵은 파서만 접근 — 뮤텍스 없음).

namespace server::stats {

using server::parser::ModuleId;
using server::parser::ParsedLine;

inline constexpr std::size_t kMaxStatsEntries = 10'000;

// 시간대 키 "YYYY-MM-DD HH"의 정렬 가능한 표현.
// YYYYMMDDHH 고정폭 패킹이라 정수 오름차순 = 시간 오름차순 (design: 22시가 이틀에
// 걸쳐 충돌하므로 날짜 포함이 강제됨)
using HourKey = std::uint64_t;

HourKey makeHourKey(int year, int month, int day, int hour);
std::string formatHourKey(HourKey key);  // → "2026-06-19 22"

class StatsCollector {
public:
    // 검증 통과 라인 1건 반영. false = 엔트리 상한 초과로 신규 키 거부(MAP_LIMIT)
    bool record(const ParsedLine& line);

    // ── 작업1 재료 ──
    std::uint64_t totalEntries() const;
    // CSV 순서(module 알파벳순 → hour 오름차순)로 콜백: cb(ModuleId, HourKey, count)
    template <typename Callback>
    void forEachBucket(Callback&& cb) const {
        // 모듈 열거 순서 ≠ 알파벳순이므로 출력 시점에 이름으로 정렬 (5개 — 비용 무시)
        std::array<ModuleId, server::parser::kModuleCount> order{};
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = static_cast<ModuleId>(i);
        }
        std::sort(order.begin(), order.end(), [](ModuleId a, ModuleId b) {
            return server::parser::moduleName(a) < server::parser::moduleName(b);
        });
        for (const ModuleId module : order) {
            for (const auto& [hour, count] : _buckets[static_cast<std::size_t>(module)]) {
                cb(module, hour, count);  // map이 이미 hour 오름차순
            }
        }
    }

    // ── 작업2 재료 ──
    std::uint64_t validSpdSamples() const { return _validSpdSamples; }
    std::uint64_t excludedSpdSamples() const { return _excludedSpdSamples; }
    // 유효 표본 없으면 0.0 (빈 파일도 정상 세션 — 0으로 나누지 않는다)
    double averageSpeed() const;

    // 세션 경계 — 다음 세션을 위해 전체 초기화 (총괄표 "세션 중단 시" 칸)
    void reset();

private:
    std::array<std::map<HourKey, std::uint64_t>, server::parser::kModuleCount> _buckets;
    std::uint64_t _validSpdSamples = 0;
    std::uint64_t _excludedSpdSamples = 0;  // 파싱은 됐으나 도메인 범위 밖 (평균에서 제외)
    double _spdSum = 0.0;
};

}  // namespace server::stats
