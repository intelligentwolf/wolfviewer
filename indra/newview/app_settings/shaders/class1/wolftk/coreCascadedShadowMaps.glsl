/**
 * @file wolftk/coreCascadedShadowMaps.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/core/src/shaders/cascadedShadowMaps.glsl (complete, lines 1-79)
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
 * Needs viewZToOrthographicDepth (three.js <packing>: wolftk/threePacking.glsl) and saturate
 * (wolftk/coreMath.glsl or threeCommon.glsl) included before it, and SHADOW_CASCADE_COUNT defined.
 * The loops are unrolled as three-geospatial packages/core/src/unrollLoops.ts:6-23 (loopReplacer, unrollLoops) unrollLoops() emits them.
 */

#ifndef WOLF_TK_CORECASCADEDSHADOWMAPS_GLSL
#define WOLF_TK_CORECASCADEDSHADOWMAPS_GLSL

// Reference: https://github.com/mrdoob/three.js/blob/r171/examples/jsm/csm/CSMShader.js

#ifndef SHADOW_CASCADE_COUNT
#error "SHADOW_CASCADE_COUNT macro must be defined."
#endif // SHADOW_CASCADE_COUNT

// Source: three-geospatial packages/core/src/shaders/cascadedShadowMaps.glsl:7 getCascadeIndex
int getCascadeIndex(
  const mat4 viewMatrix,
  const vec3 worldPosition,
  const vec2 intervals[SHADOW_CASCADE_COUNT],
  const float near,
  const float far
) {
  vec4 viewPosition = viewMatrix * vec4(worldPosition, 1.0);
  float depth = viewZToOrthographicDepth(viewPosition.z, near, far);
  vec2 interval;
  
    #if 0 < SHADOW_CASCADE_COUNT
    interval = intervals[0];
    if (depth >= interval.x && depth < interval.y) {
      return 0;
    }
    #endif // 0 < SHADOW_CASCADE_COUNT
  
    #if 1 < SHADOW_CASCADE_COUNT
    interval = intervals[1];
    if (depth >= interval.x && depth < interval.y) {
      return 1;
    }
    #endif // 1 < SHADOW_CASCADE_COUNT
  
    #if 2 < SHADOW_CASCADE_COUNT
    interval = intervals[2];
    if (depth >= interval.x && depth < interval.y) {
      return 2;
    }
    #endif // 2 < SHADOW_CASCADE_COUNT
  
    #if 3 < SHADOW_CASCADE_COUNT
    interval = intervals[3];
    if (depth >= interval.x && depth < interval.y) {
      return 3;
    }
    #endif // 3 < SHADOW_CASCADE_COUNT
  
  return SHADOW_CASCADE_COUNT - 1;
}

// Source: three-geospatial packages/core/src/shaders/cascadedShadowMaps.glsl:30 getFadedCascadeIndex
int getFadedCascadeIndex(
  const mat4 viewMatrix,
  const vec3 worldPosition,
  const vec2 intervals[SHADOW_CASCADE_COUNT],
  const float near,
  const float far,
  const float jitter
) {
  vec4 viewPosition = viewMatrix * vec4(worldPosition, 1.0);
  float depth = viewZToOrthographicDepth(viewPosition.z, near, far);

  vec2 interval;
  float intervalCenter;
  float closestEdge;
  float margin;
  int nextIndex = -1;
  int prevIndex = -1;
  float alpha;

  
    #if 0 < SHADOW_CASCADE_COUNT
    interval = intervals[0];
    intervalCenter = (interval.x + interval.y) * 0.5;
    closestEdge = depth < intervalCenter ? interval.x : interval.y;
    margin = closestEdge * closestEdge * 0.5;
    interval += margin * vec2(-0.5, 0.5);

    #if 0 < SHADOW_CASCADE_COUNT - 1
    if (depth >= interval.x && depth < interval.y) {
      prevIndex = nextIndex;
      nextIndex = 0;
      alpha = saturate(min(depth - interval.x, interval.y - depth) / margin);
    }
    #else // 0 < SHADOW_CASCADE_COUNT - 1
    // Don't fade out the last cascade.
    if (depth >= interval.x) {
      prevIndex = nextIndex;
      nextIndex = 0;
      alpha = saturate((depth - interval.x) / margin);
    }
    #endif // 0 < SHADOW_CASCADE_COUNT - 1
    #endif // 0 < SHADOW_CASCADE_COUNT
  
    #if 1 < SHADOW_CASCADE_COUNT
    interval = intervals[1];
    intervalCenter = (interval.x + interval.y) * 0.5;
    closestEdge = depth < intervalCenter ? interval.x : interval.y;
    margin = closestEdge * closestEdge * 0.5;
    interval += margin * vec2(-0.5, 0.5);

    #if 1 < SHADOW_CASCADE_COUNT - 1
    if (depth >= interval.x && depth < interval.y) {
      prevIndex = nextIndex;
      nextIndex = 1;
      alpha = saturate(min(depth - interval.x, interval.y - depth) / margin);
    }
    #else // 1 < SHADOW_CASCADE_COUNT - 1
    // Don't fade out the last cascade.
    if (depth >= interval.x) {
      prevIndex = nextIndex;
      nextIndex = 1;
      alpha = saturate((depth - interval.x) / margin);
    }
    #endif // 1 < SHADOW_CASCADE_COUNT - 1
    #endif // 1 < SHADOW_CASCADE_COUNT
  
    #if 2 < SHADOW_CASCADE_COUNT
    interval = intervals[2];
    intervalCenter = (interval.x + interval.y) * 0.5;
    closestEdge = depth < intervalCenter ? interval.x : interval.y;
    margin = closestEdge * closestEdge * 0.5;
    interval += margin * vec2(-0.5, 0.5);

    #if 2 < SHADOW_CASCADE_COUNT - 1
    if (depth >= interval.x && depth < interval.y) {
      prevIndex = nextIndex;
      nextIndex = 2;
      alpha = saturate(min(depth - interval.x, interval.y - depth) / margin);
    }
    #else // 2 < SHADOW_CASCADE_COUNT - 1
    // Don't fade out the last cascade.
    if (depth >= interval.x) {
      prevIndex = nextIndex;
      nextIndex = 2;
      alpha = saturate((depth - interval.x) / margin);
    }
    #endif // 2 < SHADOW_CASCADE_COUNT - 1
    #endif // 2 < SHADOW_CASCADE_COUNT
  
    #if 3 < SHADOW_CASCADE_COUNT
    interval = intervals[3];
    intervalCenter = (interval.x + interval.y) * 0.5;
    closestEdge = depth < intervalCenter ? interval.x : interval.y;
    margin = closestEdge * closestEdge * 0.5;
    interval += margin * vec2(-0.5, 0.5);

    #if 3 < SHADOW_CASCADE_COUNT - 1
    if (depth >= interval.x && depth < interval.y) {
      prevIndex = nextIndex;
      nextIndex = 3;
      alpha = saturate(min(depth - interval.x, interval.y - depth) / margin);
    }
    #else // 3 < SHADOW_CASCADE_COUNT - 1
    // Don't fade out the last cascade.
    if (depth >= interval.x) {
      prevIndex = nextIndex;
      nextIndex = 3;
      alpha = saturate((depth - interval.x) / margin);
    }
    #endif // 3 < SHADOW_CASCADE_COUNT - 1
    #endif // 3 < SHADOW_CASCADE_COUNT
  

  return jitter <= alpha
    ? nextIndex
    : prevIndex;
}
#endif // WOLF_TK_CORECASCADEDSHADOWMAPS_GLSL
