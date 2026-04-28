# LlamaCpp/ — llama.cpp integration for the InoAgents plugin

This directory holds the version pin and setup scripts for the **llama.cpp**
third runtime of the plugin. It is parallel to (and independent of) the
`LiteRtLm/` directory (Gemma 4 LLM runtime) and the `OnnxRuntime/` directory
(Chatterbox TTS runtime).

llama.cpp is the canonical on-device inference engine for GGUF-format LLMs.
Unlike LiteRT-LM (purpose-built for Gemma with baked-in chat templates) or
ONNX Runtime (purpose-built for generic ONNX inference), llama.cpp handles
the long tail of community-quantized GGUF models — Qwen, Phi, Llama, SmolLM,
DeepSeek-R1-Distill, TinyLlama, and many more — and runs them efficiently on
CPU, GPU (Vulkan on Win64), and Android arm64.

## Why a third runtime?

| Runtime | Specializes in | Why it's not enough alone |
|---|---|---|
| **LiteRT-LM** | Gemma 4 family, Google-blessed | No path to arbitrary GGUF models; Google-controlled chat templates |
| **ONNX Runtime** | Generic ONNX inference (Chatterbox TTS today) | GGUF-format LLMs don't compile to ONNX well; quantization support is weak |
| **llama.cpp** | GGUF-format LLMs (everything else) | Covers the rest of the LLM landscape with Q4/Q8 quantizations |

Each runtime does one thing well. Adding llama.cpp unlocks GGUF-format
consumers — first next up is a TTS backbone like NeuTTS Nano (a
Qwen2-derived ~117M-param model with official Q4/Q8 GGUFs), then general
on-device small LLMs for use cases where Gemma 4 is overkill.

## Directory layout

```
LlamaCpp/
├── LLAMACPP_VERSION                ← pinned llama.cpp release tag (e.g. "b8883")
├── scripts/
│   ├── setup-llamacpp.ps1          ← downloads + stages prebuilt binaries
│   └── clean.ps1                   ← wipes .cache/ and staged artifacts
├── README.md                       ← this file
└── .cache/                         ← download cache (gitignored)
```

After `setup-llamacpp.ps1` runs, the staging destinations under the plugin
are populated:

```
Source/ThirdParty/InoLlamaCpp/
├── Public/                         ← llama.cpp C API headers (fetched
│   ├── llama.h                       from raw.githubusercontent.com at the
│   ├── ggml.h                        pinned tag — not bundled in release ZIP)
│   ├── ggml-alloc.h
│   ├── ggml-backend.h
│   ├── ggml-cpu.h
│   ├── ggml-opt.h
│   └── gguf.h
└── .llamacpp_version               ← stamp — matches LLAMACPP_VERSION

Binaries/ThirdParty/InoLlamaCpp/
├── Win64/                          ← 19 files, ~79 MB total
│   ├── llama.dll                   (main library, 2.4 MB)
│   ├── ggml.dll                    (dispatcher, 90 KB)
│   ├── ggml-base.dll               (base implementation, 760 KB)
│   ├── ggml-cpu-*.dll              (14 CPU variants; runtime picks one)
│   ├── ggml-vulkan.dll             (Vulkan backend, 59 MB)
│   └── libomp140.x86_64.dll        (MSVC OpenMP redistributable)
└── Android/arm64-v8a/              ← 10 files, ~71 MB total
    ├── libllama.so                 (main library, 27.5 MB)
    ├── libggml.so                  (dispatcher, 4.7 MB)
    ├── libggml-base.so             (base implementation, 7.3 MB)
    └── libggml-cpu-android_*.so    (7 ARM variants: armv8.0/8.2/8.6/9.0/9.2)
```

File counts match `setup-llamacpp.ps1`'s verified output. CLI-only DLLs
(`llama-common.dll`, `ggml-rpc.dll`, `libllama-common.so`, `libmtmd.so`,
all executables) are explicitly skipped.

## Platform matrix

| Platform | Backend(s) available | Source |
|---|---|---|
| **Windows x64** | CPU + **Vulkan** (D3D12-based via Vulkan interop on Win11) | `llama-<tag>-bin-win-vulkan-x64.zip` |
| **Android arm64** | **CPU only** (Vulkan not published by upstream for Android) | `llama-<tag>-bin-android-arm64.tar.gz` |

The Win64 Vulkan ZIP bundles BOTH backends — CPU is always present as a
fallback via a co-shipped `ggml-cpu-*.dll`. Select per-call via
`FInoLlamaCppModelConfig::NumGpuLayers` (0 = pure CPU, 99 = all on GPU).

If an Android consumer later needs GPU, a build-from-source Android Vulkan
setup can be added in a separate follow-up — out of scope for initial
integration.

## Why prebuilts (and not build-from-source like LiteRT-LM)

| Concern | LiteRT-LM (build from source) | llama.cpp (prebuilts) |
|---|---|---|
| Custom export targets | Required (`//ino:LiteRtLm` with force-reference stub) | None — upstream C API is the product |
| Windows export-visibility quirks | Worked around in `BUILD.bazel` | Clean stable API |
| Dev-machine toolchain | Bazel + MSVC + Vulkan SDK + Developer Mode + long paths | **None** |
| Build time on cold cache | 30–60 minutes | ~30 seconds (download + unzip) |
| Artifact reproducibility | Pinned Bazel tag | Pinned GitHub release tag |

Matches the existing ONNX Runtime pattern, which also uses Microsoft's
prebuilt NuGet packages rather than building from source.

## Naming policy: upstream filenames preserved

Unlike `InoOnnxRuntime` (which renames `onnxruntime.dll` → `InoOnnxRuntime.dll`
and `DirectML.dll` → `InoDml.dll` to dodge verified base-name-cache
collisions with UE's NNE / Marketplace plugins), this integration ships
all llama.cpp binaries under their **original upstream filenames**.

Reason: llama.cpp ships as an interconnected graph of 20+ DLLs with PE
import tables that reference each other by base name
(`llama.dll` → `ggml.dll` → `ggml-base.dll`, plus runtime loading of the
backend `ggml-cpu-*.dll` / `ggml-vulkan.dll` via `ggml_backend_load_all`
scanning for `ggml-*.dll` patterns). Renaming any of them would require
patching the PE import tables in all dependents **and** wiring explicit
`ggml_backend_load(full_path)` calls to replace the glob scanner.

No UE 5.7 plugin currently ships llama.cpp. If a concrete collision
emerges in the future (e.g. a Marketplace plugin also ships `llama.dll`,
or we add `whisper.cpp` which uses the same `ggml.dll` by name), upgrade
to a full rename + PE-patch approach as a scoped follow-up. The directory
isolation (`Binaries/ThirdParty/InoLlamaCpp/Win64/` as a unique staging
path) provides directory-level isolation for now — DLL-loading at full
path from this directory resolves sibling imports correctly, and the
runtime auto-discovery scanner works out of the box.

This is a **pragmatic v1 tradeoff**. The plugin's UE-side naming
(`InoLlamaCpp` module, `UInoLlamaCppSubsystem` class, etc.) keeps the
Ino* prefix — only the binary filenames follow upstream.

## Getting started

```powershell
cd Plugins/InoAgents/LlamaCpp/scripts
./setup-llamacpp.ps1
```

The script:

1. Reads the pinned tag from `LlamaCpp/LLAMACPP_VERSION`.
2. Downloads two artifacts to `LlamaCpp/.cache/` (idempotent — cached if
   already present):
   - Windows: `llama-<tag>-bin-win-vulkan-x64.zip` from GitHub Releases.
   - Android: `llama-<tag>-bin-android-arm64.tar.gz` from GitHub Releases.
3. Extracts, renames, and stages into the paths shown above.
4. Writes a `.llamacpp_version` stamp so re-runs are cheap.

Safe to re-run anytime. If you delete `Source/ThirdParty/InoLlamaCpp/` or
`Binaries/ThirdParty/InoLlamaCpp/`, re-running restores them. For a full
reset use `./clean.ps1` first.

## Bumping the version

1. Check https://github.com/ggml-org/llama.cpp/releases for the latest tag.
2. Edit `LLAMACPP_VERSION` to the new build tag (e.g. `b9000`). Format is
   strictly `b\d+`.
3. Delete `Source/ThirdParty/InoLlamaCpp/.llamacpp_version` (or let the
   script detect drift — it compares staged version to pinned version).
4. Re-run `setup-llamacpp.ps1`.
5. Run the `Ino.LlamaCpp.*` smoke tests (see plugin CLAUDE.md) on both
   platforms.
6. Commit the `LLAMACPP_VERSION` bump + any API-adjustment fixups.

## CPU variants: ship all, let runtime pick

llama.cpp's Win64 Vulkan ZIP ships 14 CPU variants (haswell,
sandybridge, icelake, alderlake, cannonlake, cascadelake, cooperlake,
ivybridge, piledriver, sapphirerapids, skylakex, sse42, x64, zen4).
The Android arm64 tar.gz ships 7 (armv8.0_1, armv8.2_1, armv8.2_2,
armv8.6_1, armv9.0_1, armv9.2_1, armv9.2_2).

We stage **all** of them. llama.cpp's runtime backend-picker inspects the
user's CPU (via CPUID / `getauxval`) at model-load time and selects the
optimal variant. Total added footprint is modest (~15 MB Windows,
~31 MB Android across CPU variants combined).

## How this relates to LiteRT-LM

Both are on-device LLM runtimes, but they serve different model ecosystems:

- **LiteRT-LM** → Gemma 4 only (text/image/audio). Google's first-party
  runtime with native function-calling and chat-template-aware streaming.
- **llama.cpp** → Every other quantized LLM format that matters (Qwen,
  Phi, Llama, DeepSeek-R1-Distill, TinyLlama, NeuTTS Nano, ...). No
  function-calling support in our initial integration — deferred to a
  follow-up milestone.

Both can be loaded simultaneously; they share no state and use disjoint
module-startup DLL loading chains.

## Troubleshooting

- **`Invoke-WebRequest` fails with TLS / proxy error**: the setup script
  uses `-UseBasicParsing`. On corporate networks, set `$env:HTTPS_PROXY`
  before running.
- **`tar` not found**: Windows 10 1803+ ships `tar.exe` in `%SystemRoot%\
  system32`. If missing, install Git-for-Windows which bundles `tar.exe`
  in its PATH.
- **`.llamacpp_version` drift loops**: if staging fails partway through a
  run and the stamp file is stale, delete it (or run `clean.ps1`) and
  retry.
- **"No ggml-cpu-\*.dll variants were staged"**: upstream changed the
  CPU variant naming pattern. Update the skip-list or the inclusion
  regex in `setup-llamacpp.ps1`.
- **"llama.dll not found anywhere inside the extract"**: upstream changed
  the ZIP's internal layout. The script already searches recursively;
  inspect `.cache/win-extract-<tag>/` to see the actual layout and adjust
  the script.
- **MSYS / Git-Bash `tar` error "Cannot connect to E: resolve failed"**:
  the script invokes `%SystemRoot%\System32\tar.exe` explicitly to avoid
  this. If you hit it anyway, you're running against an unexpected
  `tar.exe` — verify `where tar` in your shell.

## What lives where

| Concern | Owned by |
|---|---|
| Pinning, downloading, staging binaries | `LlamaCpp/scripts/setup-llamacpp.ps1` |
| UBT module definition (RuntimeDependencies + UPL) | `Source/ThirdParty/InoLlamaCpp/` (Milestone B) |
| Module startup DLL preloading + backend registration | `Source/InoAgents/Private/LlamaCpp/InoLlamaCppModule.cpp` (Milestone C) |
| Blueprint API (subsystem + conversation + worker) | `Source/InoAgents/{Public,Private}/LlamaCpp/Ino*.{h,cpp}` (Milestone E) |
| Smoke tests | `Source/InoAgents/Private/SmokeTests/InoLlamaCpp*.cpp` |
