@echo off
setlocal enabledelayedexpansion

echo ========================================
echo   Auto-detect Libraries Script
echo ========================================
echo.

set "LIBS_DIR=%~dp0..\libs"
set "CMAKE_FILE=%~dp0..\CMakeLists.txt"

if not exist "%LIBS_DIR%" (
    echo libs directory not found.
    exit /b 1
)

echo Scanning libs directory...
echo.

set "LIBS_CONTENT="
set "FOUND_COUNT=0"

for /d %%d in ("%LIBS_DIR%\*") do (
    set "LIB_NAME=%%~nxd"
    set "LIB_PATH=%%d"
    
    if exist "!LIB_PATH!\CMakeLists.txt" (
        echo Found: !LIB_NAME!
        set /a FOUND_COUNT+=1
        
        if "!LIBS_CONTENT!"=="" (
            set "LIBS_CONTENT=!LIB_NAME!"
        ) else (
            set "LIBS_CONTENT=!LIBS_CONTENT!, !LIB_NAME!"
        )
    )
)

if !FOUND_COUNT! equ 0 (
    echo No libraries with CMakeLists.txt found.
    exit /b 0
)

echo.
echo Found !FOUND_COUNT! library(s): !LIBS_CONTENT!
echo.

set "TMP_FILE=%TEMP%\cmake_tmp_!.txt"
set "BLOCK_START=#### AUTO-Generated: libs start ####"
set "BLOCK_END=#### AUTO-Generated: libs end ####"

powershell -Command "(Get-Content '%CMAKE_FILE%') | Where-Object { $_ -notmatch '#### AUTO-Generated' } | Set-Content '%TMP_FILE%'"

(
    echo.
    echo %BLOCK_START%
    for /d %%d in ("%LIBS_DIR%\*") do (
        set "LIB_NAME=%%~nxd"
        if exist "%%d\CMakeLists.txt" (
            echo if^(EXISTS ${CMAKE_SOURCE_DIR}/libs/!LIB_NAME!/CMakeLists.txt^)
            echo     add_subdirectory^(libs/!LIB_NAME!^)
            echo endif^(^)
        )
    )
    echo %BLOCK_END%
) >> "%TMP_FILE%"

move /y "%TMP_FILE%" "%CMAKE_FILE%" >nul

echo.
echo ========================================
echo   Done! Libraries configured:
echo ========================================
echo !LIBS_CONTENT!
echo.
echo Please rebuild: scripts\build.bat
echo.
