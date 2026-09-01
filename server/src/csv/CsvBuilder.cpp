#include "csv/CsvBuilder.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <utility>

namespace server::csv {

namespace {

// 소수 6자리 고정 표기 — 로그 원본 정밀도(137500.000000)와 대칭 (design 근거).
// std::to_string은 로케일 영향을 받지 않지만 자릿수 제어가 안 되고, ostringstream은
// 로케일에 따라 소수점이 ','가 될 수 있어 CSV가 깨진다. 직접 조립해 둘 다 회피
std::string formatFixed6(double value) {
    if (!(value > 0.0)) {
        // 음수·NaN은 도메인상 도달 불가(spd 범위 검증 통과분만 평균) — 0으로 표기
        return "0.000000";
    }
    const auto whole = static_cast<std::uint64_t>(value);
    const double fractional = value - static_cast<double>(whole);
    auto micros = static_cast<std::uint64_t>(fractional * 1'000'000.0 + 0.5);
    std::uint64_t integral = whole;
    if (micros >= 1'000'000) {  // 반올림 자리올림
        micros -= 1'000'000;
        ++integral;
    }

    std::string out = std::to_string(integral);
    out += '.';
    std::array<char, 6> digits{};
    for (int i = 5; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = static_cast<char>('0' + micros % 10);
        micros /= 10;
    }
    out.append(digits.data(), digits.size());
    return out;
}

}  // namespace

std::string buildResultCsv(const server::stats::StatsCollector& stats,
                           const server::parser::SkipReporter& reporter,
                           std::uint64_t totalLines) {
    std::string csv;
    csv.reserve(8192);  // 실측 125버킷 ≈ 5KB — 1회 확보로 재할당 최소화

    // ── 작업1 블록: 모듈별 시간대 발생 횟수 ──
    csv += "module,hour,count\n";
    stats.forEachBucket([&csv](server::parser::ModuleId module, server::stats::HourKey key,
                               std::uint64_t count) {
        csv += server::parser::moduleName(module);
        csv += ',';
        csv += server::stats::formatHourKey(key);  // "YYYY-MM-DD HH" — 쉼표 없음, 인용 불필요
        csv += ',';
        csv += std::to_string(count);
        csv += '\n';
    });

    // ── 블록 구분 빈 줄 ──
    csv += '\n';

    // ── 작업2 블록: 평균 속도 + 증빙 카운터 (행 구성은 CsvBuilder.h의 계약 주석 참조) ──
    csv += "metric,value\n";
    csv += "total_lines," + std::to_string(totalLines) + "\n";
    csv += "avg_speed," + formatFixed6(stats.averageSpeed()) + "\n";
    csv += "valid_spd_samples," + std::to_string(stats.validSpdSamples()) + "\n";
    csv += "excluded_spd_samples," + std::to_string(stats.excludedSpdSamples()) + "\n";
    csv += "missing_spd_samples," + std::to_string(stats.missingSpdSamples()) + "\n";
    csv += "skipped_lines," + std::to_string(reporter.total()) + "\n";

    // skip_reason_* — 비0 사유만, 코드 알파벳순 (결정적 출력이라 테스트가 단순해진다).
    // enum 순서가 아니라 코드 문자열로 정렬하는 이유: 클라이언트에 보이는 것은 코드뿐이고,
    // enum 재배열이 CSV 순서를 바꾸는 원격 결합을 만들지 않기 위해서다
    std::array<std::pair<std::string_view, std::uint64_t>, server::parser::kSkipReasonCount>
        reasons{};
    std::size_t reasonCount = 0;
    for (std::size_t i = 0; i < server::parser::kSkipReasonCount; ++i) {
        const auto reason = static_cast<server::parser::SkipReason>(i);
        const std::uint64_t count = reporter.count(reason);
        if (count > 0) {
            reasons[reasonCount++] = {server::parser::skipReasonCode(reason), count};
        }
    }
    std::sort(reasons.begin(),
              reasons.begin() + static_cast<std::ptrdiff_t>(reasonCount),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < reasonCount; ++i) {
        csv += "skip_reason_";
        csv += reasons[i].first;
        csv += ',';
        csv += std::to_string(reasons[i].second);
        csv += '\n';
    }
    return csv;
}

bool writeCsvFile(const std::string& path, const std::string& csv) {
    // binary: 윈도우에서 '\n'이 CRLF로 변환되면 csvSize·CRC32가 전송본과 어긋난다
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out.is_open()) {
        return false;
    }
    out.write(csv.data(), static_cast<std::streamsize>(csv.size()));
    out.flush();
    return out.good();
}

}  // namespace server::csv
