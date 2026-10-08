/**
 * @file wolfCloudDeckF.glsl
 * @brief WolfViewer: a cloud layer to fly through in bad weather (LLPipeline::wolfCloudDeck).
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

// Paul, 2026-10-07: "if the weather is bad can we put more cloud to fly through? when up high".
// The way flight simulators draw a cloud layer: a slab of cloud between two heights, its shape from
// noise, marched along each pixel's view ray up to whatever the scene has in front (the depth
// buffer), its light the sun and sky's. From below it is the overcast, from above a sea of cloud with
// the sun on it, and inside it the world goes white until you come out of it. Composited into the lit
// HDR frame (premultiplied: rgb = light scattered toward the eye, a = 1 - transmittance) before the
// tone mapping, so exposure treats it like everything else.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D depthMap;

uniform vec3  uCamPos;        // camera, agent metres (z up)
uniform vec3  uCamAt;         // camera basis (LLCoordFrame): forward
uniform vec3  uCamLeft;
uniform vec3  uCamUp;
uniform float uTanHalf;       // tan(vertical FOV / 2)
uniform float uAspect;        // width / height
uniform float uNear;
uniform float uFar;
uniform vec2  uOffset;        // the agent frame's global origin (metres), so the clouds stay put across region crossings
uniform float uBase;          // the layer's bottom and top, agent metres
uniform float uTop;
uniform float uCover;         // 0..1: how much of the sky the layer covers
uniform float uDensity;       // extinction per metre in the thick of it
uniform float uFade;          // 0..1: the whole layer, faded in with the camera's height
uniform vec2  uWind;          // drift, metres (wind x time)
uniform vec3  uSunDir;        // to the sun (or moon), world z up
uniform vec3  uSunColor;      // linear light on a surface facing it
uniform vec3  uAmbient;       // linear sky light
uniform float uMaxDist;       // nothing beyond this is marched (the far clip, plus some)

const int STEPS = 28;

float wolfHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float wolfNoise2(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wolfHash12(i), wolfHash12(i + vec2(1.0, 0.0)), f.x),
               mix(wolfHash12(i + vec2(0.0, 1.0)), wolfHash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Cloud shapes: big masses (2.4 km), their lumps (600 m) and their ragged edges (150 m).
float wolfFbm(vec2 p)
{
    float v = wolfNoise2(p / 2400.0) * 0.55;
    v += wolfNoise2(p / 600.0 + 17.3) * 0.30;
    v += wolfNoise2(p / 150.0 + 41.7) * 0.15;
    return v;
}

// Density at a point in agent space (per metre): the coverage cut from the shape, then rounded off
// toward the base and lumpy at the top (the tops rise where the cloud is thick).
float wolfCloudDensity(vec3 p)
{
    vec2 q = p.xy + uOffset + uWind;
    float shape = wolfFbm(q);
    float cut = 1.0 - uCover;
    float d = smoothstep(cut, cut + 0.18, shape);
    if (d <= 0.0)
    {
        return 0.0;
    }
    float h = (p.z - uBase) / max(uTop - uBase, 1.0);
    float top = 0.55 + 0.45 * d;                      // thicker cloud stands taller
    float profile = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(top - 0.25, top, h));
    return d * profile * uDensity;
}

void main()
{
    // The view ray through this pixel, and how far it gets before the scene stops it.
    vec2 ndc = vary_fragcoord * 2.0 - 1.0;
    vec3 dir = normalize(uCamAt + (-uCamLeft) * (ndc.x * uTanHalf * uAspect) + uCamUp * (ndc.y * uTanHalf));
    float z = texture(depthMap, vary_fragcoord).r;
    float scene = uMaxDist;
    if (z < 1.0)
    {
        float zn = z * 2.0 - 1.0;
        float eye_z = 2.0 * uNear * uFar / (uFar + uNear - zn * (uFar - uNear));
        scene = min(eye_z / max(dot(dir, uCamAt), 0.001), uMaxDist);
    }

    // The stretch of the ray inside the slab.
    float t0, t1;
    if (abs(dir.z) < 1e-5)
    {
        if (uCamPos.z < uBase || uCamPos.z > uTop) discard;
        t0 = 0.0; t1 = scene;
    }
    else
    {
        float ta = (uBase - uCamPos.z) / dir.z;
        float tb = (uTop - uCamPos.z) / dir.z;
        t0 = max(min(ta, tb), 0.0);
        t1 = min(max(ta, tb), scene);
    }
    if (t1 <= t0)
    {
        discard;
    }

    // March it: steps closer together near the eye, spread out far away, jittered against banding.
    float jitter = wolfHash12(gl_FragCoord.xy + fract(uWind * 0.013));
    float trans = 1.0;
    vec3 light = vec3(0.0);
    float cos_sun = dot(dir, uSunDir);
    float forward = 0.6 + 0.4 * pow(max(cos_sun, 0.0), 6.0) * 2.0;   // brighter looking toward the sun
    float prev = t0;
    for (int i = 1; i <= STEPS; ++i)
    {
        float f = (float(i) - 1.0 + jitter) / float(STEPS);
        float t = t0 + (t1 - t0) * f * f;
        float dt = t - prev;
        prev = t;
        vec3 p = uCamPos + dir * t;
        float sigma = wolfCloudDensity(p);
        if (sigma <= 0.0) continue;
        float h = clamp((p.z - uBase) / max(uTop - uBase, 1.0), 0.0, 1.0);
        // lit from above: the tops in the sun, the bases in the cloud's own shadow, the sky all round
        vec3 c = uSunColor * max(uSunDir.z, 0.0) * mix(0.25, 1.0, h) * forward + uAmbient * mix(0.55, 0.9, h);
        float a = 1.0 - exp(-sigma * dt);
        light += trans * a * c;
        trans *= 1.0 - a;
        if (trans < 0.01) break;
    }
    float alpha = (1.0 - trans) * uFade;
    // far cloud thins into the haze rather than ending
    float far_fade = 1.0 - smoothstep(uMaxDist * 0.6, uMaxDist, t0);
    alpha *= far_fade;
    frag_color = vec4(light * uFade * far_fade, alpha);
}
