#include "protocol/Framer.h"

#include <algorithm>

namespace common::protocol {

void Framer::reset(MessageType expected) {
    _size = 0;
    _expected = expected;
    _status = DecodeStatus::NeedMoreData;
}

Framer::FeedResult Framer::feed(ByteView data) {
    if (_status != DecodeStatus::NeedMoreData) {
        return {_status, 0};
    }

    std::size_t consumed = 0;
    while (true) {
        // 1) 현재 누적분으로 판정 시도 — magic/version 불량은 여기서 걸러짐
        const ByteView current{_buffer.data(), _size};
        std::size_t total = 0;
        const DecodeStatus sizeStatus = expectedMessageSize(current, total);

        if (sizeStatus != DecodeStatus::Ok && sizeStatus != DecodeStatus::NeedMoreData) {
            _status = sizeStatus;
            break;
        }

        // 2) 프리앰블이 모였으면 기대 타입 검사 (코덱과 같은 순서: magic → version → type)
        if (_size >= kPreambleSize &&
            static_cast<std::uint8_t>(_buffer[kOffsetType]) !=
                static_cast<std::uint8_t>(_expected)) {
            _status = DecodeStatus::BadType;
            break;
        }

        // 3) 목표 크기 결정
        std::size_t target = 0;
        if (sizeStatus == DecodeStatus::Ok) {
            if (total > _buffer.size()) {
                // filenameLen 상한 초과 — 본문을 받지 않고 거부 (bounded 버퍼가 곧 방어선)
                _status = DecodeStatus::BadValue;
                break;
            }
            if (_size == total) {
                _status = DecodeStatus::Ok;
                break;
            }
            target = total;
        } else {
            // 전체 크기 미확정 단계: 프리앰블 → (UploadHeader라면) filenameLen까지
            target = (_size < kPreambleSize) ? kPreambleSize : kUploadHeaderFixedSize;
        }

        // 4) 입력에서 목표까지만 복사 — 메시지 경계 밖(페이로드)은 소비하지 않는다
        if (consumed == data.size()) {
            break;  // 입력 소진 — NeedMoreData 유지
        }
        const std::size_t want = target - _size;
        const std::size_t take = std::min(want, data.size() - consumed);
        std::copy_n(data.data() + consumed, take, _buffer.data() + _size);
        _size += take;
        consumed += take;
    }

    return {_status, consumed};
}

}  // namespace common::protocol
