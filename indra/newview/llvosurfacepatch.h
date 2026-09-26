/**
 * @file llvosurfacepatch.h
 * @brief Description of LLVOSurfacePatch class
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

#ifndef LL_VOSURFACEPATCH_H
#define LL_VOSURFACEPATCH_H

#include "llviewerobject.h"
#include "llstrider.h"
#include <vector>

class LLSurfacePatch;
class LLDrawPool;
class LLVector2;
class LLFacePool;
class LLFace;
class LLSurface;

class LLVOSurfacePatch : public LLStaticViewerObject
{
public:
    static F32 sLODFactor;

    LLVOSurfacePatch(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    /*virtual*/ void markDead();

    // Initialize data that's only inited once per class.
    static void initClass();

    virtual U32 getPartitionType() const;

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);
    /*virtual*/ void        updateGL();
    /*virtual*/ bool        updateGeometry(LLDrawable *drawable);
    /*virtual*/ bool        updateLOD();
    /*virtual*/ void        updateFaceSize(S32 idx);
    void getTerrainGeometry(LLStrider<LLVector3> &verticesp,
                                LLStrider<LLVector3> &normalsp,
                                LLStrider<LLVector2> &texCoords0p,
                                LLStrider<LLVector2> &texCoords1p,
                                LLStrider<LLColor4U> &colorsp,  // <WolfViewer> heightmap AO in .r
                                LLStrider<U16> &indicesp);

    /*virtual*/ void updateTextures();
    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent); // generate accurate apparent angle and area

    /*virtual*/ void updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax);
    /*virtual*/ bool isActive() const; // Whether this object needs to do an idleUpdate.

    // <WolfViewer 2026-09-26> ONE OBJECT PER BLOCK of WOLF_BLOCK_PATCHES x WOLF_BLOCK_PATCHES
    // patches (8 x 8 = 128 m on a 1 m grid), not one per patch. Every drawable costs a cull, a
    // sort and a draw call in every pass, so a region streaming terrain out to a 4,096 m draw
    // distance (Ireland) made the frame slower with every patch that arrived: 13,917 visible
    // patch objects and 32 fps with the terrain only half loaded. The patches keep their data,
    // normals and per-patch LOD strides; the block object draws them all from one face and its
    // own vertex buffer (LLTerrainPartition::getGeometry), at most 64 x 322 = 20,608 vertices,
    // inside the U16 index limit. The patch geometry code is unchanged: getTerrainGeometry
    // points mPatchp and mLast*Stride at each member patch in turn and runs it.
    static constexpr S32 WOLF_BLOCK_PATCHES = 8;
    void setBlock(LLSurface* surfacep, S32 block_i, S32 block_j);
    void addPatch(LLSurfacePatch* patchp);
    size_t removePatch(LLSurfacePatch* patchp);   // returns the patches left
    void detachSurface();                          // the surface is going: forget it and its patches
    // A member patch (any: they share the block and its draw matrix), or NULL when empty.
    LLSurfacePatch  *getPatch() const       { return mPatches.empty() ? NULL : mPatches.front(); }
    // <WolfViewer 2026-09-20> draw matrix T(origin agent): precise, because the origin is a
    // double minus the agent origin, which LLAgent::updateHugeRegionOrigin keeps near the camera.
    // 2026-09-26: the origin is the patch's 256 m draw BLOCK (LLSurfacePatch::getBlockOriginGlobal)
    // and LLSurfacePatch::eval writes block-local vertices to match, so the patches of a block
    // share one matrix (lldrawpoolterrain.cpp switches it per block).
    const LLMatrix4* wolfRenderMatrix();
    LLMatrix4        mWolfRenderMatrix;

    void dirtyPatch();
    void dirtyGeom();

    /*virtual*/ bool lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                          S32 face = -1,                        // which face to check, -1 = ALL_SIDES
                                          bool pick_transparent = false,
                                          bool pick_rigged = false,
                                          bool pick_unselectable = true,
                                          S32* face_hit = NULL,                 // which face was hit
                                          LLVector4a* intersection = NULL,       // return the intersection point
                                          LLVector2* tex_coord = NULL,          // return the texture coordinates of the intersection point
                                          LLVector4a* normal = NULL,             // return the surface normal at the intersection point
                                          LLVector4a* tangent = NULL           // return the surface tangent at the intersection point
        );

    bool            mDirtiedPatch;
protected:
    ~LLVOSurfacePatch();

    LLFacePool      *mPool;
    LLFacePool      *getPool();
    S32             mBaseComp;
    LLSurfacePatch  *mPatchp;   // <WolfViewer 2026-09-26/> the patch the geometry code is working on (getTerrainGeometry)

    // <WolfViewer 2026-09-26> the block (see WOLF_BLOCK_PATCHES)
    struct WolfPatchLOD
    {
        LLSurfacePatch* mPatch;
        S32             mStride;
        S32             mNorthStride;
        S32             mEastStride;
    };
    LLSurface*                   mSurfacep;
    S32                          mBlockI;
    S32                          mBlockJ;
    std::vector<LLSurfacePatch*> mPatches;    // member patches
    std::vector<WolfPatchLOD>    mPatchLOD;   // their strides as of the last updateGeometry
    void wolfSelectPatch(const WolfPatchLOD& lod);   // point mPatchp / mLast*Stride at one member
    // </WolfViewer>
    bool            mDirtyTexture;
    bool            mDirtyTerrain;

    S32             mLastNorthStride;
    S32             mLastEastStride;
    S32             mLastStride;
    S32             mLastLength;

    void getGeomSizesMain(const S32 stride, S32 &num_vertices, S32 &num_indices);
    void getGeomSizesNorth(const S32 stride, const S32 north_stride,
                                  S32 &num_vertices, S32 &num_indices);
    void getGeomSizesEast(const S32 stride, const S32 east_stride,
                                 S32 &num_vertices, S32 &num_indices);

    void updateMainGeometry(LLFace *facep,
                       LLStrider<LLVector3> &verticesp,
                       LLStrider<LLVector3> &normalsp,
                       LLStrider<LLVector2> &texCoords0p,
                       LLStrider<LLVector2> &texCoords1p,
                       LLStrider<LLColor4U> &colorsp,
                       LLStrider<U16> &indicesp,
                       U32 &index_offset);
    void updateNorthGeometry(LLFace *facep,
                       LLStrider<LLVector3> &verticesp,
                       LLStrider<LLVector3> &normalsp,
                       LLStrider<LLVector2> &texCoords0p,
                       LLStrider<LLVector2> &texCoords1p,
                       LLStrider<LLColor4U> &colorsp,
                       LLStrider<U16> &indicesp,
                       U32 &index_offset);
    void updateEastGeometry(LLFace *facep,
                       LLStrider<LLVector3> &verticesp,
                       LLStrider<LLVector3> &normalsp,
                       LLStrider<LLVector2> &texCoords0p,
                       LLStrider<LLVector2> &texCoords1p,
                       LLStrider<LLColor4U> &colorsp,
                       LLStrider<U16> &indicesp,
                       U32 &index_offset);
};

#endif // LL_VOSURFACEPATCH_H
