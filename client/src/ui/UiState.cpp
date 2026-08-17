#include "ui/UiState.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace client {
namespace {

const char* levelTag(common::LogLevel level) {
    switch (level) {
        case common::LogLevel::Warn:
            return "Warn";
        case common::LogLevel::Error:
            return "Error";
        case common::LogLevel::Info:
        default:
            return "Info";
    }
}

// 컨벤션 8번 포맷: "[YYYY-MM-DD HH:MM:SS] [레벨] 메시지"
std::string formatEntry(common::LogLevel level, const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    ::localtime_s(&local, &seconds);
#else
    ::localtime_r(&seconds, &local);
#endif

    std::array<char, 32> stamp{};
    std::strftime(stamp.data(), stamp.size(), "%Y-%m-%d %H:%M:%S", &local);

    std::string line;
    line.reserve(stamp.size() + message.size() + 16);
    line += '[';
    line += stamp.data();
    line += "] [";
    line += levelTag(level);
    line += "] ";
    line += message;
    return line;
}

}  // namespace

UiState::UiState() {
    // 기본 주소는 비워두고 포트만 확정값으로 채운다 (design 5번: 기본 포트 23507)
    const std::string port = std::to_string(common::protocol::kDefaultPort);
    std::memcpy(serverPort.data(), port.c_str(), port.size() + 1);
}

void UiState::log(common::LogLevel level, const std::string& message) {
    _logEntries.push_back(LogEntry{level, formatEntry(level, message)});
    while (_logEntries.size() > kMaxLogEntries) {
        _logEntries.pop_front();
    }
    _logDirty = true;

    // 파일 싱크에도 같은 줄을 남긴다 — 화면 로그는 창을 닫으면 사라진다.
    common::Logger::instance().log(level, message);
}

bool UiState::parsePort(std::uint16_t& port) const {
    const char* begin = serverPort.data();
    const char* end = begin + std::strlen(begin);
    if (begin == end) {
        return false;
    }

    unsigned int value = 0;
    const auto result = std::from_chars(begin, end, value);
    // 전체 소비 + 범위 확인 (컨벤션 2번: ec만 보면 "8080abc"가 통과한다)
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

// ── 게이팅 (design 12번 매트릭스) ────────────────────────────────────────────
// 전송 중/결과 대기 중에는 세션을 흔드는 조작을 전부 잠근다. Disconnect만 열어두는데,
// 그것이 곧 취소 경로이기 때문이다 (design 7번: Cancel도 에러도 같은 CLEANUP으로 수렴).

namespace {

bool isBusy(SessionState session) {
    switch (session) {
        case SessionState::Connecting:
        case SessionState::SendingHeader:
        case SessionState::Streaming:
        case SessionState::WaitAck:
        case SessionState::WaitResult:
        case SessionState::ReceivingResult:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool UiState::canEditAddress() const {
    return !connectIntent && !isBusy(session);
}

bool UiState::canConnect() const {
    return !connectIntent && !isBusy(session);
}

bool UiState::canDisconnect() const {
    return connectIntent;
}

bool UiState::canBrowse() const {
    // 파일 선택은 로컬 작업이라 연결과 무관하게 열어둔다 (design 12번 근거:
    // 권장 흐름이 "파일 → Connect → 즉시 Send"가 되어 서버 대기 시간이 사라진다).
    return !isBusy(session);
}

bool UiState::canSend() const {
    return connectIntent && !isBusy(session) && !filePath.empty();
}

bool UiState::canCancelUpload() const {
    return session == SessionState::SendingHeader || session == SessionState::Streaming;
}

bool UiState::canSaveResult() const {
    return session == SessionState::Done;
}

}  // namespace client
