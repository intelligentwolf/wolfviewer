/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/varianceClipping.glsl.
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

#ifdef VARIANCE_9_SAMPLES
#define VARIANCE_OFFSET_COUNT 8
// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:3-12 varianceOffsets
const ivec2 varianceOffsets[8] = ivec2[8](
  ivec2(-1, -1),
  ivec2(-1, 1),
  ivec2(1, -1),
  ivec2(1, 1),
  ivec2(1, 0),
  ivec2(0, -1),
  ivec2(0, 1),
  ivec2(-1, 0)
);
#else // VARIANCE_9_SAMPLES
#define VARIANCE_OFFSET_COUNT 4
// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:15-15 varianceOffsets
const ivec2 varianceOffsets[4] = ivec2[4](ivec2(1, 0), ivec2(0, -1), ivec2(0, 1), ivec2(-1, 0));
#endif // VARIANCE_9_SAMPLES

// Reference: https://github.com/playdeadgames/temporal
// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:19-30 clipAABB
vec4 clipAABB(const vec4 current, const vec4 history, const vec4 minColor, const vec4 maxColor) {
  vec3 pClip = 0.5 * (maxColor.rgb + minColor.rgb);
  vec3 eClip = 0.5 * (maxColor.rgb - minColor.rgb) + 1e-7;
  vec4 vClip = history - vec4(pClip, current.a);
  vec3 vUnit = vClip.xyz / eClip;
  vec3 aUnit = abs(vUnit);
  float maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
  if (maUnit > 1.0) {
    return vec4(pClip, current.a) + vClip / maUnit;
  }
  return history;
}

#ifdef VARIANCE_SAMPLER_ARRAY
#define VARIANCE_SAMPLER sampler2DArray
#define VARIANCE_SAMPLER_COORD ivec3
#else // VARIANCE_SAMPLER_ARRAY
#define VARIANCE_SAMPLER sampler2D
#define VARIANCE_SAMPLER_COORD ivec2
#endif // VARIANCE_SAMPLER_ARRAY

// Variance clipping
// Reference: https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:42-68 varianceClipping
vec4 varianceClipping(
  const VARIANCE_SAMPLER inputBuffer,
  const VARIANCE_SAMPLER_COORD coord,
  const vec4 current,
  const vec4 history,
  const float gamma
) {
  vec4 moment1 = current;
  vec4 moment2 = current * current;
  vec4 neighbor;
  
    #if 0 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[0]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 0 < VARIANCE_OFFSET_COUNT
  
    #if 1 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[1]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 1 < VARIANCE_OFFSET_COUNT
  
    #if 2 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[2]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 2 < VARIANCE_OFFSET_COUNT
  
    #if 3 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[3]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 3 < VARIANCE_OFFSET_COUNT
  
    #if 4 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[4]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 4 < VARIANCE_OFFSET_COUNT
  
    #if 5 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[5]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 5 < VARIANCE_OFFSET_COUNT
  
    #if 6 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[6]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 6 < VARIANCE_OFFSET_COUNT
  
    #if 7 < VARIANCE_OFFSET_COUNT
    neighbor = texelFetchOffset(inputBuffer, coord, 0, varianceOffsets[7]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 7 < VARIANCE_OFFSET_COUNT
  

  const float N = float(VARIANCE_OFFSET_COUNT + 1);
  vec4 mean = moment1 / N;
  vec4 varianceGamma = sqrt(max(moment2 / N - mean * mean, 0.0)) * gamma;
  vec4 minColor = mean - varianceGamma;
  vec4 maxColor = mean + varianceGamma;
  return clipAABB(clamp(mean, minColor, maxColor), history, minColor, maxColor);
}

// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:70-77 varianceClipping
vec4 varianceClipping(
  const VARIANCE_SAMPLER inputBuffer,
  const VARIANCE_SAMPLER_COORD coord,
  const vec4 current,
  const vec4 history
) {
  return varianceClipping(inputBuffer, coord, current, history, 1.0);
}

// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:79-105 varianceClipping
vec4 varianceClipping(
  const sampler2D inputBuffer,
  const vec2 coord,
  const vec4 current,
  const vec4 history,
  const float gamma
) {
  vec4 moment1 = current;
  vec4 moment2 = current * current;
  vec4 neighbor;
  
    #if 0 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[0]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 0 < VARIANCE_OFFSET_COUNT
  
    #if 1 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[1]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 1 < VARIANCE_OFFSET_COUNT
  
    #if 2 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[2]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 2 < VARIANCE_OFFSET_COUNT
  
    #if 3 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[3]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 3 < VARIANCE_OFFSET_COUNT
  
    #if 4 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[4]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 4 < VARIANCE_OFFSET_COUNT
  
    #if 5 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[5]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 5 < VARIANCE_OFFSET_COUNT
  
    #if 6 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[6]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 6 < VARIANCE_OFFSET_COUNT
  
    #if 7 < VARIANCE_OFFSET_COUNT
    neighbor = textureOffset(inputBuffer, coord, varianceOffsets[7]);
    moment1 += neighbor;
    moment2 += neighbor * neighbor;
    #endif // 7 < VARIANCE_OFFSET_COUNT
  

  const float N = float(VARIANCE_OFFSET_COUNT + 1);
  vec4 mean = moment1 / N;
  vec4 varianceGamma = sqrt(max(moment2 / N - mean * mean, 0.0)) * gamma;
  vec4 minColor = mean - varianceGamma;
  vec4 maxColor = mean + varianceGamma;
  return clipAABB(clamp(mean, minColor, maxColor), history, minColor, maxColor);
}

// Source: three-geospatial packages/clouds/src/shaders/varianceClipping.glsl:107-114 varianceClipping
vec4 varianceClipping(
  const sampler2D inputBuffer,
  const vec2 coord,
  const vec4 current,
  const vec4 history
) {
  return varianceClipping(inputBuffer, coord, current, history, 1.0);
}
