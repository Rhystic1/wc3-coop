@echo off
setlocal enabledelayedexpansion

REM Restore the original XANLIB.DLL and remove the wc3net proxy on Windows.
REM Usage:
REM   restore_proxy_windows.bat [path\to\Kilrathi_Saga]
REM   (or set WC3_GAME_DIR before running)

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

if exist "%GAME_DIR%\XanLib.dll" del /f /q "%GAME_DIR%\XanLib.dll" >nul
if exist "%GAME_DIR%\XanLibReal.dll" (
    move /y "%GAME_DIR%\XanLibReal.dll" "%GAME_DIR%\XANLIB.DLL" >nul
    echo Original XANLIB.DLL restored.
) else (
    echo XanLibReal.dll not found ^(already restored?^).
)

echo Proxy removed from "%GAME_DIR%".
exit /b 0
