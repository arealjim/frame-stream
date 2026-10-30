// frame-lab: the 90 Hz budget knobs for a URP scene, applied at play (2026-10-21 evening).
//
// The rig's FRAMELAB_MSAA sets QualitySettings.antiAliasing, which URP ignores (the pipeline asset owns MSAA), so the
// first unity-urp "MSAA makes no difference" A/B measured 4x twice. These act on the URP asset and the scene directly.
//   FRAMELAB_STEPS="<s>:<key>=<value>;..."  seconds after the first scene loads; keys:
//     msaa=1|2|4|8    UniversalRenderPipelineAsset.msaaSampleCount
//     scale=0.5..1    UniversalRenderPipelineAsset.renderScale (URP applies it to the XR eye textures too)
//     shadowdist=M    UniversalRenderPipelineAsset.shadowDistance (0 = no main-light shadows)
//     lightshadows=0  every point/spot light's shadows -> None (1 restores Soft)
//     postfx=0|1      the main camera's renderPostProcessing
//     eyescale=0.5..1 XRSettings.eyeTextureResolutionScale
//     shadowres=N     UniversalRenderPipelineAsset.mainLightShadowmapResolution (256..4096)
//     cascades=1..4   UniversalRenderPipelineAsset.shadowCascadeCount
//     queued=N        QualitySettings.maxQueuedFrames (frames the CPU may run ahead of the GPU)
//   e.g. FRAMELAB_STEPS="0:msaa=2;30:scale=0.8"
// Always: every 450 frames, "frame-lab budget: cpu X ms gpu Y ms main M render R presentwait W ms (FrameTimingManager, n=…)"
// plus the knob state. cpu is the whole frame interval (waits included); main minus presentwait is the main thread's work.
#if FRAMELAB_URP
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace FrameLab
{
    public class FrameLabBudget : MonoBehaviour
    {
        readonly List<KeyValuePair<float, string>> steps = new List<KeyValuePair<float, string>>();
        float start;
        int frames; double cpuSum, gpuSum, mainSum, renderSum, waitSum; int timed;
        readonly FrameTiming[] ft = new FrameTiming[1];

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        static void Boot()
        {
            var go = new GameObject("frame-lab budget"); DontDestroyOnLoad(go); go.AddComponent<FrameLabBudget>();
        }

        void Start()
        {
            start = Time.realtimeSinceStartup;
            var spec = System.Environment.GetEnvironmentVariable("FRAMELAB_STEPS");
            if (!string.IsNullOrEmpty(spec))
                foreach (var item in spec.Split(';'))
                {
                    int c = item.IndexOf(':'); if (c < 0) continue;
                    if (float.TryParse(item.Substring(0, c), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var t))
                        steps.Add(new KeyValuePair<float, string>(t, item.Substring(c + 1).Trim()));
                }
            steps.Sort((a, b) => a.Key.CompareTo(b.Key));
            Debug.Log("frame-lab budget: " + steps.Count + " step(s) from FRAMELAB_STEPS; " + State());
        }

        static UniversalRenderPipelineAsset Asset => GraphicsSettings.currentRenderPipeline as UniversalRenderPipelineAsset;

        string State()
        {
            var a = Asset; if (a == null) return "no URP asset";
            var cam = Camera.main; var cd = cam ? cam.GetUniversalAdditionalCameraData() : null;
            return "msaa " + a.msaaSampleCount + " scale " + a.renderScale.ToString("F2") + " shadowdist " + a.shadowDistance.ToString("F0")
                + " postfx " + (cd ? cd.renderPostProcessing.ToString() : "?") + " eyescale " + UnityEngine.XR.XRSettings.eyeTextureResolutionScale.ToString("F2")
                + " shadowres " + a.mainLightShadowmapResolution + " cascades " + a.shadowCascadeCount + " queued " + QualitySettings.maxQueuedFrames;
        }

        void Apply(string kv)
        {
            int e = kv.IndexOf('='); if (e < 0) return;
            string k = kv.Substring(0, e).Trim(); float v = float.Parse(kv.Substring(e + 1), System.Globalization.CultureInfo.InvariantCulture);
            var a = Asset;
            switch (k)
            {
                case "msaa": if (a) a.msaaSampleCount = (int)v; break;
                case "scale": if (a) a.renderScale = v; break;
                case "shadowdist": if (a) a.shadowDistance = v; break;
                case "lightshadows":
                    foreach (var l in FindObjectsByType<Light>(FindObjectsSortMode.None))
                        if (l.type == LightType.Point || l.type == LightType.Spot) l.shadows = v > 0 ? LightShadows.Soft : LightShadows.None;
                    break;
                case "postfx": { var cam = Camera.main; var cd = cam ? cam.GetUniversalAdditionalCameraData() : null; if (cd) cd.renderPostProcessing = v > 0; break; }
                case "eyescale": UnityEngine.XR.XRSettings.eyeTextureResolutionScale = v; break;
                case "shadowres": if (a) a.mainLightShadowmapResolution = (int)v; break;
                case "cascades": if (a) a.shadowCascadeCount = (int)v; break;
                case "queued": QualitySettings.maxQueuedFrames = (int)v; break;
                default: Debug.LogWarning("frame-lab budget: unknown key " + k); return;
            }
            Debug.Log("frame-lab budget: at " + (Time.realtimeSinceStartup - start).ToString("F1") + " s " + kv + " -> " + State());
        }

        string Avg(double sum) { return timed > 0 ? (sum / timed).ToString("F2") : "?"; }

        void Update()
        {
            float t = Time.realtimeSinceStartup - start;
            while (steps.Count > 0 && steps[0].Key <= t) { Apply(steps[0].Value); steps.RemoveAt(0); }
            FrameTimingManager.CaptureFrameTimings();
            if (FrameTimingManager.GetLatestTimings(1, ft) > 0 && ft[0].gpuFrameTime > 0) { cpuSum += ft[0].cpuFrameTime; gpuSum += ft[0].gpuFrameTime; mainSum += ft[0].cpuMainThreadFrameTime; renderSum += ft[0].cpuRenderThreadFrameTime; waitSum += ft[0].cpuMainThreadPresentWaitTime; timed++; }
            if (++frames % 450 == 0)
            {
                Debug.Log("frame-lab budget: t " + t.ToString("F0") + " s cpu " + (timed > 0 ? (cpuSum / timed).ToString("F2") : "?") + " ms gpu "
                    + (timed > 0 ? (gpuSum / timed).ToString("F2") : "?") + " ms main " + Avg(mainSum) + " render " + Avg(renderSum) + " presentwait " + Avg(waitSum)
                    + " ms (FrameTimingManager, n=" + timed + "; cpu is the whole frame interval, waits included); " + State());
                cpuSum = gpuSum = mainSum = renderSum = waitSum = 0; timed = 0;
            }
        }
    }
}
#endif
