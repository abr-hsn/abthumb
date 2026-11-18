@echo off
rem abthumb installer: copies to Program Files and registers machine-wide (one UAC prompt).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
