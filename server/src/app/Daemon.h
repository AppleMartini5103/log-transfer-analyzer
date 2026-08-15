#pragma once

#include <string>

// 데몬화 + 프로세스 수준 시그널 기본값 (design 10번 확정).
//
// [순서 고정 — fork 함정]
//   인자 파싱 → 데몬화(-d) → 로거 초기화 → uv_loop 초기화 → 스레드 생성 → 실행
//   fork는 호출 스레드만 복제한다. 루프·스레드를 만든 뒤 데몬화하면 자식 프로세스에는
//   잠긴 뮤텍스만 남고 그 뮤텍스를 풀어줄 스레드는 없어 데드락이 된다. 반드시 fork가 먼저.
//
// POSIX 헤더는 여기(server/)에서만 쓴다 — common/은 libuv + 표준 라이브러리만 (컨벤션 10번).

namespace server::app {

inline constexpr const char* kPidFilePath = "./server.pid";

// main() 시작 직후 호출. 특수 채널(시그널)을 일반 경로로 전환하는 첫 단계 (총괄 원칙 ①)
//  - SIGPIPE → SIG_IGN: 끊긴 소켓에 write하면 기본 동작이 프로세스 즉사다. 무시하면
//    write 실패가 errno EPIPE로 돌아와 설계된 소켓 에러 → CLEANUP 경로에 자연 흡수된다.
//    libuv가 내부적으로 처리하지만 라이브러리 내부 동작에 의존하지 않고 명시한다.
//  - SIGHUP → SIG_IGN: 설정 리로드 기능이 없으므로 무시 (기본 동작은 프로세스 종료).
//    포그라운드 실행 중 터미널을 닫아도 서버가 죽지 않는다.
//    ※ SIGTERM/SIGINT는 여기서 다루지 않는다 — uv_signal_t로 루프 스레드에서 받는다
void installProcessSignalDefaults();

// 클래식 double-fork 데몬화. 성공 시 자식(손자) 프로세스만 반환하고 부모들은 exit한다.
// chdir("/")는 의도적으로 생략 — result.csv·로그의 상대 경로 예측 가능성이 더 중요하다
// (관례 이탈이므로 README에 명기 예정).
// 실패 시 false + error (컨벤션 3번: 반환값으로 처리)
bool daemonize(std::string& error);

// PID 파일 기록 — 채점자가 kill $(cat server.pid)로 종료할 수 있게 한다
bool writePidFile(const std::string& path, std::string& error);
void removePidFile(const std::string& path);

}  // namespace server::app
