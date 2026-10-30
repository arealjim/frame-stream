#!/usr/bin/env python3
# static-noise.py DUMP.bin W H LABEL - at a fixed pose, how much of the picture changes frame to frame, and where.
# Pair with FRAME_CLIENT_DUMP_FRAMES=90 on the Frame client, headset unworn (the pose is held): scp /tmp/frame-dump.bin and run
# python3 -I static-noise.py frame-dump.bin 2752 1216 "label" (python3 -I, numpy). 2026-10-20: Unity at a fixed pose is 0.000
# frame to frame at 2x and 8x MSAA, so the at-rest vibration is in the pose path, not the picture.
import sys, numpy as np
w, h = int(sys.argv[2]), int(sys.argv[3])
raw = np.fromfile(sys.argv[1], dtype=np.uint8); n = raw.size // (w*h)
f = raw[:n*w*h].reshape(n, h, w).astype(np.float32)
d = np.abs(np.diff(f, axis=0))                      # consecutive diffs
mean_d = d.mean(); frac8 = (d > 8).mean(); frac16 = (d > 16).mean()
tstd = f.std(axis=0)                                # temporal std per pixel
m = f.mean(axis=0); gy, gx = np.gradient(m); grad = np.hypot(gx, gy)
edge = grad > 12; flat = grad < 2
print(f"{sys.argv[4]}: {n} frames {w}x{h}; mean |diff| {mean_d:.3f}; pixels changing >8: {frac8*100:.3f}%  >16: {frac16*100:.3f}%; "
      f"temporal std on edges {tstd[edge].mean():.2f} ({edge.mean()*100:.2f}% of pixels) vs flat {tstd[flat].mean():.2f}; "
      f"per-pair mean diff min/max {d.mean(axis=(1,2)).min():.3f}/{d.mean(axis=(1,2)).max():.3f}")
