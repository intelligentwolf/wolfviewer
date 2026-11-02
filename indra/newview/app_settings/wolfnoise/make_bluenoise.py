#!/usr/bin/env python3
"""[WolfViewer 2026-11-02] Our own blue-noise dither array for the Wolf sky's clouds (cloudsCommon.glsl getSTBN), made with the
void-and-cluster method as published in:

  R. Ulichney, "The void-and-cluster method for dither array generation", Proc. SPIE 1913, Human Vision, Visual Processing
  and Digital Display IV (1993), pp. 332-343, doi:10.1117/12.152707.

Written from that paper (no third-party code or texture): the energy of a pixel is its binary pattern filtered with a
Gaussian on the torus (the array tiles the screen); "tightest cluster" = the minority pixel with the highest energy, "largest
void" = the majority pixel with the lowest. Phase 0 relaxes a random initial pattern; phase 1 ranks the initial ones by
removing tightest clusters; phase 2 ranks up to half by filling largest voids; phase 3 ranks the rest by filling the tightest
clusters of the zeros (the minority from then on). Sigma 1.5 is the paper's choice.

Output wolf_bluenoise_64.r16: 64 x 64 unsigned 16-bit little-endian ranks 0..4095, row-major, row 0 first. Deterministic
(fixed seed) so the file can be rebuilt bit for bit:  python3 make_bluenoise.py
"""
import os, struct, sys
import numpy as np

N = 64
SIGMA = 1.5
INITIAL_FRACTION = 0.1
SEED = 20261102

def gaussian_kernel():
    # the Gaussian centred on (0, 0) on the N x N torus: d = the wrapped distance
    i = np.arange(N)
    d = np.minimum(i, N - i).astype(np.float64)
    dx, dy = np.meshgrid(d, d, indexing='xy')
    return np.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA))

K = gaussian_kernel()

def energy_of(pattern):
    # circular convolution (the array tiles) via the FFT
    return np.real(np.fft.ifft2(np.fft.fft2(pattern.astype(np.float64)) * np.fft.fft2(K)))

def toggle(E, y, x, sign):
    E += sign * np.roll(np.roll(K, y, axis=0), x, axis=1)

def tightest_cluster(pattern, E, value):
    # the pixel equal to `value` with the highest energy
    m = np.where(pattern == value, E, -np.inf)
    return np.unravel_index(int(np.argmax(m)), m.shape)

def largest_void(pattern, E, value):
    # the pixel NOT equal to `value` with the lowest energy
    m = np.where(pattern != value, E, np.inf)
    return np.unravel_index(int(np.argmin(m)), m.shape)

def main():
    rng = np.random.default_rng(SEED)
    ones = int(N * N * INITIAL_FRACTION)
    proto = np.zeros((N, N), np.int8)
    proto.flat[rng.choice(N * N, ones, replace=False)] = 1
    # phase 0: relax the initial pattern (move the tightest cluster's pixel to the largest void until it lands back)
    E = energy_of(proto)
    while True:
        cy, cx = tightest_cluster(proto, E, 1)
        proto[cy, cx] = 0; toggle(E, cy, cx, -1)
        vy, vx = largest_void(proto, E, 1)
        if (vy, vx) == (cy, cx):
            proto[cy, cx] = 1; toggle(E, cy, cx, +1)
            break
        proto[vy, vx] = 1; toggle(E, vy, vx, +1)
    rank = np.full((N, N), -1, np.int32)
    # phase 1: rank the prototype's ones, tightest cluster first, from ones - 1 down to 0
    p = proto.copy(); E = energy_of(p)
    for r in range(ones - 1, -1, -1):
        y, x = tightest_cluster(p, E, 1)
        p[y, x] = 0; toggle(E, y, x, -1); rank[y, x] = r
    # phase 2: from the prototype, fill the largest voids up to half
    p = proto.copy(); E = energy_of(p)
    for r in range(ones, N * N // 2):
        y, x = largest_void(p, E, 1)
        p[y, x] = 1; toggle(E, y, x, +1); rank[y, x] = r
    # phase 3: the zeros are the minority now - rank the rest by filling the tightest cluster of zeros
    E = energy_of(1 - p)
    for r in range(N * N // 2, N * N):
        y, x = tightest_cluster(p, E, 0)
        p[y, x] = 1; toggle(E, y, x, -1); rank[y, x] = r
    assert (rank >= 0).all() and len(np.unique(rank)) == N * N
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wolf_bluenoise_64.r16')
    with open(out, 'wb') as f:
        f.write(struct.pack('<%dH' % (N * N), *rank.flatten().tolist()))
    print('wrote', out)

if __name__ == '__main__':
    sys.exit(main())
