// frame-lab 2026-10-24: build Assets/Bistro/Bistro.unity from the Lumberyard Bistro FBX (CC-BY 4.0), for the
// Unity-vs-Unreal load comparison. Run: Unity -batchmode -projectPath ... -executeMethod FrameLab.BistroScene.Make -quit
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace FrameLab
{
    public static class BistroScene
    {
        const string Fbx = "Assets/Bistro/BistroExterior.fbx";
        public static void Make()
        {
            var t0 = System.DateTime.Now;
            AssetDatabase.Refresh();
            var mi = (ModelImporter)AssetImporter.GetAtPath(Fbx);
            if (mi == null) { Debug.LogError("frame-lab bistro: no importer for " + Fbx); EditorApplication.Exit(3); return; }
            mi.materialImportMode = ModelImporterMaterialImportMode.ImportViaMaterialDescription;
            mi.materialLocation = ModelImporterMaterialLocation.InPrefab;
            mi.isReadable = System.Environment.GetEnvironmentVariable("FRAMELAB_READABLE") == "1";   // FrameLabCmd "batch" needs readable meshes; off by default (A/B 2026-10-24)
            mi.importCameras = true; mi.importLights = false;
            mi.SaveAndReimport();
            if (System.IO.File.Exists("Assets/Bistro/Bistro.unity")) AssetDatabase.DeleteAsset("Assets/Bistro/Bistro.unity");
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(Fbx);
            var scene = UnityEditor.SceneManagement.EditorSceneManager.NewScene(
                UnityEditor.SceneManagement.NewSceneSetup.EmptyScene, UnityEditor.SceneManagement.NewSceneMode.Single);
            var root = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
            var rs = root.GetComponentsInChildren<MeshRenderer>();
            long tris = 0;
            foreach (var mf in root.GetComponentsInChildren<MeshFilter>()) if (mf.sharedMesh) tris += mf.sharedMesh.triangles.LongLength / 3;
            var b = rs[0].bounds; foreach (var r in rs) b.Encapsulate(r.bounds);
            int texd = rs.SelectMany(r => r.sharedMaterials).Where(m => m && m.mainTexture).Distinct().Count();
            var sun = new GameObject("Sun").AddComponent<Light>();
            sun.type = LightType.Directional; sun.intensity = 1.3f; sun.shadows = LightShadows.Soft;
            sun.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = new Color(0.55f, 0.6f, 0.7f); RenderSettings.ambientEquatorColor = new Color(0.4f, 0.38f, 0.35f);
            RenderSettings.ambientGroundColor = new Color(0.2f, 0.18f, 0.16f);
            var cam = new GameObject("Main Camera") { tag = "MainCamera" }.AddComponent<Camera>();
            // eye height on the street: the floor under the scene centre, found from the bounds' bottom
            // the same start as Unreal's (the FBX origin, inside the bistro): the floor under it by a raycast against
            // temporary mesh colliders, removed again before the save
            var cols = root.GetComponentsInChildren<MeshFilter>().Where(f => f.sharedMesh).Select(f => f.gameObject.AddComponent<MeshCollider>()).ToList();
            Physics.SyncTransforms();
            float floorY = b.min.y; string hitName = "none";
            if (Physics.Raycast(new Vector3(0f, 3f, 0f), Vector3.down, out var hit, 20f)) { floorY = hit.point.y; hitName = hit.collider.name; }
            foreach (var c in cols) Object.DestroyImmediate(c);
            Debug.Log("frame-lab bistro: floor under the origin at y=" + floorY.ToString("F2") + " (" + hitName + ")");
            cam.transform.position = new Vector3(0f, floorY + 1.6f, 0f);
            // the FBX's own authored camera (where Falcor starts) wins: put ours there, eye height over the street below it
            var authored = root.GetComponentsInChildren<Camera>().FirstOrDefault();
            if (authored)
            {
                var ap = authored.transform.position;
                Debug.Log("frame-lab bistro: authored camera '" + authored.name + "' at " + ap.ToString("F2") + " yaw " + authored.transform.eulerAngles.y.ToString("F0"));
                cam.transform.SetPositionAndRotation(ap, Quaternion.Euler(0f, authored.transform.eulerAngles.y, 0f));
                Object.DestroyImmediate(authored);
            }
            cam.clearFlags = CameraClearFlags.Skybox; cam.farClipPlane = 1000f;
            System.IO.Directory.CreateDirectory("Assets/Bistro");
            UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene, "Assets/Bistro/Bistro.unity");
            Debug.Log("frame-lab bistro: " + rs.Length + " renderers, " + tris + " tris, " + texd + " textured materials, bounds "
                + b.min.ToString("F1") + " .. " + b.max.ToString("F1") + ", camera " + cam.transform.position.ToString("F1")
                + ", took " + (System.DateTime.Now - t0).TotalSeconds.ToString("F0") + " s");
        }
    }
}
