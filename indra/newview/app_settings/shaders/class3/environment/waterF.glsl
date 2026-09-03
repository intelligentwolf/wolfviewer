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

