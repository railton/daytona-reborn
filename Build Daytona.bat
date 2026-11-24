@echo off
rem Builds the game on Windows 10/11 with portable tools (scripts\windows-build.ps1):
rem nothing to install, no administrator rights. Double-click it.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows-build.ps1" %*
echo.
pause
