# client/: Windows 전용 GUI (Dear ImGui + Win32/DX11 — design.txt 5번)
set(IMGUI_DIR ${THIRDPARTY_DIR}/imgui)

# ImGui는 라이브러리로 빌드하지 않고 소스를 이 타깃에 직접 컴파일한다
# (근거는 3rdparty/imgui/build_window.bat 주석 — CRT 설정을 client와 함께 가져가기 위함).
set(IMGUI_SOURCES
    ${IMGUI_DIR}/src/imgui.cpp
    ${IMGUI_DIR}/src/imgui_draw.cpp
    ${IMGUI_DIR}/src/imgui_tables.cpp
    ${IMGUI_DIR}/src/imgui_widgets.cpp
    ${IMGUI_DIR}/src/imgui_impl_win32.cpp
    ${IMGUI_DIR}/src/imgui_impl_dx11.cpp
)

# WIN32: 콘솔 없는 GUI 서브시스템. 초기화 실패는 콘솔이 아니라 MessageBox로 알린다.
add_executable(client WIN32
    src/app/main.cpp
    src/app/Application.cpp
    src/service/FileReader.cpp
    src/service/ResultSummary.cpp
    src/service/TransferService.cpp
    src/ui/FileDialog.cpp
    src/ui/PingConsole.cpp
    src/ui/UiRenderer.cpp
    src/ui/UiState.cpp
    ${IMGUI_SOURCES}
)

# ImGui는 3rdparty라 컨벤션(/W4)의 적용 대상이 아니다 — 경고를 낮춰 우리 코드의 경고만 보이게 한다.
if(MSVC)
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W1")
endif()

target_include_directories(client PRIVATE
    src
    ${IMGUI_DIR}/include
)

# d3d11/dxgi: DX11 백엔드. common: 프로토콜·소켓·CRC·링버퍼(서버와 공유).
# 시스템 라이브러리는 common이 PUBLIC으로 전파한다 (common/CMakeLists.txt 참조).
target_link_libraries(client PRIVATE common d3d11 dxgi)

# uv.dll 복사 단계는 없다 — libuv를 정적으로 링크하므로 동반할 DLL 자체가 없다 (design 19번).
# 종전에는 여기서 uv.dll을 실행 파일 옆으로 복사했다. 복사를 빠뜨리면 0xC0000135
# (STATUS_DLL_NOT_FOUND)로 즉시 죽었고, 그 취약함이 정적 링크로 옮긴 이유 중 하나다.
