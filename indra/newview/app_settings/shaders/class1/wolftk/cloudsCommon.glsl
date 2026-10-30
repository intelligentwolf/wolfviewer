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
  // [HURRICANE 2026-10-30] the storm here (sampleWeather / wolfStormApply): its share w (0 = none: sampleMedia does nothing more) and
  // the extra absorption per unit density of its dark base (README "The storm in 3D" item 2)
  float stormW;
  float stormAbsorb;
  // [HURRICANE 2026-10-30 ~13:10Z] the storm's rain curtain here: its extinction (1/m, 0 = none) and the layer it is counted in
  // (one-hot: the region's lowest layer, the one the storm hangs below - sampleWeather), for sampleMedia's per-layer weights
  float stormRain;
  float stormRainAlbedo;   // [2026-10-30 ~13:40Z] the share of stormRain that scatters (the rest absorbs: the storm's shade overhead)
  vec4 stormHost;
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
// [HURRICANE 2026-10-30] THE STORM IN 3D (README "The storm in 3D", item 3): the turn about the eye. A point at distance r turns by
// theta = h Omega(r) tau, Omega = K_VIS V(r) / max(r, Rm), V the Holland wind of README "Wind" (V = vmax sqrt(x e^(1 - x)),
// x = (Rm / max(r, 0.01))^1.31), K_VIS 0.3. A sample at p reads what was at p turned back by theta. tau is one of the flow map's
// two clocks (wolfStormFlow.x / .y, each t mod P, half a period apart; wolfenvironmentrender.cpp).
float wolfStormTheta(const float r, const float tau) {
  float Rm = 1.25 * wolfStormEye.w;
  float x = pow(Rm / max(r, 0.01), 1.31);
  float V = wolfStormSpin.w * sqrt(x * exp(1.0 - x));                   // wolfStormSpin.w = vmax
  return wolfStormSpin.x * 0.3 * V / max(r, Rm) * tau;                  // h K_VIS V / max(r, Rm) tau
}
vec2 wolfStormTurnXY(const vec2 xy, const float tau) {
  vec2 d = xy - wolfStormEye.xy;
  float a = -wolfStormTheta(length(d), tau);
  float ca = cos(a), sa = sin(a);
  return wolfStormEye.xy + vec2(ca * d.x - sa * d.y, sa * d.x + ca * d.y);
}
// an ECEF position turned back about the eye's vertical (in agent space, at the position's own agent height)
vec3 wolfStormTurnECEF(const vec3 position, const float tau) {
  vec3 pA = (ecefToWorldMatrix * vec4(position, 1.0)).xyz;
  pA.xy = wolfStormTurnXY(pA.xy, tau);
  return (worldToECEFMatrix * vec4(pA, 1.0)).xyz;
}

// [HURRICANE 2026-10-30] the storm's sample of the region's own field (README "Clouds" s_w): the spiral lookup q (and q_up for the
// cirrus lookup), each turned back by the flow map's clock tau (README "The storm in 3D" item 3)
vec4 wolfStormFieldAt(const vec2 c, const float r, const float phi, const float psi, const float D, const float R, const float h,
                      const float pz, const float mipLevel, const vec4 kindStreets, const vec4 kindUpper, const vec4 kindTurned,
                      const float tau) {
  vec4 sw = vec4(0.0);
  vec2 unusedCoord;
  if (any(greaterThan(kindStreets + kindTurned, vec4(0.5)))) {
    // rho = r + D; q = c(t) + rho (cos(phi - psi), sin(phi - psi))
    float rho = r + D;
    vec2 q = wolfStormTurnXY(c + rho * vec2(cos(phi - psi), sin(phi - psi)), tau);
    vec2 quv = getGlobeUv((worldToECEFMatrix * vec4(q, pz, 1.0)).xyz);
    sw += wolfReadWeather(quv, mipLevel, kindStreets, vec4(0.0), kindTurned, unusedCoord);
  }
  if (any(greaterThan(kindUpper, vec4(0.5)))) {
    // the outflow canopy (the cirrus lookup, kindUpper) turns the other way: the mirrored spiral, no inward streaming, turning
    // slowly backwards - q_up = c(t) + r (cos(phi + psi + h D / R), sin(phi + psi + h D / R))
    float a = phi + psi + h * D / R;
    vec2 q = wolfStormTurnXY(c + r * vec2(cos(a), sin(a)), tau);
    vec2 quv = getGlobeUv((worldToECEFMatrix * vec4(q, pz, 1.0)).xyz);
    sw += wolfReadWeather(quv, mipLevel, vec4(0.0), kindUpper, vec4(0.0), unusedCoord);
  }
  return pow(sw, weatherExponents);   // the weather value as the region's own (sampleWeather: pow(rawWeather, weatherExponents))
}

// [HURRICANE 2026-10-30] The storm as cloud, not geometry (Paul 11:30Z / 11:33Z, images 7 and 8: "still not really a cyclone" - a
// smooth pale lens with a crisp elliptical rim, a smooth vertical curtain, a flat underside). The masks used to multiply the
// DENSITY after the coverage cut - smooth analytic functions of r and B2, which the shape and detail noise could not erode, seen
// edge-on as curtains and rims. Now every part of the storm's shape goes in BEFORE the cut, where the cut against the region's
// own field and the shape / detail erosion in sampleMedia make the edges cloud-like (README "The storm in 3D"):
//   - each mask's radius / phase is moved by the shape noise at the sample (n, the region's own 3D noise at 4 x its repeat,
//     ~0.8 km cells): the eye and the eyewall by +-0.3 Re, the dense overcast by +-0.15 Rcdo, the storm's rim by +-0.1 R, the
//     bands' phase B2 by +-0.2 - so no edge is a circle or a straight line;
//   - every transition is wide: the eye 0.6 .. 1.4 Re (was 0.8 .. 1.2), the moats B2 0.15 .. 0.5, the rim 1 .. 1.3 R;
//   - the eye and the moats lower the weather VALUE and the storm's height envelope H(z) (item 1) - both before the cut -
//     instead of the density; the density keeps only a safety mask deep in the eye (rE < 0.3 .. 0.8 Re), where the cut has
//     already cleared it, so that edge is never seen;
//   - the dense overcast's value is O inner (0.6 + 0.4 n) (was 0.85 + 0.15 s_w): below 1, so its base and top come out of the
//     cut below 1 and the erosion carves them - a ragged base;
//   - the outer edge breaks into bands (item 4): the moat weight is O (1 - inner) max(w, s((r/R - 0.8) / 0.2)) out to 1.2 R,
//     faded out by 1.3 R.
// Outputs: the storm's share w (returned); H, the storm's height profile at this height (item 1, to mix into each layer's
// heightScale by w); densMask (the eye's safety mask); absorb, the extra absorption per unit density (item 2).
float wolfStormApply(inout vec4 localWeather, const vec3 position, const float height, const float mipLevel,
                     const vec4 kindStreets, const vec4 kindUpper, const vec4 kindTurned,
                     out float H, out float densMask, out float absorb, out float rainSigma,
                     out float rainAlbedo) {
  H = 0.0;
  rainAlbedo = 1.0;
  densMask = 1.0;
  absorb = 0.0;
  rainSigma = 0.0;
  vec3 pA = (ecefToWorldMatrix * vec4(position, 1.0)).xyz;
  vec2 c = wolfStormEye.xy;
  vec2 dp = pA.xy - c;
  float r = length(dp);
  float R = wolfStormEye.z;
  float Re = wolfStormEye.w;
  float Rcdo = 0.35 * R;    // README: Rcdo = 0.35 R
  // the shape noise at the sample, 0..1, at 4 x the shapes' own repeat (the same texture sampleMedia erodes with)
  float n = texture(shapeTexture, position * shapeRepeat * 4.0 + shapeOffset).r;
  float nr = n - 0.5;
  // README "Geometry": env(r) = 1 - s((r/R - 1) / 0.3); README "Clouds": w = C env(r) - at the noise-moved radius
  float rR = r + 0.2 * R * nr;
  float env = 1.0 - wolfStormS((rR / R - 1.0) / 0.3);
  float w = wolfStorm.w * env;
  if (w <= 0.0) return 0.0;
  float O = wolfStorm.y;
  float E = wolfStorm.z;
  float h = wolfStormSpin.x;
  float D = wolfStormSpin.y;
  float Rm = 1.25 * Re;     // README: Rm = 1.25 Re
  // phi = atan2(dy, dx) (anticlockwise from +X = east); atan2(0, 0) = 0 as C++ / PHP give it (GLSL's atan(0, 0) is undefined)
  float phi = r > 1e-3 ? atan(dp.y, dp.x) : 0.0;
  // psi(r) = h O cot(10 deg) ln(R / max(r, 0.5 Rm)); cot 10 deg = 5.671281819617709 (README 5.6713; wolfhurricane.h COT_ALPHA)
  float psi = h * O * 5.671281819617709 * log(R / max(r, 0.5 * Rm));
  float B2 = clamp(0.5 + 0.5 * cos(2.0 * (phi - psi)) + 0.4 * nr, 0.0, 1.0);   // two principal bands, phase moved by the noise
  float inner = 1.0 - wolfStormS((r + 0.3 * Rcdo * nr - Rcdo) / (0.5 * Rcdo));   // the dense overcast
  float rE = r + 0.6 * Re * nr;                                                  // the eye and eyewall radius, moved by the noise
  float e = (rE - 1.25 * Re) / (0.3 * Re);
  float ew = E * exp(-e * e);                                                    // the eyewall ring
  float meye = 1.0 - E * (1.0 - wolfStormS((rE - 0.6 * Re) / (0.8 * Re)));       // 0 in the eye, 1 beyond 1.4 Re
  // the moats (README "Clouds" Mmoat) and, from 0.8 R, the broken outer edge (README "The storm in 3D" item 4)
  float outer = wolfStormS((rR / R - 0.8) / 0.2) * (1.0 - wolfStormS((rR / R - 1.2) / 0.1));   // at the noise-moved radius
  float mmoat = 1.0 - O * (1.0 - inner) * max(w, outer) * (1.0 - wolfStormS((B2 - 0.15) / 0.35));
  // the eye's safety mask on the density: only deep inside, where the value and H below have already cleared it
  densMask = 1.0 - E * (1.0 - wolfStormS((rE - 0.3 * Re) / (0.5 * Re)));
  // THE FRAME BUDGET (wolfframegovernor.h; wolfStormSpin.z, eased over 3 s in wolfenvironmentrender.cpp): on a slow frame the
  // storm's own lookup of the field is skipped and s_w is the point's own weather value - the structure comes from the bands /
  // overcast / eyewall / masks, so it still reads. Between 0 and 1 (the ease) the two are mixed, so nothing pops.
  // THE FLOW MAP (README "The storm in 3D" item 3): two lookups half a period apart, crossfaded by the triangle weight
  // wolfStormFlow.z; at weight 1 (the frame budget below 1: one phase) only the first.
  vec4 own = localWeather;
  vec4 sw = own;
  float lookupW = clamp(wolfStormSpin.z, 0.0, 1.0);
  if (lookupW > 0.001) {
    float w0 = clamp(wolfStormFlow.z, 0.0, 1.0);
    vec4 s0 = wolfStormFieldAt(c, r, phi, psi, D, R, h, pA.z, mipLevel, kindStreets, kindUpper, kindTurned, wolfStormFlow.x);
    vec4 s1 = w0 < 0.999 ? wolfStormFieldAt(c, r, phi, psi, D, R, h, pA.z, mipLevel, kindStreets, kindUpper, kindTurned, wolfStormFlow.y)
                         : s0;
    sw = mix(own, mix(s1, s0, w0), lookupW);
  }
  // v_storm = max( clamp(s_w mix(0.35, 1, B2) (1 + 0.6 O), 0, 1), O inner (0.6 + 0.4 n), ew )   [inner term 2026-10-30]
  vec4 vStorm = max(max(clamp(sw * mix(0.35, 1.0, B2) * (1.0 + 0.6 * O), 0.0, 1.0), vec4(O * inner * (0.6 + 0.4 * n))), vec4(ew));
  localWeather = mix(localWeather, vStorm, w) * (meye * mmoat);
  // README "The storm in 3D" item 1: the storm's height profile. z_s(z) = s((z - Z_BASE) / 200) (1 - s((z - Z_zone) / 400)) per
  // zone, the zone weights' max: the eyewall (ew) and the dense overcast (inner) to Z_TOP, the bands (B2 (1 - inner)) to
  // Z_BAND = 0.55 Z_TOP; the eye and the moats clear. Z_TOP below.
  // [2026-10-30 ~13:10Z] README "The storm in 3D" item 5 (Paul: "its there but not reaching to the actual ocean"): the base comes
  // DOWN under the eyewall and the dense overcast - Z_LOCAL = mix(Z_BASE, Z_LOW, max(ew, inner)) + 120 nr, so 300 m under the bands,
  // 150 m under the eyewall and the overcast, ragged by +-60 m with the shape noise (Z_LOW 150 m, the 120 m: CONVENTIONS; a
  // hurricane's cloud base is a few hundred metres over the sea).
  float zBase = mix(wolfStormBase.x, wolfStormBase.z, max(ew, inner)) + 120.0 * nr;
  float zb = wolfStormS((height - zBase) / 200.0);
  // [2026-10-30 ~13:50Z] Z_TOP = min(the layers' highest top, 1.5 R) (CONVENTION). Paul 13:33Z, 3.25 km from the eye of a
  // R 1190 m storm, looking at it: "a uniform flat overcast". The region's layers then reached 9.5 km (Ci 7500-9500 m;
  // WolfViewer.log cloudsFrame), so the storm was a column 2.4 km wide and 9.5 km tall - from 2 km outside its rim its side
  // filled the sky to 78 deg up, a wall with no shape. A real hurricane is far wider than tall (tops ~15 km over hundreds of
  // km); at this scaled size 1.5 R keeps the eyewall and overcast a squat dome (top ~41 deg up from there) so the bands, the
  // dark base and the curtains under it read from outside, and the open sky above it shows its edge.
  float zTopS = min(maxHeight, 1.5 * R);
  float zTop = 1.0 - wolfStormS((height - zTopS) / 400.0);
  float zBand = 1.0 - wolfStormS((height - 0.55 * zTopS) / 400.0);
  H = zb * max(max(ew, inner) * zTop, B2 * (1.0 - inner) * zBand) * meye * mmoat;
  // ... and scud: ragged fragments from 60 m up to the base where the shape noise is high (n above 0.6, full by 0.75), under the
  // eyewall, the overcast and the bands (CONVENTIONS: 60 m, the 40 m / 100 m ramps, the 0.6 / 0.15)
  float scud = O * max(max(ew, inner), B2 * (1.0 - inner)) * wolfStormS((n - 0.6) / 0.15)
             * wolfStormS((height - 60.0) / 40.0) * (1.0 - wolfStormS((height - zBase) / 100.0));
  H = max(H, scud * meye * mmoat);
  // README "The storm in 3D" item 2: a dark base - absorption += w O K_BASE (1 - s((z - Z_BASE) / 1500)), K_BASE = wolfStormBase.y
  absorb = w * O * wolfStormBase.y * (1.0 - wolfStormS((height - zBase) / 1500.0));
  // README "The storm in 3D" item 6: THE RAIN CURTAINS, from the base down to the sea. The rain q of "Rain at a point" (its own eye
  // mask, radius and bands, no noise), as a rate R = q P_cat mm/h (wolfStormBase.w; hurricane_lib.php HU_RAIN_MMH); its
  // extinction from the visibility in rain V = 11.6 R^-0.63 km (Atlas's empirically adjusted relation for rain, threshold contrast
  // 0.055: core.ac.uk/download/230537853.pdf; V in km - 11.6 km at 1 mm/h) and Koschmieder's sigma = ln(1 / eps) / V with that same
  // threshold contrast, ln(1 / 0.055) = 2.900 (sampleMedia: rainAlbedo of it scatters, the rest absorbs). Shafts: the rain falls in
  // curtains - the shape noise read in x, y only (a column, the same at every height), cut at 0.35 .. 0.65 (CONVENTION; tile
  // 1200 m), so grey sheets hang under the eyewall and the bands with gaps between them. Water drops absorb almost
  // nothing in visible light; the shade of the storm overhead is put in as their albedo below (rainAlbedo).
  #ifndef SHADOW
  if (wolfStormBase.w > 0.0 && height < zBase + 100.0) {
    float meyeRain = 1.0 - E * (1.0 - wolfStormS((r - 0.8 * Re) / (0.4 * Re)));
    float er = (r - 1.25 * Re) / (0.4 * Re);
    float envR = 1.0 - wolfStormS((r / R - 1.0) / 0.3);
    float B2r = 0.5 + 0.5 * cos(2.0 * (phi - psi));
    float innerR = 1.0 - wolfStormS((r - Rcdo) / (0.5 * Rcdo));
    float q = O * meyeRain * max(max(exp(-er * er), 0.7 * innerR), (0.25 + 0.5 * B2r) * envR);
    float rate = q * wolfStormBase.w;
    if (rate > 0.01) {
      float col = texture(shapeTexture, vec3(pA.xy / 1200.0, 0.5)).r;
      float shaft = wolfStormS((col - 0.35) / 0.3);
      rainSigma = 2.900 / (11600.0 * pow(rate, -0.63)) * shaft * (1.0 - wolfStormS((height - zBase) / 100.0));
      // [2026-10-30 ~13:40Z] how much the curtain scatters: the sky light the march gives a sample (wolfSkyUnderCloud) is the
      // region's, which no longer sees the storm (the region is fair outside it), so a curtain under kilometres of storm cloud
      // was lit by the open sky and read as a white haze into the bright horizon. Its scattering is scaled by the storm's cover
      // overhead, cover = O max(ew, inner, B2 (1 - inner) env) - 1 - 0.8 cover (CONVENTION) - the rest of sigma is extinction,
      // so the curtain dims what is behind it as before and reads as a grey sheet.
      rainAlbedo = 1.0 - 0.8 * O * max(max(ew, innerR), B2r * (1.0 - innerR) * envR);
    }
  }
  #endif
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
  float stormDensMask = 1.0;
  float stormShare = 0.0;
  float stormH = 0.0;
  weather.stormAbsorb = 0.0;
  weather.stormRain = 0.0;
  weather.stormRainAlbedo = 1.0;
  weather.stormHost = vec4(0.0);
  if (wolfStorm.x > 0.5) {
    stormShare = wolfStormApply(localWeather, position, height, mipLevel, kindStreets, kindUpper, kindTurned,
                                stormH, stormDensMask, weather.stormAbsorb, weather.stormRain,
                                weather.stormRainAlbedo);
  }
  weather.stormW = stormShare;
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
  // [HURRICANE 2026-10-30] README "The storm in 3D" item 1: heightScale = mix(heightScale, H(z), w) - the storm's own vertical
  // profile in place of the layers' rounded slabs, in each layer only between its own base and top (where the genus leaves a gap
  // between layers the gap stays: declared). The eye and the moats are 0 in H, so they clear here, before the cut.
  // [2026-10-30 ~13:10Z] README "The storm in 3D" item 5: the region's LOWEST layer (one-hot, the first if two share the lowest
  // base; a layer of no height is not one) reaches down to the sea inside the storm, so the storm's low base, scud and rain
  // curtains hang below it; the shell starts at the sea while a storm runs (wolftkclouds.cpp sharedUniforms). Below its own
  // base the layer's heightFraction stays 0 (its density profile's bottom value).
  if (stormShare > 0.0 || weather.stormRain > 0.0) {
    vec4 present = step(minLayerHeights + 0.5, maxLayerHeights);
    vec4 base = mix(vec4(1e9), minLayerHeights, present);
    float lo = min(min(base.x, base.y), min(base.z, base.w));
    vec4 isLo = present * (1.0 - step(lo + 0.5, minLayerHeights));
    vec4 host = vec4(isLo.x, isLo.y * (1.0 - isLo.x), isLo.z * (1.0 - isLo.x) * (1.0 - isLo.y),
                     isLo.w * (1.0 - isLo.x) * (1.0 - isLo.y) * (1.0 - isLo.z));
    weather.stormHost = host;
    if (dot(host, vec4(1.0)) < 0.5) weather.stormRain = 0.0;   // no layer at all: nothing to hang it from
    if (stormShare > 0.0) {
      vec4 inside = step(mix(minLayerHeights, vec4(0.0), host), vec4(height)) * step(vec4(height), maxLayerHeights);
      heightScale = mix(heightScale, vec4(stormH) * inside, stormShare);
    }
  }

  // Modulation to control weather by coverage parameter.
  // Reference: https://github.com/Prograda/Skybolt/blob/master/Assets/Core/Shaders/Clouds.h#L63
  // [WolfViewer] per layer: wolfLayerCoverage (a channel < 0 takes the one coverage)
  vec4 layerCoverage = mix(vec4(coverage), wolfLayerCoverage, step(vec4(0.0), wolfLayerCoverage));
  // [HURRICANE 2026-10-30] README "Clouds": coverage (per layer) = mix(coverage, C_STORM, w O), C_STORM = 0.62 - inside the
  // storm the STORM owns the coverage. Paul 10-30 "i'm not seeing the cyclone at all": the storm's region weather (cloud 100) put
  // every genus layer at coverage ~1 (WolfViewer.log cloudsFrame 08:58:50Z: Ns 1, As 0.95, Cs 1, St 1), where the cut factor
  // 1 - coverage x heightScale reaches 0 and the remap below turns every value into density 1: the bands and moats were erased,
  // only the eye's mask was left. At 0.62 the cut falls inside v_storm's range, so moats thin or open and bands stay solid.
  // (Was mix(coverage, max(coverage, 0.75), w O), which can only raise it.)
  const float WOLF_STORM_COVERAGE = 0.62;   // README C_STORM (CONVENTION)
  if (stormShare > 0.0) layerCoverage = mix(layerCoverage, vec4(WOLF_STORM_COVERAGE), stormShare * wolfStorm.y);
  vec4 factor = 1.0 - layerCoverage * heightScale;
  weather.density = remapClamped(
    mix(localWeather, vec4(1.0), coverageFilterWidths),
    factor,
    factor + coverageFilterWidths
  );
  // [HURRICANE 2026-10-30] the eye's safety mask only (wolfStormApply: the eye and the moats now clear the value and H before the
  // cut). The build clearance below still runs after it (CLOUDS NEVER COVER BUILDS).
  if (stormShare > 0.0) weather.density *= stormDensMask;

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

  // [HURRICANE 2026-10-30] README "The storm in 3D" item 3: the shape and detail noise turn about the eye with the flow map
  // (wolfStormTurnECEF; two clocks crossfaded by wolfStormFlow.z), weighted by wolfStormFlow.w (the frame budget: 0 at or below
  // g 0.5, eased) and by the storm's share, so outside the storm nothing changes.
  bool stormTurn = weather.stormW > 0.0 && wolfStormFlow.w > 0.001;
  // the frame budget fades the turned noise against the unturned (a weight, not a smaller angle, so nothing swirls backwards)
  float turnW = stormTurn ? weather.stormW * clamp(wolfStormFlow.w, 0.0, 1.0) : 0.0;
  float flowW0 = clamp(wolfStormFlow.z, 0.0, 1.0);
  vec3 turned0 = position, turned1 = position;
  if (stormTurn) {
    turned0 = wolfStormTurnECEF(position, wolfStormFlow.x);
    turned1 = flowW0 < 0.999 ? wolfStormTurnECEF(position, wolfStormFlow.y) : turned0;
  }
  vec3 shapePosition = (position + evolution + turbulence) * shapeRepeat + shapeOffset;
  float shape = texture(shapeTexture, shapePosition).r;
  if (stormTurn) {
    float s0 = texture(shapeTexture, (turned0 + evolution + turbulence) * shapeRepeat + shapeOffset).r;
    float s1 = flowW0 < 0.999 ? texture(shapeTexture, (turned1 + evolution + turbulence) * shapeRepeat + shapeOffset).r : s0;
    shape = mix(shape, mix(s1, s0, flowW0), turnW);
  }
  density = remapClamped(density, vec4(1.0 - shape) * shapeAmounts, vec4(1.0));

  #ifdef DEBUG_SHOW_SAMPLE_COUNT
  ++sampleCount.y;
  #endif // DEBUG_SHOW_SAMPLE_COUNT

  #ifdef SHAPE_DETAIL
  if (mipLevel * 0.5 + (jitter - 0.5) * 0.5 < 0.5) {
    vec3 detailPosition = (position + turbulence) * shapeDetailRepeat + shapeDetailOffset;
    float detail = texture(shapeDetailTexture, detailPosition).r;
    if (stormTurn) {   // [HURRICANE 2026-10-30] the detail noise turns with the shapes (above)
      float d0 = texture(shapeDetailTexture, (turned0 + turbulence) * shapeDetailRepeat + shapeDetailOffset).r;
      float d1 = flowW0 < 0.999 ? texture(shapeDetailTexture, (turned1 + turbulence) * shapeDetailRepeat + shapeDetailOffset).r : d0;
      detail = mix(detail, mix(d1, d0, flowW0), turnW);
    }
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
  // [HURRICANE 2026-10-30 ~13:10Z] the rain curtain (wolfStormApply, README "The storm in 3D" item 6): extinction sigma (1/m) with
  // albedo stormRainAlbedo, counted in the host layer for the per-layer weights (its ice share, phase and powder) - a sample of rain alone has a
  // weight there instead of 0 / 0
  float rainSigma = weather.stormRain;
  media.weight = (density + rainSigma * weather.stormHost) / max(densitySum + rainSigma, 1e-12);
  media.scattering = densitySum * scatteringCoefficient + rainSigma * weather.stormRainAlbedo;
  // [WolfViewer] the absorption per layer (precipitating layers absorb more, Schneider 2015 p. 69), on top of the
  // one absorptionCoefficient
  media.extinction = densitySum * absorptionCoefficient + dot(density, wolfLayerAbsorption) + media.scattering;
  // [HURRICANE 2026-10-30] README "The storm in 3D" item 2: the storm's dark base, extra absorption per unit density in its lowest
  // 1500 m (wolfStormApply), added to every layer's
  media.extinction += densitySum * weather.stormAbsorb;
  media.extinction += rainSigma * (1.0 - weather.stormRainAlbedo);   // [2026-10-30 ~13:40Z] the rain's unscattered share
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
