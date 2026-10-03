@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 (
    echo Cannot open the LAMBDA Player project folder.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "buildExitCode=%errorlevel%"
popd
if not "%buildExitCode%"=="0" pause
exit /b %buildExitCode%
