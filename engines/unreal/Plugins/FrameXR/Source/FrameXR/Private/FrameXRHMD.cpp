// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#include "FrameXRHMD.h"

#include "DefaultSpectatorScreenController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "FrameXRRenderBridge.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/App.h"
#include "RenderGraphBuilder.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "Engine/Texture.h"
#include "StereoLayerShapes.h"
#include "TextureResource.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SWindow.h"

const FName FFrameXRHMD::SystemName(TEXT("FrameXR"));

namespace
{
	constexpr int32 NumBufferedTargets = 3;
	constexpr EHMDTrackingOrigin::Type DefaultTrackingOrigin = EHMDTrackingOrigin::LocalFloor;

	int32 EyeIndex(int32 ViewIndex)
	{
		return ViewIndex == EStereoscopicEye::eSSE_RIGHT_EYE ? 1 : 0;
	}
}

void FFrameXRTrackingFrame::ToUnreal(const FXrPose& Pose, FQuat& OutOrientation, FVector& OutPosition) const
{
	const FXrPose Relative = Origin.Inverse() * Pose;
	OutOrientation = FrameXR::ToUnrealRotation(Relative.Orientation);
	OutPosition = FrameXR::ToUnrealPosition(Relative.Position, WorldToMeters);
}

FFrameXRHMD::FFrameXRHMD()
	: FHeadMountedDisplayBase(nullptr)
	, TrackingOrigin(DefaultTrackingOrigin)
{
	SpectatorScreenController = MakeUnique<FDefaultSpectatorScreenController>(this);

	if (RHIGetInterfaceType() == ERHIInterfaceType::Metal)
	{
		RenderBridge = new FFrameXRRenderBridge();
	}
	else
	{
		UE_LOG(LogFrameXR, Warning, TEXT("The RHI isn't Metal; FrameXR can't make an OpenXR session"));
	}

	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FFrameXRHMD::OnBackBufferReadyToPresent);
	}

	GameFrame.EyeSize = FFrameXRSession::Get().GetEyeSize();
	GameFrame.Space = SpaceForOrigin();
	UpdateGameFrame();
	RenderFrame = GameFrame;
}

FFrameXRHMD::~FFrameXRHMD()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().RemoveAll(this);
	}
	if (IsInGameThread())
	{
		FlushRenderingCommands(); // No late update or present is in flight when the session goes.
	}
	FFrameXRSession::Get().Destroy();
}

FString FFrameXRHMD::GetVersionString() const
{
	return FString::Printf(TEXT("FrameXR - %s"), *FFrameXRSession::Get().GetRuntimeName());
}

// ---------------------------------------------------------------------------------------------------------------
// Tracking

const FFrameXRTrackingFrame& FFrameXRHMD::CurrentFrame() const
{
	return IsInRenderingThread() ? RenderFrame : GameFrame;
}

bool FFrameXRHMD::IsFloorOrigin() const
{
	return TrackingOrigin == EHMDTrackingOrigin::LocalFloor || TrackingOrigin == EHMDTrackingOrigin::Stage;
}

XrReferenceSpaceType FFrameXRHMD::SpaceForOrigin() const
{
	switch (TrackingOrigin)
	{
	case EHMDTrackingOrigin::Stage: return XR_REFERENCE_SPACE_TYPE_STAGE;
	case EHMDTrackingOrigin::LocalFloor: return XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
	default: return XR_REFERENCE_SPACE_TYPE_LOCAL;
	}
}

void FFrameXRHMD::FillFromViews(FFrameXRTrackingFrame& Frame)
{
	Frame.bHeadValid = Frame.Raw.bValid; // Orientation-only frames count: the camera follows the head, and the layer is stamped with the same views.
	if (!Frame.Raw.bValid)
	{
		return;
	}
	Frame.Head = FXrPose(Frame.Raw.Head);
	const FXrPose HeadInverse = Frame.Head.Inverse();
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		const XrView& View = Frame.Raw.Views[Eye];
		Frame.Eyes[Eye] = HeadInverse * FXrPose(View.pose);
		Frame.FovTangents[Eye][0] = FMath::Tan(View.fov.angleLeft);
		Frame.FovTangents[Eye][1] = FMath::Tan(View.fov.angleRight);
		Frame.FovTangents[Eye][2] = FMath::Tan(View.fov.angleUp);
		Frame.FovTangents[Eye][3] = FMath::Tan(View.fov.angleDown);
	}
	const float Ipd = (FXrPose(Frame.Raw.Views[1].pose).Position - FXrPose(Frame.Raw.Views[0].pose).Position).Size();
	if (Ipd > 0.04f && Ipd < 0.09f)
	{
		Frame.Ipd = Ipd;
	}
}

void FFrameXRHMD::UpdateGameFrame()
{
	check(IsInGameThread());
	FFrameXRSession& Session = FFrameXRSession::Get();
	Session.PollEvents();
	const bool bRunning = Session.IsRunning();

	if (bRunning != bWasRunning)
	{
		bWasRunning = bRunning;
		if (bRunning)
		{
			UE_LOG(LogFrameXR, Log, TEXT("Session running: %dx%d per eye"), GameFrame.EyeSize.X, GameFrame.EyeSize.Y);
			LocalOrigin = FXrPose(); // A floor origin resolves to STAGE on OXRSys (no LOCAL_FLOOR): the room's forward, not the wearer's; the app recentres.
			UE_LOG(LogFrameXR, Log, TEXT("Floor origin space: %s"), Session.HasExactSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) ? TEXT("LOCAL_FLOOR") : (Session.HasExactSpace(XR_REFERENCE_SPACE_TYPE_STAGE) ? TEXT("STAGE (LOCAL_FLOOR fallback)") : TEXT("LOCAL (no floor space!)")));
		}
		else
		{
			UE_LOG(LogFrameXR, Log, TEXT("Session stopped"));
		}
	}

	FFrameXRTrackingFrame Frame = GameFrame;
	Frame.bRunning = bRunning;
	Frame.Space = SpaceForOrigin();
	Frame.EyeSize = Session.GetEyeSize();
	// This frame's wait: the runtime paces the game thread here, and the predicted display time it returns is
	// what the views are located at now, late-updated to on the render thread, and stamped with at submit.
	// One xrWaitFrame per submitted frame, from here only: with stereo off nothing is presented, so no wait is
	// taken (a wait with no begin/end after it stalls a conformant runtime) and the views are located at the
	// estimated display time instead.
	const bool bWillSubmit = bRunning && bStereoEnabled;
	const XrTime Time = bWillSubmit ? Session.WaitFrame_GameThread() : (bRunning ? Session.EstimateDisplayTime() : 0);
	if (bRunning && Time > 0)
	{
		Session.LocateViews(Time, Frame.Space, Frame.Raw);
		FillFromViews(Frame);
		Session.SyncActions(Time, Frame.Space, Frame.Controllers);
	}
	else
	{
		Frame.Raw.bValid = false;
		Frame.bHeadValid = false;
		Frame.Controllers[0] = Frame.Controllers[1] = FFrameXRController();
	}

	if (PendingRecenter != ERecenter::None && Frame.bHeadValid && Frame.Raw.bPositionValid) // A recentre needs a trusted position.
	{
		const FQuat Heading = FrameXR::YawOnly(Frame.Head.Orientation, FMath::DegreesToRadians(PendingYaw));
		if (PendingRecenter != ERecenter::Position)
		{
			LocalOrigin.Orientation = Heading;
		}
		if (PendingRecenter != ERecenter::Orientation)
		{
			LocalOrigin.Position = Frame.Head.Position;
			if (IsFloorOrigin())
			{
				LocalOrigin.Position.Y = 0.0; // Keep the floor where the runtime put it.
			}
		}
		PendingRecenter = ERecenter::None;
		OnTrackingOriginChanged();
	}

	Frame.Origin = LocalOrigin;
	Frame.WorldToMeters = WorldToMeters;
	GameFrame = Frame;
}

bool FFrameXRHMD::OnStartGameFrame(FWorldContext& WorldContext)
{
	if (const UWorld* World = WorldContext.World())
	{
		if (const AWorldSettings* Settings = World->GetWorldSettings())
		{
			WorldToMeters = Settings->WorldToMeters;
		}
	}
	RefreshTrackingToWorldTransform(WorldContext);
	UpdateGameFrame();
	return true;
}

bool FFrameXRHMD::HasValidTrackingPosition()
{
	return CurrentFrame().bHeadValid && CurrentFrame().Raw.bPositionValid;
}

bool FFrameXRHMD::EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type)
{
	if (Type == EXRTrackedDeviceType::Any || Type == EXRTrackedDeviceType::HeadMountedDisplay)
	{
		OutDevices.Add(IXRTrackingSystem::HMDDeviceId);
		return true;
	}
	return false;
}

bool FFrameXRHMD::GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition)
{
	OutOrientation = FQuat::Identity;
	OutPosition = FVector::ZeroVector;
	const FFrameXRTrackingFrame& Frame = CurrentFrame();
	if (DeviceId != IXRTrackingSystem::HMDDeviceId || !Frame.bHeadValid)
	{
		return false;
	}
	Frame.ToUnreal(Frame.Head, OutOrientation, OutPosition);
	return true;
}

const FFrameXRController& FFrameXRHMD::GetController(int32 Hand) const
{
	return CurrentFrame().Controllers[FMath::Clamp(Hand, 0, 1)];
}

bool FFrameXRHMD::GetControllerPose(int32 Hand, bool bAim, float InWorldToMeters, FQuat& OutOrientation, FVector& OutPosition) const
{
	const FFrameXRTrackingFrame& Frame = CurrentFrame();
	if (Hand < 0 || Hand > 1 || !Frame.bHeadValid)
	{
		return false;
	}
	const FFrameXRController& C = Frame.Controllers[Hand];
	if (!(bAim ? C.bAimValid : C.bGripValid))
	{
		return false;
	}
	const FXrPose Relative = Frame.Origin.Inverse() * FXrPose(bAim ? C.Aim : C.Grip);
	OutOrientation = FrameXR::ToUnrealRotation(Relative.Orientation);
	OutPosition = FrameXR::ToUnrealPosition(Relative.Position, InWorldToMeters);
	return true;
}

bool FFrameXRHMD::IsControllerTracked(int32 Hand) const
{
	return Hand >= 0 && Hand < 2 && CurrentFrame().Controllers[Hand].bGripValid;
}

bool FFrameXRHMD::GetRelativeEyePose(int32 DeviceId, int32 ViewIndex, FQuat& OutOrientation, FVector& OutPosition)
{
	OutOrientation = FQuat::Identity;
	OutPosition = FVector::ZeroVector;
	if (DeviceId != IXRTrackingSystem::HMDDeviceId ||
		(ViewIndex != EStereoscopicEye::eSSE_LEFT_EYE && ViewIndex != EStereoscopicEye::eSSE_RIGHT_EYE))
	{
		return false;
	}
	const FFrameXRTrackingFrame& Frame = CurrentFrame();
	const FXrPose& Eye = Frame.Eyes[EyeIndex(ViewIndex)];
	OutOrientation = FrameXR::ToUnrealRotation(Eye.Orientation);
	OutPosition = FrameXR::ToUnrealPosition(Eye.Position, Frame.WorldToMeters);
	return true;
}

float FFrameXRHMD::GetWorldToMetersScale() const
{
	return CurrentFrame().WorldToMeters;
}

void FFrameXRHMD::SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin)
{
	if (TrackingOrigin != NewOrigin)
	{
		TrackingOrigin = NewOrigin;
		LocalOrigin = FXrPose();
		// No wait here: the current frame is re-located in the new space at the time it was already waited for.
		GameFrame.Space = SpaceForOrigin();
		GameFrame.Origin = LocalOrigin;
		if (GameFrame.bRunning && GameFrame.Raw.Time > 0)
		{
			FFrameXRSession& Session = FFrameXRSession::Get();
			Session.LocateViews(GameFrame.Raw.Time, GameFrame.Space, GameFrame.Raw);
			FillFromViews(GameFrame);
			Session.SyncActions(GameFrame.Raw.Time, GameFrame.Space, GameFrame.Controllers);
		}
		OnTrackingOriginChanged();
	}
}

bool FFrameXRHMD::GetFloorToEyeTrackingTransform(FTransform& OutFloorToEye) const
{
	return false; // The runtime's floor spaces carry the height; nothing to add.
}

void FFrameXRHMD::ResetOrientationAndPosition(float Yaw)
{
	PendingRecenter = ERecenter::Full; // Applied at the next OnStartGameFrame: no wait outside the frame loop.
	PendingYaw = Yaw;
}

void FFrameXRHMD::ResetOrientation(float Yaw)
{
	PendingRecenter = ERecenter::Orientation;
	PendingYaw = Yaw;
}

void FFrameXRHMD::ResetPosition()
{
	PendingRecenter = ERecenter::Position;
	PendingYaw = 0.f;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame pipeline: game thread -> render thread (late update) -> submission thread (present)

void FFrameXRHMD::OnBeginRendering_GameThread(FSceneViewFamily& InViewFamily)
{
	SpectatorScreenController->BeginRenderViewFamily(InViewFamily);
	TArray<FFrameXRLayer> Layers;
	TArray<FTextureResource*> LayerTextures;
	if (NativeLayers.IsValid())
	{
		NativeLayers->Gather(GameFrame, GetTrackingToWorldTransform(), FFrameXRSession::Get().SupportsCylinderLayers(), Layers, LayerTextures);
	}
	ENQUEUE_RENDER_COMMAND(FrameXRBeginFrame)(
		[this, Frame = GameFrame, Texture = AcquiredTexture_GameThread, Layers = MoveTemp(Layers), LayerTextures = MoveTemp(LayerTextures)](FRHICommandListImmediate&) mutable
		{
			RenderFrame = Frame;
			AcquiredTexture_RenderThread = Texture;
			RenderLayers = MoveTemp(Layers);
			RenderLayerTextures = MoveTemp(LayerTextures);
		});
}

void FFrameXRHMD::OnBeginRendering_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	// Late update: the newest views for the same display time, relative to the origin the game thread used.
	if (RenderFrame.bRunning && RenderFrame.Raw.Time > 0)
	{
		FFrameXRViews Views;
		if (FFrameXRSession::Get().LocateViews(RenderFrame.Raw.Time, RenderFrame.Space, Views))
		{
			RenderFrame.Raw = Views;
			FillFromViews(RenderFrame);
		}
	}
	QueueSubmit_RenderThread(RHICmdList);
}

void FFrameXRHMD::QueueSubmit_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());
	if (!RenderBridge.IsValid() || !RenderTextures.IsValidIndex(AcquiredTexture_RenderThread))
	{
		return;
	}
	// Even an unlocated frame is handed on: the session's frame loop must advance for the first display time to exist.
	FFrameXRSubmitFrame Frame;
	Frame.Texture = RenderTextures[AcquiredTexture_RenderThread];
	Frame.Views = RenderFrame.Raw;
	Frame.Space = RenderFrame.Space;
	Frame.Layers = RenderLayers;
	for (FTextureResource* Resource : RenderLayerTextures)
	{
		Frame.LayerTextures.Add(Resource ? Resource->GetTextureRHI() : nullptr);
	}
	RHICmdList.EnqueueLambda(TEXT("FrameXRQueueFrame"),
		[Bridge = RenderBridge, Frame = MoveTemp(Frame)](FRHICommandListImmediate&) mutable
		{
			Bridge->SetFrame_RHIThread(MoveTemp(Frame));
		});
}

void FFrameXRHMD::OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider)
{
	// Hook the present of whichever window shows the stereo viewport (the VR Preview / -vr game window).
	const TSharedPtr<ISlateViewport> Viewport = Window.GetViewport();
	if (RenderBridge.IsValid() && Viewport.IsValid() && Viewport->IsStereoscopic3D())
	{
		ViewportProvider.SetCustomPresent(RenderBridge);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Display

bool FFrameXRHMD::IsHMDConnected()
{
	return FFrameXRSession::Get().HasInstance();
}

void FFrameXRHMD::EnableHMD(bool bEnable)
{
	bHMDEnabled = bEnable;
	if (!bEnable)
	{
		EnableStereo(false);
	}
}

bool FFrameXRHMD::GetHMDMonitorInfo(MonitorInfo& OutInfo)
{
	const FIntPoint Eye = GameFrame.EyeSize;
	OutInfo.MonitorName = TEXT("Steam Frame (OXRSys)");
	OutInfo.MonitorId = 0;
	OutInfo.DesktopX = OutInfo.DesktopY = 0;
	OutInfo.ResolutionX = OutInfo.WindowSizeX = Eye.X * 2;
	OutInfo.ResolutionY = OutInfo.WindowSizeY = Eye.Y;
	return true;
}

void FFrameXRHMD::GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const
{
	const float* T = CurrentFrame().FovTangents[0];
	OutHFOVInDegrees = FMath::RadiansToDegrees(FMath::Atan(T[1]) - FMath::Atan(T[0]));
	OutVFOVInDegrees = FMath::RadiansToDegrees(FMath::Atan(T[2]) - FMath::Atan(T[3]));
}

float FFrameXRHMD::GetInterpupillaryDistance() const
{
	return CurrentFrame().Ipd;
}

void FFrameXRHMD::SetPixelDensity(const float NewDensity)
{
	// The swapchain is the recommended size; a denser eye target would be cropped by the blit, so cap at 1.
	PixelDensity = FMath::Clamp(NewDensity, PixelDensityMin, 1.f);
}

FIntPoint FFrameXRHMD::GetIdealRenderTargetSize() const
{
	const FIntPoint Eye = CurrentFrame().EyeSize;
	return FIntPoint(Eye.X * 2, Eye.Y);
}

bool FFrameXRHMD::EnableStereo(bool bStereo)
{
	bStereo = bStereo && bHMDEnabled;
	if (bStereo == bStereoEnabled)
	{
		return bStereoEnabled;
	}
	bStereoEnabled = bStereo;
	if (bStereo)
	{
		UE_CLOG(!GameFrame.bRunning, LogFrameXR, Log, TEXT("Stereo enabled before the OpenXR session runs; it starts at the first present"));
	}
	UE_LOG(LogFrameXR, Log, TEXT("Stereo %s"), bStereo ? TEXT("enabled") : TEXT("disabled"));
	FApp::SetUseVRFocus(bStereo);
	FApp::SetHasVRFocus(bStereo);
	return bStereoEnabled;
}

void FFrameXRHMD::AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	// Eyes side by side in the render target: left half, right half.
	const FIntPoint Ideal = GetIdealRenderTargetSize();
	const uint32 EyeWidth = FMath::CeilToInt(Ideal.X * PixelDensity) / 2;
	const uint32 EyeHeight = FMath::CeilToInt(Ideal.Y * PixelDensity);
	X = EyeIndex(ViewIndex) * EyeWidth;
	Y = 0;
	SizeX = EyeWidth;
	SizeY = EyeHeight;
}

FMatrix FFrameXRHMD::GetStereoProjectionMatrix(const int32 ViewIndex) const
{
	// Off-axis, reversed-Z, infinite far plane from the eye's signed FOV tangents (left, right, up, down).
	const float* T = CurrentFrame().FovTangents[EyeIndex(ViewIndex)];
	const float Left = T[0];
	const float Right = T[1];
	const float Up = T[2];
	const float Down = T[3];
	const float ZNear = IsInRenderingThread() ? GNearClippingPlane_RenderThread : GNearClippingPlane;

	return FMatrix(
		FPlane(2.f / (Right - Left), 0.f, 0.f, 0.f),
		FPlane(0.f, 2.f / (Up - Down), 0.f, 0.f),
		FPlane(-(Right + Left) / (Right - Left), -(Up + Down) / (Up - Down), 0.f, 1.f),
		FPlane(0.f, 0.f, ZNear, 0.f));
}

// ---------------------------------------------------------------------------------------------------------------
// Spectator (the Mac window)

void FFrameXRHMD::RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer, FRDGTextureRef SrcTexture, FVector2f WindowSize) const
{
	if (SpectatorScreenController)
	{
		SpectatorScreenController->RenderSpectatorScreen_RenderThread(GraphBuilder, BackBuffer, SrcTexture, nullptr, WindowSize);
	}
}

FIntRect FFrameXRHMD::GetFullFlatEyeRect_RenderThread(const FRHITextureDesc& EyeTexture) const
{
	return FIntRect(0, 0, EyeTexture.Extent.X / 2, EyeTexture.Extent.Y);
}

// ---------------------------------------------------------------------------------------------------------------
// Render targets: allocated here so each frame's texture is known when it's presented.

int32 FFrameXRHMD::AcquireColorTexture()
{
	check(IsInGameThread());
	const int32 Count = NumRenderTextures.load();
	if (Count <= 0)
	{
		AcquiredTexture_GameThread = INDEX_NONE;
		return INDEX_NONE;
	}
	AcquiredTexture_GameThread = (AcquiredTexture_GameThread + 1) % Count;
	return AcquiredTexture_GameThread;
}

bool FFrameXRHMD::AllocateRenderTargetTextures(FRHICommandListBase& RHICmdList, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTextureRHIRef>& OutTargetableTextures, TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples)
{
	check(IsInRenderingThread());
	RenderTextures.Reset();
	NumRenderTextures = 0;
	if (NumLayers != 1)
	{
		return false; // Side by side only; let the engine allocate (nothing is sent to the headset).
	}

	// 8-bit BGRA holds the tonemapped (sRGB-encoded) output; the swapchain's sRGB format reads it as such.
	const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("FrameXREyes"), SizeX, SizeY, PF_B8G8R8A8)
		.SetFlags(Flags | TargetableTextureFlags | ETextureCreateFlags::ShaderResource)
		.SetClearValue(FClearValueBinding::Black)
		.SetInitialState(ERHIAccess::SRVMask);

	for (int32 Index = 0; Index < NumBufferedTargets; ++Index)
	{
		FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);
		RenderTextures.Add(Texture);
		OutTargetableTextures.Add(Texture);
		OutShaderResourceTextures.Add(Texture);
	}
	NumRenderTextures = RenderTextures.Num();
	UE_LOG(LogFrameXR, Log, TEXT("Allocated %d eye targets of %ux%u (flags %llx)"), NumBufferedTargets, SizeX, SizeY, (unsigned long long)(Flags | TargetableTextureFlags));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Stereo layers

IStereoLayers* FFrameXRHMD::GetStereoLayers()
{
	// native: each Stereo Layer component is an OpenXR quad or cylinder of its own, composited by the runtime.
	// emulated: the engine's FDefaultStereoLayers draws them into the eye buffer (a cylinder comes out flat).
	// auto (default): native when the runtime offers XR_KHR_composition_layer_cylinder, which only OXRSys's
	// layer-compositing build does; the live build drops quads, so there the eye buffer is the only way they show.
	if (!bLayerModeChosen)
	{
		bLayerModeChosen = true;
		const FString Mode = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEXR_STEREO_LAYERS")).ToLower();
		const bool bCylinders = FFrameXRSession::Get().SupportsCylinderLayers();
		const bool bNative = Mode == TEXT("native") || (Mode != TEXT("emulated") && bCylinders);
		UE_LOG(LogFrameXR, Log, TEXT("Stereo layers: %s (FRAMEXR_STEREO_LAYERS '%s', runtime cylinder layers %s)"),
			bNative ? TEXT("native OpenXR quad/cylinder layers") : TEXT("emulated, drawn into the eye buffer"), *Mode, bCylinders ? TEXT("yes") : TEXT("no"));
		if (bNative)
		{
			NativeLayers = MakeShared<FFrameXRStereoLayers, ESPMode::ThreadSafe>();
		}
	}
	return NativeLayers.IsValid() ? static_cast<IStereoLayers*>(NativeLayers.Get()) : FHeadMountedDisplayBase::GetStereoLayers();
}

void FFrameXRStereoLayers::Gather(const FFrameXRTrackingFrame& Frame, const FTransform& TrackingToWorld, bool bCylinders, TArray<FFrameXRLayer>& OutLayers, TArray<FTextureResource*>& OutTextures)
{
	// The poses as Epic's OpenXR plugin makes them (OpenXRHMD_Layer.cpp): world-locked through the inverse
	// tracking-to-world transform, tracker-locked as given, face-locked in view space; then into the runtime's base
	// space through the recentred origin, which is how the views themselves are submitted.
	const float W = Frame.WorldToMeters;
	const FTransform InvTrackingToWorld = TrackingToWorld.Inverse();
	TArray<TPair<FFrameXRLayer, FTextureResource*>, TInlineAllocator<8>> Found;
	ForEachLayer([&](uint32 Id, FLayerDesc& Desc)
	{
		if (!Desc.IsVisible())
		{
			return;
		}
		const bool bQuad = Desc.HasShape<FQuadLayer>();
		const bool bCylinder = Desc.HasShape<FCylinderLayer>();
		if (!bQuad && !(bCylinder && bCylinders))
		{
			UE_CLOG(!Skipped.Contains(Id), LogFrameXR, Warning, TEXT("Stereo layer %u: %s is not sent (quads%s only)"), Id,
				bCylinder ? TEXT("a cylinder") : TEXT("its shape (equirect or cubemap)"), bCylinders ? TEXT(" and cylinders") : TEXT(""));
			Skipped.Add(Id);
			return;
		}
		FFrameXRLayer L;
		L.Id = Id;
		L.Priority = Desc.Priority;
		L.bCylinder = bCylinder;
		L.bFaceLocked = Desc.PositionType == IStereoLayers::FaceLocked;
		L.bAlpha = !(Desc.Flags & IStereoLayers::LAYER_FLAG_TEX_NO_ALPHA_CHANNEL);
		L.UVRect = FBox2f(FVector2f(Desc.UVRect.Min), FVector2f(Desc.UVRect.Max));
		const FTransform T = Desc.PositionType == IStereoLayers::WorldLocked ? Desc.Transform * InvTrackingToWorld : Desc.Transform;
		FXrPose Pose(FrameXR::FromUnrealRotation(T.GetRotation()), FrameXR::FromUnrealPosition(T.GetLocation(), W));
		if (!L.bFaceLocked)
		{
			Pose = Frame.Origin * Pose;
		}
		L.Pose = Pose.ToXr();
		if (bQuad)
		{
			FVector2D Size = Desc.QuadSize;
			UTexture* Texture = Desc.TextureObj.Get();
			if ((Desc.Flags & IStereoLayers::LAYER_FLAG_QUAD_PRESERVE_TEX_RATIO) && Texture && Texture->GetSurfaceWidth() > 0)
			{
				Size.Y = Size.X * Texture->GetSurfaceHeight() / Texture->GetSurfaceWidth();
			}
			const FVector Scale = T.GetScale3D();
			L.Size = { (float)(Size.X * Scale.Y / W), (float)(Size.Y * Scale.Z / W) };
		}
		else
		{
			const FCylinderLayer& C = Desc.GetShape<FCylinderLayer>();
			L.Radius = FMath::Abs(C.Radius / W);
			L.CentralAngle = FMath::Min(2.f * PI, FMath::Abs(C.OverlayArc / C.Radius));
			L.AspectRatio = FMath::Abs(C.OverlayArc / C.Height);
		}
		Found.Add({ L, Desc.TextureObj->GetResource() });
	});
	Found.StableSort([](const TPair<FFrameXRLayer, FTextureResource*>& A, const TPair<FFrameXRLayer, FTextureResource*>& B) { return A.Key.Priority < B.Key.Priority; });
	for (const TPair<FFrameXRLayer, FTextureResource*>& P : Found)
	{
		OutLayers.Add(P.Key);
		OutTextures.Add(P.Value);
	}
}
