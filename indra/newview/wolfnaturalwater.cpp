/**
 * @file wolfnaturalwater.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfnaturalwater.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

#include "llagent.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "pipeline.h"
#include "workqueue.h"

WolfNaturalWater::WolfNaturalWater()
{
}

WolfNaturalWater::~WolfNaturalWater()
{
    // LLPointers into the object list, which outlives this singleton's teardown order
    // guarantees — drop the references without touching the pipeline.
    mSurfaces.clear();
}

void WolfNaturalWater::reset()
{
    for (auto& p : mSurfaces)
    {
        if (p.notNull() && !p->isDead())
        {
            gObjectList.killObject(p);
        }
    }
    mSurfaces.clear();
    mAppliedStamp = 0;
}

// Changes whenever any patch of the region's surface was updated (terrain edits, new
// LayerData): LLSurfacePatch::dirtyZ() stamps mLastUpdateTime = gFrameTime (llsurfacepatch.cpp).
U64 WolfNaturalWater::terrainStamp(LLViewerRegion* regionp)
{
    const LLSurface& land = regionp->getLand();
    const S32 per_edge = land.getPatchesPerEdge();
    U64 stamp = 1469598103934665603ull;
    for (S32 y = 0; y < per_edge; ++y)
    {
        for (S32 x = 0; x < per_edge; ++x)
        {
            const LLSurfacePatch* patchp = land.getPatch(x, y);
            if (patchp)
            {
                stamp = (stamp ^ patchp->getLastUpdateTime()) * 1099511628211ull;
            }
        }
    }
    return stamp;
}

void WolfNaturalWater::idle()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfTerrainWater", true);
    static LLCachedControl<F32> catchment(gSavedSettings, "WolfTerrainWaterCatchment", 5000.f);
    if (!enabled)
    {
        if (!mSurfaces.empty())
        {
            reset();
        }
        return;
    }

    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp)
    {
        return;
    }
    if (regionp->getHandle() != mRegionHandle)
    {
        reset();
        mRegionHandle = regionp->getHandle();
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextCheck || mBusy)
    {
        return;
    }
    mNextCheck = now + CHECK_INTERVAL_SECS;

    // Snapshot the heights; the analysis must not touch the live surface off the main thread.
    const LLSurface& land = regionp->getLand();
    const S32 grids = land.getGridsPerEdge();
    if (grids < 4)
    {
        return;
    }
    std::vector<F32> z((size_t)grids * grids);
    for (S32 k = 0; k < grids * grids; ++k)
    {
        z[k] = land.getZ(k);
    }
    const F32 mpg = land.getMetersPerGrid();

    // Prims arrive long after the terrain does, so the built mask is part of the stamp:
    // a road rezzing across a stream takes the stream away on the next check.
    std::vector<U8> built;
    std::vector<F32> prim_floor;
    rasterizeBuilt(regionp, z, grids, mpg, built, prim_floor);
    U64 stamp = terrainStamp(regionp);
    for (U8 b : built)
    {
        stamp = (stamp ^ b) * 1099511628211ull;
    }
    for (F32 f : prim_floor)   // a prim rezzing above a basin floor changes the answer too
    {
        U32 bits;
        memcpy(&bits, &f, sizeof(bits));
        stamp = (stamp ^ bits) * 1099511628211ull;
    }
    if (stamp == mAppliedStamp)
    {
        return;
    }
    const F32 sea = regionp->getWaterHeight();
    const F32 catchment_m2 = llmax(25.f, (F32)catchment);

    auto result = std::make_shared<Result>();
    result->mRegionHandle = regionp->getHandle();
    result->mTerrainStamp = stamp;
    mBusy = true;

    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        compute(*result, std::move(z), std::move(built), std::move(prim_floor), grids, mpg, sea, catchment_m2);
        apply(result);
        return;
    }
    main_queue->postTo(
        general_queue,
        [result, z = std::move(z), built = std::move(built), prim_floor = std::move(prim_floor), grids, mpg, sea, catchment_m2]() mutable // General queue
        {
            compute(*result, std::move(z), std::move(built), std::move(prim_floor), grids, mpg, sea, catchment_m2);
            return true;
        },
        [result](bool) // main thread
        {
            WolfNaturalWater::instance().apply(result);
        });
}

namespace
{
    const S32 DX8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    const S32 DY8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
}

// static
void WolfNaturalWater::rasterizeBuilt(LLViewerRegion* regionp, const std::vector<F32>& z, S32 n, F32 mpg, std::vector<U8>& built, std::vector<F32>& prim_floor)
{
    built.assign((size_t)n * n, 0);
    prim_floor.assign((size_t)n * n, std::numeric_limits<F32>::infinity());
    const LLVector3 origin = regionp->getOriginAgent();
    const F32 oo = 1.f / mpg;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->getRegion() != regionp || obj->getPCode() != LL_PCODE_VOLUME
            || obj->isAttachment() || obj->isHUDAttachment() || obj->mDrawable.isNull())
        {
            continue;
        }
        // Agent-space box of what is actually drawn, from the drawable's octree entry.
        const LLVector4a* ext = obj->mDrawable->getSpatialExtents();
        if (!ext || !ext[0].isFinite3() || !ext[1].isFinite3())
        {
            continue;
        }
        const F32 minx = ext[0][0] - origin.mV[VX], miny = ext[0][1] - origin.mV[VY];
        const F32 maxx = ext[1][0] - origin.mV[VX], maxy = ext[1][1] - origin.mV[VY];
        const F32 minz = ext[0][2] - origin.mV[VZ], maxz = ext[1][2] - origin.mV[VZ];
        const S32 x0 = llclamp((S32)floorf(minx * oo), 0, n - 1), x1 = llclamp((S32)ceilf(maxx * oo), 0, n - 1);
        const S32 y0 = llclamp((S32)floorf(miny * oo), 0, n - 1), y1 = llclamp((S32)ceilf(maxy * oo), 0, n - 1);
        if (maxx < 0.f || maxy < 0.f || minx > n * mpg || miny > n * mpg)
        {
            continue;
        }
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                const F32 ground = z[x + y * n];
                if (ground >= minz - BUILT_BELOW_M && ground <= maxz + BUILT_ABOVE_M)
                {
                    built[x + y * n] = 1;
                }
                // Anything not buried is a candidate for standing in a pool's water: the
                // flood stage compares its bottom with the pool's level.
                if (maxz >= ground - BUILT_BELOW_M)
                {
                    prim_floor[x + y * n] = llmin(prim_floor[x + y * n], minz);
                }
            }
        }
    }
}

// static
void WolfNaturalWater::compute(Result& out, std::vector<F32> z, std::vector<U8> built, std::vector<F32> prim_floor, S32 n, F32 mpg, F32 sea, F32 catchment_m2)
{
    // The prim footprints, plus (after the flood) every hollow something is built in; the
    // dilation that keeps a ribbon's flat cross-section off a road beside the stream is
    // applied once both are known. Nothing before that point reads `blocked`.
    std::vector<U8> blocked(built);
    auto blocked_at = [&](F32 x, F32 y)
    {
        const S32 gx = llclamp((S32)(x / mpg + 0.5f), 0, n - 1), gy = llclamp((S32)(y / mpg + 0.5f), 0, n - 1);
        return blocked[gx + gy * n] != 0;
    };
    // Terrain height at (x, y) AS DRAWN at render stride `stride`: grid points every
    // `stride` cells (patches are 16 cells, so strides up to 16 line up with them), the same
    // two-triangle split as LLSurface::resolveHeightRegion / LLVOSurfacePatch. Stride 1 is
    // the fine surface.
    auto coarse_z = [&](F32 x, F32 y, S32 stride)
    {
        const F32 cs = stride * mpg;
        const S32 left = llclamp((S32)floorf(x / cs) * stride, 0, n - 1);
        const S32 bottom = llclamp((S32)floorf(y / cs) * stride, 0, n - 1);
        const S32 right = llmin(left + stride, n - 1), top = llmin(bottom + stride, n - 1);
        const F32 lb = z[left + bottom * n], rb = z[right + bottom * n];
        const F32 lt = z[left + top * n], rt = z[right + top * n];
        F32 dx = x - left * mpg, dy = y - bottom * mpg;
        if (dy > dx) { dy *= lt - lb; dx *= rt - lt; }
        else         { dx *= rb - lb; dy *= rt - rb; }
        return lb + (dx + dy) / cs;
    };
    // Per-vertex terrain-LOD lift (LLVOWater::ConformingMesh::mLodLift): how far the ground
    // drawn at stride 4 / 16 rises above the vertex. A vertex deliberately put UNDER the
    // fine ground (a sagged bank edge, a prim cell) stays there — hidden is hidden.
    //
    // The raw need is NOT what is baked (2026-09-05 screenshot: far water "flowing up
    // hill"): a vertex lifted to exactly the coarse ground hugs a surface that undulates
    // with the coarse grid's period along a gully — exact at the coarse grid points, high
    // between them — so the water ran up and down every 16 m. What is baked is a LEVEL that
    // never rises downstream: for a stream (columns = vertices across, stations in flow
    // order) a backward pass per column takes max(own need, the level just downstream),
    // for a pool (columns = 0) one flat lift, the largest any of its vertices needs. The
    // water then bridges a coarse dip rather than dropping into it and climbing out. The
    // stride-16 level is also never below the stride-4 one, because the shader mixes the
    // two with distance and a mix towards a LOWER value along a chain would be a rise.
    auto bake_lod = [&](LLVOWater::ConformingMesh& mesh, S32 columns)
    {
        const size_t nv = mesh.mVerts.size();
        mesh.mLodLift.resize(nv);
        std::vector<U8> hidden(nv, 0);
        for (size_t i = 0; i < nv; ++i)
        {
            const LLVector3& v = mesh.mVerts[i];
            LLVector2& lift = mesh.mLodLift[i];
            if (v.mV[VZ] < coarse_z(v.mV[VX], v.mV[VY], 1) - 0.1f)
            {
                lift.setVec(0.f, 0.f);
                hidden[i] = 1;
                continue;
            }
            lift.setVec(llmax(0.f, coarse_z(v.mV[VX], v.mV[VY], 4) + 0.03f - v.mV[VZ]),
                        llmax(0.f, coarse_z(v.mV[VX], v.mV[VY], 16) + 0.03f - v.mV[VZ]));
        }
        if (columns <= 0)
        {
            LLVector2 flat(0.f, 0.f);
            for (size_t i = 0; i < nv; ++i)
            {
                flat.mV[VX] = llmax(flat.mV[VX], mesh.mLodLift[i].mV[VX]);
                flat.mV[VY] = llmax(flat.mV[VY], mesh.mLodLift[i].mV[VY]);
            }
            flat.mV[VY] = llmax(flat.mV[VY], flat.mV[VX]);
            for (size_t i = 0; i < nv; ++i)
            {
                if (!hidden[i]) mesh.mLodLift[i] = flat;
            }
            return;
        }
        const S32 rows = (S32)(nv / columns);
        for (S32 j = 0; j < columns; ++j)
        {
            F32 level4 = -1e30f, level16 = -1e30f;   // the level just downstream, per stride
            for (S32 s = rows - 1; s >= 0; --s)
            {
                const size_t i = (size_t)s * columns + j;
                if (hidden[i]) continue;   // stays under the ground; the envelope passes over it
                const F32 zv = mesh.mVerts[i].mV[VZ];
                LLVector2& lift = mesh.mLodLift[i];
                const F32 l4 = llmax(zv + lift.mV[VX], level4);
                const F32 l16 = llmax(llmax(zv + lift.mV[VY], level16), l4);
                lift.setVec(l4 - zv, l16 - zv);
                level4 = l4;
                level16 = l16;
            }
        }
    };

    const S32 total = n * n;
    const F32 cell_m2 = mpg * mpg;

    // 1. Priority flood with epsilon (Barnes et al. 2014, "Priority-Flood+ε"): every
    //    depression is raised to its spill height AND every filled or naturally flat cell
    //    is nudged up by the smallest representable amount away from its outlet, so the
    //    steepest-descent step below always finds a way down. Without the epsilon a lake
    //    (or a plateau) is dead flat, no cell in it has a lower neighbour, and everything
    //    draining into it is LOST — its outflow starts with one cell's worth of catchment.
    //    Doubles so a thousand nudges across a big lake still sum to nothing visible.
    std::vector<F64> filled(total);
    for (S32 k = 0; k < total; ++k) filled[k] = z[k];
    std::vector<U8> done(total, 0);
    struct HeapCell
    {
        F64 mZ;
        S32 mIdx;
        bool operator>(const HeapCell& o) const { return mZ > o.mZ; }
    };
    std::priority_queue<HeapCell, std::vector<HeapCell>, std::greater<HeapCell>> heap;
    for (S32 x = 0; x < n; ++x)
    {
        for (S32 y : { 0, n - 1 })
        {
            const S32 k = x + y * n;
            if (!done[k]) { done[k] = 1; heap.push({ filled[k], k }); }
        }
    }
    for (S32 y = 1; y < n - 1; ++y)
    {
        for (S32 x : { 0, n - 1 })
        {
            const S32 k = x + y * n;
            if (!done[k]) { done[k] = 1; heap.push({ filled[k], k }); }
        }
    }
    while (!heap.empty())
    {
        const HeapCell c = heap.top();
        heap.pop();
        const S32 cx = c.mIdx % n, cy = c.mIdx / n;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX8[d], ny = cy + DY8[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
            const S32 k = nx + ny * n;
            if (done[k]) continue;
            done[k] = 1;
            if (filled[k] <= c.mZ) filled[k] = std::nextafter(c.mZ, 1e30);
            heap.push({ filled[k], k });
        }
    }

    // 2. Pools: connected runs of filled cells at one spill level, above sea level. Each
    //    accepted pool becomes a level mesh over ITS cells: one quad per grid cell that has
    //    at least one submerged corner, so the water reaches the bank and stops there.
    std::vector<U8> lake(total, 0);
    for (S32 k = 0; k < total; ++k)
    {
        lake[k] = (filled[k] - z[k] > 0.05 && filled[k] > sea + 0.05) ? 1 : 0;
    }
    struct Pool { F32 mLevel; F32 mDepth; S32 mMinX, mMinY, mMaxX, mMaxY; std::vector<S32> mCells; };
    std::vector<Pool> pools;
    {
        std::vector<U8> seen(total, 0);
        std::vector<S32> stack;
        for (S32 k0 = 0; k0 < total; ++k0)
        {
            if (!lake[k0] || seen[k0]) continue;
            Pool p{ (F32)filled[k0], 0.f, n, n, -1, -1, {} };
            stack.clear();
            stack.push_back(k0);
            seen[k0] = 1;
            while (!stack.empty())
            {
                const S32 k = stack.back();
                stack.pop_back();
                const S32 cx = k % n, cy = k / n;
                p.mCells.push_back(k);
                p.mDepth = llmax(p.mDepth, (F32)(filled[k] - z[k]));
                p.mMinX = llmin(p.mMinX, cx); p.mMaxX = llmax(p.mMaxX, cx);
                p.mMinY = llmin(p.mMinY, cy); p.mMaxY = llmax(p.mMaxY, cy);
                for (S32 d = 0; d < 8; ++d)
                {
                    const S32 nx = cx + DX8[d], ny = cy + DY8[d];
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                    const S32 j = nx + ny * n;
                    if (seen[j] || !lake[j] || fabs(filled[j] - p.mLevel) > 0.01) continue;
                    seen[j] = 1;
                    stack.push_back(j);
                }
            }
            // A hollow with a prim in it below the water line is dry land someone built on
            // (see BUILT_ABOVE_M in the header): no pool, and the whole hollow is blocked so
            // no stream runs across it either. Tested before the size gate on purpose — a
            // tiny dip under a prim is under the prim.
            bool built_on = false;
            for (S32 k : p.mCells)
            {
                if (built[k] || prim_floor[k] <= p.mLevel + BUILT_ABOVE_M)
                {
                    built_on = true;
                    break;
                }
            }
            if (built_on)
            {
                for (S32 k : p.mCells)
                {
                    blocked[k] = 1;
                    lake[k] = 0;
                }
                ++out.mBuiltBasins;
                continue;
            }
            if ((S32)p.mCells.size() >= MIN_POOL_CELLS && p.mDepth >= MIN_POOL_DEPTH_M)
            {
                pools.push_back(std::move(p));
            }
        }
    }
    // Dilate the blocked mask (prim footprints + built hollows); see BUILT_DILATE_CELLS.
    for (S32 pass = 0; pass < BUILT_DILATE_CELLS; ++pass)
    {
        std::vector<U8> next(blocked);
        for (S32 k = 0; k < n * n; ++k)
        {
            if (!blocked[k]) continue;
            const S32 cx = k % n, cy = k / n;
            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = cx + DX8[d], ny = cy + DY8[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                next[nx + ny * n] = 1;
            }
        }
        blocked.swap(next);
    }
    std::sort(pools.begin(), pools.end(), [](const Pool& a, const Pool& b) { return a.mCells.size() > b.mCells.size(); });
    if ((S32)pools.size() > MAX_POOLS) pools.resize(MAX_POOLS);
    // Cells of ACCEPTED pools: a stream that leaves one is real water even where its own
    // catchment is small — a full pool spills.
    std::vector<U8> pooled(total, 0);
    for (const Pool& p : pools)
    {
        for (S32 k : p.mCells) pooled[k] = 1;

        // Grid points from one cell outside the bbox to one cell outside, so the quads
        // around every edge lake point exist. Big lakes step coarser to stay under the
        // U16 index ceiling; a coarse quad is drawn if any lake cell lies inside it.
        const S32 x0 = llmax(p.mMinX - 1, 0), y0 = llmax(p.mMinY - 1, 0);
        const S32 x1 = llmin(p.mMaxX + 1, n - 1), y1 = llmin(p.mMaxY + 1, n - 1);
        const S32 span = llmax(x1 - x0, y1 - y0);
        const S32 step = llmax(1, (span + 253) / 254);
        const S32 cols = (x1 - x0) / step + 1, rows = (y1 - y0) / step + 1;
        auto mesh = std::make_shared<LLVOWater::ConformingMesh>();
        mesh->mVerts.reserve(cols * rows);
        mesh->mNormals.reserve(cols * rows);
        mesh->mUVs.reserve(cols * rows);
        for (S32 r = 0; r < rows; ++r)
        {
            for (S32 c = 0; c < cols; ++c)
            {
                const F32 x = (x0 + c * step) * mpg, y = (y0 + r * step) * mpg;
                mesh->mVerts.push_back(LLVector3(x, y, p.mLevel));
                mesh->mNormals.push_back(LLVector3(0.f, 0.f, 1.f));
                mesh->mUVs.push_back(LLVector2(x, y));
            }
        }
        // membership mask over the bbox grid
        const S32 bw = p.mMaxX - p.mMinX + 1;
        std::vector<U8> mask((size_t)bw * (p.mMaxY - p.mMinY + 1), 0);
        for (S32 k : p.mCells) mask[(k % n - p.mMinX) + (k / n - p.mMinY) * bw] = 1;
        auto submerged_in = [&](S32 gx0, S32 gy0, S32 gx1, S32 gy1)   // any lake point in [gx0,gx1]x[gy0,gy1]
        {
            for (S32 gy = llmax(gy0, p.mMinY); gy <= llmin(gy1, p.mMaxY); ++gy)
                for (S32 gx = llmax(gx0, p.mMinX); gx <= llmin(gx1, p.mMaxX); ++gx)
                    if (mask[(gx - p.mMinX) + (gy - p.mMinY) * bw]) return true;
            return false;
        };
        mesh->mMin.setVec(x0 * mpg, y0 * mpg, p.mLevel);
        mesh->mMax.setVec((x0 + (cols - 1) * step) * mpg, (y0 + (rows - 1) * step) * mpg, p.mLevel);
        for (S32 r = 0; r < rows - 1; ++r)
        {
            for (S32 c = 0; c < cols - 1; ++c)
            {
                const S32 gx = x0 + c * step, gy = y0 + r * step;
                if (!submerged_in(gx, gy, gx + step, gy + step)) continue;
                // prims win: no water where anything is built
                bool any_built = false;
                for (S32 gy2 = gy; gy2 <= llmin(gy + step, n - 1) && !any_built; ++gy2)
                    for (S32 gx2 = gx; gx2 <= llmin(gx + step, n - 1); ++gx2)
                        if (blocked[gx2 + gy2 * n]) { any_built = true; break; }
                if (any_built) continue;
                const U16 i0 = (U16)(r * cols + c), i1 = (U16)(i0 + 1);
                const U16 i2 = (U16)(i0 + cols), i3 = (U16)(i2 + 1);
                // (i1-i0) x (i2-i0) = +X x +Y = up, as LLVOWater's own lattice winds
                mesh->mIndices.push_back(i0); mesh->mIndices.push_back(i1); mesh->mIndices.push_back(i2);
                mesh->mIndices.push_back(i1); mesh->mIndices.push_back(i3); mesh->mIndices.push_back(i2);
            }
        }
        if (mesh->mIndices.empty()) continue;
        bake_lod(*mesh, 0);
        Surface sf;
        sf.mMesh = mesh;
        sf.mFlow = 0.f;
        sf.mDepth = p.mDepth;
        sf.mRank = (F32)p.mCells.size();
        out.mPools.push_back(std::move(sf));
    }

    // 3. Flow on the filled surface: each cell drains to its steepest lower neighbour (the
    //    epsilon guarantees one everywhere but the region edge); catchment accumulates from
    //    high to low, and so does "fed by a pool".
    std::vector<S32> downstream(total, -1);
    for (S32 k = 0; k < total; ++k)
    {
        const S32 cx = k % n, cy = k / n;
        F64 best = 0.0;
        S32 best_k = -1;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX8[d], ny = cy + DY8[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
            const S32 j = nx + ny * n;
            const F64 dist = (DX8[d] && DY8[d]) ? 1.41421356 : 1.0;
            const F64 drop = (filled[k] - filled[j]) / dist;
            if (drop > best) { best = drop; best_k = j; }
        }
        downstream[k] = best_k;
    }
    std::vector<S32> order(total);
    for (S32 k = 0; k < total; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](S32 a, S32 b) { return filled[a] > filled[b]; });
    std::vector<F32> acc(total, cell_m2);
    std::vector<U8> fed(pooled);
    for (S32 k : order)
    {
        const S32 dn = downstream[k];
        if (dn >= 0)
        {
            acc[dn] += acc[k];
            fed[dn] |= fed[k];
        }
    }

    // 4. Streams. Each drainage chain (source -> sea / pool / the river it joins) becomes ONE
    //    terrain-conforming ribbon mesh. The surface is the FILLED ground under each station
    //    (the flood-filled terrain sampled at the station's own position, with LLSurface's
    //    triangle split) plus STREAM_FILL_M; on a steep reach the fill blends to a small
    //    lift, i.e. a sheet on the face. FILLED, not raw (2026-09-05 screenshot, "streams
    //    not joined"): every dip along a gully deeper than 5 cm was a "lake" that cut the
    //    chain, and the few that were drawn as pools are the exception, so a stream was a
    //    row of separate plates; and a ribbon that did cross a dip on the raw ground dived
    //    into it and, under the no-uphill clamp, stayed buried on the way out. On the filled
    //    surface a dip is a flat puddle the stream runs straight across, which is what water
    //    does, and only an ACCEPTED pool ends a chain. Every cross-section is level (real water is), the ribbon is wide, and its outer
    //    part sags into the ground, so the visible bank is wherever the terrain rises through
    //    the surface. NOT a 3x3 minimum: on a slope that minimum is simply the downhill cell,
    //    which put every station below its own ground and hid the whole stream.
    //
    //    Terrain grid point k = x + y*n sits at (x*mpg, y*mpg) (LLSurface::resolveHeightRegion
    //    floors x/mpg to find it), so stations go on the grid points, not on cell centres.
    auto is_stream = [&](S32 k)
    {
        return !pooled[k] && !blocked[k] && z[k] > sea + 0.05f
            && (acc[k] >= catchment_m2 || (fed[k] && acc[k] >= catchment_m2 * 0.25f));
    };
    auto smoothstep = [](F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };
    // Terrain height at any region position — the same two-triangle split as
    // LLSurface::resolveHeightRegion, so the water sits on the surface that is drawn.
    auto sample_z = [&](F32 x, F32 y)
    {
        const F32 oo = 1.f / mpg;
        const S32 left = llclamp((S32)floorf(x * oo), 0, n - 1);
        const S32 bottom = llclamp((S32)floorf(y * oo), 0, n - 1);
        const S32 right = llmin(left + 1, n - 1), top = llmin(bottom + 1, n - 1);
        const F32 lb = z[left + bottom * n], rb = z[right + bottom * n];
        const F32 lt = z[left + top * n], rt = z[right + top * n];
        F32 dx = x - left * mpg, dy = y - bottom * mpg;
        if (dy > dx) { dy *= lt - lb; dx *= rt - lt; }
        else         { dx *= rb - lb; dy *= rt - rb; }
        return lb + (dx + dy) * oo;
    };
    // The same over the flood-filled surface: what a stream's level follows.
    auto sample_w = [&](F32 x, F32 y)
    {
        const F32 oo = 1.f / mpg;
        const S32 left = llclamp((S32)floorf(x * oo), 0, n - 1);
        const S32 bottom = llclamp((S32)floorf(y * oo), 0, n - 1);
        const S32 right = llmin(left + 1, n - 1), top = llmin(bottom + 1, n - 1);
        const F32 lb = (F32)filled[left + bottom * n], rb = (F32)filled[right + bottom * n];
        const F32 lt = (F32)filled[left + top * n], rt = (F32)filled[right + top * n];
        F32 dx = x - left * mpg, dy = y - bottom * mpg;
        if (dy > dx) { dy *= lt - lb; dx *= rt - lt; }
        else         { dx *= rb - lb; dy *= rt - rb; }
        return lb + (dx + dy) * oo;
    };
    std::vector<F32> width(total, 0.f), slope(total, 0.f), fallw(total, 0.f);
    for (S32 k = 0; k < total; ++k)
    {
        if (!is_stream(k)) continue;
        const S32 cx = k % n, cy = k / n;
        const S32 dn = downstream[k];
        if (dn >= 0)
        {
            const S32 dx = dn % n - cx, dy = dn / n - cy;
            const F32 run = ((dx && dy) ? 1.41421356f : 1.f) * mpg;
            slope[k] = llmax(0.f, z[k] - z[dn]) / run;
        }
        fallw[k] = smoothstep(FALL_START, FALL_FULL, slope[k]);
        // A river reads wider than a brook; a fall is narrower than the stream feeding it,
        // because its edges are in the open rather than hidden in a channel.
        const F32 grow = log2f(acc[k] / catchment_m2 + 1.f);
        width[k] = lerp(llclamp(4.f + 3.f * grow, 4.f, 24.f),
                        llclamp(2.f + 1.5f * grow, 2.f, 10.f), fallw[k]);
    }

    struct Chain { std::vector<S32> mCells; F32 mEndLevel; F32 mAcc; };
    std::vector<Chain> chains;
    std::vector<U8> used(total, 0);
    for (S32 head : order)
    {
        if (!is_stream(head) || used[head]) continue;
        Chain ch;
        S32 c = head;
        while (c >= 0 && is_stream(c) && !used[c])
        {
            used[c] = 1;
            ch.mCells.push_back(c);
            c = downstream[c];
        }
        // Where the chain ends up: under the river it joins (a shade lower, so the trunk
        // wins the depth test where they overlap instead of flickering), into the ground
        // before a prim or a built hollow, under an ACCEPTED pool's surface, under the sea,
        // or — a hollow too small or too shallow for a pool, or over the pool budget — just
        // on the ground: there is no water surface there to dive under, and the old
        // `filled` level would hang the ribbon's end in the air over that ground.
        // -1 = it ran off the region edge; nothing to add.
        bool has_end = false;
        if (c >= 0)
        {
            if (is_stream(c))               { ch.mEndLevel = (F32)filled[c] + STREAM_FILL_M - 0.03f; }
            else if (blocked[c])            { ch.mEndLevel = z[c] - BUILT_SINK_M; }
            else if (pooled[c])             { ch.mEndLevel = (F32)filled[c] - 0.03f; }
            else if (z[c] + STREAM_FILL_M <= sea) { ch.mEndLevel = sea - 0.05f; }
            else                            { ch.mEndLevel = (F32)filled[c] + STREAM_FILL_M - 0.03f; }
            ch.mCells.push_back(c);
            has_end = true;
        }
        if ((S32)ch.mCells.size() >= MIN_CHAIN_CELLS)
        {
            ch.mAcc = acc[ch.mCells[ch.mCells.size() - (has_end ? 2 : 1)]];
            chains.push_back(std::move(ch));
        }
    }
    std::sort(chains.begin(), chains.end(), [](const Chain& a, const Chain& b) { return a.mAcc > b.mAcc; });

    S32 verts_left = MAX_STREAM_VERTS;
    for (const Chain& ch : chains)
    {
        if ((S32)out.mStreams.size() >= MAX_STREAMS) break;
        const S32 nc = (S32)ch.mCells.size();

        // Per-cell width / fall / slope, the terminal cell borrowing its predecessor's.
        std::vector<F32> wd(nc), fl(nc), sl(nc);
        for (S32 i = 0; i < nc; ++i)
        {
            const S32 k = ch.mCells[i];
            const S32 src = (i == nc - 1 && !is_stream(k)) ? ch.mCells[i - 1] : k;
            wd[i] = width[src];
            fl[i] = fallw[src];
            sl[i] = slope[src];
        }
        {
            std::vector<F32> wd2(wd);
            for (S32 i = 1; i < nc - 1; ++i)
            {
                wd2[i] = 0.25f * wd[i - 1] + 0.5f * wd[i] + 0.25f * wd[i + 1];
            }
            wd.swap(wd2);
        }

        // Stations: STATIONS_PER_CELL per cell along a Catmull-Rom through the grid points,
        // which turns the D8 path's 45-degree zigzag into the bend of a real channel. Each
        // station's level comes from the ground at ITS position (the curve cuts corners the
        // straight grid path does not), so the water never dives under a spur on a bend.
        std::vector<LLVector3> st_pos;     // region space, z = level
        std::vector<F32> st_half, st_dist;
        auto cell_xy = [&](S32 i)
        {
            const S32 k = ch.mCells[llclamp(i, 0, nc - 1)];
            return LLVector2((k % n) * mpg, (k / n) * mpg);
        };
        F32 dist = 0.f;
        for (S32 i = 0; i < nc - 1; ++i)
        {
            const LLVector2 P0 = cell_xy(i - 1), P1 = cell_xy(i), P2 = cell_xy(i + 1), P3 = cell_xy(i + 2);
            for (S32 sub = 0; sub < STATIONS_PER_CELL; ++sub)
            {
                const F32 t = (F32)sub / STATIONS_PER_CELL;
                const F32 t2 = t * t, t3 = t2 * t;
                const LLVector2 xy = ((P1 * 2.f) + (P2 - P0) * t
                                    + (P0 * 2.f - P1 * 5.f + P2 * 4.f - P3) * t2
                                    + (P1 * 3.f - P0 - P2 * 3.f + P3) * t3) * 0.5f;
                const F32 fall = lerp(fl[i], fl[i + 1], t);
                const F32 ground = sample_w(xy.mV[VX], xy.mV[VY]);   // filled: a dip is a puddle, not a hole
                const LLVector3 pos(xy.mV[VX], xy.mV[VY], ground + lerp(STREAM_FILL_M, WATERFALL_LIFT_M, fall));
                if (!st_pos.empty())
                {
                    dist += (xy - LLVector2(st_pos.back().mV[VX], st_pos.back().mV[VY])).length();
                }
                st_pos.push_back(pos);
                st_half.push_back(0.5f * lerp(wd[i], wd[i + 1], t));
                st_dist.push_back(dist);
            }
        }
        {
            // Terminal station: the cell the chain ends in, at the level it was given (under
            // what it joins) — or, for a chain that ran off the region, its own ground.
            const S32 k = ch.mCells[nc - 1];
            const LLVector2 xy = cell_xy(nc - 1);
            const F32 zend = is_stream(k) ? (F32)filled[k] + lerp(STREAM_FILL_M, WATERFALL_LIFT_M, fl[nc - 1]) : ch.mEndLevel;
            const LLVector3 pos(xy.mV[VX], xy.mV[VY], zend);
            dist += (xy - LLVector2(st_pos.back().mV[VX], st_pos.back().mV[VY])).length();
            st_pos.push_back(pos);
            st_half.push_back(0.5f * wd[nc - 1]);
            st_dist.push_back(dist);
        }
        const S32 ns = (S32)st_pos.size();
        // Light 3-tap smooth on the level so a 1 m grid does not stair-step (the terminal
        // keeps its level), then the no-uphill clamp.
        {
            std::vector<F32> lv(ns);
            for (S32 i = 0; i < ns; ++i) lv[i] = st_pos[i].mV[VZ];
            for (S32 i = 1; i < ns - 1; ++i)
            {
                st_pos[i].mV[VZ] = 0.25f * lv[i - 1] + 0.5f * lv[i] + 0.25f * lv[i + 1];
            }
            // WATER NEVER FLOWS UPHILL: strictly non-increasing downstream. A station on a
            // corner the curve cuts across may end up under the ground for half a cell;
            // that is invisible, a rise is not.
            for (S32 i = 1; i < ns; ++i)
            {
                st_pos[i].mV[VZ] = llmin(st_pos[i].mV[VZ], st_pos[i - 1].mV[VZ]);
            }
        }
        const S32 W = RIBBON_ACROSS;
        // One face strides U16 indices, so a very long river is cut into pieces that share a
        // station; a piece boundary is invisible because both sides hold the same vertices.
        const S32 max_stations = 65535 / W;

        F32 mean_slope = 0.f, mean_width = 0.f;
        for (S32 i = 0; i < nc; ++i) { mean_slope += sl[i]; mean_width += wd[i]; }
        mean_slope /= nc;
        mean_width /= nc;

        for (S32 s0 = 0; s0 < ns - 1; s0 += max_stations - 1)
        {
            const S32 s1 = llmin(ns, s0 + max_stations);   // exclusive
            const S32 count = s1 - s0;
            if (count < 2) break;
            const S32 nverts = count * W;
            if (verts_left < nverts || (S32)out.mStreams.size() >= MAX_STREAMS) break;
            verts_left -= nverts;

            auto mesh = std::make_shared<LLVOWater::ConformingMesh>();
            mesh->mVerts.resize(nverts);
            mesh->mNormals.resize(nverts);
            mesh->mUVs.resize(nverts);
            mesh->mIndices.reserve((count - 1) * (W - 1) * 6);
            mesh->mMin.setVec(1e9f, 1e9f, 1e9f);
            mesh->mMax.setVec(-1e9f, -1e9f, -1e9f);

            for (S32 s = s0; s < s1; ++s)
            {
                // Horizontal tangent from the neighbouring stations; right = tangent x up,
                // so that (right x tangent) = up and the quads below wind face-up.
                const LLVector3& pa = st_pos[llmax(s - 1, 0)];
                const LLVector3& pb = st_pos[llmin(s + 1, ns - 1)];
                LLVector2 tangent(pb.mV[VX] - pa.mV[VX], pb.mV[VY] - pa.mV[VY]);
                if (tangent.length() < 1e-4f) tangent.setVec(1.f, 0.f);
                tangent.normalize();
                const LLVector2 right(tangent.mV[VY], -tangent.mV[VX]);
                for (S32 j = 0; j < W; ++j)
                {
                    const F32 u = (F32)j / (W - 1) * 2.f - 1.f;   // -1 .. 1 across
                    const F32 across = u * st_half[s];
                    const F32 sag_t = llmax(0.f, fabsf(u) - EDGE_SAG_FROM) / (1.f - EDGE_SAG_FROM);
                    const S32 v = (s - s0) * W + j;
                    LLVector3& P = mesh->mVerts[v];
                    const F32 px = st_pos[s].mV[VX] + right.mV[VX] * across;
                    const F32 py = st_pos[s].mV[VY] + right.mV[VY] * across;
                    F32 pz = st_pos[s].mV[VZ] - EDGE_SAG_M * sag_t * sag_t;
                    // Off the centreline the water may not stand deep over ground that
                    // falls away (the FILLED ground, so a puddle's cross-section stays a
                    // level sheet), and it never shows on a prim: those vertices go under
                    // the real ground.
                    const F32 ground = sample_z(px, py);
                    if (j != W / 2)
                    {
                        pz = llmin(pz, sample_w(px, py) + STREAM_FILL_M + MAX_STAND_M);
                    }
                    if (blocked_at(px, py))
                    {
                        pz = llmin(pz, ground - BUILT_SINK_M);
                    }
                    P.setVec(px, py, pz);
                    mesh->mUVs[v].setVec(st_dist[s], across);
                    mesh->mMin.mV[VX] = llmin(mesh->mMin.mV[VX], P.mV[VX]);
                    mesh->mMin.mV[VY] = llmin(mesh->mMin.mV[VY], P.mV[VY]);
                    mesh->mMin.mV[VZ] = llmin(mesh->mMin.mV[VZ], P.mV[VZ]);
                    mesh->mMax.mV[VX] = llmax(mesh->mMax.mV[VX], P.mV[VX]);
                    mesh->mMax.mV[VY] = llmax(mesh->mMax.mV[VY], P.mV[VY]);
                    mesh->mMax.mV[VZ] = llmax(mesh->mMax.mV[VZ], P.mV[VZ]);
                }
            }
            // Normals by central difference over the vertex grid: d/d(right) x d/d(along)
            // = up for a level ribbon, and tips with the surface over a ledge — which is
            // exactly what the shader reads the fall from.
            for (S32 s = 0; s < count; ++s)
            {
                for (S32 j = 0; j < W; ++j)
                {
                    const LLVector3& r0 = mesh->mVerts[s * W + llmax(j - 1, 0)];
                    const LLVector3& r1 = mesh->mVerts[s * W + llmin(j + 1, W - 1)];
                    const LLVector3& a0 = mesh->mVerts[llmax(s - 1, 0) * W + j];
                    const LLVector3& a1 = mesh->mVerts[llmin(s + 1, count - 1) * W + j];
                    LLVector3 nrm = (r1 - r0) % (a1 - a0);
                    if (nrm.length() < 1e-6f || nrm.mV[VZ] < 0.f) nrm.setVec(0.f, 0.f, 1.f);
                    nrm.normalize();
                    mesh->mNormals[s * W + j] = nrm;
                }
            }
            for (S32 s = 0; s < count - 1; ++s)
            {
                for (S32 j = 0; j < W - 1; ++j)
                {
                    const U16 i0 = (U16)(s * W + j);
                    const U16 i1 = (U16)(i0 + 1);
                    const U16 i2 = (U16)(i0 + W);
                    const U16 i3 = (U16)(i2 + 1);
                    mesh->mIndices.push_back(i0); mesh->mIndices.push_back(i1); mesh->mIndices.push_back(i2);
                    mesh->mIndices.push_back(i1); mesh->mIndices.push_back(i3); mesh->mIndices.push_back(i2);
                }
            }

            bake_lod(*mesh, W);
            Surface str;
            str.mMesh = mesh;
            str.mRank = ch.mAcc;
            // Surface speed: a brook on a gentle grade ambles, a steep one races. sqrt of the
            // grade is the shallow-water relation (celerity ~ sqrt(g h) with h set by slope).
            str.mFlow = llclamp(0.35f + 2.5f * sqrtf(mean_slope), 0.35f, 2.5f);
            // Body tint: deeper as the river widens, capped short of opaque.
            // A hill stream is shallow: the bed shows through and the water reads as a wet
            // sheen over the ground, not a canal of the region's fog colour. (0.3..0.9 made
            // every stream a dark ribbon, 2026-09-05.) Grows a little with width.
            str.mDepth = llclamp(0.08f + (mean_width - 4.f) * 0.015f, 0.08f, 0.35f);
            out.mStreams.push_back(std::move(str));
        }
    }
}

void WolfNaturalWater::apply(std::shared_ptr<Result> result)
{
    mBusy = false;
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp || !result || regionp->getHandle() != result->mRegionHandle)
    {
        return;   // teleported while computing; the next idle() recomputes for the new region
    }
    for (auto& p : mSurfaces)
    {
        if (p.notNull() && !p->isDead())
        {
            gObjectList.killObject(p);
        }
    }
    mSurfaces.clear();
    size_t verts = 0;
    auto create = [&](const Surface& sf)
    {
        LLVOWater* waterp = (LLVOWater*)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, regionp);
        if (!waterp)
        {
            return false;
        }
        // Transform before createObject — the drawable's extents are taken from what the
        // object holds at creation (fswolfwater.cpp, same rule); setConformingMesh sets
        // position and scale to the mesh's own box.
        waterp->setConformingMesh(sf.mMesh, sf.mFlow);
        waterp->setBoundedWaterDepth(llmax(sf.mDepth, 0.05f));
        waterp->mbCanSelect = false;
        gPipeline.createObject(waterp);
        mSurfaces.push_back(waterp);
        verts += sf.mMesh->mVerts.size();
        return true;
    };
    for (const Surface& sf : result->mPools)   { if (!create(sf)) break; }
    for (const Surface& sf : result->mStreams) { if (!create(sf)) break; }
    mAppliedStamp = result->mTerrainStamp;
    LL_INFOS("WolfNaturalWater") << "Natural water: " << result->mPools.size() << " pools, "
        << result->mBuiltBasins << " built hollows skipped, "
        << result->mStreams.size() << " streams (" << verts << " vertices) on region "
        << regionp->getName() << LL_ENDL;
}
