/**
 * @file wolfsurfcurlF.glsl
 * @brief WolfViewer: the barrel — shading of the curl ribbons (wolfsurfcurl.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/surf_curl.js SurfCurl._material fragment — the same terms in
// LINEAR light (the water pass writes linear; waterF.glsl's constants are linear too), so
// the sRGB tints of the web shader are converted here: glow (0.30, 0.84, 0.80) -> (0.07,
// 0.68, 0.60), sea tint (0.05, 0.42, 0.55) -> (0.004, 0.147, 0.262), sky (0.6, 0.75, 0.9)
// -> (0.32, 0.52, 0.79), foam (0.96, 0.98, 1.0) -> (0.91, 0.955, 1.0).

out vec4 frag_color;

in vec3 vWorld;
in vec3 vNormal;
in vec2 vUvM;
in float vProg;
in float vAlpha;

uniform vec3 curlWaterColor;   // the sea's fog colour, linear
uniform vec3 curlSunColor;     // the sun (or moon), linear
uniform vec3 lightDir;
uniform vec3 curlEye;
uniform sampler2D bumpMap;
uniform float time;

void main()
{
    if (vAlpha <= 0.01) discard;
    vec3 V = normalize(curlEye - vWorld);
    vec3 nm = texture(bumpMap, vec2(vUvM.x * 0.05, vUvM.y * 1.5 - time * 0.15)).xyz * 2.0 - 1.0;
    vec3 N = normalize(vNormal + nm * 0.35);
    bool back = !gl_FrontFacing;
    if (back) N = -N;
    vec3 L = normalize(lightDir);
    float ndl = max(dot(N, L), 0.0);
    float through = max(dot(-N, L), 0.0);
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    vec3 sea = mix(curlWaterColor, vec3(0.004, 0.147, 0.262), 0.5);
    vec3 body = sea * (0.7 + 0.5 * ndl) * (0.7 + 0.3 * curlSunColor);
    float thin = smoothstep(0.25, 0.95, vUvM.y);
    // <WolfViewer 2026-09-20> No translucent glow: the cyan sheet read as "this bright glow
    // thing" on painted surf (Paul), so the curl is the sea's own colour, foam and highlight
    // only. `thin`, `through` and `back` stay for the foam and the highlight below.
    vec3 color = body;
    color = mix(color, vec3(0.32, 0.52, 0.79), fresnel * 0.45);
    // <WolfViewer 2026-09-20> No lit white tube: the ribbon was 95 % near-pure white plus a
    // sun highlight, and on a 20 m breaker it read as a glowing beam along the crest (Paul:
    // "the wave glow is crazy too much"). Its foam is now the water surface's own foam rule
    // (waterF.glsl: a 0.90 white scaled by the light, mixed to at most 0.65), the highlight
    // is a glint, and the sheet stays part-transparent. Water.js surf_curl same.
    vec3 H = normalize(L + V);
    color += min(curlSunColor, vec3(1.0)) * pow(max(dot(N, H), 0.0), 120.0) * 0.15;
    float foam = smoothstep(0.85, 1.0, vUvM.y) * 0.6 + smoothstep(0.6, 1.0, vProg) * 0.5;
    foam = clamp(foam, 0.0, 0.65);
    float foamLight = clamp(dot(min(curlSunColor, vec3(1.0)), vec3(0.3333)) * (0.6 + 0.4 * ndl), 0.08, 1.0);
    color = mix(color, vec3(0.90, 0.94, 0.97) * foamLight, foam);
    frag_color = vec4(clamp(color, 0.0, 1.0), clamp(vAlpha * 0.8, 0.0, 1.0));
}
