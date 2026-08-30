@echo off
setlocal enabledelayedexpansion

REM libuv Windows STATIC Build and Deploy Script
REM
REM Builds the static library (uv_a.lib) with the static CRT (/MT) so that the
REM submitted client.exe runs without uv.dll and without the VC++ redistributable.
REM Matching the CRT is mandatory: an /MT executable linked against an /MD library
REM puts two CRTs in one process with separate heaps. The linker usually stops this
REM with LNK2038, but any combination it does not stop corrupts memory silently the
REM moment one CRT frees what the other allocated.

echo ========================================
echo libuv 1.51.0 Static Build Script
echo (Windows Library Build)
echo ========================================
echo.

REM Save current script location
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

REM Check CMake installation
echo [0/5] Checking dependencies...
where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] CMake is not installed or not in PATH!
    echo.
    echo Please install CMake from one of these options:
    echo   1. Download from: https://cmake.org/download/
    echo   2. Install with winget: winget install Kitware.CMake
    echo   3. Install with chocolatey: choco install cmake
    echo.
    echo After installation, restart your terminal and run this script again.
    echo.
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

echo [SUCCESS] CMake found
echo.

REM Path settings - MODIFIED for lib\window\async structure
set TAR_GZ_FILE=libuv-v1.51.0.tar.gz
set TAR_FILE=libuv-v1.51.0.tar
set EXTRACT_DIR=libuv-v1.51.0
set LIB_OUTPUT_DIR=lib\window\async
set INCLUDE_OUTPUT_DIR=include

REM 1. Check tar.gz file
if not exist "%TAR_GZ_FILE%" (
    echo [ERROR] %TAR_GZ_FILE% not found!
    echo Please download libuv source from https://github.com/libuv/libuv/releases
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

echo [1/5] Extracting %TAR_GZ_FILE%...
REM Remove old extracted files
if exist "%EXTRACT_DIR%" (
    echo Removing old extracted files...
    rmdir /s /q "%EXTRACT_DIR%"
)
if exist "%TAR_FILE%" (
    del /q "%TAR_FILE%"
)

REM Extract using PowerShell (supports tar.gz)
echo Decompressing .tar.gz file...
powershell -Command "& {$inFile='%TAR_GZ_FILE%'; $outFile='%TAR_FILE%'; $input=New-Object System.IO.FileStream $inFile,([IO.FileMode]::Open),([IO.FileAccess]::Read),([IO.FileShare]::Read); $output=New-Object System.IO.FileStream $outFile,([IO.FileMode]::Create),([IO.FileAccess]::Write),([IO.FileShare]::None); $gzipStream=New-Object System.IO.Compression.GzipStream $input,([IO.Compression.CompressionMode]::Decompress); $buffer=New-Object byte[](1024); while(($read=$gzipStream.Read($buffer,0,1024)) -gt 0){$output.Write($buffer,0,$read)}; $gzipStream.Close(); $output.Close(); $input.Close()}"

if not exist "%TAR_FILE%" (
    echo [ERROR] Failed to decompress %TAR_GZ_FILE%
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

echo Extracting .tar file...
powershell -Command "tar -xf '%TAR_FILE%'"

if not exist "%EXTRACT_DIR%" (
    echo [ERROR] Failed to extract %TAR_FILE%
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

REM Clean up tar file
del /q "%TAR_FILE%"

echo [SUCCESS] Extraction completed
echo.

REM 2. Create output directories - MODIFIED to create lib\window\async
echo [2/5] Creating output directories...
if not exist "lib" mkdir "lib"
if not exist "lib\window" mkdir "lib\window"
if not exist "%LIB_OUTPUT_DIR%" mkdir "%LIB_OUTPUT_DIR%"
if not exist "%INCLUDE_OUTPUT_DIR%" mkdir "%INCLUDE_OUTPUT_DIR%"
echo Created: %LIB_OUTPUT_DIR%
echo Created: %INCLUDE_OUTPUT_DIR%
echo.

REM 3. Configure CMake for DLL build
echo [3/5] Configuring libuv build with CMake...
cd "%EXTRACT_DIR%"

if not exist build mkdir build
cd build

REM Detect available compiler/generator
set GENERATOR_FOUND=0
set CONFIG_SUCCESS=0

REM Method 1: Use NMake if Visual Studio is in PATH
where cl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [INFO] Detected Visual Studio compiler, using NMake Makefiles...
    cmake .. -G "NMake Makefiles" ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DLIBUV_BUILD_TESTS=OFF ^
        -DLIBUV_BUILD_BENCH=OFF
    if !ERRORLEVEL! EQU 0 (
        set GENERATOR_FOUND=1
        set CONFIG_SUCCESS=1
    )
)

REM Method 2: Try Visual Studio generators
if !GENERATOR_FOUND! EQU 0 (
    echo [INFO] Trying Visual Studio generators...

    REM Try VS 2022
    cmake .. -G "Visual Studio 17 2022" -A x64 ^
        -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DLIBUV_BUILD_TESTS=OFF ^
        -DLIBUV_BUILD_BENCH=OFF >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo [INFO] Using Visual Studio 17 2022
        set GENERATOR_FOUND=1
        set CONFIG_SUCCESS=1
        goto build_step
    )

    REM Try VS 2019
    cmake .. -G "Visual Studio 16 2019" -A x64 ^
        -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DLIBUV_BUILD_TESTS=OFF ^
        -DLIBUV_BUILD_BENCH=OFF >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo [INFO] Using Visual Studio 16 2019
        set GENERATOR_FOUND=1
        set CONFIG_SUCCESS=1
        goto build_step
    )

    REM Try VS 2017
    cmake .. -G "Visual Studio 15 2017 Win64" ^
        -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DLIBUV_BUILD_TESTS=OFF ^
        -DLIBUV_BUILD_BENCH=OFF >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo [INFO] Using Visual Studio 15 2017
        set GENERATOR_FOUND=1
        set CONFIG_SUCCESS=1
        goto build_step
    )

    REM Try VS 2015
    cmake .. -G "Visual Studio 14 2015 Win64" ^
        -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
        -DLIBUV_BUILD_TESTS=OFF ^
        -DLIBUV_BUILD_BENCH=OFF >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo [INFO] Using Visual Studio 14 2015
        set GENERATOR_FOUND=1
        set CONFIG_SUCCESS=1
        goto build_step
    )
)

REM Method 3: Try MinGW
if !GENERATOR_FOUND! EQU 0 (
    where mingw32-make >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo [INFO] Detected MinGW, using MinGW Makefiles...
        cmake .. -G "MinGW Makefiles" ^
            -DCMAKE_BUILD_TYPE=Release ^
            -DLIBUV_BUILD_SHARED=OFF ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
            -DLIBUV_BUILD_TESTS=OFF ^
            -DLIBUV_BUILD_BENCH=OFF
        if !ERRORLEVEL! EQU 0 (
            set GENERATOR_FOUND=1
            set CONFIG_SUCCESS=1
        )
    )
)

REM Handle configuration failure
if !CONFIG_SUCCESS! EQU 0 (
    echo.
    echo [ERROR] CMake configuration failed or no suitable build system found!
    echo.
    echo Please install one of the following:
    echo   1. Visual Studio Build Tools
    echo      Download: https://visualstudio.microsoft.com/downloads/
    echo      Install "Desktop development with C++" workload
    echo.
    echo   2. MinGW-w64
    echo      Download: https://www.mingw-w64.org/downloads/
    echo      Or install via: winget install mingw
    echo.
    echo   3. Run this script from "Developer Command Prompt for VS"
    echo      Start Menu -^> Visual Studio -^> Developer Command Prompt
    echo.
    cd ..\..
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

echo [SUCCESS] CMake configuration completed
echo.

:build_step
REM 4. Build
echo [4/5] Building libuv static library...
set BUILD_SUCCESS=0

REM Check if NMake
where cl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    where nmake >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        nmake
        if !ERRORLEVEL! EQU 0 (
            set BUILD_SUCCESS=1
        )
    )
)

REM Check if Visual Studio generator
if !BUILD_SUCCESS! EQU 0 (
    if exist "libuv.sln" (
        cmake --build . --config Release
        if !ERRORLEVEL! EQU 0 (
            set BUILD_SUCCESS=1
        )
    )
)

REM Check if MinGW
if !BUILD_SUCCESS! EQU 0 (
    where mingw32-make >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        mingw32-make
        if !ERRORLEVEL! EQU 0 (
            set BUILD_SUCCESS=1
        )
    )
)

if !BUILD_SUCCESS! EQU 0 (
    echo.
    echo [ERROR] Build failed!
    echo.
    cd ..\..
    if not defined LTA_UNATTENDED pause
    exit /b 1
)

echo [SUCCESS] Build completed
echo.
REM 5. Copy files

REM Delete artifacts left by the previous DLL build. Leaving them behind suggests
REM uv.dll is still required, and a stale copy sitting next to the executable would
REM keep working without anyone noticing the build no longer produces it.
if exist "..\..\%LIB_OUTPUT_DIR%\uv.dll" del /q "..\..\%LIB_OUTPUT_DIR%\uv.dll"
if exist "..\..\%LIB_OUTPUT_DIR%\uv.lib" del /q "..\..\%LIB_OUTPUT_DIR%\uv.lib"

echo [5/5] Copying files to output directories...

REM Copy DLL and LIB files for Visual Studio build
if exist "Release\uv.dll" (
    copy /Y "Release\uv.dll" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv.dll to %LIB_OUTPUT_DIR%
)
if exist "Release\uv.lib" (
    copy /Y "Release\uv.lib" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv.lib to %LIB_OUTPUT_DIR%
)
if exist "Release\uv_a.lib" (
    copy /Y "Release\uv_a.lib" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv_a.lib to %LIB_OUTPUT_DIR%
)

REM Copy for NMake/MinGW build (root build directory)
if exist "uv.dll" (
    copy /Y "uv.dll" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv.dll to %LIB_OUTPUT_DIR%
)
if exist "libuv.dll" (
    copy /Y "libuv.dll" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied libuv.dll to %LIB_OUTPUT_DIR%
)
if exist "uv.lib" (
    copy /Y "uv.lib" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv.lib to %LIB_OUTPUT_DIR%
)
REM libuv.lib is the actual static output on Windows: CMakeLists.txt sets
REM OUTPUT_NAME "uv" and, under if(WIN32), PREFIX "lib" - so it is neither
REM uv.lib nor uv_a.lib. The name is deterministic, not build-system dependent.
if exist "libuv.lib" (
    copy /Y "libuv.lib" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied libuv.lib to %LIB_OUTPUT_DIR%
)
if exist "uv_a.lib" (
    copy /Y "uv_a.lib" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied uv_a.lib to %LIB_OUTPUT_DIR%
)
if exist "libuv.a" (
    copy /Y "libuv.a" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied libuv.a to %LIB_OUTPUT_DIR%
)
if exist "libuv.dll.a" (
    copy /Y "libuv.dll.a" "..\..\%LIB_OUTPUT_DIR%\"
    echo Copied libuv.dll.a to %LIB_OUTPUT_DIR%
)

cd ..\..

REM Copy header files
echo.
echo Copying header files...
if not exist "%INCLUDE_OUTPUT_DIR%\uv" mkdir "%INCLUDE_OUTPUT_DIR%\uv"
copy /Y "%EXTRACT_DIR%\include\uv.h" "%INCLUDE_OUTPUT_DIR%\"
copy /Y "%EXTRACT_DIR%\include\uv\*.h" "%INCLUDE_OUTPUT_DIR%\uv\"

echo.
echo ========================================
echo [SUCCESS] libuv build completed!
echo ========================================
echo.
echo Usage in your project:
echo   1. Link against: %LIB_OUTPUT_DIR%\uv_a.lib (static)
echo   2. Include: #include ^<uv.h^>
echo   3. No DLL to ship - the library is linked into the executable
echo.
echo Cleaning up temporary files...
if exist "%EXTRACT_DIR%" rmdir /s /q "%EXTRACT_DIR%"
echo Done!
echo.

if not defined LTA_UNATTENDED pause
