#pragma once

#include "protocol/Codec.h"
#include "protocol/protocol.h"

#include <array>
#include <cstdint>

// 메시지 프레이머 — "필요 바이트가 모일 때까지 누적 후 파싱" (컨벤션 9번).
// 헤더/트레일러도 TCP 스트림 경계에 걸쳐 쪼개져 도착함을 항상 가정한다 (design 8번).
//
// 세션은 1:1 순서 고정이라 "지금 기대하는 메시지 타입"이 항상 하나로 정해진다.
// 기대와 다른 타입이 오면 스트림이 꼬였다는 뜻 — 즉시 BadType (fail fast).
//
// 누적 버퍼는 최대 메시지 크기(UploadHeader 273B)로 bounded — 동적 할당 없음.
// 와이어의 filenameLen이 상한을 넘겨 전체 크기가 273B를 초과하면 본문을 받지 않고
// 즉시 BadValue ("length 프리픽스 읽기 → 상한 검증 → 본문 읽기" 순서 강제, 컨벤션 9번).

namespace common::protocol {

class Framer {
public:
    explicit Framer(MessageType expected) : _expected(expected) {}

    struct FeedResult {
        DecodeStatus status = DecodeStatus::NeedMoreData;
        std::size_t consumed = 0;  // 이번 입력에서 소비한 바이트 수 — 나머지는 호출부 몫(페이로드 등)
    };

    // 수신 바이트를 누적. 메시지 경계까지만 소비하고 초과분은 건드리지 않는다.
    // 반환 status: NeedMoreData(더 필요) / Ok(완성 — message() 사용 가능) / 에러(세션 종료 부류)
    // 완성·에러 후의 추가 feed는 아무것도 소비하지 않는다 — reset()이 먼저다.
    FeedResult feed(ByteView data);

    // status == Ok 이후 완성된 메시지 전체 바이트 (다음 reset까지 유효). 디코드는 코덱 몫.
    ByteView message() const { return ByteView{_buffer.data(), _size}; }

    // 다음 메시지 대기로 재사용 (세션 내 상태 전이: UploadHeader → UploadTrailer → DownloadDone)
    void reset(MessageType expected);

private:
    std::array<char, kUploadHeaderMaxSize> _buffer{};  // 273B — 최대 메시지가 곧 상한
    std::size_t _size = 0;
    MessageType _expected;
    DecodeStatus _status = DecodeStatus::NeedMoreData;
};

}  // namespace common::protocol
