@echo off
REM Thin delegate — all diagnostic launch logic lives in launch.ps1 (single
REM source of truth, so the variants can't drift).  A/B render-path
REM verification harness (#18): GPU-signatures every composited frame to
REM framehash.csv (tag,frame,width,height,s0..s7; tag = preview | export).
REM Slow by design — verification only, not normal editing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -FrameHash
exit /b %ERRORLEVEL%
