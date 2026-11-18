@echo off
rem Removes abthumb and restores the previous thumbnail and preview handlers (one UAC prompt).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
echo.
pause
