/**
 * @file wolfeepreference.h
 * @brief What the stock viewer puts on the screen for an EEP sky - the LL "classic" / HDR EEP renderer's formulas,
 *        transcribed line by line on the CPU, so the native Wolf sky can be DRIVEN to those values and the result
 *        CHECKED against them (WolfEnvironmentRender: the exposure, the sky's per-band gains, the cloud coverage;
 *        WolfEnvironmentTrace logs the comparison).
 *
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13). Paul: "AUDIT the standard EEP -> use it to DRIVE the physical sky ->
 * verify it is correct". Every formula cites the STOCK Firestorm source it is transcribed from
 * (firestorm/viewercode/phoenix-firestorm-master/indra, "[S]"); the audit with all paths is
 * region-terrain-work/wolf-environment-render/AUDIT_LL_EEP.md.
 */

#ifndef WOLF_EEP_REFERENCE_H
#define WOLF_EEP_REFERENCE_H

#include "llsettingssky.h"
#include "v3color.h"
#include "v3math.h"

namespace WolfEepReference
{
    /** Which of the stock renderer's paths the sky takes ([S] llsettingsvo.cpp:810, pipeline.cpp:7844, :8071-8079). */
    struct Path
    {
        bool classic = true;      // canAutoAdjust() && !RenderSkyAutoAdjustLegacy
        bool noPost = true;       // reflection probe ambiance == 0: no exposure, no tone map, legacy gamma
        F32  gamma = 1.f;         // the EEP's gamma (the legacy gamma curve's exponent)
        F32  skyHdrScale = 1.f;   // sky_hdr_scale ([S] llsettingsvo.cpp:830-852)
    };
    Path path(const LLSettingsSky::ptr_t& sky);

    /** The sky's linear value in the frame (softenLight's output, before the display chain) toward `dirWorld`
     *  (agent frame: x east, y north, z up; unit). Rainbow and halo (moisture / ice) are not included. */
    LLColor3 skyLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld);

    /** A white (albedo 1), horizontal, unshadowed legacy-material surface at the camera: its linear value in the frame. */
    LLColor3 surfaceLinear(const LLSettingsSky::ptr_t& sky);

    /** The stock light on a white up-facing legacy surface, split for a linear renderer: `ambient` = the surface's value with
     *  the sun's part left out (N.L = 0), `sunPerCos` = (the full value - ambient) / the light's N.L there, so ambient +
     *  sunPerCos x N.L gives the stock value on a level surface and the stock ambient on one turned from the light. */
    void surfaceLights(const LLSettingsSky::ptr_t& sky, LLColor3& sunPerCos, LLColor3& ambient);

    /** The same surface `distance` metres away along the horizontal `dirWorld`, after the haze pass ([S] hazeF.glsl:79-100). */
    LLColor3 hazedSurfaceLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld, F32 distance);

    /** The haze pass's transmittance (atten.r) at `distance` metres ([S] atmosphericsFuncs.glsl:79-87). */
    F32 hazeTransmittance(const LLSettingsSky::ptr_t& sky, F32 distance);

    /** The share of the overhead sky the EEP's clouds cover: the mean of the stock cloud opacity (alpha1) over the
     *  cloud noise texture's measured distribution ([S] cloudsF.glsl:59-127, cloudsV.glsl:184). */
    F32 cloudCoverage(const LLSettingsSky::ptr_t& sky);

    /** The share of the overhead sky where the stock cloud opacity (alpha1) is at least one half - the sky the EEP's clouds
     *  visibly cover, over the same noise distribution (cloudCoverage). */
    F32 cloudCoveredFraction(const LLSettingsSky::ptr_t& sky);

    /** The stock clouds' own colour toward `dirWorld`, in the frame's linear units, where they cover the sky: the mean over the
     *  noise samples with alpha1 >= 0.5 of the cloud fragment's colour ([S] cloudsV.glsl:76-190, cloudsF.glsl:59-127),
     *  linearised as the sky is (x 1.1 in classic). false where the stock renderer draws no cloud there. */
    bool cloudColourLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld, LLColor3& colour);

    /** A stock cloud fragment's G-buffer colour (cloudsF.glsl frag_data, as the stock target stores it) in the frame's linear
     *  units: linearised as the sky's G-buffer value is (x 1.1 in classic) - what cloudColourLinear gives for its mean. */
    LLColor3 cloudGbufferLinear(const LLSettingsSky::ptr_t& sky, const LLColor3& gbuffer);

    /** The stock haze pass's inputs, for a shader to evaluate it per pixel ([S] atmosphericsFuncs.glsl:51-131 calcAtmosphericVars,
     *  hazeF.glsl:79-100): additive = blueW (cs + tmpAmb) + hazeW (cs g + tmpAmb), g = clamp(pow(max(1 - (n.L)(n.l), 0.001)
     *  glow.x, glow.z), -1e5, 1e5) + 0.25, times smgf; atten = exp(-combined len DM distance_multiplier), len the eye
     *  distance with |y| held to max_y; out = lit atten.r + srgb_to_linear(2 additive (1 - atten)) skyHdrScale. */
    struct HazeParams
    {
        LLColor3 blueW;        // blue_horizon x blue_weight
        LLColor3 hazeW;        // haze_horizon x haze_weight
        LLColor3 cs;           // the light through LL's air, dimmed by cloud_shadow
        LLColor3 tmpAmb;       // ambient lifted by cloud_shadow
        LLColor3 combined;     // blue_density + haze_density
        F32 glowX = 1.f, glowZ = -0.48f, smgf = 1.f;
        F32 densityDistance = 0.f;   // density_multiplier x distance_multiplier
        F32 maxY = 1605.f;
        F32 skyHdrScale = 1.f;
        LLVector3 lightnormWorld;   // the light norm (world, x east y north z up; z held to >= -0.1, not renormalised)
        LLVector3 lightDirWorld;    // the sun's direction, else the moon's
    };
    HazeParams hazeParams(const LLSettingsSky::ptr_t& sky);

    /** The stock display chain on a linear value (no_post: clamp, sRGB, legacy gamma; else ACES-Hill mix at exposure 1). */
    LLColor3 display(const Path& p, const LLColor3& lin);
}

#endif
