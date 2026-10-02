#!/usr/bin/env python3
"""Make the world map's animated water (WolfViewer 2026-10-02, Paul: "could we use an animated
water gif instead and just tile it?").

16 frames, 256 x 256, seamless in x and y and looping in time: every wave has a whole number of
cycles across the tile and a whole number of cycles over the 16 frames, so frame 16 is frame 0
and the tile's right edge meets its left. The base is the grid's water, MapColorWater "#112D54"
(177 of the region inis on regions3, 10-02), so the moving sea reads as the same water the
region tiles draw. Drawn here from sine waves; no outside artwork.

    python3 make_water.py        (writes water_00.png .. water_15.png beside this file)
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 256
FRAMES = 16
BASE = np.array([0x11, 0x2D, 0x54], dtype=np.float64)       # MapColorWater
CREST = np.array([0x3A, 0x6E, 0xA8], dtype=np.float64)      # lit wave tops
TROUGH = np.array([0x0B, 0x22, 0x44], dtype=np.float64)     # shaded troughs

# (cycles across x, cycles across y, cycles per loop, amplitude). Whole numbers only.
WAVES = [
    (3, 1, 1, 1.00), (-2, 3, -1, 0.85), (1, -4, 2, 0.60), (5, 2, -2, 0.45),
    (-4, -3, 1, 0.40), (7, -1, 3, 0.25), (-1, 7, -2, 0.22), (6, 5, 1, 0.18),
]

y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
out = Path(__file__).resolve().parent
for f in range(FRAMES):
    t = f / FRAMES
    h = np.zeros((SIZE, SIZE))
    for kx, ky, kt, a in WAVES:
        h += a * np.sin(2 * math.pi * (kx * x + ky * y + kt * t))
    h /= sum(a for *_, a in WAVES)                      # -1 .. 1
    # Sharp bright crests and broad dark troughs, like sunlit chop seen from above.
    crest = np.clip(h, 0, 1) ** 2.2
    trough = np.clip(-h, 0, 1) ** 1.3
    rgb = BASE + (CREST - BASE) * crest[..., None] * 0.55 + (TROUGH - BASE) * trough[..., None] * 0.8
    Image.fromarray(np.clip(rgb + 0.5, 0, 255).astype(np.uint8), 'RGB').save(out / f'water_{f:02d}.png', optimize=True)
print('wrote', FRAMES, 'frames to', out)
