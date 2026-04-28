# CLAUDE.md — InoLlama plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoLlama/`. The hosting demo project is documented in
`E:/Projects/InoProject/CLAUDE.md`.

## Purpose

`InoLlama` is an Unreal Engine 5.7 runtime plugin whose job is to
**stage upstream's prebuilt llama.cpp binaries and expose them as a UE
module** that other plugins (currently `InoAgents`) declare as a
dependency. Consumers `#include "InoLlama.h"` and call
`InoAgents::LlamaCpp::GetApi()` to reach a function-pointer vtable
(`FLlamaCppApi`) that wraps the llama.cpp C API; model / context /
sampler operations are then driven through that vtable.

llama.cpp is the canonical on-device runtime for **GGUF-format LLMs** —
Qwen, Phi, Llama, SmolLM, DeepSeek-R1-Distill, TinyLlama, and
GGUF-derived TTS backbones (NeuTTS Nano's Qwen2-derived backbone today,
future GGUF consumers tomorrow). It runs on CPU on every platform we
target, plus Vulkan GPU on Win64.

The plugin is target-platform-aware: **Windows (Win64, CPU + Vulkan)**
and **Android (arm64-v8a, CPU only)** ship today. Upstream does not
publish Android Vulkan / OpenCL artifacts in releases — Android GPU
would require building from source. iOS, Linux, and macOS would slot in
by adding platform branches to `InoLlama.Build.cs` plus the matching
prebuilt download in `setup-llamacpp.ps1`.

## Layout

```
Plugins/InoLlama/
├── InoLlama.uplugin                 ← UE plugin manifest (LoadingPhase=PreLoadingScreen)
│
├── LlamaCpp/                        ← setup workspace
│   ├── LLAMACPP_VERSION             ← pinned llama.cpp build tag (e.g. "b8955")
│   ├── scripts/
│   │   ├── setup-llamacpp.ps1       ← downloads + stages prebuilt binaries
│   │   └── clean.ps1                ← wipes .cache/ and staged artifacts
│   └── .cache/                      ← downloaded ZIPs/tarballs (gitignored)
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
        │   └── (llama.h + ggml*.h — 7 headers fetched from raw.githubusercontent.com)
        ├── Win64/                   ← 19 DLLs total
        │   ├── llama.dll                   (main library)
        │   ├── ggml.dll + ggml-base.dll    (dispatcher + base)
        │   ├── ggml-cpu-*.dll              (14 microarch CPU variants)
        │   ├── ggml-vulkan.dll             (Vulkan backend)
        │   └── libomp140.x86_64.dll        (MSVC OpenMP redist)
        └── Android/arm64-v8a/        ← 10 .so files total
            ├── libllama.so
            ├── libggml.so + libggml-base.so
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
| **Android (arm64-v8a)** | ✅ shipping | CPU only (7 ARM tier variants) |
| iOS / Linux / macOS   | ⏳ not staged | No prebuilt download; consumers' vtable is null on these platforms. |

**No NPU support** — llama.cpp doesn't have an NPU backend in any of
its release artifacts on any platform.

**No Android GPU** — upstream doesn't publish Android Vulkan / OpenCL
prebuilts. Adding GPU support on Android would require building from
source (NDK + CMake + Vulkan headers).

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
plugins). Directory isolation (`Source/ThirdParty/Win64/` as a unique
staging path) is sufficient. If a future Marketplace plugin ships
llama.dll, this can be revisited as a scoped follow-up.

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

1. Reads pinned version from `LlamaCpp/LLAMACPP_VERSION`.
2. Skips early if a stamp file shows the version is already staged.
3. Downloads two archives to `LlamaCpp/.cache/` (cached if present):
   - `llama-<tag>-bin-win-vulkan-x64.zip` (Win64 — CPU + Vulkan)
   - `llama-<tag>-bin-android-arm64.tar.gz` (Android — CPU only)
4. Extracts and stages library DLLs/.so into `Source/ThirdParty/`,
   skipping CLI-only files (`llama-common`, `ggml-rpc`, `mtmd`).
5. Fetches public C API headers from `raw.githubusercontent.com` at
   the pinned tag (release archives don't bundle them).
6. Writes the version stamp.

To clean up:

```powershell
./clean.ps1
```

Wipes `.cache/` + staged headers + staged binaries. Re-run setup to
restore.

## Bumping the pin

```powershell
# 1. Edit LLAMACPP_VERSION (e.g. b8955)
# 2. Re-run setup
cd Plugins/InoLlama/LlamaCpp/scripts
./setup-llamacpp.ps1
```

llama.cpp tags every commit as a build number; expect daily-ish
release cadence. Both Win64 + Android assets ship in the same release,
so unlike ORT there's no need to wait for a separate channel to
publish.

## Vulkan backend note

`ggml-vulkan.dll` statically imports `vulkan-1.dll`. `vulkan-1.dll` is
part of the Vulkan runtime loader bundled with Windows 10 1803+ and
every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed by us
— Windows provides it. Same story on Android: `libvulkan.so` is part
of the platform from API 24+. We target API 26+.

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

## Authoritative references

- llama.cpp upstream: https://github.com/ggml-org/llama.cpp
- Public C API header (staged copy consumers `#include`):
  `Source/ThirdParty/Public/llama.h`
- API vtable accessor: `Source/InoLlama/Public/InoLlama.h` →
  `InoAgents::LlamaCpp::GetApi()`
- Pinned version: `LlamaCpp/LLAMACPP_VERSION`
