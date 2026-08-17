#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>

#include "protocol/protocol.h"
#include "util/Logger.h"

namespace client {

// 화면이 그리는 데 필요한 상태 전부. UiRenderer는 이 구조를 읽기만 하고,
// 실제 동작(연결·전송)은 Application이 콜백으로 처리한다 (design 6번: UiState/UiRenderer 분리).
//
// 게이팅과 인디케이터는 모두 "실제 링크 상태" 하나를 따른다 (design 12번).
// 끊김은 곧 연결 해제로 취급하므로 화면이 언제나 한 가지 이야기만 한다:
//   회색이면 Connect만 열리고, 초록이면 Send·Disconnect가 열린다.
// 의도(intent)를 별도 축으로 두면 "끊겼다고 표시하면서 Connect는 잠긴" 상태가 생기고,
// 사용자가 어느 버튼을 눌러야 하는지 판단해야 한다 — 그 모호함을 없애는 것이 이 규칙이다.

// 실제 연결 상태 — 인디케이터 3단계
enum class LinkState : std::uint8_t {
    Disconnected,   // 회색
    Reconnecting,   // 노랑
    Connected,      // 초록
};

// 세션 진행 상태 (design 1번 클라이언트 상태 머신)
enum class SessionState : std::uint8_t {
    Idle,
    Connecting,
    SendingHeader,
    Streaming,
    WaitAck,
    WaitResult,
    ReceivingResult,
    Done,
};

struct LogEntry {
    common::LogLevel level = common::LogLevel::Info;
    std::string text;  // "[YYYY-MM-DD HH:MM:SS] [Info] ..." 완성형 (컨벤션 8번 포맷)
};

class UiState {
public:
    // ImGui InputText가 직접 쓰는 버퍼 (std::string은 크기 변경을 다루기 번거롭다)
    std::array<char, 64> serverIp{};
    std::array<char, 16> serverPort{};

    LinkState link = LinkState::Disconnected;
    SessionState session = SessionState::Idle;

    // 선택된 파일 (검증 통과분만 채운다 — 검증 실패 시 비워 Send를 잠근다)
    std::string filePath;
    std::string fileName;
    std::uint64_t fileSize = 0;

    float uploadProgress = 0.0f;    // 0.0 ~ 1.0
    float downloadProgress = 0.0f;

    UiState();

    // 로그는 UI 창과 파일 싱크 양쪽에 남긴다 — 화면은 사용자용, 파일은 사후 확인용.
    void log(common::LogLevel level, const std::string& message);
    void logInfo(const std::string& message) { log(common::LogLevel::Info, message); }
    void logWarn(const std::string& message) { log(common::LogLevel::Warn, message); }
    void logError(const std::string& message) { log(common::LogLevel::Error, message); }

    const std::deque<LogEntry>& logEntries() const { return _logEntries; }
    bool logDirty() const { return _logDirty; }
    void clearLogDirty() { _logDirty = false; }

    // 게이팅 판정 — design 12번 컨트롤 활성 매트릭스를 한곳에서만 계산한다.
    // 화면 여기저기에 조건을 흩뿌리면 상태가 늘 때 반드시 어긋난다.
    bool canEditAddress() const;
    bool canConnect() const;
    bool canDisconnect() const;
    bool canBrowse() const;
    bool canSend() const;
    bool canCancelUpload() const;
    bool canSaveResult() const;

    // 포트 문자열 → 숫자 (컨벤션 2번: from_chars만 사용). 실패 시 false.
    bool parsePort(std::uint16_t& port) const;

private:
    // 로그 창은 bounded — 장시간 켜둬도 메모리가 늘지 않는다 (컨벤션 1번 "모든 버퍼 bounded")
    static constexpr std::size_t kMaxLogEntries = 1000;

    std::deque<LogEntry> _logEntries;
    bool _logDirty = false;  // 새 줄이 들어오면 스크롤을 맨 아래로 내리기 위한 신호
};

}  // namespace client
