/**
 * @file wolftk/compositeF.glsl
 * @brief The aerial perspective over the lit opaque frame, with the clouds laid over it (three-geospatial
 *        AerialPerspectiveEffect in light-source lighting: TRANSMITTANCE + INSCATTER, HAS_OVERLAY, HAS_SHADOW_LENGTH).
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:271-390 mainImage (the uniforms and functions
 *   before it: wolftk/aerialPerspective.glsl), packages/clouds/src/shaders/clouds.frag:101-107 getViewZ (the
 *   postprocessing Effect framework's, which takram repeats there).
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
 * WolfViewer integration (WolfEnvironmentRender::postPassInner): the input is LL's lit HDR frame (mRT->screen),
 * the depth LL's deferred depth (standard perspective, DEPTH_PACKING 0: readDepthValue = texture().r; no log depth,
 * so reverseLogDepth is the identity), SUN_LIGHT / SKY_LIGHT off (the scene is lit by LL's own shaders with the
 * atmosphere's sun and sky light: light-source lighting, three-geospatial README "Rules of thumb"), SKY off (the sky
 * pixels keep the dome's colour - wolftk/skyF.glsl, the stars and the Wolf bodies over it - as mainImage's
 * `outputColor = inputColor` branch does), HAS_SHADOW off (its only use, :116-118, is the post-process sun term),
 * no normals needed (no post-process lighting).
 */

#define DEPTH_PACKING 0
#define PERSPECTIVE_CAMERA 1
#define TRANSMITTANCE 1
#define INSCATTER 1

in vec2 vUv;
in vec3 vCameraPosition;
in vec3 vRayDirection;
in vec3 vGeometryAltitudeCorrection;
in vec3 vEllipsoidRadiiSquared;

uniform float wolfEepNoAerial;   // [WolfViewer 2026-10-13] 1: no aerial perspective on the opaque scene (the overlay carries the stock EEP haze)
uniform sampler2D inputBuffer;   // postprocessing Effect input
uniform sampler2D depthBuffer;   // postprocessing EffectAttribute.DEPTH

#wolf_include "wolftk/aerialPerspective.glsl"

out vec4 frag_color;

// Source: clouds.frag:101-107 getViewZ
float getViewZ(const float depth) {
  #ifdef PERSPECTIVE_CAMERA
  return perspectiveDepthToViewZ(depth, cameraNear, cameraFar);
  #else // PERSPECTIVE_CAMERA
  return orthographicDepthToViewZ(depth, cameraNear, cameraFar);
  #endif // PERSPECTIVE_CAMERA
}

// Source: aerialPerspectiveEffect.frag:271-390 mainImage (the branches for defines this program never sets -
// DEBUG_SHOW_LIGHTING_MASK, SKY, RECONSTRUCT_NORMAL, HAS_NORMALS, CORRECT_GEOMETRIC_ERROR, HAS_SHADOW, SUN_LIGHT,
// SKY_LIGHT, HAS_LIGHTING_MASK - kept as the source has them)
void mainImage(const vec4 inputColor, const vec2 uv, out vec4 outputColor) {
  #if defined(HAS_LIGHTING_MASK) && defined(DEBUG_SHOW_LIGHTING_MASK)
  outputColor.rgb = vec3(texture(lightingMaskBuffer, uv).LIGHTING_MASK_CHANNEL_);
  outputColor.a = 1.0;
  return;
  #endif // defined(HAS_LIGHTING_MASK) && defined(DEBUG_SHOW_LIGHTING_MASK)

  float shadowLength = 0.0;
  #ifdef HAS_SHADOW_LENGTH
  shadowLength = texture(shadowLengthBuffer, uv).r;
  #endif // HAS_SHADOW_LENGTH

  #ifdef HAS_OVERLAY
  vec4 overlay = texture(overlayBuffer, uv);
  if (overlay.a == 1.0) {
    outputColor = overlay;
    return;
  }
  #endif // HAS_OVERLAY

  vec3 rayDirection = normalize(vRayDirection);
  vec3 dRDdx = dFdx(rayDirection);
  vec3 dRDdy = dFdy(rayDirection);
  float fragmentAngle = length(dRDdx + dRDdy) / length(rayDirection);

  float depth = readDepthValue(depthBuffer, uv);
  if (depth >= 1.0 - 1e-8) {
    #ifdef SKY
    outputColor.rgb = getSkyRadiance(
      vCameraPosition,
      rayDirection,
      shadowLength,
      sunDirection,
      moonDirection,
      moonAngularRadius,
      lunarRadianceScale,
      fragmentAngle
    );
    outputColor.a = 1.0;
    #else // SKY
    outputColor = inputColor;
    #endif // SKY

    #ifdef HAS_OVERLAY
    outputColor.rgb = outputColor.rgb * (1.0 - overlay.a) + overlay.rgb;
    #endif // HAS_OVERLAY
    return;
  }
  depth = reverseLogDepth(depth, cameraNear, cameraFar);

  // Reconstruct position and normal in world space.
  vec3 viewPosition = screenToView(
    uv,
    depth,
    getViewZ(depth),
    projectionMatrix,
    inverseProjectionMatrix
  );
  vec3 worldPosition = (inverseViewMatrix * vec4(viewPosition, 1.0)).xyz;
  vec3 positionECEF = (worldToECEFMatrix * vec4(worldPosition, 1.0)).xyz;
  positionECEF = positionECEF * METER_TO_LENGTH_UNIT + vGeometryAltitudeCorrection;

  vec3 viewNormal;
  bool degenerateNormal = false;
  #ifdef RECONSTRUCT_NORMAL
  vec3 dVPdx = dFdx(viewPosition);
  vec3 dVPdy = dFdy(viewPosition);
  viewNormal = normalize(cross(dVPdx, dVPdy));
  #elif defined(HAS_NORMALS)
  viewNormal = readNormal(uv, degenerateNormal);
  #endif // defined(HAS_NORMALS)

  #if defined(RECONSTRUCT_NORMAL) || defined(HAS_NORMALS)
  vec3 worldNormal = (inverseViewMatrix * vec4(viewNormal, 0.0)).xyz;
  vec3 normalECEF = (worldToECEFMatrix * vec4(worldNormal, 0.0)).xyz;
  #else // defined(RECONSTRUCT_NORMAL) || defined(HAS_NORMALS)
  vec3 normalECEF = normalize(positionECEF);
  #endif // defined(RECONSTRUCT_NORMAL) || defined(HAS_NORMALS)

  #ifdef CORRECT_GEOMETRIC_ERROR
  correctGeometricError(positionECEF, normalECEF);
  #endif // CORRECT_GEOMETRIC_ERROR

  #ifdef HAS_SHADOW
  float stbn = getSTBN();
  float radius = getShadowRadius(worldPosition);
  float opticalDepth = sampleShadowOpticalDepth(worldPosition, positionECEF, radius, stbn);
  float sunTransmittance = exp(-opticalDepth);
  #else // HAS_SHADOW
  float sunTransmittance = 1.0;
  #endif // HAS_SHADOW

  vec3 radiance;
  #if defined(SUN_LIGHT) || defined(SKY_LIGHT)
  // WORKAROUND: When both post-process lighting and sky options are enabled,
  // stars have degenerate normals. We use this to disable irradiance, which is
  // irrelevant for them.
  if (!degenerateNormal) {
    radiance = getSunSkyIrradiance(positionECEF, normalECEF, inputColor.rgb, sunTransmittance);
  } else {
    radiance = inputColor.rgb;
  }
  #ifdef HAS_LIGHTING_MASK
  float lightingMask = texture(lightingMaskBuffer, uv).LIGHTING_MASK_CHANNEL_;
  radiance = mix(inputColor.rgb, radiance, lightingMask);
  #endif // HAS_LIGHTING_MASK
  #else // defined(SUN_LIGHT) || defined(SKY_LIGHT)
  radiance = inputColor.rgb;
  #endif // defined(SUN_LIGHT) || defined(SKY_LIGHT)

  #if defined(TRANSMITTANCE) || defined(INSCATTER)
  // [WolfViewer 2026-10-13] from the camera-relative vector: the view position turned into the world, then into ECEF axes
  // (no translation: exact), in length units - not positionECEF - vCameraPosition (aerialPerspective.glsl
  // wolfApplyTransmittanceInscatterRel)
  vec3 relECEF = mat3(worldToECEFMatrix) * (mat3(inverseViewMatrix) * viewPosition) * METER_TO_LENGTH_UNIT;
  // [WolfViewer 2026-10-13] with an EEP driving the sky the opaque scene takes the stock EEP haze (cloudsF.glsl wolfLLHaze,
  // in the overlay) and not this physical aerial perspective as well
  if (wolfEepNoAerial < 0.5) wolfApplyTransmittanceInscatterRel(relECEF, shadowLength, radiance);
  #endif // defined(TRANSMITTANCE) || defined(INSCATTER)

  outputColor = vec4(radiance, inputColor.a);

  #ifdef HAS_OVERLAY
  outputColor.rgb = outputColor.rgb * (1.0 - overlay.a) + overlay.rgb;
  #endif // HAS_OVERLAY
}

// The Effect framework's main (postprocessing EffectMaterial, blend NORMAL: the effect's output replaces the input)
void main() {
  vec4 inputColor = texture(inputBuffer, vUv);
  vec4 outputColor;
  mainImage(inputColor, vUv, outputColor);
  // never an infinity or a NaN in the frame (fp16 holds 65504)
  if (any(isnan(outputColor.rgb)) || any(isinf(outputColor.rgb))) outputColor.rgb = inputColor.rgb;
  frag_color = vec4(max(outputColor.rgb, vec3(0.0)), inputColor.a);
}
