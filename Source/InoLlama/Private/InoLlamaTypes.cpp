// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLlamaTypes.h"

// Pulls in the full llama.cpp + ggml C API. The translation glue below
// needs the actual struct layout + enum constants.
#include "InoLlama.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Map our compact KV dtype enum to the matching ggml_type. Any
     * value not in the switch falls back to F16 (the safe default).
     */
    ggml_type ToGgmlType(EInoLlamaKvDtype Dtype)
    {
        switch (Dtype)
        {
            case EInoLlamaKvDtype::F16:  return GGML_TYPE_F16;
            case EInoLlamaKvDtype::F32:  return GGML_TYPE_F32;
            case EInoLlamaKvDtype::Q8_0: return GGML_TYPE_Q8_0;
            case EInoLlamaKvDtype::Q4_0: return GGML_TYPE_Q4_0;
        }
        return GGML_TYPE_F16;
    }

    llama_split_mode ToSplitMode(EInoLlamaSplitMode Mode)
    {
        switch (Mode)
        {
            case EInoLlamaSplitMode::None:   return LLAMA_SPLIT_MODE_NONE;
            case EInoLlamaSplitMode::Layer:  return LLAMA_SPLIT_MODE_LAYER;
            case EInoLlamaSplitMode::Row:    return LLAMA_SPLIT_MODE_ROW;
            case EInoLlamaSplitMode::Tensor: return LLAMA_SPLIT_MODE_TENSOR;
        }
        return LLAMA_SPLIT_MODE_LAYER;
    }

    llama_flash_attn_type ToFlashAttnType(EInoLlamaFlashAttnType Type)
    {
        switch (Type)
        {
            case EInoLlamaFlashAttnType::Auto:     return LLAMA_FLASH_ATTN_TYPE_AUTO;
            case EInoLlamaFlashAttnType::Disabled: return LLAMA_FLASH_ATTN_TYPE_DISABLED;
            case EInoLlamaFlashAttnType::Enabled:  return LLAMA_FLASH_ATTN_TYPE_ENABLED;
        }
        return LLAMA_FLASH_ATTN_TYPE_AUTO;
    }

    const TCHAR* FlashAttnTypeName(llama_flash_attn_type Type)
    {
        switch (Type)
        {
            case LLAMA_FLASH_ATTN_TYPE_AUTO:     return TEXT("auto");
            case LLAMA_FLASH_ATTN_TYPE_DISABLED: return TEXT("disabled");
            case LLAMA_FLASH_ATTN_TYPE_ENABLED:  return TEXT("enabled");
        }
        return TEXT("unknown");
    }
}

void FInoLlamaModelParams::ApplyTo(struct llama_model_params& Native) const
{
    Native.n_gpu_layers   = NumGpuLayers;
    Native.main_gpu       = MainGpu;
    Native.split_mode     = ToSplitMode(SplitMode);
    Native.vocab_only     = bVocabOnly;
    Native.use_mmap       = bUseMmap;
    Native.use_mlock      = bUseMlock;
    Native.check_tensors  = bCheckTensors;
}

void FInoLlamaContextParams::ApplyTo(struct llama_context_params& Native) const
{
    if (NumCtx > 0)
    {
        Native.n_ctx = static_cast<uint32_t>(NumCtx);
    }
    if (NumBatch > 0)
    {
        Native.n_batch = static_cast<uint32_t>(NumBatch);
    }
    if (NumUBatch > 0)
    {
        Native.n_ubatch = static_cast<uint32_t>(NumUBatch);
    }
    if (NumSeqMax > 0)
    {
        Native.n_seq_max = static_cast<uint32_t>(NumSeqMax);
    }
    if (NumThreads > 0)
    {
        Native.n_threads       = NumThreads;
        // If the caller didn't override NumThreadsBatch, keep it tied
        // to NumThreads — matches the previous hardcoded behavior.
        Native.n_threads_batch = (NumThreadsBatch > 0) ? NumThreadsBatch : NumThreads;
    }
    else if (NumThreadsBatch > 0)
    {
        Native.n_threads_batch = NumThreadsBatch;
    }

    Native.flash_attn_type = ToFlashAttnType(FlashAttnType);
    Native.offload_kqv     = bOffloadKQV;
    Native.no_perf         = bDisablePerf;
    Native.type_k          = ToGgmlType(KvDtypeK);
    Native.type_v          = ToGgmlType(KvDtypeV);
}

// ============================================================================
//  High-level helpers
// ============================================================================

namespace InoAgents::LlamaCpp
{
    INOLLAMA_API struct llama_model* LoadModelFromFile(
        const FString& ModelPath,
        const FInoLlamaModelParams& Params,
        FString* OutError)
    {
        const FLlamaCppApi* Api = GetApi();
        if (Api == nullptr)
        {
            const FString Err(TEXT("InoLlama vtable unavailable. ")
                              TEXT("llama.cpp runtime may not be staged on this platform."));
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        if (!IFileManager::Get().FileExists(*ModelPath))
        {
            const FString Err = FString::Printf(
                TEXT("GGUF model not found: %s"), *ModelPath);
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        struct llama_model_params Native = Api->llama_model_default_params();
        Params.ApplyTo(Native);

        const double T0 = FPlatformTime::Seconds();
        const FTCHARToUTF8 PathUtf8(*ModelPath);
        struct llama_model* Model = Api->llama_model_load_from_file(PathUtf8.Get(), Native);

        if (Model == nullptr)
        {
            const FString Err = FString::Printf(
                TEXT("llama_model_load_from_file failed for '%s'."), *ModelPath);
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        UE_LOG(LogInoLlama, Log,
               TEXT("InoLlama: model loaded '%s' in %.1f ms (n_gpu_layers=%d, mmap=%s, mlock=%s)"),
               *FPaths::GetCleanFilename(ModelPath),
               (FPlatformTime::Seconds() - T0) * 1000.0,
               Params.NumGpuLayers,
               Params.bUseMmap  ? TEXT("yes") : TEXT("no"),
               Params.bUseMlock ? TEXT("yes") : TEXT("no"));

        return Model;
    }

    INOLLAMA_API struct llama_context* CreateContext(
        struct llama_model* Model,
        const FInoLlamaContextParams& Params,
        FString* OutError)
    {
        const FLlamaCppApi* Api = GetApi();
        if (Api == nullptr)
        {
            const FString Err(TEXT("InoLlama vtable unavailable."));
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        if (Model == nullptr)
        {
            const FString Err(TEXT("Model is null."));
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        struct llama_context_params Native = Api->llama_context_default_params();
        Params.ApplyTo(Native);

        struct llama_context* Ctx = Api->llama_init_from_model(Model, Native);
        if (Ctx == nullptr)
        {
            const FString Err(TEXT("llama_init_from_model returned null."));
            if (OutError) *OutError = Err;
            UE_LOG(LogInoLlama, Error, TEXT("%s"), *Err);
            return nullptr;
        }

        UE_LOG(LogInoLlama, Log,
               TEXT("InoLlama: context created (n_ctx=%u, n_threads=%d, flash_attn=%s, offload_kqv=%s)"),
               Api->llama_n_ctx(Ctx),
               Native.n_threads,
               FlashAttnTypeName(Native.flash_attn_type),
               Native.offload_kqv ? TEXT("yes") : TEXT("no"));

        return Ctx;
    }
}
