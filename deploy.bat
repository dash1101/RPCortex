@echo off
:: RPCortex Deploy Script (Windows)
:: Copies OS files to a connected MicroPython device using mpremote.
:: Can deploy from the source tree (default) or a compiled dist\ image.
::
:: Usage:
::   deploy.bat                         -- deploy source from repo root
::   deploy.bat --compiled              -- deploy compiled dist\ image
::   deploy.bat --compiled --out C:\path\dist  -- deploy a custom dist dir
::   deploy.bat --port COM3             -- specify serial port explicitly
::
:: mpremote install:
::   pip install mpremote
::
:: Notes:
::   Run compile.bat first if deploying --compiled.
::   \Nebula\ and \Users\ on the device are never touched -- user data is safe.

setlocal enabledelayedexpansion

set "REPO_DIR=%~dp0"
if "%REPO_DIR:~-1%"=="\" set "REPO_DIR=%REPO_DIR:~0,-1%"

set "DIST_DIR=%REPO_DIR%\dist"
set "PORT_ARG="
set "COMPILED=0"

:: ---------------------------------------------------------------------------
:: Argument parsing
:: ---------------------------------------------------------------------------
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--compiled" (
    set "COMPILED=1"
    shift & goto parse_args
)
if /i "%~1"=="--out" (
    set "DIST_DIR=%~2"
    shift & shift & goto parse_args
)
if /i "%~1"=="--port" (
    set "PORT_ARG=connect %~2"
    shift & shift & goto parse_args
)
if /i "%~1"=="--help" (
    findstr /b "::" "%~f0"
    exit /b 0
)
echo Unknown option: %~1  (run with --help for usage)
exit /b 1

:args_done

:: ---------------------------------------------------------------------------
:: Prereq check
:: ---------------------------------------------------------------------------
where mpremote >nul 2>&1
if errorlevel 1 (
    echo.
    echo   [!] mpremote not found in PATH.
    echo       Install it with:  pip install mpremote
    echo.
    exit /b 1
)

if "%COMPILED%"=="1" (
    if not exist "%DIST_DIR%" (
        echo   [!] Compiled dist not found: %DIST_DIR%
        echo       Run compile.bat first, or use --out to specify a path.
        exit /b 1
    )
    set "SRC_DIR=%DIST_DIR%"
    echo.
    echo   Deploying COMPILED image from: %DIST_DIR%
) else (
    set "SRC_DIR=%REPO_DIR%"
    echo.
    echo   Deploying SOURCE from: %REPO_DIR%
)

if "%PORT_ARG%"=="" (
    echo   Port: auto-detect
) else (
    echo   Port: %PORT_ARG%
)
echo.

set "MPR=mpremote %PORT_ARG%"

:: ---------------------------------------------------------------------------
:: Copy the whole OS tree in ONE mpremote session.
::
:: The old script ran a separate `mpremote cp` per file. Each invocation pays
:: the full connect + raw-REPL handshake (~1-2s), so 30+ files took a minute+.
:: `cp -r <dir> :` recurses and creates the directory on the device; chaining
:: commands with `+` keeps everything in a SINGLE raw-REPL session -- a huge
:: speedup. It also copies ALL package dirs (PicoFetch, PulseMark, NTP, ...),
:: which the per-file version missed.
::
:: Only Core\, Packages\, and main.py are touched -- \Nebula\ and \Users\
:: (user accounts, WiFi, settings) are never sent, so they're left intact.
:: ---------------------------------------------------------------------------
echo   -- Copying OS in one session: Core\, Packages\, main.py --
echo      (user data under \Nebula\ and \Users\ is left untouched)
echo.

%MPR% cp -r "%SRC_DIR%\Core" : + cp -r "%SRC_DIR%\Packages" : + cp "%SRC_DIR%\main.py" :
set "RC=%errorlevel%"

echo.
if not "%RC%"=="0" (
    echo   [!] Deploy failed ^(exit %RC%^).
    echo       - Is the board plugged in? Try:  deploy.bat --port COM3
    echo       - Close any other serial program ^(PuTTY/Thonny^) using the port.
    exit /b 1
)

echo   -------------------------------------------
echo   Deploy complete. Reboot to apply:  %MPR% reset
echo.
exit /b 0
