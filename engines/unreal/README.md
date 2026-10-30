# Unreal 5.8 → OXRSys → Steam Frame

This directory makes Unreal 5.8 VR run on a Mac and stream to the Steam Frame. Unreal renders both eyes. The FrameXR
plugin hands each frame to the OXRSys runtime through OpenXR over Metal. The runtime encodes it and sends it to the
headset. The chain runs at the panel rate: 90 Hz (11.11 ms a frame) with an 8.8 ms GPU frame on a base M5. The same
plugin runs Epic's VR Template (`../unreal-vr`). The Godot and Unity counterparts are `../godot` and `../unity`.

## Why a plugin

Epic's own OpenXR plugin (`Engine/Plugins/Runtime/OpenXR`) only knows D3D11, D3D12 and Vulkan. It has no
`XR_KHR_metal_enable` path, and `Engine/Source/ThirdParty/OpenXR/OpenXR.Build.cs` has no Mac branch, so on macOS it
cannot even load.

`Plugins/FrameXR/` is a head-mounted-display plugin (XRBase's `FHeadMountedDisplayBase` + `FXRRenderTargetManager`)
that speaks OpenXR itself. It dlopens the Khronos loader, binds the session to the engine's Metal command queue
(`IMetalDynamicRHI::RHIRunOnQueue`), renders both eyes side by side into its own BGRA8 targets, blits the frame into an
`XR_KHR_metal_enable` swapchain and ends the OpenXR frame. It is adapted from tether-unreal (MIT,
`Plugins/FrameXR/LICENSE-tether.txt`), which does the same for a WebRTC transport. The OpenXR parts are new.

| Piece | File |
|---|---|
| loader, instance, session, spaces, swapchain, actions, frame loop | `Plugins/FrameXR/Source/FrameXR/Private/FrameXRSession.cpp` |
| the HMD the engine sees: poses, eye rects, projection, render targets, recenter, Stereo Layers | `FrameXRHMD.cpp` |
| custom present: runs the OpenXR frame loop on the Metal submission thread | `FrameXRRenderBridge.cpp` |
| controllers → Oculus Touch and Steam Frame keys + grip/aim motion sources | `FrameXRInput.cpp` |
| module: priority 60, dormant without a loader, `FrameXR.Status` console command | `FrameXRModule.cpp` |

The frame loop follows Epic's pipelined shape. `xrWaitFrame` runs on the game thread at the top of each frame, once per
frame that will be submitted. The submission thread begins and ends that frame.

## The project

`mkproject.sh` makes `~/src/unreal-smoke/UnrealSmoke.uproject` from the engine's `Templates/TP_Blank`, renamed. It
writes the uproject with FrameXR on and Epic's OpenXR plugin off (it would compete for the HMD and has no Metal path),
and appends `Config/DefaultEngine.frame.ini` to the template's `DefaultEngine.ini`. This directory adds:

- `Source/UnrealSmoke/SmokeGameMode.*`: `ASmokeGameMode` spawns `ASmokePawn`, a floor-anchored pawn whose camera follows
  the headset (tracking origin LocalFloor).
  - On the first tracked head pose it recentres and places a 10 cm reference cube 50 cm ahead of the eyes. The cube
    comes back in front when it has been behind you or more than 2 m away for 3 s.
  - Each controller shows a 4 x 6 x 12 cm block at the grip and a 1 m ray along the aim. The trigger ramps the block
    from grey to bright green. The grip squashes it to half its 4 cm thickness. Both are analog.
  - A 12 cm orange grab cube sits 40 cm ahead and 35 cm under the eyes. A grip press with the block within 15 cm of it
    picks it up (it turns yellow and follows the hand). Letting go leaves it there, with no physics. The reference
    cube picks up the same way, one cube per hand. A grab cube left more than 3 m away or under the floor for 5 s comes
    back within reach.
  - Either thumbstick moves you: forward and back along the head's direction, left and right past 0.7 snaps a 30
    degree turn.
  - A head-pose and fps line every 5 s, and every press edge logged with its value (see "What the log should say").
- `Config/DefaultEngine.frame.ini`: `Template_Default` as the map, the game mode, `[HMDPluginPriority] FrameXR=60`, the
  editor's UDP message bus off, and the VR render budget (see "Resolution and frame rate"). Each line carries a comment
  with the measurement behind it.
- `Config/DefaultEditorSettings.ini`: unattended crash reports and usage data off (see "Known limits and gotchas").

## Set up

Everything here runs on the Mac. The engine is `/Users/Shared/Epic Games/UE_5.8` (set `FRAME_STREAM_UE_ENGINE` if yours
is elsewhere; `mkproject.sh` reads it too). The OXRSys runtime is built in `~/src/oxrsys` as the top-level README says.

```sh
E="/Users/Shared/Epic Games/UE_5.8"
# 1. the project: run mkproject.sh from this directory, then copy this directory over the result
./mkproject.sh
rsync -a --exclude mkproject.sh ./ "$HOME/src/unreal-smoke/"
# 2. the OpenXR loader, once, from the OXRSys build tree into the plugin (or set FRAMEXR_OPENXR_LOADER at run time)
cp "$HOME/src/oxrsys/build/macos-arm64/_deps/openxr-build/src/loader/libopenxr_loader.dylib" \
  "$HOME/src/unreal-smoke/Plugins/FrameXR/ThirdParty/OpenXR/lib/"
# 3. build the editor target (the plugin compiles with it; about 20 s warm, longer the first time)
nice -n 15 "$E/Engine/Build/BatchFiles/Mac/Build.sh" UnrealSmokeEditor Mac Development \
  -project="$HOME/src/unreal-smoke/UnrealSmoke.uproject" -waitmutex
# the Game target builds too (editor-only calls are behind WITH_EDITOR)
nice -n 15 "$E/Engine/Build/BatchFiles/Mac/Build.sh" UnrealSmoke Mac Development \
  -project="$HOME/src/unreal-smoke/UnrealSmoke.uproject" -waitmutex
```

Allow the engine through the Mac's application firewall before its first run. With stealth mode on, the firewall
silently drops the headset's discovery packet for any binary not on its allow list, and a running process does not pick
up a rule added after it started:

```sh
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$E/Engine/Binaries/Mac/UnrealEditor.app"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$E/Engine/Binaries/Mac/UnrealEditor.app"
```

The top-level `docs/troubleshooting.md` covers per-app firewalls (the Epic launcher's editor is ad-hoc signed) and which Mac address
the headset can reach.

## Run it

On the Mac:

```sh
bin/frame-stream up --app unreal --bitrate 50 --ffe medium_plus --render steam_frame_native
```

`--app unreal` launches the project with `-game -vr` (about 40 s) and passes every `FRAMELAB_`, `SMOKE_` and `OXRSYS_`
variable in your environment to the engine. The engine log goes to `~/Library/Logs/frame-stream/unreal.log`.

To launch by hand, use `open`: a GUI app launched any other way over ssh spins and never starts.

```sh
R="$HOME/src/oxrsys/build/macos-arm64/runtime/oxrsys-runtime.json"
open -n -a "$E/Engine/Binaries/Mac/UnrealEditor.app" --env XR_RUNTIME_JSON="$R" --args \
  "$HOME/src/unreal-smoke/UnrealSmoke.uproject" -game -vr -windowed -ResX=1280 -ResY=720 -NoSplash -unattended \
  -ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0 -abslog="$HOME/Library/Logs/frame-stream/unreal.log"
```

Then `bin/frame-stream up --app external` starts only the headset client. It first checks the runtime's status file
(`~/Library/Application Support/OXRSys/runtime_status.json`: app name, pid alive, fresh stamp).

Keep `-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0`. The engine's default silences a game whose window is not
frontmost on the Mac, and in a stream it never is.

## What the log should say

In order (`grep LogFrameXR`):

1. loader loaded, runtime extensions, runtime `OXRSys Runtime 1.3.0`, 2160x2160 per eye
2. stereo enabled, `Allocated 3 eye targets of 4320x2160`
3. `Session created on Metal queue`, swapchain format 81 (BGRA8 sRGB), 3 images, bindings accepted
4. `Session begun`, `First frame through the OpenXR session`, synchronized / visible / focused
5. a `Frame loop` timing line every 300 frames

`LogSmoke` lines carry the head pose and fps. Press edges read like `smoke: right trigger down at 0.62`,
`smoke: right grip down at 1.00 (block at ..., tracked, nearest cube grab 9 cm away)` and
`smoke: grab cube picked up by the right hand`. Other buttons log as `smoke: key <name> down/up`.

## Controllers

FrameXR suggests bindings for four interaction profiles: Oculus Touch, Valve Index, the Steam Frame's own controller
(when the runtime offers `XR_VALVE_frame_controller_interaction`) and `khr/simple_controller`.

- The Touch keys (`OculusTouch_*`) carry SteamVR's own Frame-to-Touch meaning, so a Touch input map works unchanged.
- With the Frame profile bound, the Frame's inputs that Touch has no key for arrive on their own keys, in the
  "Steam Frame" key category: `SteamFrame_Right_X_Click`, `SteamFrame_Right_Y_Click`, `SteamFrame_Right_Menu_Click`,
  `SteamFrame_Left_DPad_Up/Down/Left/Right`, `SteamFrame_Left_Bumper_Click`, `SteamFrame_Right_Bumper_Click`.
  `OculusTouch_Left_ThumbRest` and `OculusTouch_Right_ThumbRest` are added too.
- Motion sources: `LeftGrip`, `RightGrip`, `LeftAim`, `RightAim`. On the Frame the aim pose is about 40 degrees from the
  grip, so point rays along the aim, not along the grip.
- With no headset client connected, OXRSys reports `khr/simple_controller`.

## Menus and panels: Stereo Layers as OpenXR layers

Unreal puts a menu in VR one of two ways:

- **UMG on a WidgetComponent** (world space): a mesh in the scene, drawn into the eye buffer like any other. It needs
  nothing from the XR plugin.
- **A Stereo Layer component** (IStereoLayers): the texture (often a widget's render target) goes to the XR plugin as
  a layer of its own.

FrameXR handles Stereo Layers one of two ways, chosen once at start by `FRAMEXR_STEREO_LAYERS`:

- **native**: each visible quad or cylinder layer becomes an OpenXR `XrCompositionLayerQuad` or
  `XrCompositionLayerCylinderKHR`, composited by the runtime. Poses are made as Epic's OpenXR plugin makes them:
  world-locked through the inverse tracking-to-world transform, face-locked in VIEW space. Each layer gets its own Metal
  swapchain in the texture's format. The texture is copied in every frame (one blit for all layers), and the layers are
  submitted after the projection layer, lowest priority first. Cylinders are sent only if the runtime offers
  `XR_KHR_composition_layer_cylinder`.
- **emulated**: the engine's fallback `FDefaultStereoLayers` draws every layer into the eye buffer as a flat quad. It
  ignores the shape: a cylinder comes out as a flat quad at the cylinder's centre transform.
- **auto** (default): native if the runtime offers the cylinder extension, as the patched OXRSys runtime does.
  Otherwise emulated, because an OXRSys build without the layer patches drops quads.

The log says which mode ran (`LogFrameXR: Stereo layers: ...`) and lists the layers whenever the set changes.

Limits of the native path:

- Equirect and cubemap Stereo Layers are not sent. The log warns once per layer.
- No underlays. Godot and Unity have them, Unreal does not.
- A layer whose texture format the runtime does not offer is warned once and not submitted. The engine makes one such
  layer of its own (1280x720, format 115). It is not the test's.

### The panel test

`SMOKE_PANELS=1` (or `FRAMELAB_PANELS=1`, the switch the Godot and Unity scenes read too) adds two panels in the same
layout as the Godot scene's panels mode. `toggle` instead of `1` hides and shows both every 10 s.

```sh
FRAMELAB_PANELS=1 bin/frame-stream up --app unreal
```

- A dark panel with a white border, the title top left, then red, green and blue rows.
- Built as a UMG tree (`USmokePanelWidget`) and drawn by a hidden WidgetComponent into its render target.
- Shown by world-locked Stereo Layers: a 1.0 x 0.6 m quad 1.5 m ahead at 1.5 m (priority 1), and a cylinder of radius
  1.5 m, 60 degrees of arc, aspect 2, turned 60 degrees left (priority 2).

Measured off-head, 90 Hz, with `SMOKE_CUBE=0`, the picture read from the Mac encoder's input and the headset's eye 0:

| mode | FrameXR log | runtime `EndFrame types` | `VideoEncoder: overlays` | picture |
|---|---|---|---|---|
| native | 2 swapchains (1000x600 and 1200x600, format 81 = BGRA8 sRGB, 3 images), `1:quad 1.00x0.60 m`, `2:cylinder r 1.50 m 60 deg aspect 1.99` | `[35 36 1000017000]` (projection, quad, cylinder) | 451 frames with layers, 1804 layer draws / 5 s, encoder GPU p50 0.41 ms | the quad ahead and the curved cylinder to the left, both upright, with border, title and the three rows |
| emulated | `Stereo layers: emulated, drawn into the eye buffer` | `[35]` | none | the quad only, drawn by the engine with a near-black background (the sRGB colours come out darker); the cylinder is a flat quad at its centre transform, at the head, so it is not seen |

Both modes held 90 fps with 0 frames skipped.

Cost of the two layers, from `FRAMELAB_PANELS=toggle` on the release runtime (off-head, USB-C tether, 5 s windows wholly
shown or wholly hidden): encoder command buffer GPU p50 0.412-0.414 ms with both layers, 0.337-0.339 ms without. So the
two panels cost about 0.08 ms a frame on the encoder's GPU. 450 frames and 1800 layer draws per 5 s shown, 90 fps, app
late 0, keyframe requests 0.

## Resolution and frame rate

The runtime recommends 2160x2160 per eye (4320x2160 side by side). FrameXR's eye targets and swapchain match it.

- **Render cost comes down through the engine's screen percentage**, with the eye target kept at 2160x2160:
  `[SystemSettings] r.ScreenPercentage.Default.VR.Mode=0` (manual) and `r.ScreenPercentage`. The project ships at 100.
- **Anti-aliasing is FXAA** (`r.AntiAliasingMethod=1`). TAA ghosted head-locked HUD text and softened the scene. FXAA
  does not upsample, so below 100 % screen percentage text goes soft. Edges are FXAA-smoothed, so expect a little
  shimmer on thin lines. A shipping VR project should still use forward shading + MSAA with baked lighting, the way
  `../unreal-vr` is set up.
- **The VR budget in `DefaultEngine.frame.ini`**: Lumen, virtual shadow maps, Nanite, auto-exposure, bloom, motion blur,
  AO, volumetric cloud and volumetric fog off.
- **`[FrameXR] PixelDensity`** (or env `FRAMEXR_PIXEL_DENSITY`, 0.5-1.0) shrinks the eye buffers and the swapchain. Keep
  it at 1.0 unless you stream with `--ffe off`: OXRSys's foveated encoder refuses a source smaller than its target eye
  (`invalid foveated texture, dimensions...`), and the encoder drain then wedges reconnects. Foveation is only
  announced at the runtime's `resolution_scale` 1.0. The density is capped at 1.0.
- **`vr.PixelDensity` does nothing here.** It is registered after the plugin sizes its swapchain.

## Measured

GPU budget. Base M5, Development editor binary, `-game -vr`, no headset client attached, 4320x2160:

| settings | GPU frame | fps |
|---|---|---|
| template defaults (Lumen, VSM, TSR, volumetric cloud) | about 66 ms queued ahead of the blit | 14 |
| Lumen, VSM, Nanite, TSR and post off | 38.3 ms, of which VolumetricCloud 28.7 (CloudView PS 540x540 x 2 eyes) | 24 |
| + volumetric cloud and fog off | 8.8 ms | 90.0, locked to the runtime's 11.11 ms |

With the headset client attached, the engine shares the GPU with the runtime's copy and foveation passes (about
+1.4 ms), 90 Hz:

| settings | GPU | result |
|---|---|---|
| TAA, 100 % screen percentage | 9.2-10.2 ms (ProfileGPU) | 82-84 fps, engine frame gap 11.9-12.2 ms, app late on every runtime frame; the cube visibly lags and stutters |
| TAA, 80 % screen percentage | 7.2-7.9 ms, encoder GPU 7.4 ms | 89.6 fps |
| forward shading + MSAA 2x | | 87 fps |
| FXAA, 100 % (as shipped), worn | 6.9-7.9 ms | 90.0 fps, no late frames, HUD text clean |

Stream with the client attached, over the tailnet, unworn, `--bitrate 50 --ffe medium_plus --render
steam_frame_native`: `state streaming`, encoded 2752x1216 of the 4320x2160 render (medium_plus foveation), server
pipeline 10.2 ms, client decode 2.4 ms, displayed frame age 18.3 ms, 1800 frames with 0 drops and 0 keyframe requests.

## Settings reference

FrameXR, environment:

| variable | effect |
|---|---|
| `FRAMEXR_OPENXR_LOADER` | the loader dylib. Without it the plugin looks in its own `ThirdParty/OpenXR/lib/libopenxr_loader.dylib` (gitignored). No path from the building machine is baked into the binary |
| `FRAMEXR_STEREO_LAYERS` | `native`, `emulated` or `auto` (default); see "Menus and panels" |
| `FRAMEXR_PIXEL_DENSITY` | overrides `[FrameXR] PixelDensity` |
| `FRAMEXR_SWAPCHAIN_FORMAT` | the swapchain's MTLPixelFormat number |
| `FRAMEXR_BLIT_WAIT=1` | CPU-waits the blit. Measures the GPU queue ahead of it exactly, and costs the pipelining |
| `FRAMEXR_OPENXR_INCLUDE` | build time: the Khronos headers. Default: the plugin's vendored `ThirdParty/OpenXR/include`, OpenXR SDK 1.1.57 |

The smoke project, environment:

| variable | effect |
|---|---|
| `SMOKE_PANELS` / `FRAMELAB_PANELS` | `1` adds the panel test, `toggle` hides and shows it every 10 s |
| `SMOKE_CUBE` | the reference cube. Default: on in the smoke's own map only. `0` off, anything else on |
| `SMOKE_GRAB` | the grab cube, same rule as `SMOKE_CUBE` |
| `SMOKE_GRAB_REACH` | grab reach in cm (default 15) |
| `SMOKE_SPAWN` | `x,y,z[,yaw]`: where the pawn starts, in cm, world space. z is a point above the floor; the pawn drops onto the floor under it |
| `SMOKE_FLY` | `0` walks level with collision. Default: fly along the gaze |
| `SMOKE_WALK_SPEED` | cm/s at full stick (default 600) |
| `SMOKE_SNAP_DEG` | snap-turn angle (default 30) |
| `SMOKE_HZ` | the display rate the movement step assumes (default 90) |
| `SMOKE_MOVE_DT=tick` | steps by the game thread's frame time instead of one display period |
| `SMOKE_FLOOR_DEADBAND` | floor changes smaller than this (cm, default 2) are not followed |
| `SMOKE_PROFILEGPU=1` | two automatic `ProfileGPU` dumps, 20 s in and 30 s after the recentre. Each stalls the frame it runs in, so they are off by default |
| `SMOKE_CMDFILE` | a file polled every 0.5 s: each line runs as a console command, then the file is deleted. Default `$HOME/src/logs/smoke-cmd.txt`. `smoke.tour <loops> <m/s>` drives a scripted camera through the map, `smoke.tour stop` ends it |

## Known limits and gotchas

- **Floor origin.** OXRSys offers view, local and stage reference spaces, not `LOCAL_FLOOR`. A floor origin resolves to
  `STAGE`, whose origin is at the floor but whose forward is the room's, not the wearer's, so the app recentres. With no
  headset attached the runtime's placeholder pose is exactly 1.6 m up and unrotated.
- **A remade session has no controllers.** If the runtime exits or loses the session, FrameXR destroys it and makes a
  new one at the next present. OXRSys attaches action sets once per process, so the new session renders but has no
  controllers. Restart the app to get them back.
- **The crash reporter.** The template trips an ensure at startup (ShadowRendering.cpp:798). Without
  `DefaultEditorSettings.ini`, that launches CrashReportClientEditor on every run, and the reporter segfaults on exit.
  Silencing the ensure with `r.Shadow.StencilCulling=0` costs 3.2 ms of GPU, so the project leaves it.
- **A stuck UDP port.** A killed run can hang on to UDP 9945. The next run logs `TrackingReceiver: Failed to bind on
  port 9945` and no client connects. Check `lsof -iUDP:9945` and `kill -9` the old pid first.
- **The editor's message bus.** `[/Script/UdpMessaging.UdpMessagingSettings] EnableTransport=False` keeps the editor
  from opening a second listening socket that the Mac's firewall would ask about. The project needs only the runtime's
  ports (9944-9946).
- **Spectator window.** The Mac window shows the engine's default spectator view (the left eye).
