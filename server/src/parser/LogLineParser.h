#pragma once

#include <cstdint>
#include <string_view>

// 라인 검증 파이프라인 (design 4-1) — 블랙리스트가 아니라 화이트리스트 (default-deny).
// "알려진 정상 구조에 정확히 일치할 때만 통과, 나머지는 전부 사유 코드와 함께 스킵".
//
// 단계 (순서대로, 하나라도 실패 → 즉시 사유 반환):
//   0 바이트/라인  : 길이 상한(재조립기가 판정) · 빈 라인 · 제어문자
//   1 프레임 구조  : [ts][pid][tid][sess] BYDA::모듈: 메시지 — 위치 기반 (regex 금지, 컨벤션 2번)
//   2 타임스탬프   : 포맷 + 달력 유효성 + 세션 범위(첫 정상 라인 ±48h — 버킷 맵 증식 공격 방지)
//   3 숫자         : from_chars만 · 토큰 전체 소비 · int64 범위 · double isfinite
//   4 도메인       : 모듈 화이트리스트 5종 · spd 유효 범위 · 메시지 괄호 짝
//   (5 자원 고갈    : MAP_LIMIT — 통계 수집기 몫. 사유 코드만 여기서 정의)
//
// 파서 스레드 전용. 상태는 세션 타임스탬프 앵커 하나 — 세션 경계에서 reset().

namespace server::parser {

// 스킵 사유 11종 (design 4-1 확정) — 스킵 로그·CSV의 어휘와 일치
enum class SkipReason : std::uint8_t {
    LineTooLong = 0,  // 0단계: 64KB 상한 초과 (재조립기 판정)
    Empty,            // 0단계: 빈 라인 / 공백만
    CtrlChar,         // 0단계: 널바이트·제어문자 포함
    BadFrame,         // 1단계: 헤더 구조 불일치 / 필수 필드 부재
    BadBracket,       // 4단계: 메시지 괄호 짝 위반 (빈 [], 미닫힘, 홀로 ])
    BadTimestamp,     // 2단계: 포맷/달력 위반 (13월, 32일, 25시, 61분 …)
    TsOutOfRange,     // 2단계: 첫 정상 라인 기준 ±48시간 밖
    BadNumber,        // 3단계: 숫자 자리 비숫자 / 부분 소비 / 비유한 double
    NumOutOfRange,    // 3단계: int64 범위 초과 (jobID 7.7조류 방어)
    UnknownModule,    // 4단계: 화이트리스트 5종 외 (BeyondLimit 등 미지 모듈)
    MapLimit,         // 5단계: 통계 맵 엔트리 상한 — 통계 수집기가 사용
};

// 스킵 로그 표기용 코드 문자열 ("LINE_TOO_LONG" …)
std::string_view skipReasonCode(SkipReason reason);

// 정상 모듈 화이트리스트 (design 4 — 로그 실측으로 확정된 5종)
enum class ModuleId : std::uint8_t {
    RadarTrackNodeState = 0,
    AntennaProfileSpec,
    BeamSteerCtrlUnitImpl,  // spd 라인 (작업2)
    DetectionTaskRunner,
    SectorSchedulerRTS,
};
inline constexpr std::size_t kModuleCount = 5;
std::string_view moduleName(ModuleId id);

// spd 유효 범위 (design 4-1 4단계): 정상값 137500 기준 충분한 여유 상한.
// 파싱 성공해도 범위 밖이면 평균에서 제외 — BeyondLimit(8.9e20) 오염 방어
inline constexpr double kSpdMin = 0.0;
inline constexpr double kSpdMax = 1e7;

// 세션 타임스탬프 허용 범위 (design 4-1 2단계)
inline constexpr std::int64_t kTsRangeHours = 48;

struct ParsedLine {
    ModuleId module = ModuleId::RadarTrackNodeState;
    // 통계 버킷 재료 — 달력 검증 완료값. 키 "YYYY-MM-DD HH"는 통계 수집기가 조립
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    // 작업2 재료 (BeamSteerCtrlUnitImpl 라인만 hasSpd=true)
    bool hasSpd = false;
    bool spdInRange = false;  // hasSpd일 때: [kSpdMin, kSpdMax) 통과 여부 (제외 표본 집계용)
    double spd = 0.0;         // spdInRange일 때만 유효
};

class LogLineParser {
public:
    struct Result {
        bool ok = false;
        ParsedLine line{};        // ok일 때만 유효
        SkipReason reason{};      // !ok일 때만 유효
    };

    // 재조립기가 내준 한 라인을 검증. tooLong은 재조립기의 상한 초과 플래그
    Result parse(std::string_view text, bool tooLong = false);

    // 세션 경계 — 타임스탬프 앵커 해제 (다음 세션의 첫 정상 라인이 새 기준)
    void reset();

private:
    bool _hasAnchor = false;
    std::int64_t _anchorSeconds = 0;  // 첫 정상 라인의 절대 초 (달력 산술 — OS 시간 함수 불필요)
};

}  // namespace server::parser
