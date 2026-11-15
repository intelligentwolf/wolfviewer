/**
 * @file wolfFarProxyF.glsl
 * @brief [W107 FAR HLOD 2026-11-14] WolfViewer: the G-buffer fragment shader of a region's far proxy model
 * (wolffarproxy.cpp): the atlas colour, an alpha cut, an emissive weight and the dither mask of the hand-over band.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer - Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// Source: deferred/diffuseAlphaMaskF.glsl - the same outputs (diffuse, no specular, the encoded normal with
// GBUFFER_FLAG_HAS_ATMOS), paired with the same deferred/diffuseV.glsl. Three differences:
//  - the colour is the atlas texel alone (the baker multiplied the face colour in), not texel x vertex colour;
//  - the vertex colour's red is the emissive weight and goes to the diffuse output's alpha, which
//    class3/deferred/softenLightF.glsl:302 reads as "the legacy fullbright mask: the texture's own colour"
//    (color.rgb = mix(lit, baseColor.rgb x fullbright luminance, baseColor.a)), so a fullbright or glowing face of
//    the real object stays lit at night in its proxy;
//  - wolf_far_fade (0..1): the share of this draw's pixels that are kept, through a per-pixel noise, for the
//    band where the proxy dissolves in over the real objects.

out vec4 frag_data[4];

uniform float minimum_alpha;
uniform float wolf_far_fade;

uniform sampler2D diffuseMap;

in vec3 vary_position;

in vec3 vary_normal;
in vec4 vertex_color;
in vec2 vary_texcoord0;

void mirrorClip(vec3 pos);

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

// Interleaved gradient noise (J. Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare",
// SIGGRAPH 2014): fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715)))) - a value in [0, 1) per pixel
// with no visible grid, unlike an ordered (Bayer) matrix. It is only on screen for the 0.75 s of a cell's dissolve.
float wolfDissolveNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
    mirrorClip(vary_position);

    if (wolf_far_fade < 1.0 && wolf_far_fade <= wolfDissolveNoise(floor(gl_FragCoord.xy)))
    {
        discard;
    }

    vec4 col = texture(diffuseMap, vary_texcoord0.xy);

    if (col.a < minimum_alpha)
    {
        discard;
    }

    frag_data[0] = vec4(col.rgb, clamp(vertex_color.r, 0.0, 1.0));
    frag_data[1] = vec4(0,0,0,0); // spec
    vec3 nvn = normalize(vary_normal);
    frag_data[2] = encodeNormal(nvn.xyz, 0, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}
