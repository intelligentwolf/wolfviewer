/**
 * @file wolffftFinalF.glsl
 * @brief WolfViewer: spectral ocean, sign, unpack, Jacobian and foam accumulation (wolfoceanfft.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/ocean_fft.js OceanFFT._finMat fragment — the same maths.
// The spectrum was centred (k = (n - N/2) dk), which shifts the transform by (-1)^(x+y).
// J = (1 + dDx/dx)(1 + dDy/dy) - (dDx/dy)^2 is the Jacobian of the horizontal
// displacement — Tessendorf's whitecap criterion: the surface folds where J drops. Foam
// grows where J is below threshold and decays with a half-life (GodotOceanWaves), so
// whitecaps linger as streaks behind the crest that made them.
//   frag_data[0] = (dx, dy, dz, foam)   frag_data[1] = (dz/dx, dz/dy, J, 0)

out vec4 frag_data[2];

uniform sampler2D uIn0;
uniform sampler2D uIn1;
uniform sampler2D uPrev;     // last finished displacement texture (its alpha is the foam)
uniform float uLambda;       // Tessendorf choppiness
uniform float uFoamDecay;    // per-update multiplier, 0.5 ^ (dt / half life)
uniform float uFoamGain;
uniform float uFoamThreshold;
uniform float uDt;

void main()
{
    ivec2 id = ivec2(gl_FragCoord.xy);
    float sgn = ((id.x + id.y) & 1) == 0 ? 1.0 : -1.0;
    vec4 t0 = texelFetch(uIn0, id, 0) * sgn;
    vec4 t1 = texelFetch(uIn1, id, 0) * sgn;
    float dz = t0.x;
    float dx = t0.y * uLambda;
    float dy = t0.z * uLambda;
    float dzdx = t0.w;
    float dzdy = t1.x;
    float dxdx = t1.y * uLambda;
    float dydy = t1.z * uLambda;
    float dxdy = t1.w * uLambda;
    float J = (1.0 + dxdx) * (1.0 + dydy) - dxdy * dxdy;
    float prev = texelFetch(uPrev, id, 0).a;
    float gen = max(0.0, uFoamThreshold - J) * uFoamGain;
    float foam = clamp(prev * uFoamDecay + gen * uDt, 0.0, 1.0);
    frag_data[0] = vec4(dx, dy, dz, foam);
    frag_data[1] = vec4(dzdx, dzdy, J, 0.0);
}
