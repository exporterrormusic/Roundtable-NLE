@echo off
echo Building ROUNDTABLE...
rem Args are forwarded to build.ps1, e.g.:  build.bat -NoLto   |   build.bat -All
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    pause
    exit /b 1
)
pause
