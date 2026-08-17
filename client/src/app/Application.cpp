#include "app/Application.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "ui/FileDialog.h"
#include "util/Version.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"LogTransferAnalyzerClient";
constexpr wchar_t kWindowTitle[] = L"log-transfer-analyzer client";
constexpr int kDefaultWidth = 1000;
constexpr int kDefaultHeight = 700;

}  // namespace

// ImGui의 Win32 백엔드가 제공하는 메시지 핸들러 (헤더에 선언이 없어 직접 전방 선언한다).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace client {

Application::~Application() {
    // 초기화의 역순으로 정리한다 (design 10번). 워커를 먼저 세워야 루프 스레드가
    // 죽은 뒤에 창·디바이스가 사라진다 — 반대로 하면 콜백이 사라진 자원을 만진다.
    _worker.stop();

    if (_imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        _imguiInitialized = false;
    }
    releaseRenderTarget();
    // ComPtr 멤버는 소멸자에서 자동 Release — 수동 해제 없음 (컨벤션 1번).
    if (_hwnd != nullptr) {
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    if (_instance != nullptr) {
        ::UnregisterClassW(kWindowClassName, _instance);
    }
}

bool Application::initialize(HINSTANCE instance, std::string& error) {
    _instance = instance;

    if (!createWindow(instance, error)) {
        return false;
    }
    if (!createDeviceAndSwapChain(error)) {
        return false;
    }
    if (!createRenderTarget()) {
        error = "Failed to create the render target view.";
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(_hwnd) || !ImGui_ImplDX11_Init(_device.Get(), _deviceContext.Get())) {
        error = "Failed to initialize the Dear ImGui Win32/DX11 backend.";
        return false;
    }
    _imguiInitialized = true;
    _uiCallbacks = makeCallbacks();

    // 워커는 UI가 준비된 뒤 띄운다 — 초기화 순서는 "표시 수단 -> 스레드"여야
    // 워커의 첫 에러도 화면에 남는다 (design 10번 초기화 순서와 같은 원칙).
    if (!_worker.start(error)) {
        return false;
    }

    _uiState.logInfo(std::string(common::kProjectName) + " client started (built " +
                     std::string(common::buildDate()) + ")");

    ::ShowWindow(_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(_hwnd);
    return true;
}

// 지금은 상태 전이와 로그만 한다. 네트워크가 붙는 다음 이슈에서 각 콜백이
// 커맨드 큐 push + uv_async_send로 바뀐다 (design 7번: UI 스레드에서 uv_* 직접 호출 금지).
UiCallbacks Application::makeCallbacks() {
    UiCallbacks callbacks;

    callbacks.onConnect = [this] {
        std::uint16_t port = 0;
        if (!_uiState.parsePort(port)) {
            _uiState.logError("Port must be a number between 1 and 65535.");
            return;
        }
        const std::string ip(_uiState.serverIp.data());
        if (ip.empty()) {
            _uiState.logError("Server IP is empty.");
            return;
        }
        // 의도 상태만 여기서 바꾼다 — 실제 연결(LinkState)은 소켓 이벤트가 정한다
        // (design 12번: 게이팅은 의도를 따르고 인디케이터는 실제를 보여준다).
        _uiState.connectIntent = true;
        _worker.post(ConnectCommand{ip, port});
    };

    callbacks.onDisconnect = [this] {
        _uiState.connectIntent = false;
        _uiState.session = SessionState::Idle;
        _uiState.uploadProgress = 0.0f;
        _uiState.downloadProgress = 0.0f;
        _worker.post(DisconnectCommand{});
    };

    callbacks.onPing = [this] {
        const std::string ip(_uiState.serverIp.data());
        _uiState.logInfo("Ping requested" + (ip.empty() ? std::string{} : ": " + ip));
    };

    callbacks.onBrowse = [this] {
        SelectedFile selected;
        std::string error;
        if (!openFileDialog(_hwnd, selected, error)) {
            if (!error.empty()) {  // 비어 있으면 사용자가 취소한 것 — 로그를 남기지 않는다
                _uiState.logError(error);
            }
            return;
        }
        _uiState.filePath = selected.path;
        _uiState.fileName = selected.name;
        _uiState.fileSize = selected.size;
        _uiState.uploadProgress = 0.0f;
        _uiState.logInfo("Selected " + selected.name + " (" + std::to_string(selected.size) +
                         " bytes)");
    };

    callbacks.onSend = [this] {
        _uiState.logInfo("Send requested for " + _uiState.fileName +
                         " (transfer lands in the next issue)");
    };

    callbacks.onCancelUpload = [this] {
        _uiState.session = SessionState::Idle;
        _uiState.uploadProgress = 0.0f;
        _uiState.logWarn("Upload cancelled by user.");
    };

    callbacks.onSaveResult = [this] {
        std::string path;
        std::string error;
        if (!saveFileDialog(_hwnd, "result.csv", path, error)) {
            if (!error.empty()) {
                _uiState.logError(error);
            }
            return;
        }
        _uiState.logInfo("Result would be saved to " + path);
    };

    return callbacks;
}

bool Application::createWindow(HINSTANCE instance, std::string& error) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = &Application::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (::RegisterClassExW(&windowClass) == 0) {
        error = "Failed to register the window class.";
        return false;
    }

    // this 포인터를 CreateWindow의 lpParam으로 넘겨 WM_NCCREATE에서 창에 붙인다
    // (전역 변수 없이 windowProc에서 인스턴스를 찾기 위함).
    _hwnd = ::CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, kDefaultWidth, kDefaultHeight, nullptr, nullptr,
                              instance, this);
    if (_hwnd == nullptr) {
        error = "Failed to create the main window.";
        return false;
    }
    return true;
}

bool Application::createDeviceAndSwapChain(std::string& error) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = _hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    const HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested,
        static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION, &desc, &_swapChain, &_device,
        &obtained, &_deviceContext);
    if (FAILED(hr)) {
        error = "Failed to create the Direct3D 11 device and swap chain.";
        return false;
    }
    return true;
}

bool Application::createRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    return SUCCEEDED(_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &_renderTarget));
}

void Application::releaseRenderTarget() {
    _renderTarget.Reset();
}

void Application::resizeIfRequested() {
    if (_pendingWidth == 0 || _pendingHeight == 0) {
        return;
    }
    releaseRenderTarget();
    _swapChain->ResizeBuffers(0, _pendingWidth, _pendingHeight, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
    _pendingWidth = 0;
    _pendingHeight = 0;
}

// 워커 -> UI는 폴링으로만 흐른다. 워커가 UiState를 직접 만지면 ImGui가 그리는 도중에
// 상태가 바뀌어 한 프레임 안에서 화면이 어긋난다 (design 7번).
void Application::pumpWorkerEvents() {
    for (const TransferService::Event& event : _worker.drainEvents()) {
        if (event.hasLink) {
            _uiState.link = event.link;
        }
        if (!event.message.empty()) {
            _uiState.log(event.level, event.message);
        }
    }
}

void Application::renderFrame() {
    pumpWorkerEvents();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    _uiRenderer.render(_uiState, _uiCallbacks);

    ImGui::Render();

    constexpr float kClearColor[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    ID3D11RenderTargetView* target = _renderTarget.Get();
    _deviceContext->OMSetRenderTargets(1, &target, nullptr);
    _deviceContext->ClearRenderTargetView(target, kClearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // vsync 1: 유휴 시 CPU를 태우지 않는다 (500MB 전송 중 UI가 코어를 잡아먹지 않게).
    _swapChain->Present(1, 0);
}

void Application::run() {
    bool running = true;
    while (running) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }
        resizeIfRequested();
        renderFrame();
    }
}

LRESULT CALLBACK Application::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Application*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // ImGui가 먼저 입력을 가져간다 (텍스트 입력·마우스 캡처 등).
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0) {
        return 1;
    }

    switch (message) {
        case WM_SIZE:
            if (self != nullptr && wParam != SIZE_MINIMIZED) {
                self->_pendingWidth = LOWORD(lParam);
                self->_pendingHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) {  // Alt 키로 시스템 메뉴가 열리는 것 방지
                return 0;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace client
