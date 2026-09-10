# CLAUDE.md — InoLlama plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoLlama/`. The hosting demo project is documented in
the host UE project's own `CLAUDE.md` (not part of this repository).

## Purpose

`InoLlama` is an Unreal Engine 5.7 runtime plugin whose job is to
**stage llama.cpp binaries and expose them as a UE module** that other
plugins (currently `InoAgents`) declare as a dependency. Consumers
`#include "InoLlama.h"` and call `InoAgents::LlamaCpp::GetApi()` to
reach a function-pointer vtable (`FLlamaCppApi`) that wraps the
llama.cpp C API; model / context / sampler operations are then driven
through that vtable.

Staging is hybrid: **Win64 uses upstream's prebuilt Vulkan ZIP**
(no build toolchain required), **Android arm64-v8a is built from
source** out of a vendored submodule, and **Mac + iOS use upstream's
prebuilt XCFramework**. The from-source path on Android is non-optional
— upstream's Android release asset is CPU-only, so building ourselves
with `-DGGML_VULKAN=ON` is the only way to get GPU offload on Android.
The Mac/iOS XCFramework is upstream's standard Apple-platform delivery
vehicle and ships with Metal shaders embedded.

llama.cpp is the canonical on-device runtime for **GGUF-format LLMs** —
Qwen, Phi, Llama, SmolLM, DeepSeek-R1-Distill, TinyLlama, and
GGUF-derived TTS backbones (NeuTTS Nano's Qwen2-derived backbone today,
future GGUF consumers tomorrow). It runs on CPU on every platform we
target, plus Vulkan GPU on Win64 / Android and Metal GPU on Apple
Silicon Mac / iOS.

The plugin is target-platform-aware: **Windows (Win64, CPU + Vulkan)**,
**Android (arm64-v8a, CPU + Vulkan)**, **Mac (universal arm64+x86_64,
CPU + Metal on arm64)**, and **iOS (arm64 device, CPU + Metal)** ship
today. Linux would slot in by adding a platform branch to
`InoLlama.Build.cs` plus the matching staging logic in
`setup-llamacpp.ps1`.

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
    │   │                              (RuntimeDependencies, UPL, frameworks)
    │   ├── InoLlama_UPL_Android.xml ← APK packaging directives
    │   ├── Public/InoLlama.h        ← FInoLlamaModule + LogInoLlama category
    │   │                              + namespace InoAgents::LlamaCpp
    │   │                              (FLlamaCppApi vtable + Init/Shutdown/GetApi)
    │   └── Private/InoLlama.cpp     ← StartupModule loads DLL/dylib chain
    │                                  per-platform, resolves the vtable,
    │                                  registers backends (Win64/Android only —
    │                                  Mac/iOS backends auto-register via
    │                                  static-init ctors when the framework
    │                                  dylib is mapped).
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
        ├── Android/arm64-v8a/        ← 11 .so files total
        │   ├── libllama.so
        │   ├── libggml.so + libggml-base.so
        │   ├── libggml-vulkan.so            (Vulkan backend, built from source)
        │   └── libggml-cpu-android_*.so    (7 ARM tier variants:
        │                                    armv8.0_1, armv8.2_1, armv8.2_2,
        │                                    armv8.6_1, armv9.0_1, armv9.2_1,
        │                                    armv9.2_2)
        ├── Mac/llama.framework/      ← 1 fat dylib (universal arm64+x86_64)
        │   ├── llama                       (CPU + Metal embedded; arm64 half
        │   │                                has Metal, x86_64 half is CPU-only)
        │   ├── Headers/                    (llama + ggml C API)
        │   └── Resources/Info.plist        (framework identity)
        ├── IOS/llama.framework/      ← 1 dylib (arm64 device only)
        │   ├── llama                       (CPU + Metal embedded)
        │   ├── Headers/
        │   └── Info.plist
        └── IOS/Simulator/llama.framework/   ← 1 fat dylib (arm64+x86_64 sim)
            ├── llama
            ├── Headers/
            └── Info.plist
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

| Platform                  | Status        | Backends |
|---|---|---|
| **Windows (Win64)**       | ✅ shipping   | CPU (14 microarch variants) + Vulkan GPU |
| **Android (arm64-v8a)**   | ✅ shipping   | CPU (7 ARM tier variants) + Vulkan GPU |
| **macOS (arm64+x86_64)**  | ✅ shipping   | CPU + Metal GPU (arm64 half only — Intel macOS is CPU-only because upstream's CI Intel-Mac runner has no GPU to compile Metal against) |
| **iOS (arm64 device)**    | ✅ shipping   | CPU + Metal GPU |
| **iOS Simulator (arm64+x86_64)** | ⚙️ staged | CPU + Metal GPU. Framework is staged at `Source/ThirdParty/IOS/Simulator/llama.framework/` for dev iteration in Xcode simulator, but `InoLlama.Build.cs` only wires the device slice into shipped iOS builds (see "iOS simulator" below). |
| Linux                     | ⏳ not staged | No staging logic; consumers' vtable is null on this platform. |

**No NPU support** — llama.cpp doesn't have a generic NPU backend in
its release artifacts. Hexagon NPU exists for Snapdragon but requires
the registration-walled Qualcomm Hexagon SDK; not pursued today.
Apple Neural Engine has no public ggml backend either.

**Android GPU is built from source** — upstream doesn't publish
Android Vulkan / OpenCL prebuilts (their Android release asset is
CPU-only). The `setup-llamacpp.ps1` script builds the vendored source
under `LlamaCpp/vendor/llama.cpp/` with `-DGGML_VULKAN=ON` against the
NDK toolchain to produce `libggml-vulkan.so`. See "Setup" below.

**Mac + iOS use upstream's prebuilt XCFramework** — one
`llama-<tag>-xcframework.zip` covers macos-arm64_x86_64, ios-arm64,
and ios-arm64_x86_64-simulator (plus tvOS / visionOS slices we ignore).
Each slice's `llama.framework/llama` is a single dylib with llama +
ggml + ggml-cpu + ggml-metal + ggml-blas all statically linked
together; Metal shaders are embedded via
`-DGGML_METAL_EMBED_LIBRARY=ON` (no separate `default.metallib`).
Backends self-register at dylib load via static-init constructors —
the consumer does NOT need to call `ggml_backend_load_all_from_path`
or `ggml_backend_load` on Apple platforms. This is structurally
different from the Win64/Android paths which discover backend variants
at runtime.

**iOS simulator** — the simulator slice is staged for dev convenience
but isn't wired into `InoLlama.Build.cs`'s `PublicAdditionalFrameworks`
call, which currently uses the device-only slice. Devs needing
simulator builds can flip the framework path or add a
`Target.Architecture`-driven branch as a follow-up; this isn't blocking
shipped iOS support.

**Mac framework layout (flattened)** — upstream's macOS slice ships
the "versioned" framework structure (`Versions/A/llama` + symlinks at
the root). Symlinks in zip files are unreliable on Windows extraction,
so `setup-llamacpp.ps1` stages a FLATTENED framework (binary + headers
at the framework root, no `Versions/A/` subdir) when extracting on
Windows. Mac dyld accepts both layouts for dylib resolution, so the
flattening is invisible at runtime. iOS frameworks are already flat in
upstream and copied as-is.

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

## Why dynamic loading only (Win64 / Android / Mac)

`InoLlama.Build.cs` does NOT use `PublicAdditionalLibraries` or
`PublicDelayLoadDLLs` on Win64, Android, or Mac. The runtime resolves
every `llama_*` and `ggml_backend_*` entry via `GetProcAddress` /
`dlsym` on the loaded handles, populating `FLlamaCppApi`.

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
- **Mac**: same isolation rationale as Win64/Android. Even though no UE
  plugin currently ships its own `llama.framework`, keeping the load
  explicit (dlopen by full path resolved via IPluginManager) means we
  can never accidentally dyld-bind to a future Marketplace plugin's
  copy at app launch.

**iOS is the exception** — `PublicAdditionalFrameworks` does double
duty (adds `-framework llama` to the link command AND embeds the
framework into the .app's `Frameworks/` directory at packaging time),
because iOS has no reliable equivalent of `dlopen`-by-full-path that
works across all supported iOS versions and signing modes (App Store,
ad-hoc, dev). The framework is auto-loaded by dyld at app launch
before any UE module runs; `InoLlama.cpp`'s iOS Init populates the
vtable via `dlsym(RTLD_DEFAULT, ...)` so the consumer-facing API
stays uniform across platforms — only the load mechanism differs.

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
   public headers, at least one CPU variant per Win64/Android, and the
   `llama.framework/llama` binary in each of the three Mac/iOS slices).
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
5. **Mac + iOS path:** downloads `llama-<tag>-xcframework.zip` from
   GitHub Releases (cached if present), extracts the three slices we
   ship into `Source/ThirdParty/Mac/llama.framework/` (universal
   arm64+x86_64), `Source/ThirdParty/IOS/llama.framework/` (arm64
   device), and `Source/ThirdParty/IOS/Simulator/llama.framework/`
   (arm64+x86_64 simulator). The Mac slice is FLATTENED on
   extraction — see "Mac framework layout (flattened)" above.
6. Stages public C API headers from the vendor submodule into
   `Source/ThirdParty/Public/` (release archives don't bundle them).
7. Writes the version stamp.

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

## Vulkan backend note (Win64 / Android only)

`ggml-vulkan.dll` statically imports `vulkan-1.dll`. `vulkan-1.dll` is
part of the Vulkan runtime loader bundled with Windows 10 1803+ and
every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed by us
— Windows provides it. Same story on Android: `libvulkan.so` is part
of the platform from API 24+. We target API 30+ (matches the hosting
game's minSdk). ggml-vulkan additionally requires API >= 28 because it
uses unsuffixed Vulkan 1.1 symbols like `vkGetPhysicalDeviceFeatures2`
which the NDK's libvulkan.so stub only exposes from API 28 onward.

## Metal backend note (Mac / iOS only)

The Apple slices' `llama.framework/llama` is built with
`-DGGML_METAL_EMBED_LIBRARY=ON`, which means the Metal shader source
(`ggml-metal.metal` → compiled `default.metallib`) is baked into the
dylib's `__DATA,__ggml_metallib` section as a byte blob. At first
Metal use, ggml maps the section, hands it to
`MTLDevice.makeLibraryWithData_:` and gets a `MTLLibrary` back without
ever touching the filesystem. We do not have to ship a separate
`.metallib` file alongside the framework, and the framework remains a
single self-contained binary.

Metal requires a Metal-capable `MTLDevice`. On Apple Silicon Macs and
all iOS devices we ship to, this is always present and the backend
registers automatically. On Intel Macs and the iOS Simulator x86_64
slice running on an Intel Mac host, the GPU is unavailable and the
Metal backend self-rejects at registration time — the runtime backend
picker then drops back to CPU silently. No fallback handling is
needed in our code; the auto-registration log printed by `Init()`
on Apple platforms shows which backends actually came up.

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

When the editor opens with InoLlama enabled, UE 5.7's Live Coding
subsystem hooks every Windows DLL load via
`LDR_DLL_NOTIFICATION_REASON_LOADED` (see
`Engine/Source/Developer/Windows/LiveCoding/Private/LiveCodingModule.cpp::OnDllLoaded`)
and routes anything whose full path lives under
`{FullEngineDir, FullEnginePluginsDir, FullProjectDir, FullProjectPluginsDir}`
into its hot-patch enable queue (`FLiveCodingModule::IsUEDll` filters by
`StartsWith` on those four roots).

When `ggml_backend_load_all_from_path` glob-loads all 14
`ggml-cpu-*.dll` variants from `Source/ThirdParty/Win64/`, then
`ggml` `FreeLibrary`s the 13 whose host-CPU score probe failed,
Live Coding has already queued each — and at the next Tick logs
`Cannot enable module X because it is not loaded by this process`
for every unloaded variant. Same fate for `ggml-vulkan.dll` on a
host without a Vulkan device.

**The fix lives in `InoLlama.cpp`'s `Init()`**, not in the build
graph: at module startup we copy every backend DLL (Vulkan + the
14 CPU variants) into a per-version scratch dir under `%TEMP%`
(e.g. `%TEMP%/InoLlama_LlamaCpp_Backends_b9016/`) and call
`ggml_backend_load_all_from_path` against THAT path instead of
the in-tree `Source/ThirdParty/Win64/`. `IsUEDll`'s `StartsWith`
check rejects `%TEMP%/...`, so the variants' load notifications
are filtered out at the Live Coding gate and never reach the
hot-patch queue. The four "main" libs (`libomp140.x86_64.dll`,
`ggml-base.dll`, `ggml.dll`, `llama.dll`) stay in their original
Plugins path and are preloaded as before — Live Coding sees them
once at preload, but they stay loaded for the process lifetime so
`GetModuleHandleW` always succeeds and no error is logged.

Vulkan is intentionally NOT in `PreloadWin64Deps`'s list anymore.
If we preloaded Vulkan from the Plugins path AND let the scratch
scan also map it, Windows' loader would treat the two distinct
absolute paths as separate mappings and re-trigger the Live
Coding hook on the in-tree path. The scratch scan is the sole
load site for Vulkan now.

**Historical note (do not reintroduce):** an earlier guard in
`InoLlama.Build.cs` that excluded `ggml-cpu-*.dll` from the editor
target's `RuntimeDependencies` was based on a misdiagnosis (Live
Coding does NOT scan `RuntimeDependencies`; it reads the OS DLL-
load notification stream). The guard is harmless and left in place
because it produces a sensible cooked-build packaging shape on
shipping targets, but it is not what suppresses the editor errors.
The Android branch doesn't need any equivalent because Android
targets are never `TargetType.Editor` and the Android linker /
backend-loading path doesn't trip the Live Coding hook anyway.

## Authoritative references

Upstream sources:
- llama.cpp: https://github.com/ggml-org/llama.cpp
- Vulkan-Headers: https://github.com/KhronosGroup/Vulkan-Headers
- SPIRV-Headers: https://github.com/KhronosGroup/SPIRV-Headers

Upstream release artifacts we consume:
- Win64: `llama-<tag>-bin-win-vulkan-x64.zip`
- Mac + iOS: `llama-<tag>-xcframework.zip` (one zip, three slices)
- Android: built from source via `LlamaCpp/vendor/llama.cpp` submodule

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
