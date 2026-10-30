// frame-lab unity-smoke: live knobs + monitors while streaming, the Unity twin of the Unreal smoke's smoke-cmd.txt.
// 2026-10-24: switches and monitors for tuning Unity and Unreal while they stream. Write lines to ~/Library/Logs/frame-stream/unity-cmd.txt (FRAMELAB_CMDFILE overrides); polled every 0.5 s,
// each line applied and logged "frame-lab cmd: ...", then the file is deleted. "help" lists the verbs.
// Every 5 s: "frame-lab perf: fps, cpu/gpu ms (FrameTimingManager), draw calls, setpass, tris, batches, the head
// position and yaw, and the knob state" - one line that says whether a view is CPU-, GPU- or submission-bound.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using Unity.Profiling;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;
using UnityEngine.XR;

namespace FrameLab
{
    public class FrameLabCmd : MonoBehaviour
    {
        string path;
        float nextPoll, nextReport, reportEvery = 5f;
        int frames;
        float minFps = 999f; float lastT;
        ProfilerRecorder drawCalls, setPass, tris, batches, mainThread;
        readonly FrameTiming[] timing = new FrameTiming[1];
        double cpuSum, gpuSum; int timingN;
        Camera cam;
        // Tour (2026-10-24: simulate a wearer flying around inside the scene, for off-head runs): the origin flies a
        // closed loop through the scene's busy middle at head height and above, the view sweeping yaw and pitch the way a
        // worn head looks around, so a run off-head sees the views a wearer sees. Off-head the tracked pose is static, so
        // turning the origin turns the view; worn, the head's own motion adds to it.
        Vector3 tourHome; bool tourHomeSet; List<Vector3> tourPts; int tourLoops, tourLoop; float tourT, tourSpeed = 4f, tourLen; bool touring;
        readonly List<(float fps, Vector3 at, float yaw)> tourSecs = new List<(float, Vector3, float)>();
        int secFrames; float secStart;
        Light[] lights;

        const string Help = "scale <f> | msaa <0|2|4|8> | shadows <0 off|1 hard|2 soft> | shadowdist <m> | cascades <1|2|4> | shadowres <0..3>"
            + " | lodbias <f> | far <m> | pixellights <n> | lights <0|1 (non-sun)> | sun <0|1> | texlimit <0..3> | aniso <0|1|2>"
            + " | hdr <0|1> | instancing <0|1> | batch (static-combine the scene, one way) | goto <x> <y> <z> [yaw]"
            + " | tour [loops] [m/s] | tour stop | load <scene> | scenes | mirror <0|1|2> (Mac window: none, one eye, both) | warm | every <s> | state | help";

        static bool F(string s, out float v) => float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v);

        void Start()
        {
            path = Environment.GetEnvironmentVariable("FRAMELAB_CMDFILE");
            if (string.IsNullOrEmpty(path)) path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Personal), "src/logs/unity-cmd.txt");
            cam = GetComponent<Camera>();
            lights = FindObjectsByType<Light>(FindObjectsSortMode.None);
            drawCalls = ProfilerRecorder.StartNew(ProfilerCategory.Render, "Draw Calls Count");
            setPass = ProfilerRecorder.StartNew(ProfilerCategory.Render, "SetPass Calls Count");
            tris = ProfilerRecorder.StartNew(ProfilerCategory.Render, "Triangles Count");
            batches = ProfilerRecorder.StartNew(ProfilerCategory.Render, "Batches Count");
            mainThread = ProfilerRecorder.StartNew(ProfilerCategory.Internal, "Main Thread", 15);
            Debug.Log("frame-lab cmd: polling " + path + "; " + Help);
            Debug.Log("frame-lab cmd: " + State());
            lastT = Time.unscaledTime;
        }

        void OnDestroy() { drawCalls.Dispose(); setPass.Dispose(); tris.Dispose(); batches.Dispose(); mainThread.Dispose(); }

        string State()
        {
            return "scale " + XRSettings.eyeTextureResolutionScale.ToString("F2", CultureInfo.InvariantCulture) + " eye " + XRSettings.eyeTextureWidth + "x" + XRSettings.eyeTextureHeight
                + " msaa " + QualitySettings.antiAliasing + " shadows " + QualitySettings.shadows + " shadowdist " + QualitySettings.shadowDistance
                + " cascades " + QualitySettings.shadowCascades + " shadowres " + QualitySettings.shadowResolution + " lodbias " + QualitySettings.lodBias
                + " far " + (cam ? cam.farClipPlane : 0) + " pixellights " + QualitySettings.pixelLightCount + " texlimit " + QualitySettings.globalTextureMipmapLimit
                + " aniso " + QualitySettings.anisotropicFiltering + " hdr " + (cam && cam.allowHDR) + " mirror " + XRSettings.gameViewRenderMode;
        }

        void Update()
        {
            frames++;
            float now = Time.unscaledTime, dt = now - lastT; lastT = now;
            if (dt > 0) minFps = Mathf.Min(minFps, 1f / dt);
            FrameTimingManager.CaptureFrameTimings();
            if (FrameTimingManager.GetLatestTimings(1, timing) > 0) { cpuSum += timing[0].cpuFrameTime; gpuSum += timing[0].gpuFrameTime; timingN++; }
            if (touring) TourTick(dt, now);
            if (now >= nextReport)
            {
                if (nextReport > 0)
                {
                    var h = transform;
                    Debug.Log("frame-lab perf: fps " + (frames / reportEvery).ToString("F1", CultureInfo.InvariantCulture) + " (min " + minFps.ToString("F0") + ")"
                        + " cpu " + (timingN > 0 ? cpuSum / timingN : 0).ToString("F2", CultureInfo.InvariantCulture) + " ms gpu " + (timingN > 0 ? gpuSum / timingN : 0).ToString("F2", CultureInfo.InvariantCulture) + " ms"
                        + " main " + (mainThread.Valid ? mainThread.LastValue * 1e-6 : 0).ToString("F2", CultureInfo.InvariantCulture) + " ms"
                        + " draws " + drawCalls.LastValue + " setpass " + setPass.LastValue + " batches " + batches.LastValue + " tris " + tris.LastValue
                        + " at " + h.position.ToString("F1") + " yaw " + h.eulerAngles.y.ToString("F0") + " pitch " + h.eulerAngles.x.ToString("F0") + " | " + State());
                }
                frames = 0; minFps = 999f; cpuSum = gpuSum = 0; timingN = 0; nextReport = now + reportEvery;
            }
            if (now < nextPoll) return;
            nextPoll = now + 0.5f;
            if (!File.Exists(path)) return;
            string[] lines;
            try { lines = File.ReadAllLines(path); File.Delete(path); } catch (Exception e) { Debug.LogWarning("frame-lab cmd: " + e.Message); return; }
            foreach (var raw in lines) Apply(raw.Trim());
        }

        void TourStart()
        {
            var origin = transform.parent; if (!origin) { Debug.LogWarning("frame-lab tour: no XR origin"); return; }
            // the busy middle: 15th-85th percentile of renderer bounds corners in x and z (skydomes and far props drop out).
            // Corners, not centres: a scene imported as one big mesh (San Miguel, Power Plant) has a single centre and gave a 0-wide path.
            var xs = new List<float>(); var zs = new List<float>();
            foreach (var r in FindObjectsByType<Renderer>(FindObjectsSortMode.None)) { var b = r.bounds; xs.Add(b.min.x); xs.Add(b.max.x); zs.Add(b.min.z); zs.Add(b.max.z); }
            if (xs.Count == 0) return;
            xs.Sort(); zs.Sort();
            float P(List<float> a, float q) => a[Mathf.Clamp((int)(q * (a.Count - 1)), 0, a.Count - 1)];
            float x0 = P(xs, .15f), x1 = P(xs, .85f), z0 = P(zs, .15f), z1 = P(zs, .85f);
            float cx = (x0 + x1) / 2, cz = (z0 + z1) / 2, rx = (x1 - x0) / 2 * .8f, rz = (z1 - z0) / 2 * .8f;
            if (!tourHomeSet) { tourHome = origin.position; tourHomeSet = true; }   // back here after each tour: the floor crept up 0.2 m a tour
            origin.position = tourHome;
            float floorY = origin.position.y;   // or the first surface under the middle, when the scene has colliders
            if (Physics.Raycast(new Vector3(cx, floorY + 3f, cz), Vector3.down, out var hit, 50f)) floorY = hit.point.y;
            tourPts = new List<Vector3>();
            for (int i = 0; i < 10; i++)
            {
                float a = i * Mathf.PI * 2 / 10, k = (i % 3 == 1) ? .35f : 1f;   // every third point cuts through the middle
                float y = floorY + ((i % 4 == 2) ? 6f : 1.7f);                       // and some legs climb for verticality
                tourPts.Add(new Vector3(cx + Mathf.Cos(a) * rx * k, y, cz + Mathf.Sin(a) * rz * k));
            }
            tourLen = 0; for (int i = 0; i < tourPts.Count; i++) tourLen += Vector3.Distance(tourPts[i], tourPts[(i + 1) % tourPts.Count]);
            tourT = 0; tourLoop = 0; touring = true; tourSecs.Clear(); secFrames = 0; secStart = Time.unscaledTime;
            Debug.Log("frame-lab tour: start, " + tourLoops + " loop(s) of " + tourLen.ToString("F0") + " m at " + tourSpeed + " m/s (" + (tourLen / tourSpeed).ToString("F0")
                + " s each) over x " + x0.ToString("F0") + ".." + x1.ToString("F0") + " z " + z0.ToString("F0") + ".." + z1.ToString("F0") + " floor " + floorY.ToString("F2"));
        }

        static Vector3 CatmullRom(Vector3 a, Vector3 b, Vector3 c, Vector3 d, float t)
            => 0.5f * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t * t + (3 * b - a - 3 * c + d) * t * t * t);

        Vector3 TourAt(float dist)
        {
            int n = tourPts.Count; float acc = 0;
            for (int i = 0; i < n; i++)
            {
                float seg = Vector3.Distance(tourPts[i], tourPts[(i + 1) % n]);
                if (dist <= acc + seg || i == n - 1)
                    return CatmullRom(tourPts[(i - 1 + n) % n], tourPts[i], tourPts[(i + 1) % n], tourPts[(i + 2) % n], Mathf.Clamp01((dist - acc) / Mathf.Max(seg, 1e-3f)));
                acc += seg;
            }
            return tourPts[0];
        }

        void TourTick(float dt, float now)
        {
            var origin = transform.parent; if (!origin || tourPts == null) { touring = false; return; }
            tourT += dt;
            float dist = tourT * tourSpeed;
            if (dist >= tourLen)
            {
                tourLoop++; TourSummary();
                if (tourLoop >= tourLoops) { touring = false; origin.position = tourHome; Debug.Log("frame-lab tour: done"); return; }
                tourT = 0; dist = 0; tourSecs.Clear();
            }
            Vector3 at = TourAt(dist), ahead = TourAt(Mathf.Min(dist + 1f, tourLen - 0.01f));
            Vector3 d = ahead - at; d.y = 0;
            float heading = d.sqrMagnitude > 1e-6f ? Mathf.Atan2(d.x, d.z) * Mathf.Rad2Deg : origin.eulerAngles.y;
            // a looking-around head: +-70 deg of yaw about the direction of travel, +-25 deg of pitch, at unrelated rates
            float yaw = heading + 70f * Mathf.Sin(tourT * 0.9f) + 20f * Mathf.Sin(tourT * 2.3f);
            float pitch = 25f * Mathf.Sin(tourT * 0.55f);
            origin.rotation = Quaternion.Euler(pitch, yaw - transform.localEulerAngles.y, 0);
            origin.position += at - transform.position;
            secFrames++;
            if (now - secStart >= 1f) { tourSecs.Add((secFrames / (now - secStart), transform.position, yaw)); secFrames = 0; secStart = now; }
        }

        void TourSummary()
        {
            if (tourSecs.Count == 0) return;
            var fps = new List<float>(); foreach (var x in tourSecs) fps.Add(x.fps);
            fps.Sort();
            float mean = 0; foreach (var f in fps) mean += f; mean /= fps.Count;
            int under85 = 0, under60 = 0; foreach (var f in fps) { if (f < 85) under85++; if (f < 60) under60++; }
            tourSecs.Sort((a, b) => a.fps.CompareTo(b.fps));
            var worst = "";
            for (int i = 0; i < Mathf.Min(3, tourSecs.Count); i++) worst += " " + tourSecs[i].fps.ToString("F0") + "@" + tourSecs[i].at.ToString("F0") + "y" + tourSecs[i].yaw.ToString("F0");
            Debug.Log("frame-lab tour: loop " + tourLoop + " " + fps.Count + " s: fps mean " + mean.ToString("F1") + " p5 " + fps[(int)(fps.Count * .05f)].ToString("F0")
                + " min " + fps[0].ToString("F0") + ", " + under85 + " s under 85, " + under60 + " under 60; worst" + worst + " | " + State());
        }

        void Apply(string l)
        {
            if (l.Length == 0) return;
            var p = l.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            float v = 0; bool ok = p.Length >= 2 && F(p[1], out v);
            switch (p[0])
            {
                case "scale" when ok: XRSettings.eyeTextureResolutionScale = v; break;
                case "msaa" when ok: QualitySettings.antiAliasing = (int)v; break;
                case "shadows" when ok: QualitySettings.shadows = (ShadowQuality)(int)v; break;
                case "shadowdist" when ok: QualitySettings.shadowDistance = v; break;
                case "cascades" when ok: QualitySettings.shadowCascades = (int)v; break;
                case "shadowres" when ok: QualitySettings.shadowResolution = (ShadowResolution)(int)v; break;
                case "lodbias" when ok: QualitySettings.lodBias = v; break;
                case "far" when ok: if (cam) cam.farClipPlane = v; break;
                case "pixellights" when ok: QualitySettings.pixelLightCount = (int)v; break;
                case "texlimit" when ok: QualitySettings.globalTextureMipmapLimit = (int)v; break;
                case "aniso" when ok: QualitySettings.anisotropicFiltering = (AnisotropicFiltering)(int)v; break;
                // 2026-10-24: does keeping the picture off the Mac screen buy speed or battery? mirror 0 stops the
                // desktop copy of the eyes (one fewer blit + WindowServer compositing) so the two can be A/B timed.
                case "mirror" when ok: XRSettings.gameViewRenderMode = v <= 0 ? GameViewRenderMode.None : v >= 2 ? GameViewRenderMode.BothEyes : GameViewRenderMode.LeftEye; break;
                // the tour's first loop ran ~44 fps and the second a flat 90: first sight of each shader/texture costs.
                // warm compiles every loaded shader variant up front, so a first visit can be timed without that cost.
                // more than one test environment in one player (one firewall approval): every scene in the build has its own rig
                case "load" when p.Length >= 2: Debug.Log("frame-lab cmd: loading " + p[1]); tourHomeSet = false; SceneManager.LoadScene(p[1]); break;
                case "scenes":
                    for (int i = 0; i < SceneManager.sceneCountInBuildSettings; i++) Debug.Log("frame-lab cmd: scene " + i + " " + SceneUtility.GetScenePathByBuildIndex(i));
                    break;
                case "warm": { float t0 = Time.realtimeSinceStartup; Shader.WarmupAllShaders(); Debug.Log("frame-lab cmd: shaders warmed in " + ((Time.realtimeSinceStartup - t0) * 1000).ToString("F0") + " ms"); } break;
                case "hdr" when ok: if (cam) cam.allowHDR = v > 0; break;
                case "every" when ok: reportEvery = Mathf.Max(1f, v); break;
                case "lights" when ok: foreach (var li in lights) if (li && li.type != LightType.Directional) li.enabled = v > 0; break;
                case "sun" when ok: foreach (var li in lights) if (li && li.type == LightType.Directional) li.enabled = v > 0; break;
                case "instancing" when ok:
                    var seen = new HashSet<Material>(); int n = 0;
                    foreach (var r in FindObjectsByType<Renderer>(FindObjectsSortMode.None))
                        foreach (var m in r.sharedMaterials) if (m && seen.Add(m)) { m.enableInstancing = v > 0; n++; }
                    Debug.Log("frame-lab cmd: instancing " + (v > 0) + " on " + n + " materials");
                    break;
                case "goto" when p.Length >= 4:
                    // moves the XR origin so the HEAD lands at x y z (facing yaw if given): a repeatable benchmark view
                    var origin = transform.parent; if (!origin) break;
                    F(p[1], out var x); F(p[2], out var y); F(p[3], out var z);
                    if (p.Length >= 5 && F(p[4], out var yaw)) origin.rotation = Quaternion.Euler(0, yaw - transform.localEulerAngles.y, 0);
                    origin.position += new Vector3(x, y, z) - transform.position;
                    break;
                case "batch":
                    {
                        int roots = 0;
                        foreach (var g in gameObject.scene.GetRootGameObjects())
                            if (g != transform.root.gameObject) { StaticBatchingUtility.Combine(g); roots++; }
                        Debug.Log("frame-lab cmd: static batching combined under " + roots + " roots (meshes must be readable)");
                    }
                    break;
                case "tour" when p.Length >= 2 && p[1] == "stop": touring = false; TourSummary(); if (tourHomeSet && transform.parent) transform.parent.position = tourHome; break;
                case "tour":
                    tourLoops = ok ? Mathf.Max(1, (int)v) : 1;
                    if (p.Length >= 3 && F(p[2], out var sp)) tourSpeed = Mathf.Max(0.5f, sp);
                    TourStart();
                    break;
                case "state": break;
                case "help": Debug.Log("frame-lab cmd: " + Help); return;
                default: Debug.LogWarning("frame-lab cmd: unknown '" + l + "' - " + Help); return;
            }
            Debug.Log("frame-lab cmd: " + l + " -> " + State());
        }
    }
}
