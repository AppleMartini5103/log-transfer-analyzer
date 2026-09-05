#pragma once

#include <functional>

namespace client {

class UiState;

// 화면이 사용자 조작을 Application에 전달하는 통로.
//
// UiRenderer가 직접 소켓을 만지지 않는 이유는 스레드 규칙 때문이다 (design 7번):
// UI 스레드에서 uv_* 호출은 금지이고, 명령은 커맨드 큐를 거쳐 루프 스레드로 가야 한다.
// 그 큐잉을 Application이 담당하도록 여기서는 "무엇을 눌렀는지"만 알린다.
struct UiCallbacks {
    std::function<void()> onConnect;
    std::function<void()> onDisconnect;
    std::function<void()> onPing;
    std::function<void()> onBrowse;
    std::function<void()> onSend;
    std::function<void()> onCancelUpload;
    std::function<void()> onRequestResult;
    std::function<void()> onSaveResult;
};

// UiState를 읽어 한 프레임을 그린다. 상태를 바꾸지 않는다 — 조작은 전부 콜백으로 나간다.
// (design 6번: UiState(상태)와 UiRenderer(그리기) 분리)
class UiRenderer {
public:
    void render(UiState& state, const UiCallbacks& callbacks);

private:
    void renderServerRow(UiState& state, const UiCallbacks& callbacks);
    void renderTransferRow(UiState& state, const UiCallbacks& callbacks);
    void renderLogPanel(UiState& state);
};

}  // namespace client
