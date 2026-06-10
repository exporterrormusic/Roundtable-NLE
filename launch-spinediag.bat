@echo off
REM Thin delegate — all diagnostic launch logic lives in launch.ps1 (single
REM source of truth, so the variants can't drift).  Enables the per-clip
REM [SPINE-BLEND-DIAG] logger (warn-level, survives the warn+ filter);
REM reproduce the flicker, quit, then read logs\perf_log.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -SpineDiag
exit /b %ERRORLEVEL%
