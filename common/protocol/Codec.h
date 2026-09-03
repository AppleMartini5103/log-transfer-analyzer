#pragma once

#include "protocol/protocol.h"

#include <cstdint>
#include <string_view>
#include <vector>

// 프로토콜 코덱 — 메시지 값 타입 <-> 와이어 바이트 (design 8번).
// 직렬화 3원칙: 구조체 memcpy 금지 / 필드별 바이트 단위(시프트) 기록 / 정수는 전부 BE.
// 에러는 예외가 아니라 반환값 (컨벤션 3번). 검증은 default-deny (수신 측 검증 규칙).

namespace common::protocol {

// 디코드 입력은 길이 기반 뷰 — uv 수신 버퍼(char*)와 그대로 호환
using ByteView = std::string_view;

enum class DecodeStatus : std::uint8_t {
    Ok = 0,
    NeedMoreData,  // 잘린 입력 — 판정 불가, 프레이머가 더 누적해야 함 (에러 아님)
    // ── 스트림 신뢰 불가 부류: 응답 없이 즉시 세션 종료 (design 11번 검증 실패 ①) ──
    BadMagic,
    BadVersion,
    BadType,  // 미지의 type + "기대한 메시지가 아닌 type" 둘 다 (1:1 순서 고정이므로 동급)
    // ── 파싱됐지만 값 무효 부류: Ack(PROTOCOL_ERROR) 송신 후 종료 (검증 실패 ②) ──
    BadValue,  // fileSize > 8GiB, filenameLen 0/255 초과, 파일명 금지 문자 등
};

// ── 프레이머 지원: 메시지 전체 크기 판정 ────────────────────────────────────
// 프리앰블(+UploadHeader는 filenameLen까지)을 읽어 이 메시지의 전체 바이트 수를 알아낸다.
// 반환 Ok: outSize에 전체 크기 / NeedMoreData: 더 누적 필요 / 그 외: 프리앰블 자체가 불량
DecodeStatus expectedMessageSize(ByteView buf, std::size_t& outSize);

// ── 인코드 (송신 측 — 값은 호출부 책임, 여기선 검증하지 않음) ───────────────
std::vector<char> encode(const UploadHeader& msg);
std::vector<char> encode(const UploadTrailer& msg);
std::vector<char> encode(const Ack& msg);
std::vector<char> encode(const ResultHeader& msg);
std::vector<char> encode(const ResultRequest& msg);
std::vector<char> encodeDownloadDone();

// ── 디코드 (수신 측 — 프리앰블 검증 + 값 검증 포함, default-deny) ───────────
// buf는 최소한 해당 메시지 전체를 담아야 하며, 초과분은 읽지 않는다 (경계 분리는 프레이머 몫)
DecodeStatus decode(ByteView buf, UploadHeader& out);
DecodeStatus decode(ByteView buf, UploadTrailer& out);
DecodeStatus decode(ByteView buf, Ack& out);
DecodeStatus decode(ByteView buf, ResultHeader& out);
DecodeStatus decode(ByteView buf, ResultRequest& out);
DecodeStatus decodeDownloadDone(ByteView buf);  // 본문 없음 — 프리앰블 검증만

}  // namespace common::protocol
