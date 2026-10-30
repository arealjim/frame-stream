// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#pragma once

#include "FrameXRCommon.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include <atomic>

/** Pre-instance entry points (resolved with a null instance). */
#define FRAMEXR_PRE_INSTANCE_FUNCTIONS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)

/** Entry points resolved once the instance exists. */
#define FRAMEXR_INSTANCE_FUNCTIONS(X) \
	X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
	X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) \
	X(xrEndSession) X(xrRequestExitSession) X(xrPollEvent) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
	X(xrLocateViews) X(xrLocateSpace) X(xrCreateReferenceSpace) X(xrDestroySpace) \
	X(xrEnumerateReferenceSpaces) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
	X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
	X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrGetCurrentInteractionProfile) \
	X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
	X(xrGetActionStatePose) X(xrCreateActionSpace) X(xrResultToString) \
	X(xrGetMetalGraphicsRequirementsKHR)

/** One controller, as the last xrSyncActions saw it. Poses are in the base reference space. */
struct FFrameXRController
{
	bool bActive = false;
	bool bGripValid = false;
	bool bAimValid = false;
	XrPosef Grip{};
	XrPosef Aim{};
	float Trigger = 0.f;
	float Squeeze = 0.f;
	FVector2f Stick = FVector2f::ZeroVector;
	bool bPrimaryClick = false;   // X / A
	bool bSecondaryClick = false; // Y / B
	bool bPrimaryTouch = false;
	bool bSecondaryTouch = false;
	bool bMenu = false;
	bool bStickClick = false;
	bool bStickTouch = false;
	bool bTriggerTouch = false;
	bool bThumbRestTouch = false;
	// The Steam Frame controller's own inputs (bound only when the runtime has XR_VALVE_frame_controller_interaction).
	// The Touch fields above keep SteamVR's Frame-to-Touch meaning: right B = B, X or Y; left X = d-pad down, Y = the rest.
	bool bFrameProfile = false;   // the runtime reports the Frame profile for this hand
	bool bFrameX = false;         // right
	bool bFrameY = false;         // right
	bool bFrameRightMenu = false; // right
	bool bDpad[4] = {};           // left: up, down, left, right
	bool bBumper = false;
};

/** The two views and the head, located in a base reference space at Time; submitted with the frame as is. */
struct FFrameXRViews
{
	bool bValid = false;
	/** False during an orientation-only dropout: the views still locate and are submitted; the head position is the runtime's last. */
	bool bPositionValid = false;
	XrTime Time = 0;
	XrPosef Head{};
	XrView Views[2]{};
};

/**
 * A Stereo Layer component as an OpenXR quad or cylinder layer, submitted after the projection layer (see
 * FFrameXRStereoLayers). Its pose is in the projection layer's reference space, or in VIEW space if face-locked.
 */
struct FFrameXRLayer
{
	uint32 Id = 0;
	int32 Priority = 0;
	bool bCylinder = false;
	bool bFaceLocked = false;
	bool bAlpha = true;
	XrPosef Pose{};
	XrExtent2Df Size{};                                      // quad, metres
	float Radius = 1.f, CentralAngle = 1.f, AspectRatio = 1.f; // cylinder
	FBox2f UVRect = FBox2f(FVector2f(0.f, 0.f), FVector2f(1.f, 1.f));
	/** The layer's texture (an MTL::Texture*), set on the submission thread. */
	void* Texture = nullptr;
};

/** One quad or cylinder header in xrEndFrame's layer list. */
union FFrameXRLayerHeader
{
	XrCompositionLayerBaseHeader Base;
	XrCompositionLayerQuad Quad;
	XrCompositionLayerCylinderKHR Cylinder;
};

/**
 * The OpenXR side of FrameXR: the loader (dlopen), instance, system, session on the engine's Metal command
 * queue, reference spaces, one side-by-side swapchain, the action set, and the frame loop.
 *
 * Threads: the instance and system are made on the game thread; the session is made lazily on the Metal
 * submission thread at the first present (that's where the queue is in hand); PollEvents / LocateViews /
 * SyncActions run on the game thread (LocateViews also on the render thread for the late update);
 * WaitFrame_GameThread runs xrWaitFrame at the top of each game frame (Epic's pipelined shape): the game thread
 * is what the runtime paces, and the views are located at the predicted display time that frame will be
 * stamped with. SubmitFrame runs begin-acquire-blit-release-end on the submission thread for that same time.
 * The session's objects are made, used and destroyed under SessionLock; a session the runtime exits or loses is
 * destroyed on the game thread and a new one is made at the next present.
 */
class FFrameXRSession
{
public:
	static FFrameXRSession& Get();

	/** dlopen's the loader and resolves xrGetInstanceProcAddr. Safe to call repeatedly. */
	bool LoadLoader();
	bool IsLoaderLoaded() const { return xrGetInstanceProcAddr != nullptr; }
	const FString& GetLoaderPath() const { return LoaderPath; }

	/** Instance with XR_KHR_metal_enable, the HMD system and its stereo view sizes. Idempotent. */
	bool CreateInstance();
	bool HasInstance() const { return Instance != XR_NULL_HANDLE; }
	const FString& GetRuntimeName() const { return RuntimeName; }
	FIntPoint GetEyeSize() const { return EyeSize; }

	/** Session bound to MetalCommandQueue (an MTL::CommandQueue*), spaces, swapchain and actions. */
	bool CreateSession(void* MetalCommandQueue);
	bool HasSession() const { return Session != XR_NULL_HANDLE; }
	/** xrBeginSession has happened and no stop is pending. */
	bool IsRunning() const { return bRunning.load(); }
	void Destroy();

	// Game thread.
	void PollEvents();
	/** xrWaitFrame for this game frame: the predicted display time to locate views at and to stamp the frame with; 0 if not running. */
	XrTime WaitFrame_GameThread();
	/** The display time the next frame will most likely get (last predicted + period); 0 before the first frame. */
	XrTime EstimateDisplayTime() const;
	float GetDisplayPeriodMs() const;
	bool LocateViews(XrTime Time, XrReferenceSpaceType Space, FFrameXRViews& Out) const;
	bool SyncActions(XrTime Time, XrReferenceSpaceType Space, FFrameXRController Out[2]) const;
	/** True if a space of this type resolves (possibly to its fallback). */
	bool HasSpace(XrReferenceSpaceType Space) const { return SpaceFor(Space) != XR_NULL_HANDLE; }
	/** True only if the runtime made a space of exactly this type. */
	bool HasExactSpace(XrReferenceSpaceType Space) const
	{
		switch (Space)
		{
		case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: return LocalFloorSpace != XR_NULL_HANDLE;
		case XR_REFERENCE_SPACE_TYPE_STAGE: return StageSpace != XR_NULL_HANDLE;
		default: return SpaceFor(Space) != XR_NULL_HANDLE;
		}
	}

	// Submission thread. SrcMetalTexture is an MTL::Texture* holding both eyes side by side (may be null); Views.Time
	// names the game frame's wait (its predicted display time) and so the frame state to begin and end. Layers go
	// after the projection layer, lowest priority first, each copied into a swapchain of its own.
	bool SubmitFrame(void* MetalCommandQueue, void* SrcMetalTexture, const FFrameXRViews& Views, XrReferenceSpaceType Space, TArrayView<const FFrameXRLayer> Layers = {});

	int64 GetSwapchainFormat() const { return SwapchainFormat; }
	/** The runtime offered XR_KHR_composition_layer_cylinder (and it is enabled). */
	bool SupportsCylinderLayers() const { return bCylinderExtension; }

private:
	FFrameXRSession() = default;
	static FString FindLoader();
	bool Check(XrResult Result, const TCHAR* Call) const;
	XrSpace SpaceFor(XrReferenceSpaceType Space) const;
	bool CreateSpaces();
	bool CreateSwapchain();
	bool CreateActions();
	void HandleSessionState(XrSessionState NewState);
	/** Destroys the session and what it owns (spaces, swapchain, action spaces); the instance and the action set stay. */
	void DestroySessionObjects();
	void LogInteractionProfiles() const;
	/** Copies each layer's texture into its swapchain and appends its header (submission thread, under SessionLock). */
	void AddLayers(void* MetalCommandQueue, TArrayView<const FFrameXRLayer> Layers, XrSpace BaseSpace, TArray<FFrameXRLayerHeader, TInlineAllocator<8>>& Out);
	/** The layer's swapchain, (re)made to the texture's size and format; null if the runtime cannot take that format. */
	bool EnsureLayerSwapchain(uint32 Id, void* SrcMetalTexture);
	void DestroyLayerSwapchains();

	bool bLoaderAttempted = false;
	void* LoaderHandle = nullptr;
	FString LoaderPath;

	PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define FRAMEXR_DECLARE(Name) PFN_##Name Name = nullptr;
	FRAMEXR_PRE_INSTANCE_FUNCTIONS(FRAMEXR_DECLARE)
	FRAMEXR_INSTANCE_FUNCTIONS(FRAMEXR_DECLARE)
#undef FRAMEXR_DECLARE

	bool bInstanceAttempted = false;
	XrInstance Instance = XR_NULL_HANDLE;
	XrSystemId SystemId = XR_NULL_SYSTEM_ID;
	FString RuntimeName;
	XrViewConfigurationView ViewConfigs[2]{};
	FIntPoint EyeSize = FIntPoint(1808, 1920);

	/** Made on the submission thread, read on the game and render threads. */
	std::atomic<XrSession> Session{ XR_NULL_HANDLE };
	XrSessionState SessionState = XR_SESSION_STATE_UNKNOWN; // game thread
	std::atomic<bool> bRunning{ false };
	std::atomic<bool> bLost{ false };
	/** Set by EXITING / LOSS_PENDING; the game thread then destroys the session objects, and the next present makes a new session. */
	std::atomic<bool> bSessionDestroyPending{ false };
	/** Serialises the session objects across CreateSession / SubmitFrame (submission thread), LocateViews (game and render threads) and DestroySessionObjects (game thread). Recursive. */
	mutable FCriticalSection SessionLock;

	XrSpace ViewSpace = XR_NULL_HANDLE;
	XrSpace LocalSpace = XR_NULL_HANDLE;
	XrSpace LocalFloorSpace = XR_NULL_HANDLE;
	XrSpace StageSpace = XR_NULL_HANDLE;

	XrSwapchain Swapchain = XR_NULL_HANDLE;
	int64 SwapchainFormat = 0;
	TArray<int64> SwapchainFormats; // what the runtime offers
	TArray<XrSwapchainImageMetalKHR> SwapchainImages;

	// Stereo layers (submission thread, under SessionLock): one swapchain per layer id, the source texture's size and format.
	struct FLayerSwapchain
	{
		XrSwapchain Handle = XR_NULL_HANDLE;
		TArray<XrSwapchainImageMetalKHR> Images;
		uint32 Width = 0, Height = 0;
		int64 Format = 0;
		bool bReleased = false; // an image has been written and released: the layer can be submitted
		bool bPending = false;  // acquired, its wait timed out: waited for again next frame
		uint32 PendingIndex = 0;
		uint64 LastUsed = 0;
	};
	TMap<uint32, FLayerSwapchain> LayerSwapchains;
	TSet<int64> RefusedLayerFormats;
	bool bCylinderExtension = false;
	uint64 LayerFrame = 0;
	uint64 LayerHeadersSubmitted = 0;
	FString LastLayerSummary;

	std::atomic<int64> LastDisplayTime{ 0 };
	std::atomic<int64> LastDisplayPeriod{ 0 };
	/** Frame states from the game thread's waits, consumed in order by the submission thread. */
	struct FWaitedFrame { XrTime Time = 0; XrDuration Period = 0; bool bShouldRender = true; };
	TQueue<FWaitedFrame, EQueueMode::Spsc> WaitedFrames;
	std::atomic<int64> StatWaitMicros{ 0 }; // game thread adds, submission thread reads and clears
	uint64 FramesSubmitted = 0;
	uint64 FramesSkipped = 0;
	// A swapchain image acquired whose xrWaitSwapchainImage timed out: still acquired, waited for again next frame (submission thread).
	bool bImagePending = false;
	uint32 PendingImageIndex = 0;
	uint64 ImageWaitTimeouts = 0;
	// Frame loop timing, logged every 300 frames (submission thread only).
	int32 StatFrames = 0;
	double StatWait = 0.0, StatBlit = 0.0, StatTotal = 0.0, StatGap = 0.0, LastSubmitEnd = 0.0, StatQueueAhead = 0.0, StatBlitGpu = 0.0;

	// Input.
	XrActionSet ActionSet = XR_NULL_HANDLE;
	bool bActionsAttached = false; // to the current session
	XrPath HandPaths[2]{};
	XrAction GripPoseAction = XR_NULL_HANDLE;
	XrAction AimPoseAction = XR_NULL_HANDLE;
	XrAction TriggerAction = XR_NULL_HANDLE;
	XrAction TriggerTouchAction = XR_NULL_HANDLE;
	XrAction SqueezeAction = XR_NULL_HANDLE;
	XrAction StickAction = XR_NULL_HANDLE;
	XrAction StickClickAction = XR_NULL_HANDLE;
	XrAction StickTouchAction = XR_NULL_HANDLE;
	XrAction PrimaryClickAction = XR_NULL_HANDLE;
	XrAction SecondaryClickAction = XR_NULL_HANDLE;
	XrAction PrimaryTouchAction = XR_NULL_HANDLE;
	XrAction SecondaryTouchAction = XR_NULL_HANDLE;
	XrAction MenuAction = XR_NULL_HANDLE;
	XrAction ThumbRestAction = XR_NULL_HANDLE;
	XrAction FrameXAction = XR_NULL_HANDLE;
	XrAction FrameYAction = XR_NULL_HANDLE;
	XrAction FrameMenuAction = XR_NULL_HANDLE;
	XrAction DpadActions[4] = {};
	XrAction BumperAction = XR_NULL_HANDLE;
	bool bFrameControllerExtension = false;
	XrPath FrameProfilePath = XR_NULL_PATH;
	mutable std::atomic<bool> bFrameProfileActive[2] = {}; // written on profile change (events), read by SyncActions
	XrSpace GripSpaces[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrSpace AimSpaces[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
};
