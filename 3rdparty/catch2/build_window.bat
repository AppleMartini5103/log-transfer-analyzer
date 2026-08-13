@echo off
setlocal enabledelayedexpansion

REM Catch2 Windows Static Library Build and Deploy Script
REM Run from "x64 Native Tools Command Prompt for VS" (cl.exe / lib.exe required)

set CATCH2_VERSION=3.15.3

echo ========================================
echo Catch2 v%CATCH2_VERSION% Static Library Build Script
echo (Windows MSVC Build)
echo ========================================
echo.

cd /d "%~dp0"

REM Check dependencies
echo [0/4] Checking dependencies...

where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] cl.exe not found!
    echo.
    echo Please run this script from:
    echo   "x64 Native Tools Command Prompt for VS 2019/2022"
    echo.
    exit /b 1
)
echo   cl.exe found.

where tar >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] tar.exe not found! (included in Windows 10 1803+)
    echo.
    exit /b 1
)

set TARBALL=Catch2-v%CATCH2_VERSION%.tar.gz
if not exist "%TARBALL%" (
    echo [ERROR] %TARBALL% not found in %~dp0
    exit /b 1
)

REM Extract tarball
echo [1/4] Extracting %TARBALL%...
if exist tmp_build rmdir /s /q tmp_build
mkdir tmp_build
tar -xzf "%TARBALL%" -C tmp_build

set SRC_DIR=tmp_build\Catch2-%CATCH2_VERSION%\extras

REM Deploy header
echo [2/4] Deploying amalgamated header to include\...
if not exist include mkdir include
copy /y "%SRC_DIR%\catch_amalgamated.hpp" include\ >nul

REM Compile static library
echo [3/4] Compiling static library (C++17)...
if not exist lib\window mkdir lib\window
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /MD /c "%SRC_DIR%\catch_amalgamated.cpp" ^
    /I include /Fo:tmp_build\catch_amalgamated.obj
if errorlevel 1 (
    echo [ERROR] Compilation failed!
    exit /b 1
)
lib /nologo /out:lib\window\catch2.lib tmp_build\catch_amalgamated.obj
if errorlevel 1 (
    echo [ERROR] lib.exe failed!
    exit /b 1
)

REM Cleanup
echo [4/4] Cleaning up temporary files...
rmdir /s /q tmp_build

echo.
echo ========================================
echo [SUCCESS] Catch2 build completed!
echo ========================================
echo.
echo Output files:
echo   Header:  include\catch_amalgamated.hpp
echo   Library: lib\window\catch2.lib
echo.
echo Usage in your project:
echo   1. Include: #include ^<catch_amalgamated.hpp^>  (/I include)
echo   2. Link:    catch2.lib (default main() is provided)
echo.
echo Done!
endlocal
