/**
 * @file wolfStarfoldSkyF.glsl
 * @brief WolfViewer: Starfold's planet in the sky pass - a true sphere, ray against sphere, lit by the sun.
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

// [STARFOLD 2026-11-24] Attached to the three sky programs (llviewershadermgr.cpp: gWolfEnvSkyProgram, gDeferredWLSkyProgram,
// gEnvironmentMapProgram), as wolfAuroraF.glsl is; the uniforms come from WolfStarfold::bindSkyUniforms (wolfstarfold.cpp).
// An unset uniform reads 0, so sf_planet_on 0 (another region, another grid, below the line) draws nothing.
//
// Source: region-terrain-work/space-game-2026-11-24/design.html "The native space sky" ("Planets and moons are true spheres
// drawn in the sky pass: a ray against a sphere at a great distance, a procedural surface (continents and seas, or gas bands),
// lit by the real sun direction so they show proper phases, with a thin atmosphere rim") and the README's "client look rules"
// (one planet per system from the seed). Every direction here is the world frame: east, north, up.
//
// The sphere: its centre 1 unit away along sf_planet_dir, its radius sf_planet_sin (the sine of its angular radius), so its
// outline subtends exactly that angle. A view ray v from the eye meets it where |t v - D|^2 = s^2, i.e.
// t^2 - 2 t (v.D) + (1 - s^2) = 0 (the standard ray-sphere quadratic, e.g. Shirley & Marschner, Fundamentals of Computer
// Graphics 3rd ed. s4.4.1), nearest root t = v.D - sqrt((v.D)^2 - 1 + s^2).
// The surface: value noise (a hashed lattice smoothed with 3t^2 - 2t^3, as wolftk/skyF.glsl wolfNoise3) summed in octaves
// (fractional Brownian motion; Musgrave, "Texturing and Modeling: A Procedural Approach" 3rd ed. ch. 16). Lambert's cosine law
// for the light, so the lit part is the phase the sun gives. The rim: light scattered in a thin shell of air, strongest where
// the line of sight grazes the limb, approximated by (1 - mu)^3 on the sunward side (mu = the cosine between the normal and
// the view): a stand-in for the in-scatter of the full Bruneton model, which only the home sky's air uses.

uniform float sf_planet_on;       // 0 none, else the fade (0..1): the line's 300 m edge (README "The air")
uniform vec3  sf_planet_dir;      // to the planet's centre, world, unit
uniform float sf_planet_sin;      // sin(angular radius)
uniform vec3  sf_planet_col1;     // rocky: the sea; gas: the light bands (linear)
uniform vec3  sf_planet_col2;     // rocky: the land; gas: the dark bands (linear)
uniform float sf_planet_gas;      // 0 rocky, 1 gas giant
uniform float sf_planet_rings;    // 1: a ring system (gas giants only)
uniform vec3  sf_planet_rim;      // the air's rim colour (linear)
uniform vec3  sf_sun_dir;         // to the sun, world, unit
uniform float sf_planet_lum;      // the sky's units: what a white surface square to the sun reads

float sfHash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float sfNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(sfHash(i), sfHash(i + vec3(1.0, 0.0, 0.0)), f.x), mix(sfHash(i + vec3(0.0, 1.0, 0.0)), sfHash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y);
    float b = mix(mix(sfHash(i + vec3(0.0, 0.0, 1.0)), sfHash(i + vec3(1.0, 0.0, 1.0)), f.x), mix(sfHash(i + vec3(0.0, 1.0, 1.0)), sfHash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y);
    return mix(a, b, f.z);
}

float sfFbm(vec3 p)
{
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++)
    {
        s += a * sfNoise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s;
}

// The planet's own frame: its pole along (0.3, 0.1, 1) in the world, the same fixed axis WolfStarfold's WolfStorm side uses
// (wolfstorm js/world/wolf_starfold_draw.js), so the two viewers show the same bands and caps.
void sfFrame(out vec3 east, out vec3 north, out vec3 pole)
{
    pole = normalize(vec3(0.3, 0.1, 1.0));
    vec3 e = cross(pole, sf_planet_dir);
    if (dot(e, e) < 1e-6) e = vec3(1.0, 0.0, 0.0);
    east = normalize(e);
    north = cross(sf_planet_dir, east);
}

// Where a view ray meets the sphere: the distance along the ray, or -1.
float sfHit(vec3 v)
{
    float b = dot(v, sf_planet_dir);
    float s = sf_planet_sin;
    float disc = b * b - 1.0 + s * s;
    if (b <= 0.0 || disc <= 0.0) return -1.0;
    return b - sqrt(disc);
}

// The ring plane's hit: (shade, alpha). The rings lie in the planet's equator (normal = its pole), 1.45 to 2.25 radii.
vec2 sfRing(vec3 v, vec3 pole, float tPlanet)
{
    float dn = dot(v, pole);
    if (abs(dn) < 1e-5) return vec2(0.0);
    float t = dot(sf_planet_dir, pole) / dn;
    if (t <= 0.0) return vec2(0.0);
    if (tPlanet > 0.0 && tPlanet < t) return vec2(0.0);   // the planet is in front of this part of the ring
    vec3 p = v * t - sf_planet_dir;
    float r = length(p) / sf_planet_sin;
    if (r < 1.45 || r > 2.25) return vec2(0.0);
    float bands = 0.55 + 0.45 * sin(r * 23.0) * sin(r * 7.0 + 1.3);
    float edge = smoothstep(1.45, 1.52, r) * (1.0 - smoothstep(2.15, 2.25, r));
    // the planet's shadow on the ring: the point's line to the sun passes through the sphere
    float b = dot(p, sf_sun_dir);
    float c = dot(p, p) - sf_planet_sin * sf_planet_sin;
    float shadow = (b < 0.0 && b * b - c > 0.0) ? 0.15 : 1.0;
    return vec2(bands * shadow, edge * 0.75);
}

// The planet in direction `dir` (world, unit): rgb in the sky's units premultiplied by a, a = how much of the pixel it covers.
vec4 wolfStarfoldPlanet(vec3 dir)
{
    if (sf_planet_on <= 0.0 || sf_planet_sin <= 0.0) return vec4(0.0);
    float t = sfHit(dir);
    vec3 east;
    vec3 north;
    vec3 pole;
    sfFrame(east, north, pole);
    vec4 outc = vec4(0.0);
    if (t > 0.0)
    {
        vec3 n = (dir * t - sf_planet_dir) / sf_planet_sin;     // the unit normal
        vec3 q = vec3(dot(n, east), dot(n, north), dot(n, pole));   // the planet's own frame
        float lat = q.z;
        vec3 albedo;
        if (sf_planet_gas > 0.5)
        {
            float warp = sfFbm(q * 3.0) * 0.6;
            float band = 0.5 + 0.5 * sin(lat * 18.0 + warp * 6.0);
            albedo = mix(sf_planet_col2, sf_planet_col1, band);
        }
        else
        {
            float h = sfFbm(q * 2.2 + vec3(4.0, 1.0, 7.0));
            float land = smoothstep(0.50, 0.53, h);
            albedo = mix(sf_planet_col1, sf_planet_col2 * (0.7 + 0.6 * sfNoise(q * 9.0)), land);
            float ice = smoothstep(0.78, 0.86, abs(lat) + 0.08 * sfNoise(q * 6.0));
            albedo = mix(albedo, vec3(0.85, 0.87, 0.9), ice);
            float cloud = smoothstep(0.55, 0.75, sfFbm(q * 4.0 + vec3(11.0, 3.0, 5.0)));
            albedo = mix(albedo, vec3(0.9), cloud * 0.6);
        }
        float lit = max(dot(n, sf_sun_dir), 0.0);
        float mu = max(dot(n, -dir), 0.0);
        float rim = pow(1.0 - mu, 3.0) * smoothstep(-0.25, 0.3, dot(n, sf_sun_dir));
        // the outline: a pixel-wide soft edge (the derivative of the root's distance to the limb)
        float edgeDist = sf_planet_sin * sf_planet_sin - (1.0 - dot(dir, sf_planet_dir) * dot(dir, sf_planet_dir));
        float aa = clamp(edgeDist / max(fwidth(edgeDist), 1e-7), 0.0, 1.0);
        vec3 c = albedo * lit + sf_planet_rim * rim * 0.8;
        outc = vec4(c * sf_planet_lum, aa);
    }
    if (sf_planet_rings > 0.5 && sf_planet_gas > 0.5)
    {
        vec2 ring = sfRing(dir, pole, t);
        if (ring.y > 0.0)
        {
            vec3 rc = mix(sf_planet_col1, vec3(0.8, 0.75, 0.65), 0.5) * ring.x * sf_planet_lum;
            outc.rgb = mix(outc.rgb, rc, ring.y * (1.0 - outc.a));
            outc.a = max(outc.a, ring.y);
        }
    }
    outc.a *= clamp(sf_planet_on, 0.0, 1.0);
    outc.rgb *= outc.a;     // premultiplied: the caller keeps (1 - a) of what is behind and adds rgb
    return outc;
}

// The planet's disc (not its rings) covers `dir`: the sky's depth pass hides the stars behind it (wolftk/skyF.glsl
// wolfBodyCovers).
bool wolfStarfoldCovers(vec3 dir)
{
    return sf_planet_on > 0.5 && sfHit(dir) > 0.0;
}
