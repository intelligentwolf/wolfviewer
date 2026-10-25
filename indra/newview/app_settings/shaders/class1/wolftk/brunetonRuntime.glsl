/**
 * @file wolftk/brunetonRuntime.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/bruneton/runtime.glsl (complete, lines 1-461)
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
 * The Bruneton files keep the source's own BSD-3 notices (Eric Bruneton 2017, INRIA 2008) below.
 */

#ifndef WOLF_TK_BRUNETONRUNTIME_GLSL
#define WOLF_TK_BRUNETONRUNTIME_GLSL

// Based on: https://github.com/ebruneton/precomputed_atmospheric_scattering/blob/master/atmosphere/functions.glsl

/**
 * Copyright (c) 2017 Eric Bruneton
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Precomputed Atmospheric Scattering
 * Copyright (c) 2008 INRIA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef COMBINED_SCATTERING_TEXTURES
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:61 GetExtrapolatedSingleMieScattering
vec3 GetExtrapolatedSingleMieScattering(
    const AtmosphereParameters atmosphere, const vec4 scattering) {
  // Algebraically this can never be negative, but rounding errors can produce
  // that effect for sufficiently short view rays.
  // @shotamatsuda: Avoid division by infinitesimal values.
  // See https://github.com/takram-design-engineering/three-geospatial/issues/47
  if (scattering.r < 1e-5) {
    return vec3(0.0);
  }
  return scattering.rgb * scattering.a / scattering.r *
	    (atmosphere.rayleigh_scattering.r / atmosphere.mie_scattering.r) *
	    (atmosphere.mie_scattering / atmosphere.rayleigh_scattering);
}
#endif // COMBINED_SCATTERING_TEXTURES

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:76 GetCombinedScattering
IrradianceSpectrum GetCombinedScattering(
    const AtmosphereParameters atmosphere,
    const ReducedScatteringTexture scattering_texture,
    const ReducedScatteringTexture single_mie_scattering_texture,
    const Length r, const Number mu, const Number mu_s, const Number nu,
    const bool ray_r_mu_intersects_ground,
    out IrradianceSpectrum single_mie_scattering) {
  vec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(
      atmosphere, r, mu, mu_s, nu, ray_r_mu_intersects_ground);
  Number tex_coord_x = uvwz.x * Number(SCATTERING_TEXTURE_NU_SIZE - 1);
  Number tex_x = floor(tex_coord_x);
  Number lerp = tex_coord_x - tex_x;
  vec3 uvw0 = vec3((tex_x + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
      uvwz.z, uvwz.w);
  vec3 uvw1 = vec3((tex_x + 1.0 + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
      uvwz.z, uvwz.w);
#ifdef COMBINED_SCATTERING_TEXTURES
  vec4 combined_scattering =
      texture(scattering_texture, uvw0) * (1.0 - lerp) +
      texture(scattering_texture, uvw1) * lerp;
  IrradianceSpectrum scattering = IrradianceSpectrum(combined_scattering);
  single_mie_scattering =
      GetExtrapolatedSingleMieScattering(atmosphere, combined_scattering);
#else // COMBINED_SCATTERING_TEXTURES
  IrradianceSpectrum scattering = IrradianceSpectrum(
      texture(scattering_texture, uvw0) * (1.0 - lerp) +
      texture(scattering_texture, uvw1) * lerp);
  single_mie_scattering = IrradianceSpectrum(
      texture(single_mie_scattering_texture, uvw0) * (1.0 - lerp) +
      texture(single_mie_scattering_texture, uvw1) * lerp);
#endif // COMBINED_SCATTERING_TEXTURES
  return scattering;
}

// @shotamatsuda: Added for reading higher-order scattering texture.
#ifdef HAS_HIGHER_ORDER_SCATTERING_TEXTURE
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:112 GetScattering
IrradianceSpectrum GetScattering(
    const AtmosphereParameters atmosphere,
    const ReducedScatteringTexture scattering_texture,
    const Length r, const Number mu, const Number mu_s, const Number nu,
    const bool ray_r_mu_intersects_ground) {
  vec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(
      atmosphere, r, mu, mu_s, nu, ray_r_mu_intersects_ground);
  Number tex_coord_x = uvwz.x * Number(SCATTERING_TEXTURE_NU_SIZE - 1);
  Number tex_x = floor(tex_coord_x);
  Number lerp = tex_coord_x - tex_x;
  vec3 uvw0 = vec3((tex_x + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
      uvwz.z, uvwz.w);
  vec3 uvw1 = vec3((tex_x + 1.0 + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
      uvwz.z, uvwz.w);
  IrradianceSpectrum scattering = IrradianceSpectrum(
      texture(scattering_texture, uvw0) * (1.0 - lerp) +
      texture(scattering_texture, uvw1) * lerp);
  return scattering;
}
#endif // HAS_HIGHER_ORDER_SCATTERING_TEXTURE

// [WolfViewer 2026-10-13, round 3/4] THE EEP DRIVES THE SKY (Paul: "AUDIT the standard EEP -> use it to DRIVE the physical
// sky -> verify it is correct"). wolfenvironmentrender.cpp renders the dome at 36 probe directions with the gains off
// (skyF.glsl uProbe), reads them back and compares each with the stock renderer's sky in the same direction
// (wolfeepreference.cpp skyLinear: [S] skyV.glsl / skyF.glsl / softenLightF.glsl transcribed); the gain is their ratio, per
// channel. Between the probes the physical sky's own shape is kept and the gains are interpolated bilinearly in (sin
// elevation, azimuth). wolfEepGainOn 0 (a program the uniforms never reach, the probe itself): untouched.
//
// [WolfViewer 2026-10-25] Jimmy Olsen (W79, Wolf Territories welcome, dusk / night, the moon up): a dark seam from the zenith
// to the horizon, a yellowish glow patch, and a hard white strip on the horizon over the sea. Three faults of the grid:
//  - it was keyed to the SUN and folded: six azimuths 3 .. 180 deg measured on ONE side of the sun only (sunAz + az), and
//    applied to both sides by |azimuth| (acos). The stock sky is symmetric about its LIGHT's vertical, and its light is the
//    moon whenever the sun is down and the moon up ([S] llsettingssky.cpp:1353-1368 getLightDirection; the glow
//    haze_glow = (1 - dot(n, lightnorm)) x glow.x ^ glow.z, [S] skyV.glsl, wolfeepreference.cpp skyHaze). With the moon on
//    the measured side, the stock moon glow measured there was painted, mirrored, onto the other side of the sun (a patch
//    with no moon in it); the gain's slope turned over at the sun's and the anti-sun's meridians (a crease from the zenith
//    to the horizon), and within 3 deg of the sun's meridian it was held flat (a strip).
//  - below the lowest row (2 deg) the 2 deg gain was held while the physical sky's in-scatter keeps rising to the horizon,
//    and from above the sea further to the true horizon below the level (the limb). At night that gain is large (skyF.glsl
//    domeGain: Miss Zenn's white sea), and it turned the last two degrees above the sea white.
// Now: azimuths from the stock sky's light (wolfEepGainRef, ECEF: the sun while it is up, else the moon while it is up,
// else the sun - wolfenvironmentrender.cpp eepLightWorld), signed, measured all the way round (-180 .. 180, eleven
// columns, dense on BOTH sides of the light), and a seventh row on the true horizon (0.1 deg above the limb from where the
// ray starts), so the gain is measured where it is used.
#define WOLF_EEP_GAIN_ROWS 7
#define WOLF_EEP_GAIN_COLS 11
#define WOLF_EEP_GAINS 77
// [WolfViewer 2026-10-25] the grids live in a uniform block, not the default block: three 77-entry arrays there cost 231 of
// the 256 vec4 a fragment shader is only guaranteed (GL 4.1 core, MAX_FRAGMENT_UNIFORM_COMPONENTS 1024) and pushed skyF / cloudsF
// past it. Bound to LLGLSLShader::UB_WOLF_EEP_GAINS at link (llglslshader.cpp, as ReflectionProbes - the Apple-safe way), filled
// by wolftkatmosphere.cpp (std140: one vec4 per entry, .rgb used).
layout (std140) uniform WolfEepGains
{
  vec4 wolfEepGain[WOLF_EEP_GAINS];        // the sky's gains [height row x WOLF_EEP_GAIN_COLS + azimuth column]
  vec4 wolfEepCloudGain[WOLF_EEP_GAINS];   // the clouds' (cloudsF.glsl), the same grid
  vec4 wolfEepProbeDir[WOLF_EEP_GAINS];    // the grid's directions while the sky is probed (skyF.glsl uProbe; world)
};
uniform float wolfEepGainOn;
uniform vec3 wolfEepGainRef;   // the azimuth the sky's gains were measured from (ECEF unit vector)
// The horizon row's sin elevation seen from p (Bruneton length units, as bottomRadius): 0.1 deg above the true horizon,
// the tangent to the sea sphere (dip = asin(sqrt((r - R)(r + R)) / r)). The same row as wolfenvironmentrender.cpp
// eepHorizonRowDeg; 0.1 deg keeps the probe ray off the tangent, where RayIntersectsGround and the ray-sphere test round
// differently (skyF.glsl:505).
Number wolfEepHorizonRowMu(const Position p, const Length bottomRadius)
{
  Length r = max(length(p), bottomRadius);
  Number dip = asin(clamp(sqrt(max((r - bottomRadius) * (r + bottomRadius), 0.0)) / r, 0.0, 1.0));
  return sin(radians(0.1) - dip);
}
// the 7 x 11 grid interpolation shared by the sky's and the clouds' gains (grid: WOLF_GRID_SKY or WOLF_GRID_CLOUDS).
// ea: x sin elevation, y signed azimuth from the light in degrees (-180 .. 180, counter-clockwise about the up), z the
// horizon row's sin elevation (wolfEepHorizonRowMu)
#define WOLF_GRID_SKY 0
#define WOLF_GRID_CLOUDS 1
vec3 wolfGainAt(const int grid, const int i) { return grid == WOLF_GRID_SKY ? wolfEepGain[i].rgb : wolfEepCloudGain[i].rgb; }
vec3 wolfGainGrid(const int grid, const vec3 ea)
{
  // rows: sin 90, 40, 20, 10, 5, 2 deg and the horizon (wolfenvironmentrender.cpp EEP_GAIN_ELEV_DEG + eepHorizonRowDeg)
  float E[WOLF_EEP_GAIN_ROWS] = float[WOLF_EEP_GAIN_ROWS](1.0, 0.6427876, 0.3420201, 0.1736482, 0.0871557, 0.0348995, 0.0);
  E[6] = min(ea.z, E[5] - 1e-4);
  // columns: wolfenvironmentrender.cpp EEP_GAIN_AZ_DEG; column 0 (-180) is also +180, the column after 90
  const float A[WOLF_EEP_GAIN_COLS] = float[WOLF_EEP_GAIN_COLS](-180.0, -90.0, -50.0, -25.0, -10.0, -3.0, 3.0, 10.0, 25.0, 50.0, 90.0);
  Number s = clamp(ea.x, E[6], 1.0);
  int i = 0;
  for (int k = 0; k < WOLF_EEP_GAIN_ROWS - 1; ++k) { if (s <= E[k] && s >= E[k + 1]) { i = k; break; } }
  Number fe = (s - E[i + 1]) / (E[i] - E[i + 1]);   // 1 at row i, 0 at row i + 1
  Number az = clamp(ea.y, -180.0, 180.0);
  int j = WOLF_EEP_GAIN_COLS - 1;   // 90 .. 180: the last column and, wrapped round, the first
  for (int k = 0; k < WOLF_EEP_GAIN_COLS - 1; ++k) { if (az >= A[k] && az <= A[k + 1]) { j = k; break; } }
  int j1 = j + 1 < WOLF_EEP_GAIN_COLS ? j + 1 : 0;
  Number a1 = j + 1 < WOLF_EEP_GAIN_COLS ? A[j + 1] : 180.0;
  Number fa = (az - A[j]) / (a1 - A[j]);            // 0 at column j, 1 at column j1
  vec3 top = mix(wolfGainAt(grid, i * WOLF_EEP_GAIN_COLS + j), wolfGainAt(grid, i * WOLF_EEP_GAIN_COLS + j1), fa);
  vec3 bottom = mix(wolfGainAt(grid, (i + 1) * WOLF_EEP_GAIN_COLS + j), wolfGainAt(grid, (i + 1) * WOLF_EEP_GAIN_COLS + j1), fa);
  return mix(bottom, top, fe);
}
// a view ray's height (sin elevation), its signed azimuth from ref's in degrees (counter-clockwise about the up), and the
// horizon row's height, seen from point p (ECEF, Bruneton length units: km)
vec3 wolfGainElevAz(const Position p, const Direction view_ray, const Direction ref)
{
  vec3 up = normalize(p);
  Number mu = dot(view_ray, up);
  vec3 vh = view_ray - up * mu;
  vec3 rh = ref - up * dot(ref, up);
  Number az = (dot(vh, vh) > 1e-12 && dot(rh, rh) > 1e-12) ? degrees(atan(dot(cross(rh, vh), up), dot(rh, vh))) : 0.0;
  return vec3(mu, az, wolfEepHorizonRowMu(p, ATMOSPHERE.bottom_radius));
}
// the gain for a view ray from a point (ECEF km; up = the point's radial direction), whether the gains are on or not
// (skyF.glsl applies it to the dome's whole colour: wolfEepDomeGain)
vec3 wolfEepGainDirCore(const Position p, const Direction view_ray)
{
  return wolfGainGrid(WOLF_GRID_SKY, wolfGainElevAz(p, view_ray, wolfEepGainRef));
}
vec3 wolfEepGainDir(const Position p, const Direction view_ray)
{
  return wolfEepGainOn > 0.5 ? wolfEepGainDirCore(p, view_ray) : vec3(1.0);
}

// [WolfViewer 2026-10-13] the EEP's haze_horizon over LL's default (llsettingssky.cpp:1229 0.19), as scale - 1: LL adds its
// sunlit haze as haze_horizon x haze_weight x (sunlight x haze_glow + ambient) (class1/deferred/skyF.glsl:144,
// atmosphericsFuncs.glsl:131), linear in haze_horizon; the aerosol (Mie) in-scatter is that light here
uniform float wolfEepMieScale;

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:133 GetSkyRadiance
RadianceSpectrum GetSkyRadiance(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const ReducedScatteringTexture scattering_texture,
    const ReducedScatteringTexture single_mie_scattering_texture,
    Position camera, const Direction view_ray, const Length shadow_length,
    const Direction sun_direction,
    out DimensionlessSpectrum transmittance) {
  // Compute the distance to the top atmosphere boundary along the view ray,
  // assuming the viewer is in space (or NaN if the view ray does not intersect
  // the atmosphere).
  Length r = length(camera);
  Length rmu = dot(camera, view_ray);
  vec3 eepGain = wolfEepGainDir(camera, view_ray);   // [WolfViewer] at the camera, before it moves
  // @shotamatsuda: Use SafeSqrt instead.
  // See: https://github.com/takram-design-engineering/three-geospatial/pull/26
  Length distance_to_top_atmosphere_boundary = -rmu -
      SafeSqrt(rmu * rmu - r * r +
          atmosphere.top_radius * atmosphere.top_radius);
  // If the viewer is in space and the view ray intersects the atmosphere, move
  // the viewer to the top atmosphere boundary (along the view ray):
  if (distance_to_top_atmosphere_boundary > 0.0 * m) {
    camera = camera + view_ray * distance_to_top_atmosphere_boundary;
    r = atmosphere.top_radius;
    rmu += distance_to_top_atmosphere_boundary;
  } else if (r > atmosphere.top_radius) {
    // If the view ray does not intersect the atmosphere, simply return 0.
    transmittance = DimensionlessSpectrum(1.0);
    return RadianceSpectrum(0.0 * watt_per_square_meter_per_sr_per_nm);
  }
  // Compute the r, mu, mu_s and nu parameters needed for the texture lookups.
  Number mu = rmu / r;
  Number mu_s = dot(camera, sun_direction) / r;
  Number nu = dot(view_ray, sun_direction);

  // @shotamatsuda: For rendering points below the bottom atmosphere.
  #ifdef GROUND
  bool ray_r_mu_intersects_ground = RayIntersectsGround(atmosphere, r, mu);
  #else // GROUND
  bool ray_r_mu_intersects_ground = false;
  #endif // GROUND

  transmittance = ray_r_mu_intersects_ground ? DimensionlessSpectrum(0.0) :
      GetTransmittanceToTopAtmosphereBoundary(
          atmosphere, transmittance_texture, r, mu);
  IrradianceSpectrum single_mie_scattering;
  IrradianceSpectrum scattering;
  if (shadow_length == 0.0 * m) {
    scattering = GetCombinedScattering(
        atmosphere, scattering_texture, single_mie_scattering_texture,
        r, mu, mu_s, nu, ray_r_mu_intersects_ground,
        single_mie_scattering);
  } else {
    // Case of light shafts (shadow_length is the total length noted l in our
    // paper): we omit the scattering between the camera and the point at
    // distance l, by implementing Eq. (18) of the paper (shadow_transmittance
    // is the T(x,x_s) term, scattering is the S|x_s=x+lv term).
    Length d = shadow_length;
    Length r_p =
        ClampRadius(atmosphere, sqrt(d * d + 2.0 * r * mu * d + r * r));
    Number mu_p = (r * mu + d) / r_p;
    Number mu_s_p = (r * mu_s + d * nu) / r_p;

    scattering = GetCombinedScattering(
        atmosphere, scattering_texture, single_mie_scattering_texture,
        r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground,
        single_mie_scattering);
    DimensionlessSpectrum shadow_transmittance =
        GetTransmittance(atmosphere, transmittance_texture,
            r, mu, shadow_length, ray_r_mu_intersects_ground);
    // @shotamatsuda: Occlude only single Rayleigh scattering by the shadow.
#ifdef HAS_HIGHER_ORDER_SCATTERING_TEXTURE
    IrradianceSpectrum higher_order_scattering = GetScattering(
        atmosphere, higher_order_scattering_texture,
        r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground);
    IrradianceSpectrum single_scattering = scattering - higher_order_scattering;
    scattering = single_scattering * shadow_transmittance + higher_order_scattering;
#else // HAS_HIGHER_ORDER_SCATTERING_TEXTURE
    scattering = scattering * shadow_transmittance;
#endif // HAS_HIGHER_ORDER_SCATTERING_TEXTURE
    single_mie_scattering = single_mie_scattering * shadow_transmittance;
  }
  return eepGain * (scattering * RayleighPhaseFunction(nu) + single_mie_scattering * (1.0 + wolfEepMieScale) *
      MiePhaseFunction(atmosphere.mie_phase_function_g, nu));
}

// @shotamatsuda: Returns the point on the ray closest to the origin.
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:219 ClosestPointOnRay
vec3 ClosestPointOnRay(const Position camera, const Position point) {
  Position ray = point - camera;
  Number t = clamp(-dot(camera, ray) / dot(ray, ray), 0.0, 1.0);
  return camera + t * ray;
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:225 RaySphereIntersections
vec2 RaySphereIntersections(
    const Position camera, const Direction direction, const Length radius) {
  float b = 2.0 * dot(direction, camera);
  float c = dot(camera, camera) - radius * radius;
  float discriminant = b * b - 4.0 * c;
  float Q = sqrt(discriminant);
  return vec2(-b - Q, -b + Q) * 0.5;
}

// @shotamatsuda: Clip the view ray at the bottom atmosphere boundary.
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:235 ClipAtBottomAtmosphere
bool ClipAtBottomAtmosphere(
    const AtmosphereParameters atmosphere,
    const Direction view_ray, inout Position camera, inout Position point) {
  const Length eps = 0.0;
  Length bottom_radius = atmosphere.bottom_radius + eps;
  Length r_camera = length(camera);
  Length r_point = length(point);
  bool camera_below = r_camera < bottom_radius;
  bool point_below = r_point < bottom_radius;

  vec2 t = RaySphereIntersections(camera, view_ray, bottom_radius);
  Position intersection = camera + view_ray * (camera_below ? t.y : t.x);
  camera = camera_below ? intersection : camera;
  point = point_below ? intersection : point;

  return camera_below && point_below;
}

// [WolfViewer 2026-10-13] the body of GetSkyRadianceToPoint below (runtime.glsl:253, from its first texture lookup on),
// unchanged, for one (r, mu): GetSkyRadianceToPoint evaluates it twice across the horizon
RadianceSpectrum wolfSkyRadianceToPointAt(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const ReducedScatteringTexture scattering_texture,
    const ReducedScatteringTexture single_mie_scattering_texture,
    const Length r, Number mu, const Number mu_s, const Number nu, Length d, const Length shadow_length,
    const bool ray_r_mu_intersects_ground, out DimensionlessSpectrum transmittance) {
  // @shotamatsuda: Hack to avoid rendering artifacts near the horizon, due to
  // finite atmosphere texture resolution and finite floating point precision.
  // See: https://github.com/ebruneton/precomputed_atmospheric_scattering/pull/32
  if (!ray_r_mu_intersects_ground) {
    Number mu_horizon = -SafeSqrt(1.0 -
        (atmosphere.bottom_radius * atmosphere.bottom_radius) / (r * r));
    const Number eps = 0.004;
    mu = max(mu, mu_horizon + eps);
  }

  transmittance = GetTransmittance(atmosphere, transmittance_texture,
      r, mu, d, ray_r_mu_intersects_ground);

  IrradianceSpectrum single_mie_scattering;
  IrradianceSpectrum scattering = GetCombinedScattering(
      atmosphere, scattering_texture, single_mie_scattering_texture,
      r, mu, mu_s, nu, ray_r_mu_intersects_ground,
      single_mie_scattering);

  // Compute the r, mu, mu_s and nu parameters for the second texture lookup.
  // If shadow_length is not 0 (case of light shafts), we want to ignore the
  // scattering along the last shadow_length meters of the view ray, which we
  // do by subtracting shadow_length from d (this way scattering_p is equal to
  // the S|x_s=x_0-lv term in Eq. (17) of our paper).
  d = max(d - shadow_length, 0.0 * m);
  Length r_p = ClampRadius(atmosphere, sqrt(d * d + 2.0 * r * mu * d + r * r));
  Number mu_p = (r * mu + d) / r_p;
  Number mu_s_p = (r * mu_s + d * nu) / r_p;

  IrradianceSpectrum single_mie_scattering_p;
  IrradianceSpectrum scattering_p = GetCombinedScattering(
      atmosphere, scattering_texture, single_mie_scattering_texture,
      r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground,
      single_mie_scattering_p);

  // Combine the lookup results to get the scattering between camera and point.
  DimensionlessSpectrum shadow_transmittance = transmittance;
  if (shadow_length > 0.0 * m) {
    // This is the T(x,x_s) term in Eq. (17) of our paper, for light shafts.
    shadow_transmittance = GetTransmittance(atmosphere, transmittance_texture,
        r, mu, d, ray_r_mu_intersects_ground);
  }
  // @shotamatsuda: Occlude only single Rayleigh scattering by the shadow.
#ifdef HAS_HIGHER_ORDER_SCATTERING_TEXTURE
  IrradianceSpectrum higher_order_scattering = GetScattering(
      atmosphere, higher_order_scattering_texture,
      r, mu, mu_s, nu, ray_r_mu_intersects_ground);
  IrradianceSpectrum single_scattering = scattering - higher_order_scattering;
  IrradianceSpectrum higher_order_scattering_p = GetScattering(
      atmosphere, higher_order_scattering_texture,
      r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground);
  IrradianceSpectrum single_scattering_p =
      scattering_p - higher_order_scattering_p;
  scattering =
      single_scattering - shadow_transmittance * single_scattering_p +
      higher_order_scattering - transmittance * higher_order_scattering_p;
#else // HAS_HIGHER_ORDER_SCATTERING_TEXTURE
  scattering = scattering - shadow_transmittance * scattering_p;
#endif // HAS_HIGHER_ORDER_SCATTERING_TEXTURE

  single_mie_scattering =
      single_mie_scattering - shadow_transmittance * single_mie_scattering_p;
#ifdef COMBINED_SCATTERING_TEXTURES
  single_mie_scattering = GetExtrapolatedSingleMieScattering(
      atmosphere, vec4(scattering, single_mie_scattering.r));
#endif // COMBINED_SCATTERING_TEXTURES

  // Hack to avoid rendering artifacts when the sun is below the horizon.
  single_mie_scattering = single_mie_scattering *
      smoothstep(Number(0.0), Number(0.01), mu_s);

  return scattering * RayleighPhaseFunction(nu) + single_mie_scattering * (1.0 + wolfEepMieScale) *
      MiePhaseFunction(atmosphere.mie_phase_function_g, nu);
}

// [WolfViewer 2026-10-13] GetSkyRadianceToPoint (runtime.glsl:253) given the view ray and the distance instead of the point.
// Paul: the haze band "in front of the mountains" at Iceland / Tromso / the fjord. The debug capture (16:32) shows it as
// concentric rings along the horizon row, added by the aerial perspective: point - camera, two ECEF positions 6,360 km from
// the centre in 32-bit floats (a step of ~0.5 m at that size), gave a view ray and a distance that wobble by up to a
// metre - and near the horizon the tables' mu axis turns that into rings and a seam. The callers now pass the ray and the
// distance from their own camera-relative vector (small, exact); the body is the source's, unchanged.
RadianceSpectrum wolfSkyRadianceToPointRel(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const ReducedScatteringTexture scattering_texture,
    const ReducedScatteringTexture single_mie_scattering_texture,
    Position camera, const Direction view_ray, const Length d_in, const Length shadow_length,
    const Direction sun_direction, out DimensionlessSpectrum transmittance) {
  Position point = camera + view_ray * d_in;
  // @shotamatsuda: Avoid artifacts when the ray does not intersect the top
  // atmosphere boundary.
  if (length(ClosestPointOnRay(camera, point)) > atmosphere.top_radius) {
    transmittance = vec3(1.0);
    return vec3(0.0);
  }

  vec3 eepGain = wolfEepGainDir(camera, view_ray);   // [WolfViewer] as GetSkyRadiance
  Position camera_in = camera;
  Position point_in = point;
  if (ClipAtBottomAtmosphere(atmosphere, view_ray, camera, point)) {
    transmittance = vec3(1.0);
    return vec3(0.0);
  }
  // the exact distance unless the clip moved an end (then the source's length(point - camera))
  bool clipped = any(notEqual(camera, camera_in)) || any(notEqual(point, point_in));
  Length d_exact = clipped ? length(point - camera) : d_in;

  // Compute the distance to the top atmosphere boundary along the view ray,
  // assuming the viewer is in space (or NaN if the view ray does not intersect
  // the atmosphere).
  Length r = length(camera);
  Length rmu = dot(camera, view_ray);
  // @shotamatsuda: Use SafeSqrt instead.
  // See: https://github.com/takram-design-engineering/three-geospatial/pull/26
  Length distance_to_top_atmosphere_boundary = -rmu -
      SafeSqrt(rmu * rmu - r * r +
          atmosphere.top_radius * atmosphere.top_radius);
  // If the viewer is in space and the view ray intersects the atmosphere, move
  // the viewer to the top atmosphere boundary (along the view ray):
  if (distance_to_top_atmosphere_boundary > 0.0 * m) {
    camera = camera + view_ray * distance_to_top_atmosphere_boundary;
    r = atmosphere.top_radius;
    rmu += distance_to_top_atmosphere_boundary;
    d_exact -= distance_to_top_atmosphere_boundary;   // the camera moved along the ray toward the point
  }

  // Compute the r, mu, mu_s and nu parameters for the first texture lookup.
  Number mu = rmu / r;
  Number mu_s = dot(camera, sun_direction) / r;
  Number nu = dot(view_ray, sun_direction);
  Length d = max(d_exact, 0.0 * m);
  bool ray_r_mu_intersects_ground = RayIntersectsGround(atmosphere, r, mu);

  // [WolfViewer 2026-10-13] the horizon seam (Paul, Iceland / Tromso / the fjord: "the haze should be behind the mountains
  // not in front of it"). The tables hold rays that miss the ground and rays that hit it in two halves of the mu axis;
  // S(x) - T S(p) is a small difference of two large lookups, and the two halves disagree just across the horizon - the
  // debug capture of 16:32 shows a 15 % step in the aerial perspective along the horizon row over the mountains, with the
  // scene under it smooth. PR #32 above clamps the upper side to mu_horizon + 0.004; the ground-hitting side has no such
  // guard. Within WOLF_HORIZON_BAND below the horizon the result is blended from the ground-hitting lookup (at the band's
  // foot) into the clamped upper one (at the horizon), so the two meet without a seam.
  const Number WOLF_HORIZON_BAND = 0.02;   // 1.1 deg of elevation
  Number mu_horizon = -SafeSqrt(1.0 -
      (atmosphere.bottom_radius * atmosphere.bottom_radius) / (r * r));
  RadianceSpectrum radiance;
  if (ray_r_mu_intersects_ground && mu > mu_horizon - WOLF_HORIZON_BAND) {
    DimensionlessSpectrum t_ground, t_sky;
    RadianceSpectrum l_ground = wolfSkyRadianceToPointAt(atmosphere, transmittance_texture, scattering_texture,
        single_mie_scattering_texture, r, mu, mu_s, nu, d, shadow_length, true, t_ground);
    RadianceSpectrum l_sky = wolfSkyRadianceToPointAt(atmosphere, transmittance_texture, scattering_texture,
        single_mie_scattering_texture, r, mu, mu_s, nu, d, shadow_length, false, t_sky);
    Number a = smoothstep(mu_horizon - WOLF_HORIZON_BAND, mu_horizon, mu);
    transmittance = mix(t_ground, t_sky, a);
    radiance = mix(l_ground, l_sky, a);
  } else {
    radiance = wolfSkyRadianceToPointAt(atmosphere, transmittance_texture, scattering_texture,
        single_mie_scattering_texture, r, mu, mu_s, nu, d, shadow_length, ray_r_mu_intersects_ground, transmittance);
  }
  return eepGain * radiance;
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:253 GetSkyRadianceToPoint (the body:
// wolfSkyRadianceToPointRel above)
RadianceSpectrum GetSkyRadianceToPoint(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const ReducedScatteringTexture scattering_texture,
    const ReducedScatteringTexture single_mie_scattering_texture,
    Position camera, Position point, const Length shadow_length,
    const Direction sun_direction, out DimensionlessSpectrum transmittance) {
  return wolfSkyRadianceToPointRel(atmosphere, transmittance_texture, scattering_texture, single_mie_scattering_texture,
      camera, normalize(point - camera), length(point - camera), shadow_length, sun_direction, transmittance);
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:373 GetSunAndSkyIrradiance
IrradianceSpectrum GetSunAndSkyIrradiance(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const IrradianceTexture irradiance_texture,
    const Position point, const Direction normal, const Direction sun_direction,
    out IrradianceSpectrum sky_irradiance) {
  Length r = length(point);
  Number mu_s = dot(point, sun_direction) / r;

  // Indirect irradiance (approximated if the surface is not horizontal).
  sky_irradiance = GetIrradiance(atmosphere, irradiance_texture, r, mu_s) *
      (1.0 + dot(normal, point) / r) * 0.5;

  // Direct irradiance.
  return atmosphere.solar_irradiance *
      GetTransmittanceToSun(
          atmosphere, transmittance_texture, r, mu_s) *
      max(dot(normal, sun_direction), 0.0);
}

// @shotamatsuda: Added for the clouds.
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:394 GetSunAndSkyScalarIrradiance
IrradianceSpectrum GetSunAndSkyScalarIrradiance(
    const AtmosphereParameters atmosphere,
    const TransmittanceTexture transmittance_texture,
    const IrradianceTexture irradiance_texture,
    const Position point, const Direction sun_direction,
    out IrradianceSpectrum sky_irradiance) {
  Length r = length(point);
  Number mu_s = dot(point, sun_direction) / r;

  // Indirect irradiance. [WolfViewer] the fluence of the sky hemisphere: GetIrradiance is the
  // horizontal (cosine-weighted) irradiance E = pi L of an isotropic sky (precompute.glsl:567-616
  // ComputeIndirectIrradiance), whose radiance integrated over its hemisphere is 2 pi L = 2 E. The
  // source's x 2 pi ("Integral over sphere yields 2 pi") took E for a radiance: pi times too much.
  sky_irradiance = GetIrradiance(atmosphere, irradiance_texture, r, mu_s) *
      2.0;

  // Direct irradiance. Omit the cosine term.
  return atmosphere.solar_irradiance *
      GetTransmittanceToSun(atmosphere, transmittance_texture, r, mu_s);
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:412 GetSolarLuminance
Luminance3 GetSolarLuminance() {
  return ATMOSPHERE.solar_irradiance /
      (PI * ATMOSPHERE.sun_angular_radius * ATMOSPHERE.sun_angular_radius) *
      SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:418 GetSkyLuminance
Luminance3 GetSkyLuminance(
    const Position camera, Direction view_ray, const Length shadow_length,
    const Direction sun_direction, out DimensionlessSpectrum transmittance) {
  return GetSkyRadiance(ATMOSPHERE, transmittance_texture,
      scattering_texture, single_mie_scattering_texture,
      camera, view_ray, shadow_length, sun_direction,
      transmittance) * SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// [WolfViewer 2026-10-25] the single-Mie (aerosol) term of GetSkyLuminance alone, no light shafts: GetSkyRadiance's
// single_mie_scattering x (1 + wolfEepMieScale) x MiePhaseFunction(g, nu) x the EEP gain, as that function computes it (the
// same steps as GetSkyRadiance above, its shadow_length == 0 branch). The Cornette-Shanks phase with g = 0.8 is the forward
// lobe - the aureole round its light (the moon's by night: skyF.glsl wolfSkyWithBodies).
Luminance3 wolfSkyMieLuminance(Position camera, const Direction view_ray, const Direction sun_direction)
{
  Length r = length(camera);
  Length rmu = dot(camera, view_ray);
  vec3 eepGain = wolfEepGainDir(camera, view_ray);
  Length distance_to_top_atmosphere_boundary = -rmu -
      SafeSqrt(rmu * rmu - r * r + ATMOSPHERE.top_radius * ATMOSPHERE.top_radius);
  if (distance_to_top_atmosphere_boundary > 0.0 * m) {
    camera = camera + view_ray * distance_to_top_atmosphere_boundary;
    r = ATMOSPHERE.top_radius;
    rmu += distance_to_top_atmosphere_boundary;
  } else if (r > ATMOSPHERE.top_radius) {
    return Luminance3(0.0);
  }
  Number mu = rmu / r;
  Number mu_s = dot(camera, sun_direction) / r;
  Number nu = dot(view_ray, sun_direction);
  #ifdef GROUND
  bool ray_r_mu_intersects_ground = RayIntersectsGround(ATMOSPHERE, r, mu);
  #else // GROUND
  bool ray_r_mu_intersects_ground = false;
  #endif // GROUND
  IrradianceSpectrum single_mie_scattering;
  GetCombinedScattering(ATMOSPHERE, scattering_texture, single_mie_scattering_texture,
      r, mu, mu_s, nu, ray_r_mu_intersects_ground, single_mie_scattering);
  return eepGain * single_mie_scattering * (1.0 + wolfEepMieScale) *
      MiePhaseFunction(ATMOSPHERE.mie_phase_function_g, nu) * SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:427 GetSkyLuminanceToPoint
Luminance3 GetSkyLuminanceToPoint(
    const Position camera, const Position point, const Length shadow_length,
    const Direction sun_direction, out DimensionlessSpectrum transmittance) {
  return GetSkyRadianceToPoint(ATMOSPHERE, transmittance_texture,
      scattering_texture, single_mie_scattering_texture,
      camera, point, shadow_length, sun_direction, transmittance) *
      SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// [WolfViewer 2026-10-13] GetSkyLuminanceToPoint from the camera-relative ray and distance (wolfSkyRadianceToPointRel)
Luminance3 wolfSkyLuminanceToPointRel(
    const Position camera, const Direction view_ray, const Length d, const Length shadow_length,
    const Direction sun_direction, out DimensionlessSpectrum transmittance) {
  return wolfSkyRadianceToPointRel(ATMOSPHERE, transmittance_texture,
      scattering_texture, single_mie_scattering_texture,
      camera, view_ray, d, shadow_length, sun_direction, transmittance) *
      SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:436 GetSunAndSkyIlluminance
Illuminance3 GetSunAndSkyIlluminance(
    const Position p, const Direction normal, const Direction sun_direction,
    out IrradianceSpectrum sky_irradiance) {
  IrradianceSpectrum sun_irradiance = GetSunAndSkyIrradiance(
      ATMOSPHERE, transmittance_texture, irradiance_texture, p, normal,
      sun_direction, sky_irradiance);
  sky_irradiance *= SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
  return sun_irradiance * SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

// @shotamatsuda: Added for the clouds.
// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:447 GetSunAndSkyScalarIlluminance
Illuminance3 GetSunAndSkyScalarIlluminance(
    const Position p, const Direction sun_direction,
    out IrradianceSpectrum sky_irradiance) {
  IrradianceSpectrum sun_irradiance = GetSunAndSkyScalarIrradiance(
      ATMOSPHERE, transmittance_texture, irradiance_texture, p,
      sun_direction, sky_irradiance);
  sky_irradiance *= SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;
  return sun_irradiance * SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
}

#define GetSolarRadiance GetSolarLuminance
#define GetSkyRadiance GetSkyLuminance
#define GetSkyRadianceToPoint GetSkyLuminanceToPoint
#define GetSunAndSkyIrradiance GetSunAndSkyIlluminance
#define GetSunAndSkyScalarIrradiance GetSunAndSkyScalarIlluminance
#endif // WOLF_TK_BRUNETONRUNTIME_GLSL
