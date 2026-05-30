@echo off
REM ─── A/B render-path verification harness (#18) ──────────────────────────
REM    Same as launch.bat, but sets ROUNDTABLE_FRAMEHASH so each composited
REM    frame is GPU-signatured and logged to framehash.csv:
REM        tag,frame,width,height,s0..s7   (tag = preview | export)
REM    Usage: scrub/play (writes tag=preview rows) AND export the same project
REM    (writes tag=export rows), then diff rows by frame across the two tags.
REM    NOTE: playback is intentionally slow in this mode (a synchronous GPU
REM    signature per frame) — for verification only, not normal editing.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM Output CSV next to this script (overwritten each run).
set "ROUNDTABLE_FRAMEHASH=%~dp0framehash.csv"
echo [framehash] harness ON -> %ROUNDTABLE_FRAMEHASH%

REM Use local Qt from third_party (portable), fall back to system Qt
if exist "%~dp0third_party\qt\6.8.3\msvc2022_64\bin" (
    set "PATH=%~dp0third_party\qt\6.8.3\msvc2022_64\bin;%PATH%"
) else if exist "C:\Qt\6.8.3\msvc2022_64\bin" (
    set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
) else (
    echo WARNING: Qt 6.8.3 not found. Run setup.ps1 first.
    pause
    exit /b 1
)

REM Add FFmpeg DLLs to PATH
if exist "%~dp0third_party\ffmpeg\bin" (
    set "PATH=%~dp0third_party\ffmpeg\bin;%PATH%"
)

REM Delegate to launch.vbs (invisible launch); the env var set above is
REM inherited through wscript -> roundtable.exe.
start "" /min wscript.exe "%~dp0launch.vbs"
exit /b 0
