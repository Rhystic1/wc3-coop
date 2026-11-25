@echo off
setlocal enabledelayedexpansion

REM Install the wc3net XanLib proxy into the WC3 Kilrathi_Saga directory on Windows.
REM Reversibly renames XANLIB.DLL -> XanLibReal.dll and copies XanLib.dll in.
REM Usage:
REM   install_proxy_windows.bat [path\to\Kilrathi_Saga]
REM   (or set WC3_GAME_DIR before running)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

set "GAME_DIR=%~1"
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

set "PROXY=%REPO_ROOT%\src\wc3net\XanLib.dll"
if not exist "%PROXY%" set "PROXY=%REPO_ROOT%\lancopy\XanLib.dll"
if not exist "%PROXY%" (
    echo [ERROR] XanLib.dll not found. Run scripts\build_windows.bat first.
    exit /b 1
)

if not exist "%GAME_DIR%\XanLibReal.dll" (
    if not exist "%GAME_DIR%\XANLIB.DLL" (
        echo [ERROR] Neither XANLIB.DLL nor XanLibReal.dll found in "%GAME_DIR%"
        exit /b 1
    )
    move /y "%GAME_DIR%\XANLIB.DLL" "%GAME_DIR%\XanLibReal.dll" >nul
    echo Original XANLIB.DLL backed up to XanLibReal.dll
)

copy /y "%PROXY%" "%GAME_DIR%\XanLib.dll" >nul
if exist "%GAME_DIR%\wc3net.log" del /f /q "%GAME_DIR%\wc3net.log" >nul

if not exist "%GAME_DIR%\wc3net.ini" (
    if exist "%REPO_ROOT%\lancopy\wc3net.ini" (
        copy /y "%REPO_ROOT%\lancopy\wc3net.ini" "%GAME_DIR%\wc3net.ini" >nul
        echo Copied default wc3net.ini ^(mode=remote^) into "%GAME_DIR%"
        echo Edit "%GAME_DIR%\wc3net.ini" to set mode=client + host=^<HOST_IP^> for client, or mode=off for single-player.
    )
)

echo Proxy installed to "%GAME_DIR%" ^(wc3net.log cleared^).
exit /b 0
