@echo off
REM ════════════════════════════════════════════════════════════════════
REM  launch-appverifier.bat — Run roundtable.exe under Application Verifier
REM
REM  Purpose: catch heap corruption (0xC0000374) at the WRITE site
REM  instead of later in ntdll's heap-metadata detector.  AppVerifier
REM  enables PageHeap: every heap block is on its own page with a guard
REM  page after it, so:
REM    • One-byte buffer overruns → immediate AV at the bad store
REM    • Use-after-free reads/writes → immediate AV on freed memory
REM    • Double-free → immediate AV in the second free call
REM
REM  The existing SEH crash handler still writes logs\crash_*.dmp and
REM  appends to logs\crash_log.txt, but with a stack pointing at YOUR
REM  code instead of ntdll.
REM
REM  Trade-offs:
REM    • Memory: ~5-10x normal (each alloc rounds up to a page)
REM    • Speed: ~20-50%% slower — DO NOT ship with this on
REM    • Requires Administrator (writes IFEO registry keys)
REM
REM  AppVerifier is automatically DISABLED on exit, so a subsequent
REM  launch.bat run is unaffected.  If the .bat is killed before the
REM  cleanup runs, re-run this file and let it exit normally, or run
REM  manually:
REM      appverif -disable * -for roundtable.exe
REM ════════════════════════════════════════════════════════════════════

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "ROOT=%~dp0..\..\"

REM ── Admin check (AppVerifier requires elevation) ────────────────────
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo.
    echo This launcher requires Administrator privileges.
    echo AppVerifier writes to HKLM\...\Image File Execution Options.
    echo.
    echo Right-click launch-appverifier.bat and choose "Run as administrator".
    echo.
    pause
    exit /b 1
)

REM ── Locate appverif.exe ─────────────────────────────────────────────
where appverif.exe >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: appverif.exe not found in PATH.
    echo Expected at C:\Windows\System32\appverif.exe ^(ships with Windows^).
    pause
    exit /b 1
)

REM ── Locate the target exe ───────────────────────────────────────────
REM Prefer Release ^(matches what the user runs day-to-day per CLAUDE.md^).
if exist "%ROOT%build\bin\Release\roundtable.exe" (
    set "TARGET=%ROOT%build\bin\Release\roundtable.exe"
    set "EXE_NAME=roundtable.exe"
    echo Using Release build.
) else if exist "%ROOT%build\bin\RelWithDebInfo\roundtable.exe" (
    set "TARGET=%ROOT%build\bin\RelWithDebInfo\roundtable.exe"
    set "EXE_NAME=roundtable.exe"
    echo Using RelWithDebInfo build ^(symbols included^).
) else if exist "%ROOT%build\bin\Debug\roundtable.exe" (
    set "TARGET=%ROOT%build\bin\Debug\roundtable.exe"
    set "EXE_NAME=roundtable.exe"
    echo Using Debug build.
) else if exist "%ROOT%roundtable.exe" (
    set "TARGET=%ROOT%roundtable.exe"
    set "EXE_NAME=roundtable.exe"
    echo Using installed roundtable.exe.
) else (
    echo ERROR: roundtable.exe not found.
    echo Looked in: %ROOT%build\bin\Release\ , RelWithDebInfo\ , Debug\ , %ROOT%
    pause
    exit /b 1
)

REM ── Qt + FFmpeg PATH ^(mirrors launch.bat^) ──────────────────────────
if exist "%ROOT%third_party\qt\6.8.3\msvc2022_64\bin" (
    set "PATH=%ROOT%third_party\qt\6.8.3\msvc2022_64\bin;%PATH%"
) else if exist "C:\Qt\6.8.3\msvc2022_64\bin" (
    set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
) else (
    echo WARNING: Qt 6.8.3 not found. Run setup.ps1 first.
    pause
    exit /b 1
)

if exist "%ROOT%third_party\ffmpeg\bin" (
    set "PATH=%ROOT%third_party\ffmpeg\bin;%PATH%"
)

REM ── Enable Application Verifier ─────────────────────────────────────
REM  Heaps   = PageHeap (the main event — catches the corruption at write time)
REM  Handles = invalid HANDLE usage (catches double-CloseHandle, etc.)
REM  Add more here if needed (Locks, Exceptions, COM, ...), but expect
REM  noisier output and worse performance.
echo.
echo Enabling AppVerifier Heaps + Handles on !EXE_NAME!...
appverif.exe -enable Heaps Handles -for !EXE_NAME!
if %errorLevel% neq 0 (
    echo ERROR: failed to enable AppVerifier.
    pause
    exit /b 1
)

echo.
echo ═════════════════════════════════════════════════════════════════
echo  Running under AppVerifier - slower and uses more memory.
echo  Reproduce the crash; the SEH handler will write a minidump with
echo  an accurate stack to logs\crash_*.dmp.
echo  Console + stderr captured to debug-heap.log.
echo ═════════════════════════════════════════════════════════════════
echo.

REM ── Run (console visible, stdout+stderr to debug-heap.log) ──────────
"!TARGET!" >"%ROOT%debug-heap.log" 2>&1
set "EXITCODE=!errorlevel!"

REM ── Disable AppVerifier so launch.bat is unaffected ─────────────────
echo.
echo Disabling AppVerifier for !EXE_NAME!...
appverif.exe -disable * -for !EXE_NAME!

echo.
echo Application exited with code: !EXITCODE!
echo Output captured to:           debug-heap.log
echo Crash dumps (if any):         logs\crash_*.dmp
echo Crash log:                    logs\crash_log.txt
echo.
echo If the app crashed under AppVerifier, the newest .dmp in logs\
echo will have a stack frame pointing at the bug instead of ntdll.
echo.
pause
endlocal
