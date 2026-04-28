// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLlama.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "Containers/StringConv.h"

#include <initializer_list>

#if PLATFORM_WINDOWS
    // For GetModuleHandleW / GetModuleFileNameW — used to verify which
    // DLL Windows' base-name cache actually served when we asked to
    // load a full-path DLL.
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <windows.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

// Single definition for the shared log category declared in InoLlama.h.
DEFINE_LOG_CATEGORY(LogInoLlama);

// =============================================================
// InoAgents::LlamaCpp — DLL loader + global API vtable accessor
// =============================================================

namespace InoAgents::LlamaCpp
{

namespace
{
    /** Cached function-pointer vtable. Populated by Init(), cleared by Shutdown(). */
    FLlamaCppApi GApi;

    /** True iff Init() completed successfully. Read by GetApi(). */
    bool GApiValid = false;

    /**
     * DLL handles we own for the lifetime of the module. Opened by Init()
     * via FPlatformProcess::GetDllHandle (which reference-counts), freed
     * by Shutdown(). The order of members matches the recommended
     * Windows load order.
     *
     * On Android, only LlamaMain is populated; the Android linker
     * manages libggml*.so load/unload transparently.
     */
    struct FDllHandles
    {
        void* LibOmp      = nullptr;   // libomp140.x86_64.dll  (Windows only)
        void* GgmlBase    = nullptr;   // ggml-base.dll / libggml-base.so
        void* Ggml        = nullptr;   // ggml.dll / libggml.so
        void* GgmlVulkan  = nullptr;   // ggml-vulkan.dll (Windows only)
        void* LlamaMain   = nullptr;   // llama.dll / libllama.so
    };
    FDllHandles GHandles;

    /**
     * Per-platform library name we feed to FPlatformProcess::GetDllHandle
     * for the main llama library.
     *
     * Windows: absolute path to our staged llama.dll, resolved via
     *   IPluginManager so the loader can't pick up some other copy that
     *   happens to be on PATH.
     *
     * Android: bare soname. Android's dynamic linker resolves this via
     *   the APK's lib/<abi>/ dir (part of LD_LIBRARY_PATH for the
     *   process). The UPL's soLoadLibrary preload has already mapped
     *   the .so into the process, so dlopen just returns the existing
     *   handle.
     */
    FString ResolveMainLibraryPath()
    {
#if PLATFORM_WINDOWS
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoLlama"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Source/ThirdParty/Win64"),
            TEXT("llama.dll"));
#elif PLATFORM_ANDROID
        return FString(TEXT("libllama.so"));
#else
        return FString();
#endif
    }

#if PLATFORM_WINDOWS
    /**
     * Windows-only: our Source/ThirdParty/Win64 directory. Used both for
     * preloading sibling DLLs and for ggml_backend_load_all_from_path
     * at init.
     */
    FString ResolveWin64BinDir()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoLlama"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Source/ThirdParty/Win64"));
    }

    /**
     * Windows-only: query Windows for the full on-disk path of an
     * already-loaded DLL (by its base name). Returns empty if the DLL
     * isn't loaded, or a sentinel string on API failure.
     */
    FString GetActualLoadedModulePath(const TCHAR* BaseName)
    {
        HMODULE Handle = GetModuleHandleW(BaseName);
        if (Handle == nullptr)
        {
            return FString(TEXT("(not loaded)"));
        }
        WCHAR PathBuf[MAX_PATH + 1] = {};
        const DWORD Len = GetModuleFileNameW(Handle, PathBuf, MAX_PATH);
        if (Len == 0 || Len >= MAX_PATH)
        {
            return FString(TEXT("(GetModuleFileName failed)"));
        }
        return FString(PathBuf);
    }

    /**
     * Verify that a loaded DLL came from the path we expected. Logs a
     * Warning if Windows' base-name cache served a different copy —
     * signals a future collision with another plugin's same-named DLL.
     */
    void VerifyLoadedPath(const TCHAR* BaseName, const FString& ExpectedFullPath)
    {
        const FString ActualPath = GetActualLoadedModulePath(BaseName);

        auto Normalize = [](const FString& In) -> FString
        {
            FString Out = In;
            Out.ReplaceInline(TEXT("\\"), TEXT("/"));
            return Out.ToLower();
        };

        if (Normalize(ActualPath) == Normalize(ExpectedFullPath))
        {
            UE_LOG(LogInoLlama, Verbose,
                   TEXT("LlamaCpp: Module: verified %s is loaded from %s"),
                   BaseName, *ActualPath);
        }
        else
        {
            UE_LOG(LogInoLlama, Warning,
                   TEXT("LlamaCpp: Module: BASE-NAME CACHE COLLISION — %s loaded from %s, ")
                   TEXT("but we wanted %s. Our preload didn't win the race (another plugin ")
                   TEXT("loaded a different %s first). Symptoms may include version-skew ")
                   TEXT("bugs at runtime."),
                   BaseName, *ActualPath, *ExpectedFullPath, BaseName);
        }
    }

    /**
     * Pre-load every sibling DLL that llama.dll transitively depends on
     * or that llama.cpp's internal backend loader will LoadLibraryA by
     * bare filename at runtime. All by full path so Windows' loaded-
     * modules cache is populated with OUR copies keyed by base name
     * BEFORE anything else in the process can race.
     *
     * Load order must match the dependency graph (top of list = deepest
     * dependency, loaded first):
     *
     *   libomp140.x86_64.dll  — MSVC OpenMP redist. Imported by the
     *                           ggml-cpu-*.dll variants that use OpenMP
     *                           for CPU threading. Must be loaded before
     *                           any ggml-cpu DLL is dlopen'd by the
     *                           backend loader.
     *   ggml-base.dll         — Core ggml (tensor ops, memory, etc.).
     *   ggml.dll              — Dispatcher; depends on ggml-base.dll.
     *                           Imports the backend API + exports the
     *                           ggml_backend_* entry points we resolve.
     *   ggml-vulkan.dll       — Vulkan backend; depends on ggml +
     *                           ggml-base + OS-provided vulkan-1.dll.
     *   llama.dll             — Main library; depends on ggml.dll.
     *                           Exports the llama_* entry points.
     *
     * The 14 ggml-cpu-*.dll CPU variants are intentionally NOT preloaded
     * here. ggml_backend_load_all_from_path (called at the end of
     * Init()) glob-scans the directory for ggml-*.dll and dlopens each
     * one individually — each variant's init probes the host CPU and
     * rejects itself if the required instruction set isn't available.
     */
    bool PreloadWin64Deps(const FString& BinDir, FDllHandles& OutHandles)
    {
        struct FPreload
        {
            const TCHAR* Name;
            bool         bRequired;
            void**       StoreHandleAt;
        };

        const FPreload Preloads[] = {
            { TEXT("libomp140.x86_64.dll"), true,  &OutHandles.LibOmp     },
            { TEXT("ggml-base.dll"),        true,  &OutHandles.GgmlBase   },
            { TEXT("ggml.dll"),             true,  &OutHandles.Ggml       },
            { TEXT("ggml-vulkan.dll"),      false, &OutHandles.GgmlVulkan },
        };

        for (const FPreload& P : Preloads)
        {
            const FString FullPath = FPaths::Combine(BinDir, P.Name);
            void* Handle = FPlatformProcess::GetDllHandle(*FullPath);
            if (Handle != nullptr)
            {
                *(P.StoreHandleAt) = Handle;
                UE_LOG(LogInoLlama, Log,
                       TEXT("LlamaCpp: Module: pre-loaded %s"), P.Name);
                VerifyLoadedPath(P.Name, FullPath);
            }
            else if (P.bRequired)
            {
                UE_LOG(LogInoLlama, Error,
                       TEXT("LlamaCpp: Module: REQUIRED preload failed: %s (path=%s). ")
                       TEXT("llama.cpp will not initialise. Run ")
                       TEXT("Plugins/InoLlama/LlamaCpp/scripts/setup-llamacpp.ps1 ")
                       TEXT("to stage the binaries."),
                       P.Name, *FullPath);
                return false;
            }
            else
            {
                UE_LOG(LogInoLlama, Warning,
                       TEXT("LlamaCpp: Module: optional preload not found: %s (path=%s). ")
                       TEXT("The corresponding backend (Vulkan) will be unavailable; CPU inference still works."),
                       P.Name, *FullPath);
            }
        }
        return true;
    }
#endif // PLATFORM_WINDOWS

    /**
     * Resolve a function pointer by searching a list of DLL handles in
     * order. Returns nullptr if none of the handles export the symbol.
     */
    void* ResolveExport(const TCHAR* Name, std::initializer_list<void*> Handles)
    {
        for (void* H : Handles)
        {
            if (H == nullptr) continue;
            if (void* P = FPlatformProcess::GetDllExport(H, Name))
            {
                return P;
            }
        }
        return nullptr;
    }

    /**
     * Populate every function pointer in OutApi by resolving against the
     * given DLL handles. Returns true if every required pointer was
     * resolved.
     */
    bool ResolveApi(FLlamaCppApi& OutApi, const FDllHandles& Handles, int32& OutResolvedCount, int32& OutTotalCount)
    {
        // llama_* symbols live in llama.dll; ggml_* symbols live in
        // ggml.dll / ggml-base.dll. The search list covers both so the
        // RESOLVE macro below doesn't need to know which is which.
        const std::initializer_list<void*> SearchList = {
            Handles.LlamaMain,
            Handles.Ggml,
            Handles.GgmlBase,
        };

        bool bAllOk = true;
        int32 ResolvedCount = 0;
        int32 TotalCount = 0;

        auto ResolveOne = [&](const TCHAR* Name, void** Slot) -> bool
        {
            void* P = ResolveExport(Name, SearchList);
            *Slot = P;
            ++TotalCount;
            if (P == nullptr)
            {
                UE_LOG(LogInoLlama, Error,
                       TEXT("LlamaCpp: Api: failed to resolve symbol %s. ")
                       TEXT("Staged build at LlamaCpp/LLAMACPP_VERSION may be the wrong version ")
                       TEXT("or was built with this symbol stripped."),
                       Name);
                bAllOk = false;
            }
            else
            {
                UE_LOG(LogInoLlama, Verbose,
                       TEXT("LlamaCpp: Api: resolved %s -> %p"), Name, P);
                ++ResolvedCount;
            }
            return P != nullptr;
        };

        // Macro: resolve + cast + null-check + error-log in one line.
        #define INO_RESOLVE_LLAMA(FuncName)                                   \
            do {                                                              \
                void* P_ = nullptr;                                           \
                ResolveOne(TEXT(#FuncName), &P_);                             \
                OutApi.FuncName = reinterpret_cast<decltype(OutApi.FuncName)>(P_); \
            } while (0)

        // Module startup + backend discovery
        INO_RESOLVE_LLAMA(llama_backend_init);
        INO_RESOLVE_LLAMA(llama_backend_free);
        INO_RESOLVE_LLAMA(llama_print_system_info);
        INO_RESOLVE_LLAMA(ggml_backend_load_all_from_path);
        INO_RESOLVE_LLAMA(ggml_backend_reg_count);
        INO_RESOLVE_LLAMA(ggml_backend_reg_get);
        INO_RESOLVE_LLAMA(ggml_backend_reg_name);

        // Model
        INO_RESOLVE_LLAMA(llama_model_default_params);
        INO_RESOLVE_LLAMA(llama_model_load_from_file);
        INO_RESOLVE_LLAMA(llama_model_free);
        INO_RESOLVE_LLAMA(llama_model_get_vocab);
        INO_RESOLVE_LLAMA(llama_model_desc);
        INO_RESOLVE_LLAMA(llama_model_n_ctx_train);

        // Context
        INO_RESOLVE_LLAMA(llama_context_default_params);
        INO_RESOLVE_LLAMA(llama_init_from_model);
        INO_RESOLVE_LLAMA(llama_free);
        INO_RESOLVE_LLAMA(llama_n_ctx);

        // Memory (KV-cache)
        INO_RESOLVE_LLAMA(llama_get_memory);
        INO_RESOLVE_LLAMA(llama_memory_clear);

        // Vocab
        INO_RESOLVE_LLAMA(llama_vocab_n_tokens);
        INO_RESOLVE_LLAMA(llama_vocab_eos);
        INO_RESOLVE_LLAMA(llama_vocab_is_eog);
        INO_RESOLVE_LLAMA(llama_vocab_get_add_bos);
        INO_RESOLVE_LLAMA(llama_token_to_piece);

        // Tokenize / detokenize
        INO_RESOLVE_LLAMA(llama_tokenize);
        INO_RESOLVE_LLAMA(llama_detokenize);

        // Batch + decode
        INO_RESOLVE_LLAMA(llama_batch_init);
        INO_RESOLVE_LLAMA(llama_batch_free);
        INO_RESOLVE_LLAMA(llama_batch_get_one);
        INO_RESOLVE_LLAMA(llama_decode);
        INO_RESOLVE_LLAMA(llama_get_logits_ith);

        // Sampler chain
        INO_RESOLVE_LLAMA(llama_sampler_chain_default_params);
        INO_RESOLVE_LLAMA(llama_sampler_chain_init);
        INO_RESOLVE_LLAMA(llama_sampler_chain_add);
        INO_RESOLVE_LLAMA(llama_sampler_init_greedy);
        INO_RESOLVE_LLAMA(llama_sampler_init_dist);
        INO_RESOLVE_LLAMA(llama_sampler_init_top_k);
        INO_RESOLVE_LLAMA(llama_sampler_init_top_p);
        INO_RESOLVE_LLAMA(llama_sampler_init_min_p);
        INO_RESOLVE_LLAMA(llama_sampler_init_temp);
        INO_RESOLVE_LLAMA(llama_sampler_sample);
        INO_RESOLVE_LLAMA(llama_sampler_accept);
        INO_RESOLVE_LLAMA(llama_sampler_free);

        #undef INO_RESOLVE_LLAMA

        OutResolvedCount = ResolvedCount;
        OutTotalCount = TotalCount;
        return bAllOk;
    }
} // namespace

bool Init()
{
    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: starting llama.cpp load"));
    const double InitStartTime = FPlatformTime::Seconds();

#if PLATFORM_WINDOWS || PLATFORM_ANDROID

#if PLATFORM_WINDOWS
    // Preload every sibling DLL by full path. Seeds Windows' base-name
    // cache with our copies before llama.dll's PE imports get resolved.
    const FString BinDir = ResolveWin64BinDir();
    if (BinDir.IsEmpty())
    {
        UE_LOG(LogInoLlama, Error,
               TEXT("LlamaCpp: Module: cannot resolve plugin bin dir; llama.cpp init aborted."));
        return false;
    }
    UE_LOG(LogInoLlama, Verbose,
           TEXT("LlamaCpp: Module: resolved Win64 bin dir = %s"), *BinDir);
    if (!PreloadWin64Deps(BinDir, GHandles))
    {
        UE_LOG(LogInoLlama, Error,
               TEXT("LlamaCpp: Module: Win64 dependency preload failed; init aborted."));
        return false;
    }
#endif // PLATFORM_WINDOWS

    // Load the main library (llama.dll / libllama.so).
    const FString MainLibPath = ResolveMainLibraryPath();
    if (MainLibPath.IsEmpty())
    {
        UE_LOG(LogInoLlama, Warning,
               TEXT("LlamaCpp: Module: could not resolve llama.cpp main library path (IPluginManager failed?)."));
        return false;
    }
    UE_LOG(LogInoLlama, Verbose,
           TEXT("LlamaCpp: Module: main library path = %s"), *MainLibPath);

    GHandles.LlamaMain = FPlatformProcess::GetDllHandle(*MainLibPath);
    if (GHandles.LlamaMain == nullptr)
    {
        UE_LOG(LogInoLlama, Error,
               TEXT("LlamaCpp: Module: failed to load %s. ")
               TEXT("Did you run Plugins/InoLlama/LlamaCpp/scripts/setup-llamacpp.ps1 ")
               TEXT("and re-package?"),
               *MainLibPath);
        return false;
    }
    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: GetDllHandle succeeded for %s (handle=%p)"),
           *MainLibPath, GHandles.LlamaMain);

#if PLATFORM_WINDOWS
    VerifyLoadedPath(TEXT("llama.dll"), MainLibPath);
#endif

    // Resolve every function-pointer in the vtable by searching the
    // loaded handles. Aborts if any required symbol is missing.
    int32 ResolvedCount = 0;
    int32 TotalCount = 0;
    if (!ResolveApi(GApi, GHandles, ResolvedCount, TotalCount))
    {
        UE_LOG(LogInoLlama, Error,
               TEXT("LlamaCpp: Module: API resolution failed (%d/%d symbols resolved); aborting init."),
               ResolvedCount, TotalCount);
        return false;
    }
    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: resolved %d/%d API symbols"),
           ResolvedCount, TotalCount);

    // Register all backends.
    if (GApi.ggml_backend_load_all_from_path != nullptr)
    {
#if PLATFORM_WINDOWS
        const FTCHARToUTF8 BinDirUtf8(*BinDir);
        GApi.ggml_backend_load_all_from_path(BinDirUtf8.Get());
        UE_LOG(LogInoLlama, Log,
               TEXT("LlamaCpp: Module: ggml_backend_load_all_from_path(%s) invoked."),
               *BinDir);
#else
        // Android: pass nullptr so upstream falls back to its default.
        GApi.ggml_backend_load_all_from_path(nullptr);
        UE_LOG(LogInoLlama, Log,
               TEXT("LlamaCpp: Module: ggml_backend_load_all_from_path(nullptr) invoked (Android default search)."));
#endif
    }

    // Initialise llama.cpp's runtime globals.
    if (GApi.llama_backend_init != nullptr)
    {
        GApi.llama_backend_init();
        UE_LOG(LogInoLlama, Verbose,
               TEXT("LlamaCpp: Module: llama_backend_init invoked"));
    }

    // Summary log.
    if (GApi.llama_print_system_info != nullptr)
    {
        const char* Info = GApi.llama_print_system_info();
        UE_LOG(LogInoLlama, Log,
               TEXT("LlamaCpp: Module: llama.cpp initialised — %s"),
               Info != nullptr ? UTF8_TO_TCHAR(Info) : TEXT("(null system info)"));
    }
    else
    {
        UE_LOG(LogInoLlama, Log,
               TEXT("LlamaCpp: Module: llama.cpp initialised (system info unavailable)."));
    }

    GApiValid = true;
    const double InitElapsedMs = (FPlatformTime::Seconds() - InitStartTime) * 1000.0;
    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: Init complete (elapsed=%.1f ms, %d/%d symbols)"),
           InitElapsedMs, ResolvedCount, TotalCount);
    return true;

#else
    // iOS / Linux / macOS: not yet implemented.
    UE_LOG(LogInoLlama, Warning,
           TEXT("LlamaCpp: Module: llama.cpp is not yet available on this platform."));
    return false;
#endif
}

void Shutdown()
{
    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: Shutdown starting (was_valid=%s)"),
           GApiValid ? TEXT("true") : TEXT("false"));

    // Clear the "valid" flag first so any late callers of GetApi() see
    // nullptr rather than a vtable belonging to a DLL we are about to
    // unload.
    const bool bWasValid = GApiValid;
    GApiValid = false;

    // Tear down llama.cpp's runtime globals BEFORE unloading the DLL.
    if (bWasValid && GApi.llama_backend_free != nullptr)
    {
        GApi.llama_backend_free();
        UE_LOG(LogInoLlama, Verbose,
               TEXT("LlamaCpp: Module: llama_backend_free invoked"));
    }

    // Zero the vtable now.
    GApi = FLlamaCppApi{};

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    // Release in reverse dependency order.
    auto Free = [](const TCHAR* Name, void*& Handle)
    {
        if (Handle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(Handle);
            UE_LOG(LogInoLlama, Verbose,
                   TEXT("LlamaCpp: Module: FreeDllHandle(%s) complete"), Name);
            Handle = nullptr;
        }
    };
    Free(TEXT("llama"),        GHandles.LlamaMain);
    Free(TEXT("ggml-vulkan"),  GHandles.GgmlVulkan);
    Free(TEXT("ggml"),         GHandles.Ggml);
    Free(TEXT("ggml-base"),    GHandles.GgmlBase);
    Free(TEXT("libomp"),       GHandles.LibOmp);
#endif

    UE_LOG(LogInoLlama, Log,
           TEXT("LlamaCpp: Module: Shutdown complete"));
}

const FLlamaCppApi* GetApi()
{
    if (!GApiValid)
    {
        // Log once — consumers call GetApi() on every hot-path entry and
        // null-check the result, so we'd flood the log if we warned every
        // call. The single warning is enough to hint "you tried to use
        // llama.cpp but Init didn't succeed / Shutdown already ran".
        static bool bWarnedOnce = false;
        if (!bWarnedOnce)
        {
            bWarnedOnce = true;
            UE_LOG(LogInoLlama, Warning,
                   TEXT("LlamaCpp: Api: GetApi() called but vtable is not valid ")
                   TEXT("(Init not yet called, failed, or Shutdown already ran) — returning nullptr. ")
                   TEXT("Further nullptr returns will be silent."));
        }
        return nullptr;
    }
    return &GApi;
}

} // namespace InoAgents::LlamaCpp

// =============================================================
// FInoLlamaModule — UE module class
// =============================================================

void FInoLlamaModule::StartupModule()
{
    InoAgents::LlamaCpp::Init();
}

void FInoLlamaModule::ShutdownModule()
{
    InoAgents::LlamaCpp::Shutdown();
}

IMPLEMENT_MODULE(FInoLlamaModule, InoLlama)
