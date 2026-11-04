#!/usr/bin/env python3
# [VR 3D FIX 2026-11-04] Measures the depth in one VR 3D Mode stereo picture: the two eye images the viewer saved
# (WolfVRDebugEyeDump -> logs/wolfvr_eye_left.png, wolfvr_eye_right.png). For a grid of blocks it finds the horizontal
# shift (pixels) at which the right eye's block best matches the left eye's (sum of absolute grey differences, the
# right image moved left), and prints it per block row plus the strongest blocks with where they are. Near things
# shift a lot, far things little, the sky ~0. With the eye window tangents (L, R per eye, from the viewer's log line)
# a shift converts to a distance: Z = IPD * px_per_tangent / shift.
#   python3 -I eye_disparity.py LEFT.png RIGHT.png [ipd_m=0.064] [tanL tanR]
import sys
import numpy as np
from PIL import Image

L = np.asarray(Image.open(sys.argv[1]).convert('L'), dtype=np.float32)
R = np.asarray(Image.open(sys.argv[2]).convert('L'), dtype=np.float32)
ipd = float(sys.argv[3]) if len(sys.argv) > 3 else 0.064
span = (float(sys.argv[5]) - float(sys.argv[4])) if len(sys.argv) > 5 else None
H, W = L.shape
B = max(16, W // 24)          # block size
MAXS = W // 8                 # largest shift looked for
rows = []
best_blocks = []
for by in range(0, H - B, B):
    row = []
    for bx in range(MAXS, W - B, B):
        a = L[by:by + B, bx:bx + B]
        contrast = float(np.abs(np.diff(a, axis=1)).mean())
        if contrast < 2.0:    # flat (sky, fog, a wall): no shift can be measured
            row.append(None)
            continue
        sads = [float(np.abs(a - R[by:by + B, bx - s:bx - s + B]).mean()) for s in range(0, MAXS)]
        s = int(np.argmin(sads))
        row.append(s)
        best_blocks.append((s, bx + B // 2, by + B // 2, sads[s], sads[0]))
    rows.append((by, row))
print(f'{W}x{H} per eye, block {B} px, shifts 0..{MAXS - 1} px ("." = no texture to measure)')
for by, row in rows:
    vals = [v for v in row if v is not None]
    med = int(np.median(vals)) if vals else -1
    print(f'  rows {by:4d}-{by + B:4d}: median shift {med:3d} px  ' + ' '.join('.' if v is None else str(v) for v in row))
best_blocks.sort(reverse=True)
print('largest shifts (px, at x, y; matched / unshifted difference):')
for s, x, y, sad, sad0 in best_blocks[:8]:
    z = f', about {ipd * (W / span) / s:.2f} m away' if span and s > 0 else ''
    print(f'  {s:3d} px at ({x}, {y}); {sad:.1f} / {sad0:.1f}{z}')
flat_top = [v for _, row in rows[:2] for v in row if v is not None]
print('top two block rows (sky / far):', 'median ' + str(int(np.median(flat_top))) + ' px' if flat_top else 'no texture (plain sky)')
