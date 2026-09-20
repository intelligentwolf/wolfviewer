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
#include "llviewercamera.h"   // <WolfViewer 2026-09-20/> fieldWindow
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "wolfwavezones.h"   // [WAVES 2026-09-07]

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
    if (f.mZoneTex)
    {
        LLImageGL::deleteTextures(1, &f.mZoneTex);
        f.mZoneTex = 0;
    }
    f.mReady = false;
    f.mDepth.clear();
    f.mExpo.clear();
}

// Source: terrain_manager.js _swellExposureAt(x, y):
//   const u = (x - e.x0) / e.sx, v = (y - e.y0) / e.sy;
//   if (u < 0 || u > 1 || v < 0 || v > 1) return 1.0;
//   i = clamp(round(u * (res - 1))), j = clamp(round(v * (res - 1))); return data[j * res + i]
F32 WolfWaterField::exposureAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mExpo.size() != (size_t)ERES * ERES * 4)
    {
        return 1.f;
    }
    const F32 u = (rx - f.mExpoX0) / f.mExpoSX;
    const F32 v = (ry - f.mExpoY0) / f.mExpoSY;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
    {
        return 1.f;
    }
    const S32 i = llclamp((S32)ll_round(u * (ERES - 1)), 0, ERES - 1);
    const S32 j = llclamp((S32)ll_round(v * (ERES - 1)), 0, ERES - 1);
    return f.mExpo[((size_t)j * ERES + i) * 4];   // RGBA: R = exposure
}

// Source: terrain_manager.js _waveSampleCPU() — the shore-breaker depth read:
//   if (x >= 0 && y >= 0 && x <= rs.x && y <= rs.y) {
//     ti = clamp(round(x * (RES - 1) / rs.x)), tj = clamp(round(y * (RES - 1) / rs.y));
//     o4 = (tj * RES + ti) * 4; g = data[o4 + 1], b = data[o4 + 2], a = data[o4 + 3] ... }
// [WAVES 2026-09-07] Source: wave_zones.js zoneAt().
F32 WolfWaterField::zoneAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mZoneW <= 0 || f.mZone.size() != (size_t)f.mZoneW * f.mZoneH)
    {
        return WolfWaveZones::OPEN_ENERGY;
    }
    const F32 u = (rx - f.mZoneX0) / f.mZoneSX;
    const F32 v = (ry - f.mZoneY0) / f.mZoneSY;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
    {
        return WolfWaveZones::OPEN_ENERGY;
    }
    const S32 i = llclamp((S32)(u * f.mZoneW), 0, f.mZoneW - 1);
    const S32 j = llclamp((S32)(v * f.mZoneH), 0, f.mZoneH - 1);
    return f.mZone[(size_t)j * f.mZoneW + i];
}

F32 WolfWaterField::distanceAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mExpo.size() != (size_t)ERES * ERES * 4) return 4000.f;
    const F32 u = (rx - f.mExpoX0) / f.mExpoSX, v = (ry - f.mExpoY0) / f.mExpoSY;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return 4000.f;
    const F32 fx = u * (ERES - 1), fy = v * (ERES - 1);
    const S32 i0 = llmin(ERES - 2, (S32)fx), j0 = llmin(ERES - 2, (S32)fy);
    const F32 tx = fx - i0, ty = fy - j0;
    auto d = [&](S32 i, S32 j) { return f.mExpo[((size_t)j * ERES + i) * 4 + 1]; };
    return (d(i0, j0) * (1 - tx) + d(i0 + 1, j0) * tx) * (1 - ty) + (d(i0, j0 + 1) * (1 - tx) + d(i0 + 1, j0 + 1) * tx) * ty;
}

// <WolfViewer 2026-09-20/> distanceAt on the B channel. Source: terrain_manager.js _swellOpenDistAt.
F32 WolfWaterField::openDistanceAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mExpo.size() != (size_t)ERES * ERES * 4) return 4000.f;
    const F32 u = (rx - f.mExpoX0) / f.mExpoSX, v = (ry - f.mExpoY0) / f.mExpoSY;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return 4000.f;
    const F32 fx = u * (ERES - 1), fy = v * (ERES - 1);
    const S32 i0 = llmin(ERES - 2, (S32)fx), j0 = llmin(ERES - 2, (S32)fy);
    const F32 tx = fx - i0, ty = fy - j0;
    auto d = [&](S32 i, S32 j) { return f.mExpo[((size_t)j * ERES + i) * 4 + 2]; };
    return (d(i0, j0) * (1 - tx) + d(i0 + 1, j0) * tx) * (1 - ty) + (d(i0, j0 + 1) * (1 - tx) + d(i0 + 1, j0 + 1) * tx) * ty;
}

void WolfWaterField::invalidate()
{
    // One region is baked per check (idle), so every field is marked stale and they follow
    // one another, the agent's own first — exactly the order idle() already walks.
    for (auto& kv : mFields)
    {
        kv.second.mStamp = 0;
        kv.second.mPendingSince = -1e9;   // <WolfViewer 2026-09-20/> an explicit invalidate does not wait to settle
    }
    // At once, not at the next 2 s check: an editor preview must show as it is painted.
    mNextCheck = 0.0;
    LL_INFOS("WolfWaterField") << "invalidated " << mFields.size() << " field(s)" << LL_ENDL;
    mRebakeAll = false;
    mNextCheck = 0.0;
}

bool WolfWaterField::depthAt(const Field& f, F32 rx, F32 ry, F32 out[4])
{
    if (!f.mReady || f.mDepth.size() != (size_t)RES * RES * 4)
    {
        return false;
    }
    // <WolfViewer 2026-09-20> relative to the field's origin (a camera window on a huge region)
    rx -= f.mX0;
    ry -= f.mY0;
    if (rx < 0.f || ry < 0.f || rx > f.mSizeX || ry > f.mSizeY)
    {
        return false;
    }
    const S32 ti = llclamp((S32)ll_round(rx * (RES - 1) / f.mSizeX), 0, RES - 1);
    const S32 tj = llclamp((S32)ll_round(ry * (RES - 1) / f.mSizeY), 0, RES - 1);
    const F32* t = &f.mDepth[((size_t)tj * RES + ti) * 4];
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
    out[3] = t[3];
    return true;
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
// <WolfViewer 2026-09-20/> see wolfwaterfield.h
U64 WolfWaterField::terrainStampIn(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy)
{
    const LLSurface& land = regionp->getLand();
    const S32 per_edge = land.getPatchesPerEdge();
    if (per_edge <= 0) return 0;
    const F32 patch_m = regionp->getWidth() / (F32)per_edge;
    const S32 px0 = llclamp((S32)floorf(x0 / patch_m), 0, per_edge - 1);
    const S32 py0 = llclamp((S32)floorf(y0 / patch_m), 0, per_edge - 1);
    const S32 px1 = llclamp((S32)ceilf((x0 + sx) / patch_m), 0, per_edge - 1);
    const S32 py1 = llclamp((S32)ceilf((y0 + sy) / patch_m), 0, per_edge - 1);
    U64 stamp = 1469598103934665603ull;
    for (S32 y = py0; y <= py1; ++y)
    {
        for (S32 x = px0; x <= px1; ++x)
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
    // [WAVES 2026-09-07] The zone layouts are fetched from here: same cadence, same owner.
    WolfWaveZones::instance().idle();
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
        // [2026-09-10] Three guards from Paul's Dire Wolf teleport (25,600 m: "the region
        // freezes for about 20 seconds" ... "the waves are not right and flickering really
        // badly"). The log showed "baked Dire Wolf" every 2 s from arrival on:
        //  1. (2026-09-20: superseded by WINDOW_M, below) A region wider than 4096 m got NO field. RES x RES texels over
        //     25,600 m are 100 m of ground each — no beach can be read from that, and the
        //     shader's fallback with no field (open sea, no breakers, no swash) is stable.
        //  2. No bake until the terrain has height data (LLSurface::hasZData): before that
        //     every patch reads 0, the field is fiction, and it changes on every packet.
        //  3. At most one bake per MIN_REBAKE_SECS per region however often the terrain
        //     stamp changes: patches stream in for a minute and the sea must not re-shape
        //     itself on every one of them.
        // <WolfViewer 2026-09-20> Guard 1 is gone: a region wider than WINDOW_M gets a
        // camera-following WINDOW field instead of none (wolfwaterfield.h WINDOW_M). A
        // window that no longer holds the camera in its inner half is stale like changed
        // terrain, under the same MIN_REBAKE_SECS pace. Guards 2 and 3 stand.
        if (!regionp->getLand().hasZData())
        {
            continue;
        }
        Field& f = mFields[regionp->getHandle()];
        F32 wx0, wy0, wsx, wsy;
        fieldWindow(regionp, f.mReady ? &f : nullptr, wx0, wy0, wsx, wsy);
        const bool moved = f.mReady && (wx0 != f.mX0 || wy0 != f.mY0);
        // <WolfViewer 2026-09-20> the stamp covers the window's patches (plus a margin), and a
        // change counts only once it has held for STAMP_SETTLE_SECS (see wolfwaterfield.h).
        const bool windowed = wsx < regionp->getWidth();
        const U64 stamp = windowed ? terrainStampIn(regionp, wx0 - STAMP_MARGIN_M, wy0 - STAMP_MARGIN_M,
                                                    wsx + 2.f * STAMP_MARGIN_M, wsy + 2.f * STAMP_MARGIN_M)
                                   : terrainStamp(regionp);
        if (stamp != f.mPendingStamp)
        {
            f.mPendingStamp = stamp;
            f.mPendingSince = now;
        }
        const bool terrain_changed = f.mStamp != stamp && (now - f.mPendingSince) >= STAMP_SETTLE_SECS;
        const bool stale = !f.mReady || terrain_changed || (now - f.mBakedAt) > REBAKE_SECS
                        || f.mWaterLevel != regionp->getWaterHeight() || moved;
        if (stale && (!f.mReady || now - f.mBakedAt >= MIN_REBAKE_SECS))
        {
            pick = regionp;
            f.mStamp = stamp;
            break;
        }
    }
    if (pick)
    {
        const F64 t0 = LLFrameTimer::getElapsedSeconds();
        bake(pick, mFields[pick->getHandle()]);
        LL_INFOS("WolfWaterField") << "baked " << pick->getName() << " in "
                                   << (S32)((LLFrameTimer::getElapsedSeconds() - t0) * 1000.0) << " ms" << LL_ENDL;
    }
}

// <WolfViewer 2026-09-20> see wolfwaterfield.h. Source: WolfStorm terrain_manager.js
// _waterFieldWindow / _tickWaterFieldWindow and terrain_paint.js windowOriginFor / recentre.
void WolfWaterField::fieldWindow(const LLViewerRegion* regionp, const Field* current, F32& x0, F32& y0, F32& sx, F32& sy)
{
    const F32 rw = regionp->getWidth();
    if (rw <= WINDOW_M)
    {
        x0 = 0.f; y0 = 0.f; sx = rw; sy = rw;
        return;
    }
    sx = WINDOW_M; sy = WINDOW_M;
    // the camera, region metres (agent space minus the region's agent-space origin)
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin() - regionp->getOriginAgent();
    const F32 cx = cam.mV[VX], cy = cam.mV[VY];
    if (current && current->mSizeX == sx && current->mSizeY == sy)
    {
        const F32 mx = current->mX0 + sx * 0.5f, my = current->mY0 + sy * 0.5f;
        if (fabsf(cx - mx) <= sx * 0.25f && fabsf(cy - my) <= sy * 0.25f)
        {
            x0 = current->mX0; y0 = current->mY0;   // still in the inner half: keep it
            return;
        }
    }
    const F32 g = WINDOW_M / 8.f;
    x0 = llclamp((F32)ll_round((cx - sx * 0.5f) / g) * g, 0.f, rw - sx);
    y0 = llclamp((F32)ll_round((cy - sy * 0.5f) / g) * g, 0.f, rw - sy);
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
    // <WolfViewer 2026-09-20> sx/sy are the FIELD's size (the window's on a huge region) and
    // ox/oy its region-space origin: texel (i, j) is the region point (ox + i * texel,
    // oy + j * texel). Whole region below WINDOW_M — ox = oy = 0, byte-identical to before.
    F32 ox, oy, sx, sy;
    fieldWindow(regionp, f.mReady ? &f : nullptr, ox, oy, sx, sy);
    f.mX0 = ox;
    f.mY0 = oy;
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
        const F32 y = oy + j * sy / (R - 1);
        for (S32 i = 0; i < R; ++i)
        {
            mH[j * R + i] = land.resolveHeightRegion(ox + i * sx / (R - 1), y);
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
                    const F32 px0 = ox + i * texel_x, py0 = oy + j * texel_y;   // region space
                    F32 open = FETCH_FULL;
                    for (F32 s = FETCH_STEP; s <= FETCH_FULL; s += FETCH_STEP)
                    {
                        const F32 px = px0 + ux * s, py = py0 + uy * s;
                        // <WolfViewer 2026-09-20> past the FIELD: heightAt reads this region's
                        // own grid or a neighbour's for points outside the window too.
                        if (px < ox || py < oy || px > ox + sx || py > oy + sy)
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
                        const S32 ti = llclamp((S32)ll_round((px - ox) / texel_x), 0, R - 1);
                        const S32 tj = llclamp((S32)ll_round((py - oy) / texel_y), 0, R - 1);
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
    // <WolfViewer 2026-09-20> 3x the FIELD (window) — 3x the region below WINDOW_M as before.
    const F32 ex0 = ox - sx, ey0 = oy - sy;
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
    // <WolfViewer 2026-09-20> B = the chamfer DISTANCE FROM THE OPEN SEA (every texel 150 m or
    // more from land is sea; 0 there, growing toward the shore). The surf reads THIS for its
    // coordinate and direction: iso-lines of distance-to-land run parallel to the NEAREST
    // bank, so in a bay, beside a pier or a rock the surf turned sideways and rings formed
    // round every islet (Paul: "still not going towards the land", "the scale of the
    // circles"). Iso-lines of distance-from-the-sea are parallel to the sea front and every
    // wave travels landward. Source: terrain_manager.js _bakeSwellExposure (same passes).
    mOpen.assign((size_t)E * E, BIG);
    for (S32 k = 0; k < E * E; ++k) if (mDist[k] >= 150.f) mOpen[k] = 0.f;
    for (S32 j = 0; j < E; ++j)
    {
        const S32 row = j * E, up = row - E;
        for (S32 i = 0; i < E; ++i)
        {
            F32 d = mOpen[row + i];
            if (i > 0 && mOpen[row + i - 1] + etx < d) d = mOpen[row + i - 1] + etx;
            if (j > 0)
            {
                if (mOpen[up + i] + ety < d) d = mOpen[up + i] + ety;
                if (i > 0 && mOpen[up + i - 1] + diag < d) d = mOpen[up + i - 1] + diag;
                if (i < E - 1 && mOpen[up + i + 1] + diag < d) d = mOpen[up + i + 1] + diag;
            }
            mOpen[row + i] = d;
        }
    }
    for (S32 j = E - 1; j >= 0; --j)
    {
        const S32 row = j * E, dn = row + E;
        for (S32 i = E - 1; i >= 0; --i)
        {
            F32 d = mOpen[row + i];
            if (i < E - 1 && mOpen[row + i + 1] + etx < d) d = mOpen[row + i + 1] + etx;
            if (j < E - 1)
            {
                if (mOpen[dn + i] + ety < d) d = mOpen[dn + i] + ety;
                if (i < E - 1 && mOpen[dn + i + 1] + diag < d) d = mOpen[dn + i + 1] + diag;
                if (i > 0 && mOpen[dn + i - 1] + diag < d) d = mOpen[dn + i - 1] + diag;
            }
            mOpen[row + i] = d;
        }
    }
    // [SURF 2026-09-07] R = exposure (every existing reader unchanged), G = the chamfer
    // DISTANCE TO LAND in metres (capped at 4000), B = distance from the open sea, A = 1
    // (RGBA32F: RGB32F is not filterable). Source: terrain_manager.js _bakeSwellExposure.
    mExpoData.assign((size_t)E * E * 4, 1.f);
    for (S32 k = 0; k < E * E; ++k)
    {
        const F32 t = llclamp((mDist[k] - 6.f) / 144.f, 0.f, 1.f);   // 6 m .. 150 m
        mExpoData[(size_t)k * 4] = t * t * (3.f - 2.f * t);
        mExpoData[(size_t)k * 4 + 1] = llmin(mDist[k], 4000.f);
        mExpoData[(size_t)k * 4 + 2] = llmin(mOpen[k], 4000.f);
    }
    upload(f.mExpoTex, E, E, GL_RGBA32F, GL_RGBA, mExpoData.data());

    // [WAVES 2026-09-07] The painted wave zones over the same span, one texel per cell.
    // [2026-09-10] The cell is the region's own (16 m, bigger on huge regions — wolfwavezones
    // texelM), so a 25,600 m region bakes 3 x 200 texels an edge, not 3 x 1600 (a 92 MB
    // float texture). Source: wolfstorm wave_zones.js bake() / texelM().
    {
        const F32 texel = (F32)WolfWaveZones::instance().texelM(regionp, esx);
        const S32 zw = llmax(1, (S32)ll_round(esx / texel));
        const S32 zh = llmax(1, (S32)ll_round(esy / texel));
        WolfWaveZones::instance().fill(regionp, ex0, ey0, esx, esy, zw, zh, mZoneData);
        upload(f.mZoneTex, zw, zh, GL_R32F, GL_RED, mZoneData.data());
        f.mZoneW = zw;
        f.mZoneH = zh;
        f.mZoneX0 = ex0;
        f.mZoneY0 = ey0;
        f.mZoneSX = esx;
        f.mZoneSY = esy;
        f.mZone = mZoneData;
        // [WAVES 2026-09-07] editor confirmation (WolfPanelLandWaves::draw): count this bake
        if (regionp == gAgent.getRegion())
        {
            U32 n = 0;
            for (F32 v : mZoneData) if (v > 0.9f) ++n;
            mLastSurfTexels = n;
        }
        ++mBakes;
    }

    f.mSizeX = sx;
    f.mSizeY = sy;
    f.mWaterLevel = water_level;
    f.mExpoX0 = ex0;
    f.mExpoY0 = ey0;
    f.mExpoSX = esx;
    f.mExpoSY = esy;
    f.mBakedAt = LLFrameTimer::getElapsedSeconds();
    // Keep both bakes on the CPU for wolfboatrock.cpp's wave sampler.
    f.mDepth = mDepthData;
    f.mExpo = mExpoData;
    f.mReady = true;
}
