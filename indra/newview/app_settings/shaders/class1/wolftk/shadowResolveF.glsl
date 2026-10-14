/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/shadowResolve.frag.
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

#define VARIANCE_9_SAMPLES 1
#define VARIANCE_SAMPLER_ARRAY 1

#wolf_include "wolftk/varianceClipping.glsl"   // takram: #include "varianceClipping" (line 7)

uniform sampler2DArray inputBuffer;
uniform sampler2DArray historyBuffer;

uniform vec2 texelSize;
uniform float varianceGamma;
uniform float temporalAlpha;

in vec2 vUv;

layout(location = 0) out vec4 outputColor[CASCADE_COUNT];

// Source: three-geospatial packages/clouds/src/shaders/shadowResolve.frag:20-30 neighborOffsets
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

// Source: three-geospatial packages/clouds/src/shaders/shadowResolve.frag:32-49 getClosestFragment
vec4 getClosestFragment(const ivec3 coord) {
  vec4 result = vec4(1e7, 0.0, 0.0, 0.0);
  vec4 neighbor;
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[0]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[1]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[2]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[3]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[4]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[5]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[6]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[7]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
    neighbor = texelFetchOffset(
      inputBuffer,
      coord + ivec3(0, 0, CASCADE_COUNT),
      0,
      neighborOffsets[8]
    );
    if (neighbor.r < result.r) {
      result = neighbor;
    }
  
  return result;
}

// Source: three-geospatial packages/clouds/src/shaders/shadowResolve.frag:51-66 cascade
void cascade(const int cascadeIndex, out vec4 outputColor) {
  ivec3 coord = ivec3(gl_FragCoord.xy, cascadeIndex);
  vec4 current = texelFetch(inputBuffer, coord, 0);

  vec4 depthVelocity = getClosestFragment(coord);
  vec2 velocity = depthVelocity.gb * texelSize;
  vec2 prevUv = vUv - velocity;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    outputColor = current;
    return; // Rejection
  }

  vec4 history = texture(historyBuffer, vec3(prevUv, float(cascadeIndex)));
  vec4 clippedHistory = varianceClipping(inputBuffer, coord, current, history, varianceGamma);
  outputColor = mix(clippedHistory, current, temporalAlpha);
}

// Source: three-geospatial packages/clouds/src/shaders/shadowResolve.frag:68-76 main
void main() {
  
    #if 0 < CASCADE_COUNT
    cascade(0, outputColor[0]);
    #endif // 0 < CASCADE_COUNT
  
    #if 1 < CASCADE_COUNT
    cascade(1, outputColor[1]);
    #endif // 1 < CASCADE_COUNT
  
    #if 2 < CASCADE_COUNT
    cascade(2, outputColor[2]);
    #endif // 2 < CASCADE_COUNT
  
    #if 3 < CASCADE_COUNT
    cascade(3, outputColor[3]);
    #endif // 3 < CASCADE_COUNT
  
}
