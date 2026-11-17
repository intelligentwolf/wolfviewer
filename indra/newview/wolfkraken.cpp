/**
 * @file wolfkraken.cpp
 * @brief WolfViewer: now and then a Kraken rises in the open sea (ticket 823, Indi Moon's idea). See wolfkraken.h.
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
#include "wolfkraken.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfaltitudesky.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"
#include "wolfsoundeffects.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

// Source for every figure below: WolfStorm js/world/wolf_kraken.js (the same constants and rules, copied verbatim) and the
// shared spec region-terrain-work/kraken-2026-11-17/README.md, which says which figures are CHOSEN (game tuning).
namespace
{
    // the schedule (wolf_kraken.js CELL_M ... SALT; 'KRAK')
    constexpr F64 CELL_M = 512.0, SLOT_S = 900.0, CHANCE = 0.35, SHOW_S = 80.0, LEAD_S = 10.0;
    constexpr U32 SALT = 0x4b52414bu;
    constexpr F64 CHECK_S = 2.0;
    // where
    constexpr F32 SEE_M = 550.f, CAMERA_HIGH_M = 500.f;
    constexpr F32 DEEP_CENTRE = 12.f, DEEP_RING1 = 8.f, RING1_M = 20.f, DEEP_RING2 = 4.f, RING2_M = 40.f, BUILD_CLEAR_M = 45.f;
    // the timeline, seconds from the start
    constexpr F32 T_CHURN = 8.f, T_BREACH = 8.5f, T_RISEN = 13.f, T_ARMS = 9.f, ARMS_SPREAD_S = 2.5f, ARM_GROW_S = 2.5f;
    const F32 T_FEED[2] = { 11.f, 11.6f };
    constexpr F32 T_GRAB = 16.f, GRAB_REACH_S = 4.f, GRAB_HOLD_END = 30.f, GRAB_FREE_S = 4.f, GRAB_M = 70.f;
    const F32 LASH[2] = { 20.f, 40.f };
    constexpr F32 LASH_RAISE_S = 3.f, LASH_SLAM_S = 1.f, LASH_HOLD_S = 1.5f, LASH_BACK_S = 2.5f;
    const F32 ROARS[2] = { 10.f, 36.f };
    constexpr F32 T_SINK = 58.f, SINK_ARM_STEP_S = 0.5f, SINK_ARM_S = 4.f, T_DOWN = 62.f, T_GONE = 72.f;
    // the creature
    constexpr F32 BODY_H = 15.f;
    constexpr S32 N_BODY = 11;
    const F32 BODY[N_BODY][2] = {   // s (0 crown .. 1 top of the head), radius / BODY_H
        { 0.f, 0.17f }, { 0.07f, 0.21f }, { 0.16f, 0.235f }, { 0.27f, 0.22f }, { 0.40f, 0.27f }, { 0.55f, 0.31f },
        { 0.70f, 0.30f }, { 0.82f, 0.25f }, { 0.91f, 0.17f }, { 0.97f, 0.08f }, { 1.0f, 0.0f } };
    constexpr S32 BODY_SIDES = 16;
    constexpr F32 BODY_DEPTH = 0.88f, LEAN_DEG = 35.f, EYE_S = 0.16f, EYE_DEG = 58.f, EYE_R = 1.5f;
    constexpr S32 SEGS = 28, SIDES = 8;
    constexpr F32 ARM_LEN_LO = 22.f, ARM_LEN_HI = 28.f, FEED_LEN = 34.f, ARM_R = 1.35f, FEED_R = 0.7f, ARM_BASE_M = 2.6f;
    constexpr F32 WAVE_HZ = 0.22f;
    constexpr S32 N_PALETTES = 3;
    const F32 PALETTES[N_PALETTES][4][3] = {   // dark, mid, light (belly), sucker
        { { 0.36f, 0.05f, 0.07f }, { 0.62f, 0.13f, 0.12f }, { 0.86f, 0.52f, 0.45f }, { 0.95f, 0.83f, 0.72f } },   // crimson
        { { 0.22f, 0.06f, 0.18f }, { 0.45f, 0.14f, 0.30f }, { 0.80f, 0.55f, 0.62f }, { 0.94f, 0.82f, 0.85f } },   // abyssal violet
        { { 0.40f, 0.15f, 0.05f }, { 0.70f, 0.32f, 0.12f }, { 0.92f, 0.66f, 0.48f }, { 0.97f, 0.88f, 0.74f } },   // rust
    };
    const LLVector3 IRIS(0.97f, 0.74f, 0.12f), PUPIL(0.02f, 0.02f, 0.02f), EYE_RIM(0.12f, 0.03f, 0.03f), FOAM(0.93f, 0.95f, 0.97f);
    // the spray
    constexpr S32 MAX_DROPS = 900;
    constexpr F32 G_MS2 = 9.81f;
    // [VIEWER] the deferred pass has no soft alpha points: each drop is a small opaque blob (wolfsealife.cpp's spray)
    constexpr F32 DROP_SCALE = 0.3f;
    // [VIEWER] the sound is placed this far along its direction from the camera (wolfsealife.cpp CALL_EAR_M)
    constexpr F32 CALL_EAR_M = 4.f;
    constexpr F32 UNDER_GAIN = 1.3f;

    // Sounds: app_settings/wolfkraken (SOURCES.md; region-terrain-work/kraken-2026-11-17/tools/make_sounds.py, our own
    // synthesis). The uuids name each file's decoded copy in the sound cache (WolfWeatherSound::prepare); made for this.
    enum { SND_RUMBLE = 0, SND_ROAR = 1, SND_SPLASH = 2, SND_SINK = 3, SND_COUNT = 4 };
    struct SoundKind { WolfWeatherSound::Recording mRec; F32 mBase, mFull, mMax; };
    const SoundKind SND[SND_COUNT] = {
        { { "wolf_kraken_rumble.ogg", "85f18649-2c6d-4948-b5b5-d0d399282b43", nullptr, false, "wolfkraken" }, 0.6f, 40.f, 700.f },
        { { "wolf_kraken_roar.ogg", "e1b3ec35-c403-4e71-88ab-946ec1dba821", nullptr, false, "wolfkraken" }, 0.9f, 60.f, 900.f },
        { { "wolf_kraken_splash.ogg", "78e83a53-f68c-450f-a8c6-51c2f8c06a78", nullptr, false, "wolfkraken" }, 0.7f, 30.f, 500.f },
        { { "wolf_kraken_sink.ogg", "4e3647ad-0395-4f99-b02b-2551ca45eb75", nullptr, false, "wolfkraken" }, 0.6f, 40.f, 600.f },
    };

    F32 rnd(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }
    F32 clampf(F32 v, F32 lo, F32 hi) { return llclamp(v, lo, hi); }
    F32 smoothstep(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
    F32 ease(F32 x) { return smoothstep(0.f, 1.f, x); }
    F32 ease_out(F32 x) { const F32 t = llclamp(x, 0.f, 1.f); return 1.f - (1.f - t) * (1.f - t); }
    F32 len2(F32 x, F32 y) { return sqrtf(x * x + y * y); }
    LLVector3 nrm(const LLVector3& v) { LLVector3 r = v; if (r.normVec() <= 0.f) r.setVec(0.f, 0.f, 1.f); return r; }
    LLVector3 mixv(const LLVector3& a, const LLVector3& b, F32 t) { return a + (b - a) * t; }

    /** wolf_kraken.js mix32: the "lowbias32" finaliser on U32 (unsigned wrap-around = JavaScript's Math.imul >>> 0). */
    U32 mix32(U32 h)
    {
        h ^= h >> 16; h *= 0x7feb352du;
        h ^= h >> 15; h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }
    /** wolf_kraken.js hashU: (slot ^ SALT, cell x, cell y, k) chained; 0 <= u < 1 (F64, as JavaScript's numbers). */
    F64 hash_u(S32 cx, S32 cy, U32 slot, U32 k)
    {
        U32 h = mix32(slot ^ SALT);
        h = mix32(h ^ (U32)cx);
        h = mix32(h ^ (U32)cy);
        h = mix32(h ^ k);
        return (F64)h / 4294967296.0;
    }

    LLVector3d camera_global() { return gAgentCamera.getCameraPositionGlobal(); }
}

WolfKraken::WolfKraken() {}
WolfKraken::~WolfKraken() {}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the schedule (wolf_kraken.js plan, looks, _check, _resolve)
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfKraken::plan(S32 cx, S32 cy, U32 slot, Plan& out)
{
    if (hash_u(cx, cy, slot, 0) >= CHANCE) return false;
    out.mCx = cx; out.mCy = cy; out.mSlot = slot;
    out.mStart = (F64)slot * SLOT_S + LEAD_S + std::floor(hash_u(cx, cy, slot, 1) * (SLOT_S - SHOW_S - 2.0 * LEAD_S));
    for (U32 k = 0; k < (U32)CANDIDATES; ++k)
    {
        out.mCand[k][0] = (F64)cx * CELL_M + hash_u(cx, cy, slot, 2 + 2 * k) * CELL_M;
        out.mCand[k][1] = (F64)cy * CELL_M + hash_u(cx, cy, slot, 3 + 2 * k) * CELL_M;
    }
    out.mKey = std::to_string(cx) + "," + std::to_string(cy) + "," + std::to_string(slot);
    return true;
}

void WolfKraken::looks(S32 cx, S32 cy, U32 slot, Look& out)
{
    auto u = [&](U32 k) { return hash_u(cx, cy, slot, k); };
    out.mFacing = (F32)(u(10) * F_TWO_PI);
    out.mSize = (F32)(0.9 + 0.3 * u(12));
    out.mPalette = llmin(N_PALETTES - 1, (S32)std::floor(u(13) * N_PALETTES));
    out.mSeed = mix32((U32)std::floor(u(14) * 4294967295.0));
    for (U32 i = 0; i < (U32)ARMS; ++i)
    {
        out.mArms[i].mEmerge = T_ARMS + (F32)u(20 + i) * ARMS_SPREAD_S;
        out.mArms[i].mPhase = (F32)(u(30 + i) * F_TWO_PI);
        out.mArms[i].mPhase2 = (F32)(u(40 + i) * F_TWO_PI);
        out.mArms[i].mLen = ARM_LEN_LO + (F32)u(50 + i) * (ARM_LEN_HI - ARM_LEN_LO);
    }
    // the foam: half in a thick collar round the body (2.5-7 m out), half spread to 18 m
    for (U32 p = 0; p < (U32)FOAM_N; ++p)
    {
        const F32 r = (F32)u(100 + 3 * p);
        out.mFoam[p].mRho = (p % 2) ? 2.5f + 4.5f * r : 4.f + 14.f * powf(r, 1.5f);
        out.mFoam[p].mAlpha = (F32)(u(101 + 3 * p) * F_TWO_PI);
        out.mFoam[p].mSize = 0.35f + 0.8f * (F32)u(102 + 3 * p);
    }
}

/** UTC seconds with a fraction: time_corrected() (lltimer.cpp: time(NULL) + gUTCOffset, the grid's clock) gives whole
 *  seconds, the frame timer the fraction; re-anchored whenever the two drift more than a second apart. */
F64 WolfKraken::utcNow()
{
    const F64 el = LLFrameTimer::getElapsedSeconds();
    const F64 whole = (F64)time_corrected();
    F64 est = mUtcAnchor + (el - mElapsedAnchor);
    if (mElapsedAnchor < 0.0 || est < whole - 1.0 || est > whole + 2.0)
    {
        mUtcAnchor = whole + 0.5;
        mElapsedAnchor = el;
        est = mUtcAnchor;
    }
    return est;
}

bool WolfKraken::resolve(const Plan& p, const LLVector3d& cam_global, Decided& out)
{
    auto it = mDecided.find(p.mKey);
    if (it != mDecided.end()) { out = it->second; return true; }
    for (S32 k = 0; k < CANDIDATES; ++k)
    {
        const Judge j = qualify(p.mCand[k][0], p.mCand[k][1], cam_global);
        if (j == UNKNOWN) return false;   // not yet: never another place than the viewers that can judge it
        if (j == YES)
        {
            out.mSpot = true; out.mGx = p.mCand[k][0]; out.mGy = p.mCand[k][1];
            mDecided[p.mKey] = out;
            return true;
        }
    }
    out = Decided();
    mDecided[p.mKey] = out;
    return true;
}

void WolfKraken::check(const LLVector3d& cam_global, F64 utc)
{
    if (mDecided.size() > 256) mDecided.clear();
    const S32 cx0 = (S32)std::floor(cam_global.mdV[VX] / CELL_M), cy0 = (S32)std::floor(cam_global.mdV[VY] / CELL_M);
    const U32 slot = (U32)std::floor(utc / SLOT_S);
    bool found = false;
    Plan best;
    Decided best_spot;
    F64 best_d = 1e18;
    for (S32 cy = cy0 - 1; cy <= cy0 + 1; ++cy)
    {
        for (S32 cx = cx0 - 1; cx <= cx0 + 1; ++cx)
        {
            Plan p;
            if (!plan(cx, cy, slot, p) || utc < p.mStart || utc >= p.mStart + SHOW_S) continue;
            Decided s;
            if (!resolve(p, cam_global, s) || !s.mSpot) continue;
            const F64 d = sqrt((s.mGx - cam_global.mdV[VX]) * (s.mGx - cam_global.mdV[VX]) + (s.mGy - cam_global.mdV[VY]) * (s.mGy - cam_global.mdV[VY]));
            if (d <= SEE_M && d < best_d) { found = true; best = p; best_spot = s; best_d = d; }
        }
    }
    if (!found) { if (mHave && !mForced) mHave = false; return; }
    if (mHave && mPlan.mKey == best.mKey) return;
    if (mHave && mForced) return;
    F32 depth = DEEP_CENTRE, water = 20.f;
    if (!depthAt(best_spot.mGx, best_spot.mGy, depth, water))
    {
        LLViewerRegion* r = gAgent.getRegion();
        water = r ? r->getWaterHeight() : 20.f;
        depth = DEEP_CENTRE;
    }
    begin(best, best_spot.mGx, best_spot.mGy, water, depth, utc, false);
}

void WolfKraken::begin(const Plan& p, F64 gx, F64 gy, F32 water, F32 depth, F64 utc, bool forced)
{
    mPlan = p;
    looks(p.mCx, p.mCy, p.mSlot, mLook);
    mGx = gx; mGy = gy; mWater = water; mDepth = depth;
    mForced = forced;
    mPrevT = utc - p.mStart;
    mDrops.clear();
    mBoat = nullptr;
    mGrabDecided = false;
    mHave = true;
    LL_INFOS("WolfKraken") << (forced ? "forced " : "") << "sighting " << p.mKey << " at " << llformat("%.1f,%.1f", gx, gy)
                           << " global (water " << water << "), " << llformat("%.1f", mPrevT) << " s in" << LL_ENDL;
}

/** Source: wolf_kraken.js force() - debug and tests: a sighting now, over deep water in view 60-140 m ahead. */
bool WolfKraken::force(const LLVector3d& cam_global)
{
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 yaw = atan2f(at.mV[VY], at.mV[VX]);
    for (S32 tries = 0; tries < 80; ++tries)
    {
        const F32 a = yaw + rnd(-0.35f, 0.35f), d = rnd(60.f, 140.f);
        const F64 gx = cam_global.mdV[VX] + cos(a) * d, gy = cam_global.mdV[VY] + sin(a) * d;
        if (qualify(gx, gy, cam_global) != YES) continue;
        const F64 utc = utcNow();
        Plan p;
        p.mKey = "forced:" + std::to_string((S64)utc);
        p.mCx = ll_rand(1000000); p.mCy = ll_rand(1000000);
        p.mSlot = (U32)std::floor(utc / SLOT_S);
        p.mStart = utc;
        F32 depth = DEEP_CENTRE, water = 20.f;
        depthAt(gx, gy, depth, water);
        begin(p, gx, gy, water, depth, utc, true);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the place (wolf_kraken.js _depthAt, _regionOn, _qualify, _buildNear, _findBoat)
// ═══════════════════════════════════════════════════════════════════════════════════════

/** The water over the terrain at a global point, and that region's water height. False (not known) off every region the
 *  viewer holds or over a patch whose terrain has not come (LLSurfacePatch::getHasReceivedData; an empty patch reads 0 m:
 *  wolfsealife.cpp depth(), [SEALIFE AGROUND 2026-11-08]). */
bool WolfKraken::depthAt(F64 gx, F64 gy, F32& depth, F32& water) const
{
    const LLVector3d g(gx, gy, 0.0);
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosGlobal(g);
    if (!r) return false;
    if (r->getLand().getPatchesPerEdge() <= 0) return false;
    const LLVector3 pos_region = r->getPosRegionFromGlobal(g);
    const LLSurfacePatch* patch = r->getLand().resolvePatchRegion(pos_region);
    if (!patch || !patch->getHasReceivedData()) return false;
    const F32 h = r->getLand().resolveHeightRegion(pos_region);
    if (!std::isfinite(h)) return false;
    water = r->getWaterHeight();
    depth = llmax(0.f, water - h);
    return true;
}

/** This region's `kraken.on`; a neighbour by `kraken.near` (one within 600 m that is not listed is off: wa_kraken_near). */
bool WolfKraken::regionOn(LLViewerRegion* r) const
{
    if (!WolfRegionWeather::instanceExists() || !WolfRegionWeather::instance().krakenAllowed()) return false;
    if (r == gAgent.getRegion()) return true;
    return WolfRegionWeather::instance().krakenNear(r->getRegionID());
}

WolfKraken::Judge WolfKraken::qualify(F64 gx, F64 gy, const LLVector3d& cam_global) const
{
    const F64 dx = gx - cam_global.mdV[VX], dy = gy - cam_global.mdV[VY];
    if (sqrt(dx * dx + dy * dy) > SEE_M) return UNKNOWN;
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosGlobal(LLVector3d(gx, gy, 0.0));
    if (!r) return UNKNOWN;
    if (!regionOn(r)) { LL_DEBUGS("WolfKraken") << "place " << llformat("%.1f,%.1f", gx, gy) << ": region switched off" << LL_ENDL; return NO; }
    F32 depth = 0.f, water = 0.f;
    if (!depthAt(gx, gy, depth, water)) return UNKNOWN;
    if (depth < DEEP_CENTRE) { LL_DEBUGS("WolfKraken") << "place " << llformat("%.1f,%.1f", gx, gy) << ": " << depth << " m deep" << LL_ENDL; return NO; }
    const F32 needs[2] = { DEEP_RING1, DEEP_RING2 }, radii[2] = { RING1_M, RING2_M };
    for (S32 ring = 0; ring < 2; ++ring)
    {
        for (S32 k = 0; k < 8; ++k)
        {
            const F32 a = (F32)k * F_PI / 4.f;
            F32 d = 0.f, w = 0.f;
            if (!depthAt(gx + cosf(a) * radii[ring], gy + sinf(a) * radii[ring], d, w)) return UNKNOWN;
            if (d < needs[ring])
            {
                LL_DEBUGS("WolfKraken") << "place " << llformat("%.1f,%.1f", gx, gy) << ": " << d << " m deep " << radii[ring] << " m off" << LL_ENDL;
                return NO;
            }
        }
    }
    if (buildNear(gx, gy, water)) { LL_DEBUGS("WolfKraken") << "place " << llformat("%.1f,%.1f", gx, gy) << ": a build near" << LL_ENDL; return NO; }
    return YES;
}

/** A root object (not an avatar, an attachment or a boat moving by) within BUILD_CLEAR_M of the point, its own horizontal
 *  half size added, up to 60 m over the water (a skybox far above does not count). */
bool WolfKraken::buildNear(F64 gx, F64 gy, F32 water) const
{
    const S32 n = gObjectList.getNumObjects();
    for (S32 i = 0; i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);   // null when dead (llviewerobjectlist.h getObject)
        if (!o || o->getParent() || o->isAvatar() || o->isAttachment()) continue;
        const LLVector3d p = o->getPositionGlobal();
        if (p.mdV[VZ] > (F64)water + 60.0) continue;
        const LLVector3 v = o->getVelocity();
        if (len2(v.mV[VX], v.mV[VY]) > 0.5f) continue;   // a boat passing by is not a build
        const LLVector3 s = o->getScale();
        // a flat, level plate (a sea-bed or sea-surface megaprim, seen on Blue Ocean 2026-11-17: 1024 x 1024 x 0 at the
        // water, 128 x 128 x 0 on the bed) is not a build: under 0.5 m thick, 16 m or more across, its face level (the
        // local z axis within 18 degrees of up). Source: wolf_kraken.js _buildNear
        const LLVector3 up = LLVector3::z_axis * o->getRotation();
        if (s.mV[VZ] < 0.5f && llmin(s.mV[VX], s.mV[VY]) >= 16.f && fabsf(up.mV[VZ]) > 0.95f) continue;
        const F64 dx = p.mdV[VX] - gx, dy = p.mdV[VY] - gy;
        if (sqrt(dx * dx + dy * dy) - (F64)llmax(s.mV[VX], s.mV[VY]) / 2.0 < BUILD_CLEAR_M)
        {
            LL_DEBUGS("WolfKraken") << "build near: " << o->getID() << " at " << p << " scale " << s << LL_ENDL;
            return true;
        }
    }
    return false;
}

/** The nearest moving root object on the water within GRAB_M of the spot (wolfsealife.cpp rideCheck's test, any speed
 *  from 0.5 to 20 m/s). */
LLViewerObject* WolfKraken::findBoat() const
{
    LLViewerObject* best = nullptr;
    F64 best_d = GRAB_M;
    for (const LLPointer<LLViewerObject>& op : gObjectList.getActiveObjects())
    {
        LLViewerObject* o = op.get();
        if (!o || o->isDead() || o->isAvatar() || o->getParent() || o->isAttachment()) continue;
        const LLVector3d p = o->getPositionGlobal();
        if (fabs(p.mdV[VZ] - (F64)mWater) > 4.0) continue;
        const LLVector3 v = o->getVelocity();
        const F32 sp = len2(v.mV[VX], v.mV[VY]);
        if (sp < 0.5f || sp > 20.f) continue;
        const F64 dx = p.mdV[VX] - mGx, dy = p.mdV[VY] - mGy, d = sqrt(dx * dx + dy * dy);
        if (d < best_d) { best = o; best_d = d; }
    }
    return best;
}

LLVector3 WolfKraken::spotAgent() const
{
    return gAgent.getPosAgentFromGlobal(LLVector3d(mGx, mGy, (F64)mWater));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the animation (wolf_kraken.js _crownZ, _armOut, _spray, _bubbles, _moveDrops, _events)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfKraken::crownZ(F32 t) const
{
    // never below the sea bed: most of the grid's seas are about 20 m deep, so the lowest the crown goes is the depth there
    // less 2.5 m (the arms grow 1 m below it; in body units), between 12 and 24. Source: wolf_kraken.js _crownZ
    const F32 low = llclamp((mDepth - 2.5f) / mLook.mSize, 12.f, 24.f), mid = llmin(16.f, low);
    F32 z;
    if (t < T_CHURN) z = -low + (low - mid) * ease(t / T_CHURN);   // a dark shape coming up, still under
    else if (t < T_RISEN) z = -mid + (mid + 0.3f) * ease_out((t - T_CHURN) / (T_RISEN - T_CHURN));   // the breach
    else z = 0.3f + 0.4f * sinf(F_TWO_PI * (t - T_RISEN) / 6.f);
    const F32 down = llmin(22.f, low);
    if (t > T_DOWN) z = z + (-down - z) * ease((t - T_DOWN) / (T_GONE - T_DOWN));
    return z * mLook.mSize;
}

F32 WolfKraken::armOut(S32 i, F32 t) const
{
    const F32 start = i < ARMS ? mLook.mArms[i].mEmerge : T_FEED[i - ARMS];
    const F32 out = ease((t - start) / ARM_GROW_S);
    const F32 back = T_SINK + (F32)i * SINK_ARM_STEP_S * (i < ARMS ? 1.f : 0.5f);
    return out * (1.f - ease((t - back) / SINK_ARM_S));
}

void WolfKraken::spray(S32 n, const LLVector3& at, F32 vz_lo, F32 vz_hi, F32 out, F32 size_lo, F32 size_hi)
{
    for (S32 i = 0; i < n && (S32)mDrops.size() < MAX_DROPS; ++i)
    {
        const F32 a = ll_frand(F_TWO_PI), o = out * rnd(0.3f, 1.f);
        Drop d;
        d.mPos = at;
        d.mVel = LLVector3(cosf(a) * o, sinf(a) * o, rnd(vz_lo, vz_hi));
        d.mLife = 4.f;
        d.mSize = rnd(size_lo, size_hi);
        mDrops.push_back(d);
    }
}

void WolfKraken::bubbles(S32 n, F32 radius)
{
    for (S32 i = 0; i < n && (S32)mDrops.size() < MAX_DROPS; ++i)
    {
        const F32 a = ll_frand(F_TWO_PI), r = sqrtf(ll_frand()) * radius;
        Drop d;
        d.mPos = LLVector3(cosf(a) * r, sinf(a) * r, 0.02f);
        d.mVel = LLVector3(0.f, 0.f, rnd(0.5f, 1.4f));
        d.mLife = rnd(0.5f, 1.2f);
        d.mSize = rnd(0.25f, 0.6f);
        d.mBubble = true;
        mDrops.push_back(d);
    }
}

void WolfKraken::moveDrops(F32 dt)
{
    for (Drop& p : mDrops)
    {
        p.mAge += dt;
        if (!p.mBubble) p.mVel.mV[VZ] -= G_MS2 * dt;
        p.mPos += p.mVel * dt;
    }
    mDrops.erase(std::remove_if(mDrops.begin(), mDrops.end(), [](const Drop& p)
        { return p.mAge >= p.mLife || (!p.mBubble && p.mPos.mV[VZ] < -0.2f && p.mVel.mV[VZ] < 0.f); }), mDrops.end());
}

void WolfKraken::events(F64 prev_t, F64 t)
{
    auto crossed = [prev_t, t](F64 at) { return prev_t < at && t >= at; };
    const F32 tf = (F32)t;
    if (crossed(0.05)) sound(SND_RUMBLE, LLVector3::zero);
    if (crossed(T_BREACH)) { spray(320, LLVector3::zero, 6.f, 15.f, 4.5f, 0.8f, 2.f); sound(SND_SPLASH, LLVector3::zero); }
    for (F32 r : ROARS) if (crossed(r)) sound(SND_ROAR, LLVector3(0.f, 0.f, 3.f));
    if (crossed(T_SINK)) sound(SND_SINK, LLVector3::zero);
    // each arm breaking the surface as it rises: a burst of spray where it comes out
    const Frame bf = bodyFrame(tf);
    for (S32 i = 0; i < ARMS; ++i)
    {
        if (!crossed(mLook.mArms[i].mEmerge + 0.4f)) continue;
        const F32 ang = mLook.mFacing + ((F32)i + 0.5f) * F_TWO_PI / (F32)ARMS;
        const LLVector3 base = bf.mC + LLVector3(cosf(ang), sinf(ang), 0.f) * (ARM_BASE_M * mLook.mSize) + LLVector3(0.f, 0.f, -mLook.mSize);
        spray(25, LLVector3(base.mV[VX], base.mV[VY], 0.f), 3.f, 7.f, 1.5f, 0.4f, 0.9f);
    }
    const bool holding = mBoat.notNull() && !mBoat->isDead();
    if (holding && crossed(T_GRAB + GRAB_REACH_S * 0.8f))
    {
        const LLVector3d bp = mBoat->getPositionGlobal();
        const LLVector3 rel((F32)(bp.mdV[VX] - mGx), (F32)(bp.mdV[VY] - mGy), 0.f);
        spray(70, rel, 3.f, 8.f, 3.f, 0.5f, 1.2f);
        sound(SND_SPLASH, rel);
    }
    // a lash's slam: spray all along the part on the water (not while both long ones hold a boat)
    for (S32 k = 0; k < FEEDERS; ++k)
    {
        const F32 at = LASH[k] + LASH_RAISE_S + LASH_SLAM_S * 0.85f;
        if (!crossed(at)) continue;
        if (holding && at > T_GRAB && at < GRAB_HOLD_END + GRAB_FREE_S) continue;
        // the slam line itself (feederPose's, wolf_kraken.js _slamLine): only where it lies on the water matters here
        const S32 i = ARMS + k;
        const F32 ang = mLook.mFacing + (k == 0 ? 0.45f : -0.45f);
        const LLVector3 o(cosf(ang), sinf(ang), 0.f);
        const LLVector3 base = bf.mC + o * (ARM_BASE_M * mLook.mSize * 0.7f) + LLVector3(0.f, 0.f, -mLook.mSize);
        const F32 L2 = FEED_LEN * mLook.mSize * llmax(0.02f, armOut(i, tf));
        const LLVector3 f(cosf(mLook.mFacing), sinf(mLook.mFacing), 0.f), side_dir(-f.mV[VY], f.mV[VX], 0.f);
        LLVector3 last;
        for (S32 j = 0; j <= SEGS; ++j)
        {
            const F32 u = (F32)j / (F32)SEGS, reach = u * L2;
            const F32 side = 2.2f * sinf(u * 2.5f * F_PI) * smoothstep(0.3f, 0.6f, u) + 3.f * smoothstep(0.85f, 1.f, u);
            const LLVector3 p = base + f * (reach * 0.95f) + o * (u * 2.f) + side_dir * side;
            last = p;
            if (j >= (S32)std::floor((F32)SEGS * 0.4f)) spray(9, LLVector3(p.mV[VX], p.mV[VY], 0.f), 3.f, 10.f, 3.5f, 0.4f, 1.2f);
        }
        sound(SND_SPLASH, LLVector3(last.mV[VX], last.mV[VY], 0.f));
    }
    if (crossed(T_GRAB) && !mGrabDecided)
    {
        mGrabDecided = true;
        mBoat = findBoat();
        if (mBoat.notNull()) LL_INFOS("WolfKraken") << "the long tentacles reach for " << mBoat->getID() << LL_ENDL;
    }
}

/** One sound at `rel` (relative to the spot at the water), played as AMBIENT so the Ambient volume governs it, at the Sea
 *  life level of Wolf Sound Effects; the sea life's gentle roll-off (wolfsealife.cpp sound()), louder under the water. */
void WolfKraken::sound(S32 which, const LLVector3& rel)
{
    if (!gAudiop) return;
    const SoundKind& s = SND[which];
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    const LLVector3 at = spotAgent() + rel;
    const F32 d = (at - cam).length();
    if (d > s.mMax) return;
    if (!WolfWeatherSound::instance().recordingReady(s.mRec)) return;   // decoding: the next one will play
    const bool under = camera->cameraUnderWater();
    const F32 indoors = (WolfWeather::instanceExists() && WolfWeather::instance().cameraUnderRoof()) ? 0.35f : 1.f;
    const F32 gain = llclamp(s.mBase * (under ? UNDER_GAIN : 1.f) * (s.mFull / llmax(s.mFull, d)) * indoors
                             * WolfSoundEffects::level(WolfSoundEffects::SEA_LIFE), 0.f, 1.f);
    LLVector3 dir = at - cam;
    if (dir.isExactlyZero()) dir = camera->getAtAxis();
    dir.normVec();
    gAudiop->triggerSound(LLUUID(s.mRec.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPosGlobalFromAgent(cam + dir * CALL_EAR_M));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the shape (wolf_kraken.js _bodyFrame, _armBase, _armLine, _slamLine, _feederPose)
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfKraken::Frame WolfKraken::bodyFrame(F32 t) const
{
    const F32 lean = (LEAN_DEG + 4.f * sinf(F_TWO_PI * 0.15f * t)) * DEG_TO_RAD;
    const F32 fx = cosf(mLook.mFacing), fy = sinf(mLook.mFacing);
    Frame f;
    f.mA = nrm(LLVector3(-fx * sinf(lean), -fy * sinf(lean), cosf(lean)));
    f.mS = nrm(f.mA % LLVector3(fx, fy, 0.f));
    f.mF = f.mS % f.mA;
    f.mC = LLVector3(0.f, 0.f, crownZ(t));
    return f;
}

void WolfKraken::armLine(S32 i, F32 t, const Frame& bf, std::vector<LLVector3>& line, std::vector<LLVector3>& suck)
{
    const F32 size = mLook.mSize;
    const bool feeder = i >= ARMS;
    const F32 out = armOut(i, t);
    const F32 ang = feeder ? mLook.mFacing + (i == ARMS ? 0.45f : -0.45f) : mLook.mFacing + ((F32)i + 0.5f) * F_TWO_PI / (F32)ARMS;
    const LLVector3 o(cosf(ang), sinf(ang), 0.f), l(-o.mV[VY], o.mV[VX], 0.f);
    const LLVector3 base = bf.mC + o * (ARM_BASE_M * size * (feeder ? 0.7f : 1.f)) + LLVector3(0.f, 0.f, -1.f * size);
    const F32 len = (feeder ? FEED_LEN : mLook.mArms[i].mLen) * size;
    const F32 ph = feeder ? (i == ARMS ? 1.1f : 4.0f) : mLook.mArms[i].mPhase;
    const F32 ph2 = feeder ? (i == ARMS ? 2.3f : 5.2f) : mLook.mArms[i].mPhase2;
    const F32 display = smoothstep(T_RISEN - 2.f, T_RISEN + 2.f, t) * (1.f - smoothstep(T_SINK, T_SINK + 3.f, t));
    const F32 amp = (feeder ? 1.2f : 1.6f) * (0.35f + 0.65f * display);
    const F32 theta0 = (feeder ? 70.f : 72.f) * DEG_TO_RAD, kappa0 = feeder ? -1.4f : -1.9f, curl = feeder ? 6.f : 7.f;
    line.resize(SEGS + 1);
    line[0] = base;
    LLVector3 p = base;
    const F32 step = len * llmax(0.02f, out) / (F32)SEGS;
    for (S32 j = 0; j < SEGS; ++j)
    {
        const F32 u = ((F32)j + 0.5f) / (F32)SEGS;
        // the segment's direction at its middle: θ its angle up from the outward direction (a steady outward bend, a
        // travelling wave and the tip's curl), ψ its swing to the side
        F32 th = theta0 + kappa0 * u;
        th += amp * (0.4f + 0.5f * u) * (sinf(F_TWO_PI * WAVE_HZ * t - F_TWO_PI * 1.2f * u + ph) - sinf(F_TWO_PI * WAVE_HZ * t + ph)) / 2.f;
        const F32 c = smoothstep(0.55f, 1.f, u);
        th += curl * c * c * 0.5f;
        F32 ps = (feeder ? 1.4f : 0.9f) * u * sinf(F_TWO_PI * WAVE_HZ * 0.6f * t - F_TWO_PI * 0.8f * u + ph2) * 0.5f;
        if (feeder) ps += (i == ARMS ? 0.5f : -0.5f) * u;   // the two long ones lean apart
        const F32 ct = cosf(th), st = sinf(th), cp = cosf(ps), sp = sinf(ps);
        const LLVector3 d = (o * cp + l * sp) * ct + LLVector3(0.f, 0.f, st);
        p += d * step;
        line[(size_t)j + 1] = p;
    }
    if (feeder) feederPose(i, t, line, base, o, len, out);
    // the sucker side: in the arm's own plane, toward where it curls (T x lateral)
    suck.resize(SEGS + 1);
    for (S32 j = 0; j <= SEGS; ++j)
    {
        const LLVector3 T = nrm(line[(size_t)llmin(SEGS, j + 1)] - line[(size_t)llmax(0, j - 1)]);
        suck[(size_t)j] = nrm(T % l);
    }
}

void WolfKraken::feederPose(S32 i, F32 t, std::vector<LLVector3>& line, const LLVector3& base, const LLVector3& o, F32 len, F32 out)
{
    const S32 k = i - ARMS;
    // grab: both reach for the boat
    if (mBoat.notNull() && !mBoat->isDead())
    {
        const F32 g = ease((t - T_GRAB) / GRAB_REACH_S) * (1.f - ease((t - GRAB_HOLD_END) / GRAB_FREE_S));
        if (g > 0.f)
        {
            const LLVector3d bp = mBoat->getPositionGlobal();
            const LLVector3 s = mBoat->getScale();
            const F32 half_h = llmax(1.f, s.mV[VZ] / 2.f);
            const LLVector3 T((F32)(bp.mdV[VX] - mGx), (F32)(bp.mdV[VY] - mGy), (F32)(bp.mdV[VZ] - (F64)mWater) + half_h + 1.f);
            const F32 rb = llclamp(llmax(s.mV[VX], s.mV[VY]) / 2.f, 1.5f, 6.f) + 0.6f;
            const LLVector3 toward = nrm(LLVector3(T.mV[VX] - base.mV[VX], T.mV[VY] - base.mV[VY], 0.f));
            const F32 dist = len2(T.mV[VX] - base.mV[VX], T.mV[VY] - base.mV[VY]);
            const LLVector3 c1 = base + LLVector3(toward.mV[VX] * 0.15f * dist, toward.mV[VY] * 0.15f * dist, 0.5f * dist + 6.f);
            const LLVector3 c2 = T + LLVector3(-toward.mV[VX] * 0.25f * dist, -toward.mV[VY] * 0.25f * dist, 0.45f * dist + 4.f);
            const S32 wrap_start = (S32)std::floor((F32)SEGS * 0.72f);
            std::vector<LLVector3> pose;
            pose.reserve(SEGS + 1);
            for (S32 j = 0; j <= wrap_start; ++j)
            {
                const F32 s2 = (F32)j / (F32)wrap_start, a = 1.f - s2;
                pose.push_back(base * (a * a * a) + c1 * (3.f * a * a * s2) + c2 * (3.f * a * s2 * s2) + T * (s2 * s2 * s2));
            }
            // the wrap: a coil round the boat, the two tentacles from opposite sides, coming down 1.2 turns
            const F32 a0 = atan2f(-toward.mV[VY], -toward.mV[VX]) + (k ? F_PI : 0.f);
            const S32 n = SEGS - wrap_start;
            const F32 tug = 0.4f * sinf(F_TWO_PI * 0.8f * t);
            for (S32 j = 1; j <= n; ++j)
            {
                const F32 s2 = (F32)j / (F32)n, a = a0 + (k ? -1.f : 1.f) * s2 * 1.2f * F_TWO_PI;
                pose.push_back(T + LLVector3(cosf(a) * rb, sinf(a) * rb, -s2 * (half_h + 1.5f) + tug));
            }
            for (S32 j = 0; j <= SEGS; ++j) line[(size_t)j] = mixv(line[(size_t)j], pose[(size_t)j], g);
            return;
        }
    }
    // lash: one tentacle raised high, then slammed flat on the sea in front
    const F32 t0 = LASH[k];
    if (t < t0 || t >= t0 + LASH_RAISE_S + LASH_SLAM_S + LASH_HOLD_S + LASH_BACK_S) return;
    const LLVector3 f(cosf(mLook.mFacing), sinf(mLook.mFacing), 0.f), side_dir(-f.mV[VY], f.mV[VX], 0.f);
    const F32 L2 = len * llmax(0.02f, out);
    std::vector<LLVector3> raised(SEGS + 1), slam(SEGS + 1);
    for (S32 j = 0; j <= SEGS; ++j)
    {
        const F32 u = (F32)j / (F32)SEGS;
        const F32 sway = 1.5f * sinf(u * 3.f + t);
        raised[(size_t)j] = base + LLVector3(o.mV[VX] * u * 3.f - f.mV[VX] * u * u * 6.f, o.mV[VY] * u * 3.f - f.mV[VY] * u * u * 6.f, u * L2 + sway * u);
        // wolf_kraken.js _slamLine: an arc 8 m high over the first 40 %, then on the surface in a slow S, the tip curled
        const F32 reach = u * L2, s2 = llmin(1.f, u / 0.4f);
        const F32 z = u < 0.4f ? base.mV[VZ] * (1.f - s2) + 0.3f * s2 + 8.f * sinf(F_PI * s2) : 0.3f;
        const F32 side = 2.2f * sinf(u * 2.5f * F_PI) * smoothstep(0.3f, 0.6f, u) + 3.f * smoothstep(0.85f, 1.f, u);
        const LLVector3 flat_p = base + f * (reach * 0.95f) + o * (u * 2.f) + side_dir * side;
        slam[(size_t)j] = LLVector3(flat_p.mV[VX], flat_p.mV[VY], z);
    }
    const F32 tr = t - t0;
    F32 g = 1.f, s2 = 0.f;
    const std::vector<LLVector3>* from = &slam;
    if (tr < LASH_RAISE_S) { from = &raised; g = ease(tr / LASH_RAISE_S); }
    else if (tr < LASH_RAISE_S + LASH_SLAM_S) { s2 = ease((tr - LASH_RAISE_S) / LASH_SLAM_S); }
    else if (tr >= LASH_RAISE_S + LASH_SLAM_S + LASH_HOLD_S) g = 1.f - ease((tr - LASH_RAISE_S - LASH_SLAM_S - LASH_HOLD_S) / LASH_BACK_S);
    const bool slamming = tr >= LASH_RAISE_S && tr < LASH_RAISE_S + LASH_SLAM_S;
    for (S32 j = 0; j <= SEGS; ++j)
    {
        const LLVector3 pose = slamming ? mixv(raised[(size_t)j], slam[(size_t)j], s2) : (*from)[(size_t)j];
        line[(size_t)j] = mixv(line[(size_t)j], pose, g);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the frame
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfKraken::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerKraken", true);
    static LLCachedControl<bool> forced(gSavedSettings, "WolfViewerKrakenForce", false);
    LLViewerRegion* region = gAgent.getRegion();
    const bool wanted = on && region && WolfGrid::showsWolfRegionWeather() && WolfRegionWeather::instanceExists()
                        && WolfRegionWeather::instance().krakenAllowed();
    if (!wanted)
    {
        if (forced) gSavedSettings.setBOOL("WolfViewerKrakenForce", false);
        if (mHave) { mHave = false; mDrops.clear(); mBoat = nullptr; }
        return;   // off (or not a Wolf region): nothing at all
    }
    const LLVector3d cam_global = camera_global();
    if ((F32)cam_global.mdV[VZ] - region->getWaterHeight() > CAMERA_HIGH_M)
    {
        if (forced) gSavedSettings.setBOOL("WolfViewerKrakenForce", false);
        mHave = false;
        return;
    }
    LLTimer cost;
    // the debug switch: a sighting now, in view (only this viewer sees it)
    if (forced)
    {
        gSavedSettings.setBOOL("WolfViewerKrakenForce", false);
        if (!force(cam_global)) LL_WARNS("WolfKraken") << "no deep open sea in view for a forced sighting" << LL_ENDL;
    }
    const F64 utc = utcNow();
    if (now >= mNextCheck) { mNextCheck = now + CHECK_S; check(cam_global, utc); }
    if (!mHave) return;
    const F64 t = utc - mPlan.mStart;
    if (t >= SHOW_S || t < 0.0) { mHave = false; mDrops.clear(); mBoat = nullptr; return; }
    events(mPrevT, t);
    const F32 tf = (F32)t;
    if (tf > 0.5f && tf < T_CHURN + 1.f && ll_frand() < 0.5f) bubbles(3, 4.f + 10.f * tf / T_CHURN);
    if (tf > T_DOWN && tf < T_GONE && ll_frand() < 0.5f) bubbles(4, 8.f);
    moveDrops(dt);
    mPrevT = t;
    mCostUs += cost.getElapsedTimeF64() * 1e6;
    ++mCostFrames;
    if (now - mLastLog > 10.0)
    {
        mLastLog = now;
        LL_DEBUGS("WolfKraken") << "sighting " << mPlan.mKey << " at " << tf << " s; " << mDrops.size() << " drops; cost "
                                << (mCostFrames ? mCostUs / mCostFrames : 0.0) << " us idle (mesh included) a frame" << LL_ENDL;
        mCostUs = 0.0;
        mCostFrames = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the look (wolf_kraken.js _buildMesh, _buildSpray; our own geometry, no textures, no third-party models)
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // wolf_kraken.js maxVerts: arms and tips, body (+ the crown's underside), eyes (rim, iris; pupil; brow), foam
    constexpr S32 VERTS_ARMS = (WolfKraken::ARMS + WolfKraken::FEEDERS) * (SEGS * SIDES * 2 + SIDES) * 3;
    constexpr S32 VERTS_BODY = ((N_BODY - 1) * BODY_SIDES * 2 + BODY_SIDES) * 3;
    constexpr S32 VERTS_EYES = 2 * (16 * 3 + 12 + 8 * 2) * 3;
    constexpr S32 VERTS_FOAM = WolfKraken::FOAM_N * 6 * 3;
    constexpr S32 VERTS_PER_DROP = 8 * 3;
    constexpr S32 MAX_VERTS = VERTS_ARMS + VERTS_BODY + VERTS_EYES + VERTS_FOAM + 600 + MAX_DROPS * VERTS_PER_DROP;

    /** wolfsealife.cpp Writer: unindexed triangles into the strided buffer, wound so each faces `out`. */
    struct KWriter
    {
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        S32 n = 0, cap = 0;
        LLVector3 origin;   // added to every position (the spot, agent metres)

        void vert(const LLVector3& p, const LLVector3& nn, const LLVector3& c)
        {
            const LLVector3 q = p + origin;
            (*pos++).set(q.mV[VX], q.mV[VY], q.mV[VZ], 0.f);   // w = texture index 0 (wolfsealife.cpp Writer)
            *norm++ = nn;
            *uv++ = LLVector2(0.5f, 0.5f);
            *col++ = LLColor4U((U8)(llclamp(c.mV[0], 0.f, 1.f) * 255.f), (U8)(llclamp(c.mV[1], 0.f, 1.f) * 255.f),
                               (U8)(llclamp(c.mV[2], 0.f, 1.f) * 255.f), 255);
            ++n;
        }
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& na, const LLVector3& nb,
                 const LLVector3& nc, const LLVector3& ca, const LLVector3& cb, const LLVector3& cc, const LLVector3& out)
        {
            if (n + 3 > cap) return;
            const LLVector3 f = (b - a) % (c - a);
            if (f * out < 0.f) { vert(a, na, ca); vert(c, nc, cc); vert(b, nb, cb); }
            else { vert(a, na, ca); vert(b, nb, cb); vert(c, nc, cc); }
        }
        void flat(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& out, const LLVector3& colour)
        {
            LLVector3 nn = (b - a) % (c - a);
            if (nn.isExactlyZero()) nn = out;
            nn.normVec();
            if (nn * out < 0.f) nn = -nn;
            tri(a, b, c, nn, nn, nn, colour, colour, colour, out);
        }
    };

    LLVector3 pal(const F32 c[3]) { return LLVector3(c[0], c[1], c[2]); }
}

bool WolfKraken::ensureBuffer()
{
    if (mVB.notNull()) return true;
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    // no indices, drawn with drawArrays (wolfsealife.cpp ensureBuffer): about 42 000 vertices with all the spray
    if (!vb->allocateBuffer(MAX_VERTS, 0))
    {
        LL_WARNS("WolfKraken") << "could not allocate the Kraken's vertex buffer" << LL_ENDL;
        return false;
    }
    mVB = vb;
    return true;
}

void WolfKraken::buildMesh(F32 t, S32& verts)
{
    verts = 0;
    if (!ensureBuffer()) return;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 spot = spotAgent();
    const LLVector3 cam = camera->getOrigin();
    const F32 view_m = gAgentCamera.mDrawDistance + WolfAltitudeSky::farClipExtra();
    const bool body_seen = len2(spot.mV[VX] - cam.mV[VX], spot.mV[VY] - cam.mV[VY]) < view_m + 60.f
                           && camera->sphereInFrustum(spot + LLVector3(0.f, 0.f, 8.f), 60.f) != 0;
    S32 drops = 0;
    for (const Drop& d : mDrops) if (drops < MAX_DROPS && camera->sphereInFrustum(spot + d.mPos, d.mSize)) ++drops;
    if (!body_seen && drops == 0) return;
    const S32 count = (body_seen ? MAX_VERTS - MAX_DROPS * VERTS_PER_DROP : 0) + drops * VERTS_PER_DROP;
    KWriter wr;
    wr.cap = count;
    wr.origin = spot;
    if (!mVB->getVertexStrider(wr.pos, 0, count) || !mVB->getNormalStrider(wr.norm, 0, count)
        || !mVB->getTexCoord0Strider(wr.uv, 0, count) || !mVB->getColorStrider(wr.col, 0, count))
    {
        mVB->unmapBuffer();
        return;
    }
    const F32 size = mLook.mSize, H = BODY_H * size;
    const F32 (*P)[3] = PALETTES[mLook.mPalette];
    const U32 seed = mLook.mSeed;
    auto mottle = [seed](S32 a, S32 b, S32 c)
    {
        // wolf_kraken.js mottle: mix32(seed ^ imul(a + 1, 73856093) ^ imul(b + 7, 19349663) ^ imul(c + 3, 83492791))
        return (F32)((F64)mix32(seed ^ ((U32)(a + 1) * 73856093u) ^ ((U32)(b + 7) * 19349663u) ^ ((U32)(c + 3) * 83492791u)) / 4294967296.0);
    };
    auto skin = [&](F32 up, S32 a, S32 b, S32 c)
    {
        // dark on top, light below, mottled with darker spots (our own pattern)
        const F32 m = mottle(a, b, c);
        LLVector3 col = up > 0.f ? mixv(pal(P[1]), pal(P[0]), up) : mixv(pal(P[1]), pal(P[2]), -up);
        col *= 0.84f + 0.28f * m;
        if (m > 0.86f) col = mixv(col, pal(P[0]), 0.6f);
        return col;
    };
    if (body_seen)
    {
        const Frame bf = bodyFrame(t);
        auto body_r = [H](F32 s0)
        {
            for (S32 r = 1; r < N_BODY; ++r)
            {
                if (s0 <= BODY[r][0])
                {
                    const F32 f = (s0 - BODY[r - 1][0]) / (BODY[r][0] - BODY[r - 1][0]);
                    return (BODY[r - 1][1] + (BODY[r][1] - BODY[r - 1][1]) * f) * H;
                }
            }
            return 0.f;
        };
        // the body: the head, lofted round the leaning axis
        struct RingPt { LLVector3 p, n, c; };
        RingPt ring[N_BODY][BODY_SIDES];
        for (S32 r = 0; r < N_BODY; ++r)
        {
            const F32 s = BODY[r][0], rr = BODY[r][1];
            const F32 breathe = 1.f + 0.03f * sinf(F_TWO_PI * 0.25f * t + s * 2.f);
            const LLVector3 c0 = bf.mC + bf.mA * (s * H);
            const S32 rn = llmin(N_BODY - 1, r + 1), rp = llmax(0, r - 1);
            const F32 slope = (BODY[rp][1] - BODY[rn][1]) / llmax(0.05f, BODY[rn][0] - BODY[rp][0]);
            for (S32 k = 0; k < BODY_SIDES; ++k)
            {
                const F32 a = (F32)k * F_TWO_PI / (F32)BODY_SIDES, ca = cosf(a), sa = sinf(a);
                // lumpy, not a smooth balloon: two slow ripples round and along the head (our own shape)
                const F32 lump = 1.f + 0.035f * sinf(3.f * a + 7.f * s + mLook.mFoam[0].mAlpha) + 0.025f * sinf(7.f * a - 5.f * s + mLook.mFoam[1].mAlpha);
                const F32 rad = rr * H * breathe * lump;
                const LLVector3 dir = bf.mF * (ca * BODY_DEPTH) + bf.mS * sa;
                RingPt& e = ring[r][k];
                e.p = c0 + dir * rad;
                e.n = nrm(bf.mF * (ca / BODY_DEPTH) + bf.mS * sa + bf.mA * (slope * 0.8f));
                const F32 up = clampf(-ca * 0.7f + (s - 0.3f) * 0.9f, -1.f, 1.f);   // the back darker; the throat light
                e.c = skin(up, 50, r, k);
            }
        }
        for (S32 r = 1; r < N_BODY; ++r)
        {
            for (S32 k = 0; k < BODY_SIDES; ++k)
            {
                const S32 k2 = (k + 1) % BODY_SIDES;
                const RingPt& A0 = ring[r - 1][k];
                const RingPt& A1 = ring[r - 1][k2];
                const RingPt& B0 = ring[r][k];
                const RingPt& B1 = ring[r][k2];
                wr.tri(A0.p, B0.p, B1.p, A0.n, B0.n, B1.n, A0.c, B0.c, B1.c, B0.n);
                if (r < N_BODY - 1 || BODY[r][1] > 0.f) wr.tri(A0.p, B1.p, A1.p, A0.n, B1.n, A1.n, A0.c, B1.c, A1.c, B0.n);
            }
        }
        // the crown's underside (closed, the arms grow from it)
        {
            const LLVector3 down = -bf.mA, bottom = bf.mC + bf.mA * (-0.6f * size);
            for (S32 k = 0; k < BODY_SIDES; ++k)
            {
                const S32 k2 = (k + 1) % BODY_SIDES;
                wr.tri(bottom, ring[0][k].p, ring[0][k2].p, down, ring[0][k].n, ring[0][k2].n, pal(P[2]), ring[0][k].c, ring[0][k2].c, down);
            }
        }
        // the eyes: a dark rim, a domed golden iris, a heavy brow, a black bar of a pupil (our own design)
        for (S32 side = 0; side < 2; ++side)
        {
            const F32 sg = side == 0 ? 1.f : -1.f;
            const F32 a = sg * EYE_DEG * DEG_TO_RAD, ca = cosf(a), sa = sinf(a);
            const F32 rr = body_r(EYE_S);
            const LLVector3 dir = bf.mF * (ca * BODY_DEPTH) + bf.mS * sa;
            const LLVector3 n = nrm(bf.mF * (ca / BODY_DEPTH) + bf.mS * sa);
            const LLVector3 c0 = bf.mC + bf.mA * (EYE_S * H) + dir * rr;
            const LLVector3 e1 = nrm(bf.mA % n), e2 = nrm(n % e1);
            const F32 R = EYE_R * size;
            auto ring_pt = [&](F32 rad, F32 ang, F32 out_m) { return c0 + n * out_m + e1 * (cosf(ang) * rad) + e2 * (sinf(ang) * rad); };
            const LLVector3 iris_hi = mixv(IRIS, LLVector3(1.f, 0.92f, 0.5f), 0.5f);
            const LLVector3 dome = c0 + n * (0.35f * size);
            for (S32 q = 0; q < 16; ++q)
            {
                const F32 a1 = (F32)q * F_TWO_PI / 16.f, a2 = (F32)(q + 1) * F_TWO_PI / 16.f;
                wr.flat(ring_pt(R * 1.3f, a1, 0.05f), ring_pt(R * 1.3f, a2, 0.05f), ring_pt(R, a2, 0.12f), n, EYE_RIM);
                wr.flat(ring_pt(R * 1.3f, a1, 0.05f), ring_pt(R, a2, 0.12f), ring_pt(R, a1, 0.12f), n, EYE_RIM);
                const LLVector3 n1 = nrm(n + (ring_pt(R, a1, 0.f) - c0) * (0.3f / R)), n2 = nrm(n + (ring_pt(R, a2, 0.f) - c0) * (0.3f / R));
                wr.tri(dome, ring_pt(R, a1, 0.12f), ring_pt(R, a2, 0.12f), n, n1, n2, iris_hi, IRIS, IRIS, n);
            }
            const LLVector3 brow = mixv(pal(P[0]), LLVector3::zero, 0.3f);
            for (S32 q = 0; q < 8; ++q)
            {
                const F32 a1 = F_PI * (0.1f + 0.8f * (F32)q / 8.f), a2 = F_PI * (0.1f + 0.8f * (F32)(q + 1) / 8.f);
                auto lift = [size](F32 ang) { return 0.35f * size * sinf(ang) + 0.15f * size; };
                const LLVector3 i1 = ring_pt(R * 1.35f, a1, 0.05f), i2 = ring_pt(R * 1.35f, a2, 0.05f);
                const LLVector3 o1 = ring_pt(R * 1.85f, a1, 0.f) + n * lift(a1), o2 = ring_pt(R * 1.85f, a2, 0.f) + n * lift(a2);
                const LLVector3 up = nrm(n + e2 * 0.8f);
                wr.flat(i1, o1, o2, up, brow);
                wr.flat(i1, o2, i2, up, brow);
            }
            const LLVector3 pc = c0 + n * (0.37f * size);
            for (S32 q = 0; q < 12; ++q)
            {
                const F32 a1 = (F32)q * F_TWO_PI / 12.f, a2 = (F32)(q + 1) * F_TWO_PI / 12.f;
                const LLVector3 p1 = pc + e1 * (cosf(a1) * R * 0.75f) + e2 * (sinf(a1) * R * 0.22f);
                const LLVector3 p2 = pc + e1 * (cosf(a2) * R * 0.75f) + e2 * (sinf(a2) * R * 0.22f);
                wr.flat(pc, p1, p2, n, PUPIL);
            }
        }
        // the arms and the two long tentacles: tubes round their centre lines, suckers on the inner side
        std::vector<LLVector3> line, suck;
        struct TubePt { LLVector3 p, n, c; };
        std::vector<TubePt> rings((size_t)(SEGS + 1) * SIDES);
        for (S32 i = 0; i < ARMS + FEEDERS; ++i)
        {
            if (armOut(i, t) <= 0.001f) continue;
            armLine(i, t, bf, line, suck);
            const bool feeder = i >= ARMS;
            const F32 r_base = (feeder ? FEED_R : ARM_R) * size;
            auto rad_at = [&](F32 u)
            {
                F32 r = r_base * (1.f - (feeder ? 0.6f : 0.93f) * powf(u, 0.9f));
                if (feeder) r *= 1.f + 0.9f * smoothstep(0.74f, 0.82f, u) * (1.f - smoothstep(0.9f, 0.97f, u));   // the club
                return llmax(0.05f * size, r);
            };
            // parallel-transported frame along the line
            LLVector3 nv;
            bool have_nv = false;
            for (S32 j = 0; j <= SEGS; ++j)
            {
                const LLVector3 T = nrm(line[(size_t)llmin(SEGS, j + 1)] - line[(size_t)llmax(0, j - 1)]);
                if (!have_nv) { nv = nrm(T % (fabsf(T.mV[VZ]) > 0.9f ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f))); have_nv = true; }
                else nv = nrm(nv - T * (nv * T));
                const LLVector3 bv = T % nv;
                const F32 u = (F32)j / (F32)SEGS, r = rad_at(u);
                for (S32 k = 0; k < SIDES; ++k)
                {
                    const F32 th = (F32)k * F_TWO_PI / (F32)SIDES;
                    const LLVector3 dir = nv * cosf(th) + bv * sinf(th);
                    const F32 inner = dir * suck[(size_t)j];
                    LLVector3 c;
                    if (inner > 0.45f) c = (j % 2 == 0 && u < 0.97f) ? mixv(pal(P[3]), pal(P[2]), 0.25f * mottle(i, j, k)) : pal(P[2]);   // rows of pale suckers
                    else c = skin(clampf(-inner, -1.f, 1.f) * 0.9f, i, j, k);
                    const F32 bump = (inner > 0.45f && j % 2 == 0) ? 1.07f : 1.f;   // suckers stand a little proud of the skin
                    TubePt& e = rings[(size_t)j * SIDES + (size_t)k];
                    e.p = line[(size_t)j] + dir * (r * bump);
                    e.n = dir;
                    e.c = c;
                }
            }
            for (S32 j = 1; j <= SEGS; ++j)
            {
                for (S32 k = 0; k < SIDES; ++k)
                {
                    const S32 k2 = (k + 1) % SIDES;
                    const TubePt& A0 = rings[(size_t)(j - 1) * SIDES + (size_t)k];
                    const TubePt& A1 = rings[(size_t)(j - 1) * SIDES + (size_t)k2];
                    const TubePt& B0 = rings[(size_t)j * SIDES + (size_t)k];
                    const TubePt& B1 = rings[(size_t)j * SIDES + (size_t)k2];
                    wr.tri(A0.p, B0.p, B1.p, A0.n, B0.n, B1.n, A0.c, B0.c, B1.c, B0.n);
                    wr.tri(A0.p, B1.p, A1.p, A0.n, B1.n, A1.n, A0.c, B1.c, A1.c, B0.n);
                }
            }
            const LLVector3 tip_dir = nrm(line[SEGS] - line[SEGS - 1]);
            const LLVector3 tip = line[SEGS] + tip_dir * (rad_at(1.f) * 3.f);
            for (S32 k = 0; k < SIDES; ++k)
            {
                const S32 k2 = (k + 1) % SIDES;
                const TubePt& A0 = rings[(size_t)SEGS * SIDES + (size_t)k];
                const TubePt& A1 = rings[(size_t)SEGS * SIDES + (size_t)k2];
                wr.tri(tip, A0.p, A1.p, tip_dir, A0.n, A1.n, pal(P[0]), A0.c, A1.c, tip_dir);
            }
            // drips while waving in the air (a drop now and then from a part above the water)
            if (t > T_RISEN && t < T_SINK && ll_frand() < 0.08f)
            {
                const S32 j = (S32)std::floor(rnd(0.3f, 1.f) * (F32)SEGS);
                if (line[(size_t)j].mV[VZ] > 1.f) spray(1, line[(size_t)j], -0.5f, 0.5f, 0.3f, 0.25f, 0.5f);
            }
        }
        // the foam: churned water round it, swirling as it goes down. White: the deferred pass lights it as the sea round
        // it (seen 2026-11-17 on Blue Ocean at dusk: spray and foam also darkened by the sun's height came out black)
        const F32 foam_amt = smoothstep(0.f, T_CHURN, t) * (1.f - smoothstep(T_GONE, (F32)SHOW_S, t));
        if (foam_amt > 0.01f)
        {
            const F32 sink_f = (t - T_SINK) / ((F32)SHOW_S - T_SINK);
            const F32 swirl = t > T_SINK ? 0.6f * powf(sink_f, 1.5f) * (t - T_SINK) : 0.f;
            const F32 shrink = t > T_SINK ? 1.f - 0.5f * smoothstep(T_SINK, (F32)SHOW_S, t) : 0.3f + 0.7f * smoothstep(0.f, T_CHURN, t);
            const LLVector3 up(0.f, 0.f, 1.f);
            for (S32 q = 0; q < FOAM_N; ++q)
            {
                const FoamLook& f = mLook.mFoam[q];
                const F32 rho = f.mRho * shrink * size, al = f.mAlpha + swirl * (1.f + 4.f / llmax(1.f, f.mRho));
                const LLVector3 c0(cosf(al) * rho, sinf(al) * rho, 0.06f);
                const F32 sz = f.mSize * size * foam_amt;
                const LLVector3 col = FOAM * (0.9f + 0.1f * mottle(200, q, 0));
                for (S32 e = 0; e < 6; ++e)
                {
                    const F32 a1 = (F32)e * F_PI / 3.f, a2 = (F32)(e + 1) * F_PI / 3.f;
                    const F32 j1 = 0.5f + 0.8f * mottle(201, q, e), j2 = 0.5f + 0.8f * mottle(201, q, (e + 1) % 6);   // ragged edges
                    wr.flat(c0, LLVector3(c0.mV[VX] + cosf(a1) * sz * j1, c0.mV[VY] + sinf(a1) * sz * j1, 0.06f),
                            LLVector3(c0.mV[VX] + cosf(a2) * sz * j2, c0.mV[VY] + sinf(a2) * sz * j2, 0.06f), up, col);
                }
            }
        }
    }
    // the spray: WolfStorm draws soft points; this pass is opaque, so each drop is a small blob (an octahedron) whose size
    // follows WolfStorm's alpha ((bubble ? 0.5 : 0.7) smoothstep(0, .05, t) (1 - smoothstep(.6, 1, t))). White: the deferred
    // pass lights it (WolfStorm's unlit points are greyed at night instead)
    if (drops > 0)
    {
        const LLVector3 white(0.95f, 0.97f, 1.f);
        wr.cap = wr.n + drops * VERTS_PER_DROP;
        S32 written = 0;
        for (const Drop& d : mDrops)
        {
            if (written >= drops) break;
            if (!camera->sphereInFrustum(spot + d.mPos, d.mSize)) continue;
            ++written;
            const F32 tt = d.mAge / d.mLife;
            const F32 fade = (d.mBubble ? 0.5f : 0.7f) * smoothstep(0.f, 0.05f, tt) * (1.f - smoothstep(0.6f, 1.f, tt)) / 0.7f;
            const F32 s = DROP_SCALE * d.mSize * fade;
            const LLVector3 c = d.mPos;
            const LLVector3 rim[4] = { c + LLVector3(s, 0.f, 0.f), c + LLVector3(0.f, s, 0.f), c - LLVector3(s, 0.f, 0.f), c - LLVector3(0.f, s, 0.f) };
            const LLVector3 top = c + LLVector3(0.f, 0.f, s), bottom = c - LLVector3(0.f, 0.f, s);
            for (S32 k = 0; k < 4; ++k)
            {
                const LLVector3& r0 = rim[k];
                const LLVector3& r1 = rim[(k + 1) % 4];
                const LLVector3 mid = (r0 + r1) * 0.5f - c;
                wr.flat(top, r0, r1, mid + LLVector3(0.f, 0.f, s), white);
                wr.flat(bottom, r0, r1, mid - LLVector3(0.f, 0.f, s), white);
            }
        }
    }
    mVB->unmapBuffer();
    verts = wr.n;
}

void WolfKraken::renderDeferred()
{
    if (!mHave) return;
    LLTimer cost;
    S32 verts = 0;
    buildMesh((F32)mPrevT, verts);
    mCostUs += cost.getElapsedTimeF64() * 1e6;
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolfsealife.cpp renderDeferred (from wolfbirds.cpp) - the same program, matrices and attributes, so the Kraken
    // is lit, fogged and hazed as the scene round it is.
    if (LLViewerFetchedTexture::sWhiteImagep.isNull()) return;
    LLGLDisable blend(GL_BLEND);
    LLGLEnable cull(GL_CULL_FACE);
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
    mVB->setBuffer();
    mVB->drawArrays(LLRender::TRIANGLES, 0, (U32)verts);
    shader.unbind();
}
