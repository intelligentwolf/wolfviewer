/**
 * @file wolftk/aerialPerspective.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:16-269 (uniforms and functions; main is wolftk compositeF)
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
 */

#ifndef WOLF_TK_AERIALPERSPECTIVE_GLSL
#define WOLF_TK_AERIALPERSPECTIVE_GLSL

// Uniforms and functions of aerialPerspectiveEffect.frag (the includer writes main and declares the varying
// `vCameraPosition`, ECEF km, as aerialPerspectiveEffect.vert:44-45 makes it). Not translated:
//   readNormal (79-87): WolfViewer reads LL's G-buffer normals itself;
//   correctGeometricError (89-100, CORRECT_GEOMETRIC_ERROR): it blends ellipsoid normals/positions toward an
//   ideal sphere for globe views; WolfViewer's planet IS a sphere of bottom_radius (worldToECEFMatrix maps the
//   region onto it), so the correction is the identity and the define stays off.

precision highp sampler2DArray;

// Source: aerialPerspectiveEffect.frag:3-14 (includes); threeCommon for PI2 / RECIPROCAL_PI, which the
// postprocessing Effect framework prepends from three.js <common>; threePacking for viewZToOrthographicDepth.
#wolf_include "wolftk/threeCommon.glsl"
#wolf_include "wolftk/threePacking.glsl"
#wolf_include "wolftk/coreDepth.glsl"
#wolf_include "wolftk/coreMath.glsl"
#wolf_include "wolftk/coreTransform.glsl"
#ifdef HAS_SHADOW
#wolf_include "wolftk/coreRaySphereIntersection.glsl"
#wolf_include "wolftk/coreCascadedShadowMaps.glsl"
#wolf_include "wolftk/coreInterleavedGradientNoise.glsl"
#wolf_include "wolftk/coreVogelDisk.glsl"
#endif // HAS_SHADOW

#wolf_include "wolftk/brunetonDefinitions.glsl"
#wolf_include "wolftk/atmosphereUniforms.glsl"
#wolf_include "wolftk/brunetonCommon.glsl"
#wolf_include "wolftk/brunetonRuntime.glsl"

// The postprocessing Effect framework's own uniforms used below (resolution :261, cameraNear :228; cameraFar
// beside it), declared here once.
#ifndef WOLF_TK_EFFECT_UNIFORMS
#define WOLF_TK_EFFECT_UNIFORMS
uniform vec2 resolution;
uniform float cameraNear;
uniform float cameraFar;
#endif

uniform sampler2D normalBuffer;

uniform mat4 projectionMatrix;
uniform mat4 viewMatrix;
uniform mat4 inverseProjectionMatrix;
uniform mat4 inverseViewMatrix;
uniform float bottomRadius;
uniform mat4 worldToECEFMatrix;
uniform float geometricErrorCorrectionAmount;
uniform vec3 sunDirection;
uniform float cosSunAngularRadius;
uniform vec3 moonDirection;
uniform float moonAngularRadius;
uniform float lunarRadianceScale;
uniform float albedoScale;

// Source: aerialPerspectiveEffect.frag:45 #include "sky"
#wolf_include "wolftk/sky.glsl"

#ifdef HAS_LIGHTING_MASK
uniform sampler2D lightingMaskBuffer;
#endif // HAS_LIGHTING_MASK

// prettier-ignore
#define LIGHTING_MASK_CHANNEL_ LIGHTING_MASK_CHANNEL

#ifdef HAS_OVERLAY
uniform sampler2D overlayBuffer;
#endif // HAS_OVERLAY

#ifdef HAS_SHADOW
uniform sampler2DArray shadowBuffer;
uniform vec2 shadowIntervals[SHADOW_CASCADE_COUNT];
uniform mat4 shadowMatrices[SHADOW_CASCADE_COUNT];
uniform mat4 inverseShadowMatrices[SHADOW_CASCADE_COUNT];
uniform float shadowFar;
uniform float shadowTopHeight;
uniform float shadowRadius;
// [WolfViewer] NVIDIA STBN dropped (its licence is research/evaluation only, or NVIDIA's RTX SDK
// licence; this is a commercial product): getSTBN() below uses takram's MIT interleaved gradient
// noise with the time seed of packages/core/src/webgpu/generators.ts:20-25 (dithering). Source line 66
// declared `uniform sampler3D stbnTexture;`, line 67 `uniform int frame;`.
#ifndef WOLF_TK_TIME
uniform float time;
#define WOLF_TK_TIME time
#endif
#endif // HAS_SHADOW

#ifdef HAS_SHADOW_LENGTH
uniform sampler2D shadowLengthBuffer;
#endif // HAS_SHADOW_LENGTH


#if defined(SUN_LIGHT) || defined(SKY_LIGHT)

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:104 getSunSkyIrradiance
vec3 getSunSkyIrradiance(
  const vec3 positionECEF,
  const vec3 normal,
  const vec3 inputColor,
  const float sunTransmittance
) {
  // Assume lambertian BRDF. If both SUN_LIGHT and SKY_LIGHT are not defined,
  // regard the inputColor as radiance at the texel.
  vec3 diffuse = inputColor * albedoScale * RECIPROCAL_PI;
  vec3 skyIrradiance;
  vec3 sunIrradiance = GetSunAndSkyIrradiance(positionECEF, normal, sunDirection, skyIrradiance);

  #ifdef HAS_SHADOW
  sunIrradiance *= sunTransmittance;
  #endif // HAS_SHADOW

  #if defined(SUN_LIGHT) && defined(SKY_LIGHT)
  return diffuse * (sunIrradiance + skyIrradiance);
  #elif defined(SUN_LIGHT)
  return diffuse * sunIrradiance;
  #elif defined(SKY_LIGHT)
  return diffuse * skyIrradiance;
  #endif // defined(SUN_LIGHT) && defined(SKY_LIGHT)
}

#endif // defined(SUN_LIGHT) || defined(SKY_LIGHT)

#if defined(TRANSMITTANCE) || defined(INSCATTER)

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:133 applyTransmittanceInscatter
void applyTransmittanceInscatter(const vec3 positionECEF, float shadowLength, inout vec3 radiance) {
  vec3 transmittance;
  vec3 inscatter = GetSkyRadianceToPoint(
    vCameraPosition,
    positionECEF,
    shadowLength,
    sunDirection,
    transmittance
  );
  #ifdef TRANSMITTANCE
  radiance = radiance * transmittance;
  #endif // TRANSMITTANCE
  #ifdef INSCATTER
  radiance = radiance + inscatter;
  #endif // INSCATTER
}

// [WolfViewer 2026-10-13] applyTransmittanceInscatter from the camera-relative vector (ECEF axes, length units) instead of
// the point's absolute ECEF position: the ray and the distance exact (brunetonRuntime.glsl wolfSkyRadianceToPointRel)
void wolfApplyTransmittanceInscatterRel(const vec3 relECEF, float shadowLength, inout vec3 radiance) {
  vec3 transmittance;
  float d = length(relECEF);
  if (d <= 0.0) return;
  vec3 inscatter = wolfSkyLuminanceToPointRel(
    vCameraPosition,
    relECEF / d,
    d,
    shadowLength,
    sunDirection,
    transmittance
  );
  #ifdef TRANSMITTANCE
  radiance = radiance * transmittance;
  #endif // TRANSMITTANCE
  #ifdef INSCATTER
  radiance = radiance + inscatter;
  #endif // INSCATTER
}

#endif // defined(TRANSMITTANCE) || defined(INSCATTER)

#ifdef HAS_SHADOW

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:154 getSTBN
float getSTBN() {
  // [WolfViewer] Source lines 155-157 sampled the STBN texture; replaced (see the uniforms above) by
  // three-geospatial packages/core/src/webgpu/generators.ts:20-25 dithering's seed into
  // packages/core/src/shaders/interleavedGradientNoise.glsl:3-6 interleavedGradientNoise.
#ifdef WOLF_TK_NO_TEMPORAL_FILTER
  // [WolfViewer] the noise rotates the shadow's Vogel disk per pixel and frame for a temporal filter to
  // average; LL's scene passes have none after them, where it only shows as moving grain: one rotation
  // for every pixel instead (atmosphereLibF.glsl defines this)
  return 0.5;
#else
  return interleavedGradientNoise(gl_FragCoord.xy + fract(WOLF_TK_TIME) * 1337.0);
#endif
}
// [WolfViewer] the PCF disk's rotation (source line 205: interleavedGradientNoise(gl_FragCoord.xy) * PI2, a
// per-pixel dither for a temporal filter to average): without one (WOLF_TK_NO_TEMPORAL_FILTER, the stock
// scene passes) one rotation for every pixel, so the shadow's edge is smooth rather than a screen-fixed grain.
#ifdef WOLF_TK_NO_TEMPORAL_FILTER
#define WOLF_TK_PCF_PHI 0.0
#else
#define WOLF_TK_PCF_PHI (interleavedGradientNoise(gl_FragCoord.xy) * PI2)
#endif

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:160 getShadowUv
vec2 getShadowUv(const vec3 worldPosition, const int cascadeIndex) {
  vec4 clip = shadowMatrices[cascadeIndex] * vec4(worldPosition, 1.0);
  clip /= clip.w;
  return clip.xy * 0.5 + 0.5;
}

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:166 getDistanceToShadowTop
float getDistanceToShadowTop(const vec3 positionECEF) {
  // Distance to the top of the shadows along the sun direction, which matches
  // the ray origin of BSM.
  return raySphereSecondIntersection(
    positionECEF / METER_TO_LENGTH_UNIT, // Source note: Make units consistent
    sunDirection,
    vec3(0.0),
    bottomRadius + shadowTopHeight
  );
}

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:177 readShadowOpticalDepth
float readShadowOpticalDepth(const vec2 uv, const float distanceToTop, const int cascadeIndex) {
  // r: frontDepth, g: meanExtinction, b: maxOpticalDepth, a: maxOpticalDepthTail
  vec4 shadow = texture(shadowBuffer, vec3(uv, float(cascadeIndex)));
  // Omit adding maxOpticalDepthTail to avoid pronounced aliasing. Ground
  // shadow will be attenuated by inscatter anyways.
  return min(shadow.b, shadow.g * max(0.0, distanceToTop - shadow.r));
}

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:185 sampleShadowOpticalDepthPCF
float sampleShadowOpticalDepthPCF(
  const vec3 worldPosition,
  const float distanceToTop,
  const float radius,
  const int cascadeIndex
) {
  vec2 uv = getShadowUv(worldPosition, cascadeIndex);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return 0.0;
  }

  vec2 texelSize = vec2(1.0) / vec2(textureSize(shadowBuffer, 0).xy);
  float sum = 0.0;
  vec2 offset;
  
    #if 0 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      0,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 0 < SHADOW_SAMPLE_COUNT
  
    #if 1 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      1,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 1 < SHADOW_SAMPLE_COUNT
  
    #if 2 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      2,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 2 < SHADOW_SAMPLE_COUNT
  
    #if 3 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      3,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 3 < SHADOW_SAMPLE_COUNT
  
    #if 4 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      4,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 4 < SHADOW_SAMPLE_COUNT
  
    #if 5 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      5,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 5 < SHADOW_SAMPLE_COUNT
  
    #if 6 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      6,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 6 < SHADOW_SAMPLE_COUNT
  
    #if 7 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      7,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 7 < SHADOW_SAMPLE_COUNT
  
    #if 8 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      8,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 8 < SHADOW_SAMPLE_COUNT
  
    #if 9 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      9,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 9 < SHADOW_SAMPLE_COUNT
  
    #if 10 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      10,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 10 < SHADOW_SAMPLE_COUNT
  
    #if 11 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      11,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 11 < SHADOW_SAMPLE_COUNT
  
    #if 12 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      12,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 12 < SHADOW_SAMPLE_COUNT
  
    #if 13 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      13,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 13 < SHADOW_SAMPLE_COUNT
  
    #if 14 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      14,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 14 < SHADOW_SAMPLE_COUNT
  
    #if 15 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      15,
      SHADOW_SAMPLE_COUNT,
      WOLF_TK_PCF_PHI
    );
    sum += readShadowOpticalDepth(uv + offset * radius * texelSize, distanceToTop, cascadeIndex);
    #endif // 15 < SHADOW_SAMPLE_COUNT
  
  return sum / float(SHADOW_SAMPLE_COUNT);
}

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:214 sampleShadowOpticalDepth
float sampleShadowOpticalDepth(
  const vec3 worldPosition,
  const vec3 positionECEF,
  const float radius,
  const float jitter
) {
  float distanceToTop = getDistanceToShadowTop(positionECEF);
  if (distanceToTop <= 0.0) {
    return 0.0;
  }
  int cascadeIndex = getFadedCascadeIndex(
    viewMatrix,
    worldPosition,
    shadowIntervals,
    cameraNear,
    shadowFar,
    jitter
  );
  return cascadeIndex >= 0
    ? sampleShadowOpticalDepthPCF(worldPosition, distanceToTop, radius, cascadeIndex)
    : 0.0;
}

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:237 getShadowRadius
float getShadowRadius(const vec3 worldPosition) {
  vec4 clip = shadowMatrices[0] * vec4(worldPosition, 1.0);
  clip /= clip.w;

  // Offset by 1px in each direction in shadow's clip coordinates.
  vec2 shadowSize = vec2(textureSize(shadowBuffer, 0));
  vec3 offset = vec3(2.0 / shadowSize, 0.0);
  vec4 clipX = clip + offset.xzzz;
  vec4 clipY = clip + offset.zyzz;

  // Convert back to world space.
  vec4 worldX = inverseShadowMatrices[0] * clipX;
  vec4 worldY = inverseShadowMatrices[0] * clipY;

  // Project into the main camera's clip space.
  mat4 viewProjectionMatrix = projectionMatrix * viewMatrix;
  vec4 projected = viewProjectionMatrix * vec4(worldPosition, 1.0);
  vec4 projectedX = viewProjectionMatrix * worldX;
  vec4 projectedY = viewProjectionMatrix * worldY;
  projected /= projected.w;
  projectedX /= projectedX.w;
  projectedY /= projectedY.w;

  // Take the mean of pixel sizes.
  vec2 center = (projected.xy * 0.5 + 0.5) * resolution;
  vec2 offsetX = (projectedX.xy * 0.5 + 0.5) * resolution;
  vec2 offsetY = (projectedY.xy * 0.5 + 0.5) * resolution;
  float size = max(length(offsetX - center), length(offsetY - center));

  return remapClamped(size, 10.0, 50.0, 0.0, shadowRadius);
}

#endif // HAS_SHADOW
#endif // WOLF_TK_AERIALPERSPECTIVE_GLSL
