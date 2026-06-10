<#
.SYNOPSIS
    ROUNDTABLE NLE - unified DIAGNOSTIC launcher.  Single source of truth
    for the diagnostic launch modes; launch_debug.bat / launch-framehash.bat
    / launch-spinediag.bat / launch_with_validation.bat are thin delegates
    into this script so the variants can't drift.

    NOTE: launch.bat is intentionally NOT routed through here - it stays
    pure batch -> launch.vbs so the everyday launch has zero PowerShell
    startup latency.  Keep it that way.

.PARAMETER Debug
    Keep the console visible, prefer the Debug build (CRT heap validation),
    and capture stdout+stderr to debug.log.  (Replaces launch_debug.bat.)
.PARAMETER FrameHash
    A/B render-path verification harness (#18): sets ROUNDTABLE_FRAMEHASH so
    every composited frame is GPU-signatured to framehash.csv
    (tag,frame,width,height,s0..s7; tag = preview | export).  Playback is
    intentionally slow in this mode.  (Replaces launch-framehash.bat.)
.PARAMETER SpineDiag
    Sets ROUNDTABLE_SPINE_DIAG=1 so the per-clip [SPINE-BLEND-DIAG] logger
    fires (warn-level, survives the warn+ filter).  Read logs/perf_log.txt
    after reproducing.  (Replaces launch-spinediag.bat.)
.PARAMETER Validation
    Vulkan validation layers (ROUNDTABLE_VALIDATION=1, SYNC_VALIDATION=1,
    VALIDATION_FATAL=0).  Messages land in logs/perf_log.txt as
    [Vulkan VALIDATION ERROR] lines.  Significantly slows the app.
    (Replaces launch_with_validation.bat.)
.PARAMETER NoGpuDecode
    Kill-switch: ROUNDTABLE_GPU_RESIDENT_DECODE=0 forces the legacy CPU
    upload path (for diagnosing GPU-resident decode regressions).

.NOTES
    AppVerifier/PageHeap launching is NOT here - it needs Administrator and
    IFEO registry edits with guaranteed cleanup; use launch-debug.bat.
    Keep this file pure ASCII (PowerShell 5.1 reads BOM-less files as ANSI).
#>
param(
    [switch]$Debug,
    [switch]$FrameHash,
    [switch]$SpineDiag,
    [switch]$Validation,
    [switch]$NoGpuDecode
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

# --- Qt on PATH: local third_party (portable) first, system Qt fallback ---
$qtLocal  = Join-Path $root "third_party\qt\6.8.3\msvc2022_64\bin"
$qtSystem = "C:\Qt\6.8.3\msvc2022_64\bin"
if (Test-Path $qtLocal) {
    $env:PATH = "$qtLocal;$env:PATH"
} elseif (Test-Path $qtSystem) {
    $env:PATH = "$qtSystem;$env:PATH"
} else {
    Write-Host "WARNING: Qt 6.8.3 not found. Run setup.ps1 first." -ForegroundColor Yellow
    exit 1
}

# --- FFmpeg DLLs on PATH ---------------------------------------------------
$ffmpeg = Join-Path $root "third_party\ffmpeg\bin"
if (Test-Path $ffmpeg) { $env:PATH = "$ffmpeg;$env:PATH" }

# --- Diagnostic modes (env vars inherit into the roundtable.exe child) -----
if ($FrameHash) {
    $env:ROUNDTABLE_FRAMEHASH = Join-Path $root "framehash.csv"
    Write-Host "[framehash] harness ON -> $($env:ROUNDTABLE_FRAMEHASH)"
}
if ($SpineDiag) {
    $env:ROUNDTABLE_SPINE_DIAG = "1"
    Write-Host "[spine-diag] SPINE-BLEND-DIAG logging ON"
}
if ($Validation) {
    $env:ROUNDTABLE_VALIDATION       = "1"
    $env:ROUNDTABLE_SYNC_VALIDATION  = "1"
    $env:ROUNDTABLE_VALIDATION_FATAL = "0"
    Write-Host "[validation] Vulkan validation layers ON (non-fatal)"
}
if ($NoGpuDecode) {
    $env:ROUNDTABLE_GPU_RESIDENT_DECODE = "0"
    Write-Host "[gpu] GPU-resident decode OFF (legacy CPU upload path)"
}

if ($Debug) {
    # Console run: prefer Debug build (CRT heap validation), capture output.
    $candidates = @(
        (Join-Path $root "build\bin\Debug\roundtable.exe"),
        (Join-Path $root "build\bin\Release\roundtable.exe"),
        (Join-Path $root "roundtable.exe")
    )
    $target = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $target) {
        Write-Host "ERROR: roundtable.exe not found (build\bin\Debug, build\bin\Release, or repo root)." -ForegroundColor Red
        exit 1
    }
    if ($target -like "*\Debug\*") {
        Write-Host "Debug build selected (heap validation enabled)"
    } else {
        Write-Host "Release build selected (no heap validation)"
    }
    $logFile = Join-Path $root "debug.log"
    Write-Host "Launching: $target"
    Write-Host "Console output is being saved to $logFile"
    & $target *> $logFile
    Write-Host "Application exited. Check debug.log for output."
} else {
    # Normal/diagnostic run: delegate to launch.vbs for a truly invisible
    # launch (no terminal flash).  launch.vbs picks installed-vs-dev exe.
    Start-Process -FilePath "wscript.exe" `
        -ArgumentList "`"$(Join-Path $root 'launch.vbs')`"" `
        -WindowStyle Hidden
}
