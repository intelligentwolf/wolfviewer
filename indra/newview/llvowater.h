/**
 * @file llvowater.h
 * @brief Description of LLVOWater class
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

#ifndef LL_VOWATER_H
#define LL_VOWATER_H

#include "llviewerobject.h"
#include "llviewertexture.h"
#include "pipeline.h"
#include "v2math.h"
#include <memory>   // <WolfViewer> ConformingMesh
#include <vector>

const U32 N_RES = 16; //32          // number of subdivisions of wave tile
const U8  WAVE_STEP     = 8;

class LLSurface;
class LLHeavenBody;
class LLVOSky;
class LLFace;

class LLVOWater : public LLStaticViewerObject
{
public:
    enum
    {
        VERTEX_DATA_MASK =  (1 << LLVertexBuffer::TYPE_VERTEX) |
                            (1 << LLVertexBuffer::TYPE_NORMAL) |
                            (1 << LLVertexBuffer::TYPE_TEXCOORD0) |
                            (1 << LLVertexBuffer::TYPE_TEXCOORD1)   // <WolfViewer>
    };

    LLVOWater(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    /*virtual*/ void markDead();

    // Initialize data that's only inited once per class.
    static void initClass();
    static void cleanupClass();

    /*virtual*/ void idleUpdate(LLAgent &agent, const F64 &time);
    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);
    /*virtual*/ bool        updateGeometry(LLDrawable *drawable);
    /*virtual*/ void        updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax);

    /*virtual*/ void updateTextures();
    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent); // generate accurate apparent angle and area

    virtual U32 getPartitionType() const;

    /*virtual*/ bool isActive() const; // Whether this object needs to do an idleUpdate.

    void setIsEdgePatch(const bool edge_patch);
    bool getIsEdgePatch() const { return mIsEdgePatch; }

    // <FS:WolfViewer> A BOUNDED water surface — one fitted to a prim described "wolfwater"
    // (fswolfwater.cpp) rather than the region's own water plane.
    //
    // The value is the depth of water the surface represents, in metres, taken from the
    // prim's own Z extent; 0 means "this is region water". The water shader needs BOTH
    // facts and they come as one number because they are one question:
    //
    //   - Region water gets its colour almost entirely from the refraction buffer plus
    //     reflections, and its BODY from the water fog applied to submerged geometry. Behind
    //     a pool prim nothing has been fogged, so without a depth to absorb over, a pool is
    //     very nearly invisible.
    //   - The shoreline breaking foam keys on the gap between the surface and whatever is
    //     behind it. For a thin pool prim that gap is a few centimetres everywhere, so every
    //     fragment reads as maximally shoaling and the whole pool turns to whitewater.
    void setBoundedWaterDepth(F32 depth_m) { mBoundedWaterDepth = depth_m; }
    F32  getBoundedWaterDepth() const { return mBoundedWaterDepth; }
    // <WolfViewer> 1 = this plane is a waterfall sheet (wolfnaturalwater.cpp): the water
    // shader draws it foam-white with the flow racing down the face instead of as a pool.
    void setWaterfall(F32 on) { mWaterfall = on; }
    F32  getWaterfall() const { return mWaterfall; }

    // <WolfViewer> A TERRAIN-CONFORMING water surface (wolfnaturalwater.cpp streams).
    //
    // A stream is not a plane: its surface follows the bed downhill, bends with the
    // gully, drops over ledges and widens as tributaries join. So instead of the lattice
    // updateGeometry builds from position+scale, the owner hands in a finished mesh —
    // vertices in REGION space (updateGeometry adds the region's agent origin, which is
    // why a region crossing, which rebuilds every static water drawable, comes out right).
    //
    // Texcoords carry the ribbon's own coordinates in metres: x = distance along the flow,
    // y = signed distance across it. The water shader (waterV.glsl, uniform wolfStream)
    // scrolls its ripple maps along x so the water visibly runs downstream; region water,
    // whose texcoords are 0..1 fractions, never enters that path.
    //
    // mFlow is the stream's surface speed in m/s; > 0 marks the object as a stream mesh.
    struct ConformingMesh
    {
        std::vector<LLVector3> mVerts;     // region space
        std::vector<LLVector3> mNormals;
        std::vector<LLVector2> mUVs;       // (metres along flow, metres across)
        // TERRAIN-LOD LIFT, per vertex, metres: how far the terrain AS DRAWN at render
        // stride 4 (x) and 16 (y) rises above this vertex. The terrain is drawn coarser
        // with distance (LLSurfacePatch::updateVisibility: stride ~ distance * 0.15 / mpg,
        // so 2 at 13 m, 4 at 27 m, 8 at 53 m, 16 at 107 m), and a coarse mesh spans
        // ACROSS a gully, above the fine heights the water was fitted to — burying the
        // stream exactly where it is. waterV.glsl reproduces the stride rule from the
        // vertex's own distance and lifts the vertex by the interpolated value, so the
        // water rides the ground that is actually on screen. Zero for region water.
        std::vector<LLVector2> mLodLift;
        std::vector<U16>       mIndices;   // U16: LLFace strides indices as U16
        LLVector3 mMin, mMax;              // region-space bounds of mVerts
    };
    void setConformingMesh(std::shared_ptr<const ConformingMesh> mesh, F32 flow_mps);
    F32  getStreamFlow() const { return mStreamFlow; }
    bool hasConformingMesh() const { return mMesh != nullptr; }
    // </WolfViewer>
    // </FS:WolfViewer>

protected:
    bool mIsEdgePatch;
    S32  mRenderType;
    F32  mBoundedWaterDepth = 0.f;  // <FS:WolfViewer>
    F32  mWaterfall = 0.f;          // <WolfViewer>
    std::shared_ptr<const ConformingMesh> mMesh;   // <WolfViewer> null = lattice from scale
    F32  mStreamFlow = 0.f;                        // <WolfViewer>
};

class LLVOVoidWater : public LLVOWater
{
public:
    LLVOVoidWater(LLUUID const& id, LLPCode pcode, LLViewerRegion* regionp) : LLVOWater(id, pcode, regionp)
    {
        mRenderType = LLPipeline::RENDER_TYPE_VOIDWATER;
    }

    /*virtual*/ U32 getPartitionType() const;
};


#endif // LL_VOSURFACEPATCH_H
