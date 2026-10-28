/**
 * @file starsF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

in vec4 vertex_color;
in vec2 vary_texcoord0;
in vec2 screenpos;

uniform vec3 wolf_star_tint;   // [WOLF ENV 2026-10-12/]
// [WolfViewer 2026-10-28 Captain Wolf] 1: the native Wolf sky is drawing the real catalogue (LLVOWLSky::drawStars), whose
// vertex alpha carries each star's V magnitude as (m - WOLF_STAR_MAG_MIN) / WOLF_STAR_MAG_SPAN (llvowlsky.cpp
// wolf_star_mag_encode). 0 (unset, the stock sky, the random star field): every star exactly as before.
uniform float wolf_star_phys;
uniform sampler2D diffuseMap;
uniform float blend_factor;
uniform float custom_alpha;
uniform float time;

float twinkle(){
    float d = fract(screenpos.x + screenpos.y);
    return abs(d);
}

// See:
// ALM off: class1/environment/starsF.glsl
// ALM on : class1/deferred/starsF.glsl
void main()
{
    // camera above water: class1\deferred\starsF.glsl
    // camera below water: class1\environment\starsF.glsl
    // <WolfViewer 2026-10-03> World > Show Astronomy: constellation lines share this pass (and its
    // depth test against the scene) but are flagged with texcoords > 1.5 (llvowlsky.cpp
    // updateConstellations): their own steady colour, faded in with the stars, no twinkle.
    if (vary_texcoord0.x > 1.5)
    {
        // Full strength as soon as the stars are properly out (custom_alpha 0.25), not only at
        // the darkest sky: a half moon alone keeps custom_alpha near 0.35.
        vec4 lc = vec4(vertex_color.rgb, vertex_color.a * smoothstep(0.0f, 0.25f, custom_alpha));
        frag_data[1] = vec4(0.0f);
        frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
        frag_data[0] = vec4(0);
        frag_data[3] = lc;
#else
        frag_data[0] = lc;
#endif
        return;
    }
    vec4 col_a = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col_b = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col = mix(col_b, col_a, blend_factor);
    col.rgb *= vertex_color.rgb;
    col.rgb *= wolf_star_tint;   // [WOLF ENV 2026-10-12/] the Wolf sky's star colour; (1,1,1) otherwise (LLDrawPoolWLSky::renderStarsDeferred)
    if (wolf_star_phys > 0.5)
    {
        // [WolfViewer 2026-10-28 Captain Wolf] each star's own light. wolf_star_tint is the luminance of a magnitude -1 star
        // spread over a 32 m sprite (WolfEnvironmentRender::starTint, Jensen et al. 2001 eq. 6). A star of magnitude m gives
        // 10^(-0.4 (m + 1)) of that illuminance (Pogson's ratio: 5 magnitudes = 100 x), spread over its own sprite of
        // sc x sc m (sc = 10 + 22 intensity, intensity = clamp(sqrt(10^(-0.4 (m - 6))) / 10, 0.15, 1): llvowlsky.cpp
        // updateRealStars), so its luminance is that x (32 / sc)^2.
        // Source: llvowlsky.cpp WOLF_STAR_MAG_MIN -2.0, WOLF_STAR_MAG_SPAN 8.5 (must match)
        float m = vertex_color.a * 8.5 - 2.0;
        float intensity = clamp(sqrt(pow(10.0, -0.4 * (m - 6.0))) / 10.0, 0.15, 1.0);
        float sc = 10.0 + 22.0 * intensity;
        col.rgb *= pow(10.0, -0.4 * (m + 1.0)) * (32.0 / sc) * (32.0 / sc);
    }

    float factor = smoothstep(0.0f, 0.9f, custom_alpha);

    col.a = (col.a * factor) * 32.0f;
    col.a *= twinkle();

    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = col;
#else
    frag_data[0] = col;
#endif
}

