// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#include "FrameXRRenderBridge.h"

#include "IMetalDynamicRHI.h"

void FFrameXRRenderBridge::SetFrame_RHIThread(FFrameXRSubmitFrame&& Frame)
{
	Pending = MoveTemp(Frame);
}

bool FFrameXRRenderBridge::Present(FRHIViewport* Viewport, IRHICommandContext& RHICmdContext, int32& InOutSyncInterval)
{
	if (Pending.IsSet() && IsRHIMetal())
	{
		// The rendering of this frame has already been handed to the submission thread, which commits work
		// to the GPU in order: running there puts our blit right behind it on the same queue.
		TRefCountPtr<FFrameXRRenderBridge> Self(this);
		GetIMetalDynamicRHI()->RHIRunOnQueue(
			[Self, Frame = MoveTemp(Pending.GetValue())](MTL::CommandQueue* Queue)
			{
				Self->Submit(Queue, Frame);
			},
			false);
		Pending.Reset();
	}
	return true; // Still present the Mac window (spectator view).
}

void FFrameXRRenderBridge::Submit(void* MetalCommandQueue, const FFrameXRSubmitFrame& Frame)
{
	FFrameXRSession& Session = FFrameXRSession::Get();
	if (!Session.HasSession())
	{
		if (bSessionFailed)
		{
			return;
		}
		bSessionFailed = !Session.CreateSession(MetalCommandQueue);
		UE_CLOG(bSessionFailed, LogFrameXR, Warning, TEXT("No OpenXR session could be made; nothing will reach the headset"));
		return; // The game thread's event poll begins the session; frames follow.
	}
	if (!Session.IsRunning())
	{
		return;
	}
	void* Native = Frame.Texture.IsValid() ? Frame.Texture->GetNativeResource() : nullptr;
	TArray<FFrameXRLayer, TInlineAllocator<8>> Layers;
	for (int32 I = 0; I < Frame.Layers.Num(); ++I)
	{
		FFrameXRLayer& L = Layers.Add_GetRef(Frame.Layers[I]);
		L.Texture = Frame.LayerTextures.IsValidIndex(I) && Frame.LayerTextures[I].IsValid() ? Frame.LayerTextures[I]->GetNativeResource() : nullptr;
	}
	Session.SubmitFrame(MetalCommandQueue, Native, Frame.Views, Frame.Space, Layers);
	if (++Presents == 1)
	{
		UE_LOG(LogFrameXR, Log, TEXT("First frame through the OpenXR session (eye texture %s)"), Native ? TEXT("present") : TEXT("missing"));
	}
}
