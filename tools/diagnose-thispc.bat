@echo off
chcp 65001 >nul
set "SCRIPT=%~dp0diagnose-thispc.ps1"
where pwsh >nul 2>nul
if %errorlevel%==0 (
  pwsh -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
)
echo.
pause