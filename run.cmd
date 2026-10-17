@echo off
setlocal
"%~dp0bin\smt9_recomp.exe" %*
exit /b %errorlevel%
