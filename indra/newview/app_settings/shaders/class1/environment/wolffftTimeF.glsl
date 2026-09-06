/**
 * @file wolffftTimeF.glsl
 * @brief WolfViewer: spectral ocean, time evolution of the JONSWAP spectrum (wolfoceanfft.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/ocean_fft.js OceanFFT._timeMat fragment — the same maths,
// kept in step. h(k,t) = h0 e^{iwt} + conj(h0(-k)) e^{-iwt}, w = sqrt(g|k|) (deep water
// dispersion, Tessendorf 2001), and from it the eight real fields the water needs — height,
// two horizontal (choppy) displacements and five partial derivatives — packed two per
// complex number into two RGBA32F targets, so four inverse FFTs give all eight.

out vec4 frag_data[2];

uniform sampler2D uH0;      // (h0.re, h0.im, conj(h0(-k)).re, conj(h0(-k)).im)
uniform float uTime;
uniform float uN;
uniform float uL;

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
// C = A + iB for two REAL-valued fields A, B: after the inverse FFT the real part is A's
// field and the imaginary part is B's.
vec2 pack2(vec2 A, vec2 B) { return vec2(A.x - B.y, A.y + B.x); }

void main()
{
    ivec2 id = ivec2(gl_FragCoord.xy);
    vec4 h0 = texelFetch(uH0, id, 0);
    vec2 k = (vec2(id) - uN * 0.5) * (6.28318530718 / uL);
    float klen = length(k);
    float kl = max(klen, 1e-4);
    float w = sqrt(9.81 * klen);
    float c = cos(w * uTime), s = sin(w * uTime);
    vec2 h = cmul(h0.xy, vec2(c, s)) + cmul(h0.zw, vec2(c, -s));
    vec2 ih = vec2(-h.y, h.x);
    vec2 dz = h;
    vec2 dx = ih * (k.x / kl);
    vec2 dy = ih * (k.y / kl);
    vec2 dzdx = ih * k.x;
    vec2 dzdy = ih * k.y;
    vec2 dxdx = -h * (k.x * k.x / kl);
    vec2 dydy = -h * (k.y * k.y / kl);
    vec2 dxdy = -h * (k.x * k.y / kl);
    frag_data[0] = vec4(pack2(dz, dx), pack2(dy, dzdx));
    frag_data[1] = vec4(pack2(dzdy, dxdx), pack2(dydy, dxdy));
}
