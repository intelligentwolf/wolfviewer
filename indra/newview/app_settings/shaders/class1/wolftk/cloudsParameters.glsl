/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/parameters.glsl.
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
 */

uniform vec2 resolution;
uniform int frame;
// [WOLF] NVIDIA STBN dropped (its licence: research / evaluation only, or the RTX SDK licence - not
// for this commercial product). The blue-noise lookup is replaced by takram's own MIT temporal
// interleaved gradient noise: Source: three-geospatial packages/core/src/webgpu/generators.ts:20-25
// dithering (seed = screen coordinate + fract(time) * 1337) and :15-18 interleavedGradientNoise.
// Source: packages/clouds/src/shaders/parameters.glsl:3 stbnTexture -> generators.ts:22 `time`
uniform float time; // seconds

// Atmosphere
uniform float bottomRadius;
uniform mat4 worldToECEFMatrix;
uniform mat4 ecefToWorldMatrix;
uniform vec3 altitudeCorrection;
uniform vec3 sunDirection;

// Participating medium
uniform float scatteringCoefficient;
uniform float absorptionCoefficient;

// Primary raymarch
uniform float minDensity;
uniform float minExtinction;
uniform float minTransmittance;

// Shape and weather
uniform sampler2D localWeatherTexture;
uniform vec2 localWeatherRepeat;
uniform vec2 localWeatherOffset;
uniform float coverage;
// [WolfViewer 2026-10-13, step 3 genera] per-layer weather (wolftkclouds.cpp sharedUniforms, wolfcloudgenus.cpp)
uniform vec4 wolfLayerCoverage;    // coverage per layer channel; < 0 = the one coverage above
uniform vec4 wolfLayerAbsorption;  // absorption coefficient per layer, per unit density (precipitation darkening)
uniform vec4 wolfLayerIce;         // 0 water droplets .. 1 ice crystals per layer (the phase function, no powder)
uniform float wolfCloudEvolution;  // [WolfViewer round 17] the shapes' vertical drift, metres (clouds.glsl evolution)
// [WolfViewer round 16c] the powder exponent per layer, x powderExponent: takram tuned powderExponent 150 (CloudsMaterial.ts:
// 214-215, powderScale 0.8) with its cumulus layers' densityScale 0.2 (CloudLayers.ts:36, :48): the powder band is
// exp(-150 x 0.2 x density). The genus rows carry physical extinction from the liquid water content (wolfcloudgenus.cpp
// sigma = 3 LWC / (2 rho_w r_e): Cu mediocris 0.075 /m), so the same exponent reached 2.7 x deeper into a mediocris and
// darkened its sunlit tops (round 16c: effective top reflectance 0.52 against two-stream ~0.85). A genus layer passes
// 0.2 / its densityScale to keep takram's band in takram's density units; takram's own layers pass 1 (unchanged).
uniform vec4 wolfLayerPowder;
uniform mat2 wolfWeatherRotation;  // the weather texture turned with the wind and stretched along it (cloud streets, cirrus streaks); identity = none
uniform vec2 wolfWeatherPivot;     // [WolfViewer round 13] the globe uv the turn and stretch are about (near the camera; wolftkclouds.cpp)
// [WolfViewer 2026-10-14] the stretch is for the layers that have that shape only (cirrus, cloud streets;
// wolfcloudgenus.cpp layerStretch): the others read the texture through the turn alone
uniform mat2 wolfWeatherTurn;      // the turn R without the stretch (wolfWeatherRotation = S R)
uniform vec4 wolfLayerStretched;   // per layer: 1 = read through wolfWeatherRotation (stretched), 0 = through wolfWeatherTurn
uniform vec2 wolfWeatherOffsetTurned;   // the turned lookup's own offset (wolftkclouds.cpp localWeatherOffsetTurned)
uniform vec4 wolfLayerTower;       // per layer: 0 = a fixed band (takram); > 0 = convective, 1 - the lowest column top as a share of the depth
// [WolfViewer 2026-10-26] the curved flow and the upper wind (cloudsCommon.glsl wolfFlowWarp / sampleWeather; wolftkclouds.cpp)
// wolfLayerStretched is now 0 / 1 / 2: turned / stretched along the 10 m wind (streets) / stretched along the upper wind (cirrus)
uniform mat2 wolfWeatherRotationUpper;  // the cirrus lookup: S R_upper (R_upper = the upper wind's turn, WolfCloudGenus upperRotationDeg)
uniform vec2 wolfWeatherOffsetUpper;    // its drift with the upper wind (wolftkclouds.cpp localWeatherOffsetUpper, wrapped 0..1)
uniform vec2 wolfUpperRepeat;           // [10-26] its own repeat per face: cirrus dimensions (wolfcloudgenus.h CIRRUS_TILE_M), not the EEP cloud_scale
uniform vec4 wolfFlowDrift;             // the two flow fields' drift in globe uv, wrapped 0..1: xy the streets' (10 m wind), zw the cirrus' (upper wind)
uniform vec4 wolfFlowState;             // x = time / 6 h wrapped 0..1 (the eddies' evolution), y = the streets' bend, z = the cirrus' bend (0 = straight)
uniform vec4 wolfLayerFibre;            // per layer 0..1: the fibrous ice detail (wolfcloudgenus.cpp rows)
uniform vec3 wolfFibreScale;            // the fibre noise per texture unit of the cirrus lookup (x along, y across: whole numbers) and per metre of height (z)
// [CLOUDS CLEAR OF BUILDS 2026-10-26] the build clearance field (wolfcloudclearance.h; cloudsCommon.glsl wolfBuildClear)
uniform sampler2D wolfBuildField;       // RGBA16F, 3 levels x 256 rows: R distance to a build, G ground tops, B sky tops, A sky bottoms (agent z)
uniform vec4 wolfBuildLevel[3];         // per level: the texel grid's corner (agent x, y), the texel (m), unused
uniform vec4 wolfBuildParams;           // x 1 = on, y the camera region's sea (agent z of height 0), z the highest cleared agent z, w unused
uniform vec4 wolfLayerBuildFade;        // per layer: 1 = fades round builds, 0 = not (ground fog, wolfcloudgenus.cpp)
uniform vec2 wolfBuildFade;             // [2026-10-26] x M0: fully clear within this of a build, y W: back to cloud over this (metres; WolfCloudClearance, app_settings/wolf_clouds.xml)
uniform vec4 wolfLayerRepeat;           // [2026-10-26] per layer: the weather texture's repeat x this (a whole number) for the turned lookup - the genus's cell size (wolf_clouds.xml genera 'repeat')
uniform vec3 wolfFibreShape;            // the fibre noise's mean, 2 x its standard deviation, and the mean of the clamped normalised noise (y 0 = not measured: no fibres)
// [HURRICANE 2026-10-29] ticket #367: the region's storm (cloudsCommon.glsl wolfStormApply; wolftkclouds.cpp sharedUniforms from
// CloudsFrame storm_*; the model: region-terrain-work/hurricane-2026-10-29/README.md "THE MODEL", CPU twin wolfhurricane.cpp)
uniform vec4 wolfStorm;                 // x 1 = a storm is running (0: none, skipped), y O organisation, z E eye clear, w C gathering
uniform vec4 wolfStormEye;              // xy the eye c(t) (agent x, y metres), z R the outer radius, w Re the eye's radius (metres)
uniform vec2 wolfStormSpin;             // x h the hemisphere (1 north, -1 south), y D the metres the field has streamed inwards
// the ice phase: the forward lobe's asymmetry x this for an ice layer (wolfcloudgenus.h WOLF_ICE_G_SCALE, the viewer
// and the harness define it from the cited asymmetry parameters); 1 = water everywhere
#ifndef WOLF_ICE_G_SCALE
#define WOLF_ICE_G_SCALE 1.0
#endif
// the multiple-scattering octaves' attenuation (clouds.frag approximateMultipleScattering); 0.5 = takram's
#ifndef WOLF_MS_ATTENUATION
#define WOLF_MS_ATTENUATION 0.5
#endif
uniform sampler3D shapeTexture;
uniform vec3 shapeRepeat;
uniform vec3 shapeOffset;

#ifdef SHAPE_DETAIL
uniform sampler3D shapeDetailTexture;
uniform vec3 shapeDetailRepeat;
uniform vec3 shapeDetailOffset;
#endif // SHAPE_DETAIL

#ifdef TURBULENCE
uniform sampler2D turbulenceTexture;
uniform vec2 turbulenceRepeat;
uniform float turbulenceDisplacement;
#endif // TURBULENCE

// Haze
#ifdef HAZE
uniform float hazeDensityScale;
uniform float hazeExponent;
uniform float hazeScatteringCoefficient;
uniform float hazeAbsorptionCoefficient;
#endif // HAZE

// Cloud layers
uniform vec4 minLayerHeights;
uniform vec4 maxLayerHeights;
uniform vec3 minIntervalHeights;
uniform vec3 maxIntervalHeights;
uniform vec4 densityScales;
uniform vec4 shapeAmounts;
uniform vec4 shapeDetailAmounts;
uniform vec4 weatherExponents;
uniform vec4 shapeAlteringBiases;
uniform vec4 coverageFilterWidths;
uniform float minHeight;
uniform float maxHeight;
uniform float shadowTopHeight;
uniform float shadowBottomHeight;
uniform vec4 shadowLayerMask;
uniform CloudDensityProfile densityProfile;
