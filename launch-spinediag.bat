@echo off
REM ─── Spine loop-seam flicker diagnostic ──────────────────────────────────
REM    Same as launch.bat, but sets ROUNDTABLE_SPINE_DIAG so the per-clip
REM    [SPINE-BLEND-DIAG] logger fires (warn-level, survives the warn+ filter).
REM    It prints, edge-triggered (≈twice per loop), the batch blend signature
REM    plus the darkest slot + min vertex brightness/alpha and the animTime —
REM    pinpointing the slot + loop time behind a one-frame Spine flicker.
REM    Reproduce the Modernia leg flicker, quit, then read logs\perf_log.txt.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ROUNDTABLE_SPINE_DIAG=1"
echo [spine-diag] SPINE-BLEND-DIAG logging ON

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
