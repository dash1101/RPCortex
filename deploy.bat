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
set /a uploaded=0
set /a errors=0

:: ---------------------------------------------------------------------------
:: Helper subroutines
:: ---------------------------------------------------------------------------
goto :skip_subs

:ensure_dir
    %MPR% mkdir ":%~1" >nul 2>&1
    exit /b 0

:upload_file
    :: %1 = local path, %2 = remote path, %3 = display label
    %MPR% cp "%~1" ":%~2" >nul 2>&1
    if errorlevel 1 (
        echo   [!] FAILED: %~3
        set /a errors+=1
    ) else (
        echo   [+] %~3
        set /a uploaded+=1
    )
    exit /b 0

:skip_subs

:: ---------------------------------------------------------------------------
:: Create remote directory tree
:: ---------------------------------------------------------------------------
echo   -- Creating remote directories --
call :ensure_dir /Core
call :ensure_dir /Core/Launchpad
call :ensure_dir /Packages
call :ensure_dir /Packages/Launchpad
call :ensure_dir /Packages/Editor
echo.

:: ---------------------------------------------------------------------------
:: main.py
:: ---------------------------------------------------------------------------
echo   -- Entry point --
call :upload_file "%SRC_DIR%\main.py" "/main.py" "main.py"
echo.

:: ---------------------------------------------------------------------------
:: Core files (.py and .mpy)
:: ---------------------------------------------------------------------------
echo   -- Core modules --
for %%F in ("%SRC_DIR%\Core\*.py" "%SRC_DIR%\Core\*.mpy") do (
    if exist "%%F" call :upload_file "%%F" "/Core/%%~nxF" "Core\%%~nxF"
)
echo.

:: ---------------------------------------------------------------------------
:: Launchpad command files
:: ---------------------------------------------------------------------------
echo   -- Launchpad --
for %%F in ("%SRC_DIR%\Core\Launchpad\*.*") do (
    if exist "%%F" call :upload_file "%%F" "/Core/Launchpad/%%~nxF" "Core\Launchpad\%%~nxF"
)
echo.

:: ---------------------------------------------------------------------------
:: Package stubs
:: ---------------------------------------------------------------------------
echo   -- Package stubs --
for %%F in ("%SRC_DIR%\Packages\Launchpad\*.*") do (
    if exist "%%F" call :upload_file "%%F" "/Packages/Launchpad/%%~nxF" "Packages\Launchpad\%%~nxF"
)
for %%F in ("%SRC_DIR%\Packages\Editor\*.*") do (
    if exist "%%F" call :upload_file "%%F" "/Packages/Editor/%%~nxF" "Packages\Editor\%%~nxF"
)
echo.

:: ---------------------------------------------------------------------------
:: Summary
:: ---------------------------------------------------------------------------
echo   -------------------------------------------
if %errors% gtr 0 (
    echo   [!] %errors% file(s) failed to upload -- check output above.
)
echo   Uploaded  : %uploaded% files
echo.
echo   Deploy complete. Reboot the device to apply.
echo.

if %errors% gtr 0 exit /b 1
exit /b 0
