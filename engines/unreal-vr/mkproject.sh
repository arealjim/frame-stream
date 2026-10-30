#!/bin/bash
# unreal-vr: Epic's VR Template (TP_VirtualRealityBP, Blueprint) as a project on the FrameXR plugin - the "larger
# environment" test for the Unreal -> OXRSys -> Steam Frame chain (2026-10-21). Idempotent like unreal-smoke's.
#
# What the editor's New Project dialog would do, by hand (Templates/TP_VirtualRealityBP/Config/TemplateDefs.ini):
#   - the template's own feature pack (FeaturePacks/TP_VirtualRealityBP.upack) holds only config.ini + two thumbnails;
#     the scene is the five SharedContentPacks it mounts, each copied to Content/<DestinationFilesFolder> as that
#     pack's FeaturePack/manifest.json says (XRFramework = the map, pawn, game mode, input; LevelPrototyping at
#     detail level High = the lobby's meshes; XRMannequins = the hands; Weapons = the pistol; VRSpectator)
#   - ReplacementsInFiles TP_VirtualRealityBP -> UnrealVR in ini/cpp/h/cs (a Blueprint template has no Source,
#     so FolderRenames has nothing to do); Config/config.ini (legacy ActionMappings) and the thumbnails are ignored
#   - the template's Mac/MacEngine.ini turns instanced stereo and multiview off for Metal
# Plus frame-lab: a minimal C++ module (an editor target the plugin builds with), Plugins/FrameXR copied from
# unreal-smoke (sources, vendored OpenXR 1.1.57 headers, the loader dylib; not its Binaries/Intermediate), the
# uproject with Epic's OpenXR plugins off (no Metal path, see ../unreal/README.md) and FrameXR on, and the
# frame-lab config additions appended to DefaultEngine.ini - WITHOUT the smoke's renderer budget: the first run is
# meant to show what the stock template costs. The template already ships a VR budget of its own (forward shading,
# MSAA, no Lumen, no virtual shadow maps, static lighting built).
set -e
E=${FRAME_STREAM_UE_ENGINE:-"/Users/Shared/Epic Games/UE_5.8"}   # the engine install (same variable as bin/frame-stream)
T="$E/Templates/TP_VirtualRealityBP"
R="$E/Templates/TemplateResources"
S="$HOME/src/unreal-smoke"
P="$HOME/src/unreal-vr"
N=UnrealVR
[ -e "$P" ] && { echo "exists: $P"; exit 0; }
[ -d "$S/Plugins/FrameXR/Source" ] || { echo "no FrameXR plugin at $S/Plugins/FrameXR (rsync ../unreal there first)"; exit 1; }
mkdir -p "$P/Source/$N" "$P/Config/Mac" "$P/Content" "$P/Plugins/FrameXR"

# --- config, renamed
for f in DefaultGame.ini DefaultEngine.ini DefaultInput.ini DefaultEditor.ini; do sed "s/TP_VirtualRealityBP/$N/g" "$T/Config/$f" > "$P/Config/$f"; done
cp "$T/Config/Mac/MacEngine.ini" "$P/Config/Mac/MacEngine.ini"
cp "$S/Config/DefaultEditorSettings.ini" "$P/Config/DefaultEditorSettings.ini"   # crash reporter + usage data off

# --- content: the shared packs TemplateDefs.ini lists, at the detail level it names
for p in Standard/XRFramework Standard/XRMannequins Standard/VRSpectator Standard/Weapons High/LevelPrototyping; do
  d=$(sed -nE 's/.*"DestinationFilesFolder": *"([^"]+)".*/\1/p' "$R/$p/FeaturePack/manifest.json")
  [ -n "$d" ] || { echo "no DestinationFilesFolder in $R/$p/FeaturePack/manifest.json"; exit 1; }
  mkdir -p "$P/Content/$d"; cp -R "$R/$p/Content/." "$P/Content/$d/"
  echo "content: $p -> Content/$d ($(find "$P/Content/$d" -type f | wc -l | tr -d ' ') files)"
done

# --- a minimal C++ module, from the smoke's (no game mode of our own: the template's BP_XRGameMode stays)
sed "s/UnrealSmoke/$N/g" "$S/Source/UnrealSmokeEditor.Target.cs" > "$P/Source/${N}Editor.Target.cs"
sed "s/UnrealSmoke/$N/g" "$S/Source/UnrealSmoke.Target.cs" > "$P/Source/$N.Target.cs"
for f in UnrealSmoke.Build.cs UnrealSmoke.h UnrealSmoke.cpp; do sed "s/UnrealSmoke/$N/g" "$S/Source/UnrealSmoke/$f" > "$P/Source/$N/${f/UnrealSmoke/$N}"; done
# the repo's module body replaces the smoke's: FRAMELAB_UE_EXEC="<s>:<console cmd>;..." runs console commands at play,
# which is how the 90 Hz budget was measured one cvar at a time (README "Performance: the settings that reach 90 Hz"). Run this script from its repo dir.
H=$(cd "$(dirname "$0")" && pwd)
[ -f "$H/Source/$N/$N.cpp" ] && cp "$H/Source/$N/$N.cpp" "$P/Source/$N/$N.cpp" || echo "note: no $H/Source/$N/$N.cpp - the module has no FRAMELAB_UE_EXEC"

# --- the FrameXR plugin
for d in FrameXR.uplugin LICENSE-tether.txt Source ThirdParty; do cp -R "$S/Plugins/FrameXR/$d" "$P/Plugins/FrameXR/"; done
[ -f "$P/Plugins/FrameXR/ThirdParty/OpenXR/lib/libopenxr_loader.dylib" ] || echo "note: no loader dylib under the plugin (set FRAMEXR_OPENXR_LOADER at run time, or copy it as ../unreal/README.md says)"

# --- the uproject: the template's plugin list with Epic's OpenXR family off (OpenXRHMD has no Metal path and
#     cannot load on macOS), PICOController off (Win64/Linux/Android only anyway), XRBase + FrameXR on
cat > "$P/$N.uproject" <<JSON
{
	"FileVersion": 3,
	"EngineAssociation": "",
	"Category": "",
	"Description": "frame-lab: Epic's VR Template on the FrameXR plugin -> OXRSys -> Steam Frame",
	"Modules": [
		{
			"Name": "$N",
			"Type": "Runtime",
			"LoadingPhase": "Default"
		}
	],
	"Plugins": [
		{
			"Name": "OpenXR",
			"Enabled": false
		},
		{
			"Name": "OpenXREyeTracker",
			"Enabled": false
		},
		{
			"Name": "OpenXRHandTracking",
			"Enabled": false
		},
		{
			"Name": "PICOController",
			"Enabled": false
		},
		{
			"Name": "XRBase",
			"Enabled": true
		},
		{
			"Name": "FrameXR",
			"Enabled": true
		}
	],
	"TargetPlatforms": [
		"Mac"
	]
}
JSON

# --- frame-lab additions (the same block as unreal-smoke's DefaultEngine.ini, minus its renderer budget, plus MSAA 2x)
cat >> "$P/Config/DefaultEngine.ini" <<'INI'

; --- frame-lab additions (see ../unreal/README.md for the measurement behind each) ---
[HMDPluginPriority]
FrameXR=60

; the editor's UDP message bus is a second listening socket the Mac's application firewall asks about
[/Script/UdpMessaging.UdpMessagingSettings]
EnableTransport=False

; eye buffers + swapchain at the runtime's recommended 2160x2160: OXRSys's foveated encoder refuses a smaller source
[FrameXR]
PixelDensity=1.0

; internal render resolution 80 % with the eye target kept at 2160x2160; TAA/TSR upsamples (the template uses MSAA:
; whether r.ScreenPercentage applies with r.AntiAliasingMethod=3 is one of the things the first run reports)
[SystemSettings]
r.ScreenPercentage.Default.VR.Mode=0
r.ScreenPercentage=80

; 90 Hz budget (measured through the stream with ProfileGPU, README "Performance"): MSAA 4x -> 2x took the
; template's GPU frame from 16.9 ms (57 fps, every frame late) to 7.4 ms and the stream to 720/720 fresh frames at 90 Hz.
; Headroom if a scene needs it: r.ScreenPercentage=70 (5.9 ms). Dynamic shadow quality did not matter (sg.ShadowQuality 2).
[/Script/Engine.RendererSettings]
r.MSAACount=2

; The unreal-smoke renderer budget, one uncomment away if the stock template is too slow for 90 Hz. The template
; already runs forward shading, MSAA (r.AntiAliasingMethod=3), no Lumen (DynamicGlobalIlluminationMethod=0), no
; virtual shadow maps and built static lighting, so most of the smoke's cuts are already in force; what remains:
; [/Script/Engine.RendererSettings]
; r.ReflectionMethod=0
; r.DefaultFeature.Bloom=False
; r.VolumetricCloud=0
; r.VolumetricFog=0
; r.Nanite=0
INI

# The template's XRFramework/Blueprints/BP_AssetGuideline (on BP_XRPawn's camera) asks, on every load, to enable
# Epic's OpenXR plugins this uproject keeps off; they have no Mac build, so "enable" can never work (seen 2026-10-29).
# Emptying its list needs the editor module, so it is the step after the first build. PythonScriptPlugin is off by
# default in 5.8, so the command enables it for that one run and the streamed project never loads Python:
echo "after the first build, silence the 'missing plugins' prompt:"
echo "  \"$E/Engine/Binaries/Mac/UnrealEditor-Cmd\" $P/$N.uproject -run=pythonscript -script=$H/guideline_off.py -EnablePlugins=PythonScriptPlugin -unattended -nullrhi"
echo "  (guideline_check.py the same way prints every guideline's list: all [] = no prompt)"
echo "== $P"; find "$P" -type f -not -path "*/Content/*" -not -path "*/Plugins/FrameXR/Source/*" -not -path "*/ThirdParty/*" | sed "s|$P/||"
echo "== content files: $(find "$P/Content" -type f | wc -l | tr -d ' ')"
echo "== uproject"; cat "$P/$N.uproject"
