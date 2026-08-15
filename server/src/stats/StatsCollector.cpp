#include "stats/StatsCollector.h"

namespace server::stats {

HourKey makeHourKey(int year, int month, int day, int hour) {
    return static_cast<HourKey>(year) * 1'000'000 + static_cast<HourKey>(month) * 10'000 +
           static_cast<HourKey>(day) * 100 + static_cast<HourKey>(hour);
}

std::string formatHourKey(HourKey key) {
    const auto year = static_cast<int>(key / 1'000'000);
    const auto month = static_cast<int>((key / 10'000) % 100);
    const auto day = static_cast<int>((key / 100) % 100);
    const auto hour = static_cast<int>(key % 100);

    std::string out(13, '0');  // "YYYY-MM-DD HH"
    const auto put = [&out](std::size_t pos, int value, int digits) {
        for (int i = digits - 1; i >= 0; --i) {
            out[pos + static_cast<std::size_t>(i)] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
    };
    put(0, year, 4);
    out[4] = '-';
    put(5, month, 2);
    out[7] = '-';
    put(8, day, 2);
    out[10] = ' ';
    put(11, hour, 2);
    return out;
}

std::uint64_t StatsCollector::totalEntries() const {
    std::uint64_t total = 0;
    for (const auto& bucket : _buckets) {
        total += bucket.size();
    }
    return total;
}

bool StatsCollector::record(const ParsedLine& line) {
    auto& bucket = _buckets[static_cast<std::size_t>(line.module)];
    const HourKey key = makeHourKey(line.year, line.month, line.day, line.hour);

    const auto it = bucket.find(key);
    if (it == bucket.end()) {
        if (totalEntries() >= kMaxStatsEntries) {
            return false;  // 신규 키만 거부 — 기존 키는 계속 누적 (부분 정상 동작 유지)
        }
        bucket.emplace(key, 1);
    } else {
        ++it->second;
    }

    // 작업2: 범위 통과분만 평균에 반영, 범위 밖은 제외 표본으로 집계
    if (line.hasSpd) {
        if (line.spdInRange) {
            _spdSum += line.spd;
            ++_validSpdSamples;
        } else {
            ++_excludedSpdSamples;
        }
    }
    return true;
}

double StatsCollector::averageSpeed() const {
    if (_validSpdSamples == 0) {
        return 0.0;
    }
    return _spdSum / static_cast<double>(_validSpdSamples);
}

void StatsCollector::reset() {
    for (auto& bucket : _buckets) {
        bucket.clear();
    }
    _validSpdSamples = 0;
    _excludedSpdSamples = 0;
    _spdSum = 0.0;
}

}  // namespace server::stats
