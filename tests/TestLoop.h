#pragma once

#include <uv.h>

#include <cstdint>

// 테스트에서 uv 루프를 조건 만족까지 돌리는 공용 헬퍼.
//
// [왜 공용으로 두는가]
//   test_net / test_parser_thread / test_session이 각자 같은 함수를 복사해 갖고 있었고,
//   기본값이 2000 / 8000 / 5000으로 갈렸다. 그중 test_net 판본만 uv_sleep 보완이 빠져
//   있어서 "net: large payload arrives complete across many chunks"가 간헐적으로 실패했다.
//
// [왜 반복 횟수가 아니라 시간인가 — 실측 근거]
//   UV_RUN_NOWAIT은 처리할 이벤트가 없으면 즉시 반환한다. 그래서 반복 횟수는 대기 시간과
//   비례하지 않는다. 실제로 측정해 보니 유휴 루프에서 NOWAIT 20000회가 소비하는 시간은
//   6.5~7.8ms뿐인데, 1MB 루프백 전송은 4.3~16.3ms를 필요로 했다. 즉 "20000회"라는 넉넉해
//   보이는 상한이 실제로는 7ms짜리 상한이었고, 전송이 그보다 오래 걸리는 쪽이 절반이라
//   동전 던지기가 되었다. 조건이 "언제까지" 만족되어야 하는지를 다루는 문제이므로 기준은
//   벽시계 시간이어야 한다.
//
// [왜 uv_now가 아니라 uv_hrtime인가]
//   uv_now는 루프 반복 시작 시점에 캐시된 시간을 돌려준다. 함수 진입 시점에 그 값이 낡아
//   있으면 deadline이 이미 지나간 것으로 계산되어 조기 타임아웃이 난다 — 지금 고치는 결함과
//   같은 부류다. uv_hrtime은 루프 상태와 무관한 단조 시계라 그 함정이 없다.

namespace testsupport {

// 조건이 만족되면 true, timeoutMs가 지나도 만족되지 않으면 false.
template <typename Predicate>
bool runUntil(uv_loop_t* loop, Predicate predicate, std::uint64_t timeoutMs = 5000) {
    const std::uint64_t deadline = ::uv_hrtime() + timeoutMs * 1000000ULL;
    for (;;) {
        if (predicate()) {
            return true;
        }
        uv_run(loop, UV_RUN_NOWAIT);
        if (predicate()) {
            return true;
        }
        if (::uv_hrtime() >= deadline) {
            return false;
        }
        // 실제 시간을 흐르게 한다: NOWAIT만 반복하면 커널이 데이터를 올릴 틈도,
        // 타이머가 만료될 틈도 생기지 않는다 (타임아웃 테스트의 전제).
        uv_sleep(1);
    }
}

}  // namespace testsupport
