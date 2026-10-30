// frame-lab unity-smoke: a generated "larger environment" for the Frame stream (2026-10-21: before shipping, load a larger
// Unity and Unreal environment to see what errors come up).
//
// The editor's two bundled templates are nearly empty (Universal 3D: 17 objects; High-End 3D: 20), and a real asset
// environment needs an Asset Store sign-in, which a script cannot do. So this builds one: a 200 m Perlin terrain with a
// procedural texture, ~2000 URP/Lit renderers in 24 materials (some textured, some transparent), 12 point lights
// (2 with shadows) and a spot, a realtime reflection probe, and the template's Global Volume (bloom, tonemapping,
// vignette) left as it is. The URP asset's MSAA is set here too (QualitySettings.antiAliasing is overridden by the
// pipeline asset under URP, so the rig's Awake setting would not stick): 4x, FRAMELAB_MSAA=0 for the A/B.
//
//   Unity -batchmode -projectPath P -executeMethod FrameLab.Stress.Build -quit -logFile L
//     opens FRAMELAB_SCENE (default Assets/Scenes/SampleScene.unity), fills it, saves it as Assets/Scenes/Stress.unity
//   then Configure with FRAMELAB_SCENE=Assets/Scenes/Stress.unity gives its camera the rig.
#if FRAMELAB_URP   // the file is empty in a built-in-pipeline project (the smoke project): the asmdef defines it with the URP package
using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace FrameLab
{
    public static class Stress
    {
        const string OutScene = "Assets/Scenes/Stress.unity";
        const string Dir = "Assets/Stress";

        static Texture2D NoiseTexture(string name, int size, Color a, Color b, float scale, int seed)
        {
            var t = new Texture2D(size, size, TextureFormat.RGBA32, true); var px = new Color[size * size];
            for (int y = 0; y < size; y++) for (int x = 0; x < size; x++)
            {
                float n = Mathf.PerlinNoise(seed + x * scale / size, seed + y * scale / size) * 0.7f + Mathf.PerlinNoise(seed * 3 + x * scale * 4f / size, seed * 3 + y * scale * 4f / size) * 0.3f;
                px[y * size + x] = Color.Lerp(a, b, n);
            }
            t.SetPixels(px); t.Apply(true); t.name = name;
            AssetDatabase.CreateAsset(t, Dir + "/" + name + ".asset");
            return t;
        }

        static Material Lit(string name, Color c, float smooth, float metal, Texture2D tex, bool transparent)
        {
            var sh = Shader.Find("Universal Render Pipeline/Lit");
            var m = new Material(sh) { name = name };
            m.SetColor("_BaseColor", c); m.SetFloat("_Smoothness", smooth); m.SetFloat("_Metallic", metal);
            if (tex) m.SetTexture("_BaseMap", tex);
            if (transparent)
            {
                m.SetFloat("_Surface", 1f); m.SetFloat("_Blend", 0f); m.SetOverrideTag("RenderType", "Transparent");
                m.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); m.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha); m.SetFloat("_ZWrite", 0f);
                m.EnableKeyword("_SURFACE_TYPE_TRANSPARENT"); m.renderQueue = (int)RenderQueue.Transparent;
            }
            AssetDatabase.CreateAsset(m, Dir + "/" + name + ".mat");
            return m;
        }

        public static void Build()
        {
            var scenePath = Environment.GetEnvironmentVariable("FRAMELAB_SCENE"); if (string.IsNullOrEmpty(scenePath)) scenePath = "Assets/Scenes/SampleScene.unity";
            var scene = UnityEditor.SceneManagement.EditorSceneManager.OpenScene(scenePath, UnityEditor.SceneManagement.OpenSceneMode.Single);
            if (Directory.Exists(Dir)) AssetDatabase.DeleteAsset(Dir);
            AssetDatabase.CreateFolder("Assets", "Stress");
            var cam = Camera.main; if (cam == null) cam = UnityEngine.Object.FindFirstObjectByType<Camera>();
            var c = cam ? cam.transform.position : Vector3.zero; c.y = 0f;   // everything around the camera's floor point
            var rng = new System.Random(7);
            Func<float, float, float> R = (lo, hi) => lo + (float)rng.NextDouble() * (hi - lo);

            // --- URP asset: MSAA (the pipeline asset owns it under URP), and record what else it carries
            var urp = GraphicsSettings.currentRenderPipeline as UniversalRenderPipelineAsset;
            if (QualitySettings.renderPipeline is UniversalRenderPipelineAsset q) urp = q;
            var msaaEnv = Environment.GetEnvironmentVariable("FRAMELAB_MSAA"); int msaa = 4; if (!string.IsNullOrEmpty(msaaEnv)) int.TryParse(msaaEnv, out msaa);
            if (urp != null)
            {
                int before = urp.msaaSampleCount; urp.msaaSampleCount = msaa <= 1 ? 1 : msaa; EditorUtility.SetDirty(urp);
                Debug.Log("frame-lab stress: URP asset '" + urp.name + "' msaa " + before + " -> " + urp.msaaSampleCount + ", renderScale " + urp.renderScale + ", hdr " + urp.supportsHDR
                    + ", depthTex " + urp.supportsCameraDepthTexture + ", opaqueTex " + urp.supportsCameraOpaqueTexture + ", shadowmap " + urp.mainLightShadowmapResolution + ", shadowDistance " + urp.shadowDistance
                    + ", additionalLights " + urp.additionalLightsRenderingMode + " max/obj " + urp.maxAdditionalLightsCount + ", upscaling " + urp.upscalingFilter);
            }
            else Debug.LogWarning("frame-lab stress: no URP asset is active (QualitySettings.renderPipeline / GraphicsSettings)");

            // --- textures and materials
            var texA = NoiseTexture("tex-ground", 1024, new Color(0.35f, 0.3f, 0.22f), new Color(0.55f, 0.5f, 0.35f), 6f, 1);
            var texB = NoiseTexture("tex-stone", 512, new Color(0.4f, 0.4f, 0.42f), new Color(0.8f, 0.78f, 0.75f), 10f, 2);
            var texC = NoiseTexture("tex-paint", 512, new Color(0.9f, 0.2f, 0.15f), new Color(0.95f, 0.75f, 0.2f), 3f, 3);
            var mats = new List<Material>();
            for (int i = 0; i < 24; i++)
            {
                var col = Color.HSVToRGB(i / 24f, i % 3 == 0 ? 0.2f : 0.7f, i % 2 == 0 ? 0.9f : 0.6f);
                var tex = i % 4 == 1 ? texB : i % 4 == 3 ? texC : null;
                bool tr = i % 8 == 7; if (tr) col.a = 0.45f;
                mats.Add(Lit("mat-" + i, col, i % 5 * 0.22f, i % 6 == 0 ? 0.9f : 0.05f, tex, tr));
            }

            // --- terrain: 200 m square, 30 m tall Perlin hills, 2 m flat disc under the camera
            var td = new TerrainData { heightmapResolution = 257, size = new Vector3(200f, 30f, 200f) };
            var h = new float[257, 257];
            for (int y = 0; y < 257; y++) for (int x = 0; x < 257; x++)
            {
                float wx = x / 256f * 200f - 100f, wz = y / 256f * 200f - 100f;   // terrain-local around the camera
                float d = Mathf.Sqrt(wx * wx + wz * wz);
                float hn = Mathf.PerlinNoise(x * 0.02f + 5f, y * 0.02f + 5f) * 0.6f + Mathf.PerlinNoise(x * 0.08f, y * 0.08f) * 0.15f;
                float flat = Mathf.Clamp01((d - 6f) / 20f);   // flat within 6 m of the camera, rising beyond 26 m
                h[y, x] = 0.05f + hn * flat;
            }
            td.SetHeights(0, 0, h);
            var layer = new TerrainLayer { diffuseTexture = texA, tileSize = new Vector2(8f, 8f), name = "ground" };
            AssetDatabase.CreateAsset(layer, Dir + "/ground.terrainlayer");
            td.terrainLayers = new[] { layer };
            AssetDatabase.CreateAsset(td, Dir + "/terrain.asset");
            var terrain = Terrain.CreateTerrainGameObject(td); terrain.name = "terrain 200 m";
            terrain.transform.position = new Vector3(c.x - 100f, -0.05f * 30f - 0.02f, c.z - 100f);   // base height 0.05 lands at y ~ 0
            var tc = terrain.GetComponent<Terrain>(); tc.heightmapPixelError = 5f; tc.shadowCastingMode = ShadowCastingMode.On;

            // --- ~2000 renderers: a 36x36 field of varied primitives (2 m pitch, 3-36 m out), 300 "trees", 40 big slabs
            var root = new GameObject("stress props").transform; int n = 0;
            var types = new[] { PrimitiveType.Cube, PrimitiveType.Sphere, PrimitiveType.Cylinder, PrimitiveType.Capsule };
            for (int gx = -18; gx < 18; gx++) for (int gz = -18; gz < 18; gz++)
            {
                float x = c.x + gx * 2f + R(-0.6f, 0.6f), z = c.z + gz * 2f + R(-0.6f, 0.6f);
                float d = Mathf.Sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z)); if (d < 3f) continue;
                float s = R(0.15f, 0.9f);
                var g = GameObject.CreatePrimitive(types[n % 4]); g.name = "prop " + n; g.transform.SetParent(root);
                g.transform.position = new Vector3(x, tc.SampleHeight(new Vector3(x, 0, z)) + terrain.transform.position.y + s * 0.5f, z);
                g.transform.rotation = Quaternion.Euler(0f, R(0f, 360f), 0f); g.transform.localScale = Vector3.one * s;
                g.GetComponent<Renderer>().sharedMaterial = mats[n % 24]; UnityEngine.Object.DestroyImmediate(g.GetComponent<Collider>()); n++;
            }
            for (int i = 0; i < 300; i++)
            {
                float a = R(0f, Mathf.PI * 2f), d = R(8f, 90f); float x = c.x + Mathf.Cos(a) * d, z = c.z + Mathf.Sin(a) * d;
                float y = tc.SampleHeight(new Vector3(x, 0, z)) + terrain.transform.position.y; float ht = R(3f, 9f);
                var trunk = GameObject.CreatePrimitive(PrimitiveType.Cylinder); trunk.name = "trunk " + i; trunk.transform.SetParent(root);
                trunk.transform.position = new Vector3(x, y + ht * 0.5f, z); trunk.transform.localScale = new Vector3(0.3f, ht * 0.5f, 0.3f); trunk.GetComponent<Renderer>().sharedMaterial = mats[1];
                var crown = GameObject.CreatePrimitive(PrimitiveType.Sphere); crown.name = "crown " + i; crown.transform.SetParent(root);
                crown.transform.position = new Vector3(x, y + ht, z); crown.transform.localScale = Vector3.one * R(2f, 5f); crown.GetComponent<Renderer>().sharedMaterial = mats[i % 2 == 0 ? 8 : 9];
                UnityEngine.Object.DestroyImmediate(trunk.GetComponent<Collider>()); UnityEngine.Object.DestroyImmediate(crown.GetComponent<Collider>()); n += 2;
            }
            for (int i = 0; i < 40; i++)
            {
                float a = R(0f, Mathf.PI * 2f), d = R(12f, 80f); float x = c.x + Mathf.Cos(a) * d, z = c.z + Mathf.Sin(a) * d;
                var g = GameObject.CreatePrimitive(PrimitiveType.Cube); g.name = "slab " + i; g.transform.SetParent(root);
                g.transform.position = new Vector3(x, tc.SampleHeight(new Vector3(x, 0, z)) + terrain.transform.position.y + 3f, z);
                g.transform.rotation = Quaternion.Euler(0f, R(0f, 360f), 0f); g.transform.localScale = new Vector3(R(4f, 12f), 6f, 0.5f);
                g.GetComponent<Renderer>().sharedMaterial = mats[i % 3 == 0 ? 23 : 5]; UnityEngine.Object.DestroyImmediate(g.GetComponent<Collider>()); n++;
            }
            // a near ring at 1-2 m: thin edges for the shimmer verdict, at the heights of the smoke's props
            for (int i = 0; i < 16; i++)
            {
                float a = i / 16f * Mathf.PI * 2f; var g = GameObject.CreatePrimitive(types[i % 4]); g.name = "near " + i; g.transform.SetParent(root);
                g.transform.position = new Vector3(c.x + Mathf.Cos(a) * 1.6f, 1.0f + (i % 3) * 0.4f, c.z + Mathf.Sin(a) * 1.6f); g.transform.localScale = Vector3.one * 0.12f;
                g.GetComponent<Renderer>().sharedMaterial = mats[(i * 5) % 24]; UnityEngine.Object.DestroyImmediate(g.GetComponent<Collider>()); n++;
            }

            // --- lights: 12 points (2 shadowed), 1 spot; the template's directional stays
            for (int i = 0; i < 12; i++)
            {
                var l = new GameObject("point " + i).AddComponent<Light>(); l.transform.SetParent(root); l.type = LightType.Point;
                float a = i / 12f * Mathf.PI * 2f; l.transform.position = new Vector3(c.x + Mathf.Cos(a) * R(4f, 14f), R(2f, 5f), c.z + Mathf.Sin(a) * R(4f, 14f));
                l.color = Color.HSVToRGB(i / 12f, 0.6f, 1f); l.intensity = R(4f, 10f); l.range = R(8f, 16f); l.shadows = i < 2 ? LightShadows.Soft : LightShadows.None;
            }
            var spot = new GameObject("spot").AddComponent<Light>(); spot.transform.SetParent(root); spot.type = LightType.Spot; spot.transform.position = new Vector3(c.x, 6f, c.z + 4f);
            spot.transform.rotation = Quaternion.Euler(70f, 0f, 0f); spot.spotAngle = 60f; spot.intensity = 20f; spot.range = 25f; spot.shadows = LightShadows.Soft;
            var probe = new GameObject("reflection probe").AddComponent<ReflectionProbe>(); probe.transform.SetParent(root); probe.transform.position = new Vector3(c.x, 1.6f, c.z);
            probe.mode = ReflectionProbeMode.Realtime; probe.refreshMode = ReflectionProbeRefreshMode.OnAwake; probe.size = new Vector3(60f, 20f, 60f); probe.resolution = 256;

            // the template's Global Volume: report what it carries, leave it
            var vols = UnityEngine.Object.FindObjectsByType<Volume>(FindObjectsSortMode.None); var fx = new List<string>();
            foreach (var v in vols) if (v.sharedProfile) foreach (var comp in v.sharedProfile.components) fx.Add(comp.name + (comp.active ? "" : "(off)"));
            var sun = UnityEngine.Object.FindFirstObjectByType<Light>();
            Debug.Log("frame-lab stress: " + n + " renderers + terrain 257x257, 13 extra lights, volume [" + string.Join(", ", fx) + "], camera at " + (cam ? cam.transform.position.ToString("F1") : "none")
                + " clear " + (cam ? cam.clearFlags.ToString() : "none") + "; colour space " + PlayerSettings.colorSpace);
            AssetDatabase.SaveAssets();
            Directory.CreateDirectory("Assets/Scenes");
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene, OutScene);
            Debug.Log("frame-lab stress: saved " + OutScene);
        }

        // The Universal 3D template ships materials at URP material version 9 and URP 17 has 10 upgraders, so the first
        // rendered editor frame opens a modal "URP Material upgrade" dialog (OK only) and the editor idles behind it at
        // 0 % CPU: Play never submits a frame and the client says "no server found" (2026-10-21 16:06, read from the
        // main thread with lldb + mono_pmip). This runs the same upgrade with no dialog: URP's own ReimportAllMaterials
        // then SaveAssetsToDisk, which writes the version to ProjectSettings/URPProjectSettings.asset. Both are internal.
        //   Unity -batchmode -projectPath P -executeMethod FrameLab.Stress.UpgradeMaterials -quit -logFile L
        public static void UpgradeMaterials()
        {
            const System.Reflection.BindingFlags F = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static;
            var asm = Array.Find(AppDomain.CurrentDomain.GetAssemblies(), a => a.GetName().Name == "Unity.RenderPipelines.Universal.Editor");
            var re = asm.GetType("UnityEditor.Rendering.Universal.MaterialReimporter"); var pp = asm.GetType("UnityEditor.Rendering.Universal.MaterialPostprocessor");
            var st = asm.GetType("UnityEditor.Rendering.Universal.UniversalProjectSettings");
            var ver = st?.GetProperty("materialVersionForUpgrade", F | System.Reflection.BindingFlags.Public);
            Debug.Log("frame-lab stress: URP material version before " + (ver != null ? ver.GetValue(null) : "?"));
            re.GetMethod("ReimportAllMaterials", F).Invoke(null, null);
            pp.GetMethod("SaveAssetsToDisk", F).Invoke(null, null);
            Debug.Log("frame-lab stress: URP material version after " + (ver != null ? ver.GetValue(null) : "?"));
        }

        // The 90 Hz budget (2026-10-21 evening, unity-urp/README.md "90 Hz"), measured one knob at a time with
        // FrameLabBudget's FRAMELAB_STEPS: Single Pass Instanced, URP MSAA 2x, point/spot shadows off, post-processing
        // off. Render scale is NOT used: URP drives the XR eye texture with it, and a 1944 swapchain is below the
        // runtime's 2160 target, which foveated encoding refuses. Writes the URP asset, the OpenXR settings and the scene.
        //   Unity -batchmode -projectPath P -executeMethod FrameLab.Stress.Budget90 -quit -logFile L
        public static void Budget90()
        {
            var asset = (QualitySettings.renderPipeline ?? GraphicsSettings.defaultRenderPipeline) as UniversalRenderPipelineAsset;
            if (asset == null) { Debug.LogError("frame-lab stress: no URP asset on the active quality level"); EditorApplication.Exit(4); return; }
            Debug.Log("frame-lab stress: budget90 URP asset " + AssetDatabase.GetAssetPath(asset) + " msaa " + asset.msaaSampleCount + " -> 2");
            asset.msaaSampleCount = 2;
            // Worn 2026-10-22: MSAA 2x looks better than off, and one cascade at 25 m is what lets 2x hold ~90 with
            // the client attached (unity-urp README "MSAA 2x to 90").
            Debug.Log("frame-lab stress: budget90 shadow cascades " + asset.shadowCascadeCount + " -> 1, distance " + asset.shadowDistance + " -> 25");
            asset.shadowCascadeCount = 1; asset.shadowDistance = 25f; EditorUtility.SetDirty(asset);
#if FRAMELAB_OPENXR
            var xr = UnityEngine.XR.OpenXR.OpenXRSettings.GetSettingsForBuildTargetGroup(BuildTargetGroup.Standalone);
            Debug.Log("frame-lab stress: budget90 OpenXR render mode " + xr.renderMode + " -> SinglePassInstanced");
            xr.renderMode = UnityEngine.XR.OpenXR.OpenXRSettings.RenderMode.SinglePassInstanced; EditorUtility.SetDirty(xr);
#endif
            var scene = UnityEditor.SceneManagement.EditorSceneManager.OpenScene(OutScene, UnityEditor.SceneManagement.OpenSceneMode.Single);
            int off = 0;
            foreach (var l in UnityEngine.Object.FindObjectsByType<Light>(FindObjectsSortMode.None))
                if ((l.type == LightType.Point || l.type == LightType.Spot) && l.shadows != LightShadows.None) { l.shadows = LightShadows.None; off++; }
            int post = 0;
            foreach (var c in UnityEngine.Object.FindObjectsByType<Camera>(FindObjectsSortMode.None))
            { var cd = c.GetUniversalAdditionalCameraData(); if (cd != null && cd.renderPostProcessing) { cd.renderPostProcessing = false; post++; } }
            Debug.Log("frame-lab stress: budget90 shadows off on " + off + " point/spot lights, post-processing off on " + post + " camera(s)");
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene);
            AssetDatabase.SaveAssets();
        }
    }
}
#endif
