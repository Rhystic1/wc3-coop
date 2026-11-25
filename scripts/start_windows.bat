@echo off
setlocal enabledelayedexpansion

REM Start Wing Commander III in co-op mode on Windows.
REM Installs/updates the XanLib.dll proxy if needed, clears wc3net.log, and launches WC3W.EXE.
REM Usage:
REM   start_windows.bat [host|client|singleplayer] [path\to\Kilrathi_Saga]
REM   If no mode is given, uses mode from wc3net.ini in the game directory.

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

set "ROLE=%~1"
set "GAME_DIR=%~2"

if /i "%ROLE%"=="host" (
    set "WC3NET_MODE=remote"
) else if /i "%ROLE%"=="remote" (
    set "WC3NET_MODE=remote"
) else if /i "%ROLE%"=="client" (
    set "WC3NET_MODE=client"
) else if /i "%ROLE%"=="singleplayer" (
    set "WC3NET_MODE=off"
) else if /i "%ROLE%"=="off" (
    set "WC3NET_MODE=off"
) else if not "%ROLE%"=="" (
    if not defined GAME_DIR set "GAME_DIR=%ROLE%"
    set "ROLE="
)

if not defined GAME_DIR if defined WC3_GAME_DIR set "GAME_DIR=%WC3_GAME_DIR%"

if not defined GAME_DIR (
    for %%D in (
        "C:\GOG Games\Wing Commander III 30th Anniversary Edition\Kilrathi_Saga"
        "C:\Program Files (x86)\GOG Galaxy\Games\Wing Commander III 30th Anniversary Edition\Kilrathi_Saga"
        "C:\Program Files (x86)\GOG.com\Wing Commander III\Kilrathi_Saga"
    ) do (
        if not defined GAME_DIR if exist "%%~D\WC3W.EXE" set "GAME_DIR=%%~D"
    )
)

if not defined GAME_DIR (
    set /p "GAME_DIR=Enter full path to Kilrathi_Saga folder (containing WC3W.EXE): "
)

if not exist "%GAME_DIR%\WC3W.EXE" (
    echo [ERROR] WC3W.EXE not found in "%GAME_DIR%"
    exit /b 1
)

call "%SCRIPT_DIR%install_proxy_windows.bat" "%GAME_DIR%"
if errorlevel 1 exit /b 1

if defined WC3NET_MODE (
    echo Starting WC3W.EXE with WC3NET_MODE=%WC3NET_MODE%...
) else (
    echo Starting WC3W.EXE using mode from "%GAME_DIR%\wc3net.ini"...
)

pushd "%GAME_DIR%"
start "" "WC3W.EXE"
popd
exit /b 0
