#pragma once

#include "app/ServerConfig.h"
#include "net/Timer.h"
#include "session/SessionManager.h"

#include <uv.h>

#include <memory>
#include <string>

// 서버 애플리케이션 — 객체 조립(wiring)과 메인 루프만 담당한다.
// (design 6번 반면교사: Application.h가 725줄 god class가 된 사례 — 상태 머신·세션은
//  독립 클래스로 두고 여기서는 소유·연결만 한다. 리스너·세션·파서 스레드는 다음 이슈에서 추가)
//
// [시그널을 루프 콜백으로 받는 이유 — design 10번]
//  원시 sigaction 핸들러 안에서는 async-signal-safe 함수만 호출할 수 있다(뮤텍스 잠금조차
//  불가). uv_signal은 시그널을 루프 스레드의 평범한 콜백으로 배달하므로 그 제약이 사라지고,
//  "모든 정리는 루프 스레드에서"(7번 원칙)와도 일관된다.
//  종료는 Quit 명령·소켓 에러·타임아웃과 같은 정리 경로로 수렴한다 (총괄 원칙 ③).

namespace server::app {

// listen 백로그 — 1:1 정책상 동시 수락은 1개지만, 대기 연결이 거절되지 않도록 여유를 둔다
inline constexpr int kListenBacklog = 128;

class ServerApp {
public:
    explicit ServerApp(ServerConfig config);
    ~ServerApp();

    // 핸들이 this를 back-pointer로 들고 있어 복사·이동하면 댕글링이 된다
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;
    ServerApp(ServerApp&&) = delete;
    ServerApp& operator=(ServerApp&&) = delete;

    // 루프 초기화 + 시그널 등록. 실패는 반환값 (컨벤션 3번)
    bool init(std::string& error);
    // uv_run — SIGTERM/SIGINT를 받을 때까지 블록. 반환값이 프로세스 종료 코드
    int run();

private:
    void onSignal(int signum);
    void shutdown();  // 종료는 초기화의 역순 (총괄 원칙 ④)

    // 로그 보관 기간 정리 (design 14번). 1시간 주기로 깨서 "지역시 03시이고 오늘 아직
    // 하지 않았으면" 정리한다.
    // → 왜 "다음 03시까지 남은 ms"를 계산해 재우지 않는가: libuv 타이머는 단조 시계 기반이라
    //   NTP 시각 점프·DST 전환에서 목표 시각이 어긋난다. 시각을 확인하는 방식은 자기 교정되고,
    //   1시간에 한 번 깨는 비용은 무시할 수준이다.
    void armPruneTimer();
    void onPruneTick();

    ServerConfig _config;
    uv_loop_t _loop{};
    std::unique_ptr<session::SessionManager> _sessions;
    uv_signal_t _sigterm{};
    uv_signal_t _sigint{};

    // 파일 싱크가 있을 때만 생성한다 — 포그라운드(stdout) 모드에는 정리할 파일이 없다
    std::unique_ptr<common::net::Timer> _pruneTimer;
    std::string _lastPruneDate;  // 같은 날 두 번 돌지 않도록 (YYYYMMDD)

    bool _loopReady = false;
    bool _shuttingDown = false;  // 종료 중 두 번째 시그널을 무시 (정리 경로 재진입 방지)
};

}  // namespace server::app
