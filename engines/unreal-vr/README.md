# unreal-vr: Epic's VR Template on FrameXR → OXRSys → Steam Frame

Epic's own **VR Template** (`Templates/TP_VirtualRealityBP`, Blueprint) as a project on the FrameXR plugin. It is
the lit lobby with teleport, snap turn, grabbable cubes, a pistol, a VR menu, XR mannequin hands and the spectator
camera. It is the larger Unreal scene for this chain. The small one is `../unreal`; the Unity counterpart is
`../unity`.

**Where it stands.** Both targets build clean. 18 of the 19 project Blueprints compile; `BP_Passthrough` cannot,
because it calls Epic's OpenXR plugin (see "Passthrough does not work"). The lobby holds 90 Hz with MSAA 2x. Worn
over USB-C, 1080 of 1080 frames arrived fresh over a 12 s window. The wearer's verdict: smooth, hands show, teleport
and grab work, edges clean with no shimmer.

## What works and what does not

| | state |
|---|---|
| 90 Hz | yes, with `r.MSAACount=2` (which `mkproject.sh` sets). MSAA 4x, as Epic ships it, gives about 57 fps |
| worn, USB-C, MSAA 2x | 1080 of 1080 frames fresh over 12 s; smooth, no shimmer on edges |
| head tracking, floor height | yes. The pawn asks for `LocalFloor`; OXRSys has no LOCAL_FLOOR, so FrameXR uses STAGE (origin on the floor, room forward). The log says `Floor origin space: STAGE (LOCAL_FLOOR fallback)`. You may face the wrong way until a recentre |
| controllers, hands | yes. FrameXR emits the full OculusTouch key set and registers LeftAim/LeftGrip/RightAim/RightGrip. The mannequin hands show, worn |
| teleport, snap turn, grab | teleport and grab confirmed worn |
| hand tracking (`IMC_Hands`) | no source: Epic's OpenXRHandTracking is off. Not an error |
| the menu's Passthrough button | does nothing. See "Passthrough does not work" |
| the menu's "Real Life" button | quits the app; after 5 s the headset ends its session and SteamVR shows its home (`docs/troubleshooting.md`) |

## What the project is

`mkproject.sh` (run on the Mac; idempotent) does by hand what the editor's New Project dialog does for this
template, following `Config/TemplateDefs.ini`. It writes the project to `~/src/unreal-vr`.

- **Content.** The template's feature pack (`FeaturePacks/TP_VirtualRealityBP.upack`, 139 KB) holds only `config.ini`
  and two thumbnails. The scene is the five SharedContentPacks it mounts. Each is copied to
  `Content/<DestinationFilesFolder>` as its `FeaturePack/manifest.json` names it:

  | pack (detail level) | → Content/ | files | what |
  |---|---|---|---|
  | XRFramework (Standard) | XRFramework | 69 | the map `Levels/L_XRTemplate` (+ `_Lighting` sublevel, built data), `BP_XRGameMode`, `BP_XRPawn`, grab, teleport, pistol, menu, passthrough, enhanced-input actions + contexts, VFX |
  | LevelPrototyping (High) | LevelPrototyping | 29 | the lobby's meshes and grid materials, door, jump pad, wobble target |
  | XRMannequins (Standard) | XRMannequins | 53 | the hands (`BP_MannequinsXR`, `ABP_MannequinsXR`) |
  | Weapons (Standard) | Weapons | 27 | the pistol mesh + textures |
  | VRSpectator (Standard) | VRSpectator | 11 | the spectator camera |

  189 content files, about 55 MB. They come from your engine install and are not in this repo.
- **Config.** The template's ini files, with `TP_VirtualRealityBP` renamed to `UnrealVR`. `Config/config.ini` (legacy
  `ActionMappings`; the pawn uses Enhanced Input) and the thumbnails are on the template's own ignore list. The
  template's `Config/Mac/MacEngine.ini` turns instanced stereo and mobile multiview off for Metal.
- **A minimal C++ module**, `Source/UnrealVR`. There is no game mode of its own: the template's `BP_XRGameMode` and
  `BP_XRPawn` stay, so the map, teleport, grab and menu all run through FrameXR's HMD and motion controllers. The
  module body is this directory's `Source/UnrealVR/UnrealVR.cpp`, which adds `FRAMELAB_UE_EXEC` (see "Measuring").
  Run `mkproject.sh` from this directory so it finds that file.
- **`Plugins/FrameXR`**, copied from `~/src/unreal-smoke` (sources, vendored OpenXR 1.1.57 headers, the loader dylib).
- **`UnrealVR.uproject`**: the template's plugin list with **OpenXR, OpenXREyeTracker and OpenXRHandTracking off**
  (Epic's OpenXRHMD has no Metal path and cannot load on macOS; `../unreal/README.md` → "Why a plugin"),
  PICOController off (Win64/Linux/Android only), **XRBase and FrameXR on**. TargetPlatforms: Mac.
- **`DefaultEditorSettings.ini`** from the smoke project: crash reporter and usage data off.
- **Appended to `DefaultEngine.ini`:**
  - `[HMDPluginPriority] FrameXR=60`
  - `[/Script/UdpMessaging.UdpMessagingSettings] EnableTransport=False` (UDP messaging off)
  - `[FrameXR] PixelDensity=1.0`
  - `[SystemSettings] r.ScreenPercentage.Default.VR.Mode=0`, `r.ScreenPercentage=80`
  - `[/Script/Engine.RendererSettings] r.MSAACount=2`: the setting that reaches 90 Hz
  - the smoke project's remaining renderer cuts (`r.ReflectionMethod=0`, `r.DefaultFeature.Bloom=False`,
    `r.VolumetricCloud=0`, `r.VolumetricFog=0`, `r.Nanite=0`), commented out. 90 Hz does not need them.

The template already ships a VR budget of its own: forward shading, MSAA (`r.AntiAliasingMethod=3`), no Lumen
(`r.DynamicGlobalIlluminationMethod=0`), `r.Shadow.Virtual.Enable=0`, built static lighting. Reflections
(`r.ReflectionMethod=1`), bloom and mesh distance fields are on.

## Set it up

On the Mac. You need `~/src/unreal-smoke` built first, with the FrameXR plugin and its loader dylib in place
(`../unreal/README.md` → "Run it"). `mkproject.sh` stops if the plugin is missing and exits without changes
if `~/src/unreal-vr` already exists.

```sh
bash mkproject.sh                              # -> ~/src/unreal-vr, prints the file list
E="/Users/Shared/Epic Games/UE_5.8"; P=$HOME/src/unreal-vr
nice -n 15 "$E/Engine/Build/BatchFiles/Mac/Build.sh" UnrealVREditor Mac Development -project=$P/UnrealVR.uproject -waitmutex
nice -n 15 "$E/Engine/Build/BatchFiles/Mac/Build.sh" UnrealVR       Mac Development -project=$P/UnrealVR.uproject -waitmutex
```

On a base M5 the editor target built in 19.5 s and the game target in 21.1 s, 0 errors each.

**Silence the "missing plugins" prompt.** The template's `BP_AssetGuideline` (on `BP_XRPawn`'s camera) asks on every
load to enable Epic's OpenXR plugins. They have no Mac build, so enabling them can never work. After the first
build, empty its list (this enables Python for this one run only; the streamed project never loads it):

```sh
"$E/Engine/Binaries/Mac/UnrealEditor-Cmd" $P/UnrealVR.uproject -run=pythonscript -script=$PWD/guideline_off.py \
  -EnablePlugins=PythonScriptPlugin -unattended -nullrhi
```

`guideline_check.py`, run the same way, prints every guideline's list. All `[]` means no prompt.

**Warm the shader cache before the first run.** Nothing has compiled a shader yet. Without this step the first
`-game` launch compiles them on the CPU before its first frame, which takes far longer than `frame-stream up` waits
for the runtime, so `up` stops with an error. Run this on an otherwise idle Mac (CPU only, no headset, no runtime):

```sh
env -u XR_RUNTIME_JSON nice -n 15 "$E/Engine/Binaries/Mac/UnrealEditor-Cmd" $P/UnrealVR.uproject -run=DerivedDataCache \
  -fill -TargetPlatform=Mac -nohmd -unattended -nopause -abslog=$HOME/Library/Logs/frame-stream/ue-vr-ddc.log
```

On a base M5 with a cold cache it took **27 minutes** (1610 s): 28,559 shader jobs, about 14-18 jobs/s on 12
ShaderCompileWorkers. After it, the launch compiles no shaders.

**It exits 1, and that is expected.** The errors are Blueprint errors, not shaders: 90 of 93 `Error:` lines are
`BP_Passthrough`, the other 3 are `LogChaos: Missing registered node type (FSkeletalMeshToMeshDataflowNode_v2)`.

**Optional: compile every Blueprint headless**, to check the project without a headset. `UEngine::InitializeHMDDevice`
is skipped under a commandlet, `-nohmd` and `-nullrhi` say the same, and with `XR_RUNTIME_JSON` unset the loader finds
no runtime, so the runtime's UDP ports 9944-9946 stay untouched:

```sh
env -u XR_RUNTIME_JSON nice -n 15 "$E/Engine/Binaries/Mac/UnrealEditor-Cmd" $P/UnrealVR.uproject -run=CompileAllBlueprints \
  -nohmd -nullrhi -unattended -nopause -NoSplash -abslog=$HOME/Library/Logs/frame-stream/ue-vr-blueprints.log
```

Expect exit 36: 18 of 19 project Blueprints compile, `BP_Passthrough` fails with 36 fatal issues, 0 warnings. The 19
are BP_WobbleTarget, BP_JumpPad, BP_DoorFrame, VRSpectator, BP_Menu, BP_TeleportVisualizer, BP_Pistol,
BP_GrabComponent, BP_InputModifier_XAxisPositiveOnly, **BP_Passthrough**, BPI_PawnAnim, BP_MannequinsXR,
BP_XRGameMode, BP_AssetGuideline, BP_XRPawn, BP_Grabbable_SmallCube, BP_Projectile, WBP_Menu, ABP_MannequinsXR.
`BP_XRPawn` compiles clean: nothing in it imports OpenXRHandTracking, and `SetTrackingOrigin(LocalFloor)` is a
Blueprint library call that FrameXR answers.

## Run it

```sh
FRAME_STREAM_UNREAL_PROJECT=$HOME/src/unreal-vr/UnrealVR.uproject bin/frame-stream up --app unreal
```

Add `--mac-ip <mac-ip>` when not on the USB-C tether. The measurements below also used
`--bitrate 50 --ffe medium_plus --render steam_frame_native`.

`frame-stream` launches `UnrealEditor.app` with `-game -vr -windowed -ResX=1280 -ResY=720 -NoSplash -unattended` and
writes the engine log to `~/Library/Logs/frame-stream/unreal.log`. `bStartInVR=True` in `DefaultGame.ini` is the
template's own. With a warm cache, `up` returned with the client connected in 107 s, and the map loaded in 0.44 s.

What the log shows on a good run:
- `LoadMap(/Game/XRFramework/Levels/L_XRTemplate)`, then the usual `LogFrameXR` sequence
  (`../unreal/README.md` → "What the log should say").
- `Reference spaces: view ok, local ok, local_floor missing (falls back to local), stage ok` and
  `Floor origin space: STAGE (LOCAL_FLOOR fallback)`.
- `LogBlueprint: Error` lines for `BP_Passthrough` only (34 of them at play). Harmless; see below.
- **One handled ensure** in the renderer, `DependentView == nullptr || DependentView == &View` (ShadowRendering.cpp:798,
  from `FProjectedShadowInfo::UseShadowStencilCulling`). It is a stereo shadow path the engine flags once. The game
  keeps running.

If Unreal never connects over USB-C and you run a per-app firewall (Little Snitch, LuLu), it is the firewall: see
`docs/troubleshooting.md`.

## Passthrough does not work

The menu's Passthrough button is inert, and `BP_Passthrough` does not compile. Every one of its errors has the same
cause: the Blueprint imports `/Script/OpenXRHMD`, Epic's OpenXR plugin, which is off because it has no Metal path.

```
LogLinker: Warning: BP_Passthrough.uasset: VerifyImport: Failed to find script package for import object 'Package /Script/OpenXRHMD'
LogBlueprint: Error: [Compiler] Could not find a function named "GetSupportedEnvironmentBlendModes" in 'BP_Passthrough'   (x6)
LogBlueprint: Error: [Compiler] Could not find a function named "GetEnvironmentBlendMode" ...                             (x6)
LogBlueprint: Error: [Compiler] Could not find a function named "SetEnvironmentBlendMode" ...                             (x6)
LogBlueprint: Error: [Compiler] Could not find a function named "IsCompositionLayerInvertedAlphaEnabled" ...              (x6)
  + the "in use pin no longer exists" / "type of A|B undetermined" / "Unsupported type Wildcard" fallout of those four (x12 each)
```

Those four functions are `UOpenXRBlueprintFunctionLibrary` in the OpenXRHMD module. On BeginPlay the actor reads the
supported environment blend modes, and the menu (`WBP_Menu`) switches between opaque and alpha blend. Only two things
use it: one actor placed in `L_XRTemplate`, and `WBP_Menu`.

It does not block anything. A Blueprint that fails to compile at load logs these errors and its event graph does
not run, so the lobby loads and only the passthrough button does nothing. The content is Epic's, untouched.

FrameXR has no passthrough path yet. Ways around it, none done:
- Open the map in the editor and delete the `BP_Passthrough` actor. That silences the errors; it is a content edit.
- A stand-in for `UOpenXRBlueprintFunctionLibrary` in the UnrealVR module cannot take over, because the import is by
  package name, `/Script/OpenXRHMD`.
- FrameXR could expose its environment blend as a Blueprint library with the same four function names in a module
  named OpenXRHMD. That collides with Epic's package name, so it only makes sense where Epic's plugin is absent.

## Performance: the settings that reach 90 Hz

**MSAA 4x → 2x is the whole fix.** With 2x the lobby holds 90 Hz with about 3.6 ms of GPU to spare. All numbers
below are on a base M5, headset on its stand and unworn, over the tailnet, client attached, with `ProfileGPU` read by
pass name.

| run | ProfileGPU frame | engine loop | runtime WaitFrame | encode+send | encode-queue gpu | client fresh / 8 s |
|---|---|---|---|---|---|---|
| MSAA 4x (Epic's default) | 16.89 ms | 17.43 ms (57.2 fps) | 17.48 ms, app late 58/90 | 38.4 ms | 26.5 ms | 455 / 720 |
| MSAA 4x + `r.Shadow.PerObject 0` | 16.80 ms | 17.05 ms (58.4 fps) | 17.02 ms, late 59 | 40.6 ms | 28.5 ms | 465 |
| **`r.MSAACount 2`** | **7.43 ms** | **11.06 ms (90.0 fps)** | **11.10 ms, late 0** | **17.3 ms** | 5.9 ms | **720** |
| MSAA 2x + `r.ScreenPercentage 70` | 5.89 ms | 11.05 ms (90.0) | 11.10 ms, late 0 | 16.1 ms | 4.2 ms | 720 |
| MSAA 2x from the ini, no console command | 7.35 ms | 11.08 ms (90.0) | 11.11 ms, late 0 | 17.6 ms | 6.0 ms | (client dropped off the network) |

At MSAA 4x the GPU was 100 % active at 1620 MHz, 15.6 W, CPU 2 W (`powermetrics --samplers gpu_power`). FrameXR
itself costs nothing: `xrWaitFrame` 0.01 ms, submit 0.08 ms.

What the profile says:
- **Two costs stack at 4x.** The template saturates the GPU, and the runtime's colour conversion for the encoder
  waits behind Unreal's frame on the same GPU queue. That is why encode+send reads 38 ms at 4x and 17 ms at 2x.
- **MSAA 4x was the cost everywhere**: base pass 3.1 ms, whole-scene shadow splits 2.4 + 2.5, depth prepass 2.1, all
  roughly halving at 2x. Dynamic shadow quality did not matter.
- **Per-object shadows are a start-up transient.** In about the first 15 s the grab cubes, pistols, logs and ball
  each cast one (about 2.3 ms per object per eye; a 54 ms frame). By 30 s they leave the profile by themselves (cause
  not checked). So read the GPU after 30 s, not before.
- `EditorPrimitives` (about 2.6 ms at 4x) is the editor's `-game` mode drawing editor-only primitives. A packaged
  build would not pay it.
- Every pass shows `x2`: the eyes render as two views, because the template's `MacEngine.ini` turns instanced stereo
  off for Metal. Instanced stereo is untried headroom.
- If a heavier scene needs more room, `r.ScreenPercentage=70` is the next step (5.9 ms).

One launch, each step stacked on the last, `ProfileGPU` 12 s after each: MSAA 4x 54.4 ms (inside the per-object
shadow transient) → `sg.ShadowQuality 2` 16.95 → `r.MSAACount 2` 10.51 → `r.ScreenPercentage 70` 8.45 →
`sg.ShadowQuality 0` 6.07 → `r.AllowOcclusionQueries 0` 5.86. Quote the table above, not this ladder: the same knob
read at a different moment in one launch reads higher.

## Measuring

The game module runs console commands at play from `FRAMELAB_UE_EXEC="<seconds>:<command>;..."`. `frame-stream up
--app unreal` passes every `FRAMELAB_*` variable to the editor. The 90 Hz table came from runs like this:

```sh
FRAME_STREAM_UNREAL_PROJECT=$HOME/src/unreal-vr/UnrealVR.uproject FRAMELAB_UE_EXEC="0:r.MSAACount 2;35:ProfileGPU" \
  bin/frame-stream up --app unreal --bitrate 50 --ffe medium_plus --render steam_frame_native
```

Measure with the headset client attached. A Mac-only run is not comparable: with no client the pose differs, and the
same config ran 20.6 fps. Read the FrameXR `Frame loop` timing lines or the console's `ProfileGPU`
(`SMOKE_PROFILEGPU` belongs to the smoke project's game mode and does nothing here).
