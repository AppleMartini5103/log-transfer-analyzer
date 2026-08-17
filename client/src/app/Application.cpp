#include "app/Application.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "ui/FileDialog.h"
#include "ui/PingConsole.h"
#include "util/Logger.h"
#include "util/Version.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"LogTransferAnalyzerClient";
constexpr wchar_t kWindowTitle[] = L"log-transfer-analyzer client";
constexpr int kDefaultWidth = 1000;
constexpr int kDefaultHeight = 700;

// 창을 놓을 위치와 크기.
struct WindowPlacement {
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int width = kDefaultWidth;
    int height = kDefaultHeight;
};

// 주 모니터 작업 영역의 중앙에 놓는다 (verification_tool의 GlfwWindowManager와 같은 정책).
//
// [주 모니터 기준으로 정한 근거]
//   커서가 있는 모니터를 쓰는 방식도 있으나, 채점자가 어디서 실행해도 같은 화면에 뜨는
//   예측 가능성을 택했다. 스크립트·IDE에서 실행하면 커서는 엉뚱한 곳에 있을 수 있다.
//
// [다중 모니터에서 실제로 깨지는 세 가지를 처리한다 — 참고 구현이 빠뜨린 부분]
//   ① 작업 영역(rcWork)을 쓴다. 모니터 전체(rcMonitor)로 계산하면 창 아래쪽이 작업표시줄에
//      가린다.
//   ② 모니터 원점을 더한다. 주 모니터가 가상 화면의 (0,0)이라는 보장이 없다 — 보조 모니터를
//      왼쪽·위에 두면 좌표가 음수 영역까지 퍼지고, 원점을 무시한 계산은 그만큼 어긋난다.
//   ③ 창이 작업 영역보다 크면 먼저 줄인다. 그냥 중앙 정렬하면 제목 표시줄이 화면 위로 밀려
//      마우스로 창을 되돌릴 수 없다 (1366x768 노트북 패널에서 실제로 걸리는 크기다).
//   ※ 실행 후의 모니터 구성 변경은 따라가지 않는다 — 시작 시 한 번만 배치한다. 계속
//     따라가면 사용자가 옮겨둔 창을 강제로 되돌리게 되어 오히려 방해가 된다.
WindowPlacement centeredOnPrimaryMonitor() {
    WindowPlacement placement;

    const HMONITOR monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || ::GetMonitorInfoW(monitor, &info) == 0) {
        return placement;  // 정보를 못 얻으면 OS 기본 배치에 맡긴다 (CW_USEDEFAULT)
    }

    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    if (workWidth <= 0 || workHeight <= 0) {
        return placement;
    }

    placement.width = std::min(kDefaultWidth, workWidth);
    placement.height = std::min(kDefaultHeight, workHeight);
    // 원점(rcWork.left/top)을 더한다 — 여기가 빠지면 주 모니터가 원점이 아닌 배치에서 어긋난다.
    // 결과가 음수일 수 있고 그것이 정상이다 (음수를 0으로 막으면 다중 모니터가 깨진다).
    placement.x = info.rcWork.left + (workWidth - placement.width) / 2;
    placement.y = info.rcWork.top + (workHeight - placement.height) / 2;
    return placement;
}

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

    // 마지막 단계: 로그를 내려쓰고 파일을 닫는다 (design 10번 종료 시퀀스의 "로그 플러시").
    // 워커·창이 정리되며 남긴 줄까지 파일에 들어가야 하므로 여기가 마지막이어야 한다.
    common::Logger::instance().flush();
    common::Logger::instance().close();
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

    // UI가 준비된 뒤에 연다 — 실패 사유를 화면에 띄울 수단이 있어야 하기 때문이다.
    openLogFile();

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

void Application::openLogFile() {
    // 실행 파일 옆에 쓴다 — 작업 디렉토리는 파일 다이얼로그 사용에 따라 바뀔 수 있어
    // 로그가 어디에 생겼는지 예측할 수 없게 된다 (OFN_NOCHANGEDIR로 막아도 보장은 아니다).
    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD length =
        ::GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        _uiState.logWarn("Cannot resolve the executable path - file logging is disabled.");
        return;
    }

    // 실제 파일은 <실행 파일 디렉토리>/logs/<YYYYMMDD>/client.log에 놓인다 (design 14번)
    const std::filesystem::path baseDir = std::filesystem::path{modulePath.data()}.parent_path();

    // 서버와 같은 관례로 이어쓰기다 (Logger::openFile이 ios::app) — 실행 이력이 누적돼
    // 채점 시 여러 번의 세션을 함께 확인할 수 있다.
    if (!common::Logger::instance().openFile(baseDir.string(), "client.log")) {
        _uiState.logWarn("Cannot open a log file under " + baseDir.string() +
                         "\\logs - file logging is disabled.");
        return;
    }
    _uiState.logInfo("Logging to " + common::Logger::instance().activeFilePath());

    // 보관 기간 정리는 시작 시 한 번만 한다 — 03시 타이머는 서버 몫이다 (design 14번:
    // 클라이언트는 사용자가 열고 닫는 GUI라 새벽 3시에 켜져 있을 일이 거의 없다)
    const std::size_t pruned = common::Logger::instance().pruneOldLogs(std::time(nullptr));
    if (pruned > 0) {
        _uiState.logInfo("Removed " + std::to_string(pruned) + " expired log folder(s)");
    }
}

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
        // 링크 상태는 소켓 이벤트만 정한다 — 여기서 미리 초록으로 바꾸지 않는다.
        // 화면이 실제보다 앞서 나가면 "연결됐다는데 왜 Send가 안 되지"가 된다.
        _worker.post(ConnectCommand{ip, port});
    };

    callbacks.onDisconnect = [this] {
        _uiState.session = SessionState::Idle;
        _uiState.uploadProgress = 0.0f;
        _uiState.downloadProgress = 0.0f;
        _worker.post(DisconnectCommand{});
    };

    callbacks.onPing = [this] {
        const std::string ip(_uiState.serverIp.data());
        if (ip.empty()) {
            // 조용히 무시하지 않는다 — 버튼이 고장난 것처럼 보인다 (컨벤션 8번)
            _uiState.logWarn("Ping: enter the server IP first.");
            return;
        }
        // 워커를 거치지 않는다 → 근거: 프로세스 생성은 즉시 반환하므로 블로킹이 아니고,
        // 커맨드 큐는 "블로킹·libuv 호출을 루프 스레드로 옮기기 위한" 통로다(design 7번).
        // 파일 다이얼로그(GetOpenFileNameW)도 같은 이유로 UI 스레드에서 직접 부른다.
        std::string error;
        if (!openPingConsole(ip, error)) {
            _uiState.logWarn(error);
            return;
        }
        _uiState.logInfo("Ping console opened for " + ip + " (continuous - close the window to stop)");
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
        if (_uiState.filePath.empty()) {
            _uiState.logError("Select a file first.");
            return;
        }

        // 시작 전에 한 번 묻는다 (design 88행 유지 항목). 500MB 전송은 되돌릴 수 없고,
        // Cancel을 눌러도 서버 세션 하나가 이미 소비된다 — 1:1이라 그 사이 다른 클라이언트는
        // 대기한다. 대상 주소까지 보여주는 이유는 주소 오입력도 같이 잡기 위함이다.
        const std::string destination =
            std::string(_uiState.serverIp.data()) + ":" + std::string(_uiState.serverPort.data());
        if (!confirmUpload(_hwnd, _uiState.fileName, _uiState.fileSize, destination)) {
            // 취소는 정상 조작이므로 Info로 남긴다 — 에러가 아니다
            _uiState.logInfo("Upload cancelled before it started.");
            return;
        }

        _uiState.uploadProgress = 0.0f;
        _worker.post(StartUploadCommand{_uiState.filePath, _uiState.fileName, _uiState.fileSize});
    };

    callbacks.onCancelUpload = [this] { _worker.post(CancelUploadCommand{}); };

    callbacks.onSaveResult = [this] {
        const std::string csv = _worker.takeResultCsv();
        if (csv.empty()) {
            _uiState.logError("No result to save yet.");
            return;
        }

        std::string path;
        std::string error;
        if (!saveFileDialog(_hwnd, "result.csv", path, error)) {
            if (!error.empty()) {
                _uiState.logError(error);
            }
            return;
        }

        // 텍스트 모드로 열면 Windows가 \n을 \r\n으로 바꿔 서버가 계산한 CRC와 어긋난다.
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            _uiState.logError("Cannot write to " + path);
            return;
        }
        out.write(csv.data(), static_cast<std::streamsize>(csv.size()));
        if (!out.good()) {
            _uiState.logError("Failed while writing " + path);
            return;
        }
        _uiState.logInfo("Saved " + std::to_string(csv.size()) + " bytes to " + path);
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

    // 위치를 만들기 전에 계산해 CreateWindow에 넘긴다 — 만든 뒤 SetWindowPos로 옮기면
    // 창이 한 번 나타났다 이동하는 것이 눈에 보인다.
    const WindowPlacement placement = centeredOnPrimaryMonitor();

    // this 포인터를 CreateWindow의 lpParam으로 넘겨 WM_NCCREATE에서 창에 붙인다
    // (전역 변수 없이 windowProc에서 인스턴스를 찾기 위함).
    _hwnd = ::CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, placement.x,
                              placement.y, placement.width, placement.height, nullptr, nullptr,
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
        if (event.hasSession) {
            _uiState.session = event.session;
        }
        if (event.hasUploadProgress) {
            _uiState.uploadProgress = event.uploadProgress;
        }
        if (event.hasDownloadProgress) {
            _uiState.downloadProgress = event.downloadProgress;
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
