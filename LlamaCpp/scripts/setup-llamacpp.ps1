# setup-llamacpp.ps1
#
# One-time setup (idempotent) for the InoLlama plugin.
#
# Downloads upstream's prebuilt llama.cpp release artifacts for Win64 + Android
# arm64, stages everything (headers + DLLs/.so) under a flat tree:
#   Plugins/InoLlama/Source/ThirdParty/
#     Public/                          llama.cpp C API headers
#     Win64/                           llama.dll, ggml*.dll, libomp140.x86_64.dll
#     Android/arm64-v8a/               libllama.so, libggml*.so
#
# Pinned version lives in:
#   Plugins/InoLlama/LlamaCpp/LLAMACPP_VERSION   (e.g. "b8955")
# Bump + re-run this script to update.
#
# Why prebuilts (not build-from-source like LiteRT-LM):
#   - Upstream ships exactly the artifacts we need for both platforms on
#     every tagged release. Zero toolchain requirements on dev machines
#     (no CMake, no MSVC, no NDK, no Vulkan SDK). Mirrors the ONNX Runtime
#     setup pattern.
#   - llama.cpp has a stable, well-exported public C API and their CI
#     produces drop-in DLLs. No custom Bazel target or export-visibility
#     quirks like LiteRT-LM required.
#
# Platform matrix:
#   - Win64:  `llama-<tag>-bin-win-vulkan-x64.zip` (CPU + Vulkan in one bundle;
#             pick Vulkan so we get both backends — CPU is always loaded as
#             fallback via the co-shipped ggml-cpu-*.dll variants)
#   - Android arm64: `llama-<tag>-bin-android-arm64.tar.gz` (CPU only — upstream
#                    does not publish Android Vulkan artifacts in releases)
#
# Naming policy (IMPORTANT — don't add renames without reading this):
#   Unlike InoOnnxRuntime (which renames onnxruntime.dll -> InoOnnxRuntime.dll
#   and DirectML.dll -> InoDml.dll to dodge verified base-name-cache
#   collisions with UE's NNE / Marketplace plugins), we ship all llama.cpp
#   binaries under their ORIGINAL filenames.
#
#   Reason: llama.cpp ships as an interconnected graph of 20+ DLLs with
#   PE import tables that reference each other by base name
#   (llama.dll -> ggml.dll -> ggml-base.dll, plus runtime loading of
#   ggml-cpu-*.dll / ggml-vulkan.dll via ggml_backend_load_all scanning
#   for ggml-*.dll patterns). Renaming any of them would require patching
#   the PE import tables in all dependents AND wiring explicit
#   ggml_backend_load(full_path) calls to replace the glob scanner.
#
#   No UE 5.7 plugin currently ships llama.cpp, so no concrete collision
#   exists today. If a future collision emerges, upgrade to rename +
#   PE-patch as a scoped follow-up. The directory isolation
#   (Binaries/ThirdParty/InoLlamaCpp/Win64/ as a unique location) is
#   sufficient for now.
#
# CPU variant strategy:
#   Upstream ships ~15 Windows CPU variants per tier (haswell, sandybridge,
#   icelake, alderlake, zen4, sse42, x64, etc.) and ~7 Android ARM tiers
#   (armv8.0, armv8.2, armv8.6, armv9.0, armv9.2). We stage ALL of them
#   so llama.cpp's runtime backend-picker can select the optimal one at
#   model-load time. Total added footprint is small (~15 MB Windows CPU
#   variants combined).
#
# Skip list (files we deliberately DO NOT stage):
#   - *.exe / executables without extension on Android (CLI tools)
#   - llama-common.dll / libllama-common.so (CLI shared code only)
#   - ggml-rpc.dll / libggml-rpc.so (distributed inference, not used)
#   - libmtmd.so (multimodal CLI helper, not exposed via llama.h)
#
# Public headers:
#   Not bundled in the Windows release ZIP. We fetch them directly from
#   raw.githubusercontent.com at the pinned tag's commit. Six headers
#   total, small. Single-source-of-truth is the git tag, which matches
#   the binaries we just downloaded.
#
# Artifacts on disk after this runs (assuming llama.cpp b8955):
#
#   Source/ThirdParty/
#     .llamacpp_version                 (stamp file for idempotency check)
#     Public/                           (llama.h, ggml*.h — public C API)
#     Win64/
#       llama.dll                       (main library)
#       ggml.dll                        (dispatcher)
#       ggml-base.dll                   (base implementation)
#       ggml-cpu-*.dll                  (all CPU variants: haswell,
#                                        sandybridge, icelake, alderlake,
#                                        cannonlake, cascadelake, cooperlake,
#                                        ivybridge, piledriver,
#                                        sapphirerapids, skylakex, sse42,
#                                        x64, zen4 = 14 total)
#       ggml-vulkan.dll                 (Vulkan backend)
#       libomp140.x86_64.dll            (MSVC OpenMP redistributable)
#     Android/arm64-v8a/
#       libllama.so                     (main library)
#       libggml.so                      (dispatcher)
#       libggml-base.so                 (base implementation)
#       libggml-cpu-android_*.so        (CPU variants: armv8.0_1, armv8.2_1,
#                                        armv8.2_2, armv8.6_1, armv9.0_1,
#                                        armv9.2_1, armv9.2_2 = 7 total)

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LlamaCppDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir    = (Resolve-Path (Join-Path $LlamaCppDir "..")).Path
$VersionFile  = Join-Path $LlamaCppDir "LLAMACPP_VERSION"
$CacheDir     = Join-Path $LlamaCppDir ".cache"

# Staging destinations. Everything (headers + Win64 DLLs + Android .so)
# lives under Source/ThirdParty/ in a flat layout — Public/, Win64/,
# Android/<arch>/ — matching the sibling InoLiteRT and InoOnnx plugins.
# No Binaries/ThirdParty/ tree.
$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty"
$PublicIncDir     = Join-Path $ThirdPartyDir "Public"
$Win64BinStageDir = Join-Path $ThirdPartyDir "Win64"
$Arm64BinStageDir = Join-Path $ThirdPartyDir "Android\arm64-v8a"

# DLLs to SKIP on Windows (CLI-only / unused features).
$WinSkipList = @(
    "llama-common.dll",
    "ggml-rpc.dll"
)

# .so files to SKIP on Android (CLI-only / unused features).
$AndroidSkipList = @(
    "libllama-common.so",
    "libggml-rpc.so",
    "libmtmd.so"
)

# Public headers to fetch from raw.githubusercontent.com at the pinned tag.
# Keys are destination filenames (placed flat into Public/), values are
# repo-relative paths under the llama.cpp source tree.
$Headers = [ordered]@{
    "llama.h"        = "include/llama.h"
    "ggml.h"         = "ggml/include/ggml.h"
    "ggml-alloc.h"   = "ggml/include/ggml-alloc.h"
    "ggml-backend.h" = "ggml/include/ggml-backend.h"
    "ggml-cpu.h"     = "ggml/include/ggml-cpu.h"
    "ggml-opt.h"     = "ggml/include/ggml-opt.h"
    "gguf.h"         = "ggml/include/gguf.h"   # included by llama.h
}

#---------------------------------------------------------------------
# 1. Load pinned version
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "LLAMACPP_VERSION file not found at $VersionFile"
}
$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^b\d+$') {
    Write-Error "LLAMACPP_VERSION must be a build tag like 'b8883'. Got: '$Version'"
}

Write-Host ""
Write-Host "=== llama.cpp setup ===" -ForegroundColor Cyan
Write-Host "llama.cpp version: $Version (from LLAMACPP_VERSION)"
Write-Host "Plugin dir:        $PluginDir"
Write-Host "LlamaCpp dir:      $LlamaCppDir"
Write-Host "Cache dir:         $CacheDir"
Write-Host ""

#---------------------------------------------------------------------
# 2. Resolve download URLs + target cache paths
#---------------------------------------------------------------------
$ReleaseBase = "https://github.com/ggml-org/llama.cpp/releases/download/$Version"
$HeaderBase  = "https://raw.githubusercontent.com/ggml-org/llama.cpp/$Version"

$WinZipName  = "llama-$Version-bin-win-vulkan-x64.zip"
$WinZipUrl   = "$ReleaseBase/$WinZipName"
$WinZipPath  = Join-Path $CacheDir $WinZipName

$AndroidTarName = "llama-$Version-bin-android-arm64.tar.gz"
$AndroidTarUrl  = "$ReleaseBase/$AndroidTarName"
$AndroidTarPath = Join-Path $CacheDir $AndroidTarName

#---------------------------------------------------------------------
# 3. Idempotency: if staged binaries already match the pinned version, skip
#---------------------------------------------------------------------
$StampFile     = Join-Path $ThirdPartyDir ".llamacpp_version"
$ExpectedStamp = $Version

if ((Test-Path $StampFile) -and `
    (Test-Path (Join-Path $Win64BinStageDir "llama.dll")) -and `
    (Test-Path (Join-Path $Win64BinStageDir "ggml.dll")) -and `
    (Test-Path (Join-Path $Win64BinStageDir "ggml-base.dll")) -and `
    (Test-Path (Join-Path $Win64BinStageDir "ggml-vulkan.dll")) -and `
    (Test-Path (Join-Path $Arm64BinStageDir "libllama.so")) -and `
    (Test-Path (Join-Path $Arm64BinStageDir "libggml.so")) -and `
    (Test-Path (Join-Path $Arm64BinStageDir "libggml-base.so")) -and `
    (Test-Path (Join-Path $PublicIncDir "llama.h"))) {
    $StampValue = (Get-Content $StampFile -Raw).Trim()
    if ($StampValue -eq $ExpectedStamp) {
        Write-Host "--- Already up to date ---" -ForegroundColor Green
        Write-Host "  llama.cpp $Version staged."
        Write-Host "  Delete '$StampFile' or bump LLAMACPP_VERSION to force re-stage."
        exit 0
    } else {
        Write-Host "--- Version drift detected: staged=$StampValue, pinned=$ExpectedStamp. Re-staging. ---" -ForegroundColor Yellow
    }
}

#---------------------------------------------------------------------
# 4. Preflight: create directories + helpers
#---------------------------------------------------------------------
foreach ($d in @($CacheDir, $PublicIncDir, $Win64BinStageDir, $Arm64BinStageDir)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

function Download-IfMissing {
    param([string]$Url, [string]$Dest, [string]$Label, [int]$MinSizeMb = 1)
    if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt ($MinSizeMb * 1MB))) {
        Write-Host "  [CACHED] $Label ($(Split-Path $Dest -Leaf))"
        return
    }
    Write-Host "  [DOWNLOAD] $Label"
    Write-Host "             $Url"
    # UseBasicParsing avoids IE-engine dependency on headless / Server Core hosts.
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    $sizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($sizeMb MB)"
}

function Download-Header {
    param([string]$Url, [string]$Dest, [string]$Label)
    Write-Host "  [GET] $Label <- $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

#---------------------------------------------------------------------
# 5. Download binaries
#---------------------------------------------------------------------
Write-Host "--- Downloading release artifacts ---" -ForegroundColor Yellow
Download-IfMissing -Url $WinZipUrl     -Dest $WinZipPath     -Label "Win64 Vulkan ZIP"    -MinSizeMb 5
Download-IfMissing -Url $AndroidTarUrl -Dest $AndroidTarPath -Label "Android arm64 tar.gz" -MinSizeMb 1
Write-Host ""

#---------------------------------------------------------------------
# 6. Extract + stage Win64 (Vulkan ZIP = CPU + Vulkan backends)
#---------------------------------------------------------------------
Write-Host "--- Extracting + staging Win64 binaries ---" -ForegroundColor Yellow

$WinExtractDir = Join-Path $CacheDir "win-extract-$Version"
if (Test-Path $WinExtractDir) {
    Remove-Item -Recurse -Force $WinExtractDir
}
New-Item -ItemType Directory -Path $WinExtractDir -Force | Out-Null
Expand-Archive -Path $WinZipPath -DestinationPath $WinExtractDir -Force

# Locate flat bin dir (may vary by release: root of ZIP vs. bin/ subdir).
$LlamaDllCandidates = @(Get-ChildItem -Path $WinExtractDir -Filter "llama.dll" -Recurse -File)
if ($LlamaDllCandidates.Count -eq 0) {
    Write-Error "llama.dll not found anywhere inside $WinZipName extract. Upstream may have renamed it — inspect $WinExtractDir."
}
$WinBinDir = $LlamaDllCandidates[0].Directory.FullName
Write-Host "  Detected Windows bin dir: $WinBinDir"

# Wipe previously-staged DLLs so files removed upstream don't linger.
if (Test-Path $Win64BinStageDir) {
    Get-ChildItem -Path $Win64BinStageDir -File | Remove-Item -Force
}

# Stage every library DLL (llama.dll, ggml*.dll, libomp*.dll) except
# anything in the skip list. Runtime-critical files are verified at the
# end of this section.
$WinStaged = 0
Get-ChildItem -Path $WinBinDir -File -Filter "*.dll" | ForEach-Object {
    $name = $_.Name
    if ($WinSkipList -contains $name) {
        Write-Host "  [SKIP]  $name (explicit skip list)"
        return
    }
    if ($name -notmatch '^(llama|ggml|libomp)') {
        Write-Host "  [SKIP]  $name (not a library DLL pattern)"
        return
    }
    $dst = Join-Path $Win64BinStageDir $name
    Copy-Item -Path $_.FullName -Destination $dst -Force
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  [STAGE] $name ($mb MB)"
    $WinStaged++
}

# Sanity check: the runtime-critical files must all be present.
$WinRequired = @("llama.dll", "ggml.dll", "ggml-base.dll", "ggml-vulkan.dll", "libomp140.x86_64.dll")
foreach ($r in $WinRequired) {
    if (-not (Test-Path (Join-Path $Win64BinStageDir $r))) {
        Write-Error "Required Win64 runtime file '$r' was not staged. Upstream may have changed the archive layout."
    }
}

# At least one ggml-cpu-*.dll must exist for CPU fallback.
$CpuVariantsStaged = @(Get-ChildItem -Path $Win64BinStageDir -Filter "ggml-cpu-*.dll" -File)
if ($CpuVariantsStaged.Count -eq 0) {
    Write-Error "No ggml-cpu-*.dll variants were staged. Upstream archive layout may have changed."
}
Write-Host "  Staged $WinStaged Win64 DLLs total; $($CpuVariantsStaged.Count) ggml-cpu variants."
Write-Host ""

#---------------------------------------------------------------------
# 7. Extract + stage Android arm64 (CPU only)
#---------------------------------------------------------------------
Write-Host "--- Extracting + staging Android arm64 binaries ---" -ForegroundColor Yellow

$AndroidExtractDir = Join-Path $CacheDir "android-extract-$Version"
if (Test-Path $AndroidExtractDir) {
    Remove-Item -Recurse -Force $AndroidExtractDir
}
New-Item -ItemType Directory -Path $AndroidExtractDir -Force | Out-Null

# Use the Windows-native bsdtar (System32\tar.exe) explicitly. A bare
# "tar" call would pick up MSYS/Git-Bash tar if the script is invoked
# from a Git-Bash-derived shell, and MSYS tar mis-parses Windows paths
# like "E:\..." as "host E, path \..." (SSH-style), causing
# "Cannot connect to E: resolve failed". Windows 10 1803+ guarantees
# System32\tar.exe is present.
$WinTarExe = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $WinTarExe)) {
    Write-Error "Expected Windows tar.exe at $WinTarExe — Windows 10 1803+ is required."
}
& $WinTarExe -xzf $AndroidTarPath -C $AndroidExtractDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "tar extraction of $AndroidTarPath failed (exit $LASTEXITCODE)."
}

# Locate libllama.so and use its directory as the source dir.
$LibLlamaCandidates = @(Get-ChildItem -Path $AndroidExtractDir -Filter "libllama.so" -Recurse -File)
if ($LibLlamaCandidates.Count -eq 0) {
    Write-Error "libllama.so not found anywhere inside $AndroidTarName extract. Upstream may have renamed it."
}
$AndroidLibDir = $LibLlamaCandidates[0].Directory.FullName
Write-Host "  Detected Android lib dir: $AndroidLibDir"

# Wipe previously-staged .so files.
if (Test-Path $Arm64BinStageDir) {
    Get-ChildItem -Path $Arm64BinStageDir -File | Remove-Item -Force
}

$AndroidStaged = 0
Get-ChildItem -Path $AndroidLibDir -File -Filter "*.so" | ForEach-Object {
    $name = $_.Name
    if ($AndroidSkipList -contains $name) {
        Write-Host "  [SKIP]  $name (explicit skip list)"
        return
    }
    if ($name -notmatch '^lib(llama|ggml)') {
        Write-Host "  [SKIP]  $name (not a library .so pattern)"
        return
    }
    $dst = Join-Path $Arm64BinStageDir $name
    Copy-Item -Path $_.FullName -Destination $dst -Force
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  [STAGE] $name ($mb MB)"
    $AndroidStaged++
}

# Sanity check runtime-critical Android files.
$AndroidRequired = @("libllama.so", "libggml.so", "libggml-base.so")
foreach ($r in $AndroidRequired) {
    if (-not (Test-Path (Join-Path $Arm64BinStageDir $r))) {
        Write-Error "Required Android runtime file '$r' was not staged. Upstream archive layout may have changed."
    }
}
$CpuVariantsStagedAndroid = @(Get-ChildItem -Path $Arm64BinStageDir -Filter "libggml-cpu-*.so" -File)
if ($CpuVariantsStagedAndroid.Count -eq 0) {
    Write-Error "No libggml-cpu-*.so variants were staged on Android. Upstream archive layout may have changed."
}
Write-Host "  Staged $AndroidStaged Android .so files total; $($CpuVariantsStagedAndroid.Count) libggml-cpu variants."
Write-Host ""

#---------------------------------------------------------------------
# 8. Fetch public headers from raw.githubusercontent.com at the pinned tag
#---------------------------------------------------------------------
# Headers aren't bundled in the release archives — fetch them from the
# git tag's raw view. Six small files; single-source-of-truth is the
# tag we already pinned for binaries.
Write-Host "--- Fetching public C API headers ---" -ForegroundColor Yellow

# Wipe previously-staged headers so removed files don't linger.
if (Test-Path $PublicIncDir) {
    Get-ChildItem -Path $PublicIncDir -File | Remove-Item -Force
}

foreach ($name in $Headers.Keys) {
    $repoPath = $Headers[$name]
    $url = "$HeaderBase/$repoPath"
    $dst = Join-Path $PublicIncDir $name
    Download-Header -Url $url -Dest $dst -Label $name
}

# Verify all expected headers landed.
foreach ($name in $Headers.Keys) {
    $dst = Join-Path $PublicIncDir $name
    if (-not (Test-Path $dst) -or (Get-Item $dst).Length -lt 100) {
        Write-Error "Header '$name' did not download correctly (missing or truncated at $dst)."
    }
}
Write-Host ""

#---------------------------------------------------------------------
# 9. Write version stamp
#---------------------------------------------------------------------
Set-Content -Path $StampFile -Value $ExpectedStamp -NoNewline -Encoding ASCII

Write-Host "=== llama.cpp $Version staged successfully ===" -ForegroundColor Green
Write-Host ""
Write-Host "Staged binaries (Win64):"
$winItems = Get-ChildItem -Path $Win64BinStageDir -File | Sort-Object Name
$winItems | ForEach-Object {
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  - $($_.Name) ($mb MB)"
}
Write-Host ("  [{0} files]" -f $winItems.Count)
Write-Host ""
Write-Host "Staged binaries (Android arm64-v8a):"
$andItems = Get-ChildItem -Path $Arm64BinStageDir -File | Sort-Object Name
$andItems | ForEach-Object {
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  - $($_.Name) ($mb MB)"
}
Write-Host ("  [{0} files]" -f $andItems.Count)
Write-Host ""
Write-Host "Staged headers:"
Get-ChildItem -Path $PublicIncDir -File | Sort-Object Name | ForEach-Object {
    Write-Host "  - $($_.Name)"
}
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Milestone B: add Source/ThirdParty/InoLlamaCpp/InoLlamaCpp.Build.cs + UPL XML."
Write-Host "  2. Milestone C: InoLlamaCppModule::Init loads llama.dll by full path; ggml_backend_load_all auto-discovers ggml-*.dll backends."
