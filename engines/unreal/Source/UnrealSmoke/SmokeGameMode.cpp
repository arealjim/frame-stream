// frame-lab unreal-smoke: see SmokeGameMode.h.

#include "SmokeGameMode.h"

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "IXRTrackingSystem.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "MotionControllerComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "RHI.h"
#include "RHIStats.h"
#include "RenderTimer.h"
#include "UObject/ConstructorHelpers.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/StereoLayerComponent.h"
#include "Components/TextBlock.h"
#include "Components/WidgetComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmoke, Log, All);

namespace
{
	/** The two automatic ProfileGPU dumps stall the frame they run in (a hitch in the headset), so they are opt-in: SMOKE_PROFILEGPU=1. */
	bool ProfileGPUWanted()
	{
		static const bool bWanted = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_PROFILEGPU")) == TEXT("1");
		return bWanted;
	}
}

ASmokePawn::ASmokePawn()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->bLockToHmd = true; // The pawn's origin is the floor; the headset's pose (floor-level origin) adds the height.

	// The controllers: a 4 x 6 x 12 cm block at each grip, and a 1 m, 1 cm ray from each aim pose (on the Frame the aim
	// is ~40 degrees from the grip, so a ray along the block would point the wrong way).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	const TCHAR* Side[2] = { TEXT("Left"), TEXT("Right") };
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		Grips[Hand] = CreateDefaultSubobject<UMotionControllerComponent>(*FString::Printf(TEXT("%sGrip"), Side[Hand]));
		Grips[Hand]->SetupAttachment(Root);
		Grips[Hand]->MotionSource = FName(*FString::Printf(TEXT("%sGrip"), Side[Hand]));
		UStaticMeshComponent* Block = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("%sBlock"), Side[Hand]));
		Block->SetupAttachment(Grips[Hand]);
		Block->SetStaticMesh(CubeMesh.Object);
		Block->SetRelativeScale3D(FVector(0.12f, 0.04f, 0.06f)); // X forward: 12 cm long
		Block->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Blocks[Hand] = Block;

		Aims[Hand] = CreateDefaultSubobject<UMotionControllerComponent>(*FString::Printf(TEXT("%sAim"), Side[Hand]));
		Aims[Hand]->SetupAttachment(Root);
		Aims[Hand]->MotionSource = FName(*FString::Printf(TEXT("%sAim"), Side[Hand]));
		UStaticMeshComponent* Ray = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("%sRay"), Side[Hand]));
		Ray->SetupAttachment(Aims[Hand]);
		Ray->SetStaticMesh(CylinderMesh.Object);
		Ray->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f)); // the cylinder's axis (Z) along the aim's forward (X)
		Ray->SetRelativeLocation(FVector(50.f, 0.f, 0.f));
		Ray->SetRelativeScale3D(FVector(0.01f, 0.01f, 1.f));
		Ray->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// Every key a Frame controller can send through FrameXR: the Touch keys (SteamVR's Frame-to-Touch meaning) and the
	// Frame's own. Logged on press and release, so a worn check can walk the buttons and read the log.
	for (const TCHAR* Name : {
			 TEXT("OculusTouch_Left_X_Click"), TEXT("OculusTouch_Left_Y_Click"), TEXT("OculusTouch_Left_Menu_Click"),
			 TEXT("OculusTouch_Left_Trigger_Click"), TEXT("OculusTouch_Left_Grip_Click"), TEXT("OculusTouch_Left_Thumbstick_Click"),
			 TEXT("OculusTouch_Right_A_Click"), TEXT("OculusTouch_Right_B_Click"), TEXT("OculusTouch_Right_Trigger_Click"),
			 TEXT("OculusTouch_Right_Grip_Click"), TEXT("OculusTouch_Right_Thumbstick_Click"),
			 TEXT("SteamFrame_Right_X_Click"), TEXT("SteamFrame_Right_Y_Click"), TEXT("SteamFrame_Right_Menu_Click"),
			 TEXT("SteamFrame_Left_DPad_Up"), TEXT("SteamFrame_Left_DPad_Down"), TEXT("SteamFrame_Left_DPad_Left"),
			 TEXT("SteamFrame_Left_DPad_Right"), TEXT("SteamFrame_Left_Bumper_Click"), TEXT("SteamFrame_Right_Bumper_Click") })
	{
		WatchedKeys.Add(FKey(Name));
	}
}

bool ASmokePawn::FloorUnder(const FVector& At, float& OutZ) const
{
	// From knee height above the pawn's floor, down 1.5 m: steps up to 50 cm, never a balcony overhead.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SmokeTrace), true); // complex: imported scenes (Sponza) often have no simple collision
	Params.AddIgnoredActor(this);
	if (GetWorld()->LineTraceSingleByChannel(Hit, At + FVector(0, 0, 50), At - FVector(0, 0, 100), ECC_WorldStatic, Params))
	{
		OutZ = Hit.Location.Z;
		return true;
	}
	return false;
}

void ASmokePawn::Locomote(float DeltaSeconds)
{
	// One stick does it all, either hand: the one pushed further is the one read. Up/down walks along the head's yaw,
	// left/right past 0.7 snaps a turn (re-armed under 0.3), whichever axis dominates. No strafe: one hand does it all.
	const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC || !Camera)
	{
		return;
	}
	const FVector2D Sticks[2] = {
		FVector2D(PC->GetInputAnalogKeyState(EKeys::OculusTouch_Left_Thumbstick_X), PC->GetInputAnalogKeyState(EKeys::OculusTouch_Left_Thumbstick_Y)),
		FVector2D(PC->GetInputAnalogKeyState(EKeys::OculusTouch_Right_Thumbstick_X), PC->GetInputAnalogKeyState(EKeys::OculusTouch_Right_Thumbstick_Y)) };
	const FVector2D Stick = Sticks[0].SizeSquared() >= Sticks[1].SizeSquared() ? Sticks[0] : Sticks[1];
	const FVector Head = Camera->GetComponentLocation();

	if (FMath::Abs(Stick.X) < 0.3f)
	{
		bSnapArmed = true;
	}
	else if (bSnapArmed && FMath::Abs(Stick.X) > 0.7f && FMath::Abs(Stick.X) > FMath::Abs(Stick.Y))
	{
		// Turn about the head, not the pawn origin, so the eyes stay where they are.
		bSnapArmed = false;
		const FRotator Turn(0.f, Stick.X > 0.f ? SnapDegrees : -SnapDegrees, 0.f);
		const FVector Loc = GetActorLocation();
		SetActorLocationAndRotation(Head + Turn.RotateVector(Loc - Head), Turn + GetActorRotation());
		UE_LOG(LogSmoke, Log, TEXT("smoke: snap turn %+.0f, yaw now %.0f"), Turn.Yaw, GetActorRotation().Yaw);
		return;
	}

	const float Y = FMath::Abs(Stick.Y) > 0.2f && FMath::Abs(Stick.Y) >= FMath::Abs(Stick.X) ? (FMath::Abs(Stick.Y) - 0.2f) / 0.8f * FMath::Sign(Stick.Y) : 0.f;
	if (Y == 0.f)
	{
		return;
	}
	// Each rendered frame is shown one display period after the last, so a step of one period is what the eye sees as
	// even motion; the game thread's measured DeltaSeconds jitters around it and the client's warp (rotation only) cannot
	// hide translation jitter. A real dropped frame (over 1.5 periods) still steps by its measured time.
	// SMOKE_MOVE_DT=tick goes back to DeltaSeconds for the A/B (Sponza 2026-10-24: a stutter only while moving).
	const float Period = 1.f / MoveHz;
	const float Dt = bMoveByTick || DeltaSeconds > 1.5f * Period ? DeltaSeconds : Period;
	if (bFly)
	{
		// 2026-10-24: the camera moves up and down when moving forward while looking up or down - no floor, no walls.
		SetActorLocation(GetActorLocation() + Camera->GetComponentRotation().Vector() * (Y * WalkSpeed * Dt));
		return;
	}
	const FVector Forward = FRotator(0.f, Camera->GetComponentRotation().Yaw, 0.f).Vector();
	const FVector Step = Forward * (Y * WalkSpeed * Dt);
	// A wall or column within 25 cm of the head's path (at waist height) stops the step.
	const double TraceStart = FPlatformTime::Seconds();
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SmokeTrace), true); // complex: imported scenes (Sponza) often have no simple collision
	Params.AddIgnoredActor(this);
	const FVector Waist(Head.X, Head.Y, GetActorLocation().Z + 100.f);
	const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, Waist, Waist + Step + Step.GetSafeNormal() * 25.f, ECC_WorldStatic, Params);
	FVector Loc = GetActorLocation() + Step;
	float FloorZ;
	float AppliedDz = 0.f;
	// The floor under a big mesh is many triangles, a few mm apart: following each one every frame bobs the eyes.
	// Only a change past the deadband (a step, a ramp's accumulated rise) moves the pawn; SMOKE_FLOOR_DEADBAND=0 follows all.
	if (!bBlocked && FloorUnder(FVector(Head.X + Step.X, Head.Y + Step.Y, GetActorLocation().Z), FloorZ) &&
		FMath::Abs(FloorZ - Loc.Z) > FloorDeadband)
	{
		AppliedDz = FloorZ - Loc.Z;
		Loc.Z = FloorZ;
	}
	const double TraceUs = (FPlatformTime::Seconds() - TraceStart) * 1e6;

	// One line per second of walking: what the step was made of.
	Walk.Ticks++;
	Walk.DtMin = FMath::Min(Walk.DtMin, DeltaSeconds);
	Walk.DtMax = FMath::Max(Walk.DtMax, DeltaSeconds);
	Walk.DtSum += DeltaSeconds;
	Walk.StepSum += bBlocked ? 0.f : Step.Size();
	Walk.Blocked += bBlocked ? 1 : 0;
	Walk.DzMax = FMath::Max(Walk.DzMax, FMath::Abs(AppliedDz));
	Walk.DzMoves += AppliedDz != 0.f ? 1 : 0;
	Walk.TraceSum += TraceUs;
	Walk.TraceMax = FMath::Max(Walk.TraceMax, TraceUs);
	if (Walk.Ticks >= 90)
	{
		UE_LOG(LogSmoke, Log, TEXT("smoke: walk %d ticks dt %.2f/%.2f/%.2f ms (min/mean/max), step %s %.2f mm mean, blocked %d, floor moves %d (max %.1f cm), traces %.0f/%.0f us (mean/max)"),
			Walk.Ticks, Walk.DtMin * 1e3f, Walk.DtSum / Walk.Ticks * 1e3f, Walk.DtMax * 1e3f, bMoveByTick ? TEXT("by tick") : TEXT("by period"),
			Walk.StepSum / Walk.Ticks * 10.f, Walk.Blocked, Walk.DzMoves, Walk.DzMax, Walk.TraceSum / Walk.Ticks, Walk.TraceMax);
		Walk = FWalkStats();
	}
	if (!bBlocked)
	{
		SetActorLocation(Loc);
	}
}

void ASmokePawn::LogKeys()
{
	const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		return;
	}
	for (const FKey& Key : WatchedKeys)
	{
		const bool bDown = PC->IsInputKeyDown(Key);
		if (bDown != KeysDown.Contains(Key.GetFName()))
		{
			UE_LOG(LogSmoke, Log, TEXT("smoke: key %s %s"), *Key.GetFName().ToString(), bDown ? TEXT("down") : TEXT("up"));
			if (bDown) { KeysDown.Add(Key.GetFName()); } else { KeysDown.Remove(Key.GetFName()); }
		}
	}
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		const bool bTracked = Grips[Hand] && Grips[Hand]->IsTracked();
		if (bTracked != bControllersSeen[Hand])
		{
			bControllersSeen[Hand] = bTracked;
			UE_LOG(LogSmoke, Log, TEXT("smoke: %s controller %s"), Hand ? TEXT("right") : TEXT("left"), bTracked ? TEXT("tracked") : TEXT("lost"));
		}
	}
}

void ASmokePawn::BeginPlay()
{
	Super::BeginPlay();
	StartTime = FPlatformTime::Seconds();

	// SMOKE_SPAWN="x,y,z[,yaw]": where the pawn starts (cm, world), z a point above the floor; the trace below drops it
	// onto the floor. Sponza's player start is inside a column (2026-10-24).
	const FString Spawn = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_SPAWN"));
	if (!Spawn.IsEmpty())
	{
		TArray<FString> Parts;
		Spawn.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 3)
		{
			SetActorLocation(FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2])));
			if (Parts.Num() >= 4)
			{
				SetActorRotation(FRotator(0.f, FCString::Atof(*Parts[3]), 0.f));
			}
			UE_LOG(LogSmoke, Log, TEXT("smoke: SMOKE_SPAWN %s -> %s yaw %.0f"), *Spawn, *GetActorLocation().ToString(), GetActorRotation().Yaw);
		}
		else
		{
			UE_LOG(LogSmoke, Warning, TEXT("smoke: SMOKE_SPAWN '%s' ignored: want x,y,z[,yaw]"), *Spawn);
		}
	}
	// The reference cube only in the smoke's own empty map, unless SMOKE_CUBE says otherwise: in a real scene it is
	// clutter that keeps coming back in front of the face (Sponza, 2026-10-24: not needed there, so off).
	const FString CubeEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_CUBE"));
	bCubeWanted = CubeEnv.IsEmpty() ? GetWorld()->GetMapName().Contains(TEXT("Template_Default")) : CubeEnv != TEXT("0");
	UE_LOG(LogSmoke, Log, TEXT("smoke: reference cube %s (map %s, SMOKE_CUBE '%s')"), bCubeWanted ? TEXT("on") : TEXT("off"), *GetWorld()->GetMapName(), *CubeEnv);
	// The grab cube follows the reference cube's rule: the smoke's own map, unless SMOKE_GRAB says otherwise.
	const FString GrabEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_GRAB"));
	bGrabWanted = GrabEnv.IsEmpty() ? GetWorld()->GetMapName().Contains(TEXT("Template_Default")) : GrabEnv != TEXT("0");
	const FString Reach = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_GRAB_REACH"));
	if (!Reach.IsEmpty())
	{
		GrabReach = FMath::Clamp(FCString::Atof(*Reach), 1.f, 10000.f);
	}
	// Each block gets its own material, so the trigger can colour it: the basic shape material's one vector parameter,
	// found by listing them (its name is the material's business, not ours).
	FString ParamNames;
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		if (Blocks[Hand])
		{
			// The cube mesh itself carries WorldGridMaterial (no parameters); the basic shape material has the colour input.
			BlockMaterials[Hand] = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")), this);
			Blocks[Hand]->SetMaterial(0, BlockMaterials[Hand]);
		}
	}
	if (BlockMaterials[0])
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		BlockMaterials[0]->GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			ParamNames += Info.Name.ToString() + TEXT(" ");
			if (ColorParam.IsNone()) { ColorParam = Info.Name; }
		}
	}
	UE_LOG(LogSmoke, Log, TEXT("smoke: grab cube %s (SMOKE_GRAB '%s'), reach %.0f cm; block material %s, vector parameters [%s], colour via '%s'"),
		bGrabWanted ? TEXT("on") : TEXT("off"), *GrabEnv, GrabReach, BlockMaterials[0] ? *GetNameSafe(BlockMaterials[0]->Parent) : TEXT("none"),
		*ParamNames.TrimEnd(), ColorParam.IsNone() ? TEXT("NONE: blocks will not change colour") : *ColorParam.ToString());
	bMoveByTick = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_MOVE_DT")) == TEXT("tick");
	const FString Hz = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_HZ"));
	if (!Hz.IsEmpty())
	{
		MoveHz = FMath::Clamp(FCString::Atof(*Hz), 30.f, 144.f);
	}
	const FString Speed = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_WALK_SPEED"));
	if (!Speed.IsEmpty())
	{
		WalkSpeed = FMath::Clamp(FCString::Atof(*Speed), 10.f, 5000.f);
	}
	bFly = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_FLY")) != TEXT("0");
	UE_LOG(LogSmoke, Log, TEXT("smoke: move %s at %.0f cm/s"), bFly ? TEXT("fly (gaze)") : TEXT("walk (level)"), WalkSpeed);
	const FString Deadband = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_FLOOR_DEADBAND"));
	if (!Deadband.IsEmpty())
	{
		FloorDeadband = FMath::Max(0.f, FCString::Atof(*Deadband));
	}
	UE_LOG(LogSmoke, Log, TEXT("smoke: walking steps %s at %.0f Hz, floor deadband %.1f cm"), bMoveByTick ? TEXT("by tick") : TEXT("by display period"), MoveHz, FloorDeadband);
	const FString Snap = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_SNAP_DEG"));
	if (!Snap.IsEmpty())
	{
		SnapDegrees = FMath::Clamp(FCString::Atof(*Snap), 5.f, 90.f);
	}

	// Drop the pawn onto the floor under the player start, so the tracking origin (floor level) matches the world floor.
	FHitResult Hit;
	const FVector Start = GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SmokeTrace), true); // complex: imported scenes (Sponza) often have no simple collision
	Params.AddIgnoredActor(this);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0, 0, 1000), ECC_WorldStatic, Params))
	{
		SetActorLocation(Hit.Location);
		UE_LOG(LogSmoke, Log, TEXT("smoke: floor under the start at Z=%.1f (%s)"), Hit.Location.Z, *GetNameSafe(Hit.GetActor()));
	}
	else
	{
		UE_LOG(LogSmoke, Warning, TEXT("smoke: no floor within 10 m under the start; the pawn stays at Z=%.1f (give SMOKE_SPAWN's z as the floor)"), Start.Z);
	}
	UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::LocalFloor);
	// SMOKE_PANELS or FRAMELAB_PANELS (the one switch for all three engines, 2026-10-28): 1, or toggle to hide and
	// show both layers every 10 s, so the runtime's "VideoEncoder: overlays:" line splits the GPU cost with and without.
	FString PanelsMode = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_PANELS"));
	if (PanelsMode.IsEmpty())
	{
		PanelsMode = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMELAB_PANELS"));
	}
	if (!PanelsMode.IsEmpty() && PanelsMode != TEXT("0"))
	{
		bPanelsToggle = PanelsMode.Contains(TEXT("toggle"));
		MakePanels();
	}

	UE_LOG(LogSmoke, Log, TEXT("smoke: pawn at %s, hmd %s, xr system %s"), *GetActorLocation().ToString(),
		UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled() ? TEXT("enabled") : TEXT("disabled"),
		GEngine && GEngine->XRSystem.IsValid() ? *GEngine->XRSystem->GetSystemName().ToString() : TEXT("none"));
}

void ASmokePawn::PlaceCube()
{
	// The probe: a 10 cm cube 50 cm ahead of the eyes, placed from the real head pose (the Godot and Unity smokes
	// use the same). Spawned on the first tracked pose, and brought back in front whenever it has been out of
	// view (behind, or further than 2 m) for 3 s: an off-head attach or a turned chair must not lose it.
	if (!Camera || !bCubeWanted)
	{
		return;
	}
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Forward = FRotator(0.f, Camera->GetComponentRotation().Yaw, 0.f).Vector();
	const FVector Where = Eye + Forward * 50.f;
	if (!Cube)
	{
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			Cube = GetWorld()->SpawnActor<AStaticMeshActor>(Where, FRotator::ZeroRotator);
			if (Cube)
			{
				Cube->SetMobility(EComponentMobility::Movable);
				Cube->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Cube->SetActorScale3D(FVector(0.1f));
#if WITH_EDITOR
				Cube->SetActorLabel(TEXT("SmokeCube")); // Editor-only API: a Game target has no actor labels.
#endif
			}
		}
	}
	else
	{
		Cube->SetActorLocation(Where);
	}
	UE_LOG(LogSmoke, Log, TEXT("smoke: cube placed at %s (eye %s, yaw %.0f)"), *Where.ToString(), *Eye.ToString(), Camera->GetComponentRotation().Yaw);
}

void ASmokePawn::PlaceGrabCube()
{
	// 12 cm, orange, 40 cm ahead of the eyes and 35 cm under them: in reach of either hand, below the reference cube
	// (10 cm at eye height, 50 cm ahead), so the two never overlap. No collision: walking and the floor traces ignore it.
	if (!Camera || !bGrabWanted || IsHeld(GrabCube))
	{
		return;
	}
	const FVector Eye = Camera->GetComponentLocation();
	const FRotator Yaw(0.f, Camera->GetComponentRotation().Yaw, 0.f);
	const FVector Where = Eye + Yaw.Vector() * 40.f - FVector(0.f, 0.f, 35.f);
	if (!GrabCube)
	{
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			GrabCube = GetWorld()->SpawnActor<AStaticMeshActor>(Where, Yaw);
			if (GrabCube)
			{
				GrabCube->SetMobility(EComponentMobility::Movable);
				UStaticMeshComponent* Comp = GrabCube->GetStaticMeshComponent();
				Comp->SetStaticMesh(Mesh);
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				GrabMaterial = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")), this);
				Comp->SetMaterial(0, GrabMaterial);
				if (GrabMaterial)
				{
					GrabMaterial->SetVectorParameterValue(ColorParam, FLinearColor(1.f, 0.35f, 0.05f));
				}
				GrabCube->SetActorScale3D(FVector(0.12f));
#if WITH_EDITOR
				GrabCube->SetActorLabel(TEXT("SmokeGrabCube"));
#endif
			}
		}
	}
	else
	{
		GrabCube->SetActorLocationAndRotation(Where, Yaw);
	}
	UE_LOG(LogSmoke, Log, TEXT("smoke: grab cube placed at %s"), *Where.ToString());
}

void ASmokePawn::HandsTick()
{
	// Worn 2026-10-26: the sticks moved, grip and trigger showed nothing. Now each block shows both, analog: the
	// trigger ramps it grey to bright green, the grip squashes it to half its 4 cm thickness. A grip press (0.55 up,
	// 0.45 down, as FrameXR's own Grip_Click) with the block near the grab cube picks it up; letting go leaves it there.
	const APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		return;
	}
	const FKey TriggerKeys[2] = { EKeys::OculusTouch_Left_Trigger_Axis, EKeys::OculusTouch_Right_Trigger_Axis };
	const FKey GripKeys[2] = { EKeys::OculusTouch_Left_Grip_Axis, EKeys::OculusTouch_Right_Grip_Axis };
	const FLinearColor Grey(0.25f, 0.25f, 0.25f), Green(0.05f, 1.f, 0.05f);
	const FLinearColor Orange(1.f, 0.35f, 0.05f), Held(1.f, 0.85f, 0.1f);
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		const TCHAR* Side = Hand ? TEXT("right") : TEXT("left");
		const float Trigger = FMath::Clamp(PC->GetInputAnalogKeyState(TriggerKeys[Hand]), 0.f, 1.f);
		const float Grip = FMath::Clamp(PC->GetInputAnalogKeyState(GripKeys[Hand]), 0.f, 1.f);
		if (Blocks[Hand])
		{
			Blocks[Hand]->SetRelativeScale3D(FVector(0.12f, 0.04f * (1.f - 0.5f * Grip), 0.06f));
		}
		if (BlockMaterials[Hand])
		{
			BlockMaterials[Hand]->SetVectorParameterValue(ColorParam, FMath::Lerp(Grey, Green, Trigger));
		}

		const bool bTrigger = bTriggerDown[Hand] ? Trigger > 0.45f : Trigger > 0.55f;
		if (bTrigger != bTriggerDown[Hand])
		{
			bTriggerDown[Hand] = bTrigger;
			UE_LOG(LogSmoke, Log, TEXT("smoke: %s trigger %s at %.2f"), Side, bTrigger ? TEXT("down") : TEXT("up"), Trigger);
		}
		const bool bGrip = bGripDown[Hand] ? Grip > 0.45f : Grip > 0.55f;
		if (bGrip == bGripDown[Hand])
		{
			continue;
		}
		bGripDown[Hand] = bGrip;
		// Either cube can be picked up, the grab cube or the reference cube (worn 2026-10-26: only one of the cubes
		// responded to grabbing): the nearest one within reach that the other hand is not holding.
		const FVector At = Blocks[Hand] ? Blocks[Hand]->GetComponentLocation() : FVector::ZeroVector;
		AStaticMeshActor* Nearest = nullptr;
		float NearestDist = -1.f;
		for (AStaticMeshActor* Candidate : { GrabCube.Get(), Cube.Get() })
		{
			const float D = Candidate ? FVector::Dist(At, Candidate->GetActorLocation()) : -1.f;
			if (Candidate && Candidate != HeldBy[1 - Hand].Get() && (NearestDist < 0.f || D < NearestDist))
			{
				Nearest = Candidate;
				NearestDist = D;
			}
		}
		UE_LOG(LogSmoke, Log, TEXT("smoke: %s grip %s at %.2f (block at %s, %s, nearest cube %s %.0f cm away)"), Side, bGrip ? TEXT("down") : TEXT("up"), Grip,
			*At.ToString(), Grips[Hand] && Grips[Hand]->IsTracked() ? TEXT("tracked") : TEXT("not tracked"),
			Nearest == GrabCube ? TEXT("grab") : Nearest ? TEXT("reference") : TEXT("none"), NearestDist);
		if (bGrip && !HeldBy[Hand] && Nearest && NearestDist <= GrabReach)
		{
			HeldBy[Hand] = Nearest;
			Nearest->AttachToComponent(Grips[Hand], FAttachmentTransformRules::KeepWorldTransform);
			if (Nearest == GrabCube && GrabMaterial) { GrabMaterial->SetVectorParameterValue(ColorParam, Held); }
			UE_LOG(LogSmoke, Log, TEXT("smoke: %s cube picked up by the %s hand"), Nearest == GrabCube ? TEXT("grab") : TEXT("reference"), Side);
		}
		else if (!bGrip && HeldBy[Hand])
		{
			AStaticMeshActor* Dropped = HeldBy[Hand].Get();
			HeldBy[Hand] = nullptr;
			Dropped->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			if (Dropped == GrabCube && GrabMaterial) { GrabMaterial->SetVectorParameterValue(ColorParam, Orange); }
			UE_LOG(LogSmoke, Log, TEXT("smoke: %s cube dropped by the %s hand at %s"), Dropped == GrabCube ? TEXT("grab") : TEXT("reference"), Side,
				*Dropped->GetActorLocation().ToString());
		}
	}

	// A dropped grab cube more than 3 m away (walked off from), or under the floor (placed from a headset that was not yet
	// on a head: the worn run's first pose was 14 cm up), for 5 s comes back in reach.
	if (GrabCube && !IsHeld(GrabCube) && Camera)
	{
		const bool bFar = FVector::Dist(GrabCube->GetActorLocation(), Camera->GetComponentLocation()) > 300.f ||
			// Under the floor counts only once the head is high enough to place it above (off-head the headset can sit at
			// floor level, and re-placing every 5 s would never end).
			(GrabCube->GetActorLocation().Z < GetActorLocation().Z && Camera->GetComponentLocation().Z - GetActorLocation().Z > 60.f);
		const double Now = FPlatformTime::Seconds();
		GrabLostSince = bFar ? (GrabLostSince > 0.0 ? GrabLostSince : Now) : 0.0;
		if (GrabLostSince > 0.0 && Now - GrabLostSince > 5.0)
		{
			GrabLostSince = 0.0;
			PlaceGrabCube();
		}
	}
}

void ASmokePawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Now = FPlatformTime::Seconds();
	LogKeys();
	HandsTick();
	PanelsTick();
	if (bTouring) { TourTick(DeltaSeconds, Now); } else { Locomote(DeltaSeconds); }

	// Live console commands for load sweeps (frame-lab, 2026-10-24): every 0.5 s, run each line of SMOKE_CMDFILE
	// (default ~/Library/Logs/frame-stream/smoke-cmd.txt) through the console, log it, and delete the file.
	static double NextCmdPoll = 0.0;
	if (Now >= NextCmdPoll)
	{
		NextCmdPoll = Now + 0.5;
		static const FString CmdFile = [] {
			FString F = FPlatformMisc::GetEnvironmentVariable(TEXT("SMOKE_CMDFILE"));
			return F.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) / TEXT("src/logs/smoke-cmd.txt") : F;
		}();
		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *CmdFile))
		{
			IFileManager::Get().Delete(*CmdFile);
			for (const FString& L : Lines)
			{
				if (L.TrimStartAndEnd().IsEmpty()) continue;
				UE_LOG(LogSmoke, Log, TEXT("smoke: cmd %s"), *L);
				TArray<FString> P;
				L.TrimStartAndEnd().ParseIntoArrayWS(P);
				if (P.Num() && P[0] == TEXT("smoke.tour"))
				{
					if (P.Num() > 1 && P[1] == TEXT("stop")) { bTouring = false; UE_LOG(LogSmoke, Log, TEXT("smoke: tour stopped")); }
					else { TourStart(P.Num() > 1 ? FCString::Atoi(*P[1]) : 1, P.Num() > 2 ? FCString::Atof(*P[2]) : 4.f); }
					continue;
				}
				GEngine->Exec(GetWorld(), *L);
			}
		}
	}

	// The first real head pose (the runtime's placeholder with no headset attached is exactly 1.6 m up, unrotated):
	// recentre yaw and floor position on it, then place the cube in front of the eyes.
	if (!bCentred && UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled())
	{
		FRotator Rot;
		FVector Pos;
		UHeadMountedDisplayFunctionLibrary::GetOrientationAndPosition(Rot, Pos);
		if (!Pos.Equals(FVector(0.f, 0.f, 160.f), 2.f) || !Rot.IsNearlyZero(0.5f))
		{
			bCentred = true;
			UE_LOG(LogSmoke, Log, TEXT("smoke: first tracked head pose %s rot %s; recentring"), *Pos.ToString(), *Rot.ToString());
			UHeadMountedDisplayFunctionLibrary::ResetOrientationAndPosition(0.f);
			bPlaceNext = true;
			CentredAt = Now;
		}
	}
	else if (bPlaceNext)
	{
		bPlaceNext = false;
		PlaceCube();
		PlaceGrabCube();
	}
	else if (Cube && Camera && !IsHeld(Cube))
	{
		const FVector ToCube = Cube->GetActorLocation() - Camera->GetComponentLocation();
		const FVector Forward = FRotator(0.f, Camera->GetComponentRotation().Yaw, 0.f).Vector();
		const bool bLost = FVector::DotProduct(ToCube.GetSafeNormal(), Forward) < 0.f || ToCube.Size() > 200.f;
		LostSince = bLost ? (LostSince > 0.0 ? LostSince : Now) : 0.0;
		if (LostSince > 0.0 && Now - LostSince > 3.0)
		{
			LostSince = 0.0;
			PlaceCube();
		}
	}

	// A second GPU profile 30 s after the recentre, i.e. with the Frame client attached and the runtime encoding.
	if (bCentred && !bProfiledStreaming && CentredAt > 0.0 && Now - CentredAt > 30.0 && GEngine && GEngine->GameViewport && ProfileGPUWanted())
	{
		bProfiledStreaming = true;
		UE_LOG(LogSmoke, Log, TEXT("smoke: ProfileGPU while streaming"));
		GEngine->Exec(GetWorld(), TEXT("ProfileGPU"));
	}

	PerfFrames++;
	PerfMaxDt = FMath::Max(PerfMaxDt, (double)DeltaSeconds);
	if (Now < NextReport)
	{
		return;
	}
	NextReport = Now + 5.0;
	// One GPU profile into the log, 20 s in: what the 9 Mpx stereo frame costs on this Mac (LogRHI lines).
	if (!bProfiled && Now - StartTime > 20.0 && GEngine && ProfileGPUWanted())
	{
		bProfiled = true;
		GEngine->Exec(GetWorld(), TEXT("r.ProfileGPU.ShowUI 0"));
		GEngine->Exec(GetWorld(), TEXT("ProfileGPU"));
	}
	FRotator Rot;
	FVector Pos;
	UHeadMountedDisplayFunctionLibrary::GetOrientationAndPosition(Rot, Pos);
	// 2026-10-24: switches and monitors for tuning. One line every 5 s that says what bound the view: the
	// window's mean and worst fps, game / render / RHI thread and GPU ms (the engine's own stat-unit numbers), and
	// the draw calls and primitives of the last frame. Knobs: any console command through the command file.
	const double Window = Now - PerfSince;
	UE_LOG(LogSmoke, Log, TEXT("smoke: perf fps %.1f (min %.0f) game %.2f render %.2f rhi %.2f gpu %.2f ms, draws %d prims %d, head pos %s rot %s, %s"),
		Window > 0 ? PerfFrames / Window : 0.0, PerfMaxDt > 0 ? 1.0 / PerfMaxDt : 0.0,
		FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(GRHIThreadTime),
		FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)), GNumDrawCallsRHI[0], GNumPrimitivesDrawnRHI[0], *Pos.ToString(), *Rot.ToString(),
		UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled() ? TEXT("hmd") : TEXT("no-hmd"));
	PerfSince = Now; PerfFrames = 0; PerfMaxDt = 0.0;
}

void ASmokePawn::TourStart(int32 Loops, float SpeedMs)
{
	// The path: 10 points on an ellipse over the 15-85th percentile of the scene's mesh centres; every third point cuts
	// toward the middle; every fourth (i%4==2) climbs 6 m above the floor, the rest sit at eye height (1.7 m).
	UE_LOG(LogSmoke, Log, TEXT("smoke: tour: building the path"));
	TArray<double> Xs, Ys;
	double ZMin = TNumericLimits<double>::Max(), ZMax = TNumericLimits<double>::Lowest();
	// Every actor with visible geometry (Sponza's are not plain StaticMeshActors). Both corners of each box go into the
	// percentiles, so a scene of a few huge meshes (the power plant: 21) still spans its real extent.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (*It == this || *It == Cube || *It == GrabCube || !It->FindComponentByClass<UStaticMeshComponent>()) continue;
		FVector O, E;
		It->GetActorBounds(true, O, E);
		if (E.IsNearlyZero() || E.Size() > 1e7) continue;
		Xs.Add(O.X - E.X); Xs.Add(O.X + E.X); Ys.Add(O.Y - E.Y); Ys.Add(O.Y + E.Y);
		ZMin = FMath::Min(ZMin, O.Z - E.Z); ZMax = FMath::Max(ZMax, O.Z + E.Z);
	}
	if (Xs.Num() < 2) { UE_LOG(LogSmoke, Warning, TEXT("smoke: tour: no meshes to tour")); return; }
	Xs.Sort(); Ys.Sort();
	auto Pct = [](const TArray<double>& A, double Q) { return A[FMath::Clamp((int32)(Q * (A.Num() - 1)), 0, A.Num() - 1)]; };
	const double X0 = Pct(Xs, .15), X1 = Pct(Xs, .85), Y0 = Pct(Ys, .15), Y1 = Pct(Ys, .85);
	const FVector2D Mid((X0 + X1) / 2, (Y0 + Y1) / 2), Rad(FMath::Max(100.0, (X1 - X0) / 2), FMath::Max(100.0, (Y1 - Y0) / 2));
	const float Fallback = GetActorLocation().Z;
	TourPts.Reset();
	for (int32 i = 0; i < 10; ++i)
	{
		const double A = 2 * PI * i / 10, K = i % 3 == 1 ? .35 : 1.0;
		const FVector2D XY = Mid + FVector2D(FMath::Cos(A) * Rad.X, FMath::Sin(A) * Rad.Y) * K;
		float Floor = Fallback;
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SmokeTrace), true);
		Params.AddIgnoredActor(this);
		// The first surface below 3 m over the pawn's eye plane (an interior's floor, not the ground under it), else
		// the first from the scene's top (a city's roofs); none: the pawn's own height.
		const FVector Down(XY.X, XY.Y, ZMin - 100);
		if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(XY.X, XY.Y, Fallback + 300.f), Down, ECC_WorldStatic, Params) ||
			GetWorld()->LineTraceSingleByChannel(Hit, FVector(XY.X, XY.Y, ZMax + 100), Down, ECC_WorldStatic, Params))
		{
			Floor = Hit.Location.Z;
		}
		TourPts.Add(FVector(XY.X, XY.Y, Floor + (i % 4 == 2 ? 600.f : 170.f)));
	}
	// Sample the closed Catmull-Rom spline for arc length.
	TourSamples.Reset(); TourCum.Reset();
	const int32 N = TourPts.Num(), Per = 40;
	for (int32 s = 0; s <= N * Per; ++s)
	{
		const int32 i = (s / Per) % N; const float t = (float)(s % Per) / Per;
		const FVector P0 = TourPts[(i + N - 1) % N], P1 = TourPts[i], P2 = TourPts[(i + 1) % N], P3 = TourPts[(i + 2) % N];
		const float t2 = t * t, t3 = t2 * t;
		TourSamples.Add(0.5f * (2 * P1 + (P2 - P0) * t + (2 * P0 - 5 * P1 + 4 * P2 - P3) * t2 + (3 * P1 - P0 - 3 * P2 + P3) * t3));
		TourCum.Add(s == 0 ? 0.f : TourCum.Last() + FVector::Dist(TourSamples[s], TourSamples[s - 1]));
	}
	TourLen = TourCum.Last();
	TourLoops = FMath::Max(1, Loops); TourLoop = 0; TourSpeed = FMath::Clamp(SpeedMs, .2f, 30.f) * 100.f;
	TourDist = 0.f; TourT = 0.f; TourSecs.Reset(); TourSecPos.Reset(); SecFrames = 0; SecStart = FPlatformTime::Seconds();
	TourGpu = TourRender = TourGame = 0.0; TourSamplesN = 0;
	bTouring = true;
	UE_LOG(LogSmoke, Log, TEXT("smoke: tour: %d loops at %.1f m/s, path %.0f m over x %.0f..%.0f y %.0f..%.0f m, map %s"),
		TourLoops, TourSpeed / 100.f, TourLen / 100.f, X0 / 100, X1 / 100, Y0 / 100, Y1 / 100, *GetWorld()->GetMapName());
}

FVector ASmokePawn::TourAt(float Dist) const
{
	Dist = FMath::Fmod(Dist, TourLen);
	int32 Lo = 0, Hi = TourCum.Num() - 1;
	while (Hi - Lo > 1) { const int32 M = (Lo + Hi) / 2; (TourCum[M] <= Dist ? Lo : Hi) = M; }
	const float Seg = TourCum[Hi] - TourCum[Lo];
	return FMath::Lerp(TourSamples[Lo], TourSamples[Hi], Seg > 0 ? (Dist - TourCum[Lo]) / Seg : 0.f);
}

void ASmokePawn::TourTick(float DeltaSeconds, double Now)
{
	if (!Camera || TourLen <= 0.f) { bTouring = false; return; }
	TourDist += TourSpeed * DeltaSeconds; TourT += DeltaSeconds;
	const FVector Here = TourAt(TourDist), Ahead = TourAt(TourDist + 100.f);
	// Look around the way a person does: sway the yaw off the heading, nod the pitch.
	const float Heading = (Ahead - Here).Rotation().Yaw;
	const FRotator Rot(25.f * FMath::Sin(.55f * TourT), Heading + 70.f * FMath::Sin(.9f * TourT) + 20.f * FMath::Sin(2.3f * TourT), 0.f);
	SetActorRotation(Rot);
	SetActorLocation(GetActorLocation() + (Here - Camera->GetComponentLocation()));
	TourHeadPos = Here;
	SecFrames++;
	TourGpu += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)); TourRender += FPlatformTime::ToMilliseconds(GRenderThreadTime);
	TourGame += FPlatformTime::ToMilliseconds(GGameThreadTime); TourSamplesN++;
	if (Now - SecStart >= 1.0)
	{
		TourSecs.Add(SecFrames / (Now - SecStart)); TourSecPos.Add(Here);
		SecFrames = 0; SecStart = Now;
	}
	if (TourDist >= TourLen * (TourLoop + 1))
	{
		TourSummary(Now);
		if (++TourLoop >= TourLoops) { bTouring = false; UE_LOG(LogSmoke, Log, TEXT("smoke: tour: done")); }
	}
}

void ASmokePawn::TourSummary(double Now)
{
	if (TourSecs.Num() == 0) return;
	TArray<float> S = TourSecs; S.Sort();
	double Sum = 0; for (float F : S) Sum += F;
	int32 Under85 = 0, Under60 = 0, Worst = 0;
	for (int32 i = 0; i < TourSecs.Num(); ++i) { Under85 += TourSecs[i] < 85.f; Under60 += TourSecs[i] < 60.f; if (TourSecs[i] < TourSecs[Worst]) Worst = i; }
	const double Nn = FMath::Max(1, TourSamplesN);
	UE_LOG(LogSmoke, Log, TEXT("smoke: tour: loop %d %d s: fps mean %.1f p5 %.0f min %.0f, %d s under 85, %d under 60; gpu %.2f render %.2f game %.2f ms; worst %.0f fps at %s, map %s"),
		TourLoop + 1, TourSecs.Num(), Sum / S.Num(), S[(int32)(.05f * (S.Num() - 1))], S[0], Under85, Under60,
		TourGpu / Nn, TourRender / Nn, TourGame / Nn, TourSecs[Worst], *(TourSecPos[Worst] / 100.0).ToString(), *GetWorld()->GetMapName());
	TourSecs.Reset(); TourSecPos.Reset(); TourGpu = TourRender = TourGame = 0.0; TourSamplesN = 0;
}

ASmokeGameMode::ASmokeGameMode()
{
	DefaultPawnClass = ASmokePawn::StaticClass();
}

void ASmokeGameMode::BeginPlay()
{
	Super::BeginPlay();
	UE_LOG(LogSmoke, Log, TEXT("smoke: game mode up, map %s"), *GetWorld()->GetMapName());
}

// ---------------------------------------------------------------------------------------------------------------
// SMOKE_PANELS=1: menus the way Unreal draws them in VR (frame-lab, 2026-10-25)

namespace
{
	FLinearColor Srgb(float R, float G, float B, float A = 1.f)
	{
		// The Godot panel's colours are sRGB; UMG tints are linear.
		return FLinearColor::FromSRGBColor(FColor(FMath::RoundToInt(R * 255), FMath::RoundToInt(G * 255), FMath::RoundToInt(B * 255), FMath::RoundToInt(A * 255)));
	}
}

TSharedRef<SWidget> USmokePanelWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
		WidgetTree->RootWidget = Canvas;
		auto Box = [&](const FLinearColor& Color, const FVector2D& At, const FVector2D& Extent)
		{
			UImage* Image = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
			Image->SetColorAndOpacity(Color);
			UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Image);
			PanelSlot->SetPosition(At);
			PanelSlot->SetSize(Extent);
		};
		const double W = Size.X, H = Size.Y, B = 6.0;
		Box(Srgb(0.08f, 0.09f, 0.12f, 0.85f), FVector2D(0, 0), Size);
		const FLinearColor Border = Srgb(0.9f, 0.9f, 0.9f);
		Box(Border, FVector2D(0, 0), FVector2D(W, B));
		Box(Border, FVector2D(0, H - B), FVector2D(W, B));
		Box(Border, FVector2D(0, 0), FVector2D(B, H));
		Box(Border, FVector2D(W - B, 0), FVector2D(B, H));
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
		Text->SetText(FText::FromString(Title));
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = 48;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		UCanvasPanelSlot* TextSlot = Canvas->AddChildToCanvas(Text);
		TextSlot->SetPosition(FVector2D(24, 16));
		TextSlot->SetAutoSize(true);
		const FLinearColor Rows[3] = { Srgb(0.9f, 0.2f, 0.2f), Srgb(0.2f, 0.8f, 0.3f), Srgb(0.25f, 0.45f, 0.95f) };
		for (int32 I = 0; I < 3; ++I)
		{
			Box(Rows[I], FVector2D(24, 96 + I * 90), FVector2D(W - 48, 64));
		}
	}
	return Super::RebuildWidget();
}

void ASmokePawn::MakePanels()
{
	// The two ways a menu reaches VR in Unreal: a WidgetComponent is a world mesh drawn into the eye buffer (it already
	// works); a Stereo Layer component hands its texture to the XR plugin as a layer of its own (IStereoLayers). This is
	// the second, fed from the first: each widget component draws its UMG panel into a render target and stays out of
	// the scene, and a world-locked Stereo Layer shows that target. Layout as Godot's xr-smoke panels mode: a 1.0 x 0.6 m
	// quad 1.5 m ahead at 1.5 m, and a cylinder of radius 1.5 m, 60 degrees of arc, aspect 2, 60 degrees to the left.
	struct FPanelSpec { const TCHAR* Title; FVector2D Pixels; bool bCylinder; FVector At; float Yaw; int32 Priority; };
	const FPanelSpec Specs[2] = {
		{ TEXT("QUAD LAYER"), FVector2D(1000, 600), false, FVector(150, 0, 150), 0.f, 1 },
		{ TEXT("CYLINDER LAYER"), FVector2D(1200, 600), true, FVector(0, 0, 150), -60.f, 2 },
	};
	FByteProperty* TypeProp = FindFProperty<FByteProperty>(UStereoLayerComponent::StaticClass(), TEXT("StereoLayerType"));
	FObjectProperty* ShapeProp = FindFProperty<FObjectProperty>(UStereoLayerComponent::StaticClass(), TEXT("Shape"));
	for (int32 I = 0; I < 2; ++I)
	{
		const FPanelSpec& S = Specs[I];
		USmokePanelWidget* Panel = CreateWidget<USmokePanelWidget>(GetWorld(), USmokePanelWidget::StaticClass());
		Panel->Title = S.Title;
		Panel->Size = S.Pixels;

		UWidgetComponent* Widget = NewObject<UWidgetComponent>(this, *FString::Printf(TEXT("PanelWidget%d"), I));
		Widget->SetupAttachment(Root);
		Widget->SetRelativeLocation(S.At);
		Widget->SetWidgetSpace(EWidgetSpace::World);
		Widget->SetDrawSize(S.Pixels);
		Widget->SetBlendMode(EWidgetBlendMode::Transparent);
		Widget->SetBackgroundColor(FLinearColor::Transparent);
		Widget->SetTickWhenOffscreen(true); // it is never on screen: drawn for the layer only
		Widget->SetRenderInMainPass(false);
		Widget->SetRenderInDepthPass(false);
		Widget->SetCastShadow(false);
		Widget->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Widget->SetWidget(Panel);
		Widget->RegisterComponent();
		PanelWidgets[I] = Widget;

		UStereoLayerComponent* Layer = NewObject<UStereoLayerComponent>(this, *FString::Printf(TEXT("PanelLayer%d"), I));
		Layer->SetupAttachment(Root);
		Layer->SetRelativeLocationAndRotation(S.At, FRotator(0.f, S.Yaw, 0.f));
		Layer->bLiveTexture = true;
		Layer->SetPriority(S.Priority);
		// StereoLayerType and Shape are protected with no setter (editor-only in Epic's intent): reflection, as the details panel sets them.
		if (TypeProp)
		{
			TypeProp->SetPropertyValue_InContainer(Layer, (uint8)SLT_WorldLocked);
		}
		if (S.bCylinder)
		{
			UStereoLayerShapeCylinder* Cylinder = NewObject<UStereoLayerShapeCylinder>(Layer);
			Cylinder->Radius = 150.f;
			Cylinder->OverlayArc = 150.f * PI / 3.f;            // 60 degrees
			Cylinder->Height = FMath::RoundToInt(Cylinder->OverlayArc / 2.f); // aspect 2, as the 1200x600 texture
			if (ShapeProp)
			{
				ShapeProp->SetObjectPropertyValue_InContainer(Layer, Cylinder);
			}
		}
		else
		{
			Layer->SetQuadSize(FVector2D(100.f, 60.f));
		}
		Layer->RegisterComponent();
		PanelLayers[I] = Layer;
		UE_LOG(LogSmoke, Log, TEXT("smoke: panel %s, %s stereo layer at %s yaw %.0f, priority %d (type %s, shape %s)"), S.Title,
			S.bCylinder ? TEXT("cylinder r 150 cm arc 157 cm") : TEXT("quad 100x60 cm"), *S.At.ToString(), S.Yaw, S.Priority,
			TypeProp ? TEXT("world-locked") : TEXT("NOT SET"), ShapeProp || !S.bCylinder ? TEXT("ok") : TEXT("NOT SET"));
	}
}

void ASmokePawn::PanelsTick()
{
	if (bPanelsToggle && PanelLayers[0] && PanelLayers[1])
	{
		const double Now = FPlatformTime::Seconds();
		if (PanelsToggledAt == 0.0)
		{
			PanelsToggledAt = Now;
		}
		else if (Now - PanelsToggledAt > 10.0)
		{
			PanelsToggledAt = Now;
			const bool bShow = !PanelLayers[0]->IsVisible();
			PanelLayers[0]->SetVisibility(bShow);
			PanelLayers[1]->SetVisibility(bShow);
			UE_LOG(LogSmoke, Log, TEXT("smoke: panels %s"), bShow ? TEXT("shown") : TEXT("hidden"));
		}
	}
	// The widget component makes its render target on its first draw; the layer gets it then.
	for (int32 I = 0; I < 2; ++I)
	{
		if (bPanelTextureSet[I] || !PanelWidgets[I] || !PanelLayers[I])
		{
			continue;
		}
		if (UTextureRenderTarget2D* Target = PanelWidgets[I]->GetRenderTarget())
		{
			bPanelTextureSet[I] = true;
			PanelLayers[I]->SetTexture(Target);
			UE_LOG(LogSmoke, Log, TEXT("smoke: panel layer %d texture %dx%d %s%s"), I, Target->SizeX, Target->SizeY,
				GetPixelFormatString(Target->GetFormat()), Target->SRGB ? TEXT(" sRGB") : TEXT(""));
		}
	}
}
