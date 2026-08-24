@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0_INTERNAL - SAVR Support Tool Script.ps1"
if errorlevel 1 pause
