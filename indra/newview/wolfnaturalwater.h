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
#include <memory>
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
// Setting WolfTerrainWater (own switch, independent of WolfTerrainLook).
class WolfNaturalWater : public LLSingleton<WolfNaturalWater>
{
    LLSINGLETON(WolfNaturalWater);
    ~WolfNaturalWater();

public:
    /** Called every frame from LLAppViewer::idle(); rate-limits itself. */
    void idle();
    /** Kill every plane (region change, setting off). */
    void reset();

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
    };

private:
    static constexpr F32 CHECK_INTERVAL_SECS = 2.f;
    static constexpr S32 MAX_POOLS = 64;
    static constexpr S32 MAX_STREAMS = 200;
    static constexpr S32 MAX_STREAM_VERTS = 800000;   // across all streams, ~26 MB of buffer
    // (0.25 m / 4 cells drew every dip on a mountain as a dark spot, 2026-09-05: a pool is
    // now at least a room-sized, knee-deep hollow.)
    static constexpr F32 MIN_POOL_DEPTH_M = 0.4f;
    static constexpr S32 MIN_POOL_CELLS = 12;
    // A drainage chain shorter than this (cells) is a stub, not a stream: dropped.
    static constexpr S32 MIN_CHAIN_CELLS = 8;
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
    static constexpr F32 FALL_START = 0.3f;
    static constexpr F32 FALL_FULL = 0.8f;
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
    // for this (the lowest prim bottom over each cell, from rasterizeBuilt), not only
    // against the ground: a jetty or a slab a metre up is still in the water.
    static constexpr S32 BUILT_DILATE_CELLS = 2;
    static constexpr F32 BUILT_SINK_M = 0.5f;
    static constexpr F32 BUILT_BELOW_M = 1.0f;
    static constexpr F32 BUILT_ABOVE_M = 0.5f;
    // Off the channel, water never stands more than this above the ground at any vertex —
    // a ribbon edge over a drop beside the stream is pulled down rather than left hanging.
    static constexpr F32 MAX_STAND_M = 0.35f;

    /** Runs on the General queue: heights + built mask -> surfaces. */
    static void compute(Result& out, std::vector<F32> z, std::vector<U8> built, std::vector<F32> prim_floor, S32 grids, F32 mpg, F32 sea_level, F32 catchment_m2);
    /** Main thread: one byte per grid point, 1 where a prim stands on (or in) the ground, and
     *  per grid point the lowest bottom of any prim box over it that is not buried (+inf if none). */
    static void rasterizeBuilt(LLViewerRegion* regionp, const std::vector<F32>& z, S32 grids, F32 mpg, std::vector<U8>& built, std::vector<F32>& prim_floor);
    /** Main thread: replace the live surfaces with the computed set. */
    void apply(std::shared_ptr<Result> result);
    static U64 terrainStamp(LLViewerRegion* regionp);

    U64  mRegionHandle{ 0 };
    U64  mAppliedStamp{ 0 };
    bool mBusy{ false };
    F64  mNextCheck{ 0.0 };
    std::vector<LLPointer<LLVOWater>> mSurfaces;
};

#endif // WOLF_NATURALWATER_H
