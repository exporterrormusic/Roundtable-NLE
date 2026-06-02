<#
.SYNOPSIS
    Fails if any src/**/*.cpp or tests/*.cpp is not referenced by a CMakeLists.txt.

.DESCRIPTION
    The build uses EXPLICIT source lists in per-module CMakeLists.txt (no
    file(GLOB) for src/, and there is no cpp-include unity pattern). So a
    .cpp on disk whose basename appears in no CMakeLists is never compiled,
    i.e. dead code. This guardrail stops dead code from silently accumulating
    again (the root cause of the ~8k-line cleanup in POST-RELEASE-UPGRADE.txt).

    tests/ is covered too, but with a different match rule: a test is
    registered as `add_roundtable_test(<stem> ...)` (no .cpp), so an orphan
    test .cpp is one whose STEM appears in no CMakeLists. This catches dead
    test files left behind when the class they exercised was deleted.

    Allow-list: files that are intentionally uncompiled because they are
    work-in-progress for a planned feature. Keep this list TINY and justified.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/check_orphans.ps1
    Exit code 0 = clean, 1 = orphan .cpp found.
#>

$ErrorActionPreference = 'Stop'

# Repo root = parent of this script's tools/ dir.
$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir   = Join-Path $repoRoot 'src'
$testsDir = Join-Path $repoRoot 'tests'

# Intentionally-uncompiled WIP files (basename). Remove an entry once its
# .cpp is wired into a CMakeLists.
$allowList = @(
    'CaptionsPanel.cpp',   # Premiere-parity caption system (WIP) - see POST-RELEASE-UPGRADE.txt
    'CaptionClip.cpp'      # first-class caption clip type (WIP) - paired with CaptionsPanel
)

# Gather every CMakeLists.txt under the repo, excluding build/, third_party/,
# and the bundled cmake distribution under tools/cmake/.
$cmakeText = Get-ChildItem -Path $repoRoot -Recurse -Filter 'CMakeLists.txt' -File |
    Where-Object {
        $_.FullName -notmatch '[\\/]build[\\/]' -and
        $_.FullName -notmatch '[\\/]third_party[\\/]' -and
        $_.FullName -notmatch '[\\/]tools[\\/]cmake[\\/]'
    } |
    ForEach-Object { Get-Content -Raw $_.FullName } |
    Out-String

$orphans = @()

# src/: explicit source lists reference the full basename (Foo.cpp).
foreach ($cpp in Get-ChildItem -Path $srcDir -Recurse -Filter '*.cpp' -File) {
    $base = $cpp.Name
    if ($allowList -contains $base) { continue }
    # Match the basename as it appears in CMake source lists.
    if ($cmakeText -notmatch [regex]::Escape($base)) {
        $rel = $cpp.FullName.Substring($repoRoot.Length + 1) -replace '\\','/'
        $orphans += $rel
    }
}

# tests/: registered as add_roundtable_test(<stem> ...) — match the stem,
# anchored on a word boundary so e.g. test_audio doesn't shadow test_audio_sync.
if (Test-Path $testsDir) {
    foreach ($cpp in Get-ChildItem -Path $testsDir -Filter '*.cpp' -File) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($cpp.Name)
        if ($allowList -contains $cpp.Name) { continue }
        if ($cmakeText -notmatch ('\b' + [regex]::Escape($stem) + '\b')) {
            $rel = $cpp.FullName.Substring($repoRoot.Length + 1) -replace '\\','/'
            $orphans += $rel
        }
    }
}

if ($orphans.Count -gt 0) {
    Write-Host "ORPHAN .cpp files (on disk but in no CMakeLists.txt = dead code):" -ForegroundColor Red
    $orphans | Sort-Object | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Fix: add the file to its module CMakeLists.txt, delete it, or (if it is" -ForegroundColor Yellow
    Write-Host "intentional WIP) add its basename to the allow-list in tools/check_orphans.ps1." -ForegroundColor Yellow
    exit 1
}

Write-Host "check_orphans OK - every src cpp is referenced by a CMakeLists.txt." -ForegroundColor Green
exit 0
