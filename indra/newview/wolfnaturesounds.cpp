/**
 * @file wolfnaturesounds.cpp
 * @brief [NATURE SOUNDS 2026-11-03] Ticket 475: night insects, lapping by still water and surf at the shore
 *        (wolfnaturesounds.h). Mirror of wolfstorm/js/world/wolf_nature_sounds.js: the same rules and numbers.
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

#include "wolfnaturesounds.h"
#include "wolfsoundeffects.h"   // [SOUND EFFECTS 2026-11-06/]

#include <cmath>

#include "llagent.h"
#include "llaudioengine.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "llwind.h"
#include "llworld.h"
#include "fswolfwater.h"
#include "wolfboatrock.h"
#include "wolfdryvolumes.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"
#include "wolfterrainpaint.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

namespace
{
    // ── the recordings ──────────────────────────────────────────────────────────────────────────────────────────────
    // Source: region-terrain-work/nature-sounds-2026-11-03/tools/build_sounds.py (the recipe) and app_settings/wolfnature/
    // SOURCES.md (who, where, licence, SHA-1). The UUIDs are our own fixed ids for the decoded copies in the sound cache
    // (wolfweathersound.h "HOW IT REACHES THE AUDIO ENGINE"); nothing is asked of the grid's asset service.
    enum { INSECTS = 0, LAPPING, SURF_SMALL, SURF_MEDIUM, SURF_BIG, LAYER_COUNT };
    const WolfWeatherSound::Recording RECORDINGS[LAYER_COUNT] = {
        { "wolf_nature_insects.ogg",     "3b61b7d3-1c84-4830-b990-10d8d5cc97d1", nullptr, true, "wolfnature" },
        { "wolf_nature_lapping.ogg",     "bb917688-642a-4c9c-b238-6a5313774f5c", nullptr, true, "wolfnature" },
        { "wolf_nature_surf_small.ogg",  "986e0d01-1fc6-4e2b-bd37-1c0c1cba163e", nullptr, true, "wolfnature" },
        { "wolf_nature_surf_medium.ogg", "49835abc-823b-4abe-b8be-08a8dd242518", nullptr, true, "wolfnature" },
        { "wolf_nature_surf_big.ogg",    "cf2f22b1-c44e-4528-a40e-9ae6b5e29bf6", nullptr, true, "wolfnature" },
    };
    const char* const LAYER_NAMES[LAYER_COUNT] = { "insects", "lapping", "surf small", "surf medium", "surf big" };

    // ── the levels ──────────────────────────────────────────────────────────────────────────────────────────────────
    // FILE_LUFS: each loop's integrated loudness as built (build_sounds.py prints it; the water loops are peak-capped below
    // the rain's -14 LUFS). FULL_LUFS: the loudness each plays at in full (at the water's edge, on a warm still night), at
    // Ambient 100 %, CHOSEN to be gentle: everything under the light rain bed (about -21 LUFS: wolf_rain_bed_light at
    // -14 LUFS x BED_GAIN 0.9 x -6 dB, wolfweathersound.cpp bedLevel), the insects a far-off chorus, big surf the loudest.
    // gain = 10^((FULL - FILE) / 20), capped at 1.
    const F32 FILE_LUFS[LAYER_COUNT] = { -14.0f, -21.8f, -23.5f, -18.9f, -20.8f };
    const F32 FULL_LUFS[LAYER_COUNT] = { -33.0f, -31.0f, -29.0f, -26.0f, -23.0f };

    constexpr F32 TAU_S = 2.5f;           // seconds: every change eases in and out (chosen; the rain uses 1.2 / 1.5 s)
    constexpr F32 TAU_FAST_S = 0.3f;      // under the water, or the resident's own switch
    constexpr F32 SILENT = 0.0005f;       // wolfweathersound.cpp SILENT
    constexpr F32 START_GAIN = 0.001f;    // wolfweathersound.cpp START_GAIN (llaudioengine.cpp:808 skips silent sounds)
    constexpr F64 ASSESS_S = 0.25;        // the targets are worked out four times a second
    constexpr F64 SCAN_S = 1.0;           // the shore is looked for once a second
    constexpr F64 WAVE_S = 0.1;           // the drawn waves are sampled ten times a second
    constexpr F64 LOG_S = 60.0;           // one line a minute in the log while anything plays

    // ── the insects (all CHOSEN; README "Night insects") ────────────────────────────────────────────────────────────
    constexpr F32 NIGHT_FULL_DEG = -4.f;  // the chorus is full once the sun is 4 degrees down ...
    constexpr F32 NIGHT_NONE_DEG = 2.f;   // ... and starts as it sets (none with the sun 2 degrees up)
    constexpr F32 LIGHT_RAIN = 0.4f;      // light rain (level 1): fewer sing; heavier rain or any snow: none
    // the region's wind: from 5 m/s (Beaufort 3, a gentle breeze) fewer, none from 11 m/s (Beaufort 6, a strong breeze).
    // 2026-11-03 first in-world run: 4 / 9 left a 7 m/s night at The Caribbean almost silent.
    constexpr F32 WIND_QUIET_MS = 5.f;
    constexpr F32 WIND_NONE_MS = 11.f;
    constexpr F32 HIGH_FROM_M = 40.f;     // a camera this far over the ground or water fades the insects out ...
    constexpr F32 HIGH_NONE_M = 80.f;     // ... gone at this height (a skybox hears no crickets)

    // ── the water (all CHOSEN; README "Water") ──────────────────────────────────────────────────────────────────────
    constexpr S32 BEARINGS = 16;
    constexpr F32 STEP_M = 3.f;
    constexpr F32 REACH_M = 90.f;         // the shore is looked for this far round the camera
    constexpr F32 SEAWARD_M = 6.f;        // the waves are sampled this far off the shore, on the water
    constexpr F32 WAVE_TAU_S = 6.f;       // the running mean / mean square of the drawn waves
    constexpr F32 SHORE_MOVED_M = 40.f;   // a shore this far from the last one: start the wave statistics again
    constexpr S32 WAVE_WARM_SAMPLES = 30; // and play no water until 3 s of samples (one fresh sample reads as Hs 0: lapping)
    constexpr F32 LAP_RANGE_M = 30.f;     // lapping is heard to here ...
    constexpr F32 SURF_RANGE_M = 45.f;    // ... small surf to here, medium a little further, big much further:
    constexpr F32 SURF_RANGE_MEDIUM_M = 35.f;
    constexpr F32 SURF_RANGE_BIG_M = 80.f;
    // the significant wave height (Hs = 4 x RMS, the oceanographers' definition) that chooses the sound: still water laps
    // up to 0.1 m, small surf from 0.1-0.3 m, medium from 0.6-1.0 m, big from 1.5-2.5 m (crossfaded with equal power)
    constexpr F32 HS_LAP_TO_A = 0.10f, HS_LAP_TO_B = 0.30f;
    constexpr F32 HS_MEDIUM_A = 0.60f, HS_MEDIUM_B = 1.00f;
    constexpr F32 HS_BIG_A = 1.50f, HS_BIG_B = 2.50f;
    constexpr F32 ROOF = 0.35f;           // under a roof (wolfbirds.cpp playCall: the birds' calls are muffled the same)
    constexpr S32 MAX_PLANES = 4096;      // the builders' water planes looked at (painted rivers are many ribbons)

    F32 smooth(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    F32 full_gain(S32 i)
    {
        return llmin(1.f, powf(10.f, (FULL_LUFS[i] - FILE_LUFS[i]) / 20.f));
    }

    /** The sun's height over the horizon, degrees, in the sky on screen (wolfbirds.cpp sun_elevation_deg, the same
     *  reading). +90 when no sky is known yet. */
    F32 sun_elevation_deg()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return 90.f;
        const LLVector3 sun = sky->getSunDirection();
        return (F32)(std::asin(llclamp((F64)sun.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI);
    }

    /** 0..1: the weather on screen and the insects (wolfweatherprofile.h kind CLEAR / RAIN / SNOW, level 1..4). */
    F32 insect_weather()
    {
        if (!WolfWeather::instanceExists() || !WolfWeather::enabled()) return 1.f;
        const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
        if (p.mKind == WolfWeatherProfile::SNOW) return 0.f;
        if (p.mKind == WolfWeatherProfile::RAIN) return p.mLevel <= 1 ? LIGHT_RAIN : 0.f;
        return 1.f;
    }

    /** The water height of the region under agent point p (or the agent's region off the map). */
    bool water_height(const LLVector3& p, F32& out)
    {
        LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
        if (!r) r = gAgent.getRegion();
        if (!r) return false;
        out = r->getWaterHeight();
        return true;
    }

    /** Terrain below its region's water height at agent point p. A point no region covers counts as water (the open
     *  water past the regions this viewer has, as wolfwaterfield.h heightAt's callers treat it). */
    bool wet_at(const LLVector3& p, F32& water)
    {
        if (!water_height(p, water)) return false;
        if (!LLWorld::getInstance()->getRegionFromPosAgent(p)) return true;
        return LLWorld::getInstance()->resolveLandHeightAgent(p) < water;
    }
}

WolfNatureSounds::WolfNatureSounds() : mLayers(LAYER_COUNT) {}
WolfNatureSounds::~WolfNatureSounds() {}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the water round the camera
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfNatureSounds::scanShore(LLViewerRegion* region, const LLVector3& cam)
{
    F32 water0 = 0.f;
    const bool wet0 = wet_at(LLVector3(cam.mV[VX], cam.mV[VY], 0.f), water0);
    F32 best = REACH_M + 1.f;
    LLVector3 best_sea, best_dir;
    F32 best_water = water0;
    for (S32 b = 0; b < BEARINGS; ++b)
    {
        const F32 a = F_TWO_PI * (F32)b / (F32)BEARINGS;
        const LLVector3 dir(cosf(a), sinf(a), 0.f);
        for (F32 r = STEP_M; r <= REACH_M && r < best; r += STEP_M)
        {
            const LLVector3 p(cam.mV[VX] + dir.mV[VX] * r, cam.mV[VY] + dir.mV[VY] * r, 0.f);
            F32 water = 0.f;
            if (wet_at(p, water) == wet0) continue;
            // the shore is between this step and the last; the waves are sampled SEAWARD_M out on the water side
            const F32 shore_r = r - STEP_M * 0.5f;
            const F32 sea_r = wet0 ? llmax(0.f, shore_r - SEAWARD_M) : shore_r + SEAWARD_M;
            best = shore_r;
            best_dir = dir;
            best_sea.setVec(cam.mV[VX] + dir.mV[VX] * sea_r, cam.mV[VY] + dir.mV[VY] * sea_r, 0.f);
            best_water = wet0 ? water0 : water;
            break;
        }
    }
    if (best > REACH_M)
    {
        mHaveShore = false;
        return;
    }
    const F32 dz = cam.mV[VZ] - best_water;
    mShoreDist = sqrtf(best * best + dz * dz);
    if (!mHaveShore || (best_sea - mSeaPoint).length() > SHORE_MOVED_M || mShoreRegion != region->getRegionID())
    {
        mWaveSeeded = false;   // a new stretch of water: its waves are measured afresh
    }
    mSeaPoint = best_sea;
    mShoreRegion = region->getRegionID();
    mHaveShore = true;
}

void WolfNatureSounds::sampleWaves(F64 now)
{
    if (now < mNextWave) return;
    mNextWave = now + WAVE_S;
    if (!mHaveShore || !WolfBoatRock::instanceExists())
    {
        mHs = 0.f;
        return;
    }
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(mSeaPoint);
    if (!r) r = gAgent.getRegion();
    if (!r) return;
    const F32 h = WolfBoatRock::instance().drawnWaveHeight(r, mSeaPoint.mV[VX], mSeaPoint.mV[VY]);
    if (!std::isfinite(h)) return;
    if (!mWaveSeeded)
    {
        mWaveMean = h;
        mWaveMean2 = h * h;
        mWaveSeeded = true;
        mWaveSamples = 0;
    }
    ++mWaveSamples;
    const F32 k = 1.f - expf(-(F32)WAVE_S / WAVE_TAU_S);
    mWaveMean += (h - mWaveMean) * k;
    mWaveMean2 += (h * h - mWaveMean2) * k;
    mHs = 4.f * sqrtf(llmax(0.f, mWaveMean2 - mWaveMean * mWaveMean));
}

F32 WolfNatureSounds::stillWaterDistance(const LLVector3& cam, bool& inside) const
{
    // A builder's water: the plane's footprint, its surface at the top face and its body getBoundedWaterDepth() deep
    // (wolfswimbubbles.cpp in_plane, the same frame: fswolfwater.cpp:367-371, wolfterrainpaint.cpp rebuildWater).
    inside = false;
    F32 best = 1e9f;
    for (const LLVOWater* w : mPlanes)
    {
        if (!w || w->isDead()) continue;
        const LLVector3 half = w->getScale() * 0.5f;
        const LLVector3 local = (cam - w->getPositionAgent()) * ~w->getRotation();
        const F32 dx = llmax(0.f, fabsf(local.mV[VX]) - half.mV[VX]);
        const F32 dy = llmax(0.f, fabsf(local.mV[VY]) - half.mV[VY]);
        const F32 top = -half.mV[VZ];
        const F32 depth = w->getBoundedWaterDepth();
        if (dx == 0.f && dy == 0.f && depth > 0.f && local.mV[VZ] <= top && local.mV[VZ] >= top - depth)
        {
            inside = true;
            return 0.f;
        }
        const F32 dz = llmax(0.f, local.mV[VZ] - top);
        best = llmin(best, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    return best;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the levels
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfNatureSounds::assess(F64 now)
{
    F32 target[LAYER_COUNT] = { 0.f, 0.f, 0.f, 0.f, 0.f };
    bool fast = false;
    LLViewerRegion* region = gAgent.getRegion();
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerNatureSounds", true);
    const bool wanted = region && on && WolfGrid::showsWolfRegionWeather()
                     && WolfRegionWeather::instanceExists() && WolfRegionWeather::instance().soundsAllowed();
    if (!on) fast = true;   // the resident's own switch is near instant
    if (wanted)
    {
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        mPlanes.clear();
        if (FSWolfWater::instanceExists()) FSWolfWater::instance().appendPlanes(mPlanes);
        if (WolfTerrainPaint::instanceExists()) WolfTerrainPaint::instance().appendWaterPlanes(mPlanes);
        if ((S32)mPlanes.size() > MAX_PLANES) mPlanes.resize(MAX_PLANES);
        bool in_plane = false;
        const F32 plane_d = stillWaterDistance(cam, in_plane);
        // under the water (the sea, or a builder's pool) everything is muted; a sea tunnel or a hull is dry air
        const bool under = in_plane
            || (LLViewerCamera::getInstance()->cameraUnderWater() && !WolfDryVolumes::instance().cameraInside());
        if (under)
        {
            fast = true;
        }
        else
        {
            if (now >= mNextScan)
            {
                mNextScan = now + SCAN_S;
                scanShore(region, cam);
            }
            const F32 roof = (WolfWeather::instanceExists() && WolfWeather::instance().cameraUnderRoof()) ? ROOF : 1.f;

            // the night insects
            const F32 climate = WolfRegionWeather::instance().soundsInsects();
            if (climate > 0.f)
            {
                const F32 night = 1.f - smooth(NIGHT_FULL_DEG, NIGHT_NONE_DEG, sun_elevation_deg());
                // the region's wind in m/s: LLWind::getVelocity adds WIND_SCALE_HACK (llwind.h:38; wolfflight.cpp:511-513)
                const LLVector3 local = cam - region->getOriginAgent();
                const F32 wind = (region->mWind.getVelocity(local) / WIND_SCALE_HACK).length();
                const F32 calm = 1.f - smooth(WIND_QUIET_MS, WIND_NONE_MS, wind);
                mWind = wind;
                F32 water = 0.f;
                water_height(cam, water);
                const F32 ground = llmax(LLWorld::getInstance()->resolveLandHeightAgent(cam), water);
                const F32 low = 1.f - smooth(HIGH_FROM_M, HIGH_NONE_M, cam.mV[VZ] - ground);
                target[INSECTS] = full_gain(INSECTS) * climate * night * insect_weather() * calm * low * roof;
            }

            // the water: lapping where it is still, surf as the waves grow
            const bool shore = mHaveShore && mWaveSeeded && mWaveSamples >= WAVE_WARM_SAMPLES;
            const F32 hs = shore ? mHs : 0.f;
            const F32 w_lap = 1.f - smooth(HS_LAP_TO_A, HS_LAP_TO_B, hs);
            const F32 w_medium_up = smooth(HS_MEDIUM_A, HS_MEDIUM_B, hs);
            const F32 w_big = smooth(HS_BIG_A, HS_BIG_B, hs);
            const F32 w_small = (1.f - w_lap) * (1.f - w_medium_up);
            const F32 w_medium = w_medium_up * (1.f - w_big);
            F32 lap = 0.f;
            if (shore)
            {
                const F32 d = mShoreDist;
                const F32 near_lap = 1.f - llclamp(d / LAP_RANGE_M, 0.f, 1.f);
                lap = w_lap * near_lap * near_lap;
                const F32 range = SURF_RANGE_M + SURF_RANGE_MEDIUM_M * w_medium + SURF_RANGE_BIG_M * w_big;
                const F32 near_surf = powf(1.f - llclamp(d / range, 0.f, 1.f), 1.5f);
                // equal power across the tiers: the square roots of weights that sum to one
                target[SURF_SMALL] = full_gain(SURF_SMALL) * sqrtf(w_small) * near_surf * roof;
                target[SURF_MEDIUM] = full_gain(SURF_MEDIUM) * sqrtf(w_medium) * near_surf * roof;
                target[SURF_BIG] = full_gain(SURF_BIG) * sqrtf(w_big) * near_surf * roof;
            }
            const F32 near_plane = 1.f - llclamp(plane_d / LAP_RANGE_M, 0.f, 1.f);
            lap = llmax(lap, near_plane * near_plane);
            target[LAPPING] = full_gain(LAPPING) * sqrtf(lap) * roof;

            if (now >= mNextLog)
            {
                mNextLog = now + LOG_S;
                LL_INFOS("WolfNatureSounds") << "insects " << target[INSECTS] << " (climate " << climate << ", sun "
                                             << sun_elevation_deg() << " deg, wind " << mWind << " m/s); shore " << (mHaveShore ? mShoreDist : -1.f)
                                             << " m, Hs " << hs << " m; lapping " << target[LAPPING] << ", surf "
                                             << target[SURF_SMALL] << " / " << target[SURF_MEDIUM] << " / " << target[SURF_BIG]
                                             << "; builders' water " << (plane_d < 1e8f ? plane_d : -1.f) << " m" << LL_ENDL;
            }
        }
    }
    else
    {
        mHaveShore = false;
        mWaveSeeded = false;
        mNextLog = 0.0;
    }
    for (S32 i = 0; i < LAYER_COUNT; ++i)
    {
        mLayers[i].mTarget = llclamp(target[i] * WolfSoundEffects::level(WolfSoundEffects::NATURE), 0.f, 1.f);   // [SOUND EFFECTS 2026-11-06/]
        mLayers[i].mFast = fast;
        if (mLayers[i].mTarget > 0.f) mActive = true;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// playing (wolfweathersound.cpp updateLayer / stopSource, the same engine calls)
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfNatureSounds::stopLayer(Layer& l)
{
    if (gAudiop && l.mSource.notNull())
    {
        if (LLAudioSource* src = gAudiop->findAudioSource(l.mSource))
        {
            src->setLoop(false);
            src->setGain(0.f);
            src->play(LLUUID::null);      // llaudiosourcevo.cpp:207 - the "stop this" call
        }
    }
    l.mSource.setNull();
}

void WolfNatureSounds::stopAll()
{
    for (Layer& l : mLayers)
    {
        stopLayer(l);
        l.mGain = 0.f;
        l.mTarget = 0.f;
    }
    mActive = false;
}

void WolfNatureSounds::updateLayer(S32 i, F32 dt, F64 now)
{
    Layer& l = mLayers[i];
    const F32 tau = l.mFast ? TAU_FAST_S : TAU_S;
    l.mGain += (l.mTarget - l.mGain) * (1.f - expf(-dt / tau));
    if (l.mTarget <= 0.f && l.mGain < SILENT)
    {
        l.mGain = 0.f;
        stopLayer(l);
        return;
    }
    if (l.mGain < SILENT) return;   // fading in: nothing to start until it is audible
    LLAudioSource* src = l.mSource.notNull() ? gAudiop->findAudioSource(l.mSource) : nullptr;
    if (l.mSource.notNull() && !src)
    {
        l.mSource.setNull();          // the engine reclaimed it: start it again shortly
        l.mRetryAt = now + 0.5;
    }
    if (!src)
    {
        if (now < l.mRetryAt) return;
        // decoded into the sound cache by the rain's own worker the first time it is asked for (wolfweathersound.h)
        if (!WolfWeatherSound::instance().recordingReady(RECORDINGS[i])) return;
        const LLUUID source_id = LLUUID::generateNewID();
        // Source: wolfweathersound.cpp updateLayer - our own source id, so it can be found, looped, re-levelled and
        // stopped; a zero position is the engine's 2D path (llaudioengine.cpp:826), so the stereo recording stays stereo.
        gAudiop->triggerSound(LLUUID(RECORDINGS[i].mUuid), gAgentID, llmax(l.mGain, START_GAIN), LLAudioEngine::AUDIO_TYPE_AMBIENT,
                              LLVector3d::zero, LLUUID::null, source_id);
        src = gAudiop->findAudioSource(source_id);
        if (!src)
        {
            l.mRetryAt = now + 2.0;   // muted, or no channel: do not try every frame
            return;
        }
        src->setLoop(true);
        l.mSource = source_id;
        LL_INFOS("WolfNatureSounds") << "playing " << LAYER_NAMES[i] << LL_ENDL;
    }
    src->setGain(llclamp(llmax(l.mGain, START_GAIN), 0.f, 1.f));
}

void WolfNatureSounds::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = (mLastIdle > 0.0) ? (F32)llclamp(now - mLastIdle, 0.0, 0.25) : 0.f;
    mLastIdle = now;
    if (!gAudiop)
    {
        mActive = false;
        return;
    }
    // The agent frame moves on a region crossing or a rebase (wolfbirds.cpp idle, the same rule): the stored point moves too.
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        if (!mLastOrigin.isExactlyZero()) mSeaPoint += LLVector3(mLastOrigin - origin);
        mLastOrigin = origin;
    }
    if (now >= mNextAssess)
    {
        mNextAssess = now + ASSESS_S;
        assess(now);
    }
    // the waves are measured whenever a shore is known (sampleWaves returns at once otherwise), not only while something
    // plays: the shore's own sounds wait for these samples (review 2026-11-03: by day, with no insects, they never began)
    sampleWaves(now);
    if (!mActive) return;
    bool any = false;
    for (S32 i = 0; i < LAYER_COUNT; ++i)
    {
        updateLayer(i, dt, now);
        if (mLayers[i].mTarget > 0.f || mLayers[i].mSource.notNull()) any = true;
    }
    if (!any) stopAll();
}
