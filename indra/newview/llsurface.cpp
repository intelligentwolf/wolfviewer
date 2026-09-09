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

#include "llsurface.h"

#include "llpatchvertexarray.h"
#include "patch_dct.h"
#include "patch_code.h"
#include "llbitpack.h"
#include "llviewerobjectlist.h"
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
    // Surface data
    mSurfaceZ = nullptr;
    mNorm = nullptr;

    // Patch data
    mPatchList = nullptr;

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
    // <FS:Wolf/> calloc'd in create(), so free() — not delete[].
    free(mSurfaceZ);
    mSurfaceZ = nullptr;

    free(mNorm);
    mNorm = nullptr;

    mGridsPerEdge = 0;
    mGridsPerPatchEdge = 0;
    mPatchesPerEdge = 0;
    mNumberOfPatches = 0;
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
    mNumberOfPatches = mPatchesPerEdge * mPatchesPerEdge;
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

    S32 number_of_grids = mGridsPerEdge * mGridsPerEdge;

    /////////////////////////////////////
    //
    // Initialize data arrays for surface
    ///
    // <FS:Wolf> Reserve the surface, do not COMMIT it.
    //
    // These two arrays are 16 bytes per square metre of region: 1 MB for a standard 256 m
    // region, but 10.5 GB for a 25600 m varregion. The allocation itself was never the problem
    // — a block that large comes from mmap and its pages cost nothing until touched. The problem
    // was that we touched every one of them: `new LLVector3[n]` runs LLVector3's constructor,
    // which calls clear(), and the loop below wrote both arrays end to end. Between them they
    // faulted in all 10.5 GB before a single terrain packet had arrived.
    //
    // calloc hands back the same mmap'd, already-zero pages without walking them, so the
    // resident cost is now proportional to the terrain actually visited, not to the region's
    // area. A patch that is never approached costs nothing.
    //
    // The one behavioural difference is the normal default: it was (0,0,1), it is now (0,0,0).
    // That is only ever read through LLSurfacePatch::mDataNorm, and only for a patch that has
    // received data — such a patch is marked dirty, and LLSurface::idleUpdate runs
    // updateNormals() over it, which rewrites every normal in the patch from the heights before
    // anything draws it. LLSurface::resolveNormalGlobal does not read these at all; it
    // differentiates the heights (:1109-1130).
    mSurfaceZ = (F32*)calloc((size_t)number_of_grids, sizeof(F32));
    mNorm = (LLVector3*)calloc((size_t)number_of_grids, sizeof(LLVector3));
    if (!mSurfaceZ || !mNorm)
    {
        LL_ERRS() << "Could not reserve terrain for a " << (S32)width << " m region ("
                  << (((U64)number_of_grids * (sizeof(F32) + sizeof(LLVector3))) >> 20)
                  << " MB)" << LL_ENDL;
    }
    // </FS:Wolf>


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
    LLSurfacePatch *patchp;
    S32 i, j;
    // Need to update the southwest corners of the patches
    for (j=0; j<mPatchesPerEdge; j++)
    {
        for (i=0; i<mPatchesPerEdge; i++)
        {
            patchp = getPatch(i, j);

            new_origin_global = patchp->getOriginGlobal();

            new_origin_global.mdV[0] = mOriginGlobal.mdV[0] + i * mMetersPerGrid * mGridsPerPatchEdge;
            new_origin_global.mdV[1] = mOriginGlobal.mdV[1] + j * mMetersPerGrid * mGridsPerPatchEdge;
            patchp->setOriginGlobal(new_origin_global);
        }
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
    S32 i;
    LLSurfacePatch *patchp, *neighbor_patchp;
// <FS:CR> Aurora Sim
    S32 neighborPatchesPerEdge = neighborp->mPatchesPerEdge;
// </FS:CR> Aurora Sim

    mNeighbors[direction] = neighborp;
    neighborp->mNeighbors[gDirOpposite[direction]] = this;

// <FS:CR> Aurora Sim
    S32 ppe[2];
    S32 own_offset[2] = {0, 0};
    S32 neighbor_offset[2] = {0, 0};
    U32 own_xpos, own_ypos, neighbor_xpos, neighbor_ypos;

    ppe[0] = (mPatchesPerEdge < neighborPatchesPerEdge) ? mPatchesPerEdge : neighborPatchesPerEdge; // used for x
    ppe[1] = ppe[0]; // used for y

    from_region_handle(mRegionp->getHandle(), &own_xpos, &own_ypos);
    from_region_handle(neighborp->getRegion()->getHandle(), &neighbor_xpos, &neighbor_ypos);

    if(own_ypos >= neighbor_ypos) {
        neighbor_offset[1] = (own_ypos - neighbor_ypos) / mGridsPerPatchEdge;
        ppe[1] = llmin(mPatchesPerEdge, neighborPatchesPerEdge-neighbor_offset[1]);
    }
    else {
        own_offset[1] = (neighbor_ypos - own_ypos) / mGridsPerPatchEdge;
        ppe[1] = llmin(mPatchesPerEdge-own_offset[1], neighborPatchesPerEdge);
    }

    if(own_xpos >= neighbor_xpos) {
        neighbor_offset[0] = (own_xpos - neighbor_xpos) / mGridsPerPatchEdge;
        ppe[0] = llmin(mPatchesPerEdge, neighborPatchesPerEdge-neighbor_offset[0]);
    }
    else {
        own_offset[0] = (neighbor_xpos - own_xpos) / mGridsPerPatchEdge;
        ppe[0] = llmin(mPatchesPerEdge-own_offset[0], neighborPatchesPerEdge);
    }
// <FS:CR> Aurora Sim

    // Connect patches
    if (NORTHEAST == direction)
    {
        patchp = getPatch(mPatchesPerEdge - 1, mPatchesPerEdge - 1);
// <FS:CR> Aurora Sim
        //neighbor_patchp = neighborp->getPatch(0, 0);
        neighbor_patchp = neighborp->getPatch(neighbor_offset[0], neighbor_offset[1]);
// </FS:CR> Aurora Sim

        patchp->connectNeighbor(neighbor_patchp, direction);
        neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

        patchp->updateNorthEdge(); // Only update one of north or east.
        patchp->dirtyZ();
    }
    else if (NORTHWEST == direction)
    {
// <FS:CR> Aurora Sim
        S32 off = mPatchesPerEdge + neighbor_offset[1] - own_offset[1];
// </FS:CR> Aurora Sim
        patchp = getPatch(0, mPatchesPerEdge - 1);
// <FS:CR> Aurora Sim
        //neighbor_patchp = neighborp->getPatch(mPatchesPerEdge - 1, 0);
        neighbor_patchp = neighborp->getPatch(neighbor_offset[0] - 1, off); //neighborPatchesPerEdge - 1
// </FS:CR> Aurora Sim

        patchp->connectNeighbor(neighbor_patchp, direction);
        neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);
    }
    else if (SOUTHWEST == direction)
    {
        patchp = getPatch(0, 0);
// <FS:CR> Aurora Sim
        //neighbor_patchp = neighborp->getPatch(mPatchesPerEdge - 1, mPatchesPerEdge - 1);
        neighbor_patchp = neighborp->getPatch(neighbor_offset[0] - 1, neighbor_offset[1] - 1);
// </FS:CR> Aurora Sim

        patchp->connectNeighbor(neighbor_patchp, direction);
        neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

// <FS:CR> Aurora Sim
        //neighbor_patchp->updateNorthEdge(); // Only update one of north or east.
        neighbor_patchp->updateEastEdge(); // Only update one of north or east.
// </FS:CR> Aurora Sim
        neighbor_patchp->dirtyZ();
    }
    else if (SOUTHEAST == direction)
    {
// <FS:CR> Aurora Sim
        S32 off = mPatchesPerEdge + neighbor_offset[0] - own_offset[0];
// </FS:CR> Aurora Sim

        patchp = getPatch(mPatchesPerEdge - 1, 0);
// <FS:CR> Aurora Sim
        //neighbor_patchp = neighborp->getPatch(0, mPatchesPerEdge - 1);
        neighbor_patchp = neighborp->getPatch(off, neighbor_offset[1] - 1); //0
// </FS:CR> Aurora Sim

        patchp->connectNeighbor(neighbor_patchp, direction);
        neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);
    }
    else if (EAST == direction)
    {
        // Do east/west connections, first
// <FS:CR> Aurora Sim
        //for (i = 0; i < (S32)mPatchesPerEdge; i++)
        for (i = 0; i < ppe[1]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(mPatchesPerEdge - 1, i);
            //neighbor_patchp = neighborp->getPatch(0, i);
            patchp = getPatch(mPatchesPerEdge - 1, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(0, i + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, direction);
            neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

            patchp->updateEastEdge();
            patchp->dirtyZ();
        }

        // Now do northeast/southwest connections
// <FS:CR> Aurora Sim
        //for (i = 0; i < (S32)mPatchesPerEdge - 1; i++)
        for (i = 0; i < ppe[1] - 1; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(mPatchesPerEdge - 1, i);
            //neighbor_patchp = neighborp->getPatch(0, i+1);
            patchp = getPatch(mPatchesPerEdge - 1, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(0, i+1 + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, NORTHEAST);
            neighbor_patchp->connectNeighbor(patchp, SOUTHWEST);
        }
        // Now do southeast/northwest connections
// <FS:CR> Aurora Sim
        //for (i = 1; i < (S32)mPatchesPerEdge; i++)
        for (i = 1; i < ppe[1]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(mPatchesPerEdge - 1, i);
            //neighbor_patchp = neighborp->getPatch(0, i-1);
            patchp = getPatch(mPatchesPerEdge - 1, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(0, i-1 + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, SOUTHEAST);
            neighbor_patchp->connectNeighbor(patchp, NORTHWEST);
        }
    }
    else if (NORTH == direction)
    {
        // Do north/south connections, first
// <FS:CR> Aurora Sim
        //for (i = 0; i < (S32)mPatchesPerEdge; i++)
        for (i = 0; i < ppe[0]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, mPatchesPerEdge - 1);
            //neighbor_patchp = neighborp->getPatch(i, 0);
            patchp = getPatch(i + own_offset[0], mPatchesPerEdge - 1);
            neighbor_patchp = neighborp->getPatch(i + neighbor_offset[0], 0);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, direction);
            neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

            patchp->updateNorthEdge();
            patchp->dirtyZ();
        }

        // Do northeast/southwest connections
// <FS:CR> Aurora Sim
        //for (i = 0; i < (S32)mPatchesPerEdge - 1; i++)
        for (i = 0; i < ppe[0] - 1; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, mPatchesPerEdge - 1);
            //neighbor_patchp = neighborp->getPatch(i+1, 0);
            patchp = getPatch(i + own_offset[0], mPatchesPerEdge - 1);
            neighbor_patchp = neighborp->getPatch(i+1 + neighbor_offset[0], 0);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, NORTHEAST);
            neighbor_patchp->connectNeighbor(patchp, SOUTHWEST);
        }
        // Do southeast/northwest connections
// <FS:CR> Aurora Sim
        //for (i = 1; i < (S32)mPatchesPerEdge; i++)
        for (i = 1; i < ppe[0]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, mPatchesPerEdge - 1);
            //neighbor_patchp = neighborp->getPatch(i-1, 0);
            patchp = getPatch(i + own_offset[0], mPatchesPerEdge - 1);
            neighbor_patchp = neighborp->getPatch(i-1 + neighbor_offset[0], 0);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, NORTHWEST);
            neighbor_patchp->connectNeighbor(patchp, SOUTHEAST);
        }
    }
    else if (WEST == direction)
    {
        // Do east/west connections, first
// <FS:CR> Aurora Sim
        //for (i = 0; i < mPatchesPerEdge; i++)
        for (i = 0; i < ppe[1]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(0, i);
            //neighbor_patchp = neighborp->getPatch(mPatchesPerEdge - 1, i);
            patchp = getPatch(0, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(neighborPatchesPerEdge - 1, i + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, direction);
            neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

            neighbor_patchp->updateEastEdge();
            neighbor_patchp->dirtyZ();
        }

        // Now do northeast/southwest connections
// <FS:CR> Aurora Sim
        //for (i = 1; i < mPatchesPerEdge; i++)
        for (i = 1; i < ppe[1]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(0, i);
            //neighbor_patchp = neighborp->getPatch(mPatchesPerEdge - 1, i - 1);
            patchp = getPatch(0, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(neighborPatchesPerEdge - 1, i - 1 + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, SOUTHWEST);
            neighbor_patchp->connectNeighbor(patchp, NORTHEAST);
        }

        // Now do northwest/southeast connections
// <FS:CR> Aurora Sim
        //for (i = 0; i < mPatchesPerEdge - 1; i++)
        for (i = 0; i < ppe[1] - 1; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(0, i);
            //neighbor_patchp = neighborp->getPatch(mPatchesPerEdge - 1, i + 1);
            patchp = getPatch(0, i + own_offset[1]);
            neighbor_patchp = neighborp->getPatch(neighborPatchesPerEdge - 1, i + 1 + neighbor_offset[1]);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, NORTHWEST);
            neighbor_patchp->connectNeighbor(patchp, SOUTHEAST);
        }
    }
    else if (SOUTH == direction)
    {
        // Do north/south connections, first
// <FS:CR> Aurora Sim
        //for (i = 0; i < mPatchesPerEdge; i++)
        for (i = 0; i < ppe[0]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, 0);
            //neighbor_patchp = neighborp->getPatch(i, mPatchesPerEdge - 1);
            patchp = getPatch(i + own_offset[0], 0);
            neighbor_patchp = neighborp->getPatch(i + neighbor_offset[0], neighborPatchesPerEdge - 1);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, direction);
            neighbor_patchp->connectNeighbor(patchp, gDirOpposite[direction]);

            neighbor_patchp->updateNorthEdge();
            neighbor_patchp->dirtyZ();
        }

        // Now do northeast/southwest connections
// <FS:CR> Aurora Sim
        //for (i = 1; i < mPatchesPerEdge; i++)
        for (i = 1; i < ppe[0]; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, 0);
            //neighbor_patchp = neighborp->getPatch(i - 1, mPatchesPerEdge - 1);
            patchp = getPatch(i + own_offset[0], 0);
            neighbor_patchp = neighborp->getPatch(i - 1 + neighbor_offset[0], neighborPatchesPerEdge - 1);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, SOUTHWEST);
            neighbor_patchp->connectNeighbor(patchp, NORTHEAST);
        }
        // Now do northeast/southwest connections
// <FS:CR> Aurora Sim
        //for (i = 0; i < mPatchesPerEdge - 1; i++)
        for (i = 0; i < ppe[0] - 1; i++)
// </FS:CR> Aurora Sim
        {
// <FS:CR> Aurora Sim
            //patchp = getPatch(i, 0);
            //neighbor_patchp = neighborp->getPatch(i + 1, mPatchesPerEdge - 1);
            patchp = getPatch(i + own_offset[0], 0);
            neighbor_patchp = neighborp->getPatch(i + 1 + neighbor_offset[0], neighborPatchesPerEdge - 1);
// </FS:CR> Aurora Sim

            patchp->connectNeighbor(neighbor_patchp, SOUTHEAST);
            neighbor_patchp->connectNeighbor(patchp, NORTHWEST);
        }
    }
}

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
    for (i = 0; i < mNumberOfPatches; i++)
    {
        (mPatchList + i)->disconnectNeighbor(surfacep);
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
    mSurfaceZ[x + y*mGridsPerEdge] += delta;
}


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
        for (S32 i=0; i<mNumberOfPatches; i++)
        {
            patchp = mPatchList + i;

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
                (mPatchList + (j * mPatchesPerEdge + i))->updateVisibility();
            }
        }
    }

    // Pass 2: the patches in range now. This is the only pass that counts.
    mVisiblePatchCount = 0;
    for (S32 j = min_j; j <= max_j; j++)
    {
        for (S32 i = min_i; i <= max_i; i++)
        {
            patchp = mPatchList + (j * mPatchesPerEdge + i);
            patchp->updateVisibility();
            if (patchp->getVisible())
            {
                mVisiblePatchCount++;
                patchp->updateCameraDistanceRegion(pos_region);
            }
        }
    }

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
    // decompressor writes patch_size x patch_size values at whatever stride it is handed, so on
    // a coarse grid it must NOT be pointed straight at the surface: it would overrun the patch
    // into its neighbours. Decompress into a scratch block at full resolution instead and take
    // every step-th sample. The step is always a whole number because the resolution is only
    // ever halved.
    const S32 wire_samples = gopp->patch_size;
    const bool downsample = (mGridsPerPatchEdge < wire_samples);
    gopp->stride = downsample ? wire_samples : mGridsPerEdge;
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

        patchp = &mPatchList[j*mPatchesPerEdge + i];


        decode_patch(bitpack, patch);
        // <FS:Wolf> see the note above set_group_of_patch_header
        if (downsample)
        {
            F32 scratch[LARGE_PATCH_SIZE * LARGE_PATCH_SIZE];
            decompress_patch(scratch, patch, &ph);

            const S32 step = wire_samples / mGridsPerPatchEdge;
            F32* dst = patchp->getDataZ();
            for (S32 jj = 0; jj < mGridsPerPatchEdge; jj++)
            {
                for (S32 ii = 0; ii < mGridsPerPatchEdge; ii++)
                {
                    dst[ii + jj * mGridsPerEdge] = scratch[ii * step + jj * step * wire_samples];
                }
            }
        }
        else
        {
            decompress_patch(patchp->getDataZ(), patch, &ph);
        }
        // </FS:Wolf>

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
    if (!mSurfaceZ)
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
        U32 i, j, k;
        F32 dx, dy;
        i = (U32) ((pos_global.mdV[VX] - mOriginGlobal.mdV[VX]) * oometerspergrid);
        j = (U32) ((pos_global.mdV[VY] - mOriginGlobal.mdV[VY]) * oometerspergrid );
        k = i + j*mGridsPerEdge;

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
            dzx = *(mSurfaceZ + k + 1 + mGridsPerEdge) - *(mSurfaceZ + k + mGridsPerEdge);
            dzy = *(mSurfaceZ + k) - *(mSurfaceZ + k + mGridsPerEdge);
            normal.setVec(-dzx,dzy,1);
        }
        else
        {   // triangle 2
            dzx = *(mSurfaceZ + k) - *(mSurfaceZ + k + 1);
            dzy = *(mSurfaceZ + k + 1 + mGridsPerEdge) - *(mSurfaceZ + k + 1);
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
    S32 index = i + j * mPatchesPerEdge;
    if((index < 0) || (index >= mNumberOfPatches))
    {
        if(0 == mNumberOfPatches)
        {
            LL_WARNS() << "No patches for current region!" << LL_ENDL;
            return nullptr;
        }
        S32 old_index = index;
        index = llclamp(old_index, 0, (mNumberOfPatches - 1));
        LL_WARNS() << "Clamping out of range patch index " << old_index
                << " to " << index << LL_ENDL;
    }
    return &(mPatchList[index]);
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
    // TODO -- check for create() called when surface is not empty
    S32 i, j;
    LLSurfacePatch *patchp;

    // Allocate memory
    mPatchList = new LLSurfacePatch[mNumberOfPatches];

    // One of each for each camera
    mVisiblePatchCount = mNumberOfPatches;

    for (j=0; j<mPatchesPerEdge; j++)
    {
        for (i=0; i<mPatchesPerEdge; i++)
        {
            patchp = getPatch(i, j);
            patchp->setSurface(this);
        }
    }

    for (j=0; j<mPatchesPerEdge; j++)
    {
        for (i=0; i<mPatchesPerEdge; i++)
        {
            patchp = getPatch(i, j);
            patchp->mHasReceivedData = false;
            patchp->mSTexUpdate = true;

            S32 data_offset = i * mGridsPerPatchEdge + j * mGridsPerPatchEdge * mGridsPerEdge;

            patchp->setDataZ(mSurfaceZ + data_offset);
            patchp->setDataNorm(mNorm + data_offset);


            // We make each patch point to its neighbors so we can do resolution checking
            // when butting up different resolutions.  Patches that don't have neighbors
            // somewhere will point to NULL on that side.
            if (i < mPatchesPerEdge-1)
            {
                patchp->setNeighborPatch(EAST,getPatch(i+1, j));
            }
            else
            {
                patchp->setNeighborPatch(EAST, nullptr);
            }

            if (j < mPatchesPerEdge-1)
            {
                patchp->setNeighborPatch(NORTH, getPatch(i, j+1));
            }
            else
            {
                patchp->setNeighborPatch(NORTH, nullptr);
            }

            if (i > 0)
            {
                patchp->setNeighborPatch(WEST, getPatch(i - 1, j));
            }
            else
            {
                patchp->setNeighborPatch(WEST, nullptr);
            }

            if (j > 0)
            {
                patchp->setNeighborPatch(SOUTH, getPatch(i, j-1));
            }
            else
            {
                patchp->setNeighborPatch(SOUTH, nullptr);
            }

            if (i < (mPatchesPerEdge-1)  &&  j < (mPatchesPerEdge-1))
            {
                patchp->setNeighborPatch(NORTHEAST, getPatch(i + 1, j + 1));
            }
            else
            {
                patchp->setNeighborPatch(NORTHEAST, nullptr);
            }

            if (i > 0  &&  j < (mPatchesPerEdge-1))
            {
                patchp->setNeighborPatch(NORTHWEST, getPatch(i - 1, j + 1));
            }
            else
            {
                patchp->setNeighborPatch(NORTHWEST, nullptr);
            }

            if (i > 0  &&  j > 0)
            {
                patchp->setNeighborPatch(SOUTHWEST, getPatch(i - 1, j - 1));
            }
            else
            {
                patchp->setNeighborPatch(SOUTHWEST, nullptr);
            }

            if (i < (mPatchesPerEdge-1)  &&  j > 0)
            {
                patchp->setNeighborPatch(SOUTHEAST, getPatch(i + 1, j - 1));
            }
            else
            {
                patchp->setNeighborPatch(SOUTHEAST, nullptr);
            }

            LLVector3d origin_global;
            origin_global.mdV[0] = mOriginGlobal.mdV[0] + i * mMetersPerGrid * mGridsPerPatchEdge;
            origin_global.mdV[1] = mOriginGlobal.mdV[0] + j * mMetersPerGrid * mGridsPerPatchEdge;
            origin_global.mdV[2] = 0.f;
            patchp->setOriginGlobal(origin_global);
        }
    }
}


void LLSurface::destroyPatchData()
{
    // Delete all of the cached patch data for these patches.

    delete [] mPatchList;
    mPatchList = nullptr;
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

    return mPatchList + x + y*mPatchesPerEdge;
}


void LLSurface::dirtyAllPatches()
{
    S32 i;
    for (i = 0; i < mNumberOfPatches; i++)
    {
        mPatchList[i].dirtyZ();
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

