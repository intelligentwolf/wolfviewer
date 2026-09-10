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
uniform sampler2D wolfDepthField;
uniform sampler2D wolfExposureField;
uniform vec2 wolfRegionOrigin;
uniform vec2 depthRegionSize;
uniform float depthWaterLevel;
uniform float depthReady;
uniform vec2 exposureOrigin;
uniform vec2 exposureSize;
uniform float exposureReady;
uniform float shoreWavesEnabled;
// [WAVES 2026-09-07] Per-region wave ZONE energy over the same 3x span (wolfwavezones.cpp
// fill: surf 1, open 0.55, calm 0.15, off 0), from About Land > Waves of this region and its
// neighbours. Source: wolfstorm Water.js zoneSampler.
uniform sampler2D wolfZoneField;
uniform vec2 zoneOrigin;
uniform vec2 zoneSize;
uniform float zoneReady;
// [SURF 2026-09-07] The surf train (WAVES_PLAN §2, Water.js): wave HEIGHT offshore (m),
// the set cadence (s), the wavelength (m), a tempo scale. From About Land > Waves.
uniform float surfHeight;
uniform float surfSetInterval;
uniform float surfLength;
uniform float surfSpeed;
uniform float calmRipple;   // [WAVES 2026-09-07] the calm cells' ripple, metres (lldrawpoolwater.cpp)
uniform float smallScale;   // [WAVES 2026-09-10] the small-wave cells' swell, fraction of the open sea (lldrawpoolwater.cpp)
out vec4 vSurf;   // [SURF rev3] x crest (peak only), y breaking, z amplitude used (0 = no surf), w wash behind the crest
uniform float fftReady;
uniform sampler2D fftDisp0;
uniform sampler2D fftDisp1;
uniform vec2 fftTile;
uniform vec2 fftFade;
uniform sampler2D wakeSampler;
uniform vec2 wakeRegionSize;
uniform float wakeReady;
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
// vSwell = (localAmp, wavelengthJitter, swellScale, 0) so the fragment can re-evaluate
// the three dominant trains at PAST times for the whitecap memory.
out vec2 vShore;
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

vec3 gerstnerWave(vec2 pos, float wavelength, float amp, vec2 dir, float steepness, float numWaves)
{
    float k = 6.28318 / wavelength;
    float c = sqrt(9.8 / k);
    vec2 d = normalize(dir);
    float f = k * (dot(d, pos) - c * time * waveSpeed);
    float Q = steepness / (k * amp * numWaves + 0.001);
    return vec3(Q * amp * d.x * cos(f),
                Q * amp * d.y * cos(f),
                amp * sin(f));
}

// Analytic slope of that wave's z component: dz/dxy = d * A * k * cos(f). MUST be called
// with the same arguments as its matching gerstnerWave() or the lighting drifts off the
// geometry it is supposed to describe.
vec2 gerstnerSlope(vec2 pos, float wavelength, float amp, vec2 dir)
{
    float k = 6.28318 / wavelength;
    float c = sqrt(9.8 / k);
    vec2 d = normalize(dir);
    float f = k * (dot(d, pos) - c * time * waveSpeed);
    return d * (amp * k * cos(f));
}
// </FS:WolfViewer> -------------------------------------------------------------------

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
            swellExpo = mix(1.0, texture(wolfExposureField, xuv).r, xf.x * xf.y);
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
    if (zoneReady > 0.5)
    {
        vec2 zuv = (regionXY - zoneOrigin) / zoneSize;
        if (zuv.x >= 0.0 && zuv.x <= 1.0 && zuv.y >= 0.0 && zuv.y <= 1.0)
        {
            vec2 zf = smoothstep(vec2(0.0), vec2(0.02), zuv)
                    * (vec2(1.0) - smoothstep(vec2(0.98), vec2(1.0), zuv));
            zoneEnergy = mix(0.55, texture(wolfZoneField, zuv).r, zf.x * zf.y);
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
    vSwell = vec4(0.0, 1.0, swellScale, shoreGate);
    vShore = vec2(0.0);
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
            float localAmp = waveAmplitude * localVariation * fade * swellScale;

            float baseWavelength = 1.0 / (waveFrequency + 0.001);
            float wavelengthJitter = 1.0 + stormChaos * 0.3 * snoise(wxy * 0.001 + time * 0.01);
            vSwell.xy = vec2(localAmp, wavelengthJitter);

            // Primary swell direction is the region's own EEP wave direction, so a region
            // that sets its water rolling one way gets its geometry rolling that way too.
            vec2 dir1 = normalize(waveDir1);
            // +35 degrees and -60 degrees off the primary: a real sea is several trains
            // crossing, not one.
            vec2 dir2 = vec2(dir1.x * 0.819 - dir1.y * 0.574,
                             dir1.x * 0.574 + dir1.y * 0.819);
            vec2 dir3 = vec2(dir1.x * 0.5 + dir1.y * 0.866,
                            -dir1.x * 0.866 + dir1.y * 0.5);
            vec2 dir5 = vec2(-dir1.x * 0.5 - dir1.y * 0.866,
                              dir1.x * 0.866 - dir1.y * 0.5);

            vec3 w1 = gerstnerWave(wxy, baseWavelength * 2.0 * wavelengthJitter, localAmp * 0.40, dir1, 0.65, numWaves);
            vec3 w2 = gerstnerWave(wxy, baseWavelength * 1.5 * wavelengthJitter, localAmp * 0.30, dir2, 0.55, numWaves);
            vec3 w3 = gerstnerWave(wxy, baseWavelength * 1.2,                    localAmp * 0.25, dir3, 0.50, numWaves);
            float oppositeAmp = localAmp * (0.15 + stormChaos * 0.15) * chopKeep;
            vec3 w4 = gerstnerWave(wxy, baseWavelength * 0.8,                    oppositeAmp, -dir1 * 0.7 + dir2 * 0.3, 0.45, numWaves);
            vec3 w5 = gerstnerWave(wxy, baseWavelength * 0.5,                    localAmp * 0.15 * chopKeep, dir5, 0.35, numWaves);
            float chopAmp = localAmp * (0.08 + stormChaos * 0.12) * chopKeep;
            vec3 w6 = gerstnerWave(wxy, baseWavelength * 0.3,                    chopAmp, dir3 * 0.6 - dir2 * 0.4, 0.25, numWaves);
            vec3 w7 = vec3(0.0);
            vec3 w8 = vec3(0.0);
            if (stormChaos > 0.1)
            {
                float randAngle1 = snoise(wxy * 0.002) * 3.14159;
                w7 = gerstnerWave(wxy, baseWavelength * 0.7, localAmp * 0.12 * stormChaos, vec2(cos(randAngle1), sin(randAngle1)), 0.3, numWaves);
                float randAngle2 = snoise(wxy * 0.003 + 100.0) * 3.14159;
                w8 = gerstnerWave(wxy, baseWavelength * 0.4, localAmp * 0.1 * stormChaos, vec2(cos(randAngle2), sin(randAngle2)), 0.25, numWaves);
            }

            vec3 total = w1 + w2 + w3 + w4 + w5 + w6 + w7 + w8;
            total.z += fbm(wxy * 0.02, time) * localAmp * (0.1 + stormChaos * 0.1) * chopKeep;

            // Slopes of the three dominant trains, same arguments as their waves above.
            wave_slope = gerstnerSlope(wxy, baseWavelength * 2.0 * wavelengthJitter, localAmp * 0.40, dir1)
                       + gerstnerSlope(wxy, baseWavelength * 1.5 * wavelengthJitter, localAmp * 0.30, dir2)
                       + gerstnerSlope(wxy, baseWavelength * 1.2,                    localAmp * 0.25, dir3);

            // Backstop, not the height (2.5x the amplitude from lldrawpoolwater.cpp).
            if (maxWaveHeight > 0.0)
            {
                total.z = clamp(total.z, -maxWaveHeight, maxWaveHeight);
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
        if (f0 > 0.001) dd += texture(fftDisp0, position.xy / fftTile.x).xyz * f0;
        if (f1 > 0.001) dd += texture(fftDisp1, position.xy / fftTile.y).xyz * f1;
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
        vec2 sduv = regionXY / depthRegionSize;
        if (sduv.x >= 0.0 && sduv.x <= 1.0 && sduv.y >= 0.0 && sduv.y <= 1.0)
        {
            vec2 ef = smoothstep(vec2(0.0), vec2(0.04), sduv)
                    * (vec2(1.0) - smoothstep(vec2(0.96), vec2(1.0), sduv));
            float edgeFade = ef.x * ef.y;
            vec4 dtex = texture(wolfDepthField, sduv);
            float conf = length(dtex.gb);
            if (conf > 0.02)
            {
                float smoothDepth = max(depthWaterLevel - dtex.a, 0.0);
                // [WAVES 2026-09-10] ... times the shore gate: breakers on the open sea and the surf only.
                float shoal = (1.0 - smoothstep(0.5, 8.0, smoothDepth)) * min(conf * 1.5, 1.0) * edgeFade * shoreGate;
                if (shoal > 0.01)
                {
                    float speedScale = clamp(length(waveDir1) * 0.885, 0.4, 2.0);
                    float phase = time * 4.5 * speedScale + sqrt(smoothDepth) * 8.0;
                    vShore = vec2(phase, shoal);
                    float feed = 1.0;
                    if (exposureReady > 0.5)
                    {
                        vec2 fuv = (regionXY - dtex.gb * (45.0 / conf) - exposureOrigin) / exposureSize;
                        feed = texture(wolfExposureField, clamp(fuv, 0.0, 1.0)).r;
                    }
                    float bAmp = min(0.10 + waveAmplitude * 0.9, 0.5) * shoal * (0.35 + 0.65 * sqrt(feed));
                    float br = sin(phase);
                    float lift = (br + 0.35 * br * br) * bAmp;
                    wave_pos += surf_n * lift;
                    wave_h += lift;
                }
            }
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
    if (surfHeight > 0.01 && zoneReady > 0.5 && shoreWavesEnabled > 0.5 && boundedWaterDepth <= 0.0 && exposureReady > 0.5)
    {
        // Surf ONLY where a designer painted it (Paul 09-07: "surf is a special thing"). Water.js same.
        float surfZone = smoothstep(0.62, 0.95, zoneEnergy);
        vec2 euv = (regionXY - exposureOrigin) / exposureSize;
        if (surfZone > 0.01 && euv.x >= 0.0 && euv.x <= 1.0 && euv.y >= 0.0 && euv.y <= 1.0)
        {
            // [SURF 2026-09-07 rev2] Water.js, same math: the wave's coordinate is the baked
            // DISTANCE TO LAND (exposure G) — crests are iso-distance contours that wrap the
            // coast; direction = down the distance gradient taken 1/32 of the span wide.
            vec2 dW = vec2(1.0 / 32.0);
            float dist = texture(wolfExposureField, euv).g;
            float dxp = texture(wolfExposureField, clamp(euv + vec2(dW.x, 0.0), 0.0, 1.0)).g;
            float dxm = texture(wolfExposureField, clamp(euv - vec2(dW.x, 0.0), 0.0, 1.0)).g;
            float dyp = texture(wolfExposureField, clamp(euv + vec2(0.0, dW.y), 0.0, 1.0)).g;
            float dym = texture(wolfExposureField, clamp(euv - vec2(0.0, dW.y), 0.0, 1.0)).g;
            vec2 grad = vec2(dxp - dxm, dyp - dym);
            float gl = length(grad);
            bool haveLand = dist < 3000.0 && gl > 1.0;
            vec2 dir = haveLand ? -grad / gl : normalize(vec2(depthRegionSize.x * 0.5, depthRegionSize.y * 0.5) - regionXY + vec2(0.001, 0.0));
            float coord = haveLand ? -dist : dot(regionXY, dir);
            float h = 30.0;
            if (depthReady > 0.5)
            {
                // Past the region border the depth CONTINUES from the border texel and eases
                // to deep water over 64 m (the void planes share these fields now): a hard
                // switch to 30 m at the edge changed the shoaling and tore the crest.
                vec2 sduv = regionXY / depthRegionSize;
                vec2 over = max(max(-sduv, sduv - 1.0), 0.0) * depthRegionSize;
                float outside = smoothstep(0.0, 64.0, max(over.x, over.y));
                vec4 dt = texture(wolfDepthField, clamp(sduv, 0.0, 1.0));
                h = mix(max(depthWaterLevel - dt.a, 0.0), 30.0, outside);
            }
            const float g = 9.81;
            float lambda = max(surfLength, 12.0 * surfHeight);
            float k = 6.2831853 / max(lambda, 8.0);
            float kh = k * max(h, 0.05);
            float tk = tanh(kh);
            float omega = sqrt(g * k * tk);
            vec2 across = vec2(-dir.y, dir.x);
            float setPh = 6.2831853 * (time * surfSpeed / max(surfSetInterval, 10.0)) - coord * (0.22 / lambda);
            float setEnv = 0.30 + 0.70 * smoothstep(0.15, 1.0, 0.5 + 0.5 * sin(setPh));
            float crestVar = 0.85 + 0.15 * sin(dot(regionXY, across) * (1.1 / lambda) + time * 0.1);
            float ksh = clamp(inversesqrt(max(tk, 0.05)), 1.0, 1.8);
            float crestH = min(surfHeight * surfZone * setEnv * ksh * crestVar, surfHeight * 1.15);
            float hEff = h + 0.8 * surfHeight;
            float Hmax = 0.78 * hEff;
            float breakF = smoothstep(0.7, 1.15, crestH / max(Hmax, 0.01));
            crestH = min(crestH, Hmax);
            crestH *= smoothstep(0.2, 0.6 + 0.5 * surfHeight, h);
            if (crestH > 0.01)
            {
                float kl = k * inversesqrt(max(tk, 0.05));
                float ph = kl * coord - omega * time * surfSpeed;
                float ph2 = ph + (0.30 + 0.45 * breakF) * sin(ph);
                float sn = sin(ph2), cs = cos(ph2);
                float up = 0.5 + 0.5 * sn;
                // [SURF rev3, Paul 09-07 "looks weird": 20 m crests were 240 m plateaus with
                // cliff faces] A shoaling wave is CNOIDAL: a narrow peaked crest and a long flat
                // trough. upk peaks the crest (exponent 1.6 -> 2.8 as it breaks) and every
                // forward term is weighted by it, so only the top leans and throws — a concave
                // face, not a wall. Water.js same.
                float upk = pow(up, 1.6 + 1.2 * breakF);
                float prof = mix(-0.25, 1.0, upk) + 0.25 * breakF * upk * upk;
                float tip = upk * upk * upk;
                float lip = 0.55 * breakF * tip;   // the crest tip thrown past fold-over: the barrel
                float Q = mix(0.35, 0.9, breakF);
                float horiz = crestH * (0.5 * Q * cs * upk + 0.35 * breakF * upk * upk + lip);
                float lift = crestH * (prof - 0.35 * lip);
                wave_pos += surf_t * (dir.x * horiz) + surf_b * (dir.y * horiz) + surf_n * lift;
                wave_h += crestH * prof;
                wave_slope += dir * (crestH * 0.6 * kl * cs * (0.3 + 0.7 * upk));
                // wash = the churn a broken crest leaves BEHIND it (its back slope, cs > 0)
                float wash = breakF * smoothstep(0.3, 1.0, cs) * smoothstep(0.1, 0.5, up);
                vSurf = vec4(smoothstep(0.3, 1.0, upk), breakF, crestH, wash);   // peak only
            }
        }
    }

    // Boat wash displacement (Water.js [WAKE 2026-08-21]): the Kelvin arms lift the
    // surface and the churn astern depresses it (G lift, B dip, differenced at read).
    if (wakeReady > 0.5 && wakeStrength > 0.0)
    {
        vec2 wkuv = regionXY / wakeRegionSize;
        if (wkuv.x >= 0.0 && wkuv.x <= 1.0 && wkuv.y >= 0.0 && wkuv.y <= 1.0)
        {
            vec4 wk = texture(wakeSampler, wkuv);
            vWake = wk.r * wakeStrength;
            wave_pos += surf_n * ((wk.g - wk.b) * 1.7 * wakeStrength);
        }
    }
    // </WolfViewer>

    vary_wave_slope = wave_slope;
    vary_wave_height = wave_h;

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
    bigWave =  (v.xy) * vec2(0.04,0.04)  + waveDir1 * time * 0.055;
    //get two normal map (detail map) texture coordinates
    littleWave.xy = (v.xy) * vec2(0.45, 0.9)   + waveDir2 * time * 0.13;
    littleWave.zw = (v.xy) * vec2(0.1, 0.2) + waveDir1 * time * 0.1;
    }
    // </WolfViewer>
    view.w = bigWave.y;
    refCoord.w = bigWave.x;

    gl_Position = oPosition;
}
