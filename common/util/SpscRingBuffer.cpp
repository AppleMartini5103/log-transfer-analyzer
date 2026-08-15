#include "util/SpscRingBuffer.h"

#include <algorithm>

namespace common {

std::size_t ringSlotCountFor(std::size_t slotSize) {
    if (slotSize == 0) {
        return kDefaultSlotCount;
    }
    const std::size_t withinBudget = kRingBudgetBytes / slotSize;
    if (withinBudget < kMinSlotCount) {
        return kMinSlotCount;  // 예산보다 슬롯이 크면 최소 개수는 보장한다
    }
    return withinBudget < kDefaultSlotCount ? withinBudget : kDefaultSlotCount;
}

SpscRingBuffer::SpscRingBuffer(std::size_t slotCount, std::size_t slotSize)
    : _slotCount(slotCount),
      _slotSize(slotSize),
      _storage(slotCount * slotSize),
      _sizes(slotCount) {}

bool SpscRingBuffer::tryAcquire(WriteSlot& out) {
    const std::uint64_t tail = _tail.load(std::memory_order_relaxed);  // producer 소유 값
    const std::uint64_t head = _head.load(std::memory_order_acquire);
    if (tail - head == _slotCount) {
        return false;  // 꽉 참 — backpressure 지점
    }
    out.data = &_storage[(tail % _slotCount) * _slotSize];
    out.capacity = _slotSize;
    return true;
}

void SpscRingBuffer::commit(std::size_t size) {
    const std::uint64_t tail = _tail.load(std::memory_order_relaxed);
    _sizes[tail % _slotCount] = size;
    // release: 슬롯 데이터·크기 기록이 tail 전진보다 먼저 보이도록 — 소유권 이동의 발행 지점
    _tail.store(tail + 1, std::memory_order_release);
}

bool SpscRingBuffer::full() const {
    const std::uint64_t tail = _tail.load(std::memory_order_relaxed);
    return tail - _head.load(std::memory_order_acquire) == _slotCount;
}

bool SpscRingBuffer::tryPeek(ReadView& out) const {
    const std::uint64_t head = _head.load(std::memory_order_relaxed);  // consumer 소유 값
    // acquire: tail 전진 이전의 데이터 기록까지 함께 관측됨 (commit의 release와 짝)
    const std::uint64_t tail = _tail.load(std::memory_order_acquire);
    if (tail == head) {
        return false;  // 빔
    }
    out.data = &_storage[(head % _slotCount) * _slotSize];
    out.size = _sizes[head % _slotCount];
    return true;
}

void SpscRingBuffer::release() {
    const std::uint64_t head = _head.load(std::memory_order_relaxed);
    // release: 소비가 끝난 뒤에만 슬롯이 재사용되도록 — tryAcquire의 acquire와 짝
    _head.store(head + 1, std::memory_order_release);
}

bool SpscRingBuffer::empty() const {
    const std::uint64_t head = _head.load(std::memory_order_relaxed);
    return _tail.load(std::memory_order_acquire) == head;
}

}  // namespace common
