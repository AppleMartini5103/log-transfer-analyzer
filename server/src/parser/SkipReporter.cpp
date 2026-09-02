#include "parser/SkipReporter.h"

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

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
    if (reason == SkipReason::UnknownModule) {
        recordUnknownModule(text);
    }
    if (_samples.size() < kMaxSkipSamples) {
        if (_samples.capacity() == 0) {
            _samples.reserve(kMaxSkipSamples);  // 1회 확보 후 재할당 없음
        }
        _samples.push_back(
            Sample{reason, offset, escapeNonPrintable(text.substr(0, kMaxSkipSampleBytes))});
    }
}

// 모듈명 추출 — 이 라인은 1단계 프레임 검사를 이미 통과했으므로 " BYDA::" + 이름 + ": "
// 구조가 보장된다 (4단계에서야 화이트리스트에 걸린 라인이다). 그래도 npos는 방어적으로
// 처리한다: 이름을 못 뽑은 라인은 목록이 아니라 절단 카운터로 보내 합계 불변식을 지킨다.
void SkipReporter::recordUnknownModule(std::string_view text) {
    constexpr std::string_view kMarker = " BYDA::";
    const std::size_t marker = text.find(kMarker);
    if (marker == std::string_view::npos) {
        ++_unlistedUnknownLines;
        return;
    }
    const std::size_t begin = marker + kMarker.size();
    const std::size_t colon = text.find(": ", begin);
    if (colon == std::string_view::npos || colon == begin) {
        ++_unlistedUnknownLines;
        return;
    }
    // 원문 바이트로 먼저 자르고 이스케이프한다 (표본 처리와 같은 순서 — 상한은 입력 바이트에)
    const std::size_t length = std::min(colon - begin, kMaxModuleNameBytes);
    std::string name = escapeNonPrintable(text.substr(begin, length));

    const auto it = _unknownModules.find(name);
    if (it != _unknownModules.end()) {
        ++it->second;  // 상한 도달 후에도 기존 이름은 계속 누적 (StatsCollector::record와 같은 규칙)
        return;
    }
    if (_unknownModules.size() >= kMaxUnknownModules) {
        ++_unlistedUnknownLines;  // 신규 이름만 거부
        return;
    }
    _unknownModules.emplace(std::move(name), 1);
}

std::uint64_t SkipReporter::unknownModuleCount(const std::string& name) const {
    const auto it = _unknownModules.find(name);
    return it == _unknownModules.end() ? 0 : it->second;
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

    // 미지 모듈명 — UNKNOWN_MODULE이 0이면 블록 자체를 내지 않는다 (result.csv의 0-사유 생략과 같은 규약)
    if (_counts[static_cast<std::size_t>(SkipReason::UnknownModule)] > 0) {
        out << "\n[Unknown module names]\n";
        std::vector<std::pair<const std::string*, std::uint64_t>> ordered;
        ordered.reserve(_unknownModules.size());
        for (const auto& entry : _unknownModules) {
            ordered.emplace_back(&entry.first, entry.second);
        }
        // 건수 내림차순, 동수면 이름 오름차순 — 결정적 출력이라 회귀 비교가 가능하다
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : *a.first < *b.first;
        });
        for (const auto& entry : ordered) {
            out << *entry.first << " " << entry.second << "\n";
        }
        if (_unlistedUnknownLines > 0) {
            // distinct 상한에 걸린 라인 수 — "이름 개수"가 아니라 "라인 수"인 이유:
            // 상한을 넘은 이름을 세려면 그 이름들을 저장해야 하고, 그러면 상한이 무의미해진다.
            // 라인 수로 두면 집계 합 + 이 값 == count(UNKNOWN_MODULE) 불변식이 성립한다.
            out << "... " << _unlistedUnknownLines
                << " more skipped lines whose module name is not listed (distinct-name limit "
                << kMaxUnknownModules << ")\n";
        }
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
    _unknownModules.clear();
    _unlistedUnknownLines = 0;
    _total = 0;
}

}  // namespace server::parser
