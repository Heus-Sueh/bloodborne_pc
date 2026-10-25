@echo off
setlocal
cd /d "%~dp0.."
if not exist "out\bb-runner.exe" (
    echo Building native runner...
    call "scripts\build.sh"
)
"out\bb-runner.exe" %*
exit /b %ERRORLEVEL%
