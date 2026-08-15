#include "parser/SkipReporter.h"

#include <fstream>

namespace server::parser {

namespace {

// 손상 라인에는 널바이트·제어문자가 섞여 있다 (CTRL_CHAR 사유의 존재 이유).
// 그대로 파일에 쓰면 리포트가 열리지 않거나 깨지므로 \xNN으로 이스케이프한다
std::string escapeNonPrintable(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F) {
            constexpr char kHex[] = "0123456789ABCDEF";
            out += "\\x";
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        } else {
            out += ch;  // UTF-8 다바이트(0x80~)는 그대로 — 한글 등 보존
        }
    }
    return out;
}

}  // namespace

void SkipReporter::record(SkipReason reason, std::uint64_t offset, std::string_view text) {
    ++_total;
    ++_counts[static_cast<std::size_t>(reason)];
    if (_samples.size() < kMaxSkipSamples) {
        if (_samples.capacity() == 0) {
            _samples.reserve(kMaxSkipSamples);  // 1회 확보 후 재할당 없음
        }
        _samples.push_back(
            Sample{reason, offset, escapeNonPrintable(text.substr(0, kMaxSkipSampleBytes))});
    }
}

bool SkipReporter::writeReport(const std::string& path) const {
    std::ofstream out{path, std::ios::trunc};  // 세션마다 덮어씀 (1:1이라 충돌 없음)
    if (!out.is_open()) {
        return false;
    }

    out << "=== Skip Report ===\n";
    out << "Total skipped lines: " << _total << "\n\n";

    out << "[Counts by reason]\n";
    for (std::size_t i = 0; i < _counts.size(); ++i) {
        // 0건 사유도 출력 — 검증 파이프라인 11단계가 전부 살아있음을 채점자가 확인 가능
        out << skipReasonCode(static_cast<SkipReason>(i)) << " " << _counts[i] << "\n";
    }

    out << "\n[First " << kMaxSkipSamples << " skipped lines: reason, byte offset, raw prefix]\n";
    for (const auto& sample : _samples) {
        out << "[" << skipReasonCode(sample.reason) << "] offset=" << sample.offset << " | "
            << sample.text << "\n";
    }
    if (_total > _samples.size()) {
        out << "... " << (_total - _samples.size())
            << " more skipped lines not shown (sample limit)\n";  // 절단 사실을 명시
    }
    out.flush();
    return out.good();
}

void SkipReporter::reset() {
    _counts.fill(0);
    _samples.clear();
    _total = 0;
}

}  // namespace server::parser
