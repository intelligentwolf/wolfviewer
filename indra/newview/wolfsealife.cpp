/**
 * @file wolfsealife.cpp
 * @brief WolfViewer: dolphins and whales in the open sea (ticket 499, IndigoQueen King's idea). See wolfsealife.h.
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
#include "wolfsealife.h"
#include "wolfsoundeffects.h"   // [SOUND EFFECTS 2026-11-06/]

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"   // [SEA LIFE EASY 2026-11-06/] getHasReceivedData
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
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

// Source for every figure below: WolfStorm js/world/wolf_sea_life.js (the same table and rules, copied verbatim) and the
// shared spec region-terrain-work/sea-life-2026-11-03/README.md, which gives each figure's source or says it is CHOSEN.
namespace
{
    // Source: wolf_sea_life.js STATIONS / LOD_STATIONS - the same 11 stations for every kind (head 0 .. tail 1)
    constexpr S32 N_STATIONS = 11;
    const F32 STATIONS[N_STATIONS] = { 0.f, 0.04f, 0.10f, 0.20f, 0.32f, 0.45f, 0.58f, 0.70f, 0.82f, 0.93f, 1.0f };
    const S32 LOD_STATIONS[6] = { 0, 2, 4, 6, 8, 10 };

    struct Species
    {
        F32 mLen, mCruise, mFast, mDepthMin, mTurn, mWaveHz, mFlukeSpan;
        F32 mRadius[N_STATIONS], mW[N_STATIONS], mH[N_STATIONS];
        F32 mBack[3], mFlank[3], mBelly[3], mFin[3], mFinUnder[3];
        F32 mDorsalS0, mDorsalS1, mDorsalTip, mDorsalHeight;
        F32 mPecS0, mPecS1, mPecTipS, mPecReach;
    };
    // Source: wolf_sea_life.js SPECIES (bottlenose dolphin 2.5 m, humpback 13 m, minke 7.5 m; NOAA Fisheries species pages;
    // porpoising speed Au & Weihs 1980, Nature 284:548)
    const Species SPECIES[WolfSeaLife::KIND_COUNT] = {
        { 2.5f, 2.0f, 5.5f, 4.f, 1.2f, 2.2f, 0.25f,
          { 0.f, 0.03f, 0.055f, 0.085f, 0.105f, 0.11f, 0.10f, 0.08f, 0.055f, 0.03f, 0.018f },
          { 1.f, 1.f, 1.f, 1.f, 1.f, 0.95f, 0.85f, 0.7f, 0.55f, 0.45f, 0.4f },
          { 1.f, 1.f, 1.05f, 1.1f, 1.1f, 1.1f, 1.15f, 1.2f, 1.25f, 1.3f, 1.3f },
          { 0.33f, 0.36f, 0.40f }, { 0.50f, 0.53f, 0.57f }, { 0.86f, 0.86f, 0.86f }, { 0.30f, 0.33f, 0.37f }, { 0.45f, 0.48f, 0.52f },
          0.42f, 0.58f, 0.62f, 0.10f, 0.25f, 0.31f, 0.40f, 0.15f },
        { 13.0f, 1.6f, 1.6f, 10.f, 0.15f, 0.35f, 0.33f,
          { 0.f, 0.05f, 0.085f, 0.11f, 0.125f, 0.13f, 0.12f, 0.095f, 0.065f, 0.035f, 0.02f },
          { 1.f, 1.15f, 1.15f, 1.1f, 1.05f, 1.f, 0.95f, 0.8f, 0.6f, 0.45f, 0.4f },
          { 1.f, 0.75f, 0.85f, 0.95f, 1.f, 1.f, 1.05f, 1.1f, 1.2f, 1.25f, 1.3f },
          { 0.07f, 0.07f, 0.08f }, { 0.12f, 0.12f, 0.13f }, { 0.30f, 0.30f, 0.32f }, { 0.10f, 0.10f, 0.11f }, { 0.85f, 0.85f, 0.85f },
          0.62f, 0.70f, 0.71f, 0.025f, 0.24f, 0.33f, 0.50f, 0.30f },
        { 7.5f, 2.2f, 2.2f, 8.f, 0.25f, 0.45f, 0.27f,
          { 0.f, 0.025f, 0.05f, 0.08f, 0.10f, 0.105f, 0.095f, 0.075f, 0.05f, 0.028f, 0.016f },
          { 1.f, 1.f, 1.f, 1.f, 1.f, 0.95f, 0.85f, 0.7f, 0.55f, 0.45f, 0.4f },
          { 1.f, 0.85f, 0.95f, 1.05f, 1.1f, 1.1f, 1.15f, 1.2f, 1.25f, 1.3f, 1.3f },
          { 0.20f, 0.22f, 0.25f }, { 0.40f, 0.42f, 0.45f }, { 0.88f, 0.88f, 0.88f }, { 0.22f, 0.24f, 0.27f }, { 0.88f, 0.88f, 0.88f },
          0.62f, 0.72f, 0.75f, 0.04f, 0.26f, 0.31f, 0.38f, 0.13f },
    };
    constexpr F32 G_MS2 = 9.81f;
    // Source: wolf_sea_life.js constants
    constexpr F64 DIRECT_SECS = 3.0;
    // [SEA LIFE EASY 2026-11-06] Paul: "honestly we dont really need to make it so hard to see them". CAMERA_HIGH_M was 150
    // m: a resident flying over the sea saw none (wolf_sea_life.js the same). The distances to the camera that send groups
    // away are measured flat (dist_flat), so a camera high over the water keeps the pods under it.
    constexpr F32 NEAR_M = 60.f, FAR_M = 250.f, GONE_M = 380.f, GONE_HIDDEN_M = 160.f, TELEPORT_M = 300.f, CAMERA_HIGH_M = 500.f;
    constexpr F32 DOLPHIN_FRAC = 0.15f, WHALE_FRAC = 0.20f, WHALE_DEPTH = 10.f;
    constexpr S32 MAX_GROUPS = 4, HARD_MAX = 120, DEFAULT_MAX = 60;   // WolfStorm's default is 40 (a browser)
    constexpr F32 LOD_M = 120.f;
    constexpr F32 CRUISE_DEPTH_MIN = 0.8f, CRUISE_DEPTH_MAX = 1.6f, PORPOISE_DEPTH = 0.6f;
    constexpr F32 BREATH_MIN_S = 8.f, BREATH_MAX_S = 20.f, UNDER_MIN_S = 1.5f, UNDER_MAX_S = 4.f, MODE_MIN_S = 20.f, MODE_MAX_S = 60.f;
    constexpr F32 LEAP_DEG = 35.f, LEAP_SPEED = 5.5f, SEPARATION_M = 2.f;
    constexpr F32 RIDE_SEEK_M = 120.f, RIDE_MIN_S = 30.f, RIDE_MAX_S = 90.f, RIDE_SPEED_MIN = 2.f, RIDE_SPEED_MAX = 12.f;
    constexpr F32 RIDE_MAX = 9.f, RIDE_CHANCE = 0.5f, RIDE_LEAP_MIN_S = 3.f, RIDE_LEAP_MAX_S = 8.f, RIDE_WATER_M = 3.f;
    constexpr S32 BREATHS_MIN = 3, BREATHS_MAX = 6;
    constexpr F32 BREATH_GAP_MIN_S = 12.f, BREATH_GAP_MAX_S = 25.f, SURFACE_S = 3.5f, DOWN_MIN_S = 60.f, DOWN_MAX_S = 150.f;
    constexpr F32 DOWN_SPEED = 1.2f, DIVE_MAX_M = 25.f;
    // the blow: [kind] height, width, particles, life (humpback a bushy 3 m column, minke a faint 1.5 m one)
    struct Blow { F32 mHeight, mWidth; S32 mN; F32 mLife; };
    const Blow BLOW[WolfSeaLife::KIND_COUNT] = { { 0.f, 0.f, 0, 0.f }, { 3.0f, 1.5f, 24, 3.0f }, { 1.5f, 0.6f, 14, 2.2f } };
    constexpr S32 MAX_SPRAY = 400;
    // [VIEWER] each of WolfStorm's soft spray points is drawn here as DROPS_PER_POINT small opaque blobs round it (an
    // opaque pass has no soft edge; seen in world 11-03, one blob per point read as a few white crystals, not a plume)
    constexpr S32 DROPS_PER_POINT = 3;
    constexpr S32 MAX_DROPS = MAX_SPRAY * DROPS_PER_POINT;
    // [VIEWER] The draw distance also bounds where a group is placed (wolfbirds.cpp flock_reach_m, ticket 463): WolfStorm
    // has no such setting; at the Mid graphics level (128 m) a group 250 m out could never be seen.
    constexpr F32 VIEW_MARGIN_M = 30.f, MIN_REACH_M = 30.f;
    // [VIEWER] The sound is placed this far along its direction from the camera, its own gain falling gently with the real
    // distance (wolfbirds.cpp CALL_EAR_M and playCall); so no extra /2 here (WolfStorm's /2 stands for that roll-off).
    constexpr F32 CALL_EAR_M = 4.f;
    constexpr S32 MAX_VOICES = 6;

    // Sounds: app_settings/wolfsealife (SOURCES.md; region-terrain-work/sea-life-2026-11-03/tools/build_sounds.py). Each
    // file's length (build_report.json start/end; the two synthesised sounds 1.9 s and 0.4 s) keeps the voice count.
    struct Sound { WolfWeatherSound::Recording mRec; F32 mSecs; };
    const Sound SND_DOLPHIN[4] = {
        { { "wolf_sea_dolphin_1.ogg", "03dd3cfe-9dc7-40d5-897c-6d754a644da8", nullptr, false, "wolfsealife" }, 3.85f },
        { { "wolf_sea_dolphin_2.ogg", "67f2f4d0-50bc-405c-9a05-00e9de75a3eb", nullptr, false, "wolfsealife" }, 2.32f },
        { { "wolf_sea_dolphin_3.ogg", "80590df4-2486-4e1d-ad82-e5db658e7c8e", nullptr, false, "wolfsealife" }, 3.66f },
        { { "wolf_sea_dolphin_4.ogg", "d8855844-7a53-4c0e-b861-21394ad023af", nullptr, false, "wolfsealife" }, 3.42f },
    };
    const Sound SND_BLOW[2] = {
        { { "wolf_sea_whale_blow_1.ogg", "0521255b-a142-4a07-a1ae-3062a28a31e6", nullptr, false, "wolfsealife" }, 1.44f },
        { { "wolf_sea_whale_blow_2.ogg", "8a004dae-8042-4f63-91a6-f48685869f0c", nullptr, false, "wolfsealife" }, 1.9f },
    };
    const Sound SND_SONG[3] = {
        { { "wolf_sea_whale_song_1.ogg", "83194a2c-bc53-4e60-b341-38d9b82f28b0", nullptr, false, "wolfsealife" }, 39.76f },
        { { "wolf_sea_whale_song_2.ogg", "0a847a6e-68ed-4958-b693-6b999a144685", nullptr, false, "wolfsealife" }, 9.63f },
        { { "wolf_sea_whale_song_3.ogg", "1b819dfd-f7e9-4da9-ba48-d9f1218e4e25", nullptr, false, "wolfsealife" }, 15.62f },
    };
    const Sound SND_SPLASH[1] = {
        { { "wolf_sea_splash_1.ogg", "9eed381b-d3ba-4b5e-92a3-6299e2513d37", nullptr, false, "wolfsealife" }, 0.4f },
    };
    // Source: wolf_sea_life.js SND - base gain, full-loudness distance, heard to; under water: a multiplier (or its own
    // base) and its own range
    enum { SND_KIND_DOLPHIN = 0, SND_KIND_BLOW = 1, SND_KIND_SONG = 2, SND_KIND_SPLASH = 3 };
    struct SoundKind { const Sound* mFiles; S32 mCount; F32 mBase, mFull, mMax, mUnderMult, mUnderBase, mUnderMax; };
    const SoundKind SND[4] = {
        { SND_DOLPHIN, 4, 0.30f, 10.f, 60.f, 2.5f, -1.f, 250.f },
        { SND_BLOW, 2, 0.55f, 30.f, 400.f, 0.4f, -1.f, 400.f },
        { SND_SONG, 3, 0.07f, 50.f, 300.f, 1.f, 0.5f, 1500.f },
        { SND_SPLASH, 1, 0.25f, 8.f, 40.f, 0.5f, -1.f, 40.f },
    };

    F32 rnd(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }
    S32 irnd(S32 lo, S32 hi) { return lo + ll_rand(hi - lo + 1); }
    F32 smoothstep(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
    F32 wrap(F32 a)
    {
        while (a > F_PI) a -= F_TWO_PI;
        while (a < -F_PI) a += F_TWO_PI;
        return a;
    }
    F32 len2(F32 x, F32 y) { return sqrtf(x * x + y * y); }
    // [SEA LIFE EASY 2026-11-06/] Source: wolf_sea_life.js h2 - the flat distance between two points
    F32 dist_flat(const LLVector3& a, const LLVector3& b) { return len2(a.mV[VX] - b.mV[VX], a.mV[VY] - b.mV[VY]); }
    void mix(const F32* a, const F32* b, F32 t, F32* out)
    {
        for (S32 k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * t;
    }

    /** [VIEWER] wolfbirds.cpp flock_reach_m: the far edge of a group's place, inside the draw distance. */
    F32 reach_m()
    {
        const F32 view_m = gAgentCamera.mDrawDistance + WolfAltitudeSky::farClipExtra();
        return llclamp(view_m - VIEW_MARGIN_M, MIN_REACH_M, FAR_M);
    }

    /** The sun's height over the horizon, degrees, in the sky on screen (wolfbirds.cpp sun_elevation_deg). */
    F32 sun_elevation_deg()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return 45.f;
        const LLVector3 sun = sky->getSunDirection();
        return (F32)(std::asin(llclamp((F64)sun.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI);
    }

    bool camera_under_water() { return LLViewerCamera::getInstance()->cameraUnderWater(); }
}

WolfSeaLife::WolfSeaLife() {}
WolfSeaLife::~WolfSeaLife() {}

void WolfSeaLife::rebase(const LLVector3& delta)
{
    for (Animal& a : mAnimals) { a.mPos += delta; a.mSlot += delta; }
    for (Group& g : mGroups) g.mCentre += delta;
    for (Drop& d : mDrops) d.mPos += delta;
    mLastCamera += delta;
}

void WolfSeaLife::reset()
{
    mAnimals.clear();
    mGroups.clear();
    mDrops.clear();
    mRideTried.clear();
    mNextDirect = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the place
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfSeaLife::water() const
{
    LLViewerRegion* r = gAgent.getRegion();
    return r ? r->getWaterHeight() : 20.f;
}

bool WolfSeaLife::inRegion(const LLVector3& agent, F32 margin) const
{
    LLViewerRegion* r = gAgent.getRegion();
    if (!r) return false;
    const LLVector3 o = r->getOriginAgent();
    const F32 w = r->getWidth();
    const F32 x = agent.mV[VX] - o.mV[VX], y = agent.mV[VY] - o.mV[VY];
    return x >= -margin && y >= -margin && x <= w + margin && y <= w + margin;
}

// [SEA LIFE EASY 2026-11-06] Source: wolf_sea_life.js _neighbourAt / _neighbours `ok`. The region list is
// LLWorld::getRegionFromPosAgent (llworld.cpp: x and y only, pointInRegionGlobal).
LLViewerRegion* WolfSeaLife::neighbourAt(const LLVector3& agent, bool& ok) const
{
    ok = false;
    LLViewerRegion* here = gAgent.getRegion();
    LLViewerRegion* r = here ? LLWorld::getInstance()->getRegionFromPosAgent(agent) : nullptr;
    if (!r || r == here) return nullptr;
    // the animals swim at the agent's region's water level (water()), so a neighbour's sea must be at that height
    ok = WolfRegionWeather::instanceExists() && WolfRegionWeather::instance().seaLifeNear(r->getRegionID())
         && fabsf(r->getWaterHeight() - here->getWaterHeight()) <= 0.01f;
    return r;
}

/** Source: wolf_sea_life.js _depth - the water over the terrain, 0 on land. The sea only: terrain below the region's water
 *  height (painted water and prim water are not looked at). [SEA LIFE EASY 2026-11-06] Off the agent's region: a neighbour
 *  whose sea may hold them (neighbourAt) counts once that patch's terrain has come (LLSurfacePatch::getHasReceivedData; an
 *  empty patch reads 0 m, which would be deep water); it was 0 anywhere off the region. */
F32 WolfSeaLife::depth(const LLVector3& agent) const
{
    LLViewerRegion* here = gAgent.getRegion();
    if (!here) return 0.f;
    LLViewerRegion* r = here;
    if (!inRegion(agent, 0.f))
    {
        bool ok = false;
        r = neighbourAt(agent, ok);
        if (!r || !ok) return 0.f;
    }
    // [SEALIFE AGROUND 2026-11-08] Paul (Sandbox, 131,127): dolphins and whales swimming through the beach. The agent's
    // own region skipped this check: LLSurface::getZ (llsurface.cpp:676-691) reads 0 m where no patch exists and a new
    // patch's heights are zero-filled (llsurfacepatch.cpp initData: new F32[points]()), so until a region's terrain had
    // come every point read 20 m deep, direct() wanted pods everywhere and pickPlace put the first one, in view, on land
    // (log-nv-rez8: readSeaLife "on" at 15:50:33Z with 0 of 4096 patches, data from 15:50:43Z). A patch whose terrain
    // has not come is not sea, on every region. getPatchesPerEdge (llsurface.cpp:634): a surface not created yet has 0
    // patches and resolvePatchRegion would warn on every call (llsurface.cpp "No patches for current region!").
    const LLVector3 pos_region = agent - r->getOriginAgent();
    if (r->getLand().getPatchesPerEdge() <= 0) return 0.f;
    const LLSurfacePatch* patch = r->getLand().resolvePatchRegion(pos_region);
    if (!patch || !patch->getHasReceivedData()) return 0.f;
    const F32 h = r->getLand().resolveHeightRegion(pos_region);
    return std::isfinite(h) ? llmax(0.f, here->getWaterHeight() - h) : 0.f;
}

bool WolfSeaLife::deepDisc(const LLVector3& p, F32 need, F32 radius) const
{
    if (depth(p) < need) return false;
    for (S32 k = 0; k < 8; ++k)
    {
        const F32 a = (F32)k * F_PI / 4.f;
        if (depth(p + LLVector3(cosf(a) * radius, sinf(a) * radius, 0.f)) < need) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the director (wolf_sea_life.js _direct, _pickPlace, _spawn)
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfSeaLife::wanted(LLViewerRegion*& region) const
{
    region = gAgent.getRegion();
    if (!region) return false;
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerSeaLife", true);
    if (!on) return false;
    if (!WolfGrid::showsWolfRegionWeather()) return false;   // Wolf regions only, visitors included (wolfgrid.h)
    if (!WolfRegionWeather::instanceExists() || !WolfRegionWeather::instance().seaLifeAllowed()) return false;
    return true;
}

/** [SEA LIFE EASY 2026-11-06] Source: wolf_sea_life.js _pickPlace. `first`: no group is here yet, so this one comes where
 *  the camera looks; later ones still come out of view where possible. The place may be over a neighbour (depth answers 0
 *  over a sea that may not hold them, so deepDisc refuses it); it was the agent's region only, 10 m in. The draw distance
 *  now bounds the slant distance from a high camera: the flat reach is what is left of it after the height. */
bool WolfSeaLife::pickPlace(Kind kind, const LLVector3& cam, LLVector3& out, bool first) const
{
    const Species& sp = SPECIES[kind];
    const F32 r = (kind == DOLPHIN) ? 12.f : 30.f;
    const F32 height = llmax(0.f, cam.mV[VZ] - water());
    const F32 view_m = gAgentCamera.mDrawDistance + WolfAltitudeSky::farClipExtra() - VIEW_MARGIN_M;   // reach_m's bound
    const F32 flat_m = view_m > height ? sqrtf(view_m * view_m - height * height) : 0.f;
    if (flat_m < MIN_REACH_M) return false;   // the sea round here is beyond the draw distance from this high
    const F32 far_m = llmin(reach_m(), flat_m), near_m = llmin(NEAR_M, far_m * 0.5f);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    bool have_fallback = false;
    LLVector3 fallback;
    const S32 tries = first ? 60 : 20;   // [SEA LIFE EASY 2026-11-06/] the first one looks harder for a place in view
    for (S32 a = 0; a < tries; ++a)
    {
        const F32 ang = ll_frand(F_TWO_PI), d = rnd(near_m, far_m);
        const LLVector3 p(cam.mV[VX] + cosf(ang) * d, cam.mV[VY] + sinf(ang) * d, water() - 2.f);
        if (!deepDisc(p, sp.mDepthMin, r)) continue;
        // in view for the first group, else out of view where possible (sphereInFrustum: 0 = outside)
        if ((camera->sphereInFrustum(p, sp.mLen) != 0) == first) { out = p; return true; }
        if (!have_fallback) { fallback = p; have_fallback = true; }
    }
    if (have_fallback) out = fallback;
    return have_fallback;
}

void WolfSeaLife::leaveGroup(Group& g, const LLVector3& cam)
{
    if (g.mLeaving) return;
    g.mLeaving = true;
    g.mRide = nullptr;
    g.mHeading = atan2f(g.mCentre.mV[VY] - cam.mV[VY], g.mCentre.mV[VX] - cam.mV[VX]);
    g.mTarget = g.mHeading;
}

void WolfSeaLife::direct(const LLVector3& cam)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    S32 d_deep = 0, w_deep = 0, total = 0;
    for (S32 iy = -4; iy <= 4; ++iy)
    {
        for (S32 ix = -4; ix <= 4; ++ix)
        {
            const LLVector3 p(cam.mV[VX] + (F32)ix * FAR_M / 4.f, cam.mV[VY] + (F32)iy * FAR_M / 4.f, 0.f);
            // [SEA LIFE EASY 2026-11-06] every point over a region counts (a neighbour whose sea may not hold them as not
            // deep, depth()); it was the agent's region's points only. Source: wolf_sea_life.js _direct
            if (!inRegion(p, 0.f) && !LLWorld::getInstance()->getRegionFromPosAgent(p)) continue;
            ++total;
            const F32 d = depth(p);
            if (d >= SPECIES[DOLPHIN].mDepthMin) ++d_deep;
            if (d >= WHALE_DEPTH) ++w_deep;
        }
    }
    const F32 d_f = total ? (F32)d_deep / (F32)total : 0.f, w_f = total ? (F32)w_deep / (F32)total : 0.f;
    const bool high = cam.mV[VZ] - water() > CAMERA_HIGH_M;
    S32 want_d = 0, want_w = 0;
    if (!high)
    {
        if (d_f >= DOLPHIN_FRAC) want_d = d_f > 0.5f ? 2 : 1;
        if (w_f >= WHALE_FRAC) want_w = 1;
    }
    S32 have_d = 0, have_w = 0;
    for (S32 gi = 0; gi < (S32)mGroups.size(); ++gi)
    {
        Group& g = mGroups[(size_t)gi];
        if (g.mLeaving) continue;
        // a group drifted beyond FAR_M x 1.2 of the camera (its nearest member) is sent away and not counted
        F32 nearest = 1e9f;
        for (const Animal& a : mAnimals) if (a.mGroup == gi) nearest = llmin(nearest, dist_flat(a.mPos, cam));   // [SEA LIFE EASY 2026-11-06/] flat
        if (nearest > FAR_M * 1.2f) { leaveGroup(g, cam); continue; }
        S32& have = (g.mKind == DOLPHIN) ? have_d : have_w;
        const S32 want = (g.mKind == DOLPHIN) ? want_d : want_w;
        if (have >= want) { leaveGroup(g, cam); continue; }
        ++have;
    }
    // the log line (debug only), once a minute while there are animals
    if (now - mLastLog > 60.0 && !mAnimals.empty())
    {
        mLastLog = now;
        LL_DEBUGS("WolfSeaLife") << mAnimals.size() << " animals in " << mGroups.size() << " groups; deep for dolphins " << d_f
                                 << ", whales " << w_f << "; " << mDrops.size() << " drops; reach " << reach_m() << " m; cost "
                                 << (mCostFrames ? mCostIdleUs / mCostFrames : 0.0) << " us idle + "
                                 << (mCostFrames ? mCostMeshUs / mCostFrames : 0.0) << " us mesh a frame" << LL_ENDL;
        mCostIdleUs = mCostMeshUs = 0.0;
        mCostFrames = 0;
    }
    // where each group's leader is (debug only: the in-world test aims its camera from these lines)
    for (S32 gi = 0; gi < (S32)mGroups.size(); ++gi)
    {
        for (const Animal& a : mAnimals)
        {
            if (a.mGroup != gi) continue;
            LL_DEBUGS("WolfSeaLife") << "leader of group " << mGroups[(size_t)gi].mId << " (kind " << (S32)a.mKind << ") at "
                                     << a.mPos << " state " << (S32)a.mState << (mGroups[(size_t)gi].mLeaving ? " leaving" : "")
                                     << LL_ENDL;
            break;
        }
    }
    if ((S32)mGroups.size() >= MAX_GROUPS) return;
    const bool first = have_d + have_w == 0;   // [SEA LIFE EASY 2026-11-06/] nobody here yet: see spawn. Source: wolf_sea_life.js _direct
    // [VIEWER] the cap is lowered by the frame budget, as the birds' (WolfFrameGovernor)
    static LLCachedControl<S32> max_setting(gSavedSettings, "WolfViewerSeaLifeMax", DEFAULT_MAX);
    const S32 cap = llclamp((S32)((F32)llclamp((S32)max_setting, 0, HARD_MAX) * WolfFrameGovernor::budget()), 0, HARD_MAX);
    const S32 room = cap - (S32)mAnimals.size();
    // one new group per decision; whales first when there is room (they are rarer and bigger)
    if (have_w < want_w && room >= 1)
    {
        const Kind kind = ll_frand() < 0.6f ? HUMPBACK : MINKE;
        const S32 n = llmin(room, kind == HUMPBACK ? (ll_frand() < 0.4f ? 1 : irnd(2, 3)) : (ll_frand() < 0.7f ? 1 : 2));
        if (spawn(kind, n, cam, now, first)) return;
    }
    if (have_d < want_d && room >= 1)
    {
        // game tuning: fewer dolphins in polar seas (wa_dolphin_climate)
        if (ll_frand() > WolfRegionWeather::instance().seaLifeDolphins()) return;
        const S32 n = llmin(room, ll_frand() < 0.3f ? 1 : irnd(5, 15));
        spawn(DOLPHIN, n, cam, now, first);
    }
}

/** They appear under the water at their place, out of view where possible (dolphins 2 m under, whales down).
 *  [SEA LIFE EASY 2026-11-06] Source: wolf_sea_life.js _spawn. `first` (no group here yet): in view (pickPlace), and up
 *  soon - dolphins breathe within 1-3 s (was 8-20 s), whales start 6 m down at most and rise within 1-4 s (was up to 25 m
 *  down and 10-40 s). */
bool WolfSeaLife::spawn(Kind kind, S32 count, const LLVector3& cam, F64 now, bool first)
{
    LLVector3 c;
    if (!pickPlace(kind, cam, c, first)) return false;
    const Species& sp = SPECIES[kind];
    const F32 heading = ll_frand(F_TWO_PI);
    Group g;
    g.mId = ++mNextGroupId;
    g.mKind = kind;
    g.mCentre = LLVector3(c.mV[VX], c.mV[VY], 0.f);
    g.mHeading = g.mTarget = heading;
    g.mRetarget = rnd(20.f, 50.f);
    g.mModeFor = rnd(MODE_MIN_S, MODE_MAX_S);
    g.mNextCall = now + rnd(2.f, 8.f);
    g.mNextSong = now + rnd(5.f, 25.f);
    const S32 gi = (S32)mGroups.size();
    mGroups.push_back(g);
    const F32 w = water();
    S32 made = 0;
    for (S32 i = 0; i < count; ++i)
    {
        // up to 6 tries for an offset over water deep enough for the kind (whales after the first 15-28 m, inside the
        // 30 m disc pickPlace checked); a member with none is left out
        bool placed = false;
        LLVector3 off;
        for (S32 t = 0; t < 6 && !placed; ++t)
        {
            const F32 spread = (kind == DOLPHIN) ? rnd(2.f, 10.f) : (i == 0 ? 0.f : rnd(15.f, 28.f));
            const F32 oa = ll_frand(F_TWO_PI);
            off = LLVector3(cosf(oa) * spread, sinf(oa) * spread, 0.f);
            placed = depth(c + off) >= sp.mDepthMin;
        }
        if (!placed) continue;
        ++made;
        Animal a;
        a.mGroup = gi;
        a.mKind = kind;
        a.mOff = off;
        a.mSize = (kind == DOLPHIN) ? rnd(0.9f, 1.1f) : rnd(0.92f, 1.08f);
        a.mLen = sp.mLen * a.mSize;
        const F32 dep = depth(c + a.mOff);
        a.mPos = LLVector3(c.mV[VX] + a.mOff.mV[VX], c.mV[VY] + a.mOff.mV[VY],
                           w - ((kind == DOLPHIN) ? 2.f : llmin(first ? 6.f : DIVE_MAX_M, llmax(3.f, dep - 3.f))));
        a.mHeading = heading;
        a.mSpeed = sp.mCruise;
        a.mPhase = ll_frand(F_TWO_PI);
        a.mState = (kind == DOLPHIN) ? SWIM : W_DOWN;
        a.mStateFor = (kind == DOLPHIN) ? (first ? rnd(1.f, 3.f) : rnd(BREATH_MIN_S, BREATH_MAX_S))
                                        : (first ? rnd(1.f, 4.f) + (F32)i * rnd(0.f, 1.5f) : rnd(10.f, 40.f) + (F32)i * rnd(0.f, 3.f));
        a.mDepthT = (kind == DOLPHIN) ? rnd(CRUISE_DEPTH_MIN, CRUISE_DEPTH_MAX) : 6.f;
        mAnimals.push_back(a);
    }
    if (made == 0) { mGroups.pop_back(); return false; }   // nobody placed: no group
    LL_DEBUGS("WolfSeaLife") << "group of " << made << " (kind " << (S32)kind << ") under the water at " << c << LL_ENDL;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// swimming (wolf_sea_life.js _moveGroup, _steer, _dolphin, _whale)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfSeaLife::moveGroup(Group& g, F32 dt)
{
    const Species& sp = SPECIES[g.mKind];
    if (g.mRide.notNull()) return;
    g.mRetarget -= dt;
    if (g.mRetarget <= 0.f && !g.mLeaving) { g.mRetarget = rnd(20.f, 50.f); g.mTarget = g.mHeading + rnd(-1.2f, 1.2f); }
    const F32 ahead = 15.f + 2.f * sp.mLen;
    const F32 need = sp.mDepthMin;
    // [SEA LIFE SPIN 2026-11-05] Paul: "on sandbox there is a whale or dolphin spinning round". The heading used to be
    // checked only `ahead` metres out, so a centre that had crept into water between 0.75 and 1 x `need` with deep water
    // further on (Sandbox's shelves) kept a heading whose very next step the gate below refused: the centre froze, the
    // members' slots stood still and every member circled its slot for minutes (wolf_sea_life.js, sandbox_spin.js). Now
    // the heading must be deep enough one body length out as well, a step into deeper water is never refused, and a
    // refused step slides along the nearest heading whose step is allowed (and turns to it).
    // Source: wolf_sea_life.js _moveGroup (2026-11-05)
    auto probe = [&](F32 hd, F32 d) { return depth(g.mCentre + LLVector3(cosf(hd) * d, sinf(hd) * d, 0.f)); };
    auto ok_at = [&](F32 hd) { return probe(hd, sp.mLen) >= need && probe(hd, ahead) >= need; };
    if (!ok_at(g.mHeading))
    {
        // the nearest heading to the current one with deep water ahead (8 tries each side); none: the deepest way
        bool found = false;
        F32 best = 0.f;
        for (S32 k = 1; k <= 8 && !found; ++k)
        {
            for (S32 sgn = 1; sgn >= -1; sgn -= 2)
            {
                const F32 hd = g.mHeading + (F32)(sgn * k) * F_PI / 8.f;
                if (ok_at(hd)) { best = hd; found = true; break; }
            }
        }
        if (!found)
        {
            F32 deepest = -1.f;
            for (S32 k = 0; k < 16; ++k)
            {
                const F32 hd = g.mHeading + (F32)k * F_PI / 8.f, d = probe(hd, sp.mLen);
                if (d > deepest) { deepest = d; best = hd; }
            }
        }
        g.mTarget = best;
    }
    const F32 turn = llclamp(wrap(g.mTarget - g.mHeading), -sp.mTurn * 0.5f * dt, sp.mTurn * 0.5f * dt);
    g.mHeading = wrap(g.mHeading + turn);
    const F32 speed = (g.mKind == DOLPHIN && g.mPorpoise) ? sp.mFast : sp.mCruise;
    const F32 v = speed * (g.mLeaving ? 1.3f : 1.f);
    const F32 here = depth(g.mCentre);
    // leaving groups keep the depth gate too (never aground)
    auto step_to = [&](F32 hd, LLVector3& out)
    {
        out = g.mCentre + LLVector3(cosf(hd) * v * dt, sinf(hd) * v * dt, 0.f);
        const F32 there = depth(out);
        return there >= need * 0.75f || there > here;
    };
    LLVector3 next;
    bool stepped = step_to(g.mHeading, next);
    for (S32 k = 1; k <= 8 && !stepped; ++k)
    {
        for (S32 sgn = 1; sgn >= -1; sgn -= 2)
        {
            const F32 hd = g.mHeading + (F32)(sgn * k) * F_PI / 8.f;
            if (k < 8 && !ok_at(hd)) continue;   // slide only where the way on is deep too (k 8: straight back)
            if (step_to(hd, next)) { g.mTarget = hd; stepped = true; break; }
        }
    }
    if (stepped) g.mCentre = next;
    if (g.mKind == DOLPHIN && !g.mLeaving)
    {
        g.mModeFor -= dt;
        if (g.mModeFor <= 0.f) { g.mModeFor = rnd(MODE_MIN_S, MODE_MAX_S); g.mPorpoise = ll_frand() < 0.4f; }
    }
}

void WolfSeaLife::steer(Animal& a, F32 dt)
{
    const Group& g = mGroups[(size_t)a.mGroup];
    const Species& sp = SPECIES[a.mKind];
    F32 tx = 0.f, ty = 0.f, want = 0.f;
    const bool riding = g.mRide.notNull();
    if (riding)
    {
        tx = a.mSlot.mV[VX]; ty = a.mSlot.mV[VY]; want = llclamp(g.mRideSpeed, 0.f, RIDE_MAX);
    }
    else
    {
        const F32 c = cosf(g.mHeading), s = sinf(g.mHeading);
        tx = g.mCentre.mV[VX] + a.mOff.mV[VX] * c - a.mOff.mV[VY] * s;
        ty = g.mCentre.mV[VY] + a.mOff.mV[VX] * s + a.mOff.mV[VY] * c;
        want = (a.mKind == DOLPHIN && g.mPorpoise) ? sp.mFast : sp.mCruise;
        if (a.mKind != DOLPHIN && a.mState == W_DOWN) want = DOWN_SPEED;
        if (g.mLeaving) want *= 1.3f;
        // [SEA LIFE SPIN 2026-11-05] a slot that has swung over water too shallow for this kind (the slot turns with the
        // pod): follow the pod's centre instead, which moveGroup keeps over deep water. Source: wolf_sea_life.js _steer
        if (depth(LLVector3(tx, ty, 0.f)) < sp.mDepthMin) { tx = g.mCentre.mV[VX]; ty = g.mCentre.mV[VY]; }
    }
    const F32 dx = tx - a.mPos.mV[VX], dy = ty - a.mPos.mV[VY];
    const F32 dist = len2(dx, dy);
    const F32 rate = sp.mTurn * (a.mState == LEAP ? 0.f : 1.f) * (riding ? 2.f : 1.f);
    // [SEA LIFE SPIN 2026-11-05] steering straight at the slot point made a member that reached or overshot it turn round
    // (it cannot swim slower than 0.6 x the pod, nor turn tighter than speed / turn rate) and loop round the slot again and
    // again - a dolphin spinning on the spot (sandbox_spin.js). Near the slot it now swims the pod's heading, eased
    // sideways onto the slot, and falls back by slowing down; only a slot well beyond its turning circle is swum straight
    // at. Source: wolf_sea_life.js _steer (2026-11-05)
    const F32 pod_hd = riding ? g.mRideHeading : g.mHeading;
    const F32 turn_r = llmax(0.5f, want) / llmax(0.05f, sp.mTurn * (riding ? 2.f : 1.f));
    const F32 fx = cosf(pod_hd), fy = sinf(pod_hd);
    const F32 along = dx * fx + dy * fy, across = dy * fx - dx * fy;
    const F32 hi = llmax(want * 1.5f, want + 1.f);
    F32 aim = 0.f, speed = 0.f;
    if (dist > 4.f * turn_r + 4.f)
    {
        // a slot far off: straight there, faster (to 1.5x)
        aim = atan2f(dy, dx); speed = llclamp(want + (dist - 2.f) * 0.25f, want * 0.6f, hi);
    }
    else
    {
        // close: the pod's heading, turned onto the slot over two turning circles; ahead faster, behind slower
        aim = pod_hd + atan2f(across, 2.f * turn_r + 2.f); speed = llclamp(want + along * 0.25f, want * 0.6f, hi);
    }
    if (a.mKind == DOLPHIN)
    {
        for (const Animal& o : mAnimals)
        {
            if (&o == &a || o.mGroup != a.mGroup) continue;
            const F32 ox = a.mPos.mV[VX] - o.mPos.mV[VX], oy = a.mPos.mV[VY] - o.mPos.mV[VY], d = len2(ox, oy);
            if (d < SEPARATION_M && d > 1e-3f)
                aim = atan2f(sinf(aim) + oy / d * (SEPARATION_M - d), cosf(aim) + ox / d * (SEPARATION_M - d));
        }
    }
    // never into shallow water: look two body lengths + 15 m ahead
    const F32 ahead = 15.f + 2.f * a.mLen;
    if (depth(a.mPos + LLVector3(cosf(aim) * ahead, sinf(aim) * ahead, 0.f)) < sp.mDepthMin
        && depth(a.mPos) >= sp.mDepthMin) aim = g.mHeading;
    // [SEA LIFE SPIN 2026-11-05] and never at shallows one body length off: the nearest heading with water deep enough
    // there (8 tries each side), else the deepest way - so the aim never pushes it against the edge it turns from
    auto near_at = [&](F32 hd) { return depth(a.mPos + LLVector3(cosf(hd) * a.mLen, sinf(hd) * a.mLen, 0.f)); };
    if (near_at(aim) < sp.mDepthMin)
    {
        bool found = false;
        F32 best = aim, deepest = -1.f, deep_hd = aim;
        for (S32 k = 1; k <= 8 && !found; ++k)
        {
            for (S32 sgn = 1; sgn >= -1; sgn -= 2)
            {
                const F32 hd = aim + (F32)(sgn * k) * F_PI / 8.f, d = near_at(hd);
                if (d >= sp.mDepthMin) { best = hd; found = true; break; }
                if (d > deepest) { deepest = d; deep_hd = hd; }
            }
        }
        aim = found ? best : deep_hd;
    }
    a.mHeading = wrap(a.mHeading + llclamp(wrap(aim - a.mHeading), -rate * dt, rate * dt));
    a.mSpeed += llclamp(speed - a.mSpeed, -2.f * dt, 2.f * dt);
    // [SEA LIFE SPIN 2026-11-05] at the edge of the shallows it used to stop and turn on the spot at half a turn a second
    // (every kind: a 13 m whale spun like a top), or, where the aim pulled back as hard, stop for good. Now it slides along
    // the nearest heading whose next stroke is allowed and turns toward it at its own turn rate.
    const F32 here = depth(a.mPos);
    auto step_to = [&](F32 hd, LLVector3& out)
    {
        out = a.mPos + LLVector3(cosf(hd) * a.mSpeed * dt, sinf(hd) * a.mSpeed * dt, 0.f);
        const F32 there = depth(out);
        return there >= sp.mDepthMin * 0.6f || there > here;   // leaving too (review 2026-11-03)
    };
    LLVector3 next;
    F32 slide = a.mHeading;
    bool stepped = step_to(slide, next);
    for (S32 k = 1; k <= 8 && !stepped; ++k)
    {
        for (S32 sgn = 1; sgn >= -1; sgn -= 2)
        {
            slide = a.mHeading + (F32)(sgn * k) * F_PI / 8.f;
            if (step_to(slide, next)) { stepped = true; break; }
        }
    }
    if (stepped)
    {
        a.mPos.mV[VX] = next.mV[VX]; a.mPos.mV[VY] = next.mV[VY];
        const F32 r = llmax(rate, sp.mTurn) * dt;
        a.mHeading = wrap(a.mHeading + llclamp(wrap(slide - a.mHeading), -r, r));
    }
}

void WolfSeaLife::moveDolphin(Animal& a, F32 dt, F64 now)
{
    const F32 w = water();
    const Group& g = mGroups[(size_t)a.mGroup];
    const bool porpoise = g.mRide.notNull() || g.mPorpoise;
    a.mStateFor -= dt;
    if (a.mState == SWIM)
    {
        const F32 dep = porpoise ? PORPOISE_DEPTH : a.mDepthT;
        const F32 zt = w - llmin(dep, llmax(0.5f, depth(a.mPos) - 1.5f));
        a.mVz += llclamp((zt - a.mPos.mV[VZ]) * 2.f - a.mVz, -4.f * dt, 4.f * dt);
        a.mPos.mV[VZ] += a.mVz * dt;
        a.mPitch = atan2f(a.mVz, llmax(0.5f, a.mSpeed));
        if (a.mStateFor <= 0.f && !g.mLeaving)
        {
            if (porpoise && a.mPos.mV[VZ] > w - 1.2f)
            {
                a.mState = LEAP;
                a.mVz = LEAP_SPEED * sinf(LEAP_DEG * DEG_TO_RAD);
                a.mWasAbove = false;
                a.mSpeed = llmax(a.mSpeed, LEAP_SPEED * cosf(LEAP_DEG * DEG_TO_RAD));
            }
            else if (!porpoise)
            {
                a.mState = BREATHE;
                a.mArcT = 0.f;
                a.mArcLen = 3.f * a.mLen / llmax(1.f, a.mSpeed);
                a.mArcZ0 = a.mPos.mV[VZ];
            }
            if (a.mState == SWIM) a.mStateFor = 0.5f;   // not yet near the surface: try again soon
        }
    }
    else if (a.mState == BREATHE)
    {
        // a rolling arc: the back and the dorsal fin break the surface for about a second at the top
        a.mArcT += dt;
        const F32 s = llclamp(a.mArcT / a.mArcLen, 0.f, 1.f);
        const F32 top = w - SPECIES[DOLPHIN].mRadius[5] * a.mLen * 0.25f;
        const F32 z = a.mArcZ0 + (top - a.mArcZ0) * sinf(F_PI * s);
        a.mVz = (z - a.mPos.mV[VZ]) / llmax(dt, 1e-3f);
        a.mPos.mV[VZ] = z;
        a.mPitch = atan2f((top - a.mArcZ0) * F_PI * cosf(F_PI * s) / a.mArcLen, llmax(0.5f, a.mSpeed));
        if (s >= 1.f) { a.mState = SWIM; a.mStateFor = rnd(BREATH_MIN_S, BREATH_MAX_S); a.mVz = 0.f; }
    }
    else if (a.mState == LEAP)
    {
        // up at 35 degrees through the water, then a ballistic arc: 0.64 s in the air, 0.51 m high, 2.9 m long
        if (a.mPos.mV[VZ] > w) { a.mWasAbove = true; a.mVz -= G_MS2 * dt; }
        a.mPos.mV[VZ] += a.mVz * dt;
        a.mPitch = atan2f(a.mVz, llmax(0.5f, a.mSpeed));
        if (a.mWasAbove && a.mPos.mV[VZ] < w - 0.2f && a.mVz < 0.f)
        {
            sound(SND_KIND_SPLASH, a.mPos, 1.f, now);
            a.mState = SWIM;
            a.mVz *= 0.5f;
            a.mStateFor = g.mRide.notNull() ? rnd(RIDE_LEAP_MIN_S, RIDE_LEAP_MAX_S) : rnd(UNDER_MIN_S, UNDER_MAX_S);
        }
        if (a.mPos.mV[VZ] < w - 3.f) { a.mState = SWIM; a.mStateFor = rnd(UNDER_MIN_S, UNDER_MAX_S); }   // never stuck
    }
    if (g.mLeaving && a.mState == SWIM) a.mDepthT = 3.f;
}

void WolfSeaLife::moveWhale(Animal& a, F32 dt, F64 now)
{
    const F32 w = water();
    const Group& g = mGroups[(size_t)a.mGroup];
    const Species& sp = SPECIES[a.mKind];
    const F32 r = sp.mRadius[5] * a.mLen;
    const F32 floor_z = w - llmax(2.f, depth(a.mPos) - 2.f);
    a.mStateFor -= dt;
    auto toward = [&](F32 zt, F32 rate)
    {
        const F32 dz = llclamp(zt - a.mPos.mV[VZ], -rate * dt, rate * dt);
        a.mPos.mV[VZ] += dz;
        a.mVz = dz / llmax(dt, 1e-3f);
        a.mPitch += llclamp(atan2f(a.mVz, llmax(0.5f, a.mSpeed)) * 0.5f - a.mPitch, -0.3f * dt, 0.3f * dt);
    };
    // the group's leader: its first animal
    const Animal* lead = nullptr;
    for (const Animal& o : mAnimals) { if (o.mGroup == a.mGroup) { lead = &o; break; } }
    if (a.mState == W_DOWN)
    {
        // a group keeps roughly in step: a follower rises within 3 s of its leader (README "Whale")
        if (lead && lead != &a && lead->mState != W_DOWN && !a.mSynced) { a.mSynced = true; a.mStateFor = llmin(a.mStateFor, rnd(0.f, 3.f)); }
        toward(llmax(floor_z, w - llmin(DIVE_MAX_M, llmax(4.f, depth(a.mPos) - 3.f))), 1.0f);
        if (a.mStateFor <= 0.f && !g.mLeaving) a.mState = W_RISE;
    }
    else if (a.mState == W_RISE)
    {
        toward(w - 2.5f, 0.8f);
        if (a.mPos.mV[VZ] >= w - 2.6f)
        {
            a.mState = W_SURFACE;
            a.mStateFor = SURFACE_S;
            a.mBlown = false;
            a.mSynced = false;
            a.mBreaths = (lead && lead != &a && lead->mBreaths > 0) ? lead->mBreaths : irnd(BREATHS_MIN, BREATHS_MAX);
        }
    }
    else if (a.mState == W_SHALLOW)
    {
        toward(w - 3.f, 0.6f);
        if (a.mStateFor <= 0.f) { a.mState = W_SURFACE; a.mStateFor = SURFACE_S; a.mBlown = false; }
        if (g.mLeaving) { a.mState = W_DOWN; a.mStateFor = 999.f; }
    }
    else if (a.mState == W_SURFACE)
    {
        // the back breaks the surface for 3-4 s; the blow at the start
        toward(w - r * 0.6f, 1.5f);
        if (!a.mBlown && a.mPos.mV[VZ] > w - r * 1.2f) { a.mBlown = true; blow(a, now); }
        if (a.mStateFor <= 0.f)
        {
            --a.mBreaths;
            if (a.mBreaths > 0) { a.mState = W_SHALLOW; a.mStateFor = rnd(BREATH_GAP_MIN_S, BREATH_GAP_MAX_S); }
            else { a.mState = W_DIVE; a.mArcT = 0.f; }
        }
    }
    else if (a.mState == W_DIVE)
    {
        // arch high (1.5 s), then pitch nose-down: a humpback's flukes rise clear of the water and slide under; a minke
        // just rolls under
        a.mArcT += dt;
        const F32 steep = (a.mKind == HUMPBACK ? 60.f : 25.f) * DEG_TO_RAD;
        if (a.mArcT < 1.5f)
        {
            a.mPos.mV[VZ] += llclamp(w - r * 0.35f - a.mPos.mV[VZ], -1.f * dt, 1.f * dt);
            a.mPitch += llclamp(-12.f * DEG_TO_RAD - a.mPitch, -0.3f * dt, 0.3f * dt);
        }
        else
        {
            a.mPitch += llclamp(-steep - a.mPitch, -0.45f * dt, 0.45f * dt);
            a.mPos.mV[VZ] -= (a.mKind == HUMPBACK ? 1.6f : 1.2f) * dt;
        }
        if (a.mArcT > 6.f || a.mPos.mV[VZ] < floor_z)
        {
            a.mState = W_DOWN;
            a.mStateFor = rnd(DOWN_MIN_S, DOWN_MAX_S);   // CHOSEN: real dives are 5-15 min
        }
    }
    if (a.mState != W_DIVE && a.mState != W_SURFACE) a.mPitch += llclamp(-a.mPitch, -0.2f * dt, 0.2f * dt);
    a.mPos.mV[VZ] = llmax(a.mPos.mV[VZ], floor_z);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// bow riding (wolf_sea_life.js _rideCheck, _recentre, _rideSlots)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfSeaLife::rideCheck(F64 now)
{
    if (now < mNextRide) return;
    mNextRide = now + 1.0;
    const F32 w = water();
    // moving root objects, not avatars or attachments, 2-12 m/s, on the water over deep water
    std::vector<LLViewerObject*> boats;
    for (const LLPointer<LLViewerObject>& op : gObjectList.getActiveObjects())
    {
        LLViewerObject* o = op.get();
        if (!o || o->isDead() || o->isAvatar() || o->getParent() || o->isAttachment()) continue;
        const LLVector3 p = o->getPositionAgent();
        if (!std::isfinite(p.mV[VX]) || fabsf(p.mV[VZ] - w) > RIDE_WATER_M) continue;
        const LLVector3 v = o->getVelocity();
        const F32 sp = len2(v.mV[VX], v.mV[VY]);
        if (sp < RIDE_SPEED_MIN || sp > RIDE_SPEED_MAX) continue;
        if (depth(p) < SPECIES[DOLPHIN].mDepthMin) continue;
        boats.push_back(o);
    }
    for (S32 gi = 0; gi < (S32)mGroups.size(); ++gi)
    {
        Group& g = mGroups[(size_t)gi];
        if (g.mKind != DOLPHIN || g.mLeaving) continue;
        if (g.mRide.notNull())
        {
            LLViewerObject* o = g.mRide.get();
            const bool gone = o->isDead() || std::find(boats.begin(), boats.end(), o) == boats.end() || now > g.mRideUntil;
            if (gone) { g.mRide = nullptr; g.mPorpoise = false; recentre(gi); }
            continue;
        }
        for (LLViewerObject* o : boats)
        {
            const LLVector3 p = o->getPositionAgent();
            const std::string key = o->getID().asString() + ":" + std::to_string(g.mId);
            if (len2(p.mV[VX] - g.mCentre.mV[VX], p.mV[VY] - g.mCentre.mV[VY]) > RIDE_SEEK_M || mRideTried.count(key)) continue;
            mRideTried.insert(key);
            if (mRideTried.size() > 512) mRideTried.clear();
            if (ll_frand() >= RIDE_CHANCE) continue;
            g.mRide = o;
            g.mRideUntil = now + rnd(RIDE_MIN_S, RIDE_MAX_S);
            g.mRideSpeed = 0.f;
            g.mRideHeading = 0.f;
            LL_DEBUGS("WolfSeaLife") << "a pod rides the bow of " << o->getID() << LL_ENDL;
            break;
        }
    }
}

void WolfSeaLife::recentre(S32 gi)
{
    Group& g = mGroups[(size_t)gi];
    F32 x = 0.f, y = 0.f;
    S32 n = 0;
    const Animal* first = nullptr;
    for (const Animal& a : mAnimals)
    {
        if (a.mGroup != gi) continue;
        if (!first) first = &a;
        x += a.mPos.mV[VX]; y += a.mPos.mV[VY]; ++n;
    }
    if (n) { g.mCentre.mV[VX] = x / (F32)n; g.mCentre.mV[VY] = y / (F32)n; }
    if (first) g.mHeading = g.mTarget = first->mHeading;
}

/** Each riding dolphin's slot ahead of the bow (wolf_sea_life.js _rideSlots). */
void WolfSeaLife::rideSlots(S32 gi)
{
    Group& g = mGroups[(size_t)gi];
    LLViewerObject* o = g.mRide.get();
    const LLVector3 p = o->getPositionAgent();
    const LLVector3 v = o->getVelocity();
    const F32 sp = len2(v.mV[VX], v.mV[VY]);
    g.mRideSpeed = sp;
    if (sp > 0.3f) g.mRideHeading = atan2f(v.mV[VY], v.mV[VX]);
    const LLVector3 s = o->getScale();
    const F32 half = llclamp(llmax(s.mV[VX], s.mV[VY]) / 2.f, 1.f, 30.f);
    const F32 fx = cosf(g.mRideHeading), fy = sinf(g.mRideHeading);
    S32 i = 0;
    for (Animal& a : mAnimals)
    {
        if (a.mGroup != gi) continue;
        const F32 ahead = half + 1.f + (F32)(i % 3) * 1.4f + (F32)(i / 6) * 1.2f;
        const F32 side = ((i % 2) ? 1.f : -1.f) * (0.6f + (F32)((i / 2) % 3) * 0.9f);
        a.mSlot = LLVector3(p.mV[VX] + fx * ahead - fy * side, p.mV[VY] + fy * ahead + fx * side, 0.f);
        ++i;
    }
    g.mCentre = LLVector3(p.mV[VX] + fx * (half + 3.f), p.mV[VY] + fy * (half + 3.f), 0.f);
    g.mHeading = g.mRideHeading;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the blow (wolf_sea_life.js _blow, _moveSpray)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfSeaLife::blow(const Animal& a, F64 now)
{
    const Blow& b = BLOW[a.mKind];
    if (b.mN <= 0) return;
    const F32 k = a.mLen / SPECIES[a.mKind].mLen;
    // the blowhole: on top, a fifth of the length back from the head
    const F32 fx = cosf(a.mHeading), fy = sinf(a.mHeading);
    const LLVector3 hole(a.mPos.mV[VX] + fx * 0.3f * a.mLen, a.mPos.mV[VY] + fy * 0.3f * a.mLen,
                         llmax(water(), a.mPos.mV[VZ] + SPECIES[a.mKind].mRadius[3] * a.mLen));
    const F32 H = b.mHeight * k, W = b.mWidth * k, rise = 0.6f;
    for (S32 i = 0; i < b.mN * DROPS_PER_POINT && (S32)mDrops.size() < MAX_DROPS; ++i)
    {
        const F32 h_frac = rnd(0.45f, 1.0f), ang = ll_frand(F_TWO_PI), spread = W * 0.5f * rnd(0.2f, 1.f) * h_frac;
        Drop d;
        d.mPos = hole;
        d.mVel = LLVector3(cosf(ang) * spread / rise, sinf(ang) * spread / rise, 2.f * H * h_frac / rise);
        d.mDec = 2.f * H * h_frac / (rise * rise);
        d.mLife = b.mLife * rnd(0.75f, 1.15f);
        d.mSize = rnd(0.35f, 0.7f) * llmax(0.6f, W) * 0.6f;   // three smaller blobs for each of WolfStorm's points
        d.mRise = rise;
        mDrops.push_back(d);
    }
    sound(SND_KIND_BLOW, hole, a.mKind == MINKE ? 0.5f : 1.f, now);
}

void WolfSeaLife::moveSpray(F32 dt)
{
    for (Drop& p : mDrops)
    {
        p.mAge += dt;
        if (p.mAge < p.mRise)
        {
            p.mPos.mV[VZ] += p.mVel.mV[VZ] * dt;
            p.mVel.mV[VZ] = llmax(0.f, p.mVel.mV[VZ] - p.mDec * dt);
            p.mPos.mV[VX] += p.mVel.mV[VX] * dt;
            p.mPos.mV[VY] += p.mVel.mV[VY] * dt;
        }
        else
        {
            p.mPos.mV[VZ] -= 0.35f * dt;
            p.mPos.mV[VX] += p.mVel.mV[VX] * 0.15f * dt;
            p.mPos.mV[VY] += p.mVel.mV[VY] * 0.15f * dt;
            p.mSize += 0.25f * dt;
        }
    }
    mDrops.erase(std::remove_if(mDrops.begin(), mDrops.end(), [](const Drop& d) { return d.mAge >= d.mLife; }), mDrops.end());
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// sounds (wolf_sea_life.js _sound, _groupSounds)
// ═══════════════════════════════════════════════════════════════════════════════════════

/** One sound at `at`, played as AMBIENT so the Ambient volume governs it: the birds' gentle roll-off (wolfbirds.cpp
 *  playCall), louder or quieter under the water, at most MAX_VOICES at once, muffled under a roof as the birds'. */
void WolfSeaLife::sound(S32 which, const LLVector3& at, F32 mult, F64 now)
{
    static LLCachedControl<bool> sound_on(gSavedSettings, "WolfViewerSeaLifeSound", true);
    if (!sound_on || !gAudiop) return;
    mVoiceEnds.erase(std::remove_if(mVoiceEnds.begin(), mVoiceEnds.end(), [now](F64 t) { return t <= now; }), mVoiceEnds.end());
    if ((S32)mVoiceEnds.size() >= MAX_VOICES) return;
    const SoundKind& s = SND[which];
    const bool under = camera_under_water();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    const F32 d = (at - cam).length();
    if (d > (under ? s.mUnderMax : s.mMax)) return;
    const F32 base = under ? (s.mUnderBase >= 0.f ? s.mUnderBase : s.mBase * s.mUnderMult) : s.mBase;
    const Sound& file = s.mFiles[ll_rand(s.mCount)];
    if (!WolfWeatherSound::instance().recordingReady(file.mRec)) return;   // decoding: the next one will play
    const F32 indoors = (WolfWeather::instanceExists() && WolfWeather::instance().cameraUnderRoof()) ? 0.35f : 1.f;
    const F32 gain = llclamp(base * mult * (s.mFull / llmax(s.mFull, d)) * indoors * WolfSoundEffects::level(WolfSoundEffects::SEA_LIFE), 0.f, 1.f);   // [SOUND EFFECTS 2026-11-06/]
    LLVector3 dir = at - cam;
    if (dir.isExactlyZero()) dir = camera->getAtAxis();
    dir.normVec();
    // under the water sound carries all round: less panning (WolfStorm pans x 0.4), so the sound is placed mostly ahead
    if (under)
    {
        const LLVector3 fwd = camera->getAtAxis();
        const LLVector3 lft = camera->getLeftAxis();
        const F32 side = dir * lft;
        dir = dir - lft * (side * 0.6f);
        if (dir.isExactlyZero()) dir = fwd;
        dir.normVec();
    }
    const LLVector3 pos = cam + dir * CALL_EAR_M;
    gAudiop->triggerSound(LLUUID(file.mRec.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT, gAgent.getPosGlobalFromAgent(pos));
    mVoiceEnds.push_back(now + (F64)file.mSecs);
}

void WolfSeaLife::groupSounds(F64 now, const LLVector3& cam)
{
    for (S32 gi = 0; gi < (S32)mGroups.size(); ++gi)
    {
        Group& g = mGroups[(size_t)gi];
        F32 best = 1e9f;
        LLVector3 at;
        for (const Animal& a : mAnimals)
        {
            if (a.mGroup != gi) continue;
            const F32 d = (a.mPos - cam).length();
            if (d < best) { best = d; at = a.mPos; }
        }
        if (best > 1e8f) continue;
        if (g.mKind == DOLPHIN && now >= g.mNextCall) { g.mNextCall = now + rnd(4.f, 12.f); sound(SND_KIND_DOLPHIN, at, 1.f, now); }
        if (g.mKind == HUMPBACK && now >= g.mNextSong) { g.mNextSong = now + rnd(25.f, 60.f); sound(SND_KIND_SONG, at, 1.f, now); }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the frame (wolf_sea_life.js _move, update)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfSeaLife::move(F32 dt, F64 now, const LLVector3& cam)
{
    for (S32 gi = 0; gi < (S32)mGroups.size(); ++gi)
    {
        if (mGroups[(size_t)gi].mRide.notNull()) rideSlots(gi);
        else moveGroup(mGroups[(size_t)gi], dt);
    }
    for (Animal& a : mAnimals)
    {
        steer(a, dt);
        if (a.mKind == DOLPHIN) moveDolphin(a, dt, now);
        else moveWhale(a, dt, now);
        const Species& sp = SPECIES[a.mKind];
        const F32 hz = sp.mWaveHz * llclamp(a.mSpeed / llmax(0.5f, sp.mCruise), 0.4f, 1.5f) * (mGroups[(size_t)a.mGroup].mPorpoise ? 1.5f : 1.f);
        a.mPhase = fmodf(a.mPhase + F_TWO_PI * hz * dt, F_PI * 64.f);
    }
    // drop: leaving animals far off, out of view, or gone under while the camera is above the water; groups with none
    const F32 w = water();
    const bool cam_under = camera_under_water();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    std::vector<S32> live(mGroups.size(), 0);
    // [SEALIFE AGROUND 2026-11-08] a group aground goes at once, whole. The step gates (moveGroup step_to: >= 0.75 x the
    // kind's depth, or deeper than here; steer step_to: >= 0.6 x, or deeper) never let a swimming group into water under
    // half its depth, so a group found there has had the ground change under it: terrain that came after it was placed
    // (depth(), before this fix), land raised under it on a sandbox, or the water height changed. The gates then refuse
    // every step (0 > 0 is false, both ways), the pod stands still inside the land, moveDolphin holds it 0.5 m under the
    // water line and moveWhale on floor_z = w - 2: swimming through the terrain for as long as the camera stays. Sending
    // it away cannot help (a leaving group keeps the gates, "never aground" in moveGroup), so it is dropped.
    // Source: wolf_sea_life.js _move (2026-11-08)
    std::vector<bool> aground(mGroups.size(), false);
    for (size_t gi = 0; gi < mGroups.size(); ++gi)
    {
        const Group& g = mGroups[gi];
        const F32 limit = SPECIES[g.mKind].mDepthMin * 0.5f;
        bool stranded = depth(g.mCentre) < limit;
        for (size_t i = 0; i < mAnimals.size() && !stranded; ++i)
        {
            if (mAnimals[i].mGroup == (S32)gi && depth(mAnimals[i].mPos) < limit) stranded = true;
        }
        if (!stranded) continue;
        aground[gi] = true;
        LL_DEBUGS("WolfSeaLife") << "group " << g.mId << " (kind " << (S32)g.mKind << ") aground at " << g.mCentre
                                 << ": dropped" << LL_ENDL;
    }
    size_t keep = 0;
    for (size_t i = 0; i < mAnimals.size(); ++i)
    {
        const Animal& a = mAnimals[i];
        const F32 d = (a.mPos - cam).length();
        // [SEA LIFE EASY 2026-11-06] flat: a camera high over the water keeps the pods under it (leaving ones as before).
        // Source: wolf_sea_life.js _move
        bool drop = dist_flat(a.mPos, cam) > GONE_M * 1.6f || aground[(size_t)a.mGroup];   // [SEALIFE AGROUND 2026-11-08/]
        if (mGroups[(size_t)a.mGroup].mLeaving)
        {
            const bool under = a.mPos.mV[VZ] < w - (a.mKind == DOLPHIN ? 2.5f : 5.f);
            if (d > GONE_M || (d > GONE_HIDDEN_M && !camera->sphereInFrustum(a.mPos, a.mLen)) || (under && d > NEAR_M && !cam_under)) drop = true;
        }
        if (drop) continue;
        ++live[(size_t)a.mGroup];
        mAnimals[keep++] = a;
    }
    mAnimals.resize(keep);
    std::vector<S32> remap(mGroups.size(), -1);
    std::vector<Group> kept;
    kept.reserve(mGroups.size());
    for (size_t k = 0; k < mGroups.size(); ++k)
    {
        if (live[k] == 0) continue;
        remap[k] = (S32)kept.size();
        kept.push_back(mGroups[k]);
    }
    if (kept.size() != mGroups.size())
    {
        for (Animal& a : mAnimals) a.mGroup = remap[(size_t)a.mGroup];
        mGroups.swap(kept);
    }
}

void WolfSeaLife::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;
    LLViewerRegion* region = nullptr;
    const bool on = wanted(region);
    if (!on && mAnimals.empty() && mDrops.empty()) return;   // off (the usual case): nothing at all
    LLTimer cost;
    // the agent frame moves on a region crossing or a rebase: every stored position moves with it (wolfbirds.cpp idle)
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        const LLVector3 delta(mLastOrigin - origin);
        if (!mLastOrigin.isExactlyZero()) rebase(delta);
        mLastOrigin = origin;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (!on)
    {
        // the resident's own switch is instant; the owner's switch (or leaving Wolf) sends them away
        static LLCachedControl<bool> setting_on(gSavedSettings, "WolfViewerSeaLife", true);
        if (!setting_on || !region) { reset(); return; }
        for (Group& g : mGroups) leaveGroup(g, cam);
        if (dt > 0.f) move(dt, now, cam);
        moveSpray(dt);
        return;
    }
    const bool jumped = mHaveLastCamera && (cam - mLastCamera).length() > TELEPORT_M;
    if (region->getRegionID() != mRegionId) { mRegionId = region->getRegionID(); reset(); }   // a new region: start again
    else if (jumped) reset();
    mLastCamera = cam;
    mHaveLastCamera = true;
    if (now >= mNextDirect) { mNextDirect = now + DIRECT_SECS; direct(cam); }
    rideCheck(now);
    if (dt > 0.f && !mAnimals.empty()) move(dt, now, cam);
    moveSpray(dt);
    groupSounds(now, cam);
    if (!mAnimals.empty())
    {
        mCostIdleUs += cost.getElapsedTimeF64() * 1e6;
        ++mCostFrames;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the look (wolf_sea_life.js _buildMesh; our own geometry, no textures, no third-party models)
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: wolf_sea_life.js trisPerAnimal: tip fan + bands + tail cap + dorsal + two pectorals + two flukes
    constexpr S32 SIDES = 8, SIDES_LOD = 6;
    constexpr S32 TRIS_PER_ANIMAL = SIDES + (N_STATIONS - 2) * SIDES * 2 + SIDES + 2 + 8 + 8;
    constexpr S32 VERTS_PER_ANIMAL = TRIS_PER_ANIMAL * 3;
    constexpr S32 VERTS_PER_DROP = 8 * 3;
    constexpr S32 MAX_VERTS = HARD_MAX * VERTS_PER_ANIMAL + MAX_DROPS * VERTS_PER_DROP;   // drawn unindexed (drawArrays)

    struct Writer
    {
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        S32 n = 0;

        void vert(const LLVector3& p, const LLVector3& nn, const F32* c)
        {
            (*pos++).set(p.mV[VX], p.mV[VY], p.mV[VZ], 0.f);   // w = texture index 0 (wolfbirds.cpp, wolffarground.cpp)
            *norm++ = nn;
            *uv++ = LLVector2(0.5f, 0.5f);
            *col++ = LLColor4U((U8)(llclamp(c[0], 0.f, 1.f) * 255.f), (U8)(llclamp(c[1], 0.f, 1.f) * 255.f),
                               (U8)(llclamp(c[2], 0.f, 1.f) * 255.f), 255);
            ++n;
        }
        /** A smooth-shaded triangle wound so its front faces `out` (wolf_sea_life.js tri). */
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& na, const LLVector3& nb,
                 const LLVector3& nc, const F32* ca, const F32* cb, const F32* cc, const LLVector3& out)
        {
            const LLVector3 f = (b - a) % (c - a);
            if (f * out < 0.f) { vert(a, na, ca); vert(c, nc, cc); vert(b, nb, cb); }
            else { vert(a, na, ca); vert(b, nb, cb); vert(c, nc, cc); }
        }
        void flat(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& out, const F32* colour)
        {
            LLVector3 nn = (b - a) % (c - a);
            if (nn.isExactlyZero()) nn = out;
            nn.normVec();
            if (nn * out < 0.f) nn = -nn;
            tri(a, b, c, nn, nn, nn, colour, colour, colour, out);
        }
        void panel(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& up, const F32* top, const F32* under)
        {
            flat(a, b, c, up, top);
            flat(a, b, c, -up, under);
        }
    };
}

bool WolfSeaLife::ensureBuffer()
{
    if (mVB.notNull()) return true;
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    // no indices: every triangle has its own three vertices and the buffer is drawn with drawArrays, so it is not held to
    // the U16 index limit (120 animals and the spray are about 93 000 vertices)
    if (!vb->allocateBuffer(MAX_VERTS, 0))
    {
        LL_WARNS("WolfSeaLife") << "could not allocate the sea life's vertex buffer" << LL_ENDL;
        return false;
    }
    mVB = vb;
    return true;
}

void WolfSeaLife::buildMesh(S32& verts)
{
    verts = 0;
    LLTimer cost;
    if ((mAnimals.empty() && mDrops.empty()) || !ensureBuffer()) return;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    const F32 cam_far = gAgentCamera.mDrawDistance + WolfAltitudeSky::farClipExtra();
    std::vector<S32> vis;
    vis.reserve(mAnimals.size());
    for (size_t i = 0; i < mAnimals.size() && (S32)vis.size() < HARD_MAX; ++i)
    {
        const Animal& a = mAnimals[i];
        if (!camera->sphereInFrustum(a.mPos, a.mLen)) continue;
        const F32 dist = (a.mPos - cam).length();
        if (dist > cam_far || dist > a.mLen * 600.f) continue;
        vis.push_back((S32)i);
    }
    S32 drops = 0;
    for (const Drop& d : mDrops) if (drops < MAX_DROPS && camera->sphereInFrustum(d.mPos, d.mSize)) ++drops;
    if (vis.empty() && drops == 0) return;
    const S32 count = (S32)vis.size() * VERTS_PER_ANIMAL + drops * VERTS_PER_DROP;
    Writer wr;
    if (!mVB->getVertexStrider(wr.pos, 0, count) || !mVB->getNormalStrider(wr.norm, 0, count)
        || !mVB->getTexCoord0Strider(wr.uv, 0, count) || !mVB->getColorStrider(wr.col, 0, count))
    {
        mVB->unmapBuffer();
        return;
    }
    struct RingPt { LLVector3 p, n; F32 c[3]; };
    RingPt rings[N_STATIONS][SIDES];
    for (S32 vi : vis)
    {
        const Animal& a = mAnimals[(size_t)vi];
        const Species& sp = SPECIES[a.mKind];
        const F32 len = a.mLen;
        const bool distant = (a.mPos - cam).length() > LOD_M;   // not "far": <windef.h> defines far (and near) away
        const S32 sides = distant ? SIDES_LOD : SIDES;
        const S32 n_idx = distant ? 6 : N_STATIONS;
        const F32 cp = cosf(a.mPitch);
        const LLVector3 fwd(cosf(a.mHeading) * cp, sinf(a.mHeading) * cp, sinf(a.mPitch));
        LLVector3 L = LLVector3(0.f, 0.f, 1.f) % fwd;
        if (L.isExactlyZero()) L.setVec(0.f, 1.f, 0.f);
        L.normVec();
        const LLVector3 U = fwd % L;
        // the vertical body wave (cetaceans swim with up-and-down strokes): at s (0 head .. 1 tail) the axis moves
        // A s^2 sin(phase - 2 pi 0.8 s) along U
        const F32 amp = 0.08f * len * llclamp(a.mSpeed / llmax(0.5f, sp.mCruise), 0.3f, 1.2f) * (a.mState == LEAP ? 0.3f : 1.f);
        auto wave = [&](F32 s) { return amp * s * s * sinf(a.mPhase - F_TWO_PI * 0.8f * s); };
        auto Pt = [&](F32 s, F32 y, F32 z)
        {
            const F32 x = (0.5f - s) * len, zz = z + wave(s);
            return a.mPos + fwd * x + L * y + U * zz;
        };
        // rings: unit normal and colour round each station (back above, flanks, belly below)
        for (S32 r = 0; r < n_idx; ++r)
        {
            const S32 i = distant ? LOD_STATIONS[r] : r;
            const F32 s = STATIONS[i], rad = sp.mRadius[i] * len, rw = rad * sp.mW[i], rh = rad * sp.mH[i];
            const LLVector3 c0 = a.mPos + fwd * ((0.5f - s) * len) + U * wave(s);
            for (S32 k = 0; k < sides; ++k)
            {
                const F32 th = (F32)k * F_TWO_PI / (F32)sides, cy = sinf(th), cz = cosf(th);
                F32 ny = cy / llmax(sp.mW[i], 0.1f), nz = cz / llmax(sp.mH[i], 0.1f);
                const F32 nl = sqrtf(ny * ny + nz * nz);
                if (nl > 0.f) { ny /= nl; nz /= nl; }
                RingPt& e = rings[r][k];
                e.p = c0 + L * (rw * cy) + U * (rh * cz);
                e.n = L * ny + U * nz;
                const F32 t = (cz + 1.f) / 2.f;
                if (t > 0.5f) mix(sp.mFlank, sp.mBack, (t - 0.5f) * 2.f, e.c);
                else mix(sp.mBelly, sp.mFlank, t * 2.f, e.c);
            }
        }
        const LLVector3 nose = rings[0][0].p;   // radius 0 at s = 0: every point of the first ring is the tip
        for (S32 r = 1; r < n_idx; ++r)
        {
            for (S32 k = 0; k < sides; ++k)
            {
                const S32 k2 = (k + 1) % sides;
                const RingPt& A0 = rings[r - 1][k];
                const RingPt& A1 = rings[r - 1][k2];
                const RingPt& B0 = rings[r][k];
                const RingPt& B1 = rings[r][k2];
                if (r == 1) { wr.tri(nose, B0.p, B1.p, fwd, B0.n, B1.n, sp.mBack, B0.c, B1.c, B0.n + fwd); continue; }
                wr.tri(A0.p, B0.p, B1.p, A0.n, B0.n, B1.n, A0.c, B0.c, B1.c, B0.n);
                wr.tri(A0.p, B1.p, A1.p, A0.n, B1.n, A1.n, A0.c, B1.c, A1.c, B0.n);
            }
        }
        // the tail cap (a short point behind the last ring)
        {
            const LLVector3 tip = Pt(1.02f, 0.f, 0.f), back = -fwd;
            for (S32 k = 0; k < sides; ++k)
            {
                const S32 k2 = (k + 1) % sides;
                const RingPt& A0 = rings[n_idx - 1][k];
                const RingPt& A1 = rings[n_idx - 1][k2];
                wr.tri(tip, A0.p, A1.p, back, A0.n, A1.n, sp.mFlank, A0.c, A1.c, back);
            }
        }
        // fins: the profile's radius at s, for where a fin meets the body
        auto r_at = [&](F32 s, F32& r, F32& rw, F32& rh)
        {
            for (S32 i = 1; i < N_STATIONS; ++i)
            {
                if (s <= STATIONS[i])
                {
                    const F32 t = (s - STATIONS[i - 1]) / (STATIONS[i] - STATIONS[i - 1]);
                    r = (sp.mRadius[i - 1] + (sp.mRadius[i] - sp.mRadius[i - 1]) * t) * len;
                    rw = sp.mW[i - 1] + (sp.mW[i] - sp.mW[i - 1]) * t;
                    rh = sp.mH[i - 1] + (sp.mH[i] - sp.mH[i - 1]) * t;
                    return;
                }
            }
            r = sp.mRadius[N_STATIONS - 1] * len; rw = 0.4f; rh = 1.3f;
        };
        F32 d0r = 0.f, d0w = 0.f, d0h = 0.f, d1r = 0.f, d1w = 0.f, d1h = 0.f;
        r_at(sp.mDorsalS0, d0r, d0w, d0h);
        r_at(sp.mDorsalS1, d1r, d1w, d1h);
        wr.panel(Pt(sp.mDorsalS0, 0.f, d0r * d0h * 0.95f), Pt(sp.mDorsalS1, 0.f, d1r * d1h * 0.95f),
                 Pt(sp.mDorsalTip, 0.f, d0r * d0h + sp.mDorsalHeight * len), L, sp.mFin, sp.mFin);
        F32 p0r = 0.f, p0w = 0.f, p0h = 0.f, p1r = 0.f, p1w = 0.f, p1h = 0.f;
        r_at(sp.mPecS0, p0r, p0w, p0h);
        r_at(sp.mPecS1, p1r, p1w, p1h);
        for (S32 side = 0; side < 2; ++side)
        {
            const F32 sg = side == 0 ? 1.f : -1.f;
            const LLVector3 rf = Pt(sp.mPecS0, sg * p0r * p0w * 0.8f, -p0r * p0h * 0.45f);
            const LLVector3 rb = Pt(sp.mPecS1, sg * p1r * p1w * 0.8f, -p1r * p1h * 0.45f);
            const F32 tip_y = sg * (p0r * p0w + sp.mPecReach * len), tip_z = -p0r * p0h * 0.9f - sp.mPecReach * len * 0.35f;
            const LLVector3 tf = Pt(sp.mPecTipS - 0.02f, tip_y, tip_z), tb = Pt(sp.mPecTipS, tip_y * 0.97f, tip_z);
            const LLVector3 up = U + L * (sg * 0.3f);
            wr.panel(rf, rb, tb, up, sp.mFin, sp.mFinUnder);
            wr.panel(rf, tb, tf, up, sp.mFin, sp.mFinUnder);
        }
        // flukes: horizontal, following the body wave at the tail
        const F32 half = sp.mFlukeSpan * len * 0.5f;
        for (S32 side = 0; side < 2; ++side)
        {
            const F32 sg = side == 0 ? 1.f : -1.f;
            const LLVector3 root = Pt(0.93f, 0.f, 0.f), notch = Pt(1.03f, 0.f, 0.f);
            const LLVector3 tip_f = Pt(1.0f, sg * half, 0.f), tip_b = Pt(1.07f, sg * half * 0.92f, 0.f);
            wr.panel(root, tip_f, tip_b, U, sp.mFin, sp.mFinUnder);
            wr.panel(root, tip_b, notch, U, sp.mFin, sp.mFinUnder);
        }
    }
    // [VIEWER] the spray: WolfStorm draws soft alpha points; this pass is opaque, so each drop is a small white blob
    // (an octahedron) whose size follows WolfStorm's alpha (0.55 smoothstep(0,.08,t) (1 - smoothstep(.45,1,t))), grey at
    // night (lit = 0.25 + 0.7 smoothstep(-6, 8, sun elevation))
    if (drops > 0)
    {
        const F32 lit = 0.25f + 0.7f * smoothstep(-6.f, 8.f, sun_elevation_deg());
        const F32 spray[3] = { lit, lit, llmin(1.f, lit * 1.02f) };
        S32 written = 0;
        for (const Drop& d : mDrops)
        {
            if (written >= drops) break;
            if (!camera->sphereInFrustum(d.mPos, d.mSize)) continue;
            ++written;
            const F32 t = d.mAge / d.mLife;
            const F32 fade = smoothstep(0.f, 0.08f, t) * (1.f - smoothstep(0.45f, 1.f, t));
            const F32 s = 0.3f * d.mSize * fade;
            const LLVector3 c = d.mPos;
            const LLVector3 ring[4] = { c + LLVector3(s, 0.f, 0.f), c + LLVector3(0.f, s, 0.f), c - LLVector3(s, 0.f, 0.f), c - LLVector3(0.f, s, 0.f) };
            const LLVector3 top = c + LLVector3(0.f, 0.f, s), bottom = c - LLVector3(0.f, 0.f, s);
            for (S32 k = 0; k < 4; ++k)
            {
                const LLVector3& r0 = ring[k];
                const LLVector3& r1 = ring[(k + 1) % 4];
                const LLVector3 mid = (r0 + r1) * 0.5f - c;
                wr.flat(top, r0, r1, mid + LLVector3(0.f, 0.f, s), spray);
                wr.flat(bottom, r0, r1, mid - LLVector3(0.f, 0.f, s), spray);
            }
        }
    }
    mVB->unmapBuffer();
    verts = wr.n;
    mCostMeshUs += cost.getElapsedTimeF64() * 1e6;
}

void WolfSeaLife::renderDeferred()
{
    if (mAnimals.empty() && mDrops.empty()) return;
    S32 verts = 0;
    buildMesh(verts);
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolfbirds.cpp renderDeferred (from WolfFarGround::renderDeferred) - the same program, matrices and
    // attributes, so the animals are lit, fogged and hazed as the scene round them is.
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
