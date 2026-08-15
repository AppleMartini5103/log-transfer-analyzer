#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// 전송 계층 인터페이스 (design 6번 — verification_tool의 network 레이어 모범 사례를 따름).
// 순수 가상 ISocket → TcpSocket 구현 → 팩토리가 unique_ptr<ISocket> 반환:
// 구현 교체와 테스트 mock 주입이 쉬워진다.
//
// common/ 순수성: libuv + 표준 라이브러리만 (<windows.h>/<unistd.h> 금지 — 컨벤션 10번).
// 고정폭 정수만 사용 (long 금지 — Windows LLP64 vs Linux LP64).

namespace common::net {

// uv_alloc_cb에 내줄 수신 버퍼. 링버퍼 슬롯을 그대로 물리면 수신이 복사 0회가 된다
// (malloc 금지 규칙과 libuv alloc 요구의 화해 지점 — design 추가 설계 2번)
struct WritableBuffer {
    char* data = nullptr;  // 소유하지 않는 관찰자 — 수명은 버퍼 제공자가 보유
    std::size_t size = 0;
};

// 옵저버: 기본 구현이 빈 몸체라 필요한 이벤트만 오버라이드한다.
// on...Error / on...SendComplete / on...Closed의 구분이 "네트워크 견고성" 요구와 정확히 맞는다
// (에러·정상 종료·전송 완료를 각각 다른 경로로 처리해야 하므로).
class ISocketCallback {
public:
    virtual ~ISocketCallback() = default;

    // 수신 버퍼 대여. 빈 버퍼를 반환하면 libuv가 UV_ENOBUFS를 돌려주고 onError로 이어진다
    // → 읽기를 시작하려면 반드시 오버라이드할 것 (조용히 malloc하지 않는다)
    virtual WritableBuffer onAllocate(std::size_t /*suggestedSize*/) { return {}; }

    // data는 onAllocate가 내준 버퍼의 앞부분 — 콜백이 끝나면 유효성 보장 없음
    virtual void onRead(std::string_view /*data*/) {}
    virtual void onSendComplete(int /*status*/) {}
    // status는 libuv 에러 코드(음수). where는 발생 지점 표시용 문자열
    virtual void onError(int /*status*/, std::string_view /*where*/) {}
    // 핸들이 완전히 닫힌 시점 — 이 콜백 이후에야 소켓 객체를 파괴해도 안전하다
    virtual void onClosed() {}
    virtual void onConnect(int /*status*/) {}
};

class ISocket {
public:
    virtual ~ISocket() = default;

    // 콜백은 소유하지 않는다 (참조 관계) — 콜백 수명이 소켓보다 길어야 한다.
    // shared_ptr 보관은 순환 참조를 만들므로 금지 (design 6번 개선안)
    virtual void setCallback(ISocketCallback* callback) = 0;

    virtual int connect(const std::string& ip, std::uint16_t port) = 0;

    virtual int startRead() = 0;
    // backpressure: 링이 꽉 차면 수신을 멈춘다. 커널 버퍼가 차면 TCP 윈도우가 닫히고
    // 송신 측이 자연 감속한다 — 이것이 50MB 메모리 상한 장치 (design)
    virtual int stopRead() = 0;
    virtual bool isReading() const = 0;

    // 데이터는 내부 요청 버퍼로 복사된다 — 호출부가 수명을 관리하지 않아도 된다.
    // (design 6번 개선안대로 const char*+size 대신 string_view)
    virtual int send(std::string_view data) = 0;

    virtual void close() = 0;
    virtual bool isClosing() const = 0;

    // "127.0.0.1:54321" 형식. 실패 시 빈 문자열 — 로그 표시 전용
    virtual std::string peerAddress() const = 0;
};

}  // namespace common::net
