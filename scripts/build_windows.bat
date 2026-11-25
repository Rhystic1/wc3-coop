@echo off
setlocal enabledelayedexpansion

REM Build XanLib.dll (32-bit proxy DLL) on Windows using MinGW-w64.
REM Requires a 32-bit (i686) MinGW-w64 GCC, e.g.:
REM   - MSYS2 MINGW32: pacman -S mingw-w64-i686-gcc (C:\msys64\mingw32\bin\gcc.exe)
REM   - i686-w64-mingw32-gcc on PATH (WinLibs / Scoop / Chocolatey)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

set "CC="

where i686-w64-mingw32-gcc >nul 2>&1
if %errorlevel%==0 (
    set "CC=i686-w64-mingw32-gcc"
)

if not defined CC if exist "C:\msys64\mingw32\bin\gcc.exe" (
    set "PATH=C:\msys64\mingw32\bin;%PATH%"
    set "CC=C:\msys64\mingw32\bin\gcc.exe"
)

if not defined CC (
    where gcc >nul 2>&1
    if %errorlevel%==0 (
        for /f "delims=" %%T in ('gcc -dumpmachine 2^>nul') do set "TARGET=%%T"
        echo !TARGET! | findstr /b /i "i686- mingw32" >nul
        if !errorlevel!==0 (
            set "CC=gcc"
        )
    )
)

if not defined CC (
    echo [ERROR] Could not find a 32-bit MinGW-w64 GCC compiler.
    echo.
    echo WC3W.EXE is 32-bit, so XanLib.dll must be built with an i686 MinGW-w64 compiler:
    echo   Option 1 ^(MSYS2^):
    echo     1. Install MSYS2 ^(winget install MSYS2.MSYS2^)
    echo     2. Open MSYS2 MSYS and run: pacman -S --needed mingw-w64-i686-gcc
    echo     3. Re-run this script ^(it checks C:\msys64\mingw32\bin\gcc.exe automatically^)
    echo   Option 2 ^(WinLibs / Scoop^):
    echo     Add the 32-bit MinGW bin folder ^(with i686-w64-mingw32-gcc.exe^) to PATH.
    exit /b 1
)

echo Using compiler: %CC%

echo [1/2] Building XanLib.dll...
pushd "%REPO_ROOT%\src\wc3net"
"%CC%" -shared -o XanLib.dll dllmain.c hook.c net.c xanlib_fwd.def -Wall -lws2_32
if errorlevel 1 (
    popd
    echo [ERROR] Failed to build XanLib.dll
    exit /b 1
)
popd

echo [2/2] Staging to lancopy\...
if not exist "%REPO_ROOT%\lancopy" mkdir "%REPO_ROOT%\lancopy"
copy /y "%REPO_ROOT%\src\wc3net\XanLib.dll" "%REPO_ROOT%\lancopy\XanLib.dll" >nul

echo.
echo Build complete:
echo   %REPO_ROOT%\src\wc3net\XanLib.dll
echo   Staged to %REPO_ROOT%\lancopy\
exit /b 0
