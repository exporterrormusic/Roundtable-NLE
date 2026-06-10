@echo off
REM Thin delegate — all diagnostic launch logic lives in launch.ps1 (single
REM source of truth, so the variants can't drift).  Console run, prefers the
REM Debug build (CRT heap validation), captures stdout+stderr to debug.log.
REM (launch.bat stays pure batch — the fast everyday path.)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -Debug
pause
