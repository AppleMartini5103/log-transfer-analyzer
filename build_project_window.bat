@echo off
setlocal enabledelayedexpansion

REM Project build script (Windows) - README "Build" section entry point.
REM Runs from any prompt: the MSVC environment is entered automatically.
REM 0) makes sure the MSVC toolchain is available
REM 1) builds 3rdparty libraries from tarballs if missing
REM 2) configures + builds with CMake
REM 3) runs unit tests

cd /d "%~dp0"

REM Resolved outside the IF blocks below: %ProgramFiles(x86)% contains parentheses,
REM which are unsafe to expand inside a parenthesised block.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="

echo ========================================
echo log-transfer-analyzer Build (Windows)
echo ========================================

echo [0/3] Checking toolchain...
where cl >nul 2>nul
if not errorlevel 1 goto :toolchain_ready

REM cl.exe is absent, so this is a plain prompt rather than a developer one.
REM Locate the Visual Studio installation that carries the C++ toolset and enter
REM its x64 environment, instead of pushing that precondition onto the caller.
REM Delayed expansion (!VSWHERE!) is used deliberately: %VSWHERE% would be
REM substituted while the block is parsed, and the ")" inside "Program Files (x86)"
REM would close the IF block early - the class of failure fixed in commit [24].
REM Keep the vswhere call on one line as well; a caret continuation inside the
REM backticks makes cmd execute the fragments as separate commands.
if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if defined VSPATH (
    echo   cl.exe not in PATH - entering the x64 developer environment:
    echo     !VSPATH!
    REM 2>nul as well: vcvars64.bat itself prints "'vswhere.exe' is not recognized"
    REM on some installations (it probes PATH for optional components). That noise
    REM looks like a build error, and suppressing it is safe because the result is
    REM verified independently by the "where cl" check right below.
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
)

where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] MSVC compiler ^(cl.exe^) not found and no Visual Studio
    echo         installation with the C++ toolset could be located.
    echo         Install "Desktop development with C++" - VS 2019 16.4+ is
    echo         required ^(std::from_chars for floating point^) - or run this
    echo         script from "x64 Native Tools Command Prompt for VS".
    echo.
    exit /b 1
)

:toolchain_ready
for /f "delims=" %%c in ('where cl') do (
    echo   cl.exe: %%c
    goto :toolchain_reported
)
:toolchain_reported

REM cl.exe being present does not mean CMake is. vcvars64.bat does not put the
REM CMake that ships with Visual Studio on PATH unless the "C++ CMake tools for
REM Windows" component is installed, so a developer prompt can have a compiler and
REM no generator. Without this check the build reaches [2/3] and dies on cmd's bare
REM "'cmake' is not recognized", with 3rdparty already rebuilt for nothing.
REM 3rdparty\libuv\build_window.bat checks too, but step [1/3] skips it once
REM 3rdparty\libuv\include exists - so any second run would arrive unguarded.
where cmake >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] CMake not found in PATH. Version 3.16 or newer is required.
    echo         Install it with any one of:
    echo           winget install Kitware.CMake
    echo           choco install cmake
    echo         or tick "C++ CMake tools for Windows" in the Visual Studio
    echo         Installer ^(Individual components^), or download it from
    echo         https://cmake.org/download/
    echo.
    echo         Reopen the terminal afterwards so PATH is picked up.
    echo.
    exit /b 1
)
for /f "delims=" %%c in ('where cmake') do (
    echo   cmake.exe: %%c
    goto :cmake_reported
)
:cmake_reported

if not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
    echo [WARN] Only x64 has been verified; detected %PROCESSOR_ARCHITECTURE%. Continuing...
)

REM 하위 3rdparty 스크립트는 단독 실행(탐색기 더블클릭)을 위해 에러 경로마다
REM pause를 둔다. 여기서 부를 때는 사람이 지켜보고 있지 않으므로 그 대기가
REM "한 명령으로 끝난다"는 약속을 깨고, 비대화형 실행에서는 무한 대기가 된다.
REM 이 변수로 "부모가 부른 것"임을 알린다 — 자식의 setlocal은 이 시점의
REM 환경을 물려받으므로 그대로 보인다.
set "LTA_UNATTENDED=1"

echo [1/3] Checking 3rdparty libraries...
for %%L in (libuv catch2 imgui) do (
    if not exist "3rdparty\%%L\include" (
        echo   3rdparty\%%L not built yet - running its build script...
        pushd 3rdparty\%%L
        REM ".\" is required: with NoDefaultCurrentDirectoryInExePath set, cmd does
        REM not search the current directory and the call fails as "not recognized".
        call .\build_window.bat
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
