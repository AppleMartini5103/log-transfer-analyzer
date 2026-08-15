#include "util/Crc32.h"

#include <array>

namespace common {

namespace {

constexpr std::uint32_t kPolynomial = 0xEDB88320u;  // reflected IEEE 802.3

constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = makeTable();  // 컴파일 타임 생성 — 런타임 초기화 순서 문제 없음

}  // namespace

std::uint32_t crc32(std::uint32_t crc, std::string_view data) {
    // 호출 간 상태는 "완성된 CRC 값" — 각 호출이 init/final XOR을 내부에서 되감는다
    std::uint32_t c = crc ^ 0xFFFFFFFFu;
    for (const char ch : data) {
        const auto byte = static_cast<std::uint8_t>(ch);
        c = kTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace common
