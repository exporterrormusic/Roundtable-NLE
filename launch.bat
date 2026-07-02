@echo off
REM ─── UPGRADE_PLAN: GPU-resident decode + CUDA↔Vulkan zero-copy ────────
REM    Default ON in roundtable.exe as of 2026-05-21.  Uncomment the
REM    line below to force the legacy CPU upload path (kill-switch for
REM    diagnosing regressions or comparing cold-start latency).
REM set ROUNDTABLE_GPU_RESIDENT_DECODE=0
REM ─── Optional: Vulkan validation pass (docs/UPGRADE_PLAN.txt item 1) ──
REM    Uncomment when validating GPU code changes — significantly slows
REM    the app; not for normal use.
REM set ROUNDTABLE_VALIDATION=1
REM set ROUNDTABLE_VALIDATION_FATAL=0
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

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

REM Delegate to launch.vbs for a truly invisible launch (no terminal flash).
REM launch.vbs handles Debug vs Release selection and hides all windows.
start "" /min wscript.exe "%~dp0launch.vbs"
exit /b 0
