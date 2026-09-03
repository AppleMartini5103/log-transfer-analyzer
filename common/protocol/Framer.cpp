#include "protocol/Framer.h"

#include <algorithm>

namespace common::protocol {

void Framer::reset(MessageType expected) {
    reset(expected, expected);
}

void Framer::reset(MessageType expected, MessageType alternate) {
    _size = 0;
    _expected = expected;
    _alternate = alternate;
    _status = DecodeStatus::NeedMoreData;
}

namespace {

// 전체 크기가 아직 확정되지 않은 단계에서 "다음에 어디까지 읽어야 하는가".
// 가변 길이 메시지는 길이 프리픽스의 위치가 타입마다 다르다 — UploadHeader는 18B,
// ResultRequest는 30B를 모아야 filenameLen을 읽을 수 있다. 이 값을 타입과 무관하게
// 하나로 두면 프리픽스에 닿지 못한 채 목표가 현재 크기와 같아져 진행이 멈춘다.
std::size_t fixedSizeFor(MessageType type) {
    switch (type) {
        case MessageType::UploadHeader:
            return kUploadHeaderFixedSize;
        case MessageType::ResultRequest:
            return kResultRequestFixedSize;
        default:
            return kPreambleSize;  // 고정 길이 메시지는 프리앰블만으로 크기가 확정된다
    }
}

}  // namespace

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
        if (_size >= kPreambleSize) {
            const auto actual = static_cast<std::uint8_t>(_buffer[kOffsetType]);
            if (actual != static_cast<std::uint8_t>(_expected) &&
                actual != static_cast<std::uint8_t>(_alternate)) {
                _status = DecodeStatus::BadType;
                break;
            }
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
            // 전체 크기 미확정 단계: 프리앰블 → 그 타입의 길이 프리픽스까지
            target = (_size < kPreambleSize) ? kPreambleSize
                                             : fixedSizeFor(messageType());
        }

        // 진행 불가 방어: 목표가 현재 크기 이하인데 크기가 미확정이면 아무 바이트도
        // 소비하지 못한 채 같은 판정을 반복하게 된다 (입력이 남아 있으면 무한 루프).
        // 알려진 타입에서는 도달하지 않지만, 새 가변 길이 메시지가 fixedSizeFor()에
        // 등록되지 않은 채 들어와도 행이 아니라 거부로 끝나도록 둔다
        if (target <= _size) {
            _status = DecodeStatus::BadValue;
            break;
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
