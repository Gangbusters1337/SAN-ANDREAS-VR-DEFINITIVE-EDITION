@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-SAVR.ps1"
set "code=%errorlevel%"
echo.
pause
exit /b %code%
