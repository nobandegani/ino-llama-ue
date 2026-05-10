// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#include "InoLlama.h"            // FLlamaCppApi + GetApi + LogInoLlama

/**
 * Console command: Ino.LlamaCpp.VtableTest
 *
 * Walks every member of FLlamaCppApi and reports OK / MISSING for each.
 * Useful after:
 *   - Bumping LLAMACPP_VERSION (to catch upstream-renamed / removed exports)
 *   - Adding new members to FLlamaCppApi (to verify the new RESOLVE lines
 *     actually resolve)
 *   - Any runtime behaviour that claims "function pointer was null"
 *
 * This test is purely informational — it does NOT call any of the
 * resolved functions, so it's safe to run any time after module
 * startup regardless of whether a model is loaded.
 *
 * Expected output on a healthy install: "40 OK, 0 MISSING" (or whatever
 * the current total is) with every member logged OK.
 */

namespace
{
    // Helper macro: check slot, bump counters, log at Log (ok) / Error
    // (missing). UE_LOG requires the verbosity to be a compile-time
    // token so we can't pass a ternary — two hardcoded branches instead.
    #define INO_REPORT_MEMBER(Member)                                 \
        do {                                                          \
            if (Api->Member != nullptr)                               \
            {                                                         \
                ++NumOk;                                              \
                UE_LOG(LogInoLlama, Log,                              \
                       TEXT("    [OK]      %s"), TEXT(#Member));      \
            }                                                         \
            else                                                      \
            {                                                         \
                ++NumMissing;                                         \
                UE_LOG(LogInoLlama, Error,                            \
                       TEXT("    [MISSING] %s"), TEXT(#Member));      \
            }                                                         \
        } while (0)

    void RunVtableTest(const TArray<FString>& /*Args*/)
    {
        const InoAgents::LlamaCpp::FLlamaCppApi* Api = InoAgents::LlamaCpp::GetApi();
        if (Api == nullptr)
        {
            UE_LOG(LogInoLlama, Error,
                   TEXT("Ino.LlamaCpp.VtableTest: GetApi() returned nullptr. ")
                   TEXT("llama.cpp did not initialise during module startup — scroll up ")
                   TEXT("to find the specific DLL-load or export-resolution error from ")
                   TEXT("InoLlamaCpp::Init()."));
            return;
        }

        UE_LOG(LogInoLlama, Log, TEXT("=== Ino.LlamaCpp.VtableTest ==="));

        int32 NumOk = 0;
        int32 NumMissing = 0;

        UE_LOG(LogInoLlama, Log, TEXT("  Module startup:"));
        INO_REPORT_MEMBER(llama_backend_init);
        INO_REPORT_MEMBER(llama_backend_free);
        INO_REPORT_MEMBER(llama_print_system_info);
        INO_REPORT_MEMBER(ggml_backend_load_all_from_path);
        INO_REPORT_MEMBER(ggml_backend_reg_count);
        INO_REPORT_MEMBER(ggml_backend_reg_get);
        INO_REPORT_MEMBER(ggml_backend_reg_name);

        UE_LOG(LogInoLlama, Log, TEXT("  Model:"));
        INO_REPORT_MEMBER(llama_model_default_params);
        INO_REPORT_MEMBER(llama_model_load_from_file);
        INO_REPORT_MEMBER(llama_model_free);
        INO_REPORT_MEMBER(llama_model_get_vocab);
        INO_REPORT_MEMBER(llama_model_desc);
        INO_REPORT_MEMBER(llama_model_n_ctx_train);

        UE_LOG(LogInoLlama, Log, TEXT("  Context:"));
        INO_REPORT_MEMBER(llama_context_default_params);
        INO_REPORT_MEMBER(llama_init_from_model);
        INO_REPORT_MEMBER(llama_free);
        INO_REPORT_MEMBER(llama_n_ctx);

        UE_LOG(LogInoLlama, Log, TEXT("  Memory / KV-cache:"));
        INO_REPORT_MEMBER(llama_get_memory);
        INO_REPORT_MEMBER(llama_memory_clear);

        UE_LOG(LogInoLlama, Log, TEXT("  Vocab:"));
        INO_REPORT_MEMBER(llama_vocab_n_tokens);
        INO_REPORT_MEMBER(llama_vocab_eos);
        INO_REPORT_MEMBER(llama_vocab_is_eog);
        INO_REPORT_MEMBER(llama_vocab_get_add_bos);
        INO_REPORT_MEMBER(llama_token_to_piece);

        UE_LOG(LogInoLlama, Log, TEXT("  Tokenize / detokenize:"));
        INO_REPORT_MEMBER(llama_tokenize);
        INO_REPORT_MEMBER(llama_detokenize);

        UE_LOG(LogInoLlama, Log, TEXT("  Batch + decode:"));
        INO_REPORT_MEMBER(llama_batch_init);
        INO_REPORT_MEMBER(llama_batch_free);
        INO_REPORT_MEMBER(llama_batch_get_one);
        INO_REPORT_MEMBER(llama_decode);
        INO_REPORT_MEMBER(llama_get_logits_ith);

        UE_LOG(LogInoLlama, Log, TEXT("  Sampler chain:"));
        INO_REPORT_MEMBER(llama_sampler_chain_default_params);
        INO_REPORT_MEMBER(llama_sampler_chain_init);
        INO_REPORT_MEMBER(llama_sampler_chain_add);
        INO_REPORT_MEMBER(llama_sampler_init_greedy);
        INO_REPORT_MEMBER(llama_sampler_init_dist);
        INO_REPORT_MEMBER(llama_sampler_init_top_k);
        INO_REPORT_MEMBER(llama_sampler_init_top_p);
        INO_REPORT_MEMBER(llama_sampler_init_min_p);
        INO_REPORT_MEMBER(llama_sampler_init_temp);
        INO_REPORT_MEMBER(llama_sampler_sample);
        INO_REPORT_MEMBER(llama_sampler_accept);
        INO_REPORT_MEMBER(llama_sampler_free);

        const int32 Total = NumOk + NumMissing;
        if (NumMissing == 0)
        {
            UE_LOG(LogInoLlama, Log,
                   TEXT("=== VtableTest PASSED — %d/%d resolved ==="),
                   NumOk, Total);
        }
        else
        {
            UE_LOG(LogInoLlama, Error,
                   TEXT("=== VtableTest FAILED — %d resolved, %d MISSING (out of %d) ==="),
                   NumOk, NumMissing, Total);
        }
    }

    #undef INO_REPORT_MEMBER

    static FAutoConsoleCommand GVtableCmd(
        TEXT("Ino.LlamaCpp.VtableTest"),
        TEXT("Iterate every member of FLlamaCppApi and log OK / MISSING. "
             "No model required. See InoLlamaCppVtableTest.cpp."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunVtableTest));
} // namespace
