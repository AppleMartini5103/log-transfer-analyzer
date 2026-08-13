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
