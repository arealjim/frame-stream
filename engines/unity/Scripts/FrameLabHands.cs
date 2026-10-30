// frame-lab unity-smoke: the controllers, made visible, added to the camera's parent (the XR origin) by FrameLabRig.Awake.
// (a wear of the release copy, 2026-10-26: the sticks moved, but grip and trigger showed
// nothing. Each hand is a block at the grip pose and a 1 m ray from the aim pose (b4b61aa's controllers, which the
// stand-test kit commit 1f8197e dropped from the rig). Now:
//   - the trigger ramps the block from grey to bright green over its analog value
//   - the grip squashes the block to half its thickness, analog too
//   - one 12 cm cube sits in reach ahead of the spawn, low and to the left; grip near it (FRAMELAB_GRAB_REACH, 0.15 m
//     from the block) picks it up and it follows the hand, letting go drops it where it is. No physics.
//     FRAMELAB_GRAB=0: no cube.
//   - "frame-lab input: left grip 0.00->1.00" on each press and release edge (0.5 to press, 0.4 to let go), and every
//     controller button logged on press and release, as before
// Paths are the generic XRController usages ({Trigger}, {Grip}), which every OpenXR controller profile fills; Unity's
// Oculus Touch profile binds them to .../trigger/value and .../squeeze/value.
using System.Collections.Generic;
using UnityEngine;
#if FRAMELAB_INPUTSYSTEM
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;
#endif

namespace FrameLab
{
    public class FrameLabHands : MonoBehaviour
    {
        public Transform head;
#if FRAMELAB_INPUTSYSTEM
        static readonly Color Rest = new Color(0.8f, 0.8f, 0.85f), Pulled = new Color(0.15f, 1f, 0.25f);
        const float Thick = 0.04f, Press = 0.5f, Let = 0.4f;

        class Hand
        {
            public string name;
            public Transform grip, block;
            public Material mat;
            public InputAction trigger, gripValue, tracked;
            public bool triggerDown, gripDown;
            public float lastTrigger, lastGrip;
        }
        readonly List<Hand> hands = new List<Hand>();
        Transform cube; Hand holder; bool cubeTouched; float reach = 0.15f;
        float nextReport;

        void Start()
        {
            if (GameObject.Find("frame-lab Left controller (grip)") == null)
            {
                hands.Add(MakeController("Left"));
                hands.Add(MakeController("Right"));
                Debug.Log("frame-lab hands: controllers made (block at the grip: trigger greens it, grip squashes it; ray from the aim pose)");
            }
            var r = System.Environment.GetEnvironmentVariable("FRAMELAB_GRAB_REACH");
            if (!string.IsNullOrEmpty(r)) float.TryParse(r, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out reach);
            if (System.Environment.GetEnvironmentVariable("FRAMELAB_GRAB") == "0") Debug.Log("frame-lab hands: grab cube off (FRAMELAB_GRAB=0)");
            else
            {
                var c = GameObject.CreatePrimitive(PrimitiveType.Cube); c.name = "frame-lab grab cube 12 cm";
                c.transform.localScale = Vector3.one * 0.12f;
                var m = Mat(new Color(0.85f, 0.3f, 0.8f)); if (m) c.GetComponent<Renderer>().sharedMaterial = m;
                Destroy(c.GetComponent<Collider>());
                c.SetActive(false);   // shown once the head pose is real, below
                cube = c.transform;
                Debug.Log("frame-lab hands: grab cube made, reach " + reach.ToString("F2") + " m (FRAMELAB_GRAB_REACH)");
            }
            foreach (var device in InputSystem.devices) OnDeviceChange(device, InputDeviceChange.Added);
        }

        static Material Mat(Color c)
        {
            var shader = Shader.Find("Standard");
            if (shader == null) shader = Shader.Find("Universal Render Pipeline/Lit");
            if (shader == null) return null;
            var m = new Material(shader); m.color = c;
            if (m.HasProperty("_Glossiness")) m.SetFloat("_Glossiness", 0.3f);
            return m;
        }

        // On the Frame the aim pose is ~40 degrees from the grip, so the ray hangs off the aim pose, not the block.
        Hand MakeController(string hand)
        {
            var h = new Hand { name = hand.ToLowerInvariant() };
            var grip = new GameObject("frame-lab " + hand + " controller (grip)");
            grip.transform.SetParent(transform, false);   // the XR origin, beside the camera
            var gd = grip.AddComponent<TrackedPoseDriver>();
            gd.positionInput = new InputActionProperty(new InputAction(hand + " grip position", InputActionType.Value, "<XRController>{" + hand + "Hand}/devicePosition", null, null, "Vector3"));
            gd.rotationInput = new InputActionProperty(new InputAction(hand + " grip rotation", InputActionType.Value, "<XRController>{" + hand + "Hand}/deviceRotation", null, null, "Quaternion"));
            gd.updateType = TrackedPoseDriver.UpdateType.UpdateAndBeforeRender;
            var block = GameObject.CreatePrimitive(PrimitiveType.Cube); block.name = hand + " block";
            block.transform.SetParent(grip.transform, false);
            block.transform.localScale = new Vector3(Thick, 0.06f, 0.12f);
            h.mat = Mat(Rest); if (h.mat) block.GetComponent<Renderer>().sharedMaterial = h.mat;
            Destroy(block.GetComponent<Collider>());
            h.grip = grip.transform; h.block = block.transform;

            var aim = new GameObject("frame-lab " + hand + " controller (aim)");
            aim.transform.SetParent(transform, false);
            var ad = aim.AddComponent<TrackedPoseDriver>();
            ad.positionInput = new InputActionProperty(new InputAction(hand + " aim position", InputActionType.Value, "<XRController>{" + hand + "Hand}/pointerPosition", null, null, "Vector3"));
            ad.rotationInput = new InputActionProperty(new InputAction(hand + " aim rotation", InputActionType.Value, "<XRController>{" + hand + "Hand}/pointerRotation", null, null, "Quaternion"));
            ad.updateType = TrackedPoseDriver.UpdateType.UpdateAndBeforeRender;
            var ray = GameObject.CreatePrimitive(PrimitiveType.Cylinder); ray.name = hand + " ray";
            ray.transform.SetParent(aim.transform, false);
            ray.transform.localScale = new Vector3(0.01f, 0.5f, 0.01f);
            ray.transform.localRotation = Quaternion.Euler(90f, 0f, 0f);   // the cylinder's axis (Y) along the aim's forward (+Z)
            ray.transform.localPosition = new Vector3(0f, 0f, 0.5f);
            var rm = Mat(new Color(0.95f, 0.6f, 0.2f)); if (rm) ray.GetComponent<Renderer>().sharedMaterial = rm;
            Destroy(ray.GetComponent<Collider>());

            h.trigger = new InputAction(hand + " trigger", InputActionType.Value, "<XRController>{" + hand + "Hand}/{Trigger}");
            h.gripValue = new InputAction(hand + " grip", InputActionType.Value, "<XRController>{" + hand + "Hand}/{Grip}");
            h.tracked = new InputAction(hand + " tracked", InputActionType.Button, "<XRController>{" + hand + "Hand}/isTracked");
            h.trigger.Enable(); h.gripValue.Enable(); h.tracked.Enable();
            return h;
        }

        // An edge with hysteresis: logs the value the frame before and the value now, so an injected step reads 0.00->1.00.
        bool Edge(Hand h, string what, float v, ref float last, ref bool down)
        {
            bool was = down;
            if (!down && v >= Press) down = true; else if (down && v <= Let) down = false;
            if (down != was) Debug.Log("frame-lab input: " + h.name + " " + what + " " + last.ToString("F2") + "->" + v.ToString("F2"));
            last = v;
            return down != was;
        }

        void Update()
        {
            foreach (var h in hands)
            {
                float t = h.trigger.ReadValue<float>(), g = h.gripValue.ReadValue<float>();
                Edge(h, "trigger", t, ref h.lastTrigger, ref h.triggerDown);
                if (Edge(h, "grip", g, ref h.lastGrip, ref h.gripDown)) Grab(h);
                if (h.mat) h.mat.color = Color.Lerp(Rest, Pulled, Mathf.Clamp01(t));
                h.block.localScale = new Vector3(Thick * (1f - 0.5f * Mathf.Clamp01(g)), 0.06f, 0.12f);
            }
            // A controller that loses tracking (or sleeps) while holding drops the cube where it was: its pose jumps to the
            // origin, and the cube with it (pad test, 2026-10-26: a client restart took a held cube 1.3 m under the floor).
            if (holder != null && !holder.tracked.IsPressed())
            {
                Debug.Log("frame-lab hands: " + holder.name + " lost tracking; cube dropped at " + cube.position.ToString("F3"));
                holder = null; cube.SetParent(null, true);
            }
            PlaceCube();
            LogControllerButtons();
            if (Time.unscaledTime >= nextReport)
            {
                nextReport = Time.unscaledTime + 5f;
                var s = "frame-lab hands:";
                foreach (var h in hands)
                    s += " " + h.name + " grip " + h.grip.position.ToString("F2") + " trigger " + h.lastTrigger.ToString("F2") + " grip " + h.lastGrip.ToString("F2");
                if (cube) s += "; cube " + cube.position.ToString("F2") + (holder != null ? " held " + holder.name : cube.gameObject.activeSelf ? " free" : " hidden")
                    + (hands.Count > 0 ? " (" + Vector3.Distance(cube.position, hands[0].block.position).ToString("F2") + " m from left, " + Vector3.Distance(cube.position, hands[1].block.position).ToString("F2") + " m from right)" : "");
                Debug.Log(s);
            }
        }

        void Grab(Hand h)
        {
            if (cube == null || !cube.gameObject.activeSelf) return;
            if (h.gripDown && holder == null)
            {
                float d = Vector3.Distance(h.block.position, cube.position);
                if (d > reach) { Debug.Log("frame-lab hands: " + h.name + " grip, cube " + d.ToString("F2") + " m away (reach " + reach.ToString("F2") + "), not taken"); return; }
                holder = h; cubeTouched = true;
                cube.SetParent(h.grip, true);   // follows the hand, keeping where it was held
                Debug.Log("frame-lab hands: " + h.name + " picked up the cube at " + cube.position.ToString("F3") + " (" + d.ToString("F2") + " m from the block)");
            }
            else if (!h.gripDown && holder == h)
            {
                holder = null;
                cube.SetParent(null, true);   // dropped in place: no physics
                Debug.Log("frame-lab hands: " + h.name + " let go; cube left at " + cube.position.ToString("F3"));
            }
        }

        // In reach ahead of the spawn: 0.45 m ahead, 0.3 m below the eyes, 0.15 m left, so it is clear of the cube at eye
        // height and the props. It follows the eyes until the head is worn: above 0.9 m (seated eyes are ~1.2 m) and
        // moving for two seconds. The first wear, 2026-10-26: with the rig's rule (moving, any height) it froze while the
        // headset was being carried at 0.55 m and sat 25 cm off the floor. Left more than 3 m away for 5 s (the wearer flew
        // off with the stick) and not held, it comes back in front of them.
        Vector3 lastHead; float nextHeadSample, farSince = -1f; int headMoving; bool cubeSettled;
        void PlaceCube()
        {
            if (cube == null || holder != null || head == null || head.position.y <= 0.3f) return;
            if (cubeSettled)
            {
                bool far = Vector3.Distance(cube.position, head.position) > 3f;
                if (!far) { farSince = -1f; return; }
                if (farSince < 0f) { farSince = Time.unscaledTime; return; }
                if (Time.unscaledTime - farSince < 5f) return;
                farSince = -1f;
                HomeCube();
                Debug.Log("frame-lab hands: cube left behind, brought back to " + cube.position.ToString("F3"));
                return;
            }
            HomeCube();
            if (!cube.gameObject.activeSelf) { cube.gameObject.SetActive(true); Debug.Log("frame-lab hands: grab cube shown at " + cube.position.ToString("F3")); }
            if (Time.unscaledTime < nextHeadSample) return;
            nextHeadSample = Time.unscaledTime + 0.5f;
            float step = (head.position - lastHead).magnitude; lastHead = head.position;
            headMoving = head.position.y > 0.9f && step > 0.002f && step < 0.3f ? headMoving + 1 : 0;
            if (headMoving >= 4 || cubeTouched)
            {
                cubeSettled = true;
                Debug.Log("frame-lab hands: head worn; grab cube left at " + cube.position.ToString("F3") + " from eye " + head.position.ToString("F3"));
            }
        }

        void HomeCube()
        {
            var fwd = Vector3.ProjectOnPlane(head.forward, Vector3.up);
            if (fwd.sqrMagnitude < 1e-4f) fwd = Vector3.forward;
            fwd.Normalize();
            var right = Vector3.Cross(Vector3.up, fwd).normalized;
            cube.position = head.position + fwd * 0.45f - Vector3.up * 0.3f - right * 0.15f;
            cube.rotation = Quaternion.LookRotation(fwd, Vector3.up);
        }

        // Every button on every XR controller, logged on press and release: a worn check walks the buttons and reads the log.
        readonly Dictionary<string, bool> buttonsDown = new Dictionary<string, bool>();
        void LogControllerButtons()
        {
            foreach (var device in InputSystem.devices)
            {
                if (!(device is UnityEngine.InputSystem.XR.XRController)) continue;
                foreach (var control in device.allControls)
                {
                    var b = control as UnityEngine.InputSystem.Controls.ButtonControl;
                    if (b == null || b.synthetic) continue;
                    bool down = b.isPressed;
                    var key = device.name + "/" + b.name;
                    bool was; buttonsDown.TryGetValue(key, out was);
                    if (down != was) { buttonsDown[key] = down; Debug.Log("frame-lab rig: button " + key + (down ? " down" : " up")); }
                }
            }
        }

        void OnDeviceChange(UnityEngine.InputSystem.InputDevice device, InputDeviceChange change)   // InputDevice alone is ambiguous with UnityEngine.XR's
        {
            if (device is UnityEngine.InputSystem.XR.XRController)
                Debug.Log("frame-lab rig: controller " + change + ": " + device.name + " (layout " + device.layout + ", usages " + string.Join(",", device.usages) + ")");
        }

        void OnEnable() { InputSystem.onDeviceChange += OnDeviceChange; }
        void OnDisable() { InputSystem.onDeviceChange -= OnDeviceChange; }
#endif
    }
}
