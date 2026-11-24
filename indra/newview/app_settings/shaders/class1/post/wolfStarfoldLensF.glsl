/**
 * @file wolfStarfoldLensF.glsl
 * @brief WolfViewer: Starfold's wormhole - the light behind it bent round it, its turning ring, and the jump's tunnel.
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
// e.g. Schneider, Ehlers & Falco, "Gravitational Lenses" 1992, eq. 2.9). E (the Einstein radius) is uRadius here: the ring
// where the light from straight behind the centre lands. Inside 0.6 E the throat: a dark core with the other side's glimpse.

out vec4 frag_color;

uniform sampler2D diffuseRect;
in vec2 vary_fragcoord;

uniform vec2  uCentre;       // the wormhole's centre, 0..1 of the frame
uniform float uRadius;       // its Einstein radius, as a fraction of the frame's height
uniform float uAspect;       // width / height
uniform float uTime;         // seconds, for the ring's turn
uniform float uLens;         // 0..1: the wormhole's strength (0 = not drawn: only the tunnel)
uniform float uTunnel;       // 0..1: the jump's tunnel over the whole screen
uniform vec3  uRingColor;    // the ring (linear, display)
uniform vec3  uThroatColor;  // the glimpse of the other side

vec2 sfToUv(vec2 p)
{
    return vec2(p.x / uAspect, p.y) + uCentre;
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec3 col = texture(diffuseRect, uv).rgb;

    if (uLens > 0.0)
    {
        // the pixel's place round the centre, in frame heights (so the ring is round on any aspect)
        vec2 p = vec2((uv.x - uCentre.x) * uAspect, uv.y - uCentre.y);
        float r = length(p);
        float E = max(uRadius, 1e-5);
        float influence = 1.0 - smoothstep(2.5 * E, 4.0 * E, r);   // the bend dies away so the frame's edge never jumps
        if (influence > 0.0)
        {
            vec3 lensed;
            if (r > 0.6 * E)
            {
                // beta = theta - E^2 / theta: the source point this pixel sees
                vec2 dir = p / max(r, 1e-6);
                float beta = r - E * E / r;
                vec2 src = sfToUv(dir * beta);
                lensed = texture(diffuseRect, clamp(src, vec2(0.001), vec2(0.999))).rgb;
            }
            else
            {
                // the throat: a slow swirl of the other side's light round a dark centre
                float a = atan(p.y, p.x) + uTime * 0.4 + 3.0 / max(r / E, 0.05);
                float swirl = 0.5 + 0.5 * sin(a * 3.0);
                float depth = smoothstep(0.0, 0.6, r / E);
                lensed = uThroatColor * (0.25 + 0.75 * swirl) * depth;
            }
            col = mix(col, lensed, influence * clamp(uLens, 0.0, 1.0));

            // the ring: a thin cyan band at E with a brighter arc that turns once a minute or so
            float band = exp(-pow((r - E) / (0.06 * E), 2.0));
            float ang = atan(p.y, p.x);
            float arc = 0.55 + 0.45 * sin(ang * 2.0 - uTime * 0.12);
            col += uRingColor * band * arc * clamp(uLens, 0.0, 1.0);
        }
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
