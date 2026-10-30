// frame-lab unity-smoke: menus on OpenXR composition layers, the Unity twin of xr-smoke's panels mode (2026-10-25, line
// menus-and-panels-in-every-engine-oxrsys-dc11). Off unless FRAMELAB_PANELS is set ("1", or "toggle" to hide and show
// both layers every 10 s so the runtime's "VideoEncoder: overlays:" line splits the GPU cost with and without in one run).
//
// Same layout as engines/godot/main.gd _make_panels, so the engines compare picture for picture:
//   - a world-locked QUAD layer, 1.0 x 0.6 m, 1.5 m ahead at 1.5 m over the floor origin, order 1
//   - a CYLINDER layer, radius 1.5 m, 60 degrees, aspect 2.0, axis at (0, 1.5, 0), turned 60 degrees left, order 2
// Each is fed by a uGUI Canvas (Screen Space - Camera) drawn by its own camera into a RenderTexture: dark panel at 0.85
// alpha, light 6 px border, the title top-left, then red, green and blue rows top to bottom, so a flipped or mirrored
// layer reads at a glance. That is what the package's own "Quad UI Panel" does behind the scenes (a hidden camera
// renders the canvas), minus the XR Interaction Toolkit it needs for pointer input.
//
// Needs com.unity.xr.compositionlayers (2.5.0 for OpenXR 1.18) and com.unity.ugui, plus the OpenXR "Composition Layers
// Support" feature on (Bootstrap.Configure enables it when the package is present). The plain unity-smoke project has
// neither package, so this file compiles to nothing there; the panels project is the sibling ~/src/unity-panels
// (README "Panels").
#if FRAMELAB_COMPLAYERS && FRAMELAB_UGUI
using System.Collections;
using Unity.XR.CompositionLayers;
using Unity.XR.CompositionLayers.Extensions;
using Unity.XR.CompositionLayers.Layers;
using Unity.XR.CompositionLayers.Services;
using UnityEngine;
using UnityEngine.UI;

namespace FrameLab
{
    public class FrameLabPanels : MonoBehaviour
    {
        const int QuadUiLayer = 30, CylUiLayer = 29;   // user layers no scene of ours uses; culled from the main camera
        bool toggle;
        float toggleT;
        GameObject quad, cyl;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        static void Boot()
        {
            var mode = System.Environment.GetEnvironmentVariable("FRAMELAB_PANELS");
            if (string.IsNullOrEmpty(mode) || mode == "0") return;
            var go = new GameObject("frame-lab panels"); DontDestroyOnLoad(go);
            go.AddComponent<FrameLabPanels>().toggle = mode.Contains("toggle");
        }

        IEnumerator Start()
        {
            // the rig parents the main camera under "XR Origin (floor)"; layers are posed relative to the camera's parent
            Camera main = null;
            for (int i = 0; i < 100 && (main == null || main.transform.parent == null); i++) { main = Camera.main; yield return null; }
            if (main == null) { Debug.LogError("frame-lab panels: no main camera"); yield break; }
            var origin = main.transform.parent;
            main.cullingMask &= ~((1 << QuadUiLayer) | (1 << CylUiLayer));

            quad = MakeLayer("frame-lab quad layer", origin, new Vector3(0f, 1.5f, 1.5f), Quaternion.identity,
                MakeCanvasTexture("QUAD LAYER", 1000, 600, QuadUiLayer), 1, true);
            // cylinder: its image centre sits radius ahead along the transform's forward, so turning it 60 degrees left
            // (negative yaw in Unity) puts the panel left of the quad, both seen from the start pose
            cyl = MakeLayer("frame-lab cylinder layer", origin, new Vector3(0f, 1.5f, 0f), Quaternion.Euler(0f, -60f, 0f),
                MakeCanvasTexture("CYLINDER LAYER", 1200, 600, CylUiLayer), 2, false);

            string exts = "";
            try { foreach (var e in UnityEngine.XR.OpenXR.OpenXRRuntime.GetEnabledExtensions()) if (e.Contains("composition_layer")) exts += " " + e; }
            catch (System.Exception ex) { exts = " (could not list: " + ex.Message + ")"; }
            var feature = UnityEngine.XR.OpenXR.OpenXRSettings.Instance?.GetFeature<UnityEngine.XR.OpenXR.Features.CompositionLayers.OpenXRCompositionLayersFeature>();
            Debug.Log("frame-lab panels: quad + cylinder layers under '" + origin.name + "', toggle=" + toggle
                + ", feature " + (feature == null ? "missing" : feature.enabled ? "on" : "off")
                + ", cylinder ext " + UnityEngine.XR.OpenXR.OpenXRRuntime.IsExtensionEnabled("XR_KHR_composition_layer_cylinder")
                + ", runtime " + UnityEngine.XR.OpenXR.OpenXRRuntime.name + " " + UnityEngine.XR.OpenXR.OpenXRRuntime.version
                + ", enabled composition extensions:" + (exts.Length == 0 ? " none" : exts));
            float next = Time.realtimeSinceStartup + 5f;
            while (true)
            {
                if (Time.realtimeSinceStartup > next)
                {
                    next += 30f;
                    var mgr = CompositionLayerManager.Instance;
                    Debug.Log("frame-lab panels: manager " + (mgr == null ? "none" : "provider " + (mgr.LayerProvider == null ? "none" : mgr.LayerProvider.GetType().Name)
                        + ", layers " + (mgr.CompositionLayers == null ? 0 : mgr.CompositionLayers.Count)) + ", quad " + (quad.activeSelf ? "shown" : "hidden"));
                }
                yield return null;
            }
        }

        void Update()
        {
            if (!toggle || quad == null || cyl == null) return;
            toggleT += Time.deltaTime;
            if (toggleT < 10f) return;
            toggleT = 0f;
            // both layers together, so the runtime's "VideoEncoder: overlays:" line has a "without" bucket (2026-10-28)
            quad.SetActive(!quad.activeSelf);
            cyl.SetActive(quad.activeSelf);
            Debug.Log("frame-lab panels: " + (quad.activeSelf ? "shown" : "hidden"));
        }

        static GameObject MakeLayer(string name, Transform origin, Vector3 pos, Quaternion rot, RenderTexture rt, int order, bool isQuad)
        {
            var go = new GameObject(name);
            go.SetActive(false);   // configure before OnEnable registers it with the manager
            go.transform.SetParent(origin, false);
            go.transform.localPosition = pos; go.transform.localRotation = rot;
            var layer = go.AddComponent<CompositionLayer>();
            if (isQuad)
            {
                layer.ChangeLayerDataType<QuadLayerData>();
                var d = (QuadLayerData)layer.LayerData; d.Size = new Vector2(1.0f, 0.6f); d.ApplyTransformScale = false;
            }
            else
            {
                layer.ChangeLayerDataType<CylinderLayerData>();
                var d = (CylinderLayerData)layer.LayerData;
                d.Radius = 1.5f; d.CentralAngle = 60f * Mathf.Deg2Rad; d.AspectRatio = 2.0f; d.ApplyTransformScale = false;
            }
            var tex = go.AddComponent<TexturesExtension>();
            tex.sourceTexture = TexturesExtension.SourceTextureEnum.LocalTexture;
            tex.LeftTexture = rt;   // a RenderTexture is copied into the swapchain every frame (OpenXRCustomLayerHandler)
            layer.Order = order;
            go.SetActive(true);
            return go;
        }

        // A uGUI canvas like a game menu, rendered by its own camera into a w x h texture (1 canvas px = 1 texel).
        static RenderTexture MakeCanvasTexture(string title, int w, int h, int uiLayer)
        {
            var rt = new RenderTexture(w, h, 24, RenderTextureFormat.ARGB32) { name = title + " (canvas)" };
            rt.Create();
            var camGo = new GameObject(title + " canvas camera");
            DontDestroyOnLoad(camGo);
            camGo.transform.position = new Vector3(0f, -1000f - uiLayer * 10f, 0f);   // far from the scene
            var cam = camGo.AddComponent<Camera>();
            cam.targetTexture = rt;
            cam.stereoTargetEye = StereoTargetEyeMask.None;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0f, 0f, 0f, 0f);
            cam.cullingMask = 1 << uiLayer;
            cam.orthographic = true; cam.nearClipPlane = 0.1f; cam.farClipPlane = 10f;
            cam.depth = -10f;

            var canvasGo = new GameObject(title + " canvas", typeof(RectTransform));
            DontDestroyOnLoad(canvasGo);
            canvasGo.layer = uiLayer;
            var canvas = canvasGo.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceCamera;
            canvas.worldCamera = cam;
            canvas.planeDistance = 1f;

            Rect(canvasGo, "panel", 0, 0, w, h, new Color(0.08f, 0.09f, 0.12f, 0.85f), uiLayer);
            var light = new Color(0.9f, 0.9f, 0.9f, 1f);
            const int b = 6;
            Rect(canvasGo, "border top", 0, 0, w, b, light, uiLayer);
            Rect(canvasGo, "border bottom", 0, h - b, w, b, light, uiLayer);
            Rect(canvasGo, "border left", 0, 0, b, h, light, uiLayer);
            Rect(canvasGo, "border right", w - b, 0, b, h, light, uiLayer);
            var colours = new[] { new Color(0.9f, 0.2f, 0.2f), new Color(0.2f, 0.8f, 0.3f), new Color(0.25f, 0.45f, 0.95f) };
            for (int i = 0; i < colours.Length; i++) Rect(canvasGo, "row " + i, 24, 96 + i * 90, w - 48, 64, colours[i], uiLayer);

            var label = new GameObject("title", typeof(RectTransform));
            label.layer = uiLayer;
            label.transform.SetParent(canvasGo.transform, false);
            Place(label.GetComponent<RectTransform>(), 24, 16, w - 48, 70);
            var text = label.AddComponent<Text>();
            text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            text.fontSize = 48; text.color = Color.white; text.text = title;
            text.alignment = TextAnchor.UpperLeft; text.horizontalOverflow = HorizontalWrapMode.Overflow;
            return rt;
        }

        // x, y from the top-left corner in canvas pixels, as in the Godot panel
        static void Rect(GameObject canvas, string name, int x, int y, int w, int h, Color c, int uiLayer)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.layer = uiLayer;
            go.transform.SetParent(canvas.transform, false);
            Place(go.GetComponent<RectTransform>(), x, y, w, h);
            go.AddComponent<Image>().color = c;
        }

        static void Place(RectTransform r, int x, int y, int w, int h)
        {
            r.anchorMin = r.anchorMax = r.pivot = new Vector2(0f, 1f);
            r.anchoredPosition = new Vector2(x, -y);
            r.sizeDelta = new Vector2(w, h);
        }
    }
}
#endif
