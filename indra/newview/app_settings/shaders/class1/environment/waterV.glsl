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

    if (waveAmplitude > 0.001)
    {
        vec2 wxy = position.xy;

        // Fade displacement out with distance. Two jobs: it keeps far water from
        // shimmering into aliasing, and it flattens the stretch-to-horizon void-water
        // planes, which are still tessellated at the stock 32m step (see LLVOWater::
        // updateGeometry) and cannot express a wave at all.
        float wave_dist = length(wxy - eyeVec.xy);
        float fade = 1.0 - smoothstep(waveFade.x, waveFade.y, wave_dist);

        if (fade > 0.0)
        {
            const float numWaves = 8.0;

            // Per-patch variation, so the sea is not one repeating corrugation.
            float noiseScale = 0.008;
            float timeVary = time * 0.02;
            float noise1 = snoise(wxy * noiseScale + timeVary);
            float noise2 = snoise(wxy * noiseScale * 0.3 + timeVary * 0.5);
            float localVariation = 0.5 + 0.3 * noise1 + 0.15 * noise2;
            float localAmp = waveAmplitude * localVariation * fade;

            float baseWavelength = 1.0 / (waveFrequency + 0.001);
            float wavelengthJitter = 1.0 + 0.15 * snoise(wxy * 0.001 + time * 0.01);

            // Primary swell direction is the region's own EEP wave direction, so a region
            // that sets its water rolling one way gets its geometry rolling that way too.
            vec2 dir1 = normalize(waveDir1);
            // +35 degrees and -60 degrees off the primary: a real sea is several trains
            // crossing, not one.
            vec2 dir2 = vec2(dir1.x * 0.819 - dir1.y * 0.574,
                             dir1.x * 0.574 + dir1.y * 0.819);
            vec2 dir3 = vec2(dir1.x * 0.5 + dir1.y * 0.866,
                            -dir1.x * 0.866 + dir1.y * 0.5);

            vec3 w1 = gerstnerWave(wxy, baseWavelength * 2.0 * wavelengthJitter, localAmp * 0.40, dir1, 0.65, numWaves);
            vec3 w2 = gerstnerWave(wxy, baseWavelength * 1.5 * wavelengthJitter, localAmp * 0.30, dir2, 0.55, numWaves);
            vec3 w3 = gerstnerWave(wxy, baseWavelength * 1.2,                    localAmp * 0.25, dir3, 0.50, numWaves);
            vec3 w4 = gerstnerWave(wxy, baseWavelength * 0.8,                    localAmp * 0.15, -dir1 * 0.7 + dir2 * 0.3, 0.45, numWaves);
            vec3 w5 = gerstnerWave(wxy, baseWavelength * 0.5,                    localAmp * 0.15, -dir3, 0.35, numWaves);
            vec3 w6 = gerstnerWave(wxy, baseWavelength * 0.3,                    localAmp * 0.08, dir3 * 0.6 - dir2 * 0.4, 0.25, numWaves);

            vec3 total = w1 + w2 + w3 + w4 + w5 + w6;
            total.z += fbm(wxy * 0.02, time) * localAmp * 0.1;

            // Slopes of the three dominant trains, same arguments as their waves above.
            wave_slope = gerstnerSlope(wxy, baseWavelength * 2.0 * wavelengthJitter, localAmp * 0.40, dir1)
                       + gerstnerSlope(wxy, baseWavelength * 1.5 * wavelengthJitter, localAmp * 0.30, dir2)
                       + gerstnerSlope(wxy, baseWavelength * 1.2,                    localAmp * 0.25, dir3);

            wave_pos += total.x * surf_t + total.y * surf_b + total.z * surf_n;
            wave_h = total.z;
        }
    }

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
