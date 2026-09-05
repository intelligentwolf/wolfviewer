/**
 * @file class1\deferred\terrainF.glsl
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

uniform sampler2D detail_0;
uniform sampler2D detail_1;
uniform sampler2D detail_2;
uniform sampler2D detail_3;
uniform sampler2D alpha_ramp;

in vec3 pos;
in vec3 vary_normal;
in vec4 vary_texcoord0;
in vec4 vary_texcoord1;

// <WolfViewer 2026-09-05> Terrain look — see lldrawpoolterrain.cpp renderFullShaderTextures()
// for the uniforms and terrainV.glsl for the varyings. With wolf_terrain_look the stock blend
// above the region's four detail textures gains:
//   1. each detail texture sampled at the normal scale and at a 4.3x coarser one, cross-faded
//      by view distance (30..140 m) so far ground stops showing the tile repeat;
//   1b. TRIPLANAR sampling on steep faces (2026-09-05, Paul: "weird banding"): the stock
//      texgen projects every detail texture straight down (object_plane_s/t are XY planes,
//      lldrawpoolterrain.cpp renderFullShaderTextures), so on a cliff one texel is smeared
//      down the whole face and the mountain sides read as horizontal stripes. Here the
//      texture is also projected along X and Y at the same detail scale and the three
//      projections are blended by the region-space normal, sharpened so level ground stays
//      exactly the stock look and only faces steeper than ~40 degrees change. The layer
//      MIX is untouched — which texture shows where is still the estate's composition.
//   1c. THE COMPOSITION ITSELF, per pixel (2026-09-05, Paul: "banding that just doesn't happen
//      in real life"). The stock composition (LLVLComposition::generateHeights) is
//      (height + noise - start_height) * 4 / height_range per composition TEXEL, read per
//      VERTEX (LLSurfacePatch::eval) and fed through the alpha-ramp fade: a pure function of
//      height, so on a mountain the layers are horizontal bands (the noise is 2-D — noise2()
//      reads vec[0..1] only — so it shifts the bands, it does not break them). Here the same
//      estate mapping (wolf_comp_start / wolf_comp_range, bilinear over the region exactly as
//      generateHeights does it) is evaluated per pixel with 2-D noise, then bent by what
//      shapes real ground cover:
//        - steep faces go to layer 2, the estate's mountain/rock slot (snow and turf do not
//          hold on a 35-degree face);
//        - hollows (the heightmap AO) go down a layer (soil and vegetation collect there);
//      and the four layers are blended by HEIGHT, not by a linear fade: each texture's
//      luminance stands in for its height map, and at a boundary the higher parts of one
//      texture poke through the other (Mishkinis, "Advanced Terrain Texture Splatting",
//      gamedeveloper.com), so grass fills the cracks of rock instead of a translucent band.
//   2. the heightmap AO (vertex colour) darkening creases and valley floors;
//   3. an optional snow line whitening high, not-too-steep ground.
// Removed the same day after Paul compared a snowy mountain with the box on and off: a
// slope-to-rock rule (it overrode the estate's snow on steep faces) and boundary noise on the
// composition (it mottled ground that sits on a layer boundary). The layer choice stays the
// estate's — the composition is never altered. wolf_terrain_look == 0 is the stock shader
// verbatim. The SAME constants are in WolfStorm (terrain_manager.js) — keep them in step.
in float vary_ao;
in float vary_up;
in vec3 vary_region_pos;
in vec3 vary_region_normal;
uniform float wolf_terrain_look;
// The same uniform terrainV.glsl feeds texgen_object with: .x is the detail scale
// (LLDrawPoolTerrain::sDetailScale), which the side projections reuse.
uniform vec4 object_plane_s;
// Estate height -> layer mapping, corners SW, SE, NW, NE (lldrawpoolterrain.cpp).
uniform vec4 wolf_comp_start;
uniform vec4 wolf_comp_range;
uniform float wolf_region_width;
uniform float wolf_terrain_snow_line;
uniform vec2 wolf_noise_offset;

float wsTerrainHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float wsTerrainNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = wsTerrainHash(i);
    float b = wsTerrainHash(i + vec2(1.0, 0.0));
    float c = wsTerrainHash(i + vec2(0.0, 1.0));
    float d = wsTerrainHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// One detail texture at the near scale cross-faded to the 4.3x coarser one by distance.
vec4 wsDetail(sampler2D d, vec2 uv, float farB)
{
    return mix(texture(d, uv), texture(d, uv * 0.23), farB);
}

// One detail texture, triplanar: top projection (the stock texgen coordinates) plus the
// X and Y projections at the same scale, weighted by the sharpened normal.
vec4 wsTriplanar(sampler2D d, vec2 uvTop, vec2 uvX, vec2 uvY, vec3 w, float farB)
{
    vec4 c = wsDetail(d, uvTop, farB) * w.z;
    if (w.x + w.y > 0.002)
    {
        c += wsDetail(d, uvX, farB) * w.x + wsDetail(d, uvY, farB) * w.y;
    }
    return c;
}
// </WolfViewer>

void mirrorClip(vec3 position);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

void main()
{
    mirrorClip(pos);
    /// Note: This should duplicate the blending functionality currently used for the terrain rendering.

    vec4 color0, color1, color2, color3;
    if (wolf_terrain_look > 0.5)
    {
        // <WolfViewer> terrain look, steps 1 and 1b (see the header comment)
        float farB = smoothstep(30.0, 140.0, length(pos)) * 0.65;
        vec3 rn = normalize(vary_region_normal);
        vec3 w = pow(abs(rn), vec3(6.0));   // 6: level ground is all top, 45 degrees is half
        w /= (w.x + w.y + w.z);
        float scale = object_plane_s.x;
        vec2 uvTop = vary_texcoord0.xy;
        vec2 uvX = vary_region_pos.yz * scale;   // faces looking along X: the YZ plane
        vec2 uvY = vary_region_pos.xz * scale;   // faces looking along Y: the XZ plane
        color0 = wsTriplanar(detail_0, uvTop, uvX, uvY, w, farB);
        color1 = wsTriplanar(detail_1, uvTop, uvX, uvY, w, farB);
        color2 = wsTriplanar(detail_2, uvTop, uvX, uvY, w, farB);
        color3 = wsTriplanar(detail_3, uvTop, uvX, uvY, w, farB);
    }
    else
    {
        color0 = texture(detail_0, vary_texcoord0.xy);
        color1 = texture(detail_1, vary_texcoord0.xy);
        color2 = texture(detail_2, vary_texcoord0.xy);
        color3 = texture(detail_3, vary_texcoord0.xy);
    }

    vec4 outColor;
    if (wolf_terrain_look > 0.5)
    {
        // <WolfViewer> terrain look, step 1c: composition per pixel, then height blend.
        // Estate mapping, bilinear over the region with generateHeights' operand order:
        // bilinear(SW, SE, NW, NE, x, y) = (1-x)(1-y)SW + x(1-y)NW + (1-x)y SE + xy NE.
        vec2 f = clamp(vary_region_pos.xy / max(wolf_region_width, 1.0), 0.0, 1.0);
        float start = mix(mix(wolf_comp_start.x, wolf_comp_start.z, f.x),
                          mix(wolf_comp_start.y, wolf_comp_start.w, f.x), f.y);
        float range = mix(mix(wolf_comp_range.x, wolf_comp_range.z, f.x),
                          mix(wolf_comp_range.y, wolf_comp_range.w, f.x), f.y);
        float comp = (vary_region_pos.z - start) * 4.0 / max(range, 1.0);
        // The estate's High (start + range) is where comp reaches 4. From the 3/4 mark up to
        // High the top slot goes from "appearing" to CERTAIN: above High the peak is solid
        // snow whatever the noise or the hollows say, and only real cliffs stay rock.
        float solid = smoothstep(3.0, 4.0, comp);
        // 2-D noise in place of the stock 3-D one: a broad 25 m patchiness, a 6 m one and a
        // fine 2 m one (real cover is grainy at every scale — the reference photo's snow
        // line is hundreds of small patches, not a few big ones). Total swing +-0.65 of a
        // layer, so the estate's tuning still averages out the same but the low slot is not
        // dragged up the mountain by noise alone.
        vec2 nxy = vary_region_pos.xy + wolf_noise_offset;
        float n1 = wsTerrainNoise(nxy * 0.04);
        float n2 = wsTerrainNoise(nxy * 0.17 + 7.3);
        float n3 = wsTerrainNoise(nxy * 0.55 + 3.1);
        comp += (n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3 + (n3 - 0.5) * 0.3;
        // Steep faces are rock: from 25 degrees the mountain slot takes over, full by 45 —
        // except in the solid cap, where snow holds to 50 degrees and only a 60-degree
        // cliff is bare (the crater walls in the reference photo).
        vec3 rn2 = normalize(vary_region_normal);
        float steep = 1.0 - clamp(rn2.z, 0.0, 1.0);
        float rockW = mix(smoothstep(0.10, 0.30, steep), smoothstep(0.35, 0.55, steep), solid);
        // Hollows (the heightmap AO) hold what accumulates: in the snow zone that is snow —
        // the gullies at the snow line stay white first and longest, the streaks in the
        // reference photo — lower down it is soil and growth, so they pull towards the turf
        // slot but never below it (a sandy hollow half way up a mountain is not a thing).
        float hollow = 1.0 - clamp(vary_ao, 0.0, 1.0);
        float snowZone = smoothstep(1.6, 2.4, comp);
        float compUp = comp + hollow * 1.5;
        float compDown = max(comp - hollow * 1.5, min(comp, 1.0));
        comp = mix(compDown, compUp, snowZone);
        // Solid cap overrides the lot; the cliff rule is applied last so it still bites there.
        comp = mix(comp, 3.0, solid);
        comp = mix(comp, 2.0, rockW);
        comp = clamp(comp, 0.0, 3.0);
        // Linear layer weights (a tent of width 1 around each slot), then height blending:
        // each texture's luminance is its height, scaled to 0.45 so a fully weighted layer
        // can never be punched through (that needs another layer's height to beat 1 - depth
        // = 0.5), and 'depth' is how many height units still interlock (0.5: wide, feathered
        // edges, not cutouts). The top slot is treated as a FILLER — its
        // height is the inverse of its brightness — so where a thin snow cover meets rock the
        // rock's bright high points poke through it, as they do, instead of a white plate
        // winning everywhere because white is the brightest thing on the mountain.
        vec4 w = clamp(1.0 - abs(vec4(comp) - vec4(0.0, 1.0, 2.0, 3.0)), 0.0, 1.0);
        const vec3 lum = vec3(0.299, 0.587, 0.114);
        vec4 h = vec4(dot(color0.rgb, lum), dot(color1.rgb, lum), dot(color2.rgb, lum), 1.0 - dot(color3.rgb, lum)) * 0.45;
        vec4 wh = w + h;
        float ma = max(max(wh.x, wh.y), max(wh.z, wh.w)) - 0.5;
        vec4 b = max(wh - ma, 0.0) * step(0.001, w);
        outColor = (color0 * b.x + color1 * b.y + color2 * b.z + color3 * b.w) / max(b.x + b.y + b.z + b.w, 1e-4);
        // </WolfViewer>
    }
    else
    {
        float alpha1 = texture(alpha_ramp, vary_texcoord0.zw).a;
        float alpha2 = texture(alpha_ramp,vary_texcoord1.xy).a;
        float alphaFinal = texture(alpha_ramp, vary_texcoord1.zw).a;
        outColor = mix( mix(color3, color2, alpha2), mix(color1, color0, alpha1), alphaFinal );
    }

    if (wolf_terrain_look > 0.5)
    {
        // <WolfViewer> terrain look, steps 2 and 3
        outColor.rgb *= vary_ao;
        if (wolf_terrain_snow_line > 0.0)
        {
            vec2 nxy = vary_region_pos.xy + wolf_noise_offset;
            float n1 = wsTerrainNoise(nxy * 0.11);
            float steep = 1.0 - clamp(vary_up, 0.0, 1.0);
            float snowW = smoothstep(wolf_terrain_snow_line - 4.0, wolf_terrain_snow_line + 6.0,
                                     vary_region_pos.z + (n1 - 0.5) * 8.0)
                          * (1.0 - smoothstep(0.30, 0.55, steep));
            outColor.rgb = mix(outColor.rgb, vec3(0.92, 0.94, 0.98), snowW);
        }
    }

    outColor.a = 0.0; // yes, downstream atmospherics

    frag_data[0] = max(outColor, vec4(0));
    frag_data[1] = vec4(0.0,0.0,0.0,-1.0);
    vec3 nvn = normalize(vary_normal);
    frag_data[2] = encodeNormal(nvn.xyz, 0, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}

