/**
 * @file llsurfacepatch.h
 * @brief LLSurfacePatch class definition
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLSURFACEPATCH_H
#define LL_LLSURFACEPATCH_H

#include "v3math.h"
#include "v3dmath.h"
#include "llpointer.h"
#include <memory>

class LLSurface;
class LLVOSurfacePatch;
class LLVector2;
class LLColor4U;
class LLAgent;

// A patch shouldn't know about its visibility since that really depends on the
// camera that is looking (or not looking) at it.  So, anything about a patch
// that is specific to a camera should be in the class below.
class LLPatchVisibilityInfo
{
public:
    LLPatchVisibilityInfo() :
        mbIsVisible(false),
        mDistance(0.f),
        mRenderLevel(0),
        mRenderStride(0) { };
    ~LLPatchVisibilityInfo() { };

    bool mbIsVisible;
    F32 mDistance;          // Distance from camera
    S32 mRenderLevel;
    U32 mRenderStride;
};



class LLSurfacePatch
{
public:
    LLSurfacePatch();
    ~LLSurfacePatch();

    void reset(const U32 id);
    void connectNeighbor(LLSurfacePatch *neighborp, const U32 direction);
    void disconnectNeighbor(LLSurface *surfacep);

    void setNeighborPatch(const U32 direction, LLSurfacePatch *neighborp);
    LLSurfacePatch *getNeighborPatch(const U32 direction) const;

    void colorPatch(const U8 r, const U8 g, const U8 b);

    bool updateTexture();

    void updateVerticalStats();
    void updateCompositionStats();
    template<bool PBR>
    void updateNormals();

    void updateEastEdge();
    void updateNorthEdge();
    void updateNortheastCorner();   // <WolfViewer 2026-09-23/>

    void updateCameraDistanceRegion( const LLVector3 &pos_region);
    void updateVisibility();
    void updateGL();

    void dirtyZ(); // Dirty the z values of this patch
    void setHasReceivedData();
    bool getHasReceivedData() const;

    F32 getDistance() const;
    F32 getMaxZ() const;
    F32 getMinZ() const;
    F32 getMeanComposition() const;
    F32 getMinComposition() const;
    F32 getMaxComposition() const;
    const LLVector3 &getCenterRegion() const;
    const U64 &getLastUpdateTime() const;
    LLSurface *getSurface() const { return mSurfacep; }
    LLVector3 getPointAgent(const U32 x, const U32 y) const; // get the point at the offset.
    LLVector2 getTexCoords(const U32 x, const U32 y) const;

    // Per-vertex normals
    // *TODO: PBR=true is a test implementation solely for proof-of-concept.
    // Final implementation would likely be very different and may not even use
    // this function. If we decide to keep calcNormalFlat, remove index as it
    // is a debug parameter for testing.
    template<bool PBR>
    void calcNormal(const U32 x, const U32 y, const U32 stride);
    const LLVector3 &getNormal(const U32 x, const U32 y) const;

    // Per-triangle normals for flat edges
    void calcNormalFlat(LLVector3& normal_out, const U32 x, const U32 y, const U32 index /* 0 or 1 */);

    // <WolfViewer 2026-09-05> heightmap ambient occlusion at a patch grid point, 0..1 (1 = open)
    F32 ambientOcclusion(const U32 x, const U32 y) const;
    void eval(const U32 x, const U32 y, const U32 stride,
                LLVector3 *vertex, LLVector3 *normal, LLVector2* tex0, LLVector2 *tex1) const;



    LLVector3 getOriginAgent() const;
    const LLVector3& getOriginRegion() const { return mOriginRegion; }   // <WolfViewer> patch origin, region frame
    // <WolfViewer 2026-09-26> The 256 m terrain DRAW BLOCK this patch belongs to. Vertices are
    // block-local (eval) and drawn with T(block origin agent) (LLVOSurfacePatch::wolfRenderMatrix),
    // so every patch of a block shares one model matrix and one terrain_patch_origin: the draw
    // loop switches them per block, not per patch (lldrawpoolterrain.cpp drawLoop). Block-local
    // coordinates stay under 256 m, the range stock region-local terrain has on a 256 m region.
    static constexpr F32 WOLF_TERRAIN_BLOCK_M = 256.f;
    const LLVector3& getBlockOriginRegion() const { return mBlockOriginRegion; }
    LLVector3d getBlockOriginGlobal() const;
    const LLVector3d &getOriginGlobal() const;
    void setOriginGlobal(const LLVector3d &origin_global);

    // connectivity -- each LLPatch points at 5 neighbors (or NULL)
    // +---+---+---+
    // |   | 2 | 5 |
    // +---+---+---+
    // | 3 | 0 | 1 |
    // +---+---+---+
    // | 6 | 4 |   |
    // +---+---+---+


    bool getVisible() const;
    void setInvisible() { mVisInfo.mbIsVisible = false; }   // <WolfViewer 2026-09-26/> updateVisibility's out-of-frustum result (LLSurface block cull)
    U32 getRenderStride() const;
    S32 getRenderLevel() const;

    void setSurface(LLSurface *surfacep);
    // <WolfViewer 2026-09-23> Give the patch its index in the surface and its own height/normal
    // block: (grids_per_patch_edge + 1)^2 points, row stride getDataStride(). Patch-local, not a
    // window into a region-sized array, so a patch costs the same on any size of region.
    void initData(const S32 patch_x, const S32 patch_y);
    S32 getPatchX() const                       { return mPatchX; }
    S32 getPatchY() const                       { return mPatchY; }
    // Row stride of mDataZ / mDataNorm: grids_per_patch_edge + 1 (was the surface's grids per edge).
    U32 getDataStride() const                   { return mDataStride; }
    // Is the neighbour in this direction there with data - or not expected at all? A neighbour
    // position inside this region or a connected one whose patch has not been made yet is
    // "there without data", as it was when every patch existed from the start.
    bool neighborReady(const U32 direction) const;
    // <FS:Wolf/> Create this patch's viewer object if it has none yet. Returns false if one
    // could not be made. See the note on setSurface.
    bool ensureVObj();
    F32 *getDataZ() const                       { return mDataZ; }

    void dirty();           // Mark this surface patch as dirty...
    void clearDirty()                           { mDirty = false; }

    bool isHeightsGenerated() const { return mHeightsGenerated; }

    void clearVObj();
    void releaseVObj();   // <WolfViewer 2026-09-23/> kill the viewer object; ensureVObj makes another

public:
    bool mHasReceivedData;  // has the patch EVER received height data?
    bool mSTexUpdate;       // Does the surface texture need to be updated?

protected:
    LLSurfacePatch *mNeighborPatches[8]; // Adjacent patches
    bool mNormalsInvalid[9];  // Which normals are invalid

    bool mDirty;
    bool mDirtyZStats;
    bool mHeightsGenerated;

    F32 *mDataZ;
    LLVector3 *mDataNorm;
    // <WolfViewer 2026-09-23/> the storage mDataZ / mDataNorm point at, and where the patch is.
    std::unique_ptr<F32[]> mZStore;
    std::unique_ptr<LLVector3[]> mNormStore;
    U32 mDataStride;
    S32 mPatchX;
    S32 mPatchY;

    // Pointer to the LLVOSurfacePatch object which is used in the new renderer.
    LLPointer<LLVOSurfacePatch> mVObjp;

    // All of the camera-dependent stuff should be in its own class...
    LLPatchVisibilityInfo mVisInfo;

    // pointers to beginnings of patch data fields
    LLVector3d mOriginGlobal;
    LLVector3 mOriginRegion;
    LLVector3 mBlockOriginRegion;   // <WolfViewer 2026-09-26/> see getBlockOriginRegion


    // height field stats
    LLVector3 mCenterRegion; // Center in region-local coords
    F32 mMinZ, mMaxZ, mMeanZ;
    F32 mRadius;

    F32 mMinComposition;
    F32 mMaxComposition;
    F32 mMeanComposition;

    U8 mConnectedEdge;      // This flag is non-zero iff patch is on at least one edge
                            // of LLSurface that is "connected" to another LLSurface
    U64 mLastUpdateTime;    // Time patch was last updated

    LLSurface *mSurfacep; // Pointer to "parent" surface
};

extern template void LLSurfacePatch::updateNormals</*PBR=*/false>();
extern template void LLSurfacePatch::updateNormals</*PBR=*/true>();


#endif // LL_LLSURFACEPATCH_H
