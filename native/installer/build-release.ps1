<#
.SYNOPSIS
  Reproducible BeatShore release build: rebuilds both native binaries from
  source, restages them unconditionally, runs the real test suite against
  the actual staged tree (not the dev build directories), compiles the
  installer, and writes out every hash a release manifest needs.

.DESCRIPTION
  Replaces the manual "build, remember to copy, hope you copied the right
  thing" process this project used through 2026-08-24 -- which produced a
  real bug that session: a compiled installer was staged from a build
  that predated the source changes it was supposed to contain, caught only
  by manually comparing file timestamps after the fact. This script makes
  that specific class of bug structurally impossible rather than trusting
  a human to remember a copy step: the staging block below ALWAYS rebuilds
  both binaries and ALWAYS overwrites the staged copies immediately before
  testing/packaging, on every run, with no "has anything changed" check to
  get wrong. If a build step fails, or a test against the freshly-staged
  binaries fails, the script stops (non-zero exit) before ever compiling
  or reporting an installer as ready -- it does not compile a "best
  effort" installer from a partially-broken tree.

  This script does NOT:
    - run `npm ci` / re-trim the engine's node_modules by default (that
      step is slow -- several minutes -- and the currently-staged engine
      tree was already verified this session; pass -CleanEngine to redo
      it from scratch when the engine's own dependencies actually change,
      not on every routine desktop/plugin rebuild).
    - sign anything (no certificate is available in the environment this
      script was written in; -SigntoolPath / -CertThumbprint are accepted
      and, if supplied, used -- see the Signing section below -- but the
      default behavior with neither supplied is to skip signing loudly,
      not silently produce an unsigned build while claiming otherwise).
    - hand-edit RELEASE_MANIFEST.md's prose. It writes a structured
      machine-readable report (release-report.json, alongside this
      script's own console output) with every value a manifest update
      needs -- filenames, sizes, every SHA-256, the staging-manifest
      hash, the build ID, pass/fail per verification step -- so updating
      the manifest is a copy of real numbers, not free-form re-derivation.
      The manifest's narrative sections (what changed and why) are still
      written by a person (or an LLM working from this report), since
      that's the part that actually needs judgment.
    - touch a clean Windows VM, sign with a real certificate, or perform
      any of the human/legal/hardware-dependent release gates (clean-
      machine install, live REAPER, other DAWs, code signing, EULA/JUCE
      legal review, icon design) -- see STATUS.md for the current, honest
      status of all of those. This script only makes the *build* step
      reproducible; it doesn't shrink the list of things a human still
      needs to do before a public release.

.PARAMETER CleanEngine
  Also rebuild native/BeatShoreDesktop/engine/node_modules from scratch
  (npm ci --omit=dev using the pinned Node 24 toolchain in
  native/installer/tools/node24) and re-apply the verified tfjs-node trim
  (deps/, build-tmp-napi-v8/, source maps) before restaging. Slow (several
  minutes). Only needed when engine dependencies themselves changed.

.PARAMETER SkipInstallerCompile
  Runs every build/restage/verification step but stops before invoking
  Inno Setup -- useful for a fast "did I break anything" check without
  waiting on the ~90s compile.

.PARAMETER SigntoolPath
  Path to signtool.exe. If supplied together with -CertThumbprint, each
  produced binary (BeatShoreDesktop.exe, the VST3, the compiled installer)
  is signed and timestamped after its own build/compile step, and the
  hashes recorded in release-report.json are of the SIGNED binaries (per
  the standing project rule: hashes must be regenerated after signing,
  since signing changes them). If omitted, signing is skipped and the
  report says so explicitly rather than staying silent about it.

.PARAMETER CertThumbprint
  Certificate thumbprint to pass to signtool (see -SigntoolPath).

.PARAMETER VcVarsPath
  Explicit path to vcvars64.bat, overriding auto-detection via vswhere.exe.
  Auto-detection finds any VS2017+ install with the C++ workload, on this
  machine or a CI runner alike -- only needed if that picks the wrong one
  among multiple side-by-side VS installs.

.PARAMETER IsccPath
  Explicit path to ISCC.exe (Inno Setup 6), overriding auto-detection (PATH,
  then the standard Program Files / LOCALAPPDATA install locations).

.EXAMPLE
  .\build-release.ps1
  Fast path: rebuild both binaries, restage, verify against staged tree,
  compile the installer, unsigned.

.EXAMPLE
  .\build-release.ps1 -CleanEngine
  Full path: also rebuild the engine's node_modules from scratch first.
#>
param(
    [switch]$CleanEngine,
    [switch]$SkipInstallerCompile,
    [string]$SigntoolPath = "",
    [string]$CertThumbprint = "",
    # Override auto-detection below if it picks the wrong VS install (e.g.
    # multiple side-by-side versions) or Inno Setup isn't at a location
    # vswhere/the standard install paths would find.
    [string]$VcVarsPath = "",
    [string]$IsccPath = ""
)

# NOT "Stop": this script drives many native .exe processes (ninja,
# ISCC, node, the various test harnesses) whose own stderr output --
# including entirely benign warnings, e.g. vcvars64.bat's own
# "'vswhere.exe' is not recognized" line, which doesn't stop it from
# working -- gets wrapped into a terminating PowerShell error the moment
# it's captured via `2>&1` under $ErrorActionPreference = "Stop" (a
# documented PowerShell 5.1 quirk). Every step below that must actually
# halt the script on real failure checks $LASTEXITCODE explicitly instead
# and throws with a clear message -- that's the actual failure gate, not
# this preference. Cmdlets where a genuine failure must stop the script
# (Copy-Item, Remove-Item, Get-FileHash, Get-Item) pass -ErrorAction Stop
# explicitly at each call site instead of relying on this global setting.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # native/installer -> native -> repo root
$native = Join-Path $root "native"
$installer = Join-Path $native "installer"
$stage = Join-Path $installer "stage"

# Auto-detected, not hardcoded to this machine's own local install paths --
# a previous version of this script hardcoded a specific local VS2019
# Build Tools path and a specific local Inno Setup install path, which
# worked here but would silently fail to find either tool on any other
# machine, including a CI runner. vswhere.exe is the tool Visual Studio
# itself ships specifically so scripts don't have to guess an install
# path -- present on every VS2017+ install (including GitHub-hosted
# windows-latest runners), at this fixed location regardless of VS
# version, even though vswhere itself usually isn't on PATH.
function Find-VcVars64 {
    if ($VcVarsPath) { return $VcVarsPath }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsInstallPath) {
            $candidate = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "Could not locate vcvars64.bat -- pass -VcVarsPath explicitly, or install the 'Desktop development with C++' workload."
}

function Find-Iscc {
    if ($IsccPath) { return $IsccPath }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($candidate in @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Could not locate ISCC.exe (Inno Setup 6) -- pass -IsccPath explicitly, or install Inno Setup (e.g. 'choco install innosetup')."
}

$vcvars = Find-VcVars64
$iscc = Find-Iscc
Write-Host "Using vcvars64.bat: $vcvars"
Write-Host "Using ISCC.exe: $iscc"

$report = [ordered]@{
    startedUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    steps = [ordered]@{}
    artifacts = [ordered]@{}
    signed = $false
}

function Write-Step($name) {
    Write-Host ""
    Write-Host "=== $name ===" -ForegroundColor Cyan
}

function Invoke-NinjaBuild($sourceDir, $buildDir, $label) {
    Write-Step "Building $label ($buildDir)"

    # CMake-configure first, unconditionally -- this is a real fix, not
    # defensive padding. Every prior version of this script assumed
    # $buildDir already existed and was already configured (true on the
    # machine this script was originally developed on, where every build
    # directory had been configured interactively long before this
    # script existed) and just ran `ninja` directly. On a genuinely
    # fresh checkout -- exactly what a CI runner does -- $buildDir
    # doesn't exist at all (it's gitignored, see the repo root
    # .gitignore's own comment), so `ninja` alone fails immediately
    # ("loading 'build.ninja': ... No such file or directory"). `cmake
    # -B` is idempotent and fast when a build directory is already
    # correctly configured, so running it unconditionally doesn't cost
    # anything on a machine where it's already set up -- it only matters
    # on a fresh one, which is exactly the case that was untested before
    # a real CI run surfaced it.
    if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }
    Push-Location $buildDir
    try {
        $configureCmdLine = '"' + $vcvars + '" && cmake -S "' + $sourceDir + '" -B "' + $buildDir + '" -G Ninja -DCMAKE_BUILD_TYPE=Release'
        $configureOutput = cmd /c $configureCmdLine 2>&1
        $configureOutput | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "$label CMake configure failed (exit $LASTEXITCODE)" }

        $cmdLine = '"' + $vcvars + '" && ninja'
        $output = cmd /c $cmdLine 2>&1
        $output | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "$label build failed (ninja exit $LASTEXITCODE)" }
        $warnings = $output | Select-String -Pattern "warning C\d" -CaseSensitive:$false
        if ($warnings) {
            Write-Host "WARNING: $label build produced $($warnings.Count) compiler warning(s):" -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        }
        $report.steps["build_$label"] = @{ status = "ok"; warnings = $warnings.Count }
    }
    finally { Pop-Location }
}

function Get-Sha256($path) {
    (Get-FileHash -Path $path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
}

function Sign-IfConfigured($path, $label) {
    if ([string]::IsNullOrEmpty($SigntoolPath) -or [string]::IsNullOrEmpty($CertThumbprint)) {
        Write-Host "Signing skipped for $label (no -SigntoolPath/-CertThumbprint supplied)." -ForegroundColor Yellow
        return $false
    }
    Write-Step "Signing $label"
    & $SigntoolPath sign /sha1 $CertThumbprint /tr http://timestamp.digicert.com /td sha256 /fd sha256 $path
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $label (exit $LASTEXITCODE)" }
    & $SigntoolPath verify /pa /v $path
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $label -- signed but not valid, treat as a failed release" }
    Write-Host "$label signed and verified." -ForegroundColor Green
    return $true
}

# --- 1. Rebuild both native binaries from source -----------------------
Invoke-NinjaBuild (Join-Path $native "BeatShoreDesktop") (Join-Path $native "BeatShoreDesktop\build") "BeatShoreDesktop"
Invoke-NinjaBuild (Join-Path $native "BeatShoreBridge") (Join-Path $native "BeatShoreBridge\build") "BeatShoreBridge"

# The VST3 Validator and the BridgeClientTest/MultiSessionTest/
# SchedulerTest suite were, until this fix, assumed to already be built
# -- true on the machine this script was developed on (both had been
# built interactively long before this script existed) but never
# actually true on a fresh checkout, which is exactly what surfaced this
# on a real CI run: `native/vst3sdk/build` and
# `native/BridgeClientTest/build` don't exist at all until something
# configures and builds them. Real CMake flags, not guessed -- read
# directly from this machine's own already-configured
# native/vst3sdk/build/CMakeCache.txt (SMTG_ADD_VST3_UTILITIES=ON is
# what actually produces validator.exe; hosting/plugin examples and
# VSTGUI support are explicitly OFF, keeping this build to just what's
# needed rather than pulling in a much larger default configuration).
Write-Step "Configuring and building the VST3 SDK Validator"
$vst3sdkBuild = Join-Path $native "vst3sdk\build"
if (-not (Test-Path $vst3sdkBuild)) { New-Item -ItemType Directory -Path $vst3sdkBuild -Force | Out-Null }
Push-Location $vst3sdkBuild
try {
    $vst3sdkConfigureCmd = '"' + $vcvars + '" && cmake -S "' + (Join-Path $native "vst3sdk") + '" -B "' + $vst3sdkBuild + '" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSMTG_ADD_VST3_UTILITIES=ON -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_RUN_VST_VALIDATOR=ON'
    $vst3sdkConfigureOut = cmd /c $vst3sdkConfigureCmd 2>&1
    $vst3sdkConfigureOut | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "vst3sdk CMake configure failed (exit $LASTEXITCODE)" }
    $vst3sdkBuildCmd = '"' + $vcvars + '" && ninja validator'
    $vst3sdkBuildOut = cmd /c $vst3sdkBuildCmd 2>&1
    $vst3sdkBuildOut | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "vst3sdk validator build failed (ninja exit $LASTEXITCODE)" }
    $report.steps["build_validator"] = @{ status = "ok" }
}
finally { Pop-Location }

# BridgeClientTest.exe/MultiSessionTest.exe -- needed by the staged
# regression suite below -- and SchedulerTest.exe/LoadBoundaryTest.exe,
# which this script doesn't currently run itself but which real
# developers rely on existing after a build. `ninja` alone (no target
# name) builds every target this CMakeLists.txt defines.
Invoke-NinjaBuild (Join-Path $native "BridgeClientTest") (Join-Path $native "BridgeClientTest\build") "BridgeClientTest"

$desktopExeSrc = Join-Path $native "BeatShoreDesktop\build\BeatShoreDesktop.exe"
$vst3Src = Join-Path $native "BeatShoreBridge\build\BeatShoreBridge_artefacts\Release\VST3\BeatShore Bridge.vst3"
if (-not (Test-Path $desktopExeSrc)) { throw "BeatShoreDesktop.exe not found after build -- $desktopExeSrc" }
if (-not (Test-Path $vst3Src)) { throw "BeatShore Bridge.vst3 not found after build -- $vst3Src" }

# --- 2. Steinberg Validator against the FRESH build, not the stale staged one ---
Write-Step "Steinberg Validator"
$validator = Join-Path $native "vst3sdk\build\bin\validator.exe"
$validatorOut = & $validator $vst3Src 2>&1
$validatorOut | ForEach-Object { Write-Host $_ }
# $validatorOut is an array of lines -- -match/-notmatch against an ARRAY
# filters element-by-element and returns the non-matching elements (a
# non-empty, therefore truthy, array in almost any real case), NOT a
# boolean "did any line match" test. Join to a single string first so the
# operator does what it looks like it does.
if (($validatorOut -join "`n") -notmatch "0 tests failed") { throw "Steinberg Validator reported failures -- refusing to package" }
$report.steps["validator"] = @{ status = "ok" }

# --- 3. Optional: rebuild the engine's node_modules from scratch -------
# $engineDir is used both here and by the always-runs engine-staging
# step below (3.5) -- defined unconditionally, not just inside this
# -CleanEngine branch, which was a real bug in an earlier version of
# this fix (the staging step would have failed on the common fast path
# where -CleanEngine isn't passed, since $engineDir would never have
# been set at all).
$engineDir = Join-Path $native "BeatShoreDesktop\engine"
if ($CleanEngine) {
    Write-Step "Rebuilding engine/node_modules from scratch (npm ci --omit=dev, Node 24)"
    $node24 = Join-Path $installer "tools\node24"
    Push-Location $engineDir
    try {
        & (Join-Path $node24 "npm.cmd") ci --omit=dev
        if ($LASTEXITCODE -ne 0) { throw "npm ci failed (exit $LASTEXITCODE)" }
        # The verified trim: tfjs-node's build-time-only staging dir and
        # every source map, neither loaded at runtime -- see STATUS.md's
        # "Installer size" section for how these were identified as safe
        # to remove, not guessed at here.
        $tfjsDeps = Join-Path $engineDir "node_modules\@tensorflow\tfjs-node\deps"
        if (Test-Path $tfjsDeps) { Remove-Item -Recurse -Force $tfjsDeps -ErrorAction Stop }
        # Same category as deps/ above -- node-gyp's own build-time
        # MSBuild scaffolding (.tlog/.obj/.vcxproj files), never touched
        # once the native addon is already compiled, and never trimmed
        # before because this project's staging never actually copied
        # the engine tree until this session (the manually-assembled
        # stage\ this project used until now happened to have been built
        # from an already-trimmed copy). Also has deeply nested, long
        # filenames (MSBuild .tlog files carry a build-hash in their own
        # name) that genuinely broke both robocopy and Inno Setup's
        # compiler with Windows MAX_PATH errors once this WAS staged for
        # the first time -- not a hypothetical, the actual failure this
        # fix responds to.
        $tfjsBuildTmp = Join-Path $engineDir "node_modules\@tensorflow\tfjs-node\build-tmp-napi-v8"
        if (Test-Path $tfjsBuildTmp) { Remove-Item -Recurse -Force $tfjsBuildTmp -ErrorAction Stop }
        Get-ChildItem -Path (Join-Path $engineDir "node_modules") -Recurse -Filter "*.map" -File |
            Remove-Item -Force
        & (Join-Path $node24 "node.exe") "scripts\fix-tfjs-node-binding.js"
        if ($LASTEXITCODE -ne 0) { throw "fix-tfjs-node-binding.js postinstall repair failed" }
    }
    finally { Pop-Location }
    $report.steps["clean_engine"] = @{ status = "ok" }
}
else {
    $report.steps["clean_engine"] = @{ status = "skipped"; reason = "-CleanEngine not passed" }
}

# --- 3.5. Stage everything that ISN'T produced by compiling -------------
# The real bug a live CI run found: this script restaged the two
# COMPILED binaries but never actually staged anything else -- the EULA,
# third-party license texts, the engine's JS source + node_modules, the
# bundled Node.js runtime, and the VC++ redistributable were all sitting
# in stage\ only because they'd been assembled there by hand across many
# earlier sessions on this one machine. stage\ is entirely gitignored
# (it's ~400MB of generated content), so none of that groundwork existed
# on a genuinely fresh checkout -- this script was never actually
# reproducible from scratch, it just happened to work here. Fixed in two
# parts: real source-of-truth text content (the EULA, license notices)
# moved into native/installer/assets/ and committed to git, copied from
# there unconditionally every run (same principle as the binary restage
# below); and the large, externally-sourced binaries (Node.js runtime,
# VC++ redistributable) fetched fresh whenever they're not already
# present, rather than assumed to pre-exist.
Write-Step "Staging EULA and third-party license notices"
$assets = Join-Path $installer "assets"
Copy-Item -Path (Join-Path $assets "LicenseFile.txt") -Destination (Join-Path $stage "LicenseFile.txt") -Force -ErrorAction Stop
$licensesDest = Join-Path $stage "Licenses"
if (-not (Test-Path $licensesDest)) { New-Item -ItemType Directory -Path $licensesDest -Force | Out-Null }
Copy-Item -Path (Join-Path $assets "Licenses\*") -Destination $licensesDest -Recurse -Force -ErrorAction Stop

Write-Step "Staging the analysis engine (source + node_modules)"
$engineStageRoot = Join-Path $stage "engine"
$engineNestedDest = Join-Path $engineStageRoot "native\BeatShoreDesktop\engine"
if (-not (Test-Path $engineNestedDest)) { New-Item -ItemType Directory -Path $engineNestedDest -Force | Out-Null }
# Mirrors the dev tree's own nesting depth exactly -- analyze.js's own
# unmodified `../../../beatshore-dsp.js` import (native/BeatShoreDesktop/
# engine/analyze.js -> ../../../beatshore-dsp.js) only resolves correctly
# if beatshore-dsp.js sits exactly three directories above wherever
# analyze.js is staged. See STATUS.md's "Installer size" section for why
# this nesting exists -- not incidental structure, a real constraint an
# earlier naive "just copy engine/ into the installer" attempt failed
# against.
# /R:2 /W:2 -- a real bug in the first version of this fix: robocopy's
# DEFAULT retry behavior (no /R or /W given) is up to 1,000,000 retries
# with a 30-second wait between each for any file it can't copy on the
# first try. One transiently locked or long-path file in an ~8,000-file
# node_modules tree is enough to hang this step for a very long time --
# confirmed directly: it genuinely hung on this exact call before this
# fix, not a hypothetical concern.
#
# /XD napi-v10 -- a second real bug, found once the retry limit above
# stopped the hang and surfaced the actual error: robocopy couldn't copy
# tfjs-node's own lib/napi-v10/tensorflow.dll (ERROR 3, path not found --
# almost certainly a broken/dangling artifact from npm's own install,
# not anything this project created). This is already a known,
# documented dead file, not a functional dependency: tfjs-node 4.22.0
# doesn't ship a real prebuilt binding for N-API v10, only a symlink-ish
# placeholder; the actual working binding this project uses is
# lib/napi-v8/ (see fix-tfjs-node-binding.js's own comment). Excluding
# it is correct, not a workaround for something that mattered --
# confirmed by reproducing the exact failure directly, excluding just
# this one directory, and getting a clean robocopy exit code
# afterward.
# build-tmp-napi-v8 excluded too here, not just in the -CleanEngine trim
# above -- this runs regardless of whether -CleanEngine was passed (the
# dev tree's node_modules might already exist from an earlier session,
# from before this exclusion existed), so staging-time exclusion is the
# one place that reliably catches it either way.
robocopy $engineDir $engineNestedDest /E /R:2 /W:2 /XD napi-v10 /XD build-tmp-napi-v8 /NFL /NDL /NJH /NJS /NC /NS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "staging engine/ into stage\engine\native\BeatShoreDesktop\engine failed (robocopy exit $LASTEXITCODE)" }
Copy-Item -Path (Join-Path $root "beatshore-dsp.js") -Destination (Join-Path $engineStageRoot "beatshore-dsp.js") -Force -ErrorAction Stop
Copy-Item -Path (Join-Path $root "package.json") -Destination (Join-Path $engineStageRoot "package.json") -Force -ErrorAction Stop
$vendorDest = Join-Path $engineStageRoot "vendor"
if (Test-Path $vendorDest) { Remove-Item -Recurse -Force $vendorDest -ErrorAction Stop }
Copy-Item -Path (Join-Path $root "vendor") -Destination $vendorDest -Recurse -Force -ErrorAction Stop

Write-Step "Staging the bundled Node.js runtime and VC++ redistributable"
$nodeDest = Join-Path $stage "node\node.exe"
if (-not (Test-Path $nodeDest)) {
    Write-Host "node.exe not already staged -- downloading Node 24.19.0 LTS (pinned, matching the engine's own dev toolchain)..."
    $nodeZipUrl = "https://nodejs.org/dist/v24.19.0/node-v24.19.0-win-x64.zip"
    $nodeZipPath = Join-Path $env:TEMP "beatshore-node-runtime.zip"
    Invoke-WebRequest -Uri $nodeZipUrl -OutFile $nodeZipPath -ErrorAction Stop
    $nodeExtractDir = Join-Path $env:TEMP "beatshore-node-runtime-extract"
    if (Test-Path $nodeExtractDir) { Remove-Item -Recurse -Force $nodeExtractDir }
    Expand-Archive -Path $nodeZipPath -DestinationPath $nodeExtractDir -Force
    $nodeExeSrc = Get-ChildItem -Path $nodeExtractDir -Filter "node.exe" -Recurse | Select-Object -First 1
    if (-not $nodeExeSrc) { throw "node.exe not found inside downloaded Node.js zip" }
    New-Item -ItemType Directory -Path (Join-Path $stage "node") -Force | Out-Null
    Copy-Item -Path $nodeExeSrc.FullName -Destination $nodeDest -Force -ErrorAction Stop
    Remove-Item $nodeZipPath, $nodeExtractDir -Recurse -Force -ErrorAction SilentlyContinue
}
else { Write-Host "node.exe already staged, skipping download." }

$vcRedistDest = Join-Path $stage "vc_redist.x64.exe"
if (-not (Test-Path $vcRedistDest)) {
    Write-Host "vc_redist.x64.exe not already staged -- downloading from Microsoft..."
    Invoke-WebRequest -Uri "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $vcRedistDest -ErrorAction Stop
    # Real verification, not "downloaded successfully so it must be
    # fine" -- confirms this is genuinely Microsoft-signed before it
    # ships inside the installer, matching the check already done by
    # hand for every prior build (see RELEASE_MANIFEST.md).
    $sig = Get-AuthenticodeSignature -FilePath $vcRedistDest
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft") {
        Remove-Item $vcRedistDest -Force
        throw "vc_redist.x64.exe failed Authenticode verification (status: $($sig.Status)) -- refusing to stage an unverified redistributable"
    }
    Write-Host "vc_redist.x64.exe Authenticode-verified: $($sig.SignerCertificate.Subject)"
}
else { Write-Host "vc_redist.x64.exe already staged, skipping download." }

# --- 4. Restage, UNCONDITIONALLY -- this is the actual fix for the bug --
# this session found: never trust that a previously-staged binary is
# current. Always overwrite it with what was just built, every run.
Write-Step "Restaging BeatShoreDesktop.exe and BeatShore Bridge.vst3 (unconditional)"
Copy-Item -Path $desktopExeSrc -Destination (Join-Path $stage "BeatShoreDesktop.exe") -Force -ErrorAction Stop
$vst3Dest = Join-Path $stage "BeatShore Bridge.vst3"
if (Test-Path $vst3Dest) { Remove-Item -Recurse -Force $vst3Dest -ErrorAction Stop }
Copy-Item -Path $vst3Src -Destination $vst3Dest -Recurse -Force -ErrorAction Stop
Write-Host "Restaged. Staged copies are now byte-identical to what was just built (verified below)."

# --- 5. Validate the required directory layout --------------------------
Write-Step "Validating staged directory layout"
$requiredPaths = @(
    "BeatShoreDesktop.exe",
    "BeatShore Bridge.vst3\Contents\x86_64-win\BeatShore Bridge.vst3",
    "node\node.exe",
    "engine\native\BeatShoreDesktop\engine\analyze.js",
    "engine\beatshore-dsp.js",
    "engine\package.json",
    "LicenseFile.txt",
    "Licenses\README.txt",
    "vc_redist.x64.exe"
)
$missing = @()
foreach ($p in $requiredPaths) {
    $full = Join-Path $stage $p
    if (-not (Test-Path $full)) { $missing += $p }
}
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Host "MISSING: $_" -ForegroundColor Red }
    throw "Staged tree is missing $($missing.Count) required file(s) -- refusing to package"
}
Write-Host "All $($requiredPaths.Count) required staged paths present." -ForegroundColor Green
$report.steps["layout_validation"] = @{ status = "ok"; checkedPaths = $requiredPaths.Count }

# Immediate self-check: the staged binary must actually match what was
# just built -- this is the specific guarantee the earlier manual process
# lacked, checked explicitly rather than assumed from the copy above
# having "looked" successful.
$freshHash = Get-Sha256 $desktopExeSrc
$stagedHash = Get-Sha256 (Join-Path $stage "BeatShoreDesktop.exe")
if ($freshHash -ne $stagedHash) { throw "Staged BeatShoreDesktop.exe hash does not match the freshly-built one -- restage failed silently, refusing to continue" }

# --- 6. Self-test against the STAGED tree, not the dev build dir --------
Write-Step "Running --self-test against the staged tree"
Push-Location $stage
try {
    $selfTestOut = & ".\BeatShoreDesktop.exe" --self-test "engine\native\BeatShoreDesktop\engine\analyze.js" 2>&1
    $selfTestOut | ForEach-Object { Write-Host $_ }
    $selfTestExit = $LASTEXITCODE
}
finally { Pop-Location }
if ($selfTestExit -ne 0) { throw "Staged self-test failed (exit $selfTestExit) -- refusing to package" }
$report.steps["staged_self_test"] = @{ status = "ok" }

# --- 7. Full regression suite against the STAGED desktop process --------
Write-Step "Running BridgeClientTest/MultiSessionTest against the staged desktop"
$scriptArg = "engine\native\BeatShoreDesktop\engine\analyze.js"
$desktopProc = Start-Process -FilePath (Join-Path $stage "BeatShoreDesktop.exe") -ArgumentList $scriptArg -WorkingDirectory $stage -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 5
try {
    $fixture = Join-Path $env:TEMP "beatshore-release-fixture.bsmraw"
    $genFixture = Join-Path $PSScriptRoot "scripts\gen-test-fixture.js"
    & (Join-Path $stage "node\node.exe") $genFixture $fixture
    if ($LASTEXITCODE -ne 0) { throw "test fixture generation failed" }

    $bridgeTestExe = Join-Path $native "BridgeClientTest\build\BridgeClientTest_artefacts\Release\BridgeClientTest.exe"
    $tempoOut = & $bridgeTestExe $fixture tempo 2>&1
    $tempoOut | ForEach-Object { Write-Host $_ }
    if (($tempoOut -join "`n") -notmatch "\[test\] PASSED") { throw "staged tempo round trip failed -- refusing to package" }

    $polyOut = & $bridgeTestExe $fixture transcribePolyphonic 2>&1
    $polyOut | ForEach-Object { Write-Host $_ }
    if (($polyOut -join "`n") -notmatch "\[test\] PASSED") { throw "staged transcribePolyphonic round trip failed -- refusing to package" }
}
finally {
    if (-not $desktopProc.HasExited) { Stop-Process -Id $desktopProc.Id -Force }
    Start-Sleep -Milliseconds 500
}

# MultiSessionTest against a fresh instance (the previous one was killed above).
$desktopProc2 = Start-Process -FilePath (Join-Path $stage "BeatShoreDesktop.exe") -ArgumentList $scriptArg -WorkingDirectory $stage -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 5
try {
    $multiTestExe = Join-Path $native "BridgeClientTest\build\MultiSessionTest_artefacts\Release\MultiSessionTest.exe"
    $multiOut = & $multiTestExe $fixture 2>&1
    $multiOut | ForEach-Object { Write-Host $_ }
    if (($multiOut -join "`n") -notmatch "\[test\] ALL PASSED") { throw "staged MultiSessionTest failed -- refusing to package" }
}
finally {
    if (-not $desktopProc2.HasExited) { Stop-Process -Id $desktopProc2.Id -Force }
    Start-Sleep -Milliseconds 500
}
Get-Process -Name "node" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$report.steps["staged_regression_suite"] = @{ status = "ok"; suites = @("tempo", "transcribePolyphonic", "MultiSessionTest") }
Write-Host "Staged regression suite: ALL PASSED" -ForegroundColor Green

# --- 8. Sign the two binaries, if configured, BEFORE compiling the installer ---
$report.signed = Sign-IfConfigured (Join-Path $stage "BeatShoreDesktop.exe") "BeatShoreDesktop.exe"
$signedVst3 = Sign-IfConfigured (Join-Path $stage "BeatShore Bridge.vst3\Contents\x86_64-win\BeatShore Bridge.vst3") "BeatShore Bridge.vst3"
$report.signed = $report.signed -or $signedVst3

# --- 9. Compile the installer --------------------------------------------
if ($SkipInstallerCompile) {
    Write-Host "`n-SkipInstallerCompile passed -- stopping before Inno Setup." -ForegroundColor Yellow
    $report.steps["installer_compile"] = @{ status = "skipped"; reason = "-SkipInstallerCompile" }
}
else {
    Write-Step "Compiling installer with Inno Setup"
    $isccOut = & $iscc (Join-Path $installer "BeatShoreSetup.iss") 2>&1
    $isccOut | Select-String -Pattern "Warning|Error|Successful compile" | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup compile failed (exit $LASTEXITCODE)" }
    $warningLines = $isccOut | Select-String -Pattern "^Warning"
    if ($warningLines) { Write-Host "Inno Setup reported $($warningLines.Count) warning(s) -- review before shipping." -ForegroundColor Yellow }
    $report.steps["installer_compile"] = @{ status = "ok"; warnings = $warningLines.Count }

    $installerExe = Join-Path $installer "Output\BeatShoreSetup-0.2.0.exe"
    $signedInstaller = Sign-IfConfigured $installerExe "BeatShoreSetup-0.2.0.exe"
    $report.signed = $report.signed -or $signedInstaller

    $installerItem = Get-Item $installerExe -ErrorAction Stop
    $report.artifacts["installer"] = @{
        path = $installerExe
        sizeBytes = $installerItem.Length
        sha256 = Get-Sha256 $installerExe
        productVersion = $installerItem.VersionInfo.ProductVersionRaw.ToString()
    }
}

# --- 10. Hash everything a release manifest needs ------------------------
Write-Step "Computing final hashes"
$desktopStaged = Join-Path $stage "BeatShoreDesktop.exe"
$vst3Staged = Join-Path $stage "BeatShore Bridge.vst3\Contents\x86_64-win\BeatShore Bridge.vst3"
$report.artifacts["BeatShoreDesktop.exe"] = @{
    path = $desktopStaged
    sizeBytes = (Get-Item $desktopStaged -ErrorAction Stop).Length
    sha256 = Get-Sha256 $desktopStaged
}
$report.artifacts["BeatShoreBridge.vst3"] = @{
    path = $vst3Staged
    sizeBytes = (Get-Item $vst3Staged -ErrorAction Stop).Length
    sha256 = Get-Sha256 $vst3Staged
}

# Staging-manifest hash: SHA-256 over the sorted per-file hash listing of
# the ENTIRE staged tree -- the single value that lets a future run (or a
# human) confirm the whole staging directory's contents in one comparison,
# not just the two binaries this script itself changed.
$allFiles = Get-ChildItem -Path $stage -Recurse -File | Sort-Object FullName
$hashLines = foreach ($f in $allFiles) {
    $rel = $f.FullName.Substring($stage.Length + 1) -replace '\\', '/'
    (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash.ToLower() + "  " + $rel
}
$hashListPath = Join-Path $installer "staging-file-hashes.txt"
$hashLines | Set-Content -Path $hashListPath -Encoding utf8
$stagingManifestHash = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.Text.Encoding]::UTF8.GetBytes(($hashLines -join "`n"))
    )
).Replace("-", "").ToLower()
$report.artifacts["stagingManifestHash"] = $stagingManifestHash
$report.artifacts["stagedFileCount"] = $allFiles.Count

$report.finishedUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
$reportPath = Join-Path $installer "release-report.json"
$report | ConvertTo-Json -Depth 6 | Set-Content -Path $reportPath -Encoding utf8

Write-Host "`n=== DONE ===" -ForegroundColor Green
Write-Host "Report written to $reportPath"
Write-Host "Update RELEASE_MANIFEST.md's tables from that report's real numbers -- do not hand-type hashes."
if (-not $report.signed) {
    Write-Host "Not signed (no -SigntoolPath/-CertThumbprint supplied) -- this is an unsigned build." -ForegroundColor Yellow
}
