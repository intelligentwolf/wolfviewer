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
//  - wolf_far_fade (0..1): the share of this draw's pixels that are kept, through a 4 x 4 ordered dither, for the
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

// The 4 x 4 Bayer index matrix (B. E. Bayer, "An optimum method for two-level rendition of continuous-tone
// pictures", 1973): entry (x, y) of M4 = [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]], thresholds (m + 0.5) / 16.
float wolfBayer4(ivec2 p)
{
    const float m[16] = float[16](0.0, 8.0, 2.0, 10.0,
                                  12.0, 4.0, 14.0, 6.0,
                                  3.0, 11.0, 1.0, 9.0,
                                  15.0, 7.0, 13.0, 5.0);
    return (m[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

void main()
{
    mirrorClip(vary_position);

    if (wolf_far_fade < wolfBayer4(ivec2(gl_FragCoord.xy)))
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
