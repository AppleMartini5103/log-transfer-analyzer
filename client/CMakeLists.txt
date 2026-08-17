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
    src/service/TransferService.cpp
    src/ui/FileDialog.cpp
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
target_link_libraries(client PRIVATE common d3d11 dxgi)

# libuv는 DLL로 빌드되므로 실행 파일 옆에 uv.dll이 있어야 한다 (없으면 0xC0000135).
add_custom_command(TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THIRDPARTY_DIR}/libuv/lib/window/async/uv.dll"
            "$<TARGET_FILE_DIR:client>"
    COMMENT "Copying uv.dll next to client.exe")
