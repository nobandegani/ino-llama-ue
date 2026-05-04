# CLAUDE.md — InoLlama plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoLlama/`. The hosting demo project is documented in
`E:/Projects/InoProject/CLAUDE.md`.

## Purpose

`InoLlama` is an Unreal Engine 5.7 runtime plugin whose job is to
**stage llama.cpp binaries and expose them as a UE module** that other
plugins (currently `InoAgents`) declare as a dependency. Consumers
`#include "InoLlama.h"` and call `InoAgents::LlamaCpp::GetApi()` to
reach a function-pointer vtable (`FLlamaCppApi`) that wraps the
llama.cpp C API; model / context / sampler operations are then driven
through that vtable.

Staging is hybrid: **Win64 uses upstream's prebuilt Vulkan ZIP**
(no build toolchain required), while **Android arm64-v8a is built
from source** out of a vendored submodule. The from-source path on
Android is non-optional — upstream's Android release asset is
CPU-only, so building ourselves with `-DGGML_VULKAN=ON` is the only
way to get GPU offload on Android.

llama.cpp is the canonical on-device runtime for **GGUF-format LLMs** —
Qwen, Phi, Llama, SmolLM, DeepSeek-R1-Distill, TinyLlama, and
GGUF-derived TTS backbones (NeuTTS Nano's Qwen2-derived backbone today,
future GGUF consumers tomorrow). It runs on CPU on every platform we
target, plus Vulkan GPU on **both Win64 and Android**.

The plugin is target-platform-aware: **Windows (Win64, CPU + Vulkan)**
and **Android (arm64-v8a, CPU + Vulkan)** ship today. iOS, Linux, and
macOS would slot in by adding platform branches to `InoLlama.Build.cs`
plus the matching staging logic in `setup-llamacpp.ps1`.

## Layout

```
Plugins/InoLlama/
├── InoLlama.uplugin                 ← UE plugin manifest (LoadingPhase=PreLoadingScreen)
│
├── LlamaCpp/                        ← setup workspace
│   ├── LLAMACPP_VERSION             ← pinned llama.cpp build tag (e.g. "b9016")
│   ├── VULKAN_HEADERS_VERSION       ← pinned Khronos SDK tag for Vulkan-Headers
│   │                                  + SPIRV-Headers (e.g. "vulkan-sdk-1.4.341.0")
│   ├── scripts/
│   │   ├── setup-llamacpp.ps1       ← Win64: downloads prebuilt; Android: builds from source
│   │   └── clean.ps1                ← wipes .cache/ and staged artifacts
│   ├── vendor/llama.cpp/            ← git submodule pinned to LLAMACPP_VERSION
│   │                                  (source for the Android from-source build)
│   └── .cache/                      ← downloaded ZIPs/tarballs + Android build dirs +
│                                      staged Vulkan/SPIRV headers (gitignored)
│
└── Source/
    ├── InoLlama/                    ← The single UE module
    │   ├── InoLlama.Build.cs        ← embeds third-party wiring
    │   │                              (RuntimeDependencies, UPL, includes)
    │   ├── InoLlama_UPL_Android.xml ← APK packaging directives
    │   ├── Public/InoLlama.h        ← FInoLlamaModule + LogInoLlama category
    │   │                              + namespace InoAgents::LlamaCpp
    │   │                              (FLlamaCppApi vtable + Init/Shutdown/GetApi)
    │   └── Private/InoLlama.cpp     ← StartupModule loads DLL chain,
    │                                  resolves the vtable, registers backends
    │
    └── ThirdParty/                  ← staged setup outputs (consumed by UE)
        ├── Public/                  ← llama.cpp C API headers
        │   └── (llama.h + ggml*.h — 7 headers from vendor submodule)
        ├── Win64/                   ← 19 DLLs total
        │   ├── llama.dll                   (main library)
        │   ├── ggml.dll + ggml-base.dll    (dispatcher + base)
        │   ├── ggml-cpu-*.dll              (14 microarch CPU variants)
        │   ├── ggml-vulkan.dll             (Vulkan backend)
        │   └── libomp140.x86_64.dll        (MSVC OpenMP redist)
        └── Android/arm64-v8a/        ← 11 .so files total
            ├── libllama.so
            ├── libggml.so + libggml-base.so
            ├── libggml-vulkan.so            (Vulkan backend, built from source)
            └── libggml-cpu-android_*.so    (7 ARM tier variants:
                                             armv8.0_1, armv8.2_1, armv8.2_2,
                                             armv8.6_1, armv9.0_1, armv9.2_1,
                                             armv9.2_2)
```

## How other plugins consume this

In a consumer plugin's `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "InoLlama",   // exposes <llama.h> + <ggml*.h> + <InoLlama.h>
                  // (provides InoAgents::LlamaCpp::GetApi() accessor for the
                  //  FLlamaCppApi vtable). Stages the runtime DLLs/.so for cook.
});
```

In its `.uplugin`:

```json
"Plugins": [
    { "Name": "InoLlama", "Enabled": true }
]
```

That's it. `FInoLlamaModule::StartupModule` runs at
`LoadingPhase=PreLoadingScreen`, strictly before any consumer's
`Default`-phase `StartupModule`, so by the time consumer code runs the
DLL/.so chain is loaded, the vtable is resolved, ggml backends are
registered, and `llama_backend_init` has been called.

Consumer code:
```cpp
#include "InoLlama.h"  // for InoAgents::LlamaCpp::GetApi() + FLlamaCppApi

const auto* Api = InoAgents::LlamaCpp::GetApi();
if (Api == nullptr) { /* graceful fallback */ return; }
// ... use Api->llama_model_load_from_file(...) etc.
```

The first and currently only consumer is `Plugins/InoAgents/`
(the NeuTTS Nano TTS subsystem).

## Target platforms

| Platform              | Status      | Backends |
|---|---|---|
| **Windows (Win64)**   | ✅ shipping | CPU (14 microarch variants) + Vulkan GPU |
| **Android (arm64-v8a)** | ✅ shipping | CPU (7 ARM tier variants) + Vulkan GPU |
| iOS / Linux / macOS   | ⏳ not staged | No staging logic; consumers' vtable is null on these platforms. |

**No NPU support** — llama.cpp doesn't have a generic NPU backend in
its release artifacts. Hexagon NPU exists for Snapdragon but requires
the registration-walled Qualcomm Hexagon SDK; not pursued today.

**Android GPU is built from source** — upstream doesn't publish
Android Vulkan / OpenCL prebuilts (their Android release asset is
CPU-only). The `setup-llamacpp.ps1` script builds the vendored source
under `LlamaCpp/vendor/llama.cpp/` with `-DGGML_VULKAN=ON` against the
NDK toolchain to produce `libggml-vulkan.so`. See "Setup" below.

## Why no DLL renames (unlike InoOnnx)

llama.cpp ships as a graph of ~20 interdependent DLLs:

- `llama.dll` imports `ggml.dll`
- `ggml.dll` imports `ggml-base.dll`
- `libggml.so` glob-scans the directory for `ggml-*.{dll,so}` at runtime
  via `ggml_backend_load_all_from_path` to discover CPU variants and
  the Vulkan backend.

Renaming any of them would require:
- Patching the PE import tables in every dependent DLL
- Replacing the glob scanner with explicit `ggml_backend_load(full_path)`
  calls
- Updating the same on Android for `.so` equivalents

No UE 5.7 plugin currently ships llama.cpp, so no concrete base-name
collision exists today (unlike ORT, which collides with NNE / Marketplace
plugins). Directory isolation per platform
(`Source/ThirdParty/Win64/` and `Source/ThirdParty/Android/arm64-v8a/`
as unique staging paths) is sufficient. If a future Marketplace plugin
ships `llama.dll` or `libllama.so`, this can be revisited as a scoped
follow-up.

## Why dynamic loading only

`InoLlama.Build.cs` does NOT use `PublicAdditionalLibraries` or
`PublicDelayLoadDLLs` on either platform. The runtime resolves every
`llama_*` and `ggml_backend_*` entry via `GetProcAddress` / `dlsym`
on the loaded handles, populating `FLlamaCppApi`.

Reasons:

- **Windows**: upstream's release ZIP doesn't ship a `llama.lib` import
  library. We'd have to synthesize one (like InoOnnx does for ORT) or
  accept that import-lib linking isn't available. Dynamic load is the
  simpler path.
- **Android**: implicitly linking against `libllama.so` would add a
  `DT_NEEDED` entry to `libUnreal.so`, and if a future Marketplace
  plugin also ships its own `libllama.so` at a different ABI the
  dynamic linker could fail to satisfy versioned symbol references.
  Keeping the load explicit (`dlopen` via `FPlatformProcess::GetDllHandle`)
  keeps us ABI-isolated.

## Setup

```powershell
cd Plugins/InoLlama/LlamaCpp/scripts
./setup-llamacpp.ps1
```

The script is idempotent (safe to re-run). It:

1. Reads the pinned versions from `LlamaCpp/LLAMACPP_VERSION` and
   `LlamaCpp/VULKAN_HEADERS_VERSION`.
2. Skips early if a stamp file (`Source/ThirdParty/.llamacpp_version`)
   shows the version is already staged AND every required output file
   is present (`llama.dll`, `ggml*.dll`, `ggml-vulkan.dll` on Win64;
   `libllama.so`, `libggml*.so`, `libggml-vulkan.so` on Android; plus
   public headers and at least one CPU variant per platform).
3. **Win64 path:** downloads `llama-<tag>-bin-win-vulkan-x64.zip` from
   GitHub Releases to `LlamaCpp/.cache/` (cached if present), extracts,
   stages 19 library DLLs into `Source/ThirdParty/Win64/`, skipping
   CLI-only files (`llama-common.dll`, `ggml-rpc.dll`).
4. **Android path:** verifies `LlamaCpp/vendor/llama.cpp/` is checked
   out at the matching tag, auto-detects Android Studio's NDK r28b
   (`28.2.13676358`) + the SDK's bundled CMake/ninja, sources VS 2022's
   `vcvars64.bat` (so `cl.exe` is on PATH for the host
   `vulkan-shaders-gen` ExternalProject build), then auto-downloads
   and merges KhronosGroup's `Vulkan-Headers` and `SPIRV-Headers`
   tarballs at `VULKAN_HEADERS_VERSION` (the NDK ships `vulkan/vulkan.h`
   only — not `vulkan/vulkan.hpp` or `spirv/unified1/spirv.hpp`). Then
   runs:
   ```
   cmake -G Ninja
         -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30
         -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON
         -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF
         -DGGML_VULKAN=ON
         -DVulkan_GLSLC_EXECUTABLE=<NDK>/shader-tools/windows-x86_64/glslc.exe
         -DVulkan_INCLUDE_DIR=<.cache>/vulkan-include-<sdk-tag>
         -DLLAMA_BUILD_{TESTS,EXAMPLES,TOOLS,SERVER}=OFF
         -DLLAMA_OPENSSL=OFF -DLLAMA_CURL=OFF
         -DGGML_BUILD_{TESTS,EXAMPLES}=OFF
   ```
   Then `cmake --build` and stages every `lib(llama|ggml)-*.so` from
   `<build>/bin/` into `Source/ThirdParty/Android/arm64-v8a/`, skipping
   `libllama-common.so`, `libggml-rpc.so`, `libmtmd.so`.
5. Stages public C API headers from the vendor submodule into
   `Source/ThirdParty/Public/` (release archives don't bundle them).
6. Writes the version stamp.

**First-run cost on the Android side:** the build cross-compiles
llama.cpp arm64 plus runs `glslc` on ~180 Vulkan compute shaders.
Expect 5–15 min depending on machine. Subsequent runs are no-ops
(the stamp short-circuits, and the `.cache/android-build-<tag>/` dir
is preserved for fast incremental rebuilds).

**Host prerequisites** (one-time, all standard Android dev tooling):

- Android Studio with NDK `28.2.13676358` (r28b) installed via SDK
  Manager → SDK Tools → NDK (Side by side). Provides `glslc`,
  `vulkan/vulkan.h`, per-API `libvulkan.so` stubs, and the cross-
  compile clang.
- Android Studio's bundled CMake (3.22.1+) installed via SDK Manager
  → SDK Tools → CMake. Provides `ninja.exe`.
- Visual Studio 2022 (any edition, with C++ workload). Already
  required by UE 5.7. Used only for the host-side
  `vulkan-shaders-gen` build; the cross-target build uses NDK clang.

**No Vulkan SDK install required.** The script auto-downloads the two
Khronos header repos that the NDK doesn't ship
(`KhronosGroup/Vulkan-Headers` for `vulkan.hpp`,
`KhronosGroup/SPIRV-Headers` for `spirv/unified1/spirv.hpp`) at the
tag pinned in `VULKAN_HEADERS_VERSION` and merges them into a
unified include dir under `.cache/`. This means no LunarG SDK install
on the dev machine — the build is self-bootstrapping from
NDK + Android Studio + VS 2022.

To clean up:

```powershell
./clean.ps1
```

Wipes `.cache/` (downloads + Android build dirs) + staged headers +
staged binaries. Does NOT touch `vendor/llama.cpp/` (that's source).
Re-run setup to restore.

## Bumping the pin

### llama.cpp version

```powershell
# 1. Edit LlamaCpp/LLAMACPP_VERSION (e.g. b9100)
# 2. Sync vendor submodule to the SAME tag (the script enforces this)
git -C Plugins/InoLlama/LlamaCpp/vendor/llama.cpp fetch --tags
git -C Plugins/InoLlama/LlamaCpp/vendor/llama.cpp checkout <tag>
# 3. Re-run setup
cd Plugins/InoLlama/LlamaCpp/scripts
./setup-llamacpp.ps1
```

llama.cpp tags every commit as a build number; expect daily-ish
release cadence. Pick a tag whose release ships
`llama-<tag>-bin-win-vulkan-x64.zip` (almost every release does, but
verify on the GitHub release page if a setup run fails the Win64
download).

### Vulkan-Headers / SPIRV-Headers version

```powershell
# Edit LlamaCpp/VULKAN_HEADERS_VERSION (e.g. vulkan-sdk-1.4.350.0)
# Then re-run setup; the script will re-stage the unified include dir.
./setup-llamacpp.ps1
```

The two Khronos header repos are pinned to the same `vulkan-sdk-*`
tag (so the `.h` and `.hpp` and SPIR-V headers all come from one
LunarG SDK release). Bumping is rarely needed — only if a future
llama.cpp version starts using a Vulkan symbol or SPIR-V opcode that
isn't in the currently-pinned headers. ggml-vulkan is generally
conservative about Vulkan version requirements.

## Vulkan backend note

`ggml-vulkan.dll` statically imports `vulkan-1.dll`. `vulkan-1.dll` is
part of the Vulkan runtime loader bundled with Windows 10 1803+ and
every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed by us
— Windows provides it. Same story on Android: `libvulkan.so` is part
of the platform from API 24+. We target API 30+ (matches the hosting
game's minSdk). ggml-vulkan additionally requires API >= 28 because it
uses unsuffixed Vulkan 1.1 symbols like `vkGetPhysicalDeviceFeatures2`
which the NDK's libvulkan.so stub only exposes from API 28 onward.

## CPU variant strategy

Upstream ships ~14 Windows CPU variants per release (haswell,
sandybridge, icelake, alderlake, zen4, sse42, x64, etc.) and 7 Android
ARM tiers (armv8.0, armv8.2, armv8.6, armv9.0, armv9.2). We stage
**all of them** so llama.cpp's runtime backend-picker can select the
optimal one at model-load time. Total added footprint is small (~15 MB
Windows CPU variants combined).

Each variant's init probes the host CPU and rejects itself if the
required instruction set isn't available. Only the variants that pass
survive in the registered-backends list; the rest are silently
unregistered.

### UE 5.7 Live Coding interaction (Win64 editor)

UE 5.7's Live Coding scans every entry in the editor target's
`RuntimeDependencies` list and tries to "enable" each as a UE module
for hot-patching. With 14 ggml-cpu variants of which only one ever
loads (the host-CPU match), Live Coding logs 13 spurious
`Cannot enable module X because it is not loaded by this process`
Errors at editor startup. The legacy `StagedFileType.SystemNonUFS`
hint that older UE versions used to skip these is no longer respected
in 5.7.

The fix in `InoLlama.Build.cs` is to **exclude the variant glob from
the editor target's `RuntimeDependencies`**:

```csharp
if (Target.Type != TargetType.Editor && Directory.Exists(Win64Dir))
{
    // ... add ggml-cpu-*.dll entries ...
}
```

The editor still loads the variants at runtime via
`ggml_backend_load_all_from_path`'s directory scan of
`Source/ThirdParty/Win64/` — `RuntimeDependencies` isn't on the
load-path side of the equation. Game/Server targets keep the
declarations so the cook + stage step for shipping builds still
packages every variant. Zero behavior change on either side; Live
Coding just stops complaining about DLLs it can't patch.

The `RequiredWin64` DLLs (`llama.dll`, `ggml.dll`, `ggml-base.dll`,
`ggml-vulkan.dll`, `libomp140.x86_64.dll`) ARE kept in the editor
target's `RuntimeDependencies` because they're actually loaded by
the editor process — Live Coding finds them in the loaded-modules
list and is happy. The Android branch doesn't need an equivalent
guard because Android targets are never `TargetType.Editor`.

## Authoritative references

Upstream sources:
- llama.cpp: https://github.com/ggml-org/llama.cpp
- Vulkan-Headers: https://github.com/KhronosGroup/Vulkan-Headers
- SPIRV-Headers: https://github.com/KhronosGroup/SPIRV-Headers

Pinned versions (edit + re-run setup to bump):
- `LlamaCpp/LLAMACPP_VERSION` — llama.cpp build tag
- `LlamaCpp/VULKAN_HEADERS_VERSION` — shared tag for Vulkan-Headers
  + SPIRV-Headers (Khronos releases them under the same `vulkan-sdk-*`
  tag)

In-tree source:
- `LlamaCpp/vendor/llama.cpp/` — git submodule, must be checked out
  at the `LLAMACPP_VERSION` tag (verified by setup script)

Public C API header (staged copy consumers `#include`):
- `Source/ThirdParty/Public/llama.h`

API vtable accessor:
- `Source/InoLlama/Public/InoLlama.h` →
  `InoAgents::LlamaCpp::GetApi()` returns `const FLlamaCppApi*`

Build wiring:
- `Source/InoLlama/InoLlama.Build.cs` — RuntimeDependencies + UPL hookup
- `Source/InoLlama/InoLlama_UPL_Android.xml` — APK packaging directives
