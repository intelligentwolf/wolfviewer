/**
 * @file wolftk/skyF.glsl
 * @brief WolfViewer native sky dome: three-geospatial's sky (sky.frag / sky.glsl) in the LL G-buffer, with
 *        the Wolf Environment's own bodies, moon, nebula, aurora, night background and the sea to the horizon.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/sky.frag:1-117, packages/atmosphere/src/shaders/sky.glsl:26-81.
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
 *
 * Differences from the source, each a WolfViewer integration decision:
 *   - the view ray comes from LL's sky dome (wolfEnvSkyV.glsl, LLDrawPoolWLSky::renderDome) instead of a
 *     screen plane unprojected by sky.vert; world -> ECEF as sky.vert:48-50 (altitudeCorrection is 0: the
 *     planet is a sphere, getAltitudeCorrectionOffset.ts:7-21 gives 0 on it);
 *   - SUN is drawn as sky.glsl:48-59; takram's MOON (sky.glsl:61-76) is replaced by WolfViewer's moon (Paul's
 *     decision D5: the LRO-textured disc at the Krisciunas & Schaefer 1991 illuminance), drawn through the
 *     same transmittance;
 *   - the shadow length is LAST frame's resolved clouds shadow length (HAS_SHADOW_LENGTH, sky.frag:32-48),
 *     as takram's SkyMaterial receives it when the clouds effect runs after the scene;
 *   - the Wolf Environment's extra suns / moons / planets, nebula and the aurora (Paul's decision D12) and a
 *     natural night-sky background (D13) are added in the same frame units;
 *   - the sea out to the true horizon is takram's GROUND_ALBEDO branch (sky.frag:56-86) with a water surface
 *     in place of the Lambert albedo;
 *   - the output goes to LL's G-buffer (skyF.glsl layout, GBUFFER_FLAG_SKIP_ATMOS), in frame units: the
 *     luminance constants carry the frame's pre-exposure (WolfTk::Atmosphere::setRuntimeUniforms).
 */

#define RECIPROCAL_PI 0.3183098861837907

#wolf_include "wolftk/brunetonDefinitions.glsl"

// Source: sky.frag:10-18
uniform AtmosphereParameters ATMOSPHERE;
uniform vec3 SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
uniform vec3 SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;

uniform sampler2D transmittance_texture;
uniform sampler3D scattering_texture;
uniform sampler2D irradiance_texture;
uniform sampler3D single_mie_scattering_texture;
uniform sampler3D higher_order_scattering_texture;

#wolf_include "wolftk/brunetonCommon.glsl"
#wolf_include "wolftk/brunetonRuntime.glsl"
#wolf_include "wolftk/coreRaySphereIntersection.glsl"
#wolf_include "wolftk/seaGlint.glsl"   // [WolfViewer round 11] the sun's glint on the open sea (Cox-Munk)

// Source: sky.frag:23-24; sky.vert:6-9 (camera, frame)
uniform vec3 sunDirection;          // ECEF
uniform float cosSunAngularRadius;
uniform vec3 cameraPosition;        // world (LL agent frame, metres)
uniform mat4 worldToECEFMatrix;
uniform vec3 altitudeCorrection;

#ifdef HAS_SHADOW_LENGTH
uniform sampler2D shadowLengthBuffer;   // Source: sky.frag:32-34
uniform vec2 resolution;                // the viewport's size, for the screen uv (sky.frag reads it at vUv; the dome
uniform vec2 viewportOrigin;            // has no uv of its own: gl_FragCoord less the viewport's origin, / its size)
#endif

out vec4 frag_data[4];
// [WolfViewer 2026-10-14] the sky is drawn as the pipeline's screen triangle, not LL's dome mesh (wolfEnvSkyV.glsl):
// uScreenToWorldDir takes a pixel's clip position to its direction in the world (x east, y north, z up) - the inverse of
// the projection x the camera's rotation (wolfenvironmentrender.cpp renderSkyDome)
uniform mat4 uScreenToWorldDir;

// ── WolfViewer: the Wolf Environment's own bodies (renderSkyDome in wolfenvironmentrender.cpp) ──────────────
uniform sampler2D wolfMoonMap;      // NASA SVS CGI Moon Kit LRO colour (lldrawpoolwlsky.cpp's)
uniform vec3  uMoonDir;             // world, z up
uniform float uMoonSin;             // sine of the disc's angular radius
uniform vec3  uMoonLum;             // mean disc luminance above the atmosphere x pre-exposure (Krisciunas & Schaefer 1991)
uniform vec3  uMoonSunDir;          // the sun, as it lights the moon (world)
uniform vec3  uMoonNorth;           // lunar north, world
uniform float uMoonNorm;            // 1 / (the disc's mean Lommel-Seeliger factor x the map's mean albedo)
uniform float uMoonOn;
uniform int   uBodyCount;
uniform vec3  uBodyDir[12];         // to the body, world
uniform vec3  uBodyColor[12];       // luminance x pre-exposure (suns: disc; moons, planets: lit by uLitDir)
uniform float uBodySin[12];
uniform float uBodyType[12];        // 0 sun, 1 moon, 2 planet, 3 planet with rings
uniform vec3  uBodyRing[12];
uniform vec3  uLitDir;              // the first sun, world
uniform float uNebula;              // luminance x pre-exposure at full strength
uniform vec3  uNebulaColor;
uniform float uExposure;            // the frame's pre-exposure
uniform vec3  uMoonDirECEF;         // the moon, ECEF (for the air it lights)
// [WolfViewer 2026-10-13] the EEP's own sun (wolfenvironmentrender.cpp renderSkyDome): sun_scale - 1 (0 = the physical
// disc; an unset uniform reads 0) and, when the EEP names its own sun texture, that texture over the disc
uniform float uSunScaleD;
uniform sampler2D wolfSunMap;
uniform float uSunMapOn;
uniform float uDrawSunDisc;         // 0 in reflection-probe faces: LL lights with the sun itself (sun light + specular),
                                    // a disc in the probes would add it a second time to the ambient
uniform float uSeaProbe;            // 1: the sea mirrors LL's default reflection probe (the sky and the clouds as the
uniform samplerCubeArray reflectionProbes;   // world's water reflects them, llreflectionmapmanager.cpp probe 0)
uniform float uMoonSkyScale;        // the moon's illuminance above the atmosphere / the sun's (0: not drawn)
uniform float uNightSky;            // the natural night-sky background, cd/m2 x pre-exposure (0 = none)
uniform float wolf_aurora;
vec3 wolfAuroraRadiance(vec3 dir);  // wolfAuroraF.glsl: linear sRGB cd/m2 (east, north, up)
uniform float wolf_sea_height;      // camera metres above the sea; < 0 = no sea to the horizon
uniform vec3  waterFogColorLinear;  // the sea's body colour (reflectance-like)
uniform float fresnelScale;         // the EEP water's fresnel (LLSettingsWater fresnel_scale / fresnel_offset, as
uniform float fresnelOffset;        // LLDrawPoolWater gives waterF.glsl; renderSkyDome)
uniform float wolfSeaMss;            // [WolfViewer round 11] Cox-Munk mean square slope from the wind (seaGlint.glsl; renderSkyDome)

#define WOLF_PI 3.14159265358979

// The open sea's surface where the world's water planes end: what waterF.glsl makes of a flat far plane, so the
// planes' blend into this sea at the far clip (waterF.glsl: wolf_far_clip, wolfSeaBehind) meets the same colour.
// Source: waterF.glsl:282-299 calculateFresnelFactors - df3 = max(0, dot(viewVec, wave_i) * fresnelScale +
// fresnelOffset) for the three wave normals, squared; df2.x = their sum, df2.y = the same for the blended normal
// wavef; :689 radiance *= df2.y; :692 color = mix(fb, radiance, min(1, df2.x)); fb is wolfWaterBody (:643) over open
// water. Far off, the derivative maps' mips average to the plane (waterF.glsl:466) and every normal is the surface
// normal, so the three dots are one: f = max(0, cosView * scale + offset), df2.x = 3 f^2, df2.y = f.
// cosView = dot(viewVec, normal), the eye-space ray to the surface against its normal (:375): negative looking down.
// (The sun's glint is added by the caller: seaGlint.glsl, round 11.)
vec3 wolfFarSeaSurface(vec3 body, vec3 mirrored, float cosView)
{
    float f = max(0.0, cosView * fresnelScale + fresnelOffset);
    return mix(body, mirrored * f, min(1.0, 3.0 * f * f));
}

// The moon: a sphere lit by the sun, Lommel-Seeliger x the LRO albedo map, normalised so the disc's mean is the
// Krisciunas & Schaefer illuminance / its solid angle; lunar north where it really is. (wolfEnvSkyF.glsl
// moonDisc, kept by Paul's decision D5; its transmittance now takram's.)
vec4 wolfMoonDisc(vec3 dir, vec3 transmittance)
{
    if (uMoonOn < 0.5) return vec4(0.0);
    float c = dot(dir, uMoonDir);
    if (c <= 0.0) return vec4(0.0);
    vec3 o = dir - uMoonDir * c;
    float r = length(o) / max(uMoonSin, 1e-6);
    if (r >= 1.0) return vec4(0.0);
    vec3 n = normalize(o / max(uMoonSin, 1e-6) - uMoonDir * sqrt(max(0.0, 1.0 - r * r)));
    vec3 north = normalize(uMoonNorth - uMoonDir * dot(uMoonNorth, uMoonDir));
    vec3 east = cross(north, -uMoonDir);
    float lon = atan(dot(n, east), dot(n, -uMoonDir));
    float lat = asin(clamp(dot(n, north), -1.0, 1.0));
    vec3 albedo = texture(wolfMoonMap, vec2(0.5 + lon / (2.0 * WOLF_PI), 0.5 + lat / WOLF_PI)).rgb;
    float mu0 = max(dot(n, uMoonSunDir), 0.0);
    float mu = max(dot(n, -uMoonDir), 0.0);
    float ls = mu0 / (mu0 + mu + 1e-4);
    float edge = 1.0 - smoothstep(0.98, 1.0, r);
    return vec4(uMoonLum * albedo * ls * uMoonNorm * transmittance, edge);
}

// A value noise for the nebula (wolfEnvSkyF.glsl, unchanged)
float wolfHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float wolfNoise3(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(wolfHash13(i), wolfHash13(i + vec3(1, 0, 0)), f.x), mix(wolfHash13(i + vec3(0, 1, 0)), wolfHash13(i + vec3(1, 1, 0)), f.x), f.y);
    float b = mix(mix(wolfHash13(i + vec3(0, 0, 1)), wolfHash13(i + vec3(1, 0, 1)), f.x), mix(wolfHash13(i + vec3(0, 1, 1)), wolfHash13(i + vec3(1, 1, 1)), f.x), f.y);
    return mix(a, b, f.z);
}
vec3 wolfNebula(vec3 dir)
{
    vec3 axis = normalize(vec3(0.35, 0.5, 0.79));
    float x = dot(dir, axis) / 0.28;
    float band = exp(-x * x);
    vec3 p = dir * 3.0;
    float n = 0.5 * wolfNoise3(p) + 0.25 * wolfNoise3(p * 2.03) + 0.125 * wolfNoise3(p * 4.07) + 0.0625 * wolfNoise3(p * 8.11);
    float lanes = smoothstep(0.35, 0.75, n);
    return uNebulaColor * band * lanes * uNebula;
}

// The extra suns, moons and planets (wolfEnvSkyF.glsl bodies, unchanged except the transmittance: takram's)
vec3 wolfBodies(vec3 dir, vec3 transmittance, inout float cover)
{
    vec3 add = vec3(0.0);
    for (int i = 0; i < 12; i++)
    {
        if (i >= uBodyCount) break;
        vec3 bd = uBodyDir[i];
        float s = uBodySin[i];
        float c = dot(dir, bd);
        if (c <= 0.0) continue;
        vec3 o = dir - bd * c;
        float r = length(o) / max(s, 1e-6);
        float type = uBodyType[i];
        if (type < 0.5)
        {
            float disc = 1.0 - smoothstep(0.97, 1.0, r);
            float mu = sqrt(max(0.0, 1.0 - min(r, 1.0) * min(r, 1.0)));
            add += uBodyColor[i] * disc * pow(mu, 0.6) * 1.3 * transmittance;
            cover = max(cover, disc);
            continue;
        }
        vec3 tint = uBodyColor[i];
        if (r < 1.0)
        {
            vec3 n = normalize((o / max(s, 1e-6)) - bd * sqrt(max(0.0, 1.0 - r * r)));
            float lit = max(dot(n, uLitDir), 0.0);
            float edge = 1.0 - smoothstep(0.96, 1.0, r);
            add += tint * lit * edge * transmittance;
            cover = max(cover, edge);
        }
        if (type > 2.5)
        {
            vec3 east = normalize(cross(bd, vec3(0.0, 0.0, 1.0)) + vec3(1e-5, 0.0, 0.0));
            vec3 up = normalize(cross(east, bd));
            vec2 q = vec2(dot(o, east), dot(o, up)) / max(s, 1e-6);
            float er = length(vec2(q.x, q.y / 0.33));
            float ring = smoothstep(1.4, 1.5, er) * (1.0 - smoothstep(2.1, 2.2, er)) * (0.55 + 0.45 * sin(er * 22.0));
            bool behind = (q.y > 0.0) && (r < 1.0);
            if (!behind)
            {
                float lit = max(dot(up, uLitDir), 0.0);
                add += uBodyRing[i] * ring * lit * transmittance;
                cover = max(cover, ring * 0.8);
            }
        }
    }
    return add;
}

// Source: sky.glsl:26-81 getSkyRadiance, PERSPECTIVE_CAMERA and SUN; MOON replaced (decision D5) by the caller
vec3 wolfTkSkyRadiance(
  const vec3 cameraPositionKm,
  const vec3 rayDirection,
  const float shadowLength,
  const vec3 sunDirection,
  const float fragmentAngle,
  out vec3 transmittance
) {
  vec3 radiance = GetSkyRadiance(
    cameraPositionKm,
    rayDirection,
    shadowLength,
    sunDirection,
    transmittance
  );

  // Source: sky.glsl:48-59 (SUN)
  // [WolfViewer 2026-10-13] the disc at the EEP's sun_scale (LL draws its sun billboard sun_scale times larger,
  // llvosky.cpp setSunScale; 0.25..20, llsettingssky.cpp:788-789), its radiance the sun's: what the scene is lit by is
  // the sun itself (wolfenvironmentrender.cpp sunNormal), the disc only shows where it is
  float sunRadius = ATMOSPHERE.sun_angular_radius * (1.0 + uSunScaleD);
  float viewDotSun = dot(rayDirection, sunDirection);
  if (uDrawSunDisc > 0.5 && viewDotSun > cos(sunRadius)) {
    float angle = acos(clamp(viewDotSun, -1.0, 1.0));
    float antialias = smoothstep(
      sunRadius,
      sunRadius - fragmentAngle,
      angle
    );
    vec3 disc = vec3(1.0);
    if (uSunMapOn > 0.5) {
      // the EEP's sun texture across the disc, as LL lays it on its billboard (rgb x alpha: a transparent texture hides
      // the sun, as it does in LL); square to the disc, its up toward the zenith (ECEF: the radial up at the camera)
      vec3 zenith = normalize(cameraPositionKm);
      vec3 up0 = abs(dot(zenith, sunDirection)) < 0.999 ? zenith : normalize(cross(zenith, vec3(0.0, 0.0, 1.0)) + vec3(1e-5, 0.0, 0.0));
      vec3 east = normalize(cross(up0, sunDirection));
      vec3 up = cross(sunDirection, east);
      vec3 o = rayDirection - sunDirection * viewDotSun;
      vec2 q = vec2(dot(o, east), dot(o, up)) / max(sin(sunRadius), 1e-6);
      vec4 t = texture(wolfSunMap, q * 0.5 + 0.5);
      disc = t.rgb * t.a;
    }
    radiance += transmittance * GetSolarRadiance() * disc * antialias;
  }
  return radiance;
}

// The sky in one direction with the Wolf Environment's additions (bodies, nebula, the natural night sky, the
// aurora, the moon), all seen through the same transmittance: for the view ray and for the sea's mirrored ray.
// `originKm` ECEF km, `rayDirection` ECEF, `dirWorld` the same direction in the world (east, north, up).
vec3 wolfSkyWithBodies(vec3 originKm, vec3 rayDirection, vec3 dirWorld, float shadowLength, float fragmentAngle)
{
    vec3 transmittance;
    vec3 color = wolfTkSkyRadiance(originKm, rayDirection, shadowLength, sunDirection, fragmentAngle, transmittance);
    // [WolfViewer] the moonlit air: scattering is linear in its light source, and Bruneton's tables hold the sky
    // for the sun's irradiance (ATMOSPHERE.solar_irradiance, brunetonDefinitions.glsl), so the moon's sky is the
    // same tables looked up in the moon's direction, scaled by the moon's illuminance over the sun's above the
    // atmosphere (the moon reflects sunlight: the same spectrum, to first order). Set to 0 while the sun is up
    // (wolfenvironmentrender.cpp: its share is ~1e-6 there).
    if (uMoonSkyScale > 0.0)
    {
        vec3 trMoon;
        color += GetSkyRadiance(originKm, rayDirection, 0.0, uMoonDirECEF, trMoon) * uMoonSkyScale;
    }
    float cover = 0.0;
    color += wolfBodies(dirWorld, transmittance, cover);
    if (uNebula > 0.0) color += wolfNebula(dirWorld) * (1.0 - cover) * transmittance;
    // Decision D13: the natural night-sky background, NPS Night Skies "22.0 mag/arcsec2 or 172 ucd/m2 ...
    // the darkest part of pristine skies" (zenith luminance), as measured from the ground - so NOT dimmed by
    // the transmittance again; it is a ground observation, applied to the sky above the horizon
    // (transmittance > 0) only.
    if (uNightSky > 0.0 && any(greaterThan(transmittance, vec3(0.0)))) color += vec3(uNightSky) * (1.0 - cover);
    if (wolf_aurora > 0.0) color += wolfAuroraRadiance(dirWorld) * uExposure * transmittance;
    vec4 moon = wolfMoonDisc(dirWorld, transmittance);
    if (moon.a > 0.0)
    {
        // the moon is opaque: what lies behind it is hidden; the air in front of it still glows (the
        // scattering without the celestial terms)
        vec3 tr2;
        vec3 air = GetSkyRadiance(originKm, rayDirection, shadowLength, sunDirection, tr2);
        color = mix(color, air + moon.rgb, moon.a);
    }
    return color;
}

// [WolfViewer 2026-10-13] probe mode (wolfenvironmentrender.cpp probeSky): pixel i of the probe target shows the sky toward
// wolfEepProbeDir[i] (world: east, north, up; brunetonRuntime.glsl's WolfEepGains block since 2026-10-25), with no light
// shafts, written to every target the program writes
uniform float uProbe;
// [WolfViewer 2026-10-13] the EEP gains on the dome's WHOLE colour (the air, the sun, the moon, the bodies, the night sky,
// the aurora), not only the air's in-scatter: the verification table showed the zenith 60 % off its target with the gain
// on the air alone - another term overhead was outside it. The C++ turns the runtime gain off for this program
// (wolfEepGainOn 0) and this on, so the measured dome is exactly what is corrected. [2026-10-20] On the sea it is the air's
// in-scatter only, as on the world's water (main: domeGain).
uniform float wolfEepDomeGain;

// [WolfViewer 2026-10-13, round 16] Paul: "the moon looks like a ping pong ball also doesn't tie into the eep" / "jimmys eep i
// cant see the moon". While the EEP drives the sky the moon is LL's moon pass, as WolfViewer draws it (lldrawpoolwlsky.cpp
// renderHeavenlyBodies + class1/deferred/moonF.glsl): its quad (llvosky.cpp updateHeavenlyBodyGeometry: u along the quad's
// right, v along its up, half-size 0.1 x 0.45 x moon_scale x the low-moon enlargement, as a tangent in the direction), the
// real moon on Jimmy Olsen's real sky (the LRO sphere lit with the real phase, earthshine at night) or else the EEP's own
// moon texture x moon_brightness, the pre-EEP horizon fade - blended into the sky as LL's G-buffer blends it (BT_ALPHA) and
// read back as that buffer's sky value is (srgb_to_linear x sky_hdr_scale x 1.1 in classic: wolfeepreference.cpp
// cloudGbufferLinear). wolfMoonMap holds the map (the LRO map, or the EEP's moon texture).
uniform float uEepMoonOn;
uniform float uEepMoonReal;      // 1: Jimmy Olsen's real moon (moonF.glsl wolf_moon_phase.w)
uniform vec3  uEepMoonTo;        // the quad's centre direction (world)
uniform vec3  uEepMoonRight;
uniform vec3  uEepMoonUp;
uniform vec2  uEepMoonHalf;      // the quad's half-size over its distance, along right and up
uniform vec3  uEepMoonPhase;     // moonF.glsl wolf_moon_phase.xyz
uniform vec3  uEepMoonNorth;     // moonF.glsl wolf_moon_north.xyz (xy lunar north on the disc, z night)
uniform float uEepMoonFade;      // moonF.glsl fade
uniform float uEepMoonBright;    // moon_brightness
uniform float uEepSkyScale;      // sky_hdr_scale x (1.1 classic)
uniform float uEepGbufClamp;     // 1: the G-buffer colour target is RGBA8 (no emissive buffer) and clamps to 0..1

// Source: class1/environment/srgbF.glsl srgb_to_linear / linear_to_srgb (the clamp only where the target clamps)
vec3 wolfSrgbToLin(vec3 cs)
{
    return mix(pow((cs + vec3(0.055)) / vec3(1.055), vec3(2.4)), cs / vec3(12.92), vec3(lessThanEqual(cs, vec3(0.04045))));
}
vec3 wolfLinToSrgb(vec3 cl)
{
    cl = max(cl, vec3(0.0));
    if (uEepGbufClamp > 0.5) cl = min(cl, vec3(1.0));
    return mix(1.055 * pow(cl, vec3(0.41666)) - 0.055, cl * 12.92, vec3(lessThan(cl, vec3(0.0031308))));
}

vec3 wolfEepMoon(vec3 dirWorld, vec3 color)
{
    if (uEepMoonOn < 0.5) return color;
    float c = dot(dirWorld, uEepMoonTo);
    if (c <= 0.0) return color;
    vec2 p = vec2(dot(dirWorld, uEepMoonRight), dot(dirWorld, uEepMoonUp)) / (c * uEepMoonHalf);   // -1..1 across the quad
    if (abs(p.x) > 1.0 || abs(p.y) > 1.0) return color;
    vec3 g;
    float a;
    if (uEepMoonReal > 0.5)
    {
        // Source: moonF.glsl:52-87 (the real moon)
        float r = length(p);
        if (r > 1.0) return color;
        vec3 n = vec3(p, sqrt(max(1.0 - r * r, 0.0)));
        vec3 north = vec3(normalize(uEepMoonNorth.xy), 0.0);
        vec3 east = vec3(north.y, -north.x, 0.0);
        float lon = atan(dot(n, east), n.z);
        float lat = asin(clamp(dot(n, north), -1.0, 1.0));
        vec3 albedo = texture(wolfMoonMap, vec2(0.5 + lon / (2.0 * WOLF_PI), 0.5 + lat / WOLF_PI)).rgb;
        vec3 s = normalize(uEepMoonPhase);
        float mu0 = max(dot(n, s), 0.0);
        float mu = max(n.z, 0.0);
        float ls = 2.0 * mu0 / (mu0 + mu + 1e-4);
        vec3 lit = albedo * ls * 1.35;
        float night = clamp(uEepMoonNorth.z, 0.0, 1.0);
        g = lit + albedo * 0.03 * night;
        float lum = dot(lit, vec3(0.299, 0.587, 0.114));
        a = clamp(max(lum * 1.8, night * 0.95), 0.0, 1.0) * smoothstep(1.0, 0.98, r) * uEepMoonFade;
    }
    else
    {
        // Source: moonF.glsl:89-102 (the EEP's moon texture)
        vec4 t = texture(wolfMoonMap, p * 0.5 + 0.5);
        if (t.a <= 2.0 / 255.0) return color;
        g = t.rgb * uEepMoonBright;
        a = t.a * uEepMoonFade;
    }
    if (uEepGbufClamp > 0.5) g = clamp(g, vec3(0.0), vec3(1.0));
    vec3 skyG = wolfLinToSrgb(color / max(uEepSkyScale, 1e-4));
    return wolfSrgbToLin(mix(skyG, g, clamp(a, 0.0, 1.0))) * uEepSkyScale;
}

// [WolfViewer 2026-10-14] Paul: "add shooting stars back and meteors ... just one or two in the night sky occasionally".
// Up to two at once (wolfenvironmentrender.cpp updateMeteors picks them). Each runs along a great circle: it starts at
// uMeteorA.xyz and turns about the axis uMeteorB.xyz through uMeteorA.w radians over its life, uMeteorB.w the life gone
// (0..1); uMeteorC.x its peak brightness in the frame's linear units (0: none), .y the tail's share of the path. A thin
// line - a pixel or two wide, never thinner than the pixel (fragmentAngle) - brightest at the head and fading back along
// the tail, the whole flaring up and dying away over the life. The meteor layer (80-120 km) is far above the clouds, so
// the clouds composited over the dome cover it as they cover the stars.
uniform vec4 uMeteorA[2];
uniform vec4 uMeteorB[2];
uniform vec4 uMeteorC[2];

vec3 wolfMeteors(vec3 dirWorld, float fragmentAngle)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 2; ++i)
    {
        float peak = uMeteorC[i].x;
        if (peak <= 0.0) continue;
        vec3 start = uMeteorA[i].xyz;
        vec3 axis = uMeteorB[i].xyz;
        float arc = uMeteorA[i].w;
        float life = clamp(uMeteorB[i].w, 0.0, 1.0);
        // the pixel's angle off the meteor's great circle, and its angle along it from the start
        float off = asin(clamp(dot(dirWorld, axis), -1.0, 1.0));
        vec3 inPlane = dirWorld - axis * dot(dirWorld, axis);
        float len = length(inPlane);
        if (len < 1e-5) continue;
        inPlane /= len;
        float along = atan(dot(cross(start, inPlane), axis), dot(start, inPlane));
        float head = arc * life;
        float tail = max(arc * uMeteorC[i].y, 1e-4);
        float behind = head - along;   // > 0 behind the head
        if (behind < -2.0 * fragmentAngle || behind > tail) continue;
        float width = max(0.0005, fragmentAngle * 0.7);
        float across = exp(-(off * off) / (width * width));
        float lengthFade = behind < 0.0 ? exp(-(behind * behind) / (width * width)) : pow(1.0 - behind / tail, 2.0);
        float flare = sin(3.14159265 * life);
        sum += vec3(0.86, 0.95, 1.0) * peak * flare * across * lengthFade;
    }
    return sum;
}

void main()
{
    // this pixel's direction from the camera's own matrices (the screen triangle, wolfEnvSkyV.glsl), or a probe's
    vec3 dirWorld;
    if (uProbe > 0.5) dirWorld = normalize(wolfEepProbeDir[clamp(int(gl_FragCoord.x), 0, WOLF_EEP_GAINS - 1)].xyz);
    else
    {
        vec2 ndc = (gl_FragCoord.xy - viewportOrigin) / resolution * 2.0 - 1.0;
        vec4 p = uScreenToWorldDir * vec4(ndc, 0.0, 1.0);
        dirWorld = normalize(p.xyz / p.w);
    }

    // Source: sky.frag:45-48
    float shadowLength = 0.0;
#ifdef HAS_SHADOW_LENGTH
    if (uProbe < 0.5) shadowLength = texture(shadowLengthBuffer, (gl_FragCoord.xy - viewportOrigin) / resolution).r;
    // [WolfViewer 2026-10-21] Maximus Lear's black dome (AMD RX 7800 XT): every sky pixel reads this; a NaN or inf in it makes
    // GetSkyRadiance's shadow-length branch (brunetonRuntime.glsl ClampRadius(sqrt(d*d + ...)) with d = shadow length) NaN
    // and the guard below paints the pixel black. Not finite or negative: no shaft here.
    if (isnan(shadowLength) || isinf(shadowLength) || shadowLength < 0.0) shadowLength = 0.0;
#endif

    // Source: sky.vert:48-50 (world -> ECEF, km) and sky.frag:50-54
    vec3 cameraPositionECEF = (worldToECEFMatrix * vec4(cameraPosition, 1.0)).xyz;
    vec3 vCameraPosition = (cameraPositionECEF + altitudeCorrection) * METER_TO_LENGTH_UNIT;
    vec3 rayDirection = normalize((worldToECEFMatrix * vec4(dirWorld, 0.0)).xyz);
    vec3 dRDdx = dFdx(rayDirection);
    vec3 dRDdy = dFdy(rayDirection);
    float fragmentAngle = length(dRDdx + dRDdy) / length(rayDirection);

    // [WolfViewer 2026-10-20] the EEP gain for this view ray (wolfEepDomeGain, above). It is measured on the SKY (probeSky in
    // wolfenvironmentrender.cpp: stock sky / this dome's sky on the grid round the stock light, brunetonRuntime.glsl
    // wolfGainGrid - since 2026-10-25 down to 0.1 deg above the true horizon, every node still sky, not sea), so
    // it is applied once, to sky light only: the sky's whole colour below, and on the sea only the air's in-scatter in front
    // of it - as the world's water takes it (its aerial perspective gains the in-scatter alone: brunetonRuntime.glsl
    // wolfSkyRadianceToPointRel, eepGain x radiance; waterF.glsl wolfTkAerialPerspective). Miss Zenn's white sea (Hyena Hills,
    // night, 2026-10-20): the whole sea colour was multiplied by it, and the sea's mirror is LL's default reflection probe,
    // whose faces this same program draws with the gain already on (renderSkyDome) - the reflected sky took the gain twice,
    // the sea's own body and glint (physically lit, in the frame's units like the world's water body) took it once with no
    // reason. At night the stock / physical ratio near the horizon is large, and its square clipped the sea to white.
    vec3 domeGain = wolfEepDomeGain > 0.5 ? wolfEepGainDirCore(vCameraPosition, rayDirection) : vec3(1.0);

    // ── the sea to the true horizon: sky.frag:56-86 (GROUND_ALBEDO) with a water surface; else the sky ──
    // Source: sky.frag:58-61 the ray meets the planet (the sea is the bottom of the atmosphere)
    float r = length(vCameraPosition);
    float mu = dot(vCameraPosition, rayDirection) / r;
    bool intersectsGround = wolf_sea_height >= 0.0 && RayIntersectsGround(ATMOSPHERE, r, mu);
    // Source: sky.frag:62-64. [WolfViewer] RayIntersectsGround (r, mu) and raySphereFirstIntersection (3D dot
    // products) round differently near the tangent ray: where the second finds no hit (-1) the sky is drawn.
    float distanceToGround = intersectsGround ? raySphereFirstIntersection(vCameraPosition, rayDirection, ATMOSPHERE.bottom_radius) : -1.0;
    vec3 color;
    if (intersectsGround && distanceToGround >= 0.0)
    {
        // Source: sky.frag:62-76
        vec3 groundPosition = rayDirection * distanceToGround + vCameraPosition;
        vec3 surfaceNormal = normalize(groundPosition);
        vec3 skyIrradiance;
        vec3 sunIrradiance = GetSunAndSkyIrradiance(vCameraPosition, surfaceNormal, sunDirection, skyIrradiance);
        vec3 tr = vec3(1.0);
        vec3 inscatter = vec3(0.0);
        // [WolfViewer] a camera at the surface (within a metre: a float km at 6360 km resolves 0.49 m) has no air
        // between it and the sea, and GetSkyRadianceToPoint would normalise a zero vector (point - camera)
        if (distanceToGround > 1e-3)
        {
            inscatter = GetSkyRadianceToPoint(vCameraPosition, ATMOSPHERE.bottom_radius * surfaceNormal, shadowLength, sunDirection, tr);
        }
        // the moonlit air and the moon's light on the water, as in wolfSkyWithBodies
        if (uMoonSkyScale > 0.0)
        {
            vec3 trMoon, skyMoon;
            if (distanceToGround > 1e-3) inscatter += GetSkyRadianceToPoint(vCameraPosition, ATMOSPHERE.bottom_radius * surfaceNormal, 0.0, uMoonDirECEF, trMoon) * uMoonSkyScale;
            vec3 sunMoon = GetSunAndSkyIrradiance(vCameraPosition, surfaceNormal, uMoonDirECEF, skyMoon);
            sunIrradiance += sunMoon * uMoonSkyScale;
            skyIrradiance += skyMoon * uMoonSkyScale;
        }
        // the water surface instead of groundAlbedo * RECIPROCAL_PI (sky.frag:84): its body colour lit by the
        // same irradiance, and the sky mirrored - the body term is waterF.glsl's wolfWaterBody (the water fog
        // colour x the light on the water / pi); the mirror carries the moon, bodies and night sky as the sky does
        vec3 mirrorDir = reflect(rayDirection, surfaceNormal);
        vec3 mirrorWorld = normalize(transpose(mat3(worldToECEFMatrix)) * mirrorDir);   // a rigid frame: R^-1 = R^T
        // what the water in the world mirrors (waterF.glsl: LL's reflection probes, whose default probe holds the sky
        // with the clouds rendered into it); in a probe face itself, and with probes off, the sky alone
        vec3 mirrored;
        if (uSeaProbe > 0.5)
        {
            // the probe's faces are this program's sky with the EEP gain already on it (renderSkyDome sets wolfEepDomeGain
            // for every draw, the probe faces included): corrected sky, used as it is
            mirrored = textureLod(reflectionProbes, vec4(mirrorWorld, 0.0), 0.0).rgb;
        }
        else
        {
            // the sky the probe would hold: this program's sky in the mirrored direction with ITS gain (the sky branch below,
            // seen from the sea point), then the EEP's moon over the corrected sky, in the order the sky branch applies them
            vec3 seaPointKm = ATMOSPHERE.bottom_radius * surfaceNormal;
            vec3 mirroredSky = wolfSkyWithBodies(seaPointKm, mirrorDir, mirrorWorld, 0.0, fragmentAngle);
            if (wolfEepDomeGain > 0.5) mirroredSky *= wolfEepGainDirCore(seaPointKm, mirrorDir);
            mirrored = wolfEepMoon(mirrorWorld, mirroredSky);
        }
        vec3 body = waterFogColorLinear * RECIPROCAL_PI * (sunIrradiance + skyIrradiance);
        // the surface as waterF.glsl shades the world's water, so the planes' blend into this sea at the far clip
        // (waterF.glsl wolf_far_clip) meets the same colour: dot(viewVec, normal) is the view ray against the surface
        // normal there too (waterF.glsl:375 viewVec = normalize(pos.xyz), the eye-space ray to the surface)
        vec3 radiance = wolfFarSeaSurface(body, mirrored, dot(rayDirection, surfaceNormal));
        // [WolfViewer round 11] the sun's glint (wolftk/seaGlint.glsl, the same model the world's water uses under wolf_phys):
        // F D / (4 V.N) x the sun's irradiance perpendicular to it at the sea (GetSunAndSkyIrradiance with the sun as the
        // normal: solar irradiance x the transmittance to the sun); the moon's the same, x its scale. Without it the open
        // sea past the water planes was the mirrored sky alone - the dark band under the dawn horizon (round 10, dawn_probe.py)
        {
            vec3 seaPoint = ATMOSPHERE.bottom_radius * surfaceNormal;
            vec3 dummySky;
            vec3 sunPerp = GetSunAndSkyIrradiance(seaPoint, sunDirection, sunDirection, dummySky);
            radiance += wolfSeaGlint(surfaceNormal, -rayDirection, sunDirection, wolfSeaMss) * sunPerp;
            if (uMoonSkyScale > 0.0)
            {
                vec3 moonPerp = GetSunAndSkyIrradiance(seaPoint, uMoonDirECEF, uMoonDirECEF, dummySky) * uMoonSkyScale;
                radiance += wolfSeaGlint(surfaceNormal, -rayDirection, uMoonDirECEF, wolfSeaMss) * moonPerp;
            }
        }
        // Source: sky.frag:85 radiance * transmittance + inscatter; the EEP gain on the in-scatter only (domeGain, above)
        color = radiance * tr + inscatter * domeGain;
    }
    else
    {
        color = wolfSkyWithBodies(vCameraPosition, rayDirection, dirWorld, shadowLength, fragmentAngle) * domeGain;
    }

    // the EEP's moon over the corrected sky (not in a probe: the probes measure the sky alone, as the reference is)
    if (uProbe < 0.5 && !(intersectsGround && distanceToGround >= 0.0)) color = wolfEepMoon(dirWorld, color);
    // the shooting stars, over the sky only (not in a probe, not on the sea)
    if (uProbe < 0.5 && !(intersectsGround && distanceToGround >= 0.0)) color += wolfMeteors(dirWorld, fragmentAngle);

    // never an infinity or a NaN in the frame (fp16 holds 65504)
    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(60000.0));

    // LL G-buffer, as skyF.glsl writes it
    frag_data[1] = vec4(0);
    frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_data[0] = uProbe > 0.5 ? vec4(color, 1.0) : vec4(0);
    frag_data[3] = vec4(color, 1.0);
#else
    frag_data[0] = vec4(color, 1.0);
#endif
}
