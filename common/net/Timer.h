#pragma once

#include <uv.h>

#include <cstdint>
#include <functional>
#include <memory>

// uv_timer 래퍼 — 세션 타임아웃 전용 (design 9번).
//
// [설계 의도]
//  타이머는 "상대를 기다리는 상태"에만 건다. 자기 CPU 작업(VERIFYING/ANALYZING)에 걸면
//  느린 머신에서 스스로를 죽이는 서버가 된다. 상태→타임아웃 매핑은 한 곳(Session의 테이블)에
//  두고, 이 클래스는 "얼마 뒤에 한 번 울린다 / 활동이 있으면 처음부터 다시 잰다"만 제공한다.
//
// [스레드 규칙] 타이머도 루프 스레드 소유 — 타 스레드에서 start/stop 금지 (컨벤션 4번).
//
// 수명 규칙은 TcpSocket과 동일: uv_close가 비동기라 핸들 메모리를 별도 할당해 들고,
// 객체가 먼저 파괴되면 소유권을 close 콜백으로 넘긴다.

namespace common::net {

class Timer {
public:
    using Callback = std::function<void()>;

    explicit Timer(uv_loop_t* loop);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    // timeoutMs 뒤에 한 번 호출 (반복 없음). 이미 동작 중이면 처음부터 다시 잰다
    // → 무활동 타이머의 "활동마다 리셋"이 곧 이 재시작이다
    int start(std::uint64_t timeoutMs, Callback callback);
    // 활동 감지 시 호출 — 동작 중일 때만 같은 간격으로 재시작 (멈춰 있으면 아무 일 없음)
    void restart();
    int stop();
    bool isActive() const;

private:
    static void onTimeoutCb(uv_timer_t* handle);
    static void onCloseCb(uv_handle_t* handle);

    std::unique_ptr<uv_timer_t> _handle;
    Callback _callback;
    std::uint64_t _timeoutMs = 0;
    bool _closed = false;
};

}  // namespace common::net
