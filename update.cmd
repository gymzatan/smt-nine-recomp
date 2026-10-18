@echo off
setlocal
call "%~dp0build.cmd" --update %*
exit /b %errorlevel%
