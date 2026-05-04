// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoLlamaTypes.generated.h"

// Forward declare llama.cpp's native param structs. ApplyTo passes them
// by reference so callers don't need llama.h transitively unless they
// want to inspect / further-tweak the struct between ApplyTo and the
// llama_*_create call.
struct llama_model_params;
struct llama_context_params;

// ============================================================================
//  Enums
// ============================================================================

/**
 * KV-cache element type. F16 is the default and matches every shipped
 * GGUF without quality loss. Q8_0 / Q4_0 quantize the cache itself
 * (saves ~50% / ~75% of cache memory) at the cost of some precision in
 * long contexts. Useful when context size is the binding RAM constraint.
 */
UENUM(BlueprintType)
enum class EInoLlamaKvDtype : uint8
{
    F16   UMETA(DisplayName = "F16 (default; no quality loss)"),
    F32   UMETA(DisplayName = "F32 (highest precision)"),
    Q8_0  UMETA(DisplayName = "Q8_0 (8-bit; ~50% memory)"),
    Q4_0  UMETA(DisplayName = "Q4_0 (4-bit; ~75% memory; some quality loss)"),
};

/**
 * Multi-GPU split strategy. None = single device. Layer splits
 * transformer layers across devices (most common; cheap to set up).
 * Row splits the row dimension of large matmuls. Tensor uses tensor
 * parallelism (where the model + backend support it).
 */
UENUM(BlueprintType)
enum class EInoLlamaSplitMode : uint8
{
    None    UMETA(DisplayName = "None (single device)"),
    Layer   UMETA(DisplayName = "Layer (split layers across devices)"),
    Row     UMETA(DisplayName = "Row (split rows across devices)"),
    Tensor  UMETA(DisplayName = "Tensor (tensor parallelism)"),
};

/**
 * Flash Attention selector. Auto lets llama.cpp pick based on the
 * model architecture and active backend. Enabled forces it on (faster
 * on GPU; some kernels may not support it). Disabled forces it off.
 * Default Auto matches llama.cpp's own default.
 */
UENUM(BlueprintType)
enum class EInoLlamaFlashAttnType : uint8
{
    Auto     UMETA(DisplayName = "Auto (let llama.cpp decide)"),
    Disabled UMETA(DisplayName = "Disabled"),
    Enabled  UMETA(DisplayName = "Enabled (faster on GPU)"),
};

// ============================================================================
//  FInoLlamaModelParams — load-time options (passed to llama_model_load_from_file)
// ============================================================================

/**
 * Generic Blueprint-friendly wrapper around llama.cpp's `llama_model_params`.
 * Any GGUF consumer in the codebase can embed this struct in their own
 * load-time config (FInoNeuTtsConfig::BackboneModel today; others later).
 *
 * Defaults match llama.cpp's own defaults — load-time behavior is
 * unchanged from a default-constructed instance.
 */
USTRUCT(BlueprintType)
struct INOLLAMA_API FInoLlamaModelParams
{
    GENERATED_BODY()

    /**
     * Number of transformer layers to offload to the active GPU
     * backend. 0 = CPU only. Setting > 0 requires a GPU-capable
     * llama.cpp build (Vulkan on Win64 + Android per InoLlama's
     * shipping config).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    int32 NumGpuLayers = 0;

    /**
     * Primary GPU device index. Used for the small bookkeeping ops
     * llama.cpp keeps on one device even with multi-GPU split, plus
     * any layer not split out. 0 is fine for most setups.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    int32 MainGpu = 0;

    /**
     * How to distribute layers across multiple GPUs. Default Layer
     * works for typical multi-GPU systems. Ignored when only one GPU
     * (or when NumGpuLayers = 0).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    EInoLlamaSplitMode SplitMode = EInoLlamaSplitMode::Layer;

    /**
     * Skip loading weights — only load vocab and metadata. Useful for
     * tokenization-only tools / lookups; useless for any actual
     * inference.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    bool bVocabOnly = false;

    /**
     * Memory-map the GGUF file instead of reading it into RAM.
     * Default ON — recommended on every desktop / mobile platform we
     * target. Saves the load-time read pass and lets the OS page
     * weights in lazily as the model is touched.
     *
     * Disable only on platforms where mmap is unreliable (none of
     * ours qualify) or if you specifically need the model fully
     * resident before the first inference.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    bool bUseMmap = true;

    /**
     * mlock the model pages in RAM, preventing the OS from swapping
     * them to disk. Eliminates jitter from cold-pages-being-paged-in
     * when the model hasn't been touched in a while, at the cost of
     * permanent RAM pressure equal to model size.
     *
     * Off by default — enable on systems with enough RAM to spare and
     * a hard latency requirement. Requires platform support
     * (Linux / macOS / Windows have it; iOS doesn't).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    bool bUseMlock = false;

    /**
     * Validate every tensor's data integrity at load time. Adds ~5–10%
     * to load duration on a fresh-cache load; used as a one-time
     * "is this GGUF file corrupt?" diagnostic. Off by default.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Model")
    bool bCheckTensors = false;

    /** Translate the struct into a native llama.cpp params struct. */
    void ApplyTo(struct llama_model_params& Native) const;
};

// ============================================================================
//  FInoLlamaContextParams — session options (passed to llama_init_from_model)
// ============================================================================

/**
 * Generic Blueprint-friendly wrapper around `llama_context_params`.
 * Owns every llama.cpp inference-time knob that's worth exposing for a
 * generic GGUF consumer.
 *
 * Defaults are llama.cpp's defaults except where called out — a
 * default-constructed instance is functionally identical to the
 * previous hardcoded path.
 */
USTRUCT(BlueprintType)
struct INOLLAMA_API FInoLlamaContextParams
{
    GENERATED_BODY()

    /**
     * Maximum context size in tokens. Caps the longest prompt + output
     * a session can handle. Keep ≤ the model's training context (the
     * "n_ctx_train" baked into the GGUF) for best quality; going higher
     * forces RoPE extrapolation which degrades long-range coherence.
     * 0 = use the model's training context.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumCtx = 2048;

    /**
     * Logical batch size for prompt processing. Bigger = faster
     * prefill at the cost of more peak memory during the prefill
     * pass. 2048 is the llama.cpp default and the right answer for
     * most workloads.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumBatch = 2048;

    /**
     * Physical batch size — how many tokens are fed through the model
     * in a single decode call internally. 512 is the default. Smaller
     * uses less peak memory but slows prefill. Should be ≤ NumBatch.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumUBatch = 512;

    /**
     * Maximum number of parallel sequences this context will track.
     * 1 for single-stream generation (the common case). Setting > 1
     * costs proportionally more KV-cache memory.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumSeqMax = 1;

    /**
     * Threads for token-by-token decode (the AR generation loop's hot
     * path). 0 = ORT default (one per physical core). For TTS use
     * cases where multiple sessions don't run concurrently this is
     * usually fine; if multiple sessions share the box, capping at
     * 2-4 per session avoids over-subscription.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumThreads = 0;

    /**
     * Threads for batched decode (prompt prefill). 0 = use NumThreads.
     * Larger values amortize fixed costs across more parallel work
     * during the typically-batchy prefill pass; usually equal to or
     * a bit higher than NumThreads.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    int32 NumThreadsBatch = 0;

    /**
     * Flash Attention selector. Auto = llama.cpp picks based on model
     * + backend (the right answer almost always). Set Enabled to force
     * it on for speed (2-4× faster on GPU when supported); Disabled
     * to force off for diagnosing kernel issues.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    EInoLlamaFlashAttnType FlashAttnType = EInoLlamaFlashAttnType::Auto;

    /**
     * When NumGpuLayers > 0, also offload the KV cache to GPU memory.
     * Default ON — keeps the AR loop fully on-device. Disable only if
     * VRAM is tight and you'd rather spend extra PCIe traffic per
     * decode step.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    bool bOffloadKQV = true;

    /**
     * Disable llama.cpp's internal performance counters
     * (`llama_perf_*`). Saves ~1% on the AR hot path. Mostly useful
     * for shipping builds; leave on in dev so timing data is
     * available for diagnosis.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    bool bDisablePerf = false;

    /**
     * KV-cache K-tensor element type. F16 is the default and the
     * highest-quality choice. Q8_0 / Q4_0 quantize the cache to save
     * memory at the cost of some precision in long contexts (set both
     * KvDtypeK and KvDtypeV to the same value).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    EInoLlamaKvDtype KvDtypeK = EInoLlamaKvDtype::F16;

    /** KV-cache V-tensor element type. See KvDtypeK. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoLlama|Context")
    EInoLlamaKvDtype KvDtypeV = EInoLlamaKvDtype::F16;

    /** Translate the struct into a native llama.cpp params struct. */
    void ApplyTo(struct llama_context_params& Native) const;
};
