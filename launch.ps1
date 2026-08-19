<#
.SYNOPSIS
    ROUNDTABLE NLE - unified DIAGNOSTIC launcher.  Single source of truth
    for the diagnostic launch modes; the thin delegates in scripts\diag\
    (launch-crtdebug.bat / launch-framehash.bat / launch-spinediag.bat /
    launch-validation.bat) route into this script so the variants can't
    drift.

    NOTE: the everyday launcher is launch.vbs (runs the exe hidden, zero
    PowerShell startup latency) - it is intentionally NOT routed through
    here.  Keep it that way.

.PARAMETER Debug
    Keep the console visible, prefer the Debug build (CRT heap validation),
    and capture stdout+stderr to debug.log.
    (Delegate: scripts\diag\launch-crtdebug.bat.)
.PARAMETER FrameHash
    A/B render-path verification harness (#18): sets ROUNDTABLE_FRAMEHASH so
    every composited frame is GPU-signatured to framehash.csv
    (tag,frame,width,height,s0..s7; tag = preview | export).  Playback is
    intentionally slow in this mode.
    (Delegate: scripts\diag\launch-framehash.bat.)
.PARAMETER SpineDiag
    Sets ROUNDTABLE_SPINE_DIAG=1 so the per-clip [SPINE-BLEND-DIAG] logger
    fires (warn-level, survives the warn+ filter).  Read logs/perf_log.txt
    after reproducing.  (Delegate: scripts\diag\launch-spinediag.bat.)
.PARAMETER Validation
    Vulkan validation layers (ROUNDTABLE_VALIDATION=1, SYNC_VALIDATION=1,
    VALIDATION_FATAL=0).  Messages land in logs/perf_log.txt as
    [Vulkan VALIDATION ERROR] lines.  Significantly slows the app.
    (Delegate: scripts\diag\launch-validation.bat.)
.PARAMETER NoGpuDecode
    Kill-switch: ROUNDTABLE_GPU_RESIDENT_DECODE=0 forces the legacy CPU
    upload path (for diagnosing GPU-resident decode regressions).

.NOTES
    AppVerifier/PageHeap launching is NOT here - it needs Administrator and
    IFEO registry edits with guaranteed cleanup; use
    scripts\diag\launch-appverifier.bat.
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

function Assert-CudaRuntimeReady([string]$ExePath) {
    # CUDA imports are resolved before roundtable's main() can log an error.
    # The generated package manifest gives diagnostic launches a precise,
    # actionable failure while CPU-only emergency builds (no manifest) pass.
    $exeDir = Split-Path -Parent $ExePath
    $manifest = Join-Path $exeDir "roundtable-cuda-runtime.txt"
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { return }

    $required = Get-Content -LiteralPath $manifest |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith("#") }
    $missing = @($required | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $exeDir $_) -PathType Leaf)
    })
    if ($missing.Count -gt 0) {
        Write-Host "ERROR: Bundled CUDA runtime is incomplete beside roundtable.exe:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        Write-Host "Reinstall/rebuild ROUNDTABLE; do not add the CUDA Toolkit to PATH as a workaround." -ForegroundColor Yellow
        exit 2
    }

    $driver = Join-Path ([System.Environment]::GetFolderPath("System")) "nvcuda.dll"
    if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) {
        Write-Host "ERROR: NVIDIA display-driver component nvcuda.dll was not found." -ForegroundColor Red
        Write-Host "The CUDA runtime is bundled, but an NVIDIA display driver is still required." -ForegroundColor Yellow
        exit 3
    }
}

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
    Assert-CudaRuntimeReady $target
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
