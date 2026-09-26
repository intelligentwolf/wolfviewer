/**
 * @file llsurface.cpp
 * @brief Implementation of LLSurface class
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"
#include "llterraingridoffset.h"

#include "llsurface.h"

#include "llpatchvertexarray.h"
#include "patch_dct.h"
#include "patch_code.h"
#include "llbitpack.h"
#include "llviewerobjectlist.h"
#include "llvosurfacepatch.h"   // <WolfViewer 2026-09-26/> block objects
#include "llregionhandle.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llworld.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llsurfacepatch.h"
#include "llvowater.h"
#include "pipeline.h"
#include "llviewerregion.h"
#include "llvlmanager.h"   // <FS:Wolf/> land packet counters for the terrain report
#include "lldrawpoolterrain.h"
#include "llworldmipmap.h"

extern LLPipeline gPipeline;
extern bool gShiftFrame;

namespace
{
    static constexpr float MIN_TEXTURE_REQUEST_INTERVAL = 5.0f;
}

LLColor4U MAX_WATER_COLOR(0, 48, 96, 240);

S32 LLSurface::sTextureSize = 256;

// ---------------- LLSurface:: Public Members ---------------

LLSurface::LLSurface(U32 type, LLViewerRegion *regionp) :
    mGridsPerEdge(0),
    mOOGridsPerEdge(0.f),
    mPatchesPerEdge(0),
    mNumberOfPatches(0),
    mType(type),
    mDetailTextureScale(0.f),
    mOriginGlobal(0.0, 0.0, 0.0),
    mSTexturep(nullptr),
    mGridsPerPatchEdge(0),
    mMetersPerGrid(1.0f),
    mMetersPerEdge(1.0f),
    mRegionp(regionp)
{
    // One of each for each camera
    mVisiblePatchCount = 0;

    mHasZData = false;
    // "uninitialized" min/max z
    mMinZ = 10000.f;
    mMaxZ = -10000.f;

    mWaterObjp = nullptr;

    // In here temporarily.
    mSurfacePatchUpdateCount = 0;

    for (S32 i = 0; i < 8; i++)
    {
        mNeighbors[i] = nullptr;
    }
}


LLSurface::~LLSurface()
{
    mGridsPerEdge = 0;
    mGridsPerPatchEdge = 0;
    mPatchesPerEdge = 0;
    mNumberOfPatches = 0;
    // <WolfViewer 2026-09-26> Block objects the region has not killed yet let go of this surface
    // and of its patches before the patches are deleted.
    for (auto& entry : mBlockObjects)
    {
        entry.second->detachSurface();
    }
    mBlockObjects.clear();
    // </WolfViewer>
    destroyPatchData();

    LLDrawPoolTerrain *poolp = (LLDrawPoolTerrain*) gPipeline.findPool(LLDrawPool::POOL_TERRAIN, mSTexturep);
    if (!poolp)
    {
        // <FS:Wolf/> Only a fault if we ever had one. The pool is created by the first
        // LLVOSurfacePatch (llvosurfacepatch.cpp:105) and those are built on demand, so a
        // surface the camera never reached has no pool and never had one.
        if (mBuiltPatchObject)
        {
            LL_WARNS() << "No pool for terrain on destruction!" << LL_ENDL;
        }
    }
    else if (poolp->mReferences.empty())
    {
        gPipeline.removePool(poolp);
        // Don't enable this until we blitz the draw pool for it as well.  -- djs
        mSTexturep = nullptr;
    }
    else
    {
        LL_ERRS() << "Terrain pool not empty!" << LL_ENDL;
    }
}

void LLSurface::initClasses()
{
}

void LLSurface::setRegion(LLViewerRegion *regionp)
{
    mRegionp = regionp;
    mWaterObjp = nullptr; // depends on regionp, needs recreating
}

// Assumes that arguments are powers of 2, and that
// grids_per_edge / grids_per_patch_edge = power of 2
void LLSurface::create(const S32 grids_per_edge,
                       const S32 grids_per_patch_edge,
                       const LLVector3d &origin_global,
                       const F32 width)
{
    // Initialize various constants for the surface
    mGridsPerEdge = grids_per_edge + 1;  // Add 1 for the east and north buffer
    mOOGridsPerEdge = 1.f / mGridsPerEdge;
    mGridsPerPatchEdge = grids_per_patch_edge;
    mPatchesPerEdge = (mGridsPerEdge - 1) / mGridsPerPatchEdge;
    mNumberOfPatches = (S64)mPatchesPerEdge * mPatchesPerEdge;   // <WolfViewer 2026-09-23/> S64
    mMetersPerGrid = width / ((F32)(mGridsPerEdge - 1));

    // <FS:Wolf/> The comment above this function still says the arguments are powers of two.
    // They are not any more — Wolf Territories' 25,600 m region is 100 standard regions, and
    // 25600 is not a power of two — and the Aurora patches made the pieces that cared cope:
    // LLPatchVertexArray::create rounds its (unused) surface width up, and sTextureSize rounds
    // and clamps. The assumption that DOES still bite is divisibility: if the grid does not
    // divide exactly into patches the last row and column of the region are simply never
    // covered by any patch, silently. Say so rather than render a region with a missing edge.
    if (((mGridsPerEdge - 1) % mGridsPerPatchEdge) != 0)
    {
        LL_WARNS("Terrain") << "Region is " << (S32)width << " m with " << (mGridsPerEdge - 1)
                            << " grids per edge, which does not divide by "
                            << mGridsPerPatchEdge << " grids per patch: "
                            << ((mGridsPerEdge - 1) % mGridsPerPatchEdge)
                            << " grids along each edge will have no patch covering them."
                            << LL_ENDL;
    }
    mMetersPerEdge = mMetersPerGrid * (mGridsPerEdge - 1);
// <FS:CR> Aurora Sim
    sTextureSize = (S32)width;

    // Trap non-power of 2 widths to avoid GLtexture issues.
    if ((sTextureSize & (sTextureSize - 1)) != 0)
    {
        // Not a power of 2, find the next power of 2
        sTextureSize = 1 << static_cast<S32>( ceil(log2(sTextureSize)) ) ;
    }

    // Clamp to maximum limit
    sTextureSize = std::min(sTextureSize, 1024);
// </FS:CR> Aurora Sim

    mOriginGlobal.setVec(origin_global);

    mPVArray.create(mGridsPerEdge, mGridsPerPatchEdge, LLWorld::getInstance()->getRegionScale());

    // <WolfViewer 2026-09-23> No region-sized arrays any more. The heights and normals were two
    // calloc'd arrays of (grids_per_edge + 1)^2 points - 16 bytes per square metre, reserved not
    // committed (the 2026-09-09 change) - which is 17 TB of address space at 1,048,576 m: past
    // what Linux's commit heuristic or Windows' commit charge will reserve, and fatal (LL_ERRS)
    // when refused. Each patch now owns its own block (LLSurfacePatch::initData), made with the
    // patch the first time it is needed (createPatch).
    // </WolfViewer>

    mVisiblePatchCount = 0;


    ///////////////////////
    //
    // Initialize textures
    //

    initTextures();

    // Has to be done after texture initialization
    createPatchData();
}

LLViewerTexture* LLSurface::getSTexture()
{
    if (mSTexturep.notNull() && !mSTexturep->hasGLTexture())
    {
        createSTexture();
    }
    return mSTexturep;
}

void LLSurface::createSTexture()
{
    if (mSTexturep.isNull())
    {
        mTimer.setTimerExpirySec(MIN_TEXTURE_REQUEST_INTERVAL);
    }
    else if (mSTexturep->hasGLTexture())
    {
        // Unexpected: createSTexture() called when a valid texture already exists.
        // This may indicate a logic error in the caller, as textures should not be recreated unnecessarily.
        LL_WARNS() << "Called LLSurface::createSTexture() while we already have a valid texture!" << LL_ENDL;
        return;
    }
    else if (!mTimer.checkExpirationAndReset(MIN_TEXTURE_REQUEST_INTERVAL))
    {
        // We haven't gotten a valid texture yet, but throttle the number of requests to avoid server flooding
        return;
    }

    U64 handle = mRegionp->getHandle();
    U32 grid_x, grid_y;

    grid_from_region_handle(handle, &grid_x, &grid_y);

    mSTexturep = LLWorldMipmap::loadObjectsTile(grid_x, grid_y, 1);
}

void LLSurface::initTextures()
{
    ///////////////////////
    //
    // Main surface texture
    //
    createSTexture();

    ///////////////////////
    //
    // Water object
    //
// <FS:CR> Aurora Sim
    //if (gSavedSettings.getBOOL("RenderWater") )
    if (gSavedSettings.getBOOL("RenderWater") && LLWorld::getInstance()->getAllowRenderWater())
// </FS:CR> Aurora Sim
    {
        mWaterObjp = (LLVOWater *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, mRegionp);
        gPipeline.createObject(mWaterObjp);
        LLVector3d water_pos_global = from_region_handle(mRegionp->getHandle());
// <FS:CR> Aurora Sim
        //water_pos_global += LLVector3d(128.0, 128.0, DEFAULT_WATER_HEIGHT);       // region doesn't have a valid water height yet
        water_pos_global += LLVector3d(mRegionp->getWidth()/2, mRegionp->getWidth()/2, DEFAULT_WATER_HEIGHT);
        mWaterObjp->setPositionGlobal(water_pos_global);
    }
}

void LLSurface::rebuildWater()
{
    bool renderwater = gSavedSettings.getBOOL("RenderWater") && LLWorld::getInstance()->getAllowRenderWater();
    bool prev_renderwater = !mWaterObjp.isNull();

    if(prev_renderwater && !renderwater)
    {
        gObjectList.killObject(mWaterObjp);
    }

    if (!prev_renderwater && renderwater)
    {
        mWaterObjp = (LLVOWater *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, mRegionp);
        gPipeline.createObject(mWaterObjp);
        LLVector3d water_pos_global = from_region_handle(mRegionp->getHandle());
        water_pos_global += LLVector3d(mRegionp->getWidth()/2, mRegionp->getWidth()/2, DEFAULT_WATER_HEIGHT);
// </FS:CR> Aurora Sim
        mWaterObjp->setPositionGlobal(water_pos_global);
    }
}


void LLSurface::setOriginGlobal(const LLVector3d &origin_global)
{
    LLVector3d new_origin_global;
    mOriginGlobal = origin_global;
    // Need to update the southwest corners of the patches
    // <WolfViewer 2026-09-23/> the patches that exist; one made later gets its origin from this.
    for (LLSurfacePatch* patchp : mAllPatches)
    {
        const S32 i = patchp->getPatchX();
        const S32 j = patchp->getPatchY();

        new_origin_global = patchp->getOriginGlobal();

        new_origin_global.mdV[0] = mOriginGlobal.mdV[0] + i * mMetersPerGrid * mGridsPerPatchEdge;
        new_origin_global.mdV[1] = mOriginGlobal.mdV[1] + j * mMetersPerGrid * mGridsPerPatchEdge;
        patchp->setOriginGlobal(new_origin_global);
    }

    // Hack!
    if (mWaterObjp.notNull() && mWaterObjp->mDrawable.notNull())
    {
// <FS:CR> Aurora Sim
        //const F64 x = origin_global.mdV[VX] + 128.0;
        //const F64 y = origin_global.mdV[VY] + 128.0;
        const F64 x = origin_global.mdV[VX] + (F64)mRegionp->getWidth()/2;
        const F64 y = origin_global.mdV[VY] + (F64)mRegionp->getWidth()/2;
// </FS:CR> Aurora Sim
        const F64 z = mWaterObjp->getPositionGlobal().mdV[VZ];

        LLVector3d water_origin_global(x, y, z);

        mWaterObjp->setPositionGlobal(water_origin_global);
    }
}

void LLSurface::getNeighboringRegions( std::vector<LLViewerRegion*>& uniqueRegions )
{
    S32 i;
    for (i = 0; i < 8; i++)
    {
        if (mNeighbors[i] != nullptr)
        {
            uniqueRegions.push_back( mNeighbors[i]->getRegion() );
        }
    }
}


void LLSurface::getNeighboringRegionsStatus( std::vector<S32>& regions )
{
    S32 i;
    for (i = 0; i < 8; i++)
    {
        if (mNeighbors[i] != nullptr)
        {
            regions.push_back( i );
        }
    }
}

void LLSurface::connectNeighbor(LLSurface *neighborp, U32 direction)
{
    mNeighbors[direction] = neighborp;
    neighborp->mNeighbors[gDirOpposite[direction]] = this;

    // <WolfViewer 2026-09-23> Link the patches along the shared edge that exist now; any made
    // later link themselves as they are made (createPatch -> linkPatch). This used to walk the
    // whole edge with getPatch - the Aurora own_offset / neighbor_offset loops - pairing every
    // patch on this side with the one opposite, and for a 1,048,576 m region that is 65,536
    // patches a side, every one of them made just to be linked. linkPatch finds the same pairs
    // by world position (an east patch's NE/SE partners along the edge, the single corner patch
    // for a diagonal neighbour) and refreshes the shared edge exactly as each branch here did.
    for (LLSurfacePatch* patchp : mAllPatches)
    {
        const S32 x = patchp->getPatchX();
        const S32 y = patchp->getPatchY();
        if (x == 0 || y == 0 || x == mPatchesPerEdge - 1 || y == mPatchesPerEdge - 1)
        {
            linkPatch(patchp, x, y, neighborp);
        }
    }
    // </WolfViewer>
}

// <WolfViewer 2026-09-23> Global patch position of this surface's patch (0, 0): world metres over
// the patch size. Region origins are whole 256 m, so this is exact.
S64 LLSurface::globalPatchX() const
{
    const F64 patch_m = (F64)mMetersPerGrid * mGridsPerPatchEdge;
    return (S64)floor(mOriginGlobal.mdV[VX] / patch_m + 0.5);
}

S64 LLSurface::globalPatchY() const
{
    const F64 patch_m = (F64)mMetersPerGrid * mGridsPerPatchEdge;
    return (S64)floor(mOriginGlobal.mdV[VY] / patch_m + 0.5);
}

LLSurface *LLSurface::neighborSurfaceAt(const S64 gx, const S64 gy, S32 &nx, S32 &ny) const
{
    const F32 patch_m = mMetersPerGrid * mGridsPerPatchEdge;
    for (S32 d = 0; d < 8; d++)
    {
        LLSurface* other = mNeighbors[d];
        if (!other || other->mPatchesPerEdge <= 0)
        {
            continue;
        }
        // Patches can only pair up if they are the same size (always 16 m today).
        if (other->mMetersPerGrid * other->mGridsPerPatchEdge != patch_m)
        {
            continue;
        }
        const S64 ox = other->globalPatchX(), oy = other->globalPatchY();
        if (gx >= ox && gx < ox + other->mPatchesPerEdge && gy >= oy && gy < oy + other->mPatchesPerEdge)
        {
            nx = (S32)(gx - ox);
            ny = (S32)(gy - oy);
            return other;
        }
    }
    return nullptr;
}

bool LLSurface::expectsPatchAt(const S32 x, const S32 y) const
{
    if (x >= 0 && y >= 0 && x < mPatchesPerEdge && y < mPatchesPerEdge)
    {
        return true;
    }
    S32 nx, ny;
    return neighborSurfaceAt(globalPatchX() + x, globalPatchY() + y, nx, ny) != nullptr;
}

void LLSurface::linkPatch(LLSurfacePatch *patchp, const S32 x, const S32 y, const LLSurface *only_surface)
{
    const S64 gpx = globalPatchX(), gpy = globalPatchY();
    for (U32 dir = 0; dir < 8; dir++)
    {
        const S32 tx = x + gDirAxes[dir][0];
        const S32 ty = y + gDirAxes[dir][1];
        if (tx >= 0 && ty >= 0 && tx < mPatchesPerEdge && ty < mPatchesPerEdge)
        {
            // Same surface: plain neighbour pointers both ways, as createPatchData set them.
            if (only_surface)
            {
                continue;
            }
            if (LLSurfacePatch* other = findPatch(tx, ty))
            {
                patchp->setNeighborPatch(dir, other);
                other->setNeighborPatch(gDirOpposite[dir], patchp);
                // When the surface was one array, the west/south patch's buffer column/row WAS the
                // east/north patch's memory. With a block per patch it is a copy, and before this
                // link the older patch had its own (extrapolated or zero) cells there, so the patch
                // on the west (south, southwest) side takes the shared cells now. Patches are not
                // only made in terrain-arrival order: LLTerrainPaintMap makes all of a region's.
                LLSurfacePatch* refreshed = nullptr;
                switch (dir)
                {
                    case EAST:      patchp->updateEastEdge();        refreshed = patchp; break;
                    case WEST:      other->updateEastEdge();         refreshed = other;  break;
                    case NORTH:     patchp->updateNorthEdge();       refreshed = patchp; break;
                    case SOUTH:     other->updateNorthEdge();        refreshed = other;  break;
                    case NORTHEAST: patchp->updateNortheastCorner(); refreshed = patchp; break;
                    case SOUTHWEST: other->updateNortheastCorner();  refreshed = other;  break;
                    default:        break;   // NW / SE patches share no cells
                }
                if (refreshed)
                {
                    refreshed->dirtyZ();
                }
            }
            continue;
        }

        S32 nx, ny;
        LLSurface* surface = neighborSurfaceAt(gpx + tx, gpy + ty, nx, ny);
        if (!surface || (only_surface && surface != only_surface))
        {
            continue;
        }
        LLSurfacePatch* other = surface->findPatch(nx, ny);
        if (!other)
        {
            continue;
        }
        // Another surface: LLSurfacePatch::connectNeighbor, which also marks the connected edge.
        patchp->connectNeighbor(other, dir);
        other->connectNeighbor(patchp, gDirOpposite[dir]);

        // The edge refresh each branch of the old connectNeighbor did for its link: the patch
        // on the west (or south) side takes the shared column (or row) and is re-dirtied.
        LLSurfacePatch* sw_patch = nullptr;
        switch (dir)
        {
            case EAST:
                patchp->updateEastEdge();
                patchp->dirtyZ();
                break;
            case WEST:
                other->updateEastEdge();
                other->dirtyZ();
                break;
            case NORTH:
                patchp->updateNorthEdge();
                patchp->dirtyZ();
                break;
            case SOUTH:
                other->updateNorthEdge();
                other->dirtyZ();
                break;
            case NORTHEAST:
                sw_patch = patchp;
                break;
            case SOUTHWEST:
                sw_patch = other;
                break;
            default:
                break;   // NW / SE links never refreshed an edge
        }
        if (sw_patch)
        {
            // A NE/SW pair refreshed only when it was a pure corner connection (the NORTHEAST
            // and SOUTHWEST branches), never along an east or north edge. It is a corner when the
            // southwest patch's north and east positions are not in the northeast one's surface.
            LLSurface* ne_surface = (sw_patch == patchp) ? surface : this;
            LLSurface* sw_surface = sw_patch->getSurface();
            const S64 sgx = sw_surface->globalPatchX() + sw_patch->getPatchX();
            const S64 sgy = sw_surface->globalPatchY() + sw_patch->getPatchY();
            S32 ex, ey;
            const bool east_in_ne = sw_surface->neighborSurfaceAt(sgx + 1, sgy, ex, ey) == ne_surface;
            const bool north_in_ne = sw_surface->neighborSurfaceAt(sgx, sgy + 1, ex, ey) == ne_surface;
            if (!east_in_ne && !north_in_ne)
            {
                // Upstream refreshed the north edge from one side and the east from the other
                // ("only update one of north or east"); both are idempotent copies, so both.
                sw_patch->updateNorthEdge();
                sw_patch->updateEastEdge();
                sw_patch->dirtyZ();
            }
        }
    }
}
// </WolfViewer>

void LLSurface::disconnectNeighbor(LLSurface *surfacep)
{
    S32 i;
    for (i = 0; i < 8; i++)
    {
        if (surfacep == mNeighbors[i])
        {
            mNeighbors[i] = nullptr;
        }
    }

    // Iterate through surface patches, removing any connectivity to removed surface.
    for (LLSurfacePatch* patchp : mAllPatches)   // <WolfViewer 2026-09-23/> the ones that exist
    {
        patchp->disconnectNeighbor(surfacep);
    }
}


void LLSurface::disconnectAllNeighbors()
{
    S32 i;
    for (i = 0; i < 8; i++)
    {
        if (mNeighbors[i])
        {
            mNeighbors[i]->disconnectNeighbor(this);
            mNeighbors[i] = nullptr;
        }
    }
}



const LLVector3d &LLSurface::getOriginGlobal() const
{
    return mOriginGlobal;
}

LLVector3 LLSurface::getOriginAgent() const
{
    return gAgent.getPosAgentFromGlobal(mOriginGlobal);
}

F32 LLSurface::getMetersPerGrid() const
{
    return mMetersPerGrid;
}

S32 LLSurface::getGridsPerEdge() const
{
    return mGridsPerEdge;
}

S32 LLSurface::getPatchesPerEdge() const
{
    return mPatchesPerEdge;
}

S32 LLSurface::getGridsPerPatchEdge() const
{
    return mGridsPerPatchEdge;
}

void LLSurface::moveZ(const S32 x, const S32 y, const F32 delta)
{
    llassert(x >= 0);
    llassert(y >= 0);
    llassert(x < mGridsPerEdge);
    llassert(y < mGridsPerEdge);
    // <WolfViewer 2026-09-23> A grid point on a patch boundary is held by up to four patch
    // blocks now (its own and the west/south/southwest neighbours' buffer edges), which the one
    // array held once; move every copy.
    const S32 gpp = (S32)mGridsPerPatchEdge;
    const S32 px = llmin(x / gpp, mPatchesPerEdge - 1), py = llmin(y / gpp, mPatchesPerEdge - 1);
    for (S32 dx = 0; dx <= 1; dx++)
    {
        for (S32 dy = 0; dy <= 1; dy++)
        {
            const S32 pi = px - dx, pj = py - dy;
            const S32 lx = x - pi * gpp, ly = y - pj * gpp;
            if (pi < 0 || pj < 0 || lx > gpp || ly > gpp)
            {
                continue;
            }
            if (LLSurfacePatch* patchp = findPatch(pi, pj))
            {
                patchp->getDataZ()[lx + ly * (S32)patchp->getDataStride()] += delta;
            }
        }
    }
    // </WolfViewer>
}

// <WolfViewer 2026-09-23> See llsurface.h. Point (i, j) is read from the patch whose block
// starts at or before it - (i / gpp, j / gpp), or the last patch for the east/north buffer line.
F32 LLSurface::getZ(const S32 i, const S32 j) const
{
    if (i < 0 || j < 0 || i >= mGridsPerEdge || j >= mGridsPerEdge || mPatchesPerEdge <= 0)
    {
        return 0.f;
    }
    const S32 gpp = (S32)mGridsPerPatchEdge;
    const S32 pi = llmin(i / gpp, mPatchesPerEdge - 1);
    const S32 pj = llmin(j / gpp, mPatchesPerEdge - 1);
    const LLSurfacePatch* patchp = findPatch(pi, pj);
    if (!patchp)
    {
        return 0.f;
    }
    return patchp->getDataZ()[(i - pi * gpp) + (j - pj * gpp) * (S32)patchp->getDataStride()];
}
// </WolfViewer>


// <FS:Wolf> Terrain diagnostics. See the note on the counters in llsurface.h.
void LLSurface::noteTerrainDataArrived(const LLVector3& patch_center_region)
{
    mDiagPatchesWithData++;
    mDiagArrivedSinceReport++;

    if (!mRegionp)
    {
        return;
    }
    const LLVector3 cam = mRegionp->getPosRegionFromGlobal(gAgentCamera.getCameraPositionGlobal());
    const F32 dist = (patch_center_region - cam).magVec();

    if (mDiagNearestDataM < 0.f || dist < mDiagNearestDataM)
    {
        mDiagNearestDataM = dist;
    }
    if (dist > mDiagFarthestDataM)
    {
        mDiagFarthestDataM = dist;
    }
}

void LLSurface::reportTerrainDiagnostics()
{
    // Only worth saying anything on a region large enough for the streaming order to matter.
    // A standard region finishes before you could read the first line.
    const S32 DIAG_MIN_PATCHES = 4096;
    const F32 REPORT_SECONDS = 10.f;
    if (mNumberOfPatches < DIAG_MIN_PATCHES || !mDiagTimer.checkExpirationAndReset(REPORT_SECONDS))
    {
        return;
    }

    LL_INFOS("Terrain") << "terrain " << (mRegionp ? mRegionp->getName() : std::string("?"))
                        << " " << (S32)(mMetersPerGrid * (F32)(mGridsPerEdge - 1)) << "m"
                        << " land_pkts rx " << gVLManager.mLandPacketsReceived
                        << " unpacked " << gVLManager.mLandPacketsUnpacked
                        << " patches " << mNumberOfPatches
                        << " with_data " << mDiagPatchesWithData
                        << " (" << mDiagArrivedSinceReport << " new, nearest "
                        << (mDiagNearestDataM < 0.f ? -1 : (S32)mDiagNearestDataM) << "m farthest "
                        << (S32)mDiagFarthestDataM << "m)"
                        << " objects " << mDiagObjectsBuilt
                        << " visible " << (mDiagScanRan ? std::to_string(mVisiblePatchCount)
                                                            : std::string("not-scanned"))
                        << " dirty " << mDiagDirtyListSize
                        << " normals_skipped " << mDiagNormalsSkipped
                        << " | tex built " << mDiagTexBuilt
                        << " no_object " << mDiagTexNoVObj
                        << " wait_neighbours " << mDiagTexWaitNeighbors
                        << " wait_heights " << mDiagTexWaitHeights
                        << " wait_composition " << mDiagTexWaitComposition
                        << LL_ENDL;

    mDiagArrivedSinceReport = 0;
    mDiagNearestDataM = -1.f;
    mDiagFarthestDataM = -1.f;
    mDiagTexNoVObj = 0;
    mDiagTexWaitNeighbors = 0;
    mDiagTexWaitHeights = 0;
    mDiagTexWaitComposition = 0;
    mDiagTexBuilt = 0;
    mDiagNormalsSkipped = 0;
}
// </FS:Wolf>

void LLSurface::updatePatchVisibilities(LLAgent &agent)
{
    if (gShiftFrame)
    {
        return;
    }

    LLVector3 pos_region = mRegionp->getPosRegionFromGlobal(gAgentCamera.getCameraPositionGlobal());

    LLSurfacePatch *patchp;

    // <FS:Wolf> Only scan the patches that could possibly be visible.
    //
    // This walked EVERY patch in the region every frame. A standard region has 256 of them, so
    // it never mattered; Wolf Territories' 25600 m "Dire Wolf" has 1600 x 1600 = 2,560,000, and
    // a frustum test plus LOD maths on all of them, every frame, is what made that region hang.
    //
    // A patch beyond the camera's far plane cannot be drawn, so the scan is bounded to the patch
    // index box the far clip reaches, plus one patch of slack. Two passes: the box we scanned
    // last frame (so anything that has just fallen out of range is told it is invisible) and the
    // box we want now. Both are bounded, so a teleport across the region costs two small scans
    // rather than one enormous one.
    //
    // Small regions keep the original whole-region loop, byte for byte, so nothing that works
    // today can regress.
    const S32 BOUNDED_SCAN_MIN_PATCHES = 4096;   // i.e. regions above 1024 m

    mDiagScanRan = true;   // <FS:Wolf/>

    if (mNumberOfPatches < BOUNDED_SCAN_MIN_PATCHES)
    {
        mVisiblePatchCount = 0;
        // <WolfViewer 2026-09-23/> every patch that exists (one never made has no data to draw)
        for (LLSurfacePatch* each : mAllPatches)
        {
            patchp = each;

            patchp->updateVisibility();
            if (patchp->getVisible())
            {
                mVisiblePatchCount++;
                patchp->updateCameraDistanceRegion(pos_region);
            }
        }
        return;
    }

    const F32 patch_meters = mMetersPerGrid * (F32)mGridsPerPatchEdge;
    const F32 reach = LLViewerCamera::getInstance()->getFar() + patch_meters;
    const S32 last = mPatchesPerEdge - 1;

    const S32 min_i = llclamp((S32)floorf((pos_region.mV[VX] - reach) / patch_meters), 0, last);
    const S32 max_i = llclamp((S32)ceilf ((pos_region.mV[VX] + reach) / patch_meters), 0, last);
    const S32 min_j = llclamp((S32)floorf((pos_region.mV[VY] - reach) / patch_meters), 0, last);
    const S32 max_j = llclamp((S32)ceilf ((pos_region.mV[VY] + reach) / patch_meters), 0, last);

    // Pass 1: whatever we scanned last frame and are about to stop scanning. updateVisibility
    // will find it out of frustum or out of range and clear its visible flag.
    if (mLastScanMinI >= 0)
    {
        for (S32 j = mLastScanMinJ; j <= mLastScanMaxJ; j++)
        {
            for (S32 i = mLastScanMinI; i <= mLastScanMaxI; i++)
            {
                if (i >= min_i && i <= max_i && j >= min_j && j <= max_j)
                {
                    continue;   // still in range; pass 2 handles it
                }
                if (LLSurfacePatch* old_patch = findPatch(i, j))   // <WolfViewer 2026-09-23/>
                {
                    old_patch->updateVisibility();
                    // <WolfViewer 2026-09-23> Past the far clip it cannot be drawn: release its
                    // viewer object (LLVOSurfacePatch + drawable, ~1.4 KB) and let ensureVObj make
                    // a new one if it comes back in range. Kept, they accumulated over every patch
                    // the camera had ever reached - on a region this size, gigabytes after a long
                    // flight. The patch and its heights stay: the sim does not resend terrain.
                    old_patch->releaseVObj();
                }
            }
        }
    }

    // Pass 2: the patches in range now. This is the only pass that counts.
    //
    // <WolfViewer 2026-09-26> ...culled a 256 m BLOCK at a time first (the terrain draw block,
    // LLSurfacePatch::WOLF_TERRAIN_BLOCK_M). With a 4,096 m far clip on Ireland every loaded patch
    // in the box - tens of thousands, most beside or behind the camera - ran the frustum test and
    // LOD maths every frame (perf: 11.6% of the main thread). A block whose box is outside the
    // frustum has every patch in it marked invisible, exactly what updateVisibility decides for
    // each of them (and without making viewer objects for patches that cannot be seen). The box
    // holds every patch's own test box: a patch tests centre +- mRadius, with centre z inside the
    // region's [mMinZ, mMaxZ] and mRadius = |(P, P, dz)| / 2, dz <= mMaxZ - mMinZ
    // (LLSurfacePatch::updateVerticalStats), so r_max = |(P, P, mMaxZ - mMinZ)| / 2.
    const S32 block_patches = llmax(1, (S32)(LLSurfacePatch::WOLF_TERRAIN_BLOCK_M / patch_meters));
    const bool block_cull = mMaxZ >= mMinZ;   // no height data yet: test every patch as before
    const F32 dz = block_cull ? (mMaxZ - mMinZ) : 0.f;
    const F32 r_max = 0.5f * sqrtf(2.f * patch_meters * patch_meters + dz * dz);
    const F32 block_m = (F32)block_patches * patch_meters;
    LLVector4a block_half;
    block_half.set(0.5f * block_m - 0.5f * patch_meters + r_max, 0.5f * block_m - 0.5f * patch_meters + r_max, 0.5f * dz + r_max);
    const LLVector3 origin_agent = getOriginAgent();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mVisiblePatchCount = 0;
    for (S32 bj = (min_j / block_patches) * block_patches; bj <= max_j; bj += block_patches)
    {
        for (S32 bi = (min_i / block_patches) * block_patches; bi <= max_i; bi += block_patches)
        {
            const S32 i0 = llmax(bi, min_i), i1 = llmin(bi + block_patches - 1, max_i);
            const S32 j0 = llmax(bj, min_j), j1 = llmin(bj + block_patches - 1, max_j);
            bool in_view = true;
            if (block_cull)
            {
                LLVector4a block_center;
                block_center.set(origin_agent.mV[VX] + ((F32)bi + 0.5f * (F32)block_patches) * patch_meters,
                                 origin_agent.mV[VY] + ((F32)bj + 0.5f * (F32)block_patches) * patch_meters,
                                 origin_agent.mV[VZ] + 0.5f * (mMinZ + mMaxZ));
                in_view = camera->AABBInFrustumNoFarClip(block_center, block_half) != 0;
            }
            for (S32 j = j0; j <= j1; j++)
            {
                for (S32 i = i0; i <= i1; i++)
                {
                    // <WolfViewer 2026-09-23/> only patches that exist; one never made has no data
                    patchp = findPatch(i, j);
                    if (!patchp)
                    {
                        continue;
                    }
                    if (!in_view)
                    {
                        patchp->setInvisible();
                        continue;
                    }
                    patchp->updateVisibility();
                    if (patchp->getVisible())
                    {
                        mVisiblePatchCount++;
                        patchp->updateCameraDistanceRegion(pos_region);
                    }
                }
            }
        }
    }
    // </WolfViewer>

    mLastScanMinI = min_i; mLastScanMaxI = max_i;
    mLastScanMinJ = min_j; mLastScanMaxJ = max_j;
    // </FS:Wolf>
}

template<bool PBR>
bool LLSurface::idleUpdate(F32 max_update_time)
{
    if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_TERRAIN))
    {
        return false;
    }

    // Perform idle time update of non-critical stuff.
    // In this case, texture and normal updates.
    LLTimer update_timer;
    bool did_update = false;

    // If the Z height data has changed, we need to rebuild our
    // property line vertex arrays.
    if (mDirtyPatchList.size() > 0)
    {
        getRegion()->dirtyHeights();
    }

    // Always call updateNormals() / updateVerticalStats()
    //  every frame to avoid artifacts
    for(std::set<LLSurfacePatch *>::iterator iter = mDirtyPatchList.begin();
        iter != mDirtyPatchList.end(); )
    {
        std::set<LLSurfacePatch *>::iterator curiter = iter++;
        LLSurfacePatch *patchp = *curiter;
        patchp->updateNormals<PBR>();
        patchp->updateVerticalStats();
        if (max_update_time == 0.f || update_timer.getElapsedTimeF32() < max_update_time)
        {
            if (patchp->updateTexture())
            {
                did_update = true;
                patchp->clearDirty();
                mDirtyPatchList.erase(curiter);
            }
        }
    }

    // some patches changed, update region reflection probes
    mRegionp->updateReflectionProbes(did_update);

    // <FS:Wolf/> Reported from here, not from updatePatchVisibilities: idleUpdate runs for every
    // region every frame whether or not its land is visible, so a region that draws nothing at
    // all still says why.
    mDiagDirtyListSize = (S32)mDirtyPatchList.size();
    reportTerrainDiagnostics();

    return did_update;
}

template bool LLSurface::idleUpdate</*PBR=*/false>(F32 max_update_time);
template bool LLSurface::idleUpdate</*PBR=*/true>(F32 max_update_time);

void LLSurface::decompressDCTPatch(LLBitPack &bitpack, LLGroupHeader *gopp, bool b_large_patch)
{

    LLPatchHeader  ph;
    S32 j, i;
    S32 patch[LARGE_PATCH_SIZE*LARGE_PATCH_SIZE];
    LLSurfacePatch *patchp;

    init_patch_decompressor(gopp->patch_size);

    // <FS:Wolf> A coarse grid still receives full-resolution patches.
    //
    // A large varregion is given fewer grid points per patch than the wire sends samples, to
    // keep terrain memory bounded (llviewerregion.cpp chooseTerrainGridResolution). The
    // decompressor writes patch_size x patch_size values at whatever stride it is handed, so if
    // the grid is coarser than the wire it must NOT be pointed straight at the surface: it would
    // overrun the patch into its neighbours. Decompress into a scratch block at full resolution
    // instead and take every step-th sample.
    //
    // patch_size arrives off the wire, so this is a bounds guard and not just a resolution
    // convenience: a sim sending a larger patch_size than this surface has grid points per patch
    // would otherwise write past the end of the patch.
    //
    // grids_per_patch is taken into an S32 because mGridsPerPatchEdge is unsigned and everything
    // it is compared and divided against here is signed; MSVC makes that mismatch a hard error.
    const S32 wire_samples = gopp->patch_size;
    const S32 grids_per_patch = (S32)mGridsPerPatchEdge;
    // <WolfViewer 2026-09-23> Always decompress into a scratch block, then copy into the patch.
    // The decompressor writes rows at gopp->stride, and LLGroupHeader::stride is a U16
    // (patch_dct.h): the surface's grids per edge - once the stride of the region-wide array -
    // wrapped for every region 65,536 m or wider (102,401 -> 36,865; 1,048,577 -> 1), scrambling
    // every patch. Patches keep their own blocks now anyway, so the stride is the wire's own
    // patch width, which always fits.
    gopp->stride = wire_samples;
    if (wire_samples <= 0 || wire_samples > LARGE_PATCH_SIZE)
    {
        // The scratch block below is LARGE_PATCH_SIZE square; patch_size is off the wire.
        LL_WARNS("Terrain") << "Ignoring terrain with patch size " << wire_samples << LL_ENDL;
        return;
    }
    // </WolfViewer>
    // </FS:Wolf>
    set_group_of_patch_header(gopp);

    while (1)
    {
// <FS:CR> Aurora Sim
        //decode_patch_header(bitpack, &ph);
        decode_patch_header(bitpack, &ph, b_large_patch);
// </FS:CR> Aurora Sim
        if (ph.quant_wbits == END_OF_PATCHES)
        {
            break;
        }

// <FS:CR> Aurora Sim
        //i = ph.patchids >> 5;
        //j = ph.patchids & 0x1F;
        if (b_large_patch)
        {
            i = ph.patchids >> 16; //x
            j = ph.patchids & 0xFFFF; //y
        }
        else
        {
            i = ph.patchids >> 5; //x
            j = ph.patchids & 0x1F; //y
        }
// </FS:CR> Aurora Sim

        if ((i >= mPatchesPerEdge) || (j >= mPatchesPerEdge))
        {
            LL_WARNS() << "Received invalid terrain packet - patch header patch ID incorrect!"
                << " patches per edge " << mPatchesPerEdge
                << " i " << i
                << " j " << j
                << " dc_offset " << ph.dc_offset
                << " range " << (S32)ph.range
                << " quant_wbits " << (S32)ph.quant_wbits
                << " patchids " << (S32)ph.patchids
                << LL_ENDL;
            return;
        }

        patchp = getPatch(i, j);   // <WolfViewer 2026-09-23/> made now if this is its first data


        decode_patch(bitpack, patch);
        {
            // <FS:Wolf> A coarse grid takes every step-th wire sample (see the note above);
            // at the normal resolution step is 1 and this is a straight copy.
            F32 scratch[LARGE_PATCH_SIZE * LARGE_PATCH_SIZE];
            decompress_patch(scratch, patch, &ph);

            const S32 step = (grids_per_patch > 0 && grids_per_patch < wire_samples) ? (wire_samples / grids_per_patch) : 1;
            const S32 copy = llmin(grids_per_patch, wire_samples);
            const S32 patch_stride = (S32)patchp->getDataStride();
            F32* dst = patchp->getDataZ();
            for (S32 jj = 0; jj < copy; jj++)
            {
                for (S32 ii = 0; ii < copy; ii++)
                {
                    dst[ii + jj * patch_stride] = scratch[ii * step + jj * step * wire_samples];
                }
            }
        }
        // </FS:Wolf>
        // <WolfViewer 2026-09-10> A height off the wire that is not a number is not terrain.
        // The camera floor is LLWorld::resolveLandHeightGlobal (llagentcamera.cpp ~2200): one
        // +inf height under the camera lifts it to infinity — the "Non Finite mOrigin" warnings
        // in Paul's Dire Wolf sessions. Replace any non-finite sample with the water level and
        // say so, a few times per region, naming the patch, so a bad sim patch is visible.
        {
            F32* dst = patchp->getDataZ();
            const S32 stride = (S32)patchp->getDataStride();   // <WolfViewer 2026-09-23/>
            S32 bad = 0;
            const F32 fill = mRegionp ? mRegionp->getWaterHeight() : 0.f;
            for (S32 jj = 0; jj < grids_per_patch; jj++)
            {
                for (S32 ii = 0; ii < grids_per_patch; ii++)
                {
                    F32& z = dst[ii + jj * stride];
                    if (!llfinite(z)) { z = fill; ++bad; }
                }
            }
            if (bad && mWolfBadHeightWarnings < 5)
            {
                ++mWolfBadHeightWarnings;
                LL_WARNS("Terrain") << "terrain " << (mRegionp ? mRegionp->getName() : std::string("?")) << " patch " << i << "," << j
                                    << ": " << bad << " non-finite height(s) from the sim (dc_offset " << ph.dc_offset << " range " << (S32)ph.range
                                    << " quant_wbits " << (S32)ph.quant_wbits << "), replaced with the water level" << LL_ENDL;
            }
        }
        // </WolfViewer>

        // Update edges for neighbors.  Need to guarantee that this gets done before we generate vertical stats.
        patchp->updateNorthEdge();
        patchp->updateEastEdge();
        if (patchp->getNeighborPatch(WEST))
        {
            patchp->getNeighborPatch(WEST)->updateEastEdge();
        }
        if (patchp->getNeighborPatch(SOUTHWEST))
        {
            patchp->getNeighborPatch(SOUTHWEST)->updateEastEdge();
            patchp->getNeighborPatch(SOUTHWEST)->updateNorthEdge();
        }
        if (patchp->getNeighborPatch(SOUTH))
        {
            patchp->getNeighborPatch(SOUTH)->updateNorthEdge();
        }

        // Dirty patch statistics, and flag that the patch has data.
        patchp->dirtyZ();
        const bool was_new = !patchp->getHasReceivedData();   // <FS:Wolf/>
        patchp->setHasReceivedData();
        if (was_new)                                          // <FS:Wolf/>
        {
            noteTerrainDataArrived(patchp->getCenterRegion());
        }
    }
}


// Retrurns true if "position" is within the bounds of surface.
// "position" is region-local
bool LLSurface::containsPosition(const LLVector3 &position)
{
    if (position.mV[VX] < 0.0f  ||  position.mV[VX] > mMetersPerEdge ||
        position.mV[VY] < 0.0f  ||  position.mV[VY] > mMetersPerEdge)
    {
        return false;
    }
    return true;
}


F32 LLSurface::resolveHeightRegion(const F32 x, const F32 y) const
{
    F32 height = 0.0f;
    F32 oometerspergrid = 1.f/mMetersPerGrid;

    // Check to see if v is actually above surface
    // We use (mGridsPerEdge-1) below rather than (mGridsPerEdge)
    // becuase of the east and north buffers

    if (x >= 0.f  &&
        x <= mMetersPerEdge  &&
        y >= 0.f  &&
        y <= mMetersPerEdge)
    {
        const S32 left   = llfloor(x * oometerspergrid);
        const S32 bottom = llfloor(y * oometerspergrid);

        // Don't walk off the edge of the array!
        const S32 right  = ( left+1   < (S32)mGridsPerEdge-1 ? left+1   : left );
        const S32 top    = ( bottom+1 < (S32)mGridsPerEdge-1 ? bottom+1 : bottom );

        // Figure out if v is in first or second triangle of the square
        // and calculate the slopes accordingly
        //    |       |
        // -(i,j+1)---(i+1,j+1)--
        //    |  1   /  |          ^
        //    |    /  2 |          |
        //    |  /      |          j
        // --(i,j)----(i+1,j)--
        //    |       |
        //
        //      i ->
        // where N = mGridsPerEdge

        const F32 left_bottom  = getZ( left,  bottom );
        const F32 right_bottom = getZ( right, bottom );
        const F32 left_top     = getZ( left,  top );
        const F32 right_top    = getZ( right, top );

        // dx and dy are incremental steps from (mSurface + k)
        F32 dx = x - left   * mMetersPerGrid;
        F32 dy = y - bottom * mMetersPerGrid;

        if (dy > dx)
        {
            // triangle 1
            dy *= left_top  - left_bottom;
            dx *= right_top - left_top;
        }
        else
        {
            // triangle 2
            dx *= right_bottom - left_bottom;
            dy *= right_top    - right_bottom;
        }
        height = left_bottom + (dx + dy) * oometerspergrid;
    }
    return height;
}


F32 LLSurface::resolveHeightGlobal(const LLVector3d& v) const
{
    if (!mRegionp)
    {
        return 0.f;
    }

    LLVector3 pos_region = mRegionp->getPosRegionFromGlobal(v);

    return resolveHeightRegion(pos_region);
}


LLVector3 LLSurface::resolveNormalGlobal(const LLVector3d& pos_global) const
{
    if (mGridsPerEdge <= 0)   // <WolfViewer 2026-09-23/> was !mSurfaceZ
    {
        // Hmm.  Uninitialized surface!
        return LLVector3::z_axis;
    }
    //
    // Returns the vector normal to a surface at location specified by vector v
    //
    F32 oometerspergrid = 1.f/mMetersPerGrid;
    LLVector3 normal;
    F32 dzx, dzy;

    if (pos_global.mdV[VX] >= mOriginGlobal.mdV[VX]  &&
        pos_global.mdV[VX] < mOriginGlobal.mdV[VX] + mMetersPerEdge  &&
        pos_global.mdV[VY] >= mOriginGlobal.mdV[VY]  &&
        pos_global.mdV[VY] < mOriginGlobal.mdV[VY] + mMetersPerEdge)
    {
        U32 i, j;
        F32 dx, dy;
        i = (U32) ((pos_global.mdV[VX] - mOriginGlobal.mdV[VX]) * oometerspergrid);
        j = (U32) ((pos_global.mdV[VY] - mOriginGlobal.mdV[VY]) * oometerspergrid );
        // <WolfViewer 2026-09-23> k, k+1, k+N, k+1+N below were offsets into the region-wide
        // array; they are the points (i,j), (i+1,j), (i,j+1), (i+1,j+1), read through getZ.
        const F32 z_k = getZ(i, j), z_k1 = getZ(i + 1, j), z_kN = getZ(i, j + 1), z_k1N = getZ(i + 1, j + 1);

        // Figure out if v is in first or second triangle of the square
        // and calculate the slopes accordingly
        //    |       |
        // -(k+N)---(k+1+N)--
        //    |  1 /  |          ^
        //    |   / 2 |          |
        //    |  /    |          j
        // --(k)----(k+1)--
        //    |       |
        //
        //      i ->
        // where N = mGridsPerEdge

        // dx and dy are incremental steps from (mSurface + k)
        dx = (F32)(pos_global.mdV[VX] - i*mMetersPerGrid - mOriginGlobal.mdV[VX]);
        dy = (F32)(pos_global.mdV[VY] - j*mMetersPerGrid - mOriginGlobal.mdV[VY]);
        if (dy > dx)
        {  // triangle 1
            dzx = z_k1N - z_kN;
            dzy = z_k - z_kN;
            normal.setVec(-dzx,dzy,1);
        }
        else
        {   // triangle 2
            dzx = z_k - z_k1;
            dzy = z_k1N - z_k1;
            normal.setVec(dzx,-dzy,1);
        }
    }
    normal.normVec();
    return normal;


}

LLSurfacePatch *LLSurface::resolvePatchRegion(const F32 x, const F32 y) const
{
// x and y should be region-local coordinates.
// If x and y are outside of the surface, then the returned
// index will be for the nearest boundary patch.
//
// 12      | 13| 14|       15
//         |   |   |
//     +---+---+---+---+
//     | 12| 13| 14| 15|
// ----+---+---+---+---+-----
// 8   | 8 | 9 | 10| 11|   11
// ----+---+---+---+---+-----
// 4   | 4 | 5 | 6 | 7 |    7
// ----+---+---+---+---+-----
//     | 0 | 1 | 2 | 3 |
//     +---+---+---+---+
//         |   |   |
// 0       | 1 | 2 |        3
//

// When x and y are not region-local do the following first

    S32 i, j;
    if (x < 0.0f)
    {
        i = 0;
    }
    else if (x >= mMetersPerEdge)
    {
        i = mPatchesPerEdge - 1;
    }
    else
    {
        i = (U32) (x / (mMetersPerGrid * mGridsPerPatchEdge));
    }

    if (y < 0.0f)
    {
        j = 0;
    }
    else if (y >= mMetersPerEdge)
    {
        j = mPatchesPerEdge - 1;
    }
    else
    {
        j = (U32) (y / (mMetersPerGrid * mGridsPerPatchEdge));
    }

    // *NOTE: Super paranoia code follows.
    // <WolfViewer 2026-09-23> i + j * mPatchesPerEdge overflowed an S32 past 741,440 m. i and j
    // are each clamped into the surface above, so the patch is simply (i, j).
    if (0 == mNumberOfPatches)
    {
        LL_WARNS() << "No patches for current region!" << LL_ENDL;
        return nullptr;
    }
    return getPatch(llclamp(i, 0, mPatchesPerEdge - 1), llclamp(j, 0, mPatchesPerEdge - 1));
    // </WolfViewer>
}


LLSurfacePatch *LLSurface::resolvePatchRegion(const LLVector3 &pos_region) const
{
    return resolvePatchRegion(pos_region.mV[VX], pos_region.mV[VY]);
}


LLSurfacePatch *LLSurface::resolvePatchGlobal(const LLVector3d &pos_global) const
{
    llassert(mRegionp);
    LLVector3 pos_region = mRegionp->getPosRegionFromGlobal(pos_global);
    return resolvePatchRegion(pos_region);
}


std::ostream& operator<<(std::ostream &s, const LLSurface &S)
{
    s << "{ \n";
    s << "  mGridsPerEdge = " << S.mGridsPerEdge - 1 << " + 1\n";
    s << "  mGridsPerPatchEdge = " << S.mGridsPerPatchEdge << "\n";
    s << "  mPatchesPerEdge = " << S.mPatchesPerEdge << "\n";
    s << "  mOriginGlobal = " << S.mOriginGlobal << "\n";
    s << "  mMetersPerGrid = " << S.mMetersPerGrid << "\n";
    s << "  mVisiblePatchCount = " << S.mVisiblePatchCount << "\n";
    s << "}";
    return s;
}


// ---------------- LLSurface:: Protected ----------------

void LLSurface::createPatchData()
{
    // Assumes mGridsPerEdge, mGridsPerPatchEdge, and mPatchesPerEdge have been properly set
    // <WolfViewer 2026-09-23> Only the page directory: patches are made by createPatch when first
    // needed. upstream built every one here (and linked it to its eight neighbours).
    destroyPatchData();
    mPatchPagesPerEdge = (mPatchesPerEdge + PATCH_PAGE_EDGE - 1) / PATCH_PAGE_EDGE;
    mPatchPages.clear();
    mPatchPages.resize((size_t)mPatchPagesPerEdge * mPatchPagesPerEdge);

    // One of each for each camera
    mVisiblePatchCount = 0;
    // </WolfViewer>
}

// <WolfViewer 2026-09-23> See llsurface.h.
LLSurfacePatch *LLSurface::createPatch(const S32 x, const S32 y)
{
    auto& page = mPatchPages[(size_t)(y / PATCH_PAGE_EDGE) * mPatchPagesPerEdge + (x / PATCH_PAGE_EDGE)];
    if (!page)
    {
        page.reset(new LLSurfacePatch*[PATCH_PAGE_EDGE * PATCH_PAGE_EDGE]());
    }
    LLSurfacePatch*& slot = page[(y % PATCH_PAGE_EDGE) * PATCH_PAGE_EDGE + (x % PATCH_PAGE_EDGE)];
    if (slot)
    {
        return slot;
    }

    LLSurfacePatch* patchp = new LLSurfacePatch();
    patchp->setSurface(this);
    patchp->initData(x, y);
    patchp->mHasReceivedData = false;
    patchp->mSTexUpdate = true;

    // createPatchData's origin, with Y taken from the Y origin (it used mOriginGlobal.mdV[0] for
    // both and relied on the later setOriginGlobal pass; a patch made later gets no such pass).
    LLVector3d origin_global;
    origin_global.mdV[0] = mOriginGlobal.mdV[0] + x * mMetersPerGrid * mGridsPerPatchEdge;
    origin_global.mdV[1] = mOriginGlobal.mdV[1] + y * mMetersPerGrid * mGridsPerPatchEdge;
    origin_global.mdV[2] = 0.f;
    patchp->setOriginGlobal(origin_global);

    slot = patchp;
    mAllPatches.push_back(patchp);
    linkPatch(patchp, x, y);
    return patchp;
}
// </WolfViewer>


// <WolfViewer 2026-09-26> see llsurface.h
LLVOSurfacePatch* LLSurface::getBlockObject(S32 block_i, S32 block_j, bool create)
{
    const U64 key = ((U64)(U32)block_j << 32) | (U32)block_i;
    auto it = mBlockObjects.find(key);
    if (it != mBlockObjects.end())
    {
        return it->second;
    }
    if (!create || !mRegionp)
    {
        return NULL;
    }
    LLVOSurfacePatch* objectp = (LLVOSurfacePatch*)gObjectList.createObjectViewer(LLViewerObject::LL_VO_SURFACE_PATCH, mRegionp);
    if (!objectp)
    {
        return NULL;
    }
    objectp->setBlock(this, block_i, block_j);
    mBlockObjects[key] = objectp;
    gPipeline.createObject(objectp);
    mBuiltPatchObject = true;
    mDiagObjectsBuilt++;
    return objectp;
}

void LLSurface::forgetBlockObject(S32 block_i, S32 block_j, const LLVOSurfacePatch* objectp)
{
    const U64 key = ((U64)(U32)block_j << 32) | (U32)block_i;
    auto it = mBlockObjects.find(key);
    if (it != mBlockObjects.end() && it->second == objectp)
    {
        mBlockObjects.erase(it);
    }
}
// </WolfViewer>

void LLSurface::destroyPatchData()
{
    // Delete all of the cached patch data for these patches.
    // <WolfViewer 2026-09-23/> each patch was allocated on its own (createPatch)
    for (LLSurfacePatch* patchp : mAllPatches)
    {
        delete patchp;
    }
    mAllPatches.clear();
    for (auto& page : mPatchPages)
    {
        page.reset();
    }
    mVisiblePatchCount = 0;
}


void LLSurface::setTextureSize(const S32 texture_size)
{
    sTextureSize = texture_size;
}


U32 LLSurface::getRenderLevel(const U32 render_stride) const
{
    return mPVArray.mRenderLevelp[render_stride];
}


U32 LLSurface::getRenderStride(const U32 render_level) const
{
    return mPVArray.mRenderStridep[render_level];
}


LLSurfacePatch *LLSurface::getPatch(const S32 x, const S32 y) const
{
    if ((x < 0) || (x >= mPatchesPerEdge))
    {
        LL_ERRS() << "Asking for patch out of bounds" << LL_ENDL;
        return nullptr;
    }
    if ((y < 0) || (y >= mPatchesPerEdge))
    {
        LL_ERRS() << "Asking for patch out of bounds" << LL_ENDL;
        return nullptr;
    }

    // <WolfViewer 2026-09-23/> made on first use; every patch used to exist from create().
    if (LLSurfacePatch* patchp = findPatch(x, y))
    {
        return patchp;
    }
    return const_cast<LLSurface*>(this)->createPatch(x, y);
}

// <WolfViewer 2026-09-23> The patch at (x, y) if it has been made, else null - never makes one.
LLSurfacePatch *LLSurface::findPatch(const S32 x, const S32 y) const
{
    if (x < 0 || y < 0 || x >= mPatchesPerEdge || y >= mPatchesPerEdge || mPatchPages.empty())
    {
        return nullptr;
    }
    const auto& page = mPatchPages[(size_t)(y / PATCH_PAGE_EDGE) * mPatchPagesPerEdge + (x / PATCH_PAGE_EDGE)];
    if (!page)
    {
        return nullptr;
    }
    return page[(y % PATCH_PAGE_EDGE) * PATCH_PAGE_EDGE + (x % PATCH_PAGE_EDGE)];
}
// </WolfViewer>


void LLSurface::dirtyAllPatches()
{
    // <WolfViewer 2026-09-23/> the patches that exist; one made later starts dirty anyway.
    for (LLSurfacePatch* patchp : mAllPatches)
    {
        patchp->dirtyZ();
    }
}

void LLSurface::dirtySurfacePatch(LLSurfacePatch *patchp)
{
    // Put surface patch on dirty surface patch list
    mDirtyPatchList.insert(patchp);
}


void LLSurface::setWaterHeight(F32 height)
{
    if (!mWaterObjp.isNull())
    {
        LLVector3 water_pos_region = mWaterObjp->getPositionRegion();
        bool changed = water_pos_region.mV[VZ] != height;
        water_pos_region.mV[VZ] = height;
        mWaterObjp->setPositionRegion(water_pos_region);
        if (changed)
        {
            LLWorld::getInstance()->updateWaterObjects();
        }
    }
    else
    {
        LL_WARNS() << "LLSurface::setWaterHeight with no water object!" << LL_ENDL;
    }
}

F32 LLSurface::getWaterHeight() const
{
    if (!mWaterObjp.isNull())
    {
        // we have a water object, the usual case
        return mWaterObjp->getPositionRegion().mV[VZ];
    }
    else
    {
        return DEFAULT_WATER_HEIGHT;
    }
}

