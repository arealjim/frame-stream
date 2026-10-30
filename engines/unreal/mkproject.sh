#!/bin/bash
# unreal-smoke: a blank C++ project from the engine's TP_Blank template, renamed, with the FrameXR plugin dir ready
set -e
E=${FRAME_STREAM_UE_ENGINE:-"/Users/Shared/Epic Games/UE_5.8"}   # the engine install (same variable as bin/frame-stream)
T="$E/Templates/TP_Blank"
P="$HOME/src/unreal-smoke"
N=UnrealSmoke
[ -e "$P" ] && { echo "exists: $P"; exit 0; }
mkdir -p "$P/Source/$N" "$P/Config" "$P/Content" "$P/Plugins"
for f in DefaultGame.ini DefaultEngine.ini DefaultInput.ini DefaultEditor.ini; do sed "s/TP_Blank/$N/g" "$T/Config/$f" > "$P/Config/$f"; done
sed "s/TP_Blank/$N/g" "$T/Source/TP_BlankEditor.Target.cs" > "$P/Source/${N}Editor.Target.cs"
sed "s/TP_Blank/$N/g" "$T/Source/TP_Blank.Target.cs" > "$P/Source/${N}.Target.cs"
for f in TP_Blank.Build.cs TP_Blank.h TP_Blank.cpp; do sed "s/TP_Blank/$N/g" "$T/Source/TP_Blank/$f" > "$P/Source/$N/${f/TP_Blank/$N}"; done
sed "s/TP_Blank/$N/g" "$T/TP_Blank.uproject" > "$P/$N.uproject"
# Ours, over the template's: the module's dependencies (the smoke pawn uses the HMD library, XRBase, the RHI and UMG)
# and the plugin list (FrameXR on, Epic's OpenXR off: it would compete for the HMD and has no Metal path).
cat > "$P/Source/$N/$N.Build.cs" <<CS
using UnrealBuildTool;

public class $N : ModuleRules
{
	public $N(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "HeadMountedDisplay", "XRBase", "RHI", "RenderCore", "UMG", "Slate", "SlateCore" });
	}
}
CS
cat > "$P/$N.uproject" <<JSON
{
	"FileVersion": 3,
	"EngineAssociation": "",
	"Category": "",
	"Description": "frame-lab: the Unreal -> OXRSys -> Steam Frame smoke project",
	"Modules": [
		{
			"Name": "$N",
			"Type": "Runtime",
			"LoadingPhase": "Default"
		}
	],
	"Plugins": [
		{
			"Name": "ModelingToolsEditorMode",
			"Enabled": true,
			"TargetAllowList": [
				"Editor"
			]
		},
		{
			"Name": "XRBase",
			"Enabled": true
		},
		{
			"Name": "FrameXR",
			"Enabled": true
		},
		{
			"Name": "OpenXR",
			"Enabled": false
		}
	]
}
JSON
# The published copy ships our DefaultEngine.ini blocks (map, game mode, FrameXR priority, the VR budget) as a
# snippet, because the rest of that file is Epic's: append it to the template's copy.
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$here/Config/DefaultEngine.frame.ini" ] && { echo; cat "$here/Config/DefaultEngine.frame.ini"; } >> "$P/Config/DefaultEngine.ini"
echo "== $P"; find "$P" -type f | sed "s|$P/||"; echo "== uproject"; cat "$P/$N.uproject"
