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

// [WolfViewer 2026-10-16] THE WHITE BORDER ROUND OBJECTS (Paul: "sometimes we see like a white border round objects but i
// think it might be clouds"; the trails round objects of round 13, 2026-10-13, are the same fault). The clouds are marched
// at 1/4 x 1/4 of the pixels and temporalUpscale below rebuilds the full frame: each pixel takes a new value one frame in
// 16 (its Bayer slot) and otherwise its reprojected history, clipped against the 5-tap neighbourhood of the low-res frame
// (cloudsResolve.frag:63-118, varianceClipping gamma 2). That neighbourhood is +-1 low-res texel = +-4 screen pixels, so
// at a silhouette it holds both the bright cloud/sky texels and the object's dark ones; a pixel an object now covers (the
// object or the camera moved, an avatar sways) keeps the white it showed as sky, and the bilinear history fetch smears it
// one more pixel in each frame of motion, until its own slot comes round 16 frames later. Over an object the clouds'
// overlay should be ~0 (the march stops at the scene, cloudsF.glsl getRayDistanceToScene), so the leak is a white rim.
// takram has no depth test here (stock Firestorm has no volumetric clouds: its 2D layer is drawn in the sky before the scene).
//
// The fix: which surface a pixel shows is known exactly from the full-resolution depth, and which surface each low-res
// texel's ray stopped at this frame is in depthVelocity.a (cloudsF.glsl: the scene's view depth as a negative number, or
// >= 0 over the sky). Where the 3 x 3 low-res neighbourhood shows only this pixel's surface, takram's resolve runs
// unchanged. Where it straddles an edge, the clip box and the reprojection velocity come from the texels of this pixel's
// own surface only, so the other surface's colour can neither be kept from the history nor pulled in by the clip.
// (Round 13's attempt, switched off the same day, dropped the history outright wherever the pixel's own texel disagreed,
// and its last form classed the sky by its own rule - anything past 0.95 x far - where the clouds pass calls only depth
// >= 1 - 1e-7 the sky, so pixels the two classed differently lost their history every frame and the clouds re-formed
// (Paul: "the clouds are STILL running very very fast"). Here the history is never dropped, only clipped, and both sides
// use the clouds pass's own test, cloudsF.glsl getRayDistanceToScene.)
uniform sampler2D depthBuffer;
uniform float cameraNear;
uniform float cameraFar;
#wolf_include "wolftk/coreDepth.glsl"
// the view depth of the surface a full-resolution pixel shows, 0 = the sky (the clouds pass's test and depth read: DEPTH_PACKING
// 3200 = .r, no log depth)
float wolfSceneDepth(const ivec2 coord) {
  ivec2 size = textureSize(depthBuffer, 0);
  float depth = texelFetch(depthBuffer, clamp(coord, ivec2(0), size - ivec2(1)), 0).r;
  if (depth >= 1.0 - 1e-7) return 0.0;   // cloudsF.glsl getRayDistanceToScene: depth < 1 - 1e-7 is a surface
  return linearizeDepth(reverseLogDepth(depth, cameraNear, cameraFar), cameraNear, cameraFar);
}
// the view depth of the surface a low-res texel's ray stopped at this frame, 0 = the sky (cloudsF.glsl outputDepthVelocity.a)
float wolfTexelDepth(const vec4 depthVelocity) {
  return max(-depthVelocity.a, 0.0);
}
bool wolfSameSurface(const float a, const float b) {
  // both the sky, or both a surface within a factor of 2 of each other's view depth: an avatar against the wall or ground
  // just behind it is one surface here (the clouds' overlay over both is the same, ~0); an object against far land is not
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
// [WolfViewer 2026-10-16] with the depth-aware neighbourhood at edges (the note above wolfSceneDepth)
void temporalUpscale(
  const ivec2 coord,
  const ivec2 lowResCoord,
  const bool currentFrame,
  out vec4 outputColor,
  out float outputShadowLength
) {
  vec4 currentColor = texelFetch(colorBuffer, lowResCoord, 0);
  #ifdef SHADOW_LENGTH
  vec4 currentShadowLength = vec4(texelFetch(shadowLengthBuffer, lowResCoord, 0).rgb, 1.0);
  #endif // SHADOW_LENGTH

  if (currentFrame) {
    // Use the texel just rendered without any accumulation.
    // [WolfViewer] this frame the texel's ray went through this very pixel (the jitter, CloudsMaterial temporalJitter, puts
    // it on the pixel whose Bayer slot this is), so it saw this pixel's own surface
    outputColor = currentColor;
    #ifdef SHADOW_LENGTH
    outputShadowLength = currentShadowLength.r;
    #endif // SHADOW_LENGTH
    return;
  }

  // [WolfViewer 2026-10-16] which of the 3 x 3 texels saw this pixel's surface; the closest fragment (getClosestFragment,
  // takram's smallest front depth) over all of them and over those alone. The fetches are clamped to the low-res frame where
  // getClosestFragment's texelFetchOffset ran off it on the outermost ring (undefined there; most drivers return 0)
  float sceneDepth = wolfSceneDepth(coord);
  ivec2 lowResMax = textureSize(depthVelocityBuffer, 0) - ivec2(1);
  vec4 closestAll = vec4(1e7, 0.0, 0.0, 0.0);
  vec4 closestSame = vec4(1e7, 0.0, 0.0, 0.0);
  int sameMask = 0;
  int sameCount = 0;
  for (int i = 0; i < 9; ++i) {
    vec4 neighbor = texelFetch(depthVelocityBuffer, clamp(lowResCoord + neighborOffsets[i], ivec2(0), lowResMax), 0);
    if (neighbor.r < closestAll.r) {
      closestAll = neighbor;
    }
    if (wolfSameSurface(wolfTexelDepth(neighbor), sceneDepth)) {
      sameMask |= 1 << i;
      ++sameCount;
      if (neighbor.r < closestSame.r) {
        closestSame = neighbor;
      }
    }
  }
  // [WolfViewer 2026-10-16] THIN FEATURES: a surface thinner than the 4-pixel texels that no texel landed on this frame (a
  // mast, rigging, a thin prim frame) has nothing of its own here - all 9 texels saw the sky or a far surface behind it.
  // takram's resolve then gives it that other surface's clouds and haze (the overlay carries the EEP haze too, cloudsF.glsl
  // wolfLLHaze), which paints sky-bright cloud over the mast in dashes, changing every frame as the Bayer slots come round.
  // Paul 10-16: "the masts are flickering" (ship masts cut into dashes against the sky); Elisa Porter 10-15: thin prims
  // "look glitched ... only on shared environment" (bright lines along stage frames). A NEAR surface has no cloud in front
  // of it and next to no haze, so it gets no overlay at all. Within WOLF_THIN_NEAR_M (a design value: at 256 m the
  // EEP/physical haze in front of an object is a few per cent, against the full sky value takram would give it); beyond
  // that takram's resolve keeps running, so far thin objects still fade into the haze. The pixel's own Bayer slot (the
  // currentFrame branch above) still takes its texel as it is, since that frame's ray went through this very pixel.
  const float WOLF_THIN_NEAR_M = 256.0;
  if (sameCount == 0 && sceneDepth > 0.0 && sceneDepth < WOLF_THIN_NEAR_M) {
    outputColor = vec4(0.0);
    #ifdef SHADOW_LENGTH
    outputShadowLength = 0.0;
    #endif // SHADOW_LENGTH
    return;
  }

  // an edge: some texels saw this pixel's surface and some another. With none (a far feature thinner than the 4-pixel
  // texels that no texel landed on this frame) there is nothing of this surface to clip to, and takram's resolve runs as it is.
  bool edge = sameCount > 0 && sameCount < 9;

  // the clip box of this pixel's surface at an edge: the mean and deviation of its own texels (varianceClipping's moments,
  // texelFetch'd as cloudsResolve.frag's getClosestFragment fetches); its mean is also this surface's current value, the
  // nearest-surface upsample used where takram uses the pixel's own texel
  vec4 sameMean = currentColor;
  vec4 sameMin = currentColor;
  vec4 sameMax = currentColor;
  #ifdef SHADOW_LENGTH
  vec4 sameShadowMean = currentShadowLength;
  vec4 sameShadowMin = currentShadowLength;
  vec4 sameShadowMax = currentShadowLength;
  #endif // SHADOW_LENGTH
  if (edge) {
    vec4 moment1 = vec4(0.0);
    vec4 moment2 = vec4(0.0);
    #ifdef SHADOW_LENGTH
    vec4 shadowMoment1 = vec4(0.0);
    vec4 shadowMoment2 = vec4(0.0);
    #endif // SHADOW_LENGTH
    for (int i = 0; i < 9; ++i) {
      if ((sameMask & (1 << i)) != 0) {
        ivec2 c = clamp(lowResCoord + neighborOffsets[i], ivec2(0), lowResMax);
        vec4 neighbor = texelFetch(colorBuffer, c, 0);
        moment1 += neighbor;
        moment2 += neighbor * neighbor;
        #ifdef SHADOW_LENGTH
        vec4 shadowNeighbor = vec4(texelFetch(shadowLengthBuffer, c, 0).rgb, 1.0);
        shadowMoment1 += shadowNeighbor;
        shadowMoment2 += shadowNeighbor * shadowNeighbor;
        #endif // SHADOW_LENGTH
      }
    }
    // Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:42-68 varianceClipping (the box from the moments)
    float N = float(sameCount);
    sameMean = moment1 / N;
    vec4 deviation = sqrt(max(moment2 / N - sameMean * sameMean, 0.0)) * varianceGamma;
    sameMin = sameMean - deviation;
    sameMax = sameMean + deviation;
    #ifdef SHADOW_LENGTH
    sameShadowMean = shadowMoment1 / N;
    vec4 shadowDeviation = sqrt(max(shadowMoment2 / N - sameShadowMean * sameShadowMean, 0.0)) * varianceGamma;
    sameShadowMin = sameShadowMean - shadowDeviation;
    sameShadowMax = sameShadowMean + shadowDeviation;
    #endif // SHADOW_LENGTH
  }

  vec4 depthVelocity = edge ? closestSame : closestAll;
  vec2 velocity = depthVelocity.gb;
  vec2 prevUv = vUv - velocity;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    outputColor = edge ? sameMean : currentColor;
    #ifdef SHADOW_LENGTH
    outputShadowLength = edge ? sameShadowMean.r : currentShadowLength.r;
    #endif // SHADOW_LENGTH
    return; // Rejection
  }

  // Variance clipping with a large variance gamma seems to work fine for
  // upsampling. This increases ghosting, of course, but it's hard to notice on
  // clouds.
  // vec4 historyColor = textureCatmullRom(colorHistoryBuffer, prevUv);
  vec4 historyColor = texture(colorHistoryBuffer, prevUv);
  if (edge) {
    // Source: varianceClipping.glsl:42-68 (its last line, with this surface's box)
    outputColor = clipAABB(clamp(sameMean, sameMin, sameMax), historyColor, sameMin, sameMax);
  } else {
    outputColor = varianceClipping(colorBuffer, vUv, currentColor, historyColor, varianceGamma);
  }

  #ifdef SHADOW_LENGTH
  // Sampling the shadow length history using scene depth doesn't make much
  // sense, but it's too hard to derive it properly. At least this approach
  // resolves the edges of scene objects.
  // vec4 historyShadowLength = vec4(textureCatmullRom(shadowLengthHistoryBuffer, prevUv).rgb, 1.0);
  vec4 historyShadowLength = vec4(texture(shadowLengthHistoryBuffer, prevUv).rgb, 1.0);
  vec4 clippedShadowLength;
  if (edge) {
    clippedShadowLength = clipAABB(clamp(sameShadowMean, sameShadowMin, sameShadowMax), historyShadowLength, sameShadowMin, sameShadowMax);
  } else {
    clippedShadowLength = varianceClipping(
      shadowLengthBuffer,
      vUv,
      currentShadowLength,
      historyShadowLength,
      varianceGamma
    );
  }
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
