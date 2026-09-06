/**
 * @file wolfGodRaysF.glsl
 * @brief WolfViewer: underwater sunlight shafts, additive post pass (pipeline.cpp wolfGodRays).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/rendering/underwater_rays.js UnderwaterRays fragment — the same
// maths. Screen-space crepuscular rays (Papadopoulos & Papaioannou, GraphiCon 2009): from
// each pixel march toward the sun's screen position accumulating a radial "aperture"
// pattern with a decaying weight. The aperture is the surface above — the chop cascade's
// slope texture where the spectral ocean is running, a slow angular noise otherwise.
// Drawn ADDITIVELY over the tonemapped frame; the CPU has already scaled uStrength by
// the sun's elevation and the camera's depth, and zeroed it at night.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform vec2 uSun;
uniform float uStrength;
uniform float uTime;
uniform float uAspect;
uniform vec3 uColor;
uniform sampler2D wolfCausticTex1;
uniform float uCausticOn;
uniform vec2 uCamXY;
uniform float uCausticTile;

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float vnoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), f.x),
               mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), f.x), f.y);
}

float aperture(vec2 p)
{
    vec2 d = vec2((p.x - uSun.x) * uAspect, p.y - uSun.y);
    float ang = atan(d.y, d.x);
    float n = vnoise(vec2(ang * 5.5 + uTime * 0.05, uTime * 0.12));
    n = mix(n, vnoise(vec2(ang * 13.0 - uTime * 0.09, 3.7 + uTime * 0.2)), 0.35);
    float a = smoothstep(0.42, 0.85, n);
    if (uCausticOn > 0.5)
    {
        vec2 wp = uCamXY + normalize(d + vec2(1e-4)) * (2.0 + length(d) * 24.0);
        vec2 sl = texture(wolfCausticTex1, wp / uCausticTile).xy;
        a *= 0.55 + 0.9 * smoothstep(0.0, 0.25, length(sl));
    }
    return a;
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec2 d = uSun - uv;
    float distS = length(vec2(d.x * uAspect, d.y));
    const int STEPS = 24;
    vec2 stp = d / float(STEPS) * 0.92;
    vec2 p = uv;
    float acc = 0.0, w = 1.0, wsum = 0.0;
    for (int i = 0; i < STEPS; i++)
    {
        p += stp;
        acc += aperture(p) * w;
        wsum += w;
        w *= 0.94;
    }
    acc /= wsum;
    float radial = exp(-distS * 1.6);
    float disc = exp(-distS * distS * 60.0) * 0.6;
    float light = (acc * radial + disc) * uStrength;
    frag_color = vec4(uColor * light, 1.0);
}
