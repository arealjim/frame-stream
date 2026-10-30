// frame-lab unreal-smoke: the scene that proves the chain. A floor-anchored pawn whose camera follows the
// headset, and a 10 cm cube 50 cm ahead at eye height, the same probe as the Godot and Unity smokes. Each controller
// is a small block at its grip with a 1 m ray along its aim, and every button press is logged ("smoke: key").
// One-handed locomotion, either stick: push up/down to walk where the head points, flick left/right to snap-turn.
// SMOKE_SPAWN="x,y,z[,yaw]" (cm, degrees) starts the pawn there instead of at the player start. The cube shows only in
// the smoke's own map unless SMOKE_CUBE=1 (or =0 to hide it there too).
// Grip and trigger show on the blocks: the trigger turns a block from grey to bright green, the grip squashes it to half
// its thickness, both analog. A 12 cm orange cube sits in reach, 40 cm ahead and 35 cm under the eyes: grip near it to
// pick it up, let go to leave it where it is (no physics); the reference cube can be picked up the same way, one cube per hand.
// The grab cube shows where the reference cube does unless SMOKE_GRAB=1/0;
// SMOKE_GRAB_REACH (cm, default 15) is how near the block must be.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "Blueprint/UserWidget.h"
#include "SmokeGameMode.generated.h"

class UCameraComponent;
class UMaterialInstanceDynamic;
class AStaticMeshActor;
class UMotionControllerComponent;
class UStaticMeshComponent;
class UStereoLayerComponent;
class UWidgetComponent;

/**
 * SMOKE_PANELS=1: the menu panel every engine's panel test draws (Godot's xr-smoke panels mode): a dark panel, a white
 * border, the title top left, then red, green and blue rows top to bottom. Built in C++ as a UMG tree, like a menu.
 */
UCLASS()
class USmokePanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	FString Title;
	FVector2D Size = FVector2D(1000.0, 600.0);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
};

UCLASS()
class ASmokePawn : public APawn
{
	GENERATED_BODY()

public:
	ASmokePawn();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<UCameraComponent> Camera;

	void PlaceCube();
	/** Grip and trigger on the blocks, the grab cube, and a log line on each press edge. */
	void HandsTick();
	void PlaceGrabCube();
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Blocks[2];
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> BlockMaterials[2];
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> GrabCube;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GrabMaterial;
	FName ColorParam;          // the basic shape material's colour parameter, found at start
	bool bGrabWanted = true;
	float GrabReach = 15.f;    // SMOKE_GRAB_REACH, cm: from the block's centre to the cube's
	UPROPERTY()
	TObjectPtr<AStaticMeshActor> HeldBy[2]; // what each hand holds: the grab cube, the reference cube, or nothing
	bool IsHeld(const AStaticMeshActor* A) const { return A && (HeldBy[0].Get() == A || HeldBy[1].Get() == A); }
	bool bTriggerDown[2] = {};
	bool bGripDown[2] = {};
	double GrabLostSince = 0.0;
	/** SMOKE_PANELS=1: a quad and a cylinder Stereo Layer, each showing a UMG panel rendered by a hidden widget component. */
	void MakePanels();
	void PanelsTick();
	UPROPERTY()
	TObjectPtr<UWidgetComponent> PanelWidgets[2];
	UPROPERTY()
	TObjectPtr<UStereoLayerComponent> PanelLayers[2];
	bool bPanelTextureSet[2] = {};
	bool bPanelsToggle = false;
	double PanelsToggledAt = 0.0;
	void LogKeys();
	void Locomote(float DeltaSeconds);
	bool FloorUnder(const FVector& At, float& OutZ) const;

	bool bCubeWanted = true;
	bool bMoveByTick = false;  // SMOKE_MOVE_DT=tick: step by DeltaSeconds instead of the display period
	float MoveHz = 90.f;       // SMOKE_HZ: the display rate the step assumes
	float FloorDeadband = 2.f; // SMOKE_FLOOR_DEADBAND, cm: floor changes smaller than this are not followed
	struct FWalkStats
	{
		int32 Ticks = 0, Blocked = 0, DzMoves = 0;
		float DtMin = 1.f, DtMax = 0.f, DtSum = 0.f, StepSum = 0.f, DzMax = 0.f;
		double TraceSum = 0.0, TraceMax = 0.0;
	} Walk;
	bool bSnapArmed = true;
	float SnapDegrees = 30.f;
	float WalkSpeed = 600.f; // cm/s at full stick (SMOKE_WALK_SPEED; 4x the first 150, 2026-10-24)
	bool bFly = true;        // SMOKE_FLY=0: walk level with floor follow and wall stops; on: forward follows the gaze, pitch included

	UPROPERTY()
	TObjectPtr<UMotionControllerComponent> Grips[2];

	UPROPERTY()
	TObjectPtr<UMotionControllerComponent> Aims[2];

	TArray<FKey> WatchedKeys;
	TSet<FName> KeysDown;
	bool bControllersSeen[2] = {};

	UPROPERTY()
	TObjectPtr<AStaticMeshActor> Cube;

	double NextReport = 0.0;
	double PerfSince = 0.0;  // the perf line window
	int32 PerfFrames = 0;
	double PerfMaxDt = 0.0;
	double StartTime = 0.0;
	double LostSince = 0.0;
	bool bProfiled = false;
	bool bCentred = false;
	bool bProfiledStreaming = false;
	double CentredAt = 0.0;
	bool bPlaceNext = false;

	// The tour (frame-lab, 2026-10-25: simulate a wearer flying around inside the scene): the same closed path as the
	// Unity smoke's, so the engines' numbers compare. Console line "smoke.tour [loops] [m/s]" / "smoke.tour stop".
	void TourStart(int32 Loops, float SpeedMs);
	void TourTick(float DeltaSeconds, double Now);
	void TourSummary(double Now);
	FVector TourAt(float Dist) const;
	TArray<FVector> TourPts;  // control points
	TArray<float> TourCum;    // cumulative arc length of the sampled spline
	TArray<FVector> TourSamples;
	bool bTouring = false;
	int32 TourLoops = 1, TourLoop = 0;
	float TourSpeed = 400.f, TourDist = 0.f, TourLen = 0.f, TourT = 0.f;
	TArray<float> TourSecs;   // per-second fps
	TArray<FVector> TourSecPos;
	int32 SecFrames = 0;
	double SecStart = 0.0, TourGpu = 0.0, TourRender = 0.0, TourGame = 0.0;
	int32 TourSamplesN = 0;
	FVector TourHeadPos = FVector::ZeroVector;
};

UCLASS()
class ASmokeGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASmokeGameMode();
	virtual void BeginPlay() override;
};
