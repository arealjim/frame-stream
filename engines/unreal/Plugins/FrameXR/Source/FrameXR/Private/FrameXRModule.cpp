// FrameXR: Unreal on macOS -> OXRSys (OpenXR + Metal) -> Steam Frame.
// Adapted from tether-unreal, Copyright (c) 2026 Tether contributors, MIT License (see LICENSE-tether.txt).

#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "IHeadMountedDisplayModule.h"
#include "IInputDeviceModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "FrameXRSession.h"
#include "FrameXRHMD.h"
#include "FrameXRInput.h"

DEFINE_LOG_CATEGORY(LogFrameXR);

namespace
{
	/** Default [HMDPluginPriority] for FrameXR: above OpenXRHMD (20), OculusHMD (40) and SteamVR (30). */
	constexpr float DefaultPriority = 60.f;

	/** Creates the controller device once the platform polls input devices. */
	class FFrameXRInputDeviceFactory : public IInputDeviceModule
	{
	public:
		virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override
		{
			FFrameXRInput::RegisterKeys();
			return MakeShared<FFrameXRInput>(InMessageHandler);
		}
	};

	void LogStatus()
	{
		const FFrameXRSession& Session = FFrameXRSession::Get();
		if (!Session.IsLoaderLoaded())
		{
			UE_LOG(LogFrameXR, Display, TEXT("Status: dormant (no OpenXR loader)"));
			return;
		}
		if (!Session.HasInstance())
		{
			UE_LOG(LogFrameXR, Display, TEXT("Status: loader %s, no instance (is XR_RUNTIME_JSON set and the runtime reachable?)"), *Session.GetLoaderPath());
			return;
		}
		UE_LOG(LogFrameXR, Display, TEXT("Status: runtime %s, %dx%d per eye, session %s, running %s, display period %.2f ms"),
			*Session.GetRuntimeName(), Session.GetEyeSize().X, Session.GetEyeSize().Y,
			Session.HasSession() ? TEXT("yes") : TEXT("no"), Session.IsRunning() ? TEXT("yes") : TEXT("no"), Session.GetDisplayPeriodMs());
	}

	FAutoConsoleCommand StatusCommand(TEXT("FrameXR.Status"), TEXT("Logs the OpenXR loader, runtime and session state."), FConsoleCommandDelegate::CreateStatic(&LogStatus));
}

class FFrameXRModule : public IHeadMountedDisplayModule
{
public:
	virtual void StartupModule() override
	{
		IHeadMountedDisplayModule::StartupModule();

		if (!FFrameXRSession::Get().LoadLoader())
		{
			return;
		}

		// Win HMD selection on Mac unless the project configured a priority itself.
		float Priority = 0.f;
		if (GConfig && !GConfig->GetFloat(TEXT("HMDPluginPriority"), *GetModuleKeyName(), Priority, GEngineIni))
		{
			GConfig->SetFloat(TEXT("HMDPluginPriority"), *GetModuleKeyName(), DefaultPriority, GEngineIni);
		}

		IModularFeatures::Get().RegisterModularFeature(IInputDeviceModule::GetModularFeatureName(), &InputDeviceFactory);
		bInputRegistered = true;
	}

	virtual void ShutdownModule() override
	{
		if (bInputRegistered)
		{
			IModularFeatures::Get().UnregisterModularFeature(IInputDeviceModule::GetModularFeatureName(), &InputDeviceFactory);
			bInputRegistered = false;
		}
		IHeadMountedDisplayModule::ShutdownModule();
	}

	virtual FString GetModuleKeyName() const override { return TEXT("FrameXR"); }
	virtual FString GetDeviceSystemName() override { return FFrameXRHMD::SystemName.ToString(); }

	/** "Connected" means the runtime answered: an instance and an HMD system exist. */
	virtual bool IsHMDConnected() override { return FFrameXRSession::Get().CreateInstance(); }

	virtual TSharedPtr<IXRTrackingSystem, ESPMode::ThreadSafe> CreateTrackingSystem() override
	{
		if (!FFrameXRSession::Get().CreateInstance())
		{
			return nullptr;
		}
		UE_LOG(LogFrameXR, Log, TEXT("Creating the FrameXR head-mounted display"));
		LogStatus();
		return MakeShared<FFrameXRHMD, ESPMode::ThreadSafe>();
	}

private:
	FFrameXRInputDeviceFactory InputDeviceFactory;
	bool bInputRegistered = false;
};

IMPLEMENT_MODULE(FFrameXRModule, FrameXR)
