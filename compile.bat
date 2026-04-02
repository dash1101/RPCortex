@echo off
:: RPCortex OS Compiler (Windows)
:: Compiles Core\**\*.py -> .mpy using mpy-cross and writes a deploy-ready
:: image to dist\ (or a custom directory).
::
:: Usage:
::   compile.bat                       -- defaults: armv6m arch, .\dist output
::   compile.bat --arch armv7m         -- RP2350
::   compile.bat --arch xtensawin      -- ESP32
::   compile.bat --out C:\tmp\rpc_built
::
:: Arch reference:
::   armv6m    -- RP2040  (Pico, Pico W)
::   armv7m    -- RP2350  (Pico 2, Pico 2 W)
::   xtensawin -- ESP32 / ESP32-S2 / ESP32-S3
::
:: mpy-cross install:
::   pip install mpy-cross

setlocal enabledelayedexpansion

set "REPO_DIR=%~dp0"
:: Remove trailing backslash
if "%REPO_DIR:~-1%"=="\" set "REPO_DIR=%REPO_DIR:~0,-1%"

set "DIST_DIR=%REPO_DIR%\dist"
set "ARCH=armv6m"

:: ---------------------------------------------------------------------------
:: Argument parsing
:: ---------------------------------------------------------------------------
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--arch" (
    set "ARCH=%~2"
    shift & shift
    goto parse_args
)
if /i "%~1"=="--out" (
    set "DIST_DIR=%~2"
    shift & shift
    goto parse_args
)
if /i "%~1"=="--help" (
    goto show_help
)
echo Unknown option: %~1  (run with --help for usage)
exit /b 1

:show_help
findstr /b "::" "%~f0" | findstr /v "^::$" | for /f "tokens=* delims=:" %%L in ('more') do echo %%L
exit /b 0

:args_done

:: ---------------------------------------------------------------------------
:: Prereq check
:: ---------------------------------------------------------------------------
where mpy-cross >nul 2>&1
if errorlevel 1 (
    echo.
    echo   [!] mpy-cross not found in PATH.
    echo       Install it with:  pip install mpy-cross
    echo       Or download from: https://pypi.org/project/mpy-cross/
    echo.
    exit /b 1
)

for /f "tokens=* usebackq" %%V in (`mpy-cross --version 2^>^&1`) do set "MPY_VER=%%V"

echo.
echo   +===========================================+
echo   ^|    RPCortex OS Compiler (Windows)        ^|
echo   +===========================================+
echo.
echo   mpy-cross : %MPY_VER%
echo   Arch      : %ARCH%
echo   Output    : %DIST_DIR%
echo.

:: ---------------------------------------------------------------------------
:: Clean and create output tree
:: ---------------------------------------------------------------------------
if exist "%DIST_DIR%" rd /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%\Core\Launchpad"
mkdir "%DIST_DIR%\Packages\Launchpad"
mkdir "%DIST_DIR%\Packages\Editor"

:: ---------------------------------------------------------------------------
:: Counters
:: ---------------------------------------------------------------------------
set /a compiled=0
set /a copied=0
set /a errors=0

:: ---------------------------------------------------------------------------
:: Helper: compile_file  %1=full_src_path  %2=relative_path
:: ---------------------------------------------------------------------------
goto :skip_subs

:compile_file
    set "src=%~1"
    set "rel=%~2"
    :: derive dest dir and stem
    for %%F in ("%src%") do set "stem=%%~nF"
    :: rel path without filename -> subdir under DIST_DIR
    for %%F in ("%DIST_DIR%\%rel%") do set "dst_dir=%%~dpF"
    set "dst_mpy=%dst_dir%%stem%.mpy"
    if not exist "%dst_dir%" mkdir "%dst_dir%"

    mpy-cross -march=%ARCH% -o "%dst_mpy%" "%src%" >nul 2>&1
    if errorlevel 1 (
        echo   [!] FAILED: %rel%
        set /a errors+=1
    ) else (
        echo   [+] %rel%
        set /a compiled+=1
    )
    exit /b

:copy_file
    set "src=%~1"
    set "rel=%~2"
    for %%F in ("%DIST_DIR%\%rel%") do set "dst_dir=%%~dpF"
    if not exist "%dst_dir%" mkdir "%dst_dir%"
    copy /y "%src%" "%DIST_DIR%\%rel%" >nul
    echo   [~] %rel%  (source)
    set /a copied+=1
    exit /b

:skip_subs

:: ---------------------------------------------------------------------------
:: main.py — always source
:: ---------------------------------------------------------------------------
echo   -- Entry point --
call :copy_file "%REPO_DIR%\main.py" "main.py"
echo.

:: ---------------------------------------------------------------------------
:: Core\*.py
:: ---------------------------------------------------------------------------
echo   -- Core modules --
for %%F in ("%REPO_DIR%\Core\*.py") do (
    set "fname=%%~nxF"
    set "rel=Core\%%~nxF"
    if /i "%%~nxF"=="rpc_stub.py" (
        call :copy_file "%%F" "Core\%%~nxF"
    ) else (
        call :compile_file "%%F" "Core\%%~nxF"
    )
)
echo.

:: ---------------------------------------------------------------------------
:: Core\Launchpad\*.py
:: ---------------------------------------------------------------------------
echo   -- Launchpad command files --
for %%F in ("%REPO_DIR%\Core\Launchpad\*.py") do (
    call :compile_file "%%F" "Core\Launchpad\%%~nxF"
)
echo.

:: ---------------------------------------------------------------------------
:: Non-Python assets
:: ---------------------------------------------------------------------------
echo   -- Command registries ^& package configs --
for %%F in ("%REPO_DIR%\Core\Launchpad\*.lp") do (
    call :copy_file "%%F" "Core\Launchpad\%%~nxF"
)

if exist "%REPO_DIR%\Packages\Launchpad\" (
    for %%F in ("%REPO_DIR%\Packages\Launchpad\*.*") do (
        call :copy_file "%%F" "Packages\Launchpad\%%~nxF"
    )
)
if exist "%REPO_DIR%\Packages\Editor\" (
    for %%F in ("%REPO_DIR%\Packages\Editor\*.*") do (
        call :copy_file "%%F" "Packages\Editor\%%~nxF"
    )
)

:: ---------------------------------------------------------------------------
:: Summary
:: ---------------------------------------------------------------------------
echo.
echo   -------------------------------------------
if %errors% gtr 0 (
    echo   [!] %errors% file(s) failed to compile -- check output above.
)
echo   Compiled  : %compiled% files
echo   Copied    : %copied% files  (source -- not compiled)
echo.
echo   Image ready: %DIST_DIR%
echo.

if %errors% gtr 0 exit /b 1
exit /b 0
