@echo off
setlocal enabledelayedexpansion

echo ========================================
echo   C++ Project Configuration Script
echo ========================================
echo.

set "PROJECT_NAME=PlaygroundCpp"
set "VERSION=1.0.0"

echo Current project name: %PROJECT_NAME%
echo Current version: %VERSION%
echo.

set /p NEW_NAME="Enter new project name: "
if "%NEW_NAME%"=="" (
    echo Project name cannot be empty.
    exit /b 1
)

set /p NEW_VERSION="Enter version (default: %VERSION%): "
if "%NEW_VERSION%"=="" set "NEW_VERSION=%VERSION%"

echo.
echo Configuring project: %NEW_NAME% v%NEW_VERSION%
echo.

set "ROOT_CMAKE=CMakeLists.txt"
set "VCPKG_JSON=vcpkg.json"

if not exist "%ROOT_CMAKE%" (
    echo Error: CMakeLists.txt not found.
    exit /b 1
)

powershell -Command "(Get-Content '%ROOT_CMAKE%') -replace 'project\(PlaygroundCpp VERSION [0-9.]+ LANGUAGES CXX\)', 'project(%NEW_NAME% VERSION %NEW_VERSION% LANGUAGES CXX)' | Set-Content '%ROOT_CMAKE%'"

if errorlevel 1 (
    echo Failed to update CMakeLists.txt
    exit /b 1
)

if exist "%VCPKG_JSON%" (
    powershell -Command "$newName = '%NEW_NAME%'.ToLower().Replace(' ', '-').Replace('_', '-'); (Get-Content '%VCPKG_JSON%') -replace 'playground-cpp', $newName | Set-Content '%VCPKG_JSON%'"
    if errorlevel 1 (
        echo Warning: Failed to update vcpkg.json
    )
)

echo.
echo ========================================
echo   Configuration Complete!
echo ========================================
echo.
echo Project name: %NEW_NAME%
echo.
echo Next steps:
echo   1. Run: scripts\build.bat
echo   2. Or delete build/ and reconfigure manually
echo.
