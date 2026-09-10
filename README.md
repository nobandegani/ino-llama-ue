# InoLlama

**[llama.cpp](https://github.com/ggml-org/llama.cpp) packaged for Unreal Engine 5.** GGUF model
inference inside the game process, on Windows, Android, macOS and iOS — with Vulkan GPU offload
on Windows and Android, and Metal on Apple platforms.

It is infrastructure, not a feature. InoLlama has no gameplay API: it loads the llama.cpp runtime
chain in the right order, resolves a function-pointer vtable, registers every ggml backend the
host can actually use, and hands consumers the upstream C API. Plugins like
[InoAgents](https://github.com/nobandegani/ino-agents-ue) then call llama.cpp directly.

---

## Contents

- [What consumers get](#what-consumers-get)
- [Requirements](#requirements)
- [Install](#install)
- [Staging the runtime](#staging-the-runtime)
- [Platform support](#platform-support)
- [How startup works](#how-startup-works)
- [Smoke tests](#smoke-tests)
- [Troubleshooting](#troubleshooting)
- [Licensing](#licensing)

---

## What consumers get

Declare `InoLlama` in your `.uplugin` `Plugins` array and in your `Build.cs`
`PublicDependencyModuleNames`, then:

```cpp
#include "InoLlama.h"   // pulls in llama.h + ggml-backend.h transitively

const auto* Api = InoAgents::LlamaCpp::GetApi();
if (Api == nullptr)
{
    return;   // unsupported platform, or the runtime failed to load
}

llama_model* Model = InoAgents::LlamaCpp::LoadModelFromFile(
    ModelPath, ModelParams, &OutError);
llama_context* Ctx = InoAgents::LlamaCpp::CreateContext(
    Model, ContextParams, &OutError);

// From here it's upstream llama.cpp, through the vtable:
Api->llama_tokenize(Vocab, Text, Len, Tokens, Max, true, false);
Api->llama_decode(Ctx, Batch);
```

**`GetApi()` can return `nullptr`. Always null-check it.**

> The namespace is `InoAgents::LlamaCpp` for historical reasons — this code was extracted from
> the InoAgents plugin, and existing call sites were left working. It is not an InoAgents
> dependency; InoLlama depends on nothing but the engine.

### Why a vtable

ONNX Runtime hands you one vendor-supplied struct via a single entry point. llama.cpp doesn't —
it exports every function individually with no central dispatcher. So InoLlama synthesises its
own `FLlamaCppApi` vtable, resolving each needed symbol at startup. That also means symbol
resolution failures surface once, at module load, with a clear log line, instead of as a link
error or a crash deep in a call.

`ggml_*` symbols live in `ggml.dll` / `ggml-base.dll` rather than `llama.dll`, so resolution
searches across every loaded handle for each name, first hit wins.

### Blueprint-friendly param structs

`InoLlamaTypes.h` provides `FInoLlamaModelParams` and `FInoLlamaContextParams` (both
`BlueprintType`) plus `EInoLlamaKvDtype`, `EInoLlamaSplitMode` and `EInoLlamaFlashAttnType`, so
model and context configuration can be exposed to designers without hand-marshalling llama.cpp's
raw C structs.

---

## Requirements

**To consume it:** Unreal Engine 5 (developed against 5.7). Prebuilt binaries for all four
platforms are committed, so a clean clone compiles without a toolchain.

**To re-stage the runtime:** PowerShell, plus the Android SDK/NDK if you are rebuilding the
Android libraries. See [Staging the runtime](#staging-the-runtime).

---

## Install

```bash
cd YourProject/Plugins
git clone --recurse-submodules https://github.com/nobandegani/ino-llama-ue.git InoLlama
```

Already cloned flat?

```bash
git submodule update --init --recursive
```

Then add `InoLlama` to your `.uproject` `Plugins` array, regenerate project files, and build.

> The `llama.cpp` submodule is a **build input**, needed only to rebuild the Android libraries
> (and as the corresponding source for the binaries you ship). The plugin compiles and runs from
> the committed binaries alone.

---

## Staging the runtime

`LlamaCpp/scripts/setup-llamacpp.ps1` is idempotent and handles all four platforms from a single
pinned version — currently **`b9016`** (`LlamaCpp/LLAMACPP_VERSION`), submodule at
`846262d78` (`gguf-v0.18.0-842-g846262d78`).

| Platform | How it's obtained |
|---|---|
| **Win64** | Downloads upstream's `llama-b9016-bin-win-vulkan-x64.zip`. No toolchain needed. |
| **Android** arm64-v8a | **Built from source** via NDK r28b + ninja, Vulkan on. 5–15 min first run. |
| **Mac / iOS** | Downloads upstream's `llama-b9016-xcframework.zip` and extracts three slices. |

Android is the only platform built locally, because upstream's Android release asset is
**CPU-only** — they publish no Android Vulkan prebuilt. Building it is the only route to GPU
offload there. Vulkan headers are pinned separately in `LlamaCpp/VULKAN_HEADERS_VERSION`
(`vulkan-sdk-1.4.341.0`).

`LlamaCpp/scripts/clean.ps1` drops the build and cache trees.

> The submodule must be checked out at the same tag as `LLAMACPP_VERSION`, or the Android build
> will stage libraries that don't match the headers the other platforms are using.

---

## Platform support

| Platform | Artifact | Files | GPU |
|---|---|---|---|
| **Windows x64** | `.dll` | 19 — `llama`, `ggml`, `ggml-base`, 14 CPU microarch variants, `ggml-vulkan`, OpenMP | Vulkan |
| **Android** arm64-v8a | `.so` | 11 — `libllama`, `libggml`, `libggml-base`, 7 ARM tier variants, `libggml-vulkan` | Vulkan |
| **macOS** universal | `llama.framework` | 3 | Metal |
| **iOS** device + simulator | `llama.framework` | 6 | Metal |

The many CPU variants are deliberate: ggml builds one library per microarchitecture
(`ggml-cpu-haswell`, `ggml-cpu-zen4`, `ggml-cpu-sapphirerapids`, …) and the runtime scores them
at load, keeping only what the host CPU supports. You ship all of them; each machine picks its
best.

On Apple platforms the framework is a single dylib with llama + ggml + ggml-cpu + ggml-metal +
ggml-blas statically linked, Metal shaders embedded via `-DGGML_METAL_EMBED_LIBRARY=ON`. Backends
self-register through static-init constructors, so there's no backend-discovery step.

**Linux is not supported** — no staged binaries. `GetApi()` returns `nullptr` there and logs a
warning; consumers that null-check degrade cleanly.

---

## How startup works

The module loads at `PreLoadingScreen` so the vtable is ready before any consumer's
Default-phase `StartupModule` runs.

**Windows** preloads dependencies by full path *before* `llama.dll`, so Windows' loaded-module
cache is seeded with the plugin's copies rather than some other module's same-named DLL:

```
libomp140.x86_64.dll → ggml-base.dll → ggml.dll → llama.dll
```

All three preloads are required; a failure aborts init. `ggml-vulkan.dll` is deliberately *not*
preloaded — it's a backend, not a link-time dependency of `llama.dll`. Preloading it would skip
the score check that filters out backends the host can't use.

Then `ggml_backend_load_all_from_path()` scans the staging directory and registers every
`ggml-*.dll` — that's how the 14 CPU variants and Vulkan get picked up — and
`llama_backend_init()` initialises llama's globals. Startup also warns if the loader served a
different copy than the one requested.

**Android** skips the preload — UPL's `<soLoadLibrary>` has already pulled in `libllama.so` via
`System.loadLibrary`, and its `DT_NEEDED` chain cascades `libggml.so` and `libggml-base.so`. The
CPU variants need individual `ggml_backend_load()` calls by bare soname, because
`extractNativeLibs=false` makes the APK's `.so` files virtual — `opendir` fails on them, but
`dlopen` still resolves through the linker namespace.

**macOS** opens the framework binary by full path. **iOS** loads nothing: dyld has already mapped
the embedded framework, so the vtable resolves via `dlsym(RTLD_DEFAULT)`. iOS forbids `dlopen` of
arbitrary paths in submitted apps, so that is the only workable route.

Startup also installs `llama_log_set` and `ggml_log_set` so llama.cpp's stderr output is routed
into `LogInoLlama`. Without it, a `llama_model_load_from_file` failure shows up as a bare
"failed" with the real cause — mmap error, GGUF version mismatch, allocator failure — discarded.

**No model is loaded at startup.** That's the consumer's job.

---

## Smoke tests

Console commands, from the editor's Output Log:

```
Ino.LlamaCpp.VtableTest         every vtable slot resolved non-null
Ino.LlamaCpp.BackendInfoTest    which ggml backends registered on this host
```

`BackendInfoTest` is the quickest way to confirm GPU offload is actually available rather than
silently falling back to CPU.

---

## Troubleshooting

Everything logs under **`LogInoLlama`**.

| Symptom | Cause |
|---|---|
| `GetApi()` returns `nullptr` | Platform unsupported (Linux), or load/resolution failed — check the log for which. |
| Warning about a different copy being served | Windows base-name cache collision; another module loaded a same-named DLL first. |
| `NumGpuLayers > 0` but inference is slow | No GPU backend registered. Run `Ino.LlamaCpp.BackendInfoTest`; `llama_supports_gpu_offload` tells you whether the build has one at all. |
| Model load fails with no detail | Should not happen — log routing is installed at startup. If you see it, the log callbacks failed to resolve. |
| Android: CPU variants never register | `ggml_backend_load` unresolved. `opendir` cannot enumerate a virtual APK lib dir; loading by bare soname is required. |
| Android libraries mismatch headers | Submodule not checked out at `LLAMACPP_VERSION`. |

---

## Licensing

This plugin is **Apache-2.0** ([`LICENSE`](LICENSE)); all source files carry matching headers.

**llama.cpp and ggml are MIT** (Copyright 2023-2026 The ggml authors), which is compatible — MIT
permits the redistribution this repo does, and the Apache-2.0 grant covers only Inoland's own
code under `Source/InoLlama/` and `LlamaCpp/scripts/`.

Per-path detail is in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Nothing here is
revenue-capped, GPL, or proprietary — this is the most permissively licensed plugin in the set.

> **⚠️ The committed Android `.so` files embed absolute build paths.** ggml's `GGML_ASSERT` /
> `GGML_ABORT` macros expand `__FILE__`, and unlike a plain `assert` they survive a Release
> build. The Android libraries were built locally before a prefix map was configured, so they
> carry the builder's directory layout — `libllama.so` alone embeds 154 distinct absolute paths.
> There are no credentials involved, but it does disclose a local username and internal project
> naming, and it ships into any game using the Android build.
>
> `setup-llamacpp.ps1` now passes `-ffile-prefix-map` for both C and C++, so **re-running the
> Android build replaces them with clean libraries**. Until then the committed `.so` files still
> carry the paths. The Win64, macOS and iOS binaries are unaffected — they come from upstream's
> CI release artifacts.

**GGUF model weights are not in this repository.** Whatever model you load carries its own
license — check it, especially for commercial use.
