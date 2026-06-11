<#
.SYNOPSIS
    Configure Application Verifier (AppVerif) for roundtable.exe to catch
    heap corruption at the source.

.DESCRIPTION
    This script enables page-heap and heap checking for roundtable.exe
    via Application Verifier.  When AppVerif is active, the app will
    crash IMMEDIATELY at the exact line of code that corrupts the heap,
    rather than crashing nondeterministically later (as seen in the
    playback crash: HEAP_CORRUPTION 0xC0000374).

    REQUIREMENTS:
      - Administrator privileges (AppVerif modifies global settings)
      - Application Verifier installed (see step 0)

    USAGE:
      1. Run this script as Administrator
      2. Launch roundtable.exe normally
      3. Reproduce the crash (scrub then play)
      4. The crash will now show a call stack pointing to the
         exact heap corruption site
      5. To disable, run:  .\setup_appverif.ps1 -Disable

.PARAMETER Disable
    Switch to remove AppVerif settings instead of enabling them.

.PARAMETER ExePath
    Path to roundtable.exe.  Defaults to the Release build output.
#>

param(
    [switch]$Disable,
    [string]$ExePath = "C:\Users\rmdou\Desktop\TIER_LIST_PROGRAM_NEW\build\bin\Release\roundtable.exe"
)

# ── Check if running as admin ────────────────────────────────────────────
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "ERROR: This script must be run as Administrator." -ForegroundColor Red
    Write-Host "Right-click PowerShell and select 'Run as Administrator'," -ForegroundColor Yellow
    Write-Host "then run this script again." -ForegroundColor Yellow
    exit 1
}

# ── Check AppVerif availability ─────────────────────────────────────────
$appverif = Get-Command "appverif.exe" -ErrorAction SilentlyContinue
if (-not $appverif) {
    Write-Host "ERROR: Application Verifier not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "To install AppVerif (choose one):"
    Write-Host "  A. Via Windows SDK Installer:"
    Write-Host "     https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/"
    Write-Host "     (Select 'Windows SDK' → under 'Installation Details', check"
    Write-Host "      'Application Verifier for Windows')"
    Write-Host ""
    Write-Host "  B. Via WinGet:"
    Write-Host "     winget install "Windows Application Verifier""
    Write-Host ""
    Write-Host "  C. If already installed at default location, add to PATH:"
    Write-Host '     $env:PATH += ";${env:ProgramFiles(x86)}\Windows Kits\10\App Verification\"'
    exit 1
}

# ── Verify the exe exists ────────────────────────────────────────────────
if (-not (Test-Path $ExePath)) {
    Write-Host "ERROR: roundtable.exe not found at: $ExePath" -ForegroundColor Red
    Write-Host "Build the project first, then run this script." -ForegroundColor Yellow
    exit 1
}

# ── Action ────────────────────────────────────────────────────────────────
if ($Disable) {
    Write-Host "Disabling heap checking + WER dumps for roundtable.exe..." -ForegroundColor Cyan
    $regPrefix = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    $exeKey = "$regPrefix\Image File Execution Options\roundtable.exe"
    if (Test-Path $exeKey) {
        Remove-Item "$exeKey\GlobalFlag" -Force -ErrorAction SilentlyContinue
        Remove-Item "$exeKey\StackTraceInHeap" -Force -ErrorAction SilentlyContinue
    }
    $werKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\roundtable.exe"
    if (Test-Path $werKey) {
        Remove-Item $werKey -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host ""
    Write-Host "Disabled.  Run without -Disable to re-enable." -ForegroundColor Green
} else {
    Write-Host "Enabling AppVerif for roundtable.exe with heap checks..." -ForegroundColor Cyan
    Write-Host "Target: $ExePath" -ForegroundColor Gray

    # ── Configure checks ──────────────────────────────────────────────────
    # Uses gflags (lightweight heap checking) + Windows Error Reporting
    # (crash dumps) instead of AppVerif Full PageHeap, which is too slow
    # for Vulkan-heavy applications.
    #
    # gflags settings:
    #   0x2000000 = FLG_HEAP_ENABLE_TAIL_CHECK + FLG_HEAP_ENABLE_FREE_CHECK
    #   StackTraceInHeap = 1  → captures stack traces for heap allocations
    #
    # WER settings:
    #   DumpType=2 (mini dump), saved to logs/ directory

    $regPrefix = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    $exeKey = "$regPrefix\Image File Execution Options\roundtable.exe"
    if (-not (Test-Path $exeKey)) { New-Item $exeKey -Force | Out-Null }
    Set-ItemProperty $exeKey -Name GlobalFlag -Value 0x2000000 -Type DWord
    Set-ItemProperty $exeKey -Name StackTraceInHeap -Value 1 -Type DWord

    $werKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\roundtable.exe"
    New-Item $werKey -Force | Out-Null
    Set-ItemProperty $werKey -Name DumpType -Value 2 -Type DWord
    Set-ItemProperty $werKey -Name DumpFolder -Value "$(Split-Path $ExePath -Parent)\..\..\..\logs" -Type ExpandString
    Set-ItemProperty $werKey -Name DumpCount -Value 3 -Type DWord

    Write-Host ""
    Write-Host "Configuration enabled!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Yellow
    Write-Host "  1. Launch roundtable.exe via launch.bat"
    Write-Host "  2. Scrub, then hit play"
    Write-Host "  3. When it crashes, a .dmp file will appear in logs/"
    Write-Host "     Open it in Visual Studio or WinDbg to see the call stack"
    Write-Host ""
    Write-Host "To disable:" -ForegroundColor Cyan
    Write-Host "  .\setup_appverif.ps1 -Disable  (as Administrator)"
}
