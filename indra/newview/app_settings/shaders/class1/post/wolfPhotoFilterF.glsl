/**
 * @file wolfPhotoFilterF.glsl
 * @brief WolfViewer: World > Photo Effects colour grade, post pass (pipeline.cpp wolfPhotoFilter).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/rendering/photo_filter.js WolfPhotoFilter.GLSL — wolfPhotoGrade() is
// the same function, constant for constant; change one, change both, or the two viewers stop
// agreeing on what "Vintage" looks like. The frame here is display-referred (after tonemap
// and gamma correct), which is what WolfStorm's canvas frame is too. uMode is the
// WolfViewerPhotoFilter setting: 1 Sepia, 2 Black & White, 3 Noir, 4 Vintage, 5 Warm,
// 6 Cool, 7 Faded, 8 Vivid (0 = off; the CPU never runs this pass for it).

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform int uMode;
uniform float uStrength;
in vec2 vary_fragcoord;

vec3 wolfPhotoGrade(vec3 c, vec2 uv, int mode)
{
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float v = length(uv - 0.5) * 1.41421356;   // 0 at the centre, 1 in the corners
    if (mode == 1) {
        return vec3(dot(c, vec3(0.393, 0.769, 0.189)),
                    dot(c, vec3(0.349, 0.686, 0.168)),
                    dot(c, vec3(0.272, 0.534, 0.131)));
    }
    if (mode == 2) { return vec3(l); }
    if (mode == 3) {
        float n = clamp((l - 0.5) * 1.7 + 0.5, 0.0, 1.0);
        n = n * n * (3.0 - 2.0 * n);
        return vec3(n) * (1.0 - 0.55 * smoothstep(0.45, 1.0, v));
    }
    if (mode == 4) {
        vec3 f = mix(vec3(l), c, 0.6) * 0.82 + 0.10;
        f *= vec3(1.08, 1.0, 0.84);
        return f * (1.0 - 0.4 * smoothstep(0.5, 1.0, v));
    }
    if (mode == 5) { return c * vec3(1.10, 1.0, 0.86); }
    if (mode == 6) { return c * vec3(0.88, 1.0, 1.12); }
    if (mode == 7) { return mix(vec3(l), c, 0.7) * 0.78 + 0.14; }
    if (mode == 8) { return (mix(vec3(l), c, 1.45) - 0.5) * 1.08 + 0.5; }
    return c;
}

void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);
    diff.rgb = mix(diff.rgb, clamp(wolfPhotoGrade(diff.rgb, vary_fragcoord.xy, uMode), 0.0, 1.0), uStrength);
    frag_color = diff;
}
