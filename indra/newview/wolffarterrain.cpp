/**
 * @file wolffarterrain.cpp
 * @brief WolfViewer: the grid's ground beyond the terrain the regions have sent, from coarse elevation tiles.
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

#include "wolffarterrain.h"

#include <algorithm>

#include "llagent.h"
#include "llagentcamera.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llimage.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvlcomposition.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmipmap.h"
#include "pipeline.h"
#include "wolfaltitudesky.h"
#include "wolfgrid.h"
#include "wolfmapglobe.h"
#include "noise.h"

namespace
{
    // Source: WolfSim WolfElevationModule / wolf-grid.com elev.php (the tile format and levels)
    constexpr S32 N = 33;                       // samples per tile side
    constexpr S32 Z_MIN = 4, Z_MAX = 8;
    constexpr S16 NO_DATA = -32768;
    constexpr U32 CELL_M = 256;
    constexpr size_t TILE_BYTES = 8 + N * N * 2;
    const char* URL = "https://wolf-grid.com/elev.php?rx=%u&ry=%u&z=%d&x=%u&y=%u";

    constexpr F64 REFRESH_SECS = 1.0;
    constexpr F32 MIN_HEIGHT_M = 100.f;        // as WolfFarGround: flying, the camera this high over the sea
    constexpr F32 SPLIT = 1.5f;                // a tile closer than this many of its own sizes is split in four
    constexpr S32 MAX_FETCHING = 6;
    constexpr S32 MAX_BUILDS_PER_FRAME = 8;
    constexpr F64 RETRY_MISSING_SECS = 600.0;  // a tile the region has not baked yet (elev.php caches 404s as long)
    constexpr F64 RETRY_ERROR_SECS = 60.0;
    constexpr F64 DROP_MESH_SECS = 120.0;      // a piece not wanted this long gives its vertex buffer back
    constexpr F64 FORGET_SECS = 900.0;         // ... and is forgotten after this long
    constexpr F64 TEXTURE_FORGET_SECS = 60.0;
    // Under the real terrain: the samples are the lower envelope already; sunk a little more with distance so
    // the two never share a depth.
    constexpr F32 SINK_M = 1.0f, SINK_PER_M = 0.002f, SINK_MAX_M = 60.f;
    // Ground baked from the region's detail textures: texels per piece, the detail copies' size, how many texels one
    // detail texture spans (a pattern, as the near terrain's repeat would only alias at 16 m a texel and coarser)
    constexpr S32 GROUND_TEXELS = 128;
    constexpr S32 DETAIL_SIZE = 64;
    constexpr S32 DETAIL_REPEAT_TEXELS = 16;
    constexpr S32 MAX_BAKES_PER_FRAME = 2;
    constexpr F64 BAKE_RETRY_SECS = 5.0;

    // Source: llvlcomposition.cpp:55 bilinear() - copied as is, argument order included, so the far ground's layers
    // fall where LLVLComposition::generateHeights puts the near ground's
    F32 comp_bilinear(const F32 v00, const F32 v01, const F32 v10, const F32 v11, const F32 x_frac, const F32 y_frac)
    {
        const F32 inv_x_frac = 1.f - x_frac;
        const F32 inv_y_frac = 1.f - y_frac;
        return inv_x_frac * inv_y_frac * v00 + x_frac * inv_y_frac * v10 + inv_x_frac * y_frac * v01 + x_frac * y_frac * v11;
    }

    F32 tileMetres(S32 z) { return (F32)(CELL_M << (z - 1)); }
}

bool WolfFarTerrain::enabled()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerFarTerrain", true);
    return on && WolfGrid::isWolfTerritories();
}

WolfFarTerrain::WolfFarTerrain()
{
}

WolfFarTerrain::~WolfFarTerrain()
{
}

bool WolfFarTerrain::coversCell(U32 x, U32 y) const
{
    return mCovered.count(to_region_handle(x, y)) > 0;
}

void WolfFarTerrain::clearAll()
{
    mDraw.clear();
    mCovered.clear();
}

void WolfFarTerrain::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextRefresh)
    {
        mNextRefresh = now + REFRESH_SECS;
        refresh();
    }
    if (mDraw.empty())
    {
        return;
    }
    // the map tiles drawn on the pieces: wanted at full size every frame, and their water cut away once loaded
    for (auto& kv : mTextures)
    {
        TileTex& t = kv.second;
        if (now - t.mLastWanted > REFRESH_SECS * 2 || t.mTexture.isNull())
        {
            continue;
        }
        t.mTexture->addTextureStats((F32)(LLWorldMipmap::MAP_TILE_SIZE * LLWorldMipmap::MAP_TILE_SIZE));
        if (t.mDraw.isNull() && t.mTexture->hasGLTexture())
        {
            t.mDraw = WolfMapGlobe::instance().keyedTile(t.mTexture.get(), std::get<0>(kv.first),
                                                        std::get<1>(kv.first), std::get<2>(kv.first));
        }
    }
    // meshes for pieces that arrived or whose frame moved (a few a frame)
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    // the agent frame the vertices are in: a crossing moves it, and so does a huge-region rebase (LLAgent::rebaseOrigin),
    // which leaves the region's own origin where it was
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    S32 builds = 0;
    for (const PieceKey& key : mDraw)
    {
        auto it = mPieces.find(key);
        if (it == mPieces.end())
        {
            continue;
        }
        Piece& p = it->second;
        if (p.mState == Piece::READY && !p.mBuildFailed && (p.mVB.isNull() || p.mBuiltOrigin != origin)
            && builds < MAX_BUILDS_PER_FRAME)
        {
            buildMesh(key, p);
            p.mBuildFailed = p.mVB.isNull();
            ++builds;
        }
    }
    // the region's own ground on the pieces of connected regions, a couple a frame (the map tile stands in until then)
    S32 bakes = 0;
    const F64 now_bake = LLFrameTimer::getElapsedSeconds();
    for (const PieceKey& key : mDraw)
    {
        if (bakes >= MAX_BAKES_PER_FRAME)
        {
            break;
        }
        auto it = mPieces.find(key);
        if (it == mPieces.end())
        {
            continue;
        }
        Piece& p = it->second;
        if (p.mState != Piece::READY || p.mGround.notNull() || now_bake < p.mBakeRetryAt)
        {
            continue;
        }
        if (bakeGround(key, p))
        {
            ++bakes;
        }
        else
        {
            p.mBakeRetryAt = now_bake + BAKE_RETRY_SECS;
        }
    }
}

void WolfFarTerrain::refresh()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!enabled() || !agent_region || !WolfAltitudeSky::agentFlying() || WolfAltitudeSky::cameraAltitude() < MIN_HEIGHT_M)
    {
        clearAll();
        return;
    }
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    const F32 far_clip = LLViewerCamera::getInstance()->getFar();
    const F32 height = llmax(0.f, (F32)cam.mdV[VZ] - agent_region->getWaterHeight());
    const F32 reach = sqrtf(llmax(0.f, far_clip * far_clip - height * height));   // as WolfFarGround
    if (reach < (F32)CELL_M)
    {
        clearAll();
        return;
    }
    const F64 x0 = llmax(0.0, cam.mdV[VX] - reach), x1 = cam.mdV[VX] + reach;
    const F64 y0 = llmax(0.0, cam.mdV[VY] - reach), y1 = cam.mdV[VY] + reach;
    LLWorldMap::getInstance()->updateRegions((S32)(x0 / CELL_M), (S32)(y0 / CELL_M), (S32)(x1 / CELL_M), (S32)(y1 / CELL_M));

    // the quadtree: z8 tiles over the reach, split nearer the camera down to z4
    std::vector<std::tuple<S32, U32, U32>> leaves;
    std::vector<std::tuple<S32, U32, U32>> stack;
    {
        const U32 span8 = 1u << (Z_MAX - 1);   // cells
        const U32 cx0 = ((U32)(x0 / CELL_M) / span8) * span8, cx1 = (U32)(x1 / CELL_M);
        const U32 cy0 = ((U32)(y0 / CELL_M) / span8) * span8, cy1 = (U32)(y1 / CELL_M);
        for (U32 tx = cx0; tx <= cx1; tx += span8)
        {
            for (U32 ty = cy0; ty <= cy1; ty += span8)
            {
                stack.emplace_back(Z_MAX, tx, ty);
            }
        }
    }
    while (!stack.empty())
    {
        auto [z, tx, ty] = stack.back();
        stack.pop_back();
        const F64 size = tileMetres(z);
        const F64 rx0 = (F64)tx * CELL_M, ry0 = (F64)ty * CELL_M;
        const F64 dx = llmax(0.0, llmax(rx0 - cam.mdV[VX], cam.mdV[VX] - (rx0 + size)));
        const F64 dy = llmax(0.0, llmax(ry0 - cam.mdV[VY], cam.mdV[VY] - (ry0 + size)));
        const F64 d = sqrt(dx * dx + dy * dy);
        if (d > reach)
        {
            continue;
        }
        if (z > Z_MIN && d < SPLIT * size)
        {
            const U32 half = 1u << (z - 2);
            stack.emplace_back(z - 1, tx, ty);
            stack.emplace_back(z - 1, tx + half, ty);
            stack.emplace_back(z - 1, tx, ty + half);
            stack.emplace_back(z - 1, tx + half, ty + half);
            continue;
        }
        leaves.emplace_back(z, tx, ty);
    }
    // nearest first: the fetch budget (MAX_FETCHING) goes to the ground closest to the camera
    auto leaf_dist = [&](const std::tuple<S32, U32, U32>& l) {
        const F64 half = tileMetres(std::get<0>(l)) * 0.5;
        const F64 dx = (F64)std::get<1>(l) * CELL_M + half - cam.mdV[VX], dy = (F64)std::get<2>(l) * CELL_M + half - cam.mdV[VY];
        return dx * dx + dy * dy;
    };
    std::sort(leaves.begin(), leaves.end(), [&](const auto& a, const auto& b) { return leaf_dist(a) < leaf_dist(b); });

    // the regions to draw: the connected ones at their own size, then the rest of the map. A map block carries the
    // size as a U16 (llworldmapmessage.cpp MapBlockReply SizeX/SizeY), so a region past 65,535 m (Ireland, 460,800 m)
    // arrives wrapped, and a huge region's map block is only fetched when the camera is near its corner
    struct FarRegion { U64 mHandle; U32 mX, mY, mW, mH; bool mConnected; };
    std::vector<FarRegion> regions;
    for (LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
    {
        U32 ox = 0, oy = 0;
        from_region_handle(r->getHandle(), &ox, &oy);
        const U32 cells = llmax((U32)r->getWidth(), CELL_M) / CELL_M;
        regions.push_back({ r->getHandle(), ox / CELL_M, oy / CELL_M, cells, cells, true });
    }
    for (const auto& kv : LLWorldMap::getInstance()->getRegionMap())
    {
        const LLSimInfo* info = kv.second;
        if (!info || LLWorld::getInstance()->getRegionFromHandle(kv.first))
        {
            continue;
        }
        U32 ox = 0, oy = 0;
        from_region_handle(kv.first, &ox, &oy);
        regions.push_back({ kv.first, ox / CELL_M, oy / CELL_M,
                            llmax((U32)info->getSizeX(), CELL_M) / CELL_M, llmax((U32)info->getSizeY(), CELL_M) / CELL_M, false });
    }

    // every region that overlaps a leaf gives its piece of it
    mDraw.clear();
    mCovered.clear();
    for (const auto& [z, tx, ty] : leaves)
    {
        const U32 span = 1u << (z - 1);
        const U32 tx1 = tx + span, ty1 = ty + span;
        bool any = false;
        for (const FarRegion& fr : regions)
        {
            const U32 rx = fr.mX, ry = fr.mY, rw = fr.mW, rh = fr.mH;
            if (rx + rw <= tx || rx >= tx1 || ry + rh <= ty || ry >= ty1)
            {
                continue;
            }
            const PieceKey key(rx, ry, z, tx, ty);
            Piece& p = mPieces[key];
            p.mLastWanted = now;
            p.mRegionX = rx; p.mRegionY = ry; p.mRegionW = rw; p.mRegionH = rh;
            if ((p.mState == Piece::NEW || (p.mState == Piece::MISSING && now >= p.mRetryAt)) && mFetching < MAX_FETCHING)
            {
                fetch(key);
            }
            if (p.mState != Piece::READY)
            {
                continue;
            }
            mDraw.push_back(key);
            any = true;
            // its cells, for WolfFarGround to leave alone: it draws flat tiles only for regions not connected, so
            // a connected one (Ireland, a million cells) is never listed. Only once this piece really draws (a mesh
            // and a picture for it), or the flat tile would go and leave a hole.
            if (fr.mConnected || p.mVB.isNull() || p.mIndices == 0 || p.mBuiltOrigin != gAgent.getAgentOriginGlobal())
            {
                continue;
            }
            if (p.mGround.isNull())
            {
                auto t = mTextures.find(std::make_tuple(z, tx, ty));
                if (t == mTextures.end() || t->second.mDraw.isNull())
                {
                    continue;
                }
            }
            for (U32 cx = llmax(rx, tx); cx < llmin(rx + rw, tx1); ++cx)
            {
                for (U32 cy = llmax(ry, ty); cy < llmin(ry + rh, ty1); ++cy)
                {
                    mCovered.insert(to_region_handle(cx * CELL_M, cy * CELL_M));
                }
            }
        }
        if (any)
        {
            TileTex& t = mTextures[std::make_tuple(z, tx, ty)];
            t.mLastWanted = now;
            if (t.mTexture.isNull())
            {
                t.mTexture = LLWorldMap::getInstance()->getObjectsTile(tx, ty, z, true);
                if (t.mTexture.notNull())
                {
                    t.mTexture->setBoostLevel(LLGLTexture::BOOST_MAP);
                }
            }
        }
    }

    // forget what has not been wanted for a while
    for (auto it = mPieces.begin(); it != mPieces.end();)
    {
        Piece& p = it->second;
        if (p.mState != Piece::FETCHING && now - p.mLastWanted > FORGET_SECS)
        {
            it = mPieces.erase(it);
            continue;
        }
        if (p.mVB.notNull() && now - p.mLastWanted > DROP_MESH_SECS)
        {
            p.mVB = nullptr;
        }
        ++it;
    }
    for (auto it = mTextures.begin(); it != mTextures.end();)
    {
        it = (now - it->second.mLastWanted > TEXTURE_FORGET_SECS) ? mTextures.erase(it) : std::next(it);
    }
}

void WolfFarTerrain::fetch(const PieceKey& key)
{
    Piece& p = mPieces[key];
    p.mState = Piece::FETCHING;
    ++mFetching;
    const std::string url = llformat(URL, std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key));
    LLCoros::instance().launch("WolfFarTerrain", [key, url]() { fetchCoro(key, url); });
}

// static
void WolfFarTerrain::fetchCoro(PieceKey key, std::string url)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfFarTerrain", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(15);
    LLSD result = adapter->getRawAndSuspend(request, url, options);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);

    if (!WolfFarTerrain::instanceExists())
    {
        return;
    }
    WolfFarTerrain& self = WolfFarTerrain::instance();
    self.mFetching = llmax(0, self.mFetching - 1);
    auto it = self.mPieces.find(key);
    if (it == self.mPieces.end())
    {
        return;
    }
    Piece& p = it->second;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLSD::Binary* body = result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW)
        ? &result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary() : nullptr;
    if (status && body && body->size() == TILE_BYTES && (*body)[0] == 'W' && (*body)[1] == 'E' && (*body)[2] == 'L'
        && (*body)[3] == 'V' && (*body)[4] == 1 && (S32)(*body)[5] == std::get<2>(key) && (*body)[6] == N && (*body)[7] == 0)
    {
        p.mHeights.resize(N * N);
        for (S32 i = 0; i < N * N; ++i)
        {
            p.mHeights[i] = (S16)((U16)(*body)[8 + i * 2] | ((U16)(*body)[9 + i * 2] << 8));   // little-endian
        }
        p.mState = Piece::READY;
        p.mVB = nullptr;
        p.mBuildFailed = false;
        self.mNextRefresh = 0.0;
        return;
    }
    // not baked yet (404), or the service or the region having a moment
    p.mState = Piece::MISSING;
    p.mRetryAt = now + (status.getType() == HTTP_NOT_FOUND ? RETRY_MISSING_SECS : RETRY_ERROR_SECS);
}

void WolfFarTerrain::buildMesh(const PieceKey& key, Piece& p)
{
    p.mVB = nullptr;
    p.mIndices = 0;
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || p.mHeights.size() != (size_t)(N * N))
    {
        return;
    }
    const S32 z = std::get<2>(key);
    const U32 tx = std::get<3>(key), ty = std::get<4>(key);
    const F64 size = tileMetres(z);
    const F64 step = size / (N - 1);
    const F64 gx0 = (F64)tx * CELL_M, gy0 = (F64)ty * CELL_M;
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    auto valid = [&](S32 i, S32 j) { return p.mHeights[j * N + i] != NO_DATA; };
    auto hgt = [&](S32 i, S32 j) { return (F32)p.mHeights[j * N + i] * 0.25f; };

    // vertices: every sample with data; then skirts down from the tile's edges (they hide the cracks between levels)
    std::vector<S32> vindex(N * N, -1);
    U32 verts = 0;
    for (S32 j = 0; j < N; ++j)
    {
        for (S32 i = 0; i < N; ++i)
        {
            if (valid(i, j))
            {
                vindex[j * N + i] = (S32)verts++;
            }
        }
    }
    U32 quads = 0;
    for (S32 j = 0; j < N - 1; ++j)
    {
        for (S32 i = 0; i < N - 1; ++i)
        {
            if (valid(i, j) && valid(i + 1, j) && valid(i, j + 1) && valid(i + 1, j + 1))
            {
                ++quads;
            }
        }
    }
    // the four edges, walked as (i, j) pairs along them
    std::vector<std::pair<S32, S32>> edge_pts;
    for (S32 k = 0; k < N; ++k) edge_pts.emplace_back(k, 0);
    for (S32 k = 0; k < N; ++k) edge_pts.emplace_back(N - 1, k);
    for (S32 k = 0; k < N; ++k) edge_pts.emplace_back(N - 1 - k, N - 1);
    for (S32 k = 0; k < N; ++k) edge_pts.emplace_back(0, N - 1 - k);
    U32 skirt_quads = 0;
    for (size_t e = 0; e + 1 < edge_pts.size(); ++e)
    {
        if (valid(edge_pts[e].first, edge_pts[e].second) && valid(edge_pts[e + 1].first, edge_pts[e + 1].second)
            && edge_pts[e] != edge_pts[e + 1])
        {
            ++skirt_quads;
        }
    }
    if (quads == 0)
    {
        return;
    }
    const U32 total_verts = verts + skirt_quads * 4;
    const U32 total_idx = quads * 6 + skirt_quads * 12;   // skirts both ways round
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (total_verts > 65535 || !vb->allocateBuffer(total_verts, total_idx))
    {
        LL_WARNS("WolfFarTerrain") << "could not allocate a far terrain piece of " << total_verts << " vertices" << LL_ENDL;
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
    auto world = [&](S32 i, S32 j, F32 drop) {
        const F64 gx = gx0 + i * step, gy = gy0 + j * step;
        const F64 ddx = gx - cam.mdV[VX], ddy = gy - cam.mdV[VY];
        const F32 sink = llmin(SINK_MAX_M, SINK_M + (F32)sqrt(ddx * ddx + ddy * ddy) * SINK_PER_M);
        return gAgent.getPosAgentFromGlobal(LLVector3d(gx, gy, (F64)(hgt(i, j) - sink - drop)));
    };
    auto put = [&](const LLVector3& v, const LLVector3& n, F32 u, F32 w) {
        (*pos++).set(v.mV[VX], v.mV[VY], v.mV[VZ], 0.f);   // w = texture index 0
        *norm++ = n;
        *uv++ = LLVector2(u, w);                          // map tiles: v = 0 is the south edge (llworldmapview.cpp)
        // opaque: diffuseAlphaMaskF.glsl multiplies the texture by this colour and discards below minimum_alpha
        *col++ = LLColor4U(255, 255, 255, 255);
    };
    for (S32 j = 0; j < N; ++j)
    {
        for (S32 i = 0; i < N; ++i)
        {
            if (!valid(i, j))
            {
                continue;
            }
            // the normal from the neighbours that have data
            const F32 hl = valid(i > 0 ? i - 1 : i, j) ? hgt(i > 0 ? i - 1 : i, j) : hgt(i, j);
            const F32 hr = valid(i < N - 1 ? i + 1 : i, j) ? hgt(i < N - 1 ? i + 1 : i, j) : hgt(i, j);
            const F32 hd = valid(i, j > 0 ? j - 1 : j) ? hgt(i, j > 0 ? j - 1 : j) : hgt(i, j);
            const F32 hu = valid(i, j < N - 1 ? j + 1 : j) ? hgt(i, j < N - 1 ? j + 1 : j) : hgt(i, j);
            LLVector3 n(hl - hr, hd - hu, 2.f * (F32)step);
            n.normVec();
            put(world(i, j, 0.f), n, (F32)i / (N - 1), (F32)j / (N - 1));
        }
    }
    U32 count = 0;
    for (S32 j = 0; j < N - 1; ++j)
    {
        for (S32 i = 0; i < N - 1; ++i)
        {
            if (!(valid(i, j) && valid(i + 1, j) && valid(i, j + 1) && valid(i + 1, j + 1)))
            {
                continue;
            }
            const U16 a = (U16)vindex[j * N + i], b = (U16)vindex[j * N + i + 1];
            const U16 c = (U16)vindex[(j + 1) * N + i + 1], d = (U16)vindex[(j + 1) * N + i];
            // counter-clockwise seen from above, so the culled side is underneath
            *idx++ = a; *idx++ = b; *idx++ = c;
            *idx++ = a; *idx++ = c; *idx++ = d;
            count += 6;
        }
    }
    const F32 drop = 20.f + (F32)step * 0.5f;
    U32 v = verts;
    for (size_t e = 0; e + 1 < edge_pts.size(); ++e)
    {
        const auto [i0, j0] = edge_pts[e];
        const auto [i1, j1] = edge_pts[e + 1];
        if (!(valid(i0, j0) && valid(i1, j1)) || edge_pts[e] == edge_pts[e + 1])
        {
            continue;
        }
        const F32 u0 = (F32)i0 / (N - 1), w0 = (F32)j0 / (N - 1), u1 = (F32)i1 / (N - 1), w1 = (F32)j1 / (N - 1);
        put(world(i0, j0, 0.f), LLVector3::z_axis, u0, w0);
        put(world(i1, j1, 0.f), LLVector3::z_axis, u1, w1);
        put(world(i1, j1, drop), LLVector3::z_axis, u1, w1);
        put(world(i0, j0, drop), LLVector3::z_axis, u0, w0);
        *idx++ = (U16)v; *idx++ = (U16)(v + 1); *idx++ = (U16)(v + 2);
        *idx++ = (U16)v; *idx++ = (U16)(v + 2); *idx++ = (U16)(v + 3);
        *idx++ = (U16)v; *idx++ = (U16)(v + 2); *idx++ = (U16)(v + 1);
        *idx++ = (U16)v; *idx++ = (U16)(v + 3); *idx++ = (U16)(v + 2);
        v += 4;
        count += 12;
    }
    vb->unmapBuffer();
    p.mVB = vb;
    p.mIndices = count;
    p.mBuiltOrigin = gAgent.getAgentOriginGlobal();
}

void WolfFarTerrain::renderDeferred()
{
    if (mDraw.empty() || !enabled())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLGLDisable blend(GL_BLEND);
    // the alpha-cut diffuse program WolfFarGround draws with: the map tiles' water is cut away
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    for (const PieceKey& key : mDraw)
    {
        auto it = mPieces.find(key);
        // a piece built in an agent frame that has since moved (crossing, huge-region rebase) waits for its rebuild
        // rather than being drawn out of place
        if (it == mPieces.end() || it->second.mVB.isNull() || it->second.mIndices == 0 || it->second.mBuiltOrigin != origin)
        {
            continue;
        }
        LLViewerTexture* tex = it->second.mGround.get();
        if (!tex)
        {
            auto t = mTextures.find(std::make_tuple(std::get<2>(key), std::get<3>(key), std::get<4>(key)));
            if (t == mTextures.end() || t->second.mDraw.isNull())
            {
                continue;
            }
            tex = t->second.mDraw.get();
        }
        gGL.getTexUnit(0)->bindFast(tex);
        it->second.mVB->setBuffer();
        it->second.mVB->drawRange(LLRender::TRIANGLES, 0, it->second.mVB->getNumVerts() - 1, it->second.mIndices, 0);
    }
    shader.unbind();
}

LLImageRaw* WolfFarTerrain::detailSample(LLViewerFetchedTexture* tex)
{
    auto found = mDetail.find(tex->getID());
    if (found != mDetail.end())
    {
        return found->second.get();
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    auto retry = mDetailRetryAt.find(tex->getID());
    if (retry != mDetailRetryAt.end() && now < retry->second)
    {
        return nullptr;
    }
    // loaded far enough to be worth copying (the near terrain is drawing with it)
    if (!tex->hasGLTexture() || tex->getDiscardLevel() < 0 || (tex->getFullWidth() >> tex->getDiscardLevel()) < DETAIL_SIZE)
    {
        return nullptr;
    }
    mDetailRetryAt[tex->getID()] = now + 10.0;
    // Source: gltfscenemanager.cpp:155-167 - the decoded pixels if kept, else the saved copy, else read back from the GPU
    LLImageRaw* raw = tex->getRawImage();
    if (!raw)
    {
        raw = tex->getSavedRawImage();
    }
    if (!raw)
    {
        tex->readbackRawImage();
        raw = tex->getRawImage();
    }
    if (!raw)
    {
        return nullptr;
    }
    LLPointer<LLImageRaw> small;
    {
        LLImageDataSharedLock lock(raw);
        if (!raw->getData() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        {
            return nullptr;
        }
        small = new LLImageRaw(raw->getData(), (U16)raw->getWidth(), (U16)raw->getHeight(), raw->getComponents());
    }
    if (!small->scale(DETAIL_SIZE, DETAIL_SIZE) || small->getWidth() != DETAIL_SIZE || small->getHeight() != DETAIL_SIZE
        || !small->getData())
    {
        return nullptr;   // bakeGround indexes it as DETAIL_SIZE square
    }
    mDetail[tex->getID()] = small;
    return small.get();
}

bool WolfFarTerrain::bakeGround(const PieceKey& key, Piece& p)
{
    const U32 rx = std::get<0>(key), ry = std::get<1>(key);
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(to_region_handle(rx * CELL_M, ry * CELL_M));
    if (!region || p.mHeights.size() != (size_t)(N * N))
    {
        return false;
    }
    LLVLComposition* comp = region->getComposition();
    if (!comp || !comp->getParamsReady() || comp->getMaterialType() != LLTerrainMaterials::Type::TEXTURE)
    {
        return false;   // a PBR-material estate keeps the map tile
    }
    LLImageRaw* detail[LLTerrainMaterials::ASSET_COUNT];
    for (S32 k = 0; k < LLTerrainMaterials::ASSET_COUNT; ++k)
    {
        if (comp->mDetailTextures[k].isNull() || !(detail[k] = detailSample(comp->mDetailTextures[k].get())))
        {
            return false;
        }
    }
    const S32 z = std::get<2>(key);
    const F64 size = tileMetres(z);
    const F64 gx0 = (F64)std::get<3>(key) * CELL_M, gy0 = (F64)std::get<4>(key) * CELL_M;
    const LLVector3d origin = region->getOriginGlobal();
    const F32 width = region->getWidth();
    const F32 start[4] = { comp->getStartHeight(LLVLComposition::SOUTHWEST), comp->getStartHeight(LLVLComposition::SOUTHEAST),
                           comp->getStartHeight(LLVLComposition::NORTHWEST), comp->getStartHeight(LLVLComposition::NORTHEAST) };
    const F32 range[4] = { comp->getHeightRange(LLVLComposition::SOUTHWEST), comp->getHeightRange(LLVLComposition::SOUTHEAST),
                           comp->getHeightRange(LLVLComposition::NORTHWEST), comp->getHeightRange(LLVLComposition::NORTHEAST) };
    // Source: llvlcomposition.cpp:494-500 LLVLComposition::generateHeights() - the noise that bends the layer lines
    const F32 slope_squared = 1.5f * 1.5f;
    const F32 xyScaleInv = 1.f / 4.9215f;
    const F32 zScaleInv = 1.f / 4.f;
    const F32 noise_magnitude = 2.f;

    LLPointer<LLImageRaw> out = new LLImageRaw((U16)GROUND_TEXELS, (U16)GROUND_TEXELS, 3);
    U8* o = out->getData();
    if (!o)
    {
        return false;
    }
    for (S32 b = 0; b < GROUND_TEXELS; ++b)
    {
        for (S32 a = 0; a < GROUND_TEXELS; ++a, o += 3)
        {
            // the height here, from the samples that have data
            const F32 fx = ((F32)a + 0.5f) / GROUND_TEXELS * (N - 1), fy = ((F32)b + 0.5f) / GROUND_TEXELS * (N - 1);
            const S32 i0 = llmin((S32)fx, N - 2), j0 = llmin((S32)fy, N - 2);
            const F32 u = fx - i0, w = fy - j0;
            F32 hsum = 0.f, wsum = 0.f;
            const F32 wts[4] = { (1.f - u) * (1.f - w), u * (1.f - w), (1.f - u) * w, u * w };
            const S16 hs[4] = { p.mHeights[j0 * N + i0], p.mHeights[j0 * N + i0 + 1], p.mHeights[(j0 + 1) * N + i0],
                                p.mHeights[(j0 + 1) * N + i0 + 1] };
            for (S32 k = 0; k < 4; ++k)
            {
                if (hs[k] != NO_DATA)
                {
                    hsum += wts[k] * (F32)hs[k] * 0.25f;
                    wsum += wts[k];
                }
            }
            if (wsum <= 0.f)
            {
                o[0] = o[1] = o[2] = 128;   // outside the region: never drawn (no triangle reaches it)
                continue;
            }
            const F32 height = hsum / wsum;
            const F64 gx = gx0 + ((F64)a + 0.5) / GROUND_TEXELS * size, gy = gy0 + ((F64)b + 0.5) / GROUND_TEXELS * size;
            const F32 xf = llclamp((F32)((gx - origin.mdV[VX]) / width), 0.f, 1.f);
            const F32 yf = llclamp((F32)((gy - origin.mdV[VY]) / width), 0.f, 1.f);
            // Source: llvlcomposition.cpp:526-559 LLVLComposition::generateHeights() - layer from height + noise
            const F32 start_height = comp_bilinear(start[0], start[1], start[2], start[3], xf, yf);
            const F32 height_range = comp_bilinear(range[0], range[1], range[2], range[3], xf, yf);
            F32 vec[3] = { (F32)(gx * xyScaleInv), (F32)(gy * xyScaleInv), height * zScaleInv };
            F32 vec1[3] = { vec[0] * 0.2222222222f, vec[1] * 0.2222222222f, vec[2] * 0.2222222222f };
            F32 twiddle = noise2(vec1) * 6.5f;
            twiddle += turbulence2(vec, 2) * slope_squared;
            twiddle *= noise_magnitude;
            F32 layer = (height + twiddle - start_height) * F32(LLTerrainMaterials::ASSET_COUNT) / llmax(height_range, 0.001f);
            layer = llclamp(layer, 0.f, 3.f);
            const S32 lo = llmin((S32)layer, 2);
            const F32 f = layer - lo;
            // the two layers' pixels, the detail textures repeated every DETAIL_REPEAT_TEXELS
            const S32 sx = (a % DETAIL_REPEAT_TEXELS) * DETAIL_SIZE / DETAIL_REPEAT_TEXELS;
            const S32 sy = (b % DETAIL_REPEAT_TEXELS) * DETAIL_SIZE / DETAIL_REPEAT_TEXELS;
            for (S32 c = 0; c < 3; ++c)
            {
                F32 v[2];
                for (S32 k = 0; k < 2; ++k)
                {
                    const LLImageRaw* d = detail[lo + k];
                    const S32 dc = d->getComponents();
                    v[k] = (F32)d->getData()[(sy * d->getWidth() + sx) * dc + (dc >= 3 ? c : 0)];
                }
                o[c] = (U8)ll_round(v[0] + (v[1] - v[0]) * f);
            }
        }
    }
    p.mGround = LLViewerTextureManager::getLocalTexture(out.get(), true);
    return p.mGround.notNull();
}
