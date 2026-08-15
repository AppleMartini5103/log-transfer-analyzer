#include "util/SpscRingBuffer.h"

#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using common::SpscRingBuffer;

namespace {

// producer 측에서 문자열 하나를 슬롯에 기록·발행
bool push(SpscRingBuffer& ring, std::string_view payload) {
    SpscRingBuffer::WriteSlot slot;
    if (!ring.tryAcquire(slot)) {
        return false;
    }
    REQUIRE(payload.size() <= slot.capacity);
    std::copy(payload.begin(), payload.end(), slot.data);
    ring.commit(payload.size());
    return true;
}

// consumer 측에서 슬롯 하나를 읽어 반환·해제
bool pop(SpscRingBuffer& ring, std::string& out) {
    SpscRingBuffer::ReadView slotView;
    if (!ring.tryPeek(slotView)) {
        return false;
    }
    out.assign(slotView.data, slotView.size);
    ring.release();
    return true;
}

}  // namespace

TEST_CASE("ring: slot count keeps the ring inside its memory budget") {
    using common::kDefaultSlotCount;
    using common::kMinSlotCount;
    using common::kRingBudgetBytes;
    using common::ringSlotCountFor;

    // 기본 청크(64KB)에서는 설계값 64슬롯이 그대로 나온다
    REQUIRE(ringSlotCountFor(64 * 1024) == kDefaultSlotCount);
    REQUIRE(ringSlotCountFor(64 * 1024) * 64 * 1024 == kRingBudgetBytes);

    // 청크를 키우면 슬롯 수가 줄어 예산을 넘지 않는다 (1MB x 64 = 64MB 사고 방지)
    for (const std::size_t slotSize :
         {32u * 1024, 64u * 1024, 128u * 1024, 256u * 1024, 1024u * 1024}) {
        const std::size_t slots = ringSlotCountFor(slotSize);
        INFO("slot size " << slotSize << " -> " << slots << " slots");
        REQUIRE(slots >= kMinSlotCount);
        REQUIRE(slots <= kDefaultSlotCount);
        REQUIRE(slots * slotSize <= kRingBudgetBytes);
    }
    REQUIRE(ringSlotCountFor(1024 * 1024) == 4);

    // 예산보다 큰 슬롯이라도 최소 개수는 보장한다 (완충 지대가 없으면 처리량이 죽는다)
    REQUIRE(ringSlotCountFor(8 * 1024 * 1024) == kMinSlotCount);
}

TEST_CASE("ring: acquire-commit-peek-release round trip") {
    SpscRingBuffer ring{4, 64};
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());

    REQUIRE(push(ring, "hello ring"));
    REQUIRE_FALSE(ring.empty());

    std::string out;
    REQUIRE(pop(ring, out));
    REQUIRE(out == "hello ring");
    REQUIRE(ring.empty());
}

TEST_CASE("ring: all slots usable, full stops producer, release reopens") {
    SpscRingBuffer ring{4, 64};
    // 단조 인덱스 방식이라 낭비 슬롯 없이 4칸 전부 사용
    for (int i = 0; i < 4; ++i) {
        REQUIRE(push(ring, "slot" + std::to_string(i)));
    }
    REQUIRE(ring.full());

    SpscRingBuffer::WriteSlot slot;
    REQUIRE_FALSE(ring.tryAcquire(slot));  // 꽉 참 — uv_read_stop 판단 지점

    std::string out;
    REQUIRE(pop(ring, out));
    REQUIRE(out == "slot0");  // FIFO
    REQUIRE_FALSE(ring.full());
    REQUIRE(push(ring, "slot4"));  // 한 칸 비우면 즉시 재개
}

TEST_CASE("ring: FIFO order across many wrap-arounds") {
    SpscRingBuffer ring{4, 64};
    int produced = 0;
    int consumed = 0;
    // 용량(4)의 몇 배를 흘려 랩어라운드를 강제
    while (consumed < 100) {
        while (produced - consumed < 3 && produced < 100) {
            REQUIRE(push(ring, "msg" + std::to_string(produced)));
            ++produced;
        }
        std::string out;
        REQUIRE(pop(ring, out));
        REQUIRE(out == "msg" + std::to_string(consumed));
        ++consumed;
    }
    REQUIRE(ring.empty());
}

TEST_CASE("ring: commit size is preserved per slot (variable nread)") {
    SpscRingBuffer ring{4, 64};
    // uv_read의 nread는 슬롯 용량보다 작게도 도착한다 — 크기가 그대로 보존돼야 함
    REQUIRE(push(ring, std::string(64, 'A')));  // 꽉 채운 슬롯
    REQUIRE(push(ring, "b"));                   // 1바이트
    REQUIRE(push(ring, ""));                    // 0바이트 커밋도 왕복 보존

    std::string out;
    REQUIRE(pop(ring, out));
    REQUIRE(out.size() == 64);
    REQUIRE(pop(ring, out));
    REQUIRE(out == "b");
    REQUIRE(pop(ring, out));
    REQUIRE(out.empty());
}

TEST_CASE("ring: acquired slot pointer is stable until commit (uv_alloc_cb contract)") {
    SpscRingBuffer ring{2, 32};
    SpscRingBuffer::WriteSlot slot;
    REQUIRE(ring.tryAcquire(slot));
    // uv_alloc_cb가 받은 포인터에 read_cb 시점까지 직접 기록하는 시나리오
    std::memcpy(slot.data, "deferred", 8);
    ring.commit(8);

    SpscRingBuffer::ReadView slotView;
    REQUIRE(ring.tryPeek(slotView));
    REQUIRE(std::string_view(slotView.data, slotView.size) == "deferred");
    REQUIRE(slotView.data == slot.data);  // 같은 메모리 — 복사 0회의 증거
}

TEST_CASE("ring: consumer drain loop empties the ring (session abort path)") {
    // 세션 중단 시 파서가 링을 비우며 폐기하는 경로 (design 추가 설계 3번)
    SpscRingBuffer ring{4, 64};
    REQUIRE(push(ring, "stale1"));
    REQUIRE(push(ring, "stale2"));

    SpscRingBuffer::ReadView slotView;
    while (ring.tryPeek(slotView)) {
        ring.release();  // 내용 무시 폐기
    }
    REQUIRE(ring.empty());
    REQUIRE(push(ring, "fresh"));  // 다음 세션 즉시 사용 가능
}

TEST_CASE("ring: two-thread stress — order and content survive contention") {
    // 실제 SPSC 배치: producer 1 + consumer 1. 작은 링으로 랩어라운드·경합 극대화
    constexpr int kMessages = 200'000;
    SpscRingBuffer ring{8, 16};

    std::thread producer([&ring] {
        for (int i = 0; i < kMessages;) {
            SpscRingBuffer::WriteSlot slot;
            if (!ring.tryAcquire(slot)) {
                std::this_thread::yield();  // 실전에선 uv_read_stop — 테스트는 스핀
                continue;
            }
            std::memcpy(slot.data, &i, sizeof(int));
            ring.commit(sizeof(int));
            ++i;
        }
    });

    int mismatches = 0;
    for (int expected = 0; expected < kMessages;) {
        SpscRingBuffer::ReadView slotView;
        if (!ring.tryPeek(slotView)) {
            std::this_thread::yield();
            continue;
        }
        int value = -1;
        std::memcpy(&value, slotView.data, sizeof(int));
        if (value != expected || slotView.size != sizeof(int)) {
            ++mismatches;
        }
        ring.release();
        ++expected;
    }
    producer.join();

    REQUIRE(mismatches == 0);
    REQUIRE(ring.empty());
}
