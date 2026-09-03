<#
.SYNOPSIS
  Builds the relocatable "MT3 Model Pack" -- an embeddable Python runtime
  plus the trimmed, DAC/EnCodec-free MT3 dependencies and the licensed
  MR-MT3 checkpoint -- at
  native/BeatShoreDesktop/python_engine/mt3_model_pack/.

.DESCRIPTION
  Reproduces, as a real script, the steps this project's own session
  verified by hand (see STATUS.md's most recent section): download the
  official Python embeddable package, enable site-packages via its
  ._pth file, copy in the already-verified, DAC/EnCodec-free
  ml_env_mt3_trimmed venv's site-packages, and bundle the real MR-MT3
  checkpoint at the exact path MT3_CHECKPOINT_DIR (see main.cpp's
  defaultMt3CheckpointDir()/runMt3Worker()) expects.

  Requires ml_env_mt3_trimmed to already exist (see STATUS.md for how it
  was built: a fresh venv with `pip install torch==2.14.0+cpu
  torchaudio==2.11.0+cpu torchvision==0.29.0+cpu --index-url
  https://download.pytorch.org/whl/cpu` then `pip install
  mt3-infer==0.2.0` -- deliberately never installing descript-audio-codec/
  descript-audiotools/encodec, which is what keeps DAC/EnCodec out) and
  a real MR-MT3 checkpoint already downloaded at
  native/BeatShoreDesktop/python_engine/.mt3_checkpoints/mr_mt3/mt3.pth
  (sha256 b8a3807ed265059abd25ad7f68142c06c35e8f6144dcaa45bd55946a3745398f
  -- verified against the registry's own declared hash, see
  mt3_infer/config/checkpoints.yaml in the trimmed venv). Neither of
  those two prerequisites is reproduced by this script -- they're the
  real, slow, one-time steps (installing packages, downloading 176MB)
  this script's own job is to package AFTER they already exist, not to
  redo them on every run.

.PARAMETER PythonVersion
  Exact embeddable package version to download. Must match
  ml_env_mt3_trimmed's own interpreter version (check
  ml_env_mt3_trimmed\pyvenv.cfg's `version` line) -- a mismatched
  embeddable Python won't load compiled extensions (.pyd files) built
  for a different CPython ABI. Default matches what this project's own
  ml_env_mt3_trimmed was built with.

.EXAMPLE
  .\build_mt3_model_pack.ps1
#>
param(
    [string]$PythonVersion = "3.14.4"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))  # scripts -> installer -> native -> repo root
$pythonEngineDir = Join-Path $root "native\BeatShoreDesktop\python_engine"
$trimmedVenv = Join-Path $pythonEngineDir "ml_env_mt3_trimmed"
$checkpointSrc = Join-Path $pythonEngineDir ".mt3_checkpoints\mr_mt3\mt3.pth"
$modelPackDir = Join-Path $pythonEngineDir "mt3_model_pack"
$expectedChecksum = "b8a3807ed265059abd25ad7f68142c06c35e8f6144dcaa45bd55946a3745398f"

function Write-Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }

if (-not (Test-Path $trimmedVenv)) {
    throw "ml_env_mt3_trimmed not found at $trimmedVenv -- build it first (see this script's own .DESCRIPTION for the exact pip commands)."
}
if (-not (Test-Path $checkpointSrc)) {
    throw "MR-MT3 checkpoint not found at $checkpointSrc -- download it first (see mt3_infer/config/checkpoints.yaml's own source_url in the trimmed venv, or run a real transcribe() once against the dev-tree checkpoint dir to let mt3-infer download it)."
}
$actualChecksum = (Get-FileHash -Path $checkpointSrc -Algorithm SHA256).Hash.ToLower()
if ($actualChecksum -ne $expectedChecksum) {
    throw "Checkpoint at $checkpointSrc has hash $actualChecksum, expected $expectedChecksum -- refusing to package a checkpoint that doesn't match the registry's own declared hash."
}
Write-Host "Prerequisites confirmed: ml_env_mt3_trimmed exists, checkpoint hash matches." -ForegroundColor Green

if (Test-Path $modelPackDir) {
    Write-Host "Removing existing $modelPackDir before rebuilding..."
    Remove-Item -Recurse -Force $modelPackDir
}
New-Item -ItemType Directory -Path $modelPackDir -Force | Out-Null

# --- 1. Download and extract the official embeddable Python package -----
Write-Step "Downloading Python $PythonVersion embeddable package"
$embedUrl = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
$embedZipPath = Join-Path $env:TEMP "beatshore-python-embed.zip"
Invoke-WebRequest -Uri $embedUrl -OutFile $embedZipPath -ErrorAction Stop
$pythonDest = Join-Path $modelPackDir "python"
New-Item -ItemType Directory -Path $pythonDest -Force | Out-Null
Expand-Archive -Path $embedZipPath -DestinationPath $pythonDest -Force
Remove-Item $embedZipPath -Force
Write-Host "Extracted to $pythonDest"

# --- 2. Enable site-packages via the ._pth file --------------------------
Write-Step "Configuring python*._pth to load Lib\site-packages"
$pthFile = Get-ChildItem -Path $pythonDest -Filter "python*._pth" | Select-Object -First 1
if (-not $pthFile) { throw "No python*._pth file found in the extracted embeddable package -- unexpected package layout." }
@"
python$($PythonVersion.Split('.')[0])$($PythonVersion.Split('.')[1]).zip
.
Lib\site-packages

# BeatShore MT3 Model Pack: enable site-packages so the bundled
# Lib\site-packages (copied verbatim from the verified, DAC/EnCodec-free
# ml_env_mt3_trimmed venv) is actually importable. Uncommented
# deliberately, not a default.
import site
"@ | Set-Content -Path $pthFile.FullName -Encoding utf8
Write-Host "Wrote $($pthFile.FullName)"

# --- 3. Copy the trimmed venv's site-packages -----------------------------
Write-Step "Copying ml_env_mt3_trimmed's site-packages (this is the slow step -- ~1.3GB)"
$sitePackagesSrc = Join-Path $trimmedVenv "Lib\site-packages"
$sitePackagesDst = Join-Path $pythonDest "Lib\site-packages"
robocopy $sitePackagesSrc $sitePackagesDst /E /R:2 /W:2 /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying site-packages (exit $LASTEXITCODE)" }
Write-Host "Copied to $sitePackagesDst"

# --- 4. Bundle the checkpoint ----------------------------------------------
Write-Step "Bundling the MR-MT3 checkpoint"
$checkpointDst = Join-Path $modelPackDir "models\mt3\mr_mt3\mt3.pth"
New-Item -ItemType Directory -Path (Split-Path -Parent $checkpointDst) -Force | Out-Null
Copy-Item -Path $checkpointSrc -Destination $checkpointDst -Force
$dstChecksum = (Get-FileHash -Path $checkpointDst -Algorithm SHA256).Hash.ToLower()
if ($dstChecksum -ne $expectedChecksum) { throw "Copied checkpoint hash mismatch -- copy corrupted?" }
Write-Host "Bundled to $checkpointDst (hash verified)"

# --- 5. Verify: real, isolated, offline inference -------------------------
# The actual acceptance bar (see STATUS.md's most recent section, and the
# user's own explicit instruction this was built against): a real
# transcription, from a copy of just this directory, with system Python
# off PATH and network calls forced to fail. Not run automatically here
# (it needs a real BSM1 fixture and takes ~15-60s for model warm-up) --
# see native/BridgeClientTest or STATUS.md for the actual verification
# commands; this script's job is to produce the package, not re-prove it
# works on every build.
Write-Host "`n=== DONE ===" -ForegroundColor Green
Write-Host "MT3 Model Pack built at $modelPackDir"
Write-Host "Verify it for real before trusting it in a release -- see STATUS.md's most recent section for the exact offline/isolated verification steps this session ran."
