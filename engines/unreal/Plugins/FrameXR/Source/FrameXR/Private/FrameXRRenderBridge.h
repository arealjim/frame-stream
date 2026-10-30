// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#pragma once

#include "FrameXRCommon.h"
#include "FrameXRSession.h"
#include "XRRenderBridge.h"

/** One rendered stereo frame waiting to be handed to the OpenXR session. */
struct FFrameXRSubmitFrame
{
	/** Side-by-side eyes: left half, right half. */
	FTextureRHIRef Texture;
	/** The views the frame was rendered with (base space); invalid before the first display time exists. */
	FFrameXRViews Views;
	XrReferenceSpaceType Space = XR_REFERENCE_SPACE_TYPE_LOCAL;
	/** Stereo layers (FRAMEXR_STEREO_LAYERS=native), lowest priority first, and each one's texture. */
	TArray<FFrameXRLayer> Layers;
	TArray<FTextureRHIRef> LayerTextures;
};

/**
 * Custom present on the stereo viewport: after the engine has rendered both eyes into the viewport's render
 * target, it runs the OpenXR frame loop for that frame on the engine's own Metal command queue (creating the
 * session there the first time), then lets the normal present run so the Mac window keeps the spectator view.
 */
class FFrameXRRenderBridge : public FXRRenderBridge
{
public:
	virtual ~FFrameXRRenderBridge() override = default;

	/** Queues the frame to submit at the next present. RHI thread (enqueue from the render thread). */
	void SetFrame_RHIThread(FFrameXRSubmitFrame&& Frame);

	virtual bool NeedsNativePresent() override { return true; }
	virtual bool Present(FRHIViewport* Viewport, IRHICommandContext& RHICmdContext, int32& InOutSyncInterval) override;
	virtual bool PresentOnSubmissionThread(int32& InOutSyncInterval) override { return true; }

private:
	/** Runs on the Metal submission thread with the engine's queue, behind this frame's commands. */
	void Submit(void* MetalCommandQueue, const FFrameXRSubmitFrame& Frame);

	TOptional<FFrameXRSubmitFrame> Pending;
	bool bSessionFailed = false;
	uint64 Presents = 0;
};
