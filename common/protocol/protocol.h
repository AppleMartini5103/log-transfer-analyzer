#pragma once

#include <array>
#include <cstdint>
#include <string>

// 프로토콜 상수·타입의 단일 정의 (design 8번 헤더 바이너리 규격 / 9번 타임아웃).
// 클라/서버가 이 헤더 하나를 공유한다 — 두 벌 정의 금지 (컨벤션 9번).
// 정수는 전부 빅엔디안으로 직렬화하며, 구조체 memcpy 송수신은 금지 (코덱이 필드별로 기록).

namespace common::protocol {

// ── 공통 프리앰블 (모든 메시지 앞 8바이트 고정) ─────────────────────────────
// offset 0: magic 4B / 4: version 1B / 5: type 1B / 6: reserved 2B(=0, 수신 측 무시)
inline constexpr std::array<std::uint8_t, 4> kMagic = {0x42, 0x59, 0x44, 0x41};  // 'B','Y','D','A'
inline constexpr std::uint8_t kProtocolVersion = 1;  // 서버는 1 외 전부 거부 (협상 없음)

inline constexpr std::size_t kOffsetMagic = 0;
inline constexpr std::size_t kOffsetVersion = 4;
inline constexpr std::size_t kOffsetType = 5;
inline constexpr std::size_t kOffsetReserved = 6;
inline constexpr std::size_t kPreambleSize = 8;

// ── 메시지 타입 (design 8번 type별 본문) ────────────────────────────────────
// type=6 Heartbeat는 번호만 예약 (design 12번 — 미구현. 유효 타입 집합에 넣지 않는다)
enum class MessageType : std::uint8_t {
    UploadHeader = 1,   // 클라→서버: fileSize u64 + filenameLen u16 + filename 가변
    UploadTrailer = 2,  // 클라→서버: crc32 u32 (페이로드 마지막 바이트 직후)
    Ack = 3,            // 서버→클라: status u8 + receivedBytes u64
    ResultHeader = 4,   // 서버→클라: csvSize u64 + crc32 u32 (직후 CSV 스트림)
    DownloadDone = 5,   // 클라→서버: 본문 없음
};

// ── Ack 상태 코드 ───────────────────────────────────────────────────────────
enum class AckStatus : std::uint8_t {
    Ok = 0,
    CrcMismatch = 1,
    SizeMismatch = 2,
    ProtocolError = 3,
    ServerError = 4,
};

// ── 메시지 크기 (프리앰블 포함 전체 바이트) ─────────────────────────────────
// 수신 측은 "필요 바이트가 모일 때까지 누적 후 파싱" (컨벤션 9번) — 이 크기가 그 기준값
inline constexpr std::size_t kMaxFilenameLen = 255;  // 초과 시 PROTOCOL_ERROR (design 8번)
inline constexpr std::size_t kUploadHeaderFixedSize = kPreambleSize + 8 + 2;  // 가변부(filename) 제외
inline constexpr std::size_t kUploadHeaderMaxSize = kUploadHeaderFixedSize + kMaxFilenameLen;  // = 273
inline constexpr std::size_t kUploadTrailerSize = kPreambleSize + 4;   // = 12
inline constexpr std::size_t kAckSize = kPreambleSize + 1 + 8;         // = 17
inline constexpr std::size_t kResultHeaderSize = kPreambleSize + 8 + 4;  // = 20
inline constexpr std::size_t kDownloadDoneSize = kPreambleSize;        // = 8 (본문 없음)

// ── 값 검증 상한 (design 8번 세부 확정) ─────────────────────────────────────
// fileSize = 0 허용 (빈 파일도 정상 세션). 8GiB 초과는 u64 쓰레기 값 방어
inline constexpr std::uint64_t kMaxFileSize = 8ULL * 1024 * 1024 * 1024;

// ── 타임아웃 (design 9번 — 값 산정 근거·재실측 절차 포함) ───────────────────
// ①류: 데이터 흐름 상태의 무활동 타이머 (활동마다 리셋. 총 전송 시간 상한은 없음)
inline constexpr std::uint64_t kIdleTimeoutMs = 30'000;
// ②류: 응답 대기 상태 (WAIT_HEADER/WAIT_ACK/WAIT_RESULT/WAIT_DONE/CONNECTING).
// 120초는 실측 전 자리표시자 — 구현 후 "p99 × 3~5"로 갱신이 정식 절차 (design 9번)
inline constexpr std::uint64_t kResponseTimeoutMs = 120'000;

// ── 실행 인터페이스 (design 5번 확정 값) ────────────────────────────────────
inline constexpr std::uint16_t kDefaultPort = 23507;

// ── 메시지 값 타입 (코덱의 입출력 — 와이어 표현은 코덱이 담당) ──────────────
struct UploadHeader {
    std::uint64_t fileSize = 0;
    std::string filename;  // UTF-8, 로그 표시 전용 (경로 사용 금지 — design 8번)
};

struct UploadTrailer {
    std::uint32_t crc32 = 0;
};

struct Ack {
    AckStatus status = AckStatus::Ok;
    std::uint64_t receivedBytes = 0;
};

struct ResultHeader {
    std::uint64_t csvSize = 0;
    std::uint32_t crc32 = 0;  // CSV는 메모리 완성본이라 헤더에 포함 가능 (design 체크섬 설계)
};

// DownloadDone은 본문이 없어 값 타입 불필요 — MessageType만으로 표현

}  // namespace common::protocol
