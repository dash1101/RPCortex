@echo off
REM RPCortex OS Compiler (Windows)
REM Compiles Core\**\*.py -> .mpy using mpy-cross and writes a deploy-ready
REM image to dist\ (or a custom directory).
REM
REM Usage:
REM   compile.bat                      -- defaults: armv6m, .\dist output
REM   compile.bat --arch armv7m        -- RP2350
REM   compile.bat --arch xtensawin     -- ESP32 / ESP32-S2 / ESP32-S3
REM   compile.bat --out C:\rpc_built
REM
REM Arch reference:
REM   armv6m    -- RP2040  (Pico, Pico W)
REM   armv7m    -- RP2350  (Pico 2, Pico 2 W)
REM   xtensawin -- ESP32 / ESP32-S2 / ESP32-S3
REM
REM mpy-cross install:
REM   pip install mpy-cross

setlocal enabledelayedexpansion

set "REPO_DIR=%~dp0"
REM Remove trailing backslash
if "%REPO_DIR:~-1%"=="\" set "REPO_DIR=%REPO_DIR:~0,-1%"

set "DIST_DIR=%REPO_DIR%\dist"
set "ARCH=armv6m"

REM ---------------------------------------------------------------------------
REM Argument parsing
REM ---------------------------------------------------------------------------
:parse_args
if "%~1"=="" goto :end_parse
if /i "%~1"=="--arch" (
    set "ARCH=%~2"
    shift & shift
    goto :parse_args
)
if /i "%~1"=="--out" (
    set "DIST_DIR=%~2"
    shift & shift
    goto :parse_args
)
if /i "%~1"=="--help" goto :show_help
if /i "%~1"=="-h"     goto :show_help
echo Unknown option: %~1  (run with --help for usage)
exit /b 1

:show_help
echo Usage: compile.bat [--arch ^<arch^>] [--out ^<dir^>]
echo   --arch  mpy-cross arch target (default: armv6m for RP2040)
echo           armv7m for RP2350, xtensawin for ESP32
echo   --out   output directory (default: dist\)
exit /b 0

:end_parse

REM ---------------------------------------------------------------------------
REM Prereq check
REM ---------------------------------------------------------------------------
where mpy-cross >nul 2>&1
if errorlevel 1 (
    echo.
    echo   [!] mpy-cross not found in PATH.
    echo       Install it with:  pip install mpy-cross
    echo.
    exit /b 1
)

echo.
echo   RPCortex OS Compiler
echo   Arch   : %ARCH%
echo   Output : %DIST_DIR%
echo.

REM ---------------------------------------------------------------------------
REM Clean and create output tree
REM ---------------------------------------------------------------------------
if exist "%DIST_DIR%" (
    rd /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%\Core\Launchpad"
if exist "%REPO_DIR%\Packages\Launchpad" mkdir "%DIST_DIR%\Packages\Launchpad"
if exist "%REPO_DIR%\Packages\Editor"    mkdir "%DIST_DIR%\Packages\Editor"

set compiled=0
set copied=0
set errors=0

REM ---------------------------------------------------------------------------
REM main.py — always source (MicroPython boots main.py, never main.mpy)
REM ---------------------------------------------------------------------------
echo   -- Entry point --
copy /y "%REPO_DIR%\main.py" "%DIST_DIR%\main.py" >nul
echo   [~] main.py                                               (source)
set /a copied+=1
echo.

REM ---------------------------------------------------------------------------
REM Core\*.py  (rpc_stub.py stays as source)
REM ---------------------------------------------------------------------------
echo   -- Core modules --
for %%f in ("%REPO_DIR%\Core\*.py") do (
    set "fname=%%~nxf"
    set "stem=%%~nf"
    if /i "%%~nxf"=="rpc_stub.py" (
        copy /y "%%f" "%DIST_DIR%\Core\%%~nxf" >nul
        echo   [~] Core\%%~nxf                                   (source)
        set /a copied+=1
    ) else (
        mpy-cross -march=%ARCH% -o "%DIST_DIR%\Core\%%~nf.mpy" "%%f"
        if errorlevel 1 (
            echo   [!] FAILED: Core\%%~nxf
            set /a errors+=1
        ) else (
            echo   [+] Core\%%~nxf
            set /a compiled+=1
        )
    )
)
echo.

REM ---------------------------------------------------------------------------
REM Core\Launchpad\*.py
REM ---------------------------------------------------------------------------
echo   -- Launchpad command files --
for %%f in ("%REPO_DIR%\Core\Launchpad\*.py") do (
    mpy-cross -march=%ARCH% -o "%DIST_DIR%\Core\Launchpad\%%~nf.mpy" "%%f"
    if errorlevel 1 (
        echo   [!] FAILED: Core\Launchpad\%%~nxf
        set /a errors+=1
    ) else (
        echo   [+] Core\Launchpad\%%~nxf
        set /a compiled+=1
    )
)
echo.

REM ---------------------------------------------------------------------------
REM Non-Python assets
REM ---------------------------------------------------------------------------
echo   -- Command registries and package configs --
for %%f in ("%REPO_DIR%\Core\Launchpad\*.lp") do (
    copy /y "%%f" "%DIST_DIR%\Core\Launchpad\" >nul
    echo   [~] Core\Launchpad\%%~nxf                             (source)
    set /a copied+=1
)

REM Package stubs
if exist "%REPO_DIR%\Packages\Launchpad" (
    for %%f in ("%REPO_DIR%\Packages\Launchpad\*") do (
        copy /y "%%f" "%DIST_DIR%\Packages\Launchpad\" >nul
        set /a copied+=1
    )
)
if exist "%REPO_DIR%\Packages\Editor" (
    for %%f in ("%REPO_DIR%\Packages\Editor\*") do (
        copy /y "%%f" "%DIST_DIR%\Packages\Editor\" >nul
        set /a copied+=1
    )
)

REM ---------------------------------------------------------------------------
REM Summary
REM ---------------------------------------------------------------------------
echo.
echo   -----------------------------------------
if !errors! gtr 0 (
    echo   [!] !errors! file(s) FAILED to compile -- check output above.
)
echo   Compiled  : !compiled! files
echo   Copied    : !copied!   files  (source -- not compiled)
echo   Output    : %DIST_DIR%
echo.

if !errors! gtr 0 exit /b 1
exit /b 0
