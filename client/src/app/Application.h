#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#include "ui/UiRenderer.h"
#include "ui/UiState.h"

namespace client {

// Win32 창 + D3D11 디바이스 + ImGui 컨텍스트의 수명을 소유하고 메인 루프를 돌린다.
//
// design 6번 "Application은 객체 조립(wiring)과 메인 루프만 담당" 원칙을 따른다.
// 화면 내용은 앞으로 UiRenderer/UiState로 분리되며, 여기서는 프레임 경계
// (NewFrame ~ Present)와 창·디바이스 수명만 관리한다.
//
// 소유 자원은 전부 ComPtr(COM 참조 카운트 RAII)과 핸들 정리로 다뤄, 컨벤션 1번의
// "수동 해제 금지 / RAII" 규칙을 지킨다.
class Application {
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 실패 시 false를 반환하고 error에 사용자에게 보여줄 사유를 채운다
    // (GUI 서브시스템이라 콘솔 출력이 없다 — 호출자가 MessageBox로 표시).
    bool initialize(HINSTANCE instance, std::string& error);

    // 창이 닫힐 때까지 메시지 펌프 + 렌더 루프를 돈다.
    void run();

private:
    bool createWindow(HINSTANCE instance, std::string& error);
    bool createDeviceAndSwapChain(std::string& error);
    bool createRenderTarget();
    void releaseRenderTarget();
    void resizeIfRequested();
    void renderFrame();

    // UI 콜백 배선 — 아직 네트워크가 없으므로 상태 전이와 로그만 담당한다.
    // 다음 이슈에서 커맨드 큐 + uv_async_send로 루프 스레드에 넘기는 자리다 (design 7번).
    UiCallbacks makeCallbacks();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND _hwnd = nullptr;
    HINSTANCE _instance = nullptr;
    bool _imguiInitialized = false;

    UiState _uiState;
    UiRenderer _uiRenderer;
    UiCallbacks _uiCallbacks;

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> _deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> _swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _renderTarget;

    // WM_SIZE는 메시지 처리 중에 오므로 즉시 리사이즈하지 않고 프레임 시작 시점에 반영한다
    // (렌더 타깃을 그리는 도중에 놓아버리는 것을 피한다).
    UINT _pendingWidth = 0;
    UINT _pendingHeight = 0;
};

}  // namespace client
