<#
.SYNOPSIS
    Compare preview-vs-export GPU frame signatures from the #18 A/B harness.

.DESCRIPTION
    Reads framehash.csv (written when ROUNDTABLE_FRAMEHASH is set), groups rows by
    `frame`, and for every frame that has BOTH a tag=preview and tag=export row,
    compares the 256-bit signature. Identical signature => the two render paths
    produced a bit-identical frame; different => the divergence #18 must eliminate.

    A frame may legitimately have >1 distinct signature per tag (e.g. it was
    composited at different times with a moving playhead/overlay). Such frames are
    flagged "ambiguous": the set of preview signatures is compared to the set of
    export signatures, and they match only if the sets are equal.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File compare-framehash.ps1
    powershell -ExecutionPolicy Bypass -File compare-framehash.ps1 -Path other.csv -MaxList 100
#>

param(
    [string]$Path = "framehash.csv",
    [int]   $MaxList = 40
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Path)) {
    $alt = Join-Path $PSScriptRoot 'framehash.csv'
    if (Test-Path $alt) { $Path = $alt }
    else { Write-Error "framehash.csv not found (looked at '$Path' and '$alt'). Run launch-framehash.bat first."; exit 1 }
}

$rows = Import-Csv $Path
if (-not $rows) { Write-Error "No rows in $Path"; exit 1 }

# frame -> @{ preview = {sig:$true...}; export = {sig:$true...} }
$byFrame = @{}
foreach ($r in $rows) {
    $tag = $r.tag
    if ($tag -ne 'preview' -and $tag -ne 'export') { continue }
    $sig = @($r.s0,$r.s1,$r.s2,$r.s3,$r.s4,$r.s5,$r.s6,$r.s7) -join '-'
    $f   = $r.frame
    if (-not $byFrame.ContainsKey($f)) { $byFrame[$f] = @{ preview = @{}; export = @{} } }
    $byFrame[$f][$tag][$sig] = $true
}

$both = 0; $match = 0; $diverge = 0; $prevOnly = 0; $expOnly = 0; $ambiguous = 0
$diverged = New-Object System.Collections.ArrayList

foreach ($f in $byFrame.Keys) {
    $p = @($byFrame[$f].preview.Keys)
    $e = @($byFrame[$f].export.Keys)

    if ($p.Count -eq 0) { $expOnly++;  continue }
    if ($e.Count -eq 0) { $prevOnly++; continue }

    $both++
    if ($p.Count -gt 1 -or $e.Count -gt 1) { $ambiguous++ }

    $pSet = $p | Sort-Object
    $eSet = $e | Sort-Object
    $same = ($p.Count -eq $e.Count) -and (-not (Compare-Object $pSet $eSet))

    if ($same) {
        $match++
    } else {
        $diverge++
        [void]$diverged.Add([pscustomobject]@{
            frame       = [int64]$f
            previewSigs = $p.Count
            exportSigs  = $e.Count
        })
    }
}

Write-Host ""
Write-Host "framehash A/B compare: $Path" -ForegroundColor Cyan
Write-Host ("  rows: {0}   distinct frames: {1}" -f $rows.Count, $byFrame.Count)
Write-Host ("  comparable (both preview+export): {0}" -f $both)
if ($both -gt 0) {
    $pct = [math]::Round(100.0 * $match / $both, 1)
    $col = if ($diverge -eq 0) { 'Green' } else { 'Yellow' }
    Write-Host ("    MATCH (bit-identical): {0}  ({1}%)" -f $match, $pct) -ForegroundColor $col
    Write-Host ("    DIVERGED:              {0}" -f $diverge) -ForegroundColor $col
}
Write-Host ("  preview-only frames: {0}   export-only frames: {1}" -f $prevOnly, $expOnly)
Write-Host ("  ambiguous (>1 distinct sig per tag, e.g. overlay/playhead): {0}" -f $ambiguous)

if ($both -eq 0) {
    Write-Host ""
    Write-Host "No frame had BOTH a preview and an export signature." -ForegroundColor Yellow
    Write-Host "Run an export AND a preview pass over the SAME frames (frame-step preview" -ForegroundColor Yellow
    Write-Host "so its ticks equal export's frameToTick(N))." -ForegroundColor Yellow
}
elseif ($diverge -gt 0) {
    Write-Host ""
    Write-Host ("Diverged frames (first {0}) -- the #18 baseline:" -f $MaxList)
    $diverged | Sort-Object frame | Select-Object -First $MaxList | Format-Table -AutoSize
}
