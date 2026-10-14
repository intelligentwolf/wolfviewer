/**
 * @file class3/deferred/waterHazeF.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

out vec4 frag_color;

// Inputs
in vec4 vary_fragcoord;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);

vec4 getWaterFogView(vec3 pos);

uniform int above_water;

uniform sampler2D exclusionTex;

void main()
{
    vec2  tc           = vary_fragcoord.xy/vary_fragcoord.w*0.5+0.5;
    float depth        = getDepth(tc.xy);
    float mask = texture(exclusionTex, tc.xy).r;

    if (above_water > 0)
    {
        // Just discard if we're in the exclusion mask.
        // The previous invisiprim hack we're replacing would also crank up water fog desntiy.
        // But doing that makes exclusion surfaces very slow as we'd need to render even more into the mask.
        // - Geenz 2025-02-06
        if (mask < 1)
        {
            discard;
        }

        // we want to depth test when the camera is above water, but some GPUs have a hard time
        // with depth testing against render targets that are bound for sampling in the same shader
        // so we do it manually here

        float cur_depth = vary_fragcoord.z/vary_fragcoord.w*0.5+0.5;
        if (cur_depth > depth)
        {
            discard;
        }

        // <WolfViewer 2026-10-12> Nothing under this water but the sky: no haze. Source: hazeF.glsl:79-82 (the sky
        // haze pass) `if (depth >= 1.0) discard; // should only be true of sky, clouds, sun/moon, and stars`. This pass
        // runs before LLDrawPoolWater copies the screen for the water's refraction (LLPipeline::renderGeomPostDeferred:
        // doWaterHaze, then LLDrawPoolWater::beginPostDeferredPass -> mWaterDis), so the sky-depth pixels under the
        // open sea's planes were fogged by 4 km of water (black with a black fog colour, alpha 0) INTO that copy, and
        // waterF.glsl's blend into the sky's ocean at the far clip (wolfSeaBehind / fb) blended the far water into
        // that black: the dark line on the horizon.
        if (depth >= 1.0)
        {
            discard;
        }
    }

    vec4  pos          = getPositionWithDepth(tc, depth);

    vec4 fogged = getWaterFogView(pos.xyz);
    fogged.a = max(pow(fogged.a, 1.7), 0);

    frag_color = max(fogged, vec4(0)); //output linear since local lights will be added to this shader's results

}
