@echo off
setlocal enabledelayedexpansion

REM Dear ImGui deploy script (Windows client only).
REM
REM Unlike libuv/catch2, this does NOT build a library: it only lays out the
REM sources, which the client target compiles directly.
REM Rationale:
REM   - ImGui is designed to be compiled into the application (official examples
REM     do the same), and only 6 .cpp files are involved.
REM   - A static library would freeze the CRT choice (/MD vs /MT). Compiling the
REM     sources lets them follow whatever the client target uses, which matters
REM     if the submitted .exe is switched to static CRT linking.
REM
REM Output: include\ (headers) and src\ (.cpp to compile) - both gitignored.

set ARCHIVE=imgui-master.zip
set EXTRACT_DIR=tmp_extract
set SRC_ROOT=%EXTRACT_DIR%\imgui-master

echo ========================================
echo Dear ImGui Deploy Script (Windows)
echo ========================================
echo.

cd /d "%~dp0"

echo [0/3] Checking dependencies...
where tar >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] tar.exe not found ^(included in Windows 10 1803+^)
    echo.
    exit /b 1
)
if not exist "%ARCHIVE%" (
    echo [ERROR] %ARCHIVE% not found in %~dp0
    exit /b 1
)
echo   tar.exe: OK

echo [1/3] Extracting %ARCHIVE%...
if exist "%EXTRACT_DIR%" rmdir /s /q "%EXTRACT_DIR%"
mkdir "%EXTRACT_DIR%"
tar -xf "%ARCHIVE%" -C "%EXTRACT_DIR%"
if not exist "%SRC_ROOT%\imgui.cpp" (
    echo [ERROR] Extraction failed or unexpected archive layout.
    exit /b 1
)

echo [2/3] Deploying headers to include\ ...
if exist include rmdir /s /q include
mkdir include
copy /y "%SRC_ROOT%\imgui.h"          include\ >nul
copy /y "%SRC_ROOT%\imconfig.h"       include\ >nul
copy /y "%SRC_ROOT%\imgui_internal.h" include\ >nul
copy /y "%SRC_ROOT%\imstb_rectpack.h" include\ >nul
copy /y "%SRC_ROOT%\imstb_textedit.h" include\ >nul
copy /y "%SRC_ROOT%\imstb_truetype.h" include\ >nul
REM Backends: Win32 + DX11 only (design.txt section 5)
copy /y "%SRC_ROOT%\backends\imgui_impl_win32.h" include\ >nul
copy /y "%SRC_ROOT%\backends\imgui_impl_dx11.h"  include\ >nul

echo [3/3] Deploying sources to src\ ...
if exist src rmdir /s /q src
mkdir src
copy /y "%SRC_ROOT%\imgui.cpp"         src\ >nul
copy /y "%SRC_ROOT%\imgui_draw.cpp"    src\ >nul
copy /y "%SRC_ROOT%\imgui_tables.cpp"  src\ >nul
copy /y "%SRC_ROOT%\imgui_widgets.cpp" src\ >nul
copy /y "%SRC_ROOT%\backends\imgui_impl_win32.cpp" src\ >nul
copy /y "%SRC_ROOT%\backends\imgui_impl_dx11.cpp"  src\ >nul

rmdir /s /q "%EXTRACT_DIR%"

echo.
echo ========================================
echo [SUCCESS] Dear ImGui deployed
echo ========================================
echo   Headers: include\  ^(8 files^)
echo   Sources: src\      ^(6 files, compiled into the client target^)
echo.
endlocal
