@echo off
setlocal enabledelayedexpansion

REM Project build script (Windows) — README "Build" section entry point.
REM Run from "x64 Native Tools Command Prompt for VS" (cl.exe / cmake required)
REM 1) builds 3rdparty libraries from tarballs if missing
REM 2) configures + builds with CMake
REM 3) runs unit tests

cd /d "%~dp0"

echo ========================================
echo log-transfer-analyzer Build (Windows)
echo ========================================

REM 환경 가드: 이 스크립트는 개발자 명령 프롬프트에서 실행해야 한다.
REM 일반 프롬프트에서는 cl.exe가 PATH에 없어 3rdparty 빌드가 엉뚱하게 실패한다.
where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] MSVC compiler ^(cl.exe^) not found in PATH.
    echo         Run this script from:
    echo           "x64 Native Tools Command Prompt for VS 2022"
    echo         Requires MSVC 2019 16.4+ ^(std::from_chars for floating point^).
    echo.
    exit /b 1
)

if not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
    echo [WARN] Only x64 has been verified; detected %PROCESSOR_ARCHITECTURE%. Continuing...
)

echo [1/3] Checking 3rdparty libraries...
for %%L in (libuv catch2) do (
    if not exist "3rdparty\%%L\include" (
        echo   3rdparty\%%L not built yet — running its build script...
        pushd 3rdparty\%%L
        call build_window.bat
        if errorlevel 1 (
            echo [ERROR] 3rdparty\%%L build failed!
            exit /b 1
        )
        popd
    ) else (
        echo   3rdparty\%%L: OK
    )
)

echo [2/3] Configuring and building (CMake, Release)...
cmake -B build
if errorlevel 1 exit /b 1
cmake --build build --config Release
if errorlevel 1 exit /b 1

echo [3/3] Running unit tests...
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo ========================================
echo [SUCCESS] Build and tests completed!
echo ========================================
endlocal
