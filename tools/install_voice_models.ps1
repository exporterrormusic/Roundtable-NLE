param(
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
$modelsRoot = Join-Path $runtimeRoot 'models'

if (-not (Test-Path -LiteralPath $bootstrapPython)) {
    throw "The bundled bootstrap Python was not found: $bootstrapPython"
}
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required. Install uv, then rerun this script.'
}

New-Item -ItemType Directory -Force -Path $runtimeRoot, $modelsRoot | Out-Null

if (-not $SkipFish) {
    if (-not (Test-Path -LiteralPath (Join-Path $fishRoot '.git'))) {
        git clone --depth 1 https://github.com/fishaudio/fish-speech.git $fishRoot
    }
    Write-Host 'Installing Fish S2 Pro CUDA runtime...'
    uv sync --project $fishRoot --extra cu128 --python $bootstrapPython
    if (-not $SkipModels) {
        $fishPython = Join-Path $fishRoot '.venv\Scripts\python.exe'
        $fishModel = Join-Path $fishRoot 'checkpoints\s2-pro'
        & $fishPython -c "from huggingface_hub import snapshot_download; snapshot_download('fishaudio/s2-pro', local_dir=r'$fishModel')"
    }
}

if (-not $SkipOmniVoice) {
    if (-not (Test-Path -LiteralPath (Join-Path $omniRoot '.git'))) {
        git clone --depth 1 https://github.com/k2-fsa/OmniVoice.git $omniRoot
    }
    Write-Host 'Installing OmniVoice CUDA runtime...'
    uv sync --project $omniRoot --python $bootstrapPython
    if (-not $SkipModels) {
        $omniPython = Join-Path $omniRoot '.venv\Scripts\python.exe'
        $omniModel = Join-Path $modelsRoot 'omnivoice'
        & $omniPython -c "from huggingface_hub import snapshot_download; snapshot_download('k2-fsa/OmniVoice', local_dir=r'$omniModel')"
    }
}

Write-Host 'Voice runtimes installed successfully.'
