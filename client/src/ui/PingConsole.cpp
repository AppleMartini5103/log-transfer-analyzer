#include "ui/PingConsole.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

// 이 파일은 client 타깃에만 들어간다 (tests의 CLIENT_SOURCES에는 넣지 않는다) —
// 진단 실행은 Windows 전용이고, 테스트가 검증할 부분인 isValidIpv4는 헤더에 있어
// 이 번역 단위를 링크하지 않고도 테스트할 수 있다.

namespace client {

bool openPingConsole(const std::string& ip, std::string& error) {
    // 호출부에서 이미 검사하지만 여기서도 막는다 — 셸에 넘기는 마지막 지점이 이 함수이므로,
    // 방어를 호출부에만 두면 새 호출부가 생길 때 조용히 뚫린다 (컨벤션 3번의 방어 순서)
    if (!isValidIpv4(ip)) {
        error = "Ping: '" + ip + "' is not a valid IPv4 address.";
        return false;
    }

    // -t: 사용자가 닫을 때까지 계속 (참고 구현 verification_tool과 같은 형태).
    //     → 왜 4회가 아니라 연속인가: 연결이 안 될 때 Connect를 재시도하면서 동시에 링크를
    //       보는 것이 이 도구의 쓸모다. 4회로 끝나면 볼 때마다 다시 눌러야 하고, 전송 중
    //       패킷 유실이 생기는 순간도 놓친다. "완료까지 몇 초"라는 개념이 없어지므로
    //       타임아웃 길이가 사용자를 기다리게 하지도 않는다.
    // -w 1000: 회당 대기 1초. 안 주면 Windows 기본이 4초라 무응답 시 줄 간격이 4초가 되고,
    //          응답 시 1초와 어긋나 읽기 리듬이 들쭉날쭉해진다. LAN 실측 RTT 1ms의 1000배 여유.
    // /k: ping을 Ctrl+C로 멈춘 뒤에도 콘솔을 남긴다 → 통계를 읽을 시간을 주기 위함.
    //     /c로 하면 창이 즉시 사라져 진단 도구로 쓸 수 없다.
    //
    // ※ 이 프로세스는 우리가 소유하지 않는다 — 클라이언트를 종료해도 콘솔은 남고, 사용자가
    //   닫아야 한다. 별도 창을 띄우는 방식의 필연적 성질이며 참고 구현도 같다.
    const std::wstring parameters =
        L"/k ping -t -w 1000 " + std::wstring(ip.begin(), ip.end());  // 검사 통과분이라 ASCII 확정

    // ShellExecuteW의 반환값은 성공 시 32보다 큰 값이라는 레거시 규약을 따른다
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", L"cmd.exe", parameters.c_str(),
                                             nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        error = "Ping: could not start the ping console.";
        return false;
    }
    return true;
}

}  // namespace client
