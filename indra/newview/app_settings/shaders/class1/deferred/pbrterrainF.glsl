/**
 * @file class1\deferred\terrainF.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

/*[EXTRA_CODE_HERE]*/

#define TERRAIN_PBR_DETAIL_EMISSIVE 0
#define TERRAIN_PBR_DETAIL_OCCLUSION -1
#define TERRAIN_PBR_DETAIL_NORMAL -2
#define TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS -3

#define TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE 0
#define TERRAIN_PAINT_TYPE_PBR_PAINTMAP 1

#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
#define TerrainCoord vec4[3]
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
#define TerrainCoord vec2
#endif

#define MIX_X    1 << 3
#define MIX_Y    1 << 4
#define MIX_Z    1 << 5
#define MIX_W    1 << 6

struct TerrainMix
{
    vec4 weight;
    int type;
};

TerrainMix get_terrain_mix_weights(float alpha1, float alpha2, float alphaFinal);
TerrainMix get_terrain_usage_from_weight3(vec3 weight3);

struct PBRMix
{
    vec4 col;       // RGB color with alpha, linear space
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    vec3 orm;       // Occlusion, roughness, metallic
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    vec2 rm;        // Roughness, metallic
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    vec3 vNt;       // Unpacked normal texture sample, vector
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    vec3 emissive;  // RGB emissive color, linear space
#endif
};

PBRMix init_pbr_mix();

PBRMix terrain_sample_and_multiply_pbr(
    TerrainCoord terrain_coord
    , sampler2D tex_col
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    , sampler2D tex_orm
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    , sampler2D tex_vNt
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
    , float transform_sign
#endif
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    , sampler2D tex_emissive
#endif
    , vec4 factor_col
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    , vec3 factor_orm
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    , vec2 factor_rm
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    , vec3 factor_emissive
#endif
    );

PBRMix mix_pbr(PBRMix mix1, PBRMix mix2, float mix2_weight);

out vec4 frag_data[4];

#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
uniform sampler2D alpha_ramp;
#elif TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
uniform sampler2D paint_map;
#endif

// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#additional-textures
uniform sampler2D detail_0_base_color;
uniform sampler2D detail_1_base_color;
uniform sampler2D detail_2_base_color;
uniform sampler2D detail_3_base_color;
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
uniform sampler2D detail_0_normal;
uniform sampler2D detail_1_normal;
uniform sampler2D detail_2_normal;
uniform sampler2D detail_3_normal;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
uniform sampler2D detail_0_metallic_roughness;
uniform sampler2D detail_1_metallic_roughness;
uniform sampler2D detail_2_metallic_roughness;
uniform sampler2D detail_3_metallic_roughness;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
uniform sampler2D detail_0_emissive;
uniform sampler2D detail_1_emissive;
uniform sampler2D detail_2_emissive;
uniform sampler2D detail_3_emissive;
#endif

uniform vec4[4] baseColorFactors; // See also vertex_color in pbropaqueV.glsl
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
uniform vec4 metallicFactors;
uniform vec4 roughnessFactors;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
uniform vec3[4] emissiveColors;
#endif
uniform vec4 minimum_alphas; // PBR alphaMode: MASK, See: mAlphaCutoff, setAlphaCutoff()

in vec3 vary_position;
in vec3 vary_region_pos;   // <WolfViewer 2026-09-06>
// <WolfViewer 2026-09-06> caustics — see terrainF.glsl for the derivation; same function.
uniform sampler2D wolfCausticTex0;
uniform sampler2D wolfCausticTex1;
uniform vec2  wolf_caustic_tile;
uniform float wolf_caustic_on;
uniform vec3  wolf_caustic_sun;
uniform float wolf_caustic_strength;
uniform float wolf_caustic_fog;
uniform float wolf_water_level;
uniform vec2  wolf_caustic_origin;   // region origin in agent space: the cascades tile agent XY
float wolfWaveLaplacian(sampler2D t, vec2 uv, float texel)
{
    float hxx = texture(t, uv + vec2(texel, 0.0)).x - texture(t, uv - vec2(texel, 0.0)).x;
    float hyy = texture(t, uv + vec2(0.0, texel)).y - texture(t, uv - vec2(0.0, texel)).y;
    return (hxx + hyy) / (2.0 * texel);
}
float wolfCaustic(vec3 region_pos)
{
    if (wolf_caustic_on < 0.5 || region_pos.z >= wolf_water_level)
    {
        return 0.0;
    }
    float cdepth = wolf_water_level - region_pos.z;
    vec3 sd = normalize(wolf_caustic_sun);
    vec2 hit = region_pos.xy + wolf_caustic_origin + sd.xy / max(sd.z, 0.2) * cdepth * 0.75;
    float lap = wolfWaveLaplacian(wolfCausticTex1, hit / wolf_caustic_tile.y, 1.0 / 128.0) / wolf_caustic_tile.y
              + 0.35 * wolfWaveLaplacian(wolfCausticTex0, hit / wolf_caustic_tile.x, 1.0 / 128.0) / wolf_caustic_tile.x;
    float focus = 1.0 / max(abs(1.0 - cdepth * 0.25 * lap), 0.12);
    return clamp((focus - 1.0) * 2.0, 0.0, 3.0) * exp(-wolf_caustic_fog * 0.12 * cdepth) * wolf_caustic_strength;
}
// </WolfViewer>

// <WolfViewer 2026-09-10> Painted roads and tracks (wolfterrainpaint.cpp; Build > Paint). The
// paint map (RGBA16F) holds per texel: R,G = a slot-coded radius (0.25 + 0.2·slot) times
// (cos, sin) of the along-phase of the nearest stroke, B = metres across the band from its
// left edge / tile, A = coverage. The slot is read from the NEAREST texel (a bilinear blend of
// two radii would decode as a third texture along a seam); the angle, the across coordinate
// and the coverage are bilinear. A slot in "road" mode samples its palette texture at
// (across, along) — square tiles following the stroke; a slot in "world" mode at the region
// position / tile, phased by the region's global origin like the ground textures. LLImageRaw
// is bottom-up so v = 0 is the image bottom and the along-phase is sampled directly (WolfStorm
// samples 1 - along: its textures are top-down). Derivatives for textureGrad are taken in
// uniform control flow (dFdx in a divergent branch is undefined). Source:
// wolfstorm/js/world/terrain/terrain_manager.js wsTerrainPaint — keep them in step.
uniform sampler2D wolfPaintMap;
uniform sampler2D wolfPaintTex0;
uniform sampler2D wolfPaintTex1;
uniform sampler2D wolfPaintTex2;
uniform sampler2D wolfPaintTex3;
uniform float wolf_paint_on;
uniform vec2  wolf_paint_size;
uniform vec4  wolf_paint_mode;   // per slot: 0 follow the stroke, 1 world grid
uniform vec4  wolf_paint_tile;   // per slot: metres per repeat (world grid)
uniform vec4  wolf_paint_offx;   // per slot: region origin mod tile (metres)
uniform vec4  wolf_paint_offy;
vec4 wolfPaintSample(int slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (slot == 0) return textureGrad(wolfPaintTex0, uv, ddx, ddy);
    if (slot == 1) return textureGrad(wolfPaintTex1, uv, ddx, ddy);
    if (slot == 2) return textureGrad(wolfPaintTex2, uv, ddx, ddy);
    return textureGrad(wolfPaintTex3, uv, ddx, ddy);
}
float wolfPaintSlotValue(vec4 v, int slot) { return slot == 0 ? v.x : (slot == 1 ? v.y : (slot == 2 ? v.z : v.w)); }
vec3 wolfTerrainPaint(vec3 ground, vec2 region_xy)
{
    if (wolf_paint_on < 0.5) return ground;
    vec2 puv = region_xy / wolf_paint_size;
    if (puv.x < 0.0 || puv.y < 0.0 || puv.x > 1.0 || puv.y > 1.0) return ground;
    vec4 pm = texture(wolfPaintMap, puv);
    vec2 cs = pm.rg;
    vec2 dcx = dFdx(cs), dcy = dFdy(cs);
    float dbx = dFdx(pm.b), dby = dFdy(pm.b);
    vec2 dwx = dFdx(region_xy), dwy = dFdy(region_xy);
    float cov = pm.a;
    if (cov < 0.004) return ground;
    vec2 psz = vec2(textureSize(wolfPaintMap, 0));
    vec2 nrg = texelFetch(wolfPaintMap, ivec2(clamp(puv * psz, vec2(0.0), psz - 1.0)), 0).rg;
    int slot = int(clamp(floor((length(nrg) - 0.15) / 0.2), 0.0, 3.0));
    float r2 = max(dot(cs, cs), 1e-4);
    float along = fract(atan(cs.y, cs.x) / 6.2831853 + 1.0);
    vec2 uvRoad = vec2(pm.b, along);
    vec2 ddxRoad = vec2(dbx, (cs.x * dcx.y - cs.y * dcx.x) / r2 / 6.2831853);
    vec2 ddyRoad = vec2(dby, (cs.x * dcy.y - cs.y * dcy.x) / r2 / 6.2831853);
    float tile = max(wolfPaintSlotValue(wolf_paint_tile, slot), 0.01);
    vec2 uvWorld = (region_xy + vec2(wolfPaintSlotValue(wolf_paint_offx, slot), wolfPaintSlotValue(wolf_paint_offy, slot))) / tile;
    vec2 ddxWorld = dwx / tile, ddyWorld = dwy / tile;
    bool world = wolfPaintSlotValue(wolf_paint_mode, slot) > 0.5;
    vec4 c = wolfPaintSample(slot, world ? uvWorld : uvRoad, world ? ddxWorld : ddxRoad, world ? ddyWorld : ddyRoad);
    // The texture's own alpha is respected on top of the stroke's opacity in the coverage.
    return mix(ground, c.rgb, cov * c.a);
}
// </WolfViewer>

in vec3 vary_normal;
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
in vec3 vary_tangents[4];
flat in float vary_signs[4];
#endif

// vary_texcoord* are used for terrain composition, vary_coords are used for terrain UVs
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
in vec4 vary_texcoord0;
in vec4 vary_texcoord1;
#elif TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
in vec2 vary_texcoord;
#endif
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
in vec4[10] vary_coords;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
in vec4[2] vary_coords;
#endif

void mirrorClip(vec3 position);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

float terrain_mix(TerrainMix tm, vec4 tms4);

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
// from mikktspace.com
vec3 mikktspace(vec3 vNt, vec3 vT, float sign)
{
    vec3 vN = vary_normal;

    vec3 vB = sign * cross(vN, vT);
    vec3 tnorm = normalize( vNt.x * vT + vNt.y * vB + vNt.z * vN );

    tnorm *= gl_FrontFacing ? 1.0 : -1.0;

    return tnorm;
}
#endif

void main()
{
    // Make sure we clip the terrain if we're in a mirror.
    mirrorClip(vary_position);

    TerrainMix tm;
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    float alpha1 = texture(alpha_ramp, vary_texcoord0.zw).a;
    float alpha2 = texture(alpha_ramp,vary_texcoord1.xy).a;
    float alphaFinal = texture(alpha_ramp, vary_texcoord1.zw).a;

    tm = get_terrain_mix_weights(alpha1, alpha2, alphaFinal);
#elif TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
    tm = get_terrain_usage_from_weight3(texture(paint_map, vary_texcoord).xyz);
#endif

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    // RGB = Occlusion, Roughness, Metal
    // default values, see LLViewerTexture::sDefaultPBRORMImagep
    //   occlusion 1.0
    //   roughness 0.0
    //   metal     0.0
    vec3[4] orm_factors;
    orm_factors[0] = vec3(1.0, roughnessFactors.x, metallicFactors.x);
    orm_factors[1] = vec3(1.0, roughnessFactors.y, metallicFactors.y);
    orm_factors[2] = vec3(1.0, roughnessFactors.z, metallicFactors.z);
    orm_factors[3] = vec3(1.0, roughnessFactors.w, metallicFactors.w);
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    vec2[4] rm_factors;
    rm_factors[0] = vec2(roughnessFactors.x, metallicFactors.x);
    rm_factors[1] = vec2(roughnessFactors.y, metallicFactors.y);
    rm_factors[2] = vec2(roughnessFactors.z, metallicFactors.z);
    rm_factors[3] = vec2(roughnessFactors.w, metallicFactors.w);
#endif

    PBRMix pbr_mix = init_pbr_mix();
    PBRMix mix2;
    TerrainCoord terrain_texcoord;
    switch (tm.type & MIX_X)
    {
    case MIX_X:
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
        terrain_texcoord[0].xy = vary_coords[0].xy;
        terrain_texcoord[0].zw = vary_coords[0].zw;
        terrain_texcoord[1].xy = vary_coords[1].xy;
        terrain_texcoord[1].zw = vary_coords[1].zw;
        terrain_texcoord[2].xy = vary_coords[2].xy;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
        terrain_texcoord = vary_coords[0].xy;
#endif
        mix2 = terrain_sample_and_multiply_pbr(
            terrain_texcoord
            , detail_0_base_color
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , detail_0_metallic_roughness
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
            , detail_0_normal
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
            , vary_signs[0]
#endif
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , detail_0_emissive
#endif
            , baseColorFactors[0]
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
            , orm_factors[0]
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , rm_factors[0]
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , emissiveColors[0]
#endif
        );
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        mix2.vNt = mikktspace(mix2.vNt, vary_tangents[0], vary_signs[0]);
#endif
        pbr_mix = mix_pbr(pbr_mix, mix2, tm.weight.x);
        break;
    default:
        break;
    }
    switch (tm.type & MIX_Y)
    {
    case MIX_Y:
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
        terrain_texcoord[0].xy = vary_coords[2].zw;
        terrain_texcoord[0].zw = vary_coords[3].xy;
        terrain_texcoord[1].xy = vary_coords[3].zw;
        terrain_texcoord[1].zw = vary_coords[4].xy;
        terrain_texcoord[2].xy = vary_coords[4].zw;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
        terrain_texcoord = vary_coords[0].zw;
#endif
        mix2 = terrain_sample_and_multiply_pbr(
            terrain_texcoord
            , detail_1_base_color
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , detail_1_metallic_roughness
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
            , detail_1_normal
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
            , vary_signs[1]
#endif
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , detail_1_emissive
#endif
            , baseColorFactors[1]
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
            , orm_factors[1]
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , rm_factors[1]
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , emissiveColors[1]
#endif
        );
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        mix2.vNt = mikktspace(mix2.vNt, vary_tangents[1], vary_signs[1]);
#endif
        pbr_mix = mix_pbr(pbr_mix, mix2, tm.weight.y);
        break;
    default:
        break;
    }
    switch (tm.type & MIX_Z)
    {
    case MIX_Z:
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
        terrain_texcoord[0].xy = vary_coords[5].xy;
        terrain_texcoord[0].zw = vary_coords[5].zw;
        terrain_texcoord[1].xy = vary_coords[6].xy;
        terrain_texcoord[1].zw = vary_coords[6].zw;
        terrain_texcoord[2].xy = vary_coords[7].xy;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
        terrain_texcoord = vary_coords[1].xy;
#endif
        mix2 = terrain_sample_and_multiply_pbr(
            terrain_texcoord
            , detail_2_base_color
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , detail_2_metallic_roughness
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
            , detail_2_normal
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
            , vary_signs[2]
#endif
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , detail_2_emissive
#endif
            , baseColorFactors[2]
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
            , orm_factors[2]
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , rm_factors[2]
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , emissiveColors[2]
#endif
        );
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        mix2.vNt = mikktspace(mix2.vNt, vary_tangents[2], vary_signs[2]);
#endif
        pbr_mix = mix_pbr(pbr_mix, mix2, tm.weight.z);
        break;
    default:
        break;
    }
    switch (tm.type & MIX_W)
    {
    case MIX_W:
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
        terrain_texcoord[0].xy = vary_coords[7].zw;
        terrain_texcoord[0].zw = vary_coords[8].xy;
        terrain_texcoord[1].xy = vary_coords[8].zw;
        terrain_texcoord[1].zw = vary_coords[9].xy;
        terrain_texcoord[2].xy = vary_coords[9].zw;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
        terrain_texcoord = vary_coords[1].zw;
#endif
        mix2 = terrain_sample_and_multiply_pbr(
            terrain_texcoord
            , detail_3_base_color
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , detail_3_metallic_roughness
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
            , detail_3_normal
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
            , vary_signs[3]
#endif
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , detail_3_emissive
#endif
            , baseColorFactors[3]
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
            , orm_factors[3]
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
            , rm_factors[3]
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
            , emissiveColors[3]
#endif
        );
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        mix2.vNt = mikktspace(mix2.vNt, vary_tangents[3], vary_signs[3]);
#endif
        pbr_mix = mix_pbr(pbr_mix, mix2, tm.weight.w);
        break;
    default:
        break;
    }

    float minimum_alpha = terrain_mix(tm, minimum_alphas);
    if (pbr_mix.col.a < minimum_alpha)
    {
        discard;
    }
    float base_color_factor_alpha = terrain_mix(tm, vec4(baseColorFactors[0].z, baseColorFactors[1].z, baseColorFactors[2].z, baseColorFactors[3].z));

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    vec3 tnorm = normalize(pbr_mix.vNt);
#else
    vec3 tnorm = vary_normal;
#endif
    tnorm *= gl_FrontFacing ? 1.0 : -1.0;


#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
#define mix_emissive pbr_mix.emissive
#else
#define mix_emissive vec3(0)
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
#define mix_orm pbr_mix.orm
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
#define mix_orm vec3(1.0, pbr_mix.rm)
#else
// Matte plastic potato terrain
#define mix_orm vec3(1.0, 1.0, 0.0)
#endif
    frag_data[0] = max(vec4(wolfTerrainPaint(pbr_mix.col.xyz, vary_region_pos.xy) * (1.0 + wolfCaustic(vary_region_pos)), 0.0), vec4(0));   // Diffuse (+ <WolfViewer> painted roads, caustics)
    frag_data[1] = max(vec4(mix_orm.rgb, base_color_factor_alpha), vec4(0));                                    // PBR linear packed Occlusion, Roughness, Metal.
    frag_data[2] = encodeNormal(tnorm, 0, GBUFFER_FLAG_HAS_PBR); // normal, flags

#if defined(HAS_EMISSIVE)
    frag_data[3] = max(vec4(mix_emissive,0), vec4(0));                                                // PBR sRGB Emissive
#endif
}

