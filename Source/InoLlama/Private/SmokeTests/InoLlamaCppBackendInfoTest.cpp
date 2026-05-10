// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#include "InoLlama.h"            // InoAgents::LlamaCpp::GetApi + FLlamaCppApi + LogInoLlama

#include "ggml-backend.h"        // for ggml_backend_reg type (used in vtable sig)

/**
 * Console command for validating the llama.cpp module-startup integration
 * end-to-end:
 *
 *   Ino.LlamaCpp.BackendInfoTest
 *     No model required. Logs llama_print_system_info() (build features:
 *     AVX2 / BMI2 / BLAS / etc. compiled in) and enumerates every
 *     ggml_backend that successfully registered on the host
 *     (ggml_backend_reg_count + reg_get + reg_name).
 *
 *     Expected output on Win64:
 *       - llama.cpp version line + a feature-flags string
 *       - A list of registered backends — typically one ggml-cpu-*
 *         variant (whichever matches the host CPU) plus "Vulkan"
 *         (assuming a Vulkan driver is installed).
 *
 *     Expected output on Android arm64:
 *       - Same feature-flags line (ARM-specific)
 *       - A single libggml-cpu-android_* variant registered.
 *
 *   Use this as the first thing to run after a llama.cpp version bump:
 *   proves the DLL boundary is sound and backend discovery picked up
 *   the expected variants. If this test passes, we know Init() +
 *   ggml_backend_load_all_from_path + llama_backend_init all worked.
 *
 *   If it fails (GetApi returns nullptr), the root cause is usually
 *   one of:
 *     - Plugins/InoLlama/LlamaCpp/scripts/setup-llamacpp.ps1 not run
 *     - A required DLL preload failed (check the module-startup log)
 *     - A required export couldn't be resolved (API drift from our
 *       pinned LLAMACPP_VERSION tag)
 */

namespace
{
    void RunBackendInfoTest(const TArray<FString>& /*Args*/)
    {
        const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();
        if (Api == nullptr)
        {
            UE_LOG(LogInoLlama, Error,
                   TEXT("Ino.LlamaCpp.BackendInfoTest: GetApi() returned nullptr. ")
                   TEXT("llama.cpp did not initialise during module startup — scroll up to ")
                   TEXT("find the specific DLL-load or export-resolution error from ")
                   TEXT("InoLlamaCpp::Init()."));
            return;
        }

        UE_LOG(LogInoLlama, Log, TEXT("=== Ino.LlamaCpp.BackendInfoTest ==="));

        // -----------------------------------------------------------------
        // 1. System-info / build flags string from llama.cpp itself.
        // -----------------------------------------------------------------
        if (Api->llama_print_system_info != nullptr)
        {
            const char* Info = Api->llama_print_system_info();
            UE_LOG(LogInoLlama, Log,
                   TEXT("  llama.cpp system info: %s"),
                   Info != nullptr ? UTF8_TO_TCHAR(Info) : TEXT("(null)"));
        }
        else
        {
            UE_LOG(LogInoLlama, Warning,
                   TEXT("  llama_print_system_info resolved as nullptr — the vtable is incomplete."));
        }

        // -----------------------------------------------------------------
        // 2. Enumerate every backend that registered successfully. This is
        //    the interesting part — it tells us which ggml-cpu-* variant
        //    the runtime picker chose AND whether Vulkan registration
        //    succeeded (will appear on hosts with a working Vulkan driver
        //    only; absence doesn't indicate a bug, just no GPU path).
        // -----------------------------------------------------------------
        if (Api->ggml_backend_reg_count == nullptr ||
            Api->ggml_backend_reg_get   == nullptr ||
            Api->ggml_backend_reg_name  == nullptr)
        {
            UE_LOG(LogInoLlama, Warning,
                   TEXT("  Backend enumeration vtable incomplete — skipping registered-backend listing."));
            return;
        }

        const size_t NumBackends = Api->ggml_backend_reg_count();
        UE_LOG(LogInoLlama, Log,
               TEXT("  %llu ggml backend(s) registered:"),
               (unsigned long long)NumBackends);

        if (NumBackends == 0)
        {
            UE_LOG(LogInoLlama, Warning,
                   TEXT("  No backends registered. Inference will fail. ")
                   TEXT("Did ggml_backend_load_all_from_path find any ggml-*.dll / libggml-*.so ")
                   TEXT("in Source/ThirdParty/<platform>/ ?"));
            return;
        }

        for (size_t i = 0; i < NumBackends; ++i)
        {
            struct ggml_backend_reg* Reg = Api->ggml_backend_reg_get(i);
            if (Reg == nullptr)
            {
                UE_LOG(LogInoLlama, Warning, TEXT("    [%llu] (null reg)"),
                       (unsigned long long)i);
                continue;
            }
            const char* Name = Api->ggml_backend_reg_name(Reg);
            UE_LOG(LogInoLlama, Log,
                   TEXT("    [%llu] %s"),
                   (unsigned long long)i,
                   Name != nullptr ? UTF8_TO_TCHAR(Name) : TEXT("(null name)"));
        }

        UE_LOG(LogInoLlama, Log, TEXT("=== BackendInfoTest complete ==="));
    }

    static FAutoConsoleCommand GBackendInfoCmd(
        TEXT("Ino.LlamaCpp.BackendInfoTest"),
        TEXT("Log llama.cpp build info + list every ggml backend that registered successfully. "
             "No model required. See InoLlamaCppBackendInfoTest.cpp."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunBackendInfoTest));
} // namespace
