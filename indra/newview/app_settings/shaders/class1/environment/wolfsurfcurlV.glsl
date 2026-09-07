/**
 * @file wolfsurfcurlV.glsl
 * @brief WolfViewer: the barrel — curl ribbons along the break line (wolfsurfcurl.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/surf_curl.js SurfCurl._material vertex — the same maths, kept
// in step with waterV.glsl [SURF] (crest at ph = 0.95, lean 0.9 crestH, top 1.11 crestH).
//
// position  = ribbon base on the break contour (region metres, z unused)
// texcoord0 = (s along the line in metres, u across the profile 0..1)
// texcoord1 = direction of travel (toward land)
// texcoord2 = (distance to land at the base, 0)

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;
in vec2 texcoord1;
in vec2 texcoord2;

uniform float time;
uniform float surfHeight;
uniform float surfSetInterval;
uniform float surfLength;
uniform float surfSpeed;
uniform float waterLevel;
uniform float hBreak;
uniform vec2  wolfRegionOrigin;

out vec3 vWorld;
out vec3 vNormal;
out vec2 vUvM;
out float vProg;
out float vAlpha;

void main()
{
    vec2 base = position.xy;
    vec2 dir = texcoord1;
    float u = texcoord0.y;
    float aDist = texcoord2.x;

    // ── the sea's own surf train at the break line (waterV.glsl [SURF], same numbers) ──
    const float g = 9.81;
    float lambda = max(surfLength, 12.0 * surfHeight);
    float k = 6.2831853 / max(lambda, 8.0);
    float h = hBreak;
    float kh = k * max(h, 0.05);
    float tk = tanh(kh);
    float omega = sqrt(g * k * tk);
    float kl = k * inversesqrt(max(tk, 0.05));
    float coord = -aDist;
    float setPh = 6.2831853 * (time * surfSpeed / max(surfSetInterval, 10.0)) - coord * (0.22 / lambda);
    float setEnv = 0.30 + 0.70 * smoothstep(0.15, 1.0, 0.5 + 0.5 * sin(setPh));
    vec2 across = vec2(-dir.y, dir.x);
    float crestVar = 0.85 + 0.15 * sin(dot(base, across) * (1.1 / lambda) + time * 0.1);
    float ksh = clamp(inversesqrt(max(tk, 0.05)), 1.0, 1.8);
    float crestH = min(surfHeight * setEnv * ksh * crestVar, surfHeight * 1.15);
    float Hmax = 0.78 * (h + 0.8 * surfHeight);
    crestH = min(crestH, Hmax);
    crestH *= smoothstep(0.2, 0.6 + 0.5 * surfHeight, h);

    // ── how far past the break line is the last crest? (the sea's crest sits at ph = 0.95) ──
    float ph = kl * coord - omega * time * surfSpeed;
    float past = mod(0.95 - ph, 6.2831853);
    float shift = past / kl;
    float peel = 0.25 * lambda * (0.5 + 0.5 * sin(texcoord0.x * 0.02 + time * 0.05));
    float travel = 0.45 * lambda;
    float p = (shift - peel) / travel;
    float live = step(0.0, p) * step(p, 1.0);
    p = clamp(p, 0.0, 1.0);

    // ── the curl ──
    float Hc = crestH * 1.06;   // rev3 crest top: prof 1.25 - 0.35 lip 0.55
    float open = smoothstep(0.0, 0.55, p);
    float spent = smoothstep(0.7, 1.0, p);
    float R = 0.35 * Hc;
    float thetaMax = 1.35 * 3.14159265 * open;
    float theta = u * thetaMax;
    float throwF = 0.5 * Hc * open;
    float lean = 0.9 * crestH;
    vec2 top = base + dir * (shift + lean);
    vec2 xy = top + dir * (throwF * u + R * sin(theta));
    float z = waterLevel + Hc * (1.04 - 0.5 * spent) + R * (cos(theta) - 1.0) - 0.6 * Hc * spent * u;
    float dTheta = thetaMax;
    vec2 dxy_du = dir * (throwF + R * cos(theta) * dTheta);
    float dz_du = -R * sin(theta) * dTheta - 0.6 * Hc * spent;
    vec3 tU = vec3(dxy_du, dz_du);
    vec3 tS = vec3(across, 0.0);
    vec3 n = normalize(cross(tS, tU));
    if (n.z < 0.0 && u < 0.5) n = -n;
    vec3 pos = vec3(xy + wolfRegionOrigin, z);
    if (live < 0.5) pos = vec3(base + wolfRegionOrigin, waterLevel - 5.0);   // parked under the sea

    vWorld = pos;
    vNormal = n;
    vUvM = vec2(texcoord0.x, u);
    vProg = p;
    vAlpha = live * (1.0 - spent * 0.85);
    gl_Position = modelview_projection_matrix * vec4(pos, 1.0);
}
