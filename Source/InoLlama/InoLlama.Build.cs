// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

public class InoLlama : ModuleRules
{
	public InoLlama(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",

				// IPluginManager — used in StartupModule to resolve our
				// plugin's install path so we can load DLLs by full absolute path.
				"Projects",
			}
			);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);

		// =====================================================================
		// llama.cpp third-party integration
		// =====================================================================
		// This is the only UE module in the plugin, so it owns the third-party
		// wiring directly (no separate external module). Build artifacts are
		// produced by Plugins/InoLlama/LlamaCpp/scripts/setup-llamacpp.ps1
		// which uses a hybrid strategy: prebuilt download for Win64 + Mac + iOS,
		// from-source build (vendored submodule) for Android. Outputs are
		// staged into the consolidated tree:
		//
		//     Source/ThirdParty/Public/                       llama.cpp C API headers
		//     Source/ThirdParty/Win64/                        19 DLLs (llama + ggml +
		//                                                     14 CPU variants + Vulkan +
		//                                                     OpenMP redist)
		//     Source/ThirdParty/Android/arm64-v8a/            11 .so files (libllama +
		//                                                     libggml + 7 ARM tier
		//                                                     CPU variants + Vulkan)
		//     Source/ThirdParty/Mac/llama.framework/          1 fat dylib (arm64+x86_64)
		//                                                     with CPU + Metal backends
		//                                                     statically linked + Metal
		//                                                     shaders embedded
		//     Source/ThirdParty/IOS/llama.framework/          1 dylib (arm64 device)
		//     Source/ThirdParty/IOS/Simulator/llama.framework/ 1 fat dylib
		//                                                     (arm64 + x86_64 simulator)
		//
		// Win64 staging comes from upstream's `llama-<tag>-bin-win-vulkan-x64.zip`.
		// Mac + iOS staging comes from upstream's `llama-<tag>-xcframework.zip`
		// (one zip, three slices: macos-arm64_x86_64, ios-arm64,
		// ios-arm64_x86_64-simulator). Each Apple slice's framework binary is a
		// single dylib containing llama + ggml + ggml-cpu + ggml-metal + ggml-blas
		// statically linked together; Metal shaders are embedded
		// (-DGGML_METAL_EMBED_LIBRARY=ON, no separate default.metallib).
		// Backends self-register at dylib load via static-init constructors —
		// no ggml_backend_load_all_from_path call needed on Apple platforms.
		// Android staging is built from the LlamaCpp/vendor/llama.cpp/ submodule
		// at the same pinned tag, with -DGGML_VULKAN=ON, because upstream's
		// `llama-<tag>-bin-android-arm64.tar.gz` release is CPU-only and they
		// do not publish Android Vulkan prebuilts. See LlamaCpp/scripts/
		// setup-llamacpp.ps1 for the build flags.
		//
		// Companion file in this same module directory:
		//     InoLlama_UPL_Android.xml             Android packaging directives
		//
		// Consumers #include "llama.h" for the type definitions (llama_model,
		// llama_context, llama_batch, etc.) but do NOT call the exported
		// functions directly. All llama_* + ggml_backend_* calls go through
		// a function-pointer vtable resolved at module startup
		// (InoAgents::LlamaCpp::GetApi()).
		//
		// Because the headers define LLAMA_API / GGML_API as empty when
		// neither LLAMA_SHARED nor LLAMA_BUILD is defined, any direct call
		// to a llama_* or ggml_* function from our code will produce an
		// unresolved-external link error — which is the intended guardrail.
		// There is no implicit linking against llama.lib; upstream's
		// Windows release ZIP doesn't even ship one.
		//
		// Why no rename (unlike InoOnnx's onnxruntime.dll → InoOnnxRuntime.dll):
		//   llama.cpp ships as a graph of ~20 interdependent DLLs. llama.dll
		//   imports ggml.dll which imports ggml-base.dll, and libggml loads
		//   ggml-cpu-*.dll / ggml-vulkan.dll at runtime by glob-scanning for
		//   ggml-*.dll. Renaming any would require PE-patching every static
		//   import AND replacing the glob scanner with explicit
		//   ggml_backend_load(full_path) calls. No UE 5.7 plugin currently
		//   ships llama.cpp, so no concrete collision today. Directory
		//   isolation per platform (Source/ThirdParty/Win64/ +
		//   Source/ThirdParty/Android/arm64-v8a/ as unique staging paths)
		//   is sufficient.
		//
		// Vulkan backend note:
		//   ggml-vulkan.dll statically imports vulkan-1.dll. vulkan-1.dll is
		//   part of the Vulkan runtime loader bundled with Windows 10 1803+
		//   and every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed
		//   by us — Windows provides it. Same story on Android: libvulkan.so
		//   is part of the platform from API 24+. We target API 30+
		//   (matches the hosting game's minSdk). ggml-vulkan also
		//   needs Vulkan 1.1 symbols like vkGetPhysicalDeviceFeatures2,
		//   exposed by the NDK libvulkan.so stub from API 28 onward.

		string ThirdPartyDir   = Path.Combine(PluginDirectory, "Source", "ThirdParty");
		string PublicDir       = Path.Combine(ThirdPartyDir, "Public");
		string Win64Dir        = Path.Combine(ThirdPartyDir, "Win64");
		string AndroidBaseDir  = Path.Combine(ThirdPartyDir, "Android");
		string MacFrameworkDir = Path.Combine(ThirdPartyDir, "Mac", "llama.framework");
		string IosBaseDir      = Path.Combine(ThirdPartyDir, "IOS");

		// Public headers — consumers do
		//     #include "llama.h"
		//     #include "ggml.h"
		PublicSystemIncludePaths.Add(PublicDir);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows: dynamic loading only. NO PublicAdditionalLibraries,
			// NO PublicDelayLoadDLLs. Runtime consumers resolve every
			// llama_* and ggml_backend_* entry via GetProcAddress on
			// llama.dll, loaded at full path by the consumer's StartupModule.
			//
			// Main llama.dll's PE imports on ggml.dll cascade automatically
			// via Windows' default DLL search path (same directory as the
			// loaded module). CPU backend variants + ggml-vulkan.dll are
			// picked up at runtime by ggml_backend_load_all's glob scan of
			// the directory.
			//
			// RuntimeDependencies here ensures UE's packager stages every
			// file alongside the shipped executable.

			// Required files — always present on a correctly-staged tree.
			string[] RequiredWin64 = new string[]
			{
				"llama.dll",            // main library
				"ggml.dll",             // dispatcher
				"ggml-base.dll",        // base implementation
				"ggml-vulkan.dll",      // Vulkan backend
				"libomp140.x86_64.dll", // MSVC OpenMP redist (used by ggml-cpu-*)
			};
			foreach (string name in RequiredWin64)
			{
				RuntimeDependencies.Add(Path.Combine(Win64Dir, name));
			}

			// CPU microarchitecture variants. llama.cpp's runtime backend
			// picker selects the optimal one per-CPU at model-load time —
			// only ONE variant actually gets loaded into the process; the
			// other 13 dlopen, fail their CPU-feature probe, and unload.
			// Enumerate the staged dir so we don't have to hardcode all 14
			// tier names — the exact set may shift between upstream releases.
			//
			// Editor target is intentionally excluded. Reason: UE 5.7's
			// Live Coding scans every entry in the .target file's
			// RuntimeDependencies and tries to "enable" each as a UE module,
			// producing 13 "Cannot enable module X because it is not loaded
			// by this process" errors at editor startup — one per variant
			// the runtime probe rejected. The legacy StagedFileType.SystemNonUFS
			// hint that older UE versions used to skip Live Coding scans is
			// no longer respected in 5.7, so we just keep the variants out
			// of the editor's RuntimeDependencies entirely.
			//
			// Editor still loads them at runtime via ggml_backend_load_all_from_path's
			// directory scan of Source/ThirdParty/Win64/ (the files are
			// physically present regardless of the RuntimeDependencies
			// declaration). Game/Server targets DO list them in
			// RuntimeDependencies, so the cook + stage step for shipping
			// builds picks them up correctly. Type is still SystemNonUFS
			// for cooked builds for parity with packaging conventions.
			if (Target.Type != TargetType.Editor && Directory.Exists(Win64Dir))
			{
				string[] cpuVariants = Directory.GetFiles(Win64Dir, "ggml-cpu-*.dll");
				foreach (string path in cpuVariants)
				{
					RuntimeDependencies.Add(path, StagedFileType.SystemNonUFS);
				}
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Android arm64-v8a artifacts staged by setup-llamacpp.ps1
			// under Source/ThirdParty/Android/arm64-v8a/.
			//
			// Same as the InoOnnx integration: we do NOT use
			// PublicAdditionalLibraries on Android. Implicitly linking
			// against libllama.so would add a DT_NEEDED entry to
			// libUnreal.so, and if a future Marketplace plugin also
			// ships its own libllama.so at a different ABI the dynamic
			// linker could fail to satisfy versioned symbol references.
			// Keeping the load explicit (dlopen via FPlatformProcess::
			// GetDllHandle) keeps us ABI-isolated.
			string Arm64Dir = Path.Combine(AndroidBaseDir, "arm64-v8a");

			// Required files — every shipment.
			string[] RequiredAndroid = new string[]
			{
				"libllama.so",        // main library
				"libggml.so",         // dispatcher
				"libggml-base.so",    // base implementation
				"libggml-vulkan.so",  // Vulkan GPU backend (built from source)
			};
			foreach (string name in RequiredAndroid)
			{
				string soPath = Path.Combine(Arm64Dir, name);
				if (File.Exists(soPath))
				{
					RuntimeDependencies.Add(soPath);
				}
			}

			// CPU variants (ARM tier — armv8.0/8.2/8.6/9.0/9.2). Only one
			// gets loaded at runtime; mark the rest as SystemNonUFS for
			// parity with the Win64 branch.
			if (Directory.Exists(Arm64Dir))
			{
				string[] cpuVariants = Directory.GetFiles(Arm64Dir, "libggml-cpu-*.so");
				foreach (string path in cpuVariants)
				{
					RuntimeDependencies.Add(path, StagedFileType.SystemNonUFS);
				}
			}

			// Apply the UPL (Unreal Plugin Language) XML that tells UE's
			// APK packager to copy each .so into lib/arm64-v8a/ and emit
			// a System.loadLibrary("llama") call so libllama.so is resident
			// by the time the consumer's StartupModule runs.
			AdditionalPropertiesForReceipt.Add(
				"AndroidPlugin",
				Path.Combine(ModuleDirectory, "InoLlama_UPL_Android.xml"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// Mac: dynamic loading only — same isolation rationale as Win64
			// and Android. Even though no UE plugin currently ships its own
			// llama.framework, keeping the load explicit (dlopen by full
			// path) means we can never accidentally dyld-bind to a future
			// Marketplace plugin's copy at app launch.
			//
			// The framework's binary is the ONLY file we have to ship at
			// runtime: backends (CPU + Metal) are statically linked into
			// it and self-register via static-init constructors when the
			// dylib is mapped, so there is no ggml-cpu-*.so / Vulkan side-
			// car analog to enumerate. RuntimeDependencies.Add stages the
			// binary into the packaged .app where IPluginManager-resolved
			// paths can find it again at runtime.
			//
			// The .framework directory layout we stage is FLATTENED (binary
			// + Headers/ at the framework root, no Versions/A/ subdir) —
			// see setup-llamacpp.ps1 for why (Windows can't extract Apple
			// versioned-framework symlinks reliably). Mac dyld accepts
			// both flat and versioned layouts for dylib resolution, so the
			// flattening is invisible at runtime.
			//
			// Universal binary: arm64 half is built with Metal (matches the
			// Apple-Silicon-Macs-only GPU offload story); x86_64 half is
			// CPU-only because upstream's CI Intel-Mac runner has no GPU
			// to compile Metal against. No fallback handling needed in our
			// code — the embedded Metal backend simply won't register on
			// Intel Macs, and the runtime backend picker drops back to CPU.
			string MacBinary = Path.Combine(MacFrameworkDir, "llama");
			if (File.Exists(MacBinary))
			{
				RuntimeDependencies.Add(MacBinary);

				// Stage the Info.plist + headers if they're present (cheap
				// + harmless; lets debuggers and crash reporters pick up
				// the framework's identity).
				string InfoPlist = Path.Combine(MacFrameworkDir, "Resources", "Info.plist");
				if (File.Exists(InfoPlist))
				{
					RuntimeDependencies.Add(InfoPlist);
				}
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			// iOS: PublicAdditionalFrameworks does double duty — adds
			// `-framework llama` to the link command AND embeds
			// llama.framework into the .app's Frameworks/ directory at
			// packaging time. dyld auto-loads the framework at app launch
			// before any UE module runs, so by the time our StartupModule
			// fires every llama_* / ggml_* symbol is already in the
			// process's global namespace.
			//
			// Why this differs from Win64/Android/Mac dynamic-load pattern:
			//   iOS has no reliable equivalent of dlopen-by-full-path that
			//   works across all supported iOS versions + signing modes
			//   (App Store, ad-hoc, dev). Embedded frameworks must be
			//   declared at build time so the code-signing pass picks them
			//   up. Our InoLlama.cpp's iOS Init still populates the vtable
			//   via dlsym(RTLD_DEFAULT, ...) so the consumer-facing API
			//   stays uniform across platforms; only the load mechanism
			//   differs.
			//
			// Currently we only ship the device slice (arm64). The
			// simulator slice exists at Source/ThirdParty/IOS/Simulator/
			// for development convenience but is not wired into the build
			// — UE 5.7's iOS toolchain targets device builds in shipped
			// game flow, and devs running in simulator typically use a
			// dedicated simulator-targeted build configuration where the
			// PublicAdditionalFrameworks Path can be flipped. (Future
			// follow-up: detect Target.Architecture == sim and switch
			// the framework path; not blocking initial iOS support.)
			string IosDeviceFw = Path.Combine(IosBaseDir, "llama.framework");
			if (Directory.Exists(IosDeviceFw))
			{
				PublicAdditionalFrameworks.Add(new Framework(
					"llama",
					IosDeviceFw,
					/*CopyBundledAssets*/ null,
					/*bCopyFramework*/ true));
			}
		}
		// Linux: not yet implemented. Linking succeeds because no static
		// references; runtime calls fail gracefully when the dynamic load
		// can't find the library.
	}
}
