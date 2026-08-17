#pragma once

#include <cstddef>
#include <string>

// ICMP 진단 — OS의 ping 도구를 콘솔 창으로 띄운다 (design 12번).
//
// [왜 직접 구현하지 않는가 — 2026-08-17 전환]
//   처음에는 IcmpSendEcho로 직접 구현하고 결과를 앱 안의 ImGui 창에 그렸다. 그 방식은
//   ①회 사이 간격을 우리가 관리해야 하고(실제 ping은 1초 간격, 그게 4회를 보내는 의미를
//   만든다) ②ImGui 창은 부모 OS 창을 벗어날 수 없어 메인 창 안에 갇힌다.
//   참고 구현(verification_tool)은 ping.exe에 위임해 두 문제를 동시에 없앤다 — 간격은 ping이
//   처리하고, 콘솔은 진짜 별도 창이다. 우리가 소유할 코드도 140줄에서 이 파일 하나로 줄었다.
//
// [대가와 방어]
//   셸에 문자열을 넘기므로 주입 위험이 생긴다. 그래서 IP를 넘기기 전에 아래 isValidIpv4로
//   엄격히 검사한다 — 숫자와 점만 통과하므로 셸 메타문자가 살아남을 수 없다.
//   참고 구현은 문자 화이트리스트(영숫자 + . - : _)로 방어하는데, 우리는 주소 입력이 IPv4
//   전제(design 5번)이므로 더 좁게 잡는다. 좁은 검사가 방어에 유리하다.

namespace client {

// 엄격한 IPv4 점표기 검사 — 정확히 네 옥텟, 각 0~255, 숫자와 점 외의 문자는 불허.
// 헤더에 두는 이유: 셸 주입 방어의 핵심이라 단위 테스트로 고정해야 하고, 플랫폼 의존이 없다.
inline bool isValidIpv4(const std::string& text) {
    if (text.empty() || text.size() > 15) {  // "255.255.255.255" = 15자
        return false;
    }
    std::size_t pos = 0;
    for (int octet = 0; octet < 4; ++octet) {
        int value = 0;
        std::size_t digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
            if (digits > 3) {
                return false;
            }
        }
        if (digits == 0 || value > 255) {
            return false;
        }
        if (octet == 3) {
            break;
        }
        if (pos >= text.size() || text[pos] != '.') {
            return false;
        }
        ++pos;  // 점을 넘긴다
    }
    return pos == text.size();  // 뒤에 남는 문자가 있으면 거부
}

// ip를 대상으로 ping 콘솔을 띄운다. 실패 시 false + error (컨벤션 3번).
// 즉시 반환한다 — 프로세스 생성만 하고 결과를 기다리지 않으므로 UI 스레드에서 불러도 된다.
bool openPingConsole(const std::string& ip, std::string& error);

}  // namespace client
