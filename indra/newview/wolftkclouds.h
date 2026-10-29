/**
 * @file wolftkclouds.h
 * @brief WolfViewer: takram three-geospatial's volumetric clouds (packages/clouds), translated.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4).
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Shota Matsuda
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * The cascaded shadow maps (CascadedShadowMaps.ts, helpers/FrustumCorners.ts) are based on three-csm,
 * MIT License, Copyright (c) 2019 vtHawk (notice repeated in wolftkclouds.cpp). three.js math used by them
 * (Matrix4 lookAt / makeOrthographic / makePerspective, Vector3 applyMatrix4, MathUtils.lerp), r171,
 * MIT License, Copyright (c) 2010-2024 three.js authors.
 */

#ifndef WOLF_TK_CLOUDS_H
#define WOLF_TK_CLOUDS_H

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "llglslshader.h"
#include "v2math.h"
#include "v3math.h"
#include "v4math.h"

namespace WolfTk
{
    class Atmosphere;

    // Source: three-geospatial packages/clouds/src/DensityProfile.ts:5-37 DensityProfile
    struct DensityProfile
    {
        F32 expTerm = 0.f, exponent = 0.f, linearTerm = 0.f, constantTerm = 0.f;
    };

    // Source: packages/clouds/src/CloudLayer.ts:42-55 CloudLayer (field defaults = CloudLayer.DEFAULT)
    struct CloudLayer
    {
        char channel = 'r';
        F32 altitude = 0.f;
        F32 height = 0.f;
        F32 densityScale = 0.2f;
        F32 shapeAmount = 1.f;
        F32 shapeDetailAmount = 1.f;
        F32 weatherExponent = 1.f;
        F32 shapeAlteringBias = 0.35f;
        F32 coverageFilterWidth = 0.6f;
        DensityProfile densityProfile = { 0.f, 0.f, 0.75f, 0.25f };
        bool shadow = false;
        // [WolfViewer 2026-10-13, step 3 genera] the shader's wolfLayer* uniforms (cloudsParameters.glsl): the layer's
        // own coverage (< 0: the one CloudsFrame::coverage), its ice fraction (the phase function, no powder) and an
        // absorption per unit density (precipitation darkening, Schneider 2015 p. 69)
        F32 coverage = -1.f;
        F32 ice = 0.f;
        F32 absorption = 0.f;
        F32 powder = 1.f;   // [WolfViewer round 16c] x powderExponent (cloudsParameters.glsl wolfLayerPowder): 1 = takram's
        F32 fibre = 0.f;    // [WolfViewer 2026-10-26] the fibrous ice detail (cloudsParameters.glsl wolfLayerFibre): 0 = none (takram's)
        F32 buildFade = 1.f;   // [CLOUDS CLEAR OF BUILDS 2026-10-26] 1 = fades round builds (cloudsParameters.glsl wolfLayerBuildFade); 0 = ground fog
        F32 repeat = 1.f;      // [WolfViewer 2026-10-26] the weather texture's repeat x this for this layer, a whole number (cloudsParameters.glsl wolfLayerRepeat; WolfCloudGenus::GenusRow::repeat)
    };

    // Source: packages/clouds/src/CloudLayers.ts:30-181 CloudLayers (always 4 layers)
    struct CloudLayers
    {
        CloudLayer layer[4];
        /** CloudLayers.DEFAULT (CloudLayers.ts:31-68) */
        static CloudLayers defaults();
        /** localWeatherChannels (CloudLayers.ts:107-109) */
        std::string localWeatherChannels() const;
        /** packIntervalHeights (CloudLayers.ts:141-180) */
        void packIntervalHeights(F32 min_intervals[3], F32 max_intervals[3]) const;
    };

    /** The numbers of one quality preset. Source: packages/clouds/src/qualityPresets.ts:8-121 */
    struct QualityPreset
    {
        F32  resolutionScale;
        bool lightShafts, shapeDetail, turbulence, haze;
        // clouds
        S32  multiScatteringOctaves;
        bool accurateSunSkyLight, accuratePhaseFunction;
        S32  maxIterationCount;
        F32  minStepSize, maxStepSize, maxRayDistance, perspectiveStepScale, minDensity, minExtinction, minTransmittance;
        S32  maxIterationCountToGround, maxIterationCountToSun;
        F32  minSecondaryStepSize, secondaryStepScale;
        S32  maxShadowLengthIterationCount;
        F32  minShadowLengthStepSize, maxShadowLengthRayDistance;
        // shadow
        S32  cascadeCount;
        S32  mapSize;
        S32  shadowMaxIterationCount;
        F32  shadowMinStepSize, shadowMaxStepSize, shadowMinDensity, shadowMinExtinction, shadowMinTransmittance;

        static const QualityPreset& get(S32 quality /*0 low, 1 medium, 2 high, 3 ultra*/);
    };

    /** One frame's inputs (the contract with wolfenvironmentrender.cpp). "world" is LL's agent frame in metres. */
    struct CloudsFrame
    {
        glm::mat4  view, proj;          // LL's scene modelview (world -> eye) and projection
        F32        near_clip = 0.1f, far_clip = 512.f;
        U32        width = 1, height = 1;
        glm::dmat4 world_to_ecef;       // rigid; ecef = this x world (metres)
        LLVector3  sun_dir_ecef;        // takram sunDirection (ECEF): THE light - the sun, or the moon at night ([WolfViewer round 8])
        F32        light_scale = 1.f;   // its irradiance over the sun's (1 the sun; the moon's TOA illuminance / the sun's)
        LLVector3  other_dir_ecef;      // the other body, whose sky irradiance alone is added ..
        F32        other_scale = 0.f;   // .. with this scale (0: none)
        LLColor3   light_tint = LLColor3::white;   // [2026-10-28] per channel on THE light's direct irradiance (the moon's eye colour, ticket #304)
        // [WolfViewer 2026-10-27] the moon disc as the sky dome drew it this pass (wolfenvironmentrender.cpp sMoonDrawn), ECEF:
        // its centre, the quad's right / up and its half-size as tangents along them - the disc is length(p) <= 1 (skyF.glsl
        // wolfEepMoon / wolfMoonDisc). The corona is not added over it (cloudsF.glsl wolfCoronaOffDisc). Off: none drawn.
        bool       moon_disc_on = false;
        LLVector3  moon_disc_to_ecef, moon_disc_right_ecef, moon_disc_up_ecef;
        LLVector2  moon_disc_half;
        LLVector3  camera_world;
        F32        dt = 0.f;
        F64        time = 0.0;          // seconds (the noise seed, generators.ts:22)
        S32        quality = 2;         // 0 low, 1 medium, 2 high, 3 ultra
        F32        coverage = 0.3f;     // uniforms.ts:54 default
        LLVector2  local_weather_velocity;
        LLVector2  local_weather_velocity_turned;   // [WolfViewer 2026-10-14] the same drift for the unstretched lookup (R w, no S)
        F32        weather_layer_tower[4] = { 0.f, 0.f, 0.f, 0.f };   // [WolfViewer 2026-10-14] per-column cloud tops (cloudsCommon.glsl)
        F32        wind_speed = 0.f;   // [WolfViewer round 17] m/s: the shapes' drift wolfCloudEvolution (cloudsCommon.glsl) = 0.2 x the wind's path
        F32        turbulence_displacement = 350.f;   // metres (CloudsMaterial.ts turbulenceDisplacement 350); [WolfViewer 2026-10-13] + the EEP's cloud_variance
        F32        haze_density_scale = 3e-5f;   // CloudsMaterial.ts:536 default
        F32        haze_exponent = 1e-3f;        // [WolfViewer 2026-10-13] /m: CloudsMaterial.ts hazeExponent 1e-3, or 1 / the EEP's max_y
        bool       haze_always = false;          // [WolfViewer 2026-10-13] the EEP's haze, whatever the cloud cover (cloudsF.glsl wolfHazeAlways)
        // [WolfViewer 2026-10-13] the stock EEP haze on the ground (cloudsF.glsl wolfLLHaze; WolfEepReference::HazeParams), in place of
        // takram's haze when an EEP drives the sky; directions in ECEF
        bool       ll_haze = false;
        LLColor3   ll_blue_w, ll_haze_w, ll_cs, ll_tmp_amb, ll_combined;
        F32        ll_glow_x = 1.f, ll_glow_z = -0.48f, ll_smgf = 1.f, ll_density_distance = 0.f, ll_max_y = 1605.f, ll_hdr = 1.f;
        LLVector3  ll_lightnorm_ecef, ll_light_dir_ecef;
        F32        weather_repeat_scale = 1.f;  // [WolfViewer round 9] x localWeatherRepeat (100, CloudsEffect.ts:180-186): the EEP's
                                                 // cloud_scale as the clouds' size (wolfenvironmentrender.cpp cloudsFrame)
        F32        weather_rotation_deg = 0.f;   // [WolfViewer step 3] the weather texture turned to the wind (cloud streets, cirrus) ..
        F32        weather_stretch = 1.f;        // .. and stretched along it (wolfcloudgenus.cpp; cloudsCommon.glsl wolfWeatherRotation) ..
        F32        weather_layer_stretched[4] = { 0.f, 0.f, 0.f, 0.f };   // .. [WolfViewer 2026-10-14] for these layers only (1 = stretched;
                                                                          // WolfCloudGenus::Result::layerStretch, cloudsCommon.glsl wolfLayerStretched)
        LLVector2  weather_pivot = LLVector2(0.5f, 0.5f);   // [WolfViewer round 13] the globe uv the turn / stretch are about (cloudsCommon.glsl wolfWeatherPivot)
        // [WolfViewer 2026-10-26] the cirrus lookup (wolfLayerStretched 2): turned to the upper wind (WolfCloudGenus upperRotationDeg),
        // stretched by weather_stretch, drifting with that wind (its own offset, as local_weather_velocity for the streets)
        F32        weather_upper_rotation_deg = 0.f;
        LLVector2  local_weather_velocity_upper;
        // [WolfViewer 2026-10-26] the curved flow fields that bend the two stretched lookups (cloudsCommon.glsl wolfFlowWarp): each
        // carried by its own wind (globe uv per second: x = the 10 m wind for the streets, y = the upper wind for the cirrus) and
        // scaled by its bend (WolfCloudGenus FLOW_BEND_*; 0 = straight, the lookup as before)
        LLVector2  flow_velocity_low, flow_velocity_upper;
        F32        flow_bend_low = 0.f, flow_bend_upper = 0.f;
        // [CLOUDS CLEAR OF BUILDS 2026-10-26] the build clearance field (wolfcloudclearance.h; cloudsCommon.glsl wolfBuildClear):
        // its texture (0 = none: the clouds ignore builds), each level's corner (agent x, y) and texel, the camera region's sea
        // (agent z of the clouds' height 0, as placeOnPlanet) and the highest cleared agent z (top of any build + M0 + W)
        GLuint     build_tex = 0;
        F32        build_level[3][3] = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f } };
        F32        build_sea = 0.f;
        F32        build_z_limit = -1.0e6f;
        LLVector2  build_fade = LLVector2(20.f, 50.f);   // [2026-10-26] M0, W metres (WolfCloudClearance, the cloud table; cloudsCommon.glsl wolfBuildFade)
        // [HURRICANE 2026-10-29] ticket #367: the region's storm this frame (wolfhurricane.h; cloudsCommon.glsl wolfStormApply),
        // filled by wolfenvironmentrender.cpp cloudsFrame from WolfHurricane::timeAt / centre on the wall clock. Off = no storm
        // running (the shader's branch is skipped on the uniform). Source: region-terrain-work/hurricane-2026-10-29/README.md
        // "THE MODEL": O organisation, E eye clear, C gathering; the eye c(t) in agent x, y; R and Re; h the hemisphere;
        // D = 0.622 vmax sin(10 deg) A, the metres the field has streamed inwards
        bool       storm_on = false;
        F32        storm_O = 0.f, storm_E = 0.f, storm_C = 0.f;
        F32        storm_eye_agent[2] = { 0.f, 0.f };
        F32        storm_radius = 1000.f, storm_eye_radius = 100.f;
        F32        storm_h = 1.f, storm_D = 0.f;
        LLVector3  sky_under_cloud = LLVector3(1.f, 1.f, 1.f);   // the horizontal sky irradiance under the clouds / the
                                                                 // clear sky's, per channel (cloudsF.glsl wolfSkyUnderCloud)
        GLuint     depth_tex = 0;       // the scene depth (GL depth texture, [0,1])
        F32        pre_exposure = 1.f;
        bool       reset_history = false;
        // [WolfViewer 2026-10-19] a cut (a teleport): the weather and shape offsets start again at this phase (0..1 of a
        // repeat) instead of carrying on from wherever the session's drift had taken them - see wolfenvironmentrender.cpp
        bool       reseed = false;
        F32        seed_phase[2] = { 0.f, 0.f };
    };

    class Clouds
    {
    public:
        Clouds();
        ~Clouds();

        bool loadShaders(S32 shader_level);
        void unloadShaders();
        bool shadersReady() const;

        /** The main view: shadow pass + its resolve, clouds pass + its resolve (CloudsEffect.update). */
        bool render(const Atmosphere& atmosphere, const CloudsFrame& frame);
        /** A reflection-probe face: the clouds pass at the face's size without temporal upscaling and without
         *  the resolve (no history across faces); reuses this frame's shadow map. */
        bool renderProbe(const Atmosphere& atmosphere, const CloudsFrame& frame);

        GLuint colorTex() const;              // premultiplied radiance + alpha (CloudsPass outputBuffer)
        /** [WolfViewer 2026-10-13] This frame's low-resolution march target (depth, velocity, .a = the clouds' own opacity over
         *  the sky or -(the scene's distance): cloudsF.glsl), for wolfenvironmentrender.cpp's coverage measurement. */
        GLuint marchDepthVelocityTex(S32& w, S32& h) const;
        /** [WolfViewer 2026-10-13] .. and its colour (premultiplied radiance + alpha, the same texels). */
        GLuint marchColorTex() const;
        GLuint shadowLengthTex() const;       // R16F, km (clouds.frag:1001), 0 when light shafts are off
        GLuint probeColorTex() const;
        GLuint probeShadowLengthTex() const;
        GLuint shadowMapArray() const;        // RGBA16F 2D array, cascadeCount layers
        S32    cascadeCount() const;
        LLVector2 shadowMapSize() const;
        const F32* shadowIntervals() const;        // vec2 x cascadeCount
        const F32* shadowMatrices() const;         // mat4 column-major x cascadeCount, world -> shadow clip
        const F32* inverseShadowMatrices() const;  // mat4 column-major x cascadeCount
        F32    shadowFar() const;
        F32    shadowTopHeight() const;
        F32    shadowBottomHeight() const;   // the shadow layers' bottom, metres above the sea (uniforms.ts:171-184)
        bool   lightShafts() const;

        CloudLayers& layers() { return mLayers; }

        void release();

    private:
        struct Impl;
        Impl* mImpl;
        CloudLayers mLayers;
    };
}

#endif // WOLF_TK_CLOUDS_H
