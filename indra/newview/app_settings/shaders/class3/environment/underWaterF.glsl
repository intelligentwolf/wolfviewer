/**
 * @file class3\environment\underWaterF.glsl
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

out vec4 frag_color;

uniform sampler2D bumpMap;
uniform sampler2D exclusionTex;

#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
#endif

uniform vec4 fogCol;
uniform vec3 lightDir;
uniform vec3 specular;
uniform float lightExp;
uniform vec2 fbScale;
uniform float refScale;
uniform float znear;
uniform float zfar;
uniform float kd;
uniform vec4 waterPlane;
uniform vec3 eyeVec;
uniform vec4 waterFogColor;
uniform vec3 waterFogColorLinear;
uniform float waterFogKS;
uniform vec2 screenRes;

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;
in vec3 vary_normal;   // <WolfViewer 2026-09-06> the surface's up, eye space (waterV.glsl)

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color);
void mirrorClip(vec3 position);

void main()
{
    mirrorClip(vary_position);
    vec2 screen_tc = (refCoord.xy/refCoord.z) * 0.5 + 0.5;
    float water_mask = texture(exclusionTex, screen_tc).r;

    vec4 color;

    //get detail normals
    vec3 wave1 = texture(bumpMap, vec2(refCoord.w, view.w)).xyz*2.0-1.0;
    vec3 wave2 = texture(bumpMap, littleWave.xy).xyz*2.0-1.0;
    vec3 wave3 = texture(bumpMap, littleWave.zw).xyz*2.0-1.0;
    vec3 wavef = normalize(wave1+wave2+wave3);

    //figure out distortion vector (ripply)
    vec2 distort = screen_tc;
    distort = mix(distort, distort+wavef.xy*refScale, water_mask);

#ifdef TRANSPARENT_WATER
    vec4 fb = texture(screenTex, distort);
#else
    vec4 fb = vec4(waterFogColorLinear, 0.0);
#endif

    // <WolfViewer 2026-09-06> SNELL'S WINDOW. From under water the world above is visible
    // only inside a cone of about 97 degrees (twice the critical angle, asin(1/1.333) =
    // 48.6 deg); outside it the surface is a mirror of the water body — total internal
    // reflection, the dark disc-with-a-bright-window every diver knows. The refraction
    // buffer already holds the world seen through the surface; it is kept inside the
    // window, mixed toward the body by the below-surface Fresnel (Schlick, R0 = 0.02 for
    // 1.333 -> 1.0), replaced by the body outside it, with a soft edge over a few
    // hundredths of sin(i) and a bright rim where rays graze the surface. The water fog
    // below then takes over with distance, as before.
    // Source: wolfstorm/js/libs/Water.js eyeBelow block.
    {
        vec3 up = normalize(vary_normal);
        vec3 toSurf = normalize(vary_position);
        float cosI = clamp(dot(toSurf, up), 0.0, 1.0);
        float sinI = sqrt(max(0.0, 1.0 - cosI * cosI));
        float window = 1.0 - smoothstep(0.72, 0.78, sinI);
        const float R0 = 0.02;
        float fres = R0 + (1.0 - R0) * pow(1.0 - cosI, 5.0);
        vec4 body = vec4(waterFogColorLinear * 0.85, fb.a);
        float rim = window * (1.0 - smoothstep(0.02, 0.18, cosI)) * 0.6;
        fb = mix(mix(fb, body, fres), body, 1.0 - window);
        fb.rgb += specular * rim * 0.25;
    }
    // </WolfViewer>

    fb = applyWaterFogViewLinearNoClip(vary_position, fb);

    frag_color = max(fb, vec4(0));
}
