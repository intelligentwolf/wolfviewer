/**
 * @file llvowater.cpp
 * @brief LLVOWater class implementation
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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

#include "llvowater.h"

#include "llviewercontrol.h"

#include "lldrawable.h"
#include "lldrawpoolwater.h"
#include "llface.h"
#include "llsky.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "llspatialpartition.h"

///////////////////////////////////

template<class T> inline T LERP(T a, T b, F32 factor)
{
    return a + (b - a) * factor;
}

LLVOWater::LLVOWater(const LLUUID &id,
                     const LLPCode pcode,
                     LLViewerRegion *regionp) :
    LLStaticViewerObject(id, pcode, regionp),
    mRenderType(LLPipeline::RENDER_TYPE_WATER)
{
    // Terrain must draw during selection passes so it can block objects behind it.
    mbCanSelect = false;
// <FS:CR> Aurora Sim
    //setScale(LLVector3(256.f, 256.f, 0.f)); // Hack for setting scale for bounding boxes/visibility.
    setScale(LLVector3(mRegionp->getWidth(), mRegionp->getWidth(), 0.f));
// </FS:CR> Aurora Sim

    mIsEdgePatch = false;
}


void LLVOWater::markDead()
{
    LLViewerObject::markDead();
}


bool LLVOWater::isActive() const
{
    return false;
}


void LLVOWater::setPixelAreaAndAngle(LLAgent &agent)
{
    mAppAngle = 50;
    mPixelArea = 500*500;
}


// virtual
void LLVOWater::updateTextures()
{
}

// Never gets called
void  LLVOWater::idleUpdate(LLAgent &agent, const F64 &time)
{
}

LLDrawable *LLVOWater::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(mRenderType);

    LLDrawPoolWater *pool = (LLDrawPoolWater*) gPipeline.getPool(LLDrawPool::POOL_WATER);

    mDrawable->setNumFaces(1, pool, LLWorld::getInstance()->getDefaultWaterTexture());

    return mDrawable;
}

bool LLVOWater::updateGeometry(LLDrawable *drawable)
{
    LL_PROFILE_ZONE_SCOPED;
    LLFace *face;

    if (drawable->getNumFaces() < 1)
    {
        LLDrawPoolWater *poolp = (LLDrawPoolWater*) gPipeline.getPool(LLDrawPool::POOL_WATER);
        drawable->addFace(poolp, NULL);
    }
    face = drawable->getFace(0);
    if (!face)
    {
        return true;
    }

//  LLVector2 uvs[4];
//  LLVector3 vtx[4];

    LLStrider<LLVector3> verticesp, normalsp;
    LLStrider<LLVector2> texCoordsp;
    LLStrider<U16> indicesp;
    U16 index_offset;

    // <WolfViewer> Terrain-conforming mesh: the owner built the surface, this only uploads
    // it. See LLVOWater::ConformingMesh. Region-space vertices become agent-space here, at
    // upload time, because that is what a region crossing invalidates: LLDrawable::shiftPos
    // marks every static water drawable for rebuild, and this runs again with the new origin.
    if (mMesh)
    {
        const ConformingMesh& m = *mMesh;
        if (m.mVerts.empty() || m.mIndices.empty() || m.mVerts.size() > 65535)
        {
            return true;
        }
        face->setSize((S32)m.mVerts.size(), (S32)m.mIndices.size());
        LLVertexBuffer* mbuff = face->getVertexBuffer();
        if (!mbuff ||
            mbuff->getNumIndices() != face->getIndicesCount() ||
            mbuff->getNumVerts() != face->getGeomCount() ||
            face->getIndicesStart() != 0 ||
            face->getGeomIndex() != 0)
        {
            mbuff = new LLVertexBuffer(LLDrawPoolWater::VERTEX_DATA_MASK);
            if (!mbuff->allocateBuffer(face->getGeomCount(), face->getIndicesCount()))
            {
                LL_WARNS() << "Failed to allocate Vertex Buffer on conforming water update to "
                    << face->getGeomCount() << " vertices and "
                    << face->getIndicesCount() << " indices" << LL_ENDL;
            }
            face->setIndicesIndex(0);
            face->setGeomIndex(0);
            face->setVertexBuffer(mbuff);
        }
        index_offset = face->getGeometry(verticesp, normalsp, texCoordsp, indicesp);
        LLStrider<LLVector2> lodp;
        const bool has_lod = m.mLodLift.size() == m.mVerts.size()
                          && mbuff->getTexCoord1Strider(lodp, face->getGeomIndex(), face->getGeomCount());

        const LLVector3 origin = mRegionp->getOriginAgent();
        face->mCenterAgent = origin + (m.mMin + m.mMax) * 0.5f;
        face->mCenterLocal = face->mCenterAgent;
        for (size_t i = 0; i < m.mVerts.size(); ++i)
        {
            *verticesp++  = origin + m.mVerts[i];
            *normalsp++   = m.mNormals[i];
            *texCoordsp++ = m.mUVs[i];
            if (has_lod)
            {
                *lodp++ = m.mLodLift[i];
            }
        }
        for (U16 idx : m.mIndices)
        {
            *indicesp++ = (U16)(index_offset + idx);
        }
        mbuff->unmapBuffer();
        mDrawable->movePartition();
        LLPipeline::sCompiles++;
        return true;
    }
    // </WolfViewer>

    // A quad is 4 vertices and 6 indices (making 2 triangles)
    static const unsigned int vertices_per_quad = 4;
    static const unsigned int indices_per_quad = 6;

    const LLVector3& scale = getScale();

    // <FS:WolfViewer> WAVE GEOMETRY.
    //
    // THE PROBLEM THIS SOLVES. Stock water is 8x8 quads for a 256m region, i.e. a vertex
    // every 32 metres, and waterV.glsl displaces none of them — every "wave" in the stock
    // viewer is normal-map shading on a dead-flat plane. That is what reads as plastic.
    // Simply adding Gerstner displacement to this lattice would change nothing visible:
    // Nyquist needs at least two vertices per wavelength, so a 32m step cannot express any
    // swell shorter than 64m, and every wave we care about (a ~20m dominant swell, ~3m
    // chop) would collapse back to a flat plane.
    //
    // So the geometry has to come first. Vertices are placed on a fixed WORLD-SPACE step
    // rather than a fixed count, so a region's water is tessellated to the waves rather
    // than to the region's size.
    //
    // TWO CONSTRAINTS SET THE NUMBERS:
    //   - The index stride here is U16 (LLStrider<U16> above), so a face cannot address
    //     more than 65536 vertices. (MAX_STEPS+1)^2 = 255^2 = 65025 stays under it with
    //     room to spare.
    //   - Stock builds four SEPARATE vertices per quad. At this density that would quadruple
    //     the buffer for nothing, so the lattice below SHARES vertices between neighbouring
    //     quads: (n+1)^2 vertices instead of 4*n^2. It is also what keeps the surface
    //     watertight once the vertices start moving — duplicated vertices displaced by the
    //     same wave still agree, but only because they are computed from world position;
    //     sharing removes the question.
    //
    // A 256m region gets the full 2m step (128 quads a side, 16641 vertices). Larger
    // regions hold the step until the cap and then degrade: 512m -> 2.02m, 1024m -> 4.03m,
    // 2048m -> 8.06m. DECLARED LIMIT: at 2048m the step is coarse enough to soften the
    // dominant swell, which is the honest trade against a 4x vertex budget on a region
    // whose water is mostly beyond the draw distance anyway.
    //
    // EDGE (void) water is deliberately left at the stock tessellation. Those planes are
    // the 2048m stretch-to-horizon quads built by LLWorld::updateWaterObjects, where a
    // real step would cost tens of megabytes to displace water that is kilometres away;
    // the vertex shader fades wave amplitude out with distance long before it reaches them.
    static const F32 TARGET_STEP_M = 2.f;
    static const S32 MAX_STEPS = 254;

    S32 size_x;
    S32 size_y;
    bool shared_lattice = false;

    if (!LLPipeline::sRenderTransparentWater)
    {
        // Opaque legacy water: one quad, as stock. renderOpaqueLegacyWater() does not run
        // the wave shader at all, so tessellating it would buy nothing.
        size_x = 1;
        size_y = 1;
    }
    else if (mIsEdgePatch)
    {
        size_x = 8 * (S32)llmin(llround(scale.mV[0] / 256.f), 8);
        size_y = 8 * (S32)llmin(llround(scale.mV[1] / 256.f), 8);
    }
    else
    {
        size_x = llclamp((S32)llround(scale.mV[0] / TARGET_STEP_M), 1, MAX_STEPS);
        size_y = llclamp((S32)llround(scale.mV[1] / TARGET_STEP_M), 1, MAX_STEPS);
        shared_lattice = true;
    }

    // llround can return 0 for a degenerate scale; a zero-quad face would allocate nothing
    // and then be strided into below.
    size_x = llmax(size_x, 1);
    size_y = llmax(size_y, 1);

    const S32 num_quads = size_x * size_y;
    if (shared_lattice)
    {
        face->setSize((size_x + 1) * (size_y + 1), indices_per_quad * num_quads);
    }
    else
    {
        face->setSize(vertices_per_quad * num_quads,
                      indices_per_quad * num_quads);
    }
    // </FS:WolfViewer>

    LLVertexBuffer* buff = face->getVertexBuffer();
    if (!buff ||
        buff->getNumIndices() != face->getIndicesCount() ||
        buff->getNumVerts() != face->getGeomCount() ||
        face->getIndicesStart() != 0 ||
        face->getGeomIndex() != 0)
    {
        buff = new LLVertexBuffer(LLDrawPoolWater::VERTEX_DATA_MASK);
        if (!buff->allocateBuffer(face->getGeomCount(), face->getIndicesCount()))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer on water update to "
                << face->getGeomCount() << " vertices and "
                << face->getIndicesCount() << " indices" << LL_ENDL;
        }
        face->setIndicesIndex(0);
        face->setGeomIndex(0);
        face->setVertexBuffer(buff);
    }

    index_offset = face->getGeometry(verticesp,normalsp,texCoordsp, indicesp);

    LLVector3 position_agent;
    position_agent = getPositionAgent();
    face->mCenterAgent = position_agent;
    face->mCenterLocal = position_agent;

    S32 x, y;
    F32 step_x = getScale().mV[0] / size_x;
    F32 step_y = getScale().mV[1] / size_y;

    const LLVector3 up(0.f, step_y * 0.5f, 0.f);
    const LLVector3 right(step_x * 0.5f, 0.f, 0.f);
    const LLVector3 normal(0.f, 0.f, 1.f);

    F32 size_inv_x = 1.f / size_x;
    F32 size_inv_y = 1.f / size_y;

    // <FS:WolfViewer> Shared-vertex lattice for wave-bearing water — see the note above
    // updateGeometry's face->setSize. (size_x+1) x (size_y+1) vertices, each used by up to
    // four quads, laid out row-major so a vertex index is (j * (size_x+1) + i).
    if (shared_lattice)
    {
        // Built about the object's CENTRE rather than its corner, so an oriented surface
        // is one rotation of the offset. Region water is never rotated and takes the
        // identity path, which reduces to exactly the corner-plus-step arithmetic the
        // stock loop below uses.
        //
        // Rotation is here for wolfwater prims (fswolfwater.cpp): a prim described
        // "wolfwater" gets one of these fitted to it, and a builder is entitled to tilt it
        // into a sloping stream. The vertex normal is rotated with the surface, and
        // waterV.glsl reads that attribute rather than assuming +Z, so a tilted surface
        // shades and displaces along its own up vector.
        const LLVector3    center = getPositionAgent();
        const LLVector3    half   = getScale() * 0.5f;
        const LLQuaternion rot    = getRotation();
        const bool         rotated = (rot != LLQuaternion());
        const LLVector3    surface_normal = rotated ? (normal * rot) : normal;
        const S32          row = size_x + 1;

        for (y = 0; y <= size_y; y++)
        {
            for (x = 0; x <= size_x; x++)
            {
                LLVector3 off(x * step_x - half.mV[VX],
                              y * step_y - half.mV[VY],
                              -half.mV[VZ]);
                if (rotated)
                {
                    off = off * rot;
                }
                *verticesp++  = center + off;
                *normalsp++   = surface_normal;
                *texCoordsp++ = LLVector2(x * size_inv_x, y * size_inv_y);
            }
        }

        for (y = 0; y < size_y; y++)
        {
            for (x = 0; x < size_x; x++)
            {
                const U16 i0 = (U16)(index_offset + y * row + x);
                const U16 i1 = (U16)(i0 + 1);
                const U16 i2 = (U16)(i0 + row);
                const U16 i3 = (U16)(i2 + 1);

                // Both triangles wind counter-clockwise seen from +Z, matching the stock
                // quad below: there (BL-TL)x(TR-TL) points +Z, here (BR-BL)x(TL-BL) and
                // (TR-BR)x(TL-BR) both point +Z. The water surface must face up, or it
                // culls away when you are standing on the shore looking at it.
                *indicesp++ = i0;
                *indicesp++ = i1;
                *indicesp++ = i2;

                *indicesp++ = i1;
                *indicesp++ = i3;
                *indicesp++ = i2;
            }
        }

        buff->unmapBuffer();

        mDrawable->movePartition();
        LLPipeline::sCompiles++;
        return true;
    }
    // </FS:WolfViewer>

    for (y = 0; y < size_y; y++)
    {
        for (x = 0; x < size_x; x++)
        {
            S32 toffset = index_offset + 4*(y*size_x + x);
            position_agent = getPositionAgent() - getScale() * 0.5f;
            position_agent.mV[VX] += (x + 0.5f) * step_x;
            position_agent.mV[VY] += (y + 0.5f) * step_y;

            position_agent.mV[VX] = (F32)llround(position_agent.mV[VX]);
            position_agent.mV[VY] = (F32)llround(position_agent.mV[VY]);

            *verticesp++  = position_agent - right + up;
            *verticesp++  = position_agent - right - up;
            *verticesp++  = position_agent + right + up;
            *verticesp++  = position_agent + right - up;

            *texCoordsp++ = LLVector2(x*size_inv_x, (y+1)*size_inv_y);
            *texCoordsp++ = LLVector2(x*size_inv_x, y*size_inv_y);
            *texCoordsp++ = LLVector2((x+1)*size_inv_x, (y+1)*size_inv_y);
            *texCoordsp++ = LLVector2((x+1)*size_inv_x, y*size_inv_y);

            *normalsp++   = normal;
            *normalsp++   = normal;
            *normalsp++   = normal;
            *normalsp++   = normal;

            *indicesp++ = toffset + 0;
            *indicesp++ = toffset + 1;
            *indicesp++ = toffset + 2;

            *indicesp++ = toffset + 1;
            *indicesp++ = toffset + 3;
            *indicesp++ = toffset + 2;
        }
    }

    buff->unmapBuffer();

    mDrawable->movePartition();
    LLPipeline::sCompiles++;
    return true;
}

void LLVOWater::initClass()
{
}

void LLVOWater::cleanupClass()
{
}

void setVecZ(LLVector3& v)
{
    v.mV[VX] = 0;
    v.mV[VY] = 0;
    v.mV[VZ] = 1;
}

void LLVOWater::setIsEdgePatch(const bool edge_patch)
{
    mIsEdgePatch = edge_patch;
}

// <WolfViewer>
void LLVOWater::setConformingMesh(std::shared_ptr<const ConformingMesh> mesh, F32 flow_mps)
{
    mMesh = std::move(mesh);
    mStreamFlow = mMesh ? llmax(flow_mps, 0.f) : 0.f;
    if (mMesh)
    {
        // Keep position/scale describing the mesh's box: the constructor set the region's
        // width, and anything that reasons from the object rather than the drawable (the
        // partition's initial placement, updateSpatialExtents below) reads these.
        setPositionRegion((mMesh->mMin + mMesh->mMax) * 0.5f);
        setScale(mMesh->mMax - mMesh->mMin);
    }
    if (mDrawable.notNull())
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
    }
}
// </WolfViewer>

void LLVOWater::updateSpatialExtents(LLVector4a &newMin, LLVector4a& newMax)
{
    // <WolfViewer> a conforming mesh's box is its vertices' box, not the plane's scale
    if (mMesh)
    {
        const LLVector3 origin = mRegionp->getOriginAgent();
        // A little slack so a fragment on the edge is never culled by its own bounds.
        const LLVector3 pad(0.1f, 0.1f, 0.1f);
        newMin.load3((origin + mMesh->mMin - pad).mV);
        newMax.load3((origin + mMesh->mMax + pad).mV);
        LLVector4a mid;
        mid.setAdd(newMin, newMax);
        mid.mul(0.5f);
        mDrawable->setPositionGroup(mid);
        return;
    }
    // </WolfViewer>
    LLVector4a pos;
    pos.load3(getPositionAgent().mV);
    LLVector4a scale;
    scale.load3(getScale().mV);
    scale.mul(0.5f);

    newMin.setSub(pos, scale);
    newMax.setAdd(pos, scale);

    pos.setAdd(newMin,newMax);
    pos.mul(0.5f);

    mDrawable->setPositionGroup(pos);
}

U32 LLVOWater::getPartitionType() const
{
    if (mIsEdgePatch)
    {
        return LLViewerRegion::PARTITION_VOIDWATER;
    }

    return LLViewerRegion::PARTITION_WATER;
}

U32 LLVOVoidWater::getPartitionType() const
{
    return LLViewerRegion::PARTITION_VOIDWATER;
}

LLWaterPartition::LLWaterPartition(LLViewerRegion* regionp)
: LLSpatialPartition(0, false, regionp)
{
    mInfiniteFarClip = true;
    mDrawableType = LLPipeline::RENDER_TYPE_WATER;
    mPartitionType = LLViewerRegion::PARTITION_WATER;
}

LLVoidWaterPartition::LLVoidWaterPartition(LLViewerRegion* regionp) : LLWaterPartition(regionp)
{
    mOcclusionEnabled = false;
    mDrawableType = LLPipeline::RENDER_TYPE_VOIDWATER;
    mPartitionType = LLViewerRegion::PARTITION_VOIDWATER;
}
