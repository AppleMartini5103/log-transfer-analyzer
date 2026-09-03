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
    // 6은 위 주석의 Heartbeat 예약 — 비워 둔다. 여기서 6을 쓰면 하트비트 전환 조건이
    // 발동할 때 버전을 올려야 하고, 그러면 배포된 바이너리끼리 상호 불통이 된다.
    ResultRequest = 7,  // 클라→서버: fileSize u64 + crc32 u32 + startOffset u64
                        //            + filenameLen u16 + filename 가변
};

// ── Ack 상태 코드 ───────────────────────────────────────────────────────────
enum class AckStatus : std::uint8_t {
    Ok = 0,
    CrcMismatch = 1,
    SizeMismatch = 2,
    ProtocolError = 3,
    ServerError = 4,
    // 재요청(ResultRequest)에만 쓰인다 — 보관본이 없거나 청구표가 어긋난 경우.
    // ProtocolError와 갈라놓는 이유: 클라이언트가 "재업로드가 필요하다"와 "이 서버는
    // 재요청을 모른다"를 구분해 알려야 한다. 후자는 옛 서버가 type=7을 BadType으로
    // 거부할 때 돌아온다.
    NoSuchResult = 5,
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
inline constexpr std::size_t kResultRequestFixedSize = kPreambleSize + 8 + 4 + 8 + 2;  // = 30
inline constexpr std::size_t kResultRequestMaxSize =
    kResultRequestFixedSize + kMaxFilenameLen;  // = 285

// ── 값 검증 상한 (design 8번 세부 확정) ─────────────────────────────────────
// fileSize = 0 허용 (빈 파일도 정상 세션). 8GiB 초과는 u64 쓰레기 값 방어
inline constexpr std::uint64_t kMaxFileSize = 8ULL * 1024 * 1024 * 1024;

// ResultHeader.csvSize 상한 — 수신 측(클라이언트)이 선할당 전에 검증한다.
// 업로드 방향만 상한이 있고(kMaxFilenameLen·kMaxFileSize) 결과 방향에는 없던 비대칭을
// 메우는 값이다 (컨벤션 9번: length 읽기 → 상한 검증 → 본문 읽기).
//
// [왜 예외 처리로는 안 되는가 — MSVC x64 실측]
//   상한 없이 csvSize를 그대로 std::string::reserve에 넘기면 값에 따라 셋으로 갈린다:
//     0xFFFFFFFFFFFF0000 → std::length_error("string too long")  → 잡는 곳 없어 terminate
//     1<<62              → std::bad_alloc                        → 잡는 곳 없어 terminate
//     64GiB              → 예외 없이 성공. capacity 68,719,476,751 확보
//   세 번째가 문제의 핵심이다. 예외가 나지 않으므로 try/catch를 둬도 걸리지 않는다.
//   클라이언트는 조용히 64GiB를 커밋하고 그 커밋 때문에 프로세스가 응답을 멈춘다
//   (이 실험 자체가 3~4분간 반환되지 않았다). 즉 방어는 "예외를 잡는 것"이 아니라
//   "할당하기 전에 값을 거부하는 것"이어야 한다.
//
// [4MiB의 산출 — 추정이 아니라 서버 상한에서 계산한 값]
//   서버 CSV = 헤더 + 버킷 행 + 빈 줄 + 지표 블록 (csv/CsvBuilder.cpp)
//     · 버킷 행 수: StatsCollector가 kMaxStatsEntries(10,000)에서 신규 키를 거부하므로
//       전 모듈 합산 10,000행이 절대 상한이다.
//     · 행 최대 길이: 모듈명 최장 "BeamSteerCtrlUnitImpl"(21) + ',' + "YYYY-MM-DD HH"(13)
//       + ',' + count u64 최대 20자리 + '\n' = 57바이트
//     · 버킷 블록 ≤ 10,000 x 57 = 570,000바이트
//     · 헤더 18 + 빈 줄·지표 4행 약 168 = 186바이트
//   → 서버가 만들 수 있는 CSV의 절대 최대는 약 570,186바이트(557KiB)다.
//   4MiB는 그 7.5배로, 정상 결과를 거부할 여지가 없으면서 kMaxStatsEntries가 7배까지
//   늘어도 프로토콜을 고칠 필요가 없다. 반대로 16MiB/64MiB급은 방어 값으로서 느슨하다 —
//   상한의 목적은 "터지지 않기"가 아니라 "정상 범위를 넘은 즉시 세션을 끊기"다.
inline constexpr std::uint64_t kMaxCsvSize = 4ULL * 1024 * 1024;

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

// 결과 재요청 — 결과 수신이 끊긴 클라이언트가 재업로드 없이 이어 받기 위해 보낸다.
//
// [왜 필요한가] 끊긴 다운로드가 잃는 것은 CSV 몇 KB가 아니라 500MB 재업로드다.
//   응답 방향에 이 메시지가 없으면, 5KB를 못 받은 대가로 요청 방향 전체를 다시 치른다.
//
// [fileSize/crc32/filename = 청구표] 서버는 보관본을 만든 업로드의 이 세 값과 대조해
//   일치할 때만 결과를 준다. 인증이 아니다 — 같은 파일을 가진 사람은 어차피 업로드로
//   같은 결과를 얻으므로 노출이 늘지 않는다. 표의 목적은 접근 통제가 아니라 남의 결과를
//   잘못 건네지 않는 것이다. 그냥 "재접속하면 마지막 결과를 준다"로 하면 A의 결과가
//   B에게 가고, 그것이 조용히 잘못된 결과라 가장 나쁘다.
//
// [startOffset] 이미 받은 바이트 수. 서버는 csv[startOffset..]을 보내고, ResultHeader의
//   csvSize/crc32는 여전히 완성 CSV 전체의 값이다 — 그래야 클라이언트가 이어 붙인 뒤
//   전체 무결성을 검증할 수 있다. "남은 바이트"로 두면 CRC가 조각의 것이 되어 그 검증을 잃는다.
struct ResultRequest {
    std::uint64_t fileSize = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t startOffset = 0;
    std::string filename;
};

// DownloadDone은 본문이 없어 값 타입 불필요 — MessageType만으로 표현

}  // namespace common::protocol
