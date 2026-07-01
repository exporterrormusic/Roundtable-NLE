# ROUNDTABLE NLE v2 - Portable Setup Script
# ============================================================================
# This script makes the project fully self-contained and buildable on any
# Windows machine with Visual Studio 2022 + C++ Desktop workload + Git + Python.
#
# It installs INTO THE PROJECT DIRECTORY (no system-wide changes):
#   1. Qt 6.8.3 (via aqtinstall) into third_party/qt/
#   2. glslc shader compiler (from shaderc) into tools/shaderc/
#   3. Clears stale build cache and reconfigures
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup.ps1
# ============================================================================

$ErrorActionPreference = "Stop"
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $projectDir

Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  ROUNDTABLE NLE v2 - Portable Setup" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

# --- Detect Visual Studio 2022 and CMake ---
Write-Host "[1/5] Detecting Visual Studio 2022 and CMake..." -ForegroundColor Yellow

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "  ERROR: vswhere not found. Install Visual Studio 2022 with C++ Desktop workload." -ForegroundColor Red
    exit 1
}

$vsInstallDir = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
if (-not $vsInstallDir) {
    Write-Host "  ERROR: Visual Studio 2022 with C++ workload not found." -ForegroundColor Red
    Write-Host "  Install Visual Studio 2022 Community and select 'Desktop development with C++'." -ForegroundColor Red
    exit 1
}
Write-Host "  VS2022 found: $vsInstallDir" -ForegroundColor Green

# Find CMake (prefer local, then VS2022, then system PATH)
$cmakeExe = $null
$cmakeMinVer = [version]"3.28.0"

# 1. Check local project cmake first
$localCmake = Join-Path $projectDir "tools\cmake\cmake-*\bin\cmake.exe"
$localCmakeFile = Get-ChildItem $localCmake -ErrorAction SilentlyContinue | Select-Object -First 1
if ($localCmakeFile) {
    $cmakeExe = $localCmakeFile.FullName
}

# 2. Try VS2022 bundled cmake
if (-not $cmakeExe) {
    $vsCmakeFile = Get-ChildItem "$vsInstallDir\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -ErrorAction SilentlyContinue
    if ($vsCmakeFile) { $cmakeExe = $vsCmakeFile.FullName }
}

# 3. Try system PATH
if (-not $cmakeExe) {
    $sysCmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($sysCmake) { $cmakeExe = $sysCmake.Source }
}

if (-not $cmakeExe) {
    Write-Host "  No CMake found anywhere. Will download." -ForegroundColor DarkYellow
    $needCmakeDownload = $true
} else {
    # Check version
    $cmakeVerOutput = & $cmakeExe --version 2>&1 | Select-Object -First 1
    if ($cmakeVerOutput -match '(\d+\.\d+\.\d+)') {
        $cmakeVer = [version]$Matches[1]
        if ($cmakeVer -lt $cmakeMinVer) {
            Write-Host "  CMake found but version $cmakeVer < required $cmakeMinVer" -ForegroundColor DarkYellow
            $needCmakeDownload = $true
        } else {
            Write-Host "  CMake $cmakeVer found: $cmakeExe" -ForegroundColor Green
            $needCmakeDownload = $false
        }
    } else {
        Write-Host "  Could not determine CMake version. Will download." -ForegroundColor DarkYellow
        $needCmakeDownload = $true
    }
}

# Download CMake if needed
if ($needCmakeDownload) {
    $cmakeDownloadVer = "3.31.6"
    $cmakeZipUrl = "https://github.com/Kitware/CMake/releases/download/v$cmakeDownloadVer/cmake-$cmakeDownloadVer-windows-x86_64.zip"
    $cmakeLocalDir = Join-Path $projectDir "tools\cmake"
    $cmakeZipPath = Join-Path $env:TEMP "cmake.zip"

    Write-Host "  Downloading CMake $cmakeDownloadVer (portable, ~43MB)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try {
        Invoke-WebRequest -Uri $cmakeZipUrl -OutFile $cmakeZipPath -UseBasicParsing -TimeoutSec 120
        Write-Host "  Extracting CMake (this may take a minute)..."
        New-Item -ItemType Directory -Path $cmakeLocalDir -Force | Out-Null
        Expand-Archive -Path $cmakeZipPath -DestinationPath $cmakeLocalDir -Force
        Remove-Item $cmakeZipPath -ErrorAction SilentlyContinue

        $localCmakeFile = Get-ChildItem "$cmakeLocalDir\cmake-*\bin\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($localCmakeFile) {
            $cmakeExe = $localCmakeFile.FullName
            Write-Host "  CMake $cmakeDownloadVer installed: $cmakeExe" -ForegroundColor Green
        } else {
            Write-Host "  ERROR: CMake extraction failed." -ForegroundColor Red
            exit 1
        }
    } catch {
        Write-Host "  ERROR: Failed to download CMake: $_" -ForegroundColor Red
        exit 1
    }
}

# --- Install Qt 6.8.3 locally ---
Write-Host ""
Write-Host "[2/5] Setting up Qt 6.8.3..." -ForegroundColor Yellow

$qtLocalDir = Join-Path $projectDir "third_party\qt"
$qtInstallDir = Join-Path $qtLocalDir "6.8.3\msvc2022_64"

if (Test-Path (Join-Path $qtInstallDir "lib\cmake\Qt6\Qt6Config.cmake")) {
    Write-Host "  Qt 6.8.3 already installed at: $qtInstallDir" -ForegroundColor Green
} else {
    Write-Host "  Installing Qt 6.8.3 via aqtinstall (this may take a few minutes)..."

    # Install aqtinstall if not present
    $aqtInstalled = $false
    try {
        $aqtCheck = pip show aqtinstall 2>&1
        if ($LASTEXITCODE -eq 0) { $aqtInstalled = $true }
    } catch { }
    if (-not $aqtInstalled) {
        Write-Host "  Installing aqtinstall..."
        pip install aqtinstall --user 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  pip --user failed, trying with elevated permissions..."
            pip install aqtinstall 2>&1 | Out-Null
        }
    }

    # Create directory and install Qt
    New-Item -ItemType Directory -Path $qtLocalDir -Force | Out-Null

    Write-Host "  Downloading Qt 6.8.3 for MSVC 2022 x64 (this may take 2-5 minutes)..."
    # Temporarily allow stderr output (aqt writes INFO to stderr)
    $origEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir $qtLocalDir 2>&1 | ForEach-Object { Write-Host "    $_" }
    $aqtExitCode = $LASTEXITCODE
    $ErrorActionPreference = $origEAP
    if ($aqtExitCode -ne 0) {
        Write-Host "  ERROR: Qt installation failed (exit code $aqtExitCode)." -ForegroundColor Red
        exit 1
    }

    if (Test-Path (Join-Path $qtInstallDir "lib\cmake\Qt6\Qt6Config.cmake")) {
        Write-Host "  Qt 6.8.3 installed successfully." -ForegroundColor Green
    } else {
        Write-Host "  ERROR: Qt installation failed. Check network and try again." -ForegroundColor Red
        exit 1
    }
}

# --- Download glslc (Shader Compiler) ---
Write-Host ""
Write-Host "[3/5] Setting up glslc shader compiler..." -ForegroundColor Yellow

$shadercDir = Join-Path $projectDir "tools\shaderc"
$glslcExe = Join-Path $shadercDir "glslc.exe"

if (Test-Path $glslcExe) {
    Write-Host "  glslc already present at: $glslcExe" -ForegroundColor Green
} else {
    Write-Host "  Downloading shaderc (glslc) from Google shaderc builds..." -ForegroundColor White
    New-Item -ItemType Directory -Path $shadercDir -Force | Out-Null

    $shadercArchive = $null
    $extractDir = Join-Path $env:TEMP "shaderc_extract"

    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

        # Resolve the latest build URL from the shaderc badge redirect page
        $badgeUrl = "https://storage.googleapis.com/shaderc/badges/build_link_windows_vs2022_amd64_release.html"
        Write-Host "  Resolving latest shaderc build..."
        $badgePage = (Invoke-WebRequest -Uri $badgeUrl -UseBasicParsing -TimeoutSec 15).Content
        $downloadUrl = $null
        if ($badgePage -match 'url=(https://[^"''\s>]+)') {
            $downloadUrl = $Matches[1]
        }

        if (-not $downloadUrl) {
            throw "Could not resolve download URL from badge page"
        }

        # Determine archive type from URL
        $isTgz = $downloadUrl -match '\.tgz$' -or $downloadUrl -match '\.tar\.gz$'
        if ($isTgz) {
            $shadercArchive = Join-Path $env:TEMP "shaderc-install.tgz"
        } else {
            $shadercArchive = Join-Path $env:TEMP "shaderc-install.zip"
        }

        Write-Host "  Downloading: $downloadUrl"
        # Follow redirects and download the actual binary
        Invoke-WebRequest -Uri $downloadUrl -OutFile $shadercArchive -UseBasicParsing -TimeoutSec 120 -MaximumRedirection 5

        # Verify we got a real binary (not an HTML error page)
        $archiveSize = (Get-Item $shadercArchive).Length
        if ($archiveSize -lt 100000) {
            # Probably an error page, not a real archive
            $firstBytes = Get-Content $shadercArchive -Raw -ErrorAction SilentlyContinue
            if ($firstBytes -match '<' -or $firstBytes -match 'NoSuchKey') {
                throw "Downloaded file appears to be an error page (${archiveSize} bytes), not a valid archive"
            }
        }

        # Extract
        Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue
        if ($isTgz) {
            # Use tar to extract .tgz (available on Windows 10+)
            New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
            tar -xzf $shadercArchive -C $extractDir 2>&1 | Out-Null
        } else {
            Expand-Archive -Path $shadercArchive -DestinationPath $extractDir -Force
        }

        $foundGlslc = Get-ChildItem $extractDir -Recurse -Filter "glslc.exe" | Select-Object -First 1
        if ($foundGlslc) {
            Copy-Item $foundGlslc.FullName $glslcExe
            Write-Host "  glslc installed." -ForegroundColor Green
        } else {
            throw "glslc.exe not found in archive"
        }
    } catch {
        Write-Host "  WARNING: Could not download shaderc automatically." -ForegroundColor DarkYellow
        Write-Host "  Error: $_" -ForegroundColor DarkYellow
        Write-Host "  Shaders will not be compiled to SPIR-V. The build will still work." -ForegroundColor DarkYellow
        Write-Host "  To fix later: download glslc.exe from the Vulkan SDK or shaderc releases" -ForegroundColor DarkYellow
        Write-Host "  and place it in tools\shaderc\" -ForegroundColor DarkYellow
        Write-Host "  Source: https://github.com/google/shaderc/blob/main/downloads.md" -ForegroundColor DarkYellow
    } finally {
        Remove-Item $shadercArchive -ErrorAction SilentlyContinue
        Remove-Item $extractDir -Recurse -ErrorAction SilentlyContinue
    }
}

# --- OmniShotCut AI Scene Detection ---
Write-Host ""
Write-Host "[4/6] Setting up OmniShotCut AI scene detection..." -ForegroundColor Yellow

$omnishotDir = Join-Path $projectDir "tools\omnishotcut"
$omnishotRepo = Join-Path $omnishotDir "repo"
$omnishotCheckpointDir = Join-Path $omnishotDir "checkpoints"
$omnishotCheckpoint = Join-Path $omnishotCheckpointDir "OmniShotCut_ckpt.pth"

# Clone repo if not present
if (-not (Test-Path (Join-Path $omnishotRepo ".git"))) {
    Write-Host "  Cloning OmniShotCut repo..." -ForegroundColor Yellow
    Remove-Item $omnishotRepo -Recurse -Force -ErrorAction SilentlyContinue
    $prevErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    git clone https://github.com/UVA-Computer-Vision-Lab/OmniShotCut.git $omnishotRepo 2>&1 | Out-Null
    $cloneOk = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $prevErrorAction
    if ($cloneOk) {
        Write-Host "  OmniShotCut repo cloned." -ForegroundColor Green
    } else {
        Write-Host "  WARNING: Failed to clone OmniShotCut. AI detection unavailable." -ForegroundColor DarkYellow
    }
} else {
    Write-Host "  OmniShotCut repo already present." -ForegroundColor Green
}

# Install Python dependencies
$pythonExe = Get-Command python -ErrorAction SilentlyContinue
if ($pythonExe) {
    Write-Host "  Installing Python dependencies (CUDA)..." -ForegroundColor Yellow
    $prevErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    pip install torch torchvision opencv-python numpy --index-url https://download.pytorch.org/whl/cu124 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        pip install torch torchvision opencv-python numpy 2>&1 | Out-Null
        Write-Host "  PyTorch CPU-only installed (CUDA not available)." -ForegroundColor DarkYellow
    }
    $ErrorActionPreference = $prevErrorAction
    Write-Host "  Python deps installed." -ForegroundColor Green

    # Download checkpoint
    if (-not (Test-Path $omnishotCheckpoint)) {
        New-Item -ItemType Directory -Path $omnishotCheckpointDir -Force | Out-Null
        Write-Host "  Downloading OmniShotCut checkpoint..." -ForegroundColor Yellow
        try {
            Invoke-WebRequest -Uri "https://huggingface.co/uva-cv-lab/OmniShotCut/resolve/main/OmniShotCut_ckpt.pth" `
                -OutFile $omnishotCheckpoint -UseBasicParsing -TimeoutSec 300
            $ckptSize = (Get-Item $omnishotCheckpoint).Length
            if ($ckptSize -gt 1000000) {
                Write-Host "  Checkpoint downloaded ($([math]::Round($ckptSize/1MB, 1)) MB)." -ForegroundColor Green
            } else {
                Remove-Item $omnishotCheckpoint -Force
                Write-Host "  WARNING: Checkpoint download appears invalid. AI detection unavailable." -ForegroundColor DarkYellow
            }
        } catch {
            Write-Host "  WARNING: Failed to download checkpoint. AI detection unavailable." -ForegroundColor DarkYellow
        }
    } else {
        Write-Host "  Checkpoint already present." -ForegroundColor Green
    }
} else {
    Write-Host "  WARNING: Python not found. AI scene detection will be unavailable." -ForegroundColor DarkYellow
}

# --- Clean stale build directory ---
Write-Host ""
Write-Host "[5/6] Cleaning stale build cache..." -ForegroundColor Yellow

$buildDir = Join-Path $projectDir "build"
$cmakeCacheFile = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path $cmakeCacheFile) {
    $needClean = $false
    $cacheContent = Get-Content $cmakeCacheFile -Raw
    $currentPath = $projectDir -replace '\\', '/'

    # Check if cache has wrong paths (copied from another machine)
    if ($cacheContent -notmatch [regex]::Escape($currentPath)) {
        Write-Host "  Build cache has paths from another machine. Cleaning..."
        $needClean = $true
    }

    # Check if generator or platform changed (e.g. Ninja vs VS, x64 mismatch)
    if (-not $needClean) {
        $cachedGenerator = if ($cacheContent -match 'CMAKE_GENERATOR:INTERNAL=(.+)') { $Matches[1].Trim() } else { '' }
        $cachedPlatform  = if ($cacheContent -match 'CMAKE_GENERATOR_PLATFORM:INTERNAL=(.*)') { $Matches[1].Trim() } else { '' }
        if ($cachedGenerator -and $cachedGenerator -ne 'Visual Studio 17 2022') {
            Write-Host "  Generator changed ($cachedGenerator -> Visual Studio 17 2022). Cleaning..."
            $needClean = $true
        } elseif ($cachedPlatform -ne 'x64') {
            Write-Host "  Platform changed ('$cachedPlatform' -> x64). Cleaning..."
            $needClean = $true
        }
    }

    if ($needClean) {
        Remove-Item $cmakeCacheFile -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $buildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $buildDir "_deps") -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "  Stale build cache cleaned." -ForegroundColor Green
    } else {
        Write-Host "  Build cache looks correct. Keeping existing build." -ForegroundColor Green
    }
} else {
    Write-Host "  No stale build cache found." -ForegroundColor Green
}

# --- Configure CMake ---
Write-Host ""
Write-Host "[6/6] Configuring CMake..." -ForegroundColor Yellow

$qtCmakeDir = (Join-Path $qtInstallDir "lib\cmake\Qt6") -replace '\\', '/'
$glslcDir = $shadercDir -replace '\\', '/'

# Build the cmake command
$cmakeArgs = @(
    "-B", "build",
    "-S", ".",
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_PREFIX_PATH:PATH=$qtCmakeDir/../..",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DROUNDTABLE_DEV_BUILD=ON"
)

# Add glslc path if available
if (Test-Path $glslcExe) {
    $glslcPath = $glslcExe -replace '\\', '/'
    $cmakeArgs += "-DGLSLC_EXECUTABLE=$glslcPath"
}

# Prepend bundled cmake directory to PATH so FetchContent sub-builds
# (e.g. spdlog which requires cmake >= 3.31) also find the correct version.
$cmakeBinDir = Split-Path -Parent $cmakeExe
$env:PATH = "${cmakeBinDir};${env:PATH}"

Write-Host "  Running: cmake $($cmakeArgs -join ' ')"
& $cmakeExe @cmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  ERROR: CMake configuration failed." -ForegroundColor Red
    Write-Host "  Check the output above for details." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "================================================================" -ForegroundColor Green
Write-Host "  Setup Complete!" -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  To build:" -ForegroundColor White
Write-Host "    .\build.ps1" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Or manually:" -ForegroundColor White
Write-Host "    & '$cmakeExe' --build build --config Release --parallel" -ForegroundColor Cyan
Write-Host ""
Write-Host "  To run:" -ForegroundColor White
Write-Host "    .\launch.vbs" -ForegroundColor Cyan
Write-Host ""

Pop-Location
