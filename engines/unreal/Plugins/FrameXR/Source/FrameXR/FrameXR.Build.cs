// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame. Adapted from tether-unreal (MIT).

using System;
using System.IO;
using UnrealBuildTool;

public class FrameXR : ModuleRules
{
	public FrameXR(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// OpenXR headers: FRAMEXR_OPENXR_INCLUDE if set, else the copy vendored under the plugin
		// (ThirdParty/OpenXR/include, OpenXR SDK 1.1.57). The engine's own ThirdParty/OpenXR headers (1.1.46 in
		// UE 5.8) lack XR_KHR_metal_enable. Nothing from the building machine's home directory is baked in; the
		// loader dylib is found at run time (FFrameXRSession::FindLoader: FRAMEXR_OPENXR_LOADER, then the plugin's
		// ThirdParty/OpenXR/lib/libopenxr_loader.dylib).
		string Include = Environment.GetEnvironmentVariable("FRAMEXR_OPENXR_INCLUDE");
		if (string.IsNullOrEmpty(Include))
		{
			Include = Path.Combine(PluginDirectory, "ThirdParty", "OpenXR", "include");
		}
		PrivateIncludePaths.Add(Include);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ApplicationCore",
			"InputCore",
			"InputDevice",
			"HeadMountedDisplay",
			"XRBase",
			"Projects", // IPluginManager: the plugin-relative loader path
			"RHI",
			"RenderCore",
			"Renderer",
			"Slate",
			"SlateCore",
		});

		// UE 5.8's Metal RHI hands out its command queue through IMetalDynamicRHI (metal-cpp).
		PrivateDependencyModuleNames.Add("MetalRHI");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "MetalCPP");
	}
}
