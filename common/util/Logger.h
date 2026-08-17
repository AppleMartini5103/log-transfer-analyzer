#pragma once

#include <cstdint>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

// 로거 — 포맷·레벨은 컨벤션 8번 고정: "[YYYY-MM-DD HH:MM:SS] [레벨] 메시지"
// (client-ui.jpg Log 창과 동일 체계. 스킵 로그(skip_report.txt)는 파서 도메인 — 별개)
//
// 싱크 2모드: 기본 stdout(포그라운드) / openFile 후 파일(데몬 — stdout 소멸, design 10번).
// 사용 규칙(호출부 책임): 핫 패스(라인당 처리)에서 호출 금지 — 카운터만 올리고 요약은
// 주기/종료 시점에. 연결/세션 상태 변경은 반드시 남긴다 (컨벤션 8번).
//
// 스레드 안전: 루프 스레드·파서 스레드가 함께 쓰므로 내부 뮤텍스로 줄 단위 직렬화.
// 로그는 핫 패스가 아니라는 규칙 위에서 성립하는 설계 — 락 경합이 문제되면 규칙 위반이 먼저다.
//
// [파일 관리 — design 14번]
//  경로는 <baseDir>/logs/<YYYYMMDD>/<baseName>. 활성 파일 이름은 고정이고 회전본에만
//  _001부터 번호가 붙는다 → 근거: 활성 파일 이름이 고정이어야 tail -f / grep 대상이
//  예측 가능하다. 자정을 넘기면 새 날짜 디렉토리로 다시 연다 (데몬은 며칠 연속 돈다).
//  회전·정리 실패는 삼키고 로깅을 계속한다 → 근거: 로그 관리 실패보다 로그 유실이 더 나쁘다.

namespace common {

enum class LogLevel : std::uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2,
};

// 회전 임계 — 이 크기를 넘기려는 줄이 오면 먼저 회전한다
inline constexpr std::uintmax_t kMaxLogFileBytes = 10 * 1024 * 1024;
// 보관 일수 (오늘 포함). 이보다 오래된 날짜 디렉토리는 정리 대상
inline constexpr int kLogRetentionDays = 7;
// 서버의 주기 정리 시각 (지역시). 클라이언트는 시작 시에만 정리하므로 쓰지 않는다
// → 근거(design 14번): 클라이언트는 사용자가 열고 닫는 GUI라 새벽 3시에 켜져 있을 일이 없다
inline constexpr int kLogPruneHour = 3;

class Logger {
public:
    // 전역 단일 인스턴스 — main 초기화 순서(로거 → 루프 → 스레드, design 10번)에서
    // openFile 전 호출도 안전하게 stdout으로 나간다
    static Logger& instance();

    // 파일 싱크 전환 (데몬 모드). 실제 경로는 <baseDir>/logs/<오늘>/<baseName>.
    // 실패 시 false — 에러는 반환값 (컨벤션 3번), 종료 판단은 호출부 몫
    //
    // 인자를 baseDir/baseName으로 쪼갠 이유(design 14번): openFile("./server.log")가 내부적으로
    // ./logs/20260817/server.log를 만들면 호출부에서 경로를 읽을 수 없다. 서명에 의도가
    // 드러나야 하고, 테스트가 임시 디렉토리를 주입할 수 있어야 한다.
    bool openFile(const std::string& baseDir, const std::string& baseName);

    // 파일 싱크 닫기 — stdout으로 복귀 (종료 시퀀스·테스트용)
    void close();

    // 현재 열려 있는 파일의 실제 경로 (파일 싱크가 아니면 빈 문자열).
    // 날짜 디렉토리 때문에 호출부가 경로를 스스로 알 수 없어 필요하다 (클라 로그 안내 문구)
    std::string activeFilePath() const;

    // <baseDir>/logs 아래 YYYYMMDD 디렉토리 중 기준일로부터 keepDays일(오늘 포함) 밖에 있는
    // 것을 삭제하고 삭제한 개수를 반환한다. 활성 날짜 디렉토리는 절대 지우지 않는다.
    //
    // 기준 시각을 인자로 받는 이유(design 14번): 시스템 시계를 건드리지 않고 검증할 수 있어야
    // 한다. 과거 날짜 디렉토리를 만들어 두고 기준일을 주면 그대로 검증된다.
    std::size_t pruneOldLogs(std::time_t referenceTime, int keepDays = kLogRetentionDays);

    void log(LogLevel level, std::string_view message);
    void info(std::string_view message) { log(LogLevel::Info, message); }
    void warn(std::string_view message) { log(LogLevel::Warn, message); }
    void error(std::string_view message) { log(LogLevel::Error, message); }

    // 종료 시퀀스의 "로그 플러시" 단계 (design 10번) — 매 줄 플러시가 기본이라
    // 보통은 no-op에 가깝지만, 명시적 동기화 지점으로 제공
    void flush();

private:
    Logger() = default;

    // 아래 셋은 모두 _mutex를 잡은 상태에서만 호출한다
    bool openForDateLocked(const std::string& date);
    void rotateLocked();

    mutable std::mutex _mutex;
    std::ofstream _file;

    // _fileRequested = "파일 싱크를 쓰기로 했다" / _toFile = "지금 실제로 열려 있다".
    // 둘을 나눈 이유: 외부에서 logs/를 지우는 등으로 싱크가 끊겼을 때, 다음 기록 시점에
    // 스스로 다시 열어야 한다(design 14번 방어 생성). 한 플래그로는 "복구 대상"과
    // "애초에 stdout 모드"를 구분할 수 없다.
    bool _fileRequested = false;
    bool _toFile = false;

    std::string _baseDir;
    std::string _baseName;
    std::string _activeDate;  // 열려 있는 파일의 날짜 (YYYYMMDD) — 자정 롤오버 판정 기준
    std::string _activePath;

    // 회전 판정용 바이트 카운터. 파일을 열 때 기존 크기로 초기화한다 — 이어쓰기 모드라
    // 0에서 시작하면 9MB 파일을 다시 열었을 때 회전이 무한정 밀린다 (design 14번 함정)
    std::uintmax_t _bytesWritten = 0;
};

}  // namespace common
