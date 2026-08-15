#pragma once

#include <cstdint>
#include <string_view>

namespace common {

// CRC32 — IEEE 802.3 / zlib 호환 변형 고정 (design 8번 세부 확정):
// reflected, poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
//
// 증분 사용법 (zlib과 동일한 관례 — Python zlib.crc32와 교차 검증 가능):
//   std::uint32_t crc = 0;                  // 초기값 0
//   crc = crc32(crc, chunk1);               // 청크마다 반복
//   crc = crc32(crc, chunk2);               // 중간값도 그대로 유효한 CRC
//
// 커버 범위는 페이로드 바이트만 (프리앰블·헤더·트레일러 자신은 제외 — design 8번).
// 테스트 벡터: crc32(0, "123456789") == 0xCBF43926
std::uint32_t crc32(std::uint32_t crc, std::string_view data);

}  // namespace common
