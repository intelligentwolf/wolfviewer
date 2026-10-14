/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/turbulence.frag.
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

precision highp float;
precision highp int;

#wolf_include "wolftk/coreMath.glsl"   // takram: #include "core/math" (line 4)
#wolf_include "wolftk/perlin.glsl"   // takram: #include "perlin" (line 5)
#wolf_include "wolftk/tileableNoise.glsl"   // takram: #include "tileableNoise" (line 6)

in vec2 vUv;

layout(location = 0) out vec4 outputColor;

// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:12-12 frequency
const vec3 frequency = vec3(12.0);
// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:13-13 octaveCount
const int octaveCount = 3;

// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:15-17 perlin
float perlin(const vec3 point) {
  return getPerlinNoise(point, frequency, octaveCount);
}

// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:19-24 perlin3d
vec3 perlin3d(const vec3 point) {
  float perlin1 = perlin(point);
  float perlin2 = perlin(point.yzx + vec3(-19.1, 33.4, 47.2));
  float perlin3 = perlin(point.zxy + vec3(74.2, -124.5, 99.4));
  return vec3(perlin1, perlin2, perlin3);
}

// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:26-45 curl
vec3 curl(vec3 point) {
  const float delta = 0.1;
  vec3 dx = vec3(delta, 0.0, 0.0);
  vec3 dy = vec3(0.0, delta, 0.0);
  vec3 dz = vec3(0.0, 0.0, delta);

  vec3 px0 = perlin3d(point - dx);
  vec3 px1 = perlin3d(point + dx);
  vec3 py0 = perlin3d(point - dy);
  vec3 py1 = perlin3d(point + dy);
  vec3 pz0 = perlin3d(point - dz);
  vec3 pz1 = perlin3d(point + dz);

  float x = py1.z - py0.z - pz1.y + pz0.y;
  float y = pz1.x - pz0.x - px1.z + px0.z;
  float z = px1.y - px0.y - py1.x + py0.x;

  const float divisor = 1.0 / (2.0 * delta);
  return normalize(vec3(x, y, z) * divisor);
}

// Source: three-geospatial packages/clouds/src/shaders/turbulence.frag:47-51 main
void main() {
  vec3 point = vec3(vUv.x, vUv.y, 0.0);
  outputColor.rgb = 0.5 * curl(point) + 0.5;
  outputColor.a = 1.0;
}
