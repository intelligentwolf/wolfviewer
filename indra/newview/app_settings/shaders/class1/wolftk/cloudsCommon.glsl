/**
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * packages/clouds/src/shaders/clouds.glsl.
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

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:1-5 getSTBN
float getSTBN() {
  // [WOLF] NVIDIA STBN dropped (its licence: research / evaluation only, or the RTX SDK licence - not
  // for this commercial product). The blue-noise lookup is replaced by takram's own MIT temporal
  // interleaved gradient noise: Source: three-geospatial packages/core/src/webgpu/generators.ts:20-25
  // dithering (seed = screen coordinate + fract(time) * 1337) and :15-18 interleavedGradientNoise.
  return interleavedGradientNoise(gl_FragCoord.xy + fract(time) * 1337.0);
}

// Straightforward spherical mapping
// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:8-13 getSphericalUv
vec2 getSphericalUv(const vec3 position) {
  vec2 st = normalize(position.yx);
  float phi = atan(st.x, st.y);
  float theta = asin(normalize(position).z);
  return vec2(phi * RECIPROCAL_PI2 + 0.5, theta * RECIPROCAL_PI + 0.5);
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:15-42 getCubeSphereUv
vec2 getCubeSphereUv(const vec3 position) {
  // Cube-sphere relaxation by: http://mathproofs.blogspot.com/2005/07/mapping-cube-to-sphere.html
  // Source note: Tile and fix seams.
  // Possible improvements:
  // https://iquilezles.org/articles/texturerepetition/
  // https://gamedev.stackexchange.com/questions/184388/fragment-shader-map-dot-texture-repeatedly-over-the-sphere
  // https://github.com/mmikk/hextile-demo

  vec3 n = normalize(position);
  vec3 f = abs(n);
  vec3 c = n / max(f.x, max(f.y, f.z));
  vec2 m;
  if (all(greaterThan(f.yy, f.xz))) {
    m = c.y > 0.0 ? vec2(-n.x, n.z) : n.xz;
  } else if (all(greaterThan(f.xx, f.yz))) {
    m = c.x > 0.0 ? n.yz : vec2(-n.y, n.z);
  } else {
    m = c.z > 0.0 ? n.xy : vec2(n.x, -n.y);
  }

  vec2 m2 = m * m;
  float q = dot(m2.xy, vec2(-2.0, 2.0)) - 3.0;
  float q2 = q * q;
  vec2 uv;
  uv.x = sqrt(1.5 + m2.x - m2.y - 0.5 * sqrt(-24.0 * m2.x + q2)) * (m.x > 0.0 ? 1.0 : -1.0);
  uv.y = sqrt(6.0 / (3.0 - uv.x * uv.x)) * m.y;
  return uv * 0.5 + 0.5;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:44-46 getGlobeUv
vec2 getGlobeUv(const vec3 position) {
  return getCubeSphereUv(position);
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:48-55 getMipLevel
float getMipLevel(const vec2 uv) {
  const float mipLevelScale = 0.1;
  vec2 coord = uv * resolution;
  vec2 ddx = dFdx(coord);
  vec2 ddy = dFdy(coord);
  float deltaMaxSqr = max(dot(ddx, ddx), dot(ddy, ddy)) * mipLevelScale;
  return max(0.0, 0.5 * log2(max(1.0, deltaMaxSqr)));
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:57-61 insideLayerIntervals
bool insideLayerIntervals(const float height) {
  bvec3 gt = greaterThan(vec3(height), minIntervalHeights);
  bvec3 lt = lessThan(vec3(height), maxIntervalHeights);
  return any(bvec3(gt.x && lt.x, gt.y && lt.y, gt.z && lt.z));
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:63-66 WeatherSample
struct WeatherSample {
  vec4 heightFraction; // Normalized height of each layer
  vec4 density;
};

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:68-73 shapeAlteringFunction
vec4 shapeAlteringFunction(const vec4 heightFraction, const vec4 bias) {
  // Apply a semi-circle transform to round the clouds towards the top.
  vec4 biased = pow(heightFraction, bias);
  vec4 x = clamp(biased * 2.0 - 1.0, -1.0, 1.0);
  return 1.0 - x * x;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:75-103 sampleWeather
WeatherSample sampleWeather(const vec2 uv, const float height, const float mipLevel) {
  WeatherSample weather;
  weather.heightFraction = remapClamped(vec4(height), minLayerHeights, maxLayerHeights);

  // [WolfViewer] the texture turned by wolfWeatherRotation (the wind) about the tile's centre: the low layer's
  // cells line up along the wind (cloud streets); the identity leaves takram's lookup as it was
  // [WolfViewer round 13] about wolfWeatherPivot, the globe uv near the camera, not the cube face's centre: a turn of
  // the wind (the hourly forecast, the genus switching streets on / off, smoothed over 20 s) about a point thousands of
  // km away swept the clouds overhead past at hundreds of m/s (Paul 2026-10-13: "the clouds are rushing past")
  // [WolfViewer 2026-10-14] the stretch only for the layers that have that shape (wolfLayerStretched: cirrus, cloud
  // streets). One lookup served every layer, so a thin cirrus veil drew the cumulus below it 4x long (Paul: "all the
  // clouds ... seem to be long ... wherever i go"). Every other layer reads the texture through the turn R alone, always,
  // with its own offset (wolfWeatherOffsetTurned, integrated from R w: wolfenvironmentrender.cpp
  // local_weather_velocity_turned), so neither a change of the stretch nor a layer's switch (made only while that layer
  // shows nothing, wolfcloudgenus.cpp approach) moves a cloud. Each lookup runs only when some layer uses it.
  bvec4 stretched = greaterThan(wolfLayerStretched, vec4(0.5));
  vec4 rawWeather = vec4(0.0);
  if (any(stretched)) {
    vec2 wuv = (wolfWeatherRotation * (uv - wolfWeatherPivot)) + wolfWeatherPivot;
    rawWeather = textureLod(localWeatherTexture, wuv * localWeatherRepeat + localWeatherOffset, mipLevel).LOCAL_WEATHER_CHANNELS;
  }
  if (!all(stretched)) {
    vec2 tuv = (wolfWeatherTurn * (uv - wolfWeatherPivot)) + wolfWeatherPivot;
    vec4 turned = textureLod(localWeatherTexture, tuv * localWeatherRepeat + wolfWeatherOffsetTurned, mipLevel).LOCAL_WEATHER_CHANNELS;
    rawWeather = mix(turned, rawWeather, vec4(stretched));
  }
  vec4 localWeather = pow(rawWeather, weatherExponents);
  #ifdef SHADOW
  localWeather *= shadowLayerMask;
  #endif // SHADOW

  // [WolfViewer 2026-10-14] a departure from takram, whose layers are fixed bands (clouds.glsl sampleWeather: one
  // heightFraction per layer between its min and max), so every cumulus tower in a layer had the same top. For a convective
  // layer (wolfLayerTower > 0: how far the tops spread, 0 = takram's band) the top of each column follows the weather
  // texture's value there: top = base + depth x (1 - wolfLayerTower x (1 - weather)). That is Schneider's way in Horizon
  // Zero Dawn's Nubis ("The Real-time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015 Advances, pp. 34-42): the
  // weather map, not a fixed band, decides each column's height profile, so denser cells grow into taller towers.
  // Above its own top a column has no cloud: heightFraction is clamped to 1 there, where shapeAlteringFunction is 0.
  if (any(greaterThan(wolfLayerTower, vec4(0.0)))) {
    vec4 topShare = 1.0 - wolfLayerTower * (1.0 - clamp(localWeather, 0.0, 1.0));
    weather.heightFraction = remapClamped(
      vec4(height),
      minLayerHeights,
      minLayerHeights + (maxLayerHeights - minLayerHeights) * topShare
    );
  }

  vec4 heightScale = shapeAlteringFunction(weather.heightFraction, shapeAlteringBiases);

  // Modulation to control weather by coverage parameter.
  // Reference: https://github.com/Prograda/Skybolt/blob/master/Assets/Core/Shaders/Clouds.h#L63
  // [WolfViewer] per layer: wolfLayerCoverage (a channel < 0 takes the one coverage)
  vec4 layerCoverage = mix(vec4(coverage), wolfLayerCoverage, step(vec4(0.0), wolfLayerCoverage));
  vec4 factor = 1.0 - layerCoverage * heightScale;
  weather.density = remapClamped(
    mix(localWeather, vec4(1.0), coverageFilterWidths),
    factor,
    factor + coverageFilterWidths
  );

  return weather;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:105-110 getLayerDensity
vec4 getLayerDensity(const vec4 heightFraction) {
  // prettier-ignore
  return densityProfile.expTerms * exp(densityProfile.exponents * heightFraction) +
    densityProfile.linearTerms * heightFraction +
    densityProfile.constantTerms;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:112-117 MediaSample
struct MediaSample {
  float density;
  vec4 weight;
  float scattering;
  float extinction;
};

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:119-179 sampleMedia
MediaSample sampleMedia(
  const WeatherSample weather,
  const vec3 position,
  const vec2 uv,
  const float mipLevel,
  const float jitter,
  out ivec3 sampleCount
) {
  vec4 density = weather.density;

  // Source note: Define in physical length.
  vec3 surfaceNormal = normalize(position);
  // [WolfViewer round 17] takram drives the shape noise's vertical drift by the offset's length (x 2e4, clouds.glsl:
  // 120-121, "Source note: Define in physical length"), tuned on its repeat 100 and its demo wind (useCloudsControls.ts:
  // 705: 0.001 / s = 100 m/s of cloud motion -> 20 m/s of drift, a ratio 0.2). Round 13 put the offset in the texture's
  // own units (x repeat x the EEP cloud_scale x the streets' S R), so the drift grew with the repeat and the stretch, not
  // the wind: the viewer passes the drift itself, wolfCloudEvolution = 0.2 x the wind's path in metres (wolftkclouds.cpp).
  vec3 evolution = -surfaceNormal * wolfCloudEvolution;

  vec3 turbulence = vec3(0.0);
  #ifdef TURBULENCE
  vec2 turbulenceUv = uv * localWeatherRepeat * turbulenceRepeat;
  turbulence =
    turbulenceDisplacement *
    (texture(turbulenceTexture, turbulenceUv).rgb * 2.0 - 1.0) *
    dot(density, remapClamped(weather.heightFraction, vec4(0.3), vec4(0.0)));
  #endif // TURBULENCE

  vec3 shapePosition = (position + evolution + turbulence) * shapeRepeat + shapeOffset;
  float shape = texture(shapeTexture, shapePosition).r;
  density = remapClamped(density, vec4(1.0 - shape) * shapeAmounts, vec4(1.0));

  #ifdef DEBUG_SHOW_SAMPLE_COUNT
  ++sampleCount.y;
  #endif // DEBUG_SHOW_SAMPLE_COUNT

  #ifdef SHAPE_DETAIL
  if (mipLevel * 0.5 + (jitter - 0.5) * 0.5 < 0.5) {
    vec3 detailPosition = (position + turbulence) * shapeDetailRepeat + shapeDetailOffset;
    float detail = texture(shapeDetailTexture, detailPosition).r;
    // Fluffy at the top and whippy at the bottom.
    vec4 modifier = mix(
      vec4(pow(detail, 6.0)),
      vec4(1.0 - detail),
      remapClamped(weather.heightFraction, vec4(0.2), vec4(0.4))
    );
    modifier = mix(vec4(0.0), modifier, shapeDetailAmounts);
    density = remapClamped(density * 2.0, vec4(modifier * 0.5), vec4(1.0));

    #ifdef DEBUG_SHOW_SAMPLE_COUNT
    ++sampleCount.z;
    #endif // DEBUG_SHOW_SAMPLE_COUNT
  }
  #endif // SHAPE_DETAIL

  // Apply the density profiles.
  density = saturate(density * densityScales * getLayerDensity(weather.heightFraction));

  MediaSample media;
  float densitySum = density.x + density.y + density.z + density.w;
  media.weight = density / densitySum;
  media.scattering = densitySum * scatteringCoefficient;
  // [WolfViewer] the absorption per layer (precipitating layers absorb more, Schneider 2015 p. 69), on top of the
  // one absorptionCoefficient
  media.extinction = densitySum * absorptionCoefficient + dot(density, wolfLayerAbsorption) + media.scattering;
  return media;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:181-190 sampleMedia
MediaSample sampleMedia(
  const WeatherSample weather,
  const vec3 position,
  const vec2 uv,
  const float mipLevel,
  const float jitter
) {
  ivec3 sampleCount;
  return sampleMedia(weather, position, uv, mipLevel, jitter, sampleCount);
}
