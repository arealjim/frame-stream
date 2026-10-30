// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#include "FrameXRSession.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "IMetalDynamicRHI.h"
#include "Interfaces/IPluginManager.h"

#include <cstring>

#define XR_CHECK(Call) Check((Call), TEXT(#Call))

namespace
{
	// MTLPixelFormat values (Metal/MTLPixelFormat.h): the swapchain prefers the sRGB-encoded 8-bit BGRA the
	// engine's tonemapped eye target already holds, so the blit copies bytes and the runtime reads them as sRGB.
	constexpr int64 MetalBGRA8UnormSRGB = 81;
	constexpr int64 MetalBGRA8Unorm = 80;

	void CopyName(char* Dst, size_t Size, const char* Src)
	{
		std::strncpy(Dst, Src, Size - 1);
		Dst[Size - 1] = '\0';
	}
}

FFrameXRSession& FFrameXRSession::Get()
{
	static FFrameXRSession Instance;
	return Instance;
}

bool FFrameXRSession::Check(XrResult Result, const TCHAR* Call) const
{
	if (XR_SUCCEEDED(Result))
	{
		return true;
	}
	char Name[XR_MAX_RESULT_STRING_SIZE] = "?";
	if (xrResultToString && Instance != XR_NULL_HANDLE)
	{
		xrResultToString(Instance, Result, Name);
	}
	UE_LOG(LogFrameXR, Warning, TEXT("%s failed: %s (%d)"), Call, ANSI_TO_TCHAR(Name), (int32)Result);
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Loader

FString FFrameXRSession::FindLoader()
{
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEXR_OPENXR_LOADER"));
	if (!Env.IsEmpty())
	{
		if (FPaths::FileExists(Env))
		{
			return Env;
		}
		UE_LOG(LogFrameXR, Warning, TEXT("FRAMEXR_OPENXR_LOADER=%s does not exist"), *Env);
	}
	// The plugin's own ThirdParty/OpenXR/lib (gitignored; the recipe copies the OXRSys build's loader there). No path
	// from the building machine is baked into the binary.
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FrameXR")))
	{
		const FString Bundled = FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdParty"), TEXT("OpenXR"), TEXT("lib"), TEXT("libopenxr_loader.dylib"));
		if (FPaths::FileExists(Bundled))
		{
			return Bundled;
		}
	}
	return FString();
}

bool FFrameXRSession::LoadLoader()
{
	if (bLoaderAttempted)
	{
		return IsLoaderLoaded();
	}
	bLoaderAttempted = true;

	LoaderPath = FindLoader();
	if (LoaderPath.IsEmpty())
	{
		UE_LOG(LogFrameXR, Log, TEXT("No OpenXR loader found (FRAMEXR_OPENXR_LOADER unset, none under the plugin's ThirdParty/OpenXR/lib); FrameXR stays dormant"));
		return false;
	}
	LoaderHandle = FPlatformProcess::GetDllHandle(*LoaderPath);
	if (!LoaderHandle)
	{
		UE_LOG(LogFrameXR, Warning, TEXT("Couldn't load %s; FrameXR stays dormant"), *LoaderPath);
		return false;
	}
	xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(FPlatformProcess::GetDllExport(LoaderHandle, TEXT("xrGetInstanceProcAddr")));
	if (!xrGetInstanceProcAddr)
	{
		UE_LOG(LogFrameXR, Warning, TEXT("%s exports no xrGetInstanceProcAddr; FrameXR stays dormant"), *LoaderPath);
		return false;
	}
#define FRAMEXR_RESOLVE_PRE(Name) \
	if (!XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, #Name, reinterpret_cast<PFN_xrVoidFunction*>(&Name)))) { return false; }
	FRAMEXR_PRE_INSTANCE_FUNCTIONS(FRAMEXR_RESOLVE_PRE)
#undef FRAMEXR_RESOLVE_PRE

	const FString RuntimeJson = FPlatformMisc::GetEnvironmentVariable(TEXT("XR_RUNTIME_JSON"));
	UE_LOG(LogFrameXR, Log, TEXT("Loaded the OpenXR loader from %s (XR_RUNTIME_JSON=%s)"), *LoaderPath, RuntimeJson.IsEmpty() ? TEXT("unset: the loader's default runtime") : *RuntimeJson);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Instance + system

// Newer than the OpenXR headers the engine ships, so named here (version 1 adds only an interaction profile).
static constexpr const char* FrameControllerExtensionName = "XR_VALVE_frame_controller_interaction";
static constexpr const char* FrameControllerProfile = "/interaction_profiles/valve/frame_controller_valve";

bool FFrameXRSession::CreateInstance()
{
	if (HasInstance())
	{
		return true;
	}
	if (bInstanceAttempted || !LoadLoader())
	{
		return false;
	}
	bInstanceAttempted = true;

	uint32 Count = 0;
	if (!XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &Count, nullptr)))
	{
		return false;
	}
	TArray<XrExtensionProperties> Extensions;
	Extensions.Init(XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES }, Count);
	if (!XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, Count, &Count, Extensions.GetData())))
	{
		return false;
	}
	bool bMetal = false;
	FString Names;
	for (const XrExtensionProperties& Ext : Extensions)
	{
		bMetal |= std::strcmp(Ext.extensionName, XR_KHR_METAL_ENABLE_EXTENSION_NAME) == 0;
		bFrameControllerExtension |= std::strcmp(Ext.extensionName, FrameControllerExtensionName) == 0;
		bCylinderExtension |= std::strcmp(Ext.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0;
		Names += FString::Printf(TEXT("%s%s"), Names.IsEmpty() ? TEXT("") : TEXT(" "), ANSI_TO_TCHAR(Ext.extensionName));
	}
	UE_LOG(LogFrameXR, Log, TEXT("Runtime extensions (%u): %s"), Count, *Names);
	if (!bMetal)
	{
		UE_LOG(LogFrameXR, Warning, TEXT("The runtime has no %s; FrameXR needs Metal"), TEXT(XR_KHR_METAL_ENABLE_EXTENSION_NAME));
		return false;
	}

	TArray<const char*, TInlineAllocator<4>> Enabled = { XR_KHR_METAL_ENABLE_EXTENSION_NAME };
	if (bFrameControllerExtension) { Enabled.Add(FrameControllerExtensionName); }
	if (bCylinderExtension) { Enabled.Add(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME); }
	XrInstanceCreateInfo Info{ XR_TYPE_INSTANCE_CREATE_INFO };
	CopyName(Info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "FrameXR Unreal");
	Info.applicationInfo.applicationVersion = 1;
	CopyName(Info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Unreal Engine");
	Info.applicationInfo.engineVersion = ENGINE_MAJOR_VERSION * 100 + ENGINE_MINOR_VERSION;
	Info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	Info.enabledExtensionCount = Enabled.Num();
	Info.enabledExtensionNames = Enabled.GetData();
	if (!XR_CHECK(xrCreateInstance(&Info, &Instance)))
	{
		Instance = XR_NULL_HANDLE;
		return false;
	}
#define FRAMEXR_RESOLVE(Name) \
	if (!XR_CHECK(xrGetInstanceProcAddr(Instance, #Name, reinterpret_cast<PFN_xrVoidFunction*>(&Name)))) { Destroy(); return false; }
	FRAMEXR_INSTANCE_FUNCTIONS(FRAMEXR_RESOLVE)
#undef FRAMEXR_RESOLVE

	XrInstanceProperties Props{ XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_CHECK(xrGetInstanceProperties(Instance, &Props)))
	{
		RuntimeName = FString::Printf(TEXT("%s %u.%u.%u"), ANSI_TO_TCHAR(Props.runtimeName),
			XR_VERSION_MAJOR(Props.runtimeVersion), XR_VERSION_MINOR(Props.runtimeVersion), XR_VERSION_PATCH(Props.runtimeVersion));
	}

	XrSystemGetInfo SystemInfo{ XR_TYPE_SYSTEM_GET_INFO };
	SystemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XR_CHECK(xrGetSystem(Instance, &SystemInfo, &SystemId)))
	{
		Destroy();
		return false;
	}
	XrSystemProperties SystemProps{ XR_TYPE_SYSTEM_PROPERTIES };
	XR_CHECK(xrGetSystemProperties(Instance, SystemId, &SystemProps));

	uint32 ViewCount = 0;
	ViewConfigs[0] = ViewConfigs[1] = XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW };
	if (!XR_CHECK(xrEnumerateViewConfigurationViews(Instance, SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &ViewCount, ViewConfigs)) || ViewCount != 2)
	{
		UE_LOG(LogFrameXR, Warning, TEXT("The system has %u stereo views, not 2"), ViewCount);
		Destroy();
		return false;
	}
	EyeSize = FIntPoint(ViewConfigs[0].recommendedImageRectWidth, ViewConfigs[0].recommendedImageRectHeight);
	// [FrameXR] PixelDensity (DefaultEngine.ini; env FRAMEXR_PIXEL_DENSITY overrides) scales the eye buffers and the
	// swapchain together (OXRSys accepts any swapchain size and scales in its encode pass), so a density below 1 cuts
	// both the engine's GPU frame and the runtime's copy. Read once here: the swapchain is made once per session.
	// Not vr.PixelDensity: that cvar is registered after this runs ("deferred - dummy variable created"), and the
	// engine's r.ScreenPercentage heuristic is untested on this stereo family.
	{
		float Density = 1.f;
		if (GConfig)
		{
			GConfig->GetFloat(TEXT("FrameXR"), TEXT("PixelDensity"), Density, GEngineIni);
		}
		const FString EnvDensity = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEXR_PIXEL_DENSITY"));
		if (!EnvDensity.IsEmpty())
		{
			Density = FCString::Atof(*EnvDensity);
		}
		Density = FMath::Clamp(Density, 0.5f, 1.f);
		if (Density < 1.f)
		{
			const FIntPoint Recommended = EyeSize;
			EyeSize = FIntPoint(FMath::CeilToInt(Recommended.X * Density) & ~1, FMath::CeilToInt(Recommended.Y * Density) & ~1);
			UE_LOG(LogFrameXR, Log, TEXT("PixelDensity %.2f: eye buffers %dx%d (recommended %dx%d)"), Density, EyeSize.X, EyeSize.Y, Recommended.X, Recommended.Y);
		}
	}
	UE_LOG(LogFrameXR, Log, TEXT("Runtime %s, system '%s' (id %llu): %dx%d per eye recommended, %ux%u max, positional tracking %s"),
		*RuntimeName, ANSI_TO_TCHAR(SystemProps.systemName), (unsigned long long)SystemId, EyeSize.X, EyeSize.Y,
		ViewConfigs[0].maxImageRectWidth, ViewConfigs[0].maxImageRectHeight,
		SystemProps.trackingProperties.positionTracking ? TEXT("yes") : TEXT("no"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Session

bool FFrameXRSession::CreateSession(void* MetalCommandQueue)
{
	if (HasSession())
	{
		return true;
	}
	if (!HasInstance() || !MetalCommandQueue)
	{
		return false;
	}
	FScopeLock Lock(&SessionLock);

	XrGraphicsRequirementsMetalKHR Requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
	if (!XR_CHECK(xrGetMetalGraphicsRequirementsKHR(Instance, SystemId, &Requirements)))
	{
		return false;
	}
	MTL::Device* EngineDevice = GetIMetalDynamicRHI() ? GetIMetalDynamicRHI()->RHIGetDevice() : nullptr;
	UE_LOG(LogFrameXR, Log, TEXT("Runtime Metal device %p, engine Metal device %p%s"), Requirements.metalDevice, EngineDevice,
		(Requirements.metalDevice && EngineDevice && Requirements.metalDevice != EngineDevice) ? TEXT(" (DIFFERENT: the blit may fail)") : TEXT(""));

	XrGraphicsBindingMetalKHR Binding{ XR_TYPE_GRAPHICS_BINDING_METAL_KHR };
	Binding.commandQueue = MetalCommandQueue;
	XrSessionCreateInfo Info{ XR_TYPE_SESSION_CREATE_INFO };
	Info.next = &Binding;
	Info.systemId = SystemId;
	XrSession NewSession = XR_NULL_HANDLE;
	if (!XR_CHECK(xrCreateSession(Instance, &Info, &NewSession)))
	{
		return false;
	}
	Session = NewSession;
	UE_LOG(LogFrameXR, Log, TEXT("Session created on Metal queue %p"), MetalCommandQueue);

	if (!CreateSpaces() || !CreateSwapchain())
	{
		Destroy();
		return false;
	}
	CreateActions(); // Input is optional: a session without controllers still renders.
	bLost = false; // A session made after an exit or loss is a live one again.
	bSessionDestroyPending = false;
	return true;
}

XrSpace FFrameXRSession::SpaceFor(XrReferenceSpaceType Space) const
{
	switch (Space)
	{
	case XR_REFERENCE_SPACE_TYPE_VIEW: return ViewSpace;
	case XR_REFERENCE_SPACE_TYPE_LOCAL: return LocalSpace;
	// OXRSys anchors LOCAL at the head at session start (eye height), so a floor origin without LOCAL_FLOOR must
	// fall back to STAGE (floor-relative), never LOCAL: the first wear put the eyes on the floor (2026-10-20).
	case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: return LocalFloorSpace != XR_NULL_HANDLE ? LocalFloorSpace : (StageSpace != XR_NULL_HANDLE ? StageSpace : LocalSpace);
	case XR_REFERENCE_SPACE_TYPE_STAGE: return StageSpace != XR_NULL_HANDLE ? StageSpace : LocalSpace;
	default: return LocalSpace;
	}
}

bool FFrameXRSession::CreateSpaces()
{
	uint32 Count = 0;
	TArray<XrReferenceSpaceType> Supported;
	if (XR_CHECK(xrEnumerateReferenceSpaces(Session, 0, &Count, nullptr)))
	{
		Supported.SetNum(Count);
		XR_CHECK(xrEnumerateReferenceSpaces(Session, Count, &Count, Supported.GetData()));
	}
	auto Make = [this, &Supported](XrReferenceSpaceType Type, XrSpace& Out, bool bRequired) -> bool
	{
		if (!Supported.Contains(Type))
		{
			UE_CLOG(bRequired, LogFrameXR, Warning, TEXT("Reference space %d is not supported"), (int32)Type);
			return !bRequired;
		}
		XrReferenceSpaceCreateInfo Info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		Info.referenceSpaceType = Type;
		Info.poseInReferenceSpace.orientation.w = 1.f;
		if (!XR_CHECK(xrCreateReferenceSpace(Session, &Info, &Out)))
		{
			Out = XR_NULL_HANDLE;
			return !bRequired;
		}
		return true;
	};
	const bool bOk = Make(XR_REFERENCE_SPACE_TYPE_VIEW, ViewSpace, true)
		&& Make(XR_REFERENCE_SPACE_TYPE_LOCAL, LocalSpace, true)
		&& Make(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, LocalFloorSpace, false)
		&& Make(XR_REFERENCE_SPACE_TYPE_STAGE, StageSpace, false);
	UE_LOG(LogFrameXR, Log, TEXT("Reference spaces: view %s, local %s, local_floor %s, stage %s"),
		ViewSpace ? TEXT("ok") : TEXT("-"), LocalSpace ? TEXT("ok") : TEXT("-"),
		LocalFloorSpace ? TEXT("ok") : TEXT("missing (falls back to local)"), StageSpace ? TEXT("ok") : TEXT("missing (falls back to local)"));
	return bOk;
}

bool FFrameXRSession::CreateSwapchain()
{
	uint32 Count = 0;
	TArray<int64> Formats;
	if (XR_CHECK(xrEnumerateSwapchainFormats(Session, 0, &Count, nullptr)))
	{
		Formats.SetNum(Count);
		XR_CHECK(xrEnumerateSwapchainFormats(Session, Count, &Count, Formats.GetData()));
	}
	FString List;
	for (int64 F : Formats) { List += FString::Printf(TEXT("%lld "), (long long)F); }
	SwapchainFormats = Formats;

	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEXR_SWAPCHAIN_FORMAT"));
	if (!Override.IsEmpty() && Formats.Contains(FCString::Atoi64(*Override)))
	{
		SwapchainFormat = FCString::Atoi64(*Override);
	}
	else if (Formats.Contains(MetalBGRA8UnormSRGB))
	{
		SwapchainFormat = MetalBGRA8UnormSRGB;
	}
	else if (Formats.Contains(MetalBGRA8Unorm))
	{
		SwapchainFormat = MetalBGRA8Unorm;
	}
	else if (Formats.Num() > 0)
	{
		SwapchainFormat = Formats[0];
	}
	else
	{
		UE_LOG(LogFrameXR, Warning, TEXT("The runtime offers no swapchain formats"));
		return false;
	}

	XrSwapchainCreateInfo Info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	Info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	Info.format = SwapchainFormat;
	Info.sampleCount = 1;
	Info.width = EyeSize.X * 2;
	Info.height = EyeSize.Y;
	Info.faceCount = 1;
	Info.arraySize = 1;
	Info.mipCount = 1;
	if (!XR_CHECK(xrCreateSwapchain(Session, &Info, &Swapchain)))
	{
		Swapchain = XR_NULL_HANDLE;
		return false;
	}
	uint32 ImageCount = 0;
	if (!XR_CHECK(xrEnumerateSwapchainImages(Swapchain, 0, &ImageCount, nullptr)))
	{
		return false;
	}
	SwapchainImages.Init(XrSwapchainImageMetalKHR{ XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR }, ImageCount);
	if (!XR_CHECK(xrEnumerateSwapchainImages(Swapchain, ImageCount, &ImageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(SwapchainImages.GetData()))))
	{
		return false;
	}
	UE_LOG(LogFrameXR, Log, TEXT("Swapchain %ux%u format %lld (offered: %s), %u images"), Info.width, Info.height, (long long)SwapchainFormat, *List, ImageCount);
	return ImageCount > 0;
}

bool FFrameXRSession::CreateActions()
{
	// The action set, its actions and the suggested bindings belong to the instance: made once, and kept across a
	// session the runtime exited or lost (a second suggestion after an attach is XR_ERROR_ACTIONSETS_ALREADY_ATTACHED).
	if (ActionSet == XR_NULL_HANDLE)
	{
		XrActionSetCreateInfo SetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
		CopyName(SetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "framexr");
		CopyName(SetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "FrameXR");
		if (!XR_CHECK(xrCreateActionSet(Instance, &SetInfo, &ActionSet)))
		{
			ActionSet = XR_NULL_HANDLE;
			return false;
		}
		XR_CHECK(xrStringToPath(Instance, "/user/hand/left", &HandPaths[0]));
		XR_CHECK(xrStringToPath(Instance, "/user/hand/right", &HandPaths[1]));

		auto MakeAction = [this](const char* Name, const char* Localized, XrActionType Type, XrAction& Out)
		{
			XrActionCreateInfo Info{ XR_TYPE_ACTION_CREATE_INFO };
			CopyName(Info.actionName, XR_MAX_ACTION_NAME_SIZE, Name);
			CopyName(Info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, Localized);
			Info.actionType = Type;
			Info.countSubactionPaths = 2;
			Info.subactionPaths = HandPaths;
			if (!XR_CHECK(xrCreateAction(ActionSet, &Info, &Out)))
			{
				Out = XR_NULL_HANDLE;
			}
		};
		MakeAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, GripPoseAction);
		MakeAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, AimPoseAction);
		MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, TriggerAction);
		MakeAction("trigger_touch", "Trigger Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, TriggerTouchAction);
		MakeAction("squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT, SqueezeAction);
		MakeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, StickAction);
		MakeAction("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT, StickClickAction);
		MakeAction("thumbstick_touch", "Thumbstick Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, StickTouchAction);
		MakeAction("primary_click", "Primary Click", XR_ACTION_TYPE_BOOLEAN_INPUT, PrimaryClickAction);
		MakeAction("secondary_click", "Secondary Click", XR_ACTION_TYPE_BOOLEAN_INPUT, SecondaryClickAction);
		MakeAction("primary_touch", "Primary Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, PrimaryTouchAction);
		MakeAction("secondary_touch", "Secondary Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, SecondaryTouchAction);
		MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, MenuAction);
		MakeAction("thumbrest_touch", "Thumb Rest Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, ThumbRestAction);
		if (bFrameControllerExtension)
		{
			MakeAction("frame_x", "Frame X", XR_ACTION_TYPE_BOOLEAN_INPUT, FrameXAction);
			MakeAction("frame_y", "Frame Y", XR_ACTION_TYPE_BOOLEAN_INPUT, FrameYAction);
			MakeAction("frame_menu", "Frame Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, FrameMenuAction);
			MakeAction("dpad_up", "D-pad Up", XR_ACTION_TYPE_BOOLEAN_INPUT, DpadActions[0]);
			MakeAction("dpad_down", "D-pad Down", XR_ACTION_TYPE_BOOLEAN_INPUT, DpadActions[1]);
			MakeAction("dpad_left", "D-pad Left", XR_ACTION_TYPE_BOOLEAN_INPUT, DpadActions[2]);
			MakeAction("dpad_right", "D-pad Right", XR_ACTION_TYPE_BOOLEAN_INPUT, DpadActions[3]);
			MakeAction("bumper", "Bumper", XR_ACTION_TYPE_BOOLEAN_INPUT, BumperAction);
		}

		auto Suggest = [this](const char* Profile, const TArray<TPair<XrAction, const char*>>& Pairs)
		{
			TArray<XrActionSuggestedBinding> Bindings;
			for (const TPair<XrAction, const char*>& Pair : Pairs)
			{
				XrPath Path = XR_NULL_PATH;
				if (Pair.Key != XR_NULL_HANDLE && XR_SUCCEEDED(xrStringToPath(Instance, Pair.Value, &Path)))
				{
					Bindings.Add({ Pair.Key, Path });
				}
			}
			XrPath ProfilePath = XR_NULL_PATH;
			XrInteractionProfileSuggestedBinding Info{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
			if (!XR_SUCCEEDED(xrStringToPath(Instance, Profile, &ProfilePath)))
			{
				return;
			}
			Info.interactionProfile = ProfilePath;
			Info.countSuggestedBindings = Bindings.Num();
			Info.suggestedBindings = Bindings.GetData();
			const XrResult Result = xrSuggestInteractionProfileBindings(Instance, &Info);
			UE_LOG(LogFrameXR, Log, TEXT("Bindings for %s: %s"), ANSI_TO_TCHAR(Profile), XR_SUCCEEDED(Result) ? TEXT("accepted") : TEXT("rejected"));
		};

		auto Both = [](XrAction A, const char* Left, const char* Right) { return TArray<TPair<XrAction, const char*>>{ { A, Left }, { A, Right } }; };
		TArray<TPair<XrAction, const char*>> Touch;
		Touch.Append(Both(GripPoseAction, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"));
		Touch.Append(Both(AimPoseAction, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose"));
		Touch.Append(Both(TriggerAction, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"));
		Touch.Append(Both(TriggerTouchAction, "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch"));
		Touch.Append(Both(SqueezeAction, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"));
		Touch.Append(Both(StickAction, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"));
		Touch.Append(Both(StickClickAction, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"));
		Touch.Append(Both(StickTouchAction, "/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch"));
		Touch.Append(Both(PrimaryClickAction, "/user/hand/left/input/x/click", "/user/hand/right/input/a/click"));
		Touch.Append(Both(SecondaryClickAction, "/user/hand/left/input/y/click", "/user/hand/right/input/b/click"));
		Touch.Append(Both(PrimaryTouchAction, "/user/hand/left/input/x/touch", "/user/hand/right/input/a/touch"));
		Touch.Append(Both(SecondaryTouchAction, "/user/hand/left/input/y/touch", "/user/hand/right/input/b/touch"));
		Touch.Append(Both(ThumbRestAction, "/user/hand/left/input/thumbrest/touch", "/user/hand/right/input/thumbrest/touch"));
		Touch.Add({ MenuAction, "/user/hand/left/input/menu/click" });
		Suggest("/interaction_profiles/oculus/touch_controller", Touch);

		TArray<TPair<XrAction, const char*>> Index;
		Index.Append(Both(GripPoseAction, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"));
		Index.Append(Both(AimPoseAction, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose"));
		Index.Append(Both(TriggerAction, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"));
		Index.Append(Both(TriggerTouchAction, "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch"));
		Index.Append(Both(SqueezeAction, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"));
		Index.Append(Both(StickAction, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"));
		Index.Append(Both(StickClickAction, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"));
		Index.Append(Both(StickTouchAction, "/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch"));
		Index.Append(Both(PrimaryClickAction, "/user/hand/left/input/a/click", "/user/hand/right/input/a/click"));
		Index.Append(Both(SecondaryClickAction, "/user/hand/left/input/b/click", "/user/hand/right/input/b/click"));
		Index.Append(Both(PrimaryTouchAction, "/user/hand/left/input/a/touch", "/user/hand/right/input/a/touch"));
		Index.Append(Both(SecondaryTouchAction, "/user/hand/left/input/b/touch", "/user/hand/right/input/b/touch"));
		Suggest("/interaction_profiles/valve/index_controller", Index);

		// The Steam Frame's own controller. Bound natively, SteamVR stops folding its buttons into Touch: each arrives
		// on its own key (FrameXRInput), while the Touch actions get the meaning SteamVR's own remap gives them, so a
		// Touch input map keeps working unchanged.
		if (bFrameControllerExtension)
		{
			TArray<TPair<XrAction, const char*>> Frame;
			Frame.Append(Both(GripPoseAction, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"));
			Frame.Append(Both(AimPoseAction, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose"));
			Frame.Append(Both(TriggerAction, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"));
			Frame.Append(Both(TriggerTouchAction, "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch"));
			Frame.Append(Both(SqueezeAction, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"));
			Frame.Append(Both(StickAction, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"));
			Frame.Append(Both(StickClickAction, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"));
			Frame.Append(Both(StickTouchAction, "/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch"));
			Frame.Append(Both(PrimaryClickAction, "/user/hand/left/input/dpad_down/click", "/user/hand/right/input/a/click"));
			Frame.Append(Both(PrimaryTouchAction, "/user/hand/left/input/dpad_down/touch", "/user/hand/right/input/a/touch"));
			for (const char* Path : { "/user/hand/left/input/dpad_up/click", "/user/hand/left/input/dpad_left/click",
				 "/user/hand/left/input/dpad_right/click", "/user/hand/right/input/b/click", "/user/hand/right/input/x/click",
				 "/user/hand/right/input/y/click" })
			{
				Frame.Add({ SecondaryClickAction, Path });
			}
			for (const char* Path : { "/user/hand/left/input/dpad_up/touch", "/user/hand/left/input/dpad_left/touch",
				 "/user/hand/left/input/dpad_right/touch", "/user/hand/right/input/b/touch", "/user/hand/right/input/x/touch",
				 "/user/hand/right/input/y/touch" })
			{
				Frame.Add({ SecondaryTouchAction, Path });
			}
			Frame.Add({ MenuAction, "/user/hand/left/input/view/click" });
			Frame.Add({ FrameXAction, "/user/hand/right/input/x/click" });
			Frame.Add({ FrameYAction, "/user/hand/right/input/y/click" });
			Frame.Add({ FrameMenuAction, "/user/hand/right/input/menu/click" });
			Frame.Add({ DpadActions[0], "/user/hand/left/input/dpad_up/click" });
			Frame.Add({ DpadActions[1], "/user/hand/left/input/dpad_down/click" });
			Frame.Add({ DpadActions[2], "/user/hand/left/input/dpad_left/click" });
			Frame.Add({ DpadActions[3], "/user/hand/left/input/dpad_right/click" });
			Frame.Append(Both(BumperAction, "/user/hand/left/input/bumper/click", "/user/hand/right/input/bumper/click"));
			Suggest(FrameControllerProfile, Frame);
			xrStringToPath(Instance, FrameControllerProfile, &FrameProfilePath);
		}

		TArray<TPair<XrAction, const char*>> Simple;
		Simple.Append(Both(GripPoseAction, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"));
		Simple.Append(Both(AimPoseAction, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose"));
		Simple.Append(Both(TriggerAction, "/user/hand/left/input/select/click", "/user/hand/right/input/select/click"));
		Simple.Append(Both(MenuAction, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click"));
		Suggest("/interaction_profiles/khr/simple_controller", Simple);

	}

	XrSessionActionSetsAttachInfo Attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	Attach.countActionSets = 1;
	Attach.actionSets = &ActionSet;
	if (!XR_CHECK(xrAttachSessionActionSets(Session, &Attach)))
	{
		return false; // A recreated session on a runtime that attaches once per process: it renders, with no controllers.
	}
	bActionsAttached = true;
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		XrActionSpaceCreateInfo SpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		SpaceInfo.subactionPath = HandPaths[Hand];
		SpaceInfo.poseInActionSpace.orientation.w = 1.f;
		SpaceInfo.action = GripPoseAction;
		XR_CHECK(xrCreateActionSpace(Session, &SpaceInfo, &GripSpaces[Hand]));
		SpaceInfo.action = AimPoseAction;
		XR_CHECK(xrCreateActionSpace(Session, &SpaceInfo, &AimSpaces[Hand]));
	}
	return true;
}

void FFrameXRSession::DestroySessionObjects()
{
	FScopeLock Lock(&SessionLock);
	bRunning = false;
	bActionsAttached = false;
	bImagePending = false;
	if (Session != XR_NULL_HANDLE)
	{
		for (int32 Hand = 0; Hand < 2; ++Hand)
		{
			if (GripSpaces[Hand]) { xrDestroySpace(GripSpaces[Hand]); GripSpaces[Hand] = XR_NULL_HANDLE; }
			if (AimSpaces[Hand]) { xrDestroySpace(AimSpaces[Hand]); AimSpaces[Hand] = XR_NULL_HANDLE; }
		}
		if (Swapchain) { xrDestroySwapchain(Swapchain); Swapchain = XR_NULL_HANDLE; }
		SwapchainImages.Reset();
		DestroyLayerSwapchains();
		for (XrSpace* Space : { &ViewSpace, &LocalSpace, &LocalFloorSpace, &StageSpace })
		{
			if (*Space) { xrDestroySpace(*Space); *Space = XR_NULL_HANDLE; }
		}
		xrDestroySession(Session);
		Session = XR_NULL_HANDLE;
	}
	WaitedFrames.Empty();
	SessionState = XR_SESSION_STATE_UNKNOWN;
}

void FFrameXRSession::Destroy()
{
	if (Session != XR_NULL_HANDLE && IsRunning())
	{
		// The orderly exit: request it, end the session at STOPPING, destroy at EXITING. Bounded (200 ms), so a
		// silent runtime cannot hold the shutdown; the destroy below happens either way.
		if (XR_SUCCEEDED(xrRequestExitSession(Session)))
		{
			for (int32 Tries = 0; Tries < 40 && Session != XR_NULL_HANDLE && SessionState != XR_SESSION_STATE_EXITING; ++Tries)
			{
				PollEvents();
				if (Session != XR_NULL_HANDLE && SessionState != XR_SESSION_STATE_EXITING)
				{
					FPlatformProcess::Sleep(0.005f);
				}
			}
		}
	}
	DestroySessionObjects();
	if (ActionSet) { xrDestroyActionSet(ActionSet); ActionSet = XR_NULL_HANDLE; }
	if (Instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(Instance);
		Instance = XR_NULL_HANDLE;
	}
	UE_LOG(LogFrameXR, Log, TEXT("OpenXR instance destroyed (%llu frames submitted, %llu skipped)"), (unsigned long long)FramesSubmitted, (unsigned long long)FramesSkipped);
}

// ---------------------------------------------------------------------------------------------------------------
// Events (game thread)

void FFrameXRSession::HandleSessionState(XrSessionState NewState)
{
	SessionState = NewState;
	switch (NewState)
	{
	case XR_SESSION_STATE_READY:
	{
		XrSessionBeginInfo Info{ XR_TYPE_SESSION_BEGIN_INFO };
		Info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (XR_CHECK(xrBeginSession(Session, &Info)))
		{
			bRunning = true;
			UE_LOG(LogFrameXR, Log, TEXT("Session begun"));
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING:
		bRunning = false;
		XR_CHECK(xrEndSession(Session));
		UE_LOG(LogFrameXR, Log, TEXT("Session ended (stopping)"));
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		bRunning = false;
		bLost = true;
		bSessionDestroyPending = true; // Destroyed at the end of this poll; the next present makes a new session.
		UE_LOG(LogFrameXR, Warning, TEXT("Session %s; it is destroyed and a new one is made at the next present"), NewState == XR_SESSION_STATE_EXITING ? TEXT("exiting") : TEXT("loss pending"));
		break;
	case XR_SESSION_STATE_SYNCHRONIZED:
	case XR_SESSION_STATE_VISIBLE:
	case XR_SESSION_STATE_FOCUSED:
		UE_LOG(LogFrameXR, Log, TEXT("Session state %s"),
			NewState == XR_SESSION_STATE_SYNCHRONIZED ? TEXT("synchronized") : NewState == XR_SESSION_STATE_VISIBLE ? TEXT("visible") : TEXT("focused"));
		break;
	default:
		break;
	}
}

void FFrameXRSession::LogInteractionProfiles() const
{
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		XrInteractionProfileState State{ XR_TYPE_INTERACTION_PROFILE_STATE };
		if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(Session, HandPaths[Hand], &State)))
		{
			char Buffer[XR_MAX_PATH_LENGTH] = "none";
			uint32 Length = 0;
			if (State.interactionProfile != XR_NULL_PATH)
			{
				xrPathToString(Instance, State.interactionProfile, sizeof(Buffer), &Length, Buffer);
			}
			UE_LOG(LogFrameXR, Log, TEXT("%s hand interaction profile: %s"), Hand == 0 ? TEXT("Left") : TEXT("Right"), ANSI_TO_TCHAR(Buffer));
			bFrameProfileActive[Hand] = FrameProfilePath != XR_NULL_PATH && State.interactionProfile == FrameProfilePath;
		}
	}
}

void FFrameXRSession::PollEvents()
{
	if (!HasInstance())
	{
		return;
	}
	for (int32 Guard = 0; Guard < 32; ++Guard)
	{
		XrEventDataBuffer Event{ XR_TYPE_EVENT_DATA_BUFFER };
		const XrResult Result = xrPollEvent(Instance, &Event);
		if (Result == XR_EVENT_UNAVAILABLE)
		{
			break;
		}
		if (!XR_CHECK(Result))
		{
			break;
		}
		switch (Event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const auto* Changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&Event);
			if (Changed->session == Session)
			{
				HandleSessionState(Changed->state);
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			bRunning = false;
			bLost = true;
			UE_LOG(LogFrameXR, Warning, TEXT("Instance loss pending"));
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			LogInteractionProfiles();
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			UE_LOG(LogFrameXR, Log, TEXT("Reference space change pending (recenter)"));
			break;
		default:
			break;
		}
	}
	if (bSessionDestroyPending.exchange(false))
	{
		DestroySessionObjects();
		bLost = false; // Not dead: FFrameXRRenderBridge::Submit makes a new session at the next present. (A lost instance stays lost.)
		UE_LOG(LogFrameXR, Log, TEXT("Session destroyed; a new one is made at the next present"));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Tracking (game + render threads)

XrTime FFrameXRSession::WaitFrame_GameThread()
{
	if (!HasSession() || !IsRunning() || bLost.load())
	{
		return 0;
	}
	const double T0 = FPlatformTime::Seconds();
	XrFrameWaitInfo WaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState FrameState{ XR_TYPE_FRAME_STATE };
	if (!XR_CHECK(xrWaitFrame(Session, &WaitInfo, &FrameState)))
	{
		return 0;
	}
	StatWaitMicros.fetch_add((int64)((FPlatformTime::Seconds() - T0) * 1e6));
	LastDisplayTime = FrameState.predictedDisplayTime;
	LastDisplayPeriod = FrameState.predictedDisplayPeriod;
	WaitedFrames.Enqueue(FWaitedFrame{ FrameState.predictedDisplayTime, FrameState.predictedDisplayPeriod, FrameState.shouldRender == XR_TRUE });
	return FrameState.predictedDisplayTime;
}

XrTime FFrameXRSession::EstimateDisplayTime() const
{
	const int64 Last = LastDisplayTime.load();
	return Last > 0 ? Last + LastDisplayPeriod.load() : 0;
}

float FFrameXRSession::GetDisplayPeriodMs() const
{
	return LastDisplayPeriod.load() / 1.0e6f;
}

bool FFrameXRSession::LocateViews(XrTime Time, XrReferenceSpaceType Space, FFrameXRViews& Out) const
{
	Out.bValid = false;
	Out.bPositionValid = false;
	Out.Time = Time;
	FScopeLock Lock(&SessionLock);
	const XrSpace Base = SpaceFor(Space);
	if (!HasSession() || !IsRunning() || Time <= 0 || Base == XR_NULL_HANDLE)
	{
		return false;
	}
	XrViewLocateInfo Info{ XR_TYPE_VIEW_LOCATE_INFO };
	Info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	Info.displayTime = Time;
	Info.space = Base;
	XrViewState State{ XR_TYPE_VIEW_STATE };
	uint32 Count = 0;
	Out.Views[0] = Out.Views[1] = XrView{ XR_TYPE_VIEW };
	if (!XR_CHECK(xrLocateViews(Session, &Info, &State, 2, &Count, Out.Views)) || Count != 2)
	{
		return false;
	}
	// Orientation alone renders and submits: a dropout to orientation-only tracking keeps the runtime's last position
	// rather than a frame with no layer (black). No orientation is no frame.
	if ((State.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
	{
		return false;
	}
	Out.bPositionValid = (State.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
	XrSpaceLocation Head{ XR_TYPE_SPACE_LOCATION };
	if (!XR_CHECK(xrLocateSpace(ViewSpace, Base, Time, &Head)) || (Head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
	{
		// The head is the midpoint of the eyes if the view space will not locate.
		Head.pose.orientation = Out.Views[0].pose.orientation;
		Head.pose.position.x = 0.5f * (Out.Views[0].pose.position.x + Out.Views[1].pose.position.x);
		Head.pose.position.y = 0.5f * (Out.Views[0].pose.position.y + Out.Views[1].pose.position.y);
		Head.pose.position.z = 0.5f * (Out.Views[0].pose.position.z + Out.Views[1].pose.position.z);
	}
	Out.Head = Head.pose;
	Out.bValid = true;
	return true;
}

bool FFrameXRSession::SyncActions(XrTime Time, XrReferenceSpaceType Space, FFrameXRController Out[2]) const
{
	Out[0] = Out[1] = FFrameXRController();
	if (!HasSession() || !IsRunning() || ActionSet == XR_NULL_HANDLE || !bActionsAttached)
	{
		return false;
	}
	XrActiveActionSet Active{ ActionSet, XR_NULL_PATH };
	XrActionsSyncInfo Sync{ XR_TYPE_ACTIONS_SYNC_INFO };
	Sync.countActiveActionSets = 1;
	Sync.activeActionSets = &Active;
	const XrResult SyncResult = xrSyncActions(Session, &Sync);
	if (SyncResult == XR_SESSION_NOT_FOCUSED)
	{
		return false; // Nothing is delivered until the session is focused.
	}
	if (!Check(SyncResult, TEXT("xrSyncActions")))
	{
		return false;
	}

	const XrSpace Base = SpaceFor(Space);
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		FFrameXRController& C = Out[Hand];
		XrActionStateGetInfo Get{ XR_TYPE_ACTION_STATE_GET_INFO };
		Get.subactionPath = HandPaths[Hand];

		auto Bool = [&](XrAction Action) -> bool
		{
			if (Action == XR_NULL_HANDLE) { return false; }
			Get.action = Action;
			XrActionStateBoolean State{ XR_TYPE_ACTION_STATE_BOOLEAN };
			return XR_SUCCEEDED(xrGetActionStateBoolean(Session, &Get, &State)) && State.isActive && State.currentState;
		};
		auto Float = [&](XrAction Action) -> float
		{
			if (Action == XR_NULL_HANDLE) { return 0.f; }
			Get.action = Action;
			XrActionStateFloat State{ XR_TYPE_ACTION_STATE_FLOAT };
			return XR_SUCCEEDED(xrGetActionStateFloat(Session, &Get, &State)) && State.isActive ? State.currentState : 0.f;
		};

		Get.action = GripPoseAction;
		XrActionStatePose PoseState{ XR_TYPE_ACTION_STATE_POSE };
		C.bActive = GripPoseAction != XR_NULL_HANDLE && XR_SUCCEEDED(xrGetActionStatePose(Session, &Get, &PoseState)) && PoseState.isActive;
		if (!C.bActive)
		{
			continue;
		}
		constexpr XrSpaceLocationFlags Needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		XrSpaceLocation Location{ XR_TYPE_SPACE_LOCATION };
		if (GripSpaces[Hand] && Base && XR_SUCCEEDED(xrLocateSpace(GripSpaces[Hand], Base, Time, &Location)) && (Location.locationFlags & Needed) == Needed)
		{
			C.bGripValid = true;
			C.Grip = Location.pose;
		}
		Location = XrSpaceLocation{ XR_TYPE_SPACE_LOCATION };
		if (AimSpaces[Hand] && Base && XR_SUCCEEDED(xrLocateSpace(AimSpaces[Hand], Base, Time, &Location)) && (Location.locationFlags & Needed) == Needed)
		{
			C.bAimValid = true;
			C.Aim = Location.pose;
		}
		C.Trigger = Float(TriggerAction);
		C.Squeeze = Float(SqueezeAction);
		if (StickAction != XR_NULL_HANDLE)
		{
			Get.action = StickAction;
			XrActionStateVector2f State{ XR_TYPE_ACTION_STATE_VECTOR2F };
			if (XR_SUCCEEDED(xrGetActionStateVector2f(Session, &Get, &State)) && State.isActive)
			{
				C.Stick = FVector2f(State.currentState.x, State.currentState.y);
			}
		}
		C.bTriggerTouch = Bool(TriggerTouchAction);
		C.bStickClick = Bool(StickClickAction);
		C.bStickTouch = Bool(StickTouchAction);
		C.bPrimaryClick = Bool(PrimaryClickAction);
		C.bSecondaryClick = Bool(SecondaryClickAction);
		C.bPrimaryTouch = Bool(PrimaryTouchAction);
		C.bSecondaryTouch = Bool(SecondaryTouchAction);
		C.bMenu = Bool(MenuAction);
		C.bThumbRestTouch = Bool(ThumbRestAction);
		C.bFrameProfile = bFrameProfileActive[Hand];
		C.bFrameX = Bool(FrameXAction);
		C.bFrameY = Bool(FrameYAction);
		C.bFrameRightMenu = Bool(FrameMenuAction);
		for (int32 D = 0; D < 4; ++D)
		{
			C.bDpad[D] = Bool(DpadActions[D]);
		}
		C.bBumper = Bool(BumperAction);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The frame loop (submission thread)

bool FFrameXRSession::SubmitFrame(void* MetalCommandQueue, void* SrcMetalTexture, const FFrameXRViews& Views, XrReferenceSpaceType Space, TArrayView<const FFrameXRLayer> StereoLayers)
{
	FScopeLock Lock(&SessionLock);
	if (!HasSession() || !IsRunning() || bLost.load())
	{
		return false;
	}

	const double T0 = FPlatformTime::Seconds();
	// The game thread waited for this frame; take its state (older waits were pacing only: the engine rendered
	// nothing for them, so they are skipped here). Before the first wait, or if the queue ran dry, wait inline.
	FWaitedFrame Waited;
	bool bHaveWaited = false;
	while (WaitedFrames.Dequeue(Waited))
	{
		bHaveWaited = true;
		if (Waited.Time >= Views.Time)
		{
			break;
		}
		++FramesSkipped;
	}
	if (!bHaveWaited)
	{
		XrFrameWaitInfo WaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState FrameState{ XR_TYPE_FRAME_STATE };
		if (!XR_CHECK(xrWaitFrame(Session, &WaitInfo, &FrameState)))
		{
			return false;
		}
		LastDisplayTime = FrameState.predictedDisplayTime;
		LastDisplayPeriod = FrameState.predictedDisplayPeriod;
		Waited = FWaitedFrame{ FrameState.predictedDisplayTime, FrameState.predictedDisplayPeriod, FrameState.shouldRender == XR_TRUE };
	}
	XrFrameState FrameState{ XR_TYPE_FRAME_STATE };
	FrameState.predictedDisplayTime = Waited.Time;
	FrameState.predictedDisplayPeriod = Waited.Period;
	FrameState.shouldRender = Waited.bShouldRender ? XR_TRUE : XR_FALSE;
	const double T1 = FPlatformTime::Seconds();
	double TBlit = 0.0;

	XrFrameBeginInfo BeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	if (!XR_CHECK(xrBeginFrame(Session, &BeginInfo))) // XR_FRAME_DISCARDED (a begin without an end) still succeeds
	{
		return false;
	}

	XrFrameEndInfo EndInfo{ XR_TYPE_FRAME_END_INFO };
	EndInfo.displayTime = FrameState.predictedDisplayTime;
	EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	XrCompositionLayerProjectionView LayerViews[2]{};
	XrCompositionLayerProjection Layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	TArray<const XrCompositionLayerBaseHeader*, TInlineAllocator<9>> Layers = { reinterpret_cast<const XrCompositionLayerBaseHeader*>(&Layer) };
	TArray<FFrameXRLayerHeader, TInlineAllocator<8>> LayerHeaders;
	bool bRendered = false;

	MTL::CommandQueue* Queue = static_cast<MTL::CommandQueue*>(MetalCommandQueue);
	MTL::Texture* Src = static_cast<MTL::Texture*>(SrcMetalTexture);
	if (FrameState.shouldRender && Views.bValid && Src && Queue && SwapchainImages.Num() > 0)
	{
		uint32 ImageIndex = PendingImageIndex;
		XrSwapchainImageAcquireInfo AcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		// An image whose wait timed out last frame is still acquired: it is waited for again, not acquired again.
		if (!bImagePending && XR_CHECK(xrAcquireSwapchainImage(Swapchain, &AcquireInfo, &ImageIndex)))
		{
			bImagePending = true;
			PendingImageIndex = ImageIndex;
		}
		if (bImagePending)
		{
			XrSwapchainImageWaitInfo ImageWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			ImageWait.timeout = 1000000000; // 1 s
			const XrResult WaitResult = xrWaitSwapchainImage(Swapchain, &ImageWait);
			if (WaitResult == XR_TIMEOUT_EXPIRED)
			{
				// A success code, but not a waited image: nothing is written to it and it is not released (that would be
				// XR_ERROR_CALL_ORDER_INVALID). This frame ends with no layer; the next one waits for the same image.
				++ImageWaitTimeouts;
				UE_CLOG(ImageWaitTimeouts == 1 || ImageWaitTimeouts % 90 == 0, LogFrameXR, Warning, TEXT("xrWaitSwapchainImage timed out (%llu so far); frame ended without a layer"), (unsigned long long)ImageWaitTimeouts);
			}
			else if (!Check(WaitResult, TEXT("xrWaitSwapchainImage")))
			{
				bImagePending = false; // The runtime refused the wait; what it still holds acquired, the next acquire reports.
			}
			else
			{
				bImagePending = false;
				MTL::Texture* Dst = static_cast<MTL::Texture*>(SwapchainImages[ImageIndex].texture);
				const NS::UInteger Width = FMath::Min(Src->width(), Dst->width());
				const NS::UInteger Height = FMath::Min(Src->height(), Dst->height());
				if (Dst && Width > 1 && Height > 0)
				{
					// The engine's frame is already committed on this queue (we run on its submission thread), so a
					// blit encoded now lands behind it. The wait keeps the release honest; it is the cheapest correct
					// ordering, not the fastest (a completed handler would be).
					MTL::CommandBuffer* Commands = Queue->commandBuffer();
					MTL::BlitCommandEncoder* Blit = Commands->blitCommandEncoder();
					Blit->copyFromTexture(Src, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(Width, Height, 1), Dst, 0, 0, MTL::Origin(0, 0, 0));
					Blit->endEncoding();
					const double B0 = FPlatformTime::Seconds();
					Commands->commit();
					// The runtime snapshots a released image with its own blit on this same queue, signalled by a
					// shared event, so GPU order already protects the copy; a CPU wait here only serialises the
					// engine behind the GPU. FRAMEXR_BLIT_WAIT=1 restores it (and makes the GPU timing exact).
					static const bool bWait = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEXR_BLIT_WAIT")) == TEXT("1");
					if (bWait)
					{
						Commands->waitUntilCompleted();
						// kernelStartTime..GPUStartTime is the queue ahead of the blit (the engine's frame); GPUStart..GPUEnd the blit itself.
						StatQueueAhead += Commands->GPUStartTime() - Commands->kernelStartTime();
						StatBlitGpu += Commands->GPUEndTime() - Commands->GPUStartTime();
					}
					TBlit = FPlatformTime::Seconds() - B0;

					const int32 EyeWidth = (int32)Width / 2;
					for (int32 Eye = 0; Eye < 2; ++Eye)
					{
						LayerViews[Eye] = XrCompositionLayerProjectionView{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
						LayerViews[Eye].pose = Views.Views[Eye].pose;
						LayerViews[Eye].fov = Views.Views[Eye].fov;
						LayerViews[Eye].subImage.swapchain = Swapchain;
						LayerViews[Eye].subImage.imageArrayIndex = 0;
						LayerViews[Eye].subImage.imageRect.offset = { Eye * EyeWidth, 0 };
						LayerViews[Eye].subImage.imageRect.extent = { EyeWidth, (int32)Height };
					}
					Layer.space = SpaceFor(Space);
					Layer.viewCount = 2;
					Layer.views = LayerViews;
					bRendered = true;
				}
				XrSwapchainImageReleaseInfo ReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				XR_CHECK(xrReleaseSwapchainImage(Swapchain, &ReleaseInfo));
			}
		}
	}

	if (bRendered)
	{
		// Stereo layers ride on a frame that has its projection layer; with none, the frame ends empty as before.
		if (StereoLayers.Num() > 0 || LayerSwapchains.Num() > 0)
		{
			AddLayers(MetalCommandQueue, StereoLayers, Layer.space, LayerHeaders);
			for (const FFrameXRLayerHeader& H : LayerHeaders)
			{
				Layers.Add(&H.Base);
			}
		}
		EndInfo.layerCount = Layers.Num();
		EndInfo.layers = Layers.GetData();
		++FramesSubmitted;
	}
	else
	{
		++FramesSkipped;
	}
	const bool bOk = XR_CHECK(xrEndFrame(Session, &EndInfo));
	const double T2 = FPlatformTime::Seconds();
	StatWait += (T1 - T0) + StatWaitMicros.exchange(0) * 1e-6;
	StatBlit += TBlit;
	StatTotal += T2 - T0;
	StatGap += LastSubmitEnd > 0.0 ? T0 - LastSubmitEnd : 0.0;
	LastSubmitEnd = T2;
	if (++StatFrames >= 300)
	{
		UE_LOG(LogFrameXR, Log, TEXT("Frame loop over %d frames: xrWaitFrame (game thread) %.2f ms, blit+wait %.2f ms (GPU queue ahead %.2f ms, blit GPU %.2f ms), submit total %.2f ms, engine gap between submits %.2f ms (%.1f fps); period %.2f ms; %llu submitted, %llu skipped"),
			StatFrames, 1000.0 * StatWait / StatFrames, 1000.0 * StatBlit / StatFrames, 1000.0 * StatQueueAhead / StatFrames, 1000.0 * StatBlitGpu / StatFrames,
			1000.0 * StatTotal / StatFrames, 1000.0 * StatGap / StatFrames, StatFrames / FMath::Max(StatTotal + StatGap, 1e-6), GetDisplayPeriodMs(),
			(unsigned long long)FramesSubmitted, (unsigned long long)FramesSkipped);
		StatFrames = 0;
		StatWait = StatBlit = StatTotal = StatGap = StatQueueAhead = StatBlitGpu = 0.0;
	}
	if (FramesSubmitted == 1 && bRendered)
	{
		UE_LOG(LogFrameXR, Log, TEXT("First frame submitted: display period %.2f ms, views L fov %.1f/%.1f/%.1f/%.1f deg"),
			GetDisplayPeriodMs(), FMath::RadiansToDegrees(Views.Views[0].fov.angleLeft), FMath::RadiansToDegrees(Views.Views[0].fov.angleRight),
			FMath::RadiansToDegrees(Views.Views[0].fov.angleUp), FMath::RadiansToDegrees(Views.Views[0].fov.angleDown));
	}
	return bOk;
}

// ---------------------------------------------------------------------------------------------------------------
// Stereo layers (submission thread, under SessionLock)

bool FFrameXRSession::EnsureLayerSwapchain(uint32 Id, void* SrcMetalTexture)
{
	MTL::Texture* Src = static_cast<MTL::Texture*>(SrcMetalTexture);
	const uint32 Width = (uint32)Src->width();
	const uint32 Height = (uint32)Src->height();
	const int64 Format = (int64)Src->pixelFormat();
	FLayerSwapchain* Chain = LayerSwapchains.Find(Id);
	if (Chain && Chain->Handle && Chain->Width == Width && Chain->Height == Height && Chain->Format == Format)
	{
		return true;
	}
	if (Chain && Chain->Handle)
	{
		xrDestroySwapchain(Chain->Handle);
	}
	LayerSwapchains.Remove(Id);
	// The copy is a byte blit, so the swapchain takes the texture's own format (an 8-bit sRGB widget target: 81).
	if (!SwapchainFormats.Contains(Format) || Width == 0 || Height == 0)
	{
		if (!RefusedLayerFormats.Contains(Format))
		{
			RefusedLayerFormats.Add(Format);
			UE_LOG(LogFrameXR, Warning, TEXT("Stereo layer %u: the runtime offers no swapchain in the texture's format %lld (%ux%u); the layer is not submitted"), Id, (long long)Format, Width, Height);
		}
		return false;
	}
	XrSwapchainCreateInfo Info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	Info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	Info.format = Format;
	Info.sampleCount = 1;
	Info.width = Width;
	Info.height = Height;
	Info.faceCount = 1;
	Info.arraySize = 1;
	Info.mipCount = 1;
	FLayerSwapchain New;
	if (!XR_CHECK(xrCreateSwapchain(Session, &Info, &New.Handle)))
	{
		return false;
	}
	uint32 ImageCount = 0;
	if (XR_CHECK(xrEnumerateSwapchainImages(New.Handle, 0, &ImageCount, nullptr)) && ImageCount > 0)
	{
		New.Images.Init(XrSwapchainImageMetalKHR{ XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR }, ImageCount);
		XR_CHECK(xrEnumerateSwapchainImages(New.Handle, ImageCount, &ImageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(New.Images.GetData())));
	}
	if (New.Images.Num() == 0)
	{
		xrDestroySwapchain(New.Handle);
		return false;
	}
	New.Width = Width;
	New.Height = Height;
	New.Format = Format;
	UE_LOG(LogFrameXR, Log, TEXT("Stereo layer %u: swapchain %ux%u format %lld, %d images"), Id, Width, Height, (long long)Format, New.Images.Num());
	LayerSwapchains.Add(Id, MoveTemp(New));
	return true;
}

void FFrameXRSession::DestroyLayerSwapchains()
{
	for (TPair<uint32, FLayerSwapchain>& Pair : LayerSwapchains)
	{
		if (Pair.Value.Handle) { xrDestroySwapchain(Pair.Value.Handle); }
	}
	LayerSwapchains.Reset();
	LastLayerSummary.Reset();
}

void FFrameXRSession::AddLayers(void* MetalCommandQueue, TArrayView<const FFrameXRLayer> StereoLayers, XrSpace BaseSpace, TArray<FFrameXRLayerHeader, TInlineAllocator<8>>& Out)
{
	++LayerFrame;
	MTL::CommandQueue* Queue = static_cast<MTL::CommandQueue*>(MetalCommandQueue);

	// 1. Each layer's swapchain image: acquired and waited for. A timed-out wait keeps it acquired for the next frame,
	// and the layer meanwhile shows the image it last released.
	struct FCopy { MTL::Texture* Src; uint32 Id; }; // ids, not pointers: a later EnsureLayerSwapchain may grow the map
	TArray<FCopy, TInlineAllocator<8>> Copies;
	for (const FFrameXRLayer& L : StereoLayers)
	{
		if (!L.Texture || (L.bCylinder && !bCylinderExtension) || !EnsureLayerSwapchain(L.Id, L.Texture))
		{
			continue;
		}
		FLayerSwapchain& Chain = LayerSwapchains[L.Id];
		Chain.LastUsed = LayerFrame;
		XrSwapchainImageAcquireInfo AcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if (!Chain.bPending && XR_CHECK(xrAcquireSwapchainImage(Chain.Handle, &AcquireInfo, &Chain.PendingIndex)))
		{
			Chain.bPending = true;
		}
		if (!Chain.bPending)
		{
			continue;
		}
		XrSwapchainImageWaitInfo ImageWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		ImageWait.timeout = 100000000; // 100 ms
		const XrResult WaitResult = xrWaitSwapchainImage(Chain.Handle, &ImageWait);
		if (WaitResult == XR_TIMEOUT_EXPIRED)
		{
			continue;
		}
		if (!Check(WaitResult, TEXT("xrWaitSwapchainImage (layer)")))
		{
			Chain.bPending = false;
			continue;
		}
		Copies.Add({ static_cast<MTL::Texture*>(L.Texture), L.Id });
	}

	// 2. One blit for all of them, committed before the releases: the runtime snapshots a released image on this
	// same queue, so GPU order puts our copy first (as for the eye image). Every frame, since a widget is live.
	if (Copies.Num() > 0 && Queue)
	{
		MTL::CommandBuffer* Commands = Queue->commandBuffer();
		MTL::BlitCommandEncoder* Blit = Commands->blitCommandEncoder();
		for (const FCopy& C : Copies)
		{
			const FLayerSwapchain& Chain = LayerSwapchains.FindChecked(C.Id);
			MTL::Texture* Dst = static_cast<MTL::Texture*>(Chain.Images[Chain.PendingIndex].texture);
			Blit->copyFromTexture(C.Src, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(Chain.Width, Chain.Height, 1), Dst, 0, 0, MTL::Origin(0, 0, 0));
		}
		Blit->endEncoding();
		Commands->commit();
	}
	for (const FCopy& C : Copies)
	{
		XrSwapchainImageReleaseInfo ReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		FLayerSwapchain& Chain = LayerSwapchains.FindChecked(C.Id);
		if (XR_CHECK(xrReleaseSwapchainImage(Chain.Handle, &ReleaseInfo)))
		{
			Chain.bReleased = true;
		}
		Chain.bPending = false;
	}

	// 3. The headers, in the caller's order (lowest priority first), for every layer with a released image.
	FString Shape, Where;
	for (const FFrameXRLayer& L : StereoLayers)
	{
		const FLayerSwapchain* Chain = LayerSwapchains.Find(L.Id);
		if (!Chain || !Chain->bReleased || Chain->LastUsed != LayerFrame)
		{
			continue;
		}
		XrSwapchainSubImage Sub{};
		Sub.swapchain = Chain->Handle;
		Sub.imageArrayIndex = 0;
		Sub.imageRect.offset = { (int32)(L.UVRect.Min.X * Chain->Width), (int32)(L.UVRect.Min.Y * Chain->Height) };
		Sub.imageRect.extent = { (int32)((L.UVRect.Max.X - L.UVRect.Min.X) * Chain->Width), (int32)((L.UVRect.Max.Y - L.UVRect.Min.Y) * Chain->Height) };
		const XrCompositionLayerFlags Flags = L.bAlpha ? (XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) : 0;
		const XrSpace LayerSpace = L.bFaceLocked && ViewSpace ? ViewSpace : BaseSpace;
		FFrameXRLayerHeader& H = Out.AddZeroed_GetRef();
		if (L.bCylinder)
		{
			H.Cylinder.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
			H.Cylinder.layerFlags = Flags;
			H.Cylinder.space = LayerSpace;
			H.Cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			H.Cylinder.subImage = Sub;
			H.Cylinder.pose = L.Pose;
			H.Cylinder.radius = L.Radius;
			H.Cylinder.centralAngle = L.CentralAngle;
			H.Cylinder.aspectRatio = L.AspectRatio;
			Shape += FString::Printf(TEXT(" %u:cylinder r %.2f m %.0f deg aspect %.2f %ux%u%s"), L.Id, L.Radius, FMath::RadiansToDegrees(L.CentralAngle), L.AspectRatio,
				Chain->Width, Chain->Height, L.bFaceLocked ? TEXT(" view-locked") : TEXT(""));
		}
		else
		{
			H.Quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			H.Quad.layerFlags = Flags;
			H.Quad.space = LayerSpace;
			H.Quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			H.Quad.subImage = Sub;
			H.Quad.pose = L.Pose;
			H.Quad.size = L.Size;
			Shape += FString::Printf(TEXT(" %u:quad %.2fx%.2f m %ux%u%s"), L.Id, L.Size.width, L.Size.height,
				Chain->Width, Chain->Height, L.bFaceLocked ? TEXT(" view-locked") : TEXT(""));
		}
		Where += FString::Printf(TEXT(" %u at (%.2f %.2f %.2f)"), L.Id, L.Pose.position.x, L.Pose.position.y, L.Pose.position.z);
	}
	LayerHeadersSubmitted += Out.Num();

	// Layers gone from the scene give their swapchains back.
	for (auto It = LayerSwapchains.CreateIterator(); It; ++It)
	{
		if (It->Value.LastUsed != LayerFrame)
		{
			UE_LOG(LogFrameXR, Log, TEXT("Stereo layer %u: gone, swapchain destroyed"), It->Key);
			if (It->Value.Handle) { xrDestroySwapchain(It->Value.Handle); }
			It.RemoveCurrent();
		}
	}
	// Logged when the set changes (not per frame): the shapes, and where they were in the base space then (metres).
	if (Shape.IsEmpty())
	{
		Shape = TEXT(" none");
	}
	if (!LastLayerSummary.Equals(Shape))
	{
		UE_LOG(LogFrameXR, Log, TEXT("Stereo layers after the projection layer (%d, %llu headers so far):%s;%s"), Out.Num(), (unsigned long long)LayerHeadersSubmitted, *Shape, *Where);
		LastLayerSummary = Shape;
	}
}
