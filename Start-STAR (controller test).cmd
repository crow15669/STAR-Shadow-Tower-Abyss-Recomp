@echo off
rem STAR with the controller trace on: the log (build\bin\logs\play_last.log) gets a "[pad]" line whenever the
rem controller data the game receives changes. Play as usual, try the buttons that do not work, then close the game.
setlocal
set "ROOT=%~dp0"
set /p STAR_ISO=<"%ROOT%game\iso_path.txt"
if not exist "%ROOT%build\bin\logs" mkdir "%ROOT%build\bin\logs"
cd /d "%ROOT%build\bin"
set STAR_TRACE_PAD=1
echo STAR is running with the controller trace. This window shows the result when the game closes.
STAR.exe --iso "%STAR_ISO%" > "logs\play_last.log" 2>&1
echo.
echo STAR closed (exit code %ERRORLEVEL%). Log: %ROOT%build\bin\logs\play_last.log
pause
