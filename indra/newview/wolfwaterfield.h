/**
 * @file wolfwaterfield.h
 * @brief WolfViewer: per-region water depth + open-water exposure fields for the water shader.
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

#ifndef WOLF_WATERFIELD_H
#define WOLF_WATERFIELD_H

#include "llsingleton.h"
#include <map>
#include <set>
#include <vector>

#include "llmath.h"
#include "lluuid.h"   // [BASIC WOLF SEA 2026-10-19/] Field::mRegionID
#include "v2math.h"   // [DRY BUILDS 2026-10-26/] WolfWaterClearance
#include "v3dmath.h"

class LLViewerRegion;

// Source: wolfstorm/js/world/terrain/terrain_manager.js _bakeWaterDepthTexture() and
// _bakeSwellExposure() — the same two fields, the same layout, the same numbers.
//
// The water shader had no idea where the land was: its only depth came from the
// refraction buffer, per fragment, which cannot reach the VERTEX stage and knows nothing
// beyond the screen. These two textures give it what WolfStorm's water has had since
// August: the terrain height under every point of the region (DEPTH field, RGBA32F:
// R = height, GB = unit direction toward the beach scaled by a slope/fetch confidence,
// A = box-smoothed height for a stable wave phase), and how far each point is from any
// land over a 3x-region span, neighbours included (EXPOSURE field, R32F, smoothstep 6..150 m).
// From them: the swell calms toward every shore and dies in rivers, breakers rise and
// steepen as they shoal, the swash line pulses up the beach, the shallows tint.
class WolfWaterField : public LLSingleton<WolfWaterField>
{
    LLSINGLETON(WolfWaterField);
    ~WolfWaterField();

public:
    struct Field
    {
        U32 mDepthTex = 0;      // GL texture, RES x RES RGBA32F over the FIELD (mX0, mY0, mSizeX, mSizeY)
        U32 mExpoTex = 0;       // GL texture, ERES x ERES RGBA32F over the 3x span: R exposure, G distance to land, B distance from the open sea
        // <WolfViewer 2026-09-20> The depth field's region-space origin and size: (0, 0) and
        // the region up to WINDOW_M an edge, else a WINDOW_M window that follows the camera
        // (fieldWindow). Shader uniform depthOrigin / depthRegionSize.
        F32 mX0 = 0.f;
        F32 mY0 = 0.f;
        F32 mSizeX = 256.f;
        F32 mSizeY = 256.f;
        F32 mWaterLevel = 20.f;
        F32 mExpoX0 = -256.f;   // region-space origin and size of the exposure span
        F32 mExpoY0 = -256.f;
        F32 mExpoSX = 768.f;
        F32 mExpoSY = 768.f;
        // <WolfViewer 2026-09-21> The deep-water surf wavenumber the A-channel optical path
        // was integrated with. The shader multiplies the path by THIS, not by whatever the
        // sliders say right now, so phase and path can never disagree while a rebake is due.
        // 0 = this field has no surf path (no region record, or surf switched off).
        F32 mSurfK0 = 0.f;
        U64 mStamp = 0;
        // <WolfViewer 2026-09-20> the newest terrain stamp seen and when it first appeared: a
        // terrain change re-bakes only once the stamp has held for STAMP_SETTLE_SECS.
        U64 mPendingStamp = 0;
        F64 mPendingSince = 0.0;
        F64 mBakedAt = 0.0;
        bool mReady = false;
        LLUUID mRegionID;   // [BASIC WOLF SEA 2026-10-19/] the region baked for (handles repeat across grids; WolfWaterField::get)
        // CPU copies of the two bakes, for the boat rocker's wave sampler (wolfboatrock.cpp
        // mirrors the vertex stage on the CPU and needs the same fields). RES*RES*4 and
        // ERES*ERES floats — 1.3 MB per region.
        std::vector<F32> mDepth;    // RGBA per texel, row-major, y outer
        std::vector<F32> mExpo;     // R per texel
        // [WAVES 2026-09-07] The painted wave zones (wolfwavezones.cpp fill) over the SAME
        // span as the exposure bake, one texel per 16 m cell: surf 1, open 0.55, calm 0.15,
        // off 0. Sampled by waterV.glsl as wolfZoneField; the CPU copy feeds zoneAt().
        // <WolfViewer 2026-10-01> The texture is RG: R the energy above, G the SURF WEIGHT
        // (bake: the surf cells grown, then box-blurred, by a quarter of the surf wavelength), which is
        // what scales the surf train's height. CPU copy mSurfW, read by surfWeightAt().
        U32 mZoneTex = 0;
        S32 mZoneW = 0;
        S32 mZoneH = 0;
        F32 mZoneX0 = -256.f;
        F32 mZoneY0 = -256.f;
        F32 mZoneSX = 768.f;
        F32 mZoneSY = 768.f;
        std::vector<F32> mZone;
        std::vector<F32> mSurfW;   // <WolfViewer 2026-10-01/> G of mZoneTex, same layout as mZone
    };

    static constexpr S32 RES = 256;
    static constexpr S32 ERES = 256;
    // <WolfViewer 2026-09-21> Deep water for the surf dispersion: the depth used where no
    // region covers a texel, and the cap on the depth fed to the wavenumber. waterV.glsl uses
    // the same 30 m beyond the depth field. A 36 m wave has k0 h = 5.2 there, where Guo's
    // shallow-water correction is under 0.1 % — anything past ~20 m is deep and the exact
    // value cannot matter.
    static constexpr F32 DEEP_REF_M = 30.f;
    /**
     * <WolfViewer 2026-10-01> The seabed slope the SURF TRAIN feels where the real water is
     * shallower: 1 in 4 up to the shoreline. Paul 10-01: "if i specify 30m waves they should
     * start at 30m" — a 30 m wave needs ~38 m of water to stand (breaker index 0.78), and a
     * region's sea is a few metres deep, so on the real depth a big surf was flattened long
     * before the beach. On this slope it keeps its full height to h = H / 0.78, i.e. 5.1 H from
     * the shore (154 m for 30 m, 15 m for 3 m), then breaks and runs in as a bore that shrinks
     * to nothing at the waterline. Why 1 in 4: breakers PLUNGE (a barrel) for an Iribarren
     * number 0.5 < xi < 3.3, xi = tan(slope) / sqrt(H / L0) (Battjes 1974; Wikipedia, Iribarren
     * number); the surf wavelength is 12 H, so xi = 0.25 * sqrt(12) = 0.87 at every size.
     * Source: terrain_manager.js SURF_REEF_SLOPE.
     */
    static constexpr F32 SURF_REEF_SLOPE = 0.25f;
    /**
     * <WolfViewer 2026-10-01> The depth the surf train uses: the real depth or the reef slope's
     * depth at that distance from land, whichever is deeper. The path bake, waterV.glsl,
     * wolfsurfcurl.cpp and wolfboatrock.cpp all use it. Source: TerrainManager.surfDepth().
     */
    static F32 surfDepth(F32 h, F32 dist_to_land) { return llmax(h, SURF_REEF_SLOPE * dist_to_land); }
    static constexpr F32 CHECK_INTERVAL_SECS = 2.f;
    static constexpr F32 MIN_REBAKE_SECS = 8.f;  // [2026-09-10] never re-shape the sea faster than this per region
    // <WolfViewer 2026-09-20> A terrain change is acted on only after the stamp has been
    // unchanged for this long. On Wolf Nation (51,200 m) patches stream in for as long as you
    // fly, so the stamp never stopped changing and the fields re-baked every MIN_REBAKE_SECS
    // — the sea re-shaped itself every 8 s, the "water flickers when I move" report (and the
    // Dire Wolf freeze of 09-10, which disabling fields above 4096 m had hidden). Now the sea
    // settles once, when the streaming pauses; a window move still re-bakes at once.
    static constexpr F32 STAMP_SETTLE_SECS = 10.f;
    // <WolfViewer 2026-10-05> A region that has gone out of range keeps its field this long (at
    // most GONE_KEEP_MAX of them), so coming back finds its sea at once. Paul, sailing back into
    // Madrigal: "my 30 ft waves had become small until after about 10 seconds" - the field had
    // been dropped and the re-bake waits for the terrain (LLSurface::hasZData), 7 s in the log.
    static constexpr F64 GONE_KEEP_SECS = 600.0;
    static constexpr size_t GONE_KEEP_MAX = 8;
    static constexpr F32 STAMP_MARGIN_M = 256.f;   // patches this far outside the window still count (the 3x exposure span reads them)
    // <WolfViewer 2026-09-20> Fields bake over the whole region up to this many metres an
    // edge (8 m depth texels), else over a camera-following window this wide. Replaces the
    // 09-10 rule "no field above 4096 m" (RES texels would be > 16 m): Wolf Nation (51,200 m)
    // had no depth, no exposure and NO ZONE FIELD at all, so every stretch of water there
    // carried full open-sea swell and painted zones were ignored — Paul: "the ocean and
    // rivers don't look right on the 200x200 ... I draw on the water and it doesn't work".
    // WolfStorm terrain_manager.js WATER_FIELD_WINDOW_M same.
    static constexpr F32 WINDOW_M = 2048.f;
    /**
     * Where a region's field should sit right now: the whole region below WINDOW_M, else a
     * WINDOW_M window snapped to an eighth of itself and held inside the region, centred on
     * the camera — except that an existing field keeps its window while the camera is still
     * in the inner half of it, so the sea never re-shapes itself under a boat for a step.
     */
    static void fieldWindow(const LLViewerRegion* regionp, const Field* current, F32& x0, F32& y0, F32& sx, F32& sy);

    /** Every frame from LLAppViewer::idle(); bakes at most one region per check. */
    void idle();
    /** Drop every field (shutdown, GL reset). */
    void reset();
    /** [WAVES 2026-09-07] How many bakes have run, and the surf texel count of the agent region's last one (editor confirmation). */
    U32 bakeCount() const { return mBakes; }
    U32 lastSurfTexels() const { return mLastSurfTexels; }
    /** The field for a region, or nullptr before its first bake. */
    const Field* get(const LLViewerRegion* regionp) const;

    /**
     * Open-water exposure at region-relative (rx, ry) metres: the nearest texel of the
     * exposure bake, 1.0 (open sea) outside its span.
     * Source: wolfstorm terrain_manager.js _swellExposureAt().
     */
    static F32 exposureAt(const Field& f, F32 rx, F32 ry);
    /**
     * The depth bake, bilinear (as the GPU reads it), at region-space (rx, ry): out[0] height,
     * out[1..2] beachward unit direction x confidence, out[3] box-smoothed height. False
     * outside the field. Source: terrain_manager.js _waveSampleCPU() depth-texture read.
     */
    static bool depthAt(const Field& f, F32 rx, F32 ry, F32 out[4]);
    /** [WAVES 2026-09-07] Zone energy at region-relative (rx, ry): nearest texel, 0.55 outside. */
    static F32 zoneAt(const Field& f, F32 rx, F32 ry);
    /**
     * <WolfViewer 2026-10-01> Surf weight at region-relative (rx, ry): nearest texel like
     * zoneAt, 0 outside. The surf train's height scale is smoothstep(0.62, 0.95, 0.55 + 0.45 w)
     * — the old smoothstep on the energy, from the surf cells grown and blurred by a quarter wavelength.
     * Source: wave_zones.js surfWeightAt().
     */
    static F32 surfWeightAt(const Field& f, F32 rx, F32 ry);
    /** [SURF HEIGHT 2026-10-02] The shader's surf zone for a surf weight G (waterV.glsl surfZone), and its inverse. */
    static F32 surfZoneOf(F32 w);
    static F32 surfWeightFor(F32 z);
    /**
     * <WolfViewer 2026-10-01> Box-blur radius, in zone texels, of the surf weight: a quarter of
     * the surf train's deep-water wavelength 2 pi / k0, at least 1 (the 3x3 the energy already
     * gets) and at most SURF_BLUR_MAX_TEXELS. Source: wave_zones.js surfBlurRadius().
     */
    static S32 surfBlurRadius(F32 k0, F32 texel_m);
    static constexpr S32 SURF_BLUR_MAX_TEXELS = 16;
    /** [SURF 2026-09-07] Distance to land (exposure bake G), bilinear; 4000 outside the span. */
    static F32 distanceAt(const Field& f, F32 rx, F32 ry);
    /** <WolfViewer 2026-09-20/> Distance from the open sea (exposure bake B), bilinear; 4000 outside the span. */
    static F32 openDistanceAt(const Field& f, F32 rx, F32 ry);
    /**
     * <WolfViewer 2026-09-21/> The surf train's OPTICAL path from the open sea (exposure bake
     * A), bilinear; 40000 outside the span. Multiply by Field::mSurfK0 for the wave phase.
     */
    static F32 openPathAt(const Field& f, F32 rx, F32 ry);
    /**
     * <WolfViewer 2026-09-21/> The surf train's DEEP-WATER wavenumber: 2*pi over
     * max(surfLength, 12 * surfHeight), the same wavelength waterV.glsl builds.
     *
     * It takes no region because the surf parameters are not per region here: lldrawpoolwater
     * .cpp reads WolfWaveZones::current() — the AGENT's region — and sends those uniforms to
     * every water plane in the scene, neighbours included. The bake has to integrate the
     * optical path against the same wavelength the shader will draw with, so it asks the same
     * question. 0 = no region record, i.e. no surf train at all.
     */
    static F32 surfK0();
    /**
     * <WolfViewer 2026-09-21/> The local wavenumber of a wave of deep-water wavenumber k0 in
     * depth h. Guo (2002) Coastal Engineering 45, 71-74, restated by Fenton (2006) eq. (3):
     * k = k0 * (1 - exp(-(k0 h)^(5/4)))^(-2/5). Exact in the deep and shallow limits, 0.7 %
     * between. KEEP BYTE-IDENTICAL with waterV.glsl wolfSurfK() and the WolfStorm mirrors.
     */
    static F32 surfWavenumber(F32 k0, F32 h);
    /**
     * <WolfViewer 2026-09-21/> Green's-law shoaling gain Ks = sqrt(cg0 / cg), the factor a
     * wave's height grows by as it shoals from deep water into depth h, from conservation of
     * energy flux with the frequency held constant. KEEP BYTE-IDENTICAL with the shaders.
     */
    static F32 surfShoalGain(F32 k0, F32 k, F32 h);
    /** [WAVES 2026-09-07] Bake every field again at the next check (a layout arrived / was saved / is previewed). */
    void invalidate();

private:
    void bake(LLViewerRegion* regionp, Field& f);
    static U64 terrainStamp(LLViewerRegion* regionp);
    /**
     * <WolfViewer 2026-10-02> The terrain revisions of every region within STAMP_MARGIN_M of this
     * one, folded together: the bake reads neighbours' land too (heightAt), and that is the one
     * input no other trigger caught — a timed re-bake every 20 s (REBAKE_SECS) stood in for it,
     * re-baking every field forever whether anything had changed or not.
     */
    static U64 neighbourStamp(LLViewerRegion* regionp);
    /** <WolfViewer 2026-09-20/> terrainStamp over the patches inside region rect (x0, y0, sx, sy) only: a 51,200 m region has 10 million patches, a 2048 m window 16 thousand. */
    static U64 terrainStampIn(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy);
    /** Terrain height at region-relative (px, py) metres, answering from this region or
     *  any live neighbour; false = no region covers the point (open water). */
    static bool heightAt(LLViewerRegion* regionp, F32 px, F32 py, F32& out);
    static void upload(U32& tex, S32 w, S32 h, U32 internal_format, U32 format, const F32* data);
    static void releaseField(Field& f);

    std::map<U64, Field> mFields;
    std::map<U64, F64> mGoneSince;   // <WolfViewer 2026-10-05/> fields of regions out of range, kept GONE_KEEP_SECS
    F64 mNextCheck = 0.0;
    // Scratch, reused across bakes.
    std::vector<F32> mH, mTmp, mSm, mDist, mOpen, mDepthData, mExpoData, mZoneData;   // <WolfViewer 2026-09-20/> mOpen
    std::vector<F32> mEDepth, mPath;   // <WolfViewer 2026-09-21/> depth per exposure texel, and the eikonal path baked from it
    std::vector<F32> mSurfMask, mSurfTmp, mSurfGrown, mSurfW, mZoneRG;   // <WolfViewer 2026-10-01/> the surf weight bake
    std::vector<F32> mSurfScale, mScaleNum, mScaleDen, mScaleTmpN, mScaleTmpD;   // [SURF HEIGHT 2026-10-02/] the surf height bake
    bool mRebakeAll = false;   // [WAVES 2026-09-07] set by invalidate()
    U32 mBakes = 0;
    U32 mLastSurfTexels = 0;
};

/**
 * [DRY BUILDS 2026-10-26] THE WATERLINE CLEARANCE FIELD. Paul 2026-10-26: "water should not appear
 * on land" (unless painted or on a prim). Jupiter Rowland (social thread): with the Wolf sea's waves
 * on, builds made for flat OpenSim water - docks, jetties, boat decks, floors a few cm above the
 * water - got washed over, and crests climbed beaches.
 * [OPEN SEA SHORES 2026-10-26] Paul, after the first build: "the waves are cool but look unnatural
 * against the shore line ... on the outer edge of the region NOT RIVERS INSIDE or LAKES INSIDE the
 * water should behave MORE like waves - at the moment it's like there is a fixed edge to where the
 * waves go". So the rule is now:
 *
 *   OBJECTS (unchanged): the displaced sea is never higher than the cap of a prim at the waterline
 *   over that prim's footprint (its lowest point when it stands clear of the water, its highest when
 *   it pierces it). Held at the VERTICES, with the margin below, everywhere.
 *   LAND: never above the water level over terrain at or above it, EXCEPT the swash band of an
 *   OPEN-SEA beach: there the water may run up to at most the run-up height waterV.glsl computes
 *   (Hunt 1959, capped at RUNUP_CAP_M), and only over terrain no higher than that. On inland water
 *   (rivers, lakes, anything far from the open sea through water) and wherever no per-pixel depth
 *   field covers the water, the land rule is held at the vertices as before; on open-sea water with
 *   a depth field it is held PER PIXEL in waterF.glsl (exact, no calm band), so the waves can shoal,
 *   break and run up the beach.
 *
 * Every SOURCE s - a terrain texel with any terrain point at or above its region's water height, or
 * a texel under the box of a prim that reaches the waterline band - carries a cap height z_s. Three
 * camera-centred levels (N x N texels of TEXEL_M[k] metres) store, after a two-pass chamfer whose
 * weights are divided by CHAMFER so the 8-neighbour distance never exceeds the Euclidean one:
 *
 *   R  S_all(p) = min over all sources ( z_s + ALPHA |p - s| ) - ALPHA e
 *   G  D(p)     = max( min over all sources |p - s| - e, 0 )
 *   B  S_dry(p) = the same min over prims and INLAND land only (open-sea beach land left to the pixel)
 *   A  open-sea weight: 1 on water joined THROUGH WATER to the open sea within OPEN_NEAR_M, 0 past
 *      OPEN_FAR_M (rivers, lakes, lagoons up a channel), carried onto the land beside it, blurred.
 *      The open sea: water off every region (the open ocean), the coarsest level's border, and any
 *      water OPEN_FETCH_M or more from land (a fetch that big is a sea of its own).
 *
 * e = 1.5 x the texel diagonal: bilinear interpolation of a Lipschitz grid plus the source's place
 * inside its texel. All are LOWER bounds of the true values. waterV.glsl caps every vertex: upward
 * displacement <= S - ALPHA * deadZone(lattice step) - plane height, horizontal <= BETA * D.
 * deadZone = 1.5 step / (1 - 3 BETA) makes the cap hold BETWEEN vertices: a displaced triangle over
 * a source point x has every vertex within deadZone of x (its edges are at most 1.5 x the lattice
 * step, and each vertex moves at most BETA x its distance to x), so every vertex is capped at or
 * below z_x and so is the flat triangle between them.
 * ALPHA = 0.78 x 0.25: McCowan's breaker index on the surf train's 1-in-4 reef slope
 * (WolfWaterField::SURF_REEF_SLOPE) - a crest may stand no higher than a breaking wave on that
 * slope at that distance from a source.
 * The method: Crest Ocean System's sea floor depth cache (scene geometry rendered from above into a
 * cache that attenuates the waves), here a CPU raster of the terrain and the prims' boxes.
 * Mirrors, keep in step: waterV.glsl wolfClearSample() / the [DRY BUILDS] cap / the swash, waterF.glsl
 * the per-pixel land rule, wolfboatrock.cpp waveHeight().
 */
class WolfWaterClearance : public LLSingleton<WolfWaterClearance>
{
    LLSINGLETON(WolfWaterClearance);
    ~WolfWaterClearance();

public:
    static constexpr S32 LEVELS = 3;
    static constexpr S32 N = 256;                     // texels an edge, every level
    static constexpr F32 ALPHA = 0.78f * 0.25f;       // waterV.glsl WOLF_CLEAR_ALPHA
    static constexpr F32 BETA = 0.1f;                 // waterV.glsl WOLF_CLEAR_BETA
    static constexpr F32 CHAMFER = 1.0824f;           // max octagonal / Euclidean distance (at 22.5 degrees)
    static constexpr F32 BAND_M = 40.f;               // prims whose bottom is up to this far above the water count (30 m surf)
    static constexpr F32 BIG = 1.0e6f;                // no source in reach
    static constexpr F32 EYE_CLEAR_M = 0.3f;          // the camera guard: crests stay this far under the eye
    static constexpr F32 RUNUP_CAP_M = 0.5f;          // the swash band's ceiling over the water level (Paul: "a small swash band")
    static constexpr F32 OPEN_FETCH_M = 150.f;        // water this far from land is open sea (the exposure bake's own rule)
    static constexpr F32 OPEN_NEAR_M = 100.f;         // through-water distance from the open sea: fully open below this...
    static constexpr F32 OPEN_FAR_M = 300.f;          // ...inland water past this
    static constexpr F32 OPEN_ONTO_LAND_M = 8.f;      // the open weight carried onto the beach beside open water
    static constexpr F64 OBJECT_INTERVAL_SECS = 1.0;  // prims are re-rastered this often
    static constexpr F64 TERRAIN_INTERVAL_SECS = 1.0; // a terrain change is re-rastered at most this often
    static constexpr F32 MOVING_MPS = 0.5f;           // faster linksets (vehicles under way) are not sources
    static const F32 TEXEL_M[LEVELS];                 // 1, 4, 16 m: windows of 256, 1024 and 4096 m
    // [OPEN SEA SHORES 2026-10-26] The TERRAIN blocks waterF.glsl holds the land per pixel with, camera-centred so they
    // serve every region whatever its size (Paul's var regions run to 3072 m and more): each texel holds the HIGHEST
    // drawn terrain within one texel of its centre (the grid points of every grid cell touching that 2t square - the
    // drawn terrain is triangles between grid points), so the bilinear read at any point is at or above the terrain
    // there - a conservative land test, never a missed sandbar. -BIG where no terrain stands at or above its water.
    static constexpr S32 TBLOCKS = 2;
    static const F32 TERRAIN_M[TBLOCKS];              // 1, 4 m: windows of 256 and 1024 m
    static constexpr S32 ROWS = (LEVELS + TBLOCKS) * N;   // the texture: the cap levels, then the terrain blocks
    static constexpr F32 HELD_MARGIN_M = 32.f;        // waterV.glsl: a vertex is pixel-held this far (+ its dead zone) inside a block...
    static constexpr F32 HELD_BLEND_M = 32.f;         // ...blended in over this much more

    /** What wolfClearSample() returns: the four channels, combined over the levels. */
    struct Sample
    {
        F32 mSAll = -BIG;   // cap height from every source
        F32 mD = 0.f;       // distance to the nearest source
        F32 mSDry = -BIG;   // cap height from prims and inland land
        F32 mOpen = 0.f;    // open-sea weight
    };

    /** Every frame from WolfWaterField::idle() on every grid that shows the Wolf sea. */
    void idle();
    void reset();
    bool isReady() const { return mReady; }
    U32 texture() const { return mTex; }
    /** Level k's texel-grid corner in agent space this frame (the grid is held in global space). */
    LLVector2 levelOriginAgent(S32 k) const;
    /** Terrain block b's texel-grid corner in agent space this frame. */
    LLVector2 terrainOriginAgent(S32 b) const;
    /** waterV.glsl wolfPixelHeld: 0..1, how far inside a terrain block (minus the dead zone) the point is. */
    F32 pixelHeld(F32 ax, F32 ay, F32 dead) const;
    /** The lowest cap any source can have: the lowest water height of the live regions. */
    F32 minSourceZ() const { return mMinZ; }
    /** CPU twin of waterV.glsl wolfClearSample(). False = no level holds the point (the shader draws no swell there). */
    bool sample(F32 ax, F32 ay, Sample& out) const;
    /** waterV.glsl's dead zone for a lattice step (0 = not a graded lattice: the 128 m maximum step). */
    static F32 deadZone(F32 lattice_step) { return 1.5f * (lattice_step > 0.f ? lattice_step : 128.f) / (1.f - 3.f * BETA); }
    /** llvowater.cpp wolf_graded_axis's step at agent (ax, ay), focus = the camera, step scale 1. */
    static F32 latticeStepAt(F32 ax, F32 ay);
    /** waterV.glsl wolfSoftCap: up * tanh(dz / up) for a crest, 0 when up <= 0; a trough keeps TROUGH_KEEP of itself at least. */
    static F32 softCap(F32 dz, F32 up, F32 calm_ref);
    /** waterV.glsl wolfSoftCap: up * tanh(dz / up), odd in dz (the depth limit), 0 when up <= 0. */
    static F32 tanhCap(F32 dz, F32 up) { return up > 0.f ? up * tanhf(llclamp(dz / up, -10.f, 10.f)) : 0.f; }
    static constexpr F32 TROUGH_KEEP = 0.35f;
    /**
     * waterV.glsl's `up` at agent (ax, ay) over a plane at plane_z: the vertex-held room to rise. pixel_held
     * (0..1, waterV.glsl wolfPixelHeld) blends from every land source to the inland ones only. -BIG = no level.
     */
    F32 upwardRoom(F32 ax, F32 ay, F32 plane_z, F32 pixel_held) const;

private:
    void rasterTerrain(S32 k);
    void rasterTerrainMax(S32 b);
    void classifyOpen(S32 k);
    void rasterObjects();
    void solve(S32 k, const std::vector<F32>& src, std::vector<F32>& S, std::vector<F32>* D);
    void uploadRows(S32 row0, S32 rows, const F32* data);
    void uploadLevels();
    void uploadTerrain(S32 b);
    U64 terrainStamp() const;

    LLVector3d mOrigin[LEVELS];                 // global SW corner of each level's texel grid
    bool mHaveOrigin[LEVELS] = { false, false, false };
    U64 mTerrainStamp[LEVELS] = { 0, 0, 0 };
    F64 mTerrainAt[LEVELS] = { 0.0, 0.0, 0.0 };
    bool mLandValid[LEVELS] = { false, false, false };
    std::vector<F32> mLand[LEVELS];             // terrain cap per texel (its region's water height), BIG = water
    std::vector<U8> mVoid[LEVELS];              // 1 = no live region covers the texel: the open ocean
    std::vector<F32> mOpenDist[LEVELS];         // through-water distance from the open sea, BIG = not joined
    std::vector<F32> mOpenW[LEVELS];            // open-sea weight, water and the beach beside it
    std::vector<F32> mSrcAll[LEVELS];           // terrain + prims
    std::vector<F32> mSrcDry[LEVELS];           // inland terrain + prims
    std::vector<F32> mS[LEVELS], mD[LEVELS], mSDry[LEVELS];   // solved, margins folded in
    std::vector<F32> mUpload, mTmp;
    LLVector3d mTOrigin[TBLOCKS];
    bool mHaveTOrigin[TBLOCKS] = { false, false };
    bool mTerrValid[TBLOCKS] = { false, false };
    U64 mTerrStamp[TBLOCKS] = { 0, 0 };
    F64 mTerrAt[TBLOCKS] = { 0.0, 0.0 };
    std::vector<F32> mTerr[TBLOCKS];
    U32 mTex = 0;
    bool mTexAllocated = false;
    bool mReady = false;
    bool mObjectsValid = false;
    F32 mMinZ = 20.f;
    F64 mNextObjects = 0.0;
};

#endif // WOLF_WATERFIELD_H
