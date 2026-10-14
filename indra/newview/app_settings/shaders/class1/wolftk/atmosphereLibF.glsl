/**
 * @file wolftk/atmosphereLibF.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:329-331, 354-358, 381-383
 *   packages/atmosphere/src/shaders/aerialPerspectiveEffect.vert:3-5, 44-48
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
 * Uses wolftk/aerialPerspective.glsl (aerialPerspectiveEffect.frag:16-269).
 */

// [WolfViewer] A compilation unit of its own, attached beside a stock LL fragment shader (softenLightF, waterF,
// alphaF, materialF) as Bruneton's demo attaches the model's atmosphere shader to its own program
// (ebruneton precomputed_atmospheric_scattering 34f14e7 atmosphere/demo/demo.cc:110-131, model.cc:768-776).
// Only plain-typed functions cross the unit boundary, as demo.glsl's prototypes do; everything takram declares
// (ATMOSPHERE, the tables, the shadow-map uniforms) stays inside this unit.
//
// Exported:
//   vec3  wolfTkAerialPerspective(vec3 eyePos, vec3 radiance)  radiance x transmittance + inscatter, frame units
//   float wolfTkCloudShadow(vec3 eyePos)                         the sun's transmittance through the clouds
// eyePos: LL eye space (metres); wolfTkEyeToWorld takes it to the world (LL agent frame) takram's matrices use.

precision highp float;
precision highp sampler3D;

// The noise seed's clock under its own name: stock waterF.glsl / starsF.glsl already have a `time` uniform
// (their waves' and twinkle's), and two units' uniforms of one name are one uniform once linked.
uniform float wolfTkTime;
#define WOLF_TK_TIME wolfTkTime

// Source: aerialPerspectiveEffect.frag:381-383 runs applyTransmittanceInscatter when TRANSMITTANCE or INSCATTER
// is defined; AerialPerspectiveEffect.ts:160-161 defaults both on. HAS_SHADOW: the clouds' shadow map
// (AerialPerspectiveEffect.ts:404-434 updateShadow); SHADOW_SAMPLE_COUNT 8 (AerialPerspectiveEffect.ts:671-672);
// SHADOW_CASCADE_COUNT 3 (clouds qualityPresets.ts:30 defaults.shadow.cascadeCount, medium and high).
#define TRANSMITTANCE
#define INSCATTER
#define HAS_SHADOW
#define WOLF_TK_NO_TEMPORAL_FILTER
#ifndef SHADOW_CASCADE_COUNT
#define SHADOW_CASCADE_COUNT 3
#endif
#ifndef SHADOW_SAMPLE_COUNT
#define SHADOW_SAMPLE_COUNT 8
#endif

// aerialPerspectiveEffect.vert:9 `varying vec3 vCameraPosition` - here a plain global the functions below set
// before calling the source's functions, which read it.
vec3 vCameraPosition;

#wolf_include "wolftk/aerialPerspective.glsl"

// Source: aerialPerspectiveEffect.vert:3-5 (cameraPosition, worldToECEFMatrix, altitudeCorrection) - the
// first two are declared by the fragment uniforms already (aerialPerspectiveEffect.frag:36); these are the
// vertex stage's.
uniform vec3 cameraPosition;
uniform vec3 altitudeCorrection;
uniform mat4 wolfTkEyeToWorld;    // [WolfViewer] LL eye space -> world (the inverse of the scene's modelview)
uniform float wolfTkShadowOn;     // [WolfViewer] 1 when the clouds' shadow map is bound (wolfenvironmentrender.cpp)
uniform float wolfTkFarCloudT;    // [WolfViewer 2026-10-13] the cloud field's mean sun transmittance beyond the shadow map (1 - cover)
uniform float wolfTkUnderwater;   // [WolfViewer] 1 when the camera is under water: no air between it and what it sees

// Source: aerialPerspectiveEffect.vert:44-45 vCameraPosition = (cameraPositionECEF + altitudeCorrection) *
// METER_TO_LENGTH_UNIT, cameraPositionECEF = (worldToECEFMatrix * vec4(origin, 1.0)).xyz (perspective: origin =
// cameraPosition, :21).
void wolfTkSetCamera()
{
  vec3 cameraPositionECEF = (worldToECEFMatrix * vec4(cameraPosition, 1.0)).xyz;
  vCameraPosition = (cameraPositionECEF + altitudeCorrection) * METER_TO_LENGTH_UNIT;
}

// Source: aerialPerspectiveEffect.frag:329-331 (world -> ECEF km, + vGeometryAltitudeCorrection =
// altitudeCorrection * METER_TO_LENGTH_UNIT, aerialPerspectiveEffect.vert:48) and :381-383
// applyTransmittanceInscatter with shadowLength 0 (no light-shaft buffer in a forward shader).
vec3 wolfTkAerialPerspective(vec3 eyePos, vec3 radiance)
{
  if (wolfTkUnderwater > 0.5) return radiance;   // LL's own underwater fog is the medium there
  wolfTkSetCamera();
  // [WolfViewer 2026-10-13] from the camera-relative vector (eye space turned into the world, then into ECEF axes: exact),
  // not the point's absolute ECEF position less the camera's (aerialPerspective.glsl wolfApplyTransmittanceInscatterRel)
  vec3 relECEF = mat3(worldToECEFMatrix) * (mat3(wolfTkEyeToWorld) * eyePos) * METER_TO_LENGTH_UNIT;
  wolfApplyTransmittanceInscatterRel(relECEF, 0.0, radiance);
  return radiance;
}

// Source: aerialPerspectiveEffect.frag:329-331 (positions) and :354-358 (the shadow: getSTBN, getShadowRadius,
// sampleShadowOpticalDepth, exp(-opticalDepth)). Used by WolfViewer on the sun term of LL's own lighting
// (takram applies it to its post-process sun light, :116-118): an adaptation, see wolfenvironmentrender.cpp.
float wolfTkCloudShadow(vec3 eyePos)
{
  if (wolfTkShadowOn < 0.5) return 1.0;
  vec3 worldPosition = (wolfTkEyeToWorld * vec4(eyePos, 1.0)).xyz;
  vec3 positionECEF = (worldToECEFMatrix * vec4(worldPosition, 1.0)).xyz;
  positionECEF = positionECEF * METER_TO_LENGTH_UNIT + altitudeCorrection * METER_TO_LENGTH_UNIT;
  float stbn = getSTBN();
  float radius = getShadowRadius(worldPosition);
  float opticalDepth = sampleShadowOpticalDepth(worldPosition, positionECEF, radius, stbn);
  // [WolfViewer 2026-10-13] past the shadow map (shadowFar) sampleShadowOpticalDepth returns 0 = full sun, so under a cloud
  // field the shaded near ground met fully sunlit far ground in a hard line that read as a haze band in front of the
  // mountains (Paul, Tromso, WE on only). Over the last fifth of the map's reach the shadow fades into the field's mean.
  float d = length(worldPosition - cameraPosition);
  float far = smoothstep(0.8 * shadowFar, shadowFar, d);
  return mix(exp(-opticalDepth), wolfTkFarCloudT, far);
}

// [WolfViewer] The light under the clouds, for the light meter and the scene's sky light (not in three-geospatial,
// whose SkyLightProbe is the clear sky's irradiance): the clouds' own Beer shadow map read over the sky the camera
// sees, each column through a two-stream layer.
//   - directions: 64 cosine-weighted directions of the upper hemisphere (Hammersley points, as LL's
//     irradianceGenF.glsl samples its probes), each followed from the camera to the shadow layers' bottom height
//     (`cloudBaseHeight`, metres above the sea); the column above that point is read from the shadow map
//     (in the first cascade that holds it; a point no cascade holds is left out);
//   - the column's slant optical depth (towards the clouds' light: the direction the shadow map is cast along,
//     wolfTkCloudLightDirection - the sun, or the moon once it lights the clouds, wolfenvironmentrender.cpp
//     Frame::cloudLightECEF) times mu_s is its vertical optical depth tau;
//   - its diffuse transmittance, Stephens, Colorado State AT622 notes section 15 "Radiative Transfer Revisited:
//     Two-Stream Models", eq. 15.20b for a non-absorbing layer: T = 2 / (2 + (1 - g) tau), with g = 0.85 for water
//     droplet clouds (same section, example 15.5: "g is quasi-constant and ~ 0.85");
//   - its direct (unscattered) transmittance exp(-optical depth).
// Returns (the sun's direct transmittance at the camera, mean diffuse transmittance, mean diffusely transmitted
// share of the direct beam = max(T - T_direct, 0)); cosine-weighted means, so they weight the light on a
// horizontal surface. No clouds (or none of the columns known): (1, 1, 0).
uniform float wolfTkCloudBaseHeight;
uniform vec3 wolfTkCloudLightDirection;   // [WolfViewer round 8] ECEF, bindSceneUniforms
vec3 wolfTkCloudSkyLight()
{
  if (wolfTkShadowOn < 0.5) return vec3(1.0, 1.0, 0.0);
  float direct = wolfTkCloudShadow(vec3(0.0, 0.0, -2.0 * max(cameraNear, 0.01)));
  vec3 camECEF = (worldToECEFMatrix * vec4(cameraPosition, 1.0)).xyz;
  vec3 upECEF = normalize(camECEF);
  float cameraHeight = length(camECEF) - bottomRadius;
  float muS = max(dot(wolfTkCloudLightDirection, upECEF), 0.05);
  float sumT = 0.0, sumS = 0.0, count = 0.0;
  for (int i = 0; i < 64; ++i)
  {
    // irradianceGenF.glsl hammersley2d + Lambertian (cosine-weighted), about the world's up (LL agent z)
    uint bits = uint(i);
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    vec2 xi = vec2(float(i) / 64.0, float(bits) * 2.3283064365386963e-10);
    float cosTheta = sqrt(1.0 - xi.y), sinTheta = sqrt(xi.y), phi = 2.0 * PI * xi.x;
    vec3 dir = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float t = max(wolfTkCloudBaseHeight - cameraHeight, 0.0) / max(dir.z, 0.05);
    vec3 worldPosition = cameraPosition + dir * t;
    vec3 positionECEF = (worldToECEFMatrix * vec4(worldPosition, 1.0)).xyz * METER_TO_LENGTH_UNIT + altitudeCorrection * METER_TO_LENGTH_UNIT;
    float distanceToTop = getDistanceToShadowTop(positionECEF);
    float opticalDepth = -1.0;
    if (distanceToTop <= 0.0) opticalDepth = 0.0;
    else
    {
      for (int c = 0; c < SHADOW_CASCADE_COUNT; ++c)
      {
        vec2 uv = getShadowUv(worldPosition, c);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
        {
          // the whole column: shadow.frag (clouds/src/shaders/shadow.frag:99-159) stores r frontDepth, g mean
          // extinction, b the optical depth marched, a the tail - non-zero only where the march stopped early at its
          // minimum transmittance (the rest of the column unmarched); there the mean extinction over the distance
          // behind the front depth stands for it (readShadowOpticalDepth's second term), else the march is complete.
          // Not readShadowOpticalDepth itself, which caps at the marched depth (min(b, ...)) for the sun term: the
          // diffuse transmittance needs the whole column (the mean extinction across gaps can over-read it)
          vec4 bsm = textureLod(shadowBuffer, vec3(uv, float(c)), 0.0);
          opticalDepth = bsm.a > 0.0 ? max(bsm.b, bsm.g * max(0.0, distanceToTop - bsm.r)) : bsm.b;
          break;
        }
      }
    }
    if (opticalDepth < 0.0) continue;
    float tau = opticalDepth * muS;
    float T = 2.0 / (2.0 + (1.0 - 0.85) * tau);
    sumT += T;
    sumS += max(T - exp(-opticalDepth), 0.0);
    count += 1.0;
  }
  if (count < 1.0) return vec3(direct, 1.0, 0.0);
  return vec3(direct, sumT / count, sumS / count);
}
