// frame-lab unity-smoke: the Unity -> OXRSys -> Steam Frame probe, driven from the command line.
//
//   stage 1   Unity -batchmode -projectPath P -executeMethod FrameLab.Bootstrap.AddPackages -quit -logFile L
//             adds com.unity.xr.openxr (and its dependencies); the editor recompiles with FRAMELAB_OPENXR on the next run
//   stage 2   Unity -batchmode -projectPath P -executeMethod FrameLab.Bootstrap.Configure -quit -logFile L
//             Standalone XR: OpenXR loader first, init on startup, multi-pass, Touch Plus profile; linear colour; Input System;
//             a scene with a tracked camera and a 0.1 m cube 0.5 m ahead at eye height, saved and added to the build
//   play      FRAME_XR_RUNTIME_JSON=<oxrsys-runtime.json> Unity -projectPath P -executeMethod FrameLab.Bootstrap.Play -logFile L
//             (no -batchmode: Play Mode needs the editor) sets XR_RUNTIME_JSON for the editor process and enters Play
//   build     Unity -batchmode -projectPath P -executeMethod FrameLab.Bootstrap.BuildPlayer -quit -logFile L
//             a macOS arm64 player at <project>/../unity-build/unity-smoke.app; run it with XR_RUNTIME_JSON set
//
// MetalQuestLink (ahmetfirat23, Apache-2.0) is the prior art for the Standalone-loader-in-Play-Mode shape; OXRSys's
// EntryPoint.cpp lists "XR_KHRX2_metal_enable" as a Unity alias, so upstream intends this path.
using System;
using System.IO;
using UnityEditor;
using UnityEditor.PackageManager;
using UnityEditor.PackageManager.Requests;
using UnityEngine;

namespace FrameLab
{
    public static class Bootstrap
    {
        // ---- stage 1 -------------------------------------------------------------------------------------------
        public static void AddPackages()
        {
            var req = Client.AddAndRemove(new[] { "com.unity.xr.openxr", "com.unity.inputsystem" }, null);
            Pump(req, "AddAndRemove");
            if (req.Status != StatusCode.Success) { Debug.LogError("frame-lab: package add failed: " + req.Error?.message); EditorApplication.Exit(2); return; }
            foreach (var p in req.Result) Debug.Log("frame-lab: package " + p.name + " " + p.version);
            Debug.Log("frame-lab: stage 1 done");
        }

        // Panels (2026-10-25): the composition-layers package and uGUI for Scripts/FrameLabPanels.cs. Run on the sibling
        // project ~/src/unity-panels only, so the plain smoke project (tools/regress.sh unity) keeps its package set.
        // 2.5.0 is the release that pairs with OpenXR 1.18 (2.6.0 wants 1.19-pre; the package's installation.md).
        public static void AddPanelPackages()
        {
            var req = Client.AddAndRemove(new[] { "com.unity.xr.compositionlayers@2.5.0", "com.unity.ugui" }, null);
            Pump(req, "AddAndRemove");
            if (req.Status != StatusCode.Success) { Debug.LogError("frame-lab: panel package add failed: " + req.Error?.message); EditorApplication.Exit(2); return; }
            foreach (var p in req.Result) if (p.name.Contains("composition") || p.name.Contains("ugui")) Debug.Log("frame-lab: package " + p.name + " " + p.version);
            Debug.Log("frame-lab: panel packages done; run Configure next (it turns on Composition Layers Support)");
        }

        static void Pump(Request req, string what)
        {
            var t0 = DateTime.Now;
            while (!req.IsCompleted)
            {
                System.Threading.Thread.Sleep(100);
                if ((DateTime.Now - t0).TotalMinutes > 15) { Debug.LogError("frame-lab: " + what + " timed out"); EditorApplication.Exit(3); return; }
            }
        }

        // ---- stage 2 -------------------------------------------------------------------------------------------
        public static void Configure()
        {
#if FRAMELAB_OPENXR
            PlayerSettings.colorSpace = ColorSpace.Linear;
            SetActiveInputHandling(2);   // both: the OpenXR package wants the Input System, legacy input keeps working
            PlayerSettings.enableFrameTimingStats = true;   // FrameLabCmd perf line: cpu/gpu ms
            PlayerSettings.runInBackground = true;   // 2026-10-24: a player that loses focus pauses and the stream freezes
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneOSX, new[] { UnityEngine.Rendering.GraphicsDeviceType.Metal });

            var group = BuildTargetGroup.Standalone;
            // the asset: the registered config object, else any in the project, else the package's own (internal) creator
            UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget settings;
            EditorBuildSettings.TryGetConfigObject(UnityEngine.XR.Management.XRGeneralSettings.k_SettingsKey, out settings);
            if (settings == null)
            {
                var guids = AssetDatabase.FindAssets("t:XRGeneralSettingsPerBuildTarget");
                if (guids.Length > 0) settings = AssetDatabase.LoadAssetAtPath<UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget>(AssetDatabase.GUIDToAssetPath(guids[0]));
            }
            if (settings == null)
            {
                var m = typeof(UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget).GetMethod("GetOrCreate",
                    System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public);
                settings = m?.Invoke(null, null) as UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget;
            }
            if (settings == null) { Debug.LogError("frame-lab: XR Plug-in Management settings could not be created"); EditorApplication.Exit(4); return; }
            EditorBuildSettings.AddConfigObject(UnityEngine.XR.Management.XRGeneralSettings.k_SettingsKey, settings, true);
            if (!settings.HasSettingsForBuildTarget(group)) settings.CreateDefaultSettingsForBuildTarget(group);
            if (!settings.HasManagerSettingsForBuildTarget(group)) settings.CreateDefaultManagerSettingsForBuildTarget(group);
            var general = settings.SettingsForBuildTarget(group);
            var manager = settings.ManagerSettingsForBuildTarget(group);
            const string loaderType = "UnityEngine.XR.OpenXR.OpenXRLoader";
            bool have = false;
            foreach (var l in manager.activeLoaders) if (l != null && l.GetType().FullName == loaderType) have = true;
            if (!have && !UnityEditor.XR.Management.Metadata.XRPackageMetadataStore.AssignLoader(manager, loaderType, group))
            { Debug.LogError("frame-lab: could not assign the OpenXR loader for Standalone"); EditorApplication.Exit(4); return; }
            general.InitManagerOnStart = true;
            EditorUtility.SetDirty(general); EditorUtility.SetDirty(manager); EditorUtility.SetDirty(settings);

            UnityEditor.XR.OpenXR.Features.FeatureHelpers.RefreshFeatures(group);
            var xr = UnityEngine.XR.OpenXR.OpenXRSettings.GetSettingsForBuildTargetGroup(group);
            xr.renderMode = UnityEngine.XR.OpenXR.OpenXRSettings.RenderMode.MultiPass;   // the conservative first rung on Metal
            // FRAMELAB_RENDERMODE=single: both eyes in one instanced pass (2026-10-24, Bistro at ~50 fps multi-pass).
            if (Environment.GetEnvironmentVariable("FRAMELAB_RENDERMODE") == "single")
                xr.renderMode = UnityEngine.XR.OpenXR.OpenXRSettings.RenderMode.SinglePassInstanced;
            Debug.Log("frame-lab: render mode " + xr.renderMode);
            xr.depthSubmissionMode = UnityEngine.XR.OpenXR.OpenXRSettings.DepthSubmissionMode.None;
            int enabled = 0;
            foreach (var f in xr.GetFeatures())
            {
                var n = f.GetType().Name;
                // HandInteractionProfile off (2026-10-24): offered both, the runtime bound hand_interaction_ext and the sticks read nothing
                // OculusTouch (2026-10-24): the client reports /interaction_profiles/oculus/touch_controller and the runtime does not map
                // meta/touch_controller_plus onto it, so Touch Plus alone left the sticks dead; Unreal binds oculus/touch and works.
                bool want = n.Contains("OculusTouchController") || n.Contains("MetaQuestTouchPlus");
                if (n.Contains("HandInteractionProfile")) f.enabled = false;
                // exists only with com.unity.xr.compositionlayers installed (the panels project); requests the cylinder etc.
                // extensions and hands layers to the runtime (Scripts/FrameLabPanels.cs)
                if (n == "OpenXRCompositionLayersFeature") { f.enabled = true; Debug.Log("frame-lab: feature on: " + n); }
                if (want) { f.enabled = true; enabled++; Debug.Log("frame-lab: feature on: " + n); }
            }
            if (enabled == 0) Debug.LogWarning("frame-lab: no controller profile matched; the stream will run without input");
            EditorUtility.SetDirty(xr);
            AssetDatabase.SaveAssets();

            MakeScene();
            Debug.Log("frame-lab: stage 2 done");
#else
            Debug.LogError("frame-lab: FRAMELAB_OPENXR is not defined - run AddPackages first (and let the editor recompile)");
            EditorApplication.Exit(5);
#endif
        }

        static void SetActiveInputHandling(int v)
        {
            var ps = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/ProjectSettings.asset");
            if (ps == null || ps.Length == 0) return;
            var so = new SerializedObject(ps[0]);
            var p = so.FindProperty("activeInputHandler");
            if (p != null) { p.intValue = v; so.ApplyModifiedPropertiesWithoutUndo(); }
        }

        // FRAMELAB_SCENE=<Assets/...unity>: adopt that scene (a template's sample, a real environment) instead of building the
        // smoke one (2026-10-21: before shipping, load a larger Unity and Unreal environment to see what errors come
        // up). The scene's lighting, post-processing and skybox stay as they are; its camera
        // gets the rig. Play and BuildPlayer read the same variable, so the editor plays what Configure prepared.
        static string ScenePath()
        {
            var e = Environment.GetEnvironmentVariable("FRAMELAB_SCENE");
            if (!string.IsNullOrEmpty(e)) return e;
            if (File.Exists("Assets/Scenes/Smoke.unity")) return "Assets/Scenes/Smoke.unity";
            // a project Configure prepared from an adopted scene (unity-urp): the build list's first scene is the one it set
            foreach (var bs in EditorBuildSettings.scenes) if (bs.enabled && File.Exists(bs.path)) return bs.path;
            return "Assets/Scenes/Smoke.unity";
        }

        static void MakeScene()
        {
#if FRAMELAB_OPENXR   // the Input System (TrackedPoseDriver) arrives with the OpenXR package
            if (ScenePath() != "Assets/Scenes/Smoke.unity") { AdoptScene(ScenePath()); return; }
            var scene = UnityEditor.SceneManagement.EditorSceneManager.NewScene(
                UnityEditor.SceneManagement.NewSceneSetup.DefaultGameObjects, UnityEditor.SceneManagement.NewSceneMode.Single);
            var cam = Camera.main;
            cam.transform.position = Vector3.zero;
            cam.nearClipPlane = 0.05f;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0.02f, 0.02f, 0.05f, 1f);
            cam.gameObject.AddComponent<UnityEngine.InputSystem.XR.TrackedPoseDriver>();
            cam.gameObject.AddComponent<FrameLab.FrameLabRig>();   // floor origin + a pose report every 5 s
            var origin = new GameObject("XR Origin (floor)");
            cam.transform.SetParent(origin.transform, false);   // tracking origin = floor: the runtime reports the head height
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = "cube 0.5 m ahead";
            cube.transform.position = new Vector3(0f, 1.6f, 0.5f);   // +Z ahead in Unity; 1.6 m = eye height over the floor origin
            cube.transform.localScale = Vector3.one * 0.1f;
            var shader = Shader.Find("Standard");                                   // built-in pipeline (what -createProject makes)
            if (shader == null) shader = Shader.Find("Universal Render Pipeline/Lit");  // URP template
            if (shader != null) { var mat = new Material(shader); mat.color = new Color(0.9f, 0.3f, 0.2f); cube.GetComponent<Renderer>().sharedMaterial = mat; }
            else Debug.LogWarning("frame-lab: neither Standard nor URP/Lit shader found; the cube keeps the primitive's default material");
            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane); floor.name = "floor"; floor.transform.localScale = Vector3.one * 0.4f;
            Directory.CreateDirectory("Assets/Scenes");
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene, "Assets/Scenes/Smoke.unity");
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene("Assets/Scenes/Smoke.unity", true) };
            AssetDatabase.SaveAssets();
#endif
        }

#if FRAMELAB_OPENXR
        static void AdoptScene(string path)
        {
            if (!File.Exists(path)) { Debug.LogError("frame-lab: FRAMELAB_SCENE " + path + " does not exist"); EditorApplication.Exit(7); return; }
            var scene = UnityEditor.SceneManagement.EditorSceneManager.OpenScene(path, UnityEditor.SceneManagement.OpenSceneMode.Single);
            var cam = Camera.main;
            if (cam == null) cam = UnityEngine.Object.FindFirstObjectByType<Camera>();
            if (cam == null) { Debug.LogError("frame-lab: no Camera in " + path); EditorApplication.Exit(7); return; }
            // Re-adopting an adopted scene (every Configure run) took the lowered camera as the authored one and sank the
            // origin another 1.6 m each time (2026-10-24: each relaunch put the wearer lower).
            if (cam.transform.parent != null && cam.transform.parent.name == "XR Origin (floor)")
            {
                Debug.Log("frame-lab: adopted " + path + " already: origin at " + cam.transform.parent.position.ToString("F2") + ", left as is");
                EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(path, true) };
                return;
            }
            var camPos = cam.transform.position; var camYaw = cam.transform.eulerAngles.y;
            cam.nearClipPlane = 0.05f;
            // The tracking origin is the floor (the rig asks for Floor mode), so the origin goes on the floor under the scene
            // camera, facing the camera's yaw; the camera's own placement (an authored shot) is replaced by the head pose.
            // A camera 0.5-3 m up is taken as eye height over a y=0 floor; anything else keeps its y minus 1.6 m.
            // Better: the top of the highest flat mesh under the camera (a street, a floor slab), taken from renderer bounds
            // because imported meshes have no colliders to raycast; FRAMELAB_FLOOR_Y overrides both.
            float originY = (camPos.y > 0.5f && camPos.y < 3f) ? 0f : camPos.y - 1.6f;
            string floorFrom = "eye-height rule";
            float best = float.NegativeInfinity;
            foreach (var r in UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None))
            {
                var b = r.bounds;
                if (b.size.y > 1.0f || b.max.y >= camPos.y - 0.3f) continue;
                if (camPos.x < b.min.x || camPos.x > b.max.x || camPos.z < b.min.z || camPos.z > b.max.z) continue;
                if (b.max.y > best) { best = b.max.y; floorFrom = "mesh '" + r.name + "'"; }
            }
            if (best > float.NegativeInfinity) originY = best;
            var fy = Environment.GetEnvironmentVariable("FRAMELAB_FLOOR_Y");
            if (!string.IsNullOrEmpty(fy) && float.TryParse(fy, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var fyv)) { originY = fyv; floorFrom = "FRAMELAB_FLOOR_Y"; }
            Debug.Log("frame-lab: floor y " + originY.ToString("F2") + " from " + floorFrom);
            var origin = new GameObject("XR Origin (floor)");
            origin.transform.position = new Vector3(camPos.x, originY, camPos.z);
            origin.transform.rotation = Quaternion.Euler(0f, camYaw, 0f);
            cam.transform.SetParent(origin.transform, false);
            cam.transform.localPosition = Vector3.zero; cam.transform.localRotation = Quaternion.identity;
            if (cam.GetComponent<UnityEngine.InputSystem.XR.TrackedPoseDriver>() == null) cam.gameObject.AddComponent<UnityEngine.InputSystem.XR.TrackedPoseDriver>();
            if (cam.GetComponent<FrameLab.FrameLabRig>() == null) cam.gameObject.AddComponent<FrameLab.FrameLabRig>();
            int renderers = UnityEngine.Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None).Length;
            int lights = UnityEngine.Object.FindObjectsByType<Light>(FindObjectsSortMode.None).Length;
            Debug.Log("frame-lab: adopted " + path + ": camera '" + cam.name + "' was at " + camPos.ToString("F2") + " yaw " + camYaw.ToString("F0")
                + " -> origin at " + origin.transform.position.ToString("F2") + "; renderers " + renderers + " lights " + lights
                + " clear " + cam.clearFlags + " cube " + (GameObject.Find("cube 0.5 m ahead") ? "present" : "absent (the rig logs 'missing')"));
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene, path);
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(path, true) };
        }
#endif

        // ---- play / build --------------------------------------------------------------------------------------
        public static void Play()
        {
            var json = Environment.GetEnvironmentVariable("FRAME_XR_RUNTIME_JSON");
            if (string.IsNullOrEmpty(json) || !File.Exists(json)) { Debug.LogError("frame-lab: FRAME_XR_RUNTIME_JSON must point at oxrsys-runtime.json"); return; }
            Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", json);   // the loader reads it at xrCreateInstance, i.e. at Play
            // Play runs whatever scene the editor has open; a fresh editor opens an empty default scene (skybox, no cube),
            // which is what the Frame showed on the first wear. Open the smoke scene first.
            string scenePath = ScenePath();   // FRAMELAB_SCENE or the smoke scene (open --env passes it to the editor)
            if (UnityEditor.SceneManagement.EditorSceneManager.GetActiveScene().path != scenePath)
            {
                UnityEditor.SceneManagement.EditorSceneManager.OpenScene(scenePath, UnityEditor.SceneManagement.OpenSceneMode.Single);
                Debug.Log("frame-lab: opened " + scenePath);
            }
            Debug.Log("frame-lab: XR_RUNTIME_JSON=" + json + "; entering Play");
            // A single delayCall was dropped on an unattended relaunch (2026-10-20: "entering Play" logged, no
            // "Reloading assemblies for play mode", editor idle). Ask directly and keep asking until it is playing.
            EditorApplication.isPlaying = true;
            double last = EditorApplication.timeSinceStartup;
            int asks = 0;
            EditorApplication.CallbackFunction watch = null;
            watch = () =>
            {
                if (EditorApplication.isPlaying) { Debug.Log("frame-lab: play mode entered after " + asks + " extra asks"); EditorApplication.update -= watch; return; }
                if (EditorApplication.timeSinceStartup - last < 3.0) return;
                last = EditorApplication.timeSinceStartup;
                if (++asks > 20) { Debug.LogError("frame-lab: play mode never entered"); EditorApplication.update -= watch; return; }
                Debug.Log("frame-lab: not playing yet (compiling=" + EditorApplication.isCompiling + ", updating=" + EditorApplication.isUpdating + "); asking again");
                EditorApplication.EnterPlaymode();
            };
            EditorApplication.update += watch;
        }

        public static void BuildPlayer()
        {
            var outDir = Path.GetFullPath(Path.Combine(Application.dataPath, "..", "..", "unity-smoke-build"));
            Directory.CreateDirectory(outDir);
            var opts = new BuildPlayerOptions
            {
                // FRAMELAB_SCENES=a.unity,b.unity: several adopted scenes in one player, the first one boots (FrameLabCmd load)
                scenes = Environment.GetEnvironmentVariable("FRAMELAB_SCENES") is string ss && ss.Length > 0 ? ss.Split(',') : new[] { ScenePath() },
                locationPathName = Path.Combine(outDir, "unity-smoke.app"),
                target = BuildTarget.StandaloneOSX,
                options = BuildOptions.None,
            };
            UnityEditor.OSXStandalone.UserBuildSettings.architecture = UnityEditor.Build.OSArchitecture.ARM64;
            var report = BuildPipeline.BuildPlayer(opts);
            Debug.Log("frame-lab: build " + report.summary.result + " -> " + opts.locationPathName);
            if (report.summary.result != UnityEditor.Build.Reporting.BuildResult.Succeeded) EditorApplication.Exit(6);
        }
    }
}
