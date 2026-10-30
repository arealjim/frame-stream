# unity-smoke — Unity → OXRSys → Steam Frame

The Unity counterpart of `xr-smoke` (Godot): the smallest project that drives the OXRSys runtime on the Mac through
`XR_KHR_metal_enable`, so the same stream reaches the Frame. Everything is driven from the command line. The only GUI
act is Play Mode, and the `Play` method enters it by itself.

Chain: Unity 6000.3.26f1 → com.unity.xr.openxr 1.18 → OXRSys (in-process, the same runtime build as Unreal) → Frame.

## How it works

Unity's OpenXR package has a Standalone (macOS, Metal) loader. MetalQuestLink runs Play Mode on it against the Meta XR
Simulator. OXRSys's `EntryPoint.cpp` lists `XR_KHRX2_metal_enable` as a Unity alias for the Metal extension, so
upstream built for this. OXRSys advertises: KHR metal_enable, convert_timespec_time, EXT hand_tracking,
conformance_automation, hand_interaction, debug_utils, META touch_controller_plus, and Vulkan.

If Unity asks for an extension OXRSys lacks, `xrCreateInstance` fails and the runtime log names it. The runtime logs
to `~/Library/Application Support/OXRSys/oxrsys-runtime.log`, not to the Unity log. It says whether the session came
up and what Unity asked for.

## Set up

On the Mac, with editor 6000.3.26f1 in `~/Applications/Unity/Hub/Editor`:

    U=~/Applications/Unity/Hub/Editor/6000.3.26f1/Unity.app/Contents/MacOS/Unity
    P=~/src/unity-smoke
    "$U" -batchmode -createProject "$P" -quit -logFile ~/Library/Logs/frame-stream/unity-create.log
    cp -r <repo>/engines/unity/Editor "$P/Assets/Editor"
    cp -r <repo>/engines/unity/Scripts "$P/Assets/Scripts"   # the runtime rig (FrameLab.Runtime; compiles before stage 1 too: its Input System code is under FRAMELAB_INPUTSYSTEM)
    "$U" -batchmode -projectPath "$P" -executeMethod FrameLab.Bootstrap.AddPackages -quit -logFile ~/Library/Logs/frame-stream/unity-stage1.log
    "$U" -batchmode -projectPath "$P" -executeMethod FrameLab.Bootstrap.Configure   -quit -logFile ~/Library/Logs/frame-stream/unity-stage2.log

The first GUI launch shows a software-terms dialog on the Mac's screen. Only the person at the Mac can accept it.
Nothing in any log says so; `sample <pid>` shows `SoftwareTermsWindow`.

## Run

`frame-stream up --app unity` starts the Frame client and the editor together. It removes a stale lock and scene
backups, checks UDP 9944-9946, launches with `open` and the runtime path resolved on the Mac, then waits for the
runtime's status file and for the rig's first `frame-lab rig: scene` line. `frame-stream down` quits the editor and
clears the lock.

`FrameLab.Bootstrap.BuildPlayer` makes a standalone arm64 player for a repeatable, editor-free run.

To launch the editor by hand:

    R=~/src/oxrsys/build/macos-arm64/runtime/oxrsys-runtime.json
    open -n -a ~/Applications/Unity/Hub/Editor/6000.3.26f1/Unity.app --env XR_RUNTIME_JSON=$R --env FRAME_XR_RUNTIME_JSON=$R \
      --args -projectPath "$P" -executeMethod FrameLab.Bootstrap.Play -logFile ~/Library/Logs/frame-stream/unity-play.log

A manual relaunch that works unattended: kill the editor (kill, then kill -9), remove `Temp/__Backupscenes` and
`Temp/UnityLockfile`, check that `lsof -nP -iUDP:9944-9946` is empty, then the `open` launch above. Wait for
`bound to port 9944` in the runtime log, then run `frame-stream up --app external ...`. Old play logs are rotated to
`unity-play-N.log`.

## The scene and the rig

`Bootstrap.Play` opens `Assets/Scenes/Smoke.unity` before entering Play Mode. Play Mode runs whatever scene the
editor has open, and a fresh editor (no `Library/LastSceneManagerSetup.txt`) opens an empty default scene with
Unity's procedural skybox.

`Scripts/FrameLabRig.cs` (assembly `FrameLab.Runtime`, referenced by the editor assembly) is attached to the camera
by `MakeScene`. It:
- binds the `TrackedPoseDriver` to `<XRHMD>/centerEyePosition|Rotation|trackingState`. Unity's XR display does not
  move the camera by itself; without this binding the camera sits at (0,0,0) and the client only reprojects a static
  image.
- calls `TrySetTrackingOriginMode(Floor)` (returns True on OXRSys, supported 3). The default origin is device level,
  which would put the camera at floor height.
- keeps a cube 0.5 m ahead at eye height until the head moves, then leaves it in place. "Moves" means four
  consecutive half-second samples with 2 mm-30 cm of motion each.
- logs `frame-lab rig: scene '...' cam pos ... euler ... clear ... floor ... cube ... xr ... head action ...
  tracking ...` every 5 s.

Off-head, the runtime feeds a static pose with the tracking bits set: a canned 1.6 m before a client attaches, the
headset's resting height (1.147 m on the test rig) after. The attach is a single jump, and the tracking-state action
reads 3 for the canned pose too. The consecutive-sample rule is what keeps that jump from freezing the cube: with the
client attached for 45 s off-head, the cube still follows at 1.147 m and no "head moving" line is logged.

To see what the Frame is actually shown, use the client's eye dump: `FRAME_CLIENT_DUMP=1 frame-stream up ...`, let
it run, `frame-stream down`, then read `/tmp/frame_client_eye0.rgba` (2160x2160, sRGB-double-encoded) and
`/tmp/frame_client_decoded.nv12` (2752x1216 SBS) on the Frame. With the rig in place the dump shows the lit floor and
the cube with its shadow, eye at 1.147 m (headset unworn).

## Measured

Client attached, headset unworn, from `tools/regress.sh unity` (the command is in the script):

| | |
|---|---|
| runtime pacing | 90 Hz, actual mean 11.10 ms, app late 0 |
| render | 2160x2160 per eye (runtime render_width 4320), encoded 2752x1216 foveated |
| runtime latency line (`sample->display latency (measured)`) | render 7.6 ms, encode+send 12.8 ms, client displayed age 18.3 ms, rtt 6.2 ms, 0/361 lost (Unreal: 3.1 / 18.6 / 18.6 / 5.9; Godot 1.2 / 18.7 / 18.1 / 5.9) |
| Frame client | 720 fresh frames per 8 s at 90.1 Hz, 0-0.8 % would-be repaints |

`tools/regress.sh` runs Godot, Unity and Unreal off-head in turn and prints one line each. In our run all three had
0 late frames, 720/720 fresh and 0 lost packets.

Worn, over one minute with the rig above: the camera sits at 1.18-1.25 m and moves and turns with the head (runtime
flags 0x15). The client's warp numbers: stamped->displayed mean 0.09-0.18 deg, max 0.2-0.8 deg; translation slip
0.4-1.9 cm. The wearer judged it about as stable as Unreal, and close to, not quite at, the stability of WayVR's own
panels.

## Gotchas

- **Launch through `open`.** The editor launched from an ssh shell spins at 100 % with a frozen log. `open` goes
  through LaunchServices into the Mac's GUI session.
- **Ports:** an engine that holds UDP 9944-9946 (Unreal, or a wedged Unity) makes the runtime log
  `Failed to bind control socket on port 9946`, and it never retries: the session runs, but no client can attach.
  Take the other engine down first and check `lsof -iUDP:9944-9946`.
- **Dialogs after a kill:** a killed editor leaves `Temp/UnityLockfile` and `Temp/__Backupscenes/` behind. The next
  launch sits at 0 % CPU behind the "project already open" and "recover scene backup" dialogs, visible only in a
  `sample` of the main thread (`HandleDanglingSceneBackups` / `DisplayDecisionDialogNative`). Remove both before
  `open`. The scene is rebuilt by `Configure`, so nothing is lost. `frame-stream up --app unity` does this for you.
- **Play request dropped:** an `EditorApplication.delayCall` can fail to fire on an unattended relaunch ("entering
  Play" logged, no "Reloading assemblies for play mode"). `Bootstrap.Play` therefore sets `isPlaying` directly and
  re-asks every 3 s until the editor is playing, logging each ask.

## Traces

The editor talks to Unity's licensing and analytics services. `Unity.Licensing.Client.log` shows a BigQuery
analytics exporter, failing, and the play log repeats `Curl error 60: Cert verify failed` against a Unity endpoint.
Neither affects the stream. To turn editor usage analytics off, quit the editor and run
`defaults write com.unity3d.UnityEditor5.x EnableEditorAnalytics -int 0` on the Mac (the default is 1). The licensing
and `Curl error 60` traffic remains: sign-in and cert checks, not usage data. Unity's crash reporting is not switched
off the way Unreal's is.

## Panels: menus on composition layers

How Unity draws menus in OpenXR: a world-space Canvas renders into the eye buffer like any mesh, so it works on every
runtime and already streams. Real layers come only from com.unity.xr.compositionlayers (Quad, Cylinder, Cube,
Equirect, Projection; a UI canvas source via a hidden camera, its "Quad/Cylinder UI Panel" prefabs need the XR
Interaction Toolkit). The OpenXR plugin is the provider (Runtime/CompositionLayers in com.unity.xr.openxr 1.18),
enabled by the OpenXR feature "Composition Layers Support". From the package sources:
- Version: 2.5.0 pairs with OpenXR 1.18 (installation.md; 2.6.0 wants 1.19-pre).
- Extensions the feature requests: XR_KHR_composition_layer_cylinder, _equirect, _equirect2, _cube,
  _color_scale_bias, XR_KHR_android_surface_swapchain. Quad and projection layers are core and need none. Each other
  layer handler is created only if its extension is enabled (OpenXRLayerProvider.InitializeAndRegisterBuiltInHandlers),
  so a runtime without the cylinder extension gets no cylinder at all, not a fallback.
- macOS: nothing refuses it in source. The feature lists BuildTargetGroup.Standalone, and the osx UnityOpenXR.dylib
  (universal, arm64) exports all 17 ext_composition_layers_* entry points next to its Metal graphics plugin. The
  package's own platform table names only Quest, Android XR and SteamVR (Quad yes, Cylinder blank for SteamVR).

The test uses the same layout as xr-smoke's panels mode. `Scripts/FrameLabPanels.cs` builds a 1.0 x 0.6 m quad 1.5 m
ahead at 1.5 m (order 1) and a 1.5 m radius, 60 degree, aspect 2 cylinder turned 60 degrees left (order 2). Each is
fed by a uGUI canvas (dark panel, light border, title top-left, red/green/blue rows top to bottom) rendered into a
RenderTexture. It is off unless FRAMELAB_PANELS is 1 or toggle. Toggle hides and shows both layers every 10 s, so
the runtime's "VideoEncoder: overlays:" line gives the GPU cost with and without in one run. The script compiles to
nothing without the package, so the plain project and `tools/regress.sh unity` are unchanged. The panels project is a
sibling:

    cp -cR ~/src/unity-smoke ~/src/unity-panels     # APFS clone, no disk cost; rm -rf its Temp and Logs
    copy Scripts/FrameLabPanels.cs, Scripts/FrameLab.Runtime.asmdef, Editor/FrameLabBootstrap.cs into its Assets
    "$U" -batchmode -projectPath ~/src/unity-panels -executeMethod FrameLab.Bootstrap.AddPanelPackages -quit -logFile ~/Library/Logs/frame-stream/unity-panels-pkg.log
    "$U" -batchmode -projectPath ~/src/unity-panels -executeMethod FrameLab.Bootstrap.Configure -quit -logFile ~/Library/Logs/frame-stream/unity-panels-cfg.log
    FRAME_STREAM_UNITY_PROJECT='~/src/unity-panels' FRAMELAB_PANELS=toggle \
      FRAME_STREAM_RUNTIME_DIR='~/src/oxrsys/build/macos-arm64/runtime' FRAME_CLIENT_DUMP=1 frame-stream up --app unity

Read "frame-lab panels:" in ~/Library/Logs/frame-stream/unity-play.log (feature on/off, the cylinder extension, the enabled
composition extensions, the provider and layer count every 30 s) and "VideoEncoder: overlays:" in the runtime log.

**Measured: a two-minute run, off-head (headset on a dummy head), the panels runtime build, with a build of the test
that toggled only the quad:**
- AddPanelPackages 3 min 11 s (compositionlayers 2.5.0, ugui 2.0.0), Configure: "feature on: OpenXRCompositionLayersFeature".
- Unity: "frame-lab panels: ... feature on, cylinder ext True, runtime OXRSys Runtime 1.3.0, enabled composition
  extensions: XR_KHR_composition_layer_cylinder"; "manager provider OpenXRLayerProvider, layers 3". Unity asks for
  all six extensions and enables only the one OXRSys offers; xrCreateInstance does not fail on the others.
- Runtime: "EndFrame layers=3 types=[35 36 1000017000]" with the quad shown, "[35 1000017000]" hidden;
  "VideoEncoder: overlays: 451 frames with (1804 layer draws) ... GPU p50/p95 0.582/0.643 ms" (both) and
  "450 frames with (902 layer draws) ... 0.564/0.628 ms" (cylinder only). There is no "without" figure because the
  cylinder never hid in that build. Pacing 90 Hz, app late 0.
- Frame eye dump (eye0.png and decoded_sbs.png): both panels are in the pixels and upright. The quad has its red row
  on top and its title top-left, and the cylinder is to the left with its arc curving the right way. The quad's thin
  border breaks into dashes: minification aliasing with no mips, a known limit.
- Crash sweep: 0 new Mac DiagnosticReports, 0 Frame coredumps.
- One harmless warning: "Emulation of composition layers in Game View is not supported when XR device is connected".

## Controllers: grip, trigger and a cube to grab

`Scripts/FrameLabHands.cs` is added to the XR origin by the rig. Each hand is a block at the grip pose and a 1 m ray
from the aim pose. The trigger ramps the block from grey to green over its analog value, and the grip squashes it to
half its thickness. A 12 cm magenta cube sits in reach ahead of the spawn: grip within 0.15 m of it
(`FRAMELAB_GRAB_REACH`) picks it up, and letting go drops it in place, with no physics (`FRAMELAB_GRAB=0`: no cube).
Each press and release edge logs `frame-lab input: left grip 0.00->1.00`, and every controller button logs
`frame-lab rig: button <device>/<control> down|up`. Bindings are the generic `<XRController>{LeftHand}/{Trigger}` and
`{Grip}`. Unity's Oculus Touch profile binds them to `.../trigger/value` and `.../squeeze/value`, so the bare-path fix
(runtime 0030) is not on Unity's path. The Frame client reports the Touch profile to Unity.

The grab cube follows the eyes until the head is worn: above 0.9 m and moving for 2 s. It comes back if left 3 m
behind for 5 s, and a hand that loses tracking drops it. This keeps it from freezing at carrying height while the
headset is being put on, which otherwise leaves it 25 cm off the floor.

Tested with a clone of the Mac's unity-smoke project carrying the release copy's `engines/unity` Scripts and Editor,
over the hotspot:
- **Off-head, injected** (a lab test pad that injects controller input; it is not in the shipped series):
  `left grip 0.00->1.00`, `right trigger 0.00->1.00`, `left picked up the cube`, then both `1.00->0.00`. The client
  dump catches only the floor (the dummy head looks down), so the picture proof is the worn run below.
- **Worn, real presses** (the shipped 31-patch runtime): analog edges like `right trigger 0.49->0.69`, two grabs and
  drops by the right hand 0.05-0.06 m from the block. The cube was found and picked up, the triggers changed the
  wands' colour and the grips compressed the hands. Buttons that arrive, both hands: trigger, grip, primary (A/X),
  secondary (B/Y), stick click, and the touches (trigger, thumbstick, thumbrest, A/B/X/Y); left menu. Nothing for a
  right menu (the Touch profile has none).
- **Placement rule, off-head** (compiled in batchmode, release copy, the test pad): the dummy head's eye reads
  -0.02 m, so the cube waits at its first spot as designed (no "head worn" line), and a pad grab picks it up and lets
  it go. The drop lands at the floor: the pad ends grip, tracking and pose in one tick, so the hand's pose has already
  jumped when the let-go runs. Real controllers rarely lose tracking at the instant of release.
- **Not checked worn:** the placement rule itself (needs a head) and the drop point on release.

