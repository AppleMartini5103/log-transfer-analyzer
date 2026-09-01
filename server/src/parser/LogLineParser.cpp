#include "parser/LogLineParser.h"

#include <array>
#include <charconv>
#include <cmath>

namespace server::parser {

namespace {

using Result = LogLineParser::Result;

Result skip(SkipReason reason) {
    Result r;
    r.ok = false;
    r.reason = reason;
    return r;
}

// ── 숫자 파싱 (컨벤션 2번: from_chars만, 전체 소비 확인, 부호 없는 정수 필드) ──
bool parseInt64Field(std::string_view token, std::int64_t& out, SkipReason& reason) {
    if (token.empty() || token[0] < '0' || token[0] > '9') {
        reason = SkipReason::BadNumber;  // 부호·공백·비숫자 선두 거부
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(token.data(), token.data() + token.size(), out);
    if (ec == std::errc::result_out_of_range) {
        reason = SkipReason::NumOutOfRange;  // int64 초과 — 32비트 파싱 금지 근거의 연장
        return false;
    }
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
        reason = SkipReason::BadNumber;  // "123abc" 부분 소비 거부
        return false;
    }
    return true;
}

bool parseDoubleField(std::string_view token, double& out) {
    if (token.empty()) {
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(token.data(), token.data() + token.size(), out);
    // from_chars도 "inf"/"nan" 문자열은 파싱 성공함 — isfinite 필수 (design 4-1 3단계)
    return ec == std::errc{} && ptr == token.data() + token.size() && std::isfinite(out);
}

// "d+->d+" (lockState[1->0] 류 상태 전이 값)
bool isArrowPair(std::string_view token) {
    const std::size_t arrow = token.find("->");
    if (arrow == 0 || arrow == std::string_view::npos) {
        return false;
    }
    const auto allDigits = [](std::string_view s) {
        if (s.empty()) {
            return false;
        }
        for (const char ch : s) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        return true;
    };
    return allDigits(token.substr(0, arrow)) && allDigits(token.substr(arrow + 2));
}

// ── 달력 (Howard Hinnant days_from_civil — 순수 산술, OS 시간 함수 불필요) ──
constexpr std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool isLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int daysInMonth(int y, int m) {
    constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && isLeapYear(y)) {
        return 29;
    }
    return kDays[static_cast<std::size_t>(m - 1)];
}

bool allDigitsAt(std::string_view s, std::size_t pos, std::size_t count) {
    for (std::size_t i = pos; i < pos + count; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

int digitsToInt(std::string_view s, std::size_t pos, std::size_t count) {
    int v = 0;
    for (std::size_t i = pos; i < pos + count; ++i) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// "YYYY-MM-DD_hh:mm:ss.ffffff" (26자 고정) — 포맷 + 달력 유효성 (2단계)
bool parseTimestamp(std::string_view ts, ParsedLine& out, std::int64_t& absSeconds) {
    if (ts.size() != 26 || ts[4] != '-' || ts[7] != '-' || ts[10] != '_' || ts[13] != ':' ||
        ts[16] != ':' || ts[19] != '.') {
        return false;
    }
    if (!allDigitsAt(ts, 0, 4) || !allDigitsAt(ts, 5, 2) || !allDigitsAt(ts, 8, 2) ||
        !allDigitsAt(ts, 11, 2) || !allDigitsAt(ts, 14, 2) || !allDigitsAt(ts, 17, 2) ||
        !allDigitsAt(ts, 20, 6)) {
        return false;
    }
    const int year = digitsToInt(ts, 0, 4);
    const int month = digitsToInt(ts, 5, 2);
    const int day = digitsToInt(ts, 8, 2);
    const int hour = digitsToInt(ts, 11, 2);
    const int minute = digitsToInt(ts, 14, 2);
    const int second = digitsToInt(ts, 17, 2);
    // 달력 유효성: 13월, 32일, 25시, 61분, 61초, 평년 2/29 전부 거부
    if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) || hour > 23 ||
        minute > 59 || second > 59) {
        return false;
    }
    out.year = year;
    out.month = month;
    out.day = day;
    out.hour = hour;
    absSeconds = daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

// ── 메시지 괄호 짝 검사 (1단계 규칙: 미닫힘·빈 []·홀로 ] 전부 거부) ──
bool bracketsSane(std::string_view message) {
    bool open = false;
    std::size_t contentLen = 0;
    for (const char ch : message) {
        if (ch == '[') {
            if (open) {
                return false;  // 중첩 없음이 정상 구조 — [ 안의 [ 거부
            }
            open = true;
            contentLen = 0;
        } else if (ch == ']') {
            if (!open || contentLen == 0) {
                return false;  // 홀로 ] / 빈 []
            }
            open = false;
        } else if (open) {
            ++contentLen;
        }
    }
    return !open;  // 미닫힘 거부 — 짝 없으면 경계를 넘지 않는다
}

// "name[" 필드 토큰 추출 — 선행 경계(시작/공백/쉼표/콜론) 확인으로 오탐 방지
bool findField(std::string_view message, std::string_view name, std::string_view& outToken) {
    std::size_t pos = 0;
    while (pos < message.size()) {
        const std::size_t found = message.find(name, pos);
        if (found == std::string_view::npos) {
            return false;
        }
        const bool boundaryOk =
            found == 0 || message[found - 1] == ' ' || message[found - 1] == ',' ||
            message[found - 1] == ':';
        const std::size_t bracket = found + name.size();
        if (boundaryOk && bracket < message.size() && message[bracket] == '[') {
            const std::size_t close = message.find(']', bracket + 1);
            if (close == std::string_view::npos) {
                return false;  // bracketsSane가 먼저 거르지만 방어적으로
            }
            outToken = message.substr(bracket + 1, close - bracket - 1);
            return true;
        }
        pos = found + 1;
    }
    return false;
}

// ── 모듈 화이트리스트 + 모듈별 관측 필드 명세 (design 4 로그 실측 스키마) ──
// 통계와 무관한 필드는 구조(괄호 짝)만 확인 — 과잉 거부 방지 (4단계 규칙).
// 문서화된 숫자 필드는 "존재하면 엄격, 부재는 관용" (리뷰 2, design 4-1 갱신):
//   - 존재하는 값의 비정상(nodeUID[NONE], rfLane[X])은 여전히 차단 — CorruptPayload 방어
//   - 부재는 스킵 사유가 아니다. 이 목록은 500MB 표본의 관측 스키마일 뿐이라, 부재를
//     훼손으로 단정하면 스키마가 조금 다른 정상 로그의 라인을 통째로 잃는다 (필드 과신).
//   - 관용이 우회로가 되지 않는 근거: bracketsSane()이 이 검사보다 앞서 돈다. nodeUID[
//     처럼 괄호를 깨뜨린 훼손은 BAD_BRACKET으로 먼저 걸리므로, 여기 도달한 "부재"는
//     훼손이 아니라 스키마 차이만을 의미한다.
//   - 483MB 실측(2026-09-02): 부재로 스킵되던 정상 라인 0건 — 이 완화는 참조 로그의
//     판정을 하나도 바꾸지 않으며(스킵 26건 전원 독약), 미래 스키마 변화만 대비한다.
struct ModuleSpec {
    std::string_view name;
    std::array<std::string_view, 2> intFields;   // 빈 항목은 ""
    std::string_view arrowField;                 // "d->d" 필드 (없으면 "")
};

constexpr std::array<ModuleSpec, kModuleCount> kModules = {{
    {"RadarTrackNodeState", {"nodeUID", "rfLane"}, "lockState"},
    {"AntennaProfileSpec", {"sectorID", ""}, ""},      // element[i][i->j][k]는 괄호 짝만
    {"BeamSteerCtrlUnitImpl", {"unitAddr", ""}, ""},   // spd/advDelta는 double 전용 처리
    {"DetectionTaskRunner", {"jobID", ""}, ""},        // command[RUN] 등 워드 필드는 구조만
    {"SectorSchedulerRTS", {"jobID", "gatedFlag"}, ""},
}};

}  // namespace

std::string_view skipReasonCode(SkipReason reason) {
    switch (reason) {
        case SkipReason::LineTooLong:
            return "LINE_TOO_LONG";
        case SkipReason::Empty:
            return "EMPTY";
        case SkipReason::CtrlChar:
            return "CTRL_CHAR";
        case SkipReason::BadFrame:
            return "BAD_FRAME";
        case SkipReason::BadBracket:
            return "BAD_BRACKET";
        case SkipReason::BadTimestamp:
            return "BAD_TIMESTAMP";
        case SkipReason::TsOutOfRange:
            return "TS_OUT_OF_RANGE";
        case SkipReason::BadNumber:
            return "BAD_NUMBER";
        case SkipReason::NumOutOfRange:
            return "NUM_OUT_OF_RANGE";
        case SkipReason::UnknownModule:
            return "UNKNOWN_MODULE";
        case SkipReason::MapLimit:
            return "MAP_LIMIT";
    }
    return "UNKNOWN";  // 도달 불가
}

std::string_view moduleName(ModuleId id) {
    return kModules[static_cast<std::size_t>(id)].name;
}

void LogLineParser::reset() {
    _hasAnchor = false;
    _anchorSeconds = 0;
}

LogLineParser::Result LogLineParser::parse(std::string_view text, bool tooLong) {
    // ── 0단계: 바이트/라인 레벨 ──
    if (tooLong) {
        return skip(SkipReason::LineTooLong);
    }
    bool allSpaces = true;
    for (const char ch : text) {
        if (ch != ' ') {
            allSpaces = false;
            break;
        }
    }
    if (allSpaces) {
        return skip(SkipReason::Empty);  // 빈 라인 포함
    }
    for (const char ch : text) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x20 || b == 0x7F) {
            return skip(SkipReason::CtrlChar);  // 널바이트·탭·내부 \r 포함 전부
        }
    }

    // ── 1단계: 프레임 구조 — [ts][pid][tid][sess] BYDA::모듈: 메시지 (위치 기반) ──
    std::array<std::string_view, 4> headerTokens;
    std::size_t pos = 0;
    for (auto& token : headerTokens) {
        if (pos >= text.size() || text[pos] != '[') {
            return skip(SkipReason::BadFrame);  // 선두 [ 누락(HeadBraceLoss) 포함
        }
        const std::size_t close = text.find(']', pos + 1);
        if (close == std::string_view::npos || close == pos + 1) {
            return skip(SkipReason::BadFrame);  // 미닫힘·빈 헤더 필드
        }
        token = text.substr(pos + 1, close - pos - 1);
        pos = close + 1;
    }
    constexpr std::string_view kSeparator = " BYDA::";
    if (text.compare(pos, kSeparator.size(), kSeparator) != 0) {
        return skip(SkipReason::BadFrame);  // 헤더 필드 개수 과다/garbage 프레임도 여기서 걸림
    }
    pos += kSeparator.size();
    const std::size_t moduleEnd = text.find(':', pos);
    if (moduleEnd == std::string_view::npos || moduleEnd == pos ||
        moduleEnd + 1 >= text.size() || text[moduleEnd + 1] != ' ') {
        return skip(SkipReason::BadFrame);  // "모듈명: " 구조 불일치
    }
    const std::string_view module = text.substr(pos, moduleEnd - pos);
    const std::string_view message = text.substr(moduleEnd + 2);

    // ── 2단계: 타임스탬프 ──
    ParsedLine line;
    std::int64_t absSeconds = 0;
    if (!parseTimestamp(headerTokens[0], line, absSeconds)) {
        return skip(SkipReason::BadTimestamp);
    }
    if (_hasAnchor) {
        const std::int64_t limit = kTsRangeHours * 3600;
        const std::int64_t delta = absSeconds - _anchorSeconds;
        if (delta > limit || delta < -limit) {
            return skip(SkipReason::TsOutOfRange);  // 버킷 맵 무한 증식 방지
        }
    }

    // ── 3단계: 헤더 숫자 (pid/tid/sess — int64 전체 소비) ──
    for (std::size_t i = 1; i < headerTokens.size(); ++i) {
        std::int64_t value = 0;
        SkipReason reason{};
        if (!parseInt64Field(headerTokens[i], value, reason)) {
            return skip(reason);
        }
    }

    // ── 4단계: 도메인 — 괄호 짝 → 모듈 화이트리스트 → 모듈별 필수 필드 ──
    if (!bracketsSane(message)) {
        return skip(SkipReason::BadBracket);
    }
    const ModuleSpec* spec = nullptr;
    for (std::size_t i = 0; i < kModules.size(); ++i) {
        if (kModules[i].name == module) {
            spec = &kModules[i];
            line.module = static_cast<ModuleId>(i);
            break;
        }
    }
    if (spec == nullptr) {
        return skip(SkipReason::UnknownModule);  // BeyondLimit 등 — default-deny
    }
    for (const std::string_view fieldName : spec->intFields) {
        if (fieldName.empty()) {
            continue;
        }
        std::string_view token;
        if (!findField(message, fieldName, token)) {
            continue;  // 부재는 관용 — 근거는 kModules 주석 (bracketsSane이 앞서 돈다)
        }
        std::int64_t value = 0;
        SkipReason reason{};
        if (!parseInt64Field(token, value, reason)) {
            return skip(reason);  // 존재하면 엄격 — 독약 방어 불변
        }
    }
    if (!spec->arrowField.empty()) {
        std::string_view token;
        if (findField(message, spec->arrowField, token) && !isArrowPair(token)) {
            return skip(SkipReason::BadNumber);  // 부재는 관용, 존재하면 형식 검증
        }
    }

    // 작업2 재료: BeamSteerCtrlUnitImpl의 spd/advDelta (double + isfinite + 도메인 범위)
    // 부재 관용의 두 갈래 (리뷰 2):
    //   - advDelta 부재 → 카운트 + spd 표본 정상 산입 (unitAddr처럼 검증만 하고 결과에
    //     쓰지 않는 필드라 같은 취급이 일관된다)
    //   - spd 부재 → 카운트되지만 표본 없음. 이 라인 수는 현재 CSV에서
    //     "BeamSteer 계수 − valid − excluded"로 유도만 가능하다 — 라벨 붙은 행
    //     (missing_spd_samples)으로의 노출은 리뷰 3의 CSV 작업에서 함께 한다
    if (line.module == ModuleId::BeamSteerCtrlUnitImpl) {
        std::string_view spdToken;
        std::string_view advToken;
        const bool hasSpdField = findField(message, "spd", spdToken);
        const bool hasAdvField = findField(message, "advDelta", advToken);
        double spd = 0.0;
        double advDelta = 0.0;
        if (hasSpdField && !parseDoubleField(spdToken, spd)) {
            return skip(SkipReason::BadNumber);  // 존재하면 엄격
        }
        if (hasAdvField && !parseDoubleField(advToken, advDelta)) {
            return skip(SkipReason::BadNumber);
        }
        if (hasSpdField) {
            line.hasSpd = true;
            line.spdInRange = spd >= kSpdMin && spd < kSpdMax;
            line.spd = line.spdInRange ? spd : 0.0;
            // 범위 밖(8.9e20류)은 라인 자체는 유효 — 표본만 제외 (excluded_spd_samples 재료)
        }
    }

    // 전 단계 통과 — 첫 정상 라인이면 세션 앵커 확정
    if (!_hasAnchor) {
        _hasAnchor = true;
        _anchorSeconds = absSeconds;
    }
    Result result;
    result.ok = true;
    result.line = line;
    return result;
}

}  // namespace server::parser
