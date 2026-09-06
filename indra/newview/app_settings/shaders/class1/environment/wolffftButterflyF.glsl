/**
 * @file wolffftButterflyF.glsl
 * @brief WolfViewer: spectral ocean, one Stockham radix-2 inverse-FFT stage (wolfoceanfft.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/ocean_fft.js OceanFFT._flyMat fragment — the same maths.
// Gather form for output index j at stage half-span Ns:
//   i = (j / 2Ns) * Ns + (j mod Ns), k = j mod Ns, b = in[i + N/2] * e^{+2 pi i k / 2Ns}
//   out[j] = (j mod 2Ns) < Ns ? in[i] + b : in[i] - b
// Ns runs 1, 2, ..., N/2 (log2 N passes per axis); after the last stage the output is
// in natural order. Inverse transform: +i twiddle, no 1/N (the sum h(x) = sum h(k) e^{ikx}
// is exactly what the ocean wants). Both targets are transformed together.

out vec4 frag_data[2];

uniform sampler2D uIn0;
uniform sampler2D uIn1;
uniform int uNs;
uniform int uN;
uniform int uHorizontal;

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

void main()
{
    ivec2 id = ivec2(gl_FragCoord.xy);
    int j = (uHorizontal == 1) ? id.x : id.y;
    int twoNs = uNs * 2;
    int k = j % uNs;
    int i = (j / twoNs) * uNs + k;
    int ib = i + uN / 2;
    ivec2 ia = (uHorizontal == 1) ? ivec2(i, id.y) : ivec2(id.x, i);
    ivec2 ibb = (uHorizontal == 1) ? ivec2(ib, id.y) : ivec2(id.x, ib);
    float ang = 6.28318530718 * float(k) / float(twoNs);
    vec2 tw = vec2(cos(ang), sin(ang));
    vec4 a0 = texelFetch(uIn0, ia, 0);
    vec4 b0 = texelFetch(uIn0, ibb, 0);
    vec4 a1 = texelFetch(uIn1, ia, 0);
    vec4 b1 = texelFetch(uIn1, ibb, 0);
    b0 = vec4(cmul(b0.xy, tw), cmul(b0.zw, tw));
    b1 = vec4(cmul(b1.xy, tw), cmul(b1.zw, tw));
    bool lower = (j % twoNs) < uNs;
    frag_data[0] = lower ? a0 + b0 : a0 - b0;
    frag_data[1] = lower ? a1 + b1 : a1 - b1;
}
