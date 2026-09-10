/**
 * @file waterF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

// class3/environment/waterF.glsl

#define WATER_MINIMAL 1

out vec4 frag_color;

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

vec3 scaleSoftClipFragLinear(vec3 l);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec4 applyWaterFogViewLinear(vec3 pos, vec4 color);

void mirrorClip(vec3 pos);

// PBR interface
vec2 BRDF(float NoV, float roughness);

void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

void pbrIbl(vec3 diffuseColor,
    vec3 specularColor,
    vec3 radiance, // radiance map sample
    vec3 irradiance, // irradiance map sample
    float ao,       // ambient occlusion factor
    float nv,       // normal dot view vector
    float perceptualRoughness,
    out vec3 diffuse,
    out vec3 specular);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

uniform sampler2D bumpMap;
uniform sampler2D bumpMap2;
uniform float     blend_factor;
#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
uniform sampler2D depthMap;
#endif

uniform sampler2D exclusionTex;

uniform int classic_mode;
uniform vec3 lightDir;
uniform vec3 specular;
uniform float blurMultiplier;
uniform float refScale;
uniform float kd;
uniform vec3 normScale;
uniform float fresnelScale;
uniform float fresnelOffset;
// <FS:WolfViewer> Swell height, the same value the vertex stage displaced by — the foam
// below has to know how big a crest IS before it can tell one from a trough. waveDir1 is
// the region's EEP wave direction; it is uploaded to the program, so declaring it here
// simply reads the same uniform waterV.glsl does, and the shoreline sets keep the tempo
// the region asked for.
uniform float waveAmplitude;
uniform vec2 waveDir1;
// The same wave clock waterV.glsl runs on (LLDrawPoolWater uploads WATER_TIME once for the
// whole program), so the shoreline sets keep step with the swell geometry.
uniform float time;
// 0 for the region's own water; for a wolfwater prim surface, the depth of water the prim
// represents (its Z extent). See LLVOWater::setBoundedWaterDepth.
uniform float boundedWaterDepth;
// The EEP water fog, uploaded for every water shader by LLSettingsVOWater::applySpecial.
// Region water never needs these here because its body comes from fog applied to submerged
// geometry; a bounded surface has to colour itself.
uniform vec3  waterFogColorLinear;
uniform float waterFogDensity;
// </FS:WolfViewer>
// <WolfViewer 2026-09-06> The rest of WolfStorm's water, ported (Water.js fragment stage
// — same names, same numbers, keep in step). See waterV.glsl for what each field is.
uniform float waveFrequency;
uniform float waveSpeed;
uniform float stormChaos;
uniform vec3  eyeVec;
uniform sampler2D wolfDepthField;
uniform vec2  wolfRegionOrigin;
uniform vec2  depthRegionSize;
uniform float depthWaterLevel;
uniform float depthReady;
uniform float shoreWavesEnabled;
uniform float fftReady;
uniform sampler2D fftDisp0;
uniform sampler2D fftDisp1;
uniform sampler2D fftDeriv0;
uniform sampler2D fftDeriv1;
uniform vec2  fftTile;
uniform vec2  fftFade;
uniform sampler2D wakeSampler;
uniform vec2  wakeRegionSize;
uniform float wakeReady;
uniform float wakeStrength;
// </WolfViewer>

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;
in vec3 vary_normal;
in vec3 vary_tangent;
in vec3 vary_light_dir;
// <FS:WolfViewer> Analytic slope and crest height of the Gerstner swell the vertex stage
// displaced. See waterV.glsl.
in vec2 vary_wave_slope;
in float vary_wave_height;
// <WolfViewer> Waterfall sheets (wolfnaturalwater.cpp, LLVOWater::setWaterfall). Per plane:
// 1 = draw this surface as falling water — normals from the bump maps racing DOWN the face
// (scrolled by world height) and a foam-white body — instead of as a still surface.
uniform float wolfWaterfall;
in vec3 vary_world_pos;
// Per-fragment falling-water amount, 0..1 — max(wolfWaterfall, slope of a stream mesh),
// interpolated from the vertex stage so a ribbon blends into and out of a fall.
in float vary_fall;
uniform float wolfStream;
// </WolfViewer>
// <WolfViewer 2026-09-06> from waterV.glsl: shore phase + shoal, wake foam, the swell's
// local amplitude / wavelength jitter / exposure scale.
in vec2 vShore;
in float vWake;
in vec4 vSwell;
in vec4 vSurf;   // [SURF rev3] crest (peak only), breaking, amplitude, wash
// </WolfViewer>
// </FS:WolfViewer>

vec3 BlendNormal(vec3 bump1, vec3 bump2)
{
    vec3 n = mix(bump1, bump2, blend_factor);
    return n;
}

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosLighting(vec3 light);
vec3 scaleSoftClip(vec3 light);
vec3 toneMapNoExposure(vec3 color);

vec3 vN, vT, vB;

vec3 transform_normal(vec3 vNt)
{
    return normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);
}

void sampleReflectionProbesWater(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, vec3 amblit_linear);

void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit);


vec3 getPositionWithNDC(vec3 ndc);

void generateWaveNormals(out vec3 wave1, out vec3 wave2, out vec3 wave3)
{
    // Generate all of our wave normals.
    // We layer these back and forth.

    vec2 bigwave = vec2(refCoord.w, view.w);

    vec3 wave1_a = texture(bumpMap, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_a = texture(bumpMap, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_a = texture(bumpMap, littleWave.zw).xyz * 2.0 - 1.0;

    vec3 wave1_b = texture(bumpMap2, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_b = texture(bumpMap2, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_b = texture(bumpMap2, littleWave.zw).xyz * 2.0 - 1.0;

    wave1 = BlendNormal(wave1_a, wave1_b);
    wave2 = BlendNormal(wave2_a, wave2_b);
    wave3 = BlendNormal(wave3_a, wave3_b);
}

void calculateFresnelFactors(out vec3 df3, out vec2 df2, vec3 viewVec, vec3 wave1, vec3 wave2, vec3 wave3, vec3 wavef)
{
    // We calculate the fresnel here.
    // We do this by getting the dot product for each sets of waves, and applying scale and offset.

    df3 = max(vec3(0), vec3(
        dot(viewVec, wave1),
        dot(viewVec, (wave2 + wave3) * 0.5),
        dot(viewVec, wave3)
    ) * fresnelScale + fresnelOffset);

    df3 *= df3;

    df2 = max(vec2(0), vec2(
        df3.x + df3.y + df3.z,
        dot(viewVec, wavef) * fresnelScale + fresnelOffset
    ));
}

// <WolfViewer 2026-09-06> Jacobian of the Gerstner swell — Tessendorf's whitecap
// criterion ("Whitecap Phenomenology for Ocean Surface Simulation"): a surface point
// x' = x + D(x) folds where J = (1 + dDx/dx)(1 + dDy/dy) - (dDx/dy)^2 drops toward 0.
// For one train x' = x + Q A d cos(f), f = k(d.p - c t): dDx/dx = -Q A k d.x^2 sin f,
// dDy/dy = -Q A k d.y^2 sin f, dDx/dy = -Q A k d.x d.y sin f. Same three trains, same
// arguments, as waterV.glsl's w1..w3. Q is normalised by (k A numWaves) there, which makes
// J independent of the HEIGHT; real breaking is not (it starts near a steepness A k of
// 0.05), so the fold fraction is scaled by the dominant train's true steepness.
// Source: wolfstorm/js/libs/Water.js swellFoldFraction / swellWhitecap.
float swellFoldFraction(vec2 p, float t)
{
    float baseWl = 1.0 / (waveFrequency + 0.001);
    vec2 d1 = normalize(waveDir1);
    vec2 d2 = vec2(d1.x * 0.819 - d1.y * 0.574, d1.x * 0.574 + d1.y * 0.819);
    vec2 d3 = vec2(d1.x * 0.5 + d1.y * 0.866, -d1.x * 0.866 + d1.y * 0.5);
    float A = vSwell.x;
    float jit = vSwell.y;
    float dxx = 0.0, dyy = 0.0, dxy = 0.0;
    vec3 wl = vec3(baseWl * 2.0 * jit, baseWl * 1.5 * jit, baseWl * 1.2);
    vec3 amp = vec3(A * 0.4, A * 0.3, A * 0.25);
    vec3 st = vec3(0.65, 0.55, 0.5);
    vec2 dirs[3] = vec2[3](d1, d2, d3);
    for (int i = 0; i < 3; ++i)
    {
        float k = 6.28318 / wl[i];
        float c = sqrt(9.8 / k);
        float f = k * (dot(dirs[i], p) - c * t * waveSpeed);
        float Q = st[i] / (k * amp[i] * 8.0 + 0.001);
        float sn = Q * amp[i] * k * sin(f);
        dxx -= dirs[i].x * dirs[i].x * sn;
        dyy -= dirs[i].y * dirs[i].y * sn;
        dxy -= dirs[i].x * dirs[i].y * sn;
    }
    float J = (1.0 + dxx) * (1.0 + dyy) - dxy * dxy;
    // Q A k summed over the trains is (0.65 + 0.55 + 0.5) / 8 = 0.2125: normalise 1 - J.
    return clamp((1.0 - J) / 0.2125, 0.0, 1.0);
}

float swellWhitecap(vec2 p, float t)
{
    float baseWl = 1.0 / (waveFrequency + 0.001);
    float k1 = 6.28318 / (baseWl * 2.0 * vSwell.y);
    float steep = vSwell.x * 0.4 * k1;
    float gain = smoothstep(0.02, 0.06, steep);
    // Only the very tip of the crest breaks — a band in the fold fraction.
    return smoothstep(0.78, 0.97, swellFoldFraction(p, t)) * gain;
}
// </WolfViewer>

void main()
{
    mirrorClip(vary_position);

    vN = vary_normal;
    vT = vary_tangent;
    vB = cross(vN, vT);

    vec3 pos = vary_position.xyz;
    float linear_depth = 1 / -pos.z;

    float dist = length(pos.xyz);

    //normalize view vector
    vec3 viewVec = normalize(pos.xyz);

    // Setup our waves.

    vec3 wave1 = vec3(0, 0, 1);
    vec3 wave2 = vec3(0, 0, 1);
    vec3 wave3 = vec3(0, 0, 1);

    generateWaveNormals(wave1, wave2, wave3);

    float dmod = sqrt(dist);
    vec2 distort = (refCoord.xy/refCoord.z) * 0.5 + 0.5;

    vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    // <FS:WolfViewer> Tilt the shading normal with the swell the vertex stage actually
    // built. Without this the geometry rolls but the LIGHT does not: a surface z = h(x,y)
    // has normal proportional to (-dh/dx, -dh/dy, 1), so a displaced crest that is still
    // shaded as though it were flat catches the sun in the wrong place and reads as a
    // painted stripe rather than a wave. wavef is tangent-space here (vT = +X, vB =
    // cross(vN,vT) = +Y, vN = +Z, all from waterV.glsl), so the slope goes straight into
    // its xy with no basis change.
    //
    // The 2.5 gain is carried over from WolfStorm's water (Water.js), where it was tuned
    // against this same formula: the raw slope of a gentle swell is around 0.05, which is
    // an order of magnitude below the normal-map detail it has to compete with, and at
    // unity gain the swell simply does not register in the lighting.
    wavef.xy += -vary_wave_slope * 2.5;
    // <WolfViewer> waterfall: replace the still-water ripples with a fast downward flow.
    // u runs across the sheet, v is the world height PLUS time: a feature of the map sits
    // where z*k + time*c is constant, so as time grows its z FALLS. (With the time term
    // subtracted the foam climbed the face — "waterfalls are going up hill", 2026-09-05.)
    // Blended by vary_fall rather than switched, so a stream mesh runs level, breaks
    // into whitewater over a ledge and calms again without a seam.
    if (vary_fall > 0.005)
    {
        vec2 fuv1 = vec2((vary_world_pos.x + vary_world_pos.y) * 0.35, vary_world_pos.z * 0.6 + time * 3.0);
        vec2 fuv2 = vec2((vary_world_pos.x - vary_world_pos.y) * 0.8,  vary_world_pos.z * 1.4 + time * 4.5);
        vec3 f1 = texture(bumpMap, fuv1).xyz * 2.0 - 1.0;
        vec3 f2 = texture(bumpMap2, fuv2).xyz * 2.0 - 1.0;
        vec3 fallN = normalize(vec3((f1.xy + f2.xy * 0.5) * 1.5, 1.0));
        wavef = normalize(mix(wavef, fallN, vary_fall));
        wave1 = normalize(mix(wave1, fallN, vary_fall));
        wave2 = normalize(mix(wave2, fallN, vary_fall));
        wave3 = normalize(mix(wave3, fallN, vary_fall));
    }
    // </WolfViewer>
    // <WolfViewer 2026-09-06> The shore-reactive wave field (Water.js [SHORE 2026-08-15]):
    // the baked depth field bends the surface detail toward the beach — crest lines follow
    // iso-depth contours and travel shoreward — with the phase and shoal factor from the
    // vertex stage, the single source of truth that also lifts the breaker geometry, so
    // the foam below sits on the geometric crests. The fragment adds only a noise wobble.
    vec2 regionXY = vary_world_pos.xy - wolfRegionOrigin;
    float shoal = 0.0;
    float shoreCrest = 0.0;
    float shoreCos = 0.0;
    vec2 shoreDir = vec2(0.0);
    float shoreGain = clamp(waveAmplitude * 6.67, 0.25, 1.6);
    float fieldDepth = 1e6;
    if (depthReady > 0.5 && boundedWaterDepth <= 0.0)
    {
        vec2 sduv = regionXY / depthRegionSize;
        if (sduv.x >= 0.0 && sduv.x <= 1.0 && sduv.y >= 0.0 && sduv.y <= 1.0)
        {
            vec4 dtex = texture(wolfDepthField, sduv);
            fieldDepth = max(depthWaterLevel - dtex.r, 0.0);
            if (shoreWavesEnabled > 0.5)
            {
                float conf = length(dtex.gb);
                if (conf > 0.02)
                {
                    shoreDir = dtex.gb / conf;
                    shoal = vShore.y;
                    if (shoal > 0.01)
                    {
                        float phase = vShore.x + (wave3.x + wave3.y) * (0.45 + stormChaos * 0.4);
                        float crest = sin(phase);
                        shoreCos = cos(phase);
                        shoreCrest = smoothstep(0.30, 0.95, crest) * shoal;
                    }
                }
            }
        }
    }

    // Tilt with the wind-sea cascades' slopes (Water.js FFT block): the normals persist
    // to 2.5x the displacement fade — at range the eye reads the lighting of the chop,
    // not its height — and the derivative textures are mipmapped so far water averages
    // toward flat. The cascades' accumulated whitecap channel is picked up here too.
    float fftFoam = 0.0;
    bool cascades = fftReady > 0.5 && boundedWaterDepth <= 0.0 && wolfStream <= 0.0 && wolfWaterfall <= 0.0;
    if (cascades)
    {
        float fdist = length(eyeVec.xy - vary_world_pos.xy);
        float n0f = 1.0 - smoothstep(fftFade.x, fftFade.x * 2.5, fdist);
        float n1f = 1.0 - smoothstep(fftFade.y, fftFade.y * 2.5, fdist);
        vec2 uv0 = vary_world_pos.xy / fftTile.x;
        vec2 uv1 = vary_world_pos.xy / fftTile.y;
        vec4 dv0 = texture(fftDeriv0, uv0);
        vec4 dv1 = texture(fftDeriv1, uv1);
        wavef.xy += -(dv0.xy * n0f + dv1.xy * n1f) * 2.5 * vSwell.z;
        float fm0 = texture(fftDisp0, uv0).a * n0f;
        float fm1 = texture(fftDisp1, uv1).a * n1f;
        fftFoam = max(fm0, fm1 * 0.6) * vSwell.z;
    }

    // Re-aim the surface detail toward the beach in shallow water: an extra detail tap
    // scrolled along -shoreDir plus a rhythmic crest tilt, so each passing wave catches
    // the sun like a real approaching swell. The tap is UNCONDITIONAL (a mipmapped
    // sample inside a non-uniform branch has undefined derivatives); in deep water the
    // mix weight is simply 0.
    vec2 shoreUV = vary_world_pos.xy * 0.22 - shoreDir * time * 0.45;
    vec3 waveShore = texture(bumpMap, shoreUV).xyz * 2.0 - 1.0;
    wavef = mix(wavef, waveShore, shoal * 0.5);
    wavef.xy += shoreDir * shoreCos * shoal * min(0.4 * shoreGain, 0.55);

    // Tilt with the wake's own slope (Water.js [WAKE 2026-08-21]) so the wash catches sun
    // and reflection instead of reading as a flat white smear.
    if (wakeReady > 0.5 && wakeStrength > 0.0)
    {
        vec2 wkuv = regionXY / wakeRegionSize;
        if (wkuv.x >= 0.0 && wkuv.x <= 1.0 && wkuv.y >= 0.0 && wkuv.y <= 1.0)
        {
            float wkTexel = 1.0 / 512.0;
            vec4 tL = texture(wakeSampler, wkuv - vec2(wkTexel, 0.0));
            vec4 tR = texture(wakeSampler, wkuv + vec2(wkTexel, 0.0));
            vec4 tD = texture(wakeSampler, wkuv - vec2(0.0, wkTexel));
            vec4 tU = texture(wakeSampler, wkuv + vec2(0.0, wkTexel));
            float cL = tL.g - tL.b, cR = tR.g - tR.b;
            float cD = tD.g - tD.b, cU = tU.g - tU.b;
            wavef.xy += vec2(cL - cR, cD - cU) * 28.0 * wakeStrength;
        }
    }
    // </WolfViewer>
    // </FS:WolfViewer>

    vec3 df3 = vec3(0);
    vec2 df2 = vec2(0);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos.xyz, wavef, vary_light_dir, sunlit, amblit, additive, atten);

    calculateFresnelFactors(df3, df2, normalize(view.xyz), wave1, wave2, wave3, wavef);

    vec3 waver = wavef*3;

    vec3 up = transform_normal(vec3(0,0,1));
    float vdu = -dot(viewVec, up)*2;

    vec3 wave_ibl = wavef * normScale;
    wave_ibl.z *= 2.0;
    wave_ibl = transform_normal(normalize(wave_ibl));

    vec3 norm = transform_normal(normalize(wavef));

    vdu = clamp(vdu, 0, 1);
    //wavef.z *= max(vdu*vdu*vdu, 0.1);

    wavef = normalize(wavef);

    //wavef = vec3(0, 0, 1);
    wavef = transform_normal(wavef);

    float dist2 = dist;
    dist = max(dist, 5.0);

    //figure out distortion vector (ripply)
    vec2 distort2 = distort + waver.xy * refScale / max(dmod, 1.0) * 2;

    distort2 = clamp(distort2, vec2(0), vec2(0.999));

    float shadow = 1.0f;

    float water_mask = texture(exclusionTex, distort).r;

#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, distort);
#endif

    vec3 sunlit_linear = sunlit;
    float fade = 1;
    // <FS:WolfViewer> How deep the water is at this fragment, in metres along the view ray.
    // Set from the refraction depth buffer inside the TRANSPARENT_WATER block below, which
    // already unprojects it for the shoreline fade; left effectively infinite when there is
    // no depth to read (opaque water), which switches the shoreline foam off rather than
    // making it up.
    float wolf_water_depth = 1e6;
    // </FS:WolfViewer>
#ifdef TRANSPARENT_WATER
    float depth = texture(depthMap, distort).r;

    vec3 refPos = getPositionWithNDC(vec3(distort*2.0-vec2(1.0), depth*2.0-1.0));

    // Calculate some distance fade in the water to better assist with refraction blending and reducing the refraction texture's "disconnect".
#ifdef SHORELINE_FADE
    fade = max(0,min(1, (pos.z - refPos.z) / 10));
#else
    fade = 1;
#endif
    fade *= water_mask;
    distort2 = mix(distort, distort2, min(1, fade * 10));
    depth = texture(depthMap, distort2).r;

    refPos = getPositionWithNDC(vec3(distort2 * 2.0 - vec2(1.0), depth * 2.0 - 1.0));

    if (pos.z < refPos.z - 0.05)
    {
        distort2 = distort;
    }

    // <FS:WolfViewer> refPos is the geometry seen THROUGH the water, unprojected from the
    // refraction pass's depth buffer; pos is the water surface. Both are eye-space, so the
    // difference is how much water the view ray crosses before it hits the bottom. That is
    // a true depth — it reads the actual seabed, riverbed or pool floor, including prims —
    // and it costs nothing, because the two positions are already computed above for the
    // refraction blend.
    wolf_water_depth = max(0.0, pos.z - refPos.z);
    // </FS:WolfViewer>

    vec4 fb = texture(screenTex, distort2);

#else
    vec4 fb = applyWaterFogViewLinear(viewVec*2048.0, vec4(1.0));

    if (water_mask < 1)
        discard;
#endif

    float metallic = 1.0;
    float perceptualRoughness = blurMultiplier;
    float gloss      = 1 - perceptualRoughness;

    vec3  irradiance = vec3(0);
    vec3  radiance  = vec3(0);
    vec3 legacyenv = vec3(0);

    // TODO: Make this an option.
#ifdef WATER_MINIMAL
    sampleReflectionProbesWater(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, amblit);
#elif WATER_MINIMAL_PLUS
    sampleReflectionProbes(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, false, amblit);
#endif

    vec3 diffuseColor = vec3(0);
    vec3 specularColor = vec3(0);
    vec3 specular_linear = srgb_to_linear(specular);
    calcDiffuseSpecular(specular_linear, metallic, diffuseColor, specularColor);

    vec3 v = -normalize(pos.xyz);

    vec3 colorEmissive = vec3(0);
    float ao = 1.0;
    vec3 light_dir = transform_normal(lightDir);

    float NdotV = clamp(abs(dot(norm, v)), 0.001, 1.0);

    float nl = 0;
    vec3 diffPunc = vec3(0);
    vec3 specPunc = vec3(0);

    pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, normalize(wavef+up*max(dist, 32.0)/32.0*(1.0-vdu)), v, normalize(light_dir), nl, diffPunc, specPunc);

    vec3 punctual = clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit_linear * shadow * atten;
    radiance *= df2.y;
    //radiance = toneMapNoExposure(radiance);
    vec3 color = vec3(0);
    color = mix(fb.rgb, radiance, min(1, df2.x)) + punctual.rgb;

    // <WolfViewer 2026-09-06> Light through the crests (Water.js [SSS 2026-09-06]): a
    // crest is a thin sheet of water with the sun behind it, so it glows the translucent
    // green-cyan of the water body where a flat surface would only reflect (Atlas GDC
    // 2019, GodotOceanWaves). View direction against the sun direction bent by the
    // surface normal, raised to a power for the lobe, times how high on the crest the
    // fragment is; tied to the EEP fog colour, scaled by the attenuated sunlight so it
    // cannot glow at night, suppressed where fresnel says the surface is a mirror.
    if (boundedWaterDepth <= 0.0 && waveAmplitude > 0.001)
    {
        vec3 toEye = -viewVec;
        vec3 sunE = normalize(vary_light_dir);
        float crestH = clamp(vary_wave_height / (waveAmplitude * 1.2 + 0.001), 0.0, 1.0);
        vec3 sunToEye = normalize(-sunE + norm * 0.35);
        float lobe = pow(max(0.0, dot(toEye, sunToEye)), 3.0);
        float thin = crestH * crestH;
        float sss = lobe * thin * (1.0 - clamp(df2.x, 0.0, 1.0)) * clamp(waveAmplitude * 2.5, 0.0, 1.0);
        vec3 sssColor = waterFogColorLinear * 3.0 + vec3(0.02, 0.10, 0.06);
        color += sssColor * sunlit_linear * atten * sss * 0.8;
    }
    // </WolfViewer>

    float water_haze_scale = 4;

    if (classic_mode > 0)
        water_haze_scale = 1;

    // This looks super janky, but we do this to restore water haze in the distance.
    // These values were finagled in to try and bring back some of the distant brightening on legacy water.  Also works reasonably well on PBR skies such as PBR midday.
    // color = mix(color, additive * water_haze_scale, (1 - atten));

    // We shorten the fade here at the shoreline so it doesn't appear too soft from a distance.
    fade *= 60;
    fade = min(1, fade);
    color = mix(fb.rgb, color, fade);

    // <FS:WolfViewer> Open-water foam lace on the swell crests.
    //
    // Ported from WolfStorm's water (Water.js, "ambient open-water foam lace"). Real sea
    // is not a clean surface: it carries thin lacy streaks of aerated water that gather on
    // the tops of the swell and thin out in the troughs. Without them a displaced surface
    // still reads as coloured jelly, because nothing on it tells you which way is up
    // except the lighting.
    //
    // The streaks come from thresholding two normal-map taps that have already been
    // sampled (no extra texture fetch), and they are concentrated by vary_wave_height —
    // the actual Gerstner crest height at this fragment — so the foam sits on the
    // geometry rather than floating over it. Everything scales with waveAmplitude, so
    // calm water stays clean and setting the swell to 0 removes this with it.
    //
    // Scaled by the scene light. An earlier version of this in WolfStorm used a constant
    // colour and the foam GLOWED white under a night sky.
    // <FS:WolfViewer> Give a BOUNDED surface a body of its own.
    //
    // Firestorm's water takes nearly all its colour from the refraction buffer and its
    // reflections; what makes the region ocean look like water rather than glass is the
    // water fog applied to the geometry BENEATH it. Nothing behind a pool prim has been
    // fogged, so without this a pool is very nearly invisible — which is exactly what a
    // transparent prim underneath one shows.
    //
    // Beer-Lambert absorption toward the region's EEP water fog colour over the prim's own
    // thickness, so a deep prim gives deep water and a shallow one gives a wash. The lower
    // clamp is a deliberate choice, not physics: a builder marking a paper-thin panel as
    // wolfwater still means "this is water", and 0.22 is the least tint that still reads as
    // such. The upper clamp keeps a very deep prim from going fully opaque.
    if (boundedWaterDepth > 0.0)
    {
        float absorb = 1.0 - exp(-waterFogDensity * boundedWaterDepth);
        color = mix(color, waterFogColorLinear, clamp(absorb, 0.22, 0.85));
    }
    // <WolfViewer> waterfall body: aerated water is mostly white, in streaks that fall with
    // the flow (the same bump maps read as a brightness field, scrolled by world height).
    if (vary_fall > 0.005)
    {
        // z PLUS time, so the streaks fall (see the normal block above for the sign).
        float streak = texture(bumpMap, vec2((vary_world_pos.x - vary_world_pos.y) * 0.9, vary_world_pos.z * 0.25 + time * 2.2)).r;
        float streak2 = texture(bumpMap2, vec2((vary_world_pos.x + vary_world_pos.y) * 1.7, vary_world_pos.z * 0.7 + time * 3.6)).g;
        float foam = clamp(0.35 + 0.45 * streak + 0.25 * streak2, 0.0, 0.92) * vary_fall;
        float fallLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.93, 0.96, 0.99) * fallLight, foam);
    }
    // </WolfViewer>
    // </FS:WolfViewer>

    // <FS:WolfViewer> Shoreline foam — sets breaking on the shallows.
    //
    // Real water goes white where it shoals, and it does it in TRAVELLING BANDS, not as a
    // static rim: as a wave runs into shallow water it slows, the crests bunch up behind
    // it, and it breaks. The phase term below is WolfStorm's (Water.js shore waves):
    // sqrt(depth) is the shallow-water celerity relation, so adding it to the clock makes
    // crests bunch and slow exactly where the water gets thin, and the sets roll beachward
    // on their own.
    //
    // The depth is the real one measured off the refraction buffer above, so this follows
    // a hand-built riverbed or the lip of a wolfwater pool as readily as it follows terrain.
    //
    // DECLARED LIMIT vs WolfStorm: the foam is a fragment effect only. WolfStorm also LIFTS
    // the breaker geometry as it shoals, which needs the depth in the VERTEX stage — a
    // screen-space depth buffer cannot be read there, so that would need a baked terrain
    // heightfield and is not done here. The bands read correctly; they do not stand up.
    // A BOUNDED surface has no shoreline to break on. wolf_water_depth measures the gap to
    // whatever is behind the surface, and for a pool prim that gap is a few centimetres
    // everywhere — so this block, written for a beach, read the whole pool as maximally
    // shoaling and turned it to whitewater. Region water only.
    // <WolfViewer 2026-09-06> With the region's depth field baked, the shore is WolfStorm's
    // (Water.js [SHORE] / [SWASH 2026-08-21]): a Beer-Lambert shallow tint, a swash line
    // that advances up the beach on each crest and drains back on the trough (a cosine
    // synced to the same approaching-wave phase as the breakers, Cyanilux's shoreline
    // breakdown), ragged by the shore ripple tap, breaking foam brightest on the crests,
    // and faint caustic veins in the shallows. The depth is the baked TERRAIN field's, not
    // the refraction buffer's: that buffer measures the gap to whatever is behind the surface,
    // so a dock post, a hull or an avatar standing in the water got a white foam rim, and its
    // quantisation in the shallows drew a striped moire (2026-09-06 screenshot). The terrain
    // field is smooth and knows only the land — the same source WolfStorm's swash uses.
    float wDepth = fieldDepth;
    if (depthReady > 0.5 && boundedWaterDepth <= 0.0 && wDepth < 1e5)
    {
        float shallowF = exp(-waterFogDensity * 0.35 * wDepth);
        float depthLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        vec3 shallowTint = color * 1.25 + vec3(0.03, 0.14, 0.12) * depthLight;
        color = mix(color, shallowTint, shallowF * 0.25 * (1.0 - clamp(df2.x, 0.0, 1.0) * 0.5));
        float clump = clamp((waveShore.x + waveShore.y) * 0.7 + 0.5, 0.0, 1.0);
        float swash = shoreCos * 0.5 + 0.5;
        float bandW = 0.34 + 1.15 * swash;
        float band = 1.0 - clamp(wDepth / bandW, 0.0, 1.0);
        band = clamp(band + (clump - 0.5) * 0.30 * shoal, 0.0, 1.0);
        float fNoise = 0.55 + 0.45 * ((wave2.z + 1.0) * 0.5);
        // [WAVES 2026-09-10] ... on the open sea and the surf only (vSwell.w = shoreGate, 0 in
        // off / calm / small cells): an enclosed lake never goes white. Water.js same.
        float shoreFoam = pow(band, 1.5) * fNoise * (0.78 + 0.34 * swash) * vSwell.w;
        float breakFoam = shoreCrest * shoreCrest * (0.30 + 0.70 * clump) * shoreGain;
        float foamAmt = max(shoreFoam, breakFoam * 0.85);
        vec3 foamCol = vec3(0.93, 0.96, 0.98) * depthLight;
        float foamMix = clamp(foamAmt, 0.0, 0.85);
        color = mix(color, foamCol, foamMix);
        float veins = pow(smoothstep(0.35, 0.85, abs(wave2.x + wave3.y)), 2.0);
        color += vec3(0.10, 0.14, 0.13) * veins * shallowF * depthLight;
    }
    else
    // </WolfViewer>
    if (boundedWaterDepth <= 0.0 && waveAmplitude > 0.001 && wolf_water_depth < 8.0)
    {
        float shoal = 1.0 - smoothstep(0.4, 5.0, wolf_water_depth);
        // Tempo follows the region's EEP wave direction magnitude, as the swell does.
        float speedScale = clamp(length(waveDir1) * 0.885, 0.4, 2.0);
        float phase = time * 4.5 * speedScale + sqrt(max(wolf_water_depth, 0.0)) * 8.0
                    + (wave3.x + wave3.y) * 0.45;
        float crest = smoothstep(0.30, 0.95, sin(phase)) * shoal;
        // 0.85 was far too strong — every shallow patch went solidly white rather than
        // reading as a band of surf. Foam is a highlight on the water, not a replacement
        // for it.
        float breakFoam = clamp(crest * 0.45, 0.0, 0.45);
        float breakLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.95, 0.97, 0.99) * breakLight, breakFoam);
    }
    // </FS:WolfViewer>

    // [SURF 2026-09-07] Whitewater on the surf train (waterV.glsl vSurf; Water.js fragment,
    // same rule): a little on every crest top, a breaking lip and trailing wash where the
    // wave has hit the 0.78 h limit.
    if (vSurf.z > 0.005)
    {
        float sc = vSurf.x, sb = vSurf.y, sw = vSurf.w;
        float sNoise = 0.7 + 0.3 * ((wave2.z + 1.0) * 0.5);
        float sNoise2 = 0.6 + 0.4 * clamp((wave1.x + wave3.y) * 0.7 + 0.5, 0.0, 1.0);
        // (Paul 09-07: more whitewater, never full-bright, NOT a sheet over the whole crest
        // band) the PEAK of every surf wave carries some, a breaking wave is white from the
        // lip down, and the churn it leaves behind it (vSurf.w) is patchy wash. Water.js same.
        float crestFoam = sc * (0.5 + 0.5 * sb) * sNoise;
        float face = smoothstep(0.3, 1.0, sc) * sb;
        float wash = sw * 0.35 * sNoise2 * sNoise2;
        float sf = clamp(max(max(crestFoam, face), wash), 0.0, 0.85);
        float surfLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.90, 0.94, 0.97) * surfLight, sf);
    }

    // <WolfViewer 2026-09-06> Breaking crests with MEMORY (Water.js [WHITECAPS]): the fold
    // at this world position now and 0.9 / 1.8 / 2.7 s ago, decaying — foam lingers
    // behind the crest that made it, so a sea reads as streaked rather than dotted; no
    // buffer, no readback. Combined by max with the cascades' accumulated foam. Gated
    // into patches by the slow big-scale tap: a whitecap is a streak here and there along
    // a crest, never the whole crest.
    if (boundedWaterDepth <= 0.0 && waveAmplitude > 0.001)
    {
        vec2 wp = vary_world_pos.xy;
        float cap = swellWhitecap(wp, time);
        cap = max(cap, swellWhitecap(wp, time - 0.9) * 0.62);
        cap = max(cap, swellWhitecap(wp, time - 1.8) * 0.38);
        cap = max(cap, swellWhitecap(wp, time - 2.7) * 0.22);
        // (`patch` is a reserved GLSL keyword — NVIDIA rejects it, 2026-09-06 magenta water)
        float capPatch = smoothstep(0.35, 0.75, wave1.x * 0.5 + 0.5 + wave3.y * 0.2);
        cap = max(cap * capPatch, fftFoam);
        float ragged = 0.55 + 0.45 * clamp((wave2.x + wave3.y) * 0.8 + 0.5, 0.0, 1.0);
        float capMix = clamp(cap * ragged * 0.85, 0.0, 0.7);
        float capLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.93, 0.96, 0.98) * capLight, capMix);
    }
    // Boat wash (Water.js [WAKE]): churned water is full of air, so it scatters rather
    // than reflects — brighter and denser than the swell's foam.
    if (vWake > 0.003)
    {
        float wash = clamp(pow(vWake, 0.7) * 0.80, 0.0, 0.70);
        float washLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.95, 0.97, 0.99) * washLight, wash);
    }
    // </WolfViewer>

    if (waveAmplitude > 0.001)
    {
        float lace = smoothstep(0.5, 0.9, abs(wave1.x + wave2.y));
        float crestF = clamp(vary_wave_height / (waveAmplitude * 1.2 + 0.001), 0.0, 1.0);
        float ambFoam = lace * (0.05 + 0.30 * crestF)
                      * clamp(waveAmplitude * 5.0, 0.0, 1.0)
                      // Open sea carries whitecaps; a garden pond does not.
                      * (boundedWaterDepth > 0.0 ? 0.3 : 1.0);
        float ambLight = clamp(dot(sunlit_linear + amblit, vec3(0.3333)), 0.08, 1.0);
        color = mix(color, vec3(0.90, 0.94, 0.97) * ambLight, ambFoam);
    }
    // </FS:WolfViewer>

    float spec = min(max(max(punctual.r, punctual.g), punctual.b), 0);

    frag_color = min(vec4(1),max(vec4(color.rgb, spec * water_mask), vec4(0)));
}

