#!/usr/bin/env python3
# sharpness.py EYE.rgba|DECODED.nv12 [W H] - stripe modulation (MTF) of the xr-smoke test card in a client eye dump.
#
# The card (engines/godot, head-locked 1 m ahead, 0.6 m square, 512x512 texels) is a black-bordered
# mid-grey square holding 8 bands of vertical black/white stripes with texel periods 2,3,4,6,8,12,16,24 (top to
# bottom). FRAME_CLIENT_DUMP=1 on the Frame client writes the rendered left eye as raw RGBA (2160x2160 today) to
# /tmp/frame_client_eye0.rgba at teardown; scp it to another machine and run this (python3 -I, numpy). For each band it prints
# the period it measures in panel pixels (autocorrelation of the band's mean row) and the modulation
# (p95-p5)/(p95+p5) of that row: 1.0 = the stripes arrive black and white, 0 = grey mush. Compare configurations
# on the same geometry, never absolute numbers across geometries. Also writes card.png (the crop) next to the dump.
import sys, numpy as np, zlib, struct, os
path = sys.argv[1]
w = int(sys.argv[2]) if len(sys.argv) > 2 else 2160
h = int(sys.argv[3]) if len(sys.argv) > 3 else 2160
raw = np.fromfile(path, dtype=np.uint8)
if raw.size == w*h or raw.size == w*h*3//2:   # a luma plane, or NV12 (luma first; /tmp/frame_client_decoded.nv12)
    img = raw[:w*h].reshape(h, w).astype(np.float32)
    if raw.size == w*h*3//2: img = img[:, :w//2]   # the decoded frame is side-by-side: keep the left eye
else:
    rgb = raw[:w*h*4].reshape(h, w, 4)[:, :, :3].astype(np.float32) / 255.0
    # The client's swapchain is an sRGB format: the shader writes the video's (already gamma-encoded) values and the
    # GPU stores them sRGB-encoded again, so the dump holds sRGB(video). Undo that one encode to get the video values
    # back (measured 2026-10-19: decoded 48 -> dump 120, 1 -> 14, 245 -> 250, exactly the curve); what the compositor
    # samples is decoded the same way, so this is also what reaches the panel.
    rgb = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4) * 255.0
    img = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
# Card finder (10-27, mac-push stand): it used to find the card as the only near-black frame, but this scene's floor is
# dark too (after the sRGB undo it reads < 24), and the box slid onto the floor. Find it by its white stripes instead
# (luma > 200 only on the card and its caption), take the topmost black border row above them, the side border
# columns below that row, and make the box square (the card is 0.6 m square).
dark = img < 24; bright = img > 200
bc = np.where(bright.mean(axis=0) > 0.02)[0]; br = np.where(bright.mean(axis=1) > 0.02)[0]
if bc.size == 0 or br.size == 0:
    print("no card found (no white stripes)"); sys.exit(1)
cw = bc.max() - bc.min() + 1; xa, xb = max(0, bc.min() - cw // 8), min(w - 1, bc.max() + cw // 8)
ys = [y for y in range(max(0, br.min() - cw // 4), br.min() + 1) if dark[y, bc.min():bc.max() + 1].mean() > 0.85]
y0 = ys[0] if ys else br.min()
band = dark[y0:min(h, y0 + cw), xa:xb + 1].mean(axis=0)
cx = np.where(band > 0.85)[0]
x0, x1 = (xa + cx.min(), xa + cx.max()) if cx.size else (bc.min(), bc.max())
y1 = y0 + (x1 - x0)
print(f"card box x {x0}-{x1} ({x1-x0+1} px) y {y0}-{y1} ({y1-y0+1} px) in {w}x{h}; {(x1-x0+1)/512:.3f} px per texel")
# layout in texels: border 8, then bands at y 24 + i*60 (48 tall, 12 gap), stripes span x 24..488
periods = [2, 3, 4, 6, 8, 12, 16, 24]
sx = (x1 - x0 + 1) / 512.0; sy = (y1 - y0 + 1) / 512.0
def png(fn, a):
    a = np.clip(a, 0, 255).astype(np.uint8); hh, ww = a.shape
    rawd = b"".join(b"\x00" + a[i].tobytes() for i in range(hh))
    def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(fn, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", ww, hh, 8, 0, 0, 0, 0)) + ch(b"IDAT", zlib.compress(rawd, 6)) + ch(b"IEND", b""))
png(os.path.join(os.path.dirname(path) or ".", "card.png"), img[y0:y1+1, x0:x1+1])
# modulation is also given relative to the coarsest band, so a limited-range decoded plane (16..235 = 0.87 raw)
# compares with a full-range eye dump (0..255 = 1.0 raw)
res = []
for i, p in enumerate(periods):
    ya = int(y0 + (24 + i*60 + 12) * sy); yb = int(y0 + (24 + i*60 + 36) * sy)
    xa = int(x0 + 40 * sx); xb = int(x0 + 472 * sx)
    row = img[ya:yb, xa:xb].mean(axis=0)
    lo, hi = np.percentile(row, 5), np.percentile(row, 95)
    mod = (hi - lo) / max(hi + lo, 1e-6)
    r = row - row.mean(); ac = np.correlate(r, r, "full")[r.size-1:]; ac /= max(ac[0], 1e-6)
    # the autocorrelation peak nearest the expected period (within +-40 %), so a harmonic cannot be picked
    exp = p * sx; lo_k, hi_k = max(1, int(exp*0.6)), max(2, int(np.ceil(exp*1.4)) + 1)
    seg = ac[lo_k:hi_k]; per = float("nan")
    if seg.size and seg.max() > 0.1: per = lo_k + int(seg.argmax())
    res.append((i+1, p, per, p*sx, mod, lo, hi))
ref = max(r[4] for r in res) or 1.0
print("band  texel-period  measured-px (expected)  modulation  relative  (row luma p5/p95)")
for (b, p, per, exp, mod, lo, hi) in res:
    print(f"  {b}    {p:3d}            {per:5.1f} ({exp:4.1f})        {mod:.3f}      {mod/ref:.3f}    {lo:5.1f}/{hi:5.1f}")
