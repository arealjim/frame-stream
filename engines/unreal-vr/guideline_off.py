# Silence the VR Template's asset guideline: it asks to enable Epic's OpenXR plugins, which FrameXR replaces and
# which have no Mac build, so "enable" can never succeed. Empties its plugin list; the pawn keeps the (now empty) entry.
import unreal
path = "/Game/XRFramework/Blueprints/BP_AssetGuideline"
bp = unreal.load_asset(path)
cdo = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
before = list(cdo.get_editor_property("plugins"))
cdo.set_editor_property("plugins", [])
ok = unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False)
unreal.log_warning("frame-lab guideline: plugins %s -> %s, saved=%s" % (before, list(cdo.get_editor_property("plugins")), ok))
