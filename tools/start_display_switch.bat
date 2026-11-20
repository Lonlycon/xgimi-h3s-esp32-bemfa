@echo off
rem ===========================================================================
rem  Start display_switch.py with pythonw (no console window)
rem  ---------------------------------------------------------------------------
rem  Put this BAT in the same folder as display_switch.py and double-click it.
rem
rem  Why this is not a one-liner:
rem    If several Pythons are installed, "pythonw" on PATH may point to one
rem    that does NOT have pystray installed -> the script silently falls back
rem    to console mode. So this BAT probes every candidate and picks the
rem    first one that can actually "import pystray, PIL".
rem
rem  Usage:
rem     double-click            start it
rem     start_display_switch.bat /check    only report which Python it picks
rem ===========================================================================

setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

set "SCRIPT=%~dp0display_switch.py"
set "N=0"

rem ---- collect candidates -------------------------------------------------
call :add "%LOCALAPPDATA%\Python\pythoncore-3.14-64\pythonw.exe"
for /d %%D in ("%LOCALAPPDATA%\Python\pythoncore-*") do call :add "%%~D\pythonw.exe"
for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do call :add "%%~D\pythonw.exe"
for /f "delims=" %%P in ('where pythonw 2^>nul') do call :add "%%P"
if defined LOCALAPPDATA call :add "%ProgramFiles%\Python313\pythonw.exe"
call :add "C:\Python313\pythonw.exe"
call :add "C:\Python312\pythonw.exe"
call :add "C:\Python311\pythonw.exe"

if "%N%"=="0" (
    echo.
    echo [ERROR] pythonw.exe not found anywhere.
    echo         Install Python and tick "Add python.exe to PATH".
    echo.
    pause
    exit /b 1
)

rem ---- probe each candidate: does its python.exe have pystray? -------------
set "PICK="
for /l %%i in (1,1,%N%) do (
    if not defined PICK call :probe %%i
)

if not defined PICK (
    set "PICK=!C1!"
    set "WARN=1"
)

if /i "%~1"=="/check" (
    echo.
    echo   script : "!SCRIPT!"
    echo   picked : "!PICK!"
    if defined WARN echo   note   : no candidate has pystray - tray will be OFF
    echo.
    if exist "!SCRIPT!" ( exit /b 0 ) else ( exit /b 3 )
)

if not exist "%SCRIPT%" (
    echo.
    echo [ERROR] display_switch.py not found next to this BAT.
    echo         Put both files in the same folder.
    echo.
    pause
    exit /b 1
)

if defined WARN (
    echo.
    echo [WARN] None of the found Pythons has pystray installed.
    echo        The program will still run, but WITHOUT a tray icon.
    echo        Fix it with:   <that python>.exe -m pip install pystray pillow
    echo.
    timeout /t 4 >nul
)

netstat -ano -p UDP 2>nul | findstr /C:":54321" >nul
if not errorlevel 1 (
    echo.
    echo [HINT] UDP port 54321 already looks busy.
    echo        It may already be running - check the tray before starting again.
    echo.
    timeout /t 3 >nul
)

echo Starting with: "!PICK!"
start "" "!PICK!" "%SCRIPT%"
exit /b 0


rem ===========================================================================
rem  helpers
rem ===========================================================================
:add
if "%~1"=="" exit /b
if not exist "%~1" exit /b
set /a N+=1
set "C!N!=%~1"
exit /b

:probe
set "W=!C%~1!"
set "P=!W:pythonw.exe=python.exe!"
if not exist "!P!" exit /b
"!P!" -c "import pystray,PIL" >nul 2>&1
if errorlevel 1 exit /b
set "PICK=!W!"
exit /b
