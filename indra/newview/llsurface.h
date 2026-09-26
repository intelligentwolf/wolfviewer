/**
 * @file llsurface.h
 * @brief Description of LLSurface class
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

#ifndef LL_LLSURFACE_H
#define LL_LLSURFACE_H

#include "llterraingridoffset.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include "v3math.h"
#include "v3dmath.h"

#include "lltimer.h"
#include "llvowater.h"
#include "llpatchvertexarray.h"
#include "llviewertexture.h"

class LLTimer;
class LLUUID;
class LLAgent;

static const U8 NO_EDGE    = 0x00;
static const U8 EAST_EDGE  = 0x01;
static const U8 NORTH_EDGE = 0x02;
static const U8 WEST_EDGE  = 0x04;
static const U8 SOUTH_EDGE = 0x08;

static const S32 ONE_MORE_THAN_NEIGHBOR = 1;
static const S32 EQUAL_TO_NEIGHBOR      = 0;
static const S32 ONE_LESS_THAN_NEIGHBOR = -1;

const S32 ABOVE_WATERLINE_ALPHA = 32;  // The alpha of water when the land elevation is above the waterline.

class LLViewerRegion;
class LLSurfacePatch;
class LLVOSurfacePatch;
class LLBitPack;
class LLGroupHeader;

class LLSurface
{
public:
    LLSurface(U32 type, LLViewerRegion *regionp = nullptr);
    virtual ~LLSurface();

    static void initClasses(); // Do class initialization for LLSurface and its child classes.

    void create(const S32 surface_grid_width,
                const S32 surface_patch_width,
                const LLVector3d &origin_global,
                const F32 width);   // Allocates and initializes surface

    void setRegion(LLViewerRegion *regionp);

    void setOriginGlobal(const LLVector3d &origin_global);

    void connectNeighbor(LLSurface *neighborp, U32 direction);
    void disconnectNeighbor(LLSurface *neighborp);
    void disconnectAllNeighbors();

// <FS:CR> Aurora Sim
    void rebuildWater();
// </FS:CR> Aurora Sim
    virtual void decompressDCTPatch(LLBitPack &bitpack, LLGroupHeader *gopp, bool b_large_patch);
    virtual void updatePatchVisibilities(LLAgent &agent);

    // <WolfViewer 2026-09-23> Height at surface grid point (i, j), 0..mGridsPerEdge-1 each way.
    // The surface no longer holds one region-sized array (16 bytes per square metre: 17 TB at
    // 1,048,576 m); each patch holds its own (grids_per_patch + 1)^2 heights, and this finds the
    // patch that owns the point. A point on ground no patch has been made for reads 0, as the
    // zero-filled array did.
    F32 getZ(const S32 i, const S32 j) const;

    LLVector3 getOriginAgent() const;
    const LLVector3d &getOriginGlobal() const;
    F32 getMetersPerGrid() const;
    S32 getGridsPerEdge() const;
    S32 getPatchesPerEdge() const;
    S32 getGridsPerPatchEdge() const;
    U32 getRenderStride(const U32 render_level) const;
    U32 getRenderLevel(const U32 render_stride) const;

    // Returns the height of the surface immediately above (or below) location,
    // or if location is not above surface returns zero.
    F32 resolveHeightRegion(const F32 x, const F32 y) const;
    F32 resolveHeightRegion(const LLVector3 &location) const
            { return resolveHeightRegion( location.mV[VX], location.mV[VY] ); }
    F32 resolveHeightGlobal(const LLVector3d &position_global) const;
    LLVector3 resolveNormalGlobal(const LLVector3d& v) const;               //  Returns normal to surface

    LLSurfacePatch *resolvePatchRegion(const F32 x, const F32 y) const;
    LLSurfacePatch *resolvePatchRegion(const LLVector3 &position_region) const;
    LLSurfacePatch *resolvePatchGlobal(const LLVector3d &position_global) const;
    // <WolfViewer 2026-09-23> Patches are created on first use (see createPatch). getPatch makes
    // the patch if it does not exist yet, exactly as every patch used to exist from the start;
    // findPatch only returns one that already does.
    LLSurfacePatch *getPatch(const S32 x, const S32 y) const;
    LLSurfacePatch *findPatch(const S32 x, const S32 y) const;
    // Every patch created so far, in creation order.
    const std::vector<LLSurfacePatch*>& getAllPatches() const  { return mAllPatches; }
    // <WolfViewer 2026-09-26> The terrain BLOCK objects (LLVOSurfacePatch, one per
    // WOLF_BLOCK_PATCHES x WOLF_BLOCK_PATCHES patches), by block index. getBlockObject makes one
    // on first use when create is set; the object calls forgetBlockObject when it dies.
    LLVOSurfacePatch* getBlockObject(S32 block_i, S32 block_j, bool create);
    void forgetBlockObject(S32 block_i, S32 block_j, const LLVOSurfacePatch* objectp);
    // Goes up whenever any patch's heights change (LLSurfacePatch::dirtyZ), so a whole-surface
    // "has the terrain changed" test no longer has to visit every patch.
    U64 getTerrainRevision() const                  { return mTerrainRevision; }
    // Would a patch at (x, y) - which may lie outside this surface - have a neighbour patch that
    // simply has not been made yet? True inside this surface and inside any connected neighbour.
    bool expectsPatchAt(const S32 x, const S32 y) const;

    // Update methods (called during idle, normally)
    template<bool PBR>
    bool idleUpdate(F32 max_update_time);

    bool containsPosition(const LLVector3 &position);

    void moveZ(const S32 x, const S32 y, const F32 delta);

    LLViewerRegion *getRegion() const               { return mRegionp; }

    F32 getMinZ() const                             { return mMinZ; }
    F32 getMaxZ() const                             { return mMaxZ; }

    void setWaterHeight(F32 height);
    F32 getWaterHeight() const;

    LLViewerTexture *getSTexture();

    bool hasZData() const                           { return mHasZData; }

    void dirtyAllPatches(); // Use this to dirty all patches when changing terrain parameters

    void dirtySurfacePatch(LLSurfacePatch *patchp);
    LLVOWater *getWaterObj()                        { return mWaterObjp; }

    static void setTextureSize(const S32 texture_size);

    friend class LLSurfacePatch;
    friend std::ostream& operator<<(std::ostream &s, const LLSurface &S);

    void getNeighboringRegions( std::vector<LLViewerRegion*>& uniqueRegions );
    void getNeighboringRegionsStatus( std::vector<S32>& regions );

public:
    // Number of grid points on one side of a region, including +1 buffer for
    // north and east edge.
    S32 mGridsPerEdge;

    F32 mOOGridsPerEdge;            // Inverse of grids per edge

    S32 mPatchesPerEdge;            // Number of patches on one side of a region
    // <WolfViewer 2026-09-23/> S64: 65536 x 65536 patches (a 1,048,576 m region) is 2^32, which
    // an S32 wrapped to exactly 0.
    S64 mNumberOfPatches;           // Total number of patches


    // Each surface points at 8 neighbors (or NULL)
    // +---+---+---+
    // |NW | N | NE|
    // +---+---+---+
    // | W | 0 | E |
    // +---+---+---+
    // |SW | S | SE|
    // +---+---+---+
    LLSurface *mNeighbors[8]={}; // Adjacent patches <FS:Beq/> ensure initialised.

    U32 mType;              // Useful for identifying derived classes

    F32 mDetailTextureScale;    //  Number of times to repeat detail texture across this surface

private:
    void createSTexture();
    void initTextures();

    void createPatchData();     // Allocates memory for patches.
    void destroyPatchData();    // Deallocates memory for patches.

    // <WolfViewer 2026-09-23> Make patch (x, y): its own height/normal block, its origin, and its
    // links to whichever of its eight neighbours exist - in this surface or a connected one.
    LLSurfacePatch *createPatch(const S32 x, const S32 y);
    // Link patchp to its existing neighbours (only those in only_surface, if given); cross-surface
    // links also refresh the shared edge the way connectNeighbor always did (see the note there).
    void linkPatch(LLSurfacePatch *patchp, const S32 x, const S32 y, const LLSurface *only_surface = nullptr);
    // The connected surface that holds the patch at global patch position (gx, gy) - patches
    // counted from the world origin - and that patch's index in it; null if none does.
    LLSurface *neighborSurfaceAt(const S64 gx, const S64 gy, S32 &nx, S32 &ny) const;
    S64 globalPatchX() const;
    S64 globalPatchY() const;

    LLVector3d  mOriginGlobal;      // In absolute frame

    // <WolfViewer 2026-09-23> Sparse patch directory, replacing the region-sized LLSurfacePatch
    // array (one object per 16 m patch: 4.3 billion at 1,048,576 m, and new[] of a count that
    // wrapped to 0). Pages of PATCH_PAGE_EDGE^2 patch pointers, a page made when any patch in it
    // is; patches are allocated one by one so their addresses never move - LLVOSurfacePatch,
    // grass, the dirty list and every neighbour link hold them.
    static constexpr S32 PATCH_PAGE_EDGE = 64;
    S32 mPatchPagesPerEdge = 0;
    std::vector<std::unique_ptr<LLSurfacePatch*[]>> mPatchPages;
    std::vector<LLSurfacePatch*> mAllPatches;
    U64 mTerrainRevision = 0;
    // <FS:Wolf/> The patch index box updatePatchVisibilities scanned last frame, so patches that
    // have just fallen out of range can be told they are no longer visible without rescanning
    // the whole region. -1 means "nothing scanned yet".
    S32 mLastScanMinI = -1, mLastScanMaxI = -1, mLastScanMinJ = -1, mLastScanMaxJ = -1;

    std::set<LLSurfacePatch *> mDirtyPatchList;


    // The textures should never be directly initialized - use the setter methods!
    LLPointer<LLViewerTexture> mSTexturep;      // Texture for surface

    LLPointer<LLVOWater>    mWaterObjp;

    // When we want multiple cameras we'll need one of each these for each camera
    S32 mVisiblePatchCount;

    U32         mGridsPerPatchEdge;         // Number of grid points on a side of a patch
    F32         mMetersPerGrid;             // Converts (i,j) indecies to distance
    F32         mMetersPerEdge;             // = mMetersPerGrid * (mGridsPerEdge-1)

    LLPatchVertexArray mPVArray;

    bool        mHasZData;

    S32  mWolfBadHeightWarnings = 0;   // <WolfViewer 2026-09-10> non-finite wire heights reported so far              // We've received any patch data for this surface.
    // <FS:Wolf/> Has any patch on this surface built its viewer object? Patch objects are made
    // on demand (LLSurfacePatch::ensureVObj) and the first one is what creates the terrain draw
    // pool, so a surface nobody ever looked at legitimately has no pool at destruction. Without
    // this the destructor warns about that every time.
    bool        mBuiltPatchObject = false;
    std::unordered_map<U64, LLVOSurfacePatch*> mBlockObjects;   // <WolfViewer 2026-09-26/> getBlockObject

public:
    // <FS:Wolf> Terrain diagnostics for very large varregions.
    //
    // A 25,600 m region is 1600 x 1600 = 2.56 M patches and the sim streams them one patch at a
    // time. When no terrain appears there are only a handful of possible reasons, and guessing
    // between them costs a test flight each time. These name the one that is actually happening.
    // Every counter is O(1) per event and the report is rate limited, so this is cheap enough to
    // leave in a shipping build; it only says anything on a region big enough to have a problem.
    void noteTerrainDataArrived(const LLVector3& patch_center_region);
    void reportTerrainDiagnostics();

    S32 mDiagPatchesWithData = 0;     // patches that have received terrain from the sim
    S32 mDiagObjectsBuilt = 0;        // ensureVObj() successes
    S32 mDiagTexNoVObj = 0;           // updateTexture: no viewer object, nothing to build into
    S32 mDiagTexWaitNeighbors = 0;    // updateTexture: a neighbour has no terrain data yet
    S32 mDiagTexWaitHeights = 0;      // updateTexture: generateHeights() not ready
    S32 mDiagTexWaitComposition = 0;  // updateTexture: generateComposition() not ready
    S32 mDiagTexBuilt = 0;            // updateTexture: geometry marked for rebuild
    S32 mDiagNormalsSkipped = 0;      // updateNormals: skipped, patch has no viewer object
    S32 mDiagDirtyListSize = 0;       // patches waiting in the dirty list at the last report
    bool mDiagScanRan = false;        // has updatePatchVisibilities run even once on this surface?
private:
    // Where the sim is sending, relative to the camera, since the last report. This is the
    // number that separates "the viewer is not drawing it" from "the sim has not sent it here".
    F32 mDiagNearestDataM = -1.f;
    F32 mDiagFarthestDataM = -1.f;
    S32 mDiagArrivedSinceReport = 0;
    LLTimer mDiagTimer;
    // </FS:Wolf>
    F32         mMinZ;                  // min z for this region (during the session)
    F32         mMaxZ;                  // max z for this region (during the session)

    S32         mSurfacePatchUpdateCount;                   // Number of frames since last update.

private:
    LLViewerRegion *mRegionp; // Patch whose coordinate system this surface is using.
    static S32  sTextureSize;               // Size of the surface texture
    LLTimer     mTimer; // timer to throttle initial requests until the mSTexture is fully fetched
};

extern template bool LLSurface::idleUpdate</*PBR=*/false>(F32 max_update_time);
extern template bool LLSurface::idleUpdate</*PBR=*/true>(F32 max_update_time);



//        .   __.
//     Z /|\   /| Y                                 North
//        |   /
//        |  /             |<----------------- mGridsPerSurfaceEdge --------------->|
//        | /              __________________________________________________________
//        |/______\ X     /_______________________________________________________  /
//                /      /      /      /      /      /      /      /M*M-2 /M*M-1 / /
//                      /______/______/______/______/______/______/______/______/ /
//                     /      /      /      /      /      /      /      /      / /
//                    /______/______/______/______/______/______/______/______/ /
//                   /      /      /      /      /      /      /      /      / /
//                  /______/______/______/______/______/______/______/______/ /
//      West       /      /      /      /      /      /      /      /      / /
//                /______/______/______/______/______/______/______/______/ /     East
//               /...   /      /      /      /      /      /      /      / /
//              /______/______/______/______/______/______/______/______/ /
//       _.    / 2M   /      /      /      /      /      /      /      / /
//       /|   /______/______/______/______/______/______/______/______/ /
//      /    / M    / M+1  / M+2  / ...  /      /      /      / 2M-1 / /
//     j    /______/______/______/______/______/______/______/______/ /
//         / 0    / 1    / 2    / ...  /      /      /      / M-1  / /
//        /______/______/______/______/______/______/______/______/_/
//                                South             |<-L->|
//             i -->
//
// where M = mSurfPatchWidth
// and L = mPatchGridWidth
//
// Notice that mGridsPerSurfaceEdge = a power of two + 1
// This provides a buffer on the east and north edges that will allow us to
// fill the cracks between adjacent surfaces when rendering.
#endif
