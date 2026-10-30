// frame-lab 2026-10-24: any model -> a scene the rig can adopt, for more test environments than the Bistro (one test
// environment is not enough). First users: the McGuire archive's San Miguel, Rungholt, Power Plant.
// Run: FRAMELAB_MODEL=Assets/Scenes3/rungholt/rungholt.obj [FRAMELAB_MODEL_SCALE=1] Unity -batchmode -projectPath ...
//      -executeMethod FrameLab.ModelScene.Make -quit   -> <model dir>/<name>.unity
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace FrameLab
{
    public static class ModelScene
    {
        public static void Make()
        {
            var t0 = System.DateTime.Now;
            string model = System.Environment.GetEnvironmentVariable("FRAMELAB_MODEL");
            float scale = float.TryParse(System.Environment.GetEnvironmentVariable("FRAMELAB_MODEL_SCALE"), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var s) ? s : 1f;
            AssetDatabase.Refresh();
            var mi = AssetImporter.GetAtPath(model ?? "") as ModelImporter;
            if (mi == null) { Debug.LogError("frame-lab model: no model importer for " + model); EditorApplication.Exit(3); return; }
            mi.indexFormat = ModelImporterIndexFormat.UInt32;   // these run to millions of vertices per mesh
            mi.globalScale = scale; mi.isReadable = false; mi.importCameras = false; mi.importLights = false;
            mi.materialImportMode = ModelImporterMaterialImportMode.ImportViaMaterialDescription;
            mi.SaveAndReimport();
            string dir = System.IO.Path.GetDirectoryName(model), name = System.IO.Path.GetFileNameWithoutExtension(model);
            string outPath = dir + "/" + name + ".unity";
            if (System.IO.File.Exists(outPath)) AssetDatabase.DeleteAsset(outPath);
            var scene = UnityEditor.SceneManagement.EditorSceneManager.NewScene(
                UnityEditor.SceneManagement.NewSceneSetup.EmptyScene, UnityEditor.SceneManagement.NewSceneMode.Single);
            var root = (GameObject)PrefabUtility.InstantiatePrefab(AssetDatabase.LoadAssetAtPath<GameObject>(model));
            foreach (var g in root.GetComponentsInChildren<Transform>()) g.gameObject.isStatic = true;
            var rs = root.GetComponentsInChildren<MeshRenderer>();
            long tris = 0;
            foreach (var mf in root.GetComponentsInChildren<MeshFilter>()) if (mf.sharedMesh) for (int i = 0; i < mf.sharedMesh.subMeshCount; i++) tris += mf.sharedMesh.GetIndexCount(i) / 3;
            var b = rs[0].bounds; foreach (var r in rs) b.Encapsulate(r.bounds);
            var sun = new GameObject("Sun").AddComponent<Light>();
            sun.type = LightType.Directional; sun.intensity = 1.3f; sun.shadows = LightShadows.Soft;
            sun.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = new Color(0.55f, 0.6f, 0.7f); RenderSettings.ambientEquatorColor = new Color(0.4f, 0.38f, 0.35f);
            RenderSettings.ambientGroundColor = new Color(0.2f, 0.18f, 0.16f);
            // start at the middle, standing on whatever is under it (temporary colliders, removed before the save)
            var cols = root.GetComponentsInChildren<MeshFilter>().Where(f => f.sharedMesh).Select(f => f.gameObject.AddComponent<MeshCollider>()).ToList();
            Physics.SyncTransforms();
            float floorY = b.min.y; string hitName = "none";
            if (Physics.Raycast(new Vector3(b.center.x, b.max.y + 1f, b.center.z), Vector3.down, out var hit, b.size.y + 2f)) { floorY = hit.point.y; hitName = hit.collider.name; }
            foreach (var c in cols) Object.DestroyImmediate(c);
            var cam = new GameObject("Main Camera") { tag = "MainCamera" }.AddComponent<Camera>();
            cam.transform.position = new Vector3(b.center.x, floorY + 1.6f, b.center.z);
            cam.clearFlags = CameraClearFlags.Skybox; cam.farClipPlane = Mathf.Max(1000f, b.size.magnitude * 1.2f);
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene, outPath);
            Debug.Log("frame-lab model: " + outPath + ": " + rs.Length + " renderers, " + tris + " tris, bounds " + b.min.ToString("F1") + " .. " + b.max.ToString("F1")
                + " (size " + b.size.ToString("F1") + "), floor " + floorY.ToString("F2") + " from " + hitName + ", scale " + scale + ", took " + (System.DateTime.Now - t0).TotalSeconds.ToString("F0") + " s");
        }
    }
}
