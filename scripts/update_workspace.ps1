# ROUNDTABLE NLE - Update Default Workspace Layout
# ============================================================================
# Reads the current "USE_AS_DEFAULT" workspace preset directly from the
# Windows registry (QSettings native format), converts it to the bundled
# default_layout.bin binary format, and writes it to assets/.
#
# Does NOT launch the app — reads the registry directly.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File update_workspace.ps1
#   update_workspace.bat
# ============================================================================

$ErrorActionPreference = "Stop"
# This script lives in scripts\; the repo root (project dir) is one level up.
$projectDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $projectDir

# ═══════════════════════════════════════════════════════════════════════════
# QSettings registry path
# Dev builds use ("ROUNDTABLE", "NLE-Dev"); release uses ("ROUNDTABLE", "NLE").
# See src/core/Settings.h — appSettings().
# ═══════════════════════════════════════════════════════════════════════════
$regBases = @(
    "HKCU:\Software\ROUNDTABLE\NLE-Dev",
    "HKCU:\Software\ROUNDTABLE\NLE"
)

$presetPath = $null
foreach ($base in $regBases) {
    $candidate = "$base\WorkspacePresets\USE_AS_DEFAULT"
    if (Test-Path $candidate) {
        $presetPath = $candidate
        Write-Host "  Found preset at: $presetPath" -ForegroundColor Gray
        break
    }
}

if (-not $presetPath) {
    Write-Host "ERROR: USE_AS_DEFAULT preset not found in registry." -ForegroundColor Red
    Write-Host ""
    Write-Host "Checked:" -ForegroundColor Gray
    foreach ($base in $regBases) {
        Write-Host "  $base\WorkspacePresets\USE_AS_DEFAULT" -ForegroundColor Gray
    }
    Write-Host ""
    Write-Host "To create it:" -ForegroundColor Yellow
    Write-Host "  1. Launch Roundtable" -ForegroundColor Gray
    Write-Host "  2. Open the Timeline page" -ForegroundColor Gray
    Write-Host "  3. Arrange your panels how you want" -ForegroundColor Gray
    Write-Host "  4. Window -> Workspace -> Save Workspace as..." -ForegroundColor Gray
    Write-Host "  5. Name it: USE_AS_DEFAULT" -ForegroundColor Gray
    Pop-Location
    exit 1
}

Write-Host "Reading USE_AS_DEFAULT preset from registry..." -ForegroundColor Cyan

# ═══════════════════════════════════════════════════════════════════════════
# Helper: Convert a byte array to QSettings INI @ByteArray(\xNN...) format
# ═══════════════════════════════════════════════════════════════════════════
function ConvertTo-QByteArrayIni([byte[]]$bytes) {
    if (-not $bytes -or $bytes.Count -eq 0) {
        return '@ByteArray()'
    }
    $hex = ($bytes | ForEach-Object { '\x' + $_.ToString('x2') }) -join ''
    return '@ByteArray(' + $hex + ')'
}

# ═══════════════════════════════════════════════════════════════════════════
# Helper: Check if a registry key represents a QSettings list (QStringList
# or QVariantList). These have a "size" value and numbered child values.
# ═══════════════════════════════════════════════════════════════════════════
function Test-IsQSettingsList($keyPath) {
    $size = Get-ItemProperty -Path $keyPath -Name 'size' -ErrorAction SilentlyContinue
    if (-not $size) { return $false }
    $testChild = Get-ItemProperty -Path $keyPath -Name '1' -ErrorAction SilentlyContinue
    return $testChild -ne $null
}

# ═══════════════════════════════════════════════════════════════════════════
# Helper: Read a QSettings QStringList from the registry.
# Stored as: size=N (REG_DWORD), 1=val1 (REG_SZ), 2=val2 (REG_SZ), ...
# Returns INI-format: val1, val2, val3
# ═══════════════════════════════════════════════════════════════════════════
function Read-QStringList($keyPath) {
    $size = (Get-ItemProperty -Path $keyPath -Name 'size' -ErrorAction SilentlyContinue).size
    if (-not $size) { return '' }
    $values = @()
    for ($i = 1; $i -le $size; $i++) {
        $val = (Get-ItemProperty -Path $keyPath -Name ([string]$i) -ErrorAction SilentlyContinue).([string]$i)
        if ($val) { $values += $val }
    }
    return ($values -join ', ')
}

# ═══════════════════════════════════════════════════════════════════════════
# Helper: Read a QSettings QVariantList from the registry.
# Stored as: size=N (REG_DWORD), 1=val1 (REG_DWORD), 2=val2 (REG_DWORD), ...
# Returns array of values
# ═══════════════════════════════════════════════════════════════════════════
function Read-QVariantList($keyPath) {
    $size = (Get-ItemProperty -Path $keyPath -Name 'size' -ErrorAction SilentlyContinue).size
    if (-not $size) { return @() }
    $values = @()
    for ($i = 1; $i -le $size; $i++) {
        $val = (Get-ItemProperty -Path $keyPath -Name ([string]$i) -ErrorAction SilentlyContinue).([string]$i)
        if ($null -ne $val) { $values += $val }
    }
    return $values
}

# ═══════════════════════════════════════════════════════════════════════════
# Main converter: recursively walk a registry key and produce INI lines.
# Returns an array of strings, each like "key=value".
# ═══════════════════════════════════════════════════════════════════════════
function Convert-RegistryToIni($keyPath, [string]$iniPrefix) {
    $lines = @()

    # ── Process direct values (leaf values in this key) ────────────────
    $props = Get-ItemProperty -Path $keyPath -ErrorAction SilentlyContinue
    if ($props) {
        $propNames = $props.PSObject.Properties | Where-Object {
            $_.Name -notmatch '^PS(ChildName|Drive|ParentPath|Path|Provider)$'
        } | ForEach-Object { $_.Name }

        foreach ($name in $propNames) {
            # Skip the "size" key — it's a list indicator
            if ($name -eq 'size') { continue }
            # Skip numbered keys that are part of a list
            if ($name -match '^\d+$') { continue }

            $val = $props.$name
            $fullKey = if ($iniPrefix) { "$iniPrefix\$name" } else { $name }

            if ($val -is [byte[]]) {
                $lines += "$fullKey=`"$(ConvertTo-QByteArrayIni $val)`""
            } elseif ($val -is [int]) {
                $lines += "$fullKey=$val"
            } elseif ($val -is [string]) {
                $lines += "$fullKey=$val"
            } elseif ($val -is [string[]]) {
                $lines += "$fullKey=$($val -join ', ')"
            } else {
                Write-Host "  WARNING: Unknown type $($val.GetType().Name) for $fullKey" -ForegroundColor Yellow
                $lines += "$fullKey=$val"
            }
        }
    }

    # ── Process subkeys ────────────────────────────────────────────────
    $subkeys = Get-ChildItem -Path $keyPath -ErrorAction SilentlyContinue
    foreach ($subkey in $subkeys) {
        $subName = $subkey.PSChildName
        $subPath = $subkey.PSPath
        $fullKey = if ($iniPrefix) { "$iniPrefix\$subName" } else { $subName }

        if (Test-IsQSettingsList $subPath) {
            # Check if it's a QStringList (REG_SZ children) or QVariantList (REG_DWORD children)
            $firstChild = (Get-ItemProperty -Path $subPath -Name '1' -ErrorAction SilentlyContinue).'1'

            if ($firstChild -is [int]) {
                # QVariantList → numbered subkeys in INI
                $listValues = Read-QVariantList $subPath
                for ($i = 0; $i -lt $listValues.Count; $i++) {
                    $idx = $i + 1
                    $lines += "$fullKey\$idx=$($listValues[$i])"
                }
                $lines += "$fullKey\size=$($listValues.Count)"
            } else {
                # QStringList → comma-separated
                $listStr = Read-QStringList $subPath
                $lines += "$fullKey=$listStr"
            }
        } else {
            # Regular sub-group — recurse
            $subLines = Convert-RegistryToIni $subPath $fullKey
            $lines += $subLines
        }
    }

    return $lines
}

# ═══════════════════════════════════════════════════════════════════════════
# Build the INI content
# ═══════════════════════════════════════════════════════════════════════════
Write-Host "  Converting registry keys to INI format..." -ForegroundColor Gray

$iniLines = @()
$iniLines += '[workspace/last_session]'

# Dummy geometry/activePage/navCollapsed — these are window-state keys
# that saveWorkspaceToFile always writes. They're not critical for the
# dock layout but loaders expect them to exist.
$iniLines += 'geometry="@ByteArray()"'
$iniLines += 'activePage=0'
$iniLines += 'navCollapsed=false'

# Convert all preset keys
$presetLines = Convert-RegistryToIni $presetPath ''
$iniLines += $presetLines

$iniContent = ($iniLines -join "`r`n") + "`r`n"

# ═══════════════════════════════════════════════════════════════════════════
# Write the binary file
# ═══════════════════════════════════════════════════════════════════════════
$outputFile = Join-Path $projectDir "assets\default_layout.bin"
$assetsDir = Join-Path $projectDir "assets"

if (-not (Test-Path $assetsDir)) {
    New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null
}

# Show existing file info
if (Test-Path $outputFile) {
    $existingSize = (Get-Item $outputFile).Length
    Write-Host "  Existing default_layout.bin: $existingSize bytes" -ForegroundColor Gray
} else {
    Write-Host "  No existing default_layout.bin (will create new)" -ForegroundColor Gray
}

# Convert INI to UTF-8 bytes
$iniBytes = [System.Text.Encoding]::UTF8.GetBytes($iniContent)
$iniLength = $iniBytes.Length

# QDataStream format: quint32 big-endian length + raw bytes
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# Write big-endian uint32 length
$bw.Write([byte]((($iniLength -shr 24) -band 0xFF)))
$bw.Write([byte]((($iniLength -shr 16) -band 0xFF)))
$bw.Write([byte]((($iniLength -shr 8) -band 0xFF)))
$bw.Write([byte](($iniLength -band 0xFF)))

# Write the INI bytes
$bw.Write($iniBytes)
$bw.Flush()

[System.IO.File]::WriteAllBytes($outputFile, $ms.ToArray())
$bw.Close()
$ms.Close()

$finalSize = (Get-Item $outputFile).Length
Write-Host ""
Write-Host "SUCCESS: Default workspace layout updated!" -ForegroundColor Green
Write-Host "  File:    assets\default_layout.bin ($finalSize bytes)" -ForegroundColor Gray
Write-Host "  Preset:  USE_AS_DEFAULT ($($iniBytes.Length) bytes INI, $($presetLines.Count) keys)" -ForegroundColor Gray
Write-Host ""
Write-Host "This layout will now:" -ForegroundColor Cyan
Write-Host "  - Be used for 'Reset to Default Layout' in the dev build" -ForegroundColor Gray
Write-Host "  - Be bundled with the installer for new users" -ForegroundColor Gray
Write-Host ""
Write-Host "Run update_workspace.bat again whenever you re-save USE_AS_DEFAULT." -ForegroundColor Yellow

Pop-Location
