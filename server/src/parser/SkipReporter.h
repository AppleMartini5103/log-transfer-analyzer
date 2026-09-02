#pragma once

#include "parser/LogLineParser.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 스킵 리포터 — 손상 라인의 사유별 집계 + 원문 표본 기록 (design 4-1 5단계).
//
// bounded 설계가 핵심: 독약이 수백만 줄이어도 스킵 로그가 디스크를 폭발시키지 않는다.
//   - 사유별 카운터는 uint64 (무제한 집계, 메모리 고정)
//   - 원문은 처음 100개만, 각 200바이트까지 → 최대 ~20KB
// 이것이 "핫 패스에서 로그 호출 금지, 카운터만 올리고 요약은 종료 시점에" 규칙의 구현
// (컨벤션 8번 — 라인당 Logger 호출은 350만 번의 I/O가 되므로 금지).
//
// 파일명은 ./skip_report.txt (design 5번 실행 인터페이스, .gitignore 예약과 일치).
// 파서 스레드 전용 — 스킵 카운터는 파서 소유 (총괄표).

namespace server::parser {

inline constexpr std::size_t kMaxSkipSamples = 100;      // 원문 보관 개수 상한
inline constexpr std::size_t kMaxSkipSampleBytes = 200;  // 표본당 원문 바이트 상한
inline constexpr std::size_t kSkipReasonCount = 11;

// 미지 모듈명 집계의 상한 — 모듈명은 입력에서 온 미신뢰 문자열이다 (design 4-1 [보강 2]).
// 서로 다른 이름 수백만 개가 들어오면 맵이 무한 증식하므로, 스킵 표본을 100개로 묶은 것과
// 같은 이유로 distinct 개수와 이름 길이 양쪽에 상한을 둔다.
inline constexpr std::size_t kMaxUnknownModules = 50;    // distinct 이름 상한
inline constexpr std::size_t kMaxModuleNameBytes = 64;   // 이름 하나의 원문 바이트 상한

class SkipReporter {
public:
    // 손상 라인 1건 기록. offset = 스트림 내 라인 시작 바이트 (재조립기 제공)
    void record(SkipReason reason, std::uint64_t offset, std::string_view text);

    std::uint64_t total() const { return _total; }
    std::uint64_t count(SkipReason reason) const {
        return _counts[static_cast<std::size_t>(reason)];
    }
    std::size_t sampleCount() const { return _samples.size(); }

    // ── 미지 모듈명 집계 (UNKNOWN_MODULE 사유 전용) ──
    // "규칙이 깨지면 무슨 일이 벌어지는가"를 산출물이 스스로 말하게 하는 부분:
    // 화이트리스트에 없는 이름이 무엇이었고 몇 건이었는지 보고서에 남긴다.
    std::size_t distinctUnknownModules() const { return _unknownModules.size(); }
    std::uint64_t unknownModuleCount(const std::string& name) const;
    // 상한 초과·추출 실패로 이름이 목록에 없는 라인 수 (절단 문구의 숫자)
    std::uint64_t unlistedUnknownModuleLines() const { return _unlistedUnknownLines; }

    // 리포트 파일 생성 (기본 ./skip_report.txt). 실패는 반환값 (컨벤션 3번)
    bool writeReport(const std::string& path) const;

    void reset();

private:
    struct Sample {
        SkipReason reason;
        std::uint64_t offset;
        std::string text;  // 원문 앞부분 — 제어문자는 기록 시 이스케이프
    };

    // UNKNOWN_MODULE 라인의 원문에서 모듈명을 뽑아 집계 (상한 도달 시 신규 이름만 거부)
    void recordUnknownModule(std::string_view text);

    std::array<std::uint64_t, kSkipReasonCount> _counts{};
    std::map<std::string, std::uint64_t> _unknownModules;  // 이름 → 건수 (상한 kMaxUnknownModules)
    std::uint64_t _unlistedUnknownLines = 0;
    std::vector<Sample> _samples;
    std::uint64_t _total = 0;
};

}  // namespace server::parser
