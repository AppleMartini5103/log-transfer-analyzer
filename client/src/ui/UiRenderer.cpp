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

    // 진단 창은 메인 창 밖에 띄운다 — 사용자가 옮기고 닫을 수 있어야 하고,
    // 메인 화면 배치를 밀어내지 않아야 한다 (design 12번: 콘솔풍 별도 창)
    renderPingWindow(state);
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
    if (buttonGated("Save", state.canSaveResult())) {
        invoke(callbacks.onSaveResult);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("State: %s", sessionLabel(state.session));

    // 세션이 끝난 뒤 Save가 왜 아직 열려 있는지 화면이 스스로 설명한다.
    //
    // 왜 필요한가: 서버는 1:1 정책상 DownloadDone을 받는 즉시 연결을 닫는다(design 11번의
    // WAIT_DONE → CLEANUP — CLEANUP이 다음 연결을 받는 유일한 지점이라 닫지 않을 수 없다).
    // 그래서 완료 직후 인디케이터는 Disconnected가 되고, 화면에는 "끊겼는데 Save만 활성"인
    // 조합이 남는다. 동작은 옳지만(CSV는 이미 메모리에 있고 저장은 순수 로컬 작업) 이유를
    // 적어두지 않으면 사용자가 그 조합을 모순으로 읽는다 — design 12번의 "화면은 언제나 한
    // 가지 이야기만 한다" 원칙은 상태를 줄이는 것뿐 아니라 설명하는 것으로도 지켜진다.
    if (state.canSaveResult()) {
        ImGui::SameLine();
        ImGui::TextDisabled("- result.csv received; Save needs no connection");
    }
}

// ICMP 진단 출력 — ping.exe처럼 줄이 쌓이는 콘솔 모양 (design 12번).
//
// 왜 별도 창인가: 진단은 "연결이 안 될 때 원인을 가르는" 도구라 세션 로그와 함께 읽을 일이
// 많다. 메인 화면 안에 넣으면 Log 창을 밀어내고, 로그와 섞으면 둘 다 읽기 어려워진다.
void UiRenderer::renderPingWindow(UiState& state) {
    if (!state.pingWindowOpen) {
        return;
    }

    // 크기·위치는 첫 표시 때만 정하고 이후에는 사용자가 옮긴 자리를 존중한다(FirstUseEver).
    //
    // 위치를 Server/Transfer 줄 아래로 내려놓는 이유: 진단 중에 사용자가 다시 만지는 것은
    // IP 입력칸과 Ping 버튼이다. 그 위를 덮으면 주소를 바꿔 다시 시도할 때마다 창을 밀어야 한다.
    // 메인 창(클라이언트 영역 약 984x661) 안에 여유를 두고 들어가는 크기로 잡는다 —
    // ImGui 창은 부모 OS 창 경계를 넘지 못하므로(참고: verification_tool도 단일 OS 창 구조)
    // 너무 크게 잡으면 옮길 여지가 사라진다.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 180.0f, viewport->WorkPos.y + 220.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ping diagnostic", &state.pingWindowOpen)) {
        ImGui::End();  // 접혀 있어도 End는 짝을 맞춰야 한다
        return;
    }

    if (ImGui::Button("Clear")) {
        state.clearPingLines();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("network-layer reachability (Connect tests the TCP port)");

    ImGui::Separator();
    ImGui::BeginChild("ping-output", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : state.pingLines()) {
        ImGui::TextUnformatted(line.c_str());
    }
    // 새 줄이 들어온 프레임에만 맨 아래로 — 매 프레임 하면 사용자가 위로 스크롤할 수 없다
    if (state.pingDirty()) {
        ImGui::SetScrollHereY(1.0f);
        state.clearPingDirty();
    }
    ImGui::EndChild();
    ImGui::End();
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
