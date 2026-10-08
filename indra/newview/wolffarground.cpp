/**
 * @file wolffarground.cpp
 * @brief WolfViewer: regions that are not connected yet stand in as their world-map tiles.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolffarground.h"

#include <map>

#include "llagent.h"
#include "llagentcamera.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmipmap.h"
#include "pipeline.h"
#include "wolfgrid.h"
#include "wolfmapglobe.h"

namespace
{
    constexpr F64 REFRESH_SECS = 1.0;           // look again this often (the map and the regions change slowly)
    constexpr F64 WATER_UPDATE_SECS = 2.0;      // at most this often re-cut the sea round the tiles
    constexpr S32 MAX_TILES_ACROSS = 8;         // the tile level: no more than this many tiles across the reach
    constexpr U32 MAX_QUADS = 16383;            // U16 indices: 4 vertices a cell
    constexpr U32 CELL_M = 256;

    // Every connected region has one of these cells' handles, so "is it drawn for real?" is one lookup.
    bool connected(U64 handle)
    {
        return LLWorld::getInstance()->getRegionFromHandle(handle) != nullptr;
    }

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, "WolfViewerFarGroundMapTiles", true);
        return on && WolfGrid::isWolfTerritories();
    }
}

WolfFarGround::WolfFarGround()
{
}

WolfFarGround::~WolfFarGround()
{
}

bool WolfFarGround::coversCell(U32 x, U32 y) const
{
    return mCovered.count(to_region_handle(x, y)) > 0;
}

void WolfFarGround::clear()
{
    if (!mCovered.empty())
    {
        mWaterDirty = true;
    }
    mTiles.clear();
    mCovered.clear();
    mVB = nullptr;
    mVerts = 0;
}

void WolfFarGround::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (now >= mNextRefresh)
    {
        mNextRefresh = now + REFRESH_SECS;
        refresh();
    }
    else if (agent_region && (agent_region->getOriginGlobal() != mBuiltOrigin
                              || agent_region->getWaterHeight() != mBuiltWaterZ) && !mTiles.empty())
    {
        rebuildBuffer();   // a crossing moved the agent frame the vertices are in
    }
    // Wanted at full size every frame, as the world map view marks the tiles it draws: the texture
    // list lowers what nothing has asked for since the last frame.
    bool made = false;
    for (Tile& t : mTiles)
    {
        if (t.mTexture.notNull())
        {
            t.mTexture->addTextureStats((F32)(LLWorldMipmap::MAP_TILE_SIZE * LLWorldMipmap::MAP_TILE_SIZE));
            if (t.mDraw.isNull() && t.mTexture->hasGLTexture())
            {
                // made a few a frame (WolfMapGlobe's own limit); a new one is drawn from the next look
                t.mDraw = WolfMapGlobe::instance().keyedTile(t.mTexture.get(), mLevel, t.mGridX, t.mGridY);
                made = made || t.mDraw.notNull();
            }
        }
    }
    if (made)
    {
        mNextRefresh = 0.0;
    }
    // The sea leaves out the cells drawn here (LLWorld::updateWaterObjects): cut it again when they change.
    if (mWaterDirty && now - mLastWaterUpdate >= WATER_UPDATE_SECS && agent_region)
    {
        mWaterDirty = false;
        mLastWaterUpdate = now;
        LLWorld::getInstance()->updateWaterObjects();
    }
}

void WolfFarGround::refresh()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!enabled() || !agent_region)
    {
        clear();
        return;
    }
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    const F32 water_z = agent_region->getWaterHeight();
    const F32 far_clip = LLViewerCamera::getInstance()->getFar();
    const F32 height = llmax(0.f, (F32)cam.mdV[VZ] - water_z);
    // How far out the ground is in view: the far clip, less the height it is seen from.
    const F32 reach = sqrtf(llmax(0.f, far_clip * far_clip - height * height));
    if (reach < (F32)CELL_M)
    {
        clear();
        return;
    }
    // The tile level: coarse enough that the reach is at most MAX_TILES_ACROSS tiles across.
    S32 level = 1;
    while (level < LLWorldMipmap::MAP_LEVELS && (F32)(CELL_M << (level - 1)) * MAX_TILES_ACROSS < 2.f * reach)
    {
        ++level;
    }
    mLevel = level;

    const F64 x0 = llmax(0.0, cam.mdV[VX] - reach), x1 = cam.mdV[VX] + reach;
    const F64 y0 = llmax(0.0, cam.mdV[VY] - reach), y1 = cam.mdV[VY] + reach;
    // Ask the map server about the regions there (it skips blocks it asked for lately).
    LLWorldMap::getInstance()->updateRegions((S32)(x0 / CELL_M), (S32)(y0 / CELL_M), (S32)(x1 / CELL_M), (S32)(y1 / CELL_M));

    std::map<U64, Tile> tiles;   // keyed by the tile's grid position
    U32 cells = 0;
    for (const auto& kv : LLWorldMap::getInstance()->getRegionMap())
    {
        const LLSimInfo* info = kv.second;
        if (!info)
        {
            continue;
        }
        U32 ox = 0, oy = 0;
        from_region_handle(kv.first, &ox, &oy);
        const U32 sx = llmax((U32)info->getSizeX(), CELL_M), sy = llmax((U32)info->getSizeY(), CELL_M);
        if ((F64)(ox + sx) <= x0 || (F64)ox >= x1 || (F64)(oy + sy) <= y0 || (F64)oy >= y1)
        {
            continue;
        }
        // only the region's cells within reach (a huge region has millions)
        const U32 cx0 = llmax(ox, (U32)(x0 / CELL_M) * CELL_M), cx1 = llmin(ox + sx, (U32)x1 + 1);
        const U32 cy0 = llmax(oy, (U32)(y0 / CELL_M) * CELL_M), cy1 = llmin(oy + sy, (U32)y1 + 1);
        for (U32 x = cx0; x < cx1; x += CELL_M)
        {
            for (U32 y = cy0; y < cy1; y += CELL_M)
            {
                const U64 handle = to_region_handle(x, y);
                if (connected(handle) || cells >= MAX_QUADS)
                {
                    continue;
                }
                U32 tgx = 0, tgy = 0;
                LLWorldMipmap::globalToMipmap((F64)x, (F64)y, level, &tgx, &tgy);
                Tile& t = tiles[((U64)tgx << 32) | tgy];
                t.mGridX = tgx;
                t.mGridY = tgy;
                t.mCells.push_back(handle);
                ++cells;
            }
        }
    }

    std::unordered_set<U64> covered;
    mTiles.clear();
    mTiles.reserve(tiles.size());
    for (auto& kv : tiles)
    {
        Tile& t = kv.second;
        t.mTexture = LLWorldMap::getInstance()->getObjectsTile(t.mGridX, t.mGridY, level, true);
        if (t.mTexture.isNull())
        {
            continue;
        }
        // Keep it wanted at full size, as the world map view does for the tiles it draws.
        t.mTexture->setBoostLevel(LLGLTexture::BOOST_MAP);
        if (t.mTexture->hasGLTexture())
        {
            t.mDraw = WolfMapGlobe::instance().keyedTile(t.mTexture.get(), level, t.mGridX, t.mGridY);
        }
        if (t.mDraw.notNull())
        {
            covered.insert(t.mCells.begin(), t.mCells.end());
        }
        mTiles.push_back(std::move(t));
    }
    if (covered != mCovered)
    {
        mCovered.swap(covered);
        mWaterDirty = true;
    }
    rebuildBuffer();
}

void WolfFarGround::rebuildBuffer()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    mVB = nullptr;
    mVerts = 0;
    if (!agent_region)
    {
        return;
    }
    mBuiltOrigin = agent_region->getOriginGlobal();
    mBuiltWaterZ = agent_region->getWaterHeight();
    U32 quads = 0;
    for (const Tile& t : mTiles)
    {
        if (t.mDraw.notNull())
        {
            quads += (U32)t.mCells.size();
        }
    }
    quads = llmin(quads, MAX_QUADS);
    if (quads == 0)
    {
        return;
    }
    // The attributes LLDrawPoolSimple's batches carry for the diffuse programs, with the texture
    // index (position.w) 0: every tile is drawn on texture unit 0.
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (!vb->allocateBuffer(quads * 4, quads * 6))
    {
        LL_WARNS("WolfFarGround") << "could not allocate " << quads << " map tile quads" << LL_ENDL;
        return;
    }
    LLStrider<LLVector4a> pos;
    LLStrider<LLVector3> norm;
    LLStrider<LLVector2> uv;
    LLStrider<LLColor4U> col;
    LLStrider<U16> idx;
    if (!vb->getVertexStrider(pos) || !vb->getNormalStrider(norm) || !vb->getTexCoord0Strider(uv)
        || !vb->getColorStrider(col) || !vb->getIndexStrider(idx))
    {
        return;
    }
    const F32 tile_m = (F32)(CELL_M << (mLevel - 1));
    const F32 z = mBuiltWaterZ;
    U32 v = 0, i = 0;
    for (Tile& t : mTiles)
    {
        t.mStart = i;
        t.mCount = 0;
        if (t.mDraw.isNull())
        {
            continue;
        }
        const F64 tx = (F64)t.mGridX * CELL_M, ty = (F64)t.mGridY * CELL_M;
        for (U64 handle : t.mCells)
        {
            if (v / 4 >= quads)
            {
                break;
            }
            U32 cx = 0, cy = 0;
            from_region_handle(handle, &cx, &cy);
            // Map tiles are north up: v = 0 is the south edge, u = 0 the west (llworldmapview.cpp).
            const F32 u0 = (F32)((cx - tx) / tile_m), u1 = (F32)((cx + CELL_M - tx) / tile_m);
            const F32 v0 = (F32)((cy - ty) / tile_m), v1 = (F32)((cy + CELL_M - ty) / tile_m);
            const LLVector3 sw = gAgent.getPosAgentFromGlobal(LLVector3d(cx, cy, z));
            const LLVector3 se = gAgent.getPosAgentFromGlobal(LLVector3d(cx + CELL_M, cy, z));
            const LLVector3 ne = gAgent.getPosAgentFromGlobal(LLVector3d(cx + CELL_M, cy + CELL_M, z));
            const LLVector3 nw = gAgent.getPosAgentFromGlobal(LLVector3d(cx, cy + CELL_M, z));
            const LLVector3 corners[4] = { sw, se, ne, nw };
            const LLVector2 uvs[4] = { LLVector2(u0, v0), LLVector2(u1, v0), LLVector2(u1, v1), LLVector2(u0, v1) };
            for (S32 k = 0; k < 4; ++k)
            {
                (*pos++).set(corners[k].mV[VX], corners[k].mV[VY], corners[k].mV[VZ], 0.f);   // w = texture index 0
                *norm++ = LLVector3::z_axis;
                *uv++ = uvs[k];
                *col++ = LLColor4U(255, 255, 255, 0);   // alpha = no shininess (diffuseIndexedF.glsl)
            }
            // counter-clockwise seen from above, so the culled side is underneath
            *idx++ = (U16)v; *idx++ = (U16)(v + 1); *idx++ = (U16)(v + 2);
            *idx++ = (U16)v; *idx++ = (U16)(v + 2); *idx++ = (U16)(v + 3);
            v += 4;
            i += 6;
            t.mCount += 6;
        }
    }
    vb->unmapBuffer();
    mVB = vb;
    mVerts = v;
}

void WolfFarGround::renderDeferred()
{
    if (mVB.isNull() || mVerts == 0 || !enabled())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLGLDisable blend(GL_BLEND);
    // the alpha-cut diffuse program LLDrawPoolGrass uses: the tiles' water is cut away
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    mVB->setBuffer();
    for (const Tile& t : mTiles)
    {
        if (t.mCount == 0 || t.mDraw.isNull())
        {
            continue;
        }
        gGL.getTexUnit(0)->bindFast(t.mDraw);
        mVB->drawRange(LLRender::TRIANGLES, 0, mVerts - 1, t.mCount, t.mStart);
    }
    shader.unbind();
}
