# setup-llamacpp.ps1
#
# Hybrid setup (idempotent) for the InoLlama plugin.
#
# Two staging paths from a single pinned llama.cpp version:
#
#   Win64 (prebuilt download)
#     Downloads upstream's `llama-<tag>-bin-win-vulkan-x64.zip` and stages
#     ~19 DLLs (llama, ggml, 14 CPU microarch variants, Vulkan, OpenMP) to
#     Source/ThirdParty/Win64/. Headers are fetched from
#     raw.githubusercontent.com at the same tag and placed in
#     Source/ThirdParty/Public/. No build toolchain required.
#
#   Android arm64-v8a (from source)
#     Builds llama.cpp from the LlamaCpp/vendor/llama.cpp/ submodule
#     (which MUST be checked out at the same `LLAMACPP_VERSION` tag) using
#     Android Studio's NDK r28b + the SDK's bundled ninja, with Vulkan
#     enabled via NDK's bundled glslc. Stages ~10 .so files (libllama,
#     libggml, libggml-base, 7 ARM tier libggml-cpu-android_*.so, plus
#     libggml-vulkan.so) to Source/ThirdParty/Android/arm64-v8a/.
#
#     Why from source: upstream's `llama-<tag>-bin-android-arm64.tar.gz`
#     release asset is CPU-only — they do not publish Android Vulkan
#     prebuilts. The Vulkan backend code is Android-compatible (no
#     __ANDROID__ hostile bits; libvulkan.so is an Android platform
#     library from API 24+). Building ourselves is the only path to
#     Android GPU offload.
#
# Pinned version lives in:
#   LlamaCpp/LLAMACPP_VERSION                    (e.g. "b9016")
#   LlamaCpp/vendor/llama.cpp                    (git submodule, MUST match)
# Bump LLAMACPP_VERSION + `git -C vendor/llama.cpp checkout <tag>` to update.
#
# Naming policy (IMPORTANT): no .so/.dll renames. llama.cpp ships an
# interconnected DLL graph (llama -> ggml -> ggml-base, plus runtime glob
# scan for ggml-*.{dll,so}). Renaming would require PE/ELF import-table
# patches and replacing the glob scanner with explicit
# ggml_backend_load(full_path) calls. Directory isolation
# (Source/ThirdParty/Win64/ + Source/ThirdParty/Android/arm64-v8a/) is the
# collision-avoidance strategy. See InoLlama.Build.cs for rationale.
#
# CPU variant strategy: stage ALL variants. llama.cpp's runtime backend
# picker probes the host CPU and registers only those that pass; the
# rest sit on disk unused. Total footprint ~15 MB across all variants.
#
# Skip list:
#   - llama-common (CLI shared code only, no public symbols)
#   - ggml-rpc    (distributed inference, not used)
#   - libmtmd     (multimodal CLI helper, not exposed via llama.h)
#
# Idempotency stamp: Source/ThirdParty/.llamacpp_version. If the stamp
# matches LLAMACPP_VERSION AND every required staged file is present
# (now including libggml-vulkan.so on Android), we skip both the Win64
# download and the Android build. To force re-stage: delete the stamp
# file or run clean.ps1.

$ErrorActionPreference = "Stop"

#---------------------------------------------------------------------
# 0. Paths
#---------------------------------------------------------------------
$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LlamaCppDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir    = (Resolve-Path (Join-Path $LlamaCppDir "..")).Path
$VersionFile        = Join-Path $LlamaCppDir "LLAMACPP_VERSION"
$VulkanHeadersFile  = Join-Path $LlamaCppDir "VULKAN_HEADERS_VERSION"
$CacheDir     = Join-Path $LlamaCppDir ".cache"
$VendorSrcDir = Join-Path $LlamaCppDir "vendor\llama.cpp"

$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty"
$PublicIncDir     = Join-Path $ThirdPartyDir "Public"
$Win64BinStageDir = Join-Path $ThirdPartyDir "Win64"
$Arm64BinStageDir = Join-Path $ThirdPartyDir "Android\arm64-v8a"

$StampFile = Join-Path $ThirdPartyDir ".llamacpp_version"

# Skip lists
$WinSkipList     = @("llama-common.dll", "ggml-rpc.dll")
$AndroidSkipList = @("libllama-common.so", "libggml-rpc.so", "libmtmd.so")

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
    "gguf.h"         = "ggml/include/gguf.h"
}

#---------------------------------------------------------------------
# 1. Load pinned version
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "LLAMACPP_VERSION file not found at $VersionFile"
}
$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^b\d+$') {
    Write-Error "LLAMACPP_VERSION must be a build tag like 'b9016'. Got: '$Version'"
}

if (-not (Test-Path $VulkanHeadersFile)) {
    Write-Error "VULKAN_HEADERS_VERSION file not found at $VulkanHeadersFile"
}
$VulkanHeadersVersion = (Get-Content $VulkanHeadersFile -Raw).Trim()
if ($VulkanHeadersVersion -notmatch '^vulkan-sdk-\d+\.\d+\.\d+\.\d+$') {
    Write-Error "VULKAN_HEADERS_VERSION must be a tag like 'vulkan-sdk-1.4.341.0'. Got: '$VulkanHeadersVersion'"
}

Write-Host ""
Write-Host "=== llama.cpp setup ===" -ForegroundColor Cyan
Write-Host "llama.cpp version:    $Version (from LLAMACPP_VERSION)"
Write-Host "Vulkan-Headers version: $VulkanHeadersVersion (from VULKAN_HEADERS_VERSION)"
Write-Host "Plugin dir:           $PluginDir"
Write-Host "LlamaCpp dir:         $LlamaCppDir"
Write-Host "Vendor source:        $VendorSrcDir"
Write-Host "Cache dir:            $CacheDir"
Write-Host ""

#---------------------------------------------------------------------
# 2. Idempotency: stamp + presence of every required staged file
#---------------------------------------------------------------------
$RequiredWin64 = @("llama.dll", "ggml.dll", "ggml-base.dll", "ggml-vulkan.dll", "libomp140.x86_64.dll")
$RequiredAndroid = @("libllama.so", "libggml.so", "libggml-base.so", "libggml-vulkan.so")

function Test-StagedComplete {
    if (-not (Test-Path $StampFile)) { return $false }
    if ((Get-Content $StampFile -Raw).Trim() -ne $Version) { return $false }
    foreach ($f in $RequiredWin64)   { if (-not (Test-Path (Join-Path $Win64BinStageDir $f))) { return $false } }
    foreach ($f in $RequiredAndroid) { if (-not (Test-Path (Join-Path $Arm64BinStageDir $f))) { return $false } }
    if (-not (Test-Path (Join-Path $PublicIncDir "llama.h"))) { return $false }

    # At least one CPU variant must be present per platform
    $winCpuVariants = @(Get-ChildItem -Path $Win64BinStageDir -Filter "ggml-cpu-*.dll" -File -ErrorAction SilentlyContinue)
    if ($winCpuVariants.Count -eq 0) { return $false }
    $androidCpuVariants = @(Get-ChildItem -Path $Arm64BinStageDir -Filter "libggml-cpu-*.so" -File -ErrorAction SilentlyContinue)
    if ($androidCpuVariants.Count -eq 0) { return $false }
    return $true
}

if (Test-StagedComplete) {
    Write-Host "--- Already up to date ---" -ForegroundColor Green
    Write-Host "  llama.cpp $Version staged (Win64 + Android arm64-v8a)."
    Write-Host "  Delete '$StampFile' or bump LLAMACPP_VERSION to force re-stage."
    exit 0
}

#---------------------------------------------------------------------
# 3. Preflight: create directories + helpers
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
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    $sizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($sizeMb MB)"
}

#=====================================================================
# WIN64: download + stage upstream's prebuilt Vulkan ZIP
#=====================================================================
Write-Host "=== Win64 (prebuilt) ===" -ForegroundColor Cyan

$ReleaseBase    = "https://github.com/ggml-org/llama.cpp/releases/download/$Version"
$WinZipName     = "llama-$Version-bin-win-vulkan-x64.zip"
$WinZipPath     = Join-Path $CacheDir $WinZipName
$WinZipUrl      = "$ReleaseBase/$WinZipName"

Write-Host "--- Downloading Win64 release artifact ---" -ForegroundColor Yellow
Download-IfMissing -Url $WinZipUrl -Dest $WinZipPath -Label "Win64 Vulkan ZIP" -MinSizeMb 5

Write-Host "--- Extracting + staging Win64 binaries ---" -ForegroundColor Yellow
$WinExtractDir = Join-Path $CacheDir "win-extract-$Version"
if (Test-Path $WinExtractDir) { Remove-Item -Recurse -Force $WinExtractDir }
New-Item -ItemType Directory -Path $WinExtractDir -Force | Out-Null
Expand-Archive -Path $WinZipPath -DestinationPath $WinExtractDir -Force

# Locate flat bin dir (root of ZIP vs. bin/ subdir varies).
$LlamaDllCandidates = @(Get-ChildItem -Path $WinExtractDir -Filter "llama.dll" -Recurse -File)
if ($LlamaDllCandidates.Count -eq 0) {
    Write-Error "llama.dll not found anywhere inside $WinZipName extract. Upstream may have changed the layout — inspect $WinExtractDir."
}
$WinBinDir = $LlamaDllCandidates[0].Directory.FullName
Write-Host "  Detected Windows bin dir: $WinBinDir"

# Wipe previously-staged DLLs so files removed upstream don't linger.
if (Test-Path $Win64BinStageDir) {
    Get-ChildItem -Path $Win64BinStageDir -File | Remove-Item -Force
}

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
    Copy-Item -Path $_.FullName -Destination (Join-Path $Win64BinStageDir $name) -Force
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  [STAGE] $name ($mb MB)"
    $WinStaged++
}

foreach ($r in $RequiredWin64) {
    if (-not (Test-Path (Join-Path $Win64BinStageDir $r))) {
        Write-Error "Required Win64 runtime file '$r' was not staged. Upstream may have changed the archive layout."
    }
}
$WinCpuStaged = @(Get-ChildItem -Path $Win64BinStageDir -Filter "ggml-cpu-*.dll" -File)
if ($WinCpuStaged.Count -eq 0) {
    Write-Error "No ggml-cpu-*.dll variants were staged. Upstream archive layout may have changed."
}
Write-Host "  Staged $WinStaged Win64 DLLs total; $($WinCpuStaged.Count) ggml-cpu variants."
Write-Host ""

#=====================================================================
# ANDROID: build from source (vendored submodule) with Vulkan
#=====================================================================
Write-Host "=== Android arm64-v8a (from source) ===" -ForegroundColor Cyan

# 1. Verify vendor submodule is checked out at the matching tag.
if (-not (Test-Path (Join-Path $VendorSrcDir "CMakeLists.txt"))) {
    Write-Error @"
Vendor submodule not initialized at $VendorSrcDir.
Run from the InoLlama plugin repo:
  git submodule update --init --recursive
"@
}

Write-Host "--- Verifying vendor submodule tag ---" -ForegroundColor Yellow
$VendorTag = ""
try {
    Push-Location $VendorSrcDir
    $VendorTag = (& git describe --tags --exact-match HEAD 2>$null).Trim()
} finally {
    Pop-Location
}
if ($VendorTag -ne $Version) {
    Write-Error @"
Vendor submodule is checked out at '$VendorTag', but LLAMACPP_VERSION is '$Version'.
They MUST match. To sync:
  git -C "$VendorSrcDir" fetch --tags
  git -C "$VendorSrcDir" checkout $Version
"@
}
Write-Host "  Submodule tag: $VendorTag (matches LLAMACPP_VERSION)"

# 2. Detect Android SDK + NDK
Write-Host "--- Detecting Android SDK + NDK ---" -ForegroundColor Yellow
$SdkRoot = ""
foreach ($candidate in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA "Android\Sdk"))) {
    if ($candidate -and (Test-Path $candidate)) {
        $SdkRoot = $candidate
        break
    }
}
if (-not $SdkRoot) {
    Write-Error "Android SDK root not found. Set ANDROID_HOME, ANDROID_SDK_ROOT, or install Android Studio (default: %LOCALAPPDATA%\Android\Sdk)."
}
Write-Host "  Android SDK:    $SdkRoot"

$NdkVersion = "28.2.13676358"
$NdkRoot    = Join-Path $SdkRoot "ndk\$NdkVersion"
if (-not (Test-Path $NdkRoot)) {
    Write-Error @"
NDK $NdkVersion not found at $NdkRoot.
Install via Android Studio SDK Manager:
  Tools > SDK Manager > SDK Tools > NDK (Side by side) > pin version $NdkVersion
"@
}
Write-Host "  Android NDK:    $NdkRoot (r28b)"

$ToolchainFile  = Join-Path $NdkRoot "build\cmake\android.toolchain.cmake"
$Glslc          = Join-Path $NdkRoot "shader-tools\windows-x86_64\glslc.exe"
$NdkClang       = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
foreach ($p in @($ToolchainFile, $Glslc, $NdkClang)) {
    if (-not (Test-Path $p)) {
        Write-Error "Required NDK component missing: $p"
    }
}
Write-Host "  Toolchain file: $ToolchainFile"
Write-Host "  glslc (shaders): $Glslc"

# 3. Detect SDK-bundled ninja (system ninja is rarely installed)
$SdkCMakeDir = Join-Path $SdkRoot "cmake\3.22.1\bin"
$Ninja       = Join-Path $SdkCMakeDir "ninja.exe"
if (-not (Test-Path $Ninja)) {
    # Fall back to PATH lookup
    $NinjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($NinjaCmd) {
        $Ninja = $NinjaCmd.Source
    } else {
        Write-Error @"
ninja.exe not found at $Ninja and not on PATH.
Install via Android Studio SDK Manager: Tools > SDK Manager > SDK Tools > CMake.
"@
    }
}
Write-Host "  ninja:          $Ninja"

# Use system cmake if available (newer), else SDK cmake.
$CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $CMake) {
    $CMake = Join-Path $SdkCMakeDir "cmake.exe"
}
if (-not (Test-Path $CMake)) {
    Write-Error "cmake not found on PATH or in $SdkCMakeDir"
}
Write-Host "  cmake:          $CMake"

# 4. Detect Visual Studio + import vcvars64.bat env so cl.exe is on PATH for
#    the host-side vulkan-shaders-gen ExternalProject build.
Write-Host "--- Setting up host MSVC env (for vulkan-shaders-gen) ---" -ForegroundColor Yellow
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    Write-Error "vswhere.exe not found at $VsWhere. Install Visual Studio 2022 (any edition) — required by UE 5.7."
}
$VsInstallPath = (& $VsWhere -latest -property installationPath 2>$null).Trim()
if (-not $VsInstallPath -or -not (Test-Path $VsInstallPath)) {
    Write-Error "Visual Studio install not located via vswhere. UE 5.7 requires VS 2022."
}
$VcVarsBat = Join-Path $VsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVarsBat)) {
    Write-Error "vcvars64.bat not found at $VcVarsBat — VS install may be incomplete (missing C++ workload)."
}
Write-Host "  VS install:     $VsInstallPath"

# Import vcvars64 env into this PowerShell session so child cmake invocations
# find cl/link via PATH for the host shader-gen subbuild.
$VcVarsLines = & cmd /c "`"$VcVarsBat`" > nul 2>&1 && set"
foreach ($line in $VcVarsLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
    }
}
$ClPath = (Get-Command cl -ErrorAction SilentlyContinue).Source
if (-not $ClPath) {
    Write-Error "cl.exe not on PATH after sourcing $VcVarsBat. VS C++ workload may not be installed."
}
Write-Host "  cl.exe:         $ClPath"

# 5. Stage Vulkan headers — both KhronosGroup/Vulkan-Headers (provides
#    <vulkan/vulkan.hpp>, the C++ binding) AND KhronosGroup/SPIRV-Headers
#    (provides <spirv/unified1/spirv.hpp>) into a unified include dir.
#    Android NDK ships <vulkan/vulkan_core.h> only; everything else
#    ggml-vulkan.cpp uses comes from these two Khronos repos.
#    Both are pinned to the same vulkan-sdk-* tag so versions stay in
#    lockstep with what a LunarG Vulkan SDK installation would provide.
Write-Host "--- Staging Vulkan + SPIRV headers ($VulkanHeadersVersion) ---" -ForegroundColor Yellow

$VulkanIncludeDir = Join-Path $CacheDir "vulkan-include-$VulkanHeadersVersion"
$VulkanHppMarker  = Join-Path $VulkanIncludeDir "vulkan\vulkan.hpp"
$SpirvHppMarker   = Join-Path $VulkanIncludeDir "spirv\unified1\spirv.hpp"

# Use Windows-native bsdtar (System32\tar.exe) — explicit to avoid
# Git-Bash MSYS tar mis-parsing Windows paths like "E:\..." as SSH host:path.
$WinTarExe2 = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $WinTarExe2)) {
    Write-Error "Expected Windows tar.exe at $WinTarExe2 — Windows 10 1803+ required."
}

if (-not ((Test-Path $VulkanHppMarker) -and (Test-Path $SpirvHppMarker))) {
    if (Test-Path $VulkanIncludeDir) {
        Remove-Item -Recurse -Force $VulkanIncludeDir
    }
    New-Item -ItemType Directory -Path $VulkanIncludeDir -Force | Out-Null

    # Download both tarballs (cached if already in $CacheDir).
    $VulkanHeadersTarPath = Join-Path $CacheDir "Vulkan-Headers-$VulkanHeadersVersion.tar.gz"
    $SpirvHeadersTarPath  = Join-Path $CacheDir "SPIRV-Headers-$VulkanHeadersVersion.tar.gz"
    Download-IfMissing `
        -Url "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VulkanHeadersVersion.tar.gz" `
        -Dest $VulkanHeadersTarPath -Label "Vulkan-Headers" -MinSizeMb 1
    Download-IfMissing `
        -Url "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$VulkanHeadersVersion.tar.gz" `
        -Dest $SpirvHeadersTarPath  -Label "SPIRV-Headers"  -MinSizeMb 1

    # Extract both into a scratch dir, then copy `include/*` from each
    # into our unified include dir.
    $ScratchExtract = Join-Path $CacheDir "vulkan-extract-$VulkanHeadersVersion"
    if (Test-Path $ScratchExtract) { Remove-Item -Recurse -Force $ScratchExtract }
    New-Item -ItemType Directory -Path $ScratchExtract -Force | Out-Null

    & $WinTarExe2 -xzf $VulkanHeadersTarPath -C $ScratchExtract
    if ($LASTEXITCODE -ne 0) { Write-Error "Vulkan-Headers extraction failed (exit $LASTEXITCODE)." }
    & $WinTarExe2 -xzf $SpirvHeadersTarPath  -C $ScratchExtract
    if ($LASTEXITCODE -ne 0) { Write-Error "SPIRV-Headers extraction failed (exit $LASTEXITCODE)." }

    $VulkanIncSrc = Join-Path $ScratchExtract "Vulkan-Headers-$VulkanHeadersVersion\include"
    $SpirvIncSrc  = Join-Path $ScratchExtract "SPIRV-Headers-$VulkanHeadersVersion\include"
    if (-not (Test-Path $VulkanIncSrc)) { Write-Error "Vulkan-Headers archive missing 'include/' at $VulkanIncSrc" }
    if (-not (Test-Path $SpirvIncSrc))  { Write-Error "SPIRV-Headers archive missing 'include/' at $SpirvIncSrc" }

    Copy-Item -Path (Join-Path $VulkanIncSrc "*") -Destination $VulkanIncludeDir -Recurse -Force
    Copy-Item -Path (Join-Path $SpirvIncSrc  "*") -Destination $VulkanIncludeDir -Recurse -Force

    Remove-Item -Recurse -Force $ScratchExtract
}

if (-not (Test-Path $VulkanHppMarker)) { Write-Error "vulkan.hpp not staged at $VulkanHppMarker" }
if (-not (Test-Path $SpirvHppMarker))  { Write-Error "spirv/unified1/spirv.hpp not staged at $SpirvHppMarker" }
Write-Host "  Unified include:  $VulkanIncludeDir"
Write-Host "    vulkan/vulkan.hpp + spirv/unified1/spirv.hpp present."

# 6. Configure + build
# API 30 (Android 11) — matches the hosting game's minSdk. ggml-vulkan
# requires API >= 28 anyway because it calls Vulkan 1.1 symbols like
# vkGetPhysicalDeviceFeatures2 which the NDK's libvulkan.so stub only
# exposes from API 28 onward (API 26's stub is Vulkan 1.0 only).
$AndroidPlatform = "android-30"
$BuildDir        = Join-Path $CacheDir "android-build-$Version"
$InstallDir      = Join-Path $CacheDir "android-install-$Version"

Write-Host "--- Configuring CMake (cross-compile arm64-v8a + Vulkan) ---" -ForegroundColor Yellow
Write-Host "  Build dir:      $BuildDir"
Write-Host "  Install dir:    $InstallDir"
Write-Host "  ANDROID_PLATFORM: $AndroidPlatform"

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$ConfigureArgs = @(
    "-S", $VendorSrcDir,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
    "-DANDROID_ABI=arm64-v8a",
    "-DANDROID_PLATFORM=$AndroidPlatform",
    "-DCMAKE_INSTALL_PREFIX=$InstallDir",
    # ggml backend toggles
    "-DGGML_NATIVE=OFF",
    "-DGGML_BACKEND_DL=ON",
    "-DGGML_CPU_ALL_VARIANTS=ON",
    "-DGGML_OPENMP=OFF",
    "-DGGML_LLAMAFILE=OFF",
    "-DGGML_VULKAN=ON",
    "-DVulkan_GLSLC_EXECUTABLE=$Glslc",
    "-DVulkan_INCLUDE_DIR=$VulkanIncludeDir",
    # llama.cpp toggles — minimal build, no CLI/server/tests/examples
    "-DLLAMA_BUILD_TESTS=OFF",
    "-DLLAMA_BUILD_EXAMPLES=OFF",
    "-DLLAMA_BUILD_TOOLS=OFF",
    "-DLLAMA_BUILD_SERVER=OFF",
    "-DLLAMA_OPENSSL=OFF",
    "-DLLAMA_CURL=OFF",
    "-DGGML_BUILD_TESTS=OFF",
    "-DGGML_BUILD_EXAMPLES=OFF"
)

& $CMake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed (exit $LASTEXITCODE). See output above."
}

Write-Host ""
Write-Host "--- Building (this takes 5-15 min on first run) ---" -ForegroundColor Yellow
& $CMake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed (exit $LASTEXITCODE). See output above."
}

# 7. Stage outputs to Source/ThirdParty/Android/arm64-v8a/
Write-Host ""
Write-Host "--- Staging Android binaries ---" -ForegroundColor Yellow

# llama.cpp puts shared libs in <build>/bin/ via CMAKE_LIBRARY_OUTPUT_DIRECTORY.
$BuildBinDir = Join-Path $BuildDir "bin"
if (-not (Test-Path $BuildBinDir)) {
    Write-Error "Expected build output dir not found: $BuildBinDir. Inspect $BuildDir."
}

# Wipe previously-staged .so files so removed-upstream files don't linger.
if (Test-Path $Arm64BinStageDir) {
    Get-ChildItem -Path $Arm64BinStageDir -File | Remove-Item -Force
}

$AndroidStaged = 0
Get-ChildItem -Path $BuildBinDir -File -Filter "*.so" | ForEach-Object {
    $name = $_.Name
    if ($AndroidSkipList -contains $name) {
        Write-Host "  [SKIP]  $name (explicit skip list)"
        return
    }
    if ($name -notmatch '^lib(llama|ggml)') {
        Write-Host "  [SKIP]  $name (not a library .so pattern)"
        return
    }
    Copy-Item -Path $_.FullName -Destination (Join-Path $Arm64BinStageDir $name) -Force
    $mb = [math]::Round($_.Length / 1MB, 2)
    Write-Host "  [STAGE] $name ($mb MB)"
    $AndroidStaged++
}

foreach ($r in $RequiredAndroid) {
    if (-not (Test-Path (Join-Path $Arm64BinStageDir $r))) {
        Write-Error "Required Android runtime file '$r' was not built/staged. Inspect $BuildBinDir for the actual output."
    }
}
$AndroidCpuStaged = @(Get-ChildItem -Path $Arm64BinStageDir -Filter "libggml-cpu-*.so" -File)
if ($AndroidCpuStaged.Count -eq 0) {
    Write-Error "No libggml-cpu-*.so variants were built. Check that GGML_CPU_ALL_VARIANTS=ON took effect."
}
Write-Host "  Staged $AndroidStaged Android .so files total; $($AndroidCpuStaged.Count) libggml-cpu variants."
Write-Host ""

#=====================================================================
# 7. Public headers from raw.githubusercontent.com at the pinned tag
#=====================================================================
# Headers aren't bundled in the Windows release ZIP. Use the vendor submodule
# for headers when possible (already on disk at the correct tag), with
# raw.githubusercontent.com as a fallback.
Write-Host "=== Public C API headers ===" -ForegroundColor Cyan
Write-Host "--- Staging from vendor submodule ---" -ForegroundColor Yellow

if (Test-Path $PublicIncDir) {
    Get-ChildItem -Path $PublicIncDir -File | Remove-Item -Force
}

foreach ($name in $Headers.Keys) {
    $repoPath = $Headers[$name]
    $src = Join-Path $VendorSrcDir $repoPath
    $dst = Join-Path $PublicIncDir $name
    if (-not (Test-Path $src)) {
        Write-Error "Header '$repoPath' missing in vendor submodule at $src. Submodule may not be fully checked out."
    }
    Copy-Item -Path $src -Destination $dst -Force
    Write-Host "  [STAGE] $name <- $repoPath"
}

# Sanity check
foreach ($name in $Headers.Keys) {
    $dst = Join-Path $PublicIncDir $name
    if (-not (Test-Path $dst) -or (Get-Item $dst).Length -lt 100) {
        Write-Error "Header '$name' did not stage correctly (missing or truncated at $dst)."
    }
}
Write-Host ""

#=====================================================================
# 8. Write version stamp
#=====================================================================
Set-Content -Path $StampFile -Value $Version -NoNewline -Encoding ASCII

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
