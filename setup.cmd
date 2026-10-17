@echo off
setlocal
set PYTHONUTF8=1
py -3.14 -m venv "%~dp0.venv"
if errorlevel 1 exit /b %errorlevel%
"%~dp0.venv\Scripts\python.exe" -m pip install --disable-pip-version-check -r "%~dp0requirements.txt"
if errorlevel 1 exit /b %errorlevel%
rustup toolchain install 1.96.0 --profile minimal
exit /b %errorlevel%
