#!/usr/bin/env python3
# resample-model.py - predict tools/sharpness.py's stripe modulation for a Mac render size and a client swapchain size,
# from the code on both ends and nothing fitted (perf-chain.md Q7, 2026-10-27). Pure numpy, no headset.
#
# The chain, 1-D across the card's stripe rows (the stripes are vertical, so the vertical axis changes nothing):
#   1. Godot samples the 512-texel card (xr-smoke/main.gd _make_card: mipmapped, StandardMaterial3D default filter =
#      trilinear) at the Mac render size. The card is 451 px at a 2160 eye (measured), so 451 x eye/2160 px elsewhere,
#      and the mip LOD is log2(512 / card px): 0.18 at 2160, 0.50 at 1728, 0 (magnified) at 2880.
#   2. Linear light to sRGB (the runtime encodes before VideoToolbox). The foveation centre is one encoded px per
#      render px at every eye size used here, with its edges on whole pixels (Foveation.h CalculateFoveationLayout), so
#      the encode is treated as lossless (min_qp and bitrate moved nothing, 10-19).
#   3. The Frame client samples the decoded luma bilinearly with ONE mip level (main.cpp sampler: LINEAR, maxLod 1,
#      mipLevels 1) into the swapchain. Equal sizes = a straight copy; any other ratio resamples with no prefilter.
#   4. Scored like sharpness.py: (p95-p5)/(p95+p5) of the band row, x 40..472 texels. Since client patch 0015 the dump
#      holds video values and sharpness.py undoes sRGB once more, so today's numbers are in linear light (--video
#      scores video values, which is what the 10-19 dumps held).
# Each case runs 40 random sub-pixel placements (card on the Mac grid, swapchain grid on the decoded grid): mean (min-max).
import sys, numpy as np
VIDEO = "--video" in sys.argv

def card_rows():
    t = np.full(512, 0.5); t[:8] = 0; t[504:] = 0
    rows = {}
    for p in (3, 4):
        r = t.copy(); x = np.arange(24, 488)
        r[24:488] = np.where((np.floor((x - 24) * 2 / p).astype(int) % 2) == 0, 1.0, 0.0)
        rows[p] = r
    return rows

def mips(r):
    m = [r]
    while m[-1].size > 1: a = m[-1]; m.append(0.5 * (a[0::2] + a[1::2]))
    return m

def bilinear(a, u):   # u in texel units, texel centres at i + 0.5, clamp to edge
    x = u - 0.5; i = np.floor(x).astype(int); f = x - i
    return a[np.clip(i, 0, a.size - 1)] * (1 - f) + a[np.clip(i + 1, 0, a.size - 1)] * f

def srgb(c): return np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(np.maximum(c, 0), 1 / 2.4) - 0.055)
def linear(v): v = v / 255; return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4) * 255

def mac_render(m, card_px, off):
    n = int(np.ceil(card_px)) + 2
    u = (np.arange(n) + 0.5 - off) / card_px * 512
    lod = np.log2(512 / card_px)
    if lod <= 0: v = bilinear(m[0], u)
    else:
        l0 = int(np.floor(lod)); f = lod - l0
        v = (1 - f) * bilinear(m[l0], u / 2 ** l0) + f * bilinear(m[l0 + 1], u / 2 ** (l0 + 1))
    return srgb(np.clip(v, 0, 1)) * 255, n

def score(row, card_px):
    s = card_px / 512; r = row[int(40 * s):int(472 * s)]
    if not VIDEO: r = linear(r)
    lo, hi = np.percentile(r, 5), np.percentile(r, 95)
    return (hi - lo) / max(hi + lo, 1e-6)

M = {p: mips(r) for p, r in card_rows().items()}
def case(render_eye, swap_eye, p, seeds=40):
    rng = np.random.default_rng(1); out = []
    for _ in range(seeds):
        cp = 451.0 * render_eye / 2160
        img, n = mac_render(M[p], cp, rng.random())
        ratio = swap_eye / render_eye
        if abs(ratio - 1) < 1e-9: row, cd = img, cp
        else:
            k = int(n * ratio) - 2
            row = bilinear(img, (np.arange(k) + 0.5) / ratio + rng.random()); cd = cp * ratio
        out.append(score(row, cd))
    return np.mean(out), np.min(out), np.max(out)

# (label, Mac render eye, client swapchain eye, measured 10-27 4-texel, measured 10-27 3-texel)
CASES = [("swapchain 1.0, native render", 2160, 1728, "0.906-0.921", "0.55-0.56"),
         ("swapchain 1.1, native", 2160, 1901, "0.917", "0.554"),
         ("swapchain 1.15, native", 2160, 1987, "0.928", "0.583"),
         ("swapchain 1.2, native", 2160, 2074, "0.916", "0.580"),
         ("swapchain 1.25, native (1:1)", 2160, 2160, "0.978-0.982", "0.747-0.752"),
         ("swapchain 1.0, render 1728 (Q22)", 1728, 1728, "0.948", "0.594-0.600"),
         ("swapchain 1984, native", 2160, 1984, "-", "-"),
         ("swapchain 1984, render 1984", 1984, 1984, "-", "-"),
         ("swapchain 2080, render 2080", 2080, 2080, "-", "-"),
         ("dense: 2880 render, 2880 swapchain", 2880, 2880, "-", "-")]
if VIDEO:   # the 10-19 dumps (older scorer input): only these two were taken
    V = {2160: ("0.83", "0.41"), 2880: ("1.00", "0.52")}
    CASES = [(l, r, w, *(V[r] if r == w and r in V else ("-", "-"))) for l, r, w, _, _ in CASES]
print("scored in", "video values (10-19 dumps)" if VIDEO else "linear light (dumps since client patch 0015, i.e. 10-27)")
print(f"{'case':38s} {'4-texel (3.83 cpd) model':>26s} {'measured':>12s}   {'3-texel (5.11 cpd) model':>26s} {'measured':>12s}")
for label, re_, se, m4, m3 in CASES:
    a4 = case(re_, se, 4); a3 = case(re_, se, 3)
    print(f"{label:38s} {a4[0]:.3f} ({a4[1]:.3f}-{a4[2]:.3f})    {m4:>12s}   {a3[0]:.3f} ({a3[1]:.3f}-{a3[2]:.3f})    {m3:>12s}")
