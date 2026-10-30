#!/usr/bin/env python3
"""clip-jitter — read a frame-record clip (the Frame's composited mirror, 1080p30) for the streamed scene's
placement error against the compositor's own keyboard, which is world-locked and rendered at the display pose.

    python3 -I clip-jitter.py <clip.mp4 | dir-of-grey-pngs>

Phase correlation per frame on two regions (keyboard = bottom-centre, scene = upper-middle), then:
  1. scene-minus-keyboard displacement binned by head speed (keyboard px/frame) — an error that grows with
     speed and is near zero when still is a timing error, not encode noise;
  2. the slope of the offset CHANGE on the keyboard's VELOCITY change — a consistent lag/lead in mirror
     frames (x33 ms); a near-zero slope with large residuals is per-frame timing JITTER, not latency.
Written 2026-10-18 for a "vibration" seen on a worn run. Needs numpy +
PIL; ffmpeg if given an mp4. The mirror is 30 fps of a 90 Hz panel: the numbers are indicative, the trend is real.
"""
import sys, os, glob, subprocess, tempfile
import numpy as np
from PIL import Image

def frames(src):
    if os.path.isdir(src):
        files = sorted(glob.glob(os.path.join(src, "*.png")))
    else:
        d = tempfile.mkdtemp(prefix="clip-jitter-")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", "format=gray", os.path.join(d, "%05d.png")], check=True)
        files = sorted(glob.glob(os.path.join(d, "*.png")))
    return [np.asarray(Image.open(f), dtype=np.float32) for f in files]

def shift(a, b):
    a = a - a.mean(); b = b - b.mean()
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    A = np.fft.fft2(a * win); B = np.fft.fft2(b * win)
    R = A * np.conj(B); R /= (np.abs(R) + 1e-6)
    r = np.fft.ifft2(R).real
    iy, ix = np.unravel_index(np.argmax(r), r.shape)
    if iy > a.shape[0] // 2: iy -= a.shape[0]
    if ix > a.shape[1] // 2: ix -= a.shape[1]
    def sp(v, i):
        n = len(v); a_, b_, c_ = v[(i - 1) % n], v[i], v[(i + 1) % n]; d = a_ - 2 * b_ + c_
        return 0.5 * (a_ - c_) / d if d != 0 else 0.0
    return ix + sp(r[iy % r.shape[0], :], ix % r.shape[1]), iy + sp(r[:, ix % r.shape[1]], iy % r.shape[0]), r.max()

def main(src):
    imgs = frames(src); H, W = imgs[0].shape
    kb = (slice(int(H * 0.62), H), slice(int(W * 0.15), int(W * 0.62)))
    sc = (slice(0, int(H * 0.5)), slice(int(W * 0.25), int(W * 0.75)))
    K, Sc, ok = [], [], []
    for i in range(1, len(imgs)):
        k = shift(imgs[i][kb], imgs[i - 1][kb]); s = shift(imgs[i][sc], imgs[i - 1][sc])
        K.append(k[:2]); Sc.append(s[:2]); ok.append(k[2] >= 0.15 and s[2] >= 0.15 and abs(k[0]) < 40 and abs(s[0]) < 40)
    K = np.array(K); Sc = np.array(Sc); ok = np.array(ok)
    ks = np.hypot(K[:, 0], K[:, 1]); rel = np.hypot(Sc[:, 0] - K[:, 0], Sc[:, 1] - K[:, 1])
    print(f"{len(imgs)} frames, {int(ok.sum())} with a confident match in both regions")
    print("scene-minus-keyboard displacement by head speed (keyboard px per mirror frame):")
    for lo, hi in [(0, 0.5), (0.5, 2), (2, 5), (5, 10), (10, 20), (20, 60)]:
        m = ok & (ks >= lo) & (ks < hi)
        if m.sum():
            print(f"  {lo:>4}-{hi:<3} px/frame  n={int(m.sum()):3d}  median={np.median(rel[m]):.2f}  p75={np.percentile(rel[m], 75):.2f}  p90={np.percentile(rel[m], 90):.2f} px")
    d = (Sc - K)[1:]; dv = np.diff(K, axis=0); m = ok[1:] & ok[:-1]
    x = dv[m].ravel(); y = d[m].ravel(); sel = np.abs(x) > 0.3; x = x[sel]; y = y[sel]
    if len(x) > 10:
        L = np.sum(x * y) / np.sum(x * x); res = y - L * x
        keep = np.abs(res) <= np.percentile(np.abs(res), 90)
        L2 = np.sum(x[keep] * y[keep]) / np.sum(x[keep] * x[keep]); r = np.corrcoef(x[keep], y[keep])[0, 1]
        print(f"lag regression: slope {L2:+.3f} mirror frames = scene {'lags' if L2 < 0 else 'leads'} by {abs(L2) * 33.3:.1f} ms, r={r:.2f} (n={int(keep.sum())}); |r| under 0.5 means jitter, not lag")

if __name__ == "__main__":
    main(sys.argv[1])
