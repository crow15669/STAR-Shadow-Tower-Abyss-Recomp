@echo off
rem Starts STAR (Shadow Tower Abyss recomp): the launcher first (the disc image, presets and options), then the game.
rem The disc image named in game\iso_path.txt is the launcher's choice until you pick one there.
rem The run's log goes to build\bin\logs\play_last.log. F11 = full screen, F12 = the runtime debugger.
setlocal
set "ROOT=%~dp0"
if not exist "%ROOT%build\bin\STAR.exe" (
    echo STAR.exe is not built yet: python -I tools\build.py build
    pause
    exit /b 1
)
if exist "%ROOT%game\iso_path.txt" set /p STAR_ISO=<"%ROOT%game\iso_path.txt"
if not exist "%ROOT%build\bin\logs" mkdir "%ROOT%build\bin\logs"
cd /d "%ROOT%build\bin"
rem The three runs before this one are kept (play_previous_1 is the newest), so a stutter that will not repeat
rem can still be looked up: the log has a [stutter] line for each one.
if exist "logs\play_previous_2.log" move /y "logs\play_previous_2.log" "logs\play_previous_3.log" >nul
if exist "logs\play_previous_1.log" move /y "logs\play_previous_1.log" "logs\play_previous_2.log" >nul
if exist "logs\play_last.log" move /y "logs\play_last.log" "logs\play_previous_1.log" >nul
echo STAR is running. This window shows the result when the game closes.
STAR.exe > "logs\play_last.log" 2>&1
echo.
echo STAR closed (exit code %ERRORLEVEL%). Log: %ROOT%build\bin\logs\play_last.log
pause
