@echo off
REM Thin delegate — all diagnostic launch logic lives in launch.ps1 (single
REM source of truth, so the variants can't drift).  Vulkan validation layers
REM (non-fatal); messages land in logs\perf_log.txt as
REM [Vulkan VALIDATION ERROR] lines.  Significantly slows the app.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -Validation
exit /b %ERRORLEVEL%
