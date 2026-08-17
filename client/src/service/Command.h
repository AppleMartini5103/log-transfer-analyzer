#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace client {

// UI 스레드 -> 워커(uv 루프) 스레드로 넘기는 명령.
//
// design 7번: libuv API 중 타 스레드에서 안전한 것은 uv_async_send 하나뿐이라,
// UI에서 uv_tcp_connect/uv_write를 직접 부를 수 없다. 그래서 "무엇을 해달라"만 값으로
// 큐에 넣고 루프 스레드가 꺼내 실행한다.
//
// std::variant 값 타입으로 두는 이유는 컨벤션 1번(new/delete 금지) 때문이기도 하다 —
// 명령마다 힙 객체를 만들 이유가 없다.

struct ConnectCommand {
    std::string ip;
    std::uint16_t port = 0;
};

struct DisconnectCommand {};

// 종료도 같은 통로로 보낸다: UI 스레드에서 uv_stop을 직접 부르는 것보다
// 루프가 스스로 핸들을 닫고 uv_run을 반환하는 편이 정리 순서가 깔끔하다 (design 7번).
struct QuitCommand {};

using Command = std::variant<ConnectCommand, DisconnectCommand, QuitCommand>;

}  // namespace client
