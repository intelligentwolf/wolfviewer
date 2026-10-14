/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/cloudsResolve.frag.
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
precision highp sampler2DArray;

#wolf_include "wolftk/coreTurbo.glsl"   // takram: #include "core/turbo" (line 4)
#wolf_include "wolftk/catmullRomSampling.glsl"   // takram: #include "catmullRomSampling" (line 5)
#wolf_include "wolftk/varianceClipping.glsl"   // takram: #include "varianceClipping" (line 6)

uniform sampler2D colorBuffer;
uniform sampler2D depthVelocityBuffer;
uniform sampler2D colorHistoryBuffer;

#ifdef SHADOW_LENGTH
uniform sampler2D shadowLengthBuffer;
uniform sampler2D shadowLengthHistoryBuffer;
#endif // SHADOW_LENGTH

uniform vec2 texelSize;
uniform int frame;
uniform float varianceGamma;
uniform float temporalAlpha;
uniform vec2 jitterOffset;

// [WolfViewer round 13] the trails round objects when the camera moves (Paul, 2026-10-13). temporalUpscale below keeps
// a pixel's cloud history for 15 frames of 16 and rejects it only off screen (cloudsResolve.frag:63-118); a pixel an
// object now covers keeps the cloud it showed when it was sky, clipped only against its 3 x 3 low-res neighbourhood
// (varianceClipping, gamma 2), which at a silhouette holds both. The full-resolution scene depth says which surface a
// pixel shows; the clouds pass now stores the surface its texel's ray stopped at (depthVelocity.a, 0 = the sky). A
// pixel whose own texel saw another surface takes the first texel of its 3 x 3 ring that saw its own, and no history
// (the history carried the other surface's clouds). WolfViewer's own rule; takram has no depth rejection here.
uniform sampler2D depthBuffer;
uniform float cameraNear;
uniform float cameraFar;
#wolf_include "wolftk/coreDepth.glsl"
float wolfSceneDistance(const ivec2 coord) {
  ivec2 size = textureSize(depthBuffer, 0);
  float depth = texelFetch(depthBuffer, clamp(coord, ivec2(0), size - ivec2(1)), 0).r;
  if (depth >= 1.0 - 1e-7) return 0.0;   // nothing there: the sky
  float d = linearizeDepth(reverseLogDepth(depth, cameraNear, cameraFar), cameraNear, cameraFar);
  // [WolfViewer 2026-10-13] the viewer's sky is drawn AT the far clip, not at depth 1.0 exactly (the harness clears to
  // 1.0), so a sky pixel read as a surface ~far away, every sky texel failed wolfSameSurface against the clouds' "0 = sky",
  // and the history was dropped on every sky pixel every frame: the clouds were rebuilt from 1/16 of the pixels each
  // frame and dissolved and re-formed in ~0.5 s (Paul's video 09:53). Anything at the far clip is the sky.
  if (d >= 0.95 * cameraFar) return 0.0;
  return d;
}
bool wolfSameSurface(const float a, const float b) {
  // both the sky, or both a surface within a factor of 2 (a ray distance against a view distance: a cosine apart)
  if (a <= 0.0 || b <= 0.0) return (a <= 0.0) == (b <= 0.0);
  return max(a, b) <= 2.0 * min(a, b);
}

in vec2 vUv;

layout(location = 0) out vec4 outputColor;
#ifdef SHADOW_LENGTH
layout(location = 1) out float outputShadowLength;
#endif // SHADOW_LENGTH

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:30-40 neighborOffsets
const ivec2 neighborOffsets[9] = ivec2[9](
  ivec2(-1, -1),
  ivec2(-1, 0),
  ivec2(-1, 1),
  ivec2(0, -1),
  ivec2(0, 0),
  ivec2(0, 1),
  ivec2(1, -1),
  ivec2(1, 0),
  ivec2(1, 1)
);

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:42-47 bayerIndices
const ivec4[4] bayerIndices = ivec4[4](
  ivec4(0, 12, 3, 15),
  ivec4(8, 4, 11, 7),
  ivec4(2, 14, 1, 13),
  ivec4(10, 6, 9, 5)
);

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:49-61 getClosestFragment
vec4 getClosestFragment(const ivec2 coord) {
  vec4 result = vec4(1e7, 0.0, 0.0, 0.0);
  vec4 neighbor;
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[0]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[1]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[2]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[3]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[4]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[5]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[6]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[7]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(depthVelocityBuffer, coord, 0, neighborOffsets[8]);
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
  return result;
}

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:63-118 temporalUpscale
void temporalUpscale(
  const ivec2 coord,
  const ivec2 lowResCoord,
  const bool currentFrame,
  out vec4 outputColor,
  out float outputShadowLength
) {
  // [WolfViewer round 13] the depth-consistent texel (wolfSceneDistance above)
  float sceneD = wolfSceneDistance(coord);
  ivec2 dvSize = textureSize(depthVelocityBuffer, 0);
  ivec2 pick = lowResCoord;
  // [WolfViewer 2026-10-13] the round-13 depth rejection is OFF: in the viewer it dropped the cloud history on sky
  // pixels and the clouds re-formed every frame (Paul: "the clouds are STILL running very very fast"). takram's own
  // resolve (cloudsResolve.frag:63-118) has no depth rejection; back to it.
  bool ownOk = true;
  if (!ownOk) {
    for (int i = 0; i < 9; ++i) {
      ivec2 c = clamp(lowResCoord + neighborOffsets[i], ivec2(0), dvSize - ivec2(1));
      if (wolfSameSurface(max(-texelFetch(depthVelocityBuffer, c, 0).a, 0.0), sceneD)) { pick = c; break; }   // .a: -(scene distance), or >= 0 over the sky (cloudsF.glsl)
    }
  }
  vec4 currentColor = texelFetch(colorBuffer, pick, 0);
  #ifdef SHADOW_LENGTH
  vec4 currentShadowLength = vec4(texelFetch(shadowLengthBuffer, pick, 0).rgb, 1.0);
  #endif // SHADOW_LENGTH

  if (currentFrame || !ownOk) {
    // Use the texel just rendered without any accumulation.
    outputColor = currentColor;
    #ifdef SHADOW_LENGTH
    outputShadowLength = currentShadowLength.r;
    #endif // SHADOW_LENGTH
    return;
  }

  vec4 depthVelocity = getClosestFragment(lowResCoord);
  vec2 velocity = depthVelocity.gb;
  vec2 prevUv = vUv - velocity;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    outputColor = currentColor;
    #ifdef SHADOW_LENGTH
    outputShadowLength = currentShadowLength.r;
    #endif // SHADOW_LENGTH
    return; // Rejection
  }

  // Variance clipping with a large variance gamma seems to work fine for
  // upsampling. This increases ghosting, of course, but it's hard to notice on
  // clouds.
  // vec4 historyColor = textureCatmullRom(colorHistoryBuffer, prevUv);
  vec4 historyColor = texture(colorHistoryBuffer, prevUv);
  vec4 clippedColor = varianceClipping(colorBuffer, vUv, currentColor, historyColor, varianceGamma);
  outputColor = clippedColor;

  #ifdef SHADOW_LENGTH
  // Sampling the shadow length history using scene depth doesn't make much
  // sense, but it's too hard to derive it properly. At least this approach
  // resolves the edges of scene objects.
  // vec4 historyShadowLength = vec4(textureCatmullRom(shadowLengthHistoryBuffer, prevUv).rgb, 1.0);
  vec4 historyShadowLength = vec4(texture(shadowLengthHistoryBuffer, prevUv).rgb, 1.0);
  vec4 clippedShadowLength = varianceClipping(
    shadowLengthBuffer,
    vUv,
    currentShadowLength,
    historyShadowLength,
    varianceGamma
  );
  outputShadowLength = clippedShadowLength.r;
  #endif // SHADOW_LENGTH
}

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:120-152 temporalAntialiasing
void temporalAntialiasing(const ivec2 coord, out vec4 outputColor, out float outputShadowLength) {
  vec4 currentColor = texelFetch(colorBuffer, coord, 0);
  #ifdef SHADOW_LENGTH
  vec4 currentShadowLength = vec4(texelFetch(shadowLengthBuffer, coord, 0).rgb, 1.0);
  #endif // SHADOW_LENGTH

  vec4 depthVelocity = getClosestFragment(coord);
  vec2 velocity = depthVelocity.gb;

  vec2 prevUv = vUv - velocity;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    outputColor = currentColor;
    #ifdef SHADOW_LENGTH
    outputShadowLength = currentShadowLength.r;
    #endif // SHADOW_LENGTH
    return; // Rejection
  }

  vec4 historyColor = texture(colorHistoryBuffer, prevUv);
  vec4 clippedColor = varianceClipping(colorBuffer, coord, currentColor, historyColor);
  outputColor = mix(clippedColor, currentColor, temporalAlpha);

  #ifdef SHADOW_LENGTH
  vec4 historyShadowLength = vec4(texture(shadowLengthHistoryBuffer, prevUv).rgb, 1.0);
  vec4 clippedShadowLength = varianceClipping(
    shadowLengthBuffer,
    coord,
    currentShadowLength,
    historyShadowLength
  );
  outputShadowLength = mix(clippedShadowLength.r, currentShadowLength.r, temporalAlpha);
  #endif // SHADOW_LENGTH
}

// Source: three-geospatial packages/clouds/src/shaders/cloudsResolve.frag:154-177 main
void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy);

  #if !defined(SHADOW_LENGTH)
  float outputShadowLength;
  #endif // !defined(SHADOW_LENGTH)

  #ifdef TEMPORAL_UPSCALE
  ivec2 lowResCoord = coord / 4;
  int bayerValue = bayerIndices[coord.x % 4][coord.y % 4];
  bool currentFrame = bayerValue == frame % 16;
  temporalUpscale(coord, lowResCoord, currentFrame, outputColor, outputShadowLength);
  #else // TEMPORAL_UPSCALE
  temporalAntialiasing(coord, outputColor, outputShadowLength);
  #endif // TEMPORAL_UPSCALE

  #if defined(SHADOW_LENGTH) && defined(DEBUG_SHOW_SHADOW_LENGTH)
  outputColor = vec4(turbo(outputShadowLength * 0.05), 1.0);
  #endif // defined(SHADOW_LENGTH) && defined(DEBUG_SHOW_SHADOW_LENGTH)

  #ifdef DEBUG_SHOW_VELOCITY
  outputColor.rgb = outputColor.rgb + vec3(abs(texture(depthVelocityBuffer, vUv).gb) * 10.0, 0.0);
  #endif // DEBUG_SHOW_VELOCITY
}
