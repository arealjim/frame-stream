// frame-lab unreal-vr game module (replaces mkproject.sh's copy of the smoke's empty module, 2026-10-21).
//
// FRAMELAB_UE_EXEC="<seconds>:<console command>;<seconds>:<command>;..." runs each console command that many seconds
// after the game world begins play, and logs "frame-lab exec at T s: CMD" before it. It is how the 90 Hz budget work
// changes one renderer cvar per run, or takes a ProfileGPU dump at a steady moment, without editing an ini or relaunching
// twice:  FRAMELAB_UE_EXEC="0:r.ReflectionMethod 0;20:ProfileGPU"
// Unset, the module does nothing.
#include "UnrealVR.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

class FUnrealVRModule : public FDefaultGameModuleImpl
{
	FTSTicker::FDelegateHandle Handle;
	TArray<TPair<double, FString>> Queue;
	double Start = 0.0;

	virtual void StartupModule() override
	{
		const FString Spec = FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMELAB_UE_EXEC"));
		if (Spec.IsEmpty()) return;
		TArray<FString> Items; Spec.ParseIntoArray(Items, TEXT(";"));
		for (const FString& Item : Items)
		{
			FString When, Cmd;
			if (Item.Split(TEXT(":"), &When, &Cmd)) Queue.Add({ FCString::Atod(*When), Cmd.TrimStartAndEnd() });
		}
		Queue.StableSort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key < B.Key; });
		UE_LOG(LogTemp, Display, TEXT("frame-lab exec: %d command(s) queued from FRAMELAB_UE_EXEC"), Queue.Num());
		Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUnrealVRModule::Tick));
	}

	bool Tick(float)
	{
		if (!GEngine) return true;
		UWorld* World = nullptr;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
			if ((C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE) && C.World()) World = C.World();
		if (!World || !World->HasBegunPlay()) return true;
		if (Start == 0.0) Start = FPlatformTime::Seconds();
		const double T = FPlatformTime::Seconds() - Start;
		while (Queue.Num() && Queue[0].Key <= T)
		{
			UE_LOG(LogTemp, Display, TEXT("frame-lab exec at %.1f s: %s"), T, *Queue[0].Value);
			GEngine->Exec(World, *Queue[0].Value);
			Queue.RemoveAt(0);
		}
		return Queue.Num() > 0;
	}

	virtual void ShutdownModule() override
	{
		if (Handle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Handle);
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FUnrealVRModule, UnrealVR, "UnrealVR");
