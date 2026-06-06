<#
.SYNOPSIS
    Commit + push your changes to git, with an optional version tag.
.DESCRIPTION
    Lightweight counterpart to publish_release.ps1 - it does NOT build the
    installer or create a GitHub Release. It just:
      1. Stages everything and commits
      2. Pushes the branch to origin
      3. OPTIONALLY tags a version (only if you provide one)

    The version tag here is just a git bookmark - it is NOT the published
    setup.exe version. Leave it blank to simply push your files.
#>

param(
    [string]$Version = "",
    [string]$Message = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

# Make sure we're in a git repo
git rev-parse --is-inside-work-tree *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Not a git repository." -ForegroundColor Red
    exit 1
}

# Current branch
$branch = (git rev-parse --abbrev-ref HEAD).Trim()

# --- Commit message -----------------------------------------------------
if (-not $Message) {
    $Message = Read-Host "Commit message (blank = 'Update')"
    if (-not $Message) { $Message = "Update" }
}

# --- Optional version tag ----------------------------------------------
if (-not $Version) {
    $lastTag = git describe --tags --abbrev=0 2>$null
    if ($lastTag) { Write-Host "Last tag: $lastTag" -ForegroundColor DarkGray }
    $Version = Read-Host "Optional version tag (blank = just push, no tag)"
}
$Version = $Version.Trim().TrimStart('v')

$tag = ""
$reuseTag = $false
if ($Version) {
    $tag = "v$Version"
    $existing = git tag -l $tag
    if ($existing) {
        if (-not $Force) {
            $ans = Read-Host "Tag $tag already exists. Move it to the new commit and force-push? (y/N)"
            if ($ans -notmatch '^(y|yes)$') {
                Write-Host "Will push without changing the tag." -ForegroundColor Yellow
                $tag = ""
            } else {
                $reuseTag = $true
            }
        } else {
            $reuseTag = $true
        }
    }
}

Write-Host ""
Write-Host "Branch : $branch"  -ForegroundColor Cyan
Write-Host "Commit : $Message"  -ForegroundColor Cyan
if ($tag) { Write-Host "Tag    : $tag" -ForegroundColor Cyan } else { Write-Host "Tag    : (none)" -ForegroundColor DarkGray }
Write-Host ""

# --- Stage + commit (skip commit if nothing changed) -------------------
git add -A
$pending = git status --porcelain
if ($pending) {
    git commit -m $Message
    if ($LASTEXITCODE -ne 0) { Write-Host "Commit failed." -ForegroundColor Red; exit 1 }
} else {
    Write-Host "No changes to commit - pushing the current commit." -ForegroundColor Yellow
}

# --- Tag (only if requested) -------------------------------------------
if ($tag) {
    if ($reuseTag) { git tag -f $tag } else { git tag $tag }
    if ($LASTEXITCODE -ne 0) { Write-Host "Tag failed." -ForegroundColor Red; exit 1 }
}

# --- Push branch -------------------------------------------------------
Write-Host ""
Write-Host "Pushing $branch ..." -ForegroundColor Yellow
git push origin $branch
if ($LASTEXITCODE -ne 0) { Write-Host "Push of branch failed." -ForegroundColor Red; exit 1 }

# --- Push tag (only if requested) --------------------------------------
if ($tag) {
    Write-Host "Pushing $tag ..." -ForegroundColor Yellow
    if ($reuseTag) { git push origin $tag --force } else { git push origin $tag }
    if ($LASTEXITCODE -ne 0) { Write-Host "Push of tag failed." -ForegroundColor Red; exit 1 }
}

Write-Host ""
if ($tag) {
    Write-Host "Pushed $branch and $tag to origin." -ForegroundColor Green
} else {
    Write-Host "Pushed $branch to origin." -ForegroundColor Green
}
