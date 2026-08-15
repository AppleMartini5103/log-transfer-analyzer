#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// bounded lock-free SPSC 링버퍼 (design "공통 골격" — 클라/서버 동일 자료구조).
//
// 소유권 모델: 슬롯은 producer → consumer 로 단방향 이동 (뮤텍스·참조 카운트 없음).
//   producer(1스레드):  tryAcquire() → 슬롯 버퍼에 직접 기록 → commit(실제 크기)
//   consumer(1스레드):  tryPeek() → 처리 → release()
// tryAcquire가 내주는 생포인터를 uv_alloc_cb에 그대로 물리면 수신 = 복사 0회
// (malloc 금지 규칙과 libuv alloc 요구의 화해 지점 — design 추가 설계 2번).
//
// 계약 (어기면 버그 — SPSC이므로 검사 비용을 넣지 않는다):
//   - producer 연산(tryAcquire/commit/full)과 consumer 연산(tryPeek/release/empty)은
//     각각 정확히 한 스레드에서만 호출
//   - acquire 없이 commit 금지, peek 없이 release 금지, 미결 acquire는 최대 1개
//   - 여기가 컨벤션 4번 "락 없는 공유 금지"의 유일한 예외 (내부 atomic 인덱스만 공유)
//
// 깨우기/backpressure는 이 클래스 밖의 일: full()이면 호출부가 uv_read_stop,
// 비워지면 consumer가 uv_async_send — 링은 상태만 보고한다 (design 7번 참고 절).

namespace common {

// 64슬롯 × 64KB ≈ 4MB — 50MB 예산의 8%. 링은 처리량과 무관한 완충 지대라 작게 잡는다.
// 64KB 청크는 시스템 콜 오버헤드가 수렴하는 실용 기본값 (둘 다 벤치 스윕으로 재확정 예정 — design)
inline constexpr std::size_t kDefaultSlotCount = 64;
inline constexpr std::size_t kDefaultSlotSize = 64 * 1024;

// 링 전체가 쓸 메모리 예산 (design 메모리 예산표의 "링버퍼 4MB").
// ★ 슬롯 수를 64로 고정하면 chunk_size를 키울 때 링이 예산을 뚫는다:
//   2026-08-16 벤치 스윕에서 chunk_size=1MB × 64슬롯 = 64MB가 되어 peak RSS 69.4MB로
//   과제의 50MB 제약을 위반했다. 처리량은 32KB~1MB에서 평평했으므로 큰 청크는
//   이득 없이 메모리만 먹는다 — 예산을 고정하고 슬롯 수를 줄이는 쪽이 옳다
inline constexpr std::size_t kRingBudgetBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMinSlotCount = 4;  // 완충 지대로서 최소한의 여유

// 슬롯 크기에 맞춰 예산 안에 들어가는 슬롯 수를 계산한다
std::size_t ringSlotCountFor(std::size_t slotSize);

class SpscRingBuffer {
public:
    struct WriteSlot {
        char* data = nullptr;  // 소유하지 않는 관찰자 — 링이 수명 보유 (컨벤션 1번)
        std::size_t capacity = 0;
    };

    struct ReadView {
        const char* data = nullptr;
        std::size_t size = 0;
    };

    explicit SpscRingBuffer(std::size_t slotCount = kDefaultSlotCount,
                            std::size_t slotSize = kDefaultSlotSize);

    // ── producer 전용 ──────────────────────────────────────────────────────
    // 빈 슬롯의 버퍼를 빌려줌. 링이 꽉 찼으면 false (→ 호출부는 uv_read_stop 판단)
    bool tryAcquire(WriteSlot& out);
    // 빌린 슬롯에 실제 기록된 바이트 수로 발행 — 이 순간 소유권이 consumer로 이동
    void commit(std::size_t size);
    bool full() const;

    // ── consumer 전용 ──────────────────────────────────────────────────────
    // 가장 오래된 미소비 슬롯을 보여줌. 없으면 false
    bool tryPeek(ReadView& out) const;
    // 소비 완료 — 슬롯이 빈 슬롯 풀로 복귀 (backpressure 해제 신호는 호출부 몫)
    void release();
    bool empty() const;

    std::size_t slotCount() const { return _slotCount; }
    std::size_t slotSize() const { return _slotSize; }

private:
    static constexpr std::size_t kCacheLine = 64;  // head/tail 위조 공유(false sharing) 방지

    const std::size_t _slotCount;
    const std::size_t _slotSize;
    std::vector<char> _storage;        // slotCount × slotSize 연속 배치, 생성 시 1회 확보
    std::vector<std::size_t> _sizes;   // 슬롯별 커밋 크기

    // 단조 증가 인덱스 (mod slotCount로 슬롯 결정) — 64슬롯 전부 사용 가능
    alignas(kCacheLine) std::atomic<std::uint64_t> _head{0};  // consumer가 전진
    alignas(kCacheLine) std::atomic<std::uint64_t> _tail{0};  // producer가 전진
};

}  // namespace common
