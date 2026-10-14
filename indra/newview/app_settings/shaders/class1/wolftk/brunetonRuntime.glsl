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
// channel. The grid: six heights (90, 40, 20, 10, 5, 2 deg) x six azimuths from the sun's (3, 10, 25, 50, 90, 180 deg) -
// dense round the sun, where the stock sky's glow (glow.x / glow.z) and the physical sky's own differ most (round 4: the
// eight probes all 30 deg or more off the sun left that glow unmatched). Between the probes the physical sky's own shape
// is kept and the gains are interpolated bilinearly in (sin elevation, azimuth from the sun); below 2 deg - the land's
// haze, the air in front of the clouds - the 2 deg row. wolfEepGainOn 0 (a program the uniforms never reach, the probe
// itself): untouched.
uniform vec3 wolfEepGain[36];   // [height row x 6 + azimuth column]
uniform float wolfEepGainOn;
// the 6 x 6 grid interpolation shared by the sky's and the clouds' gains (cloudsF.glsl wolfEepCloudGain)
vec3 wolfGainGrid(const vec3 G[36], const Number mu, const Number azDeg)
{
  const float E[6] = float[6](1.0, 0.6427876, 0.3420201, 0.1736482, 0.0871557, 0.0348995);   // sin 90, 40, 20, 10, 5, 2
  const float A[6] = float[6](3.0, 10.0, 25.0, 50.0, 90.0, 180.0);
  Number s = clamp(mu, E[5], 1.0);
  int i = 0;
  for (int k = 0; k < 5; ++k) { if (s <= E[k] && s >= E[k + 1]) { i = k; break; } }
  Number fe = (s - E[i + 1]) / (E[i] - E[i + 1]);   // 1 at row i, 0 at row i + 1
  Number az = clamp(azDeg, A[0], A[5]);
  int j = 0;
  for (int k = 0; k < 5; ++k) { if (az >= A[k] && az <= A[k + 1]) { j = k; break; } }
  Number fa = (az - A[j]) / (A[j + 1] - A[j]);       // 0 at column j, 1 at column j + 1
  vec3 top = mix(G[i * 6 + j], G[i * 6 + j + 1], fa);
  vec3 bottom = mix(G[(i + 1) * 6 + j], G[(i + 1) * 6 + j + 1], fa);
  return mix(bottom, top, fe);
}
vec3 wolfEepGainCore(const Number mu, const Number azDeg) { return wolfGainGrid(wolfEepGain, mu, azDeg); }
// a view ray's height (sin elevation) and its azimuth from the sun's, in degrees, seen from point p (ECEF)
vec2 wolfElevAzFromSun(const Position p, const Direction view_ray, const Direction sun_direction)
{
  vec3 up = normalize(p);
  Number mu = dot(view_ray, up);
  vec3 vh = view_ray - up * mu;
  vec3 sh = sun_direction - up * dot(sun_direction, up);
  Number cosAz = (dot(vh, vh) > 1e-12 && dot(sh, sh) > 1e-12) ? dot(normalize(vh), normalize(sh)) : 1.0;
  return vec2(mu, degrees(acos(clamp(cosAz, -1.0, 1.0))));
}
// the gain for a view ray from a point (ECEF; up = the point's radial direction) with the sun along sun_direction, whether
// the gains are on or not (skyF.glsl applies it to the dome's whole colour: wolfEepDomeGain)
vec3 wolfEepGainDirCore(const Position p, const Direction view_ray, const Direction sun_direction)
{
  vec2 ea = wolfElevAzFromSun(p, view_ray, sun_direction);
  return wolfEepGainCore(ea.x, ea.y);
}
vec3 wolfEepGainDir(const Position p, const Direction view_ray, const Direction sun_direction)
{
  return wolfEepGainOn > 0.5 ? wolfEepGainDirCore(p, view_ray, sun_direction) : vec3(1.0);
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
  vec3 eepGain = wolfEepGainDir(camera, view_ray, sun_direction);   // [WolfViewer] at the camera, before it moves
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

  vec3 eepGain = wolfEepGainDir(camera, view_ray, sun_direction);   // [WolfViewer] as GetSkyRadiance
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
