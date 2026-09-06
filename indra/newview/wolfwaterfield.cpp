/**
 * @file wolfwaterfield.cpp
 * @brief WolfViewer: per-region water depth + open-water exposure fields for the water shader.
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

#include "wolfwaterfield.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <cmath>
#include <cstring>
#include <set>

WolfWaterField::WolfWaterField()
{
}

WolfWaterField::~WolfWaterField()
{
    reset();
}

void WolfWaterField::reset()
{
    for (auto& kv : mFields)
    {
        releaseField(kv.second);
    }
    mFields.clear();
}

void WolfWaterField::releaseField(Field& f)
{
    if (f.mDepthTex)
    {
        LLImageGL::deleteTextures(1, &f.mDepthTex);
        f.mDepthTex = 0;
    }
    if (f.mExpoTex)
    {
        LLImageGL::deleteTextures(1, &f.mExpoTex);
        f.mExpoTex = 0;
    }
    f.mReady = false;
}

const WolfWaterField::Field* WolfWaterField::get(const LLViewerRegion* regionp) const
{
    if (!regionp)
    {
        return nullptr;
    }
    auto it = mFields.find(regionp->getHandle());
    if (it == mFields.end() || !it->second.mReady)
    {
        return nullptr;
    }
    return &it->second;
}

// Source: wolfnaturalwater.cpp WolfNaturalWater::terrainStamp() — changes whenever any patch
// of the region's surface was updated (LLSurfacePatch::dirtyZ() stamps mLastUpdateTime).
U64 WolfWaterField::terrainStamp(LLViewerRegion* regionp)
{
    const LLSurface& land = regionp->getLand();
    const S32 per_edge = land.getPatchesPerEdge();
    U64 stamp = 1469598103934665603ull;
    for (S32 y = 0; y < per_edge; ++y)
    {
        for (S32 x = 0; x < per_edge; ++x)
        {
            const LLSurfacePatch* patchp = land.getPatch(x, y);
            if (patchp)
            {
                stamp = (stamp ^ patchp->getLastUpdateTime()) * 1099511628211ull;
            }
        }
    }
    return stamp;
}

void WolfWaterField::idle()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerWaterShoreField", true);
    if (!enabled)
    {
        if (!mFields.empty())
        {
            reset();
        }
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextCheck)
    {
        return;
    }
    mNextCheck = now + CHECK_INTERVAL_SECS;

    LLWorld* world = LLWorld::getInstance();
    if (!world)
    {
        return;
    }
    // Drop fields of regions that have gone.
    std::set<U64> live;
    for (LLViewerRegion* regionp : world->getRegionList())
    {
        if (regionp && regionp->isAlive())
        {
            live.insert(regionp->getHandle());
        }
    }
    for (auto it = mFields.begin(); it != mFields.end();)
    {
        if (live.find(it->first) == live.end())
        {
            releaseField(it->second);
            it = mFields.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Bake ONE region per check, the agent's own first: a bake is a few hundred thousand
    // height lookups, and nine regions at once would be a visible hitch.
    LLViewerRegion* pick = nullptr;
    LLViewerRegion* agent_region = gAgent.getRegion();
    std::vector<LLViewerRegion*> order;
    if (agent_region)
    {
        order.push_back(agent_region);
    }
    for (LLViewerRegion* regionp : world->getRegionList())
    {
        if (regionp && regionp != agent_region)
        {
            order.push_back(regionp);
        }
    }
    for (LLViewerRegion* regionp : order)
    {
        if (!regionp || !regionp->isAlive())
        {
            continue;
        }
        // Source: wolfnaturalwater.cpp idle() — the land is not there until the grid is.
        if (regionp->getLand().getGridsPerEdge() < 4)
        {
            continue;
        }
        Field& f = mFields[regionp->getHandle()];
        const U64 stamp = terrainStamp(regionp);
        const bool stale = !f.mReady || f.mStamp != stamp || (now - f.mBakedAt) > REBAKE_SECS
                        || f.mWaterLevel != regionp->getWaterHeight();
        if (stale)
        {
            pick = regionp;
            f.mStamp = stamp;
            break;
        }
    }
    if (pick)
    {
        bake(pick, mFields[pick->getHandle()]);
    }
}

bool WolfWaterField::heightAt(LLViewerRegion* regionp, F32 px, F32 py, F32& out)
{
    const F32 w = regionp->getWidth();
    if (px >= 0.f && px <= w && py >= 0.f && py <= w)
    {
        out = regionp->getLand().resolveHeightRegion(px, py);
        return true;
    }
    // Source: terrain_manager.js _heightAtScene — neighbour heightfields answer off-region
    // points; no region = no data = open water.
    LLVector3d global = regionp->getOriginGlobal();
    global.mdV[VX] += px;
    global.mdV[VY] += py;
    LLViewerRegion* other = LLWorld::getInstance()->getRegionFromPosGlobal(global);
    if (!other || !other->isAlive() || other->getLand().getGridsPerEdge() < 4)
    {
        return false;
    }
    out = other->getLand().resolveHeightGlobal(global);
    return true;
}

void WolfWaterField::upload(U32& tex, S32 w, S32 h, U32 internal_format, U32 format, const F32* data)
{
    // Source: pipeline.cpp:1613-1618 (light function texture) — a raw GL texture from
    // float data via LLImageGL::setManualImage, bound through the tex unit.
    if (!tex)
    {
        LLImageGL::generateTextures(1, &tex);
    }
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, tex);
    LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, internal_format,
                              w, h, format, GL_FLOAT, data, false);
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
    gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

void WolfWaterField::bake(LLViewerRegion* regionp, Field& f)
{
    LL_PROFILE_ZONE_SCOPED;
    const F32 sx = regionp->getWidth();
    const F32 sy = regionp->getWidth();
    const F32 water_level = regionp->getWaterHeight();
    const LLSurface& land = regionp->getLand();

    // Source: terrain_manager.js _bakeWaterDepthTexture() — pass 1: raw heights (bilinear
    // region sample); pass 2: separable box blur (radius 6 texels) -> smoothed field, so
    // the shader's wave phase does not jitter texel to texel on rough seabeds; pass 3:
    // pack + central-difference gradient (+-3 texels) of the smoothed field, per-metre
    // slope, confidence 0..1 over a 0..5% grade, FETCH suppression by marching seaward
    // (4 m steps, 96 m for full strength) through the raw heights, neighbours included.
    const S32 R = RES;
    mH.assign((size_t)R * R, 0.f);
    mTmp.assign((size_t)R * R, 0.f);
    mSm.assign((size_t)R * R, 0.f);
    mDepthData.assign((size_t)R * R * 4, 0.f);
    for (S32 j = 0; j < R; ++j)
    {
        const F32 y = j * sy / (R - 1);
        for (S32 i = 0; i < R; ++i)
        {
            mH[j * R + i] = land.resolveHeightRegion(i * sx / (R - 1), y);
        }
    }
    const S32 RAD = 6;
    for (S32 j = 0; j < R; ++j)
    {
        const S32 row = j * R;
        for (S32 i = 0; i < R; ++i)
        {
            const S32 lo = llmax(i - RAD, 0), hi = llmin(i + RAD, R - 1);
            F32 acc = 0.f;
            for (S32 k = lo; k <= hi; ++k) acc += mH[row + k];
            mTmp[row + i] = acc / (hi - lo + 1);
        }
    }
    for (S32 i = 0; i < R; ++i)
    {
        for (S32 j = 0; j < R; ++j)
        {
            const S32 lo = llmax(j - RAD, 0), hi = llmin(j + RAD, R - 1);
            F32 acc = 0.f;
            for (S32 k = lo; k <= hi; ++k) acc += mTmp[k * R + i];
            mSm[j * R + i] = acc / (hi - lo + 1);
        }
    }
    const F32 texel_x = sx / (R - 1), texel_y = sy / (R - 1);
    const F32 FETCH_STEP = 4.f;
    const F32 FETCH_FULL = 96.f;
    for (S32 j = 0; j < R; ++j)
    {
        for (S32 i = 0; i < R; ++i)
        {
            const S32 idx = j * R + i;
            const S32 ip = llmin(i + 3, R - 1), im = llmax(i - 3, 0);
            const S32 jp = llmin(j + 3, R - 1), jm = llmax(j - 3, 0);
            const F32 gx = (mSm[j * R + ip] - mSm[j * R + im]) / ((ip - im) * texel_x);
            const F32 gy = (mSm[jp * R + i] - mSm[jm * R + i]) / ((jp - jm) * texel_y);
            const F32 mag = sqrtf(gx * gx + gy * gy);
            F32* o = &mDepthData[(size_t)idx * 4];
            o[0] = mH[idx];
            F32 conf = 0.f;
            if (mag > 1e-6f)
            {
                conf = llmin(mag / 0.05f, 1.f);
                if (conf > 0.02f)
                {
                    const F32 ux = -(gx / mag), uy = -(gy / mag);   // seaward unit
                    const F32 px0 = i * texel_x, py0 = j * texel_y;
                    F32 open = FETCH_FULL;
                    for (F32 s = FETCH_STEP; s <= FETCH_FULL; s += FETCH_STEP)
                    {
                        const F32 px = px0 + ux * s, py = py0 + uy * s;
                        if (px < 0.f || py < 0.f || px > sx || py > sy)
                        {
                            F32 hn;
                            if (!heightAt(regionp, px, py, hn))
                            {
                                break;   // no region: open water, the march ends full
                            }
                            if (hn >= water_level)
                            {
                                open = s - FETCH_STEP;
                                break;
                            }
                            continue;
                        }
                        const S32 ti = llclamp((S32)ll_round(px / texel_x), 0, R - 1);
                        const S32 tj = llclamp((S32)ll_round(py / texel_y), 0, R - 1);
                        if (mH[tj * R + ti] >= water_level)
                        {
                            open = s - FETCH_STEP;
                            break;
                        }
                    }
                    conf *= llmin(open / FETCH_FULL, 1.f);
                }
            }
            if (conf > 0.f)
            {
                o[1] = (gx / mag) * conf;
                o[2] = (gy / mag) * conf;
            }
            else
            {
                o[1] = 0.f;
                o[2] = 0.f;
            }
            o[3] = mSm[idx];
        }
    }
    upload(f.mDepthTex, R, R, GL_RGBA32F, GL_RGBA, mDepthData.data());

    // Source: terrain_manager.js _bakeSwellExposure() — exposure = smoothstep(6 m, 150 m,
    // distance to nearest land) over a 3x-region span centred on the region, land being
    // terrain at or above the water level in this region or any neighbour; space no
    // region covers counts as open water. Two-pass chamfer distance transform in metres.
    const S32 E = ERES;
    const F32 ex0 = -sx, ey0 = -sy;
    const F32 esx = 3.f * sx, esy = 3.f * sy;
    const F32 etx = esx / (E - 1), ety = esy / (E - 1);
    const F32 diag = sqrtf(etx * etx + ety * ety);
    const F32 BIG = 1e9f;
    mDist.assign((size_t)E * E, BIG);
    for (S32 j = 0; j < E; ++j)
    {
        const F32 py = ey0 + j * ety;
        for (S32 i = 0; i < E; ++i)
        {
            F32 hh;
            const bool has = heightAt(regionp, ex0 + i * etx, py, hh);
            mDist[j * E + i] = (has && hh >= water_level) ? 0.f : BIG;
        }
    }
    for (S32 j = 0; j < E; ++j)
    {
        const S32 row = j * E, up = row - E;
        for (S32 i = 0; i < E; ++i)
        {
            F32 d = mDist[row + i];
            if (i > 0 && mDist[row + i - 1] + etx < d) d = mDist[row + i - 1] + etx;
            if (j > 0)
            {
                if (mDist[up + i] + ety < d) d = mDist[up + i] + ety;
                if (i > 0 && mDist[up + i - 1] + diag < d) d = mDist[up + i - 1] + diag;
                if (i < E - 1 && mDist[up + i + 1] + diag < d) d = mDist[up + i + 1] + diag;
            }
            mDist[row + i] = d;
        }
    }
    for (S32 j = E - 1; j >= 0; --j)
    {
        const S32 row = j * E, dn = row + E;
        for (S32 i = E - 1; i >= 0; --i)
        {
            F32 d = mDist[row + i];
            if (i < E - 1 && mDist[row + i + 1] + etx < d) d = mDist[row + i + 1] + etx;
            if (j < E - 1)
            {
                if (mDist[dn + i] + ety < d) d = mDist[dn + i] + ety;
                if (i < E - 1 && mDist[dn + i + 1] + diag < d) d = mDist[dn + i + 1] + diag;
                if (i > 0 && mDist[dn + i - 1] + diag < d) d = mDist[dn + i - 1] + diag;
            }
            mDist[row + i] = d;
        }
    }
    mExpoData.assign((size_t)E * E, 1.f);
    for (S32 k = 0; k < E * E; ++k)
    {
        const F32 t = llclamp((mDist[k] - 6.f) / 144.f, 0.f, 1.f);   // 6 m .. 150 m
        mExpoData[k] = t * t * (3.f - 2.f * t);
    }
    upload(f.mExpoTex, E, E, GL_R32F, GL_RED, mExpoData.data());

    f.mSizeX = sx;
    f.mSizeY = sy;
    f.mWaterLevel = water_level;
    f.mExpoX0 = ex0;
    f.mExpoY0 = ey0;
    f.mExpoSX = esx;
    f.mExpoSY = esy;
    f.mBakedAt = LLFrameTimer::getElapsedSeconds();
    f.mReady = true;
}
