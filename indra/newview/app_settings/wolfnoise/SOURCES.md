# wolfnoise — the clouds' blue noise

`wolf_bluenoise_64.r16` is a 64 x 64 blue-noise dither array: unsigned 16-bit little-endian ranks 0..4095, row-major, row 0
first (8192 bytes). The Wolf sky's volumetric clouds take their per-texel ray offsets from it (`shaders/class1/wolftk/cloudsCommon.glsl`
`getSTBN`, loaded by `wolftkclouds.cpp`), tiled over the low-resolution march and stepped through time with the golden-ratio
additive recurrence.

## Origin

Made by WolfViewer (IntelligentWolf Ltd) with `make_bluenoise.py` in this folder, our own implementation of the method published in

- R. Ulichney, "The void-and-cluster method for dither array generation", *Proc. SPIE 1913, Human Vision, Visual Processing,
  and Digital Display IV* (1993), pp. 332-343, doi:10.1117/12.152707.

No third-party code, texture or data is used. NVIDIA's spatio-temporal blue-noise textures, which the three-geospatial clouds
use, are not included (their licence does not cover this product). The temporal scheme (blue noise in space, golden ratio in
time) is the baseline described in A. Wolfe, N. Morrical, T. Akenine-Möller, R. Ramamoorthi, "Spatiotemporal Blue Noise
Masks", *Eurographics Symposium on Rendering* (2022).

## Rebuilding

    python3 make_bluenoise.py

The seed is fixed, so on the same Python/NumPy build it writes the same file bit for bit (md5
de7faba11c6ffbd9ef76cde84b963a74, NumPy FFT on Linux x86-64). Ties between equal energies are broken by scan order, so another
NumPy or FFT build could pick a different but equally valid array; the committed file is the reference. The repository stores
`*.r16` as binary (`.gitattributes`).
