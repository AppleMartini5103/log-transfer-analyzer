#include "util/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace common {

namespace {

std::string_view levelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "Info";
        case LogLevel::Warn:
            return "Warn";
        case LogLevel::Error:
            return "Error";
    }
    return "Info";  // 도달 불가 — 컴파일러 경고 억제용
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;  // C++11 매직 스태틱 — 초기화 자체가 스레드 안전
    return logger;
}

bool Logger::openFile(const std::string& path) {
    const std::lock_guard<std::mutex> lock(_mutex);
    _file.open(path, std::ios::app);  // 재시작 시 이어쓰기 — 데몬 로그 관례
    if (!_file.is_open()) {
        return false;
    }
    _toFile = true;
    return true;
}

void Logger::close() {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_toFile) {
        _file.flush();
        _file.close();
        _toFile = false;
    }
}

void Logger::log(LogLevel level, std::string_view message) {
    // 타임스탬프 조립 — std::localtime은 스레드 불안전이라 뮤텍스 안에서만 사용
    // (localtime_r/_s는 POSIX/MSVC 확장 — common/ 순수성 때문에 표준 함수 + 락으로 해결)
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    const std::lock_guard<std::mutex> lock(_mutex);
    const std::tm* local = std::localtime(&now);
    char prefix[32];
    if (local == nullptr ||
        std::strftime(prefix, sizeof(prefix), "[%Y-%m-%d %H:%M:%S]", local) == 0) {
        prefix[0] = '\0';  // 시계 이상 시에도 메시지는 잃지 않는다
    }

    std::ostream& out = _toFile ? static_cast<std::ostream&>(_file) : std::cout;
    out << prefix << " [" << levelTag(level) << "] " << message << '\n';
    // 매 줄 플러시: 데몬이 크래시해도 직전 로그가 남아야 원인 추적 가능.
    // 로그는 핫 패스 금지 규칙(컨벤션 8번) 위라 플러시 비용은 무시 가능
    out.flush();
}

void Logger::flush() {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_toFile) {
        _file.flush();
    } else {
        std::cout.flush();
    }
}

}  // namespace common
