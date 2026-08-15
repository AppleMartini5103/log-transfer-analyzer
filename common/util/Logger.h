#pragma once

#include <cstdint>
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

namespace common {

enum class LogLevel : std::uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2,
};

class Logger {
public:
    // 전역 단일 인스턴스 — main 초기화 순서(로거 → 루프 → 스레드, design 10번)에서
    // openFile 전 호출도 안전하게 stdout으로 나간다
    static Logger& instance();

    // 파일 싱크 전환 (데몬 모드). 실패 시 false — 에러는 반환값 (컨벤션 3번),
    // 종료 판단은 호출부 몫. 성공 시 이후 로그는 파일로만 기록
    bool openFile(const std::string& path);

    // 파일 싱크 닫기 — stdout으로 복귀 (종료 시퀀스·테스트용)
    void close();

    void log(LogLevel level, std::string_view message);
    void info(std::string_view message) { log(LogLevel::Info, message); }
    void warn(std::string_view message) { log(LogLevel::Warn, message); }
    void error(std::string_view message) { log(LogLevel::Error, message); }

    // 종료 시퀀스의 "로그 플러시" 단계 (design 10번) — 매 줄 플러시가 기본이라
    // 보통은 no-op에 가깝지만, 명시적 동기화 지점으로 제공
    void flush();

private:
    Logger() = default;

    std::mutex _mutex;
    std::ofstream _file;
    bool _toFile = false;
};

}  // namespace common
