# Prints what every asset guideline loaded with BP_XRPawn really carries (the instances, not just the class default).
# UAssetGuideline::PostLoad prompts only when a listed plugin is off or a listed project setting differs, so two
# empty lists mean no "missing plugins" prompt.
import unreal
unreal.EditorAssetLibrary.load_asset("/Game/XRFramework/Blueprints/BP_XRPawn")
n = 0
for o in unreal.ObjectIterator(unreal.AssetGuideline):
    unreal.log_warning("frame-lab guideline: %s plugins=%s settings=%d" % (o.get_path_name(),
        list(o.get_editor_property("plugins")), len(o.get_editor_property("project_settings"))))
    n += 1
unreal.log_warning("frame-lab guideline: %d guideline objects" % n)
