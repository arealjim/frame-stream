// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#pragma once

#include "FrameXRCommon.h"
#include "FrameXRSession.h"
#include "HeadMountedDisplayBase.h"
#include "StereoLayerManager.h"
#include "XRRenderTargetManager.h"

class FFrameXRRenderBridge;
class FTextureResource;
class ISlateViewportProvider;
class SWindow;

/** Everything one frame (game or render thread) needs about the headset. */
struct FFrameXRTrackingFrame
{
	bool bRunning = false;
	bool bHeadValid = false;
	/** The views and head as the runtime located them in Space (what the frame is submitted with). */
	FFrameXRViews Raw;
	XrReferenceSpaceType Space = XR_REFERENCE_SPACE_TYPE_LOCAL;
	FXrPose Head;
	/** Each eye relative to the head (canting included). */
	FXrPose Eyes[2];
	FFrameXRController Controllers[2];
	/** Tracking-space origin the engine sees: the recentred heading/position. */
	FXrPose Origin;
	float WorldToMeters = 100.f;
	float Ipd = 0.063f;
	/** Per eye: signed tangents left, right, up, down (left and down negative), as XrFovf gives them. */
	float FovTangents[2][4] = { { -1.376f, 0.839f, 0.966f, -1.428f }, { -0.839f, 1.376f, 0.966f, -1.428f } };
	FIntPoint EyeSize = FIntPoint(1808, 1920);

	/** A tracked pose relative to Origin, in Unreal tracking space. */
	void ToUnreal(const FXrPose& Pose, FQuat& OutOrientation, FVector& OutPosition) const;
};

/**
 * Stereo Layer components as OpenXR quad and cylinder layers of their own (FRAMEXR_STEREO_LAYERS=native), instead
 * of the engine's default of drawing them into the eye buffer. Game thread, like the manager it extends.
 */
class FFrameXRStereoLayers : public FSimpleLayerManager
{
public:
	/** This frame's visible layers, lowest priority first, posed in Frame's base space (face-locked: view space). */
	void Gather(const FFrameXRTrackingFrame& Frame, const FTransform& TrackingToWorld, bool bCylinders, TArray<FFrameXRLayer>& OutLayers, TArray<FTextureResource*>& OutTextures);
	virtual FString GetReferencerName() const override { return TEXT("FFrameXRStereoLayers"); }

private:
	TSet<uint32> Skipped; // layers whose shape is not sent, warned about once each
};

/**
 * The Steam Frame through OXRSys: an XRBase head-mounted display that renders both eyes side by side into its
 * own render targets and hands each finished frame to the OpenXR session (see FFrameXRRenderBridge).
 */
class FFrameXRHMD
	: public FHeadMountedDisplayBase
	, public FXRRenderTargetManager
	, public TSharedFromThis<FFrameXRHMD, ESPMode::ThreadSafe>
{
public:
	static const FName SystemName;

	FFrameXRHMD();
	virtual ~FFrameXRHMD() override;

	/** Pose of a controller's grip (or aim) in Unreal tracking space; picks the game or render thread frame. */
	bool GetControllerPose(int32 Hand, bool bAim, float WorldToMeters, FQuat& OutOrientation, FVector& OutPosition) const;
	bool IsControllerTracked(int32 Hand) const;
	const FFrameXRController& GetController(int32 Hand) const;
	bool IsRunning() const { return GameFrame.bRunning; }

	// IXRSystemIdentifier / IXRTrackingSystem
	virtual FName GetSystemName() const override { return SystemName; }
	virtual int32 GetXRSystemFlags() const override { return EXRSystemFlags::IsHeadMounted; }
	virtual FString GetVersionString() const override;
	virtual bool DoesSupportPositionalTracking() const override { return true; }
	virtual bool HasValidTrackingPosition() override;
	virtual bool EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type = EXRTrackedDeviceType::Any) override;
	virtual bool GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual bool GetRelativeEyePose(int32 DeviceId, int32 ViewIndex, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual float GetWorldToMetersScale() const override;
	virtual void SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin) override;
	virtual EHMDTrackingOrigin::Type GetTrackingOrigin() const override { return TrackingOrigin; }
	virtual bool GetFloorToEyeTrackingTransform(FTransform& OutFloorToEye) const override;
	virtual void ResetOrientationAndPosition(float Yaw = 0.f) override;
	virtual void ResetOrientation(float Yaw = 0.f) override;
	virtual void ResetPosition() override;
	virtual IHeadMountedDisplay* GetHMDDevice() override { return this; }
	virtual TSharedPtr<IStereoRendering, ESPMode::ThreadSafe> GetStereoRenderingDevice() override { return SharedThis(this); }
	virtual bool OnStartGameFrame(FWorldContext& WorldContext) override;
	virtual void OnBeginRendering_GameThread(FSceneViewFamily& InViewFamily) override;
	virtual void OnBeginRendering_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily) override;

	// IHeadMountedDisplay
	virtual bool IsHMDConnected() override;
	virtual bool IsHMDEnabled() const override { return bHMDEnabled; }
	virtual void EnableHMD(bool bEnable = true) override;
	virtual bool GetHMDMonitorInfo(MonitorInfo& OutInfo) override;
	virtual void GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const override;
	virtual void SetInterpupillaryDistance(float NewInterpupillaryDistance) override {}
	virtual float GetInterpupillaryDistance() const override;
	virtual bool IsChromaAbCorrectionEnabled() const override { return false; }
	virtual float GetPixelDenity() const override { return PixelDensity; }
	virtual void SetPixelDensity(const float NewDensity) override;
	virtual FIntPoint GetIdealRenderTargetSize() const override;

	// IStereoRendering
	virtual bool IsStereoEnabled() const override { return bStereoEnabled; }
	virtual bool EnableStereo(bool bStereo = true) override;
	virtual void AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const override;
	virtual FMatrix GetStereoProjectionMatrix(const int32 ViewIndex) const override;
	virtual IStereoRenderTargetManager* GetRenderTargetManager() override { return this; }
	virtual void RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer, FRDGTextureRef SrcTexture, FVector2f WindowSize) const override;
	virtual FIntRect GetFullFlatEyeRect_RenderThread(const FRHITextureDesc& EyeTexture) const override;
	/** Native OpenXR layers or the engine's eye-buffer drawing: FRAMEXR_STEREO_LAYERS=native|emulated|auto (see the cpp). */
	virtual IStereoLayers* GetStereoLayers() override;

	// IStereoRenderTargetManager
	virtual bool ShouldUseSeparateRenderTarget() const override { return IsStereoEnabled(); }
	virtual EPixelFormat GetActualColorSwapchainFormat() const override { return PF_B8G8R8A8; }
	virtual int32 AcquireColorTexture() override;
	virtual bool AllocateRenderTargetTextures(FRHICommandListBase& RHICmdList, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTextureRHIRef>& OutTargetableTextures, TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples = 1) override;

private:
	enum class ERecenter : uint8 { None, Full, Orientation, Position };

	const FFrameXRTrackingFrame& CurrentFrame() const;
	bool IsFloorOrigin() const;
	XrReferenceSpaceType SpaceForOrigin() const;
	/** Derives head, eyes, ipd and fov from located views. */
	static void FillFromViews(FFrameXRTrackingFrame& Frame);
	/** Game thread: polls events, locates, syncs input, applies recentring, refreshes GameFrame. */
	void UpdateGameFrame();
	void QueueSubmit_RenderThread(FRHICommandListImmediate& RHICmdList);
	void OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider);

	FFrameXRTrackingFrame GameFrame;
	FFrameXRTrackingFrame RenderFrame;

	// Recentring (game thread). LocalOrigin is the recentred origin inside the runtime's reference space.
	FXrPose LocalOrigin;
	ERecenter PendingRecenter = ERecenter::None;
	float PendingYaw = 0.f;
	EHMDTrackingOrigin::Type TrackingOrigin;
	bool bWasRunning = false;
	float WorldToMeters = 100.f;

	std::atomic<bool> bStereoEnabled{ false };
	bool bHMDEnabled = true;
	float PixelDensity = 1.f;

	// Render targets the engine renders both eyes into; one is acquired per frame.
	TArray<FTextureRHIRef> RenderTextures; // render thread
	std::atomic<int32> NumRenderTextures{ 0 };
	int32 AcquiredTexture_GameThread = INDEX_NONE;
	int32 AcquiredTexture_RenderThread = INDEX_NONE;

	TRefCountPtr<FFrameXRRenderBridge> RenderBridge;

	// Stereo layers: chosen once, on the first GetStereoLayers. Gathered on the game thread, submitted with the frame.
	bool bLayerModeChosen = false;
	TSharedPtr<FFrameXRStereoLayers, ESPMode::ThreadSafe> NativeLayers;
	TArray<FFrameXRLayer> RenderLayers;              // render thread
	TArray<FTextureResource*> RenderLayerTextures;   // render thread
};
