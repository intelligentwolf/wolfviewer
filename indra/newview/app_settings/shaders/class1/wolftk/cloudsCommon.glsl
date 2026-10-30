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
  // [WolfViewer 2026-10-26] the cirrus lookup's texture coordinate (kind 2 below), for the fibres along its flow
  // (sampleMedia wolfLayerFibre); (0, 0) when no layer uses that lookup
  vec2 upperCoord;
};

// [WolfViewer 2026-10-26] THE CURVED FLOW. Jimmy Olsen 10-26 (UiT webcam, Tromso 02:27): real cirrus/altostratus "swirling,
// curved, feathery"; ours "two huge, smooth, straight, parallel bands" - the stretched lookup was one global affine map (the
// wind's turn x a 4x stretch), so every streak in the sky was a straight line parallel to every other. Real high-cloud streaks
// follow the flow they are carried in, and that flow curves from place to place (WMO International Cloud Atlas, Cirrus
// fibratus: "nearly straight or irregularly curved filaments", https://cloudatlas.wmo.int/en/cirrus-fibratus.html; uncinus
// "comma-shaped ... hook", https://cloudatlas.wmo.int/en/cirrus-uncinus.html).
// The lookup point is displaced by a divergence-free 2D field, d = curl(psi), psi a sum of random Fourier modes - the
// kinematic-simulation flow of Kraichnan (1970, "Diffusion by a random velocity field", Phys. Fluids 13:22-31) and the curl
// construction of Bridson, Hourihan & Nordenstam (2007, "Curl-noise for procedural fluid flow", ACM TOG 26(3) 46: "the curl of
// a potential ... is automatically divergence-free"). Divergence-free means the warp neither bunches nor thins the cloud field
// (no area change to first order), so the coverage the forecast asked for is kept; warping a texture lookup by a field is
// Quilez's domain warping (https://iquilezles.org/articles/warp/), f(p + h(p)).
// Mode i: psi_i = A_i sin(2 pi n_i . (uv - drift) + phi_i + 2 pi m_i tau); d_i = curl psi_i = A_i 2 pi |n_i| cos(...) (n^_y, -n^_x).
// Written with the displacement amplitude D_i = beta_i / (2 pi |n_i|): the local turn of a streak by mode i is |grad d_i| =
// beta_i radians. The modes:
//   n_i   whole cycles per cube-face uv (one face = R pi / 2 = 9,990 km on the 6,360 km sphere, wolfenvironmentrender.cpp
//         uvPerMetre), so the field repeats exactly across a whole uv and the drift wraps into 0..1 with no jump.
//         |n| 208 -> 48 km, |n| 625 -> 16 km: the swirls a few tens of km across that span the sky under a layer 8 km up
//         (the sky to 20 deg elevation reaches 22 km out). Directions 60 deg apart, offset between the two octaves.
//   beta_i 0.22 rad (48 km) and 0.10 rad (16 km): the six add to 0.96 < 1, so the map uv -> uv + d stays one-to-one (no fold,
//         no mirrored clouds; the rms turn is 17 deg, peaks ~55 deg). Equal turn per octave is what a k^-5/3 kinetic-energy
//         spectrum gives for a frozen displacement field (velocity ~ k^-1/3 per octave, displacement ~ velocity / (u k) ...
//         ~ 1/k, turn = displacement x k = constant; Nastrom & Gage 1985, J. Atmos. Sci. 42:950-960, the mesoscale spectrum).
//   m_i   whole cycles per tau (wolfFlowState.x = time / 6 h, wrapped): the eddies evolve over 6, 6, 3 h (48 km) and 2, 1.5,
//         1.2 h (16 km), slow beside their advection (frozen turbulence, Taylor 1938, Proc. R. Soc. A 164:476-490); the
//         displacement changes at < 1.1 m/s, so nothing in the sky visibly slides.
// The beta, n and m are modelling numbers chosen as above (CLOUD_SHAPE_PLAN.md section 5.1), not measured values.
// The streets' field (flow 0) and the cirrus field (flow 1) are two realisations (phases differ), each carried by its own wind
// (wolfFlowDrift: xy streets, zw cirrus) and scaled by its bend (wolfFlowState.y streets, .z cirrus; 0 = no warp).
const int WOLF_FLOW_MODES = 6;
const vec2 WOLF_FLOW_N[6] = vec2[6](
  vec2(199.0, 61.0), vec2(47.0, 203.0), vec2(-152.0, 142.0),     // |n| ~208 at 17, 77, 137 deg
  vec2(426.0, 457.0), vec2(-183.0, 598.0), vec2(-609.0, 141.0)   // |n| ~625 at 47, 107, 167 deg
);
const float WOLF_FLOW_BETA[6] = float[6](0.22, 0.22, 0.22, 0.10, 0.10, 0.10);
const float WOLF_FLOW_M[6] = float[6](1.0, 1.0, 2.0, 3.0, 4.0, 5.0);
// the phases of the two realisations: flow 0 a golden-angle sequence, flow 1 the same + 2.0 rad (any fixed values serve:
// random-phase modes; fixed so a region looks the same on every visit, as wolfenvironmentrender.cpp's seed)
const float WOLF_FLOW_PHI[6] = float[6](0.0, 2.39996, 4.79993, 7.19989, 9.59985, 11.99982);

vec2 wolfFlowWarp(const vec2 uv, const vec2 drift, const float bend, const float phaseOffset) {
  vec2 d = vec2(0.0);
  for (int i = 0; i < WOLF_FLOW_MODES; ++i) {
    vec2 n = WOLF_FLOW_N[i];
    float len = length(n);
    // the phase in cycles first (fract keeps it exact: n . uv reaches ~600 cycles), then radians
    float cycles = fract(dot(n, uv - drift) + WOLF_FLOW_M[i] * wolfFlowState.x);
    float c = cos(6.283185307179586 * cycles + WOLF_FLOW_PHI[i] + phaseOffset);
    float D = WOLF_FLOW_BETA[i] / (6.283185307179586 * len);
    d += (D * c / len) * vec2(n.y, -n.x);
  }
  return d * bend;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:68-73 shapeAlteringFunction
vec4 shapeAlteringFunction(const vec4 heightFraction, const vec4 bias) {
  // Apply a semi-circle transform to round the clouds towards the top.
  vec4 biased = pow(heightFraction, bias);
  vec4 x = clamp(biased * 2.0 - 1.0, -1.0, 1.0);
  return 1.0 - x * x;
}

// [CLOUDS CLEAR OF BUILDS 2026-10-26] Paul's rule: clouds never cover anyone's builds. The clouds keep their weather heights;
// their density fades to 0 in and round builds (wolfcloudclearance.h: the field, the margins M0 / W and why, the invariant
// "inside any build's box clear = 1"). The CPU twin is WolfCloudClearance::clearAt (keep in step): the finest level that holds
// the point (two texels in from its edges, so a bilinear read never reaches the next level's rows), blended into the next over
// the last tenth of its half-window, the outermost faded out over its own last tenth.
// [2026-10-26] M0 and W come from the cloud table as the uniform wolfBuildFade (cloudsParameters.glsl): WolfCloudClearance::M0 / W
const float WOLF_BUILD_N = 256.0;   // WolfCloudClearance::N (texels per level edge; 3 levels = 768 rows)
float wolfBuildClearOf(const vec4 s, const float z) {
  float dvg = max(z - s.g, 0.0);
  float dvs = max(max(z - s.b, s.a - z), 0.0);
  float d = min(length(vec2(s.r, dvg)), length(vec2(s.r, dvs)));
  return 1.0 - smoothstep(wolfBuildFade.x, wolfBuildFade.x + wolfBuildFade.y, d);
}
bool wolfBuildLevelRead(const int k, const vec2 a, out vec4 s, out float edge) {
  vec2 tx = (a - wolfBuildLevel[k].xy) / wolfBuildLevel[k].z;
  edge = min(min(tx.x - 2.0, (WOLF_BUILD_N - 2.0) - tx.x), min(tx.y - 2.0, (WOLF_BUILD_N - 2.0) - tx.y));
  if (edge < 0.0) return false;
  // texel centres at (i + 0.5) t: u = tx / N; the level's rows k N .. (k + 1) N of 3 N
  s = textureLod(wolfBuildField, vec2(tx.x / WOLF_BUILD_N, (float(k) * WOLF_BUILD_N + tx.y) / (3.0 * WOLF_BUILD_N)), 0.0);
  return true;
}
float wolfBuildClear(const vec2 a, const float z) {
  for (int k = 0; k < 3; ++k) {
    vec4 s;
    float edge;
    if (!wolfBuildLevelRead(k, a, s, edge)) continue;
    float c = wolfBuildClearOf(s, z);
    float blend = 1.0 - smoothstep(0.0, WOLF_BUILD_N * 0.1, edge);
    if (blend > 0.0) {
      vec4 s2;
      float edge2;
      float next = (k + 1 < 3 && wolfBuildLevelRead(k + 1, a, s2, edge2)) ? wolfBuildClearOf(s2, z) : 0.0;
      c = mix(c, next, blend);
    }
    return c;
  }
  return 0.0;
}

// [HURRICANE 2026-10-29] The weather texture's three lookups of sampleWeather, unchanged, as a function of the globe uv: the
// storm reads the region's OWN field at the point its spiral carries there (wolfStormApply). Each lookup runs only for the
// layers in its mask; upperCoord is the cirrus lookup's coordinate ((0, 0) when no layer in the masks uses it).
vec4 wolfReadWeather(const vec2 uv, const float mipLevel, const vec4 kindStreets, const vec4 kindUpper, const vec4 kindTurned,
                     out vec2 upperCoord) {
  vec4 rawWeather = vec4(0.0);
  upperCoord = vec2(0.0);
  if (any(greaterThan(kindStreets, vec4(0.5)))) {
    vec2 fuv = uv;
    if (wolfFlowState.y > 0.0) fuv += wolfFlowWarp(uv, wolfFlowDrift.xy, wolfFlowState.y, 0.0);
    vec2 wuv = (wolfWeatherRotation * (fuv - wolfWeatherPivot)) + wolfWeatherPivot;
    rawWeather += kindStreets * textureLod(localWeatherTexture, wuv * localWeatherRepeat + localWeatherOffset, mipLevel).LOCAL_WEATHER_CHANNELS;
  }
  if (any(greaterThan(kindUpper, vec4(0.5)))) {
    vec2 fuv = uv;
    if (wolfFlowState.z > 0.0) fuv += wolfFlowWarp(uv, wolfFlowDrift.zw, wolfFlowState.z, 2.0);
    vec2 wuv = (wolfWeatherRotationUpper * (fuv - wolfWeatherPivot)) + wolfWeatherPivot;
    // [10-26 third pass] the cirrus' own horizontal scale (wolfUpperRepeat, wolfcloudgenus.h CIRRUS_TILE_M), not the EEP's
    // cloud_scale; the mip level moves by the ratio of the two repeats (the texel footprint scales with the repeat)
    upperCoord = wuv * wolfUpperRepeat + wolfWeatherOffsetUpper;
    float upperMip = max(mipLevel + log2(max(wolfUpperRepeat.x, 1e-3) / max(localWeatherRepeat.x, 1e-3)), 0.0);
    rawWeather += kindUpper * textureLod(localWeatherTexture, upperCoord, upperMip).LOCAL_WEATHER_CHANNELS;
  }
  if (any(greaterThan(kindTurned, vec4(0.5)))) {
    vec2 tuv = (wolfWeatherTurn * (uv - wolfWeatherPivot)) + wolfWeatherPivot;
    vec2 tcoord = tuv * localWeatherRepeat + wolfWeatherOffsetTurned;
    vec4 turned = textureLod(localWeatherTexture, tcoord, mipLevel).LOCAL_WEATHER_CHANNELS;
    // [WolfViewer 2026-10-26] A GENUS'S OWN CELL SIZE (wolfLayerRepeat, wolf_clouds.xml genera 'repeat', a whole number). The UiT
    // Tromso webcam (10-26 02:27 and 16:49 local): altocumulus in many soft cells of 1 - 3 deg, stratocumulus fragments 2 - 10 deg;
    // WMO: altocumulus elements "an apparent width of between one and five degrees", cirrocumulus under one degree,
    // stratocumulus over five. One repeat for every layer gave every genus the same cells. A layer with a repeat above 1 reads
    // the texture again at repeat x that - its offset scaled with it, so it drifts at the same ground speed and its wrap (a
    // whole unit) stays seamless - with the mip level raised by log2 of it (the texel footprint shrinks by it).
    for (int i = 0; i < 4; ++i) {
      float rs = wolfLayerRepeat[i];
      if (kindTurned[i] > 0.5 && rs > 1.001) {
        vec4 fine = textureLod(localWeatherTexture, tcoord * rs, mipLevel + log2(rs)).LOCAL_WEATHER_CHANNELS;
        turned[i] = fine[i];
      }
    }
    rawWeather += kindTurned * turned;
  }
  return rawWeather;
}

// [HURRICANE 2026-10-29] Ticket #367 (Jimmy Olsen: "Is it possible to hurricanes in W.E?"; Paul: "it will form properly from the
// clouds"). The storm is made of the region's OWN cloud field: gathered in along a logarithmic spiral, turned, banded, made into
// a dense overcast over the inner third with a defined eyewall, and cleared in the eye. The model is the contract every reader
// follows - Source: region-terrain-work/hurricane-2026-10-29/README.md "THE MODEL", sections "Geometry at a point p" and
// "Clouds"; the CPU twin is wolfhurricane.cpp (WolfHurricane::geometry; keep in step). The time functions O, E, C and D come from
// the CPU each frame (wolfStorm, wolfStormSpin; WolfHurricane::timeAt / streamed), the eye in agent x, y (wolfStormEye).
// The point and the lookup point q are in agent metres (the plane the region's metres are, shifted by its origin): the agent
// position is the ECEF position through ecefToWorldMatrix (as wolfBuildClear reads it), and q is taken back to the globe through
// worldToECEFMatrix at the sample's own agent height, then getGlobeUv - the exact lookup of that point, not a linearisation.
// Returns the storm's share w = C env(r) and writes the eye mask Meye; localWeather (the weather value per layer, after the
// weather exponent) becomes mix(value, v_storm, w).
float wolfStormS(const float x) { return smoothstep(0.0, 1.0, x); }   // README "Time" s(x): c = clamp(x, 0, 1); c c (3 - 2 c)
float wolfStormApply(inout vec4 localWeather, const vec3 position, const float mipLevel, const vec4 kindStreets,
                     const vec4 kindUpper, const vec4 kindTurned, out float meye) {
  meye = 1.0;
  vec3 pA = (ecefToWorldMatrix * vec4(position, 1.0)).xyz;
  vec2 c = wolfStormEye.xy;
  vec2 dp = pA.xy - c;
  float r = length(dp);
  float R = wolfStormEye.z;
  float Re = wolfStormEye.w;
  // README "Geometry": env(r) = 1 - s((r/R - 1) / 0.3), 1 inside R, 0 beyond 1.3 R; README "Clouds": w = C env(r)
  float env = 1.0 - wolfStormS((r / R - 1.0) / 0.3);
  float w = wolfStorm.w * env;
  if (w <= 0.0) return 0.0;
  float O = wolfStorm.y;
  float E = wolfStorm.z;
  float h = wolfStormSpin.x;
  float D = wolfStormSpin.y;
  float Rm = 1.25 * Re;     // README: Rm = 1.25 Re
  float Rcdo = 0.35 * R;    // README: Rcdo = 0.35 R
  // phi = atan2(dy, dx) (anticlockwise from +X = east); atan2(0, 0) = 0 as C++ / PHP give it (GLSL's atan(0, 0) is undefined)
  float phi = r > 1e-3 ? atan(dp.y, dp.x) : 0.0;
  // psi(r) = h O cot(10 deg) ln(R / max(r, 0.5 Rm)); cot 10 deg = 5.671281819617709 (README 5.6713; wolfhurricane.h COT_ALPHA)
  float psi = h * O * 5.671281819617709 * log(R / max(r, 0.5 * Rm));
  float B2 = 0.5 + 0.5 * cos(2.0 * (phi - psi));                       // two principal bands
  float inner = 1.0 - wolfStormS((r - Rcdo) / (0.5 * Rcdo));            // the dense overcast
  float e = (r - 1.25 * Re) / (0.3 * Re);
  float ew = E * exp(-e * e);                                           // the eyewall ring
  meye = 1.0 - E * (1.0 - wolfStormS((r - 0.8 * Re) / (0.4 * Re)));     // 0 in the eye (r < 0.8 Re), 1 beyond 1.2 Re
  // the storm's sample of the region's own field: the same texture, the same layer, read at q
  vec4 sw = vec4(0.0);
  vec2 unusedCoord;
  vec4 kindLow = kindStreets + kindTurned;
  if (any(greaterThan(kindLow, vec4(0.5)))) {
    // rho = r + D; q = c(t) + rho (cos(phi - psi), sin(phi - psi))
    float rho = r + D;
    vec2 q = c + rho * vec2(cos(phi - psi), sin(phi - psi));
    vec2 quv = getGlobeUv((worldToECEFMatrix * vec4(q, pA.z, 1.0)).xyz);
    sw += wolfReadWeather(quv, mipLevel, kindStreets, vec4(0.0), kindTurned, unusedCoord);
  }
  if (any(greaterThan(kindUpper, vec4(0.5)))) {
    // the outflow canopy (the cirrus lookup, kindUpper) turns the other way: the mirrored spiral, no inward streaming, turning
    // slowly backwards - q_up = c(t) + r (cos(phi + psi + h D / R), sin(phi + psi + h D / R))
    float a = phi + psi + h * D / R;
    vec2 q = c + r * vec2(cos(a), sin(a));
    vec2 quv = getGlobeUv((worldToECEFMatrix * vec4(q, pA.z, 1.0)).xyz);
    sw += wolfReadWeather(quv, mipLevel, vec4(0.0), kindUpper, vec4(0.0), unusedCoord);
  }
  sw = pow(sw, weatherExponents);   // the weather value as the region's own (sampleWeather: pow(rawWeather, weatherExponents))
  // v_storm = max( clamp(s_w mix(0.35, 1, B2) (1 + 0.6 O), 0, 1), O inner (0.85 + 0.15 s_w), ew )
  vec4 vStorm = max(max(clamp(sw * mix(0.35, 1.0, B2) * (1.0 + 0.6 * O), 0.0, 1.0), O * inner * (0.85 + 0.15 * sw)), vec4(ew));
  localWeather = mix(localWeather, vStorm, w);
  return w;
}

// Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:75-103 sampleWeather
// [CLOUDS CLEAR OF BUILDS 2026-10-26] + position (ECEF metres), for the build clearance at the end
WeatherSample sampleWeather(const vec2 uv, const float height, const float mipLevel, const vec3 position) {
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
  // [WolfViewer 2026-10-26] three lookups (wolfLayerStretched per layer): 0 turned only; 1 stretched along the 10 m wind (cloud
  // streets); 2 stretched along the upper wind (cirrus, wolfWeatherRotationUpper, its own drift wolfWeatherOffsetUpper). Both
  // stretched lookups are read at the point the curved flow carries there (wolfFlowWarp above), each with its own field.
  vec4 kindStreets = step(vec4(0.5), wolfLayerStretched) * (1.0 - step(vec4(1.5), wolfLayerStretched));
  vec4 kindUpper = step(vec4(1.5), wolfLayerStretched);
  vec4 kindTurned = 1.0 - step(vec4(0.5), wolfLayerStretched);
  // [HURRICANE 2026-10-29] the three lookups moved into wolfReadWeather (unchanged), so the storm can read the same field at
  // another point (wolfStormApply)
  vec4 rawWeather = wolfReadWeather(uv, mipLevel, kindStreets, kindUpper, kindTurned, weather.upperCoord);
  vec4 localWeather = pow(rawWeather, weatherExponents);
  // [HURRICANE 2026-10-29] ticket #367: the region's storm gathers, winds and clears this field, before the fibres and the
  // coverage cut (README "Clouds": "applied to the cloud field's weather value before the coverage cut, per layer")
  float stormMeye = 1.0;
  float stormShare = 0.0;
  if (wolfStorm.x > 0.5) {
    stormShare = wolfStormApply(localWeather, position, mipLevel, kindStreets, kindUpper, kindTurned, stormMeye);
  }
  // [WolfViewer 2026-10-26, fourth pass] THE FIBRES CARVE THE CIRRUS BEFORE THE COVERAGE CUT. Paul at ICELAND (14:35Z) and TROMSO
  // (15:35Z): the high cloud drawn as "a handful of wide, smooth ribbons that fan across the entire sky from a point on the
  // horizon, like spokes". The cause: the streak lookup's weather value is the high channel's Perlin fbm (localWeatherF.glsl
  // frequency 6 x 12 per tile, 8 octaves, roughness 0.5 - the first octave carries half the weight), so over the 120 km tile its
  // lobes are 20 x 10 km, 80 x 10 km after the 4x stretch: bands 5 km wide seen from under a layer 7.5 km up subtend 37 deg
  // at the zenith, and a few of them, parallel, converge in perspective to the horizon (WMO radiatus). The fibres (the Worley
  // detail noise read in the lookup's own coordinate: cells 1 km x 10 km down to 125 m x 1.25 km, wolfFibreScale) were a +-50 %
  // factor on the density AFTER the cut, so a band stayed one unbroken sheet. Now the fibre noise is added to the weather
  // value BEFORE the cut, +- wolfLayerFibre (Ci 0.8) of its 0..1 range, mean-preserving (c has its mean E[c] subtracted,
  // wolfFibreShape, so the layer's mean optical depth is kept): the cut then falls inside every band and carves it into many
  // filaments with gaps between them, 1 km-scale generating cells and 10 km fall streaks (Sassen et al., FIRE cirrus lidar,
  // NTRS 19910001165: "approximately 1.0 km cirrus uncinus cells, to organized MUC structures up to approximately 120 km
  // across"; "longitudinal bands (20 km in width) along the ambient wind", NTRS 19900018964) - the band itself stays as the
  // envelope, as a mesoscale uncinus complex is. Only the layers read through the streak lookup (kindUpper: Ci, Ci spissatus;
  // wolfcloudgenus.cpp GenusRow::streaks) are carved; a veil (Cs) and grains (Cc) read the turned lookup and keep their smooth
  // low-frequency variation. Faded out with the weather mip level (0.5 .. 2) as before: far away the filaments merge into the
  // band's mean, as unresolved filaments do.
  #ifdef SHAPE_DETAIL
  if (any(greaterThan(wolfLayerFibre * kindUpper, vec4(0.0))) && wolfFibreShape.y > 0.0) {
    float fibreFade = 1.0 - smoothstep(0.5, 2.0, mipLevel);
    if (fibreFade > 0.0) {
      vec3 fp = vec3(weather.upperCoord * wolfFibreScale.xy, height * wolfFibreScale.z);
      float c = clamp((textureLod(shapeDetailTexture, fp, 0.0).r - wolfFibreShape.x) / wolfFibreShape.y, -1.0, 1.0);
      localWeather = clamp(localWeather + (wolfLayerFibre * kindUpper) * (fibreFade * (c - wolfFibreShape.z)), 0.0, 1.0);
    }
  }
  #endif // SHAPE_DETAIL
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
  // [HURRICANE 2026-10-29] README "Clouds": coverage (per layer) = mix(coverage, max(coverage, 0.75), w O)
  if (stormShare > 0.0) layerCoverage = mix(layerCoverage, max(layerCoverage, vec4(0.75)), stormShare * wolfStorm.y);
  vec4 factor = 1.0 - layerCoverage * heightScale;
  weather.density = remapClamped(
    mix(localWeather, vec4(1.0), coverageFilterWidths),
    factor,
    factor + coverageFilterWidths
  );
  // [HURRICANE 2026-10-29] README "Clouds": density *= mix(1, Meye, w > 0 ? 1 : 0) - the eye is clear through every layer.
  // The build clearance below still runs after it (CLOUDS NEVER COVER BUILDS).
  if (stormShare > 0.0) weather.density *= stormMeye;

  // [CLOUDS CLEAR OF BUILDS 2026-10-26] the build clearance (wolfBuildClear above): only below the highest cleared height and
  // where some fading layer has cloud; the cloud's height above the sea sphere is its height above the ground there, as a
  // build's agent z less the sea is (wolfenvironmentrender.cpp placeOnPlanet), so the comparison is in agent z
  float buildZ = height + wolfBuildParams.y;
  if (wolfBuildParams.x > 0.5 && buildZ < wolfBuildParams.z && any(greaterThan(weather.density * wolfLayerBuildFade, vec4(0.0)))) {
    vec2 buildXY = (ecefToWorldMatrix * vec4(position, 1.0)).xy;
    weather.density *= 1.0 - wolfBuildClear(buildXY, buildZ) * wolfLayerBuildFade;
  }

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

  // [WolfViewer 2026-10-26, fourth pass] the fibres of an ice layer are carved into the weather value in sampleWeather now (before
  // the coverage cut), not applied as a factor here: a factor on a thin sheet left the sheet whole - the ribbons.
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
