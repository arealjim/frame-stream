// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#include "FrameXRInput.h"

#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "IXRTrackingSystem.h"
#include "FrameXRHMD.h"

#define LOCTEXT_NAMESPACE "FrameXR"

namespace
{
	// The engine has no thumb rest keys; these follow its "OculusTouch_<Hand>_<Input>" naming.
	const FKey LeftThumbRest(TEXT("OculusTouch_Left_ThumbRest"));
	const FKey RightThumbRest(TEXT("OculusTouch_Right_ThumbRest"));

	// The Steam Frame controller's inputs that Touch has no key for, each on its own (only with the Frame profile).
	// The Touch keys keep SteamVR's own Frame-to-Touch meaning, so a Touch input map works unchanged on the Frame.
	const FKey FrameRightX(TEXT("SteamFrame_Right_X_Click"));
	const FKey FrameRightY(TEXT("SteamFrame_Right_Y_Click"));
	const FKey FrameRightMenu(TEXT("SteamFrame_Right_Menu_Click"));
	const FKey FrameLeftDpadUp(TEXT("SteamFrame_Left_DPad_Up"));
	const FKey FrameLeftDpadDown(TEXT("SteamFrame_Left_DPad_Down"));
	const FKey FrameLeftDpadLeft(TEXT("SteamFrame_Left_DPad_Left"));
	const FKey FrameLeftDpadRight(TEXT("SteamFrame_Left_DPad_Right"));
	const FKey FrameLeftBumper(TEXT("SteamFrame_Left_Bumper_Click"));
	const FKey FrameRightBumper(TEXT("SteamFrame_Right_Bumper_Click"));

	const FName LeftGripSource(TEXT("LeftGrip"));
	const FName RightGripSource(TEXT("RightGrip"));
	const FName LeftAimSource(TEXT("LeftAim"));
	const FName RightAimSource(TEXT("RightAim"));

	// Button order within FHand::Buttons; SendControllerEvents fills the same order.
	enum EButton
	{
		FaceLowerClick, FaceUpperClick, FaceLowerTouch, FaceUpperTouch, Menu,
		TriggerClick, TriggerTouch, GripClick, ThumbstickClick, ThumbstickTouch, ThumbRest,
		ThumbstickUp, ThumbstickDown, ThumbstickLeft, ThumbstickRight,
		FrameX, FrameY, FrameMenu, DpadUp, DpadDown, DpadLeft, DpadRight, Bumper,
		NumButtons
	};

	enum EAxis { TriggerAxis, GripAxis, ThumbstickX, ThumbstickY, NumAxes };

	/** Press above Press, release below Release, otherwise keep the previous state. */
	bool Hysteresis(float Value, bool bWasPressed, float Press, float Release)
	{
		return bWasPressed ? Value > Release : Value > Press;
	}

	/** Resolves a MotionControllerComponent source to a hand and whether it wants the aim pose. */
	bool ResolveSource(const FName Source, int32& OutHand, bool& bOutAim)
	{
		if (Source == IMotionController::LeftHandSourceId || Source == LeftGripSource || Source == LeftAimSource)
		{
			OutHand = 0;
			bOutAim = Source == LeftAimSource;
			return true;
		}
		if (Source == IMotionController::RightHandSourceId || Source == RightGripSource || Source == RightAimSource)
		{
			OutHand = 1;
			bOutAim = Source == RightAimSource;
			return true;
		}
		return false;
	}
}

FFrameXRInput::FFrameXRInput(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	const FKey Keys[2][NumButtons] = {
		{
			EKeys::OculusTouch_Left_X_Click, EKeys::OculusTouch_Left_Y_Click,
			EKeys::OculusTouch_Left_X_Touch, EKeys::OculusTouch_Left_Y_Touch, EKeys::OculusTouch_Left_Menu_Click,
			EKeys::OculusTouch_Left_Trigger_Click, EKeys::OculusTouch_Left_Trigger_Touch, EKeys::OculusTouch_Left_Grip_Click,
			EKeys::OculusTouch_Left_Thumbstick_Click, EKeys::OculusTouch_Left_Thumbstick_Touch, LeftThumbRest,
			EKeys::OculusTouch_Left_Thumbstick_Up, EKeys::OculusTouch_Left_Thumbstick_Down,
			EKeys::OculusTouch_Left_Thumbstick_Left, EKeys::OculusTouch_Left_Thumbstick_Right,
			FKey(), FKey(), FKey(), FrameLeftDpadUp, FrameLeftDpadDown, FrameLeftDpadLeft, FrameLeftDpadRight, FrameLeftBumper,
		},
		{
			EKeys::OculusTouch_Right_A_Click, EKeys::OculusTouch_Right_B_Click,
			EKeys::OculusTouch_Right_A_Touch, EKeys::OculusTouch_Right_B_Touch, FKey(), // The right menu button is the system button.
			EKeys::OculusTouch_Right_Trigger_Click, EKeys::OculusTouch_Right_Trigger_Touch, EKeys::OculusTouch_Right_Grip_Click,
			EKeys::OculusTouch_Right_Thumbstick_Click, EKeys::OculusTouch_Right_Thumbstick_Touch, RightThumbRest,
			EKeys::OculusTouch_Right_Thumbstick_Up, EKeys::OculusTouch_Right_Thumbstick_Down,
			EKeys::OculusTouch_Right_Thumbstick_Left, EKeys::OculusTouch_Right_Thumbstick_Right,
			FrameRightX, FrameRightY, FrameRightMenu, FKey(), FKey(), FKey(), FKey(), FrameRightBumper,
		},
	};
	const FKey AxisKeys[2][NumAxes] = {
		{ EKeys::OculusTouch_Left_Trigger_Axis, EKeys::OculusTouch_Left_Grip_Axis, EKeys::OculusTouch_Left_Thumbstick_X, EKeys::OculusTouch_Left_Thumbstick_Y },
		{ EKeys::OculusTouch_Right_Trigger_Axis, EKeys::OculusTouch_Right_Grip_Axis, EKeys::OculusTouch_Right_Thumbstick_X, EKeys::OculusTouch_Right_Thumbstick_Y },
	};
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		for (const FKey& Key : Keys[Hand])
		{
			Hands[Hand].Buttons.Add({ Key, false });
		}
		for (const FKey& Key : AxisKeys[Hand])
		{
			Hands[Hand].Axes.Add({ Key, 0.f });
		}
	}

	IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
}

FFrameXRInput::~FFrameXRInput()
{
	IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
}

void FFrameXRInput::RegisterKeys()
{
	auto AddIfMissing = [](const FKey& Key, const FText& DisplayName)
	{
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			EKeys::AddKey(FKeyDetails(Key, DisplayName, FKeyDetails::GamepadKey | FKeyDetails::NotBlueprintBindableKey, TEXT("OculusTouch")));
		}
	};
	AddIfMissing(LeftThumbRest, LOCTEXT("OculusTouch_Left_ThumbRest", "Oculus Touch (L) Thumb Rest"));
	AddIfMissing(RightThumbRest, LOCTEXT("OculusTouch_Right_ThumbRest", "Oculus Touch (R) Thumb Rest"));

	auto AddFrameKey = [](const FKey& Key, const FText& DisplayName)
	{
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			EKeys::AddKey(FKeyDetails(Key, DisplayName, FKeyDetails::GamepadKey, TEXT("SteamFrame")));
		}
	};
	EKeys::AddMenuCategoryDisplayInfo(TEXT("SteamFrame"), LOCTEXT("SteamFrameCategory", "Steam Frame"), TEXT("GraphEditor.PadEvent_16x"));
	AddFrameKey(FrameRightX, LOCTEXT("SteamFrame_Right_X_Click", "Steam Frame (R) X"));
	AddFrameKey(FrameRightY, LOCTEXT("SteamFrame_Right_Y_Click", "Steam Frame (R) Y"));
	AddFrameKey(FrameRightMenu, LOCTEXT("SteamFrame_Right_Menu_Click", "Steam Frame (R) Menu"));
	AddFrameKey(FrameLeftDpadUp, LOCTEXT("SteamFrame_Left_DPad_Up", "Steam Frame (L) D-pad Up"));
	AddFrameKey(FrameLeftDpadDown, LOCTEXT("SteamFrame_Left_DPad_Down", "Steam Frame (L) D-pad Down"));
	AddFrameKey(FrameLeftDpadLeft, LOCTEXT("SteamFrame_Left_DPad_Left", "Steam Frame (L) D-pad Left"));
	AddFrameKey(FrameLeftDpadRight, LOCTEXT("SteamFrame_Left_DPad_Right", "Steam Frame (L) D-pad Right"));
	AddFrameKey(FrameLeftBumper, LOCTEXT("SteamFrame_Left_Bumper_Click", "Steam Frame (L) Bumper"));
	AddFrameKey(FrameRightBumper, LOCTEXT("SteamFrame_Right_Bumper_Click", "Steam Frame (R) Bumper"));
}

FFrameXRHMD* FFrameXRInput::GetHMD()
{
	if (GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == FFrameXRHMD::SystemName)
	{
		return static_cast<FFrameXRHMD*>(GEngine->XRSystem.Get());
	}
	return nullptr;
}

void FFrameXRInput::SetButton(FButton& Button, bool bPressed)
{
	if (Button.bPressed == bPressed || !Button.Key.IsValid())
	{
		return;
	}
	Button.bPressed = bPressed;
	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	if (bPressed)
	{
		MessageHandler->OnControllerButtonPressed(Button.Key.GetFName(), User, Device, false);
	}
	else
	{
		MessageHandler->OnControllerButtonReleased(Button.Key.GetFName(), User, Device, false);
	}
}

void FFrameXRInput::SetAxis(FAxis& Axis, float Value)
{
	if (FMath::IsNearlyEqual(Axis.Value, Value, 1e-4f))
	{
		return;
	}
	Axis.Value = Value;
	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	MessageHandler->OnControllerAnalog(Axis.Key.GetFName(), User, Device, Value);
}

void FFrameXRInput::ReleaseAll()
{
	for (FHand& Hand : Hands)
	{
		for (FButton& Button : Hand.Buttons)
		{
			SetButton(Button, false);
		}
		for (FAxis& Axis : Hand.Axes)
		{
			SetAxis(Axis, 0.f);
		}
	}
}

void FFrameXRInput::SendControllerEvents()
{
	// Only while something renders to the headset (VR Preview, -vr): the editor UI shouldn't see Touch input.
	const FFrameXRHMD* HMD = GetHMD();
	const bool bLive = HMD && HMD->IsStereoEnabled() && HMD->IsRunning();
	if (!bLive)
	{
		if (bActive)
		{
			ReleaseAll();
			bActive = false;
		}
		return;
	}
	bActive = true;

	for (int32 HandIndex = 0; HandIndex < 2; ++HandIndex)
	{
		FHand& Hand = Hands[HandIndex];
		const FFrameXRController& C = HMD->GetController(HandIndex);
		if (!C.bActive)
		{
			for (FButton& Button : Hand.Buttons) { SetButton(Button, false); }
			for (FAxis& Axis : Hand.Axes) { SetAxis(Axis, 0.f); }
			continue;
		}

		const bool bLeft = HandIndex == 0;
		TArray<FButton>& B = Hand.Buttons;
		SetButton(B[FaceLowerClick], C.bPrimaryClick);
		SetButton(B[FaceUpperClick], C.bSecondaryClick);
		SetButton(B[FaceLowerTouch], C.bPrimaryTouch);
		SetButton(B[FaceUpperTouch], C.bSecondaryTouch);
		SetButton(B[Menu], bLeft && C.bMenu);
		SetButton(B[TriggerClick], Hysteresis(C.Trigger, B[TriggerClick].bPressed, 0.55f, 0.45f));
		SetButton(B[TriggerTouch], C.bTriggerTouch || C.Trigger > 0.05f);
		SetButton(B[GripClick], Hysteresis(C.Squeeze, B[GripClick].bPressed, 0.55f, 0.45f));
		SetButton(B[ThumbstickClick], C.bStickClick);
		SetButton(B[ThumbstickTouch], C.bStickTouch);
		SetButton(B[ThumbRest], C.bThumbRestTouch);
		SetButton(B[ThumbstickUp], Hysteresis(C.Stick.Y, B[ThumbstickUp].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickDown], Hysteresis(-C.Stick.Y, B[ThumbstickDown].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickLeft], Hysteresis(-C.Stick.X, B[ThumbstickLeft].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickRight], Hysteresis(C.Stick.X, B[ThumbstickRight].bPressed, 0.7f, 0.6f));
		SetButton(B[FrameX], C.bFrameX);
		SetButton(B[FrameY], C.bFrameY);
		SetButton(B[FrameMenu], C.bFrameRightMenu);
		SetButton(B[DpadUp], C.bDpad[0]);
		SetButton(B[DpadDown], C.bDpad[1]);
		SetButton(B[DpadLeft], C.bDpad[2]);
		SetButton(B[DpadRight], C.bDpad[3]);
		SetButton(B[Bumper], C.bBumper);

		SetAxis(Hand.Axes[TriggerAxis], C.Trigger);
		SetAxis(Hand.Axes[GripAxis], C.Squeeze);
		SetAxis(Hand.Axes[ThumbstickX], C.Stick.X);
		SetAxis(Hand.Axes[ThumbstickY], C.Stick.Y);
	}
}

FName FFrameXRInput::GetMotionControllerDeviceTypeName() const
{
	return FFrameXRHMD::SystemName;
}

bool FFrameXRInput::GetControllerOrientationAndPosition(const int32 ControllerIndex, const FName MotionSource, FRotator& OutOrientation, FVector& OutPosition, float WorldToMetersScale) const
{
	int32 Hand = 0;
	bool bAim = false;
	const FFrameXRHMD* HMD = GetHMD();
	FQuat Orientation;
	if (ControllerIndex != 0 || !HMD || !ResolveSource(MotionSource, Hand, bAim) ||
		!HMD->GetControllerPose(Hand, bAim, WorldToMetersScale, Orientation, OutPosition))
	{
		return false;
	}
	OutOrientation = Orientation.Rotator();
	return true;
}

ETrackingStatus FFrameXRInput::GetControllerTrackingStatus(const int32 ControllerIndex, const FName MotionSource) const
{
	int32 Hand = 0;
	bool bAim = false;
	const FFrameXRHMD* HMD = GetHMD();
	const bool bTracked = ControllerIndex == 0 && HMD && ResolveSource(MotionSource, Hand, bAim) && HMD->IsControllerTracked(Hand);
	return bTracked ? ETrackingStatus::Tracked : ETrackingStatus::NotTracked;
}

void FFrameXRInput::EnumerateSources(TArray<FMotionControllerSource>& SourcesOut) const
{
	FXRMotionControllerBase::EnumerateSources(SourcesOut);
	for (const FName& Source : { LeftGripSource, RightGripSource, LeftAimSource, RightAimSource })
	{
		if (!SourcesOut.ContainsByPredicate([&Source](const FMotionControllerSource& Existing) { return Existing.SourceName == Source; }))
		{
			SourcesOut.Add(FMotionControllerSource(Source));
		}
	}
}

#undef LOCTEXT_NAMESPACE
