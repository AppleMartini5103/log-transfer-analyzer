#include "net/Timer.h"

#include <utility>

namespace common::net {

Timer::Timer(uv_loop_t* loop) : _handle(std::make_unique<uv_timer_t>()) {
    if (uv_timer_init(loop, _handle.get()) != 0) {
        _handle.reset();
        return;
    }
    _handle->data = this;
}

Timer::~Timer() {
    if (!_handle || _closed) {
        return;
    }
    _handle->data = nullptr;  // 죽은 this로 돌아오는 통로 차단
    uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(_handle.release());
    if (uv_is_closing(raw)) {
        return;
    }
    uv_close(raw, onCloseCb);
}

int Timer::start(std::uint64_t timeoutMs, Callback callback) {
    if (!_handle) {
        return UV_EINVAL;
    }
    _callback = std::move(callback);
    _timeoutMs = timeoutMs;
    // repeat=0 — 한 번만 울린다. 타임아웃은 "이 상태에 머무를 수 있는 최대 시간"이라
    // 반복 발화가 의미 없다 (만료되면 세션이 CLEANUP으로 간다)
    return uv_timer_start(_handle.get(), onTimeoutCb, timeoutMs, 0);
}

void Timer::restart() {
    if (!_handle || !isActive()) {
        return;  // 멈춘 타이머를 활동만으로 되살리지 않는다 — 상태 전이가 명시적으로 걸어야 함
    }
    // repeat=0(1회성)이므로 uv_timer_again은 쓸 수 없다 — 같은 간격으로 다시 start
    uv_timer_start(_handle.get(), onTimeoutCb, _timeoutMs, 0);
}

int Timer::stop() {
    if (!_handle) {
        return 0;
    }
    return uv_timer_stop(_handle.get());
}

bool Timer::isActive() const {
    return _handle && uv_is_active(reinterpret_cast<const uv_handle_t*>(_handle.get())) != 0;
}

void Timer::onTimeoutCb(uv_timer_t* handle) {
    auto* self = static_cast<Timer*>(handle->data);
    if (self == nullptr || !self->_callback) {
        return;
    }
    // 콜백이 세션을 CLEANUP으로 보내며 이 타이머를 파괴할 수 있다 — 복사본으로 호출해
    // 콜백 실행 중 멤버가 사라져도 안전하게 한다
    Callback callback = self->_callback;
    callback();
}

void Timer::onCloseCb(uv_handle_t* handle) {
    auto* self = static_cast<Timer*>(handle->data);
    if (self == nullptr) {
        std::unique_ptr<uv_timer_t> owned{reinterpret_cast<uv_timer_t*>(handle)};
        return;
    }
    self->_closed = true;
}

}  // namespace common::net
