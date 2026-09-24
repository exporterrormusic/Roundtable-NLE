param(
    [switch]$SkipBreeze,
    [switch]$SkipFish,
    [switch]$SkipOmniVoice,
    [switch]$SkipModels
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeRoot = Join-Path $repoRoot '.voice-runtime'
$bootstrapPython = Join-Path $repoRoot '.venv\Scripts\python.exe'
$fishRoot = Join-Path $runtimeRoot 'fish-speech'
$omniRoot = Join-Path $runtimeRoot 'omnivoice-src'
$breezeRoot = Join-Path $runtimeRoot 'breeze'
$modelsRoot = Join-Path $runtimeRoot 'models'
$downloadsRoot = Join-Path $runtimeRoot 'downloads'

function Download-File {
    param([string]$Url, [string]$Destination)
    if (Test-Path -LiteralPath $Destination) { return }
    $partial = "$Destination.partial"
    Write-Host "Downloading $([IO.Path]::GetFileName($Destination))..."
    & curl.exe -L --fail --retry 5 --retry-delay 3 -C - -o $partial $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
}

# Upstream revisions this app was tested against. The worker imports Fish
# internals, so an unpinned clone can break on any upstream refactor.
$fishRevision = 'befe4001745417f8c42131739d862b8a6fdbd15a'
$omniRevision = '08be0b4ccbac3e13e374e86fbfead4b4cac343e2'
$fishModelRevision = '1de9996b6be38b745688de084d87a5633f714e4e'
$omniModelRevision = 'c5fdb5ccb189668d56333f77ba2629f4cd7535f4'

function Sync-PinnedRepository {
    param([string]$Url, [string]$Destination, [string]$Revision)
    if (-not (Test-Path -LiteralPath (Join-Path $Destination '.git'))) {
        git init --quiet $Destination
        if ($LASTEXITCODE -ne 0) { throw "git init failed: $Destination" }
        git -C $Destination remote add origin $Url
    }
    $current = git -C $Destination rev-parse HEAD 2>$null
    if ($current -eq $Revision) { return }
    git -C $Destination fetch --depth 1 origin $Revision
    if ($LASTEXITCODE -ne 0) { throw "Could not fetch $Url at $Revision" }
    git -C $Destination checkout --quiet --force FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "Could not check out $Revision in $Destination" }
}

function Get-ReleaseAsset {
    param([string]$Repository, [string]$Tag, [string]$Pattern)
    $headers = @{
        'User-Agent' = 'ROUNDTABLE-Voice-Installer'
        'Accept' = 'application/vnd.github+json'
    }
    $release = Invoke-RestMethod -Headers $headers `
        -Uri "https://api.github.com/repos/$Repository/releases/tags/$Tag"
    $asset = $release.assets | Where-Object { $_.name -like $Pattern } | Select-Object -First 1
    if (-not $asset) {
        throw "No release asset matching '$Pattern' in $Repository $Tag"
    }
    return $asset
}

function Confirm-AssetHash {
    param($Asset, [string]$Archive)
    if ($Asset.digest -and $Asset.digest.StartsWith('sha256:')) {
        $actual = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $Asset.digest.Substring(7).ToLowerInvariant()) {
            throw "Checksum mismatch: $($Asset.name)"
        }
    }
}

if (-not (Test-Path -LiteralPath $bootstrapPython)) {
    throw "The bundled bootstrap Python was not found: $bootstrapPython"
}
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required. Install uv, then rerun this script.'
}

New-Item -ItemType Directory -Force -Path $runtimeRoot, $modelsRoot, $downloadsRoot | Out-Null

if (-not $SkipBreeze) {
    Write-Host 'Installing Breeze-TTS-2 native CUDA runtime...'
    $breezeVenv = Join-Path $breezeRoot '.venv'
    $breezePython = Join-Path $breezeVenv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $breezePython)) {
        uv venv $breezeVenv --python $bootstrapPython
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the Breeze adapter environment.' }
    }

    $audioCppVersion = 'v0.7.2'
    $audioCppRoot = Join-Path $breezeRoot 'audio-cpp'
    $audioCppServer = Join-Path $audioCppRoot 'audiocpp_server.exe'
    if (-not (Test-Path -LiteralPath $audioCppServer)) {
        $binaryAsset = Get-ReleaseAsset '0xShug0/audio.cpp' $audioCppVersion `
            'audio-*-bin-windows-x64-cuda12.4.zip'
        $runtimeAsset = Get-ReleaseAsset '0xShug0/audio.cpp' $audioCppVersion `
            'audio-*-cudart-windows-x64-cuda12.4.zip'
        $binaryArchive = Join-Path $downloadsRoot $binaryAsset.name
        $runtimeArchive = Join-Path $downloadsRoot $runtimeAsset.name
        Download-File $binaryAsset.browser_download_url $binaryArchive
        Download-File $runtimeAsset.browser_download_url $runtimeArchive
        Confirm-AssetHash $binaryAsset $binaryArchive
        Confirm-AssetHash $runtimeAsset $runtimeArchive
        New-Item -ItemType Directory -Force -Path $audioCppRoot | Out-Null
        Expand-Archive -LiteralPath $binaryArchive -DestinationPath $audioCppRoot -Force
        Expand-Archive -LiteralPath $runtimeArchive -DestinationPath $audioCppRoot -Force
    }

    if (-not $SkipModels) {
        $breezeModel = Join-Path $modelsRoot 'breeze-tts-2-q8_0.gguf'
        Download-File `
            'https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/Breeze-TTS-2-GGUF/breeze-tts-2-q8_0.gguf?download=true' `
            $breezeModel
    }
}

if (-not $SkipFish) {
    Sync-PinnedRepository 'https://github.com/fishaudio/fish-speech.git' $fishRoot $fishRevision
    Write-Host 'Installing Fish S2 Pro CUDA runtime...'
    uv sync --project $fishRoot --extra cu128 --python $bootstrapPython
    if (-not $SkipModels) {
        $fishPython = Join-Path $fishRoot '.venv\Scripts\python.exe'
        $fishModel = Join-Path $fishRoot 'checkpoints\s2-pro'
        & $fishPython -c "from huggingface_hub import snapshot_download; snapshot_download('fishaudio/s2-pro', revision='$fishModelRevision', local_dir=r'$fishModel')"
    }
}

if (-not $SkipOmniVoice) {
    Sync-PinnedRepository 'https://github.com/k2-fsa/OmniVoice.git' $omniRoot $omniRevision
    Write-Host 'Installing OmniVoice CUDA runtime...'
    uv sync --project $omniRoot --python $bootstrapPython
    if (-not $SkipModels) {
        $omniPython = Join-Path $omniRoot '.venv\Scripts\python.exe'
        $omniModel = Join-Path $modelsRoot 'omnivoice'
        & $omniPython -c "from huggingface_hub import snapshot_download; snapshot_download('k2-fsa/OmniVoice', revision='$omniModelRevision', local_dir=r'$omniModel')"
    }
}

Write-Host 'Voice runtimes installed successfully.'
