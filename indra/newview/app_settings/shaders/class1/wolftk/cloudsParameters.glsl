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
uniform mat2 wolfWeatherTurn;      // the turn R without the stretch
uniform float wolfWeatherStretch;  // the stretch S = diag(1 / this, 1) in wolfWeatherRotation = S R
uniform vec4 wolfLayerStretched;   // per layer: 1 = read through wolfWeatherRotation (stretched), 0 = through wolfWeatherTurn
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
