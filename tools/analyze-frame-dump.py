#!/usr/bin/env python3
# analyze-frame-dump.py DUMP.bin WIDTH HEIGHT DUMP.pts - content order + motion cadence of consecutive decoded frames.
# Pair with FRAME_CLIENT_DUMP_FRAMES=N on the Frame client (steam-frame 5a4a3bc): off-head, after 300 loops, it writes N
# luma planes + pts to /tmp/frame-dump.{bin,pts}; scp them to another machine and run this (python3 -I, numpy). Uniform motion in
# order => every diff(i,i+1) alike, diff(i,i+2) ~ 2x, and the busiest column drifts monotonically (2026-10-18: it did).
import sys, numpy as np
w, h = int(sys.argv[2]), int(sys.argv[3])
raw = np.fromfile(sys.argv[1], dtype=np.uint8)
n = raw.size // (w*h); f = raw[:n*w*h].reshape(n, h, w).astype(np.int16)
pts = [int(x) for x in open(sys.argv[4])]
d1 = [float(np.abs(f[i+1]-f[i]).mean()) for i in range(n-1)]
d2 = [float(np.abs(f[i+2]-f[i]).mean()) for i in range(n-2)]
dp = np.diff(pts)
print(f"{n} frames {w}x{h}; pts step ms: mean {dp.mean()/1000:.2f} min {dp.min()/1000:.2f} max {dp.max()/1000:.2f}")
print("diff(i,i+1) mean abs luma:", " ".join(f"{x:.2f}" for x in d1))
print(f"d1 mean {np.mean(d1):.2f} sd {np.std(d1):.2f} min {min(d1):.2f} max {max(d1):.2f}; d2 mean {np.mean(d2):.2f} (uniform motion => d2 ~ 2*d1, every d1 alike)")
# column-of-motion test: the brightest-moving column per frame pair (cube edges) should drift monotonically
cols = [int(np.abs(f[i+1]-f[i]).mean(axis=0).argmax()) for i in range(n-1)]
print("argmax motion column per pair:", cols)
