#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ICMP echo 진단 (design 12번 Ping 버튼).
//
// [왜 별도 번역 단위인가]
//   구현은 Windows의 IcmpSendEcho 계열(iphlpapi/icmpapi)을 쓴다. 그 헤더를 호출부에 노출하면
//   TransferService.cpp가 windows.h를 끌어오게 되는데, 그 파일은 tests 타깃을 통해 Linux에서도
//   컴파일된다([37]에서 강제 단절 경로를 ASan으로 검증하려고 그렇게 구성했다). 그래서 선언만
//   이식 가능하게 두고, 플랫폼 코드는 .cpp 안에 가둔다.
//
// [왜 Ping이 필요한가 — 진단 층위 분리]
//   Ping = 네트워크 계층 도달성 / Connect = 앱 계층(TCP 포트) 도달성.
//   연결이 안 될 때 "네트워크가 죽었나 vs 서버 프로세스가 죽었나"를 가른다. 이 구분이 없으면
//   사용자는 Connect 실패 하나만 보고 원인을 추측해야 한다.
//
// [스레드] 이 함수는 블로킹이다. 루프 스레드에서 직접 부르면 UI가 멈춘다 —
//   반드시 uv_queue_work(libuv 스레드풀)에서 호출할 것 (컨벤션 4번, design 12번).

namespace client {

// 한 번의 echo 시도 결과 (콘솔풍 출력의 한 줄에 대응)
struct PingReply {
    bool success = false;
    std::uint32_t roundTripMs = 0;
    std::string detail;  // 실패 사유 또는 응답 주소
};

struct PingOutcome {
    bool supported = true;   // 이 플랫폼에서 ICMP 진단을 제공하는가
    std::string error;       // 시도조차 못 한 경우의 사유 (주소 파싱 실패 등)
    std::vector<PingReply> replies;
};

// 기본 4회 — design 12번 확정값 (ping 관례와 같고, 간헐적 손실을 한 번의 실패로
// 오판하지 않을 만큼의 표본)
inline constexpr int kDefaultPingAttempts = 4;

// ip로 ICMP echo를 attempts회 보낸다. 관리자 권한은 필요하지 않다.
// 블로킹 호출 — 위 스레드 규칙을 지킬 것.
PingOutcome icmpPing(const std::string& ip, int attempts = kDefaultPingAttempts);

}  // namespace client
