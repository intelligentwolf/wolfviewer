/**
 * @file wolftk/filamentAgx.glsl
 * @brief Filament's AgX tone mapper and the colour-grading steps around it, for the native Wolf sky.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from Google Filament,
 * commit 764fe6cac9bdb4ba24d3d852344dfa04a5f70f75:
 *   filament/src/ToneMapper.cpp:690-788 (AgxToneMapper), filament/src/ColorSpaceUtils.h:43-66, 163-165,
 *   filament/src/details/ColorGrading.cpp:706-718, 1062-1089 (where the tone mapper runs).
 *
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Changes from the source: C++ -> GLSL (filament::math::mat3f and GLSL mat3 are both built from nine
 * values in column-major order, filament libs/math/include/math/mat3.h:208-218, so the constants are in
 * the same order); the look is chosen at build time (WOLF_AGX_LOOK, below; Filament's AgxLook, ToneMapper.h:230-234);
 * the colour grading has no adjustments (ColorGrading.cpp:1062 else-branch).
 */

// Source: ColorSpaceUtils.h:43-47 XYZ_to_sRGB
const mat3 WOLF_AGX_XYZ_to_sRGB = mat3(
     3.2404542, -0.9692660,  0.0556434,
    -1.5371385,  1.8760108, -0.2040259,
    -0.4985314,  0.0415560,  1.0572252);
// Source: ColorSpaceUtils.h:49-53 sRGB_to_XYZ
const mat3 WOLF_AGX_sRGB_to_XYZ = mat3(
     0.4124560,  0.2126730,  0.0193339,
     0.3575760,  0.7151520,  0.1191920,
     0.1804380,  0.0721750,  0.9503040);
// Source: ColorSpaceUtils.h:55-59 Rec2020_to_XYZ
const mat3 WOLF_AGX_Rec2020_to_XYZ = mat3(
     0.6369530,  0.2626983,  0.0000000,
     0.1446169,  0.6780088,  0.0280731,
     0.1688558,  0.0592929,  1.0608272);
// Source: ColorSpaceUtils.h:61-65 XYZ_to_Rec2020
const mat3 WOLF_AGX_XYZ_to_Rec2020 = mat3(
     1.7166634, -0.6666738,  0.0176425,
    -0.3556733,  1.6164557, -0.0427770,
    -0.2533681,  0.0157683,  0.9422433);

// Source: ToneMapper.cpp:697-703 AgXInsetMatrix ("taken from Blender's implementation of AgX, which works
// with Rec.2020 primaries")
const mat3 WOLF_AGX_InsetMatrix = mat3(
    0.856627153315983, 0.137318972929847, 0.11189821299995,
    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
    0.0482516061458583, 0.101439036467562, 0.811302368396859);
// Source: ToneMapper.cpp:704-708 AgXOutsetMatrixInv
const mat3 WOLF_AGX_OutsetMatrixInv = mat3(
    0.899796955911611, 0.11142098895748, 0.11142098895748,
    0.0871996192028351, 0.875575586156966, 0.0871996192028349,
    0.013003424885555, 0.0130034248855548, 0.801379391839686);

// Source: ToneMapper.cpp:711-715 (LOG2_MIN -10, LOG2_MAX +6.5, MIDDLE_GRAY 0.18)
const float WOLF_AGX_MinEv = -12.47393;   // log2(pow(2, LOG2_MIN) * MIDDLE_GRAY)
const float WOLF_AGX_MaxEv = 4.026069;    // log2(pow(2, LOG2_MAX) * MIDDLE_GRAY)

// Source: ToneMapper.cpp:717-730 agxDefaultContrastApprox
vec3 wolfAgxDefaultContrastApprox(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    vec3 x6 = x4 * x2;
    return  - 17.86    * x6 * x
            + 78.01    * x6
            - 126.7    * x4 * x
            + 92.06    * x4
            - 28.72    * x2 * x
            + 4.361    * x2
            - 0.1718   * x
            + 0.002857;
}

// Source: ToneMapper.h:230-234 AgxLook: 0 NONE "Base contrast with no look applied", 1 PUNCHY "A punchy and more
// chroma laden look for sRGB displays", 2 GOLDEN "A golden tinted, slightly washed look for BT.1886 displays".
// WolfViewer uses Filament's default, NONE (ToneMapper.h:241 `AgxLook look = AgxLook::NONE`): PUNCHY's power 1.35
// and saturation 1.4 clip the clear sky's red channel to 0 (measured: scratchpad phys2/cie_compare.py, round 4).
#ifndef WOLF_AGX_LOOK
#define WOLF_AGX_LOOK 0
#endif

// Source: ToneMapper.cpp:733-761 agxLook (ASC CDL slope / offset / power, then saturation about the Rec.709 luma)
vec3 wolfAgxLook(vec3 val)
{
#if WOLF_AGX_LOOK == 0
    return val;
#else
    const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
    float luma = dot(val, lw);

    // Default
    const vec3 offset = vec3(0.0);
    vec3 slope = vec3(1.0);
    vec3 power = vec3(1.0);
    float sat = 1.0;

#if WOLF_AGX_LOOK == 2
    slope = vec3(1.0, 0.9, 0.5);
    power = vec3(0.8);
    sat = 1.3;
#endif
#if WOLF_AGX_LOOK == 1
    slope = vec3(1.0);
    power = vec3(1.35, 1.35, 1.35);
    sat = 1.4;
#endif

    // ASC CDL
    val = pow(val * slope + offset, power);
    return luma + sat * (val - luma);
#endif
}

// Source: ToneMapper.cpp:763-788 AgxToneMapper::operator()
vec3 wolfAgxToneMapper(vec3 v)
{
    // Ensure no negative values
    v = max(vec3(0.0), v);

    v = WOLF_AGX_InsetMatrix * v;

    // Log2 encoding
    v = max(v, vec3(1E-10)); // avoid 0 or negative numbers for log2
    v = log2(v);
    v = (v - WOLF_AGX_MinEv) / (WOLF_AGX_MaxEv - WOLF_AGX_MinEv);

    v = clamp(v, 0.0, 1.0);

    // Apply sigmoid
    v = wolfAgxDefaultContrastApprox(v);

    // Apply AgX look
    v = wolfAgxLook(v);

    // Source: ToneMapper.cpp:709 AgXOutsetMatrix = inverse(AgXOutsetMatrixInv)
    v = inverse(WOLF_AGX_OutsetMatrixInv) * v;

    // Linearize
    v = pow(max(vec3(0.0), v), vec3(2.2));

    return v;
}

// Source: ColorGrading.cpp:1062-1089 FColorGrading::hdrColorAt without adjustments: the exposed linear sRGB
// colour moved to the grading space (selectColorGradingTransformIn = sRGB_to_Rec2020, :706-711, with
// ColorSpaceUtils.h:163), tone mapped, back to the display space (selectColorGradingTransformOut =
// Rec2020_to_sRGB, :713-718, ColorSpaceUtils.h:165), clamped for the transfer function ("We need to clamp for
// the output transfer function", :1086-1087). The sRGB OETF (ColorSpaceUtils.h:270-279, :1089) is the
// caller's: LL's linear_to_srgb in the gamma-correct step is the same formula.
// [WolfViewer] the camera's white balance before the tone curve: Bruneton's demo (atmosphere/demo/demo.cc:327-340,
// demo.glsl:394 radiance / white_point) divides by the sun's colour above the atmosphere (Model::
// ConvertSpectrumToLinearSrgb of the solar spectrum, normalised to a mean of 1), a "daylight" white balance: the sun
// (1.08, 0.97, 0.95 in linear sRGB for ASTM G-173) is the white. The demo has it as a key toggle, off by default
// (demo.cc:108 do_white_balance_(false), :466); WolfViewer turns it on, as a camera set to daylight balance: it acts on
// the whole frame (lamps included). wolfenvironmentrender.cpp sets it; unset (0) = none.
uniform vec3 wolf_white_point;
// [WolfViewer 2026-10-13, round 6] the look, scene-referred, before the curve (BEAUTY_RESEARCH.md section 4.5 item 3;
// AgX's own outset cannot resaturate highlights, section 4.1): Hable, "Minimal Color Grading Tools" (filmicworlds.com,
// 2016) - saturation about the Rec.709 luma, then log-space contrast about middle grey, adjX = logMid + (log2(x + eps)
// - logMid) x contrast with logMid = log2(0.18), in that order (Exposure -> Saturation -> Log contrast -> curve). The
// magnitudes are Blender's AgX contrast looks (release/datafiles/colormanagement/config.ocio, GradingPrimaryTransform
// in AgX Log: Medium High 1.2 / sat 1.0, High 1.4 / 0.95, Very High 1.57 / 0.9, Medium Low 0.9 / 1.05); Blender's
// OCIO log pivot (-0.2) is not reproduced - the pivot here is Hable's middle grey. <= 0 = 1 (no look).
uniform float wolf_tone_contrast;
uniform float wolf_tone_sat;
// the pivot: the image's middle (Hable's logMid) - the frame's log-average luminance as exposed (WolfExposure::Meter::
// tonePivot; Reinhard 2002 eq. 1 is that middle), so a dim scene the meter keeps dim is not darkened a second time
// by a contrast about 0.18; <= 0 = 0.18
uniform float wolf_tone_pivot;

vec3 wolfToneLook(vec3 lin)
{
    const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
    float sat = wolf_tone_sat > 0.0 ? wolf_tone_sat : 1.0;
    float contrast = wolf_tone_contrast > 0.0 ? wolf_tone_contrast : 1.0;
    if (sat != 1.0)
    {
        float luma = dot(lin, lw);
        lin = max(vec3(luma) + sat * (lin - vec3(luma)), vec3(0.0));
    }
    if (contrast != 1.0)
    {
        float logMid = log2(wolf_tone_pivot > 0.0 ? wolf_tone_pivot : 0.18);
        const float eps = 1e-5;
        vec3 l = logMid + (log2(lin + eps) - logMid) * contrast;
        lin = max(exp2(l) - eps, vec3(0.0));
    }
    return lin;
}

vec3 wolfFilamentAgx(vec3 exposedLinearSrgb)
{
    if (all(greaterThan(wolf_white_point, vec3(0.0)))) exposedLinearSrgb /= wolf_white_point;
    exposedLinearSrgb = wolfToneLook(exposedLinearSrgb);
    vec3 v = (WOLF_AGX_XYZ_to_Rec2020 * WOLF_AGX_sRGB_to_XYZ) * exposedLinearSrgb;
    v = wolfAgxToneMapper(v);
    v = (WOLF_AGX_XYZ_to_sRGB * WOLF_AGX_Rec2020_to_XYZ) * v;
    return clamp(v, vec3(0.0), vec3(1.0));
}
