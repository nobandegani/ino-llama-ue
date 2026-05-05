// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

// Pull in the full llama.cpp C API headers. The vtable below uses
// function-pointer types that embed struct-by-value parameters and
// return types (llama_model_default_params, llama_batch, etc.), so
// forward declarations aren't sufficient. Consumers of GetApi() will
// also need these types (llama_model, llama_context, llama_vocab,
// llama_batch, llama_sampler, etc.) so having them here means they
// get them transitively.
#include "llama.h"
#include "ggml-backend.h"

// Generic Blueprint-friendly param structs used by the helper functions
// below. Pulled in here so consumers only need this single header.
#include "InoLlamaTypes.h"

/**
 * Shared log category for the InoLlama plugin and the llama.cpp loader
 * code that ships with it. Consumer plugins (InoAgents, etc.) have
 * their own log categories and continue to use them; only the runtime
 * DLL load / unload + vtable resolution logs to LogInoLlama.
 */
INOLLAMA_API DECLARE_LOG_CATEGORY_EXTERN(LogInoLlama, Log, All);

/**
 * InoLlama plugin runtime module.
 *
 * Loads at LoadingPhase=PreLoadingScreen so the llama.cpp DLL/.so chain
 * is mapped and the function-pointer vtable is resolved before any
 * consumer plugin's Default-phase StartupModule runs. Consumers
 * (InoAgents today, future ones tomorrow) declare InoLlama in their
 * .uplugin's Plugins array and "InoLlama" in their Build.cs
 * PublicDependencyModuleNames, then `#include "InoLlama.h"` and call
 * `InoAgents::LlamaCpp::GetApi()` to reach the API vtable.
 */
class FInoLlamaModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};

/**
 * llama.cpp DLL loader + global API vtable accessor.
 *
 * The namespace is `InoAgents::LlamaCpp::` for historical reasons (this
 * code was extracted from the InoAgents plugin). Consumer code that
 * already spells `InoAgents::LlamaCpp::GetApi()` continues to work
 * unchanged.
 *
 * Init() / Shutdown() are called by FInoLlamaModule (above). Consumer
 * plugins should NOT call them directly — the lifecycle is owned by the
 * InoLlama plugin's PreLoadingScreen module. Consumers only need
 * `GetApi()` to reach the vtable.
 *
 * Unlike ORT — which exposes a single vendor-supplied vtable (OrtApi)
 * reached via one exported entry point (OrtGetApiBase) — llama.cpp
 * exports every function individually with no central dispatcher. We
 * synthesise our own vtable (FLlamaCppApi) by resolving each needed
 * function pointer at module startup. Consumers call Api-> methods,
 * gated on a null check against GetApi().
 *
 * Init() on Windows:
 *   - PreloadWin64Deps(): loads libomp140.x86_64.dll, ggml-base.dll,
 *     ggml.dll, ggml-vulkan.dll by full path so Windows' loaded-modules
 *     cache is seeded with OUR copies before llama.dll's PE imports
 *     are resolved. Load order is intentional (dependencies first).
 *   - FPlatformProcess::GetDllHandle on llama.dll (full path).
 *   - FPlatformProcess::GetDllExport to resolve every function pointer
 *     in FLlamaCppApi. ggml_* symbols are exported from ggml.dll /
 *     ggml-base.dll, so we search across all the loaded llama.cpp
 *     handles for each name — first hit wins.
 *   - ggml_backend_load_all_from_path(bin_dir): scans our staging
 *     directory for ggml-*.dll and registers each as a backend. This
 *     is how the 14 CPU variants + ggml-vulkan get picked up — the
 *     runtime discards variants the host CPU doesn't support.
 *   - llama_backend_init(): initialises the llama.cpp runtime globals.
 *
 * Init() on Android:
 *   - No PreloadWin64Deps. UPL's <soLoadLibrary> has already preloaded
 *     libllama.so via System.loadLibrary, and libllama.so's DT_NEEDED
 *     chain cascaded libggml.so and libggml-base.so. The CPU variants
 *     (libggml-cpu-android_*.so) are staged in the APK's lib/arm64-v8a/
 *     but not preloaded — ggml_backend_load_all_from_path dlopens them
 *     on demand once we've resolved its function pointer.
 *
 * Init() on iOS / Linux / macOS:
 *   - Warns. GetApi() returns nullptr. Every consumer null-checks the
 *     return and handles gracefully.
 */
namespace InoAgents::LlamaCpp
{
    /**
     * Function-pointer vtable for the subset of llama.cpp's C API that
     * consumers actually call. Populated by Init() via
     * FPlatformProcess::GetDllExport against the loaded llama.cpp
     * handles; cleared to zeros by Shutdown().
     *
     * Lifetime: the FLlamaCppApi instance lives in a file-static in
     * InoLlama.cpp. The pointer returned by GetApi() is valid from a
     * successful Init() until Shutdown(). Multiple threads may read
     * freely (the pointer is set once at module startup and cleared
     * once at module shutdown; there are no writes in between).
     */
    struct FLlamaCppApi
    {
        // ==================================================================
        // Module startup + backend discovery (always present)
        // ==================================================================

        // Runtime lifecycle. Called once each by Init() / Shutdown() on
        // the game thread during module bring-up/tear-down.
        void        (*llama_backend_init)(void) = nullptr;
        void        (*llama_backend_free)(void) = nullptr;

        // Build / system information. llama_print_system_info returns a
        // pointer to a static internal buffer owned by llama.cpp — valid
        // until the next call, so callers should copy if they want to
        // retain across calls.
        const char* (*llama_print_system_info)(void) = nullptr;

        // Backend loader: scans a directory for ggml-*.{dll,so} and
        // registers each as a backend. Exported from ggml-base.dll /
        // libggml-base.so. Called once by Init() after the module
        // handles are loaded to register the 14 Windows CPU variants +
        // ggml-vulkan (or the 7 Android ARM variants).
        void        (*ggml_backend_load_all_from_path)(const char* dir_path) = nullptr;

        // Backend enumeration — used by smoke tests to log which
        // backends actually registered successfully on the host CPU / GPU.
        size_t                   (*ggml_backend_reg_count)(void) = nullptr;
        struct ggml_backend_reg* (*ggml_backend_reg_get)(size_t index) = nullptr;
        const char*              (*ggml_backend_reg_name)(struct ggml_backend_reg* reg) = nullptr;

        // ==================================================================
        // Model, context, vocab, tokenize, decode, sampler.
        // Generally useful for any GGUF consumer (NeuTTS Nano today, future
        // GGUF models tomorrow). All signatures are 1:1 with llama.h at the
        // pinned tag.
        // ==================================================================

        // --- Model lifecycle ---
        struct llama_model_params (*llama_model_default_params)(void) = nullptr;
        struct llama_model*       (*llama_model_load_from_file)(
                                      const char* path_model,
                                      struct llama_model_params params) = nullptr;
        void                      (*llama_model_free)(struct llama_model* model) = nullptr;
        const struct llama_vocab* (*llama_model_get_vocab)(const struct llama_model* model) = nullptr;
        int32_t                   (*llama_model_desc)(
                                      const struct llama_model* model,
                                      char* buf, size_t buf_size) = nullptr;
        int32_t                   (*llama_model_n_ctx_train)(const struct llama_model* model) = nullptr;

        // --- Context lifecycle ---
        struct llama_context_params (*llama_context_default_params)(void) = nullptr;
        struct llama_context*       (*llama_init_from_model)(
                                        struct llama_model* model,
                                        struct llama_context_params params) = nullptr;
        void                        (*llama_free)(struct llama_context* ctx) = nullptr;
        uint32_t                    (*llama_n_ctx)(const struct llama_context* ctx) = nullptr;

        // --- Memory / KV-cache reset (separate memory handle in modern llama.cpp) ---
        llama_memory_t (*llama_get_memory)(const struct llama_context* ctx) = nullptr;
        void           (*llama_memory_clear)(llama_memory_t mem, bool data) = nullptr;

        // Per-sequence KV trimming. Removes tokens [p0, p1) from seq_id
        // (with -1 meaning "any sequence", p0<0 meaning "from 0", p1<0
        // meaning "to infinity"). Used by KV-snapshot consumers to clear
        // post-snapshot tokens before restoring state across iterations.
        // Returns false if a partial sequence cannot be removed.
        bool           (*llama_memory_seq_rm)(
                           llama_memory_t mem,
                           llama_seq_id   seq_id,
                           llama_pos      p0,
                           llama_pos      p1) = nullptr;

        // --- State / sequence cache (snapshot + restore for KV reuse) ---
        // Used by NeuTTS voice caching: the fixed voice prefix is
        // prefilled once into seq 0, snapshotted via _get_data, and
        // restored via _set_data at the start of every synth so the
        // ~50–100 prefix tokens don't re-prefill per call. Equivalent
        // pattern works for any "fixed long prefix + variable suffix"
        // GGUF consumer.

        // Returns the exact byte size needed to copy the state of a
        // single sequence (call before allocating the buffer; the size
        // is generally O(KV_size_per_token * n_tokens_in_seq)).
        size_t (*llama_state_seq_get_size)(
                   struct llama_context* ctx,
                   llama_seq_id          seq_id) = nullptr;

        // Copy a sequence's state into the caller-provided buffer.
        // Returns the number of bytes written (0 on failure).
        size_t (*llama_state_seq_get_data)(
                   struct llama_context* ctx,
                   uint8_t*              dst,
                   size_t                size,
                   llama_seq_id          seq_id) = nullptr;

        // Load sequence state previously saved with _get_data into the
        // specified destination sequence. Returns the number of bytes
        // read (0 on failure).
        size_t (*llama_state_seq_set_data)(
                   struct llama_context* ctx,
                   const uint8_t*        src,
                   size_t                size,
                   llama_seq_id          dest_seq_id) = nullptr;

        // --- Vocab queries ---
        int32_t      (*llama_vocab_n_tokens)(const struct llama_vocab* vocab) = nullptr;
        llama_token  (*llama_vocab_eos)(const struct llama_vocab* vocab) = nullptr;
        bool         (*llama_vocab_is_eog)(const struct llama_vocab* vocab, llama_token token) = nullptr;
        bool         (*llama_vocab_get_add_bos)(const struct llama_vocab* vocab) = nullptr;
        int32_t      (*llama_token_to_piece)(
                         const struct llama_vocab* vocab,
                         llama_token token,
                         char* buf, int32_t length,
                         int32_t lstrip, bool special) = nullptr;

        // --- Tokenize / detokenize ---
        int32_t (*llama_tokenize)(
                    const struct llama_vocab* vocab,
                    const char* text, int32_t text_len,
                    llama_token* tokens, int32_t n_tokens_max,
                    bool add_special, bool parse_special) = nullptr;
        int32_t (*llama_detokenize)(
                    const struct llama_vocab* vocab,
                    const llama_token* tokens, int32_t n_tokens,
                    char* text, int32_t text_len_max,
                    bool remove_special, bool unparse_special) = nullptr;

        // --- Batch + decode ---
        struct llama_batch (*llama_batch_init)(
                               int32_t n_tokens, int32_t embd, int32_t n_seq_max) = nullptr;
        void               (*llama_batch_free)(struct llama_batch batch) = nullptr;
        struct llama_batch (*llama_batch_get_one)(
                               llama_token* tokens, int32_t n_tokens) = nullptr;
        int32_t            (*llama_decode)(
                               struct llama_context* ctx,
                               struct llama_batch batch) = nullptr;
        float*             (*llama_get_logits_ith)(
                               struct llama_context* ctx, int32_t i) = nullptr;

        // --- Sampler chain (takes ownership of added samplers — do NOT call
        //     llama_sampler_free on members after llama_sampler_chain_add) ---
        struct llama_sampler_chain_params (*llama_sampler_chain_default_params)(void) = nullptr;
        struct llama_sampler*             (*llama_sampler_chain_init)(
                                              struct llama_sampler_chain_params params) = nullptr;
        void                              (*llama_sampler_chain_add)(
                                              struct llama_sampler* chain,
                                              struct llama_sampler* smpl) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_greedy)(void) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_dist)(uint32_t seed) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_top_k)(int32_t k) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_top_p)(float p, size_t min_keep) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_min_p)(float p, size_t min_keep) = nullptr;
        struct llama_sampler*             (*llama_sampler_init_temp)(float t) = nullptr;
        llama_token                       (*llama_sampler_sample)(
                                              struct llama_sampler* smpl,
                                              struct llama_context* ctx,
                                              int32_t idx) = nullptr;
        void                              (*llama_sampler_accept)(
                                              struct llama_sampler* smpl,
                                              llama_token token) = nullptr;
        void                              (*llama_sampler_free)(struct llama_sampler* smpl) = nullptr;
    };

    /**
     * Load the llama.cpp runtime, resolve the API vtable, register
     * backends, and initialise llama's globals. Logs a summary line on
     * success; Errors on any failure.
     *
     * Returns true on full success, false if any required step failed.
     * Even on false the caller MUST still call Shutdown() to release
     * any partial state (open DLL handles, etc.) — Shutdown is
     * idempotent and handles the partial-init case.
     */
    INOLLAMA_API bool Init();

    /**
     * Inverse of Init. Calls llama_backend_free (if the pointer was
     * resolved), releases every DLL handle we opened, and clears the
     * cached API pointer. Safe to call whether or not Init() ran or
     * succeeded.
     */
    INOLLAMA_API void Shutdown();

    /**
     * Return the cached FLlamaCppApi vtable pointer, or nullptr if
     * Init() did not succeed (platform not supported, DLL load failed,
     * function resolution failed, etc.).
     *
     * Lifetime: valid from a successful Init() until Shutdown(). Threads
     * may read freely; the pointer itself is set once at module startup
     * and cleared once at module shutdown, with no writes in between.
     *
     * Callers MUST null-check before dereferencing.
     */
    INOLLAMA_API const FLlamaCppApi* GetApi();

    // ========================================================================
    //  High-level helpers — generic for any GGUF consumer
    // ========================================================================

    /**
     * Load a GGUF model with the given params. Wraps the
     * llama_model_default_params + ApplyTo + llama_model_load_from_file
     * boilerplate that every consumer would otherwise repeat, and adds
     * file-existence check, error logging, and load-timing diagnostics.
     *
     * Returns nullptr on any failure (with diagnostic in *OutError when
     * OutError is non-null, plus a Log/Error line either way).
     *
     * Callers own the returned llama_model* — release with
     * GetApi()->llama_model_free(Model) when done.
     */
    INOLLAMA_API struct llama_model* LoadModelFromFile(
        const FString& ModelPath,
        const FInoLlamaModelParams& Params,
        FString* OutError = nullptr);

    /**
     * Create an inference context for a previously loaded model. Wraps
     * llama_context_default_params + ApplyTo + llama_init_from_model.
     *
     * Returns nullptr on any failure.
     *
     * Callers own the returned llama_context* — release with
     * GetApi()->llama_free(Context) when done.
     */
    INOLLAMA_API struct llama_context* CreateContext(
        struct llama_model* Model,
        const FInoLlamaContextParams& Params,
        FString* OutError = nullptr);
}
