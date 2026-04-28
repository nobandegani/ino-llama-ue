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
		// which downloads upstream's prebuilt release artifacts and stages
		// them into the consolidated tree:
		//
		//     Source/ThirdParty/Public/                       llama.cpp C API headers
		//     Source/ThirdParty/Win64/                        19 DLLs (llama + ggml +
		//                                                     14 CPU variants + Vulkan +
		//                                                     OpenMP redist)
		//     Source/ThirdParty/Android/arm64-v8a/            10 .so files (libllama +
		//                                                     libggml + 7 ARM tier
		//                                                     CPU variants)
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
		//   ships llama.cpp, so no concrete collision today. The directory
		//   isolation (Source/ThirdParty/Win64/ as a unique staging path)
		//   is sufficient.
		//
		// Vulkan backend note:
		//   ggml-vulkan.dll statically imports vulkan-1.dll. vulkan-1.dll is
		//   part of the Vulkan runtime loader bundled with Windows 10 1803+
		//   and every modern GPU driver (NVIDIA, AMD, Intel). Not redistributed
		//   by us — Windows provides it. Same story on Android: libvulkan.so
		//   is part of the platform from API 24+. We target API 26+.

		string ThirdPartyDir  = Path.Combine(PluginDirectory, "Source", "ThirdParty");
		string PublicDir      = Path.Combine(ThirdPartyDir, "Public");
		string Win64Dir       = Path.Combine(ThirdPartyDir, "Win64");
		string AndroidBaseDir = Path.Combine(ThirdPartyDir, "Android");

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
			// other 13 remain on disk. Enumerate the staged dir so we
			// don't have to hardcode all 14 tier names — the exact set may
			// shift between upstream releases.
			//
			// Staging type MUST be StagedFileType.SystemNonUFS here (not
			// the default NonUFS). Reason: the default causes Live Coding
			// to scan every .dll in RuntimeDependencies as a potential
			// hot-patch target, producing 13 "Cannot enable module X
			// because it is not loaded by this process" error lines on
			// every Live Coding compile — one per unused variant. Marking
			// them SystemNonUFS tells UBT they're system-ish files that
			// LiveCoding should skip, while still getting them staged
			// into packaged builds.
			if (Directory.Exists(Win64Dir))
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
				"libllama.so",     // main library
				"libggml.so",      // dispatcher
				"libggml-base.so", // base implementation
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
		// iOS / Linux / macOS: not yet implemented. Linking succeeds because
		// no static references; runtime calls fail gracefully when the
		// dynamic load can't find the library.
	}
}
