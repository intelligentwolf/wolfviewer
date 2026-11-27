/**
 * @file wolfsunterrain.cpp
 * @brief WolfViewer: the terrain round the camera as height blocks, for the sun and moon behind hills (wolfsunterrain.h).
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

#include "wolfsunterrain.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <cmath>

// [T871 SUN BEHIND TERRAIN 2026-11-15] A texel of each block is four times the one before, as WolfWaterClearance's levels
// (wolfwaterfield.cpp TEXEL_M 1, 4, 16 m): 1 m out to 128 m from the camera, then 4, 16 and 64 m. 16384 m holds every
// height a region sends: the terrain comes only from within 4 km of the avatar (wolffarterrain.h, TerrainModule.cs:1679).
const F32 WolfSunTerrain::TEXEL_M[WolfSunTerrain::BLOCKS] = { 1.f, 4.f, 16.f, 64.f };

WolfSunTerrain::WolfSunTerrain()
{
}

WolfSunTerrain::~WolfSunTerrain()
{
    reset();
}

void WolfSunTerrain::reset()
{
    if (mTex)
    {
        LLImageGL::deleteTextures(1, &mTex);
        mTex = 0;
    }
    mTexAllocated = false;
    for (S32 b = 0; b < BLOCKS; ++b)
    {
        mHaveOrigin[b] = false;
        mValid[b] = false;
        mUploaded[b] = false;
        mStamp[b] = 0;
        mStampAt[b] = 0.0;
        mMaxZ[b] = NO_TERRAIN;
    }
    mBuf.clear();
}

bool WolfSunTerrain::isReady() const
{
    if (!mTex || !mTexAllocated)
    {
        return false;
    }
    for (S32 b = 0; b < BLOCKS; ++b)
    {
        if (mUploaded[b])
        {
            return true;
        }
    }
    return false;
}

LLVector2 WolfSunTerrain::blockOriginAgent(S32 b) const
{
    const LLVector3 a = gAgent.getPosAgentFromGlobal(mPubOrigin[b]);
    return LLVector2(a.mV[VX], a.mV[VY]);
}

F32 WolfSunTerrain::highestZ() const
{
    F32 top = NO_TERRAIN;
    for (S32 b = 0; b < BLOCKS; ++b)
    {
        if (mUploaded[b])
        {
            top = llmax(top, mMaxZ[b]);
        }
    }
    return top;
}

// Changes whenever a region comes or goes or any of its heights change (LLSurface::getTerrainRevision goes up at every
// height change, llsurface.h). Source: wolfwaterfield.cpp WolfWaterClearance::terrainStamp, without the water heights,
// which hide no light.
U64 WolfSunTerrain::terrainStamp() const
{
    U64 h = 1469598103934665603ULL;
    auto mix = [&h](U64 v) { h ^= v; h *= 1099511628211ULL; };
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionp->isAlive())
        {
            continue;
        }
        mix(regionp->getHandle());
        mix(regionp->getLand().hasZData() ? regionp->getLand().getTerrainRevision() : 0ULL);
    }
    return h;
}

// Block b's texels: each the highest drawn terrain in its own square [corner + i t, corner + (i + 1) t]. The drawn terrain
// is triangles between grid points (LLSurface::resolveHeightRegion), so the highest grid point of every grid cell touching
// the square is at or above it, and is it wherever a grid point lies in the square. A texel at least a patch wide takes the
// maxima of the patches it touches (LLSurfacePatch::getMaxZ), which is the same bound. Source: the walk over regions and
// patches is wolfwaterfield.cpp WolfWaterClearance::rasterTerrainMax's, without its water test (a hill hides the sun
// whatever the water does).
void WolfSunTerrain::raster(S32 b)
{
    const F64 t = TEXEL_M[b];
    mBuf.assign((size_t)N * N, NO_TERRAIN);
    const F64 ox = mOrigin[b].mdV[VX], oy = mOrigin[b].mdV[VY];
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionp->isAlive())
        {
            continue;
        }
        const LLSurface& surf = regionp->getLand();
        if (!surf.hasZData())
        {
            continue;
        }
        const F32 mpg = surf.getMetersPerGrid();
        const S32 gpp = surf.getGridsPerPatchEdge();
        const S32 ppe = surf.getPatchesPerEdge();
        if (mpg <= 0.f || gpp <= 0 || ppe <= 0)
        {
            continue;
        }
        const F64 patch_m = (F64)mpg * gpp;
        const LLVector3d rg = regionp->getOriginGlobal();
        const F64 wx0 = ox - rg.mdV[VX], wy0 = oy - rg.mdV[VY];   // the block's corner, region metres
        const S32 pi0 = llmax(0, (S32)floor(wx0 / patch_m)), pi1 = llmin(ppe - 1, (S32)floor((wx0 + N * t) / patch_m));
        const S32 pj0 = llmax(0, (S32)floor(wy0 / patch_m)), pj1 = llmin(ppe - 1, (S32)floor((wy0 + N * t) / patch_m));
        for (S32 pj = pj0; pj <= pj1; ++pj)
        {
            for (S32 pi = pi0; pi <= pi1; ++pi)
            {
                const LLSurfacePatch* patchp = surf.findPatch(pi, pj);
                if (!patchp || !patchp->getDataZ())
                {
                    continue;
                }
                const F64 px0 = pi * patch_m, py0 = pj * patch_m;
                // texels whose square overlaps the patch's: wx0 + i t < px0 + patch_m and wx0 + (i + 1) t > px0
                const S32 ti0 = llmax(0, (S32)floor((px0 - wx0) / t)), ti1 = llmin(N - 1, (S32)ceil((px0 + patch_m - wx0) / t) - 1);
                const S32 tj0 = llmax(0, (S32)floor((py0 - wy0) / t)), tj1 = llmin(N - 1, (S32)ceil((py0 + patch_m - wy0) / t) - 1);
                if (ti0 > ti1 || tj0 > tj1)
                {
                    continue;
                }
                if (t >= patch_m)
                {
                    const F32 pmax = patchp->getMaxZ();
                    for (S32 tj = tj0; tj <= tj1; ++tj)
                    {
                        for (S32 ti = ti0; ti <= ti1; ++ti)
                        {
                            F32& cell = mBuf[(size_t)tj * N + ti];
                            cell = llmax(cell, pmax);
                        }
                    }
                    continue;
                }
                // the patch's grid points, (gpp + 1)^2 at row stride getDataStride() (llsurfacepatch.h)
                const F32* z = patchp->getDataZ();
                const S32 stride = (S32)patchp->getDataStride();
                for (S32 tj = tj0; tj <= tj1; ++tj)
                {
                    const F64 sy0 = wy0 + tj * t - py0;
                    const S32 gj0 = llclamp((S32)floor(sy0 / mpg), 0, gpp), gj1 = llclamp((S32)ceil((sy0 + t) / mpg), 0, gpp);
                    for (S32 ti = ti0; ti <= ti1; ++ti)
                    {
                        const F64 sx0 = wx0 + ti * t - px0;
                        const S32 gi0 = llclamp((S32)floor(sx0 / mpg), 0, gpp), gi1 = llclamp((S32)ceil((sx0 + t) / mpg), 0, gpp);
                        F32& cell = mBuf[(size_t)tj * N + ti];
                        F32 m = cell;
                        for (S32 gj = gj0; gj <= gj1; ++gj)
                        {
                            for (S32 gi = gi0; gi <= gi1; ++gi)
                            {
                                m = llmax(m, z[gi + gj * stride]);
                            }
                        }
                        cell = m;
                    }
                }
            }
        }
    }
    F32 top = NO_TERRAIN;
    for (F32 v : mBuf)
    {
        top = llmax(top, v);
    }
    mMaxZ[b] = top;
}

void WolfSunTerrain::upload(S32 b)
{
    if (!mTex) mTexAllocated = false;
    if (mTexAllocated)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTex);
    }
    else
    {
        std::vector<F32> none((size_t)N * N * BLOCKS, NO_TERRAIN);
        // [VK S6 2026-11-26] in Vulkan-owned memory when the Vulkan renderer runs (LLImageGL::wolfVkManualImage2D makes
        // the name when there is none and leaves the texture bound on unit 0), else as before
        LLImageGL::wolfVkManualImage2D(mTex, GL_R32F, N, N * BLOCKS, GL_RED, GL_FLOAT, none.data());
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        mTexAllocated = true;
    }
    if (gGLManager.mIsNativeVulkan)
    {
        if (LLImageGL::sWolfVkNativeStore)
        {
            LLImageGL::sWolfVkNativeStore->uploadSubImage2D(mTex, 0, 0, b * N, N, N, N, GL_R32F, GL_RED, GL_FLOAT, mBuf.data(), false);
        }
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, b * N, N, N, GL_RED, GL_FLOAT, mBuf.data());
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    mPubOrigin[b] = mOrigin[b];
    mUploaded[b] = true;
}

void WolfSunTerrain::idle()
{
    LL_PROFILE_ZONE_SCOPED;
    if (!gAgent.getRegion())
    {
        if (mTex)
        {
            reset();
        }
        return;
    }
    const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const U64 stamp = terrainStamp();
    for (S32 b = 0; b < BLOCKS; ++b)
    {
        // Recentre when the camera is an eighth of the window from its centre; the corner snaps to the texel grid. Source:
        // wolfwaterfield.cpp WolfWaterClearance::idle's terrain blocks.
        const F64 t = TEXEL_M[b];
        const F64 half = N * t * 0.5;
        const bool far_off = !mHaveOrigin[b]
            || fabs(cam.mdV[VX] - (mOrigin[b].mdV[VX] + half)) > half * 0.25
            || fabs(cam.mdV[VY] - (mOrigin[b].mdV[VY] + half)) > half * 0.25;
        if (far_off)
        {
            mOrigin[b].mdV[VX] = floor((cam.mdV[VX] - half) / t) * t;
            mOrigin[b].mdV[VY] = floor((cam.mdV[VY] - half) / t) * t;
            mOrigin[b].mdV[VZ] = 0.0;
            mHaveOrigin[b] = true;
            mValid[b] = false;
        }
    }
    // One block a frame, finest first: until a block is rastered again the texture keeps its last one, with its own corner
    // (mPubOrigin), so what the shader reads always matches where it reads it.
    for (S32 b = 0; b < BLOCKS; ++b)
    {
        if (!mValid[b] || (stamp != mStamp[b] && now >= mStampAt[b]))
        {
            raster(b);
            upload(b);
            mValid[b] = true;
            mStamp[b] = stamp;
            mStampAt[b] = now + TERRAIN_INTERVAL_SECS;
            break;
        }
    }
}
