/**
 * @file wolftk/precomputeF.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/precompute/{transmittance,directIrradiance,singleScattering,scatteringDensity,indirectIrradiance,multipleScattering}.frag (complete)
 *
 * three-geospatial licence:
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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * The Bruneton files keep the source's own BSD-3 notices (Eric Bruneton 2017, INRIA 2008) below.
 */

// One program per precompute pass (and OUTPUT variant), selected by WOLF_TK_PRECOMPUTE as
// PrecomputedTexturesGenerator.ts:283-371 builds one material per pass:
//   1 transmittance.frag  2 directIrradiance.frag  3 singleScattering.frag
//   4 scatteringDensity.frag  5 indirectIrradiance.frag  6 multipleScattering.frag
// OUTPUT is a program define, as PrecomputedTexturesGenerator.ts sets material.defines.OUTPUT (:476, :502, :553, :573); in
// WolfViewer each (pass, OUTPUT) is its own program (wolftkatmosphere.cpp Atmosphere::loadShaders adds the define).
// Each pass's own lines 1-7 are the shared precision / include lines below.

precision highp float;
precision highp sampler3D;

#wolf_include "wolftk/brunetonDefinitions.glsl"
#wolf_include "wolftk/brunetonCommon.glsl"
#wolf_include "wolftk/brunetonPrecompute.glsl"

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/*.frag:8 (every pass)
uniform AtmosphereParameters ATMOSPHERE;

#if WOLF_TK_PRECOMPUTE == 1
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/transmittance.frag:9-18

layout(location = 0) out vec4 transmittance;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/transmittance.frag:12 main
void main() {
  transmittance.rgb = ComputeTransmittanceToTopAtmosphereBoundaryTexture(
    ATMOSPHERE,
    gl_FragCoord.xy
  );
  transmittance.a = 1.0;
}
#endif // WOLF_TK_PRECOMPUTE == 1

#if WOLF_TK_PRECOMPUTE == 2
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/directIrradiance.frag:9-24

uniform sampler2D transmittanceTexture;

layout(location = 0) out vec4 outputColor;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/directIrradiance.frag:14 main
void main() {
  vec3 deltaIrradiance;
  vec3 irradiance;
  deltaIrradiance = ComputeDirectIrradianceTexture(
    ATMOSPHERE,
    transmittanceTexture,
    gl_FragCoord.xy
  );
  irradiance = vec3(0.0);
  outputColor = vec4(OUTPUT, 1.0);
}
#endif // WOLF_TK_PRECOMPUTE == 2

#if WOLF_TK_PRECOMPUTE == 3
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/singleScattering.frag:9-37

uniform mat3 luminanceFromRadiance;
uniform sampler2D transmittanceTexture;
uniform int layer;

layout(location = 0) out vec4 outputColor;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/singleScattering.frag:16 main
void main() {
  vec4 deltaRayleigh;
  vec4 deltaMie;
  vec4 scattering;
  vec4 singleMieScattering;
  ComputeSingleScatteringTexture(
    ATMOSPHERE,
    transmittanceTexture,
    vec3(gl_FragCoord.xy, float(layer) + 0.5),
    deltaRayleigh.rgb,
    deltaMie.rgb
  );
  deltaRayleigh.a = 1.0;
  deltaMie.a = 1.0;
  scattering = vec4(
    luminanceFromRadiance * deltaRayleigh.rgb,
    (luminanceFromRadiance * deltaMie.rgb).r
  );
  singleMieScattering.rgb = luminanceFromRadiance * deltaMie.rgb;
  singleMieScattering.a = 1.0;
  outputColor = OUTPUT;
}
#endif // WOLF_TK_PRECOMPUTE == 3

#if WOLF_TK_PRECOMPUTE == 4
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/scatteringDensity.frag:9-32

uniform sampler2D transmittanceTexture;
uniform sampler3D singleRayleighScatteringTexture;
uniform sampler3D singleMieScatteringTexture;
uniform sampler3D multipleScatteringTexture;
uniform sampler2D irradianceTexture;
uniform int scatteringOrder;
uniform int layer;

layout(location = 0) out vec4 scatteringDensity;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/scatteringDensity.frag:20 main
void main() {
  scatteringDensity.rgb = ComputeScatteringDensityTexture(
    ATMOSPHERE,
    transmittanceTexture,
    singleRayleighScatteringTexture,
    singleMieScatteringTexture,
    multipleScatteringTexture,
    irradianceTexture,
    vec3(gl_FragCoord.xy, float(layer) + 0.5),
    scatteringOrder
  );
  scatteringDensity.a = 1.0;
}
#endif // WOLF_TK_PRECOMPUTE == 4

#if WOLF_TK_PRECOMPUTE == 5
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/indirectIrradiance.frag:9-31

uniform mat3 luminanceFromRadiance;
uniform sampler3D singleRayleighScatteringTexture;
uniform sampler3D singleMieScatteringTexture;
uniform sampler3D multipleScatteringTexture;
uniform int scatteringOrder;

layout(location = 0) out vec4 outputColor;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/indirectIrradiance.frag:18 main
void main() {
  vec3 deltaIrradiance;
  vec3 irradiance;
  deltaIrradiance = ComputeIndirectIrradianceTexture(
    ATMOSPHERE,
    singleRayleighScatteringTexture,
    singleMieScatteringTexture,
    multipleScatteringTexture,
    gl_FragCoord.xy,
    scatteringOrder
  );
  irradiance = luminanceFromRadiance * deltaIrradiance;
  outputColor = vec4(OUTPUT, 1.0);
}
#endif // WOLF_TK_PRECOMPUTE == 5

#if WOLF_TK_PRECOMPUTE == 6
// Source: three-geospatial packages/atmosphere/src/shaders/precompute/multipleScattering.frag:9-34

uniform mat3 luminanceFromRadiance;
uniform sampler2D transmittanceTexture;
uniform sampler3D scatteringDensityTexture;
uniform int layer;

layout(location = 0) out vec4 outputColor;

// Source: three-geospatial packages/atmosphere/src/shaders/precompute/multipleScattering.frag:17 main
void main() {
  vec4 deltaMultipleScattering;
  vec4 scattering;
  float nu;
  deltaMultipleScattering.rgb = ComputeMultipleScatteringTexture(
    ATMOSPHERE,
    transmittanceTexture,
    scatteringDensityTexture,
    vec3(gl_FragCoord.xy, float(layer) + 0.5),
    nu
  );
  deltaMultipleScattering.a = 1.0;
  scattering = vec4(
    luminanceFromRadiance * deltaMultipleScattering.rgb / RayleighPhaseFunction(nu),
    0.0
  );
  outputColor = OUTPUT;
}
#endif // WOLF_TK_PRECOMPUTE == 6

