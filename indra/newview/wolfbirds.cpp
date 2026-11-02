/**
 * @file wolfbirds.cpp
 * @brief WolfViewer: wild birds over Wolf Territories regions (ticket 445, Maximus Lear's idea). See wolfbirds.h.
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
#include "wolfbirds.h"

#include <cmath>

#include "llagent.h"
#include "llaudioengine.h"
#include "lldate.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfhurricane.h"
#include "wolfregionweather.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

// Source for every bird figure below (sizes, speeds, wingbeats): region-terrain-work/birds-2026-11-02/README.md "The
// birds" - RSPB species pages for size, Pennycuick (1990, J. Exp. Biol. 150:171) and Pennycuick (2008, Modelling the
// Flying Bird) for flight speed and wingbeat frequency, Ballerini et al. (2008, PNAS 105:1232) for the starlings'
// seven nearest neighbours.
namespace
{
    struct Species
    {
        F32 mSpan;          // wingspan, m
        F32 mLength;        // body length, m
        F32 mSpeed;         // cruising airspeed, m/s
        F32 mBeatHz;        // wingbeat frequency
        F32 mBeatDeg;       // inner wing amplitude, degrees
        F32 mAltMin, mAltMax;   // metres above the surface below
        F32 mTop[3], mTip[3], mUnder[3], mBody[3], mHead[3];   // colours (sRGB 0..1)
    };
    // GULL a herring gull: 1.37 m span, 0.60 m long, 10.6 m/s, 2.8 Hz; grey back, black wingtips, white below.
    // TERN a common tern: 0.82 m span, 0.33 m long, 8.9 m/s, 3.4 Hz; pale grey back, black cap, white below.
    // SMALL a sparrow or finch: 0.23 m span, 0.15 m long, 10 m/s, 14 Hz (bounding flight); brown above, buff below.
    // STARLING 0.39 m span, 0.21 m long, 12 m/s, 12 Hz; glossy black-brown all over.
    const Species SPECIES[WolfBirds::KIND_COUNT] = {
        { 1.37f, 0.60f, 10.6f,  2.8f, 34.f, 10.f, 45.f,
          {0.60f, 0.64f, 0.70f}, {0.06f, 0.06f, 0.07f}, {0.93f, 0.93f, 0.92f}, {0.94f, 0.94f, 0.93f}, {0.95f, 0.95f, 0.94f} },
        { 0.82f, 0.33f,  8.9f,  3.4f, 40.f,  5.f, 14.f,
          {0.78f, 0.80f, 0.84f}, {0.50f, 0.52f, 0.56f}, {0.95f, 0.95f, 0.95f}, {0.95f, 0.95f, 0.95f}, {0.04f, 0.04f, 0.05f} },
        { 0.23f, 0.15f, 10.0f, 14.0f, 55.f,  4.f, 18.f,
          {0.45f, 0.33f, 0.22f}, {0.28f, 0.21f, 0.14f}, {0.80f, 0.72f, 0.60f}, {0.50f, 0.38f, 0.26f}, {0.40f, 0.30f, 0.21f} },
        { 0.39f, 0.21f, 12.0f, 12.0f, 45.f, 35.f, 90.f,
          {0.08f, 0.08f, 0.10f}, {0.05f, 0.05f, 0.06f}, {0.13f, 0.12f, 0.13f}, {0.09f, 0.09f, 0.11f}, {0.07f, 0.07f, 0.09f} },
    };

    // The director: how often it decides, and how far round the camera birds live.
    constexpr F64 DIRECT_SECS = 3.0;
    constexpr F32 NEAR_M = 60.f, FAR_M = 220.f;      // where a flock's centre may be
    constexpr F32 ENTRY_M = 170.f;                   // a new flock flies in from this far beyond its centre
    constexpr F32 GONE_M = 320.f;                    // a leaving bird is dropped beyond this
    constexpr F32 GONE_HIDDEN_M = 140.f;             // ... or beyond this when out of view
    constexpr F32 TELEPORT_M = 300.f;                // the camera jumped: start again
    // [BIRDS 2026-11-02] A camera this high over the ground or sea is in a skybox or on a high build: no birds there.
    // Birds keep to their own heights over the surface (the tallest, a murmuration, to 90 m), never up in the sky.
    constexpr F32 CAMERA_HIGH_M = 150.f;
    // Flock sizes before the daylight / weather / budget scale.
    constexpr S32 GULLS_MIN = 4, GULLS_MAX = 9;
    constexpr S32 TERNS_MIN = 3, TERNS_MAX = 6;
    constexpr S32 SMALL_MIN = 8, SMALL_MAX = 18;
    constexpr S32 STARLINGS = 140;
    constexpr S32 MAX_FLOCKS = 5;
    constexpr S32 HARD_MAX_BIRDS = 300;
    // Built cells: CELL_M squares; a cell with this much build in it is "built" and birds keep out of it.
    constexpr F32 CELL_M = 16.f;
    constexpr S32 MAX_CELLS = 512;                   // a 8192 m region
    constexpr F32 BUILT_WEIGHT = 14.f;
    constexpr F32 SKYBOX_ABOVE_M = 60.f;             // objects higher than this over the ground are not the town below
    constexpr S32 SCAN_PER_FRAME = 3000;
    constexpr F64 SCAN_SECS = 30.0;
    // Calls: how far each kind is heard, how often a flock calls.
    constexpr F32 CALL_EAR_M = 4.f;                  // the call is placed this far along the bird's direction (see playCall)
    constexpr F32 BIRD_G_MS2 = 9.81f;
    // Beyond this many wingspans a bird's body is a few pixels across: only its wings are drawn (12 of its 28 triangles saved).
    constexpr F32 LOD_SPANS = 120.f;

    // The calls, app_settings/wolfbirds (SOURCES.md there; region-terrain-work/birds-2026-11-02/tools/build_sounds.py).
    const WolfWeatherSound::Recording CALL_GULL[3] = {
        { "wolf_bird_gull_1.ogg", "c75a55b5-6904-4389-8d66-830368e62d2f", nullptr, false, "wolfbirds" },
        { "wolf_bird_gull_2.ogg", "8178967f-63ec-423a-a9ca-2edf64dd43bf", nullptr, false, "wolfbirds" },
        { "wolf_bird_gull_3.ogg", "22e8f39f-50e8-445d-b966-1beea24af28b", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_TERN[3] = {
        { "wolf_bird_tern_1.ogg", "a1d20907-e572-491f-a508-c05b73b74ebc", nullptr, false, "wolfbirds" },
        { "wolf_bird_tern_2.ogg", "38f4bd21-41d8-4a3e-aa64-b79c569ff3bb", nullptr, false, "wolfbirds" },
        { "wolf_bird_tern_3.ogg", "462eae77-e9f2-4163-bc74-678b095ef76b", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_SMALL[3] = {
        { "wolf_bird_small_1.ogg", "289b6347-4726-4eff-8ad7-a86ce199920b", nullptr, false, "wolfbirds" },
        { "wolf_bird_small_2.ogg", "c0e58dbc-8ceb-44d4-ad10-33a9370d9877", nullptr, false, "wolfbirds" },
        { "wolf_bird_small_3.ogg", "a194a129-3cfc-45ec-969c-415cd4cb3600", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_FLOCK =
        { "wolf_bird_flock_1.ogg", "5ccc9fba-3b59-4c33-8b31-5b6255759964", nullptr, false, "wolfbirds" };

    F32 frand_range(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }
    S32 rand_range(S32 lo, S32 hi) { return lo + ll_rand(hi - lo + 1); }

    F32 smoothstep(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    LLVector3 flat(const LLVector3& v) { return LLVector3(v.mV[VX], v.mV[VY], 0.f); }

    /** The sun's height over the horizon, degrees, in the sky on screen (llenvironment.h getCurrentSky; as
     *  wolfautoenvironment.cpp:313). +90 when no sky is known yet (birds are not held back by a missing sky). */
    F32 sun_elevation_deg(bool& west)
    {
        west = false;
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return 90.f;
        const LLVector3 sun = sky->getSunDirection();
        west = sun.mV[VX] < 0.f;   // +X is east (region coordinates): the evening sun is in the west
        return (F32)(std::asin(llclamp((F64)sun.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI);
    }

    /** [BIRDS 2026-11-02] Paul: "not everyone will want bird flocks on their region - for example a space region". An
     *  Earth sky has an atmosphere: the sky on screen (llsettingssky.h getBlueDensity / getBlueHorizon / getHazeDensity,
     *  the EEP's own sky colour and haze) is not all black. A sky with no blue and no haze is space (or a black void), and
     *  no birds fly in it. The thresholds are "black to the eye" (2 % of full scale), not any preset's value. */
    bool earth_sky()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return true;
        const LLColor3 bd = sky->getBlueDensity(), bh = sky->getBlueHorizon();
        const F32 blue = llmax(llmax(bd.mV[0], bd.mV[1]), llmax(bd.mV[2], llmax(bh.mV[0], llmax(bh.mV[1], bh.mV[2]))));
        return !(blue < 0.02f && sky->getHazeDensity() < 0.02f);
    }

    /** The month now (1..12, UTC), and whether the region is south of the equator: its real place when it follows one
     *  (the automatic environment), else the north (the grid's own clock is the UK's). */
    S32 month_now()
    {
        S32 month = 0;
        LLDate::now().split(nullptr, &month);
        return month;
    }
    bool southern()
    {
        if (!WolfRegionWeather::instanceExists()) return false;
        const WolfRegionWeather& rw = WolfRegionWeather::instance();
        return (rw.autoEnvSky() || rw.autoEnvWeather()) && rw.autoEnvLat() < 0.0;
    }

    /** Starlings gather in winter: October to March north of the equator, April to September south of it. */
    bool murmuration_season()
    {
        const S32 month = month_now();
        const bool winter_north = (month >= 10 || month <= 3);
        return southern() ? !winter_north : winter_north;
    }

    /** Terns breed here and fish the coasts from April to September (north); otherwise gulls only. */
    bool tern_season()
    {
        const S32 month = month_now();
        const bool summer_north = (month >= 4 && month <= 9);
        return southern() ? !summer_north : summer_north;
    }

    /** 0..1: how much the weather on screen keeps birds down. Source: WolfWeatherProfile (wolfweatherprofile.h): kind,
     *  level 1..4 (light .. torrential / blizzard), fog 0..100. */
    F32 weather_factor()
    {
        if (!WolfWeather::instanceExists() || !WolfWeather::enabled()) return 1.f;
        const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
        F32 f = 1.f;
        if (p.mKind != WolfWeatherProfile::CLEAR)
        {
            if (p.mLevel >= 3) return 0.f;          // a downpour or a blizzard: they shelter
            f = (p.mLevel <= 1) ? 0.7f : 0.35f;
        }
        if (p.mFog > 60) f *= 0.5f;
        return f;
    }

    /** True while a hurricane is over the region (wolfhurricane.h timeAt: stage NONE before it forms and after it is gone). */
    bool hurricane_now()
    {
        if (!WolfRegionWeather::instanceExists()) return false;
        const WolfHurricane* h = WolfRegionWeather::instance().hurricane();
        return h && h->timeAt(LLDate::now().secondsSinceEpoch()).active();
    }
}

WolfBirds::WolfBirds() {}
WolfBirds::~WolfBirds() {}

void WolfBirds::rebase(const LLVector3& delta)
{
    for (Bird& b : mBirds)
    {
        b.mPos += delta;
        b.mWaypoint += delta;   // mOrbitOffset is relative to the flock's centre
    }
    for (Flock& f : mFlocks)
    {
        f.mCentre += delta;
        f.mTarget += delta;
        f.mExit += delta;
    }
    mLastCamera += delta;
}

void WolfBirds::reset()
{
    mBirds.clear();
    mFlocks.clear();
    mNextDirect = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the place
// ═══════════════════════════════════════════════════════════════════════════════════════

/** The region under a point: its own water height for the sea level (a neighbour's can differ). Null over ground no
 *  region covers; then a point is not water, and the surface is the land the world resolves (review 2026-11-02). */
F32 WolfBirds::surfaceZ(const LLVector3& agent) const
{
    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(agent);
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(agent);
    return r ? llmax(land, r->getWaterHeight()) : land;
}

bool WolfBirds::overWater(const LLVector3& agent) const
{
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(agent);
    if (!r) return false;
    // half a metre under: the wet sand at the edge is not sea
    return LLWorld::getInstance()->resolveLandHeightAgent(agent) < r->getWaterHeight() - 0.5f;
}

bool WolfBirds::inRegion(LLViewerRegion* region, const LLVector3& agent, F32 margin) const
{
    if (!region) return false;
    const LLVector3 o = region->getOriginAgent();
    const F32 w = region->getWidth();
    const F32 x = agent.mV[VX] - o.mV[VX], y = agent.mV[VY] - o.mV[VY];
    return x >= -margin && y >= -margin && x <= w + margin && y <= w + margin;
}

bool WolfBirds::built(const LLVector3& agent) const
{
    LLViewerRegion* r = gAgent.getRegion();
    if (!r || !mHaveBuilt || mCells <= 0) return false;
    const LLVector3 o = r->getOriginAgent();
    const S32 cx = (S32)floorf((agent.mV[VX] - o.mV[VX]) / CELL_M);
    const S32 cy = (S32)floorf((agent.mV[VY] - o.mV[VY]) / CELL_M);
    if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) return false;
    return (F32)mBuilt[(size_t)(cy * mCells + cx)] >= BUILT_WEIGHT;
}

/**
 * Which cells are crowded with builds, from the objects the viewer already holds (gObjectList), spread over frames
 * (SCAN_PER_FRAME objects each) and repeated every SCAN_SECS. Only prims count (LL_PCODE_VOLUME, llvolume.h): not trees
 * or grass (birds like trees), not avatars or their attachments, not skyboxes high over the ground. Each prim adds
 * 1 + its largest side / 8 m to the cell its centre is in, so a few big walls count as much as many small parts.
 */
void WolfBirds::scanBuilds(LLViewerRegion* region, F64 now)
{
    if (mScanIndex < 0)
    {
        if (now < mNextScan) return;
        mCells = llclamp((S32)ceilf(region->getWidth() / CELL_M), 1, MAX_CELLS);
        mBuiltScan.assign((size_t)(mCells * mCells), 0);
        mScanIndex = 0;
    }
    const LLVector3 o = region->getOriginAgent();
    const S32 n = gObjectList.getNumObjects();
    const S32 end = llmin(n, mScanIndex + SCAN_PER_FRAME);
    for (S32 i = mScanIndex; i < end; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isDead() || obj->getRegion() != region) continue;
        if (obj->getPCode() != LL_PCODE_VOLUME || obj->isAttachment() || obj->isAvatar()) continue;
        const LLVector3 p = obj->getPositionAgent();
        const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(p);
        if (p.mV[VZ] > ground + SKYBOX_ABOVE_M) continue;
        const S32 cx = (S32)floorf((p.mV[VX] - o.mV[VX]) / CELL_M);
        const S32 cy = (S32)floorf((p.mV[VY] - o.mV[VY]) / CELL_M);
        if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) continue;
        const LLVector3 s = obj->getScale();
        const F32 w = 1.f + llmax(s.mV[VX], llmax(s.mV[VY], s.mV[VZ])) / 8.f;
        U16& c = mBuiltScan[(size_t)(cy * mCells + cx)];
        c = (U16)llmin(60000.f, (F32)c + w);
    }
    mScanIndex = end;
    if (mScanIndex >= n)
    {
        mBuilt.swap(mBuiltScan);
        mHaveBuilt = true;
        mScanIndex = -1;
        mNextScan = now + SCAN_SECS;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the director
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfBirds::wanted(LLViewerRegion*& region) const
{
    region = gAgent.getRegion();
    if (!region) return false;
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerBirds", true);
    if (!on) return false;
    if (!WolfGrid::showsWolfRegionWeather()) return false;   // Wolf regions only, visitors included (wolfgrid.h)
    if (!WolfRegionWeather::instanceExists() || !WolfRegionWeather::instance().birdsAllowed()) return false;
    return true;
}

bool WolfBirds::pickPlace(LLViewerRegion* region, Kind kind, F32 min_m, F32 max_m, LLVector3& out) const
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (S32 attempt = 0; attempt < 16; ++attempt)
    {
        const F32 a = ll_frand(F_TWO_PI), d = frand_range(min_m, max_m);
        LLVector3 p(cam.mV[VX] + cosf(a) * d, cam.mV[VY] + sinf(a) * d, 0.f);
        if (!inRegion(region, p, -8.f)) continue;
        const bool sea = overWater(p);
        if ((kind == GULL || kind == TERN) != sea) continue;
        if (built(p)) continue;
        p.mV[VZ] = surfaceZ(p);
        out = p;
        return true;
    }
    return false;
}

void WolfBirds::direct(LLViewerRegion* region, F64 now)
{
    // What the place offers round the camera: a 9 x 9 sample of points within FAR_M, inside the region.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    S32 sea = 0, land = 0, total = 0;
    for (S32 iy = -4; iy <= 4; ++iy)
    {
        for (S32 ix = -4; ix <= 4; ++ix)
        {
            const LLVector3 p(cam.mV[VX] + (F32)ix * (FAR_M / 4.f), cam.mV[VY] + (F32)iy * (FAR_M / 4.f), 0.f);
            if (!inRegion(region, p, 0.f)) continue;
            ++total;
            if (overWater(p)) ++sea;
            else if (!built(p)) ++land;
        }
    }
    const F32 sea_f = total ? (F32)sea / (F32)total : 0.f;
    const F32 land_f = total ? (F32)land / (F32)total : 0.f;

    bool west = false;
    const F32 sun = sun_elevation_deg(west);
    // Day: full from 4 degrees up; nothing below -4 (they roost by civil dusk).
    const F32 day = smoothstep(-4.f, 4.f, sun);
    const F32 weather = weather_factor();
    const bool storm = hurricane_now();
    const F32 budget = WolfFrameGovernor::budget();
    static LLCachedControl<S32> max_setting(gSavedSettings, "WolfViewerBirdsMax", 160);
    const S32 cap = llclamp((S32)((F32)llclamp((S32)max_setting, 10, HARD_MAX_BIRDS) * budget), 0, HARD_MAX_BIRDS);
    // [BIRDS 2026-11-02] not in a space sky, not up in a skybox; sea birds only where the region's default is sea and coast
    const bool earth = earth_sky();
    const bool high = cam.mV[VZ] - surfaceZ(cam) > CAMERA_HIGH_M;
    const bool sea_only = WolfRegionWeather::instance().birdsSeaOnly();
    const F32 scale = (storm || !earth || high) ? 0.f : day * weather;
    // Murmuration: an evening in the season, the sun from 5 degrees up to 4 down, over open land.
    const bool dusk = west && sun < 5.f && sun > -4.f;
    const bool murmur = !storm && earth && !high && !sea_only && weather > 0.5f && dusk && murmuration_season() && land_f >= 0.25f;

    // Wanted flocks of each kind now.
    S32 want[KIND_COUNT] = { 0, 0, 0, 0 };
    if (scale > 0.05f)
    {
        if (sea_f >= 0.08f) want[GULL] = (sea_f > 0.4f) ? 2 : 1;
        if (sea_f >= 0.08f && tern_season() && day > 0.9f) want[TERN] = 1;
        if (land_f >= 0.15f && !sea_only) want[SMALL] = (land_f > 0.5f) ? 2 : 1;
    }
    if (murmur) want[STARLING] = 1;

    S32 have[KIND_COUNT] = { 0, 0, 0, 0 };
    for (Flock& f : mFlocks)
    {
        if (f.mLeaving || f.mRoosting) continue;
        if (have[f.mKind] >= want[f.mKind])
        {
            // more than wanted: the starlings go down to roost where they are; the others fly off
            if (f.mKind == STARLING && !dusk) f.mRoosting = true;
            else
            {
                f.mLeaving = true;
                const LLVector3 away = flat(f.mCentre - cam);
                LLVector3 dir = away.isExactlyZero() ? LLVector3(1.f, 0.f, 0.f) : away;
                dir.normVec();
                f.mExit = cam + dir * 500.f;
                f.mExit.mV[VZ] = f.mCentre.mV[VZ] + 40.f;
            }
            continue;
        }
        ++have[f.mKind];
    }
    S32 flying = (S32)mBirds.size();
    S32 flocks = (S32)mFlocks.size();   // move() drops a flock as soon as its last bird has gone
    for (S32 k = 0; k < KIND_COUNT && flocks < MAX_FLOCKS; ++k)
    {
        if (have[k] >= want[k]) continue;
        S32 n = 0;
        switch (k)
        {
        case GULL:     n = rand_range(GULLS_MIN, GULLS_MAX); break;
        case TERN:     n = rand_range(TERNS_MIN, TERNS_MAX); break;
        case SMALL:    n = rand_range(SMALL_MIN, SMALL_MAX); break;
        default:       n = STARLINGS; break;
        }
        n = (S32)llround((F32)n * (k == STARLING ? weather : scale));
        n = llmin(n, cap - flying);
        if (n < 2) continue;
        if (!spawnFlock(region, (Kind)k, n, now)) continue;   // no place for this kind here: try the next kind
        flying += n;
        ++flocks;
        break;   // one new flock per decision: they arrive one after another, not all at once
    }

    if (now - mLastLog > 60.0 && (!mBirds.empty() || flocks > 0))
    {
        mLastLog = now;
        LL_DEBUGS("WolfBirds") << mBirds.size() << " birds in " << flocks << " flocks; sea " << sea_f << " land " << land_f
                               << " sun " << sun << (west ? " (evening)" : "") << " weather " << weather << " budget "
                               << budget << " cap " << cap << LL_ENDL;
    }
}

bool WolfBirds::spawnFlock(LLViewerRegion* region, Kind kind, S32 count, F64 now)
{
    Flock f;
    f.mKind = kind;
    if (!pickPlace(region, kind, NEAR_M, FAR_M, f.mCentre)) return false;
    f.mTarget = f.mCentre;
    f.mRetarget = now + frand_range(8.f, 20.f);
    f.mNextCall = now + frand_range(3.f, 10.f);
    f.mLoop = ll_frand(F_TWO_PI);
    // Where they fly in from: ENTRY_M beyond the centre, out of view if that can be found, else simply far away.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    LLVector3 entry = f.mCentre;
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 a = ll_frand(F_TWO_PI);
        LLVector3 e = f.mCentre + LLVector3(cosf(a), sinf(a), 0.f) * ENTRY_M;
        e.mV[VZ] = surfaceZ(e) + SPECIES[kind].mAltMax;
        entry = e;
        if (!camera->sphereInFrustum(e, 2.f) || (e - cam).length() > FAR_M + 40.f) break;
    }
    const S32 index = (S32)mFlocks.size();
    mFlocks.push_back(f);
    const Species& sp = SPECIES[kind];
    const F32 spread = (kind == STARLING) ? 18.f : 8.f;
    for (S32 i = 0; i < count; ++i)
    {
        Bird b;
        b.mFlock = index;
        b.mPos = entry + LLVector3(frand_range(-spread, spread), frand_range(-spread, spread), frand_range(-spread, spread) * 0.4f);
        b.mSpeed = sp.mSpeed * frand_range(0.9f, 1.1f);
        b.mSize = frand_range(0.9f, 1.1f);
        LLVector3 to = f.mCentre - b.mPos;
        to.mV[VZ] = 0.f;
        to.normVec();
        b.mVel = to * b.mSpeed;
        b.mPhase = ll_frand(F_TWO_PI);
        b.mAlt = frand_range(sp.mAltMin, sp.mAltMax);
        b.mOrbitOffset = LLVector3(frand_range(-30.f, 30.f), frand_range(-30.f, 30.f), 0.f);
        b.mOrbitR = frand_range(12.f, 30.f);
        b.mOrbitDir = (ll_frand() < 0.5f) ? -1.f : 1.f;
        b.mWaypoint = f.mCentre;
        b.mFlapFor = frand_range(1.f, 3.f);
        mBirds.push_back(b);
    }
    LL_DEBUGS("WolfBirds") << "flock " << (S32)kind << " of " << count << " coming in" << LL_ENDL;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// flying
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::steer(Bird& b, const LLVector3& desired_vel, F32 max_accel, F32 dt)
{
    LLVector3 dv = desired_vel - b.mVel;
    const F32 m = dv.length();
    if (m > max_accel * dt) dv *= (max_accel * dt) / m;
    const LLVector3 old = b.mVel;
    b.mVel += dv;
    // Bank into the turn: tan(bank) = v * turn rate / g (a coordinated turn).
    const F32 v = llmax(1.f, flat(b.mVel).length());
    const F32 turn = (flat(old) % flat(b.mVel)).mV[VZ] / (llmax(1.f, flat(old).length()) * v * llmax(dt, 1e-3f));
    const F32 bank = llclamp(atanf(v * turn / BIRD_G_MS2), -1.0f, 1.0f);
    b.mBank += (bank - b.mBank) * llmin(1.f, dt * 4.f);
}

/** Every bird: keep above the surface below, and out of built cells: pushed away from every built cell of the 5 x 5 round
 *  the point 15 m ahead of it, and climbing while over one, unless it is already 50 m above it. */
void WolfBirds::keepClear(Bird& b, F32 dt)
{
    const F32 ground = surfaceZ(b.mPos);
    if (b.mPos.mV[VZ] < ground + 1.5f)
    {
        b.mPos.mV[VZ] = ground + 1.5f;
        if (b.mVel.mV[VZ] < 0.f) b.mVel.mV[VZ] = 0.f;
    }
    LLViewerRegion* r = gAgent.getRegion();
    if (!r || !mHaveBuilt || mCells <= 0 || b.mPos.mV[VZ] > ground + 50.f) return;
    LLVector3 dir = flat(b.mVel);
    if (dir.isExactlyZero()) dir.setVec(1.f, 0.f, 0.f);
    dir.normVec();
    const LLVector3 o = r->getOriginAgent();
    const LLVector3 ahead = b.mPos + dir * 15.f;
    const S32 cx0 = (S32)floorf((ahead.mV[VX] - o.mV[VX]) / CELL_M), cy0 = (S32)floorf((ahead.mV[VY] - o.mV[VY]) / CELL_M);
    F32 rx = 0.f, ry = 0.f;
    bool hit = false;
    for (S32 cy = cy0 - 2; cy <= cy0 + 2; ++cy)
    {
        for (S32 cx = cx0 - 2; cx <= cx0 + 2; ++cx)
        {
            if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) continue;
            if ((F32)mBuilt[(size_t)(cy * mCells + cx)] < BUILT_WEIGHT) continue;
            const F32 dx = b.mPos.mV[VX] - (o.mV[VX] + ((F32)cx + 0.5f) * CELL_M);
            const F32 dy = b.mPos.mV[VY] - (o.mV[VY] + ((F32)cy + 0.5f) * CELL_M);
            const F32 d2 = llmax(16.f, dx * dx + dy * dy);
            rx += dx / d2;
            ry += dy / d2;
            hit = true;
        }
    }
    if (!hit) return;
    const F32 k = 600.f * dt;   // the same push as wolf_birds.js _keepClear (tested there: none low over a town)
    b.mVel.mV[VX] += rx * k;
    b.mVel.mV[VY] += ry * k;
    if (built(b.mPos)) b.mVel.mV[VZ] += 6.f * dt;
}

void WolfBirds::moveGull(Bird& b, Flock& f, F32 dt)
{
    // Soaring circles round its own centre near the flock's, which drifts over the sea.
    const LLVector3 c = f.mCentre + b.mOrbitOffset;
    LLVector3 r = flat(b.mPos - c);
    if (r.isExactlyZero()) r.setVec(1.f, 0.f, 0.f);
    const F32 ang = atan2f(r.mV[VY], r.mV[VX]) + b.mOrbitDir * 0.6f;   // aim a little round the circle
    LLVector3 aim(c.mV[VX] + cosf(ang) * b.mOrbitR, c.mV[VY] + sinf(ang) * b.mOrbitR, 0.f);
    aim.mV[VZ] = surfaceZ(aim) + b.mAlt;
    LLVector3 want = aim - b.mPos;
    want.normVec();
    want *= b.mSpeed;
    want.mV[VZ] = llclamp(want.mV[VZ], -1.5f, 1.5f);
    steer(b, want, 4.f, dt);
    // Glide most of the time; a few beats now and then, and whenever it has to climb.
    if (b.mFlapFor > 0.f) b.mFlapFor -= dt;
    else if (b.mGlideFor > 0.f) b.mGlideFor -= dt;
    else if (ll_frand() < 0.5f) b.mFlapFor = frand_range(1.2f, 2.8f);
    else b.mGlideFor = frand_range(5.f, 14.f);
    if (b.mVel.mV[VZ] > 0.6f && b.mFlapFor <= 0.f) b.mFlapFor = 1.f;
    b.mAlt += frand_range(-1.f, 1.f) * dt;
    b.mAlt = llclamp(b.mAlt, SPECIES[GULL].mAltMin, SPECIES[GULL].mAltMax);
}

void WolfBirds::moveTern(Bird& b, Flock& f, F32 dt)
{
    // Patrol low over the water, flapping; now and then hover, then plunge to the surface and climb away.
    const F32 surface = surfaceZ(b.mPos);
    b.mStateFor -= dt;
    switch (b.mState)
    {
    case 0:   // patrol
    {
        LLVector3 to = b.mWaypoint - b.mPos;
        if (flat(to).length() < 8.f || b.mStateFor < -30.f)
        {
            b.mWaypoint = f.mCentre + LLVector3(frand_range(-50.f, 50.f), frand_range(-50.f, 50.f), 0.f);
            if (!overWater(b.mWaypoint)) b.mWaypoint = f.mCentre;
            b.mStateFor = 0.f;
        }
        LLVector3 aim = b.mWaypoint;
        aim.mV[VZ] = surfaceZ(aim) + b.mAlt + 1.2f * sinf(b.mPhase * 0.08f);
        LLVector3 want = aim - b.mPos;
        want.normVec();
        want *= b.mSpeed;
        steer(b, want, 6.f, dt);
        if (ll_frand() < 0.04f * dt && overWater(b.mPos)) { b.mState = 1; b.mStateFor = frand_range(1.2f, 2.2f); }
        break;
    }
    case 1:   // hover
        steer(b, LLVector3(0.f, 0.f, 0.f) + flat(b.mVel) * 0.15f, 8.f, dt);
        if (b.mStateFor <= 0.f) b.mState = (ll_frand() < 0.6f) ? 2 : 0;
        break;
    case 2:   // plunge
        steer(b, LLVector3(flat(b.mVel).mV[VX] * 0.3f, flat(b.mVel).mV[VY] * 0.3f, -7.f), 20.f, dt);
        if (b.mPos.mV[VZ] <= surface + 1.6f) { b.mState = 3; b.mStateFor = 2.f; }
        break;
    default:  // climb away
        steer(b, LLVector3(b.mVel.mV[VX], b.mVel.mV[VY], 0.f) * 0.5f + LLVector3(0.f, 0.f, 3.f) + flat(f.mCentre - b.mPos) * 0.05f, 8.f, dt);
        if (b.mStateFor <= 0.f) b.mState = 0;
        break;
    }
    b.mFlapFor = 1.f;   // terns flap all the time
}

/** Small flocks and starlings: boids (separation, alignment, cohesion) after a moving target. Starlings take their seven
 *  nearest of a sample of the flock (Ballerini et al. 2008: each starling follows about seven neighbours). */
void WolfBirds::moveFlocking(Bird& b, Flock& f, S32 index, F32 dt)
{
    const bool starling = (f.mKind == STARLING);
    LLVector3 sep, ali, coh;
    S32 n = 0;
    const S32 count = (S32)mBirds.size();
    if (starling)
    {
        // seven nearest of up to 24 random flock-mates
        F32 best_d[7];
        S32 best_i[7];
        for (S32 k = 0; k < 7; ++k) { best_d[k] = 1e9f; best_i[k] = -1; }
        for (S32 s = 0; s < 24; ++s)
        {
            const S32 j = ll_rand(count);
            if (j == index || mBirds[j].mFlock != b.mFlock) continue;
            const F32 d = (mBirds[j].mPos - b.mPos).lengthSquared();
            for (S32 k = 0; k < 7; ++k)
            {
                if (best_i[k] == j) break;
                if (d < best_d[k])
                {
                    for (S32 m = 6; m > k; --m) { best_d[m] = best_d[m - 1]; best_i[m] = best_i[m - 1]; }
                    best_d[k] = d; best_i[k] = j;
                    break;
                }
            }
        }
        for (S32 k = 0; k < 7; ++k)
        {
            if (best_i[k] < 0) continue;
            const Bird& o = mBirds[best_i[k]];
            const LLVector3 d = b.mPos - o.mPos;
            const F32 len = d.length();
            if (len < 1.2f && len > 1e-3f) sep += d / (len * len);
            ali += o.mVel;
            coh += o.mPos;
            ++n;
        }
    }
    else
    {
        for (S32 j = 0; j < count; ++j)
        {
            const Bird& o = mBirds[j];
            if (j == index || o.mFlock != b.mFlock) continue;
            const LLVector3 d = b.mPos - o.mPos;
            const F32 len = d.length();
            if (len > 12.f) continue;
            if (len < 1.0f && len > 1e-3f) sep += d / (len * len);
            ali += o.mVel;
            coh += o.mPos;
            ++n;
        }
    }
    LLVector3 want = b.mVel;
    if (n > 0)
    {
        ali /= (F32)n;
        coh /= (F32)n;
        want += (ali - b.mVel) * (starling ? 0.9f : 0.5f);
        want += (coh - b.mPos) * (starling ? 0.25f : 0.4f);
        want += sep * 2.5f;
    }
    // the flock's target, with its height over the ground
    LLVector3 aim = f.mTarget;
    if (!f.mRoosting) aim.mV[VZ] = surfaceZ(aim) + b.mAlt;
    LLVector3 to = aim - b.mPos;
    const F32 dist = to.length();
    const F32 pull = starling ? llclamp((dist - 25.f) / 40.f, 0.05f, 1.f) : 0.6f;
    if (dist > 1e-3f) want += to / dist * b.mSpeed * pull;
    // cruise at its own speed
    const F32 sp = want.length();
    if (sp > 1e-3f) want *= b.mSpeed / sp;
    if (!f.mRoosting) want.mV[VZ] = llclamp(want.mV[VZ], -3.f, 3.f);
    steer(b, want, starling ? 14.f : 10.f, dt);

    // wings: small birds bound (a burst of beats, then a closed-wing glide); starlings beat with short glides
    if (b.mFlapFor > 0.f) { b.mFlapFor -= dt; if (b.mFlapFor <= 0.f) b.mGlideFor = starling ? frand_range(0.3f, 0.9f) : frand_range(0.18f, 0.32f); }
    else if (b.mGlideFor > 0.f) { b.mGlideFor -= dt; b.mVel.mV[VZ] -= (starling ? 0.5f : 1.5f) * dt; }
    else b.mFlapFor = starling ? frand_range(0.8f, 2.0f) : frand_range(0.28f, 0.45f);
}

void WolfBirds::move(F32 dt, F64 now)
{
    // the flocks' own movement
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLViewerRegion* region = gAgent.getRegion();
    for (Flock& f : mFlocks)
    {
        if (f.mLeaving) { f.mTarget = f.mExit; continue; }
        if (f.mKind == STARLING)
        {
            // they wheel round a slow loop over the roost; going to roost, the target sinks to the ground
            f.mLoop += dt * 0.12f;
            if (f.mRoosting)
            {
                f.mTarget = f.mCentre;
                f.mTarget.mV[VZ] = surfaceZ(f.mCentre) + 0.5f;
            }
            else
            {
                f.mTarget = f.mCentre + LLVector3(cosf(f.mLoop) * 55.f, sinf(f.mLoop * 1.7f) * 40.f, 0.f);
            }
            continue;
        }
        if (now >= f.mRetarget)
        {
            f.mRetarget = now + frand_range(8.f, 20.f);
            // drift the centre a little over the same kind of place, and pick the next target near it
            LLVector3 c = f.mCentre + LLVector3(frand_range(-25.f, 25.f), frand_range(-25.f, 25.f), 0.f);
            const bool sea_kind = (f.mKind == GULL || f.mKind == TERN);
            if (inRegion(region, c, -8.f) && overWater(c) == sea_kind && !built(c) && (flat(c - cam).length() < FAR_M + 30.f))
            {
                c.mV[VZ] = surfaceZ(c);
                f.mCentre = c;
            }
            f.mTarget = f.mCentre + LLVector3(frand_range(-40.f, 40.f), frand_range(-40.f, 40.f), 0.f);
            if (built(f.mTarget) || overWater(f.mTarget) != sea_kind) f.mTarget = f.mCentre;
        }
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (S32 i = 0; i < (S32)mBirds.size(); ++i)
    {
        Bird& b = mBirds[i];
        Flock& f = mFlocks[b.mFlock];
        if (f.mLeaving)
        {
            moveFlocking(b, f, i, dt);
        }
        else
        {
            switch (f.mKind)
            {
            case GULL: moveGull(b, f, dt); break;
            case TERN: moveTern(b, f, dt); break;
            default:   moveFlocking(b, f, i, dt); break;
            }
        }
        b.mPos += b.mVel * dt;
        if (!(f.mRoosting)) keepClear(b, dt);
        const Species& sp = SPECIES[f.mKind];
        const F32 beat = (b.mFlapFor > 0.f) ? sp.mBeatHz : 0.f;
        b.mPhase += F_TWO_PI * beat * dt;   // a glide holds the phase; buildMesh sets the gliding wings itself
        if (b.mPhase > F_TWO_PI * 64.f) b.mPhase -= F_TWO_PI * 64.f;
    }

    // drop: leaving birds far off, roosting starlings that reached the ground, flocks with no birds
    std::vector<S32> live(mFlocks.size(), 0);
    size_t w = 0;
    for (size_t i = 0; i < mBirds.size(); ++i)
    {
        Bird& b = mBirds[i];
        const Flock& f = mFlocks[b.mFlock];
        const F32 d = (b.mPos - cam).length();
        bool drop = false;
        if (f.mLeaving && (d > GONE_M || (d > GONE_HIDDEN_M && !camera->sphereInFrustum(b.mPos, 1.f)))) drop = true;
        if (f.mRoosting && b.mPos.mV[VZ] < surfaceZ(b.mPos) + 3.f) drop = true;
        if (d > GONE_M * 1.6f) drop = true;   // anything that has strayed this far
        if (drop) continue;
        ++live[(size_t)b.mFlock];
        mBirds[w++] = b;
    }
    mBirds.resize(w);
    // a flock with no birds left is gone: compact the list and renumber the birds' flocks (the list stays at most
    // MAX_FLOCKS long however long the session runs)
    std::vector<S32> remap(mFlocks.size(), -1);
    std::vector<Flock> kept;
    kept.reserve(mFlocks.size());
    for (size_t k = 0; k < mFlocks.size(); ++k)
    {
        if (live[k] == 0) continue;
        remap[k] = (S32)kept.size();
        kept.push_back(mFlocks[k]);
    }
    if (kept.size() != mFlocks.size())
    {
        for (Bird& b : mBirds) b.mFlock = remap[(size_t)b.mFlock];
        mFlocks.swap(kept);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// calls
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::calls(F64 now)
{
    static LLCachedControl<bool> sound_on(gSavedSettings, "WolfViewerBirdsSound", true);
    if (!sound_on || !gAudiop) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (size_t k = 0; k < mFlocks.size(); ++k)
    {
        Flock& f = mFlocks[k];
        if (now < f.mNextCall) continue;
        switch (f.mKind)
        {
        case GULL:     f.mNextCall = now + frand_range(6.f, 18.f); break;
        case TERN:     f.mNextCall = now + frand_range(4.f, 11.f); break;
        case SMALL:    f.mNextCall = now + frand_range(3.f, 9.f); break;
        default:       f.mNextCall = now + frand_range(2.5f, 4.f); break;
        }
        // the nearest bird of the flock calls
        F32 best = 1e9f;
        LLVector3 at;
        for (const Bird& b : mBirds)
        {
            if (b.mFlock != (S32)k) continue;
            const F32 d = (b.mPos - cam).length();
            if (d < best) { best = d; at = b.mPos; }
        }
        if (best < 1e8f) playCall(f.mKind, at, best);
    }
}

/**
 * One call. OpenAL's inverse-distance model at a 1 m reference (llaudioengine_openal.cpp sets only the rolloff factor)
 * would make a gull 60 m off 36 dB down, inaudible; the real birds carry much further. So the call is placed CALL_EAR_M
 * along the bird's direction (it still comes from where the bird is) and its own gain falls more gently with the real
 * distance. Played as AMBIENT, so the Ambient volume slider governs it, like the rain (wolfweathersound.cpp).
 */
void WolfBirds::playCall(Kind kind, const LLVector3& at_agent, F32 dist)
{
    const WolfWeatherSound::Recording* rec = nullptr;
    F32 base = 0.f, full_m = 0.f, max_m = 0.f;
    switch (kind)
    {
    case GULL:  rec = &CALL_GULL[ll_rand(3)];  base = 0.60f; full_m = 30.f; max_m = 150.f; break;
    case TERN:  rec = &CALL_TERN[ll_rand(3)];  base = 0.45f; full_m = 20.f; max_m = 110.f; break;
    case SMALL: rec = &CALL_SMALL[ll_rand(3)]; base = 0.35f; full_m = 12.f; max_m = 45.f;  break;
    default:    rec = &CALL_FLOCK;             base = 0.50f; full_m = 40.f; max_m = 130.f; break;
    }
    if (dist > max_m) return;
    WolfWeatherSound& ws = WolfWeatherSound::instance();
    if (!ws.recordingReady(*rec)) return;   // decoding: the next call will play
    const F32 indoors = (WolfWeather::instanceExists() && WolfWeather::instance().cameraUnderRoof()) ? 0.35f : 1.f;
    const F32 gain = llclamp(base * (full_m / llmax(full_m, dist)) * indoors, 0.f, 1.f);
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 dir = at_agent - cam;
    if (dir.isExactlyZero()) dir.setVec(0.f, 0.f, 1.f);
    dir.normVec();
    const LLVector3 pos = cam + dir * CALL_EAR_M;
    gAudiop->triggerSound(LLUUID(rec->mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT, gAgent.getPosGlobalFromAgent(pos));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the frame
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;
    // The agent frame moves when the agent crosses into another region (LLAgent::setRegion) or rebases on a huge one
    // (LLAgent::rebaseOrigin): every stored agent-metre position moves with it, so the birds stay where they were in the
    // world (review 2026-11-02). Only then is the camera's jump a teleport or not.
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        const LLVector3 delta(mLastOrigin - origin);
        if (!mLastOrigin.isExactlyZero()) rebase(delta);
        mLastOrigin = origin;
    }
    LLViewerRegion* region = nullptr;
    if (!wanted(region))
    {
        if (!mBirds.empty() || !mFlocks.empty())
        {
            // switched off here (setting, owner, left Wolf): the ones in the air fly away
            bool any_left = false;
            for (Flock& f : mFlocks)
            {
                if (!f.mLeaving)
                {
                    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
                    LLVector3 dir = flat(f.mCentre - cam);
                    if (dir.isExactlyZero()) dir.setVec(1.f, 0.f, 0.f);
                    dir.normVec();
                    f.mLeaving = true;
                    f.mRoosting = false;
                    f.mExit = cam + dir * 500.f;
                    f.mExit.mV[VZ] = f.mCentre.mV[VZ] + 40.f;
                }
                any_left = true;
            }
            static LLCachedControl<bool> on(gSavedSettings, "WolfViewerBirds", true);
            if (!any_left || !on || !region) reset();   // the resident's own switch is instant
            else move(dt, now);
        }
        return;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (region->getRegionID() != mRegionId)
    {
        mRegionId = region->getRegionID();
        mHaveBuilt = false;
        mScanIndex = -1;
        mNextScan = 0.0;
        // a teleport starts again; a walk or flight over the border keeps the birds in the air
        if ((cam - mLastCamera).length() > TELEPORT_M) reset();
    }
    else if ((cam - mLastCamera).length() > TELEPORT_M)
    {
        reset();   // a teleport inside the region
    }
    mLastCamera = cam;
    scanBuilds(region, now);
    // no new flock before the first scan of this region is done: never over a town
    if (mHaveBuilt && now >= mNextDirect)
    {
        mNextDirect = now + DIRECT_SECS;
        direct(region, now);
    }
    if (dt > 0.f && !mBirds.empty()) move(dt, now);
    calls(now);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the look
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Triangles per bird: body 8, tail 2 panels x 2 sides = 4, each wing 4 panels x 2 sides = 8.
    constexpr S32 TRIS_PER_BIRD = 8 + 4 + 16;
    constexpr S32 VERTS_PER_BIRD = TRIS_PER_BIRD * 3;
    constexpr S32 MAX_VERTS = HARD_MAX_BIRDS * VERTS_PER_BIRD;   // 25 200: inside the U16 indices
    static_assert(MAX_VERTS <= 65536, "the birds' indices are U16");

    struct Writer
    {
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        S32 n = 0;

        void vert(const LLVector3& p, const LLVector3& nn, const F32* c)
        {
            (*pos++).set(p.mV[VX], p.mV[VY], p.mV[VZ], 0.f);   // w = texture index 0 (wolffarground.cpp)
            *norm++ = nn;
            *uv++ = LLVector2(0.5f, 0.5f);
            *col++ = LLColor4U((U8)(c[0] * 255.f), (U8)(c[1] * 255.f), (U8)(c[2] * 255.f), 255);
            ++n;
        }
        /** One triangle facing `out` (its winding set so the face that is drawn is the one `out` points from). */
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& out,
                 const F32* ca, const F32* cb, const F32* cc)
        {
            LLVector3 nn = (b - a) % (c - a);
            if (nn.isExactlyZero()) nn = out;
            nn.normVec();
            if (nn * out < 0.f)
            {
                vert(a, -nn, ca); vert(c, -nn, cc); vert(b, -nn, cb);
            }
            else
            {
                vert(a, nn, ca); vert(b, nn, cb); vert(c, nn, cc);
            }
        }
        /** Both faces of a thin panel: the upper face coloured `top_*`, the lower `under_*`. */
        void panel(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& up,
                   const F32* ta, const F32* tb, const F32* tc, const F32* ua, const F32* ub, const F32* uc)
        {
            tri(a, b, c, up, ta, tb, tc);
            tri(a, b, c, -up, ua, ub, uc);
        }
    };
}

bool WolfBirds::ensureBuffer()
{
    if (mVB.notNull()) return true;
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (!vb->allocateBuffer(MAX_VERTS, MAX_VERTS))
    {
        LL_WARNS("WolfBirds") << "could not allocate the birds' vertex buffer" << LL_ENDL;
        return false;
    }
    // The indices never change: every triangle has its own three vertices (flat shading, two colours a panel).
    LLStrider<U16> idx;
    if (!vb->getIndexStrider(idx)) return false;
    for (S32 i = 0; i < MAX_VERTS; ++i) *idx++ = (U16)i;
    vb->unmapBuffer();
    mVB = vb;
    return true;
}

void WolfBirds::buildMesh(S32& verts)
{
    verts = 0;
    if (mBirds.empty() || !ensureBuffer()) return;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    // the birds in view (and big enough to be seen: beyond about 900 spans a bird is under a pixel on a 1080p frame),
    // so only their vertices are written and uploaded
    mVisible.clear();
    for (size_t i = 0; i < mBirds.size() && (S32)(mVisible.size() + 1) * VERTS_PER_BIRD <= MAX_VERTS; ++i)
    {
        const Bird& b = mBirds[i];
        const F32 span = SPECIES[mFlocks[b.mFlock].mKind].mSpan * b.mSize;
        if (!camera->sphereInFrustum(b.mPos, span)) continue;
        if ((b.mPos - cam).length() > span * 900.f) continue;
        mVisible.push_back((S32)i);
    }
    if (mVisible.empty()) return;
    const S32 count = (S32)mVisible.size() * VERTS_PER_BIRD;
    Writer w;
    if (!mVB->getVertexStrider(w.pos, 0, count) || !mVB->getNormalStrider(w.norm, 0, count)
        || !mVB->getTexCoord0Strider(w.uv, 0, count) || !mVB->getColorStrider(w.col, 0, count))
    {
        mVB->unmapBuffer();   // nothing drawn this frame, and nothing left mapped (review 2026-11-02)
        return;
    }
    for (S32 vi : mVisible)
    {
        const Bird& b = mBirds[(size_t)vi];
        const Species& sp = SPECIES[mFlocks[b.mFlock].mKind];
        const F32 span = sp.mSpan * b.mSize, len = sp.mLength * b.mSize;
        const bool distant = (b.mPos - cam).length() > span * LOD_SPANS;   // not "far": <windef.h> defines far (and near) away

        // its frame: forward along its flight, banked into its turn
        LLVector3 fwd = b.mVel;
        if (fwd.isExactlyZero()) fwd.setVec(1.f, 0.f, 0.f);
        fwd.normVec();
        LLVector3 left = LLVector3(0.f, 0.f, 1.f) % fwd;
        if (left.isExactlyZero()) left.setVec(0.f, 1.f, 0.f);
        left.normVec();
        LLVector3 up = fwd % left;
        const F32 cb = cosf(b.mBank), sb = sinf(b.mBank);
        const LLVector3 left_b = left * cb + up * sb;
        const LLVector3 up_b = up * cb - left * sb;
        auto P = [&](F32 x, F32 y, F32 z) { return b.mPos + fwd * x + left_b * y + up_b * z; };

        // the wings' angles: a beat about the shoulder, the hand lagging behind; a glide holds them nearly level, a
        // gull's in its bent "M", a small bird's bound folds them to its sides
        const F32 s = sinf(b.mPhase);
        F32 a1 = sp.mBeatDeg * DEG_TO_RAD * s;
        F32 a2 = sp.mBeatDeg * 0.6f * DEG_TO_RAD * sinf(b.mPhase - 0.7f);
        F32 reach = 1.f;
        const bool gliding = (b.mFlapFor <= 0.f);
        const Kind kind = mFlocks[b.mFlock].mKind;
        if (gliding)
        {
            if (kind == GULL)      { a1 = 10.f * DEG_TO_RAD; a2 = -16.f * DEG_TO_RAD; }
            else if (kind == SMALL){ a1 = 0.f; a2 = 0.f; reach = 0.18f; }
            else                   { a1 = 4.f * DEG_TO_RAD; a2 = -2.f * DEG_TO_RAD; }
        }

        // body: nose, a ring of four at the shoulders, the tail root
        const F32 h = len * 0.09f, wb = len * 0.08f;
        const LLVector3 nose = P(len * 0.5f, 0.f, 0.f);
        const LLVector3 tailr = P(-len * 0.3f, 0.f, 0.f);
        const LLVector3 rt = P(len * 0.08f, 0.f, h), rb = P(len * 0.08f, 0.f, -h * 0.9f);
        const LLVector3 rl = P(len * 0.08f, wb, 0.f), rr = P(len * 0.08f, -wb, 0.f);
        const LLVector3 mid = P(len * 0.08f, 0.f, 0.f);
        const LLVector3 ring[4] = { rt, rl, rb, rr };
        const F32* ring_c[4] = { sp.mBody, sp.mBody, sp.mUnder, sp.mBody };
        for (S32 k = 0; k < 4 && !distant; ++k)
        {
            const LLVector3& r0 = ring[k];
            const LLVector3& r1 = ring[(k + 1) % 4];
            const LLVector3 outward = ((r0 + r1) * 0.5f) - mid;
            w.tri(nose, r0, r1, outward + fwd * 0.2f, sp.mHead, ring_c[k], ring_c[(k + 1) % 4]);
            w.tri(tailr, r0, r1, outward - fwd * 0.2f, sp.mBody, ring_c[k], ring_c[(k + 1) % 4]);
        }
        // tail: a fan, forked for the tern
        const F32 tw = len * ((kind == TERN) ? 0.16f : 0.11f);
        const LLVector3 tl = P(-len * 0.55f, tw, 0.f), tr = P(-len * 0.55f, -tw, 0.f);
        const LLVector3 tc = (kind == TERN) ? P(-len * 0.38f, 0.f, 0.f) : P(-len * 0.52f, 0.f, 0.f);
        if (!distant)
        {
            w.panel(tailr, tl, tc, up_b, sp.mTop, sp.mTip, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            w.panel(tailr, tc, tr, up_b, sp.mTop, sp.mTop, sp.mTip, sp.mUnder, sp.mUnder, sp.mUnder);
        }

        // wings: shoulder - wrist (45 % of the half-span) - tip, swept back towards the tip
        const F32 half = span * 0.5f * reach;
        for (S32 side = 0; side < 2; ++side)
        {
            const F32 sg = side == 0 ? 1.f : -1.f;
            const F32 yw = half * 0.45f, yt = half;
            const F32 c1 = cosf(a1), s1 = sinf(a1), c2 = cosf(a1 + a2), s2 = sinf(a1 + a2);
            // y and z of a point along the wing at span y (from the shoulder at wb)
            auto inner = [&](F32 x, F32 y) { const F32 d = y - wb; return P(x, sg * (wb + d * c1), d * s1); };
            const F32 wy = wb + (yw - wb) * c1, wz = (yw - wb) * s1;
            auto outer = [&](F32 x, F32 y) { const F32 d = y - yw; return P(x, sg * (wy + d * c2), wz + d * s2); };
            const LLVector3 root_l = inner(len * 0.14f, wb), root_t = inner(-len * 0.10f, wb);
            const LLVector3 wrist_l = inner(len * 0.10f, yw), wrist_t = inner(-len * 0.16f, yw);
            const LLVector3 tip_l = outer(-len * 0.04f, yt), tip_t = outer(-len * 0.18f, yt * 0.94f);
            const LLVector3 wing_up = up_b;
            w.panel(root_l, root_t, wrist_t, wing_up, sp.mTop, sp.mTop, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            w.panel(root_l, wrist_t, wrist_l, wing_up, sp.mTop, sp.mTop, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            const F32* under_tip = (kind == GULL) ? sp.mTip : sp.mUnder;
            w.panel(wrist_l, wrist_t, tip_t, wing_up, sp.mTop, sp.mTop, sp.mTip, sp.mUnder, sp.mUnder, under_tip);
            w.panel(wrist_l, tip_t, tip_l, wing_up, sp.mTop, sp.mTip, sp.mTip, sp.mUnder, under_tip, under_tip);
        }
    }
    mVB->unmapBuffer();
    verts = w.n;
}

void WolfBirds::renderDeferred()
{
    if (mBirds.empty()) return;
    S32 verts = 0;
    buildMesh(verts);
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: WolfFarGround::renderDeferred (wolffarground.cpp) - the same program, matrices and attributes, so the birds
    // are lit, fogged and hazed as the scene round them is; a white texture leaves the vertex colours as they are.
    if (LLViewerFetchedTexture::sWhiteImagep.isNull()) return;   // as pipeline.cpp checks it
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
    mVB->drawRange(LLRender::TRIANGLES, 0, (U32)(verts - 1), (U32)verts, 0);
    shader.unbind();
}
