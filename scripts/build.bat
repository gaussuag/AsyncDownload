@echo off
setlocal enabledelayedexpansion

set "BUILD_DIR=build"
set "GENERATOR=Visual Studio 17 2022"

if "%1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Build directory cleaned.
    exit /b 0
)

if not defined VCPKG_ROOT (
    echo Error: VCPKG_ROOT environment variable not set.
    exit /b 1
)

set "CONFIG=Debug"
if "%1"=="release" (
    set "CONFIG=Release"
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "VCPKG_ARGS=-DVCPKG_ROOT=%VCPKG_ROOT% -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_MANIFEST_MODE=ON"

cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" %VCPKG_ARGS%
if errorlevel 1 (
    echo CMake configuration failed!
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build succeeded!
exit /b 0
