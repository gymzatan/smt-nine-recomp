@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Apply-Update.ps1" %*
if errorlevel 1 (
  echo Update failed. Read the error above.
  pause
  exit /b 1
)
pause
