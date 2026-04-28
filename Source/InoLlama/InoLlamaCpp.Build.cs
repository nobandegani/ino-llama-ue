// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// External UE module that exposes the prebuilt llama.cpp runtime to the
/// InoAgents plugin.
///
/// The binaries consumed here are downloaded and staged by
///   Plugins/InoAgents/LlamaCpp/scripts/setup-llamacpp.ps1
/// which pulls the official upstream release artifacts for the tag pinned
/// in LlamaCpp/LLAMACPP_VERSION, and writes:
///
///   Source/ThirdParty/InoLlamaCpp/
///     Public/                          llama.cpp C API headers (compile-time only)
///
///   Binaries/ThirdParty/InoLlamaCpp/
///     Win64/                           19 DLLs: main llama + ggml + 14 CPU
///                                      microarch variants + ggml-vulkan +
///                                      libomp redist
///     Android/arm64-v8a/               10 .so files: main libllama + libggml
///                                      + 7 ARM tier CPU variants
///
/// Consumers #include "llama.h" for the type definitions (llama_model,
/// llama_context, llama_batch, etc.) but do NOT call the exported
/// functions directly. All llama_* + ggml_backend_* calls go through a
/// function-pointer vtable resolved at module startup by
///   Source/InoAgents/Private/LlamaCpp/InoLlamaCppModule.{h,cpp}
/// (Milestone C).
///
/// Because the headers define LLAMA_API / GGML_API as empty when neither
/// LLAMA_SHARED nor LLAMA_BUILD is defined, any direct call to a llama_*
/// or ggml_* function from our code will produce an unresolved-external
/// link error — which is the intended guardrail. There is no implicit
/// linking against llama.lib; upstream's Windows release ZIP doesn't even
/// ship one.
///
/// Why no rename (unlike InoOnnxRuntime):
///   llama.cpp ships as a graph of ~20 interdependent DLLs. llama.dll
///   imports ggml.dll which imports ggml-base.dll, and libggml loads
///   ggml-cpu-*.dll / ggml-vulkan.dll at runtime by glob-scanning for
///   ggml-*.dll. Renaming any would require PE-patching every static
///   import AND replacing the glob scanner with explicit
///   ggml_backend_load(full_path) calls. No UE 5.7 plugin currently
///   ships llama.cpp, so no concrete collision today. The directory
///   isolation (Binaries/ThirdParty/InoLlamaCpp/Win64/ as a unique
///   staging path) is sufficient; if a future Marketplace plugin also
///   ships llama.dll, upgrade to full rename + PE-patch as a scoped
///   follow-up. See Plugins/InoAgents/LlamaCpp/README.md for the full
///   rationale.
///
/// Vulkan backend note:
///   ggml-vulkan.dll statically imports vulkan-1.dll. vulkan-1.dll is
///   part of the Vulkan runtime loader bundled with Windows 10 1803+
///   and every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed
///   by us — Windows provides it. Same story on Android: libvulkan.so
///   is part of the platform from API 24+. We target API 26+.
/// </summary>
public class InoLlamaCpp : ModuleRules
{
    public InoLlamaCpp(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;

        // Public headers for llama.cpp. Consumers reference them as
        //     #include "llama.h"
        //     #include "ggml.h"
        // rather than with relative paths, so expose as a system include.
        PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Windows: no implicit linking. No PublicAdditionalLibraries,
            // no PublicDelayLoadDLLs. Runtime consumers resolve every
            // llama_* and ggml_backend_* entry via GetProcAddress on
            // llama.dll, loaded at full path by InoLlamaCppModule::Init.
            //
            // Main llama.dll gets loaded by the UE module. Its PE imports
            // on ggml.dll cascade automatically via Windows' default DLL
            // search path (same directory as the loaded module). CPU
            // backend variants + ggml-vulkan.dll are picked up at runtime
            // by ggml_backend_load_all's glob scan of the directory.
            //
            // RuntimeDependencies here ensures UE's packager stages every
            // file alongside the shipped executable.
            string Win64BinDirRel = "$(PluginDir)/Binaries/ThirdParty/InoLlamaCpp/Win64";
            string Win64BinDirAbs = Path.Combine(
                PluginDirectory, "Binaries/ThirdParty/InoLlamaCpp/Win64");

            // Required files — always present on a correctly-staged tree.
            string[] RequiredWin64 = new string[]
            {
                "llama.dll",           // main library
                "ggml.dll",            // dispatcher
                "ggml-base.dll",       // base implementation
                "ggml-vulkan.dll",     // Vulkan backend
                "libomp140.x86_64.dll" // MSVC OpenMP redist (used by ggml-cpu-*)
            };
            foreach (string name in RequiredWin64)
            {
                RuntimeDependencies.Add(Win64BinDirRel + "/" + name);
            }

            // CPU microarchitecture variants. llama.cpp's runtime backend
            // picker selects the optimal one per-CPU at model-load time —
            // only ONE variant actually gets loaded into the process; the
            // other 13 remain on disk. Enumerate the staged dir so we
            // don't have to hardcode all 14 tier names (haswell,
            // sandybridge, icelake, alderlake, etc.) — the exact set may
            // shift between upstream releases.
            //
            // Staging type MUST be StagedFileType.SystemNonUFS here (not
            // the default NonUFS). Reason: the default causes Live Coding
            // to scan every .dll in RuntimeDependencies as a potential
            // hot-patch target, which produces 13 "Cannot enable module
            // X because it is not loaded by this process" error lines on
            // every Live Coding compile — one per unused variant. Marking
            // them SystemNonUFS tells UBT they're system-ish files that
            // LiveCoding should skip, while still getting them staged
            // into packaged builds. Harmless but noisy otherwise.
            if (Directory.Exists(Win64BinDirAbs))
            {
                string[] cpuVariants = Directory.GetFiles(
                    Win64BinDirAbs, "ggml-cpu-*.dll");
                foreach (string path in cpuVariants)
                {
                    RuntimeDependencies.Add(
                        Win64BinDirRel + "/" + Path.GetFileName(path),
                        StagedFileType.SystemNonUFS);
                }
            }
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            // Android arm64-v8a artifacts staged by setup-llamacpp.ps1
            // under Binaries/ThirdParty/InoLlamaCpp/Android/arm64-v8a/.
            //
            // Same as the ORT integration, we do NOT use
            // PublicAdditionalLibraries on Android. Implicitly linking
            // against libllama.so would add a DT_NEEDED entry to
            // libUnreal.so, and if a future Marketplace plugin also
            // ships its own libllama.so at a different ABI the dynamic
            // linker could fail to satisfy versioned symbol references.
            // Keeping the load explicit (dlopen via FPlatformProcess::
            // GetDllHandle in InoLlamaCppModule) keeps us ABI-isolated.
            string Arm64BinDirAbs = Path.Combine(
                PluginDirectory, "Binaries/ThirdParty/InoLlamaCpp/Android/arm64-v8a");

            // Required files — every shipment.
            string[] RequiredAndroid = new string[]
            {
                "libllama.so",    // main library
                "libggml.so",     // dispatcher
                "libggml-base.so" // base implementation
            };
            foreach (string name in RequiredAndroid)
            {
                string soPath = Path.Combine(Arm64BinDirAbs, name);
                if (File.Exists(soPath))
                {
                    RuntimeDependencies.Add(soPath);
                }
            }

            // CPU variants (ARM tier — armv8.0/8.2/8.6/9.0/9.2). Only one
            // gets loaded at runtime; mark the rest as SystemNonUFS for
            // parity with the Win64 branch (see rationale above). Live
            // Coding on Android is uncommon but the classification is the
            // correct one for system-ish redistributables either way.
            if (Directory.Exists(Arm64BinDirAbs))
            {
                string[] cpuVariants = Directory.GetFiles(
                    Arm64BinDirAbs, "libggml-cpu-*.so");
                foreach (string path in cpuVariants)
                {
                    RuntimeDependencies.Add(path, StagedFileType.SystemNonUFS);
                }
            }

            // Apply the UPL (Unreal Plugin Language) XML that tells UE's
            // APK packager to copy each .so into lib/arm64-v8a/ and emit
            // a System.loadLibrary("llama") call so libllama.so is resident
            // by the time InoLlamaCppModule::Init runs. libllama.so's
            // DT_NEEDED on libggml.so cascades via the Android linker.
            AdditionalPropertiesForReceipt.Add(
                "AndroidPlugin",
                Path.Combine(ModuleDirectory, "InoLlamaCpp_UPL_Android.xml"));
        }
        else
        {
            // iOS / Linux / macOS not yet implemented. Any plugin code that
            // #includes llama.cpp headers and tries to resolve a llama_*
            // function pointer at runtime will get nullptr on these
            // platforms. See Source/InoAgents/Private/LlamaCpp/
            // InoLlamaCppStubs_NonSupported.cpp for the graceful-fail
            // stubs (added in Milestone E).
        }
    }
}
