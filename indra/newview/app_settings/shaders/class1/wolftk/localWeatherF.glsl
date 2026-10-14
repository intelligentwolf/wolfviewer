/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/localWeather.frag.
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

// Source: three-geospatial packages/clouds/src/shaders/localWeather.frag:12-27 getWorleyFbm
float getWorleyFbm(
  const vec3 point,
  float frequency,
  float amplitude,
  const float lacunarity,
  const float gain,
  const int octaveCount
) {
  float noise = 0.0;
  for (int i = 0; i < octaveCount; ++i) {
    noise += amplitude * (1.0 - getWorleyNoise(point, frequency));
    frequency *= lacunarity;
    amplitude *= gain;
  }
  return noise;
}

// Source: three-geospatial packages/clouds/src/shaders/localWeather.frag:29-83 main
void main() {
  vec3 point = vec3(vUv.x, vUv.y, 0.0);

  // Mid clouds
  {
    float worley = getWorleyFbm(
      point + vec3(0.5),
      8.0, // frequency
      0.4, // amplitude
      2.0, // lacunarity
      0.95, // gain
      4 // octaveCount
    );
    worley = smoothstep(1.0, 1.4, worley);
    outputColor.g = worley;
  }

  // Low clouds
  {
    float worley = getWorleyFbm(
      point,
      16.0, // frequency
      0.4, // amplitude
      2.0, // lacunarity
      0.95, // gain
      4 // octaveCount
    );
    worley = smoothstep(0.8, 1.4, worley);
    outputColor.r = saturate(worley - outputColor.g);
  }

  // High clouds
  {
    float perlin = getPerlinNoise(
      point,
      vec3(6.0, 12.0, 1.0), // frequency
      8 // octaveCount
    );
    perlin = smoothstep(-0.5, 0.5, perlin);
    outputColor.b = perlin;
  }

  // Extra
  {
    float perlin = getPerlinNoise(
      point + vec3(-19.1, 33.4, 47.2),
      32.0, // frequency
      4 // octaveCount
    );
    perlin = smoothstep(-0.5, 0.5, perlin);
    outputColor.a = perlin;
  }

  outputColor.a = 1.0;
}
