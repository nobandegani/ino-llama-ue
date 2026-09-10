# Third-Party Notices

InoLlama is distributed under the [Apache License 2.0](LICENSE), Copyright 2026 Inoland.

That grant covers Inoland's own code — `Source/InoLlama/` and `LlamaCpp/scripts/`. This
repository also redistributes 39 prebuilt llama.cpp / ggml binaries under `Source/ThirdParty/`,
which are MIT-licensed. The table below is authoritative where it disagrees with any other
document here.

Unlike the sibling plugins in this set, **nothing in this repository is GPL, proprietary, or
revenue-capped.**

---

## Summary

| Path | Component | License |
|---|---|---|
| `Source/InoLlama/`, `LlamaCpp/scripts/` | InoLlama | Apache-2.0 |
| `Source/ThirdParty/Public/*.h` | llama.cpp + ggml C API headers | MIT |
| `Source/ThirdParty/Win64/*.dll` (19) | llama.cpp + ggml runtime | MIT |
| `Source/ThirdParty/Android/arm64-v8a/*.so` (11) | llama.cpp + ggml runtime | MIT |
| `Source/ThirdParty/Mac/llama.framework` | llama.cpp + ggml runtime | MIT |
| `Source/ThirdParty/IOS/llama.framework` (device + Simulator) | llama.cpp + ggml runtime | MIT |
| `Source/ThirdParty/Win64/libomp140.x86_64.dll` | LLVM OpenMP runtime | Apache-2.0 WITH LLVM-exception |
| `LlamaCpp/vendor/llama.cpp` *(submodule)* | ggml-org/llama.cpp | MIT |

The submodule is a **reference**, not redistribution — cloning this repo does not copy its
contents unless you pass `--recurse-submodules`.

---

## 1. llama.cpp and ggml — The ggml authors

**License:** MIT. **Upstream:** https://github.com/ggml-org/llama.cpp
**Copyright:** (c) 2023-2026 The ggml authors
**Full text:** `LlamaCpp/vendor/llama.cpp/LICENSE`

MIT requires that the copyright notice and permission notice accompany copies of the software.
This file, together with the `LICENSE` retained in the submodule, serves that purpose for the
binaries under `Source/ThirdParty/`.

### Pin

| Item | Value |
|---|---|
| Release tag | `b9016` (`LlamaCpp/LLAMACPP_VERSION`) |
| Submodule commit | `846262d7875dcabf502a150fa3d7b9c770dde7eb` |
| `git describe` | `gguf-v0.18.0-842-g846262d78` |
| Vulkan headers | `vulkan-sdk-1.4.341.0` (`LlamaCpp/VULKAN_HEADERS_VERSION`) |

### Provenance of each platform's binaries

Three of the four platforms ship **unmodified upstream release artifacts**:

- **Win64** — extracted from upstream's `llama-b9016-bin-win-vulkan-x64.zip`
- **macOS / iOS** — extracted from upstream's `llama-b9016-xcframework.zip`

**Android arm64-v8a is built from source**, because upstream's Android release asset is CPU-only
and publishes no Vulkan build. It is compiled from the pinned submodule with NDK r28b, Vulkan
enabled, `CMAKE_BUILD_TYPE=Release`, and tests/examples/tools/server off. The exact CMake
invocation is in `LlamaCpp/scripts/setup-llamacpp.ps1` and is the complete record of how those
`.so` files were produced.

**Modifications by Inoland:** none to llama.cpp or ggml source. The macOS framework is re-staged
from the upstream "versioned" layout (`Versions/A/llama` plus root symlinks) into a flattened
layout, because symlinks inside zip archives extract unreliably on Windows. The binary itself is
byte-identical to upstream's.

### Embedded build paths in the Android binaries

> The committed Android `.so` files contain absolute paths from the machine that built them —
> `libllama.so` embeds 154 distinct ones. This is not a licensing issue but it is a disclosure
> one, so it is recorded here.

ggml's `GGML_ASSERT` and `GGML_ABORT` macros expand `__FILE__`, and unlike a plain `assert()`
they are deliberately **not** removed by `NDEBUG` — the assertion text has to survive to be
useful. Without a prefix map every `__FILE__` becomes the builder's absolute path.

`setup-llamacpp.ps1` now passes
`-ffile-prefix-map=<vendor-src>=llama.cpp -ffile-prefix-map=<build-dir>=build` for both C and
C++, which rewrites `__FILE__`, debug info, and profiling paths together while keeping assertion
messages readable. **Re-running the Android staging step replaces the affected libraries.** The
Win64, macOS and iOS artifacts were never affected — they are built on upstream CI.

---

## 2. LLVM OpenMP runtime (Win64 only)

`Source/ThirdParty/Win64/libomp140.x86_64.dll` ships as part of upstream's Windows release zip.
It is the LLVM OpenMP runtime, under **Apache-2.0 WITH LLVM-exception**.

It is preloaded first on Windows because `ggml-base.dll` imports from it. The Android build sets
`-DGGML_OPENMP=OFF`, so no equivalent is needed there.

---

## 3. Vulkan headers (build-time only)

The Android build consumes Vulkan headers at the version pinned in
`LlamaCpp/VULKAN_HEADERS_VERSION` (`vulkan-sdk-1.4.341.0`), from the NDK's bundled copy, along
with the NDK's `glslc` shader compiler. These are **build inputs only** — no Vulkan headers or
loader are redistributed here. `libvulkan.so` is an Android platform library present on API 24+
devices, so nothing needs shipping.

---

## 4. Model weights

**No GGUF model weights are in this repository.** Models are supplied by the consuming
application and remain subject to their own licenses, which for many open-weight models include
use restrictions that MIT and Apache-2.0 do not. Check the license of any model you ship.

---

## Reporting a problem with these notices

If a component is misattributed or a notice is missing, please open an issue at
https://github.com/nobandegani/ino-llama-ue/issues and it will be corrected.
