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
#include "wolfgrid.h"   // <WolfViewer 2026-09-22/> Wolf Territories only

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
#include "llviewerobject.h"       // [DRY BUILDS 2026-10-26/] WolfWaterClearance::rasterObjects
#include "llviewerobjectlist.h"
#include "wolfboatrock.h"

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
    mGoneSince.clear();   // <WolfViewer 2026-10-05/>
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
    f.mSurfW.clear();   // <WolfViewer 2026-10-01/>
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
    // <WolfViewer 2026-09-27/> isfinite: NaN passed every compare below and reached the (S32) index casts.
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f)
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
    // <WolfViewer 2026-09-27/> isfinite: NaN passed every compare below and reached the (S32) index casts.
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f)
    {
        return WolfWaveZones::OPEN_ENERGY;
    }
    const S32 i = llclamp((S32)(u * f.mZoneW), 0, f.mZoneW - 1);
    const S32 j = llclamp((S32)(v * f.mZoneH), 0, f.mZoneH - 1);
    return f.mZone[(size_t)j * f.mZoneW + i];
}

// [SURF HEIGHT 2026-10-02] The shader's surf zone for a weight G: waterV.glsl surfZone =
// smoothstep(0.62, 0.95, 0.55 + 0.45 * surfW) (GLSL smoothstep: t = clamp((x - e0) / (e1 - e0)),
// t * t * (3 - 2t)). wave_zones.js surfZoneOf() same.
F32 WolfWaterField::surfZoneOf(F32 w)
{
    const F32 t = llclamp((0.55f + 0.45f * w - 0.62f) / (0.95f - 0.62f), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// [SURF HEIGHT 2026-10-02] The weight G whose surfZoneOf is z (0..1): the inverse of
// t * t * (3 - 2t) on [0, 1] is t = 0.5 - sin(asin(1 - 2z) / 3); then undo the 0.62..0.95 window
// and the 0.55 + 0.45 G. 0 stays 0 (no surf). wave_zones.js surfWeightFor() same.
F32 WolfWaterField::surfWeightFor(F32 z)
{
    if (z <= 0.f) return 0.f;
    z = llmin(z, 1.f);
    const F32 t = 0.5f - sinf(asinf(1.f - 2.f * z) / 3.f);
    return llclamp((0.62f + (0.95f - 0.62f) * t - 0.55f) / 0.45f, 0.f, 1.f);
}

// <WolfViewer 2026-10-01> Source: wave_zones.js surfWeightAt() — zoneAt()'s texel rule on G.
F32 WolfWaterField::surfWeightAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mZoneW <= 0 || f.mSurfW.size() != (size_t)f.mZoneW * f.mZoneH)
    {
        return 0.f;
    }
    const F32 u = (rx - f.mZoneX0) / f.mZoneSX;
    const F32 v = (ry - f.mZoneY0) / f.mZoneSY;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f)
    {
        return 0.f;
    }
    const S32 i = llclamp((S32)(u * f.mZoneW), 0, f.mZoneW - 1);
    const S32 j = llclamp((S32)(v * f.mZoneH), 0, f.mZoneH - 1);
    return f.mSurfW[(size_t)j * f.mZoneW + i];
}

// <WolfViewer 2026-10-01> Source: wave_zones.js surfBlurRadius() — see wolfwaterfield.h.
S32 WolfWaterField::surfBlurRadius(F32 k0, F32 texel_m)
{
    if (k0 <= 0.f || texel_m <= 0.f)
    {
        return 1;
    }
    const F32 quarter = 0.25f * F_TWO_PI / k0;
    return llclamp((S32)ll_round(quarter / texel_m), 1, SURF_BLUR_MAX_TEXELS);
}

F32 WolfWaterField::distanceAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mExpo.size() != (size_t)ERES * ERES * 4) return 4000.f;
    const F32 u = (rx - f.mExpoX0) / f.mExpoSX, v = (ry - f.mExpoY0) / f.mExpoSY;
    // <WolfViewer 2026-09-27/> isfinite: NaN passed every compare below and reached the (S32) index casts.
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f) return 4000.f;
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
    // <WolfViewer 2026-09-27/> isfinite: NaN passed every compare below and reached the (S32) index casts.
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f) return 4000.f;
    const F32 fx = u * (ERES - 1), fy = v * (ERES - 1);
    const S32 i0 = llmin(ERES - 2, (S32)fx), j0 = llmin(ERES - 2, (S32)fy);
    const F32 tx = fx - i0, ty = fy - j0;
    auto d = [&](S32 i, S32 j) { return f.mExpo[((size_t)j * ERES + i) * 4 + 2]; };
    return (d(i0, j0) * (1 - tx) + d(i0 + 1, j0) * tx) * (1 - ty) + (d(i0, j0 + 1) * (1 - tx) + d(i0 + 1, j0 + 1) * tx) * ty;
}

// <WolfViewer 2026-09-21/> see wolfwaterfield.h. Source: exposure bake A channel.
F32 WolfWaterField::openPathAt(const Field& f, F32 rx, F32 ry)
{
    if (!f.mReady || f.mExpo.size() != (size_t)ERES * ERES * 4) return 40000.f;
    const F32 u = (rx - f.mExpoX0) / f.mExpoSX, v = (ry - f.mExpoY0) / f.mExpoSY;
    // <WolfViewer 2026-09-27/> isfinite: NaN passed every compare below and reached the (S32) index casts.
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || u > 1.f || v < 0.f || v > 1.f) return 40000.f;
    const F32 fx = u * (ERES - 1), fy = v * (ERES - 1);
    const S32 i0 = llmin(ERES - 2, (S32)fx), j0 = llmin(ERES - 2, (S32)fy);
    const F32 tx = fx - i0, ty = fy - j0;
    auto d = [&](S32 i, S32 j) { return f.mExpo[((size_t)j * ERES + i) * 4 + 3]; };
    return (d(i0, j0) * (1 - tx) + d(i0 + 1, j0) * tx) * (1 - ty) + (d(i0, j0 + 1) * (1 - tx) + d(i0 + 1, j0 + 1) * tx) * ty;
}

// <WolfViewer 2026-09-21/> see wolfwaterfield.h. The wavelength lldrawpoolwater.cpp sends the
// shader, from the same clamps: surfLength 12..400 m, surfHeight 0.2..20 m.
F32 WolfWaterField::surfK0()
{
    const WolfWaveZones::Region* wr = WolfWaveZones::instance().current();
    if (!wr) return 0.f;
    const LLSD& p = WolfWaveZones::instance().params();
    const F32 h = llclamp(p.has("surfHeight") ? (F32)p["surfHeight"].asReal() : 3.f, 0.2f, 30.f);   // <WolfViewer 2026-10-01/> up to 30 m (Paul)
    const F32 len = llclamp(p.has("surfLength") ? (F32)p["surfLength"].asReal() : 36.f, 12.f, 400.f);
    if (h <= 0.01f) return 0.f;
    return 6.2831853f / llmax(len, 12.f * h);
}

// <WolfViewer 2026-09-21/> see wolfwaterfield.h — Guo (2002) / Fenton (2006) eq. (3).
F32 WolfWaterField::surfWavenumber(F32 k0, F32 h)
{
    const F32 u = powf(llmax(k0 * llmax(h, 0.02f), 1e-4f), 1.25f);
    return k0 * powf(llmax(1.f - expf(-u), 1e-6f), -0.4f);
}

// <WolfViewer 2026-09-21/> see wolfwaterfield.h.
// cg = n * omega / k with n = 0.5 * (1 + 2kh / sinh 2kh); cg0 = 0.5 * omega / k0 (deep water).
// Ks = sqrt(cg0 / cg) = sqrt(0.5 * (k / k0) / n).
F32 WolfWaterField::surfShoalGain(F32 k0, F32 k, F32 h)
{
    const F32 kh2 = llmin(2.f * k * llmax(h, 0.02f), 20.f);   // sinh overflows past ~20; n is 0.5 long before
    const F32 n = 0.5f * (1.f + kh2 / sinhf(llmax(kh2, 1e-4f)));
    return sqrtf(0.5f * (k / llmax(k0, 1e-6f)) / llmax(n, 1e-4f));
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
    // <WolfViewer 2026-09-27/> isfinite: as above.
    if (!std::isfinite(rx) || !std::isfinite(ry) || rx < 0.f || ry < 0.f || rx > f.mSizeX || ry > f.mSizeY)
    {
        return false;
    }
    // <WolfViewer 2026-09-27> Bilinear, like the GPU's read of the same texture (upload():
    // TFO_BILINEAR) and like distanceAt below. The nearest texel was 8 m wide on a windowed
    // field: the boat rocker now differences this across a hull, and a step between texels
    // read as a slope that is not in the drawn water.
    const F32 fx = rx * (RES - 1) / f.mSizeX, fy = ry * (RES - 1) / f.mSizeY;
    const S32 i0 = llclamp((S32)fx, 0, RES - 2), j0 = llclamp((S32)fy, 0, RES - 2);
    const F32 tx = llclamp(fx - i0, 0.f, 1.f), ty = llclamp(fy - j0, 0.f, 1.f);
    const F32* t00 = &f.mDepth[((size_t)j0 * RES + i0) * 4];
    const F32* t10 = t00 + 4;
    const F32* t01 = t00 + (size_t)RES * 4;
    const F32* t11 = t01 + 4;
    for (S32 c = 0; c < 4; ++c)
    {
        out[c] = (t00[c] * (1.f - tx) + t10[c] * tx) * (1.f - ty) + (t01[c] * (1.f - tx) + t11[c] * tx) * ty;
    }
    return true;
}

const WolfWaterField::Field* WolfWaterField::get(const LLViewerRegion* regionp) const
{
    if (!regionp)
    {
        return nullptr;
    }
    auto it = mFields.find(regionp->getHandle());
    // [BASIC WOLF SEA 2026-10-19/] keyed by handle, which another grid's region can share: only the region it was baked for
    if (it == mFields.end() || !it->second.mReady || it->second.mRegionID != regionp->getRegionID())
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
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(sx) || !std::isfinite(sy)) return 0;   // <WolfViewer 2026-09-27/> before the (S32) casts
    const S32 px0 = llclamp((S32)floorf(x0 / patch_m), 0, per_edge - 1);
    const S32 py0 = llclamp((S32)floorf(y0 / patch_m), 0, per_edge - 1);
    const S32 px1 = llclamp((S32)ceilf((x0 + sx) / patch_m), 0, per_edge - 1);
    const S32 py1 = llclamp((S32)ceilf((y0 + sy) / patch_m), 0, per_edge - 1);
    U64 stamp = 1469598103934665603ull;
    for (S32 y = py0; y <= py1; ++y)
    {
        for (S32 x = px0; x <= px1; ++x)
        {
            // <WolfViewer 2026-09-23/> findPatch: patches are made on first use now, and a
            // stamp must not make them. A patch not made yet has no update to stamp.
            const LLSurfacePatch* patchp = land.findPatch(x, y);
            if (patchp)
            {
                stamp = (stamp ^ patchp->getLastUpdateTime()) * 1099511628211ull;
            }
        }
    }
    return stamp;
}

U64 WolfWaterField::neighbourStamp(LLViewerRegion* regionp)
{
    const LLVector3d o = regionp->getOriginGlobal();
    const F64 m = (F64)STAMP_MARGIN_M;
    const F64 x0 = o.mdV[VX] - m, y0 = o.mdV[VY] - m;
    const F64 x1 = o.mdV[VX] + (F64)regionp->getWidth() + m, y1 = o.mdV[VY] + (F64)regionp->getWidth() + m;
    // Order-independent (the region list order is not fixed): a sum of per-region mixes. A
    // neighbour arriving, leaving or getting its height data all change it, as do new heights.
    U64 sum = 0;
    for (LLViewerRegion* n : LLWorld::getInstance()->getRegionList())
    {
        if (!n || n == regionp || !n->isAlive()) continue;
        const LLVector3d no = n->getOriginGlobal();
        const F64 w = (F64)n->getWidth();
        if (no.mdV[VX] >= x1 || no.mdV[VX] + w <= x0 || no.mdV[VY] >= y1 || no.mdV[VY] + w <= y0) continue;
        U64 h = n->getHandle() * 0x9E3779B97F4A7C15ULL;
        h ^= (n->getLand().hasZData() ? terrainStamp(n) : 0ULL) + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
        sum += h;
    }
    return sum;
}

U64 WolfWaterField::terrainStamp(LLViewerRegion* regionp)
{
    // <WolfViewer 2026-09-23> Changes whenever any patch's heights change, like the hash of
    // every patch's update time it replaces - which visited every patch of the region (and,
    // with patches now made on first use, would have made them all).
    return regionp->getLand().getTerrainRevision();
}

void WolfWaterField::idle()
{
    // <WolfViewer 2026-09-22> Wolf Territories only (Paul 09-22: "I don't want people from
    // other grids getting a free ride on what we have created"). Off-grid this whole
    // subsystem is inert, which also saves the work it would otherwise do for nothing.
    // [WOLF GRID GATE 2026-10-17] and what it built goes: it is kept by region HANDLE, which a foreign grid's region can
    // share, so after a hypergrid jump the last Wolf fields would have stayed on that grid's water.
    // [BASIC WOLF SEA 2026-10-19] Now on every OpenSim grid (wolfgrid.h showsBasicWolfSea; Frank Hurt, Paul 2026-10-19): the depth
    // and exposure fields are baked here from the region's OWN terrain and water height (bake), nothing is read from a Wolf
    // service for them, and without them the swell either stays flat (the round-15 wait in lldrawpoolwater.cpp) or runs as open
    // sea up every river. The painted zones baked alongside stay Wolf only: WolfWaveZones::idle / fill read nothing off a Wolf
    // region (wolfwavezones.cpp), so off Wolf the zone texture is uniform open sea and there is no surf train.
    // The handle reuse above is covered per region instead: a field counts only for the region it was baked for (mRegionID).
    if (!WolfGrid::showsBasicWolfSea())
    {
        if (!mFields.empty())
        {
            reset();
        }
        WolfWaterClearance::instance().reset();   // [DRY BUILDS 2026-10-26/] no Wolf sea, nothing to keep dry
        return;
    }
    // [DRY BUILDS 2026-10-26] Before the shore-field switch: the dry-builds guarantee holds with the shore field off too
    // (waterV.glsl draws no swell at all until this field is ready).
    WolfWaterClearance::instance().idle();
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
    // Drop fields of regions that have gone - <WolfViewer 2026-10-05> after GONE_KEEP_SECS, and
    // only beyond the GONE_KEEP_MAX most recent: a region that comes back (same handle) uses its
    // field at once instead of plain swell until its terrain has streamed in and re-baked.
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
        if (live.find(it->first) != live.end())
        {
            mGoneSince.erase(it->first);
            ++it;
            continue;
        }
        auto gone = mGoneSince.emplace(it->first, now).first;
        if (now - gone->second > GONE_KEEP_SECS)
        {
            mGoneSince.erase(gone);
            releaseField(it->second);
            it = mFields.erase(it);
        }
        else
        {
            ++it;
        }
    }
    while (mGoneSince.size() > GONE_KEEP_MAX)
    {
        auto oldest = mGoneSince.begin();
        for (auto g = mGoneSince.begin(); g != mGoneSince.end(); ++g)
        {
            if (g->second < oldest->second) oldest = g;
        }
        auto f = mFields.find(oldest->first);
        if (f != mFields.end())
        {
            releaseField(f->second);
            mFields.erase(f);
        }
        mGoneSince.erase(oldest);
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
        const U64 own = windowed ? terrainStampIn(regionp, wx0 - STAMP_MARGIN_M, wy0 - STAMP_MARGIN_M,
                                                  wsx + 2.f * STAMP_MARGIN_M, wsy + 2.f * STAMP_MARGIN_M)
                                 : terrainStamp(regionp);
        // <WolfViewer 2026-10-02/> neighbours' land counts too, under the same settle rule.
        const U64 stamp = own ^ (neighbourStamp(regionp) * 0xBF58476D1CE4E5B9ULL);
        if (stamp != f.mPendingStamp)
        {
            f.mPendingStamp = stamp;
            f.mPendingSince = now;
        }
        const bool terrain_changed = f.mStamp != stamp && (now - f.mPendingSince) >= STAMP_SETTLE_SECS;
        // <WolfViewer 2026-10-02> No timed re-bake: every input now has its own trigger — this
        // region's and its neighbours' terrain (the stamp), the water level, the window, and the
        // painted zones (WolfWaveZones calls invalidate()).
        // [BASIC WOLF SEA 2026-10-19/] a field baked for another region of the same handle (a hypergrid jump) is no field at all
        const bool other_region = f.mReady && f.mRegionID != regionp->getRegionID();
        const bool stale = !f.mReady || other_region || terrain_changed
                        || f.mWaterLevel != regionp->getWaterHeight() || moved;
        if (stale && (!f.mReady || other_region || now - f.mBakedAt >= MIN_REBAKE_SECS))
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
    f.mRegionID = regionp->getRegionID();   // [BASIC WOLF SEA 2026-10-19/] see get()
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
    // <WolfViewer 2026-09-21> The water DEPTH at every exposure texel, kept for the eikonal
    // pass below. Space no region covers is open ocean: 30 m, the same deep reference
    // waterV.glsl uses beyond the depth field. Anything past ~20 m is deep water to a 36 m
    // wave (k0 h > 3.5 puts Guo's correction inside 0.1 %), so the exact value cannot matter.
    mEDepth.assign((size_t)E * E, DEEP_REF_M);
    for (S32 j = 0; j < E; ++j)
    {
        const F32 py = ey0 + j * ety;
        for (S32 i = 0; i < E; ++i)
        {
            F32 hh;
            const bool has = heightAt(regionp, ex0 + i * etx, py, hh);
            mDist[j * E + i] = (has && hh >= water_level) ? 0.f : BIG;
            mEDepth[j * E + i] = has ? llclamp(water_level - hh, 0.f, DEEP_REF_M) : DEEP_REF_M;
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
    // <WolfViewer 2026-09-21> ------------------------------------------------------------
    // THE EIKONAL (OPTICAL) PATH from the open sea — what the surf train's phase is made of.
    //
    // THE BUG THIS REPLACES (Paul 09-21: "doesn't really look like surfing waves just a
    // mess"): the shader built its phase as `kl * coord`, the LOCAL shallow-water wavenumber
    // times the absolute distance from the sea. That is not a wave. A wave train in water of
    // varying depth has phase  phi = INTEGRAL of k along the ray, minus omega t with omega
    // CONSTANT — frequency is the conserved quantity as a wave shoals, not wavelength
    // (Wikipedia, Wave shoaling: "wave crests are conserved and the frequency must remain
    // constant along a wave ray"). Writing k(x)*s instead gives
    //   d(phase)/dx = k + s * dk/dx,
    // and with s in the hundreds of metres the second term swamps the first: crests piled
    // into ribbons wherever the seabed sloped and cancelled to a stationary plateau where it
    // did not — the giant smooth white dome in Paul's screenshot was one such plateau. The
    // old code also used a LOCAL omega, so deep and shallow water drifted apart by ~0.55
    // rad/s: after an hour of uptime the two were 300 crests out of step.
    //
    // Fix: bake the true eikonal solution. The chamfer sweeps above already relax a shortest
    // path; relaxing the same neighbourhood with the step WEIGHTED by k/k0 gives Fermat's
    // shortest OPTICAL path T = integral of (k/k0) ds, and phi = k0 * T is then exactly the
    // WKB/ray-theory phase, with |grad phi| = k and grad phi along the ray — so crest
    // spacing, crest bending (refraction) and direction all come out of one field.
    // T is stored rather than phi so the texture holds a SMOOTH metre-scale quantity that
    // bilinear filtering can carry; the shader multiplies by k0 and gets the oscillation.
    //
    // The local wavenumber is Guo (2002), "Simple and explicit solution of wave dispersion
    // equation", Coastal Engineering 45, 71-74, as restated by Fenton (2006) eq. (3):
    //     k d = (sigma^2 d / g) * (1 - exp(-(sigma sqrt(d/g))^(5/2)))^(-2/5)
    // Here sigma^2 = g k0 by definition of surfLength (the DEEP-water wavelength), so
    // sigma sqrt(d/g) = sqrt(k0 d) and the whole thing collapses to
    //     k = k0 * (1 - exp(-(k0 h)^(5/4)))^(-2/5),
    // exact in both the deep and the shallow limit and within 0.7 % between them.
    // KEEP BYTE-IDENTICAL with waterV.glsl wolfSurfK(), wolfsurfcurlV.glsl, wolfboatrock.cpp
    // and WolfStorm Water.js / surf_curl.js / terrain_manager.js.
    // -------------------------------------------------------------------------------------
    const F32 k0 = surfK0();
    mPath.assign((size_t)E * E, BIG);
    std::vector<F32> kr((size_t)E * E, 1.f);   // k / k0 at each texel, >= 1 everywhere
    // No region record, or surf switched off: no wavelength to integrate against, so the
    // path stays 0 and the shader keeps its geometric fallback (mSurfK0 0 says so).
    if (k0 > 0.f)
    {
        for (S32 k = 0; k < E * E; ++k)
        {
            kr[k] = surfWavenumber(k0, surfDepth(mEDepth[k], mDist[k])) / k0;   // <WolfViewer 2026-10-01/> real or reef depth
            if (mOpen[k] <= 0.f) mPath[k] = 0.f;   // seeded on the same open-sea texels as mOpen
        }
        // Weight for a step INTO texel c from texel n: the mean of the two texels' k/k0 times the
        // step length — the trapezoid rule for the integral along that step.
        auto relax = [&](S32 c, S32 n, F32 len)
        {
            const F32 w = mPath[n] + 0.5f * (kr[c] + kr[n]) * len;
            if (w < mPath[c]) mPath[c] = w;
        };
        for (S32 j = 0; j < E; ++j)
        {
            const S32 row = j * E, up = row - E;
            for (S32 i = 0; i < E; ++i)
            {
                if (i > 0) relax(row + i, row + i - 1, etx);
                if (j > 0)
                {
                    relax(row + i, up + i, ety);
                    if (i > 0) relax(row + i, up + i - 1, diag);
                    if (i < E - 1) relax(row + i, up + i + 1, diag);
                }
            }
        }
        for (S32 j = E - 1; j >= 0; --j)
        {
            const S32 row = j * E, dn = row + E;
            for (S32 i = E - 1; i >= 0; --i)
            {
                if (i < E - 1) relax(row + i, row + i + 1, etx);
                if (j < E - 1)
                {
                    relax(row + i, dn + i, ety);
                    if (i < E - 1) relax(row + i, dn + i + 1, diag);
                    if (i > 0) relax(row + i, dn + i - 1, diag);
                }
            }
        }
    }
    else
    {
        mPath.assign((size_t)E * E, 0.f);
    }
    // <WolfViewer 2026-10-01> SHORE EXTRAPOLATION. The relaxation runs through LAND texels too,
    // where the surf depth is ~0 and k/k0 is ~50 (Guo at h = 0.02 m), so one 24 m land texel
    // (a 2048 m window's exposure span is 6144 m over 255 texels) adds ~1,270 m of path. The
    // shader reads the path BILINEARLY, so every water vertex within a texel of the shore
    // blended toward that jump: the phase k0 * path climbed ~23 rad across one texel, 3-4 crests
    // in 24 m, drawn as a row of spikes -- worst in a channel one or two texels wide, which is
    // nearly all shore (Paul 10-01, Wolf Territories Home: "spikey waves near the shore").
    // Land texels next to water (8-connected, two rings) take the LOWEST path of their water /
    // already-filled neighbours, so a bilinear read across the waterline stays continuous.
    // Water texels are untouched. Source: terrain_manager.js _bakeSwellExposure, same passes.
    if (k0 > 0.f)
    {
        std::vector<F32> filled(mPath);
        std::vector<U8> known((size_t)E * E, 0);
        for (S32 k = 0; k < E * E; ++k) known[k] = mDist[k] > 0.f ? 1 : 0;
        for (S32 ring = 0; ring < 2; ++ring)
        {
            std::vector<U8> next(known);
            for (S32 j = 0; j < E; ++j)
            {
                for (S32 i = 0; i < E; ++i)
                {
                    const S32 c = j * E + i;
                    if (known[c]) continue;
                    F32 best = BIG;
                    for (S32 dj = -1; dj <= 1; ++dj)
                    {
                        const S32 jj = j + dj; if (jj < 0 || jj >= E) continue;
                        for (S32 di = -1; di <= 1; ++di)
                        {
                            const S32 ii = i + di; if (ii < 0 || ii >= E) continue;
                            const S32 n = jj * E + ii;
                            if (known[n] && filled[n] < best) best = filled[n];
                        }
                    }
                    if (best < BIG)
                    {
                        filled[c] = best;
                        next[c] = 1;
                    }
                }
            }
            known.swap(next);
        }
        mPath.swap(filled);
    }
    // [SURF 2026-09-07] R = exposure (every existing reader unchanged), G = the chamfer
    // DISTANCE TO LAND in metres (capped at 4000), B = distance from the open sea.
    // <WolfViewer 2026-09-21> A = the OPTICAL path from the open sea (was a constant 1).
    // Capped at 40000: 10x the distance cap, and k/k0 never exceeds ~10 above 2 cm of water.
    mExpoData.assign((size_t)E * E * 4, 1.f);
    for (S32 k = 0; k < E * E; ++k)
    {
        const F32 t = llclamp((mDist[k] - 6.f) / 144.f, 0.f, 1.f);   // 6 m .. 150 m
        mExpoData[(size_t)k * 4] = t * t * (3.f - 2.f * t);
        mExpoData[(size_t)k * 4 + 1] = llmin(mDist[k], 4000.f);
        mExpoData[(size_t)k * 4 + 2] = llmin(mOpen[k], 4000.f);
        mExpoData[(size_t)k * 4 + 3] = llmin(mPath[k], 40000.f);
    }
    upload(f.mExpoTex, E, E, GL_RGBA32F, GL_RGBA, mExpoData.data());
    f.mSurfK0 = k0;

    // [WAVES 2026-09-07] The painted wave zones over the same span, one texel per cell.
    // [2026-09-10] The cell is the region's own (16 m, bigger on huge regions — wolfwavezones
    // texelM), so a 25,600 m region bakes 3 x 200 texels an edge, not 3 x 1600 (a 92 MB
    // float texture). Source: wolfstorm wave_zones.js bake() / texelM().
    // <WolfViewer 2026-09-26> Now the 16 m paint cell on every region: the span is at most
    // 3 x WINDOW_M, 384 texels at 16 m (wolfwavezones.h texelM).
    {
        const F32 texel = (F32)WolfWaveZones::texelM(esx);
        const S32 zw = llmax(1, (S32)ll_round(esx / texel));
        const S32 zh = llmax(1, (S32)ll_round(esy / texel));
        WolfWaveZones::instance().fill(regionp, ex0, ey0, esx, esy, zw, zh, mZoneData, &mSurfMask, &mSurfScale);
        // <WolfViewer 2026-10-01> THE SURF WEIGHT (G). Paul 10-01, Wolf Territories Home: a
        // 20 m surf (wavelength 12 x 20 = 240 m) painted as a 2-3 cell strip stood up as jagged
        // white walls. The surf height followed the painted cells through the energy's 3x3
        // (48 m) blur, so at every painted edge a crest of 8-16 m appeared or vanished within
        // ~10 m. A wave cannot change height along its crest faster than its own scale allows.
        // Paul then: "if i specify 30m waves they should start at 30m" — so a PAINTED cell is
        // always full height and the ramp lies OUTSIDE the paint: the surf cells are grown by a
        // quarter wavelength (a max filter), then box-blurred by the same radius. Every painted
        // texel's blur window lies inside the grown area, so it stays exactly 1; the weight
        // falls to 0 over the next half wavelength (120 m for a 240 m wave, 32 m for the
        // default 36 m). Both passes separable; the blur averages in-bounds taps only, the
        // energy 3x3's edge rule (wolfwavezones.cpp fill).
        // Source: wave_zones.js bake() surf weight, same passes.
        {
            const S32 rad = surfBlurRadius(k0, texel);
            const size_t n = (size_t)zw * zh;
            mSurfW.assign(n, 0.f);
            mSurfTmp.assign(n, 0.f);
            mSurfGrown.assign(n, 0.f);
            for (S32 j = 0; j < zh; ++j)
            {
                for (S32 i = 0; i < zw; ++i)
                {
                    F32 m = 0.f;
                    for (S32 ii = llmax(0, i - rad); ii <= llmin(zw - 1, i + rad); ++ii) m = llmax(m, mSurfMask[(size_t)j * zw + ii]);
                    mSurfTmp[(size_t)j * zw + i] = m;
                }
            }
            for (S32 j = 0; j < zh; ++j)
            {
                for (S32 i = 0; i < zw; ++i)
                {
                    F32 m = 0.f;
                    for (S32 jj = llmax(0, j - rad); jj <= llmin(zh - 1, j + rad); ++jj) m = llmax(m, mSurfTmp[(size_t)jj * zw + i]);
                    mSurfGrown[(size_t)j * zw + i] = m;
                }
            }
            for (S32 j = 0; j < zh; ++j)
            {
                for (S32 i = 0; i < zw; ++i)
                {
                    F32 acc = 0.f; S32 c = 0;
                    for (S32 ii = llmax(0, i - rad); ii <= llmin(zw - 1, i + rad); ++ii) { acc += mSurfGrown[(size_t)j * zw + ii]; ++c; }
                    mSurfTmp[(size_t)j * zw + i] = acc / (F32)c;
                }
            }
            for (S32 j = 0; j < zh; ++j)
            {
                for (S32 i = 0; i < zw; ++i)
                {
                    F32 acc = 0.f; S32 c = 0;
                    for (S32 jj = llmax(0, j - rad); jj <= llmin(zh - 1, j + rad); ++jj) { acc += mSurfTmp[(size_t)jj * zw + i]; ++c; }
                    mSurfW[(size_t)j * zw + i] = acc / (F32)c;
                }
            }
            // [SURF HEIGHT 2026-10-02] Paul: "per area surf lets build that now". Each surf cell
            // may be painted at its own height (WolfWaveZones::surfScaleOf). The height is
            // averaged over the surf texels within TWICE the radius — the weight's own ramp is the
            // grow plus the blur — so between a 30% and a 100% area it changes over about a
            // wavelength, as the weight does at a painted edge, and the ramp outside the paint
            // takes the height of the surf it leads to. Then G is written so the shader's own rule,
            // surfZone = smoothstep(0.62, 0.95, 0.55 + 0.45 G) (waterV.glsl; wolfboatrock.cpp and
            // wave_zones.js the same), gives exactly (that rule on the coverage) x height: nothing
            // that reads G changes. A layout painted only at 100% keeps G exactly as before.
            // Source: wave_zones.js bake() surf height, same passes.
            {
                const S32 rad2 = 2 * rad;
                mScaleNum.assign(n, 0.f); mScaleDen.assign(n, 0.f);
                mScaleTmpN.assign(n, 0.f); mScaleTmpD.assign(n, 0.f);
                for (size_t k = 0; k < n; ++k)
                {
                    mScaleTmpN[k] = mSurfMask[k] * mSurfScale[k];
                    mScaleTmpD[k] = mSurfMask[k];
                }
                for (S32 j = 0; j < zh; ++j)
                {
                    for (S32 i = 0; i < zw; ++i)
                    {
                        F32 a = 0.f, b = 0.f;
                        for (S32 ii = llmax(0, i - rad2); ii <= llmin(zw - 1, i + rad2); ++ii)
                        {
                            a += mScaleTmpN[(size_t)j * zw + ii];
                            b += mScaleTmpD[(size_t)j * zw + ii];
                        }
                        mScaleNum[(size_t)j * zw + i] = a;
                        mScaleDen[(size_t)j * zw + i] = b;
                    }
                }
                for (S32 j = 0; j < zh; ++j)
                {
                    for (S32 i = 0; i < zw; ++i)
                    {
                        F32 a = 0.f, b = 0.f;
                        for (S32 jj = llmax(0, j - rad2); jj <= llmin(zh - 1, j + rad2); ++jj)
                        {
                            a += mScaleNum[(size_t)jj * zw + i];
                            b += mScaleDen[(size_t)jj * zw + i];
                        }
                        const size_t k = (size_t)j * zw + i;
                        const F32 height = b > 0.f ? llclamp(a / b, 0.f, 1.f) : 1.f;
                        if (height >= 0.999f || mSurfW[k] <= 0.f) continue;   // full height: G as before
                        const F32 want = surfZoneOf(mSurfW[k]) * height;
                        mSurfW[k] = surfWeightFor(want);
                    }
                }
            }
            mZoneRG.resize(n * 2);
            for (size_t k = 0; k < n; ++k)
            {
                mZoneRG[k * 2] = mZoneData[k];
                mZoneRG[k * 2 + 1] = mSurfW[k];
            }
        }
        upload(f.mZoneTex, zw, zh, GL_RG32F, GL_RG, mZoneRG.data());
        f.mSurfW = mSurfW;
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

// =====================================================================================================================
// [DRY BUILDS 2026-10-26] WolfWaterClearance - see wolfwaterfield.h for the rule, the bounds and their proof.
// [OPEN SEA SHORES 2026-10-26] + the open-sea classification and the inland-only land sources (Paul: "a fixed edge").
// =====================================================================================================================

const F32 WolfWaterClearance::TEXEL_M[WolfWaterClearance::LEVELS] = { 1.f, 4.f, 16.f };
const F32 WolfWaterClearance::TERRAIN_M[WolfWaterClearance::TBLOCKS] = { 1.f, 4.f };

WolfWaterClearance::WolfWaterClearance()
{
}

WolfWaterClearance::~WolfWaterClearance()
{
    reset();
}

void WolfWaterClearance::reset()
{
    if (mTex)
    {
        LLImageGL::deleteTextures(1, &mTex);
        mTex = 0;
    }
    for (S32 k = 0; k < LEVELS; ++k)
    {
        mHaveOrigin[k] = false;
        mLandValid[k] = false;
        mTerrainStamp[k] = 0;
        mLand[k].clear();
        mVoid[k].clear();
        mOpenDist[k].clear();
        mOpenW[k].clear();
        mSrcAll[k].clear();
        mSrcDry[k].clear();
        mS[k].clear();
        mD[k].clear();
        mSDry[k].clear();
    }
    for (S32 b = 0; b < TBLOCKS; ++b)
    {
        mHaveTOrigin[b] = false;
        mTerrValid[b] = false;
        mTerrStamp[b] = 0;
        mTerr[b].clear();
    }
    mTexAllocated = false;
    mUpload.clear();
    mTmp.clear();
    mReady = false;
    mObjectsValid = false;
    mNextObjects = 0.0;
}

LLVector2 WolfWaterClearance::levelOriginAgent(S32 k) const
{
    const LLVector3 a = gAgent.getPosAgentFromGlobal(mOrigin[k]);
    return LLVector2(a.mV[VX], a.mV[VY]);
}

LLVector2 WolfWaterClearance::terrainOriginAgent(S32 b) const
{
    const LLVector3 a = gAgent.getPosAgentFromGlobal(mTOrigin[b]);
    return LLVector2(a.mV[VX], a.mV[VY]);
}

// waterV.glsl wolfPixelHeld, term for term: inside the hull of a terrain block's texel centres by more than the dead
// zone + HELD_MARGIN_M (so the whole displaced triangle is in it), blended in over HELD_BLEND_M; the best block.
F32 WolfWaterClearance::pixelHeld(F32 ax, F32 ay, F32 dead) const
{
    if (!mReady)
    {
        return 0.f;
    }
    F32 held = 0.f;
    for (S32 b = 0; b < TBLOCKS; ++b)
    {
        const F32 t = TERRAIN_M[b];
        const LLVector2 o = terrainOriginAgent(b);
        const F32 gx = (ax - o.mV[VX]) / t, gy = (ay - o.mV[VY]) / t;
        const F32 lo = 0.5f, hi = N - 0.5f;
        const F32 inner = llmin(llmin(gx - lo, gy - lo), llmin(hi - gx, hi - gy)) * t;
        const F32 u = llclamp((inner - (dead + HELD_MARGIN_M)) / HELD_BLEND_M, 0.f, 1.f);
        held = llmax(held, u * u * (3.f - 2.f * u));
    }
    return held;
}

// Every live region's terrain revision and water height, folded together: what the terrain raster reads.
U64 WolfWaterClearance::terrainStamp() const
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
        mix(regionp->getLand().getTerrainRevision());
        U32 wbits = 0;
        const F32 w = regionp->getWaterHeight();
        memcpy(&wbits, &w, sizeof(wbits));
        mix(wbits);
    }
    return h;
}

// The terrain sources of level k, and which texels no region covers (the open ocean). A texel is a source when ANY
// terrain point inside it is at or above its region's water height: the drawn terrain is triangles between the grid
// points (llsurfacepatch.cpp), so its highest point over a texel is the highest grid point of the grid cells the texel
// touches. Whole patches are settled by their own min / max (LLSurfacePatch::updateVerticalStats, refreshed every
// frame for a changed patch - llsurface.cpp idleUpdate), and the 16 m level takes a patch with any land in it as land
// throughout - conservative, never less.
void WolfWaterClearance::rasterTerrain(S32 k)
{
    const F32 t = TEXEL_M[k];
    std::vector<F32>& land = mLand[k];
    land.assign((size_t)N * N, BIG);
    mVoid[k].assign((size_t)N * N, 1);
    const F64 ox = mOrigin[k].mdV[VX], oy = mOrigin[k].mdV[VY];
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionp->isAlive())
        {
            continue;
        }
        const LLSurface& surf = regionp->getLand();
        const F32 water = regionp->getWaterHeight();
        const F32 mpg = surf.getMetersPerGrid();
        const S32 gpp = surf.getGridsPerPatchEdge();
        const S32 ppe = surf.getPatchesPerEdge();
        if (mpg <= 0.f || gpp <= 0 || ppe <= 0)
        {
            continue;
        }
        const F32 patch_m = mpg * gpp;
        const LLVector3d rg = regionp->getOriginGlobal();
        // The region's patches that overlap the window.
        const F64 wx0 = ox - rg.mdV[VX], wy0 = oy - rg.mdV[VY];   // window corner, region metres
        const F64 wx1 = wx0 + N * t, wy1 = wy0 + N * t;
        {
            // Texels this region covers, even in part, are not open ocean.
            const F64 rw = regionp->getWidth();
            const S32 vi0 = llmax(0, (S32)floor(-wx0 / t)), vi1 = llmin(N - 1, (S32)ceil((rw - wx0) / t) - 1);
            const S32 vj0 = llmax(0, (S32)floor(-wy0 / t)), vj1 = llmin(N - 1, (S32)ceil((rw - wy0) / t) - 1);
            for (S32 tj = vj0; tj <= vj1; ++tj)
            {
                for (S32 ti = vi0; ti <= vi1; ++ti)
                {
                    mVoid[k][(size_t)tj * N + ti] = 0;
                }
            }
        }
        const S32 pi0 = llmax(0, (S32)floor(wx0 / patch_m)), pi1 = llmin(ppe - 1, (S32)floor(wx1 / patch_m));
        const S32 pj0 = llmax(0, (S32)floor(wy0 / patch_m)), pj1 = llmin(ppe - 1, (S32)floor(wy1 / patch_m));
        for (S32 pj = pj0; pj <= pj1; ++pj)
        {
            for (S32 pi = pi0; pi <= pi1; ++pi)
            {
                const LLSurfacePatch* patchp = surf.findPatch(pi, pj);
                if (!patchp || !patchp->getDataZ() || patchp->getMaxZ() < water)
                {
                    continue;   // no terrain here yet, or all of it under the water
                }
                // The patch's footprint in window texels (region metres -> texels).
                const F64 px0 = pi * (F64)patch_m, py0 = pj * (F64)patch_m;
                const S32 ti0 = llmax(0, (S32)floor((px0 - wx0) / t));
                const S32 ti1 = llmin(N - 1, (S32)ceil((px0 + patch_m - wx0) / t) - 1);
                const S32 tj0 = llmax(0, (S32)floor((py0 - wy0) / t));
                const S32 tj1 = llmin(N - 1, (S32)ceil((py0 + patch_m - wy0) / t) - 1);
                if (ti0 > ti1 || tj0 > tj1)
                {
                    continue;
                }
                const bool whole = patchp->getMinZ() >= water || t >= patch_m;
                const F32* z = patchp->getDataZ();
                const S32 stride = (S32)patchp->getDataStride();
                for (S32 tj = tj0; tj <= tj1; ++tj)
                {
                    for (S32 ti = ti0; ti <= ti1; ++ti)
                    {
                        F32& cell = land[(size_t)tj * N + ti];
                        if (cell <= water)
                        {
                            continue;
                        }
                        bool is_land = whole;
                        if (!is_land)
                        {
                            // The grid points of every grid cell the texel touches, inside this patch.
                            const F64 tx0 = wx0 + ti * (F64)t - px0, ty0 = wy0 + tj * (F64)t - py0;
                            const S32 gi0 = llclamp((S32)floor(tx0 / mpg), 0, gpp), gi1 = llclamp((S32)ceil((tx0 + t) / mpg), 0, gpp);
                            const S32 gj0 = llclamp((S32)floor(ty0 / mpg), 0, gpp), gj1 = llclamp((S32)ceil((ty0 + t) / mpg), 0, gpp);
                            for (S32 gj = gj0; gj <= gj1 && !is_land; ++gj)
                            {
                                for (S32 gi = gi0; gi <= gi1; ++gi)
                                {
                                    if (z[gi + gj * stride] >= water)
                                    {
                                        is_land = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (is_land)
                        {
                            cell = llmin(cell, water);
                        }
                    }
                }
            }
        }
    }
    mLandValid[k] = true;
}

// [OPEN SEA SHORES 2026-10-26] Terrain block b for waterF.glsl's per-pixel land rule (wolfwaterfield.h TERRAIN_M): each
// texel the highest drawn terrain over the 2t square about its centre, i.e. the highest grid point of every grid cell
// that square touches. Shortcuts that cannot change a decision (waterF.glsl only asks "at or above the water?" and
// "above the water + the swash band?"): a patch wholly under its water is skipped (-BIG reads as water, as its true
// height would), and a patch wholly above it gives its own maximum (at or above every point of it).
void WolfWaterClearance::rasterTerrainMax(S32 b)
{
    const F32 t = TERRAIN_M[b];
    std::vector<F32>& out = mTerr[b];
    out.assign((size_t)N * N, -BIG);
    const F64 ox = mTOrigin[b].mdV[VX], oy = mTOrigin[b].mdV[VY];
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionp->isAlive())
        {
            continue;
        }
        const LLSurface& surf = regionp->getLand();
        const F32 water = regionp->getWaterHeight();
        const F32 mpg = surf.getMetersPerGrid();
        const S32 gpp = surf.getGridsPerPatchEdge();
        const S32 ppe = surf.getPatchesPerEdge();
        if (mpg <= 0.f || gpp <= 0 || ppe <= 0)
        {
            continue;
        }
        const F32 patch_m = mpg * gpp;
        const LLVector3d rg = regionp->getOriginGlobal();
        const F64 wx0 = ox - rg.mdV[VX], wy0 = oy - rg.mdV[VY];   // block corner, region metres
        // texel i's square is [wx0 + i t - t/2, wx0 + i t + 3t/2] (centre wx0 + (i + 0.5) t, half-side t)
        const S32 pi0 = llmax(0, (S32)floor((wx0 - t) / patch_m)), pi1 = llmin(ppe - 1, (S32)floor((wx0 + (N + 1) * t) / patch_m));
        const S32 pj0 = llmax(0, (S32)floor((wy0 - t) / patch_m)), pj1 = llmin(ppe - 1, (S32)floor((wy0 + (N + 1) * t) / patch_m));
        for (S32 pj = pj0; pj <= pj1; ++pj)
        {
            for (S32 pi = pi0; pi <= pi1; ++pi)
            {
                const LLSurfacePatch* patchp = surf.findPatch(pi, pj);
                if (!patchp || !patchp->getDataZ() || patchp->getMaxZ() < water)
                {
                    continue;
                }
                const F64 px0 = pi * (F64)patch_m, py0 = pj * (F64)patch_m;
                // texels whose square overlaps the patch: wx0 + i t - t/2 <= px0 + patch_m and wx0 + i t + 3t/2 >= px0
                const S32 ti0 = llmax(0, (S32)ceil((px0 - wx0 - 1.5 * t) / t)), ti1 = llmin(N - 1, (S32)floor((px0 + patch_m - wx0 + 0.5 * t) / t));
                const S32 tj0 = llmax(0, (S32)ceil((py0 - wy0 - 1.5 * t) / t)), tj1 = llmin(N - 1, (S32)floor((py0 + patch_m - wy0 + 0.5 * t) / t));
                if (ti0 > ti1 || tj0 > tj1)
                {
                    continue;
                }
                const bool whole = patchp->getMinZ() >= water;
                const F32 pmax = patchp->getMaxZ();
                const F32* z = patchp->getDataZ();
                const S32 stride = (S32)patchp->getDataStride();
                for (S32 tj = tj0; tj <= tj1; ++tj)
                {
                    for (S32 ti = ti0; ti <= ti1; ++ti)
                    {
                        F32& cell = out[(size_t)tj * N + ti];
                        if (whole)
                        {
                            cell = llmax(cell, pmax);
                            continue;
                        }
                        const F64 sx0 = wx0 + ti * (F64)t - 0.5 * t - px0, sy0 = wy0 + tj * (F64)t - 0.5 * t - py0;
                        const S32 gi0 = llclamp((S32)floor(sx0 / mpg), 0, gpp), gi1 = llclamp((S32)ceil((sx0 + 2.0 * t) / mpg), 0, gpp);
                        const S32 gj0 = llclamp((S32)floor(sy0 / mpg), 0, gpp), gj1 = llclamp((S32)ceil((sy0 + 2.0 * t) / mpg), 0, gpp);
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
    mTerrValid[b] = true;
}

// [OPEN SEA SHORES 2026-10-26] Which water is the open sea, level k (call coarsest first). Paul: "on the outer edge of
// the region NOT RIVERS INSIDE or LAKES INSIDE". The THROUGH-WATER distance from the open sea - land texels neither
// carry it nor pass it on, so a lake has none and a river grows it with every metre upstream - seeded on the open
// ocean (no region), the coarsest window's edge, water OPEN_FETCH_M or more from land, and, on the finer levels, the
// coarser level's own answer wherever that level's texel is water through and through (its land test is conservative).
// Then the weight 1 - smoothstep(OPEN_NEAR_M, OPEN_FAR_M, distance), carried OPEN_ONTO_LAND_M onto the land beside it
// (the beach the swash runs up), and a 3 x 3 blur so nothing switches at a texel edge.
void WolfWaterClearance::classifyOpen(S32 k)
{
    const F32 t = TEXEL_M[k];
    const size_t NN = (size_t)N * N;
    const std::vector<F32>& land = mLand[k];
    const F32 a = t, b = t * F_SQRT2;
    // Distance to land (for the fetch seeds), plain chamfer.
    std::vector<F32>& dl = mTmp;
    dl.assign(NN, BIG);
    for (size_t i = 0; i < NN; ++i) if (land[i] < BIG) dl[i] = 0.f;
    auto sweep = [&](std::vector<F32>& d, bool water_only)
    {
        auto relax = [&](size_t i, size_t j, F32 w)
        {
            if (water_only && (land[i] < BIG || land[j] < BIG)) return;
            const F32 v = d[j] + w;
            if (v < d[i]) d[i] = v;
        };
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                const size_t i = (size_t)y * N + x;
                if (x > 0) relax(i, i - 1, a);
                if (y > 0)
                {
                    relax(i, i - N, a);
                    if (x > 0) relax(i, i - N - 1, b);
                    if (x < N - 1) relax(i, i - N + 1, b);
                }
            }
        }
        for (S32 y = N - 1; y >= 0; --y)
        {
            for (S32 x = N - 1; x >= 0; --x)
            {
                const size_t i = (size_t)y * N + x;
                if (x < N - 1) relax(i, i + 1, a);
                if (y < N - 1)
                {
                    relax(i, i + N, a);
                    if (x < N - 1) relax(i, i + N + 1, b);
                    if (x > 0) relax(i, i + N - 1, b);
                }
            }
        }
    };
    sweep(dl, false);

    std::vector<F32>& od = mOpenDist[k];
    od.assign(NN, BIG);
    const bool coarsest = (k == LEVELS - 1);
    const F64 ox = mOrigin[k].mdV[VX], oy = mOrigin[k].mdV[VY];
    for (S32 y = 0; y < N; ++y)
    {
        for (S32 x = 0; x < N; ++x)
        {
            const size_t i = (size_t)y * N + x;
            if (land[i] < BIG)
            {
                continue;
            }
            if (mVoid[k][i] || dl[i] >= OPEN_FETCH_M
                || (coarsest && (x == 0 || y == 0 || x == N - 1 || y == N - 1)))
            {
                od[i] = 0.f;
                continue;
            }
            if (!coarsest && !mOpenDist[k + 1].empty())
            {
                // The coarser level's texel holding this texel's centre.
                const F32 tc = TEXEL_M[k + 1];
                const S32 cx = (S32)floor((ox + (x + 0.5) * t - mOrigin[k + 1].mdV[VX]) / tc);
                const S32 cy = (S32)floor((oy + (y + 0.5) * t - mOrigin[k + 1].mdV[VY]) / tc);
                if (cx >= 0 && cy >= 0 && cx < N && cy < N)
                {
                    const size_t c = (size_t)cy * N + cx;
                    if (mLand[k + 1][c] >= BIG && mOpenDist[k + 1][c] < BIG)
                    {
                        od[i] = mOpenDist[k + 1][c];
                    }
                }
            }
        }
    }
    sweep(od, true);

    std::vector<F32>& w = mOpenW[k];
    w.assign(NN, 0.f);
    for (size_t i = 0; i < NN; ++i)
    {
        if (land[i] >= BIG && od[i] < BIG)
        {
            const F32 u = llclamp((od[i] - OPEN_NEAR_M) / (OPEN_FAR_M - OPEN_NEAR_M), 0.f, 1.f);
            w[i] = 1.f - u * u * (3.f - 2.f * u);
        }
    }
    // Onto the land beside it: land texels take the largest weight within OPEN_ONTO_LAND_M (3 x 3 dilations).
    const S32 rings = llmax(1, (S32)ceilf(OPEN_ONTO_LAND_M / t));
    std::vector<F32> next;
    for (S32 r = 0; r < rings; ++r)
    {
        next = w;
        for (S32 y = 0; y < N; ++y)
        {
            for (S32 x = 0; x < N; ++x)
            {
                const size_t i = (size_t)y * N + x;
                if (land[i] >= BIG) continue;
                F32 m = next[i];
                for (S32 dy = -1; dy <= 1; ++dy)
                {
                    const S32 yy = y + dy; if (yy < 0 || yy >= N) continue;
                    for (S32 dx = -1; dx <= 1; ++dx)
                    {
                        const S32 xx = x + dx; if (xx < 0 || xx >= N) continue;
                        m = llmax(m, w[(size_t)yy * N + xx]);
                    }
                }
                next[i] = m;
            }
        }
        w.swap(next);
    }
    // 3 x 3 box blur.
    next = w;
    for (S32 y = 0; y < N; ++y)
    {
        for (S32 x = 0; x < N; ++x)
        {
            F32 acc = 0.f; S32 n = 0;
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                const S32 yy = y + dy; if (yy < 0 || yy >= N) continue;
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const S32 xx = x + dx; if (xx < 0 || xx >= N) continue;
                    acc += w[(size_t)yy * N + xx]; ++n;
                }
            }
            next[(size_t)y * N + x] = acc / n;
        }
    }
    w.swap(next);
}

// The prim sources, every level at once: each prim's box (scale, render rotation) as an agent-space AABB, a source
// where it reaches the waterline band. Its cap is its lowest point if it stands clear of the water, its highest if it
// pierces it - water may lap a hull or a post up to its top, but never rise over a deck. Not sources: avatars and
// what they wear, anything moving faster than MOVING_MPS (a vehicle under way rides its own wake), and the hulls the
// boat rocker is floating (wolfboatrock.cpp) - they ride the waves, the waves do not stop at them.
// Then the two source sets: every land texel (S_all), and only the land beside INLAND water (S_dry) - an open-sea
// beach is held per pixel in waterF.glsl instead, so its waves can run up it.
void WolfWaterClearance::rasterObjects()
{
    for (S32 k = 0; k < LEVELS; ++k)
    {
        mSrcAll[k] = mLand[k];
        mSrcDry[k] = mLand[k];
        const size_t NN = (size_t)N * N;
        for (size_t i = 0; i < NN; ++i)
        {
            if (mSrcDry[k][i] < BIG && mOpenW[k][i] >= 0.5f)
            {
                mSrcDry[k][i] = BIG;
            }
        }
    }
    const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    const WolfBoatRock& rocker = WolfBoatRock::instance();
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAvatar()
            || o->isAttachment() || o->isHUDAttachment() || !o->getRegion())
        {
            continue;
        }
        LLViewerObject* root = o->getRootEdit();
        if (!root || root->isAvatar() || root->isAttachment() || root->isHUDAttachment())
        {
            continue;   // a prim worn or sat on by an avatar
        }
        if (root->getVelocity().lengthSquared() > MOVING_MPS * MOVING_MPS || rocker.isRocking(root))
        {
            continue;
        }
        const F32 water = o->getRegion()->getWaterHeight();
        const LLVector3 c = o->getPositionAgent();
        const LLVector3 half = o->getScale() * 0.5f;
        const LLQuaternion rot = o->getRotationRegion();
        LLVector3 lo(BIG, BIG, BIG), hi(-BIG, -BIG, -BIG);
        for (S32 n = 0; n < 8; ++n)
        {
            const LLVector3 corner((n & 1) ? half.mV[VX] : -half.mV[VX],
                                   (n & 2) ? half.mV[VY] : -half.mV[VY],
                                   (n & 4) ? half.mV[VZ] : -half.mV[VZ]);
            const LLVector3 p = c + corner * rot;
            lo.set(llmin(lo.mV[VX], p.mV[VX]), llmin(lo.mV[VY], p.mV[VY]), llmin(lo.mV[VZ], p.mV[VZ]));
            hi.set(llmax(hi.mV[VX], p.mV[VX]), llmax(hi.mV[VY], p.mV[VY]), llmax(hi.mV[VZ], p.mV[VZ]));
        }
        if (hi.mV[VZ] < water || lo.mV[VZ] > water + BAND_M)
        {
            continue;   // under the water, or far above any crest
        }
        const F32 cap = lo.mV[VZ] >= water ? lo.mV[VZ] : hi.mV[VZ];
        for (S32 k = 0; k < LEVELS; ++k)
        {
            const F32 t = TEXEL_M[k];
            const F64 ox = mOrigin[k].mdV[VX] - agent_origin.mdV[VX], oy = mOrigin[k].mdV[VY] - agent_origin.mdV[VY];
            const S32 ti0 = llmax(0, (S32)floor((lo.mV[VX] - ox) / t)), ti1 = llmin(N - 1, (S32)floor((hi.mV[VX] - ox) / t));
            const S32 tj0 = llmax(0, (S32)floor((lo.mV[VY] - oy) / t)), tj1 = llmin(N - 1, (S32)floor((hi.mV[VY] - oy) / t));
            for (S32 tj = tj0; tj <= tj1; ++tj)
            {
                for (S32 ti = ti0; ti <= ti1; ++ti)
                {
                    const size_t idx = (size_t)tj * N + ti;
                    mSrcAll[k][idx] = llmin(mSrcAll[k][idx], cap);
                    mSrcDry[k][idx] = llmin(mSrcDry[k][idx], cap);
                }
            }
        }
    }
    mObjectsValid = true;
}

// Two-pass weighted chamfer of S (and D when asked), then the margins folded in (wolfwaterfield.h).
void WolfWaterClearance::solve(S32 k, const std::vector<F32>& src, std::vector<F32>& S, std::vector<F32>* D)
{
    const F32 t = TEXEL_M[k];
    const F32 a = t / CHAMFER, b = t * F_SQRT2 / CHAMFER;
    const size_t NN = (size_t)N * N;
    S.resize(NN);
    if (D) D->resize(NN);
    for (size_t i = 0; i < NN; ++i)
    {
        S[i] = src[i];
        if (D) (*D)[i] = src[i] < BIG ? 0.f : BIG;
    }
    auto relax = [&](size_t i, size_t j, F32 w)
    {
        const F32 s = S[j] + ALPHA * w;
        if (s < S[i]) S[i] = s;
        if (D)
        {
            const F32 d = (*D)[j] + w;
            if (d < (*D)[i]) (*D)[i] = d;
        }
    };
    for (S32 y = 0; y < N; ++y)
    {
        for (S32 x = 0; x < N; ++x)
        {
            const size_t i = (size_t)y * N + x;
            if (x > 0) relax(i, i - 1, a);
            if (y > 0)
            {
                relax(i, i - N, a);
                if (x > 0) relax(i, i - N - 1, b);
                if (x < N - 1) relax(i, i - N + 1, b);
            }
        }
    }
    for (S32 y = N - 1; y >= 0; --y)
    {
        for (S32 x = N - 1; x >= 0; --x)
        {
            const size_t i = (size_t)y * N + x;
            if (x < N - 1) relax(i, i + 1, a);
            if (y < N - 1)
            {
                relax(i, i + N, a);
                if (x < N - 1) relax(i, i + N + 1, b);
                if (x > 0) relax(i, i + N - 1, b);
            }
        }
    }
    const F32 e = 1.5f * F_SQRT2 * t;
    for (size_t i = 0; i < NN; ++i)
    {
        S[i] = S[i] < BIG * 0.5f ? S[i] - ALPHA * e : BIG;
        if (D) (*D)[i] = (*D)[i] < BIG * 0.5f ? llmax((*D)[i] - e, 0.f) : BIG;
    }
}

// The texture: ROWS x N RGBA32F. Rows k*N.. the cap levels (R S_all, G D, B S_dry, A open weight), then the terrain
// blocks (R the highest terrain, the rest 0). Allocated once, then updated a block at a time: the caps every second, the
// terrain only when it changed. waterV.glsl / waterF.glsl wolfClearSample and the terrain read.
void WolfWaterClearance::uploadRows(S32 row0, S32 rows, const F32* data)
{
    if (!mTex)
    {
        LLImageGL::generateTextures(1, &mTex);
        mTexAllocated = false;
    }
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTex);
    if (!mTexAllocated)
    {
        std::vector<F32> zero((size_t)N * ROWS * 4, 0.f);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGBA32F,
                                  N, ROWS, GL_RGBA, GL_FLOAT, zero.data(), false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        mTexAllocated = true;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, N, rows, GL_RGBA, GL_FLOAT, data);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

void WolfWaterClearance::uploadLevels()
{
    mUpload.resize((size_t)N * N * LEVELS * 4);
    for (S32 k = 0; k < LEVELS; ++k)
    {
        F32* dst = &mUpload[(size_t)k * N * N * 4];
        for (size_t i = 0; i < (size_t)N * N; ++i)
        {
            dst[i * 4] = mS[k][i];
            dst[i * 4 + 1] = mD[k][i];
            dst[i * 4 + 2] = mSDry[k][i];
            dst[i * 4 + 3] = mOpenW[k][i];
        }
    }
    uploadRows(0, N * LEVELS, mUpload.data());
}

void WolfWaterClearance::uploadTerrain(S32 b)
{
    std::vector<F32> buf((size_t)N * N * 4, 0.f);
    for (size_t i = 0; i < (size_t)N * N; ++i)
    {
        buf[i * 4] = mTerr[b][i];
    }
    uploadRows((LEVELS + b) * N, N, buf.data());
}

void WolfWaterClearance::idle()
{
    LL_PROFILE_ZONE_SCOPED;
    LLWorld* world = LLWorld::getInstance();
    if (!world || !gAgent.getRegion())
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    F32 min_z = BIG;
    for (LLViewerRegion* regionp : world->getRegionList())
    {
        if (regionp && regionp->isAlive())
        {
            min_z = llmin(min_z, regionp->getWaterHeight());
        }
    }
    mMinZ = min_z < BIG ? min_z : gAgent.getRegion()->getWaterHeight();

    const U64 stamp = terrainStamp();
    bool changed = false;
    // Coarsest first: each level's open-sea classification starts from the coarser one's.
    for (S32 k = LEVELS - 1; k >= 0; --k)
    {
        const F64 t = TEXEL_M[k];
        const F64 half = N * t * 0.5;
        // Recentre when the camera is an eighth of the window from its centre; the corner snaps to the texel grid,
        // so the texels a recentre keeps hold exactly the values they had.
        const bool far_off = !mHaveOrigin[k]
            || fabs(cam.mdV[VX] - (mOrigin[k].mdV[VX] + half)) > half * 0.25
            || fabs(cam.mdV[VY] - (mOrigin[k].mdV[VY] + half)) > half * 0.25;
        if (far_off)
        {
            mOrigin[k].mdV[VX] = floor((cam.mdV[VX] - half) / t) * t;
            mOrigin[k].mdV[VY] = floor((cam.mdV[VY] - half) / t) * t;
            mOrigin[k].mdV[VZ] = 0.0;
            mHaveOrigin[k] = true;
            mLandValid[k] = false;
        }
        // A coarser level that changed re-classifies this one too.
        if (!mLandValid[k] || changed || (stamp != mTerrainStamp[k] && now >= mTerrainAt[k]))
        {
            rasterTerrain(k);
            classifyOpen(k);
            mTerrainStamp[k] = stamp;
            mTerrainAt[k] = now + TERRAIN_INTERVAL_SECS;
            changed = true;
        }
    }
    if (changed || !mObjectsValid || now >= mNextObjects)
    {
        rasterObjects();
        mNextObjects = now + OBJECT_INTERVAL_SECS;
        for (S32 k = 0; k < LEVELS; ++k)
        {
            solve(k, mSrcAll[k], mS[k], &mD[k]);
            solve(k, mSrcDry[k], mSDry[k], nullptr);
        }
        uploadLevels();
    }
    // [OPEN SEA SHORES 2026-10-26] The terrain blocks: recentred like the levels, re-rastered when the terrain changes.
    bool terrain_uploaded = true;
    for (S32 b = 0; b < TBLOCKS; ++b)
    {
        const F64 t = TERRAIN_M[b];
        const F64 half = N * t * 0.5;
        const bool far_off = !mHaveTOrigin[b]
            || fabs(cam.mdV[VX] - (mTOrigin[b].mdV[VX] + half)) > half * 0.25
            || fabs(cam.mdV[VY] - (mTOrigin[b].mdV[VY] + half)) > half * 0.25;
        if (far_off)
        {
            mTOrigin[b].mdV[VX] = floor((cam.mdV[VX] - half) / t) * t;
            mTOrigin[b].mdV[VY] = floor((cam.mdV[VY] - half) / t) * t;
            mTOrigin[b].mdV[VZ] = 0.0;
            mHaveTOrigin[b] = true;
            mTerrValid[b] = false;
        }
        if (!mTerrValid[b] || (stamp != mTerrStamp[b] && now >= mTerrAt[b]))
        {
            rasterTerrainMax(b);
            mTerrStamp[b] = stamp;
            mTerrAt[b] = now + TERRAIN_INTERVAL_SECS;
            if (mTex && mTexAllocated)
            {
                uploadTerrain(b);
            }
            else
            {
                terrain_uploaded = false;
            }
        }
    }
    if (mTex && mTexAllocated)
    {
        if (!terrain_uploaded)
        {
            for (S32 b = 0; b < TBLOCKS; ++b) uploadTerrain(b);
        }
        mReady = true;
    }
}

// waterV.glsl wolfClearSample(), term for term.
bool WolfWaterClearance::sample(F32 ax, F32 ay, Sample& out) const
{
    out = Sample();
    if (!mReady)
    {
        return false;
    }
    bool any = false;
    for (S32 k = LEVELS - 1; k >= 0; --k)
    {
        const F32 t = TEXEL_M[k];
        const LLVector2 o = levelOriginAgent(k);
        const F32 gx = (ax - o.mV[VX]) / t, gy = (ay - o.mV[VY]) / t;   // texel units, centres at i + 0.5
        const F32 lo = 0.5f, hi = N - 0.5f;
        if (gx < lo || gy < lo || gx > hi || gy > hi)
        {
            continue;
        }
        const F32 d_edge = llmin(llmin(gx - lo, gy - lo), llmin(hi - gx, hi - gy)) * t;
        // Bilinear, as the GPU filters it.
        const F32 fx = gx - 0.5f, fy = gy - 0.5f;
        const S32 x0 = llclamp((S32)floorf(fx), 0, N - 1), y0 = llclamp((S32)floorf(fy), 0, N - 1);
        const S32 x1 = llmin(x0 + 1, N - 1), y1 = llmin(y0 + 1, N - 1);
        const F32 u = llclamp(fx - x0, 0.f, 1.f), v = llclamp(fy - y0, 0.f, 1.f);
        auto bil = [&](const std::vector<F32>& g)
        {
            const F32 a0 = g[(size_t)y0 * N + x0] * (1.f - u) + g[(size_t)y0 * N + x1] * u;
            const F32 a1 = g[(size_t)y1 * N + x0] * (1.f - u) + g[(size_t)y1 * N + x1] * u;
            return a0 * (1.f - v) + a1 * v;
        };
        const F32 outside = mMinZ + ALPHA * d_edge;
        out.mSAll = llmax(out.mSAll, llmin(bil(mS[k]), outside));
        out.mD = llmax(out.mD, llmin(bil(mD[k]), d_edge));
        out.mSDry = llmax(out.mSDry, llmin(bil(mSDry[k]), outside));
        // The finest level holding the point, faded in over 32 m from its edge.
        const F32 fade = llclamp(d_edge / 32.f, 0.f, 1.f);
        out.mOpen = out.mOpen + (bil(mOpenW[k]) - out.mOpen) * (fade * fade * (3.f - 2.f * fade));
        any = true;
    }
    return any;
}

F32 WolfWaterClearance::latticeStepAt(F32 ax, F32 ay)
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 sx = llclamp(fabsf(ax - cam.mV[VX]) * 0.04f, 2.f, 128.f);
    const F32 sy = llclamp(fabsf(ay - cam.mV[VY]) * 0.04f, 2.f, 128.f);
    return llmax(sx, sy);
}

// waterV.glsl wolfSoftCap.
F32 WolfWaterClearance::softCap(F32 dz, F32 up, F32 calm_ref)
{
    if (dz >= 0.f)
    {
        return tanhCap(dz, up);
    }
    const F32 g = llclamp(up / llmax(calm_ref, 0.01f), 0.f, 1.f);
    return dz * (TROUGH_KEEP + (1.f - TROUGH_KEEP) * g);
}

// waterV.glsl `up` (the [DRY BUILDS] cap), with the lattice step taken about the camera and the camera guard.
F32 WolfWaterClearance::upwardRoom(F32 ax, F32 ay, F32 plane_z, F32 pixel_held) const
{
    Sample cs;
    if (!sample(ax, ay, cs))
    {
        return -BIG;
    }
    const F32 dead = deadZone(latticeStepAt(ax, ay));
    const F32 up_all = cs.mSAll - ALPHA * dead - plane_z;
    const F32 up_dry = cs.mSDry - ALPHA * dead - plane_z;
    F32 up = up_all + (up_dry - up_all) * llclamp(pixel_held, 0.f, 1.f);
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (cam.mV[VZ] > plane_z)
    {
        const F32 dx = ax - cam.mV[VX], dy = ay - cam.mV[VY];
        up = llmin(up, cam.mV[VZ] - EYE_CLEAR_M - plane_z + ALPHA * (sqrtf(dx * dx + dy * dy) - dead));
    }
    return up;
}
