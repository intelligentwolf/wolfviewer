#!/usr/bin/env python3
# [VR DEPTH 2026-11-04] Reads e2e_parallax.mjs's headset views: for the LEFT eye (left half of the canvas), the
# horizontal shift of a near band (the bottom: the ground close in front) and a far band (above the middle: the distance)
# between the head at 0 and 10 cm to the right, the picture held. World-locked eyes: the near band shifts left by about
# f * 0.10 / z px, the far band ~0. Flat cards: neither shifts against the other.
#   python3 -I parallax_check.py OUT_DIR
import json, sys, os
import numpy as np
from PIL import Image
d = sys.argv[1]
meta = json.load(open(os.path.join(d, 'canvas.json')))
x0, y0, w, h, dpr = meta['rect']
x0, y0, w, h = [int(round(v * dpr)) for v in (x0, y0, w, h)]
def eye(name):
    a = np.asarray(Image.open(os.path.join(d, name)).convert('L'), dtype=np.float32)
    return a[y0:y0 + h, x0:x0 + w // 2]
def shift(a, b, r0, r1, maxs=120):
    H, W = a.shape
    rows = slice(int(H * r0), int(H * r1))
    best, bsad, sad0 = 0, 1e9, None
    for s in range(-maxs, maxs + 1):
        lo, hi = maxs, W - maxs
        sad = float(np.abs(a[rows, lo:hi] - b[rows, lo + s:hi + s]).mean())
        if s == 0: sad0 = sad
        if sad < bsad: bsad, best = sad, s
    return best, round(bsad, 2), round(sad0, 2)
for tag in [t for t in ('depth', 'depthcut', 'cards') if os.path.exists(os.path.join(d, f'p_{t}_0.png'))]:   # [Wolf 2026-11-06] depthcut: the old cut edges
    a, b = eye(f'p_{tag}_0.png'), eye(f'p_{tag}_10cm.png')
    near, far = shift(a, b, 0.80, 0.97), shift(a, b, 0.33, 0.50)
    print(f'{tag:6s}: eye view {a.shape[1]}x{a.shape[0]} px; near band shift {near[0]:+d} px (sad {near[1]} vs {near[2]} unshifted), '
          f'far band {far[0]:+d} px; near minus far {near[0] - far[0]:+d} px')
