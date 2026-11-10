/**
 * @file wolfundersea.cpp
 * @brief WolfViewer: sea plants, schools of fish and, in warm seas, the odd shark under the water (ticket 652, Jimmy
 *        Olsen's "Undersea with better scenery"). See wolfundersea.h and docs/UNDERSEA_LIFE.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"
#include "wolfundersea.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "lldrawable.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llimage.h"
#include "llrand.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfboatrock.h"
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"

// Every figure below is either sourced in its comment or CHOSEN (a look or a budget, docs/UNDERSEA_LIFE.md).
namespace
{
    // ── the garden ──
    constexpr F32 CELL_M = 4.f;                  // CHOSEN: plants are decided per 4 m world cell
    constexpr F32 CHUNK_M = 32.f;                // CHOSEN: one vertex buffer per 32 m x 32 m
    constexpr S32 CELLS_PER_CHUNK = 8;           // CHUNK_M / CELL_M
    constexpr F32 BUILD_R = 72.f;                // CHOSEN: chunks within this (flat, nearest edge) are built and drawn
    constexpr F32 KEEP_R = 104.f;                // CHOSEN: built chunks are kept to here (no rebuild going to and fro)
    constexpr F64 IDLE_RELEASE_S = 20.0;         // CHOSEN: a chunk not wanted this long is released
    constexpr F64 INCOMPLETE_RETRY_S = 3.0;      // a chunk built before all its terrain came is built again after this
    constexpr F32 PLANT_ABOVE_M = 60.f;          // CHOSEN: plants while the camera is under the water or this far above it
    constexpr F32 FISH_ABOVE_M = 20.f;           // CHOSEN: fish the same, nearer the water
    constexpr F32 CAMERA_ONWARDS_M = 10.f;       // fish are dropped this much above FISH_ABOVE_M (no on/off flicker)
    constexpr U32 MAX_CHUNK_VERTS = 150000;      // CHOSEN: a guard; a dense meadow chunk is about 25 000
    // Paul 2026-10-26 "water should not appear on land", and its mirror: nothing of the sea bed shows above the water. A
    // plant's top stays this far under the region's water height, more in deeper water (bigger troughs in open sea).
    constexpr F32 TOP_CLEAR_MIN_M = 1.f, TOP_CLEAR_FRAC = 0.3f;

    // ── the objects' cells ──
    constexpr F32 BLOCK_M = 2.f;                 // CHOSEN: obstacle cells
    constexpr F32 SCAN_R = 120.f;                // obstacles are kept within this of the camera (beyond KEEP_R)
    constexpr S32 SCAN_PER_FRAME = 3000;         // Source: wolfbirds.cpp SCAN_PER_FRAME (the same object list, same pace)
    constexpr F32 TELEPORT_M = 300.f;            // Source: wolfsealife.cpp TELEPORT_M

    // ── the fish ──
    constexpr F64 DIRECT_SECS = 1.5;             // CHOSEN: how often schools are counted and placed
    constexpr F32 SPAWN_NEAR_M = 12.f, SPAWN_FAR_M = 45.f, GONE_M = 70.f, GONE_HIDDEN_M = 30.f;   // CHOSEN
    constexpr F32 SURFACE_CLEAR_M = 0.6f, BED_CLEAR_M = 0.4f;   // CHOSEN: fish keep under the drawn waves, over the sea bed
    constexpr S32 DEFAULT_FISH_MAX = 200, HARD_FISH_MAX = 400;
    constexpr F32 SHARK_CHANCE = 0.35f;          // CHOSEN ("rare"): the chance a dive into warm water has a shark
    constexpr F64 SHARK_ROLL_S = 120.0;          // and it is rolled again this often
    constexpr F32 SHARK_KEEP_M = 8.f;            // CHOSEN: a shark turns away from anyone nearer than this; never nearer
    constexpr F32 FISH_LOD_M = 18.f;
    constexpr F32 REEF_ROCK_TOP_M = 1.6f;        // buildChunk: a reef mound is at most 1.6 m over the bed

    struct FishSpec
    {
        F32 mLenLo, mLenHi;       // metres
        F32 mCruise, mBurst;      // m/s
        F32 mSep, mView;          // separation and neighbour radii, metres
        F32 mFlee;                // a swimmer nearer than this makes it flee (0: never flees; a shark keeps away instead)
        F32 mMinDepth;            // the water it needs, metres
        S32 mGroupLo, mGroupHi;
        F32 mWide;                // body width / height (fish are flattened sideways; a shark is rounder)
        F32 mRadius[7];           // body radius / length at STATIONS
    };
    // Sizes and speeds: CHOSEN to read as the common kinds, not any one species (small schooling fish swim ~1 body length a
    // second cruising and dart several times that; a reef shark of 2-2.8 m cruises slowly, about 1 m/s).
    const F32 STATIONS[7] = { 0.f, 0.08f, 0.2f, 0.38f, 0.58f, 0.78f, 0.9f };
    const FishSpec FISH[WolfUnderSea::FISH_KINDS] = {
        // silver schooling fish (everywhere)
        { 0.16f, 0.24f, 0.8f, 3.2f, 0.35f, 2.5f, 4.0f, 2.5f, 15, 35, 0.45f, { 0.f, 0.06f, 0.095f, 0.10f, 0.08f, 0.045f, 0.022f } },
        // reef fish (warm seas): deeper bodies, slower
        { 0.10f, 0.25f, 0.45f, 2.4f, 0.30f, 1.8f, 3.0f, 2.5f, 10, 24, 0.38f, { 0.f, 0.09f, 0.16f, 0.18f, 0.15f, 0.08f, 0.03f } },
        // big fish near the bottom (a grouper in warm seas, a cod-like fish in cold): alone or a pair
        { 0.6f, 1.0f, 0.45f, 1.8f, 1.5f, 4.0f, 3.5f, 4.0f, 1, 2, 0.6f, { 0.f, 0.08f, 0.12f, 0.13f, 0.11f, 0.06f, 0.03f } },
        // shark (warm seas only)
        { 2.0f, 2.8f, 1.0f, 1.4f, 3.0f, 6.f, 0.f, 6.0f, 1, 1, 0.85f, { 0.f, 0.05f, 0.075f, 0.085f, 0.07f, 0.04f, 0.02f } },
        // [COLD SEAS 2026-11-09] cod, pollock, saithe (cool and cold seas): bigger than the silver fish, loose schools of
        // 6-16 through the kelp; CHOSEN 0.35-0.65 m, cruising about a body length a second
        { 0.35f, 0.65f, 0.55f, 2.2f, 0.9f, 3.5f, 4.0f, 3.0f, 6, 16, 0.5f, { 0.f, 0.07f, 0.11f, 0.12f, 0.10f, 0.055f, 0.025f } },
    };

    // ── helpers ──
    U64 mix64(U64 x)
    {
        // splitmix64's finaliser (Steele, Lea, Flood 2014): a good 64-bit hash of an integer
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }
    U64 hash2(S64 a, S64 b, U64 salt) { return mix64((U64)a ^ mix64((U64)b ^ mix64(salt))); }
    F32 unit(U64 h) { return (F32)((h >> 40) & 0xFFFFFFULL) / 16777216.f; }

    struct Rng
    {
        U64 mS;
        explicit Rng(U64 s) : mS(s) {}
        F32 next() { mS = mix64(mS); return unit(mS); }
        F32 range(F32 lo, F32 hi) { return lo + (hi - lo) * next(); }
        S32 irange(S32 lo, S32 hi) { return lo + llmin(hi - lo, (S32)(next() * (F32)(hi - lo + 1))); }
    };

    F32 smoothstep(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    /** Value noise in world metres (0..1): smooth patches `scale` metres across, the same for everyone. */
    F32 noise(F64 gx, F64 gy, F64 scale, U64 salt)
    {
        const F64 x = gx / scale, y = gy / scale;
        const F64 fx0 = floor(x), fy0 = floor(y);
        const S64 ix = (S64)fx0, iy = (S64)fy0;
        const F32 fx = (F32)(x - fx0), fy = (F32)(y - fy0);
        const F32 sx = fx * fx * (3.f - 2.f * fx), sy = fy * fy * (3.f - 2.f * fy);
        const F32 a = unit(hash2(ix, iy, salt)), b = unit(hash2(ix + 1, iy, salt));
        const F32 c = unit(hash2(ix, iy + 1, salt)), d = unit(hash2(ix + 1, iy + 1, salt));
        return (a + (b - a) * sx) * (1.f - sy) + (c + (d - c) * sx) * sy;
    }

    LLColor4U rgb(F32 r, F32 g, F32 b)
    {
        return LLColor4U((U8)(llclamp(r, 0.f, 1.f) * 255.f), (U8)(llclamp(g, 0.f, 1.f) * 255.f), (U8)(llclamp(b, 0.f, 1.f) * 255.f), 255);
    }
    LLColor4U mixc(const F32* a, const F32* b, F32 t)
    {
        return rgb(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
    }

    /** Source: wolfbirds.cpp earth_sky - a sky with no blue and no haze is space (or a black void): nothing lives there. */
    bool earth_sky()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return true;
        const LLColor3 bd = sky->getBlueDensity(), bh = sky->getBlueHorizon();
        const F32 blue = llmax(llmax(bd.mV[0], bd.mV[1]), llmax(bd.mV[2], llmax(bh.mV[0], llmax(bh.mV[1], bh.mV[2]))));
        return !(blue < 0.02f && sky->getHazeDensity() < 0.02f);
    }

    S64 cell_of(F64 g, F32 size) { return (S64)floor(g / (F64)size); }

    // ── the plants' vertices (chunk-local) ──
    /** One vertex of a plant: position, normal, its place in the atlas (texcoord0), its sway (texcoord1, wolfUnderSeaV.glsl:
     *  x = metres this vertex may move, y = n + phase with n in 1/60 Hz) and its colour. */
    struct PV { LLVector3 mP, mN; LLVector2 mUV, mSway; LLColor4U mC; };

    LLVector2 sway(F32 metres, S32 n60, F32 phase01) { return LLVector2(metres, (F32)n60 + llclamp(phase01, 0.f, 0.999f)); }
    const LLVector2 STILL(0.f, 1.f);   // no sway (1/60 Hz, 0 m)

    struct PlantWriter
    {
        std::vector<PV>& mV;
        explicit PlantWriter(std::vector<PV>& v) : mV(v) {}
        void tri(const PV& a, const PV& b, const PV& c) { mV.push_back(a); mV.push_back(b); mV.push_back(c); }
    };

    /** A thin plant's normal: mostly up, a little towards its face, so both sides of a blade are lit alike (the plants are
     *  drawn without back-face culling). */
    LLVector3 leaf_normal(const LLVector3& face)
    {
        LLVector3 n = face * 0.35f + LLVector3(0.f, 0.f, 0.95f);
        n.normVec();
        return n;
    }

    // ── the atlas: our own textures, drawn here in code from value noise (no image file, nothing copied) ──
    // [LOOK 2026-11-09] Paul: "it doesn't look right - look at some pictures of the undersea and emulate it". Reference
    // photographs looked at (reference only, nothing taken from them): Wikimedia Commons "Snorkeling Bari Reef, Bonaire"
    // (12840799335, 12841304654) and "Snorkeling Karpata Reef, Bonaire" (12841929245), CC BY 2.0; "Thalassia-Syringodium
    // seagrass bed (Graham's Harbour, San Salvador Island, Bahamas)" (15667912398), CC BY 2.0. What they show and this
    // code draws: rough mottled rock carrying the corals, grooved brain corals, pitted (polyp) coral and sponge surfaces,
    // lace-like sea fans, veined grass blades.
    enum Tile : S32 { T_WHITE = 0, T_ROCK = 1, T_BRAIN = 2, T_POLYP = 3, T_LATTICE = 4, T_BLADE = 5, T_KELP = 6, T_SPONGE = 7, T_SCALES = 8 };
    constexpr S32 ATLAS_GRID = 4, TILE_PX = 256, ATLAS_PX = ATLAS_GRID * TILE_PX, TILE_PAD = 10;

    /** u, v in [0, 1] on a tile -> the atlas (texcoord0). Each tile keeps a margin so the mip levels do not bleed. */
    LLVector2 atlas_uv(S32 tile, F32 u, F32 v)
    {
        const F32 in = (F32)TILE_PAD / (F32)TILE_PX;
        const F32 tx = (F32)(tile % ATLAS_GRID), ty = (F32)(tile / ATLAS_GRID);
        const F32 uu = in + llclamp(u, 0.f, 1.f) * (1.f - 2.f * in), vv = in + llclamp(v, 0.f, 1.f) * (1.f - 2.f * in);
        return LLVector2((tx + uu) / (F32)ATLAS_GRID, (ty + vv) / (F32)ATLAS_GRID);
    }

    /** Value noise that repeats every `period` lattice steps (so a tile's pattern has no seam inside it). */
    F32 pnoise(F32 x, F32 y, S32 period, U64 salt)
    {
        const F32 fx0 = floorf(x), fy0 = floorf(y);
        const S32 ix = (S32)fx0, iy = (S32)fy0;
        const F32 fx = x - fx0, fy = y - fy0;
        const F32 sx = fx * fx * (3.f - 2.f * fx), sy = fy * fy * (3.f - 2.f * fy);
        auto h = [period, salt](S32 a, S32 b) { return unit(hash2((S64)(((a % period) + period) % period), (S64)(((b % period) + period) % period), salt)); };
        const F32 a = h(ix, iy), b = h(ix + 1, iy), c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
        return (a + (b - a) * sx) * (1.f - sy) + (c + (d - c) * sx) * sy;
    }
    F32 fbm(F32 u, F32 v, S32 period, S32 octaves, U64 salt)
    {
        F32 sum = 0.f, amp = 0.5f, norm = 0.f;
        S32 p = period;
        for (S32 o = 0; o < octaves; ++o)
        {
            sum += amp * pnoise(u * (F32)p, v * (F32)p, p, salt + (U64)o);
            norm += amp;
            amp *= 0.5f;
            p *= 2;
        }
        return sum / norm;
    }
    /** Distance to the nearest jittered point of an n x n grid (a cellular pattern: polyp cups, sponge pores, scales). */
    F32 cell_dist(F32 u, F32 v, S32 n, U64 salt)
    {
        const F32 x = u * (F32)n, y = v * (F32)n;
        const S32 ix = (S32)floorf(x), iy = (S32)floorf(y);
        F32 best = 9.f;
        for (S32 dy = -1; dy <= 1; ++dy)
        {
            for (S32 dx = -1; dx <= 1; ++dx)
            {
                const S32 cx = ix + dx, cy = iy + dy;
                const U64 h = hash2((S64)(((cx % n) + n) % n), (S64)(((cy % n) + n) % n), salt);
                const F32 px = (F32)cx + 0.15f + 0.7f * unit(h), py = (F32)cy + 0.15f + 0.7f * unit(mix64(h));
                best = llmin(best, sqrtf((x - px) * (x - px) + (y - py) * (y - py)));
            }
        }
        return best;
    }

    /** One texel of a tile: grey (multiplies the vertex colour) and alpha (only the sea fan's lace cuts holes). */
    void texel(S32 tile, F32 u, F32 v, F32& grey, F32& alpha)
    {
        alpha = 1.f;
        switch (tile)
        {
            case T_ROCK:
            {
                const F32 n = fbm(u, v, 6, 5, 101ULL), pits = cell_dist(u, v, 20, 103ULL);
                grey = 0.55f + 0.45f * n;
                if (pits < 0.18f) grey *= 0.65f + 0.35f * pits / 0.18f;   // small holes in the limestone
                break;
            }
            case T_BRAIN:
            {
                // [LOOK 2026-11-09, round 3] a labyrinth of evenly spaced ridges (the first version, contours of a noise field,
                // read as wood grain): stripes about 1/30 of the tile apart whose direction wanders with a slow noise, narrow
                // dark grooves between broad ridges, each ridge lit on one side and shaded on the other (relief in the texture:
                // this pass has no normal map)
                const F32 theta = fbm(u, v, 3, 3, 111ULL) * F_TWO_PI * 1.2f;
                const F32 phase = (u * cosf(theta) + v * sinf(theta)) * 30.f + fbm(u, v, 6, 2, 113ULL) * 1.5f;
                const F32 f = phase - floorf(phase);
                const F32 ridge = 0.5f - fabsf(f - 0.5f);   // 0 in a groove, 0.5 on a ridge's crest
                grey = 0.30f + 0.62f * smoothstep(0.03f, 0.2f, ridge) + 0.10f * sinf(F_TWO_PI * f);
                break;
            }
            case T_POLYP:
            {
                const F32 d = cell_dist(u, v, 28, 121ULL);
                grey = (0.62f + 0.38f * smoothstep(0.1f, 0.38f, d)) * (0.85f + 0.15f * fbm(u, v, 5, 3, 123ULL));
                break;
            }
            case T_LATTICE:
            {
                // ribs radiating along v and cross-links across them, wavy: about two thirds solid, so the lace still
                // passes the 0.5 alpha mask in the smaller mip levels (it then reads as a solid fan far off)
                const F32 rib = fmodf(u * 16.f + 0.25f * sinf(v * 11.f), 1.f), link = fmodf(v * 20.f + 0.2f * sinf(u * 13.f), 1.f);
                alpha = (rib < 0.42f || link < 0.38f) ? 1.f : 0.f;
                grey = 0.82f + 0.18f * fbm(u, v, 6, 2, 131ULL);
                break;
            }
            case T_BLADE:
            {
                // fine lengthwise veins (u across the blade) and pale specks of growth on it
                grey = 0.80f + 0.10f * sinf(u * F_TWO_PI * 7.f) + 0.06f * fbm(u, v, 4, 3, 141ULL);
                if (fbm(u, v, 12, 2, 143ULL) > 0.72f) grey = 0.97f;
                break;
            }
            case T_KELP:
                grey = 0.70f + 0.18f * fbm(u, v * 3.f, 3, 4, 151ULL) + 0.10f * sinf(v * 70.f + fbm(u, v, 4, 2, 153ULL) * 8.f);
                break;
            case T_SPONGE:
            {
                const F32 d = cell_dist(u, v, 14, 161ULL);
                grey = (d < 0.2f ? 0.45f + 0.35f * d / 0.2f : 0.80f) + 0.15f * (fbm(u, v, 6, 3, 163ULL) - 0.5f);
                break;
            }
            case T_SCALES:
            {
                const F32 d = cell_dist(u, v * 0.6f, 22, 171ULL);
                grey = 0.86f + 0.14f * smoothstep(0.25f, 0.5f, d);
                break;
            }
            default:
                grey = 1.f;
                break;
        }
        grey = llclamp(grey, 0.f, 1.f);
    }

    /** Rows y0 .. y1-1 of the atlas (ATLAS_PX square, RGBA), each tile from texel(); a tile's margin repeats its edge. */
    void fill_atlas_rows(LLImageRaw* raw, S32 y0, S32 y1)
    {
        U8* data = raw->getData();
        if (!data) return;
        const F32 in = (F32)TILE_PAD / (F32)TILE_PX;
        for (S32 y = y0; y < y1 && y < ATLAS_PX; ++y)
        {
            for (S32 x = 0; x < ATLAS_PX; ++x)
            {
                const S32 tile = (y / TILE_PX) * ATLAS_GRID + (x / TILE_PX);
                const F32 u = llclamp(((F32)(x % TILE_PX) / (F32)TILE_PX - in) / (1.f - 2.f * in), 0.f, 1.f);
                const F32 v = llclamp(((F32)(y % TILE_PX) / (F32)TILE_PX - in) / (1.f - 2.f * in), 0.f, 1.f);
                F32 grey = 1.f, alpha = 1.f;
                texel(tile, u, v, grey, alpha);
                U8* p = data + ((size_t)y * ATLAS_PX + (size_t)x) * 4;
                p[0] = p[1] = p[2] = (U8)(grey * 255.f);
                p[3] = (U8)(alpha * 255.f);
            }
        }
    }
}

size_t WolfUnderSea::CellKeyHash::operator()(const CellKey& k) const
{
    return (size_t)hash2(k.first, k.second, 0x5EA5EAULL);
}

WolfUnderSea::WolfUnderSea() {}
WolfUnderSea::~WolfUnderSea() {}

void WolfUnderSea::reset()
{
    mChunks.clear();
    mBlocks.clear();
    mBlocksScan.clear();
    mScanIndex = 0;
    mScanDone = false;
    mCheckedGeneration = mScanGeneration;
    mFish.clear();
    mSchools.clear();
    mSharkDecided = false;
    mSharkWanted = false;
    mNextDirect = 0.0;
}

void WolfUnderSea::rebase(const LLVector3& delta)
{
    for (Fish& f : mFish) f.mPos += delta;
    for (School& s : mSchools)
    {
        s.mGoal += delta;
        s.mAlarmFrom += delta;
        s.mHome.mV[VX] += delta.mV[VX];
        s.mHome.mV[VY] += delta.mV[VY];
        s.mHome.mV[VZ] += delta.mV[VZ];
    }
    mLastCamera += delta;
}

bool WolfUnderSea::wanted(LLViewerRegion*& region) const
{
    region = gAgent.getRegion();
    if (!region) return false;
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerUnderSea", true);
    if (!on) return false;
    if (!WolfGrid::showsWolfRegionWeather()) return false;   // Wolf regions only, visitors included (wolfgrid.h)
    if (!WolfRegionWeather::instanceExists() || !WolfRegionWeather::instance().underSeaAllowed()) return false;
    return earth_sky();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the place (the sea-life rules: wolfsealife.cpp depth / neighbourAt)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfUnderSea::water() const
{
    LLViewerRegion* r = gAgent.getRegion();
    return r ? r->getWaterHeight() : 20.f;
}

LLViewerRegion* WolfUnderSea::regionFor(const LLVector3& agent) const
{
    LLViewerRegion* here = gAgent.getRegion();
    if (!here) return nullptr;
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(agent);
    if (!r) return nullptr;
    if (r == here) return r;
    // a neighbour: only when its own switch is on (undersea.near) and its sea is at this region's height (Source:
    // wolfsealife.cpp neighbourAt, the same rule for the dolphins)
    const bool ok = WolfRegionWeather::instanceExists() && WolfRegionWeather::instance().underSeaNear(r->getRegionID())
                    && fabsf(r->getWaterHeight() - here->getWaterHeight()) <= 0.01f;
    return ok ? r : nullptr;
}

/** Source: wolfsealife.cpp depth() (with the 2026-11-08 aground fix): the water over the terrain, 0 on land, 0 where the
 *  patch's terrain has not come (an empty patch reads 0 m: deep water everywhere), 0 over a sea that may not hold them. */
F32 WolfUnderSea::depth(const LLVector3& agent) const
{
    LLViewerRegion* r = regionFor(agent);
    if (!r) return 0.f;
    const LLVector3 pos_region = agent - r->getOriginAgent();
    if (r->getLand().getPatchesPerEdge() <= 0) return 0.f;
    const LLSurfacePatch* patch = r->getLand().resolvePatchRegion(pos_region);
    if (!patch || !patch->getHasReceivedData()) return 0.f;
    const F32 h = r->getLand().resolveHeightRegion(pos_region);
    return std::isfinite(h) ? llmax(0.f, water() - h) : 0.f;
}

F32 WolfUnderSea::seabed(const LLVector3& agent) const
{
    const F32 d = depth(agent);
    return d > 0.f ? water() - d : -1e9f;
}

/** What the garden was built for: the agent's region, its climate and the neighbours that may hold plants. A change
 *  rebuilds the chunks (nearest first). */
U32 WolfUnderSea::climateStamp() const
{
    if (!WolfRegionWeather::instanceExists()) return 0;
    const WolfRegionWeather& rw = WolfRegionWeather::instance();
    U64 h = mix64((U64)(rw.underSeaWarm() * 100.f + 0.5f)) ^ mix64(0x1000ULL + (U64)(rw.underSeaCold() * 100.f + 0.5f));
    if (LLViewerRegion* r = gAgent.getRegion())
    {
        const LLUUID& id = r->getRegionID();
        h = mix64(h ^ (U64)id.getCRC32());
        // the neighbours round the agent's region: whether each may hold plants
        for (LLViewerRegion* n : LLWorld::getInstance()->getRegionList())
        {
            if (n == r || !n) continue;
            if (rw.underSeaNear(n->getRegionID())) h = mix64(h ^ (U64)n->getRegionID().getCRC32());
        }
        h = mix64(h ^ (U64)(S64)ll_round(r->getWaterHeight() * 100.f));
    }
    return (U32)(h ^ (h >> 32));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the objects' cells: nothing grows in or swims through a build
// ═══════════════════════════════════════════════════════════════════════════════════════

/** One slice of the object list a frame (wolfbirds.cpp scan: SCAN_PER_FRAME); a whole pass replaces the obstacle map. Every
 *  prim's drawable extents (agent box, LLViewerOctreeEntryData::getSpatialExtents) that reaches under the water marks the
 *  2 m cells under it with its height range. Avatars and attachments are not obstacles (fish flee swimmers instead). */
void WolfUnderSea::scanObjects(const LLVector3& cam)
{
    const S32 n = gObjectList.getNumObjects();
    if (mScanIndex == 0)
    {
        mBlocksScan.clear();
        mScanCentre = gAgent.getPosGlobalFromAgent(cam);
    }
    const F32 top = water() + 0.5f;
    const S32 end = llmin(n, mScanIndex + SCAN_PER_FRAME);
    const F64 cx = mScanCentre.mdV[VX], cy = mScanCentre.mdV[VY];
    for (S32 i = mScanIndex; i < end; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || obj->mDrawable.isNull()) continue;
        // prims and meshes only (Source: wolfbirds.cpp scan, the same test; LL_PCODE_VOLUME = 9, llvolume.h:102): the
        // region's own water, void water and sky objects are in this list too, and their boxes span the whole sea (seen
        // 2026-11-09 at The Caribbean: every cell within SCAN_R blocked, no plant grew and no school could be placed)
        if (obj->getPCode() != LL_PCODE_VOLUME) continue;
        const LLVector4a* ext = obj->mDrawable->getSpatialExtents();
        if (!ext) continue;
        const F32* lo = ext[0].getF32ptr();
        const F32* hi = ext[1].getF32ptr();
        if (!std::isfinite(lo[0]) || !std::isfinite(hi[0]) || !std::isfinite(lo[2]) || !std::isfinite(hi[2])) continue;
        if (lo[2] > top || hi[0] < lo[0] || hi[1] < lo[1]) continue;   // wholly above the water, or no box yet
        const LLVector3d glo = gAgent.getPosGlobalFromAgent(LLVector3(lo[0], lo[1], 0.f));
        const LLVector3d ghi = gAgent.getPosGlobalFromAgent(LLVector3(hi[0], hi[1], 0.f));
        const F64 x0 = llmax(glo.mdV[VX], cx - SCAN_R), x1 = llmin(ghi.mdV[VX], cx + SCAN_R);
        const F64 y0 = llmax(glo.mdV[VY], cy - SCAN_R), y1 = llmin(ghi.mdV[VY], cy + SCAN_R);
        if (x0 > x1 || y0 > y1) continue;
        for (S64 bx = cell_of(x0, BLOCK_M); bx <= cell_of(x1, BLOCK_M); ++bx)
        {
            for (S64 by = cell_of(y0, BLOCK_M); by <= cell_of(y1, BLOCK_M); ++by)
            {
                Block& b = mBlocksScan[CellKey(bx, by)];
                b.mZMin = llmin(b.mZMin, lo[2]);
                b.mZMax = llmax(b.mZMax, hi[2]);
            }
        }
    }
    mScanIndex = end;
    if (end >= n)
    {
        mBlocks.swap(mBlocksScan);
        mBlocksScan.clear();
        mScanIndex = 0;
        mScanDone = true;
        ++mScanGeneration;
    }
}

bool WolfUnderSea::blocked(F64 gx, F64 gy, F32 radius, F32 zlo, F32 zhi) const
{
    if (mBlocks.empty()) return false;
    const F64 r = (F64)radius;
    for (S64 bx = cell_of(gx - r, BLOCK_M); bx <= cell_of(gx + r, BLOCK_M); ++bx)
    {
        for (S64 by = cell_of(gy - r, BLOCK_M); by <= cell_of(gy + r, BLOCK_M); ++by)
        {
            auto it = mBlocks.find(CellKey(bx, by));
            if (it != mBlocks.end() && it->second.mZMin <= zhi && it->second.mZMax >= zlo) return true;
        }
    }
    return false;
}

U64 WolfUnderSea::blockHash(const LLVector3d& corner) const
{
    const S64 bx0 = cell_of(corner.mdV[VX], BLOCK_M), by0 = cell_of(corner.mdV[VY], BLOCK_M);
    const S64 per = (S64)(CHUNK_M / BLOCK_M);
    U64 h = 0;
    for (S64 bx = bx0; bx < bx0 + per; ++bx)
    {
        for (S64 by = by0; by < by0 + per; ++by)
        {
            auto it = mBlocks.find(CellKey(bx, by));
            if (it == mBlocks.end()) continue;
            h ^= hash2(bx, by, (U64)(S64)floorf(it->second.mZMin * 4.f) * 1315423911ULL + (U64)(S64)floorf(it->second.mZMax * 4.f));
        }
    }
    return h;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the plants (our own procedural shapes, colours and atlas; no third-party models or images)
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    const LLVector3 UP(0.f, 0.f, 1.f);

    LLColor4U col3(const F32* c, F32 k = 1.f) { return rgb(c[0] * k, c[1] * k, c[2] * k); }
    /** A colour varied a little per plant (the same plant always the same). */
    void vary(const F32* base, Rng& r, F32 amount, F32* out)
    {
        const F32 k = 1.f + r.range(-amount, amount);
        for (S32 i = 0; i < 3; ++i) out[i] = llclamp(base[i] * k + r.range(-amount, amount) * 0.3f, 0.f, 1.f);
    }

    /** Seagrass: a tuft of 5-7 long thin ribbon blades in three segments, bending out; sways at 0.37-0.47 Hz (CHOSEN). The
     *  reference meadow (turtle grass) is a dense olive-green sward with paler, sometimes brown tips. */
    void plant_seagrass(PlantWriter& w, const LLVector3& base, F32 h, Rng& r)
    {
        static const F32 LO[3] = { 0.20f, 0.30f, 0.12f }, TIP[3] = { 0.46f, 0.50f, 0.22f }, DEAD[3] = { 0.48f, 0.40f, 0.22f };
        const S32 blades = r.irange(5, 7);
        const S32 n60 = r.irange(22, 28);
        F32 lo[3], tip[3];
        vary(LO, r, 0.12f, lo);
        vary(r.next() < 0.2f ? DEAD : TIP, r, 0.1f, tip);
        const F32 bend = 0.12f * h / 0.5f;
        for (S32 b = 0; b < blades; ++b)
        {
            const F32 ang = r.range(0.f, F_TWO_PI);
            const LLVector3 dir(cosf(ang), sinf(ang), 0.f), side(-sinf(ang), cosf(ang), 0.f);
            const F32 lean = r.range(0.1f, 0.4f) * h, bh = h * r.range(0.7f, 1.15f), wd = r.range(0.006f, 0.011f);
            const F32 phase = r.next();
            const LLVector3 root = base + dir * r.range(0.f, 0.07f);
            auto at = [&](F32 t) { return root + dir * (lean * t * t) + UP * (bh * t); };
            const LLVector3 n = leaf_normal(side);
            const F32 T[3] = { 0.f, 0.45f, 0.8f };
            PV prev0{}, prev1{};
            for (S32 k = 0; k < 3; ++k)
            {
                const F32 t = T[k], wk = wd * (1.f - 0.3f * t);
                const LLColor4U c = mixc(lo, tip, t);
                const PV a0{ at(t) - side * wk, n, atlas_uv(T_BLADE, 0.f, t), sway(bend * t * t, n60, phase), c };
                const PV a1{ at(t) + side * wk, n, atlas_uv(T_BLADE, 1.f, t), sway(bend * t * t, n60, phase), c };
                if (k > 0) { w.tri(prev0, prev1, a1); w.tri(prev0, a1, a0); }
                prev0 = a0; prev1 = a1;
            }
            const PV tp{ at(1.f), n, atlas_uv(T_BLADE, 0.5f, 1.f), sway(bend, n60, phase), mixc(lo, tip, 1.f) };
            w.tri(prev0, prev1, tp);
        }
    }

    /** Kelp (cool seas): a stipe of two crossed ribbons rising `h` metres with a frond at each joint and a few at the top;
     *  sways slowly, 0.12-0.15 Hz (CHOSEN: a kelp forest moves with the swell's period, several seconds). */
    void plant_kelp(PlantWriter& w, const LLVector3& base, F32 h, Rng& r)
    {
        static const F32 STIPE[3] = { 0.30f, 0.25f, 0.11f }, LEAF0[3] = { 0.40f, 0.34f, 0.13f }, LEAF1[3] = { 0.55f, 0.46f, 0.18f };
        const S32 segs = llclamp((S32)ceilf(h / 1.0f), 3, 12);
        const S32 n60 = r.irange(7, 9);
        const F32 phase = r.next();
        F32 stipe[3], leaf0[3], leaf1[3];
        vary(STIPE, r, 0.08f, stipe); vary(LEAF0, r, 0.1f, leaf0); vary(LEAF1, r, 0.1f, leaf1);
        const F32 la = r.range(0.f, F_TWO_PI), lean = r.range(0.04f, 0.12f) * h;
        const LLVector3 ldir(cosf(la), sinf(la), 0.f);
        const F32 bend = 0.12f * h;
        auto at = [&](F32 t) { return base + ldir * (lean * t * t) + UP * (h * t); };
        auto sw = [&](F32 t) { return bend * powf(t, 1.5f); };
        for (S32 ribbon = 0; ribbon < 2; ++ribbon)
        {
            const F32 a = la + (ribbon ? F_PI_BY_TWO : 0.f);
            const LLVector3 side(cosf(a), sinf(a), 0.f);
            const LLVector3 n = leaf_normal(LLVector3(-side.mV[VY], side.mV[VX], 0.f));
            for (S32 k = 0; k < segs; ++k)
            {
                const F32 t0 = (F32)k / (F32)segs, t1 = (F32)(k + 1) / (F32)segs;
                const F32 w0 = 0.04f * (1.f - 0.4f * t0), w1 = 0.04f * (1.f - 0.4f * t1);
                const LLColor4U c0 = mixc(stipe, leaf0, t0 * 0.5f), c1 = mixc(stipe, leaf0, t1 * 0.5f);
                const PV a0{ at(t0) - side * w0, n, atlas_uv(T_KELP, 0.f, t0), sway(sw(t0), n60, phase), c0 };
                const PV a1{ at(t0) + side * w0, n, atlas_uv(T_KELP, 0.2f, t0), sway(sw(t0), n60, phase), c0 };
                const PV b0{ at(t1) - side * w1, n, atlas_uv(T_KELP, 0.f, t1), sway(sw(t1), n60, phase), c1 };
                const PV b1{ at(t1) + side * w1, n, atlas_uv(T_KELP, 0.2f, t1), sway(sw(t1), n60, phase), c1 };
                w.tri(a0, a1, b1);
                w.tri(a0, b1, b0);
            }
        }
        const S32 fronds = segs + 3;
        for (S32 k = 1; k <= fronds; ++k)
        {
            const F32 t = llmin(1.f, (F32)llmin(k, segs) / (F32)segs - (k > segs ? 0.02f * (F32)(k - segs) : 0.f));
            const F32 a = la + (F32)k * 2.39996f;   // the golden angle, radians
            const LLVector3 out(cosf(a), sinf(a), 0.f), side(-sinf(a), cosf(a), 0.f);
            const F32 len = r.range(0.6f, 1.1f) * llmin(1.f, h / 4.f + 0.4f), wd = r.range(0.10f, 0.16f);
            const LLVector3 root = at(t);
            const LLVector3 mid = root + out * (len * 0.35f) + UP * (len * 0.35f);
            const LLVector3 tipp = root + out * (len * 0.7f) + UP * (len * 0.65f);
            const LLVector3 n = leaf_normal(UP % side);
            const F32 s0 = sw(t), s1 = s0 + 0.08f, s2 = s0 + 0.15f;
            const PV pr{ root, n, atlas_uv(T_KELP, 0.5f, 0.f), sway(s0, n60, phase), mixc(leaf0, leaf1, 0.f) };
            const PV pl{ mid - side * wd, n, atlas_uv(T_KELP, 0.f, 0.5f), sway(s1, n60, phase), mixc(leaf0, leaf1, 0.5f) };
            const PV pm{ mid + side * wd, n, atlas_uv(T_KELP, 1.f, 0.5f), sway(s1, n60, phase), mixc(leaf0, leaf1, 0.5f) };
            const PV pt{ tipp, n, atlas_uv(T_KELP, 0.5f, 1.f), sway(s2, n60, phase), mixc(leaf0, leaf1, 1.f) };
            w.tri(pr, pm, pt);
            w.tri(pr, pt, pl);
        }
    }

    /** A tapered limb from a to b (radii ra, rb), `sides` faces, smooth-shaded, a point at its end; sway weights at a and b. */
    void limb(PlantWriter& w, const LLVector3& a, const LLVector3& b, F32 ra, F32 rb, const LLColor4U& ca, const LLColor4U& cb,
              S32 sides, const LLVector2& swa, const LLVector2& swb, S32 tile = T_POLYP)
    {
        LLVector3 ax = b - a;
        const F32 len = ax.normVec();
        if (len <= 0.f) return;
        LLVector3 u = (fabsf(ax.mV[VZ]) < 0.9f ? UP : LLVector3(1.f, 0.f, 0.f)) % ax;
        u.normVec();
        const LLVector3 v = ax % u;
        for (S32 k = 0; k < sides; ++k)
        {
            const F32 th0 = (F32)k * F_TWO_PI / (F32)sides, th1 = (F32)(k + 1) * F_TWO_PI / (F32)sides;
            const LLVector3 d0 = u * cosf(th0) + v * sinf(th0), d1 = u * cosf(th1) + v * sinf(th1);
            const F32 u0 = (F32)k / (F32)sides, u1 = (F32)(k + 1) / (F32)sides;
            const PV a0{ a + d0 * ra, d0, atlas_uv(tile, u0, 0.f), swa, ca }, a1{ a + d1 * ra, d1, atlas_uv(tile, u1, 0.f), swa, ca };
            const PV b0{ b + d0 * rb, d0, atlas_uv(tile, u0, 1.f), swb, cb }, b1{ b + d1 * rb, d1, atlas_uv(tile, u1, 1.f), swb, cb };
            w.tri(a0, a1, b1);
            w.tri(a0, b1, b0);
            const PV tip{ b + ax * (rb * 1.2f), ax, atlas_uv(tile, (u0 + u1) * 0.5f, 1.f), swb, cb };
            w.tri(b0, b1, tip);
        }
    }

    /** A lumpy dome: the reef's rock (T_ROCK), the brain corals (T_BRAIN) and the stones. rx/ry the radii across, hz its
     *  height. [LOOK 2026-11-09] The lumps are smooth noise over the surface (fbm of the direction; the first version's
     *  per-vertex random bumps made a faceted slab), `ledges` pushes out bands that overhang like eroded limestone shelves,
     *  `patches` (4 colours, or nullptr) lays encrusting colour patches over it; the normals are the displaced surface's. */
    void dome(PlantWriter& w, const LLVector3& base, F32 rx, F32 ry, F32 hz, S32 sides, S32 rings, F32 bumps, S32 tile,
              const F32* c_low, const F32* c_top, F32 mottle, U64 seed, F32 ledges = 0.f, const F32 (*patches)[3] = nullptr)
    {
        const S32 cols = sides, rows = rings + 1;
        std::vector<LLVector3> P((size_t)(rows * cols)), N(P.size());
        std::vector<LLColor4U> C(P.size());
        auto shape = [&](S32 i, S32 k, F32& bump, F32& lip)
        {
            const F32 u = (F32)k / (F32)cols, v = (F32)i / (F32)rings;
            const F32 n = fbm(u, v * 0.5f, 4, 3, seed) * 2.f - 1.f;
            bump = 1.f + bumps * n;
            const F32 band = sinf(v * F_PI * 3.f + n * 2.f);
            lip = 1.f + ledges * 0.18f * powf(llmax(0.f, band), 6.f);
        };
        F32 pole_bump = 1.f, pole_lip = 1.f;
        shape(rings, 0, pole_bump, pole_lip);
        for (S32 i = 0; i < rows; ++i)
        {
            const F32 el = -0.25f + (F_PI_BY_TWO + 0.25f) * (F32)i / (F32)rings;   // from under the bed to the top
            for (S32 k = 0; k < cols; ++k)
            {
                const F32 az = (F32)k * F_TWO_PI / (F32)cols;
                F32 bump = pole_bump, lip = pole_lip;
                if (i < rings) shape(i, k, bump, lip);
                const LLVector3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
                const size_t idx = (size_t)(i * cols + k);
                P[idx] = base + LLVector3(dir.mV[VX] * rx * bump * lip, dir.mV[VY] * ry * bump * lip, dir.mV[VZ] * hz * bump);
                const U64 h = mix64(seed ^ (U64)(i * 977 + k));
                const F32 t = llclamp(sinf(el), 0.f, 1.f), m = 1.f + mottle * (unit(h) * 2.f - 1.f);
                F32 c[3];
                for (S32 j = 0; j < 3; ++j) c[j] = (c_low[j] + (c_top[j] - c_low[j]) * t) * m;
                if (patches)
                {
                    // encrusting patches: where a low-frequency noise is high, one of the four colours (chosen by another)
                    const F32 u = (F32)k / (F32)cols, v = (F32)i / (F32)rings;
                    const F32 mask = smoothstep(0.56f, 0.62f, fbm(u, v * 0.5f, 3, 2, seed ^ 0x77ULL));   // crisp crust edges
                    const S32 which = llclamp((S32)(fbm(u, v * 0.5f, 2, 1, seed ^ 0x99ULL) * 4.f), 0, 3);
                    for (S32 j = 0; j < 3; ++j) c[j] = c[j] + (patches[which][j] * m - c[j]) * mask;
                }
                C[idx] = rgb(c[0], c[1], c[2]);
            }
        }
        // the normals of the displaced surface (the neighbours round each point), facing out from the dome's middle
        for (S32 i = 0; i < rows; ++i)
        {
            for (S32 k = 0; k < cols; ++k)
            {
                const LLVector3& pk1 = P[(size_t)(i * cols + (k + 1) % cols)];
                const LLVector3& pk0 = P[(size_t)(i * cols + (k + cols - 1) % cols)];
                const LLVector3& pi1 = P[(size_t)(llmin(i + 1, rings) * cols + k)];
                const LLVector3& pi0 = P[(size_t)(llmax(i - 1, 0) * cols + k)];
                LLVector3 n = (pk1 - pk0) % (pi1 - pi0);
                const size_t idx = (size_t)(i * cols + k);
                const LLVector3 out = P[idx] - (base + LLVector3(0.f, 0.f, hz * 0.2f));
                if (n * out < 0.f) n = -n;
                if (n.isExactlyZero() || i == rings) n = LLVector3(0.f, 0.f, 1.f);
                n.normVec();
                N[idx] = n;
            }
        }
        auto uv = [&](const LLVector3& p)
        { return atlas_uv(tile, (p.mV[VX] - base.mV[VX]) / (2.f * rx) + 0.5f, (p.mV[VY] - base.mV[VY]) / (2.f * ry) + 0.5f); };
        for (S32 i = 0; i < rings; ++i)
        {
            for (S32 k = 0; k < cols; ++k)
            {
                const S32 k2 = (k + 1) % cols;
                const size_t a0 = (size_t)(i * cols + k), a1 = (size_t)(i * cols + k2);
                const size_t b0 = (size_t)((i + 1) * cols + k), b1 = (size_t)((i + 1) * cols + k2);
                const PV A0{ P[a0], N[a0], uv(P[a0]), STILL, C[a0] }, A1{ P[a1], N[a1], uv(P[a1]), STILL, C[a1] };
                const PV B0{ P[b0], N[b0], uv(P[b0]), STILL, C[b0] }, B1{ P[b1], N[b1], uv(P[b1]), STILL, C[b1] };
                w.tri(A0, A1, B1);
                if (i + 1 < rings) w.tri(A0, B1, B0);
            }
        }
    }

    /** Reef rock: one boulder of a reef mound (reference: rough grey-brown limestone, mottled, with encrusting crusts).
     *  [LOOK 2026-11-09, round 3] a mound is now a cluster of these with crevices between (buildChunk), not one dome: Paul
     *  and the coordinator saw "a smooth olive mound with dots". Greyer stone (the turf tint was most of the olive), lumpier,
     *  shelved, with distinct crust patches (coralline pink, a purple and an ochre sponge crust, algal turf; CHOSEN tones). */
    void plant_boulder(PlantWriter& w, const LLVector3& base, F32 rx, F32 ry, F32 hz, Rng& r, F32 warm, F32 cold)
    {
        static const F32 ROCK[3] = { 0.47f, 0.45f, 0.41f }, TURF_WARM[3] = { 0.48f, 0.46f, 0.34f }, TURF_COOL[3] = { 0.38f, 0.40f, 0.30f };
        static const F32 CORALLINE[3] = { 0.58f, 0.42f, 0.44f };
        F32 low[3], top[3];
        vary(ROCK, r, 0.12f, low);
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 turf = TURF_COOL[i] + (TURF_WARM[i] - TURF_COOL[i]) * warm;
            const F32 t = turf + (CORALLINE[i] - turf) * cold * 0.6f;
            top[i] = low[i] * 1.06f + (t - low[i]) * 0.3f;   // a light turf over the stone, not an olive coat
        }
        static const F32 WARM_PATCHES[4][3] = { { 0.66f, 0.42f, 0.46f }, { 0.42f, 0.30f, 0.44f }, { 0.72f, 0.54f, 0.26f }, { 0.36f, 0.42f, 0.24f } };
        static const F32 COLD_PATCHES[4][3] = { { 0.68f, 0.42f, 0.47f }, { 0.60f, 0.38f, 0.44f }, { 0.54f, 0.52f, 0.44f }, { 0.34f, 0.37f, 0.25f } };
        dome(w, base, rx, ry, hz, 18, 9, 0.32f, T_ROCK, low, top, 0.16f, mix64((U64)(r.next() * 1e7f)), 0.7f,
             warm > 0.4f ? WARM_PATCHES : COLD_PATCHES);
    }

    /** Brain coral (warm seas): a grooved dome, olive to tan (reference). */
    void plant_brain(PlantWriter& w, const LLVector3& base, F32 rad, Rng& r)
    {
        static const F32 COLS[3][3] = { { 0.58f, 0.54f, 0.34f }, { 0.62f, 0.52f, 0.34f }, { 0.50f, 0.50f, 0.32f } };
        F32 c[3], c_low[3];
        vary(COLS[r.irange(0, 2)], r, 0.08f, c);
        for (S32 i = 0; i < 3; ++i) c_low[i] = c[i] * 0.8f;
        dome(w, base, rad, rad * r.range(0.85f, 1.1f), rad * r.range(0.6f, 0.85f), 20, 9, 0.04f, T_BRAIN, c_low, c, 0.06f,
             mix64((U64)(r.next() * 1e7f)));
    }

    /** A thick tapered paddle from `a` along `dir` (unit), `len` long, w0 -> w1 wide and `thick` thick: top, underside,
     *  both edges and the two ends, flat-shaded; the paddle lies across `side`. For elkhorn's branches. */
    void paddle(PlantWriter& w, const LLVector3& a, const LLVector3& dir, F32 len, F32 w0, F32 w1, F32 thick,
                const LLColor4U& ca, const LLColor4U& cb)
    {
        LLVector3 side = UP % dir;
        if (side.isExactlyZero()) side.setVec(1.f, 0.f, 0.f);
        side.normVec();
        LLVector3 up = dir % side;
        up.normVec();
        const LLVector3 b = a + dir * len;
        const F32 t0 = thick * 0.5f, t1 = thick * 0.35f;
        // the eight corners: root (r) and tip (t), left/right, top/bottom
        const LLVector3 rlt = a - side * (w0 * 0.5f) + up * t0, rrt = a + side * (w0 * 0.5f) + up * t0;
        const LLVector3 rlb = a - side * (w0 * 0.5f) - up * t0, rrb = a + side * (w0 * 0.5f) - up * t0;
        const LLVector3 tlt = b - side * (w1 * 0.5f) + up * t1, trt = b + side * (w1 * 0.5f) + up * t1;
        const LLVector3 tlb = b - side * (w1 * 0.5f) - up * t1, trb = b + side * (w1 * 0.5f) - up * t1;
        auto quad = [&](const LLVector3& p0, const LLVector3& p1, const LLVector3& p2, const LLVector3& p3, const LLVector3& nn,
                        const LLColor4U& c0, const LLColor4U& c1, F32 v0, F32 v1)
        {
            const PV A{ p0, nn, atlas_uv(T_POLYP, 0.f, v0), STILL, c0 }, B{ p1, nn, atlas_uv(T_POLYP, 1.f, v0), STILL, c0 };
            const PV C{ p2, nn, atlas_uv(T_POLYP, 1.f, v1), STILL, c1 }, D{ p3, nn, atlas_uv(T_POLYP, 0.f, v1), STILL, c1 };
            w.tri(A, B, C);
            w.tri(A, C, D);
        };
        const LLColor4U ca_dark = rgb(ca.mV[0] / 255.f * 0.7f, ca.mV[1] / 255.f * 0.7f, ca.mV[2] / 255.f * 0.7f);
        const LLColor4U cb_dark = rgb(cb.mV[0] / 255.f * 0.7f, cb.mV[1] / 255.f * 0.7f, cb.mV[2] / 255.f * 0.7f);
        quad(rlt, rrt, trt, tlt, up, ca, cb, 0.f, 1.f);                       // top
        quad(rrb, rlb, tlb, trb, -up, ca_dark, cb_dark, 0.f, 1.f);            // underside (in its own shade)
        quad(rlb, rlt, tlt, tlb, -side, ca, cb, 0.f, 1.f);                    // left edge
        quad(rrt, rrb, trb, trt, side, ca, cb, 0.f, 1.f);                     // right edge
        quad(tlt, trt, trb, tlb, dir, cb, cb, 0.9f, 1.f);                     // tip end
        quad(rrt, rlt, rlb, rrb, -dir, ca, ca, 0.f, 0.1f);                    // root end
    }

    /** Elkhorn coral (warm seas): [LOOK 2026-11-09, round 3] a stout trunk and THICK flattened branches that widen and fork
     *  twice, spreading up and out like antlers (reference: mustard-ochre, paler tips, pitted). The first two versions were
     *  flat sheets that read as yellow stars. */
    void plant_elkhorn(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 OCHRE[3] = { 0.72f, 0.55f, 0.30f };
        F32 c[3], tipc[3];
        vary(OCHRE, r, 0.08f, c);
        for (S32 i = 0; i < 3; ++i) tipc[i] = llmin(1.f, c[i] * 1.22f + 0.05f);
        const LLVector3 top = base + UP * (size * 0.22f);
        limb(w, base - UP * 0.05f, top, size * 0.1f, size * 0.08f, col3(c, 0.75f), col3(c), 6, STILL, STILL);
        const F32 thick = size * 0.045f;
        const S32 branches = r.irange(3, 5);
        for (S32 b = 0; b < branches; ++b)
        {
            const F32 az = r.range(0.f, F_TWO_PI), rise = r.range(0.2f, 0.55f);
            const LLVector3 d0(cosf(az) * cosf(rise), sinf(az) * cosf(rise), sinf(rise));
            const F32 l0 = size * r.range(0.35f, 0.5f), w0 = size * 0.12f, w1 = size * 0.2f;
            paddle(w, top, d0, l0, w0, w1, thick, col3(c), mixc(c, tipc, 0.3f));
            const LLVector3 e0 = top + d0 * l0;
            // two forks, each forking again into two tips (a palmate antler)
            for (S32 f = 0; f < 2; ++f)
            {
                const F32 sg = f ? 1.f : -1.f;
                const F32 az1 = az + sg * r.range(0.3f, 0.6f), rise1 = rise + r.range(-0.1f, 0.2f);
                const LLVector3 d1(cosf(az1) * cosf(rise1), sinf(az1) * cosf(rise1), sinf(rise1));
                const LLVector3 side0 = LLVector3(-d0.mV[VY], d0.mV[VX], 0.f) * (sg * w1 * 0.25f);
                const F32 l1 = size * r.range(0.25f, 0.38f);
                paddle(w, e0 + side0, d1, l1, w1 * 0.5f, w1 * 0.6f, thick * 0.85f, mixc(c, tipc, 0.3f), mixc(c, tipc, 0.6f));
                const LLVector3 e1 = e0 + side0 + d1 * l1;
                for (S32 t = 0; t < 2; ++t)
                {
                    const F32 sg2 = t ? 1.f : -1.f;
                    const F32 az2 = az1 + sg2 * r.range(0.25f, 0.5f), rise2 = rise1 + r.range(0.f, 0.25f);
                    const LLVector3 d2(cosf(az2) * cosf(rise2), sinf(az2) * cosf(rise2), sinf(rise2));
                    const LLVector3 side1 = LLVector3(-d1.mV[VY], d1.mV[VX], 0.f) * (sg2 * w1 * 0.15f);
                    paddle(w, e1 + side1, d2, size * r.range(0.12f, 0.22f), w1 * 0.3f, w1 * 0.2f, thick * 0.7f, mixc(c, tipc, 0.6f),
                           col3(tipc));
                }
            }
        }
    }

    /** Staghorn thicket (warm seas): many slim branches from one clump, forking once, pale tan with paler tips. */
    void plant_staghorn(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 TAN[3] = { 0.70f, 0.60f, 0.42f };
        F32 c[3], tipc[3];
        vary(TAN, r, 0.08f, c);
        for (S32 i = 0; i < 3; ++i) tipc[i] = llmin(1.f, c[i] * 1.25f + 0.05f);
        const S32 n = r.irange(12, 20);
        for (S32 k = 0; k < n; ++k)
        {
            const F32 az = r.range(0.f, F_TWO_PI), rise = r.range(0.6f, 1.4f);
            LLVector3 d(cosf(az) * cosf(rise), sinf(az) * cosf(rise), sinf(rise));
            const LLVector3 a = base + LLVector3(cosf(az), sinf(az), 0.f) * r.range(0.f, size * 0.25f) - UP * 0.03f;
            const F32 len = size * r.range(0.35f, 0.65f), rad = size * 0.03f;
            const LLVector3 b = a + d * len;
            limb(w, a, b, rad, rad * 0.75f, col3(c, 0.85f), mixc(c, tipc, 0.6f), 4, STILL, STILL);
            if (r.next() < 0.6f)
            {
                LLVector3 d2 = d + LLVector3(r.range(-0.5f, 0.5f), r.range(-0.5f, 0.5f), 0.2f);
                d2.normVec();
                limb(w, a + d * (len * 0.6f), a + d * (len * 0.6f) + d2 * (len * 0.5f), rad * 0.8f, rad * 0.6f, mixc(c, tipc, 0.4f),
                     col3(tipc), 4, STILL, STILL);
            }
        }
    }

    /** Tube sponges: a cluster of open tubes, a darker inside (reference: muted purple, rusty orange, olive yellow). */
    void plant_sponges(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 COLS[4][3] = { { 0.42f, 0.28f, 0.42f }, { 0.64f, 0.40f, 0.24f }, { 0.60f, 0.56f, 0.30f }, { 0.50f, 0.42f, 0.50f } };
        F32 c[3];
        vary(COLS[r.irange(0, 3)], r, 0.1f, c);
        const S32 tubes = r.irange(3, 6);
        constexpr S32 SIDES = 9;
        for (S32 t = 0; t < tubes; ++t)
        {
            const F32 az = r.range(0.f, F_TWO_PI), off = r.range(0.f, size * 0.25f);
            const LLVector3 a = base + LLVector3(cosf(az), sinf(az), 0.f) * off - UP * 0.04f;
            LLVector3 ax = UP + LLVector3(cosf(az), sinf(az), 0.f) * r.range(0.05f, 0.3f);
            ax.normVec();
            const F32 h = size * r.range(0.45f, 1.0f), rad = size * r.range(0.07f, 0.12f);
            const LLVector3 b = a + ax * h;
            LLVector3 u = UP % ax;
            if (u.isExactlyZero()) u.setVec(1.f, 0.f, 0.f);
            u.normVec();
            const LLVector3 v = ax % u;
            for (S32 k = 0; k < SIDES; ++k)
            {
                const F32 th0 = (F32)k * F_TWO_PI / (F32)SIDES, th1 = (F32)(k + 1) * F_TWO_PI / (F32)SIDES;
                const LLVector3 d0 = u * cosf(th0) + v * sinf(th0), d1 = u * cosf(th1) + v * sinf(th1);
                const F32 u0 = (F32)k / (F32)SIDES, u1 = (F32)(k + 1) / (F32)SIDES;
                const F32 flare = 1.15f;   // the lip a little wider than the base
                // outside
                const PV o0{ a + d0 * rad, d0, atlas_uv(T_SPONGE, u0, 0.f), STILL, col3(c, 0.85f) };
                const PV o1{ a + d1 * rad, d1, atlas_uv(T_SPONGE, u1, 0.f), STILL, col3(c, 0.85f) };
                const PV p0{ b + d0 * rad * flare, d0, atlas_uv(T_SPONGE, u0, 1.f), STILL, col3(c) };
                const PV p1{ b + d1 * rad * flare, d1, atlas_uv(T_SPONGE, u1, 1.f), STILL, col3(c) };
                w.tri(o0, o1, p1);
                w.tri(o0, p1, p0);
                // inside (a little in, facing in, darker), down to a third of the height
                const F32 ri = rad * flare * 0.8f;
                const LLVector3 bin = b - ax * (h * 0.66f);
                const PV i0{ b + d0 * ri, -d0, atlas_uv(T_SPONGE, u0, 1.f), STILL, col3(c, 0.45f) };
                const PV i1{ b + d1 * ri, -d1, atlas_uv(T_SPONGE, u1, 1.f), STILL, col3(c, 0.45f) };
                const PV j0{ bin + d0 * ri, -d0, atlas_uv(T_SPONGE, u0, 0.3f), STILL, col3(c, 0.25f) };
                const PV j1{ bin + d1 * ri, -d1, atlas_uv(T_SPONGE, u1, 0.3f), STILL, col3(c, 0.25f) };
                w.tri(i0, i1, j1);
                w.tri(i0, j1, j0);
                // the rim between them
                const PV q0{ b + d0 * rad * flare, ax, atlas_uv(T_SPONGE, u0, 1.f), STILL, col3(c, 0.9f) };
                const PV q1{ b + d1 * rad * flare, ax, atlas_uv(T_SPONGE, u1, 1.f), STILL, col3(c, 0.9f) };
                const PV r0{ b + d0 * ri, ax, atlas_uv(T_SPONGE, u0, 0.95f), STILL, col3(c, 0.7f) };
                const PV r1{ b + d1 * ri, ax, atlas_uv(T_SPONGE, u1, 0.95f), STILL, col3(c, 0.7f) };
                w.tri(q0, q1, r1);
                w.tri(q0, r1, r0);
            }
        }
    }

    /** Sea rods (warm seas): slim branching rods swaying in the surge, purple-brown or tan (reference: soft corals). */
    void plant_rods(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 COLS[2][3] = { { 0.42f, 0.30f, 0.34f }, { 0.58f, 0.48f, 0.32f } };
        F32 c[3], tipc[3];
        vary(COLS[r.irange(0, 1)], r, 0.1f, c);
        for (S32 i = 0; i < 3; ++i) tipc[i] = llmin(1.f, c[i] * 1.2f);
        const S32 rods = r.irange(5, 9);
        const S32 n60 = r.irange(15, 20);
        for (S32 k = 0; k < rods; ++k)
        {
            const F32 phase = r.next(), az = r.range(0.f, F_TWO_PI), rise = r.range(1.1f, 1.45f);
            const LLVector3 d(cosf(az) * cosf(rise), sinf(az) * cosf(rise), sinf(rise));
            const F32 len = size * r.range(0.6f, 1.f), rad = 0.012f + size * 0.008f;
            const LLVector3 a = base + LLVector3(cosf(az), sinf(az), 0.f) * r.range(0.f, 0.1f) - UP * 0.03f;
            const LLVector3 m = a + d * (len * 0.5f), b = a + d * len;
            const F32 bend = 0.06f * len;
            limb(w, a, m, rad, rad * 0.85f, col3(c, 0.85f), col3(c), 3, sway(0.f, n60, phase), sway(bend * 0.3f, n60, phase));
            limb(w, m, b, rad * 0.85f, rad * 0.6f, col3(c), col3(tipc), 3, sway(bend * 0.3f, n60, phase), sway(bend, n60, phase));
            if (r.next() < 0.5f)
            {
                LLVector3 d2 = d + LLVector3(r.range(-0.5f, 0.5f), r.range(-0.5f, 0.5f), 0.f);
                d2.normVec();
                limb(w, m, m + d2 * (len * 0.4f), rad * 0.7f, rad * 0.5f, col3(c), col3(tipc), 3, sway(bend * 0.3f, n60, phase),
                     sway(bend * 0.8f, n60, phase));
            }
        }
    }

    /** The surge's mean direction, radians: renderDeferred moves wolf_sway_dir round 0.6 +- 0.5 (CHOSEN there). */
    constexpr F32 CURRENT_RAD = 0.6f;

    /** Sea fan (warm seas): [LOOK 2026-11-09] a flat upright fan, taller than wide (half-width 0.75 x rad, height 1.2 x
     *  rad), its ragged rim from noise, standing ACROSS the current (as sea fans grow, to sieve it - the first version
     *  faced any way and read as wire domes); the lace is the atlas's alpha mask (T_LATTICE), ribs radiating from the stalk.
     *  Sways a little. */
    void plant_fan(PlantWriter& w, const LLVector3& base, F32 rad, Rng& r)
    {
        static const F32 COLS[3][3] = { { 0.50f, 0.32f, 0.52f }, { 0.60f, 0.45f, 0.30f }, { 0.62f, 0.56f, 0.34f } };
        F32 c[3];
        vary(COLS[r.irange(0, 2)], r, 0.1f, c);
        const F32 a = CURRENT_RAD + F_PI_BY_TWO + r.range(-0.3f, 0.3f);   // the fan's span: across the current
        const LLVector3 across(cosf(a), sinf(a), 0.f), face(-sinf(a), cosf(a), 0.f);
        const LLVector3 n = leaf_normal(face);
        const S32 n60 = r.irange(18, 22);
        const F32 phase = r.next();
        const LLVector3 stalk = base + UP * (0.08f + rad * 0.08f);
        limb(w, base - UP * 0.03f, stalk, 0.02f, 0.016f, col3(c, 0.7f), col3(c, 0.8f), 3, STILL, STILL);
        constexpr S32 WEDGES = 16;
        const F32 a0 = 0.12f, a1 = F_PI - 0.12f, step = (a1 - a0) / (F32)WEDGES;
        const F32 half_w = rad * 0.75f, tall = rad * 1.2f;
        const U64 seed = mix64((U64)(r.next() * 1e7f));
        auto rim = [&](S32 k, LLVector3& p, F32& u)
        {
            const F32 t = a0 + step * (F32)k;
            const F32 ragged = 0.82f + 0.18f * pnoise((F32)k * 0.45f, 0.f, 64, seed);
            p = stalk + across * (cosf(t) * half_w * ragged) + UP * (sinf(t) * tall * ragged);
            u = (F32)k / (F32)WEDGES;
        };
        const PV root{ stalk, n, atlas_uv(T_LATTICE, 0.5f, 0.f), sway(0.f, n60, phase), col3(c, 0.8f) };
        for (S32 k = 0; k < WEDGES; ++k)
        {
            LLVector3 p0, p1;
            F32 u0 = 0.f, u1 = 0.f;
            rim(k, p0, u0);
            rim(k + 1, p1, u1);
            const F32 bend = 0.05f * rad;
            w.tri(root, PV{ p0, n, atlas_uv(T_LATTICE, u0, 1.f), sway(bend, n60, phase), col3(c) },
                  PV{ p1, n, atlas_uv(T_LATTICE, u1, 1.f), sway(bend, n60, phase), col3(c) });
        }
    }

    /** A loose stone on the sand (any sea). */
    void plant_stone(PlantWriter& w, const LLVector3& base, F32 rad, Rng& r)
    {
        static const F32 STONE[3] = { 0.44f, 0.41f, 0.36f };
        F32 c[3], top[3];
        vary(STONE, r, 0.1f, c);
        for (S32 i = 0; i < 3; ++i) top[i] = c[i] * 1.1f;
        dome(w, base, rad, rad * r.range(0.7f, 1.f), rad * r.range(0.4f, 0.6f), 9, 4, 0.2f, T_ROCK, c, top, 0.12f,
             mix64((U64)(r.next() * 1e7f)));
    }

    // ── [COLD SEAS 2026-11-09] the cool and cold sea floor (Paul at TROMSO: "i cant see any fish or anything") ──
    // What a North Atlantic / Norwegian rocky shallow holds (CHOSEN as the common sights, our own shapes and tones): kelp
    // forests, wrack in the shallows, sea urchins grazing, starfish, anemones on the rock.

    /** A cluster of 2-6 sea urchins: dark spiny balls (a small dome and its spines). */
    void plant_urchins(PlantWriter& w, const LLVector3& base, Rng& r)
    {
        static const F32 COLS[3][3] = { { 0.28f, 0.14f, 0.28f }, { 0.36f, 0.30f, 0.20f }, { 0.50f, 0.24f, 0.30f } };
        F32 c[3];
        vary(COLS[r.irange(0, 2)], r, 0.1f, c);
        const S32 n = r.irange(2, 6);
        for (S32 k = 0; k < n; ++k)
        {
            const F32 a = r.range(0.f, F_TWO_PI), d = r.range(0.f, 0.35f), rad = r.range(0.035f, 0.06f);
            const LLVector3 b = base + LLVector3(cosf(a) * d, sinf(a) * d, 0.f);
            dome(w, b, rad, rad, rad * 0.75f, 8, 4, 0.05f, T_POLYP, c, c, 0.1f, mix64((U64)(r.next() * 1e7f)));
            for (S32 sp = 0; sp < 7; ++sp)   // CHOSEN: 7 spines read as spiny at a metre or two (cost: see docs)
            {
                const F32 az = r.range(0.f, F_TWO_PI), el = r.range(0.15f, 1.4f);
                const LLVector3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
                const LLVector3 from = b + LLVector3(dir.mV[VX] * rad * 0.8f, dir.mV[VY] * rad * 0.8f, dir.mV[VZ] * rad * 0.6f);
                limb(w, from, from + dir * (rad * 1.3f), rad * 0.08f, rad * 0.02f, col3(c, 0.8f), col3(c, 0.6f), 3, STILL, STILL);
            }
        }
    }

    /** A starfish lying on the bed: five tapering arms, slightly curled. Muted orange, purple or red. */
    void plant_starfish(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 COLS[3][3] = { { 0.78f, 0.42f, 0.20f }, { 0.52f, 0.26f, 0.44f }, { 0.66f, 0.22f, 0.16f } };
        F32 c[3];
        vary(COLS[r.irange(0, 2)], r, 0.1f, c);
        const F32 spin = r.range(0.f, F_TWO_PI), thick = size * 0.08f;
        const LLVector3 centre = base + UP * thick;
        for (S32 k = 0; k < 5; ++k)
        {
            const F32 a = spin + (F32)k * F_TWO_PI / 5.f + r.range(-0.15f, 0.15f);
            const LLVector3 dir(cosf(a), sinf(a), 0.f), side(-sinf(a), cosf(a), 0.f);
            const F32 len = size * r.range(0.42f, 0.55f), curl = r.range(0.f, 0.06f) * size;
            const LLVector3 root0 = centre + side * (size * 0.1f), root1 = centre - side * (size * 0.1f);
            const LLVector3 mid = centre + dir * (len * 0.55f) + UP * (curl * 0.4f), tip = base + dir * len + UP * curl;
            const LLVector3 m0 = mid + side * (size * 0.06f), m1 = mid - side * (size * 0.06f);
            auto P = [&](const LLVector3& p, F32 v) { return PV{ p, UP, atlas_uv(T_POLYP, 0.5f, v), STILL, col3(c) }; };
            w.tri(P(centre, 0.f), P(root0, 0.f), P(m0, 0.5f));
            w.tri(P(centre, 0.f), P(m0, 0.5f), P(m1, 0.5f));
            w.tri(P(centre, 0.f), P(m1, 0.5f), P(root1, 0.f));
            w.tri(P(m0, 0.5f), P(tip, 1.f), P(m1, 0.5f));
        }
    }

    /** An anemone: a short column with a crown of tentacles that sway (plumose white, orange or pink; CHOSEN tones). */
    void plant_anemone(PlantWriter& w, const LLVector3& base, F32 size, Rng& r)
    {
        static const F32 COLS[3][3] = { { 0.86f, 0.82f, 0.74f }, { 0.84f, 0.50f, 0.26f }, { 0.80f, 0.52f, 0.58f } };
        F32 c[3];
        vary(COLS[r.irange(0, 2)], r, 0.08f, c);
        const F32 colh = size * r.range(0.6f, 1.f), rad = size * 0.3f;
        const LLVector3 top = base + UP * colh;
        limb(w, base - UP * 0.02f, top, rad, rad * 1.15f, col3(c, 0.7f), col3(c, 0.85f), 7, STILL, STILL, T_SPONGE);
        const S32 n60 = r.irange(20, 26);
        const F32 phase = r.next();
        const S32 tent = r.irange(12, 16);
        for (S32 k = 0; k < tent; ++k)
        {
            const F32 az = (F32)k * F_TWO_PI / (F32)tent + r.range(-0.1f, 0.1f), el = r.range(0.5f, 1.2f);
            const LLVector3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
            const LLVector3 from = top + LLVector3(cosf(az), sinf(az), 0.f) * (rad * r.range(0.4f, 1.1f));
            const F32 len = size * r.range(0.35f, 0.6f);
            limb(w, from, from + dir * len, size * 0.03f, size * 0.01f, col3(c), col3(c, 1.1f), 3, sway(0.f, n60, phase),
                 sway(len * 0.25f, n60, phase));
        }
    }

    /** Wrack (bladder-wrack-like brown seaweed) in the shallows: 4-7 flat fronds forking twice, swaying. */
    void plant_wrack(PlantWriter& w, const LLVector3& base, F32 h, Rng& r)
    {
        static const F32 LO[3] = { 0.34f, 0.28f, 0.12f }, HI[3] = { 0.50f, 0.42f, 0.18f };
        F32 lo[3], hi[3];
        vary(LO, r, 0.1f, lo); vary(HI, r, 0.1f, hi);
        const S32 n60 = r.irange(14, 20);
        const S32 fronds = r.irange(4, 7);
        for (S32 f = 0; f < fronds; ++f)
        {
            const F32 phase = r.next(), az = r.range(0.f, F_TWO_PI), rise = r.range(0.9f, 1.35f);
            const LLVector3 d(cosf(az) * cosf(rise), sinf(az) * cosf(rise), sinf(rise));
            const F32 len = h * r.range(0.4f, 0.55f), wd = h * 0.05f;
            const LLVector3 a = base + LLVector3(cosf(az), sinf(az), 0.f) * r.range(0.f, 0.05f), m = a + d * len;
            const LLVector3 side(-sinf(az), cosf(az), 0.f), n = leaf_normal(UP % side);
            auto ribbon = [&](const LLVector3& p0, const LLVector3& p1, F32 w0, F32 w1, F32 s0, F32 s1, F32 t0, F32 t1)
            {
                const PV A{ p0 - side * w0, n, atlas_uv(T_KELP, 0.f, t0), sway(s0, n60, phase), mixc(lo, hi, t0) };
                const PV B{ p0 + side * w0, n, atlas_uv(T_KELP, 1.f, t0), sway(s0, n60, phase), mixc(lo, hi, t0) };
                const PV C{ p1 + side * w1, n, atlas_uv(T_KELP, 1.f, t1), sway(s1, n60, phase), mixc(lo, hi, t1) };
                const PV D{ p1 - side * w1, n, atlas_uv(T_KELP, 0.f, t1), sway(s1, n60, phase), mixc(lo, hi, t1) };
                w.tri(A, B, C);
                w.tri(A, C, D);
            };
            const F32 bend = 0.15f * h;
            ribbon(a, m, wd * 0.6f, wd, 0.f, bend * 0.4f, 0.f, 0.5f);
            for (S32 k = 0; k < 2; ++k)
            {
                const F32 sg = k ? 1.f : -1.f;
                LLVector3 d2 = d + side * (sg * 0.35f);
                d2.normVec();
                const LLVector3 e = m + d2 * len;
                ribbon(m, e, wd * 0.8f, wd * 0.5f, bend * 0.4f, bend, 0.5f, 1.f);
            }
        }
    }

    // ── reef patches: where the reef is (a hash of 24 m world cells, the same for everyone) ──
    constexpr F32 PATCH_GRID_M = 24.f;   // CHOSEN
    struct Patch { F64 mX = 0.0, mY = 0.0; F32 mR = 0.f; U64 mSeed = 0; };

    /** Whether the 24 m world cell (px, py) holds a reef patch, and where. CHOSEN shares: a warm shallow sea is half reef
     *  patches (the reference shallows), cooler seas have rocky patches (kelp holds to rock) a little less often. */
    bool patch_at(S64 px, S64 py, F32 warm, Patch& out)
    {
        const U64 h = hash2(px, py, 0xB0B1ULL);
        const F32 chance = 0.35f + 0.2f * warm;
        if (unit(h) >= chance) return false;
        Rng r(mix64(h));
        out.mR = r.range(3.f, 7.f);
        out.mX = (F64)px * PATCH_GRID_M + (F64)r.range(out.mR, PATCH_GRID_M - out.mR);
        out.mY = (F64)py * PATCH_GRID_M + (F64)r.range(out.mR, PATCH_GRID_M - out.mR);
        out.mSeed = mix64(h ^ 0x77ULL);
        return true;
    }
}

/** The reef patches whose disc reaches within `reach` of world point (gx, gy) (for the fish, which gather round them). */
void WolfUnderSea::patchesNear(F64 gx, F64 gy, F32 reach, std::vector<LLVector4>& out) const
{
    out.clear();
    if (!WolfRegionWeather::instanceExists()) return;
    const F32 warm = WolfRegionWeather::instance().underSeaWarm();
    const S64 p0x = cell_of(gx - reach - 8.0, PATCH_GRID_M), p1x = cell_of(gx + reach + 8.0, PATCH_GRID_M);
    const S64 p0y = cell_of(gy - reach - 8.0, PATCH_GRID_M), p1y = cell_of(gy + reach + 8.0, PATCH_GRID_M);
    for (S64 px = p0x; px <= p1x; ++px)
    {
        for (S64 py = p0y; py <= p1y; ++py)
        {
            Patch p;
            if (!patch_at(px, py, warm, p)) continue;
            const F64 dx = p.mX - gx, dy = p.mY - gy;
            if (sqrt(dx * dx + dy * dy) - (F64)p.mR > (F64)reach) continue;
            // the patch's centre (agent metres) and radius; only patches over sea that may hold them
            const LLVector3 agent = gAgent.getPosAgentFromGlobal(LLVector3d(p.mX, p.mY, 0.0));
            const F32 d = depth(agent);
            if (d < 2.5f) continue;
            out.push_back(LLVector4(agent.mV[VX], agent.mV[VY], water() - d, p.mR));
        }
    }
}

/** Builds one chunk's plants (the same garden for everyone: everything is a hash of world position) from the terrain, the
 *  sea's climate and the obstacle map. The layout follows the reference photographs: reef PATCHES (a rock mound or two
 *  crowded with corals, sponges, rods and fans in warm seas; rock with kelp in cool ones), seagrass MEADOWS, and bare sand
 *  between with the odd stone. Returns false only when its vertex buffer cannot be had. */
bool WolfUnderSea::buildChunk(Chunk& c, S64 cx, S64 cy)
{
    LLTimer cost;
    c.mOriginGlobal = LLVector3d((F64)cx * CHUNK_M, (F64)cy * CHUNK_M, 0.0);
    c.mBlockHash = blockHash(c.mOriginGlobal);
    c.mClimate = climateStamp();
    c.mRetryAt = 0.0;
    c.mDirty = false;
    c.mVB = nullptr;
    c.mVerts = 0;
    const WolfRegionWeather& rw = WolfRegionWeather::instance();
    const F32 warm = rw.underSeaWarm(), cold = rw.underSeaCold();
    const F32 w_h = water();
    std::vector<PV> verts;
    verts.reserve(16384);
    PlantWriter wr(verts);
    F32 zmin = 1e9f, zmax = -1e9f;
    bool incomplete = false;
    const F64 ox = c.mOriginGlobal.mdV[VX], oy = c.mOriginGlobal.mdV[VY];
    auto in_chunk = [&](F64 gx, F64 gy) { return gx >= ox && gx < ox + CHUNK_M && gy >= oy && gy < oy + CHUNK_M; };

    // Is the spot at world (gx, gy) sea floor, between min_depth and max_depth deep, no steeper than max_slope_z (the
    // terrain normal's z), with sea all round `radius` out? Gives its bed height and `room`, the height above the bed a
    // plant may reach there and stay under the water (Paul's rule).
    auto spot = [&](F64 gx, F64 gy, F32 radius, F32 min_depth, F32 max_depth, F32 max_slope_z, F32& bed, F32& room) -> bool
    {
        const LLVector3 agent = gAgent.getPosAgentFromGlobal(LLVector3d(gx, gy, 0.0));
        LLViewerRegion* r = regionFor(agent);
        if (!r) return false;
        const LLVector3 pr = agent - r->getOriginAgent();
        if (r->getLand().getPatchesPerEdge() <= 0) { incomplete = true; return false; }
        const LLSurfacePatch* patch = r->getLand().resolvePatchRegion(pr);
        if (!patch || !patch->getHasReceivedData()) { incomplete = true; return false; }
        const F32 h = r->getLand().resolveHeightRegion(pr);
        if (!std::isfinite(h)) return false;
        const F32 d = w_h - h;
        if (d < min_depth || d > max_depth) return false;
        const LLVector3 n = r->getLand().resolveNormalGlobal(LLVector3d(gx, gy, (F64)h));
        if (n.mV[VZ] < max_slope_z) return false;
        for (S32 k = 0; k < 4; ++k)
        {
            const F32 a = (F32)k * F_PI_BY_TWO + 0.4f;
            if (depth(agent + LLVector3(cosf(a) * radius, sinf(a) * radius, 0.f)) < min_depth * 0.6f) return false;
        }
        bed = h;
        room = d - llmax(TOP_CLEAR_MIN_M, TOP_CLEAR_FRAC * d);
        return room > 0.f;
    };
    auto local = [&](F64 gx, F64 gy, F32 z) { return LLVector3((F32)(gx - ox), (F32)(gy - oy), z); };
    auto grew = [&](F32 bottom, F32 top) { zmin = llmin(zmin, bottom - 0.3f); zmax = llmax(zmax, top + 0.3f); };
    // [COLD SEAS 2026-11-09] One item of a cool or cold sea floor at world (gx, gy), standing at `foot` with `head` metres of
    // room above it: kelp with chance kelp_share (a tall canopy: three quarters of the room or more, up to 16 m - the
    // canopy reaches up to the clearance under the water), else (CHOSEN shares) an anemone, a group of urchins, a starfish,
    // pale sponges or a stone. Returns true when nothing was placed (no room, or a build is there).
    auto cool_item = [&](Rng& ir, F64 gx, F64 gy, F32 foot, F32 head, F32 kelp_share) -> bool
    {
        const F32 pick = ir.next();
        if (pick < kelp_share)
        {
            const F32 h = llmin(head, ir.range(0.75f, 1.f) * head, 16.f);
            if (h < 2.f || blocked(gx, gy, 0.6f, foot - 0.2f, foot + h)) return true;
            plant_kelp(wr, local(gx, gy, foot - 0.05f), h, ir);
            grew(foot, foot + h + 1.6f);
            return false;
        }
        const F32 q = (pick - kelp_share) / llmax(0.01f, 1.f - kelp_share);   // 0..1 over the rest
        if (q < 0.25f)
        {
            const F32 size = ir.range(0.15f, 0.35f);
            if (head < size * 1.6f || blocked(gx, gy, size, foot - 0.2f, foot + size * 1.6f)) return true;
            plant_anemone(wr, local(gx, gy, foot), size, ir);
            grew(foot, foot + size * 1.6f);
        }
        else if (q < 0.45f)
        {
            if (head < 0.15f || blocked(gx, gy, 0.4f, foot - 0.2f, foot + 0.15f)) return true;
            plant_urchins(wr, local(gx, gy, foot), ir);
            grew(foot, foot + 0.15f);
        }
        else if (q < 0.62f)
        {
            const F32 size = ir.range(0.15f, 0.3f);
            if (head < 0.1f || blocked(gx, gy, size * 0.6f, foot - 0.2f, foot + 0.1f)) return true;
            plant_starfish(wr, local(gx, gy, foot), size, ir);
            grew(foot, foot + 0.1f);
        }
        else if (q < 0.76f)
        {
            const F32 size = ir.range(0.25f, 0.5f);
            if (head < size * 1.05f || blocked(gx, gy, size * 0.3f, foot - 0.2f, foot + size)) return true;
            plant_sponges(wr, local(gx, gy, foot), size, ir);
            grew(foot, foot + size * 1.05f);
        }
        else
        {
            const F32 rad = ir.range(0.25f, 0.7f);
            if (head < rad * 0.6f || blocked(gx, gy, rad, foot - 0.2f, foot + rad * 0.6f)) return true;
            plant_stone(wr, local(gx, gy, foot), rad, ir);
            grew(foot, foot + rad * 0.6f);
        }
        return false;
    };

    // 1. the reef patches reaching into this chunk (each item is drawn by the chunk its foot stands in)
    struct Mound { F64 mX, mY; F32 mR, mH; };
    struct Boulder { F64 mX, mY; F32 mRx, mRy, mH; };   // [LOOK 2026-11-09, round 3] a mound's stones
    std::vector<Patch> patches;
    {
        const S64 p0x = cell_of(ox - 8.0, PATCH_GRID_M), p1x = cell_of(ox + CHUNK_M + 8.0, PATCH_GRID_M);
        const S64 p0y = cell_of(oy - 8.0, PATCH_GRID_M), p1y = cell_of(oy + CHUNK_M + 8.0, PATCH_GRID_M);
        for (S64 px = p0x; px <= p1x; ++px)
            for (S64 py = p0y; py <= p1y; ++py)
            {
                Patch p;
                if (patch_at(px, py, warm, p)) patches.push_back(p);
            }
    }
    for (const Patch& p : patches)
    {
        Rng pr(p.mSeed);
        // the rock: [LOOK 2026-11-09] three to six smaller mounds clustered near the middle (it was one to three big ones, a
        // slab), CHOSEN 0.5-1.6 m high and never above the room under the water. `candidates` keeps every mound the hash
        // gives, placed or not, so the items below draw the same numbers whatever the terrain refused.
        std::vector<Mound> candidates;
        std::vector<Boulder> boulders;
        const S32 n_mounds = pr.irange(3, 6);
        for (S32 m = 0; m < n_mounds; ++m)
        {
            const F32 a = pr.range(0.f, F_TWO_PI), d = pr.range(0.f, p.mR * 0.55f);
            Mound md{ p.mX + (F64)(cosf(a) * d), p.mY + (F64)(sinf(a) * d), p.mR * pr.range(0.18f, 0.32f), pr.range(0.5f, REEF_ROCK_TOP_M) };
            candidates.push_back(md);
            Rng mr(mix64(p.mSeed ^ (0x900ULL + (U64)m)));
            F32 bed = 0.f, room = 0.f;
            if (!spot(md.mX, md.mY, md.mR * 0.8f, 2.f, 40.f, 0.55f, bed, room) || room < 0.5f) continue;
            md.mH = llmin(md.mH, room - 0.3f, md.mR * 0.7f);
            if (md.mH < 0.3f) continue;
            // [LOOK 2026-11-09, round 3] the mound as 3-7 boulders of different sizes, jostled together with crevices between
            // (CHOSEN); each on its own bed, kept under the room there; all drawn by the chunk the mound's centre is in
            const S32 n_b = mr.irange(3, 7);
            for (S32 b = 0; b < n_b; ++b)
            {
                const F32 ba = mr.range(0.f, F_TWO_PI), bd = mr.range(0.f, md.mR * 0.6f);
                Boulder bo{ md.mX + (F64)(cosf(ba) * bd), md.mY + (F64)(sinf(ba) * bd), md.mR * mr.range(0.3f, 0.55f), 0.f,
                            md.mH * mr.range(0.45f, 1.f) * (1.f - 0.4f * bd / md.mR) };
                bo.mRy = bo.mRx * mr.range(0.7f, 1.2f);
                Rng br(mix64(p.mSeed ^ (0x5000ULL + (U64)(m * 16 + b))));
                F32 bbed = 0.f, broom = 0.f;
                if (!spot(bo.mX, bo.mY, bo.mRx * 0.7f, 2.f, 40.f, 0.5f, bbed, broom)) continue;
                bo.mH = llmin(bo.mH, broom - 0.3f);
                if (bo.mH < 0.2f) continue;
                boulders.push_back(bo);
                if (in_chunk(md.mX, md.mY) && !blocked(bo.mX, bo.mY, bo.mRx, bbed - 0.3f, bbed + bo.mH))
                {
                    plant_boulder(wr, local(bo.mX, bo.mY, bbed - 0.12f), bo.mRx, bo.mRy, bo.mH + 0.12f, br, warm, cold);
                    grew(bbed - 0.3f, bbed + bo.mH);
                }
            }
        }
        // the height of the rock under (gx, gy), above the bed: the highest boulder there (each a dome; a little under its
        // lumpy surface), 0 in a crevice - so nothing stands in the air over a gap between stones
        auto rock_at = [&](F64 gx, F64 gy)
        {
            F32 h = 0.f;
            for (const Boulder& bo : boulders)
            {
                const F32 ex = (F32)(gx - bo.mX) / bo.mRx, ey = (F32)(gy - bo.mY) / bo.mRy;
                const F32 dd = sqrtf(ex * ex + ey * ey);
                if (dd < 0.85f) h = llmax(h, bo.mH * sqrtf(1.f - dd * dd) * 0.8f);
            }
            return h;
        };
        // the life on it: [LOOK 2026-11-09] crowded so little bare rock shows - CHOSEN about 1.4 items a square metre, seven in
        // ten standing on a mound (the rest round the patch's edge)
        const S32 items = (S32)(p.mR * p.mR * 1.4f);
        for (S32 k = 0; k < items; ++k)
        {
            Rng ir(mix64(p.mSeed ^ (0x1000ULL + (U64)k)));
            F64 gx = 0.0, gy = 0.0;
            const F32 on_rock = ir.next(), a = ir.range(0.f, F_TWO_PI), rr = sqrtf(ir.next());
            const Mound& mc = candidates[(size_t)ir.irange(0, (S32)candidates.size() - 1)];
            if (on_rock < 0.7f) { gx = mc.mX + (F64)(cosf(a) * rr * mc.mR * 0.85f); gy = mc.mY + (F64)(sinf(a) * rr * mc.mR * 0.85f); }
            else { gx = p.mX + (F64)(cosf(a) * rr * p.mR * 1.05f); gy = p.mY + (F64)(sinf(a) * rr * p.mR * 1.05f); }
            const F32 pick = ir.next();
            if (!in_chunk(gx, gy)) continue;
            F32 bed = 0.f, room = 0.f;
            if (!spot(gx, gy, 0.3f, 1.8f, 30.f, 0.5f, bed, room)) continue;
            const F32 lift = rock_at(gx, gy);
            const F32 foot = bed + lift - 0.04f;
            const F32 head = room - lift;   // how tall it may be, standing on the rock
            if (warm > 0.4f)
            {
                // warm reef (reference: Bonaire shallows): brain domes, elkhorn, staghorn, sponges, rods, fans
                // [LOOK 2026-11-09] more and bigger brain-coral heads (0.6-1.5 m across), fewer fans (7 %)
                if (pick < 0.3f)
                {
                    const F32 rad = ir.range(0.3f, 0.75f);
                    if (head < rad * 0.85f || blocked(gx, gy, rad, foot - 0.2f, foot + rad)) continue;
                    plant_brain(wr, local(gx, gy, foot - rad * 0.1f), rad, ir);
                    grew(foot, foot + rad);
                }
                else if (pick < 0.42f)
                {
                    const F32 size = ir.range(0.6f, 1.3f);
                    // plant_elkhorn reaches about 0.92 x size up (trunk 0.22, the forks' rises): checked against 1.0 x size
                    if (head < size || blocked(gx, gy, size * 0.8f, foot - 0.2f, foot + size)) continue;
                    plant_elkhorn(wr, local(gx, gy, foot), size, ir);
                    grew(foot, foot + size);
                }
                else if (pick < 0.57f)
                {
                    const F32 size = ir.range(0.5f, 1.0f);
                    if (head < size * 0.7f || blocked(gx, gy, size * 0.5f, foot - 0.2f, foot + size * 0.7f)) continue;
                    plant_staghorn(wr, local(gx, gy, foot), size, ir);
                    grew(foot, foot + size * 0.7f);
                }
                else if (pick < 0.75f)
                {
                    const F32 size = ir.range(0.4f, 0.9f);
                    if (head < size * 1.05f || blocked(gx, gy, size * 0.3f, foot - 0.2f, foot + size)) continue;
                    plant_sponges(wr, local(gx, gy, foot), size, ir);
                    grew(foot, foot + size * 1.05f);
                }
                else if (pick < 0.93f)
                {
                    const F32 size = ir.range(0.5f, 1.1f);
                    if (head < size + 0.1f || blocked(gx, gy, 0.3f, foot - 0.2f, foot + size)) continue;
                    plant_rods(wr, local(gx, gy, foot), size, ir);
                    grew(foot, foot + size + 0.1f);
                }
                else
                {
                    const F32 rad = ir.range(0.6f, 1.1f);   // the fan's height is 1.2 x this (plant_fan)
                    if (head < rad * 1.3f + 0.1f || blocked(gx, gy, rad, foot - 0.2f, foot + rad * 1.3f)) continue;
                    plant_fan(wr, local(gx, gy, foot), rad, ir);
                    grew(foot, foot + rad * 1.3f + 0.1f);
                }
            }
            else
            {
                // [COLD SEAS 2026-11-09] a cool or cold sea's rocky patch: kelp holding to the rock in a tall canopy (fewer
                // where it is warmer), and round its feet anemones, urchins, starfish, pale sponges and stones
                if (cool_item(ir, gx, gy, foot, head, 0.45f * (1.f - warm / 0.4f))) continue;
            }
        }
    }

    // 2. the sand between: seagrass in meadows (a dense sward inside, thinning at the edge), else bare sand and the odd stone
    for (S32 iy = 0; iy < CELLS_PER_CHUNK && verts.size() < MAX_CHUNK_VERTS; ++iy)
    {
        for (S32 ix = 0; ix < CELLS_PER_CHUNK && verts.size() < MAX_CHUNK_VERTS; ++ix)
        {
            const S64 gcx = cx * CELLS_PER_CHUNK + ix, gcy = cy * CELLS_PER_CHUNK + iy;
            const F64 gx0 = (F64)gcx * CELL_M, gy0 = (F64)gcy * CELL_M;
            const F64 mx = gx0 + CELL_M * 0.5, my = gy0 + CELL_M * 0.5;
            const LLVector3 centre_agent = gAgent.getPosAgentFromGlobal(LLVector3d(mx, my, 0.0));
            const F32 centre_depth = depth(centre_agent);
            if (centre_depth < 1.2f)
            {
                LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(centre_agent);
                if (r && regionFor(centre_agent))
                {
                    const LLSurfacePatch* patch = r->getLand().getPatchesPerEdge() > 0
                        ? r->getLand().resolvePatchRegion(centre_agent - r->getOriginAgent()) : nullptr;
                    if (!patch || !patch->getHasReceivedData()) incomplete = true;
                }
                continue;
            }
            // not on a reef patch (the patch has its own life)
            bool on_patch = false;
            for (const Patch& p : patches)
                if (sqrt((mx - p.mX) * (mx - p.mX) + (my - p.mY) * (my - p.mY)) < (F64)p.mR + 1.0) { on_patch = true; break; }
            if (on_patch) continue;
            Rng cell_rng(hash2(gcx, gcy, 0x652ULL));
            const U64 cell_seed = hash2(gcx, gcy, 0x6520ULL);
            auto rand_pos = [&](Rng& r, F64& gx, F64& gy) { gx = gx0 + (F64)r.range(0.2f, CELL_M - 0.2f); gy = gy0 + (F64)r.range(0.2f, CELL_M - 0.2f); };
            const F32 meadow = noise(mx, my, 30.0, 11ULL);
            // [COLD SEAS 2026-11-09] cool and cold seas: kelp forests where a 40 m noise is high (most of a cold sea's floor
            // 2-20 m deep, patches of a cool one; CHOSEN thresholds), 1-4 kelp in 16 m2 with their understory; wrack in the
            // shallows; urchins, starfish, anemones and stones on the gravel between
            const F32 kelp_zone = noise(mx, my, 40.0, 0x4E1ULL);
            const F32 kz_thr = cold > 0.5f ? 0.38f : 0.55f;
            if (warm < 0.35f && kelp_zone > kz_thr && centre_depth >= 2.f && centre_depth <= 20.f)
            {
                const F32 dens = smoothstep(kz_thr, kz_thr + 0.2f, kelp_zone) * (1.f - warm / 0.35f);
                const S32 n = 1 + (S32)(dens * 3.f + cell_rng.next());
                for (S32 k = 0; k < n; ++k)
                {
                    Rng rng(mix64(cell_seed ^ (0x500ULL + (U64)k)));
                    F64 gx, gy; rand_pos(rng, gx, gy);
                    F32 bed = 0.f, room = 0.f;
                    if (!spot(gx, gy, 0.5f, 2.f, 20.f, 0.6f, bed, room)) continue;
                    cool_item(rng, gx, gy, bed, room, 0.75f);
                }
                continue;
            }
            if (warm < 0.4f && centre_depth < 4.f)
            {
                // wrack in the shallows (CHOSEN 2-5 tufts in 16 m2, 0.3-0.7 m)
                const S32 n = cell_rng.irange(2, 5);
                for (S32 k = 0; k < n; ++k)
                {
                    Rng rng(mix64(cell_seed ^ (0x600ULL + (U64)k)));
                    F64 gx, gy; rand_pos(rng, gx, gy);
                    F32 bed = 0.f, room = 0.f;
                    if (!spot(gx, gy, 0.3f, 1.2f, 4.5f, 0.6f, bed, room) || room < 0.25f) continue;
                    const F32 h = llmin(room, rng.range(0.3f, 0.7f));
                    if (blocked(gx, gy, 0.3f, bed - 0.2f, bed + h)) continue;
                    plant_wrack(wr, local(gx, gy, bed - 0.03f), h, rng);
                    grew(bed, bed + h);
                }
                continue;
            }
            if (cold < 0.85f && meadow > 0.52f)
            {
                // CHOSEN: up to about 14 tufts in 16 m^2 in the heart of a meadow (each 5-7 blades)
                const F32 dens = smoothstep(0.52f, 0.72f, meadow) * (1.f - 0.6f * cold);
                const S32 n = (S32)(dens * 14.f + cell_rng.next());
                for (S32 k = 0; k < n; ++k)
                {
                    Rng rng(mix64(cell_seed ^ (0x100ULL + (U64)k)));
                    F64 gx, gy; rand_pos(rng, gx, gy);
                    F32 bed = 0.f, room = 0.f;
                    const F32 h = rng.range(0.3f, 0.75f) * (0.7f + 0.5f * dens);
                    if (!spot(gx, gy, 0.3f, 1.6f, 14.f, 0.85f, bed, room) || room < 0.25f) continue;
                    const F32 hh = llmin(h, room);
                    if (blocked(gx, gy, 0.3f, bed - 0.2f, bed + hh)) continue;
                    plant_seagrass(wr, local(gx, gy, bed - 0.03f), hh, rng);
                    grew(bed, bed + hh + 0.15f);
                }
            }
            else if (warm < 0.4f)
            {
                // the gravel of a cool or cold sea: an urchin group, a starfish, an anemone, a stone or the odd kelp
                if (cell_rng.next() < 0.25f + 0.2f * cold)
                {
                    Rng rng(mix64(cell_seed ^ 0x700ULL));
                    F64 gx, gy; rand_pos(rng, gx, gy);
                    F32 bed = 0.f, room = 0.f;
                    if (spot(gx, gy, 0.4f, 1.5f, 40.f, 0.6f, bed, room)) cool_item(rng, gx, gy, bed, room, 0.2f);
                }
            }
            else if (cell_rng.next() < 0.03f + 0.06f * cold)
            {
                Rng rng(mix64(cell_seed ^ 0x400ULL));
                F64 gx, gy; rand_pos(rng, gx, gy);
                const F32 rad = rng.range(0.2f, 0.6f);
                F32 bed = 0.f, room = 0.f;
                if (spot(gx, gy, rad, 1.5f, 60.f, 0.6f, bed, room) && room >= rad * 0.6f && !blocked(gx, gy, rad, bed - 0.3f, bed + rad))
                {
                    plant_stone(wr, local(gx, gy, bed), rad, rng);
                    grew(bed, bed + rad * 0.6f);
                }
            }
        }
    }
    if (incomplete) c.mRetryAt = LLFrameTimer::getElapsedSeconds() + INCOMPLETE_RETRY_S;
    c.mZMin = zmin;
    c.mZMax = zmax;
    mCostBuildUs += cost.getElapsedTimeF64() * 1e6;
    ++mChunksBuilt;
    if (verts.empty()) return true;
    if (verts.size() > MAX_CHUNK_VERTS) verts.resize(MAX_CHUNK_VERTS - MAX_CHUNK_VERTS % 3);
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_TEXCOORD1
                                                      | LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXTURE_INDEX);
    const U32 count = (U32)verts.size();
    if (!vb->allocateBuffer(count, 0))
    {
        LL_WARNS("WolfUnderSea") << "could not allocate a sea-plant vertex buffer of " << count << " vertices" << LL_ENDL;
        return false;
    }
    LLStrider<LLVector4a> pos;
    LLStrider<LLVector3> norm;
    LLStrider<LLVector2> uv, sw;
    LLStrider<LLColor4U> col;
    if (!vb->getVertexStrider(pos, 0, count) || !vb->getNormalStrider(norm, 0, count) || !vb->getTexCoord0Strider(uv, 0, count)
        || !vb->getTexCoord1Strider(sw, 0, count) || !vb->getColorStrider(col, 0, count))
    {
        vb->unmapBuffer();
        return false;
    }
    for (const PV& v : verts)
    {
        (*pos++).set(v.mP.mV[VX], v.mP.mV[VY], v.mP.mV[VZ], 0.f);   // w = texture index 0 (wolfsealife.cpp Writer::vert)
        *norm++ = v.mN;
        *uv++ = v.mUV;
        *sw++ = v.mSway;
        *col++ = v.mC;
    }
    vb->unmapBuffer();
    c.mVB = vb;
    c.mVerts = count;
    return true;
}

/** The atlas: drawn ATLAS_ROWS_A_FRAME rows a frame the first time plants or fish are wanted (CHOSEN: about a
 *  millisecond a frame, two seconds in all, instead of one long stall), then uploaded once with its mip levels. */
bool WolfUnderSea::atlasReady()
{
    if (mAtlas.notNull()) return true;
    if (mAtlasRaw.isNull())
    {
        mAtlasRaw = new LLImageRaw(ATLAS_PX, ATLAS_PX, 4);
        mAtlasRow = 0;
        if (!mAtlasRaw->getData()) { mAtlasRaw = nullptr; return false; }
    }
    constexpr S32 ATLAS_ROWS_A_FRAME = 8;
    fill_atlas_rows(mAtlasRaw.get(), mAtlasRow, mAtlasRow + ATLAS_ROWS_A_FRAME);
    mAtlasRow += ATLAS_ROWS_A_FRAME;
    if (mAtlasRow < ATLAS_PX) return false;
    mAtlas = LLViewerTextureManager::getLocalTexture(mAtlasRaw.get(), true);
    mAtlasRaw = nullptr;
    return mAtlas.notNull();
}

/** The chunks round the camera: wanted ones built (one a frame, nearest first), changed ones built again, far or long
 *  unwanted ones released. */
void WolfUnderSea::garden(const LLVector3& cam, F64 now, bool plants_on)
{
    // the obstacle map changed: the chunks whose cells changed are built again
    if (mScanGeneration != mCheckedGeneration)
    {
        mCheckedGeneration = mScanGeneration;
        for (auto& kv : mChunks)
            if (blockHash(kv.second.mOriginGlobal) != kv.second.mBlockHash) kv.second.mDirty = true;
    }
    const LLVector3d camg = gAgent.getPosGlobalFromAgent(cam);
    const U32 stamp = climateStamp();
    // release: beyond KEEP_R, or not wanted for a while
    for (auto it = mChunks.begin(); it != mChunks.end();)
    {
        const Chunk& c = it->second;
        const F64 dx = llmax(0.0, llmax(c.mOriginGlobal.mdV[VX] - camg.mdV[VX], camg.mdV[VX] - (c.mOriginGlobal.mdV[VX] + CHUNK_M)));
        const F64 dy = llmax(0.0, llmax(c.mOriginGlobal.mdV[VY] - camg.mdV[VY], camg.mdV[VY] - (c.mOriginGlobal.mdV[VY] + CHUNK_M)));
        const bool far_off = sqrt(dx * dx + dy * dy) > (F64)KEEP_R;
        if (far_off || now - c.mLastWanted > IDLE_RELEASE_S) it = mChunks.erase(it);
        else ++it;
    }
    if (!plants_on || !mScanDone) return;   // nothing grows until the builds round here are known
    // wanted: every chunk whose nearest edge is within BUILD_R; build the nearest missing (or stale) one this frame
    const S64 c0x = cell_of(camg.mdV[VX] - BUILD_R, CHUNK_M), c1x = cell_of(camg.mdV[VX] + BUILD_R, CHUNK_M);
    const S64 c0y = cell_of(camg.mdV[VY] - BUILD_R, CHUNK_M), c1y = cell_of(camg.mdV[VY] + BUILD_R, CHUNK_M);
    F64 best = 1e30;
    S64 bx = 0, by = 0;
    bool have = false;
    for (S64 x = c0x; x <= c1x; ++x)
    {
        for (S64 y = c0y; y <= c1y; ++y)
        {
            const F64 ox = (F64)x * CHUNK_M, oy = (F64)y * CHUNK_M;
            const F64 dx = llmax(0.0, llmax(ox - camg.mdV[VX], camg.mdV[VX] - (ox + CHUNK_M)));
            const F64 dy = llmax(0.0, llmax(oy - camg.mdV[VY], camg.mdV[VY] - (oy + CHUNK_M)));
            const F64 d = sqrt(dx * dx + dy * dy);
            if (d > (F64)BUILD_R) continue;
            auto it = mChunks.find(CellKey(x, y));
            if (it != mChunks.end())
            {
                Chunk& c = it->second;
                c.mLastWanted = now;
                const bool stale = c.mDirty || c.mClimate != stamp || (c.mRetryAt > 0.0 && now >= c.mRetryAt);
                if (!stale) continue;
            }
            if (d < best) { best = d; bx = x; by = y; have = true; }
        }
    }
    if (!have) return;
    Chunk fresh;
    fresh.mLastWanted = now;
    // a chunk whose buffer could not be had stays empty and is tried again in a while (not every frame)
    if (!buildChunk(fresh, bx, by)) fresh.mRetryAt = now + 10.0;
    mChunks[CellKey(bx, by)] = fresh;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the fish: where and how many (direct, pickSpot, spawn)
// ═══════════════════════════════════════════════════════════════════════════════════════

/** A place for a new school of `kind` between near_m and far_m of the camera (flat): water deep enough, no build, away
 *  from swimmers (a shark much further), and out of view where possible (wolfsealife.cpp pickPlace's rule). */
bool WolfUnderSea::pickSpot(FishKind kind, const LLVector3& cam, F32 near_m, F32 far_m, LLVector3& out, bool prefer_hidden) const
{
    const FishSpec& sp = FISH[kind];
    const F32 keep = kind == F_SHARK ? SHARK_KEEP_M * 2.f : 6.f;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    bool have_fallback = false;
    LLVector3 fallback;
    for (S32 t = 0; t < 24; ++t)
    {
        const F32 a = ll_frand(F_TWO_PI), d = near_m + ll_frand(far_m - near_m);
        LLVector3 p(cam.mV[VX] + cosf(a) * d, cam.mV[VY] + sinf(a) * d, 0.f);
        const F32 dep = depth(p);
        if (dep < sp.mMinDepth) continue;
        const F32 bed = water() - dep;
        // the height in the water column: silver fish anywhere in the upper water, reef fish low over the reef, big fish and
        // sharks a little over the bottom (CHOSEN)
        F32 z = 0.f;
        switch (kind)
        {
            case F_SILVER: z = bed + dep * (0.35f + ll_frand(0.4f)); break;
            case F_REEF:   z = bed + llmin(dep * 0.5f, 0.8f + ll_frand(1.5f)); break;
            case F_BIG:    z = bed + llmin(dep * 0.5f, 1.0f + ll_frand(1.2f)); break;
            case F_COD:    z = bed + llmin(dep * 0.6f, 1.2f + ll_frand(3.f)); break;
            default:       z = bed + llmin(dep * 0.5f, 2.0f + ll_frand(2.0f)); break;
        }
        p.mV[VZ] = llclamp(z, bed + BED_CLEAR_M + 0.3f, water() - SURFACE_CLEAR_M - 0.5f);
        const LLVector3d g = gAgent.getPosGlobalFromAgent(p);
        if (blocked(g.mdV[VX], g.mdV[VY], 2.f, p.mV[VZ] - 1.f, p.mV[VZ] + 1.f)) continue;
        bool near_someone = false;
        for (const LLVector3& th : mThreats) if ((th - p).length() < keep) { near_someone = true; break; }
        if (near_someone) continue;
        if (!prefer_hidden || camera->sphereInFrustum(p, 2.f) == 0) { out = p; return true; }
        if (!have_fallback) { fallback = p; have_fallback = true; }
    }
    if (have_fallback) out = fallback;
    return have_fallback;
}

void WolfUnderSea::spawn(FishKind kind, const LLVector3& cam, F64 now, S32 room, bool in_view)
{
    const FishSpec& sp = FISH[kind];
    LLVector3 at;
    // [LOOK 2026-11-09] reef fish and big fish gather round the reef (the reference photographs: they swarm the coral
    // heads, in view); the first school of each kind comes where the camera looks, later ones out of view where possible
    bool has_home = false;
    LLVector4 home;
    if (kind == F_REEF || kind == F_BIG || kind == F_COD)
    {
        std::vector<LLVector4> patches;
        const LLVector3d camg = gAgent.getPosGlobalFromAgent(cam);
        patchesNear(camg.mdV[VX], camg.mdV[VY], 25.f, patches);   // [LOOK 2026-11-09, round 3/] nearer (was 35 m)
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        for (S32 t = 0; t < 16 && !patches.empty() && !has_home; ++t)
        {
            const LLVector4& p = patches[(size_t)ll_rand((S32)patches.size())];
            const LLVector3 c(p.mV[VX], p.mV[VY], p.mV[VZ] + 1.f);
            const F32 dc = dist_vec(LLVector3(c.mV[VX], c.mV[VY], cam.mV[VZ]), cam);
            if (dc < 4.f || dc > 22.f) continue;   // [LOOK 2026-11-09, round 3/] at the reef near the camera (was 5-35 m)
            if (in_view && t < 12 && camera->sphereInFrustum(c, p.mV[VW]) == 0) continue;
            const F32 a = ll_frand(F_TWO_PI), d = p.mV[VW] + 0.5f + ll_frand(2.f);
            LLVector3 q(c.mV[VX] + cosf(a) * d, c.mV[VY] + sinf(a) * d, 0.f);
            const F32 dep = depth(q);
            if (dep < sp.mMinDepth) continue;
            const F32 bed = water() - dep;
            q.mV[VZ] = llclamp(bed + (kind == F_BIG ? 0.6f + ll_frand(0.9f) : 0.6f + ll_frand(1.6f)), bed + BED_CLEAR_M + 0.3f,
                               water() - SURFACE_CLEAR_M - 0.5f);
            const LLVector3d qg = gAgent.getPosGlobalFromAgent(q);
            if (blocked(qg.mdV[VX], qg.mdV[VY], 1.f, q.mV[VZ] - 0.8f, q.mV[VZ] + 0.8f)) continue;
            at = q;
            home = p;
            has_home = true;
        }
    }
    if (!has_home && !pickSpot(kind, cam, SPAWN_NEAR_M, SPAWN_FAR_M, at, !in_view)) return;
    const WolfRegionWeather& rw = WolfRegionWeather::instance();
    const bool warm_sea = rw.underSeaWarm() > 0.4f;
    // wrasse keep in small loose groups (3-8), not the reef fish's schools
    const S32 lo = (kind == F_REEF && !warm_sea) ? 3 : sp.mGroupLo;
    const S32 hi = (kind == F_REEF && !warm_sea) ? 8 : sp.mGroupHi;
    const S32 n = llmin(room, lo + ll_rand(hi - lo + 1));
    if (n <= 0) return;
    School s;
    s.mKind = kind;
    // [COLD SEAS 2026-11-09] cool seas' reef fish are wrasse (colourways 4-5), cold seas' big fish a cod or a wolffish (1-2),
    // cod schools a cod or a pollock/saithe colourway (0-1)
    s.mColourway = kind == F_REEF ? (U8)(warm_sea ? ll_rand(4) : 4 + ll_rand(2))
                 : kind == F_BIG ? (U8)(warm_sea ? 0 : (rw.underSeaCold() > 0.5f ? 1 + ll_rand(2) : 1))
                 : kind == F_COD ? (U8)ll_rand(2) : 0;
    s.mCruise = sp.mCruise * (0.85f + ll_frand(0.3f));
    s.mKeepUp = kind == F_BIG ? 1.2f : kind == F_SHARK ? 2.5f : kind == F_REEF ? 0.8f : kind == F_COD ? 1.5f : 0.f;
    s.mGoal = at;
    s.mNextGoal = 0.0;
    s.mHasHome = has_home;
    s.mHome = home;
    const S32 si = (S32)mSchools.size();
    mSchools.push_back(s);
    const F32 heading = ll_frand(F_TWO_PI);
    const F32 len = sp.mLenLo + ll_frand(sp.mLenHi - sp.mLenLo);
    const F32 spread = 0.7f * sqrtf((F32)n) + len;
    for (S32 i = 0; i < n; ++i)
    {
        Fish f;
        f.mSchool = si;
        f.mLen = len * (0.7f + ll_frand(0.6f));   // [LOOK 2026-11-09/] sizes vary within a school (it was +-10 %)
        f.mSpeedK = 0.85f + ll_frand(0.3f);
        f.mPos = at + LLVector3(ll_frand(2.f * spread) - spread, ll_frand(2.f * spread) - spread, (ll_frand(2.f) - 1.f) * spread * 0.3f);
        const F32 h = heading + (ll_frand(0.4f) - 0.2f);
        f.mVel = LLVector3(cosf(h), sinf(h), 0.f) * s.mCruise;
        f.mHeading = h;
        f.mPhase = ll_frand(F_TWO_PI);
        mFish.push_back(f);
    }
    newGoal(mSchools[(size_t)si], si, cam, now);
}

void WolfUnderSea::direct(const LLVector3& cam, F64 now, bool fish_on)
{
    // the sea round the camera: a 7 x 7 sample 12 m apart (+-36 m)
    S32 deep2 = 0, deep4 = 0, deep6 = 0, total = 0;
    for (S32 iy = -3; iy <= 3; ++iy)
    {
        for (S32 ix = -3; ix <= 3; ++ix)
        {
            const LLVector3 p(cam.mV[VX] + (F32)ix * 12.f, cam.mV[VY] + (F32)iy * 12.f, 0.f);
            if (!LLWorld::getInstance()->getRegionFromPosAgent(p)) continue;
            ++total;
            const F32 d = depth(p);
            if (d >= 2.5f) ++deep2;
            if (d >= 4.f) ++deep4;
            if (d >= 6.f) ++deep6;
        }
    }
    const F32 f2 = total ? (F32)deep2 / (F32)total : 0.f, f4 = total ? (F32)deep4 / (F32)total : 0.f;
    const F32 f6 = total ? (F32)deep6 / (F32)total : 0.f;
    const WolfRegionWeather& rw = WolfRegionWeather::instance();
    const F32 warm = rw.underSeaWarm(), cold = rw.underSeaCold();
    S32 want[FISH_KINDS] = { 0, 0, 0, 0, 0 };
    if (fish_on)
    {
        if (f2 >= 0.15f) want[F_SILVER] = cold > 0.7f ? 1 : (f2 > 0.5f ? 3 : 2);
        // [LOOK 2026-11-09, round 3] more reef fish (the reference reefs swarm with them): 2-6 schools (was 1-4)
        if (warm > 0.4f && f2 >= 0.15f) want[F_REEF] = llclamp((S32)(2.f + 4.f * warm * f2 + 0.5f), 2, 6);
        // [COLD SEAS 2026-11-09] Paul at TROMSO (11-09): "i cant see any fish or anything" - a cold sea had one silver school
        // and the odd big fish. Cool and cold seas: cod/pollock/saithe schools by depth (2-4 in a cold sea, 1-2 in a cool
        // one), and in a cool-temperate sea small groups of wrasse round the rock (the F_REEF kind in its cool colourways)
        if (warm <= 0.4f && f4 >= 0.2f) want[F_COD] = cold > 0.5f ? llclamp((S32)(2.f + 2.f * f4 + 0.5f), 2, 4) : (f4 > 0.5f ? 2 : 1);
        if (warm <= 0.4f && cold <= 0.5f && f2 >= 0.15f) want[F_REEF] = llclamp((S32)(1.f + 2.f * f2 + 0.5f), 1, 3);
        if (f4 >= 0.2f) want[F_BIG] = f4 > 0.6f ? 2 : 1;
        // the shark: warm water only, rolled once a dive and again every SHARK_ROLL_S ("rare")
        if (!mSharkDecided || now >= mNextSharkRoll)
        {
            mSharkDecided = true;
            mSharkWanted = ll_frand() < SHARK_CHANCE;
            mNextSharkRoll = now + SHARK_ROLL_S;
        }
        if (warm > 0.5f && f6 >= 0.3f && mSharkWanted) want[F_SHARK] = f6 > 0.7f ? 2 : 1;
        if (want[F_SHARK] == 2 && ll_frand() < 0.7f) want[F_SHARK] = 1;   // two only now and then
    }
    S32 have[FISH_KINDS] = { 0, 0, 0, 0, 0 };
    S32 sharks_here = 0;   // every shark still in the water, leaving or not: no new one while one swims off (11-09: 5 at once)
    for (School& s : mSchools)
    {
        if (s.mKind == F_SHARK) ++sharks_here;
        if (s.mLeaving) continue;
        if (have[s.mKind] >= want[s.mKind]) { s.mLeaving = true; continue; }
        ++have[s.mKind];
    }
    have[F_SHARK] = llmax(have[F_SHARK], sharks_here);
    // the cap (lowered by the frame budget, as the birds' and the dolphins')
    static LLCachedControl<S32> max_setting(gSavedSettings, "WolfViewerUnderSeaFishMax", DEFAULT_FISH_MAX);
    const S32 cap = llclamp((S32)((F32)llclamp((S32)max_setting, 0, HARD_FISH_MAX) * WolfFrameGovernor::budget()), 0, HARD_FISH_MAX);
    const S32 room = cap - (S32)mFish.size();
    if (room > 0 && fish_on)
    {
        // one new school a decision: the kind most short of what is wanted (sharks first, they are the rare sight)
        const FishKind order[FISH_KINDS] = { F_SHARK, F_REEF, F_COD, F_SILVER, F_BIG };
        for (FishKind k : order)
        {
            if (have[k] < want[k])
            {
                spawn(k, cam, now, room, have[k] == 0);
                break;
            }
        }
    }
    // the state now, every decision (debug only: the in-world test finds its sea spots and its scattering fish by these)
    {
        // the nearest reef patch (agent x, y, bed height, radius): the in-world test aims its pictures at it
        LLVector4 near_patch(0.f, 0.f, 0.f, 0.f);
        {
            std::vector<LLVector4> ps;
            const LLVector3d camg = gAgent.getPosGlobalFromAgent(cam);
            patchesNear(camg.mdV[VX], camg.mdV[VY], 40.f, ps);
            F32 best = 1e9f;
            for (const LLVector4& q : ps)
            {
                const F32 d = sqrtf((q.mV[VX] - cam.mV[VX]) * (q.mV[VX] - cam.mV[VX]) + (q.mV[VY] - cam.mV[VY]) * (q.mV[VY] - cam.mV[VY]));
                if (d < best) { best = d; near_patch = q; }
            }
        }
        S32 fleeing = 0, sharks = 0;
        for (const Fish& f : mFish) { if (f.mFlee > 0.f) ++fleeing; if (mSchools[(size_t)f.mSchool].mKind == F_SHARK) ++sharks; }
        U64 cv = 0;
        for (const auto& kv : mChunks) cv += kv.second.mVerts;
        LL_DEBUGS("WolfUnderSea") << "state: camera " << cam << " depth " << depth(cam) << " water " << water()
                                  << " under " << (LLViewerCamera::getInstance()->cameraUnderWater() ? 1 : 0) << " fish_on "
                                  << (fish_on ? 1 : 0) << " plants_on " << (mPlantsActive ? 1 : 0) << " scan " << (mScanDone ? 1 : 0)
                                  << " blocks " << mBlocks.size() << "; fish " << mFish.size() << " schools " << mSchools.size()
                                  << " fleeing " << fleeing << " sharks " << sharks << "; chunks " << mChunks.size()
                                  << " vertices " << cv << "; deep " << f2 << " " << f4 << " " << f6 << "; patch " << near_patch.mV[VX] << " "
                                  << near_patch.mV[VY] << " " << near_patch.mV[VZ] << " " << near_patch.mV[VW] << LL_ENDL;
    }
    // the debug log once a minute while there are fish or plants
    if (now - mLastLog > 60.0 && (!mFish.empty() || !mChunks.empty()))
    {
        mLastLog = now;
        U64 verts = 0;
        for (const auto& kv : mChunks) verts += kv.second.mVerts;
        LL_DEBUGS("WolfUnderSea") << mFish.size() << " fish in " << mSchools.size() << " schools; " << mChunks.size()
                                  << " plant chunks (" << verts << " vertices, " << mChunksBuilt << " built); deep " << f2
                                  << "; warm " << warm << " cold " << cold << "; cost "
                                  << (mCostFrames ? mCostIdleUs / mCostFrames : 0.0) << " us idle + "
                                  << (mCostFrames ? mCostFishMeshUs / mCostFrames : 0.0) << " us fish mesh a frame, "
                                  << (mChunksBuilt ? mCostBuildUs / mChunksBuilt : 0.0) << " us a chunk" << LL_ENDL;
        mCostIdleUs = mCostFishMeshUs = mCostBuildUs = 0.0;
        mCostFrames = 0;
        mChunksBuilt = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the fish: swimming (flocking, the sea's bounds, builds, swimmers)
// ═══════════════════════════════════════════════════════════════════════════════════════

/** Swimmers (every avatar near the camera) and the camera itself while it is under the water. */
void WolfUnderSea::threats(const LLVector3& cam)
{
    mThreats.clear();
    LLWorld::getInstance()->getAvatars(nullptr, &mAvatarScratch, gAgent.getPosGlobalFromAgent(cam), GONE_M + 10.f);
    for (const LLVector3d& g : mAvatarScratch) mThreats.push_back(gAgent.getPosAgentFromGlobal(g));
    if (LLViewerCamera::getInstance()->cameraUnderWater()) mThreats.push_back(cam);
}

void WolfUnderSea::newGoal(School& s, S32 si, const LLVector3& cam, F64 now)
{
    const FishSpec& sp = FISH[s.mKind];
    LLVector3 centre;
    S32 n = 0;
    for (const Fish& f : mFish) if (f.mSchool == si) { centre += f.mPos; ++n; }
    if (n == 0) return;
    centre /= (F32)n;
    s.mNextGoal = now + 4.0 + (F64)ll_frand(6.f);
    for (S32 t = 0; t < 10; ++t)
    {
        LLVector3 g;
        if (s.mLeaving)
        {
            // away from the camera, out of sight
            LLVector3 away = centre - cam;
            away.mV[VZ] = 0.f;
            if (away.isExactlyZero()) away.setVec(1.f, 0.f, 0.f);
            away.normVec();
            const F32 a = ll_frand(1.f) - 0.5f;
            g = centre + LLVector3(away.mV[VX] * cosf(a) - away.mV[VY] * sinf(a), away.mV[VX] * sinf(a) + away.mV[VY] * cosf(a), 0.f) * 30.f;
        }
        else if (s.mKind == F_SHARK)
        {
            // a slow circuit round the camera, 14-30 m out, a quarter turn on
            const F32 a = atan2f(centre.mV[VY] - cam.mV[VY], centre.mV[VX] - cam.mV[VX]) + 0.5f + ll_frand(0.6f);
            const F32 r = 14.f + ll_frand(16.f);
            g = LLVector3(cam.mV[VX] + cosf(a) * r, cam.mV[VY] + sinf(a) * r, 0.f);
        }
        else if (s.mHasHome)
        {
            // [LOOK 2026-11-09] round its reef patch: a point over the patch's edge or just off it
            const F32 a = ll_frand(F_TWO_PI), d = s.mHome.mV[VW] * (0.5f + ll_frand(0.6f)) + ll_frand(2.5f);
            g = LLVector3(s.mHome.mV[VX] + cosf(a) * d, s.mHome.mV[VY] + sinf(a) * d, 0.f);
        }
        else
        {
            // wander 8-20 m, pulled back towards the camera when the school is getting far
            const F32 a = ll_frand(F_TWO_PI), d = 8.f + ll_frand(12.f);
            g = centre + LLVector3(cosf(a) * d, sinf(a) * d, 0.f);
            LLVector3 to_cam = g - cam;
            to_cam.mV[VZ] = 0.f;
            if (to_cam.length() > 35.f) g = cam + to_cam * (35.f / to_cam.length());
        }
        const F32 dep = depth(g);
        if (dep < sp.mMinDepth) continue;
        const F32 bed = water() - dep;
        F32 z = 0.f;
        switch (s.mKind)
        {
            case F_SILVER: z = bed + dep * (0.3f + ll_frand(0.45f)); break;
            default:       z = bed + llmin(dep * 0.5f, s.mKeepUp + ll_frand(1.2f)); break;
        }
        // over its reef patch, above the reef's own rock (buildChunk's mounds are at most REEF_ROCK_TOP_M over the bed and are
        // not in the obstacle map, which holds the prims of builds only)
        if (s.mHasHome && dist_vec(LLVector3(g.mV[VX], g.mV[VY], 0.f), LLVector3(s.mHome.mV[VX], s.mHome.mV[VY], 0.f)) < s.mHome.mV[VW])
            z = llmax(z, bed + REEF_ROCK_TOP_M + 0.3f);
        g.mV[VZ] = llclamp(z, bed + BED_CLEAR_M + 0.3f, water() - SURFACE_CLEAR_M - 0.5f);
        const LLVector3d gg = gAgent.getPosGlobalFromAgent(g);
        if (blocked(gg.mdV[VX], gg.mdV[VY], 1.f, g.mV[VZ] - 1.f, g.mV[VZ] + 1.f)) continue;
        if (s.mKind == F_SHARK)
        {
            bool near_someone = false;
            for (const LLVector3& th : mThreats) if ((th - g).length() < SHARK_KEEP_M * 1.5f) { near_someone = true; break; }
            if (near_someone) continue;
        }
        s.mGoal = g;
        return;
    }
    s.mGoal = centre;   // nowhere better: stay about here
}

void WolfUnderSea::swim(F32 dt, F64 now, const LLVector3& cam)
{
    if (dt <= 0.f) return;
    const F32 w = water();
    LLViewerRegion* agent_region = gAgent.getRegion();
    const size_t ns = mSchools.size();
    std::vector<std::vector<S32>> members(ns);
    for (S32 i = 0; i < (S32)mFish.size(); ++i) members[(size_t)mFish[(size_t)i].mSchool].push_back(i);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    std::vector<bool> drop_school(ns, false);
    std::vector<bool> drop_fish(mFish.size(), false);
    for (size_t si = 0; si < ns; ++si)
    {
        School& s = mSchools[si];
        const FishSpec& sp = FISH[s.mKind];
        const std::vector<S32>& m = members[si];
        if (m.empty()) { drop_school[si] = true; continue; }
        LLVector3 centre, avg_vel;
        for (S32 i : m) { centre += mFish[(size_t)i].mPos; avg_vel += mFish[(size_t)i].mVel; }
        centre /= (F32)m.size();
        avg_vel /= (F32)m.size();
        // gone: far off, or out of view and well away, or leaving and out of view
        const F32 dc = (centre - cam).length();
        const bool hidden = camera->sphereInFrustum(centre, 2.f) == 0;
        if (dc > GONE_M || (hidden && dc > GONE_HIDDEN_M && s.mLeaving) || (hidden && dc > GONE_M * 0.8f))
        {
            drop_school[si] = true;
            continue;
        }
        if (now >= s.mNextGoal || (s.mGoal - centre).length() < 2.f) newGoal(s, (S32)si, cam, now);
        // the drawn surface over the school (one sample a school a frame: WolfBoatRock's whole wave model)
        const F32 wave = (WolfBoatRock::instanceExists() && agent_region)
                         ? WolfBoatRock::instance().drawnWaveHeight(agent_region, centre.mV[VX], centre.mV[VY]) : 0.f;
        s.mCeiling = w + llmin(0.f, std::isfinite(wave) ? wave : 0.f) - SURFACE_CLEAR_M;
        s.mAlarm = llmax(0.f, s.mAlarm - dt);
        for (S32 i : m)
        {
            Fish& f = mFish[(size_t)i];
            LLVector3 acc;
            // flocking (not the shark: it swims alone). Weights CHOSEN by eye.
            if (s.mKind != F_SHARK && m.size() > 1)
            {
                LLVector3 sum_pos, sum_vel;
                S32 seen = 0;
                for (S32 j : m)
                {
                    if (j == i) continue;
                    const Fish& o = mFish[(size_t)j];
                    LLVector3 d = f.mPos - o.mPos;
                    const F32 dist = d.length();
                    const F32 sep = llmax(sp.mSep, f.mLen * 2.5f);   // [LOOK 2026-11-09/] room by size
                    if (dist < sep && dist > 1e-4f) acc += d * ((sep - dist) / (sep * dist) * 6.f);
                    if (dist < sp.mView) { sum_pos += o.mPos; sum_vel += o.mVel; ++seen; }
                }
                if (seen > 0)
                {
                    // [LOOK 2026-11-09] softer alignment and cohesion (1.2, 0.8 before): a loose school, not a block
                    acc += (sum_vel / (F32)seen - f.mVel) * 0.6f;
                    acc += (sum_pos / (F32)seen - f.mPos) * 0.5f;
                }
            }
            // the school's goal, and this fish's own place round it ([LOOK 2026-11-09] milling; CHOSEN radii)
            if (now >= f.mJitterUntil)
            {
                const F32 jr = s.mKind == F_REEF ? 1.5f : s.mKind == F_SILVER ? 1.0f : s.mKind == F_COD ? 1.5f : 0.5f;
                f.mJitter = LLVector3(ll_frand(2.f) - 1.f, ll_frand(2.f) - 1.f, (ll_frand(2.f) - 1.f) * 0.4f) * jr;
                f.mJitterUntil = now + 2.0 + (F64)ll_frand(3.f);
            }
            LLVector3 to_goal = s.mGoal + f.mJitter - f.mPos;
            const F32 gl = to_goal.length();
            if (gl > 0.01f) acc += to_goal * (s.mCruise / gl);
            // swimmers: small and big fish flee and the school bolts; a shark turns away and keeps its distance
            for (const LLVector3& th : mThreats)
            {
                LLVector3 away = f.mPos - th;
                const F32 d = away.length();
                if (d < 1e-3f) continue;
                if (s.mKind == F_SHARK)
                {
                    if (d < SHARK_KEEP_M * 1.5f)
                    {
                        away.mV[VZ] *= 0.3f;
                        acc += away * (4.f * (SHARK_KEEP_M * 1.5f - d) / d);
                        if (now + 1.0 < s.mNextGoal) s.mNextGoal = now;   // a new goal away from them at once
                    }
                    continue;
                }
                if (d < sp.mFlee)
                {
                    f.mFlee = 1.5f + ll_frand(0.7f);
                    away.mV[VZ] *= 0.4f;
                    away.normVec();
                    f.mFleeDir = away;
                    s.mAlarm = 2.f;
                    s.mAlarmFrom = th;
                }
            }
            if (f.mFlee <= 0.f && s.mAlarm > 0.f && s.mKind != F_SHARK)
            {
                // a school-mate saw a swimmer: bolt the same way, a moment later
                LLVector3 away = f.mPos - s.mAlarmFrom;
                away.mV[VZ] *= 0.4f;
                if (!away.isExactlyZero() && away.length() < sp.mFlee * 3.f)
                {
                    away.normVec();
                    f.mFlee = 0.8f + ll_frand(0.6f);
                    f.mFleeDir = away;
                }
            }
            F32 target = s.mCruise * f.mSpeedK * (s.mLeaving ? 1.3f : 1.f);
            if (f.mFlee > 0.f)
            {
                f.mFlee -= dt;
                acc += f.mFleeDir * (sp.mBurst * 6.f);
                target = sp.mBurst;
            }
            // the sea's bounds, looked at a little ahead: the bed (and the height the kind keeps over it), the surface,
            // shallows and land, and builds
            LLVector3 dir = f.mVel;
            if (dir.isExactlyZero()) dir.setVec(cosf(f.mHeading), sinf(f.mHeading), 0.f);
            dir.normVec();
            const LLVector3 ahead = f.mPos + dir * llmax(1.f, f.mLen * 3.f);
            const F32 bed_ahead = seabed(ahead);
            const F32 dep_ahead = bed_ahead > -1e8f ? w - bed_ahead : 0.f;
            if (dep_ahead < sp.mMinDepth * 0.6f)
            {
                // shallows or land ahead: turn back towards the goal and the deeper water behind
                acc -= LLVector3(dir.mV[VX], dir.mV[VY], 0.f) * (3.f * llmax(1.f, s.mCruise));
                if (now + 0.5 < s.mNextGoal) s.mNextGoal = now + 0.5;
            }
            else
            {
                const F32 floor_z = bed_ahead + BED_CLEAR_M + s.mKeepUp * 0.5f;
                if (f.mPos.mV[VZ] < floor_z + 0.3f) acc.mV[VZ] += (floor_z + 0.3f - f.mPos.mV[VZ]) * 4.f;
            }
            if (f.mPos.mV[VZ] > s.mCeiling - 0.2f) acc.mV[VZ] -= (f.mPos.mV[VZ] - s.mCeiling + 0.2f) * 4.f;
            // over its reef patch: up and over the reef's rock (see newGoal), not through it
            if (s.mHasHome && dep_ahead > 0.f)
            {
                const F32 hd = dist_vec(LLVector3(f.mPos.mV[VX], f.mPos.mV[VY], 0.f), LLVector3(s.mHome.mV[VX], s.mHome.mV[VY], 0.f));
                const F32 over = bed_ahead + REEF_ROCK_TOP_M + 0.2f;
                if (hd < s.mHome.mV[VW] * 0.75f && f.mPos.mV[VZ] < over) acc.mV[VZ] += (over - f.mPos.mV[VZ]) * 4.f;
            }
            {
                const LLVector3d ga = gAgent.getPosGlobalFromAgent(ahead);
                if (blocked(ga.mdV[VX], ga.mdV[VY], f.mLen, ahead.mV[VZ] - 0.4f, ahead.mV[VZ] + 0.4f))
                {
                    // a build ahead: turn aside (always the same side for a fish, so it does not dither) and slow
                    const F32 side = (i & 1) ? 1.f : -1.f;
                    acc += LLVector3(-dir.mV[VY] * side, dir.mV[VX] * side, 0.f) * (4.f * llmax(1.f, s.mCruise)) - f.mVel;
                    if (now + 0.5 < s.mNextGoal) s.mNextGoal = now + 0.5;
                }
            }
            // move
            f.mVel += acc * dt;
            F32 speed = f.mVel.length();
            speed += (target - speed) * llmin(1.f, dt * 2.f);
            speed = llclamp(speed, s.mCruise * 0.3f, llmax(sp.mBurst, s.mCruise * 1.3f));
            if (f.mVel.isExactlyZero()) f.mVel = dir;
            f.mVel.normVec();
            f.mVel *= speed;
            const F32 vz_max = 0.5f * speed;
            f.mVel.mV[VZ] = llclamp(f.mVel.mV[VZ], -vz_max, vz_max);
            f.mPos += f.mVel * dt;
            // hard bounds: never in the bed or out of the water; aground (the bed rose under it) = gone (sea life's rule)
            const F32 bed = seabed(f.mPos);
            if (bed < -1e8f || s.mCeiling - bed < BED_CLEAR_M + 0.3f) { drop_fish[(size_t)i] = true; continue; }
            f.mPos.mV[VZ] = llclamp(f.mPos.mV[VZ], bed + BED_CLEAR_M, s.mCeiling);
            // heading, pitch and the tail beat (about 2.5 beats a body length swum, CHOSEN)
            const F32 hs = sqrtf(f.mVel.mV[VX] * f.mVel.mV[VX] + f.mVel.mV[VY] * f.mVel.mV[VY]);
            if (hs > 1e-3f) f.mHeading = atan2f(f.mVel.mV[VY], f.mVel.mV[VX]);
            f.mPitch = atan2f(f.mVel.mV[VZ], llmax(hs, 1e-3f));
            f.mPhase += dt * F_TWO_PI * llclamp(2.5f * speed / f.mLen, 0.5f, 12.f) * (s.mKind == F_SHARK ? 0.35f : 1.f);
            if (f.mPhase > 1000.f) f.mPhase -= F_TWO_PI * 100.f;
        }
    }
    // compact: dropped schools and fish go, the school indices follow
    std::vector<S32> remap(ns, -1);
    std::vector<School> schools;
    for (size_t si = 0; si < ns; ++si)
    {
        if (drop_school[si]) continue;
        remap[si] = (S32)schools.size();
        schools.push_back(mSchools[si]);
    }
    std::vector<Fish> fish;
    fish.reserve(mFish.size());
    for (size_t i = 0; i < mFish.size(); ++i)
    {
        const Fish& f = mFish[i];
        if (drop_fish[i] || remap[(size_t)f.mSchool] < 0) continue;
        fish.push_back(f);
        fish.back().mSchool = remap[(size_t)f.mSchool];
    }
    mSchools.swap(schools);
    mFish.swap(fish);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the fish: the look (our own shapes; side-to-side swimming)
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    constexpr S32 FISH_SIDES = 6, FISH_SIDES_LOD = 4;
    // nose fan + 5 bands + tail cap + tail fin (2) + dorsal (1) + pectorals (2)
    constexpr S32 TRIS_PER_FISH = FISH_SIDES + 5 * FISH_SIDES * 2 + FISH_SIDES + 2 + 1 + 2;
    constexpr S32 VERTS_PER_FISH = TRIS_PER_FISH * 3;
    const S32 LOD_STATIONS[4] = { 0, 2, 4, 6 };

    struct Colours { F32 mBack[3], mFlank[3], mBelly[3], mFin[3]; };
    // CHOSEN, ours: a silver schooling fish; four reef fish colourways after the kinds in the reference photographs (Bonaire
    // shallows: a blue-grey fish with a yellow side stripe and tail, a blue damselfish, a white fish with dark bands, a
    // yellow fish with blue lines - the patterns are drawn in buildFish); a grouper-like brown (warm) and a cod-like olive
    // (cold) big fish; a grey shark with a white belly
    const Colours SILVER_C = { { 0.20f, 0.30f, 0.38f }, { 0.62f, 0.66f, 0.70f }, { 0.86f, 0.87f, 0.89f }, { 0.45f, 0.52f, 0.58f } };
    const Colours REEF_C[4] = {
        { { 0.36f, 0.43f, 0.55f }, { 0.74f, 0.77f, 0.80f }, { 0.92f, 0.92f, 0.90f }, { 0.95f, 0.80f, 0.20f } },
        { { 0.10f, 0.26f, 0.62f }, { 0.18f, 0.42f, 0.82f }, { 0.50f, 0.68f, 0.88f }, { 0.14f, 0.34f, 0.72f } },
        { { 0.88f, 0.88f, 0.84f }, { 0.92f, 0.92f, 0.88f }, { 0.95f, 0.95f, 0.92f }, { 0.90f, 0.84f, 0.48f } },
        { { 0.86f, 0.70f, 0.22f }, { 0.92f, 0.78f, 0.28f }, { 0.95f, 0.88f, 0.55f }, { 0.92f, 0.78f, 0.26f } },
    };
    const Colours BIG_C[3] = {
        { { 0.35f, 0.27f, 0.20f }, { 0.50f, 0.40f, 0.30f }, { 0.75f, 0.68f, 0.58f }, { 0.30f, 0.23f, 0.17f } },
        { { 0.30f, 0.33f, 0.22f }, { 0.48f, 0.48f, 0.34f }, { 0.85f, 0.83f, 0.75f }, { 0.32f, 0.34f, 0.24f } },
        // [COLD SEAS 2026-11-09] a wolffish: blue-grey with dark bands (drawn in buildFish)
        { { 0.36f, 0.40f, 0.44f }, { 0.52f, 0.56f, 0.58f }, { 0.70f, 0.70f, 0.68f }, { 0.34f, 0.38f, 0.40f } },
    };
    // [COLD SEAS 2026-11-09] cod (olive-brown back, pale lateral line belly) and pollock/saithe (dark green-grey); CHOSEN tones
    const Colours COD_C[2] = {
        { { 0.42f, 0.40f, 0.28f }, { 0.62f, 0.60f, 0.46f }, { 0.88f, 0.86f, 0.80f }, { 0.45f, 0.42f, 0.30f } },
        { { 0.22f, 0.28f, 0.26f }, { 0.50f, 0.56f, 0.56f }, { 0.85f, 0.86f, 0.86f }, { 0.26f, 0.32f, 0.30f } },
    };
    // [COLD SEAS 2026-11-09] wrasse of cool rocky shores: a mottled green-brown one and an orange one with a blue back
    const Colours WRASSE_C[2] = {
        { { 0.36f, 0.42f, 0.28f }, { 0.52f, 0.54f, 0.36f }, { 0.74f, 0.70f, 0.50f }, { 0.40f, 0.44f, 0.30f } },
        { { 0.30f, 0.40f, 0.66f }, { 0.82f, 0.54f, 0.30f }, { 0.90f, 0.74f, 0.54f }, { 0.78f, 0.50f, 0.30f } },
    };
    const Colours SHARK_C = { { 0.36f, 0.40f, 0.44f }, { 0.52f, 0.56f, 0.60f }, { 0.88f, 0.88f, 0.88f }, { 0.34f, 0.38f, 0.42f } };

    /** [LOOK 2026-11-09] "tone down the cartoon flat colours slightly": a fifth of the way to the colour's own grey. */
    LLColor4U tone(const LLColor4U& c)
    {
        const F32 lum = 0.3f * (F32)c.mV[0] + 0.59f * (F32)c.mV[1] + 0.11f * (F32)c.mV[2];
        auto t = [lum](U8 v) { return (U8)llclamp((F32)v + (lum - (F32)v) * 0.2f, 0.f, 255.f); };
        return LLColor4U(t(c.mV[0]), t(c.mV[1]), t(c.mV[2]), c.mV[3]);
    }

    struct FishWriter
    {
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        S32 n = 0;
        void vert(const LLVector3& p, const LLVector3& nn, const LLColor4U& c, const LLVector2& t)
        {
            (*pos++).set(p.mV[VX], p.mV[VY], p.mV[VZ], 0.f);
            *norm++ = nn;
            *uv++ = t;
            *col++ = c;
            ++n;
        }
        /** A triangle with the atlas's plain white (fins). */
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& na, const LLVector3& nb,
                 const LLVector3& nc, const LLColor4U& ca, const LLColor4U& cb, const LLColor4U& cc)
        {
            const LLVector2 w = atlas_uv(T_WHITE, 0.5f, 0.5f);
            vert(a, na, ca, w); vert(b, nb, cb, w); vert(c, nc, cc, w);
        }
        /** A triangle of the body, with its scales (T_SCALES). */
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& na, const LLVector3& nb,
                 const LLVector3& nc, const LLColor4U& ca, const LLColor4U& cb, const LLColor4U& cc, const LLVector2& ta,
                 const LLVector2& tb, const LLVector2& tc)
        {
            vert(a, na, ca, ta); vert(b, nb, cb, tb); vert(c, nc, cc, tc);
        }
    };
}

bool WolfUnderSea::ensureFishBuffer()
{
    if (mFishVB.notNull()) return true;
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    // unindexed (drawArrays), as wolfsealife.cpp: 400 fish are about 92 000 vertices, past the U16 index limit
    if (!vb->allocateBuffer((U32)(HARD_FISH_MAX * VERTS_PER_FISH), 0))
    {
        LL_WARNS("WolfUnderSea") << "could not allocate the fish vertex buffer" << LL_ENDL;
        return false;
    }
    mFishVB = vb;
    return true;
}

void WolfUnderSea::buildFish(S32& verts)
{
    verts = 0;
    if (mFish.empty() || !ensureFishBuffer()) return;
    LLTimer cost;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    const F32 far_m = llmin(gAgentCamera.mDrawDistance, GONE_M);
    std::vector<S32> vis;
    vis.reserve(mFish.size());
    for (S32 i = 0; i < (S32)mFish.size() && (S32)vis.size() < HARD_FISH_MAX; ++i)
    {
        const Fish& f = mFish[(size_t)i];
        if ((f.mPos - cam).length() > far_m || !camera->sphereInFrustum(f.mPos, f.mLen)) continue;
        vis.push_back(i);
    }
    if (vis.empty()) return;
    const S32 count = (S32)vis.size() * VERTS_PER_FISH;
    FishWriter wr;
    if (!mFishVB->getVertexStrider(wr.pos, 0, count) || !mFishVB->getNormalStrider(wr.norm, 0, count)
        || !mFishVB->getTexCoord0Strider(wr.uv, 0, count) || !mFishVB->getColorStrider(wr.col, 0, count))
    {
        mFishVB->unmapBuffer();
        return;
    }
    struct RingPt { LLVector3 p, n; LLColor4U c; LLVector2 t; };
    RingPt rings[7][FISH_SIDES];
    for (S32 vi : vis)
    {
        const Fish& f = mFish[(size_t)vi];
        const School& s = mSchools[(size_t)f.mSchool];
        const FishSpec& sp = FISH[s.mKind];
        const Colours& cl = s.mKind == F_SILVER ? SILVER_C
                          : s.mKind == F_REEF ? (s.mColourway < 4 ? REEF_C[s.mColourway] : WRASSE_C[(s.mColourway - 4) % 2])
                          : s.mKind == F_BIG ? BIG_C[s.mColourway % 3] : s.mKind == F_COD ? COD_C[s.mColourway % 2] : SHARK_C;
        const F32 len = f.mLen;
        const bool distant = (f.mPos - cam).length() > FISH_LOD_M;
        const S32 sides = distant ? FISH_SIDES_LOD : FISH_SIDES;
        const S32 n_rings = distant ? 4 : 7;
        const F32 cp = cosf(f.mPitch);
        const LLVector3 fwd(cosf(f.mHeading) * cp, sinf(f.mHeading) * cp, sinf(f.mPitch));
        LLVector3 L = LLVector3(0.f, 0.f, 1.f) % fwd;
        if (L.isExactlyZero()) L.setVec(0.f, 1.f, 0.f);
        L.normVec();
        const LLVector3 U = fwd % L;
        // the side-to-side body wave of a swimming fish: at s (0 head .. 1 tail) the axis moves A s^2 sin(phase - 2 pi 0.9 s)
        const F32 amp = (s.mKind == F_SHARK ? 0.07f : 0.11f) * len;
        auto wave = [&](F32 t) { return amp * t * t * sinf(f.mPhase - F_TWO_PI * 0.9f * t); };
        auto axis = [&](F32 t) { return f.mPos + fwd * ((0.5f - t) * len) + L * wave(t); };
        auto radius_at = [&](F32 t)
        {
            for (S32 i = 1; i < 7; ++i)
                if (t <= STATIONS[i]) return (sp.mRadius[i - 1] + (sp.mRadius[i] - sp.mRadius[i - 1]) * (t - STATIONS[i - 1]) / (STATIONS[i] - STATIONS[i - 1])) * len;
            return sp.mRadius[6] * len;
        };
        // the reef colourways' patterns (REEF_C): 0 a yellow stripe along the side, 2 dark bands across, 3 blue lines along
        // [COLD SEAS 2026-11-09] and the wolffish's dark bands (BIG_C[2]) as reef colourway 2's; the wrasse are mottled by
        // the scale texture only
        const S32 way = (s.mKind == F_REEF && s.mColourway < 4) ? (S32)s.mColourway : (s.mKind == F_BIG && s.mColourway == 2) ? 2 : -1;
        for (S32 r = 0; r < n_rings; ++r)
        {
            const S32 i = distant ? LOD_STATIONS[r] : r;
            const F32 t = STATIONS[i], rad = sp.mRadius[i] * len;
            const LLVector3 c0 = axis(t);
            const bool band = way == 2 && (fabsf(t - 0.24f) < 0.05f || fabsf(t - 0.52f) < 0.06f || fabsf(t - 0.76f) < 0.04f);
            for (S32 k = 0; k < sides; ++k)
            {
                const F32 th = (F32)k * F_TWO_PI / (F32)sides, sy = sinf(th), cz = cosf(th);
                RingPt& e = rings[r][k];
                e.p = c0 + L * (rad * sp.mWide * sy) + U * (rad * cz);
                LLVector3 nn = L * (sy / sp.mWide) + U * cz;
                nn.normVec();
                e.n = nn;
                const F32 tt = (cz + 1.f) * 0.5f;
                if (band) e.c = rgb(0.12f, 0.12f, 0.12f);
                else if (way == 0 && fabsf(sy) > 0.8f && t > 0.08f) e.c = rgb(0.95f, 0.80f, 0.20f);
                else if (way == 3 && fmodf(tt * 4.f, 1.f) < 0.3f) e.c = rgb(0.25f, 0.42f, 0.75f);
                else if (tt > 0.5f) e.c = mixc(cl.mFlank, cl.mBack, (tt - 0.5f) * 2.f);
                else e.c = mixc(cl.mBelly, cl.mFlank, tt * 2.f);
                e.c = tone(e.c);
                e.t = atlas_uv(T_SCALES, t, (F32)k / (F32)sides);
            }
        }
        const LLVector3 nose = rings[0][0].p;
        const LLColor4U back_c = tone(rgb(cl.mBack[0], cl.mBack[1], cl.mBack[2]));
        for (S32 r = 1; r < n_rings; ++r)
        {
            for (S32 k = 0; k < sides; ++k)
            {
                const S32 k2 = (k + 1) % sides;
                const RingPt& A0 = rings[r - 1][k];
                const RingPt& A1 = rings[r - 1][k2];
                const RingPt& B0 = rings[r][k];
                const RingPt& B1 = rings[r][k2];
                if (r == 1) { wr.tri(nose, B1.p, B0.p, fwd, B1.n, B0.n, back_c, B1.c, B0.c, A0.t, B1.t, B0.t); continue; }
                wr.tri(A0.p, B1.p, B0.p, A0.n, B1.n, B0.n, A0.c, B1.c, B0.c, A0.t, B1.t, B0.t);
                wr.tri(A0.p, A1.p, B1.p, A0.n, A1.n, B1.n, A0.c, A1.c, B1.c, A0.t, A1.t, B1.t);
            }
        }
        // the tail cap (to the root of the tail fin)
        const LLVector3 tail_root = axis(0.96f);
        for (S32 k = 0; k < sides; ++k)
        {
            const S32 k2 = (k + 1) % sides;
            const RingPt& A0 = rings[n_rings - 1][k];
            const RingPt& A1 = rings[n_rings - 1][k2];
            wr.tri(A0.p, A1.p, tail_root, A0.n, A1.n, -fwd, A0.c, A1.c, A0.c);
        }
        const LLColor4U fin_c = tone(rgb(cl.mFin[0], cl.mFin[1], cl.mFin[2]));
        LLVector3 fin_n = L * 0.3f + U * 0.95f;
        fin_n.normVec();
        // the tail fin: upright, forked (a shark's upper lobe longer)
        const F32 up_lobe = len * (s.mKind == F_SHARK ? 0.17f : 0.12f), down_lobe = len * (s.mKind == F_SHARK ? 0.08f : 0.12f);
        const LLVector3 t_tip = axis(1.1f), t_mid = axis(1.0f);
        wr.tri(axis(0.88f), t_tip + U * up_lobe, t_mid, fin_n, fin_n, fin_n, fin_c, fin_c, fin_c);
        wr.tri(axis(0.88f), t_mid, t_tip - U * down_lobe, fin_n, fin_n, fin_n, fin_c, fin_c, fin_c);
        // the dorsal fin
        const F32 dh = len * (s.mKind == F_SHARK ? 0.11f : s.mKind == F_REEF ? 0.08f : 0.06f);
        const F32 r30 = radius_at(0.30f), r52 = radius_at(0.52f), r36 = radius_at(0.36f);
        wr.tri(axis(0.30f) + U * (r30 * 0.95f), axis(0.52f) + U * (r52 * 0.95f), axis(0.40f) + U * (r36 + dh),
               fin_n, fin_n, fin_n, fin_c, fin_c, fin_c);
        // pectoral fins (the big fish and the shark; small fish's are too small to see)
        if (s.mKind == F_BIG || s.mKind == F_SHARK)
        {
            const F32 reach = len * (s.mKind == F_SHARK ? 0.16f : 0.09f);
            const F32 rp = radius_at(0.28f);
            for (S32 side = 0; side < 2; ++side)
            {
                const F32 sg = side == 0 ? 1.f : -1.f;
                const LLVector3 a = axis(0.24f) + L * (sg * rp * sp.mWide * 0.8f) - U * (rp * 0.4f);
                const LLVector3 b = axis(0.36f) + L * (sg * rp * sp.mWide * 0.8f) - U * (rp * 0.4f);
                const LLVector3 tip = axis(0.38f) + L * (sg * (rp * sp.mWide + reach)) - U * (rp * 0.6f + reach * 0.35f);
                wr.tri(a, b, tip, fin_n, fin_n, fin_n, fin_c, fin_c, fin_c);
            }
        }
    }
    mFishVB->unmapBuffer();
    verts = wr.n;
    mCostFishMeshUs += cost.getElapsedTimeF64() * 1e6;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// each frame
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfUnderSea::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;
    LLViewerRegion* region = nullptr;
    const bool on = wanted(region);
    if (!on && mChunks.empty() && mFish.empty() && mBlocks.empty()) { mPlantsActive = mFishActive = false; return; }   // off: nothing at all
    LLTimer cost;
    // the agent frame moves on a region crossing or a rebase: the fish move with it (the plants are kept in world metres)
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        const LLVector3 delta(mLastOrigin - origin);
        if (!mLastOrigin.isExactlyZero()) rebase(delta);
        mLastOrigin = origin;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (!on)
    {
        // the owner's switch, the resident's, leaving Wolf or a sky that is not an Earth sky: gone at once
        reset();
        mPlantsActive = mFishActive = false;
        return;
    }
    if (mHaveLastCamera && (cam - mLastCamera).length() > TELEPORT_M) reset();   // a teleport: start again
    mLastCamera = cam;
    mHaveLastCamera = true;
    const bool under = LLViewerCamera::getInstance()->cameraUnderWater();
    const F32 height = cam.mV[VZ] - water();
    mPlantsActive = under || height < PLANT_ABOVE_M;
    mFishActive = under || height < FISH_ABOVE_M;
    if (mPlantsActive || mFishActive)
    {
        scanObjects(cam);
        atlasReady();
    }
    garden(cam, now, mPlantsActive);
    if (!mFishActive && height > FISH_ABOVE_M + CAMERA_ONWARDS_M && !mFish.empty())
    {
        mFish.clear();
        mSchools.clear();
        mSharkDecided = false;   // the next dive rolls its own shark
    }
    if (mFishActive || !mFish.empty()) threats(cam);   // once a frame: placing new schools and swimming both read them
    if (now >= mNextDirect)
    {
        mNextDirect = now + DIRECT_SECS;
        direct(cam, now, mFishActive);
    }
    if (!mFish.empty()) swim(dt, now, cam);
    if (mPlantsActive || !mFish.empty())
    {
        mCostIdleUs += cost.getElapsedTimeF64() * 1e6;
        ++mCostFrames;
    }
}

void WolfUnderSea::renderDeferred()
{
    const bool plants = mPlantsActive && !mChunks.empty() && mAtlas.notNull();   // the plants wait for their atlas (2 s)
    if (!plants && mFish.empty()) return;
    if (LLViewerFetchedTexture::sWhiteImagep.isNull()) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolfsealife.cpp renderDeferred (from wolfbirds.cpp, from WolfFarGround::renderDeferred): the same G-buffer
    // program family, matrices and attributes, so the plants and fish are lit, shadowed, hazed and water-fogged as the
    // terrain round them is. No back-face culling: the blades and fins are single sheets.
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    if (plants)
    {
        const bool sway_ok = gWolfUnderSeaProgram.isComplete();
        LLGLSLShader& shader = sway_ok ? gWolfUnderSeaProgram : gDeferredNonIndexedDiffuseAlphaMaskProgram;
        shader.bind();
        shader.setMinimumAlpha(0.5f);
        if (sway_ok)
        {
            static LLStaticHashedString s_time("wolf_sway_time");
            static LLStaticHashedString s_dir("wolf_sway_dir");
            const F64 t = LLFrameTimer::getElapsedSeconds();
            shader.uniform1f(s_time, (F32)fmod(t, 60.0));   // wolfUnderSeaV.glsl: every frequency is n/60 Hz
            const F32 a = CURRENT_RAD + 0.5f * (F32)sin(t / 83.0);   // the surge's direction wanders slowly round CURRENT_RAD (CHOSEN)
            shader.uniform2f(s_dir, cosf(a), sinf(a));
        }
        gGLLastMatrix = nullptr;
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(gGLModelView);
        gGL.getTexUnit(0)->bindFast(mAtlas);
        const F32 draw_r = llmin(BUILD_R, gAgentCamera.mDrawDistance);
        const F32 half = CHUNK_M * 0.5f;
        for (auto& kv : mChunks)
        {
            Chunk& c = kv.second;
            if (c.mVB.isNull() || c.mVerts == 0) continue;
            const LLVector3 o = gAgent.getPosAgentFromGlobal(c.mOriginGlobal);
            const LLVector3 centre(o.mV[VX] + half, o.mV[VY] + half, (c.mZMin + c.mZMax) * 0.5f);
            const F32 dz = (c.mZMax - c.mZMin) * 0.5f;
            const F32 radius = sqrtf(2.f * half * half + dz * dz) + 1.5f;   // + the sway
            const F32 flat = sqrtf((centre.mV[VX] - cam.mV[VX]) * (centre.mV[VX] - cam.mV[VX]) + (centre.mV[VY] - cam.mV[VY]) * (centre.mV[VY] - cam.mV[VY]));
            if (flat - half * 1.415f > draw_r) continue;
            if (!camera->sphereInFrustum(centre, radius)) continue;
            gGL.pushMatrix();
            gGL.translatef(o.mV[VX], o.mV[VY], o.mV[VZ]);
            c.mVB->setBuffer();
            c.mVB->drawArrays(LLRender::TRIANGLES, 0, c.mVerts);
            gGL.popMatrix();
        }
        shader.unbind();
    }
    if (!mFish.empty())
    {
        S32 verts = 0;
        buildFish(verts);
        if (verts > 0)
        {
            LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
            shader.bind();
            shader.setMinimumAlpha(0.5f);
            gGLLastMatrix = nullptr;
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.loadMatrix(gGLModelView);
            if (mAtlas.notNull()) gGL.getTexUnit(0)->bindFast(mAtlas);
            else gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
            mFishVB->setBuffer();
            mFishVB->drawArrays(LLRender::TRIANGLES, 0, (U32)verts);
            shader.unbind();
        }
    }
}
