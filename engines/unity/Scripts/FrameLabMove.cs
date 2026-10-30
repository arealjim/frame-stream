// frame-lab unity-smoke: stick locomotion, added to the camera's parent (the XR origin) by FrameLabRig.Awake.
// 2026-10-24 (Unity Bistro wear): there were no controls for moving. Same scheme as the Unreal smoke:
// left stick flies along the gaze (pitch included), right stick snap-turns FRAMELAB_SNAP_DEG (30) about the head.
// No collision: the scene is there to see and render, not to play.
using UnityEngine;
#if FRAMELAB_INPUTSYSTEM
using UnityEngine.InputSystem;
#endif

namespace FrameLab
{
    public class FrameLabMove : MonoBehaviour
    {
        public Transform head;
        float speed = 6f, snapDeg = 30f;   // 2026-10-24: 4x the first 1.5 m/s
        bool snapArmed = true;
        int moves, turns; float nextReport;
#if FRAMELAB_INPUTSYSTEM
        InputAction walk, turn;

        void Start()
        {
            var s = System.Environment.GetEnvironmentVariable("FRAMELAB_SNAP_DEG");
            if (!string.IsNullOrEmpty(s)) float.TryParse(s, out snapDeg);
            walk = new InputAction("frame-lab walk", InputActionType.Value, "<XRController>{LeftHand}/{Primary2DAxis}");
            turn = new InputAction("frame-lab turn", InputActionType.Value, "<XRController>{RightHand}/{Primary2DAxis}");
            walk.Enable(); turn.Enable();
            Debug.Log("frame-lab move: left stick walks " + speed + " m/s, right stick snaps " + snapDeg + " deg");
        }

        void Update()
        {
            if (head == null) return;
            Vector2 w = walk.ReadValue<Vector2>();
            if (w.sqrMagnitude > 0.04f)
            {
                // Forward follows the gaze, pitch included (2026-10-24: move up and down while looking up or down,
                // for verticality); strafe stays level.
                Vector3 fwd = head.forward;
                Vector3 flat = fwd; flat.y = 0; flat.Normalize();
                Vector3 right = new Vector3(flat.z, 0, -flat.x);
                transform.position += (fwd * w.y + right * w.x) * speed * Time.deltaTime;
                moves++;
            }
            float t = turn.ReadValue<Vector2>().x;
            if (snapArmed && Mathf.Abs(t) > 0.7f)
            {
                transform.RotateAround(head.position, Vector3.up, Mathf.Sign(t) * snapDeg);
                snapArmed = false; turns++;
            }
            else if (Mathf.Abs(t) < 0.3f) snapArmed = true;
            if (Time.unscaledTime >= nextReport)
            {
                nextReport = Time.unscaledTime + 5f;
                if (moves > 0 || turns > 0) Debug.Log("frame-lab move: " + moves + " walk frames, " + turns + " snaps; origin " + transform.position.ToString("F2"));
                moves = 0; turns = 0;
            }
        }
#endif
    }
}
