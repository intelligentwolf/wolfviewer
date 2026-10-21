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
// <WolfViewer 2026-09-06> Sunlight caustics on submerged ground, focused through the
// spectral ocean's actual wave field (lldrawpoolterrain.cpp wolf_bind_caustics; the
// slope textures are wolfoceanfft.cpp's attachment 1: xy = dz/dx, dz/dy). A ray of sun
// reaching a submerged point came through the surface where the refracted sun direction
// hits it; the light there is focused by the surface curvature — a trough converges the
// rays below it, a crest spreads them — the footprint of a bundle of rays scaling by about
// 1 - d c lap (c ~ 0.25 for the water/air bend), so the irradiance is its inverse.
// Beer-Lambert over the depth with the EEP fog density.
// Source: wolfstorm/js/world/terrain/terrain_manager.js _installSplatShader caustics block.
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
// The same uniform terrainV.glsl feeds texgen_object with: .x is the detail scale
// (LLDrawPoolTerrain::sDetailScale), which the side projections reuse.
uniform vec4 object_plane_s;
// Estate height -> layer mapping, corners SW, SE, NW, NE (lldrawpoolterrain.cpp).
uniform vec4 wolf_comp_start;
uniform vec4 wolf_comp_range;
uniform float wolf_region_width;
uniform float wolf_terrain_snow_line;
uniform vec2 wolf_noise_offset;
// <WolfViewer 2026-09-18> SNOW ON THE GROUND. Source: wolfstorm terrain_manager.js, the block
// after wsTerrainPaint - same numbers. wolf_snow_cover 0..1 is how much has settled
// (WolfWeather::updateSnowCover); wolfShelterMap is the roof grid round the camera, 1 = open
// sky, at wolf_shelter_origin / wolf_shelter_size in region metres; outside it the ground is
// open. Patchy as it starts, solid at a full cover; none on steep faces, none on the sea bed,
// none under a roof. Over the painted roads, like the ground around them.
uniform float wolf_snow_cover;
uniform sampler2D wolfShelterMap;
uniform vec2  wolf_shelter_origin;
uniform float wolf_shelter_size;

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

// <WolfViewer 2026-09-10> Painted roads and tracks (wolfterrainpaint.cpp; Build > Paint). The
// paint map (RGBA16F) holds per texel: R,G = a slot-coded radius (0.25 + 0.2·slot, slots 0-5,
// WolfTerrainPaint::SLOT_R0 / SLOT_DR; a code above 1 is fine in a half float) times
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
uniform sampler2D wolfPaintTex4;   // [6 SLOTS 2026-09-19] WolfTerrainPaint::SLOTS = 6
uniform sampler2D wolfPaintTex5;
uniform float wolf_paint_on;
uniform vec2  wolf_paint_size;     // metres the paint map covers ...
uniform vec2  wolf_paint_origin;   // ... starting here, region metres: (0,0) unless the region is
                                   // too big for one map and the map is a camera-following window
uniform float wolf_paint_mode[6];  // per slot (WolfTerrainPaint::SLOTS): 0 follow the stroke, 1 world grid
uniform float wolf_paint_tile[6];  // per slot: metres per repeat (world grid)
uniform float wolf_paint_offx[6];  // per slot: region origin mod tile (metres)
uniform float wolf_paint_offy[6];
vec4 wolfPaintSample(int slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (slot == 0) return textureGrad(wolfPaintTex0, uv, ddx, ddy);
    if (slot == 1) return textureGrad(wolfPaintTex1, uv, ddx, ddy);
    if (slot == 2) return textureGrad(wolfPaintTex2, uv, ddx, ddy);
    if (slot == 3) return textureGrad(wolfPaintTex3, uv, ddx, ddy);
    if (slot == 4) return textureGrad(wolfPaintTex4, uv, ddx, ddy);
    return textureGrad(wolfPaintTex5, uv, ddx, ddy);
}
vec3 wolfTerrainPaint(vec3 ground, vec2 region_xy)
{
    if (wolf_paint_on < 0.5) return ground;
    vec2 puv = (region_xy - wolf_paint_origin) / wolf_paint_size;
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
    // floor((r - (SLOT_R0 - SLOT_DR / 2)) / SLOT_DR) for r = 0.25 + 0.2 * slot, slots 0-5 (wolfterrainpaint.h)
    int slot = int(clamp(floor((length(nrg) - 0.15) / 0.2), 0.0, 5.0));
    float r2 = max(dot(cs, cs), 1e-4);
    float along = fract(atan(cs.y, cs.x) / 6.2831853 + 1.0);
    vec2 uvRoad = vec2(pm.b, along);
    vec2 ddxRoad = vec2(dbx, (cs.x * dcx.y - cs.y * dcx.x) / r2 / 6.2831853);
    vec2 ddyRoad = vec2(dby, (cs.x * dcy.y - cs.y * dcy.x) / r2 / 6.2831853);
    float tile = max(wolf_paint_tile[slot], 0.01);
    vec2 uvWorld = (region_xy + vec2(wolf_paint_offx[slot], wolf_paint_offy[slot])) / tile;
    vec2 ddxWorld = dwx / tile, ddyWorld = dwy / tile;
    bool world = wolf_paint_mode[slot] > 0.5;
    vec4 c = wolfPaintSample(slot, world ? uvWorld : uvRoad, world ? ddxWorld : ddxRoad, world ? ddyWorld : ddyRoad);
    // The texture's own alpha is respected on top of the stroke's opacity in the coverage.
    return mix(ground, c.rgb, cov * c.a);
}
// </WolfViewer>

// <WolfViewer 2026-10-16> WOLF TERRAIN TEXTURES (wolfterrainlayers.cpp; Paul: "goes up to 4096
// metres so wolfviewer and wolfstorm people can see on high mountains"). With wolf_layers_on the
// ground colour comes from the region's eight Wolf layers instead of the four detail textures; the
// paint, the look's AO and snow line, the settled snow and the caustics below apply as before.
// The record and the band model are documented in wolfstorm php/terrain_paint.php; the same maths
// is in WolfStorm terrain_manager.js wsWolfLayers — keep them in step:
//   hz = height - bilinear(corner offsets SW, SE, NW, NE; x east, y north) + noise * 2 * amplitude
//   (noise: wsTerrainNoise at 25 m / 6 m / 2 m, weights 0.6 / 0.25 / 0.15, centred, -0.5..0.5);
//   layer 0 is the ground from the bottom; each later present layer i covers what is below it by
//   t_i = smoothstep(h_i - b_i/2, h_i + b_i/2, hz) (a nested mix: the weights sum to 1);
//   steep faces (1 - n.z from 0.22 to 0.40, about 39 to 53 degrees) turn to the cliff layer.
// Each layer tiles on the world grid at its own tile size, phased by the region's global origin
// (wolf_layer_off) so it is seamless across region borders; with the Enhanced terrain look the
// layers are also dual-scale and triplanar like the detail textures above. All eight textures are
// ONE sampler2DArray (one texture unit). SAMPLER COUNT: with it this program declares 16 samplers, so it is only compiled in above 16 units (the guard below)
// (detail_0-3, alpha_ramp, wolfCausticTex0/1, wolfPaintMap, wolfPaintTex0-5, wolfShelterMap,
// wolfTerrainLayers) — exactly the 16 an Apple GPU has; anything more here needs a texture array
// elsewhere first. The derivatives are taken in uniform control flow and the taps use textureGrad
// (dFdx in a divergent branch is undefined).
// [2026-10-16 MAC GUARD] Paul: "protect the mac version". The layers are compiled in only where the GPU reports more
// than 16 texture image units (llviewershadermgr.cpp WOLF_TERRAIN_LAYERS permutation, WolfTerrainLayers::supported:
// GL_MAX_TEXTURE_IMAGE_UNITS > 16, never on macOS). Without it this program declares no extra sampler at all - the
// 15 it had before - and wolf_layers_on is a constant 0, so the branch in main() is compiled away.
#ifdef WOLF_TERRAIN_LAYERS
uniform sampler2DArray wolfTerrainLayers;
uniform float wolf_layers_on;
uniform vec4  wolf_layer[8];         // start m, blend m, tile m, present (0/1)
uniform vec2  wolf_layer_off[8];     // region origin mod tile, metres
uniform vec4  wolf_layer_corner;     // band offsets at SW, SE, NW, NE, metres
uniform float wolf_layer_noise;      // band-edge noise amplitude, metres
uniform float wolf_layer_cliff;      // layer for steep faces, -1 = none
uniform vec2  wolf_layer_size;       // region width, height (m)
uniform vec2  wolf_layer_noise_off;  // region origin mod 1024 m
vec4 wolfLayerTap(float layer, vec2 uv, vec2 gx, vec2 gy, float farB)
{
    vec4 c = textureGrad(wolfTerrainLayers, vec3(uv, layer), gx, gy);
    if (farB > 0.0)
    {
        c = mix(c, textureGrad(wolfTerrainLayers, vec3(uv * 0.23, layer), gx * 0.23, gy * 0.23), farB);
    }
    return c;
}
vec4 wolfLayerGround(vec3 region_pos, vec3 rn, float farB, bool look)
{
    vec2 rxy = region_pos.xy;
    float z = region_pos.z;
    vec2 f = clamp(rxy / max(wolf_layer_size, vec2(1.0)), 0.0, 1.0);
    float corner = mix(mix(wolf_layer_corner.x, wolf_layer_corner.y, f.x),
                       mix(wolf_layer_corner.z, wolf_layer_corner.w, f.x), f.y);
    vec2 nxy = rxy + wolf_layer_noise_off;
    float n = (wsTerrainNoise(nxy * 0.04) - 0.5) * 0.6 + (wsTerrainNoise(nxy * 0.17 + 7.3) - 0.5) * 0.25
            + (wsTerrainNoise(nxy * 0.55 + 3.1) - 0.5) * 0.15;
    float hz = z - corner + n * 2.0 * wolf_layer_noise;
    float w[8];
    w[0] = 1.0;
    for (int i = 1; i < 8; i++)
    {
        w[i] = 0.0;
        vec4 L = wolf_layer[i];
        if (L.w < 0.5) continue;
        float t = smoothstep(L.x - 0.5 * L.y, L.x + 0.5 * L.y, hz);
        for (int j = 0; j < i; j++) w[j] *= 1.0 - t;
        w[i] = t;
    }
    if (wolf_layer_cliff > -0.5)
    {
        float cw = smoothstep(0.22, 0.40, 1.0 - clamp(rn.z, 0.0, 1.0));
        int ci = int(wolf_layer_cliff + 0.5);
        for (int j = 0; j < 8; j++) w[j] *= 1.0 - cw;
        w[ci] += cw;
    }
    vec2 dX = dFdx(rxy), dY = dFdy(rxy);
    float dzX = dFdx(z), dzY = dFdy(z);
    vec3 tw = vec3(0.0, 0.0, 1.0);
    if (look)
    {
        tw = pow(abs(rn), vec3(6.0));
        tw /= (tw.x + tw.y + tw.z);
    }
    vec4 c = vec4(0.0);
    float ws = 0.0;
    for (int i = 0; i < 8; i++)
    {
        if (w[i] < 0.002) continue;
        float s = max(wolf_layer[i].z, 0.01);
        vec2 off = wolf_layer_off[i];
        float layer = float(i);
        vec4 col = wolfLayerTap(layer, (rxy + off) / s, dX / s, dY / s, farB) * tw.z;
        if (tw.x + tw.y > 0.002)
        {
            col += wolfLayerTap(layer, vec2(rxy.y + off.y, z) / s, vec2(dX.y, dzX) / s, vec2(dY.y, dzY) / s, farB) * tw.x;
            col += wolfLayerTap(layer, vec2(rxy.x + off.x, z) / s, vec2(dX.x, dzX) / s, vec2(dY.x, dzY) / s, farB) * tw.y;
        }
        c += col * w[i];
        ws += w[i];
    }
    return c / max(ws, 1e-4);
}
#else // WOLF_TERRAIN_LAYERS
const float wolf_layers_on = 0.0;
vec4 wolfLayerGround(vec3 region_pos, vec3 rn, float farB, bool look) { return vec4(0.0); }   // never reached: wolf_layers_on is 0
#endif // WOLF_TERRAIN_LAYERS
// </WolfViewer>

// <WolfViewer 2026-10-21> TUNNELS (wolffloatertunnels.cpp WolfTunnels::bindTerrain): no ground is drawn inside a region's
// tunnel boxes where the ground passes through them, so a tunnel's mouth is an open hole and the build inside shows (Paul on
// Moonfold: "there is no hole in the terrain"). Each box: centre (region metres) + turn about +Z in radians, half sizes. Only
// the visible surface and its depth are cut; the terrain's shadow pass is unchanged, so the hill still shades the mouth.
#define WOLF_TUNNELS_MAX 32
uniform int  wolf_tunnel_count;                    // 0 off a region with tunnels (no array is read then)
uniform vec4 wolf_tunnel_cy[WOLF_TUNNELS_MAX];     // xyz centre, w yaw
uniform vec4 wolf_tunnel_hs[WOLF_TUNNELS_MAX];     // xyz half sizes
void wolfTunnelClip(vec3 rp)
{
    for (int i = 0; i < WOLF_TUNNELS_MAX; ++i)
    {
        if (i >= wolf_tunnel_count) break;
        vec3 d = rp - wolf_tunnel_cy[i].xyz;
        float c = cos(wolf_tunnel_cy[i].w), s = sin(wolf_tunnel_cy[i].w);
        // local = R(-yaw) * d, as WolfTunnels inBox / WolfSim WolfUndergroundVolumes
        vec3 l = vec3(c * d.x + s * d.y, -s * d.x + c * d.y, d.z);
        if (all(lessThanEqual(abs(l), wolf_tunnel_hs[i].xyz))) discard;
    }
}
// </WolfViewer>

void mirrorClip(vec3 position);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

void main()
{
    mirrorClip(pos);
    wolfTunnelClip(vary_region_pos);   // <WolfViewer 2026-10-21/>
    /// Note: This should duplicate the blending functionality currently used for the terrain rendering.

    vec4 outColor;
    if (wolf_layers_on > 0.5)
    {
        // <WolfViewer 2026-10-16> the eight Wolf layers (wolfLayerGround above)
        float farB = wolf_terrain_look > 0.5 ? smoothstep(30.0, 140.0, length(pos)) * 0.65 : 0.0;
        outColor = wolfLayerGround(vary_region_pos, normalize(vary_region_normal), farB, wolf_terrain_look > 0.5);
    }
    else
    {
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
    }   // <WolfViewer 2026-10-16/> end of the stock four-texture path (wolf_layers_on == 0)

    // <WolfViewer 2026-09-10> painted roads/tracks: over the composition blend, under the
    // enhanced look's AO / snow (a road lies in its hollow and is snowed on like the ground).
    outColor.rgb = wolfTerrainPaint(outColor.rgb, vary_region_pos.xy);
    // </WolfViewer>

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

    // <WolfViewer 2026-09-18> snow that has settled from the weather
    if (wolf_snow_cover > 0.0)
    {
        vec2 suv = (vary_region_pos.xy - wolf_shelter_origin) / wolf_shelter_size;
        float open = (suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0) ? 1.0 : texture(wolfShelterMap, suv).r;
        float cn = wsTerrainNoise((vary_region_pos.xy + wolf_noise_offset) * 0.23);
        float csteep = 1.0 - clamp(vary_up, 0.0, 1.0);
        float cover = smoothstep(0.0, 0.6, wolf_snow_cover - cn * 0.35)
                      * (1.0 - smoothstep(0.30, 0.55, csteep))
                      * smoothstep(wolf_water_level - 0.2, wolf_water_level + 0.4, vary_region_pos.z) * open;
        outColor.rgb = mix(outColor.rgb, vec3(0.92, 0.94, 0.98), cover);
    }
    // </WolfViewer>

    // <WolfViewer 2026-09-06> caustics on the sea bed
    outColor.rgb *= 1.0 + wolfCaustic(vary_region_pos);
    // </WolfViewer>

    outColor.a = 0.0; // yes, downstream atmospherics

    frag_data[0] = max(outColor, vec4(0));
    frag_data[1] = vec4(0.0,0.0,0.0,-1.0);
    vec3 nvn = normalize(vary_normal);
    frag_data[2] = encodeNormal(nvn.xyz, 0, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}

