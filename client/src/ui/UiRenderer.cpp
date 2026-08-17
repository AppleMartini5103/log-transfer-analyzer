#include "ui/UiRenderer.h"

#include "imgui.h"

#include "ui/UiState.h"

namespace client {
namespace {

constexpr float kLabelWidth = 90.0f;
constexpr float kButtonWidth = 110.0f;

const ImVec4 kColorConnected{0.35f, 0.80f, 0.40f, 1.0f};
const ImVec4 kColorReconnecting{0.95f, 0.75f, 0.25f, 1.0f};
const ImVec4 kColorDisconnected{0.60f, 0.60f, 0.60f, 1.0f};
const ImVec4 kColorWarn{0.95f, 0.75f, 0.25f, 1.0f};
const ImVec4 kColorError{0.95f, 0.45f, 0.45f, 1.0f};

// 비활성 컨트롤은 숨기지 않고 흐리게 만든다 — 사라지면 "어디 갔지?"가 되고,
// 흐리게 두면 "지금은 못 누른다"가 전달된다 (design 12번의 게이팅 의도).
void beginDisabled(bool disabled) {
    if (disabled) {
        ImGui::BeginDisabled();
    }
}

void endDisabled(bool disabled) {
    if (disabled) {
        ImGui::EndDisabled();
    }
}

bool buttonGated(const char* label, bool enabled, float width = kButtonWidth) {
    beginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    endDisabled(!enabled);
    return pressed;
}

void invoke(const std::function<void()>& callback) {
    if (callback) {
        callback();
    }
}

const char* sessionLabel(SessionState session) {
    switch (session) {
        case SessionState::Connecting:       return "Connecting";
        case SessionState::SendingHeader:    return "Sending header";
        case SessionState::Streaming:        return "Uploading";
        case SessionState::WaitAck:          return "Waiting for ack";
        case SessionState::WaitResult:       return "Analyzing on server";
        case SessionState::ReceivingResult:  return "Receiving result";
        case SessionState::Done:             return "Done";
        case SessionState::Idle:
        default:                             return "Idle";
    }
}

std::string humanSize(std::uint64_t bytes) {
    constexpr double kMega = 1024.0 * 1024.0;
    char buffer[64];
    if (bytes >= static_cast<std::uint64_t>(kMega)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / kMega);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu bytes",
                      static_cast<unsigned long long>(bytes));
    }
    return std::string(buffer);
}

}  // namespace

void UiRenderer::render(UiState& state, const UiCallbacks& callbacks) {
    // 창 하나가 화면 전체를 채운다 — 목업(client-ui.jpg)과 같은 단일 화면 구성.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("main", nullptr, flags);

    renderServerRow(state, callbacks);
    ImGui::Separator();
    renderTransferRow(state, callbacks);
    ImGui::Separator();
    renderLogPanel(state);

    ImGui::End();
}

void UiRenderer::renderServerRow(UiState& state, const UiCallbacks& callbacks) {
    ImGui::TextUnformatted("Server");
    ImGui::Spacing();

    const bool addressEditable = state.canEditAddress();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("IP");
    ImGui::SameLine(kLabelWidth);
    beginDisabled(!addressEditable);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##ip", state.serverIp.data(), state.serverIp.size());
    ImGui::SameLine();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputText("##port", state.serverPort.data(), state.serverPort.size(),
                     ImGuiInputTextFlags_CharsDecimal);
    endDisabled(!addressEditable);

    ImGui::SameLine();
    if (buttonGated("Connect", state.canConnect())) {
        invoke(callbacks.onConnect);
    }
    ImGui::SameLine();
    if (buttonGated("Disconnect", state.canDisconnect())) {
        invoke(callbacks.onDisconnect);
    }
    ImGui::SameLine();
    // Ping은 항상 열려 있다 — 연결이 안 될 때 "네트워크가 죽었나 vs 서버가 죽었나"를
    // 가르는 진단 도구라, 연결 상태와 무관해야 쓸모가 있다 (design 12번).
    if (buttonGated("Ping", true, 70.0f)) {
        invoke(callbacks.onPing);
    }

    // 인디케이터는 의도가 아니라 실제 연결 상태를 보여준다 (design 12번의 두 축 분리).
    ImGui::SameLine();
    switch (state.link) {
        case LinkState::Connected:
            ImGui::TextColored(kColorConnected, "* Connected");
            break;
        case LinkState::Reconnecting:
            ImGui::TextColored(kColorReconnecting, "* Reconnecting...");
            break;
        case LinkState::Disconnected:
        default:
            ImGui::TextColored(kColorDisconnected, "* Disconnected");
            break;
    }
}

void UiRenderer::renderTransferRow(UiState& state, const UiCallbacks& callbacks) {
    ImGui::TextUnformatted("Transfer");
    ImGui::Spacing();

    // 파일 선택
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(400.0f);
    const char* shown = state.filePath.empty() ? "(no file selected)" : state.filePath.c_str();
    ImGui::TextUnformatted(shown);
    ImGui::SameLine();
    if (buttonGated("...", state.canBrowse(), 40.0f)) {
        invoke(callbacks.onBrowse);
    }
    if (!state.filePath.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", humanSize(state.fileSize).c_str());
    }

    // 업로드
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Send");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(400.0f);
    ImGui::ProgressBar(state.uploadProgress, ImVec2(400.0f, 0.0f));
    ImGui::SameLine();
    if (buttonGated("Send", state.canSend())) {
        invoke(callbacks.onSend);
    }
    ImGui::SameLine();
    if (buttonGated("Cancel", state.canCancelUpload())) {
        invoke(callbacks.onCancelUpload);
    }

    // 결과 다운로드 — CSV는 자동 수신되고, 이 버튼은 "저장"이다 (design 1번 C-1 확정).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Result");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(400.0f);
    ImGui::ProgressBar(state.downloadProgress, ImVec2(400.0f, 0.0f));
    ImGui::SameLine();
    if (buttonGated("Save...", state.canSaveResult())) {
        invoke(callbacks.onSaveResult);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("State: %s", sessionLabel(state.session));
}

void UiRenderer::renderLogPanel(UiState& state) {
    ImGui::TextUnformatted("Log");
    ImGui::Spacing();

    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const LogEntry& entry : state.logEntries()) {
        switch (entry.level) {
            case common::LogLevel::Warn:
                ImGui::TextColored(kColorWarn, "%s", entry.text.c_str());
                break;
            case common::LogLevel::Error:
                ImGui::TextColored(kColorError, "%s", entry.text.c_str());
                break;
            case common::LogLevel::Info:
            default:
                ImGui::TextUnformatted(entry.text.c_str());
                break;
        }
    }
    // 새 줄이 들어왔을 때만 맨 아래로 — 매 프레임 강제하면 사용자가 위로 스크롤할 수 없다.
    if (state.logDirty()) {
        ImGui::SetScrollHereY(1.0f);
        state.clearLogDirty();
    }
    ImGui::EndChild();
}

}  // namespace client
