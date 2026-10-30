/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/clouds.frag.
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

// The `#pragma unroll_loop_start/end` loops of the source are written out unrolled here, exactly as
// three-geospatial packages/core/src/unrollLoops.ts:6-23 (loopReplacer, unrollLoops) expands them before compilation.

precision highp float;
precision highp sampler3D;
precision highp sampler2DArray;

#wolf_include "wolftk/threeCommon.glsl"   // takram: #include <common> (line 5)
#wolf_include "wolftk/threePacking.glsl"   // takram: #include <packing> (line 6)

#wolf_include "wolftk/coreDepth.glsl"   // takram: #include "core/depth" (line 8)
#wolf_include "wolftk/coreMath.glsl"   // takram: #include "core/math" (line 9)
#wolf_include "wolftk/coreTurbo.glsl"   // takram: #include "core/turbo" (line 10)
#wolf_include "wolftk/coreGenerators.glsl"   // takram: #include "core/generators" (line 11)
#wolf_include "wolftk/coreRaySphereIntersection.glsl"   // takram: #include "core/raySphereIntersection" (line 12)
#wolf_include "wolftk/coreCascadedShadowMaps.glsl"   // takram: #include "core/cascadedShadowMaps" (line 13)
#wolf_include "wolftk/coreInterleavedGradientNoise.glsl"   // takram: #include "core/interleavedGradientNoise" (line 14)
#wolf_include "wolftk/coreVogelDisk.glsl"   // takram: #include "core/vogelDisk" (line 15)

#wolf_include "wolftk/brunetonDefinitions.glsl"   // takram: #include "atmosphere/bruneton/definitions" (line 17)

uniform AtmosphereParameters ATMOSPHERE;
uniform vec3 SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
uniform vec3 SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;

uniform sampler2D transmittance_texture;
uniform sampler3D scattering_texture;
uniform sampler2D irradiance_texture;
uniform sampler3D single_mie_scattering_texture;
uniform sampler3D higher_order_scattering_texture;

#wolf_include "wolftk/brunetonCommon.glsl"   // takram: #include "atmosphere/bruneton/common" (line 29)
#wolf_include "wolftk/brunetonRuntime.glsl"   // takram: #include "atmosphere/bruneton/runtime" (line 30)

#wolf_include "wolftk/cloudsTypes.glsl"   // takram: #include "types" (line 32)
#wolf_include "wolftk/cloudsParameters.glsl"   // takram: #include "parameters" (line 33)
#wolf_include "wolftk/cloudsLight.glsl"   // [WolfViewer round 8] the sun or the moon as the clouds' light
#wolf_include "wolftk/cloudsCommon.glsl"   // takram: #include "clouds" (line 34)
#wolf_include "wolftk/nightLight.glsl"   // [WolfViewer 2026-10-25] the night's light as the eye sees it (Jimmy Olsen, #226)

#if !defined(RECIPROCAL_PI4)
#define RECIPROCAL_PI4 0.07957747154594767
#endif // !defined(RECIPROCAL_PI4)

uniform sampler2D depthBuffer;
uniform mat4 viewMatrix;
uniform mat4 reprojectionMatrix;
uniform mat4 viewReprojectionMatrix;
uniform float cameraNear;
uniform float cameraFar;
uniform float cameraHeight;
uniform vec2 temporalJitter;
uniform vec2 targetUvScale;
uniform float mipLevelScale;

// Scattering
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:52-52 scatterAnisotropy
const vec2 scatterAnisotropy = vec2(SCATTER_ANISOTROPY_1, SCATTER_ANISOTROPY_2);
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:53-53 scatterAnisotropyMix
const float scatterAnisotropyMix = SCATTER_ANISOTROPY_MIX;
uniform float skyLightScale;
// [WolfViewer] the horizontal sky irradiance under the clouds / the clear sky's at the camera (wolfenvironmentrender.cpp
// updateLight; 1 with no clouds): the haze below a deck is lit by the sky below the deck (approximateHaze)
uniform vec3 wolfSkyUnderCloud;
// [WolfViewer 2026-10-13] the hue of LL's own cloud light - cloud_color x (its sun + ambient light, cloudsV.glsl:174-175) -
// over the hue of the physical light the march gathers (wolfenvironmentrender.cpp updateLight), as tint - 1 (0 untinted;
// Atmosphere::setRuntimeUniforms)
uniform vec3 wolfEepCloudTint;
// [WolfViewer 2026-10-13, round 16] how much thinner than marched the clouds are drawn (0 = as marched): the stock EEP clouds' own
// opacity, measured (wolfenvironmentrender.cpp measureCloudCover against renderStockCloudProbe)
uniform float wolfEepCloudThin;
// [WolfViewer 2026-10-27] 1: the clouds take the SKY's per-direction gain (wolfEepGainDir) instead of gains toward the stock EEP
// cloud colour - Wolf weather by day (wolfenvironmentrender.cpp measureCloudCover; TROMSO 09:30Z, the stock clouds the sky's own
// colour). Used only while wolfEepCloudGainOn is 0.
uniform float wolfEepCloudSkyGain;
// [WolfViewer 2026-10-13] the clouds' colour gains toward the stock EEP clouds, per direction (the sky's grid,
// brunetonRuntime.glsl wolfGainGrid; measured, wolfenvironmentrender.cpp measureCloudCover). Off: 1.
// [WolfViewer 2026-10-25] the sky's 7 x 11 grid round the stock light (brunetonRuntime.glsl, Jimmy Olsen's seam), its azimuths
// from wolfEepCloudGainRef (ECEF): the light the cloud gains were measured round
// (wolfEepCloudGain itself: brunetonRuntime.glsl's WolfEepGains block)
uniform vec3 wolfEepCloudGainRef;
uniform float wolfEepCloudGainOn;
uniform float wolfHazeAlways;   // [WolfViewer 2026-10-13] 1: the haze ignores the cloud cover (approximateHaze)
// [WolfViewer 2026-10-13] the stock EEP haze pass, evaluated per pixel (wolfenvironmentrender.cpp cloudsFrame,
// WolfEepReference::HazeParams; [S] atmosphericsFuncs.glsl:51-131, hazeF.glsl:79-100). Directions ECEF.
uniform float wolfLLHazeOn;
uniform vec3 wolfLLBlueW;      // blue_horizon x blue_weight
uniform vec3 wolfLLHazeW;      // haze_horizon x haze_weight
uniform vec3 wolfLLCs;         // the light through the stock air, x (1 - cloud_shadow)
uniform vec3 wolfLLTmpAmb;     // ambient lifted by cloud_shadow
uniform vec3 wolfLLCombined;   // blue_density + haze_density
uniform vec3 wolfLLGlow;       // glow.x, glow.z, sun_moon_glow_factor
uniform vec3 wolfLLHazeScale;  // density_multiplier x distance_multiplier, max_y, sky_hdr_scale
uniform vec3 wolfLLLightnorm;  // the stock light norm (not unit length: as the stock code uses it)
uniform vec3 wolfLLLightDir;   // the sun's (or the moon's) direction
float wolfSrgbToLinear1(float x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }   // [S] srgbF.glsl
// the stock haze in front of a surface d metres along `ray` (unit, ECEF) seen from `camera` (ECEF): what it adds (rgb) and
// the share of the surface it hides (the hazeF.glsl blend: out = lit x atten.r + srgb_to_linear(2 additive) x hdr)
vec4 wolfLLHaze(const vec3 camera, const vec3 ray, const float d)
{
  vec3 up = normalize(camera);
  float y = d * dot(ray, up);
  float len = d * (abs(y) > wolfLLHazeScale.y ? wolfLLHazeScale.y / abs(y) : 1.0);   // :57 |rel.y| held to max_y
  vec3 atten = exp(-wolfLLCombined * len * wolfLLHazeScale.x);                       // :79-87
  float g = dot(ray, wolfLLLightnorm) * max(0.0, dot(wolfLLLightDir, ray));          // :90-95
  g = max(1.0 - g, 0.001) * wolfLLGlow.x;
  g = (clamp(pow(g, wolfLLGlow.y), -100000.0, 100000.0) + 0.25) * wolfLLGlow.z;     // :97-108
  vec3 additive = wolfLLBlueW * (wolfLLCs + wolfLLTmpAmb) + wolfLLHazeW * (wolfLLCs * g + wolfLLTmpAmb);   // :120
  additive = min(additive * (vec3(1.0) - atten), vec3(10.0));                        // :127-130
  vec3 a2 = additive * 2.0;
  vec3 rgb = vec3(wolfSrgbToLinear1(a2.r), wolfSrgbToLinear1(a2.g), wolfSrgbToLinear1(a2.b)) * wolfLLHazeScale.z;
  return vec4(rgb, 1.0 - atten.r);
}
uniform float groundBounceScale;
uniform float powderScale;
uniform float powderExponent;

// Primary raymarch
uniform int maxIterationCount;
uniform float minStepSize;
uniform float maxStepSize;
uniform float maxRayDistance;
uniform float perspectiveStepScale;

// Secondary raymarch
uniform int maxIterationCountToSun;
uniform int maxIterationCountToGround;
uniform float minSecondaryStepSize;
uniform float secondaryStepScale;

// Beer shadow map
uniform sampler2DArray shadowBuffer;
uniform vec2 shadowTexelSize;
uniform vec2 shadowIntervals[SHADOW_CASCADE_COUNT];
uniform mat4 shadowMatrices[SHADOW_CASCADE_COUNT];
uniform float shadowFar;
uniform float maxShadowFilterRadius;

// Shadow length
#ifdef SHADOW_LENGTH
uniform int maxShadowLengthIterationCount;
uniform float minShadowLengthStepSize;
uniform float maxShadowLengthRayDistance;
#endif // SHADOW_LENGTH

in vec2 vUv;
in vec3 vCameraPosition;
in vec3 vCameraDirection; // Direction to the center of screen
in vec3 vRayDirection; // Direction to the texel
in vec3 vViewPosition;
in GroundIrradiance vGroundIrradiance;
in CloudsIrradiance vCloudsIrradiance;

layout(location = 0) out vec4 outputColor;
layout(location = 1) out vec4 outputDepthVelocity;   // [WolfViewer round 13 / 2026-10-16] .a = the view Z of the surface the ray stopped at (< 0), or >= 0 over the sky (main)
#ifdef SHADOW_LENGTH
layout(location = 2) out float outputShadowLength;
#endif // SHADOW_LENGTH

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:101-107 getViewZ
float getViewZ(const float depth) {
  #ifdef PERSPECTIVE_CAMERA
  return perspectiveDepthToViewZ(depth, cameraNear, cameraFar);
  #else // PERSPECTIVE_CAMERA
  return orthographicDepthToViewZ(depth, cameraNear, cameraFar);
  #endif // PERSPECTIVE_CAMERA
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:109-111 ecefToWorld
vec3 ecefToWorld(const vec3 positionECEF) {
  return (ecefToWorldMatrix * vec4(positionECEF - altitudeCorrection, 1.0)).xyz;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:113-117 getShadowUv
vec2 getShadowUv(const vec3 worldPosition, const int cascadeIndex) {
  vec4 clip = shadowMatrices[cascadeIndex] * vec4(worldPosition, 1.0);
  clip /= clip.w;
  return clip.xy * 0.5 + 0.5;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:119-128 getDistanceToShadowTop
float getDistanceToShadowTop(const vec3 rayPosition) {
  // Distance to the top of the shadows along the sun direction, which matches
  // the ray origin of BSM.
  return raySphereSecondIntersection(
    rayPosition,
    sunDirection,
    vec3(0.0),
    bottomRadius + shadowTopHeight
  );
}

#ifdef DEBUG_SHOW_CASCADES

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:132-137 cascadeColors
const vec3 cascadeColors[4] = vec3[4](
  vec3(1.0, 0.0, 0.0),
  vec3(0.0, 1.0, 0.0),
  vec3(0.0, 0.0, 1.0),
  vec3(1.0, 1.0, 0.0)
);

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:139-153 getCascadeColor
vec3 getCascadeColor(const vec3 rayPosition) {
  vec3 worldPosition = ecefToWorld(rayPosition);
  int cascadeIndex = getCascadeIndex(
    viewMatrix,
    worldPosition,
    shadowIntervals,
    cameraNear,
    shadowFar
  );
  vec2 uv = getShadowUv(worldPosition, cascadeIndex);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return vec3(1.0);
  }
  return cascadeColors[cascadeIndex];
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:155-168 getFadedCascadeColor
vec3 getFadedCascadeColor(const vec3 rayPosition, const float jitter) {
  vec3 worldPosition = ecefToWorld(rayPosition);
  int cascadeIndex = getFadedCascadeIndex(
    viewMatrix,
    worldPosition,
    shadowIntervals,
    cameraNear,
    shadowFar,
    jitter
  );
  return cascadeIndex >= 0
    ? cascadeColors[cascadeIndex]
    : vec3(1.0);
}

#endif // DEBUG_SHOW_CASCADES

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:172-183 readShadowOpticalDepth
float readShadowOpticalDepth(
  const vec2 uv,
  const float distanceToTop,
  const float distanceOffset,
  const int cascadeIndex
) {
  // r: frontDepth, g: meanExtinction, b: maxOpticalDepth, a: maxOpticalDepthTail
  // Also see the discussion here: https://x.com/shotamatsuda/status/1885322308908442106
  vec4 shadow = texture(shadowBuffer, vec3(uv, float(cascadeIndex)));
  float distanceToFront = max(0.0, distanceToTop - distanceOffset - shadow.r);
  return min(shadow.b + shadow.a, shadow.g * distanceToFront);
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:185-219 sampleShadowOpticalDepthPCF
float sampleShadowOpticalDepthPCF(
  const vec3 worldPosition,
  const float distanceToTop,
  const float distanceOffset,
  const float radius,
  const int cascadeIndex
) {
  vec2 uv = getShadowUv(worldPosition, cascadeIndex);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return 0.0;
  }
  if (radius < 0.1) {
    return readShadowOpticalDepth(uv, distanceToTop, distanceOffset, cascadeIndex);
  }
  float sum = 0.0;
  vec2 offset;
  
    #if 0 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      0,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 0 < SHADOW_SAMPLE_COUNT
  
    #if 1 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      1,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 1 < SHADOW_SAMPLE_COUNT
  
    #if 2 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      2,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 2 < SHADOW_SAMPLE_COUNT
  
    #if 3 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      3,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 3 < SHADOW_SAMPLE_COUNT
  
    #if 4 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      4,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 4 < SHADOW_SAMPLE_COUNT
  
    #if 5 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      5,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 5 < SHADOW_SAMPLE_COUNT
  
    #if 6 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      6,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 6 < SHADOW_SAMPLE_COUNT
  
    #if 7 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      7,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 7 < SHADOW_SAMPLE_COUNT
  
    #if 8 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      8,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 8 < SHADOW_SAMPLE_COUNT
  
    #if 9 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      9,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 9 < SHADOW_SAMPLE_COUNT
  
    #if 10 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      10,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 10 < SHADOW_SAMPLE_COUNT
  
    #if 11 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      11,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 11 < SHADOW_SAMPLE_COUNT
  
    #if 12 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      12,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 12 < SHADOW_SAMPLE_COUNT
  
    #if 13 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      13,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 13 < SHADOW_SAMPLE_COUNT
  
    #if 14 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      14,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 14 < SHADOW_SAMPLE_COUNT
  
    #if 15 < SHADOW_SAMPLE_COUNT
    offset = vogelDisk(
      15,
      SHADOW_SAMPLE_COUNT,
      interleavedGradientNoise(gl_FragCoord.xy + temporalJitter * resolution) * PI2
    );
    sum += readShadowOpticalDepth(
      uv + offset * radius * shadowTexelSize,
      distanceToTop,
      distanceOffset,
      cascadeIndex
    );
    #endif // 15 < SHADOW_SAMPLE_COUNT
  
  return sum / float(SHADOW_SAMPLE_COUNT);
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:221-249 sampleShadowOpticalDepth
float sampleShadowOpticalDepth(
  const vec3 rayPosition,
  const float distanceOffset,
  const float radius,
  const float jitter
) {
  float distanceToTop = getDistanceToShadowTop(rayPosition);
  if (distanceToTop <= 0.0) {
    return 0.0;
  }
  vec3 worldPosition = ecefToWorld(rayPosition);
  int cascadeIndex = getFadedCascadeIndex(
    viewMatrix,
    worldPosition,
    shadowIntervals,
    cameraNear,
    shadowFar,
    jitter
  );
  return cascadeIndex >= 0
    ? sampleShadowOpticalDepthPCF(
      worldPosition,
      distanceToTop,
      distanceOffset,
      radius,
      cascadeIndex
    )
    : 0.0;
}

#ifdef DEBUG_SHOW_SHADOW_MAP
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:252-295 getCascadedShadowMaps
vec4 getCascadedShadowMaps(vec2 uv) {
  vec4 coord = vec4(vUv, vUv - 0.5) * 2.0;
  vec4 shadow = vec4(0.0);
  if (uv.y > 0.5) {
    if (uv.x < 0.5) {
      shadow = texture(shadowBuffer, vec3(coord.xw, 0.0));
    } else {
      #if SHADOW_CASCADE_COUNT > 1
      shadow = texture(shadowBuffer, vec3(coord.zw, 1.0));
      #endif // SHADOW_CASCADE_COUNT > 1
    }
  } else {
    if (uv.x < 0.5) {
      #if SHADOW_CASCADE_COUNT > 2
      shadow = texture(shadowBuffer, vec3(coord.xy, 2.0));
      #endif // SHADOW_CASCADE_COUNT > 2
    } else {
      #if SHADOW_CASCADE_COUNT > 3
      shadow = texture(shadowBuffer, vec3(coord.zy, 3.0));
      #endif // SHADOW_CASCADE_COUNT > 3
    }
  }

  #if !defined(DEBUG_SHOW_SHADOW_MAP_TYPE)
  #define DEBUG_SHOW_SHADOW_MAP_TYPE 0
  #endif // !defined(DEBUG_SHOW_SHADOW_MAP_TYPE

  const float frontDepthScale = 1e-5;
  const float meanExtinctionScale = 10.0;
  const float maxOpticalDepthScale = 0.01;
  vec3 color;
  #if DEBUG_SHOW_SHADOW_MAP_TYPE == 1
  color = vec3(shadow.r * frontDepthScale);
  #elif DEBUG_SHOW_SHADOW_MAP_TYPE == 2
  color = vec3(shadow.g * meanExtinctionScale);
  #elif DEBUG_SHOW_SHADOW_MAP_TYPE == 3
  color = vec3((shadow.b + shadow.a) * maxOpticalDepthScale);
  #else // DEBUG_SHOW_SHADOW_MAP_TYPE
  color =
    (shadow.rgb + vec3(0.0, 0.0, shadow.a)) *
    vec3(frontDepthScale, meanExtinctionScale, maxOpticalDepthScale);
  #endif // DEBUG_SHOW_SHADOW_MAP_TYPE
  return vec4(color, 1.0);
}
#endif // DEBUG_SHOW_SHADOW_MAP

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:298-303 henyeyGreenstein
vec2 henyeyGreenstein(const vec2 g, const float cosTheta) {
  vec2 g2 = g * g;
  // prettier-ignore
  return RECIPROCAL_PI4 *
    ((1.0 - g2) / max(vec2(1e-7), pow(1.0 + g2 - 2.0 * g * cosTheta, vec2(1.5))));
}

#ifdef ACCURATE_PHASE_FUNCTION

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:307-313 draine
float draine(float u, float g, float a) {
  float g2 = g * g;
  // prettier-ignore
  return (1.0 - g2) *
    (1.0 + a * u * u) /
    (4.0 * (1.0 + a * (1.0 + 2.0 * g2) / 3.0) * PI * pow(1.0 + g2 - 2.0 * g * u, 1.5));
}

// Numerically-fitted large particles (d=10) phase function It won't be
// plausible without a more precise multiple scattering.
// Reference: https://research.nvidia.com/labs/rtr/approximate-mie/
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:318-328 phaseFunction
float phaseFunction(const float cosTheta, const float attenuation) {
  const float gHG = 0.988176691700256; // exp(-0.0990567/(d-1.67154))
  const float gD = 0.5556712547839497; // exp(-2.20679/(d+3.91029) - 0.428934)
  const float alpha = 21.995520856274638; // exp(3.62489 - 8.29288/(d+5.52825))
  const float weight = 0.4819554318404214; // exp(-0.599085/(d-0.641583)-0.665888)
  return mix(
    henyeyGreenstein(vec2(gHG) * attenuation, cosTheta).x,
    draine(cosTheta, gD * attenuation, alpha),
    weight
  );
}

#else // ACCURATE_PHASE_FUNCTION

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:332-338 phaseFunction
float phaseFunction(const float cosTheta, const float attenuation, const float ice) {
  // [WolfViewer] ice: the forward lobe's asymmetry scaled by WOLF_ICE_G_SCALE (wolfcloudgenus.h)
  vec2 g = scatterAnisotropy * vec2(mix(1.0, WOLF_ICE_G_SCALE, ice), 1.0);
  const vec2 weights = vec2(1.0 - scatterAnisotropyMix, scatterAnisotropyMix);
  // A similar approximation is described in the Frostbite's paper, where phase
  // angle is attenuated instead of anisotropy.
  return dot(henyeyGreenstein(g * attenuation, cosTheta), weights);
}

#endif // ACCURATE_PHASE_FUNCTION

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:342-344 phaseFunction
float phaseFunction(const float cosTheta, const float attenuation) {
  return phaseFunction(cosTheta, attenuation, 0.0);
}

float phaseFunction(const float cosTheta) {
  return phaseFunction(cosTheta, 1.0);
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:346-377 marchOpticalDepth
float marchOpticalDepth(
  const vec3 rayOrigin,
  const vec3 rayDirection,
  const int maxIterationCount,
  const float mipLevel,
  const float jitter,
  out float rayDistance
) {
  int iterationCount = int(
    max(0.0, remap(mipLevel, 0.0, 1.0, float(maxIterationCount + 1), 1.0) - jitter)
  );
  if (iterationCount == 0) {
    // Fudge factor to approximate the mean optical depth.
    // Source note: Remove it.
    return 0.5;
  }
  float stepSize = minSecondaryStepSize / float(iterationCount);
  float nextDistance = stepSize * jitter;
  float opticalDepth = 0.0;
  for (int i = 0; i < iterationCount; ++i) {
    rayDistance = nextDistance;
    vec3 position = rayDistance * rayDirection + rayOrigin;
    vec2 uv = getGlobeUv(position);
    float height = length(position) - bottomRadius;
    WeatherSample weather = sampleWeather(uv, height, mipLevel, position);
    MediaSample media = sampleMedia(weather, position, uv, mipLevel, jitter);
    opticalDepth += media.extinction * stepSize;
    nextDistance += stepSize;
    stepSize *= secondaryStepScale;
  }
  return opticalDepth;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:379-395 marchOpticalDepth
float marchOpticalDepth(
  const vec3 rayOrigin,
  const vec3 rayDirection,
  const int maxIterationCount,
  const float mipLevel,
  const float jitter
) {
  float rayDistance;
  return marchOpticalDepth(
    rayOrigin,
    rayDirection,
    maxIterationCount,
    mipLevel,
    jitter,
    rayDistance
  );
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:397-415 approximateMultipleScattering
// [WolfViewer 2026-10-26] msWeight: the share of the octaves after the first (multiple scattering) that this sample keeps;
// 1 = takram's sum. See marchClouds (wolfMsWeight) for where it is less than 1.
float approximateMultipleScattering(const float opticalDepth, const float cosTheta, const float ice, const float msWeight) {
  // Multiple scattering approximation
  // See: https://fpsunflower.github.io/ckulla/data/oz_volumes.pdf
  // a: attenuation, b: contribution, c: phase attenuation
  vec3 coeffs = vec3(1.0); // [a, b, c]
  // [WolfViewer] a = b = c = WOLF_MS_ATTENUATION, from the cloud albedo (see gen_clouds_shaders.py); 0.5 = takram's
  const vec3 attenuation = vec3(WOLF_MS_ATTENUATION); // Should satisfy a <= b
  float scattering = 0.0;
  float beerLambert;
  float singleScattering = 0.0;   // [WolfViewer 2026-10-26] the first octave, kept whole
  
    #if 0 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    singleScattering = scattering;
    #endif // 0 < MULTI_SCATTERING_OCTAVES
  
    #if 1 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 1 < MULTI_SCATTERING_OCTAVES
  
    #if 2 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 2 < MULTI_SCATTERING_OCTAVES
  
    #if 3 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 3 < MULTI_SCATTERING_OCTAVES
  
    #if 4 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 4 < MULTI_SCATTERING_OCTAVES
  
    #if 5 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 5 < MULTI_SCATTERING_OCTAVES
  
    #if 6 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 6 < MULTI_SCATTERING_OCTAVES
  
    #if 7 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 7 < MULTI_SCATTERING_OCTAVES
  
    #if 8 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 8 < MULTI_SCATTERING_OCTAVES
  
    #if 9 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 9 < MULTI_SCATTERING_OCTAVES
  
    #if 10 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 10 < MULTI_SCATTERING_OCTAVES
  
    #if 11 < MULTI_SCATTERING_OCTAVES
    beerLambert = exp(-opticalDepth * coeffs.y);
    scattering += coeffs.x * beerLambert * phaseFunction(cosTheta, coeffs.z, ice);
    coeffs *= attenuation;
    #endif // 11 < MULTI_SCATTERING_OCTAVES
  
  return singleScattering + msWeight * (scattering - singleScattering);
}

// [WolfViewer 2026-10-26] THE CORONA. A thin cloud of water droplets in front of the moon (or the sun) shows a bright bluish-white
// aureole round it with coloured rings, red outside: Fraunhofer diffraction by the droplets, "Smaller particles generally produce
// larger coronae" and it needs droplets "nearly uniform in size" (https://en.wikipedia.org/wiki/Corona_(optical_phenomenon);
// BEAUTY_RESEARCH.md 8.1); ice crystals give halos instead, never a corona (WMO: Cs "generally producing halo phenomena").
// The takram phase (two Henyey-Greenstein lobes 0.7 / -0.2) is a smooth fit with ~1 sr^-1 at the light, where a droplet's
// forward diffraction peak is hundreds: with it the cloud edges round the moon came out as dull as anywhere else.
// Physics: for a sphere of size parameter x = pi d / lambda >> 1, diffraction carries half the extinction ("extinction paradox",
// van de Hulst 1957, Light Scattering by Small Particles, section 8.22) in the Airy pattern [2 J1(u) / u]^2, u = x sin(theta);
// normalised over the sphere (int 2 pi theta dtheta [2 J1(x theta)/(x theta)]^2 = 4 pi / x^2) its phase function is
// p(theta) = x^2 / (4 pi) [2 J1(u) / u]^2. J1 from Bessel's integral, DLMF 10.9.2: J1(z) = (1/pi) int_0^pi cos(z sin t - t) dt,
// summed by the trapezoid rule on [0, pi] with 20 intervals - the integrand extends to a smooth periodic one, so this is the
// 40-point periodic rule, exact to ~J_41(z) (< 1e-6 for z < 24); its end points cancel (cos 0 + cos(-pi) = 0).
// d = 20 um: twice the effective radius 10 um the genus rows use for every water cloud (wolfcloudgenus.cpp sigmaWater), with a
// spread of 0.8 / 1 / 1.2 d (weights 1/4, 1/2, 1/4; a modelling choice: a narrow distribution, the case a corona is seen in),
// at the atmosphere's own RGB wavelengths 680 / 550 / 440 nm (wolftkatmosphere.cpp:164, Bruneton model.h kLambdaR/G/B). The
// first dark ring then sits at sin(theta) = 1.22 lambda / d = 1.9 deg (green), the colours outside it.
// Added on top of the takram phase only where the diffraction exceeds it (0.5 p - HG > 0: within ~4-5 deg of the light) and only
// to the single-scattered light (x exp(-optical depth to the light): multiple scattering washes a corona out, which is why only
// thin cloud shows one), outside the powder term (that models missing multiple in-scatter, not the forward peak). The sky away
// from the light is exactly as before; the energy added is the diffraction lobe the smooth fit lacks, within that cone.
// Computed once per pixel (the light's angle is the ray's), zero beyond 8 deg - the march itself only multiplies.
// [10-26 second pass] BY MOONLIGHT ONLY (the light is the moon when wolfLightScale is the moon's share, 0 < scale < 1;
// cloudsLight.glsl, wolfenvironmentrender.cpp cloudsFrame). By day the same diffraction exists round the sun, but there it sits
// inside the sun's own glare - the reason a solar corona is only seen with the sun hidden - which this renderer draws as the sun
// disc, the sky's Mie aureole and bloom; added on top it lit thin cloud edges near the low sun on top of that glare. Paul's two
// 10-26 frames were not this term (ICELAND 09:03Z: the cirrus, pure ice, which gets none; TROMSO 09:03Z: the smear on the far
// side of the frame from the sun's glow, outside the 8 deg cone), but by day it could only ever add to the sun's glare, so it is
// off by day.
// The cloud light is also not shadowed by terrain (no terrain shadow in the cloud march), so a moon behind a hill still lights
// clouds in front of it - true of all cloud light here, noted in CLOUD_SHAPE_PLAN.md section 8.
const float WOLF_CORONA_COS_MAX = 0.9902680687415704;   // cos(8 deg)
vec3 wolfCoronaExcess(const float cosTheta) {
  if (cosTheta < WOLF_CORONA_COS_MAX || !(wolfLightScale > 0.0 && wolfLightScale < 0.5)) {
    return vec3(0.0);
  }
  float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
  const vec3 lambdaUm = vec3(0.680, 0.550, 0.440);
  const float dUm = 20.0;
  const vec3 sizes = vec3(0.8, 1.0, 1.2);
  const vec3 weights = vec3(0.25, 0.5, 0.25);
  // x per size (rows) and wavelength (columns)
  vec3 x0 = PI * dUm * sizes.x / lambdaUm;
  vec3 x1 = PI * dUm * sizes.y / lambdaUm;
  vec3 x2 = PI * dUm * sizes.z / lambdaUm;
  vec3 u0 = x0 * sinTheta, u1 = x1 * sinTheta, u2 = x2 * sinTheta;
  vec3 j0 = vec3(0.0), j1 = vec3(0.0), j2 = vec3(0.0);
  const int N = 20;
  for (int j = 1; j < N; ++j) {
    float t = PI * float(j) / float(N);
    float st = sin(t);
    j0 += cos(u0 * st - t);
    j1 += cos(u1 * st - t);
    j2 += cos(u2 * st - t);
  }
  j0 /= float(N); j1 /= float(N); j2 /= float(N);
  // 2 J1(u) / u -> 1 as u -> 0
  vec3 a0 = mix(2.0 * j0 / max(u0, vec3(1e-4)), vec3(1.0), step(u0, vec3(1e-4)));
  vec3 a1 = mix(2.0 * j1 / max(u1, vec3(1e-4)), vec3(1.0), step(u1, vec3(1e-4)));
  vec3 a2 = mix(2.0 * j2 / max(u2, vec3(1e-4)), vec3(1.0), step(u2, vec3(1e-4)));
  vec3 p = weights.x * x0 * x0 * a0 * a0 + weights.y * x1 * x1 * a1 * a1 + weights.z * x2 * x2 * a2 * a2;
  p *= RECIPROCAL_PI4;
  return max(0.5 * p - vec3(phaseFunction(cosTheta)), vec3(0.0));
}

// [WolfViewer 2026-10-27] ticket #247 (Jimmy Olsen, TROMSO by night, w81): "A bright coming from moon center and not behind it..
// but sometimes it goes away". The corona above is the diffraction of the light taken as a point, its central lobe out to the
// first dark ring at 1.9 deg (green). The moon the dome draws while an EEP drives the sky is LL's quad (skyF.glsl wolfEepMoon:
// half-size moon_scale x 0.1 x 0.45 tangent, wolfenvironmentrender.cpp renderSkyDome; about 2.6 deg at moon_scale 1, the Wolf
// weather's own value), ten times the real moon (0.26 deg) - so the whole lobe, ~0.5 x 1000 sr^-1 at the centre, lay ON the
// disc, and the clouds are laid over the dome after it (compositeF.glsl: dome x (1 - alpha) + clouds): a white spot over the
// moon's middle whenever thin water cloud (Sc / Ac / As edges; never ice: x (1 - ice) in the march) drifted across it - gone
// again as the cloud moved on. Over the disc the forward-diffracted light is the disc's own light (it is part of the extinction
// the cloud already takes off the disc, van de Hulst 8.22): for the real moon it is ~1 % of the disc's luminance and never seen
// as a spot; the EEP disc is not drawn at that luminance, so it showed. So the corona is left out over the disc as drawn - the
// glow round it, outside the disc, is exactly as before. The edge is the disc's own (wolfEepMoon / wolfMoonDisc alpha
// 1 - smoothstep(0.98, 1.0, r)), so corona and disc meet without a seam. 1 everywhere when no disc is drawn.
uniform float wolfMoonDiscOn;
uniform vec3 wolfMoonDiscTo;      // ECEF, unit
uniform vec3 wolfMoonDiscRight;
uniform vec3 wolfMoonDiscUp;
uniform vec2 wolfMoonDiscHalf;    // tangents along right / up
float wolfCoronaOffDisc(const vec3 rayDirection) {
  if (wolfMoonDiscOn < 0.5) {
    return 1.0;
  }
  float c = dot(rayDirection, wolfMoonDiscTo);
  if (c <= 0.0) {
    return 1.0;
  }
  vec2 p = vec2(dot(rayDirection, wolfMoonDiscRight), dot(rayDirection, wolfMoonDiscUp)) / (c * max(wolfMoonDiscHalf, vec2(1e-6)));
  return smoothstep(0.98, 1.0, length(p));
}

// Source note: Construct spherical harmonics of degree 2 using 2 sample points
// positioned near the horizon occlusion points on the sun direction plane.
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:419-436 getGroundSunSkyIrradiance
vec3 getGroundSunSkyIrradiance(
  const vec3 position,
  const vec3 surfaceNormal,
  const float height,
  out vec3 skyIrradiance
) {
  #ifdef ACCURATE_SUN_SKY_LIGHT
  // [WolfViewer round 8] the sun or the moon, plus the other's sky (clouds.glsl wolfLightIrradiance)
  return wolfLightIrradiance((position - surfaceNormal * height) * METER_TO_LENGTH_UNIT, surfaceNormal, skyIrradiance);
  #else // ACCURATE_SUN_SKY_LIGHT
  skyIrradiance = vGroundIrradiance.sky;
  return vGroundIrradiance.sun;
  #endif // ACCURATE_SUN_SKY_LIGHT
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:438-446 getCloudsSunSkyIrradiance
vec3 getCloudsSunSkyIrradiance(const vec3 position, const float height, out vec3 skyIrradiance) {
  #ifdef ACCURATE_SUN_SKY_LIGHT
  return wolfLightScalarIrradiance(position * METER_TO_LENGTH_UNIT, skyIrradiance);   // [WolfViewer round 8]
  #else // ACCURATE_SUN_SKY_LIGHT
  float alpha = remapClamped(height, minHeight, maxHeight);
  skyIrradiance = mix(vCloudsIrradiance.minSky, vCloudsIrradiance.maxSky, alpha);
  return mix(vCloudsIrradiance.minSun, vCloudsIrradiance.maxSun, alpha);
  #endif // ACCURATE_SUN_SKY_LIGHT
}

#ifdef GROUND_BOUNCE
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:449-469 approximateRadianceFromGround
vec3 approximateRadianceFromGround(
  const vec3 position,
  const vec3 surfaceNormal,
  const float height,
  const float mipLevel,
  const float jitter
) {
  float opticalDepthToGround = marchOpticalDepth(
    position,
    -surfaceNormal,
    maxIterationCountToGround,
    mipLevel,
    jitter
  );
  vec3 skyIrradiance;
  vec3 sunIrradiance = getGroundSunSkyIrradiance(position, surfaceNormal, height, skyIrradiance);
  const float groundAlbedo = 0.3;
  vec3 groundIrradiance = skyIrradiance + (1.0 - coverage) * sunIrradiance;
  vec3 bouncedRadiance = groundAlbedo * RECIPROCAL_PI * groundIrradiance;
  return bouncedRadiance * exp(-opticalDepthToGround);
}
#endif // GROUND_BOUNCE

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:472-618 marchClouds
vec4 marchClouds(
  const vec3 rayOrigin,
  const vec3 rayDirection,
  const vec2 rayNearFar,
  const float cosTheta,
  const vec3 corona,   // [WolfViewer 2026-10-26] wolfCoronaExcess(cosTheta), sr^-1 per channel
  const float jitter,
  const float rayStartTexelsPerPixel,
  out float frontDepth,
  out ivec3 sampleCount
) {
  vec3 radianceIntegral = vec3(0.0);
  float transmittanceIntegral = 1.0;
  float weightedDistanceSum = 0.0;
  float transmittanceSum = 0.0;

  float maxRayDistance = rayNearFar.y - rayNearFar.x;
  float stepSize = minStepSize + (perspectiveStepScale - 1.0) * rayNearFar.x;
  // [WolfViewer round 8b] never longer than half the ray: the first sample sits at up to 2 x jitter x stepSize
  // (the next line), so a ray shorter than that - the ground a few metres away, seen from inside fog - got no
  // sample at all on most frames and a whole minStepSize (50 m) of fog on the rest: bright square dots on fogged
  // ground (measured: alpha 0.0002 at 5 m where exp(-0.0375 x 5) gives 0.17). Two steps of half the ray give the
  // Frostbite 5.6.3 integration below the exact transmittance of a uniform slab; the 1 cm floor keeps a zero-length
  // ray from stepping nowhere for maxIterationCount iterations.
  stepSize = min(stepSize, max(0.5 * maxRayDistance, 0.01));
  // I don't understand why spatial aliasing remains unless doubling the jitter.
  float rayDistance = stepSize * jitter * 2.0;

  for (int i = 0; i < maxIterationCount; ++i) {
    if (rayDistance > maxRayDistance) {
      break; // Termination
    }

    vec3 position = rayDistance * rayDirection + rayOrigin;
    float height = length(position) - bottomRadius;
    float mipLevel = log2(max(1.0, rayStartTexelsPerPixel + rayDistance * 1e-5));

    #if !defined(DEBUG_MARCH_INTERVALS)
    if (insideLayerIntervals(height)) {
      stepSize *= perspectiveStepScale;
      rayDistance += mix(stepSize, maxStepSize, min(1.0, mipLevel));
      continue;
    }
    #endif // !defined(DEBUG_MARCH_INTERVALS)

    // Sample rough weather.
    vec2 uv = getGlobeUv(position);
    WeatherSample weather = sampleWeather(uv, height, mipLevel, position);

    #ifdef DEBUG_SHOW_SAMPLE_COUNT
    ++sampleCount.x;
    #endif // DEBUG_SHOW_SAMPLE_COUNT

    // [HURRICANE 2026-10-30] a rain curtain alone (no cloud) is still a medium (cloudsCommon.glsl sampleMedia)
    if (!any(greaterThan(weather.density, vec4(minDensity))) && !(weather.stormRain > 0.0)) {
      // Step longer in empty space.
      // Source note: This produces banding artifacts.
      // Possible improvement: Binary search refinement
      stepSize *= perspectiveStepScale;
      rayDistance += mix(stepSize, maxStepSize, min(1.0, mipLevel));
      continue;
    }

    // Sample detailed participating media.
    MediaSample media = sampleMedia(weather, position, uv, mipLevel, jitter, sampleCount);

    if (media.extinction > minExtinction) {
      vec3 skyIrradiance;
      vec3 sunIrradiance = getCloudsSunSkyIrradiance(position, height, skyIrradiance);
      vec3 surfaceNormal = normalize(position);

      // March optical depth to the sun for finer details, which BSM lacks.
      float sunRayDistance = 0.0;
      float opticalDepth = marchOpticalDepth(
        position,
        sunDirection,
        maxIterationCountToSun,
        mipLevel,
        jitter,
        sunRayDistance
      );

      if (height < shadowTopHeight) {
        // Obtain the optical depth from BSM at the ray position.
        opticalDepth += sampleShadowOpticalDepth(
          position,
          // Take account of only positions further than the marched ray
          // distance.
          sunRayDistance,
          // Apply PCF only when the sun is close to the horizon.
          maxShadowFilterRadius * remapClamped(dot(sunDirection, surfaceNormal), 0.1, 0.0),
          jitter
        );
      }

      // [WolfViewer] the sample's ice fraction (its layers' wolfLayerIce by density share) sets the phase function
      float ice = clamp(dot(media.weight, wolfLayerIce), 0.0, 1.0);
      // [WolfViewer 2026-10-26] THIN ICE CLOUD: multiple scattering only as far as the sheet is thick. The octave sum (Wrenninge's
      // a = b = c = WOLF_MS_ATTENUATION 0.815, tuned so a THICK cloud has albedo 0.8, wolfcloudgenus.h) tends to sum a^k = 4.4 x the
      // single scattering as the optical depth to the light goes to 0, i.e. a veil of cirrus came out ~4.4 x too bright - and in a
      // slab of vertical optical thickness tau only 1 - exp(-tau) of the light interacts at all (Beer-Lambert), so a second
      // scattering is at most that likely. Nubis Evolved scales its multiple-scattering term the same way by how deep in the cloud
      // the sample is ("ms_volume = Remap(dimensional_profile ...)", Schneider, SIGGRAPH 2022 Advances; BEAUTY_RESEARCH.md 1.3).
      // tau of the sheet = the local extinction x the depth of the sample's layer(s) (a uniform-profile ice layer: exact).
      // ONLY FOR ICE (mixed by the ice share): water clouds - the cumulus and stratocumulus looks Paul approved - are untouched
      // (msWeight 1); their sunlit sides are bright because of the octaves and must stay so. Cirrus at tau 0.37 (the measured mean,
      // wolfcloudgenus.cpp Ci) keeps 0.31 of its octaves: 2.0 x single scattering instead of 4.4 x.
      float iceSheetTau = media.extinction * dot(media.weight, maxLayerHeights - minLayerHeights);
      float msWeight = mix(1.0, 1.0 - exp(-iceSheetTau), ice);
      vec3 radiance = sunIrradiance * approximateMultipleScattering(opticalDepth, cosTheta, ice, msWeight);

      #ifdef GROUND_BOUNCE
      // Fudge factor for the irradiance from ground.
      if (height < shadowTopHeight && mipLevel < 0.5) {
        vec3 groundRadiance = approximateRadianceFromGround(
          position,
          surfaceNormal,
          height,
          mipLevel,
          jitter
        );
        radiance += groundRadiance * RECIPROCAL_PI4 * groundBounceScale;
      }
      #endif // GROUND_BOUNCE

      // Crude approximation of sky gradient. Better than none in the shadows.
      float skyGradient = dot(weather.heightFraction * 0.5 + 0.5, media.weight);
      radiance += skyIrradiance * RECIPROCAL_PI4 * skyGradient * skyLightScale;

      // [WolfViewer] the sunlight diffused down through the cloud above the sample, which the octave
      // approximation above (oz_volumes, approximateMultipleScattering) loses in an optically thick layer
      // (an overcast deck's base came out lit by the clear sky's blue alone): the two-stream diffuse
      // transmittance of a non-absorbing layer, Stephens, Colorado State AT622 notes section 15 eq. 15.20b,
      // T = 2 / (2 + (1 - g) tau), g = 0.85 for water droplet clouds (example 15.5), with tau the vertical optical
      // depth above the sample (the slant one towards the sun x mu_s); the diffuse share T - exp(-optical depth)
      // of the horizontal sun irradiance F, a downward field of isotropic radiance F / pi scattered here with an
      // isotropic phase over its hemisphere: F / pi x 2 pi / (4 pi) = F / (2 pi). Weighted by 1 - exp(-optical
      // depth): in an optically thin layer the octaves' first (single-scattering) term already holds that light.
      // mu_s is clamped only inside T (the source itself goes to 0 with the sun).
      {
        float muS = max(dot(sunDirection, surfaceNormal), 0.0);
        // [WolfViewer round 10b] the column ABOVE the sample, marched straight up to the layers' top (3 equal steps, the
        // media sampled as marchOpticalDepth samples them, clouds.frag:346-377): the two-stream layer is the vertical one.
        // Round 8 took the slant depth towards the sun x mu_s for it, which for a tower's shaded side (the sun blocked by
        // the tower, clear air above) gave a thick "layer" and lit the side with diffused sunlight - the flat look from
        // above (round 10b: Cu congestus top / side 2.5x with it, 8.6x without; photo 19 6.0x). The diffuse share of the
        // beam is then the same column's: T(tau_v) - exp(-tau_v / mu_s), Stephens AT622 section 15 (eq. 15.20b and the
        // direct beam exp(-tau / mu0)): 0 under clear air, the deck's diffuse light under a deck.
        float tauV = 0.0;
        {
          float toTop = max(maxHeight - height, 0.0);
          float dz = toTop / 3.0;
          for (int k = 0; k < 3; ++k) {
            vec3 p = position + surfaceNormal * (dz * (float(k) + 0.5));
            vec2 puv = getGlobeUv(p);
            WeatherSample pw = sampleWeather(puv, length(p) - bottomRadius, mipLevel, p);
            if (!any(greaterThan(pw.density, vec4(minDensity)))) continue;
            MediaSample pm = sampleMedia(pw, p, puv, mipLevel, jitter);
            tauV += pm.extinction * dz;
          }
        }
        float diffuseT = 2.0 / (2.0 + (1.0 - 0.85) * tauV);
        radiance += sunIrradiance * muS * max(diffuseT - exp(-tauV / max(muS, 0.05)), 0.0) * (1.0 - exp(-opticalDepth)) * (0.5 * RECIPROCAL_PI);
      }

      // Finally multiply by scattering.
      radiance *= media.scattering;

      #ifdef POWDER
      // [WolfViewer] no powder in ice; [round 16c] the exponent in takram's own density units, per layer (wolfLayerPowder)
      radiance *= 1.0 - powderScale * (1.0 - ice) * exp(-media.extinction * powderExponent * max(dot(media.weight, wolfLayerPowder), 1e-3));
      #endif // POWDER

      // [WolfViewer 2026-10-26] the corona (wolfCoronaExcess): the droplets' share of the sample (1 - ice), single-scattered light
      // only (the beam's transmittance to the sample), x scattering as the radiance above
      radiance += sunIrradiance * corona * ((1.0 - ice) * exp(-opticalDepth) * media.scattering);

      #ifdef DEBUG_SHOW_CASCADES
      if (height < shadowTopHeight) {
        radiance = 1e-3 * getFadedCascadeColor(position, jitter);
      }
      #endif // DEBUG_SHOW_CASCADES

      // Energy-conserving analytical integration of scattered light
      // See 5.6.3 in https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
      float transmittance = exp(-media.extinction * stepSize);
      float clampedExtinction = max(media.extinction, 1e-7);
      vec3 scatteringIntegral = (radiance - radiance * transmittance) / clampedExtinction;
      radianceIntegral += transmittanceIntegral * scatteringIntegral;
      transmittanceIntegral *= transmittance;

      // Aerial perspective affecting clouds
      // See 5.9.1 in https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
      weightedDistanceSum += rayDistance * transmittanceIntegral;
      transmittanceSum += transmittanceIntegral;
    }

    if (transmittanceIntegral <= minTransmittance) {
      break; // Early termination
    }

    // Take a shorter step because we've already hit the clouds.
    stepSize *= perspectiveStepScale;
    rayDistance += stepSize;
  }

  // The final product of 5.9.1 and we'll evaluate this in aerial perspective.
  frontDepth = transmittanceSum > 0.0 ? weightedDistanceSum / transmittanceSum : -1.0;

  return vec4(radianceIntegral, remapClamped(transmittanceIntegral, 1.0, minTransmittance));
}

// [WolfViewer 2026-10-21] Maximus Lear (AMD Radeon RX 7800 XT, driver 26.10.1, High 5/7): the Wolf sky's dome went black
// a moment after login and the clouds had black blotches - skyF.glsl turns a NaN / inf sky colour into black, so the dome's
// inputs were not finite on his GPU. A NaN written here would be read by every later pass (the LUTs by every sky pixel, the
// clouds' history by the next frame), so nothing that is not finite leaves this pass. Finite values are unchanged.
vec4 wolfFiniteOrZero(vec4 v) { return mix(v, vec4(0.0), greaterThan(vec4(isnan(v)) + vec4(isinf(v)), vec4(0.0))); }
vec3 wolfFiniteOrZero(vec3 v) { return mix(v, vec3(0.0), greaterThan(vec3(isnan(v)) + vec3(isinf(v)), vec3(0.0))); }

#ifdef SHADOW_LENGTH

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:622-648 marchShadowLength
float marchShadowLength(
  const vec3 rayOrigin,
  const vec3 rayDirection,
  const vec2 rayNearFar,
  const float jitter
) {
  float shadowLength = 0.0;
  float maxRayDistance = rayNearFar.y - rayNearFar.x;
  float stepSize = minShadowLengthStepSize;
  float rayDistance = stepSize * jitter;
  const float attenuationFactor = 1.0 - 5e-4;
  float attenuation = 1.0;

  // Source note: This march is closed, and sample resolution can be much lower.
  // Refining the termination by binary search will make it much more efficient.
  for (int i = 0; i < maxShadowLengthIterationCount; ++i) {
    if (rayDistance > maxRayDistance) {
      break; // Termination
    }
    vec3 position = rayDistance * rayDirection + rayOrigin;
    float opticalDepth = sampleShadowOpticalDepth(position, 0.0, 0.0, jitter);
    shadowLength += (1.0 - exp(-opticalDepth)) * stepSize * attenuation;
    stepSize *= perspectiveStepScale;
    rayDistance += stepSize;
  }
  return shadowLength;
}

#endif // SHADOW_LENGTH

#ifdef HAZE

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:654-697 approximateHaze
vec4 approximateHaze(
  const vec3 rayOrigin,
  const vec3 rayDirection,
  const float maxRayDistance,
  const float cosTheta,
  const float shadowLength
) {
  // [WolfViewer 2026-10-13] an EEP's haze is there whatever the cloud cover (the stock haze pass, hazeF.glsl, has no such
  // gate): wolfHazeAlways (wolfenvironmentrender.cpp cloudsFrame) lifts takram's coverage ramp
  float modulation = max(remapClamped(coverage, 0.2, 0.4), wolfHazeAlways);
  if (cameraHeight * modulation < 0.0) {
    return vec4(0.0);
  }
  float density = modulation * hazeDensityScale * exp(-cameraHeight * hazeExponent);
  if (density < 1e-7) {
    return vec4(0.0); // Prevent artifact in views from space
  }

  // Blend two normals by the difference in angle so that normal near the
  // ground becomes that of the origin, and in the sky that of the horizon.
  vec3 normalAtOrigin = normalize(rayOrigin);
  vec3 normalAtHorizon = (rayOrigin - dot(rayOrigin, rayDirection) * rayDirection) / bottomRadius;
  float alpha = remapClamped(dot(normalAtOrigin, normalAtHorizon), 0.9, 1.0);
  vec3 normal = mix(normalAtOrigin, normalAtHorizon, alpha);

  // Analytical optical depth where density exponentially decreases with height.
  // Based on: https://iquilezles.org/articles/fog/
  float angle = max(dot(normal, rayDirection), 1e-5);
  float exponent = angle * hazeExponent;
  float linearTerm = density / hazeExponent / angle;

  // Derive the optical depths separately for with and without shadow length.
  float expTerm = 1.0 - exp(-maxRayDistance * exponent);
  float shadowExpTerm = 1.0 - exp(-min(maxRayDistance, shadowLength) * exponent);
  float opticalDepth = expTerm * linearTerm;
  float shadowOpticalDepth = max((expTerm - shadowExpTerm) * linearTerm, 0.0);
  float transmittance = saturate(1.0 - exp(-opticalDepth));
  float shadowTransmittance = saturate(1.0 - exp(-shadowOpticalDepth));

  vec3 skyIrradiance = vGroundIrradiance.sky;
  vec3 sunIrradiance = vGroundIrradiance.sun;
  vec3 inscatter = sunIrradiance * phaseFunction(cosTheta) * shadowTransmittance;
  // [WolfViewer] x the sky light under the clouds (clouds.frag:694 lights the haze with the clear sky; under an
  // overcast deck its long horizon path then outshone the deck - a bright band on the horizon)
  inscatter += skyIrradiance * wolfSkyUnderCloud * RECIPROCAL_PI4 * skyLightScale * transmittance;
  inscatter *= hazeScatteringCoefficient / (hazeAbsorptionCoefficient + hazeScatteringCoefficient);
  return vec4(inscatter, transmittance);
}

#endif // HAZE

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:701-716 applyAerialPerspective
// [WolfViewer 2026-10-13] given the ray and the distance to the front (exact) rather than the front's ECEF position
// (brunetonRuntime.glsl wolfSkyRadianceToPointRel: frontPosition - cameraPosition at 6,360 km in floats wobbled)
void applyAerialPerspective(
  const vec3 cameraPosition,
  const vec3 rayDirection,
  const float frontDepth,
  const float shadowLength,
  inout vec4 color
) {
  vec3 transmittance;
  vec3 inscatter = wolfSkyLuminanceToPointRel(
    cameraPosition * METER_TO_LENGTH_UNIT,
    rayDirection,
    frontDepth * METER_TO_LENGTH_UNIT,
    shadowLength * METER_TO_LENGTH_UNIT,
    sunDirection,
    transmittance
  );
  // [WolfViewer round 8] the air between the camera and the cloud is lit as the clouds are (cloudsLight.glsl): the
  // light's in-scatter x its scale (sunDirection is the moon at night: the tables hold the sun's strength), plus the
  // other body's in-scatter x its scale (the twilight sun's at night, the moon's faint air by day) with no light
  // shafts (the shadow length is marched along the light alone). The transmittance is the same for both.
  inscatter *= wolfLightScale > 0.0 ? wolfLightScale : 1.0;
  if (wolfOtherScale > 0.0) {
    vec3 transmittanceOther;
    inscatter += wolfSkyLuminanceToPointRel(
      cameraPosition * METER_TO_LENGTH_UNIT,
      rayDirection,
      frontDepth * METER_TO_LENGTH_UNIT,
      0.0,
      wolfOtherDirection,
      transmittanceOther
    ) * wolfOtherScale;
  }
  color.rgb = color.rgb * transmittance + inscatter * color.a;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:718-722 rayIntersectsGround
bool rayIntersectsGround(const vec3 cameraPosition, const vec3 rayDirection) {
  float r = length(cameraPosition);
  float mu = dot(cameraPosition, rayDirection) / r;
  return mu < 0.0 && r * r * (mu * mu - 1.0) + bottomRadius * bottomRadius >= 0.0;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:724-728 IntersectionResult
struct IntersectionResult {
  bool ground;
  vec4 first;
  vec4 second;
};

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:730-741 getIntersections
// [WolfViewer 2026-10-13] the same intersections (rayIntersectsGround, raySphereIntersections of
// coreRaySphereIntersection.glsl:154-170 with the four shells) computed so they hold at the horizon. The source takes
// c = |o|^2 - R^2 and b^2 - c for a camera 6,360 km from the centre: two numbers of ~4e13 in 32-bit floats whose
// difference is a few thousand, so near the horizon the ground hit flickered on and off and the haze's length with it -
// the ring band over the mountains at eye level (Paul, Iceland / Tromso / the fjord; the debug capture's rings). Here
// |o|^2 - R^2 = (h - H)(r + R) from the camera's height h (cameraHeight, set in double precision, wolftkclouds.cpp
// "cameraHeight") and the shells' heights H, and the near root is taken in the cancellation-free form c / q (Numerical
// Recipes 5.6: q = -(b + sgn(b) sqrt(b^2 - c)), roots q and c / q, here with the half b). Same roots, same order, same -1
// where a ray misses.
IntersectionResult getIntersections(const vec3 cameraPosition, const vec3 rayDirection) {
  IntersectionResult intersections;
  float r = bottomRadius + cameraHeight;
  float b = r * dot(rayDirection, normalize(cameraPosition));   // half of the source's b
  vec4 shellHeight = vec4(0.0, minHeight, maxHeight, shadowTopHeight);
  vec4 c = (cameraHeight - shellHeight) * (r + bottomRadius + shellHeight);
  vec4 discriminant = b * b - c;
  vec4 Q = sqrt(max(vec4(0.0), discriminant));
  vec4 q = b >= 0.0 ? -(b + Q) : -(b - Q);
  vec4 other = c / mix(q, vec4(1.0), equal(q, vec4(0.0)));
  vec4 t1 = min(q, other), t2 = max(q, other);
  vec4 mask = step(discriminant, vec4(0.0));
  intersections.first = mix(t1, vec4(-1.0), mask);
  intersections.second = mix(t2, vec4(-1.0), mask);
  // Source: clouds.frag:718-722 rayIntersectsGround - mu < 0 and r^2 (mu^2 - 1) + R^2 >= 0, i.e. the ground shell's
  // discriminant (b^2 - c.x) >= 0
  intersections.ground = b < 0.0 && discriminant.x >= 0.0;
  return intersections;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:743-769 getRayNearFar
vec2 getRayNearFar(const IntersectionResult intersections) {
  vec2 nearFar;
  if (cameraHeight < minHeight) {
    // View below the clouds
    if (intersections.ground) {
      nearFar = vec2(-1.0); // No clouds to the ground
    } else {
      nearFar = vec2(intersections.second.y, intersections.second.z);
      nearFar.y = min(nearFar.y, maxRayDistance);
    }
  } else if (cameraHeight < maxHeight) {
    // View inside the total cloud layer
    if (intersections.ground) {
      nearFar = vec2(cameraNear, intersections.first.y);
    } else {
      nearFar = vec2(cameraNear, intersections.second.z);
    }
  } else {
    // View above the clouds
    nearFar = vec2(intersections.first.z, intersections.second.z);
    if (intersections.ground) {
      // Clamp the ray at the min height.
      nearFar.y = intersections.first.y;
    }
  }
  return nearFar;
}

#ifdef SHADOW_LENGTH
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:772-789 getShadowRayNearFar
vec2 getShadowRayNearFar(const IntersectionResult intersections) {
  vec2 nearFar;
  if (cameraHeight < shadowTopHeight) {
    if (intersections.ground) {
      nearFar = vec2(cameraNear, intersections.first.x);
    } else {
      nearFar = vec2(cameraNear, intersections.second.w);
    }
  } else {
    nearFar = vec2(intersections.first.w, intersections.second.w);
    if (intersections.ground) {
      // Clamp the ray at the ground.
      nearFar.y = intersections.first.x;
    }
  }
  nearFar.y = min(nearFar.y, maxShadowLengthRayDistance);
  return nearFar;
}
#endif // SHADOW_LENGTH

#ifdef HAZE
// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:793-809 getHazeRayNearFar
vec2 getHazeRayNearFar(const IntersectionResult intersections) {
  vec2 nearFar;
  if (cameraHeight < maxHeight) {
    if (intersections.ground) {
      nearFar = vec2(cameraNear, intersections.first.x);
    } else {
      nearFar = vec2(cameraNear, intersections.second.z);
    }
  } else {
    nearFar = vec2(cameraNear, intersections.second.z);
    if (intersections.ground) {
      // Clamp the ray at the ground.
      nearFar.y = intersections.first.x;
    }
  }
  return nearFar;
}
#endif // HAZE

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:812-821 getRayDistanceToScene
float getRayDistanceToScene(const vec3 rayDirection, out float viewZ) {
  float depth = readDepthValue(depthBuffer, vUv * targetUvScale + temporalJitter);
  if (depth < 1.0 - 1e-7) {
    depth = reverseLogDepth(depth, cameraNear, cameraFar);
    viewZ = getViewZ(depth);
    return -viewZ / dot(rayDirection, vCameraDirection);
  }
  viewZ = 0.0;
  return 0.0;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.frag:823-1003 main
void main() {
  #ifdef DEBUG_SHOW_SHADOW_MAP
  outputColor = getCascadedShadowMaps(vUv);
  outputDepthVelocity = vec4(0.0);
  #ifdef SHADOW_LENGTH
  outputShadowLength = 0.0;
  #endif // SHADOW_LENGTH
  return;
  #endif // DEBUG_SHOW_SHADOW_MAP

  vec3 cameraPosition = vCameraPosition + altitudeCorrection;
  vec3 rayDirection = normalize(vRayDirection);
  float cosTheta = dot(sunDirection, rayDirection);
  // [WolfViewer 2026-10-26] this pass's pixel as an angle (radians), taken here in uniform control flow (derivatives inside the
  // branches below would be undefined): the far sea's haze below uses it at the horizon
  float wolfPixelAngle = length(fwidth(rayDirection));

  IntersectionResult intersections = getIntersections(cameraPosition, rayDirection);
  vec2 rayNearFar = getRayNearFar(intersections);
  #ifdef SHADOW_LENGTH
  vec2 shadowRayNearFar = getShadowRayNearFar(intersections);
  #endif // SHADOW_LENGTH
  #ifdef HAZE
  vec2 hazeRayNearFar = getHazeRayNearFar(intersections);
  #endif // HAZE

  float sceneViewZ;
  float rayDistanceToScene = getRayDistanceToScene(rayDirection, sceneViewZ);
  if (rayDistanceToScene > 0.0) {
    rayNearFar.y = min(rayNearFar.y, rayDistanceToScene);
    #ifdef SHADOW_LENGTH
    shadowRayNearFar.y = min(shadowRayNearFar.y, rayDistanceToScene);
    #endif // SHADOW_LENGTH
    #ifdef HAZE
    hazeRayNearFar.y = min(hazeRayNearFar.y, rayDistanceToScene);
    #endif // HAZE
  }

  bool intersectsGround = any(lessThan(rayNearFar, vec2(0.0)));
  bool intersectsScene = rayNearFar.y < rayNearFar.x;

  float stbn = getSTBN();

  vec4 color = vec4(0.0);
  float frontDepth = rayNearFar.y;
  vec4 depthVelocity = vec4(0.0);
  float shadowLength = 0.0;
  bool hitClouds = false;
  float wolfCloudAlpha = 0.0;   // [WolfViewer 2026-10-13] the clouds' own opacity, before the haze (outputDepthVelocity.a below)

  if (!intersectsGround && !intersectsScene) {
    vec3 rayOrigin = rayNearFar.x * rayDirection + cameraPosition;

    vec2 globeUv = getGlobeUv(rayOrigin);
    #ifdef DEBUG_SHOW_UV
    outputColor = vec4(vec3(checker(globeUv, localWeatherRepeat + localWeatherOffset)), 1.0);
    outputDepthVelocity = vec4(0.0);
    #ifdef SHADOW_LENGTH
    outputShadowLength = 0.0;
    #endif // SHADOW_LENGTH
    return;
    #endif // DEBUG_SHOW_UV

    float mipLevel = getMipLevel(globeUv * localWeatherRepeat) * mipLevelScale;
    mipLevel = mix(0.0, mipLevel, min(1.0, 0.2 * cameraHeight / maxHeight));

    float marchedFrontDepth;
    ivec3 sampleCount = ivec3(0);
    color = marchClouds(
      rayOrigin,
      rayDirection,
      rayNearFar,
      cosTheta,
      wolfCoronaExcess(cosTheta) * wolfCoronaOffDisc(rayDirection),   // [2026-10-27] not over the moon disc (ticket #247)
      stbn,
      pow(2.0, mipLevel),
      marchedFrontDepth,
      sampleCount
    );

    #ifdef DEBUG_SHOW_SAMPLE_COUNT
    outputColor = vec4(vec3(sampleCount) / vec3(500.0, 5.0, 5.0), 1.0);
    outputDepthVelocity = vec4(0.0);
    #ifdef SHADOW_LENGTH
    outputShadowLength = 0.0;
    #endif // SHADOW_LENGTH
    return;
    #endif // DEBUG_SHOW_SAMPLE_COUNT

    color.rgb *= vec3(1.0) + wolfEepCloudTint;   // [WolfViewer 2026-10-13] the EEP's cloud colour, before the air in front
    if (wolfEepCloudGainOn > 0.5) {
      color.rgb *= wolfGainGrid(WOLF_GRID_CLOUDS, wolfGainElevAz(cameraPosition * METER_TO_LENGTH_UNIT, rayDirection, wolfEepCloudGainRef));
    } else if (wolfEepCloudSkyGain > 0.5) {
      // [WolfViewer 2026-10-27] Wolf weather by day: the sky's own gain in this direction (as the haze takes it below), so the
      // clouds keep the EEP's palette and their physical contrast with the sky (wolfenvironmentrender.cpp measureCloudCover)
      color.rgb *= wolfEepGainDir(cameraPosition * METER_TO_LENGTH_UNIT, rayDirection);
    }
    wolfCloudAlpha = color.a;   // measured before the thinning below (measureCloudCover sets the thinning from it)
    color *= 1.0 - clamp(wolfEepCloudThin, 0.0, 0.95);   // premultiplied: the same light per unit of opacity, less of both

    // Front depth will be -1.0 when no samples are accumulated.
    hitClouds = marchedFrontDepth >= 0.0;
    if (hitClouds) {
      frontDepth = rayNearFar.x + marchedFrontDepth;

      #ifdef SHADOW_LENGTH
      // Clamp the shadow length ray at the clouds.
      shadowRayNearFar.y = mix(
        shadowRayNearFar.y,
        min(frontDepth, shadowRayNearFar.y),
        color.a // Interpolate by the alpha for smoother edges.
      );

      // Shadow length must be computed before applying aerial perspective.
      if (all(greaterThanEqual(shadowRayNearFar, vec2(0.0)))) {
        shadowLength = marchShadowLength(
          shadowRayNearFar.x * rayDirection + cameraPosition,
          rayDirection,
          shadowRayNearFar,
          stbn
        );
      }
      #endif // SHADOW_LENGTH

      #ifdef HAZE
      // Clamp the haze ray at the clouds.
      hazeRayNearFar.y = mix(
        hazeRayNearFar.y,
        min(frontDepth, hazeRayNearFar.y),
        color.a // Interpolate by the alpha for smoother edges.
      );
      #endif // HAZE

      // Apply aerial perspective.
      applyAerialPerspective(cameraPosition, rayDirection, frontDepth, shadowLength, color);

      // Velocity for temporal resolution.
      // [WolfViewer 2026-10-16] IN VIEW SPACE, as takram's own scene branch below (if (!hitClouds)) does "for greatly
      // reducing the precision errors". takram reprojects the cloud front from ECEF: cameraPosition + frontDepth x rayDirection,
      // then ecefToWorld - 32-bit floats about the planet's centre, whose step at the 6,360 km bottom radius
      // (wolfenvironmentrender.cpp Frame::bottomRadius) is 0.5 m, cancelled by ecefToWorldMatrix's translation. Clouds a km
      // away barely notice (under a pixel); a front a few metres to ~150 m away - mountains that rise into the cloud deck
      // (JOTUNHEIM 10-16: summits ~956 m above its water, and the viewer's own cloudsFrame log line there read "L0 Sc alt 600
      // h 900"), a camera inside the clouds - gets a velocity wrong by about 2 px (front 150 m) to 50 px (front 5 m) on a
      // 690 px tall view (a float32 emulation of these lines with placeOnPlanet's matrices), different for every low-res
      // texel and every jitter, even with the camera still. The resolve
      // (cloudsResolveF.glsl temporalUpscale) then fetches each texel's history from the wrong place and clips it to that
      // texel's box, so the 1/4-resolution clouds never refine: 4 x 4 pixel blocks over the ground, the frame's own Bayer
      // pixel a dot in each (the "pixelation on the terrain"). The front's view-space position is the view ray
      // (vViewPosition: inverseProjectionMatrix x the far-plane vertex, cloudsV.glsl main; its direction is rayDirection's,
      // the world-to-ECEF map being a rigid rotation) times the distance along it; viewReprojectionMatrix is
      // reprojectionMatrix x inverseView, built in double (wolftkclouds.cpp cameraUniforms).
      vec3 frontView = normalize(vViewPosition) * frontDepth;
      vec4 prevClip = viewReprojectionMatrix * vec4(frontView, 1.0);
      prevClip /= prevClip.w;
      vec2 prevUv = prevClip.xy * 0.5 + 0.5;
      vec2 velocity = vUv - prevUv;
      depthVelocity = vec4(frontDepth, velocity, rayDistanceToScene > 0.0 ? sceneViewZ : wolfCloudAlpha);   // [WolfViewer round 13 / 2026-10-13 / 2026-10-16: .a = the scene's view Z (negative: -(view depth), the quantity cloudsResolveF.glsl wolfSceneDepth reads from the full-res depth), or the clouds' own opacity over the sky (wolfenvironmentrender.cpp measureCloudCover)]
    }
  }

  if (!hitClouds) {
    #ifdef SHADOW_LENGTH
    if (all(greaterThanEqual(shadowRayNearFar, vec2(0.0)))) {
      shadowLength = marchShadowLength(
        shadowRayNearFar.x * rayDirection + cameraPosition,
        rayDirection,
        shadowRayNearFar,
        stbn
      );
    }
    #endif // SHADOW_LENGTH

    // Velocity for temporal resolution. Here reproject in the view space for
    // greatly reducing the precision errors.
    frontDepth = sceneViewZ < 0.0 ? -sceneViewZ : cameraFar;
    vec3 frontView = vViewPosition * frontDepth;
    vec4 prevClip = viewReprojectionMatrix * vec4(frontView, 1.0);
    prevClip /= prevClip.w;
    vec2 prevUv = prevClip.xy * 0.5 + 0.5;
    vec2 velocity = vUv - prevUv;
    depthVelocity = vec4(frontDepth, velocity, rayDistanceToScene > 0.0 ? sceneViewZ : wolfCloudAlpha);   // [WolfViewer round 13 / 2026-10-13 / 2026-10-16: .a = the scene's view Z (negative: -(view depth), the quantity cloudsResolveF.glsl wolfSceneDepth reads from the full-res depth), or the clouds' own opacity over the sky (wolfenvironmentrender.cpp measureCloudCover)]
  }

  #ifdef DEBUG_SHOW_FRONT_DEPTH
  outputColor = vec4(turbo(frontDepth / maxRayDistance), 1.0);
  outputDepthVelocity = vec4(0.0);
  #ifdef SHADOW_LENGTH
  outputShadowLength = 0.0;
  #endif // SHADOW_LENGTH
  return;
  #endif // DEBUG_SHOW_FRONT_DEPTH

  #ifdef HAZE
  // [WolfViewer 2026-10-13] an EEP's haze lies on the ground only: the stock haze pass discards the sky (hazeF.glsl:79-83,
  // depth >= 1), whose own colour already holds the EEP's haze (skyV.glsl), and the stock clouds carry their own haze below
  // them (cloudsV.glsl additiveColorBelowCloud). Over the sky the screen check showed the native sky 30 points too bright
  // and orange round the sun - the EEP's haze laid over a sky already matched to the EEP.
  if (wolfLLHazeOn > 0.5 && rayDistanceToScene > 0.0) {
    // a surface: no haze here. [WolfViewer 2026-10-14] Under an EEP the scene shaders run with wolf_phys 2, which keeps the
    // stock haze (atmosphericsFuncs.glsl), so the stock haze pass (pipeline.cpp doAtmospherics, hazeF.glsl) has already laid
    // the EEP's haze on every opaque surface, exactly as with WE off, and the forward shaders theirs; laying wolfLLHaze here
    // too would haze the ground twice. The composite still leaves its aerial perspective off (wolfEepNoAerial).
    outputColor = clamp(wolfFiniteOrZero(vec4(wolfNightLight(color.rgb), color.a)), vec4(-65504.0), vec4(65504.0));   // [2026-10-25] nightLight.glsl
    outputDepthVelocity = depthVelocity;
    #ifdef SHADOW_LENGTH
    outputShadowLength = clamp(wolfFiniteOrZero(vec4(shadowLength * METER_TO_LENGTH_UNIT)).r, 0.0, 65504.0);
    #endif // SHADOW_LENGTH
    return;
  }
  if (wolfHazeAlways > 0.5 && rayDistanceToScene <= 0.0) {
    // [WolfViewer 2026-10-14] the open sea the sky draws below the horizon (skyF.glsl, the sea out to the true horizon) is a
    // SURFACE, not sky: the stock haze pass hazes everything below the horizon by its distance, and left unhazed here the sea
    // showed as a dark strip (31,46,62 against the EEP's 75,79,85) between the sky and the hazed land - Paul's "line on the
    // horizon" at Iceland. Its distance: the ray's near hit of the sea sphere, in getIntersections' cancellation-free form
    // (c = h (r + R), b = r mu, t = c / (-b + sqrt(b^2 - c))).
    if (wolfLLHazeOn > 0.5) {
      float rS = bottomRadius + cameraHeight;
      float bS = rS * dot(rayDirection, normalize(cameraPosition));
      float cS = cameraHeight * (rS + bottomRadius);
      float discS = bS * bS - cS;
      // [WolfViewer 2026-10-26] Paul, Knightwood 12:59Z: "a hard bright line along the horizon" - one row (196,185,199 in his
      // screenshot) brighter than the hazed far sea under it (157,148,157) and the sky over it (131-138). This pass runs at a
      // lower resolution than the dome, with a temporal jitter (wolftkclouds.cpp temporalJitter), so at the true horizon one of
      // its texels can test as missing the sea (discS < 0) while the dome under it (skyF.glsl, its own full-resolution test)
      // drew sea there: those dome pixels kept the raw, unhazed far sea - the mirror of the bright horizon sky at grazing
      // Fresnel. A texel within 1.5 of its own pixels of the tangent is now hazed as reaching the horizon (discS taken as 0);
      // d(discS)/d(mu) = 2 r^2 mu = 2 r sqrt(cS) at the tangent (mu = -sqrt(cS) / r), so the margin in discS is that x the angle.
      // Above the horizon this only touches that one texel row, which then takes the horizon's haze colour - as the sea does.
      float discMargin = 2.0 * rS * sqrt(max(cS, 0.0)) * 1.5 * wolfPixelAngle;
      if (bS < 0.0 && discS >= -discMargin && cameraHeight > 0.0) {
        float tS = cS / (-bS + sqrt(max(discS, 0.0)));
        vec4 llh = wolfLLHaze(cameraPosition, rayDirection, tS);
        color.rgb = color.rgb * (1.0 - llh.a) + llh.rgb;
        color.a = 1.0 - (1.0 - color.a) * (1.0 - llh.a);
      }
    }
    outputColor = clamp(wolfFiniteOrZero(vec4(wolfNightLight(color.rgb), color.a)), vec4(-65504.0), vec4(65504.0));   // [2026-10-25] nightLight.glsl
    outputDepthVelocity = depthVelocity;
    #ifdef SHADOW_LENGTH
    outputShadowLength = clamp(wolfFiniteOrZero(vec4(shadowLength * METER_TO_LENGTH_UNIT)).r, 0.0, 65504.0);
    #endif // SHADOW_LENGTH
    return;
  }
  vec4 haze = approximateHaze(
    cameraNear * rayDirection + cameraPosition,
    rayDirection,
    hazeRayNearFar.y - hazeRayNearFar.x,
    cosTheta,
    shadowLength
  );
  // [WolfViewer 2026-10-13] the haze is the air's light too: the EEP's sky gains the aerial perspective gets
  // (brunetonRuntime.glsl wolfEepGainDir), for this ray
  haze.rgb *= wolfEepGainDir(cameraPosition * METER_TO_LENGTH_UNIT, rayDirection);
  color.rgb = mix(color.rgb, haze.rgb, haze.a);
  color.a = color.a * (1.0 - haze.a) + haze.a;
  #endif // HAZE

  outputColor = clamp(wolfFiniteOrZero(vec4(wolfNightLight(color.rgb), color.a)), vec4(-65504.0), vec4(65504.0));   // [2026-10-25] nightLight.glsl
  outputDepthVelocity = depthVelocity;
  #ifdef SHADOW_LENGTH
  outputShadowLength = clamp(wolfFiniteOrZero(vec4(shadowLength * METER_TO_LENGTH_UNIT)).r, 0.0, 65504.0);
  #endif // SHADOW_LENGTH
}
