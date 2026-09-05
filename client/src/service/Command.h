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

// 업로드 시작. filename은 헤더에 실어 보낼 파일명(경로 아님 — design 8번),
// size는 파일 선택 시 확인한 크기로, 헤더의 fileSize와 스트림 길이가 이 값으로 일치해야 한다.
struct StartUploadCommand {
    std::string path;
    std::string filename;
    std::uint64_t size = 0;
};

// 사용자 취소. 소켓 에러·타임아웃과 같은 CLEANUP 경로로 수렴한다 (design 7번).
struct CancelUploadCommand {};

// 결과 재요청. 결과 수신이 끊긴 뒤, 같은 파일을 다시 올리지 않고 서버가 보관한 결과를
// 이어 받는다. 무엇을 청구하는지(파일명·크기·CRC)와 어디부터 받을지(오프셋)는 워커가
// 직전 업로드에서 이미 알고 있으므로 본문이 없다 — UI가 그 값을 들고 다닐 이유가 없다.
struct RequestResultCommand {};

// 종료도 같은 통로로 보낸다: UI 스레드에서 uv_stop을 직접 부르는 것보다
// 루프가 스스로 핸들을 닫고 uv_run을 반환하는 편이 정리 순서가 깔끔하다 (design 7번).
struct QuitCommand {};

using Command = std::variant<ConnectCommand, DisconnectCommand, StartUploadCommand,
                             CancelUploadCommand, RequestResultCommand, QuitCommand>;

}  // namespace client
