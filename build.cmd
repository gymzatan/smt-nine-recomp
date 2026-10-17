@echo off
setlocal
set PYTHONUTF8=1
if not exist "%~dp0.venv\Scripts\python.exe" (
  echo Run setup.cmd first.
  exit /b 1
)
"%~dp0.venv\Scripts\python.exe" "%~dp0tools\build.py" %*
exit /b %errorlevel%
