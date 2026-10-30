/**
 * @file class1\environment\waterV.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// [2026-09-12] WOLF_WATER_FULL: our wind-sea cascades, boat wash and baked shore/exposure/zone
// fields are 6 extra texture units in this stage. An Apple GPU has 16 for the whole fragment
// stage and the water program already spends 13 (19 with sun shadows) before any of ours, so the
// Water Shader would not link on Macs ("No definition of calcAtmospherics" is Apple's linker
// giving up) and every Mac crashed at startup. llviewershadermgr.cpp defines WOLF_WATER_FULL
// only when the measured unit count affords it; otherwise the samplers are compiled out, their
// *Ready gates are constant 0 so every dependent branch is dead, and the water is Firestorm's.
#ifdef WOLF_WATER_FULL
uniform sampler2D wolfDepthField;
uniform sampler2D wolfExposureField;
uniform sampler2D wolfZoneField;
uniform sampler2D fftDisp0;
uniform sampler2D fftDisp1;
uniform sampler2D wakeSampler;
uniform float depthReady;
uniform float fftReady;
uniform float wakeReady;
uniform float exposureReady;
uniform float zoneReady;
// [DRY BUILDS 2026-10-26] The waterline clearance field (wolfwaterfield.h WolfWaterClearance): LEVELS (3) camera-window
// levels of N (256) texels stacked in rows, RGBA32F (R S_all, G D, B S_dry, A open weight - see wolfClearSample), then the
// two terrain blocks waterF.glsl reads. wolfClearLevel[k] = (agent-space SW corner of block k's texel grid, texel metres, 0);
// wolfClearMisc = (lowest source cap = lowest live water height, ready, 0, 0). Bound by lldrawpoolwater.cpp.
uniform sampler2D wolfClearField;
uniform vec4 wolfClearLevel[5];   // [OPEN SEA SHORES 2026-10-26/] + [3], [4]: the terrain blocks (waterF.glsl)
uniform vec4 wolfClearMisc;
#define WOLF_CLEAR_READY wolfClearMisc.y
#define WOLF_TEX_WOLF_DEPTH_FIELD(uv) texture(wolfDepthField, uv)
#define WOLF_TEX_WOLF_EXPOSURE_FIELD(uv) texture(wolfExposureField, uv)
#define WOLF_TEX_WOLF_ZONE_FIELD(uv) texture(wolfZoneField, uv)
#define WOLF_TEX_FFT_DISP_0(uv) texture(fftDisp0, uv)
#define WOLF_TEX_FFT_DISP_1(uv) texture(fftDisp1, uv)
#define WOLF_TEX_WAKE_SAMPLER(uv) texture(wakeSampler, uv)
#else
#define WOLF_TEX_WOLF_DEPTH_FIELD(uv) vec4(0.0)
#define WOLF_TEX_WOLF_EXPOSURE_FIELD(uv) vec4(0.0)
#define WOLF_TEX_WOLF_ZONE_FIELD(uv) vec4(0.0)
#define WOLF_TEX_FFT_DISP_0(uv) vec4(0.0)
#define WOLF_TEX_FFT_DISP_1(uv) vec4(0.0)
#define WOLF_TEX_WAKE_SAMPLER(uv) vec4(0.0)
#define depthReady 0.0
#define fftReady 0.0
#define wakeReady 0.0
#define exposureReady 0.0
#define zoneReady 0.0
#define WOLF_CLEAR_READY 0.0
#endif

uniform mat4 modelview_matrix;
uniform mat3 normal_matrix;
uniform mat4 modelview_projection_matrix;

in vec3 position;
// <FS:WolfViewer> The water vertex buffer has always carried normals (LLDrawPoolWater::
// VERTEX_DATA_MASK includes MAP_NORMAL) and stock never read them — vary_normal was
// hardcoded to +Z. A wolfwater prim surface can be TILTED, so the surface's own up vector
// has to come from the geometry. Region water writes (0,0,1) into every one of these, so
// reading the attribute changes nothing for it.
in vec3 normal;
// <WolfViewer> Ribbon coordinates of a terrain-conforming STREAM mesh (LLVOWater::
// ConformingMesh): x = metres along the flow, y = metres across. Read only when
// wolfStream > 0; region water's texcoords are 0..1 fractions and are never used here.
in vec2 texcoord0;
// Terrain-LOD lift (LLVOWater::ConformingMesh::mLodLift): metres the terrain as drawn at
// render stride 4 (x) and 16 (y) rises above this vertex. Read only when wolfTerrainLod > 0.
in vec2 texcoord1;
// </WolfViewer>
// </FS:WolfViewer>


void calcAtmospherics(vec3 inPositionEye);

uniform vec2 waveDir1;
uniform vec2 waveDir2;
uniform float time;
// [SMOOTH SEA 2026-10-28] Phases ACCUMULATED on the CPU from the eased sea state (lldrawpoolwater.cpp wolfUpdateSea), in
// place of `time` x a weather-set speed, which moved every crest by hours x the change when the weather changed:
//   wolfSeaClock.x  the swell's clock, integral of sqrt(waveFrequency + 0.001) x waveSpeed (wolfSwellPhaseT)
//   wolfSeaClock.y  the shore breakers' phase, integral of 4.5 x clamp(|waveDir1| x 0.885, 0.4, 2.0), mod 2 pi
//   wolfSeaClock.z  the automatic surf's omega0 x time, integrated, mod 2 pi
//   wolfWaveScroll  xy = integral of waveDir1, zw = integral of waveDir2: the normal maps' scroll (stock: dir x time)
uniform vec4 wolfSeaClock;
uniform vec4 wolfWaveScroll;
uniform vec3 eyeVec;
uniform float waterHeight;
uniform vec3 lightDir;

// <FS:WolfViewer> Gerstner swell. Stock water is a flat plane and every "wave" in it is
// normal-map shading in the fragment stage — which is exactly why it reads as plastic.
// These drive real vertex displacement; waveAmplitude 0 reproduces stock behaviour
// bit-for-bit (the whole block below is skipped).
//   waveAmplitude  metres, the height of the swell        (WolfViewerWaterWaveHeight)
//   waveFrequency  1 / base wavelength in metres          (WolfViewerWaterWaveScale)
//   waveSpeed      multiplier on the physical phase speed (WolfViewerWaterWaveSpeed)
//   waveFade       (start, end) metres from the camera over which displacement fades out
uniform float waveAmplitude;
uniform float waveFrequency;
uniform float waveSpeed;
uniform vec2 waveFade;
// <WolfViewer> wolfStream: surface speed of a stream mesh in m/s, 0 for everything else.
// wolfWaterfall: 1 = the whole plane is a waterfall sheet (legacy per-plane flag).
uniform float wolfStream;
uniform float wolfWaterfall;
// Terrain stride per metre of distance (LLSurfacePatch::updateVisibility's
// DEFAULT_DELTA_ANGLE / metresPerGrid, over the LOD factor); 0 = no lift.
uniform float wolfTerrainLod;
// </WolfViewer>
// <WolfViewer 2026-09-06> The rest of WolfStorm's wave field, ported (Water.js vertex
// stage — same names, same numbers, keep in step):
//   stormChaos / maxWaveHeight — the sea state's chaos term and the crest backstop
//     (lldrawpoolwater.cpp, from wolfseastate.h);
//   wolfDepthField / wolfExposureField — the region's baked terrain depth (RGBA32F: R =
//     height, GB = beachward direction x confidence, A = smoothed height) and open-water
//     exposure (R32F over a 3x span), REGION space, from wolfwaterfield.cpp; the vertex
//     positions are AGENT space, so wolfRegionOrigin converts. Exposure calms the swell
//     toward every shore (0.15x) and lets it roll offshore (1.3x); the depth field lifts
//     shoaling breakers on the actual beach;
//   fftDisp0/1 — the spectral wind-sea cascades' displacement (wolfoceanfft.cpp), tiling
//     agent XY every fftTile metres, faded out by fftFade metres from the camera;
//   wakeSampler — the boat wash field (wolfwakefield.cpp): R foam, G lift, B dip.
uniform float stormChaos;
uniform float maxWaveHeight;
// [HURRICANE 2026-10-30] the region's storm on the sea (wolfStormSeaFactor below; lldrawpoolwater.cpp renderPostDeferred)
uniform vec4 wolfSeaStorm;       // x 1 = a storm is running (0: none, skipped), y W intensity, z h hemisphere, w the floor = S_eep [ROUND 10]
uniform vec4 wolfSeaStormEye;    // xy the eye c(t) (agent metres), z R, w Re
uniform vec4 wolfSeaStormWind;   // xy the track velocity speed (sin H, cos H), zw the ambient wind (the way the air moves), m/s
uniform vec4 wolfSeaStormMisc;   // x vmax, y S_eep (the index this water is drawn with), zw the r < 1 m bearing (sin, cos)
// [ROUND 10] the storm swell (README "The storm swell"; wolfhurricane.cpp stormSwellAmp): x A_MAX (m), y V_5 (m/s), z 1 = on,
// w its length L_STORM (m). It runs out from the eye (wolfSeaStormEye.xy) in rings over the whole region.
uniform vec4 wolfSeaStormSwell;
// [ROUND 10] README "The storm swell": a = A_MAX W g(r), g = (V_H(max(r, Rm)) / V_5)^2 inside R, (V_H(R) / V_5)^2 sqrt(R / r) beyond
// (Pierson & Moskowitz 1964: height ~ U^2; Collard, Ardhuin & Chapron 2009: swell energy ~ 1 / distance) - the CPU twin is
// wolfhurricane.cpp stormSwellAmp, the rocker's wolfboatrock.cpp; before the water's own exposure, edge and distance fades.
float wolfStormSwellAmp(vec2 p)
{
    float R = wolfSeaStormEye.z;
    float Rm = 1.25 * wolfSeaStormEye.w;
    float r = length(p - wolfSeaStormEye.xy);
    float rr = r <= R ? max(r, Rm) : R;
    float x = pow(Rm / max(rr, 0.01), 1.31);
    float v = wolfSeaStormMisc.x * sqrt(x * exp(1.0 - x)) / max(wolfSeaStormSwell.y, 1.0);
    float g = v * v * (r <= R ? 1.0 : sqrt(R / max(r, 1.0)));
    return wolfSeaStormSwell.x * wolfSeaStorm.y * g;
}
out float vStormSwellAmp;   // [HURRICANE 2026-10-30/] the storm swell train's amplitude here (class3 waterF.glsl swellFoldFraction)
// [WILD SEA 2026-10-30] the storm's sea here for the fragment's foam (class3 waterF.glsl, README "The wild sea"): x k = W env(r)
// (the storm's share of the sea, 0 beyond 1.3 R), y U_sea (m/s), zw U_sea's direction (unit, the way the air moves). 0 with no storm.
out vec4 vStormSea;
out float vStormGain;       // [WILD SEA 2026-10-30/] the drawn amplitude's storm factor here (1 with no storm)
uniform vec2 wolfRegionOrigin;
// <WolfViewer 2026-09-26> SWELL TOWARD THE REGION CENTRE (Paul: "waves should ALWAYS travel
// towards the center of the region" — the next region's waves pointed the way of the one the
// agent stood in, because the swell ran on one EEP compass heading for every region). The
// agent-space centre and half width of the region this water belongs to, per plane
// (lldrawpoolwater.cpp); wolfSwellHalf 0 = no region: keep the EEP heading.
uniform vec2 wolfSwellCentre;
uniform float wolfSwellHalf;
uniform vec2 depthRegionSize;
// <WolfViewer 2026-09-20> region-space origin of the depth field: (0,0) for a whole-region
// bake, the camera window's corner on regions wider than 2048 m (wolfwaterfield.cpp fieldWindow).
uniform vec2 depthOrigin;
uniform float depthWaterLevel;
uniform vec2 exposureOrigin;
uniform vec2 exposureSize;
uniform float shoreWavesEnabled;
// [WAVES 2026-09-07] Per-region wave ZONE energy over the same 3x span (wolfwavezones.cpp
// fill: surf 1, open 0.55, calm 0.15, off 0), from About Land > Waves of this region and its
// neighbours. Source: wolfstorm Water.js zoneSampler.
uniform vec2 zoneOrigin;
uniform vec2 zoneSize;
// [SURF 2026-09-07] The surf train (WAVES_PLAN §2, Water.js): wave HEIGHT offshore (m),
// the set cadence (s), the wavelength (m), a tempo scale. From About Land > Waves.
uniform float surfHeight;
uniform float surfSetInterval;
uniform float surfLength;
uniform float surfSpeed;
// <WolfViewer 2026-09-21> The lattice this water plane was tessellated with
// (llvowater.cpp wolf_graded_axis, pushed per face by lldrawpoolwater.cpp):
// xy = the agent-space camera position it was graded about, zw = the per-axis step scale the
// U16 vertex cap forced. zw = 0 = not a graded lattice (a conforming stream mesh).
uniform vec4 wolfLatticeGrade;
// <WolfViewer 2026-09-21> The deep-water surf wavenumber the exposure bake's A channel (the
// OPTICAL PATH from the open sea) was integrated with — wolfwaterfield.cpp, Field::mSurfK0.
// The phase is k0 * path, so it must be THIS k0 and not whatever the sliders read this frame,
// or phase and path disagree for the seconds between a slider move and the rebake.
// 0 = no baked path; the surf falls back to a constant-wavelength train on the plain distance.
uniform float surfPathK0;
uniform float calmRipple;   // [WAVES 2026-09-07] the calm cells' ripple, metres (lldrawpoolwater.cpp)
uniform float smallScale;   // [WAVES 2026-09-10] the small-wave cells' swell, fraction of the open sea (lldrawpoolwater.cpp)
out vec4 vSurf;   // [SURF rev3] x crest (peak only), y breaking, z amplitude used (0 = no surf), w wash behind the crest
// [OPEN SEA SHORES 2026-10-26] The automatic surf on open-sea beaches (lldrawpoolwater.cpp, from the sea state):
// x = wave height at the break (m), y = set interval (s), z = deep-water wavelength (m), w = 1 on, 0 off.
uniform vec4 wolfAutoSurf;
// [DRY BUILDS 2026-10-26] For waterF.glsl's per-pixel land rule: xy = the DISPLACED agent-space position, z = the swash
// band's ceiling over the water level here (0 = none), w = the final displacement along the surface normal.
out vec4 vary_wolf_dry;
uniform vec2 fftTile;
uniform vec2 fftFade;
uniform vec2 wakeRegionSize;
uniform vec2 wakeOrigin;   // <WolfViewer 2026-10-06/> the wake field window's south-west corner, region metres
uniform float wakeStrength;
uniform float boundedWaterDepth;
// </WolfViewer>
// </FS:WolfViewer>

out vec4 refCoord;
out vec4 littleWave;
out vec4 view;
out vec3 vary_position;
out vec3 vary_light_dir;
out vec3 vary_tangent;
out vec3 vary_normal;
out vec2 vary_fragcoord;
// <FS:WolfViewer> Analytic slope (dz/dx, dz/dy) of the displaced swell, and the crest
// height at this vertex. The fragment stage tilts its shading normal with the slope —
// without it the geometry rolls but the LIGHT does not, and big swells read as flat
// painted stripes. Height feeds the crest highlight / foam.
out vec2 vary_wave_slope;
out float vary_wave_height;
// <WolfViewer> Agent-space vertex position: a waterfall sheet (waterF.glsl wolfWaterfall)
// scrolls its foam by world HEIGHT, i.e. down the fall, whatever way the sheet faces.
out vec3 vary_world_pos;
// How much of this vertex is FALLING water, 0..1. For a stream mesh it comes from the
// surface's own slope, so one ribbon runs level, tips over a ledge into a fall and
// levels out again, all in one draw; the fragment stage blends the waterfall look by it.
out float vary_fall;
// </WolfViewer>
// <WolfViewer 2026-09-06> vShore = (shore wave phase, shoal factor) — the single source of
// truth the fragment's swash and breaker foam follow; vWake = the wake field's foam here;
// [HARBOUR STRIPES 2026-10-28] vShore.z = the breaker's height H crest to trough (2 x bAmp below; 0 = no breaker),
// which sets where it breaks and how much whitewater it makes (waterF.glsl); vShore.w = 1 where the surf train
// below is the AUTOMATIC surf, whose foam waterF.glsl breaks into patches (painted surf stays as designed);
// vSwell = (localAmp, wavelengthJitter, swellScale, 0) so the fragment can re-evaluate
// the three dominant trains at PAST times for the whitecap memory.
out vec4 vShore;
out float vWake;
out vec4 vSwell;
// </WolfViewer>
// </FS:WolfViewer>

float wave(vec2 v, float t, float f, vec2 d, float s)
{
   return (dot(d, v)*f + t*s)*f;
}

// <FS:WolfViewer> ---------------------------------------------------------------------
// Gerstner swell, ported from WolfStorm's water (wolfstorm/js/libs/Water.js), which is
// itself the standard formulation from GPU Gems chapter 1 "Effective Water Simulation
// from Physical Models" (Finch, 2004).
//
// A Gerstner (trochoidal) wave moves each surface point in a circle rather than only up
// and down, so crests sharpen and troughs flatten the way real swell does:
//
//     x += Q * A * d.x * cos(k * dot(d, p) - w * t)
//     y += Q * A * d.y * cos(k * dot(d, p) - w * t)
//     z +=     A       * sin(k * dot(d, p) - w * t)
//
// with wavenumber k = 2*pi / wavelength and, in deep water, phase speed c = sqrt(g/k) —
// so long waves genuinely travel faster than short ones with nothing to tune. Q is the
// steepness, divided by (k * A * numWaves) so summing many waves cannot pinch the surface
// into self-intersecting loops.
//
// Simplex noise gives each patch of sea its own amplitude and wavelength so the swell is
// not a repeating corrugation.

vec3 permute3(vec3 x) { return mod(((x*34.0)+1.0)*x, 289.0); }

float snoise(vec2 v)
{
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                       -0.577350269189626, 0.024390243902439);
    vec2 i  = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod(i, 289.0);
    vec3 p = permute3(permute3(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);
    m = m*m; m = m*m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0*a0 + h*h);
    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

// Four octaves of the same noise: the fine chop riding on the swell.
float fbm(vec2 p, float t)
{
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++)
    {
        value += amp * snoise(p + t * (0.1 + float(i) * 0.05));
        p *= 2.0;
        amp *= 0.5;
    }
    return value;
}

// [SMOOTH SEA 2026-10-28] The temporal phase of a train of wavelength wl (before jitter): sqrt(9.8 k) x time x waveSpeed
// was the old term, k = 2 pi / wl; wl is a multiple of 1 / (waveFrequency + 0.001), so it is
// sqrt(9.8 k / (waveFrequency + 0.001)) x wolfSeaClock.x, the clock carrying sqrt(waveFrequency + 0.001) x waveSpeed.
// The same in a steady sea; under a weather change the crests change speed and keep their place. The wavelength jitter
// (chaos x 0.3 x a slowly drifting noise) is left out of the TEMPORAL phase: multiplied by the session clock its drift
// alone moved the crests faster and faster the longer the viewer ran, and with the chaos changing it was a race.
float wolfSwellPhaseT(float wl)
{
    return sqrt(9.8 * (6.28318 / wl) / (waveFrequency + 0.001)) * wolfSeaClock.x;
}

// [HURRICANE 2026-10-30] gerstnerWave with the temporal phase given (the storm swell train, wolfSeaClock.w: its own clock at its
// fixed length, lldrawpoolwater.cpp mStormSwell - not wolfSwellPhaseT, whose factor follows waveFrequency), no jitter
vec3 gerstnerWaveAt(vec2 pos, float wavelength, float amp, vec2 dir, float steepness, float numWaves, float phaseT)
{
    float k = 6.28318 / wavelength;
    vec2 d = normalize(dir);
    float f = mod(k * dot(d, pos) - phaseT, 6.2831853);
    float Q = steepness / (k * amp * numWaves + 0.001);
    return vec3(Q * amp * d.x * cos(f), Q * amp * d.y * cos(f), amp * sin(f));
}
vec2 gerstnerSlopeAt(vec2 pos, float wavelength, float amp, vec2 dir, float phaseT)
{
    float k = 6.28318 / wavelength;
    vec2 d = normalize(dir);
    float f = mod(k * dot(d, pos) - phaseT, 6.2831853);
    return d * (amp * k * cos(f));
}

vec3 gerstnerWave(vec2 pos, float wavelength, float jit, float amp, vec2 dir, float steepness, float numWaves)
{
    float k = 6.28318 / (wavelength * jit);
    vec2 d = normalize(dir);
    // <WolfViewer 2026-09-20> wrap the phase before sin/cos: 50,000 m from the region origin it
    // is ~10^5 rad, beyond where GPU sin() precision is specified (Intel and the D3D path lose
    // it first). mod() in float32 keeps ~1 mrad; identical below a few hundred metres.
    float f = mod(k * dot(d, pos) - wolfSwellPhaseT(wavelength), 6.2831853);
    float Q = steepness / (k * amp * numWaves + 0.001);
    return vec3(Q * amp * d.x * cos(f),
                Q * amp * d.y * cos(f),
                amp * sin(f));
}

// <WolfViewer 2026-09-21> ----------------------------------------------------------------
// METRES BETWEEN THIS VERTEX AND ITS NEIGHBOURS on the water lattice.
//
// THE PROBLEM THIS SOLVES (Paul 09-21: "these are supposed to be surf waves I painted on the
// water, they look jagged and rubbish"). The lattice is graded about the camera — 2 m beside
// the eye, 4 % of the distance further out — so at 84 m the vertices are 3.4 m apart and at
// 310 m they are 12.4 m apart. The surf train's own wavelength in the shallows is ~21 m
// (36 m deep-water length, shortened by finite-depth dispersion), which is 6 vertices per
// wave at 84 m but 1.7 at 310 m — BELOW NYQUIST. The crest could not be drawn, so it was
// aliased instead: one vertex per crest pulled up into a sharp pyramid, a whole seascape of
// them, each exactly one lattice quad wide. The same screenshot shows the near water, where
// the lattice does resolve the crest, rolling smoothly.
//
// So the wave has to be band-limited to the lattice, the way the spectral cascades already
// fade their displacement with distance. This function is the lattice half of that: it MUST
// mirror llvowater.cpp wolf_graded_axis — step out from the focus is 4 % of the distance to
// it, never under 2 m nor over 128 m, times the decimation the vertex cap forced.
// Returns 0 where this surface is not a graded lattice; callers treat that as fully resolved.
float wolfLatticeStep(vec2 p)
{
    if (wolfLatticeGrade.z <= 0.0 || wolfLatticeGrade.w <= 0.0)
    {
        return 0.0;
    }
    vec2 s = clamp(abs(p - wolfLatticeGrade.xy) * 0.04, 2.0, 128.0) * wolfLatticeGrade.zw;
    return max(s.x, s.y);
}
// </WolfViewer> ---------------------------------------------------------------------------

// <WolfViewer 2026-09-21> ----------------------------------------------------------------
// SURF DISPERSION. KEEP BYTE-IDENTICAL with wolfsurfcurlV.glsl, wolfboatrock.cpp,
// wolfwaterfield.cpp (WolfWaterField::surfWavenumber / surfShoalGain) and the WolfStorm
// mirrors in Water.js, surf_curl.js and terrain_manager.js.
//
// The local wavenumber of a wave whose DEEP-water wavenumber is k0, in depth h.
// Source: Guo (2002), "Simple and explicit solution of wave dispersion equation", Coastal
// Engineering 45, 71-74, restated by Fenton (2006) "A note on two approximations to the
// linear dispersion relation for surface gravity water waves" eq. (3):
//     k d = (sigma^2 d / g) (1 - exp(-(sigma sqrt(d/g))^(5/2)))^(-2/5).
// surfLength IS the deep-water wavelength, so sigma^2 = g k0 and sigma sqrt(d/g) = sqrt(k0 d),
// which collapses the whole expression to the line below. Exact in both the deep-water limit
// (k -> k0) and the shallow-water limit (k -> omega / sqrt(g h)), within 0.7 % between them.
float wolfSurfK(float k0, float h)
{
    float u = pow(max(k0 * max(h, 0.02), 1e-4), 1.25);
    return k0 * pow(max(1.0 - exp(-u), 1e-6), -0.4);
}

// Green's-law shoaling gain: the factor a wave's height grows by coming from deep water into
// depth h, from conservation of energy flux at CONSTANT frequency.
//     Ks = sqrt(cg0 / cg),  cg = n omega / k,  n = 0.5 (1 + 2kh / sinh 2kh),  cg0 = 0.5 omega / k0
// so Ks = sqrt(0.5 (k / k0) / n). 1 in deep water; the familiar h^(-1/4) of Green's law in the
// shallows. Source: Wikipedia, Wave shoaling (shoaling coefficient / energy flux).
float wolfSurfShoal(float k0, float k, float h)
{
    float kh2 = min(2.0 * k * max(h, 0.02), 20.0);   // sinh overflows past ~20; n is 0.5 long before
    float n = 0.5 * (1.0 + kh2 / sinh(max(kh2, 1e-4)));
    return sqrt(0.5 * (k / max(k0, 1e-6)) / max(n, 1e-4));
}
// </WolfViewer> ---------------------------------------------------------------------------

// [DRY BUILDS 2026-10-26] ---------------------------------------------------------------------------------------------
// "Water should not appear on land" (Paul 2026-10-26) and builds made for flat OpenSim water stay dry (Jupiter Rowland).
// The proof of the bounds is in wolfwaterfield.h (WolfWaterClearance); the CPU twins are WolfWaterClearance::sample /
// upwardRoom / softCap, used by wolfboatrock.cpp. KEEP IN STEP.
const float WOLF_CLEAR_ALPHA = 0.195;   // 0.78 (McCowan) x 0.25 (SURF_REEF_SLOPE): the cap's rise per metre from a source
const float WOLF_CLEAR_BETA = 0.1;      // the horizontal push allowed per metre from a source
const float WOLF_CLEAR_EYE = 0.3;       // the camera guard: crests stay this far under the eye
const float WOLF_CLEAR_N = 256.0;
// The four channels of the clearance field at agent-space p (wolfwaterfield.h WolfWaterClearance):
//   x = S_all (every source), y = D, z = S_dry (prims and inland land only), w = the open-sea weight.
// x, y, z: the best of the levels whose inner box (the hull of the texel centres) holds p, each level's own value bounded
// by what lies outside it (the lowest source cap + ALPHA x the distance out). Every one is a lower bound of the true
// value, so the largest of them still is. -1e9 where no level holds p. w: the finest level holding p, faded in over 32 m
// from that level's edge so the classification never switches at a window edge.
vec4 wolfClearSample(vec2 p)
{
    vec4 best = vec4(-1e9, 0.0, -1e9, 0.0);
#ifdef WOLF_WATER_FULL
    for (int k = 2; k >= 0; --k)
    {
        vec4 L = wolfClearLevel[k];
        if (L.z <= 0.0)
        {
            continue;
        }
        vec2 g = (p - L.xy) / L.z;   // texel units, centres at i + 0.5
        const float lo = 0.5;
        const float hi = WOLF_CLEAR_N - 0.5;
        if (g.x < lo || g.y < lo || g.x > hi || g.y > hi)
        {
            continue;
        }
        float dEdge = min(min(g.x - lo, g.y - lo), min(hi - g.x, hi - g.y)) * L.z;
        vec2 uv = vec2(g.x / WOLF_CLEAR_N, (float(k) * WOLF_CLEAR_N + g.y) / (5.0 * WOLF_CLEAR_N));
        vec4 c = texture(wolfClearField, uv);
        float outside = wolfClearMisc.x + WOLF_CLEAR_ALPHA * dEdge;
        best.x = max(best.x, min(c.x, outside));
        best.y = max(best.y, min(c.y, dEdge));
        best.z = max(best.z, min(c.z, outside));
        best.w = mix(best.w, c.w, smoothstep(0.0, 32.0, dEdge));
    }
#endif
    return best;
}
// The soft cap up * tanh(dz / up): odd, so troughs calm with the crests and the mean stays put, strictly under up, and
// the identity to first order where there is room. 0 where there is none.
// tanh is written out: far from any source up is ~1e6 m and dz / up ~1e-7, where a driver's tanh built as
// (e^2x - 1) / (e^2x + 1) loses every significant bit to cancellation; below 0.05 the series x - x^3 / 3 is exact
// to 3e-8 relative, above it the exponential form has no cancellation to speak of.
float wolfTanh(float x)
{
    float a = abs(x);
    if (a < 0.05)
    {
        return x * (1.0 - x * x / 3.0);
    }
    float e = exp(-2.0 * min(a, 10.0));
    return sign(x) * (1.0 - e) / (1.0 + e);
}
float wolfSoftCap(float dz, float up)
{
    return up > 0.0 ? up * wolfTanh(dz / up) : 0.0;
}
// [OPEN SEA SHORES 2026-10-26] The clearance cap's version: a crest is held under up as above, but a trough keeps at
// least WOLF_TROUGH_KEEP of itself, fading to all of it as the room grows to calmRef - so water beside a dock still
// moves instead of lying dead flat inside a line (Paul: "like there is a fixed edge"). Troughs cannot wet anything.
// WolfWaterClearance::softCap same.
const float WOLF_TROUGH_KEEP = 0.35;
float wolfClearCap(float dz, float up, float calmRef)
{
    if (dz >= 0.0)
    {
        return wolfSoftCap(dz, up);
    }
    return dz * mix(WOLF_TROUGH_KEEP, 1.0, clamp(up / max(calmRef, 0.01), 0.0, 1.0));
}
const float WOLF_RUNUP_CAP = 0.5;   // WolfWaterClearance::RUNUP_CAP_M: the swash band's ceiling over the water level
// [OPEN SEA SHORES 2026-10-26] How far this point is held per pixel (0..1): inside the hull of a terrain block's texel
// centres by more than its dead zone + 32 m (so its whole displaced triangle is in the block waterF.glsl reads), blended in
// over 32 m more; the best of the two blocks. WolfWaterClearance::pixelHeld same.
float wolfPixelHeld(vec2 p, float dead)
{
    float held = 0.0;
#ifdef WOLF_WATER_FULL
    for (int b = 3; b < 5; ++b)
    {
        vec4 L = wolfClearLevel[b];
        if (L.z <= 0.0)
        {
            continue;
        }
        vec2 g = (p - L.xy) / L.z;
        float inner = min(min(g.x - 0.5, g.y - 0.5), min(WOLF_CLEAR_N - 0.5 - g.x, WOLF_CLEAR_N - 0.5 - g.y)) * L.z;
        held = max(held, smoothstep(dead + 32.0, dead + 64.0, inner));
    }
#endif
    return held;
}
// [DRY BUILDS 2026-10-26] ---------------------------------------------------------------------------------------------

// Analytic slope of that wave's z component: dz/dxy = d * A * k * cos(f). MUST be called
// with the same arguments as its matching gerstnerWave() or the lighting drifts off the
// geometry it is supposed to describe.
vec2 gerstnerSlope(vec2 pos, float wavelength, float jit, float amp, vec2 dir)
{
    float k = 6.28318 / (wavelength * jit);
    vec2 d = normalize(dir);
    // <WolfViewer 2026-09-20> wrap the phase before sin/cos: 50,000 m from the region origin it
    // is ~10^5 rad, beyond where GPU sin() precision is specified (Intel and the D3D path lose
    // it first). mod() in float32 keeps ~1 mrad; identical below a few hundred metres.
    float f = mod(k * dot(d, pos) - wolfSwellPhaseT(wavelength), 6.2831853);  // [SMOOTH SEA 2026-10-28/] as gerstnerWave
    return d * (amp * k * cos(f));
}
// </FS:WolfViewer> -------------------------------------------------------------------


// [HURRICANE 2026-10-30] Paul: "hurricanes should affect the sea" (ticket #367). The factor on the drawn swell and wind-sea
// amplitude at the agent-space point p: README "The sea", viewers per point (region-terrain-work/hurricane-2026-10-29/README.md);
// CPU twin wolfhurricane.cpp windAt / seaIndexAt / seaAmplitudeFactor - keep in step.
//   U_sea(p) = |v(p')| (README "Wind", track term included); p' = p where r >= Rm, else c + Rm (p - c) / r (the eyewall's wind at
//              the same bearing); at r < 1 m the bearing heading + 90 h degrees
//   S_storm  = clamp((U_sea - 3) / 3, 0.15, 3); S_local = clamp(max(S_storm, (1 - W env(r_c)) S_eep, 0.15), 0.15, 3)
//   factor   = (S_local / S_eep)^1.5 (wolfseastate.h amplitude = 0.22 index^1.5)
// Also returns S_storm(p) (sStorm) and env(r) at p itself (envP) for the storm swell train (README "The storm swell's length").
// [WILD SEA 2026-10-30] README "The wild sea": the DRAWN index runs on to WOLF_STORM_SEA_MAX (S_storm itself, for the swell train,
// stays capped at 3); also returns U_sea and its direction (seaWind) for the fragment's foam.
const float WOLF_STORM_SEA_MAX = 4.5;   // README "The wild sea" (CONVENTION; held by the steepness guard below)
float wolfStormSeaFactor(vec2 p, out float sStorm, out float envP, out vec3 seaWind)
{
    vec2 c = wolfSeaStormEye.xy;
    float R = wolfSeaStormEye.z;
    float Rm = 1.25 * wolfSeaStormEye.w;   // README: Rm = 1.25 Re
    vec2 d = p - c;
    float r = length(d);
    envP = 1.0 - smoothstep(0.0, 1.0, (r / R - 1.0) / 0.3);   // env(r) at p (README "Geometry")
    if (r < Rm)
    {
        vec2 u = r >= 1.0 ? d / r : wolfSeaStormMisc.zw;
        p = c + Rm * u;
        d = p - c;
        r = length(d);
    }
    // README "Wind": V = vmax sqrt(x exp(1 - x)), x = (Rm / max(r, 0.01))^1.31
    float x = pow(Rm / max(r, 0.01), 1.31);
    float V = wolfSeaStormMisc.x * sqrt(x * exp(1.0 - x));
    vec2 vs = vec2(0.0);
    if (r > 0.01)
    {
        vec2 th = wolfSeaStorm.z * vec2(-d.y, d.x) / r;   // t^ = h (-dy, dx) / r
        vec2 rh = d / r;
        vs = V * (0.9232102171129809 * th - 0.38429532265980376 * rh);   // cos / sin 22.6 deg
    }
    vs += wolfSeaStormWind.xy * (Rm * r / (Rm * Rm + r * r));        // Jelesnianski (1966) track term
    float env = 1.0 - smoothstep(0.0, 1.0, (r / R - 1.0) / 0.3);
    float k = wolfSeaStorm.y * env;                                  // k = W env(r)
    vec2 v = (1.0 - k) * wolfSeaStormWind.zw + k * vs;
    float U = min(length(v), 75.0);                                  // |v| capped at 75 m/s
    float S = clamp((U - 3.0) / 3.0, 0.15, 3.0);
    sStorm = S;
    seaWind = vec3(U, U > 1e-3 ? v / length(v) : vec2(0.0));
    float Sd = clamp((U - 3.0) / 3.0, 0.15, WOLF_STORM_SEA_MAX);       // [WILD SEA 2026-10-30/] the drawn index, to 4.5
    float Sl = clamp(max(max(Sd, wolfSeaStorm.w), 0.15), 0.15, WOLF_STORM_SEA_MAX);   // [ROUND 10] w = S_eep: the storm only adds
    return pow(Sl / max(wolfSeaStormMisc.y, 0.15), 1.5);
}

void main()
{
    // <FS:WolfViewer> DISPLACE FIRST, then let every stock calculation below run on the
    // displaced point. The order matters: stock derives the eye vector, the projected
    // position, the reflection coordinate and the atmospheric sample from `position`, and
    // a vertex that has moved must be seen to have moved by all of them, or the water
    // shades as if it were still flat.
    //
    // The wave field is keyed on AGENT-space XY — the same space `position` and `eyeVec`
    // are already in — so neighbouring regions' water objects, which all live in that one
    // space, join up seamlessly across region borders. DECLARED LIMIT: agent space shifts
    // when the agent crosses into a new region, so the swell re-phases at that moment. It
    // is not visible in practice because the crossing redraws the whole scene anyway, and
    // the alternative (global coordinates) puts numbers in the millions through sin/cos,
    // where float precision falls apart into visible banding.
    // Surface basis. surf_n is the plane's own up; surf_t/surf_b span the plane. Waves are
    // displaced along this basis rather than along world XYZ, which is what lets a tilted
    // surface roll along itself instead of being sheared vertically. cross(+Y, +Z) = +X, so
    // an unrotated surface gets the stock tangent exactly; the fallback covers a surface
    // standing on edge, where +Y is no longer a usable reference.
    vec3 surf_n = normalize(normal);
    vec3 surf_ref = (abs(surf_n.y) > 0.99) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 surf_t = normalize(cross(surf_ref, surf_n));
    vec3 surf_b = cross(surf_n, surf_t);

    vec3 wave_pos = position.xyz;
    vec2 wave_slope = vec2(0.0);
    float wave_h = 0.0;

    // <WolfViewer> Ride the terrain that is actually drawn. The terrain picks a render
    // stride from its patch's distance (trunc(distance * stride_per_metre), floored to a
    // power of two, at most 16 modelled here); a coarse stride spans across a gully ABOVE
    // the fine heights this water was fitted to. The vertex's own distance is never less
    // than its patch's, so the stride found here is never smaller than the one drawn —
    // water may sit a little high at distance, it is never buried. Stride 4 and 16 are
    // baked; stride 1 IS the fine surface the water was fitted to (lift 0), 2 follows
    // the parabolic growth of a chord above a curve, (s*s - 1) / 15 being 0 at 1 and 1
    // at 4; 8 sits between the two baked values.
    if (wolfTerrainLod > 0.0)
    {
        float lod_dist = length(position.xyz - eyeVec);
        float stride = floor(lod_dist * wolfTerrainLod);
        stride = clamp(exp2(floor(log2(max(stride, 1.0)))), 1.0, 16.0);
        float lift = (stride <= 4.0)
            ? texcoord1.x * (stride * stride - 1.0) / 15.0
            : mix(texcoord1.x, texcoord1.y, log2(stride / 4.0) / 2.0);
        wave_pos.z += lift;
    }
    // </WolfViewer>
    vec3 base_pos = wave_pos;   // [DRY BUILDS 2026-10-26/] the undisplaced surface point the caps below measure from

    // <WolfViewer 2026-09-06> Region-space position for the baked fields, and the
    // open-water exposure at this vertex: open sea rolls at 1.3x, water hugging any shore
    // or river bank calms to 0.15x (Water.js: "waves bigger out to sea, much smaller at
    // the shore, and rivers should not have waves"). Outside the baked span, or before
    // the first bake, water counts as open sea; a 2% band inside the span edge blends to
    // that so nothing pops as the camera moves.
    vec2 regionXY = position.xy - wolfRegionOrigin;
    float swellExpo = 1.0;
    if (exposureReady > 0.5)
    {
        vec2 xuv = (regionXY - exposureOrigin) / exposureSize;
        if (xuv.x >= 0.0 && xuv.x <= 1.0 && xuv.y >= 0.0 && xuv.y <= 1.0)
        {
            vec2 xf = smoothstep(vec2(0.0), vec2(0.02), xuv)
                    * (vec2(1.0) - smoothstep(vec2(0.98), vec2(1.0), xuv));
            swellExpo = mix(1.0, WOLF_TEX_WOLF_EXPOSURE_FIELD( xuv).r, xf.x * xf.y);
        }
    }
    float swellScale = mix(0.15, 1.3, swellExpo);
    // [WAVES 2026-09-07] The zone painted for this water (About Land > Waves,
    // wolfwavezones.cpp); same 2% edge band as the exposure fetch.
    // [WAVES 2026-09-10] zoneScale is the ONE mapping from the painted energy to a swell
    // scale — WolfWaveZones::zoneScale (the boat rocker) and WolfStorm Water.js /
    // wave_zones.js carry the same numbers, keep them in step. Off (0) -> 0: FLAT, no swell,
    // no chop, no breakers, no swash (Paul: "NO waves inside the region" unless painted —
    // every region defaults to off). Calm (0.15) -> the designer's calm ripple as a fraction
    // of the swell. Small (0.35) -> smallScale of the open sea. Open (0.55) and surf (1) ->
    // 1x; the surf cells' big waves are the SURF TRAIN below, not a louder swell.
    float zoneEnergy = 0.55;
    float zoneScale = 1.0;
    float shoreGate = 1.0;
    // <WolfViewer 2026-10-01> the surf weight (zone field G: the surf cells grown and blurred by a
    // quarter of the surf wavelength, wolfwaterfield.cpp bake). Water.js surfW same.
    float surfW = 0.0;
    if (zoneReady > 0.5)
    {
        vec2 zuv = (regionXY - zoneOrigin) / zoneSize;
        if (zuv.x >= 0.0 && zuv.x <= 1.0 && zuv.y >= 0.0 && zuv.y <= 1.0)
        {
            vec2 zf = smoothstep(vec2(0.0), vec2(0.02), zuv)
                    * (vec2(1.0) - smoothstep(vec2(0.98), vec2(1.0), zuv));
            vec4 ztex = WOLF_TEX_WOLF_ZONE_FIELD( zuv);
            zoneEnergy = mix(0.55, ztex.r, zf.x * zf.y);
            surfW = ztex.g * zf.x * zf.y;
            float calmFloor = clamp(calmRipple / max(waveAmplitude, 0.02), 0.0, 1.0);
            float small = clamp(smallScale, calmFloor, 1.0);
            if (zoneEnergy < 0.15)      zoneScale = calmFloor * max(zoneEnergy, 0.0) / 0.15;
            else if (zoneEnergy < 0.35) zoneScale = mix(calmFloor, small, (zoneEnergy - 0.15) / 0.20);
            else if (zoneEnergy < 0.55) zoneScale = mix(small, 1.0, (zoneEnergy - 0.35) / 0.20);
            else                        zoneScale = 1.0;
            swellScale *= zoneScale;
            // Shore breakers and the swash foam belong to the open sea and the surf: an
            // enclosed lake or river (small / calm cells) has waves with direction but never
            // goes white (Paul 09-10). 0 at small (0.35), 1 at open (0.55).
            shoreGate = smoothstep(0.35, 0.55, zoneEnergy);
        }
    }
    // With the spectral cascades on, the short Gerstner trains (4-6) and the fbm chop are
    // the same wavelengths the spectrum supplies with far more variety: they fade out as
    // the cascades come in rather than doubling up. Bounded / stream surfaces never get
    // the cascades, so they keep the full Gerstner field.
    bool cascades = fftReady > 0.5 && boundedWaterDepth <= 0.0 && wolfStream <= 0.0 && wolfWaterfall <= 0.0;
    float chopKeep = cascades ? 0.0 : 1.0;
    // .w = shoreGate: the fragment gates the swash line with it (open sea and surf only).
    // [OPEN SEA SHORES 2026-10-26] The clearance field once, here: its open-sea weight decides which shores get the
    // breakers, the automatic surf and the swash (Paul: the outer edge of the region, "NOT RIVERS INSIDE or LAKES
    // INSIDE"); its caps are applied last, below. Not ready: no open sea anywhere, and (below) no swell at all.
    vec4 wclear = WOLF_CLEAR_READY > 0.5 ? wolfClearSample(position.xy) : vec4(-1e9, 0.0, -1e9, 0.0);
    float openW = clamp(wclear.w, 0.0, 1.0);
    shoreGate *= openW;
    vSwell = vec4(0.0, 1.0, swellScale, shoreGate);
    vShore = vec4(0.0);
    // [HURRICANE 2026-10-30] the storm's sea at this point: x the swell, its backstop and the cascades below. It comes BEFORE
    // the depth limit and the waterline clearance caps further down, which still hold every crest off land, decks and
    // builds (NO WATER ON LAND). 1 with no storm.
    float stormSea = 1.0;
    // [ROUND 10] README "The storm swell": its amplitude here before the water's own fades (wolfStormSwellAmp), 0 with no storm
    float stormSwellA = 0.0;
    vStormSwellAmp = 0.0;
    vStormSea = vec4(0.0);
    if (wolfSeaStorm.x > 0.5)
    {
        float sStorm = 0.0;
        float envP = 0.0;
        vec3 seaWind = vec3(0.0);
        stormSea = wolfStormSeaFactor(position.xy, sStorm, envP, seaWind);
        stormSwellA = wolfSeaStormSwell.z > 0.5 ? wolfStormSwellAmp(position.xy) : 0.0;
        // [WILD SEA 2026-10-30] README "The wild sea": THE STEEPNESS GUARD. The storm may raise the drawn amplitude only until the
        // steepest train ([ROUND 10] below; was the dominant one) reaches k a = WOLF_STORM_STEEP_MAX, two
        // thirds of the steepest wave that can exist (Stokes' limit H / L = 0.142, Michell 1893: k a = pi x 0.142 = 0.446); never
        // below 1 (the region's own sea is never calmed by this). wolfboatrock.cpp and WolfStorm Water.js use the same rule.
        const float WOLF_STORM_STEEP_MAX = 0.3;   // CONVENTION (README "The wild sea")
        // [ROUND 10] the guard holds the STEEPEST of the eight trains, not the dominant one: their k a = 2 pi (f + 0.001) A x
        // (amplitude share / wavelength multiple), largest for w6 at full chaos, (0.08 + 0.12) / 0.3 = 0.667 (w1: 0.4 / 2 = 0.2,
        // w4 0.3 / 0.8, w5 0.15 / 0.5, w8 0.1 / 0.4 - the lines below). The dominant train alone let the short ones reach k a ~1
        // on a calm region sea raised by a storm (x ~100): the "jagged shards" a test render showed at SandBox South, 14:58Z.
        float kaDom = 6.28318530 * (waveFrequency + 0.001) * waveAmplitude * 0.6667;
        if (kaDom > 1e-6) stormSea = min(stormSea, max(1.0, WOLF_STORM_STEEP_MAX / kaDom));
        vStormSea = vec4(clamp(wolfSeaStorm.y * envP, 0.0, 1.0), seaWind);
    }
    vStormGain = stormSea;
    vWake = 0.0;
    // </WolfViewer>

    if (waveAmplitude > 0.001)
    {
        vec2 wxy = position.xy;

        // Fade displacement out with distance: it keeps far water from shimmering into
        // aliasing. The void planes carry a distance-graded lattice now (LLVOWater::
        // updateGeometry), so the swell reaches toward the horizon before it fades.
        float wave_dist = length(wxy - eyeVec.xy);
        float fade = 1.0 - smoothstep(waveFade.x, waveFade.y, wave_dist);

        if (fade > 0.0)
        {
            const float numWaves = 8.0;

            // Per-patch variation, so the sea is not one repeating corrugation; the chaos
            // term (sea state above the default) adds a third octave and roughens it.
            // Source: Water.js vertex stage — same numbers.
            float noiseScale = 0.008;
            float timeVary = time * 0.02;
            float noise1 = snoise(wxy * noiseScale + timeVary);
            float noise2 = snoise(wxy * noiseScale * 0.3 + timeVary * 0.5) * stormChaos;
            float noise3 = snoise(wxy * noiseScale * 4.0 + timeVary * 2.0) * stormChaos * 0.5;
            float localVariation = 0.5 + 0.3 * noise1 + 0.15 * noise2 + 0.1 * noise3;
            localVariation = mix(localVariation, localVariation * (0.6 + 0.8 * abs(noise1)), stormChaos);
            float localAmp = waveAmplitude * localVariation * fade * swellScale * stormSea;   // [HURRICANE 2026-10-30/] x stormSea

            float baseWavelength = 1.0 / (waveFrequency + 0.001);

            // <WolfViewer 2026-09-26> The swell converges on the region's centre: phased on the
            // position RELATIVE to the centre with d = the unit vector toward it, dot(d, rel) is
            // minus the distance, so the crests are rings rolling inward. Each region has its own
            // rings, which do not line up with the next region's, so the swell calms to nothing
            // within SWELL_EDGE_CALM m of every region edge (inside and in the open-sea planes
            // round it, so both sides of a border are flat and nothing tears), and within two
            // wavelengths of the centre, where the rings would pile into a point.
            // wolfboatrock.cpp sampleWave mirrors this term for term; waterF.glsl
            // swellFoldFraction uses the same directions and phase.
            vec2 swellPos = wxy;
            vec2 swellDir = normalize(waveDir1);
            float edgeK = 1.0;   // [ROUND 10] the region-edge calm alone (the storm swell takes it, not the centre calm)
            if (wolfSwellHalf > 0.0)
            {
                const float SWELL_EDGE_CALM = 32.0;
                vec2 rel = wxy - wolfSwellCentre;
                float r = length(rel);
                vec2 q = abs(rel) - vec2(wolfSwellHalf);
                float toEdge = (q.x < 0.0 && q.y < 0.0) ? min(-q.x, -q.y) : length(max(q, vec2(0.0)));
                edgeK = smoothstep(0.0, SWELL_EDGE_CALM, toEdge);
                localAmp *= edgeK * smoothstep(0.0, 4.0 * baseWavelength, r);
                if (r > 0.5)
                {
                    swellDir = -rel / r;
                }
                swellPos = rel;
            }
            float wavelengthJitter = 1.0 + stormChaos * 0.3 * snoise(wxy * 0.001 + time * 0.01);
            vSwell.xy = vec2(localAmp, wavelengthJitter);

            // Primary swell direction is the region's own EEP wave direction, so a region
            // that sets its water rolling one way gets its geometry rolling that way too.
            vec2 dir1 = swellDir;
            // +35 degrees and -60 degrees off the primary: a real sea is several trains
            // crossing, not one.
            vec2 dir2 = vec2(dir1.x * 0.819 - dir1.y * 0.574,
                             dir1.x * 0.574 + dir1.y * 0.819);
            vec2 dir3 = vec2(dir1.x * 0.5 + dir1.y * 0.866,
                            -dir1.x * 0.866 + dir1.y * 0.5);
            vec2 dir5 = vec2(-dir1.x * 0.5 - dir1.y * 0.866,
                              dir1.x * 0.866 - dir1.y * 0.5);

            vec3 w1 = gerstnerWave(swellPos, baseWavelength * 2.0, wavelengthJitter, localAmp * 0.40, dir1, 0.65, numWaves);
            vec3 w2 = gerstnerWave(swellPos, baseWavelength * 1.5, wavelengthJitter, localAmp * 0.30, dir2, 0.55, numWaves);
            vec3 w3 = gerstnerWave(swellPos, baseWavelength * 1.2, 1.0,              localAmp * 0.25, dir3, 0.50, numWaves);
            float oppositeAmp = localAmp * (0.15 + stormChaos * 0.15) * chopKeep;
            vec3 w4 = gerstnerWave(swellPos, baseWavelength * 0.8, 1.0,              oppositeAmp, -dir1 * 0.7 + dir2 * 0.3, 0.45, numWaves);
            vec3 w5 = gerstnerWave(swellPos, baseWavelength * 0.5, 1.0,              localAmp * 0.15 * chopKeep, dir5, 0.35, numWaves);
            float chopAmp = localAmp * (0.08 + stormChaos * 0.12) * chopKeep;
            vec3 w6 = gerstnerWave(swellPos, baseWavelength * 0.3, 1.0,              chopAmp, dir3 * 0.6 - dir2 * 0.4, 0.25, numWaves);
            vec3 w7 = vec3(0.0);
            vec3 w8 = vec3(0.0);
            if (stormChaos > 0.1)
            {
                float randAngle1 = snoise(wxy * 0.002) * 3.14159;
                w7 = gerstnerWave(wxy, baseWavelength * 0.7, 1.0, localAmp * 0.12 * stormChaos, vec2(cos(randAngle1), sin(randAngle1)), 0.3, numWaves);
                float randAngle2 = snoise(wxy * 0.003 + 100.0) * 3.14159;
                w8 = gerstnerWave(wxy, baseWavelength * 0.4, 1.0, localAmp * 0.1 * stormChaos, vec2(cos(randAngle2), sin(randAngle2)), 0.25, numWaves);
            }

            // [ROUND 10] THE STORM SWELL (README "The storm swell"; Paul: "with a massive hurricane the sea should be HUGE", "the
            // waves we can do up to 30 metres"): ONE train of fixed length L_STORM (300 m: a Gerstner length cannot vary per
            // point without breaking its crests) running OUT FROM THE EYE in rings over the whole region - phase k |p - c|, the
            // direction (p - c) / |p - c| - amplitude a = wolfStormSwellAmp(p) x the water's own distance fade, exposure (calm
            // in harbours, rivers and by the shore: NO WATER ON LAND, the clearance caps below still apply last) and region-edge
            // calm. In time by its own clock (wolfSeaClock.w). Its sideways sway held to its own height (steepness <= 8 k a, at
            // most w1's 0.65), the deep-water orbit's radius.
            vec3 wS = vec3(0.0);
            vec2 relS = wxy - wolfSeaStormEye.xy;
            float rS = length(relS);
            vec2 dirS = rS > 1.0 ? relS / rS : vec2(wolfSeaStormMisc.z, wolfSeaStormMisc.w);
            if (stormSwellA > 0.0)
            {
                float aS = stormSwellA * fade * swellScale * edgeK;
                float kS = 6.28318 / wolfSeaStormSwell.w;
                wS = gerstnerWaveAt(relS, wolfSeaStormSwell.w, aS, dirS, min(0.65, numWaves * kS * aS), numWaves, wolfSeaClock.w);
                vStormSwellAmp = aS;
            }
            vec3 total = w1 + w2 + w3 + w4 + w5 + w6 + w7 + w8 + wS;
            total.z += fbm(wxy * 0.02, time) * localAmp * (0.1 + stormChaos * 0.1) * chopKeep;

            // Slopes of the three dominant trains, same arguments as their waves above.
            wave_slope = gerstnerSlope(swellPos, baseWavelength * 2.0, wavelengthJitter, localAmp * 0.40, dir1)
                       + gerstnerSlope(swellPos, baseWavelength * 1.5, wavelengthJitter, localAmp * 0.30, dir2)
                       + gerstnerSlope(swellPos, baseWavelength * 1.2, 1.0,              localAmp * 0.25, dir3);
            // [HURRICANE 2026-10-30] + the storm swell train's slope (same arguments as its wave above)
            if (vStormSwellAmp > 0.0)
            {
                wave_slope += gerstnerSlopeAt(relS, wolfSeaStormSwell.w, vStormSwellAmp, dirS, wolfSeaClock.w);
            }

            // Backstop, not the height (2.5x the amplitude from lldrawpoolwater.cpp).
            if (maxWaveHeight > 0.0)
            {
                // [HURRICANE 2026-10-30] 2.5x the LOCAL amplitude: the backstop scales with the storm's sea at this point
                // + the storm swell train's own height (README: up to 1.12 x the drawn wave height in the core)
                float backstop = maxWaveHeight * stormSea + vStormSwellAmp;
                total.z = clamp(total.z, -backstop, backstop);
            }

            wave_pos += total.x * surf_t + total.y * surf_b + total.z * surf_n;
            wave_h = total.z;
        }
    }

    // <WolfViewer 2026-09-06> Wind-sea displacement from the spectral cascades, on top of
    // the swell. Keyed on agent XY like the swell, so neighbouring regions' water joins up;
    // scaled by the exposure like the swell; faded per cascade — displacing far vertices
    // by a 12 m chop is aliasing, the fragment keeps the NORMALS out much further.
    // Source: Water.js vertex stage FFT block.
    if (cascades)
    {
        float fdist = length(position.xy - eyeVec.xy);
        float f0 = 1.0 - smoothstep(fftFade.x * 0.5, fftFade.x, fdist);
        float f1 = 1.0 - smoothstep(fftFade.y * 0.5, fftFade.y, fdist);
        vec3 dd = vec3(0.0);
        if (f0 > 0.001) dd += WOLF_TEX_FFT_DISP_0( position.xy / fftTile.x).xyz * f0;
        if (f1 > 0.001) dd += WOLF_TEX_FFT_DISP_1( position.xy / fftTile.y).xyz * f1;
        // [ROUND 10] the cascades keep the region's own spectrum: their choppy displacement was tuned not to fold at the sea state
        // they are built for, and x the storm's factor (to ~100 over a calm sea) it folded into the "jagged shards" (SandBox South
        // test render 14:58Z). The storm's sea is the guarded trains above and the storm swell.
        dd *= swellScale;
        wave_pos += dd.x * surf_t + dd.y * surf_b + dd.z * surf_n;
        wave_h += dd.z;
    }

    // GEOMETRIC shore breakers (Water.js [SWELL 2026-08-15]): the baked depth field's
    // smoothed height gives a stable phase, sin(w t + 8 sqrt(d)) travels beachward with
    // shallow-water celerity so crests slow and bunch as the water shallows, and
    // sharpened crests rise as they shoal. Faded out over a band INSIDE the region edge
    // so camera motion can never pop a vertex across a hard boundary (the [FLASH FIX
    // 2026-08-21] lesson). Fed by the exposure ~45 m seaward: a real coast keeps its
    // set, a river bank (no fetch anywhere) drops to 0.35x.
    if (depthReady > 0.5 && shoreWavesEnabled > 0.5 && boundedWaterDepth <= 0.0)
    {
        vec2 sduv = (regionXY - depthOrigin) / depthRegionSize;
        if (sduv.x >= 0.0 && sduv.x <= 1.0 && sduv.y >= 0.0 && sduv.y <= 1.0)
        {
            vec2 ef = smoothstep(vec2(0.0), vec2(0.04), sduv)
                    * (vec2(1.0) - smoothstep(vec2(0.96), vec2(1.0), sduv));
            float edgeFade = ef.x * ef.y;
            vec4 dtex = WOLF_TEX_WOLF_DEPTH_FIELD( sduv);
            float conf = length(dtex.gb);
            if (conf > 0.02)
            {
                float smoothDepth = max(depthWaterLevel - dtex.a, 0.0);
                // [WAVES 2026-09-10] ... times the shore gate: breakers on the open sea and the surf only.
                float shoal = (1.0 - smoothstep(0.5, 8.0, smoothDepth)) * min(conf * 1.5, 1.0) * edgeFade * shoreGate;
                if (shoal > 0.01)
                {
                    // [SMOOTH SEA 2026-10-28] wolfSeaClock.y is time x 4.5 x clamp(|waveDir1| x 0.885, 0.4, 2.0), accumulated
                    // (lldrawpoolwater.cpp wolfUpdateSea), in place of that product
                    float phase = wolfSeaClock.y + sqrt(smoothDepth) * 8.0;
                    vShore.xy = vec2(phase, shoal);
                    float feed = 1.0;
                    if (exposureReady > 0.5)
                    {
                        vec2 fuv = (regionXY - dtex.gb * (45.0 / conf) - exposureOrigin) / exposureSize;
                        feed = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(fuv, 0.0, 1.0)).r;
                    }
                    float bAmp = min(0.10 + waveAmplitude * 0.9, 0.5) * shoal * (0.35 + 0.65 * sqrt(feed));
                    // [OPEN SEA SHORES 2026-10-26] Sets: the breakers come in groups, each a different height, so not
                    // every wave reaches the same place - the automatic surf's set clock, offset along the coast.
                    // wolfboatrock.cpp same.
                    float bSet = 6.2831853 * time / max(wolfAutoSurf.y, 10.0) + dot(regionXY, vec2(0.013, 0.017));
                    bAmp *= 0.45 + 0.55 * smoothstep(0.1, 1.0, 0.5 + 0.5 * sin(bSet));
                    // [HARBOUR STRIPES 2026-10-28] the breaker's height for the fragment's foam: the lift below runs from
                    // (-1 + 0.175) bAmp at br = -1 to (1 + 0.175) bAmp at br = 1, so crest to trough is 2 bAmp.
                    vShore.z = 2.0 * bAmp;
                    float br = sin(phase);
                    // [DRY BUILDS 2026-10-26] re-centred on the water level: br^2 averages 0.5 over a period, so
                    // (br + 0.35 br^2) stood 0.175 bAmp above it on average. wolfboatrock.cpp same.
                    float lift = (br + 0.35 * (br * br - 0.5)) * bAmp;
                    wave_pos += surf_n * lift;
                    wave_h += lift;
                }
            }
        }
    }

    // [DRY BUILDS 2026-10-26] DEPTH-LIMITED HEIGHT. A wave in water h deep breaks once it is higher than about 0.78 h
    // (McCowan's breaker index; Bosboom & Stive, Coastal Dynamics 5.2.5), so no crest stands more than 0.39 h above the
    // mean: the swell, the cascades and the breakers die down to meet the beach instead of climbing it. The surf train
    // below keeps its own reef-slope depth and McCowan limit (Paul 10-01: "if i specify 30m waves they should start at
    // 30m"). Region water with a depth field only; wolfboatrock.cpp same.
    if (depthReady > 0.5 && boundedWaterDepth <= 0.0 && wolfStream <= 0.0 && wolfWaterfall <= 0.0)
    {
        vec2 lduv = (regionXY - depthOrigin) / depthRegionSize;
        if (lduv.x >= 0.0 && lduv.x <= 1.0 && lduv.y >= 0.0 && lduv.y <= 1.0)
        {
            float capH = 0.39 * max(depthWaterLevel - WOLF_TEX_WOLF_DEPTH_FIELD( lduv).r, 0.0);
            float dn0 = dot(wave_pos - base_pos, surf_n);
            float dn1 = wolfSoftCap(dn0, capH);
            float sech = 1.0 / cosh(clamp(dn0 / max(capH, 1e-4), -10.0, 10.0));
            wave_pos += surf_n * (dn1 - dn0);
            wave_h = wolfSoftCap(wave_h, capH);
            wave_slope *= capH > 0.0 ? sech * sech : 0.0;
        }
    }

    // ============================================================
    // [SURF 2026-09-07] THE SURF TRAIN — Water.js vertex stage, same math, keep in step.
    // A long wave rolling toward the land through the SURF cells painted in About Land >
    // Waves: finite-depth dispersion, crests bunching in the shallows, shoaling growth, the
    // McCowan limit H <= 0.78 h where the crest narrows and leans forward and the fragment
    // lays foam on it, and a dissipation ramp to ZERO height by the waterline.
    // ============================================================
    vSurf = vec4(0.0);
    // [OPEN SEA SHORES 2026-10-26] What the swash below needs from the surf train: its phase at this point, the set
    // envelope, the height at the break, the deep-water wavelength and the beach slope. swHave = 0: no swash here.
    float swHave = 0.0;
    float swPhase = 0.0;
    float swSetEnv = 1.0;
    float swH = 0.0;
    float swLambda = 36.0;
    float swSlope = 0.05;
    // [OPEN SEA SHORES 2026-10-26] THE AUTOMATIC SURF. Paul 2026-10-26: open-sea shores should behave like a coast -
    // waves shoal, break with moving foam, run up and wash back, in sets. Where no surf is painted, every open-sea shore
    // gets this surf train with the sea state's own height and wavelength (wolfAutoSurf, lldrawpoolwater.cpp), weighted
    // by the open-sea weight. It runs on the REAL depth (no reef slope: an unpainted beach breaks where its own water
    // says, McCowan below), and as the baked optical path belongs to the painted surf's wavelength, its phase is the
    // open-sea distance plus the shoaling term below. Painted surf is untouched.
    // [TICKET 271 2026-10-27] ...and by the painted zone, as the breakers and the swash foam are (shoreGate = the open-sea
    // weight x smoothstep(0.35, 0.55, zone energy), above): Contessa Lacombe painted her bay Calm and then Off and still got
    // full-height automatic surf, because this gate read the open-sea weight alone. Off and Calm cells get none, Small cells
    // none, Open cells all of it - the same rule the 09-10 shore foam follows (Paul: "an enclosed lake never goes white").
    bool autoSurfOn = wolfAutoSurf.w > 0.5 && wolfAutoSurf.x > 0.01 && waveAmplitude > 0.001 && shoreGate > 0.01;
    if ((surfHeight > 0.01 || autoSurfOn) && shoreWavesEnabled > 0.5 && boundedWaterDepth <= 0.0 && exposureReady > 0.5)
    {
        // Surf ONLY where a designer painted it (Paul 09-07: "surf is a special thing"). Water.js same.
        // <WolfViewer 2026-10-01> From the surf WEIGHT, not the 3x3 energy: 0.55 + 0.45 w is
        // the energy a surf/open mix of that ratio has. w is 1 on every painted cell and fades
        // over half a wavelength outside the paint (Paul 10-01: 20 m surf on a 48 m strip was
        // jagged walls; "if i specify 30m waves they should start at 30m").
        float surfZone = (surfHeight > 0.01 && zoneReady > 0.5) ? smoothstep(0.62, 0.95, 0.55 + 0.45 * surfW) : 0.0;
        bool autoS = surfZone <= 0.01 && autoSurfOn;
        float sH = autoS ? wolfAutoSurf.x : surfHeight;
        float sSet = autoS ? wolfAutoSurf.y : surfSetInterval;
        float sLen = autoS ? wolfAutoSurf.z : surfLength;
        if (autoS)
        {
            surfZone = shoreGate;   // [TICKET 271 2026-10-27/] was openW: the painted zone gates it too
        }
        vec2 euv = (regionXY - exposureOrigin) / exposureSize;
        if (surfZone > 0.01 && euv.x >= 0.0 && euv.x <= 1.0 && euv.y >= 0.0 && euv.y <= 1.0)
        {
            // [SURF 2026-09-07 rev2] Water.js, same math: the wave's coordinate is the baked
            // DISTANCE TO LAND (exposure G) — crests are iso-distance contours that wrap the
            // coast; direction = down the distance gradient taken 1/32 of the span wide.
            // [SURF 2026-09-20] The direction is read 80 m either side (was 1/32 of the span, 24 m
            // on a 256 m region): differenced that close, the distance field points at the NEAREST
            // bank — sideways along a bay, at a rock, at a pier — and the surf ran along the shore
            // (Paul: "they should always go towards the shore not sideways"). 80 m averages the
            // small features out and points at the coast; the crest coordinate is the same five-tap
            // mean so the crest lines follow the smoothed contours. CPU mirrors: terrain_manager.js
            // _surfSampleCPU / wolfboatrock.cpp use 80 m too.
            // <WolfViewer 2026-09-20> The wave's coordinate is the DISTANCE FROM THE OPEN SEA
            // (exposure bake B): crests parallel to the sea front, travelling LANDWARD, up a bay
            // to its head. Distance to land (G) put crests parallel to the nearest bank and rings
            // round every islet; it stays as the fallback where no open sea is in reach.
            // Source: Water.js [SURF 2026-09-20]; CPU mirror wolfboatrock.cpp.
            // <WolfViewer 2026-09-21> The wave's coordinate is the baked OPTICAL PATH from
            // the open sea (exposure A, wolfwaterfield.cpp): the integral of k along the ray,
            // in units of the deep-water wavelength, so that phase = surfPathK0 * path is the
            // WKB/ray-theory phase of a real wave train. Its gradient IS the ray direction
            // and its magnitude IS the local wavenumber, so refraction round a headland,
            // crest bunching in the shallows and the direction of travel all come out of the
            // one field and cannot disagree with each other.
            // Direction is still differenced 80 m either side: the chamfer sweep that built
            // the path is 8-connected, and over one texel its gradient carries that
            // quantisation. Over 80 m it does not. The PHASE uses the centre tap, not a mean
            // of the five — the field is already smooth, and averaging it would flatten
            // exactly the crest curvature that makes a wave wrap into a bay.
            vec2 dW = vec2(80.0) / exposureSize;
            vec4 eC = WOLF_TEX_WOLF_EXPOSURE_FIELD( euv);
            vec4 eXp = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv + vec2(dW.x, 0.0), 0.0, 1.0));
            vec4 eXm = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv - vec2(dW.x, 0.0), 0.0, 1.0));
            vec4 eYp = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv + vec2(0.0, dW.y), 0.0, 1.0));
            vec4 eYm = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv - vec2(0.0, dW.y), 0.0, 1.0));
            bool havePath = surfPathK0 > 0.0 && eC.b < 3000.0 && !autoS;
            vec2 pgrad = vec2(eXp.a - eXm.a, eYp.a - eYm.a);
            float pl = length(pgrad);
            havePath = havePath && pl > 1.0;
            vec2 ograd = vec2(eXp.b - eXm.b, eYp.b - eYm.b);
            float ol = length(ograd);
            bool haveOpen = eC.b < 3000.0 && ol > 1.0;
            float dist = eC.g;
            vec2 grad = vec2(eXp.g - eXm.g, eYp.g - eYm.g);
            float gl = length(grad);
            bool haveLand = dist < 3000.0 && gl > 1.0;
            vec2 dir = havePath ? pgrad / pl
                     : (haveOpen ? ograd / ol
                     : (haveLand ? -grad / gl : normalize(depthOrigin + depthRegionSize * 0.5 - regionXY + vec2(0.001, 0.0))));
            float openS = (eC.b + eXp.b + eXm.b + eYp.b + eYm.b) * 0.2;   // smoothed contours
            float distS = (dist + eXp.g + eXm.g + eYp.g + eYm.g) * 0.2;
            // Fallbacks (a landlocked lake with surf painted on it, or a field baked before
            // the region's record arrived) travel on the plain geometric distance at the
            // deep-water wavelength: no shoaling, but a clean train rather than a mess.
            float coord = haveOpen ? openS : (haveLand ? -distS : dot(regionXY, dir));
            float path = havePath ? eC.a : coord;
            // [OPEN SEA SHORES 2026-10-26] The automatic surf is the swell meeting the coast: it rises over the last
            // 200 m to land (where the exposure has already calmed the open-sea swell, 150 m down to 6 m), not a second
            // wave train over the whole sea. wolfboatrock.cpp same.
            if (autoS)
            {
                surfZone *= 1.0 - smoothstep(80.0, 200.0, dist);
                // [HARBOUR STRIPES 2026-10-28] SHELTER. The automatic surf is the swell arriving at the coast, so it can be no
                // bigger than the swell drawn where it comes from. It came in along dir (landward), so the swell that feeds
                // it is the one 150 m seaward of here - the far side of the 80-200 m band it rises over. The drawn swell
                // there is mix(0.15, 1.3, exposure) of the sea state (swellScale, above); over the open sea's 1.3 that is
                // 1 on an open coast (the surf exactly as before) and a fraction of it in a harbour or a sound, where the
                // land all round keeps the exposure down. It used to be the open sea's full height everywhere within
                // 200 m of land, so a sheltered harbour got the same regular train of crests as a surf beach, every crest
                // foamed: the white zebra stripes in Jimmy Olsen's TROMSO picture (2026-10-28). Outside the exposure span
                // the sea counts as open, as WolfWaterField::exposureAt does. wolfboatrock.cpp same.
                vec2 fuv = euv - dir * (150.0 / exposureSize);
                float feedExpo = (fuv.x >= 0.0 && fuv.x <= 1.0 && fuv.y >= 0.0 && fuv.y <= 1.0)
                               ? WOLF_TEX_WOLF_EXPOSURE_FIELD( fuv).r : 1.0;
                sH *= mix(0.15, 1.3, feedExpo) / 1.3;
            }
            float h = 30.0;
            if (depthReady > 0.5)
            {
                // Past the region border the depth CONTINUES from the border texel and eases
                // to deep water over 64 m (the void planes share these fields now): a hard
                // switch to 30 m at the edge changed the shoaling and tore the crest.
                vec2 sduv = (regionXY - depthOrigin) / depthRegionSize;
                vec2 over = max(max(-sduv, sduv - 1.0), 0.0) * depthRegionSize;
                float outside = smoothstep(0.0, 64.0, max(over.x, over.y));
                vec4 dt = WOLF_TEX_WOLF_DEPTH_FIELD( clamp(sduv, 0.0, 1.0));
                h = mix(max(depthWaterLevel - dt.a, 0.0), 30.0, outside);
            }
            // <WolfViewer 2026-10-01> The surf's depth: the real depth, or a 1-in-4 reef slope
            // up to the shoreline, whichever is deeper (WolfWaterField::SURF_REEF_SLOPE /
            // surfDepth — the path bake integrates the same depth). Paul 10-01: "if i specify
            // 30m waves they should start at 30m": the region's shallow sea broke a 30 m wave
            // long before the beach; on this slope it keeps its height to 5.1 H out, plunges,
            // and runs in as a bore that shrinks to nothing at the waterline. Water.js same.
            const float SURF_REEF_SLOPE = 0.25;
            float hReal = h;
            if (!autoS)
            {
                h = max(h, SURF_REEF_SLOPE * dist);
            }
            // <WolfViewer 2026-09-21> k0 is the DEEP-water wavenumber and omega0 its
            // deep-water frequency. omega0 is the same number everywhere in the field: in a
            // shoaling wave train the FREQUENCY is what is conserved, the wavelength is what
            // changes. The old code took omega from the LOCAL depth, so deep water and the
            // surf zone ran at frequencies ~0.55 rad/s apart and drifted a whole crest out of
            // step every 11 s — after an hour of uptime they were 300 crests apart, which is
            // the "mess" in Paul's screenshot. k is the local wavenumber (Guo 2002).
            const float g = 9.81;
            float lambda = max(sLen, 12.0 * sH);
            float k0 = (surfPathK0 > 0.0 && !autoS) ? surfPathK0 : 6.2831853 / max(lambda, 8.0);
            // [OPEN SEA SHORES 2026-10-26] The beach slope, along the travel direction, from the smoothed depth field.
            float beachSlope = 0.05;
            if (depthReady > 0.5)
            {
                vec2 sdu = dir * 4.0 / depthRegionSize;
                vec2 suv0 = clamp((regionXY - depthOrigin) / depthRegionSize, 0.0, 1.0);
                float aP = WOLF_TEX_WOLF_DEPTH_FIELD( clamp(suv0 + sdu, 0.0, 1.0)).a;
                float aM = WOLF_TEX_WOLF_DEPTH_FIELD( clamp(suv0 - sdu, 0.0, 1.0)).a;
                beachSlope = clamp(abs(aP - aM) / 8.0, 0.01, 1.0);
            }
            // [OPEN SEA SHORES 2026-10-26] SHOALING FOR THE AUTOMATIC SURF. The phase of a wave train is the integral of
            // its local wavenumber along the ray (WKB; the optical path the painted surf bakes). On a beach of slope s, in
            // the shallow-water limit k = sqrt(k0 / h) (omega^2 = g k0 conserved, k = omega / sqrt(g h)), the extra path
            // past the deep-water one from depth 1 / k0 in to depth h is
            //     (1 / s) * integral_h^(1/k0) ( 1 / sqrt(k0 h') - 1 ) dh' = (1 / s) (1 / k0 - 2 sqrt(h / k0) + h),
            // whose gradient across the beach is exactly sqrt(k0 / h): the crests close up and slow as the water
            // shallows. Constant at the waterline and on the beach, where the swash takes over.
            if (autoS)
            {
                float hs = min(max(hReal, 0.0), 1.0 / k0);
                path += (1.0 / k0 - 2.0 * sqrt(hs / k0) + hs) / beachSlope;
            }
            float omega0 = sqrt(g * k0);
            float k = wolfSurfK(k0, h);
            vec2 across = vec2(-dir.y, dir.x);
            float setPh = 6.2831853 * (time * surfSpeed / max(sSet, 10.0)) - path * (0.22 / lambda);
            float setEnv = 0.30 + 0.70 * smoothstep(0.15, 1.0, 0.5 + 0.5 * sin(setPh));
            float crestVar = 0.85 + 0.15 * sin(dot(regionXY, across) * (1.1 / lambda) + time * 0.1);
            // <WolfViewer 2026-10-01> Floor 1 (was 0.8). Green's law dips to 0.915 at intermediate
            // depth (k h ~ 1.2) and a 360 m wave is intermediate in 100 m of water, so a 30 m surf
            // arrived at 27-28 m. Paul 10-01: "if i specify 30m waves they should start at 30m":
            // the set height is the least a wave carries before it breaks; it still grows as it
            // stands up at the break. Water.js, wolfsurfcurlV.glsl, wolfboatrock.cpp same.
            float ksh = clamp(wolfSurfShoal(k0, k, h), 1.0, 1.8);
            float crestH = min(sH * surfZone * setEnv * ksh * crestVar, sH * 1.15);
            // [OPEN SEA SHORES 2026-10-26] for the swash below
            swHave = 1.0;
            // [SMOOTH SEA 2026-10-28] the automatic surf's omega0 follows the weather (wolfAutoSurf.z): its phase is the
            // accumulated wolfSeaClock.z; the painted surf's omega0 is fixed by its own wavelength and keeps time.
            float surfT = autoS ? wolfSeaClock.z : omega0 * time;
            swPhase = k0 * path - surfT * surfSpeed;
            swSetEnv = setEnv * crestVar;
            swH = sH;
            swLambda = lambda;
            swSlope = beachSlope;
            // <WolfViewer 2026-10-01> McCowan on the surf depth h above (real or reef). It was
            // 0.78 (h + 0.8 surfHeight): 16 m of pretend depth EVERYWHERE at a 20 m surf, right up
            // to the waterline, so 7-16 m crests stood over the bank (Paul 10-01, Wolf
            // Territories Home). The reef depth is 0 at the shore, so the bore runs out there.
            // A crest higher than 0.78 h has broken; it cannot stand. Water.js, wolfsurfcurlV.glsl, wolfboatrock.cpp,
            // surf_curl.js and terrain_manager.js _surfSampleCPU same.
            float Hmax = 0.78 * h;
            float breakF = smoothstep(0.7, 1.15, crestH / max(Hmax, 0.01));
            crestH = min(crestH, Hmax);
            crestH *= smoothstep(0.2, 0.6 + 0.5 * sH, h);
            if (crestH > 0.01)
            {
                // <WolfViewer 2026-09-21> BAND LIMIT — the crest can only be as sharp as the
                // lattice under it. res = vertices this wave gets here, mapped through the
                // boundary Paul's 09-21 screenshot draws for us: at 84 m the sea had 6.2
                // vertices per wave and rolled smoothly, at 310 m it had 1.7 and was a field
                // of one-quad pyramids. So res = 1 at 6 vertices per wave and 0 at 3, and
                // every term that SHARPENS the crest — the forward lean, the cnoidal peaking,
                // the barrel, the Gerstner pinch — and finally the crest height itself fade
                // with it. res = 1 leaves the arithmetic below bit-identical to the pre-band-
                // limit code. [TICKET 271 2026-10-27] The FOAM (vSurf) used to keep the full crest
                // here; per vertex on a coarse lattice it aliased into zig-zags, so it is band-limited
                // too now (foamRes, below).
                // The wavelength measured here is 2 pi / k, the LOCAL one, which is now the
                // wavelength actually drawn: the phase is k0 * path and |grad(k0 path)| = k
                // by construction of the eikonal bake. Under the old k(x) * distance phase it
                // was not — the drawn spacing had a second term, distance * dk/dx, that this
                // test could not see, which is why one-quad spikes survived the first pass.
                // NOT mirrored on the CPU (wolfboatrock.cpp): a boat rides the real wave, and
                // this is a drawing limit, not a change to the sea.
                float vstep = wolfLatticeStep(position.xy);
                // <WolfViewer 2026-10-01> ...and the wavenumber the DRAWN phase actually has: k0 times
                // the path's slope across one exposure texel either side along the travel direction.
                // Where the baked path bends faster than the local depth says (a shore texel, a
                // channel narrower than a texel -- Paul 10-01 "spikey waves near the shore"), the
                // crest is band-limited by what is drawn, not by what the depth predicts. Water.js same.
                float kDraw = k;
                if (havePath)
                {
                    vec2 du = dir / 255.0;   // one texel of the 256-texel exposure bake (wolfwaterfield.h ERES)
                    float aP = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv + du, 0.0, 1.0)).a;
                    float aM = WOLF_TEX_WOLF_EXPOSURE_FIELD( clamp(euv - du, 0.0, 1.0)).a;
                    kDraw = max(k, k0 * abs(aP - aM) / max(2.0 * length(du * exposureSize), 0.01));
                }
                float lamEff = 6.2831853 / max(kDraw, 0.0001);
                float vpw = (vstep > 0.0) ? lamEff / vstep : 64.0;
                float res = smoothstep(3.0, 6.0, vpw);
                // <WolfViewer 2026-10-05> FACE LIMIT. The profile below is scale-free - the same
                // pinched peak, squeezed front and thrown lip at every size, since a wave's length
                // grows with its height (lambda >= 12 H). On a 3 m wave that reads as a breaking
                // wave; on Madrigal's 30 m surf the same shape is a near-vertical face 30 m high,
                // which a displaced grid can only draw as a sheer cliff with stretched streaks
                // (Paul 10-05: "water walls", moving to the shore). Every sharpening term fades to
                // half between 8 and 20 m of crest: a 30 m wave's breaking face drops from ~60 to
                // ~33 degrees (measured on this profile), the height is untouched, and the foam (vSurf) and the curl
                // ribbons (wolfsurfcurlV.glsl, same factor) still show the break. Under 8 m: the
                // arithmetic below is bit-identical. Water.js same.
                float sharp = res * mix(1.0, 0.5, smoothstep(8.0, 20.0, crestH));
                float breakG = breakF * sharp;  // the break as GEOMETRY
                float crestG = crestH * res;    // the crest height the lattice can carry
                float ph = k0 * path - surfT * surfSpeed;   // [SMOOTH SEA 2026-10-28/] surfT above
                float ph2 = ph + (0.30 + 0.45 * breakF) * sharp * sin(ph);
                float sn = sin(ph2), cs = cos(ph2);
                float up = 0.5 + 0.5 * sn;
                // [SURF rev3, Paul 09-07 "looks weird": 20 m crests were 240 m plateaus with
                // cliff faces] A shoaling wave is CNOIDAL: a narrow peaked crest and a long flat
                // trough. upk peaks the crest (exponent 1.6 -> 2.8 as it breaks) and every
                // forward term is weighted by it, so only the top leans and throws — a concave
                // face, not a wall. Water.js same.
                float upk = pow(up, 1.6 + 1.2 * breakF);                    // foam shape
                float upkG = pow(up, mix(1.0, 1.6 + 1.2 * breakF, sharp));  // geometry shape
                float prof = mix(-0.25, 1.0, upkG) + 0.25 * breakG * upkG * upkG;
                float tip = upkG * upkG * upkG;
                float lip = 0.55 * breakG * tip;   // the crest tip thrown past fold-over: the barrel
                float Q = mix(0.35, 0.9, breakG);
                float horiz = crestG * (0.5 * Q * cs * upkG + 0.35 * breakG * upkG * upkG + lip);
                // <WolfViewer 2026-10-01> FOLD GUARD. A displaced grid cannot draw an overhang:
                // once the forward push changes faster than the wave itself, neighbouring vertices
                // pass each other and the surface folds into jagged triangles (the Gerstner loop
                // condition, Q k A > 1 -- Tessendorf, "Simulating Ocean Water"). Measured from this
                // profile: d(horiz)/d(phase) peaks at crestG * (0.19 / 0.37 / 0.67 / 1.07 / 1.57) for
                // breakF 0 / .25 / .5 / .75 / 1, fitted below. A 30 m surf on a straight coast
                // reached k * crest * D = 0.96 just after the break, and where crests converge in a
                // cove (Paul 10-01: "choppiness near the shore ... two land masses next to each
                // other") over 1. So: hold that product at <= 0.7, with k the larger of the local
                // wavenumber and the one the baked path actually shows over the 160 m baseline
                // (convergence), and fade the push where the path is incoherent -- two trains
                // meeting behind a headland, where the 160 m difference cancels below the local k.
                // The height and profile are untouched; the plunge is the curl ribbons' job.
                // Water.js same.
                {
                    float foldD = 0.2 + 0.45 * breakF + 0.92 * breakF * breakF;
                    float kPh = max(havePath ? max(k, k0 * pl / 160.0) : k, kDraw);   // 10-01: and the drawn phase (kDraw, above)
                    float coh = havePath ? clamp(pl / max(160.0 * k / k0, 1e-3), 0.0, 1.0) : 1.0;
                    horiz *= min(1.0, 0.7 / max(kPh * crestG * foldD, 1e-4)) * smoothstep(0.5, 0.9, coh);
                }
                float lift = crestG * (prof - 0.35 * lip);
                wave_pos += surf_t * (dir.x * horiz) + surf_b * (dir.y * horiz) + surf_n * lift;
                wave_h += crestG * prof;
                wave_slope += dir * (crestG * 0.6 * k * cs * (0.3 + 0.7 * upkG));
                // wash = the churn a broken crest leaves BEHIND it (its back slope, cs > 0)
                float wash = breakF * smoothstep(0.3, 1.0, cs) * smoothstep(0.1, 0.5, up);
                // [TICKET 271 2026-10-27] FOAM BAND LIMIT. The foam is a per-vertex value too, so it can only be as sharp
                // as the lattice under it, like the geometry above. The crest foam (upk, a narrow peak) interpolated over
                // triangles wider than a third of a wave drew saw-tooth white zig-zags and lone white triangles across a
                // whole bay (Contessa Lacombe's Flea Report, New Brigantia 49, seen from the air: "the waves look like
                // strange lines"). It now fades out between 5 and 3 vertices per wave, the band the geometry's res uses;
                // near the camera, where the lattice draws the wave, it is unchanged. Water.js same.
                float foamRes = smoothstep(3.0, 5.0, vpw);
                // [HARBOUR STRIPES 2026-10-28] On the AUTOMATIC surf the crest foam is the BREAKING crest's only (x breakF):
                // a swell crest that has not broken carries no whitewater (the wind's whitecaps are drawn by their own
                // terms), and a wave breaks where it is higher than about 0.78 h (McCowan, above), so the foam stays in the
                // surf zone by the water's edge instead of on every crest within 200 m of land. Painted surf is the
                // designer's (Paul 09-07: "the PEAK of every surf wave carries some") and keeps every crest.
                float crestFoamW = autoS ? breakF : 1.0;
                vSurf = vec4(smoothstep(0.3, 1.0, upk) * foamRes * crestFoamW, breakF, crestH, wash * foamRes);   // peak only
                vShore.w = autoS ? 1.0 : 0.0;
            }
        }
    }

    // [OPEN SEA SHORES 2026-10-26] THE SWASH. Paul 2026-10-26: "a natural swash run-up and backwash ... a moving,
    // irregular water edge that advances and retreats with each wave instead of a fixed line".
    // Run-up height: Hunt (1959), R = xi H with the Iribarren number xi = tan(beta) / sqrt(H / L0) (Battjes 1974), H the
    // wave's height at the break and L0 its deep-water wavelength, capped at WOLF_RUNUP_CAP (Paul: "a small swash band")
    // and scaled by the open-sea weight (no swash on a lake or a river). Each wave of a set carries its own H, so each
    // reaches its own height; an alongshore noise breaks the edge up.
    // Motion: over the band - from just offshore (depth under half the break height) up to a little past the run-up
    // limit - the surface is drawn to the swash level R * eta, eta rising fast as the bore arrives (the uprush, a third
    // of the wave period) and draining slowly (the backwash), on the surf train's own phase. The water edge is where that
    // level meets the beach, so it advances and retreats with the terrain's own shape. waterF.glsl holds the band:
    // nothing over land above the water level + Rcap, and nothing at all over land higher than that.
    float Rcap = 0.0;
    // [TICKET 271 2026-10-27] Weighted by shoreGate (the open-sea weight x the painted zone), not the open-sea weight alone:
    // no swash on an Off / Calm / Small cell, even where painted surf's blurred weight reaches into it.
    if (swHave > 0.5 && depthReady > 0.5 && shoreGate > 0.01)
    {
        vec2 swuv = (regionXY - depthOrigin) / depthRegionSize;
        if (swuv.x >= 0.0 && swuv.x <= 1.0 && swuv.y >= 0.0 && swuv.y <= 1.0)
        {
            float elev = WOLF_TEX_WOLF_DEPTH_FIELD( swuv).r - depthWaterLevel;   // terrain over the water level here
            float L0 = max(swLambda, 1.0);
            float xiMax = swSlope / sqrt(max(swH, 0.01) / L0);
            float Hw = max(swH * swSetEnv, 0.01);
            float xi = swSlope / sqrt(Hw / L0);
            Rcap = min(xiMax * swH, WOLF_RUNUP_CAP) * shoreGate;
            float R = min(xi * Hw, WOLF_RUNUP_CAP) * shoreGate * (0.8 + 0.2 * snoise(regionXY * 0.07 + vec2(time * 0.03, 0.0)));
            R = clamp(R, 0.0, Rcap);
            float u = fract(swPhase / 6.2831853);
            float eta = u < 0.33 ? smoothstep(0.0, 0.33, u) : 1.0 - smoothstep(0.33, 1.0, u);
            float wsw = (1.0 - smoothstep(0.0, 0.5 * swH + 0.1, -elev))
                      * (1.0 - smoothstep(1.2 * Rcap, 1.2 * Rcap + 0.3, elev));
            float dnNow = dot(wave_pos - base_pos, surf_n);
            wave_pos += surf_n * ((mix(dnNow, R * eta, wsw)) - dnNow);
            wave_h = mix(wave_h, R * eta, wsw);
            wave_slope *= 1.0 - wsw;
        }
    }

    // Boat wash displacement (Water.js [WAKE 2026-08-21]): the Kelvin arms lift the
    // surface and the churn astern depresses it (G lift, B dip, differenced at read).
    if (wakeReady > 0.5 && wakeStrength > 0.0)
    {
        vec2 wkuv = (regionXY - wakeOrigin) / wakeRegionSize;   // <WolfViewer 2026-10-06/> the field window
        if (wkuv.x >= 0.0 && wkuv.x <= 1.0 && wkuv.y >= 0.0 && wkuv.y <= 1.0)
        {
            vec4 wk = WOLF_TEX_WAKE_SAMPLER( wkuv);
            vWake = wk.r * wakeStrength;
            wave_pos += surf_n * ((wk.g - wk.b) * 1.7 * wakeStrength);
        }
    }
    // </WolfViewer>

    // [DRY BUILDS 2026-10-26] THE CLEARANCE CAP, last, over everything above (swell, cascades, breakers, surf, swash,
    // wake). Region, hole, edge and neighbour planes; not a wolfwater prim surface (the owner's own water), a stream or
    // a fall.
    //   up   = S - ALPHA * deadZone - plane: the room to rise; the dead zone is what holds the cap between vertices
    //          (wolfwaterfield.h) - 1.5 x the lattice step here (wolfLatticeStep, the triangle's longest edge with 6 %
    //          spare) over 1 - 3 BETA, the vertices' own horizontal travel;
    //   S    = [OPEN SEA SHORES 2026-10-26] S_all (prims and every land texel) where this plane has no depth field to
    //          hold the land per pixel, S_dry (prims and the land of INLAND water only) where it has, blended over 32 m
    //          inside the field's edge (pixelHeld). Prims are in both, so docks and decks are held exactly as before;
    //          an open-sea beach is held per pixel by waterF.glsl instead, so the waves can run up it with no calm band;
    //   hcap = BETA * D: the horizontal push, so no vertex is carried onto land or a deck;
    //   the camera guard: no crest closer than WOLF_CLEAR_EYE under the eye (LLViewerCamera::cameraUnderWater() tests
    //          the flat water level, so a crest over the eye was drawn from below with the above-water shader).
    // Until the field is ready there is no room at all: the sea is flat, never wet.
    if (boundedWaterDepth <= 0.0 && wolfStream <= 0.0 && wolfWaterfall <= 0.0)
    {
        vec3 disp = wave_pos - base_pos;
        float dn = dot(disp, surf_n);
        vec3 dtan = disp - dn * surf_n;
        float up = -1.0;
        float hcap = 0.0;
        if (WOLF_CLEAR_READY > 0.5)
        {
            float lstep = wolfLatticeStep(position.xy);
            float dead = 1.5 * (lstep > 0.0 ? lstep : 128.0) / (1.0 - 3.0 * WOLF_CLEAR_BETA);
            // [OPEN SEA SHORES 2026-10-26] camera-centred terrain blocks, so every region gets it near the camera,
            // whatever its size (Paul's var regions run to 3072 m and more).
            float pixelHeld = wolfPixelHeld(position.xy, dead);
            up = mix(wclear.x, wclear.z, pixelHeld) - WOLF_CLEAR_ALPHA * dead - position.z;
            if (eyeVec.z > position.z)
            {
                up = min(up, eyeVec.z - WOLF_CLEAR_EYE - position.z
                             + WOLF_CLEAR_ALPHA * (length(position.xy - eyeVec.xy) - dead));
            }
            hcap = WOLF_CLEAR_BETA * wclear.y;
        }
        float hl = length(dtan);
        if (hl > hcap)
        {
            dtan *= hcap / max(hl, 1e-6);
        }
        float calmRef = max(maxWaveHeight, 0.1);
        float dnc = wolfClearCap(dn, up, calmRef);
        float sech = 1.0 / cosh(clamp(dn / max(up, 1e-4), -10.0, 10.0));
        wave_pos = base_pos + dtan + dnc * surf_n;
        wave_h = wolfClearCap(wave_h, up, calmRef);
        wave_slope *= dn >= 0.0 ? (up > 0.0 ? sech * sech : 0.0) : mix(WOLF_TROUGH_KEEP, 1.0, clamp(up / calmRef, 0.0, 1.0));
        // The fragment's breaker and surf foam follow the geometry: none where the cap has flattened it.
        float calm = up > 0.0 ? clamp(up / max(max(maxWaveHeight, vSurf.z), 0.1), 0.0, 1.0) : 0.0;
        vShore.y *= calm;
        vShore.z *= calm;   // [HARBOUR STRIPES 2026-10-28/] no breaker height where the cap has flattened the water
        vSurf.x *= calm;
        vSurf.w *= calm;
    }
    // [DRY BUILDS 2026-10-26/]

    vary_wave_slope = wave_slope;
    vary_wave_height = wave_h;
    vary_wolf_dry = vec4(wave_pos.xy, Rcap, dot(wave_pos - base_pos, surf_n));   // [DRY BUILDS 2026-10-26/] waterF.glsl

    // <WolfViewer> Falling water from the geometry. rise/run of the surface at this
    // vertex; steeper than about 1:3 (17 degrees) starts to break white, past 0.8 (39
    // degrees) it is a sheet. Keep in step with WolfNaturalWater::FALL_START / FALL_FULL.
    float fall = wolfWaterfall;
    if (wolfStream > 0.0)
    {
        float rise_run = length(surf_n.xy) / max(abs(surf_n.z), 0.02);
        fall = max(fall, smoothstep(0.3, 0.8, rise_run));
    }
    vary_fall = fall;
    // </WolfViewer>

    //transform vertex
    vec4 pos = vec4(wave_pos, 1.0);
    // </FS:WolfViewer>
    mat4 modelViewProj = modelview_projection_matrix;

    vary_position = (modelview_matrix * pos).xyz;
    vary_world_pos = position.xyz;   // <WolfViewer> undisplaced, for the waterfall flow
    vary_light_dir = normal_matrix * lightDir;
    // <FS:WolfViewer> Surface basis from the geometry rather than a hardcoded +Z/+X, so a
    // tilted wolfwater surface shades as the sloping plane it is. For region water the
    // normal attribute is (0,0,1) and the tangent below works out to (1,0,0), i.e. exactly
    // the stock values.
    vary_normal = normal_matrix * surf_n;
    vary_tangent = normal_matrix * surf_t;
    // </FS:WolfViewer>

    vec4 oPosition;

    //get view vector
    vec3 oEyeVec;
    oEyeVec.xyz = pos.xyz-eyeVec;

    float d = length(oEyeVec.xy);
    float ld = min(d, 2560.0);

    pos.xy = eyeVec.xy + oEyeVec.xy/d*ld;
    view.xyz = oEyeVec;

    d = clamp(ld/1536.0-0.5, 0.0, 1.0);
    d *= d;

    // <FS:WolfViewer> wave_pos, not position — this is the vertex that is actually drawn.
    oPosition = vec4(wave_pos, 1.0);
    // </FS:WolfViewer>
//  oPosition.z = mix(oPosition.z, max(eyeVec.z*0.75, 0.0), d); // SL-11589 remove "U" shaped horizon

    oPosition = modelViewProj * oPosition;

    refCoord.xyz = oPosition.xyz + vec3(0,0,0.2);

    //get wave position parameter (create sweeping horizontal waves)
    vec3 v = pos.xyz;
    v.x += (cos(v.x*0.08/*+time*0.01*/)+sin(v.y*0.02))*6.0;

    //push position for further horizon effect.
    pos.xyz = oEyeVec.xyz*(waterHeight/oEyeVec.z);
    pos.w = 1.0;
    pos = modelview_matrix*pos;

    calcAtmospherics(pos.xyz);

    //pass wave parameters to pixel shader
    vec2 bigWave;
    // <WolfViewer> A stream's ripples run DOWNSTREAM, not in the region's EEP wave
    // direction: the maps are addressed by the ribbon's own coordinates and slid along
    // the flow at the stream's surface speed. The coarse layer runs a little faster than
    // the fine ones, which is what stops it reading as one printed conveyor belt, and a
    // slow lateral sway does the rest.
    if (wolfStream > 0.0)
    {
        vec2 suv = vec2(texcoord0.x - time * wolfStream,
                        texcoord0.y + 0.35 * sin(texcoord0.x * 0.7 + time * 1.3));
        bigWave       = suv * vec2(0.04, 0.08);
        littleWave.xy = suv * vec2(0.45, 0.9);
        littleWave.zw = suv * vec2(0.1, 0.2) + vec2(-time * wolfStream * 0.05, 0.0);
    }
    else
    {
    // [SMOOTH SEA 2026-10-28] the scroll ACCUMULATED from the eased directions (wolfWaveScroll), not waveDir x time:
    // a weather change set every ripple racing by hours x the change in direction
    bigWave =  (v.xy) * vec2(0.04,0.04)  + wolfWaveScroll.xy * 0.055;
    //get two normal map (detail map) texture coordinates
    littleWave.xy = (v.xy) * vec2(0.45, 0.9)   + wolfWaveScroll.zw * 0.13;
    littleWave.zw = (v.xy) * vec2(0.1, 0.2) + wolfWaveScroll.xy * 0.1;
    }
    // </WolfViewer>
    view.w = bigWave.y;
    refCoord.w = bigWave.x;

    gl_Position = oPosition;
}
