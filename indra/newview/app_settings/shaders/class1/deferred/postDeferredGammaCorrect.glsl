/**
 * @file postDeferredGammaCorrect.glsl
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

out vec4 frag_color;

uniform sampler2D diffuseRect;

uniform float gamma;
uniform vec2 screen_res;
in vec2 vary_fragcoord;

vec3 linear_to_srgb(vec3 cl);

vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}

// [WOLF ENV TAKRAM 2026-10-12] Under the native Wolf sky (wolf_phys) the frame is pre-exposed luminance
// (Filament's physically based camera, WolfExposure: exposure compensation from RenderExposure is already in
// its EV100), so it is tone mapped with Filament's AgX (wolftk/filamentAgx.glsl: Rec.2020 grading space,
// AgX, back to sRGB, clamped) and then takes the sRGB OETF below like every other frame.
uniform float wolf_phys;
#wolf_include "wolftk/filamentAgx.glsl"

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(diffuseRect, vary_fragcoord);
    if (wolf_phys > 0.5) diff.rgb = wolfFilamentAgx(diff.rgb);
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    if (wolf_phys < 0.5) diff.rgb = legacyGamma(diff.rgb);
#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
    frag_color = diff;
}

