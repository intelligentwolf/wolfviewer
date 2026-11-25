/**
 * @file wolfStarfoldLensF.glsl
 * @brief WolfViewer: WolfTrek's wormholes - the light behind them bent round them, their gas, sparks and ring, and the jump's tunnel.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

// [STARFOLD 2026-11-24] LLPipeline::wolfStarfoldLens (pipeline.cpp), after the glow is combined, the way wolfPhotoFilterF.glsl
// reads the finished frame (diffuseRect, post/exoPostBaseV.glsl's vary_fragcoord).
//
// Source: region-terrain-work/space-game-2026-11-24/design.html "Wormholes" ("A sphere that bends the starlight behind it (a
// screen-space lensing effect in the native renderer), with a slowly turning cyan ring, and, through the middle, a glimpse of
// the other region's sky ... The screen holds a short tunnel effect while that happens").
// The bending: a point mass deflects light by an angle proportional to 1 / b (b the impact parameter; Einstein 1936, "Lens-like
// action of a star by the deviation of light in the gravitational field", Science 84:506), so an image point at screen
// distance r from the centre shows what lies at r - E^2 / r (the thin-lens point-mass equation beta = theta - thetaE^2 / theta,
// e.g. Schneider, Ehlers & Falco, "Gravitational Lenses" 1992, eq. 2.9). E (the Einstein radius) is uLensA[i].z here: the ring
// where the light from straight behind the centre lands. Inside 0.6 E the throat: the other side's glimpse.

// [WOLFTREK v11 stage D 2026-11-25] design.html#wormholes2 "How they look": "A swirling lens: the stars behind it bend round the mouth
// (the real Einstein-ring look from Thorne and Double Negative's paper). A slow spiral of glowing gas circles into it, sparks drift
// inwards, and through the middle you glimpse the other side's sky, tinted by its seed. Colour shows its state, always in words too ...
// It visibly shrinks as it is used." Up to two mouths (the region's own and a found wandering wormhole); several point lenses in one
// plane deflect by the sum of their deflections (Schneider, Ehlers & Falco 1992, the multiple-lens equation in one lens plane), so
// each pixel's source point is the pixel minus every mouth's E^2 / r towards it. The gas, the sparks and the ring are added light; the
// throat is the far side. Closing (red) mouths pulse. All the looks are GAME TUNING.

out vec4 frag_color;

uniform sampler2D diffuseRect;
in vec2 vary_fragcoord;

uniform vec4  uLensA[2];     // per mouth: centre x, y (0..1 of the frame), Einstein radius (frame heights), strength (0 = none)
uniform vec4  uLensB[2];     // per mouth: the ring and gas colour (state: cyan / amber / red), closing (1 = pulse)
uniform vec4  uLensC[2];     // per mouth: the far side's tint, a seed for its sparks
uniform float uAspect;       // width / height
uniform float uTime;         // seconds
uniform float uTunnel;       // 0..1: the jump's tunnel over the whole screen
uniform vec3  uRingColor;    // the tunnel's streaks (linear, display)
uniform vec3  uThroatColor;  // the tunnel's middle

float sfHash(float n)
{
    return fract(sin(n) * 43758.5453);
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec2 q = vec2(uv.x * uAspect, uv.y);          // frame-height units, so a mouth is round on any aspect
    vec3 col;

    // 1. the bend: every mouth pulls the source point towards itself by E^2 / r (inside 0.6 E the throat hides it anyway)
    vec2 srcq = q;
    for (int i = 0; i < 2; ++i)
    {
        vec4 A = uLensA[i];
        if (A.w <= 0.0) continue;
        vec2 c = vec2(A.x * uAspect, A.y);
        float E = max(A.z, 1e-5);
        vec2 p = q - c;
        float r = length(p);
        float influence = (1.0 - smoothstep(2.5 * E, 4.0 * E, r)) * clamp(A.w, 0.0, 1.0);   // dies away: the frame's edge never jumps
        if (r > 1e-6) srcq -= (p / r) * (E * E / max(r, 0.6 * E)) * influence;
    }
    col = texture(diffuseRect, clamp(vec2(srcq.x / uAspect, srcq.y), vec2(0.001), vec2(0.999))).rgb;

    // 2. per mouth: the far side in the throat, the inspiralling gas, the sparks, the ring
    for (int i = 0; i < 2; ++i)
    {
        vec4 A = uLensA[i];
        if (A.w <= 0.0) continue;
        vec2 c = vec2(A.x * uAspect, A.y);
        float E = max(A.z, 1e-5);
        vec2 p = q - c;
        float r = length(p);
        if (r > 3.0 * E) continue;
        float s = clamp(A.w, 0.0, 1.0);
        vec3 glowCol = uLensB[i].rgb;
        float pulse = uLensB[i].a > 0.5 ? 0.55 + 0.45 * sin(uTime * 6.0) : 1.0;   // a closing mouth beats
        float ang = atan(p.y, p.x);
        float rn = r / E;

        // the throat: the other side's sky, tinted, slowly turning, with a few stars, darker towards its middle
        float a = ang + uTime * 0.25 + 2.0 / max(rn, 0.08);
        vec2 cell = floor(vec2(a * 6.0, rn * 14.0));
        float star = step(0.93, sfHash(cell.x * 17.0 + cell.y * 131.0 + uLensC[i].a)) * (0.6 + 0.4 * sin(uTime * 3.0 + cell.x));
        vec3 far = uLensC[i].rgb * (0.30 + 0.25 * sin(a * 3.0)) * smoothstep(0.0, 0.6, rn) + vec3(star) * 0.8;
        col = mix(col, far, (1.0 - smoothstep(0.48, 0.62, rn)) * s);

        // the gas: two arms whose lines of equal phase move inwards as time goes on (2 ang - 6 ln r - 1.2 t constant: ln r falls)
        float arms = pow(0.5 + 0.5 * sin(2.0 * ang - 6.0 * log(max(rn, 1e-3)) - uTime * 1.2), 6.0);
        float disc = smoothstep(0.6, 0.75, rn) * (1.0 - smoothstep(1.3, 2.2, rn));
        vec3 gas = mix(glowCol, vec3(1.0), 0.35 * (1.0 - smoothstep(0.7, 1.2, rn))) * (arms * 0.9 + 0.22) * disc;

        // the sparks: each falls from 2.4 E to 0.65 E, faster round as it nears, fading in and out
        float spark = 0.0;
        for (int k = 0; k < 16; ++k)
        {
            float sd = uLensC[i].a * 7.13 + float(k) * 3.71;
            float f = fract(sfHash(sd) * 7.0 + uTime * 0.18 * (0.6 + 0.8 * sfHash(sd + 1.0)));
            float rs = E * (2.4 - 1.75 * f);
            float as = sfHash(sd + 2.0) * 6.2832 + uTime * 0.9 / (rs / E);
            vec2 sp = c + rs * vec2(cos(as), sin(as));
            float dd = length(q - sp) / (0.035 * E);
            spark += exp(-dd * dd) * sin(3.14159 * f);
        }

        // the ring at E, a brighter arc turning slowly
        float band = exp(-pow((r - E) / (0.06 * E), 2.0)) * (0.55 + 0.45 * sin(ang * 2.0 - uTime * 0.12));

        col += (gas + glowCol * band + mix(glowCol, vec3(1.0), 0.5) * spark) * s * pulse;
    }

    if (uTunnel > 0.0)
    {
        // the jump: streaks rushing out from the middle, the frame pulled in and tinted
        vec2 p = vec2((uv.x - 0.5) * uAspect, uv.y - 0.5);
        float r = length(p);
        float ang = atan(p.y, p.x);
        float streak = pow(0.5 + 0.5 * sin(ang * 40.0 + 7.0 * sin(ang * 3.0)), 8.0) * smoothstep(0.05, 0.6, r);
        float rush = fract(r * 3.0 - uTime * 2.5);
        vec2 pulled = vec2(0.5) + (uv - vec2(0.5)) * (1.0 - 0.35 * uTunnel);
        vec3 inner = texture(diffuseRect, pulled).rgb * (1.0 - 0.7 * uTunnel);
        vec3 tunnel = inner + uRingColor * streak * rush * 1.5 + uThroatColor * smoothstep(0.7, 0.0, r) * 0.4;
        col = mix(col, tunnel, clamp(uTunnel, 0.0, 1.0));
    }

    frag_color = vec4(col, 1.0);
}
