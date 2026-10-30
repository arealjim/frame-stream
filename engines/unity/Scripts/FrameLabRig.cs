// frame-lab unity-smoke: the rig script on the main camera.
//   - asks the XR input subsystem for a floor tracking origin (Unity defaults to device/eye level, which put the
//     camera on the floor plane in the first wear, 2026-10-20: the Frame saw a skybox horizon at eye height)
//   - logs the camera pose, the active scene and the clear flags every 5 s, so a wear can be checked from the log
//   - adds FrameLabMove (stick locomotion) and FrameLabHands (the controllers, grip and trigger shown, a cube to grab)
using System.Collections.Generic;
using UnityEngine;
#if FRAMELAB_INPUTSYSTEM   // the package arrives with stage 1; before it, the rig compiles without the pose binding
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;
#endif
using UnityEngine.SceneManagement;
using UnityEngine.XR;

namespace FrameLab
{
    public class FrameLabRig : MonoBehaviour
    {
        bool floorSet;
        int asks;
        float nextAsk, nextReport;
#if FRAMELAB_INPUTSYSTEM
        TrackedPoseDriver driver;
#endif
        bool cubePlaced;
        float nextMotionSample;
        int movingSamples;
        Vector3 lastSample;

        // The TrackedPoseDriver added by the bootstrap has no actions, and Unity's XR display does not move the
        // camera by itself: the second dump (2026-10-20 18:32) was the bare clear colour rendered from the origin,
        // cam pos (0,0,0) in the report. Bind the head pose here, whatever the scene says.
        void Awake()
        {
            // 2026-10-21 props wear: a shimmer mostly on the edges, a little on all the colours, none on the grey backdrop
            // or the black = edge crawl, i.e. no MSAA (the project's active quality level has
            // antiAliasing 0). 4x MSAA unless FRAMELAB_MSAA says otherwise (0 = off, for the A/B).
            var msaaEnv = System.Environment.GetEnvironmentVariable("FRAMELAB_MSAA");
            int msaa = 4; if (!string.IsNullOrEmpty(msaaEnv)) int.TryParse(msaaEnv, out msaa);
            int before = QualitySettings.antiAliasing;
            QualitySettings.antiAliasing = msaa;
            Debug.Log("frame-lab rig: MSAA " + before + " -> " + QualitySettings.antiAliasing + " (FRAMELAB_MSAA)");
            if (GetComponent<FrameLabCmd>() == null) gameObject.AddComponent<FrameLabCmd>();   // live scale/msaa + fps log
            if (transform.parent != null && transform.parent.GetComponent<FrameLabMove>() == null)
                transform.parent.gameObject.AddComponent<FrameLabMove>().head = transform;   // stick locomotion moves the origin
            if (transform.parent != null && transform.parent.GetComponent<FrameLabHands>() == null)
            {
                var hands = transform.parent.gameObject.AddComponent<FrameLabHands>();   // the controllers, grip/trigger shown, a cube to grab
                hands.head = transform;
            }
#if FRAMELAB_INPUTSYSTEM
            driver = GetComponent<TrackedPoseDriver>();
            if (driver == null) driver = gameObject.AddComponent<TrackedPoseDriver>();
            var pos = driver.positionInput.action;
            if (pos == null || pos.bindings.Count == 0)
            {
                driver.positionInput = new InputActionProperty(new InputAction("frame-lab head position", InputActionType.Value, "<XRHMD>/centerEyePosition", null, null, "Vector3"));
                driver.rotationInput = new InputActionProperty(new InputAction("frame-lab head rotation", InputActionType.Value, "<XRHMD>/centerEyeRotation", null, null, "Quaternion"));
                driver.trackingStateInput = new InputActionProperty(new InputAction("frame-lab head tracking state", InputActionType.Value, "<XRHMD>/trackingState", null, null, "Integer"));
                driver.trackingType = TrackedPoseDriver.TrackingType.RotationAndPosition;
                driver.updateType = TrackedPoseDriver.UpdateType.UpdateAndBeforeRender;
                Debug.Log("frame-lab rig: bound the TrackedPoseDriver to <XRHMD>/centerEye*");
            }
#else
            Debug.LogWarning("frame-lab rig: compiled without the Input System; the camera is not bound to the head pose");
#endif
        }

        // 2026-10-21 (regression wear): a shimmer, not a pulse, perhaps from the black background, so there are more
        // objects to look at. Props around the eye, placed
        // once with the cube: a sphere, a cylinder, a capsule, a grid of thin bars on the floor, and a mid-grey backdrop
        // so a shimmer can be judged against grey as well as against the near-black clear colour.
        static Material PropMaterial(Color c)
        {
            var shader = Shader.Find("Standard");
            if (shader == null) shader = Shader.Find("Universal Render Pipeline/Lit");
            if (shader == null) return null;
            var m = new Material(shader); m.color = c;
            if (m.HasProperty("_Glossiness")) m.SetFloat("_Glossiness", 0.3f);   // matte-ish: a sharp specular highlight shimmers under sub-pixel motion
            return m;
        }

        static GameObject Prop(Transform parent, PrimitiveType t, string name, Vector3 pos, Vector3 scale, Color c)
        {
            var g = GameObject.CreatePrimitive(t); g.name = name; g.transform.SetParent(parent, true);
            g.transform.position = pos; g.transform.localScale = scale;
            var m = PropMaterial(c); if (m) g.GetComponent<Renderer>().sharedMaterial = m;
            var col = g.GetComponent<Collider>(); if (col) Destroy(col);
            return g;
        }

        void PlaceProps(Vector3 eye, Vector3 fwd)
        {
            if (GameObject.Find("frame-lab props") != null) return;
            // FRAMELAB_PROPS=0: no props. The character room shares this rig and its grey backdrop hid the characters (2026-10-24).
            if (System.Environment.GetEnvironmentVariable("FRAMELAB_PROPS") == "0") { Debug.Log("frame-lab rig: props off (FRAMELAB_PROPS=0)"); return; }
            var root = new GameObject("frame-lab props").transform;
            var right = Vector3.Cross(Vector3.up, fwd).normalized;
            var eyeH = new Vector3(eye.x, eye.y, eye.z);
            Prop(root, PrimitiveType.Sphere,   "sphere 0.8 m ahead-left",   eyeH + fwd * 0.8f - right * 0.35f,                  Vector3.one * 0.15f, new Color(0.25f, 0.5f, 0.95f));
            Prop(root, PrimitiveType.Cylinder, "cylinder 1.2 m ahead-right", eyeH + fwd * 1.2f + right * 0.45f - Vector3.up * 0.2f, new Vector3(0.12f, 0.25f, 0.12f), new Color(0.3f, 0.85f, 0.35f));
            Prop(root, PrimitiveType.Capsule,  "capsule 2 m ahead",          eyeH + fwd * 2.0f + Vector3.up * 0.15f,             new Vector3(0.15f, 0.2f, 0.15f), new Color(0.95f, 0.85f, 0.25f));
            Prop(root, PrimitiveType.Cube,     "small cube 0.35 m, right",   eyeH + fwd * 0.35f + right * 0.2f - Vector3.up * 0.1f, Vector3.one * 0.04f, new Color(0.9f, 0.9f, 0.9f));
            // A mid-grey backdrop 3 m ahead, 4 m wide, 2.5 m tall: the shimmer against grey, not black.
            var back = Prop(root, PrimitiveType.Quad, "grey backdrop 3 m", eyeH + fwd * 3.0f, new Vector3(4f, 2.5f, 1f), new Color(0.45f, 0.45f, 0.47f));
            back.transform.rotation = Quaternion.LookRotation(-fwd, Vector3.up) * Quaternion.Euler(0f, 180f, 0f);
            // A floor grid: thin bars every 0.5 m over a 3 m square under the eye, 1 cm tall, light grey.
            var floorY = 0.005f;
            for (int i = -3; i <= 3; i++)
            {
                var off = i * 0.5f;
                Prop(root, PrimitiveType.Cube, "grid bar x" + i, new Vector3(eye.x, floorY, eye.z) + right * off + fwd * 0f, new Vector3(0.01f, 0.01f, 3f), new Color(0.7f, 0.7f, 0.7f)).transform.rotation = Quaternion.LookRotation(fwd, Vector3.up);
                Prop(root, PrimitiveType.Cube, "grid bar z" + i, new Vector3(eye.x, floorY, eye.z) + fwd * off, new Vector3(3f, 0.01f, 0.01f), new Color(0.7f, 0.7f, 0.7f)).transform.rotation = Quaternion.LookRotation(fwd, Vector3.up);
            }
            Debug.Log("frame-lab rig: props placed around eye " + eye.ToString("F3") + " fwd " + fwd.ToString("F2") + " (sphere, cylinder, capsule, small cube, grey backdrop, floor grid)");
        }

        void Update()
        {
            if (!floorSet && asks < 60 && Time.unscaledTime >= nextAsk)
            {
                nextAsk = Time.unscaledTime + 1f;
                asks++;
                var subs = new List<XRInputSubsystem>();
                SubsystemManager.GetSubsystems(subs);
                foreach (var s in subs)
                {
                    var ok = s.TrySetTrackingOriginMode(TrackingOriginModeFlags.Floor);
                    Debug.Log("frame-lab rig: TrySetTrackingOriginMode(Floor) -> " + ok + "; mode now " + s.GetTrackingOriginMode()
                        + "; supported " + s.GetSupportedTrackingOriginModes() + " (ask " + asks + ")");
                    floorSet |= ok;
                }
                if (subs.Count == 0 && asks % 10 == 1) Debug.Log("frame-lab rig: no XRInputSubsystem yet (ask " + asks + ")");
                if (asks == 60 && !floorSet) Debug.LogError("frame-lab rig: floor tracking origin never set");
            }
            // Like the Unreal smoke: the cube sits 0.5 m ahead of the eyes (yaw only) at eye height. Off-head the runtime
            // feeds a static pose (a canned 1.6 m before a client attaches, the headset's resting pose after) with the
            // tracking bits set, and the attach is a single jump. A worn head never holds still, so the cube follows the
            // eyes until the pose has moved a little every half second for two seconds, then stays put. (Freezing on
            // "moved 5 cm" fired on the attach jump and left the cube 0.5 m ahead of the origin: the second wear
            // found the cube at head position.)
            if (!cubePlaced && floorSet && transform.position.y > 0.3f)
            {
                var cubeObj = GameObject.Find("cube 0.5 m ahead");
                if (cubeObj)
                {
                    var fwd = Vector3.ProjectOnPlane(transform.forward, Vector3.up);
                    if (fwd.sqrMagnitude < 1e-4f) fwd = Vector3.forward;
                    cubeObj.transform.position = transform.position + fwd.normalized * 0.5f;
                    if (Time.unscaledTime >= nextMotionSample)
                    {
                        nextMotionSample = Time.unscaledTime + 0.5f;
                        float step = (transform.position - lastSample).magnitude;
                        lastSample = transform.position;
                        movingSamples = step > 0.002f && step < 0.3f ? movingSamples + 1 : 0;
                        if (movingSamples >= 4)
                        {
                            cubePlaced = true;
                            PlaceProps(transform.position, fwd.normalized);
                            Debug.Log("frame-lab rig: head moving; cube left at " + cubeObj.transform.position.ToString("F3") + " from eye " + transform.position.ToString("F3") + " euler " + transform.eulerAngles.ToString("F1"));
                        }
                    }
                }
            }
            // No timed fallback: off-head the resting pose points anywhere (a headset on the desk, eye at 1.15 m), and props
            // placed around it would meet the wearer at desk height. They place with the cube, on the first head motion.
            if (Time.unscaledTime >= nextReport)
            {
                nextReport = Time.unscaledTime + 5f;
                var c = Camera.main;
                var cube = GameObject.Find("cube 0.5 m ahead");
#if FRAMELAB_INPUTSYSTEM
                var pa = driver ? driver.positionInput.action : null;
                var ta = driver ? driver.trackingStateInput.action : null;
                string head = pa == null ? "none" : (pa.enabled ? "enabled" : "disabled") + " controls " + pa.controls.Count + " value " + pa.ReadValue<Vector3>().ToString("F3");
                string tracking = ta == null || !ta.enabled ? "none" : ta.ReadValue<int>().ToString();
#else
                string head = "no Input System", tracking = "none";
#endif
                Debug.Log(string.Format("frame-lab rig: scene '{0}' cam pos {1} euler {2} clear {3} floor {4} cube {5} xr {6} head action {7} tracking {8}",
                    SceneManager.GetActiveScene().name,
                    c ? c.transform.position.ToString("F3") : "none",
                    c ? c.transform.eulerAngles.ToString("F1") : "none",
                    c ? c.clearFlags.ToString() : "none",
                    floorSet,
                    cube ? cube.transform.position.ToString("F2") : "missing",
                    XRSettings.isDeviceActive ? XRSettings.loadedDeviceName + " " + XRSettings.eyeTextureWidth + "x" + XRSettings.eyeTextureHeight : "off",
                    head, tracking));
            }
        }
    }
}
