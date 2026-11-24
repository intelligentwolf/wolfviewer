/**
 * @file wolfnaturalwater.h
 * @brief Natural water: streams and pools derived from the terrain heightmap (WolfViewer)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — IntelligentWolf Ltd
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_NATURALWATER_H
#define WOLF_NATURALWATER_H

#include "llsingleton.h"
#include "llpointer.h"
#include "v3math.h"
#include "llvowater.h"
#include "wolfobjectsweep.h"   // [FAR DRAW PERF 2026-11-14/] the built mask is read in the shared object pass
#include <map>
#include <memory>
#include <set>
#include <vector>

class LLViewerRegion;

// Viewer-side water where the terrain says water would be. From the region's heightmap:
//   - POOLS: basins above sea level are flood-filled to their spill height (priority flood)
//     and get a level mesh over exactly the basin's cells — not its bounding box, which on a
//     mountain would hang out over the flank wherever the ground is below the spill level;
//   - STREAMS: every cell drains to its lowest neighbour; where the collected catchment
//     exceeds WolfTerrainWaterCatchment (m^2) a TERRAIN-CONFORMING ribbon mesh follows the
//     drainage line downhill (LLVOWater::setConformingMesh): it bends with the gully, its
//     surface tracks the channel bed, it tips over ledges as falling water and widens as
//     more catchment joins. One mesh per drainage chain, from source to the sea, a pool or
//     the river it feeds.
// Purely rendered: the sim's heightmap is never touched. The analysis runs on the General
// work queue and the objects are (re)created on the main thread whenever the terrain changes.
// Setting WolfTerrainWater (own switch, independent of WolfTerrainLook; on by default since w9).
class WolfNaturalWater : public LLSingleton<WolfNaturalWater>, public WolfObjectSweep::Client
{
    LLSINGLETON(WolfNaturalWater);
    ~WolfNaturalWater();

public:
    /** Called every frame from LLAppViewer::idle(); rate-limits itself. */
    void idle();
    /** Kill every plane (region change, setting off). */
    void reset();

    // [FAR DRAW PERF 2026-11-14] The built mask (the old rasterizeBuilt's walk of the object list) is this feature's share of
    // the shared object pass (wolfobjectsweep.h): each prim of the checked region is rastered in visit(), and endPass() does
    // what idle() did after rasterizeBuilt (the stamp, the settle wait, the analysis).
    void beginPass() override;
    void beginSlice() override;
    void visit(LLViewerObject* objectp) override;
    void endPass() override;

    struct Surface  // one water object: a conforming ribbon (stream) or a basin mesh (pool)
    {
        std::shared_ptr<LLVOWater::ConformingMesh> mMesh;
        F32 mFlow = 0.f;     // stream surface speed, m/s (the shader scrolls ripples by it); 0 = pool
        F32 mDepth = 0.3f;   // body tint depth for the shader (LLVOWater::setBoundedWaterDepth)
        F32 mRank = 0.f;     // catchment (streams) / cells (pools), for the budget sort
    };
    struct Result
    {
        U64 mRegionHandle = 0;
        U64 mTerrainStamp = 0;
        std::vector<Surface> mPools;
        std::vector<Surface> mStreams;
        S32 mBuiltBasins = 0;   // hollows that got no pool because something is built in them
        S32 mSeaBasins = 0;     // hollows that got no pool because the region's water is already in them
        U64 mShape = 0;         // <WolfViewer 2026-10-07/> hash of every surface (shapeHash), to keep the live water when nothing changed
    };

private:
    // [FAR DRAW PERF 2026-11-14] A check now waits for its region's prims to be read in a shared object pass
    // (wolfobjectsweep.h) before its stamp is taken, so a built-mask change is seen up to one pass later and the checks come
    // every max(CHECK_INTERVAL_SECS / 2, one pass). Well inside what this feature already waits on purpose: a changed region is
    // re-analysed only once its objects have been still for SETTLE_SECS (6 s), or after MAX_PENDING_SECS (30 s).
    static constexpr F32 CHECK_INTERVAL_SECS = 2.f;
    // <WolfViewer 2026-10-07> Paul: water "jerky and horrible" at 4096 m draw distance while
    // Magical Fairies Pass rezzed. With ObjectsCullingByDistance the sims keep sending and killing
    // objects as the agent moves, every object change changed the built mask, and Madrigal's
    // water was re-analysed and its surfaces killed and re-created every 2 s check (50 times in
    // 4 minutes, the same "2 pools, 253 vertices" each time). A region is now re-analysed only
    // once its objects have stopped changing for SETTLE_SECS, or after MAX_PENDING_SECS of
    // constant change, and a result identical to the live water (mShape) keeps the live water.
    static constexpr F64 SETTLE_SECS = 6.0;
    static constexpr F64 MAX_PENDING_SECS = 30.0;
    // [2026-09-10] Grid points an edge the analysis will handle; bigger regions are decimated
    // (see idle()). 1025 points = a 1024 m region at 1 m, ~1 M cells, ~60 MB of working arrays.
    static constexpr S32 MAX_ANALYSIS_GRIDS = 1024;
    static constexpr S32 MAX_POOLS = 64;
    static constexpr S32 MAX_STREAMS = 200;
    static constexpr S32 MAX_STREAM_VERTS = 800000;   // across all streams, ~26 MB of buffer
    // (0.25 m / 4 cells drew every dip on a mountain as a dark spot, 2026-09-05: a pool is
    // now at least a room-sized, knee-deep hollow.)
    // [2026-09-10] Paul: "auto generated lakes should not appear on flat ground ever". A pool
    // needs a real basin: a metre of water at its deepest AND banks that rise at least 25 %
    // (14 degrees) on average where the water meets the land. A 0.5 m dip across a lawn, or a
    // wide saucer that deepens over 100 m, fails both; a bulldozed lake passes both.
    // natural_water_worker.js same numbers.
    static constexpr F32 MIN_POOL_DEPTH_M = 1.0f;
    static constexpr F32 MIN_POOL_RIM_GRADE = 0.25f;
    static constexpr S32 MIN_POOL_CELLS = 12;
    // [2026-09-19, Wolf Nation] No generated lakes when the analysis is coarser than this many
    // metres per point. A 51200 m region is analysed every 50 m (MAX_ANALYSIS_GRIDS), and at
    // that spacing a mountain valley whose outlet is narrower than a cell looks closed: the
    // priority flood raised a whole valley to its rim and drew a lake kilometres across, its
    // edge stepped in 50 m cells, white with reflected sky from above. 4 m is where a basin's
    // outline is still the basin (regions up to 4096 m), the same limit wolfwaterfield.h
    // MAX_FIELD_REGION_M puts on the shore field. Streams are not gated: none were drawn on
    // Wolf Nation, and a stream is a line, not a sheet. natural_water_worker.js same number.
    static constexpr F32 MAX_POOL_ANALYSIS_M = 4.0f;
    // A drainage chain shorter than this (cells) is a stub, not a stream: dropped.
    // [2026-09-10] A fall can be short: four cells of 40-degree face is a fall worth drawing.
    static constexpr S32 MIN_CHAIN_CELLS = 4;
    // [2026-09-08, Tromsø: "the pond the user created and those weird streams appeared"] a chain
    // must FALL — a pool's overflow on a flat lawn was drawn as a stream over ground with no
    // grade. Source: natural_water_worker.js MIN_CHAIN_DROP_M / MIN_CHAIN_GRADE (same numbers).
    static constexpr F32 MIN_CHAIN_DROP_M = 0.3f;
    static constexpr F32 MIN_CHAIN_GRADE = 0.005f;
    // Streams: water stands this far above the channel bed (the lowest ground around the
    // centreline), so the ground hides the ribbon everywhere except inside the channel.
    static constexpr F32 STREAM_FILL_M = 0.12f;
    // Falling water: rise/run of a reach below FALL_START is a stream on its bed, above
    // FALL_FULL it is a sheet ON the face (the ground itself, lifted WATERFALL_LIFT_M so the
    // cliff does not swallow it); between the two the bed and the fill blend. The water
    // shader derives the same blend from the mesh's own normals (waterV.glsl vary_fall), so
    // keep these in step with its smoothstep(0.3, 0.8, rise_run). (0.18/0.45 turned every
    // mountain gully into one white sheet, 2026-09-05 screenshot: 0.3 = 17 degrees is where
    // white water starts, 0.8 = 39 degrees is a fall.)
    // [2026-09-10] Paul: streams / falls "should only appear over 40 degrees in angle". A cell
    // is a stream cell only when its ground drops at least tan(40deg) = 0.839 per metre toward
    // its downhill neighbour (STEEP_MIN_GRADE), and everything drawn is a fall: the fall blend
    // runs 40 -> 45 degrees. Flat brooks are no longer drawn at all. Worker same.
    static constexpr F32 STEEP_MIN_GRADE = 0.8391f;   // tan(40 deg)
    static constexpr F32 FALL_START = 0.8391f;
    static constexpr F32 FALL_FULL = 1.0f;            // tan(45 deg)
    static constexpr F32 WATERFALL_LIFT_M = 0.08f;
    // Ribbon tessellation: vertices across the channel, and stations per terrain cell along
    // it (2 = a station every half cell, so the plan bends and the bed profile are followed
    // at the terrain's own resolution).
    static constexpr S32 RIBBON_ACROSS = 7;
    static constexpr S32 STATIONS_PER_CELL = 2;
    // The outer part of the ribbon sinks below the level surface so its edge is always in
    // the ground: from |u| = EDGE_SAG_FROM (0 = centreline, 1 = edge) it drops EDGE_SAG_M.
    static constexpr F32 EDGE_SAG_FROM = 0.45f;
    static constexpr F32 EDGE_SAG_M = 0.4f;
    // PRIMS WIN. Water never shows on ground a prim occupies (a road, a house, a wolfwater
    // pool): grid points under a prim's box are BUILT, dilated by this many cells so the
    // wide ribbons cannot lap onto a road beside a stream, and every water vertex there is
    // pushed BUILT_SINK_M under the ground. A prim's box counts as on the ground where the
    // ground lies within [box.minZ - BUILT_BELOW_M, box.maxZ + BUILT_ABOVE_M]; a bridge
    // well above the ground does not block the stream under it. BUILT_ABOVE_M covers the
    // most a stream can stand above the ground (STREAM_FILL_M + MAX_STAND_M), so a road
    // slab bridging a dip in bumpy terrain still counts.
    //
    // A HOLLOW WITH A PRIM IN IT IS DRY LAND (2026-09-05, a flooded road): a terraformed
    // plot with a berm round it is a closed depression to the flood fill, and a pool at
    // the berm's spill height drowns every road and garden between the buildings. So a
    // basin gets no pool if anything built stands in it below the spill level — a prim on
    // the floor of a hollow proves the hollow is not a lake — and all of its cells are
    // blocked, so streams stop at its edge too. Prims are tested against the WATER level
    // for this (the lowest prim bottom over each cell, from rasterizeObject), not only
    // against the ground: a jetty or a slab a metre up is still in the water.
    static constexpr S32 BUILT_DILATE_CELLS = 2;
    static constexpr F32 BUILT_SINK_M = 0.5f;
    static constexpr F32 BUILT_BELOW_M = 1.0f;
    static constexpr F32 BUILT_ABOVE_M = 0.5f;
    // <WolfViewer 2026-10-06> OBJECTS SITTING ON THE GROUND. Paul: "make sure natural pools and
    // water dont draw over or under objects that sit on the ground". An object whose underside is
    // within OBJECT_CLEAR_M of the ground (the built mask: streams stop, a hollow it stands in gets
    // no pool) or of a pool's water (a deck, a floor on posts, a jetty) sits on it: no natural
    // water under it or over it. Was BUILT_BELOW_M (1 m) and BUILT_ABOVE_M (0.5 m): a floor 1-3 m
    // up had a stream or a pool drawn under it, showing through gaps and at its edges. A bridge
    // or deck higher than this still has the stream run under it. natural_water_worker.js same.
    static constexpr F32 OBJECT_CLEAR_M = 3.0f;
    // Off the channel, water never stands more than this above the ground at any vertex —
    // a ribbon edge over a drop beside the stream is pulled down rather than left hanging.
    static constexpr F32 MAX_STAND_M = 0.35f;

    // [W112 WATERFALLS 2026-11-24] Ticket 1039 (Indigo Blue Realm 10, very hilly generated land, Intel UHD 630): every
    // fall was a wide white sheet. Paul 2026-11-24: soften them - the water's own width, translucent, white only where it
    // breaks.
    //
    // THE WATER'S WIDTH FROM ITS FLOW. Finnegan, Roe, Montgomery & Hallet (2005), "Controls on the channel width of rivers:
    // Implications for modeling fluvial incision of bedrock", Geology 33(3):229-232, eq. 5 (from Manning's equation and
    // Q = U A, constant width-to-depth ratio and roughness): W = [a (a + 2)^(2/3)]^(3/8) Q^(3/8) S^(-3/16) n^(3/8). With
    // the same rain on every square metre the discharge Q is proportional to the catchment, so relative to the smallest
    // drawn stream (catchment = WolfTerrainWaterCatchment, on a 45-degree face):
    //     W = STREAM_W0_M x (catchment / WolfTerrainWaterCatchment)^(3/8) x (rise/run)^(-3/16)
    // A steep reach is narrower than a gentle one carrying the same water (faster, so a smaller cross-section: same paper).
    // The old width (4 + 3 log2(...) metres, at least 3.5 m for a fall) made every tiny summit basin on hilly land a
    // 3.5-10 m sheet; a hollow spilling a quarter of the threshold catchment (the `fed` rule) is now a 0.9 m trickle.
    // STREAM_W0_M is a CONVENTION, not a measurement: the threshold catchment is a setting (5000 m^2 by default, far below
    // where a real channel would be a metre wide), so 1.5 m is chosen: the narrowest water that still reads as a line at
    // 100-250 m (1 m went to a pixel or two there, Shadow 2026-11-24). STREAM_W_MIN_M keeps a ribbon above a pixel;
    // STREAM_W_MAX_M is the old 24 m cap.
    static constexpr F32 STREAM_W0_M = 1.5f;
    static constexpr F32 STREAM_W_MIN_M = 0.5f;
    static constexpr F32 STREAM_W_MAX_M = 24.f;
    static constexpr F32 WIDTH_Q_EXP = 0.375f;       // 3/8, Finnegan et al. 2005 eq. 5
    static constexpr F32 WIDTH_S_EXP = -0.1875f;     // -3/16, Finnegan et al. 2005 eq. 5
    // The ribbon is drawn wider than the water, because its outer part sags into the ground (EDGE_SAG_FROM / EDGE_SAG_M):
    // on a plane face a vertex at |u| shows only while the sag is less than the lift above the ground, so the water shows to
    // |u| = EDGE_SAG_FROM + (1 - EDGE_SAG_FROM) sqrt(lift / EDGE_SAG_M) (0.70 of the half-width for a fall, 0.75 for a stream
    // on its bed). The drawn half-width is the water's half-width over that.
    //
    // WHERE FALLING WATER IS WHITE. Water goes white where air is mixed into it (the brink, the impact, the spray); a thin
    // film running down a face is clear, with white threads that thicken the further it has fallen. Koepke (1984),
    // "Effective reflectance of oceanic whitecaps", Applied Optics 23(11):1816-1824: foam taken over its whole area reflects
    // only ~22 % in the visible, half or less of what had been assumed, because most of it is thin and patchy - so
    // white-water is drawn as threads and patches with clear water between them, never as one white sheet. The lengths
    // below are CONVENTIONS for the look (no field data for terrain-generated falls): the brink band, how far down the threads
    // develop, the plunge spray as a share of the fall's height, and the fade-in of a stream that starts from no pool.
    static constexpr F32 LIP_BAND_M = 0.6f;          // white at a pool's brink, gone this far (vertical) below it
    static constexpr F32 AERATION_M = 2.5f;          // the threads are 63 % developed this far (vertical) below the start
    static constexpr F32 PLUNGE_SHARE = 0.15f;       // the plunge foam / spray rises this share of the fall's height...
    static constexpr F32 PLUNGE_MIN_M = 0.5f;        // ...at least this...
    static constexpr F32 PLUNGE_MAX_M = 4.f;         // ...at most this
    static constexpr F32 HEAD_FADE_M = 3.f;          // a stream with no pool above it gathers over this length
    static constexpr F32 TAIL_FADE_M = 2.f;          // a stream that ends in the ground (no water to join) thins out over this
    // A trickle has sparse threads and little foam, a big fall dense ones: the share of each scales with the water's width
    // from STREAM_W_MIN_M (sparse) to DENSE_W_M (full). CONVENTION.
    static constexpr F32 DENSE_W_M = 8.f;

    /** Runs on the General queue: heights + built mask -> surfaces. */
    /** Hash of everything apply() would build from a result (any thread). */
    static U64 shapeHash(const Result& result);
    static void compute(Result& out, std::vector<F32> z, std::vector<U8> built, std::vector<F32> prim_floor, S32 grids, F32 mpg, F32 sea_level, F32 catchment_m2);
    /** Main thread: one prim into the built mask - one byte per grid point, 1 where a prim stands on (or in) the ground, and
     *  per grid point the lowest bottom of any prim box over it that is not buried (+inf if none). origin: the region's
     *  origin in agent space THIS frame (the drawable's extents are read in the same frame). [FAR DRAW PERF 2026-11-14/] was
     *  rasterizeBuilt, which walked the whole object list in one frame. */
    static void rasterizeObject(LLViewerObject* obj, const LLVector3& origin, const std::vector<F32>& z, S32 grids, F32 mpg,
                                std::vector<U8>& built, std::vector<F32>& prim_floor);
    /** Main thread: replace the live surfaces with the computed set. */
    void apply(std::shared_ptr<Result> result);
    static U64 terrainStamp(LLViewerRegion* regionp);

    /** Main thread: kill one region's surfaces. */
    static void killSurfaces(std::vector<LLPointer<LLVOWater>>& surfaces);

    // <WolfViewer 2026-09-23> Paul: "the region next to the one i'm in needs to show its water
    // ... even if i'm not in that region". Every connected region keeps its own surfaces and
    // stamp; one analysis runs at a time (mBusy). The agent's region is looked at on every
    // other check, so it reacts as fast as it did when it was the only one; the neighbours
    // share the checks in between, nearest first (wolfnearbyregions.h).
    struct RegionWater
    {
        U64 mAppliedStamp{ 0 };
        U64 mAppliedShape{ 0 };      // <WolfViewer 2026-10-07/> Result::mShape of the live surfaces
        bool mApplied{ false };      // <WolfViewer 2026-10-07/> a result has been applied this visit
        U64 mSeenStamp{ 0 };         // <WolfViewer 2026-10-07/> stamp at the last check, and since when
        F64 mSeenSince{ 0.0 };
        F64 mPendingSince{ -1.0 };   // <WolfViewer 2026-10-07/> first check that differed from the applied stamp
        std::vector<LLPointer<LLVOWater>> mSurfaces;
    };
    std::map<U64, RegionWater> mRegions;
    bool mBusy{ false };
    F64  mNextCheck{ 0.0 };
    bool mAgentTurn{ true };
    size_t mNeighbourCursor{ 0 };
    std::set<U64> mLoggedDecimation;   // <WolfViewer 2026-09-10> regions told once about the coarse grid
    // [FAR DRAW PERF 2026-11-14] the check whose prims the shared pass is reading: what idle() had in locals around
    // rasterizeBuilt. mPassPending from the request to endPass (or reset). The region pointer is compared with the objects'
    // own, never read without first finding it again by its handle (beginSlice); spoiled = it went during the pass.
    bool mPassPending{ false };
    bool mPassSpoiled{ false };
    LLViewerRegion* mPassRegionp{ nullptr };
    U64 mPassHandle{ 0 };
    std::vector<F32> mPassZ;
    S32 mPassGrids{ 0 };
    F32 mPassMpg{ 1.f };
    U64 mPassTerrainStamp{ 0 };
    F32 mPassSea{ 0.f };
    std::vector<U8> mPassBuilt;
    std::vector<F32> mPassFloor;
    LLVector3 mSliceOrigin;
};

#endif // WOLF_NATURALWATER_H
