/**
 * @file wolftk/starfoldPlanet.glsl
 * [STARFOLD 2026-11-24, polish] The world far below, seen from above a Starfold space line, as a lit planet surface: the sky's sea
 * (wolftk/skyF.glsl) and the world's water (class3/environment/waterF.glsl) both shade it from here, so the two meet.
 *
 * The rig's shots read the world below DARK: the sea mirrored the black sky over it. README "client look rules": WolfStorm "draws a
 * far blue planet surface with cloud swirls" (wolf_starfold_draw.js, render_manager.js createSky wolfSurface); the same look here:
 * the Glass Arc mock-up's sea (#1d3b66, linear) under domain-warped cloud swirls, lit by the light on the surface (the caller's
 * irradiance / pi, so it follows day and night and the real sun). Used only where sf_space_on > 0 (above the line's 300 m edge,
 * never in a reflection-probe face). GAME TUNING:
 *   SF_SEA_ALBEDO    the mock-up's sea, linear (WolfStorm's wolfGround at the limb)
 *   SF_CLOUD_ALBEDO  a cloud deck's (a little grey: thin cloud lets the sea through)
 *   SF_CLOUD_KM      the swirls' size; SF_CLOUD_COVER the share of the sea under cloud
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

const vec3 SF_SEA_ALBEDO = vec3(0.012, 0.043, 0.133);
const vec3 SF_CLOUD_ALBEDO = vec3(0.62, 0.64, 0.68);
const float SF_CLOUD_KM = 40.0;
const float SF_CLOUD_COVER = 0.85;

// a value noise (the same hash as skyF.glsl's wolfHash13 / wolfNoise3, its own names so either program may include this)
float sfHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float sfNoise3(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(sfHash13(i), sfHash13(i + vec3(1.0, 0.0, 0.0)), f.x), mix(sfHash13(i + vec3(0.0, 1.0, 0.0)), sfHash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y);
    float b = mix(mix(sfHash13(i + vec3(0.0, 0.0, 1.0)), sfHash13(i + vec3(1.0, 0.0, 1.0)), f.x), mix(sfHash13(i + vec3(0.0, 1.0, 1.0)), sfHash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y);
    return mix(a, b, f.z);
}

// the cloud cover (0..SF_CLOUD_COVER) at a point given in km
float wolfStarfoldCloud(vec3 km)
{
    vec3 p = km / SF_CLOUD_KM;
    vec3 warp = vec3(sfNoise3(p * 0.5 + vec3(3.1)), sfNoise3(p * 0.5 + vec3(7.7)), sfNoise3(p * 0.5 + vec3(11.3))) - vec3(0.5);
    p += warp * 2.5;
    float f = 0.5 * sfNoise3(p) + 0.25 * sfNoise3(p * 2.03) + 0.125 * sfNoise3(p * 4.07) + 0.0625 * sfNoise3(p * 8.11);
    return smoothstep(0.50, 0.72, f) * SF_CLOUD_COVER;
}

// the surface's radiance: its albedo under that cloud x the light on it / pi (the caller's units)
vec3 wolfStarfoldSurface(float cloud, vec3 lightOverPi)
{
    return mix(SF_SEA_ALBEDO, SF_CLOUD_ALBEDO, cloud) * lightOverPi;
}
