/**
 * @file wolfweather.cpp
 * @brief WolfViewer: weather — rain and snow, from the menu or from "wolfrain" / "wolfsnow" prims.
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

#include "wolfweather.h"

#include <cmath>           // cosf / sinf for the wind direction

#include "llagent.h"
#include "llframetimer.h"
#include "llvector4a.h"
#include "llworld.h"
#include "pipeline.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerpartsim.h"
#include "llviewertexture.h"
#include "llvoavatar.h"        // LLViewerPartSource holds an LLPointer<LLVOAvatar>: the destructor needs the complete type
#include "wolfobjectprops.h"
#include "wolfregionweather.h"
#include "wolfweathersound.h"

const std::string WolfWeather::KEYWORD_RAIN("wolfrain");
const std::string WolfWeather::KEYWORD_SNOW("wolfsnow");
const std::string WolfWeather::KEYWORD_CLEAR("wolfclear");

namespace
{
    // Source: wolfstorm environment_manager.js createRainSystem / createSnowSystem — the same
    // shapes, scaled to what the viewer's particle cap (RenderMaxPartCount, default 4096)
    // can carry: a few hundred rain streaks or snow flakes alive at once round the camera.
    // Four levels (Paul: "create 4 levels of rain and 4 levels of snow"): spawn rate per
    // second and fall speed, index 1..4 = light, moderate (default), heavy, torrential / blizzard.
    constexpr F32 RAIN_RATE_BY_LEVEL[5]  = { 0.f, 220.f, 500.f, 900.f, 1500.f };
    constexpr F32 RAIN_SPEED_BY_LEVEL[5] = { 0.f, 11.f, 14.f, 17.f, 20.f };
    constexpr F32 SNOW_RATE_BY_LEVEL[5]  = { 0.f, 70.f, 160.f, 320.f, 560.f };
    constexpr F32 SNOW_SPEED_BY_LEVEL[5] = { 0.f, 1.1f, 1.6f, 2.1f, 3.0f };
    constexpr F32 RAIN_RATE_PER_S   = 700.f;   // (kept for reference; the tables above are used)
    constexpr F32 RAIN_BOX_XY_M     = 28.f;    // half-width of the box round the camera
    constexpr F32 RAIN_TOP_M        = 18.f;    // spawn height above the camera
    constexpr F32 RAIN_BOTTOM_M     = 6.f;
    constexpr F32 RAIN_SPEED_MPS    = 16.f;
    constexpr F32 RAIN_AGE_S        = 1.6f;    // ~26 m of fall

    constexpr F32 SNOW_RATE_PER_S   = 260.f;
    constexpr F32 SNOW_BOX_XY_M     = 24.f;
    constexpr F32 SNOW_TOP_M        = 16.f;
    constexpr F32 SNOW_BOTTOM_M     = 2.f;
    constexpr F32 SNOW_SPEED_MPS    = 1.6f;
    constexpr F32 SNOW_AGE_S        = 12.f;

    // Both keyword tests take the store's already lower-cased description
    // (WolfObjectProps::Props::mDescriptionLower); no per-prim copy per sweep.
    bool matches(const std::string& description_lower, const std::string& keyword)
    {
        if (description_lower.empty()) return false;
        return description_lower.find(keyword) != std::string::npos;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfWeatherPartSource
// ═══════════════════════════════════════════════════════════════════════════════════════

// LL_PART_SOURCE_NULL: the simulation only reads a source's type nowhere and its owner for
// mute checks against a source OBJECT this source does not have (llviewerpartsim.cpp:770).
WolfWeatherPartSource::WolfWeatherPartSource() : LLViewerPartSource(LL_PART_SOURCE_NULL) {}

// Source: LLViewerPartSourceSpiral::update for the emitter shape (image fallback, the
// shouldAddPart() cap, LLViewerPart::init, addPart). The simulation integrates mVelocity and
// mAccel itself (llviewerpartsim.cpp:350-352) and applies the region wind to LL_PART_WIND_MASK
// particles (:317-318), so snow needs no callback.
// THE ROOF TEST (Paul: "rain and snow must not go inside houses or buildings"). A grid of
// LANDING_N x LANDING_N cells over the weather box round the camera; for each cell one ray is
// cast straight DOWN from above the box through the world (gPipeline.lineSegmentIntersectInWorld,
// the pick's own object raycast), and the higher of that hit and the land
// (LLWorld::resolveLandHeightAgent) is where weather in that cell lands. A particle is only
// spawned above its cell's landing height and is given exactly the lifetime it needs to fall
// to it, so nothing ever falls through a roof and the room under it stays dry. A few rays per
// frame (LANDING_RAYS_PER_FRAME) so a full refresh takes well under a second and no frame pays
// for all of it. Source: wolfstorm environment_manager.js _updateLanding / _landingZ.
void WolfWeatherPartSource::updateLanding(const LLVector3& cam, F32 half_xy, F32 top)
{
    if (!mLandingInit)
    {
        for (F32& z : mLandingZ) z = -1e9f;
        mLandingInit = true;
    }
    const F32 cell = (2.f * half_xy) / (F32)LANDING_N;
    for (S32 r = 0; r < LANDING_RAYS_PER_FRAME; ++r)
    {
        const S32 k = mLandingNext;
        mLandingNext = (mLandingNext + 1) % (LANDING_N * LANDING_N);
        const S32 cx = k % LANDING_N, cy = k / LANDING_N;
        const F32 wx = cam.mV[VX] - half_xy + ((F32)cx + 0.5f) * cell;
        const F32 wy = cam.mV[VY] - half_xy + ((F32)cy + 0.5f) * cell;
        F32 land_z = -1e9f;
        const LLVector3 probe(wx, wy, cam.mV[VZ]);
        if (gAgent.getRegion())
        {
            land_z = LLWorld::getInstance()->resolveLandHeightAgent(probe);
        }
        LLVector4a start, end, hit;
        const LLVector3 s3(wx, wy, cam.mV[VZ] + top + 20.f), e3(wx, wy, cam.mV[VZ] - 60.f);
        start.load3(s3.mV);
        end.load3(e3.mV);
        // pick_transparent false: glass roofs do not stop rain in this test either way, but a
        // transparent prim is most often a window, and rain must not fall THROUGH a roof that
        // happens to carry alpha. pick_unselectable true: builds are often locked / no-select.
        if (gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, NULL, NULL, NULL, &hit, NULL, NULL, NULL))
        {
            const F32 hz = hit.getF32ptr()[2];
            if (hz > land_z) land_z = hz;
        }
        mLandingZ[k] = land_z;
    }
}

F32 WolfWeatherPartSource::landingZ(const LLVector3& cam, F32 x, F32 y, F32 half_xy) const
{
    if (!mLandingInit) return -1e9f;
    const F32 cell = (2.f * half_xy) / (F32)LANDING_N;
    const S32 cx = (S32)floorf((x - (cam.mV[VX] - half_xy)) / cell);
    const S32 cy = (S32)floorf((y - (cam.mV[VY] - half_xy)) / cell);
    if (cx < 0 || cy < 0 || cx >= LANDING_N || cy >= LANDING_N) return -1e9f;
    return mLandingZ[cy * LANDING_N + cx];
}

/**
 * [WEATHER 2026-09-12] The wind the weather leans on, in metres/second, from the profile's
 * movement speed (hundredths). The direction turns slowly — about a full turn every two
 * minutes — so rain does not fall in one fixed diagonal for the whole session.
 * Source: wolfstorm environment_manager.js _weatherWind().
 */
LLVector3 WolfWeatherPartSource::wind() const
{
    const F32 mps = (F32)mProfile.mMoveSpeed / 100.f;
    const F32 a = mAge * 0.05f;
    return LLVector3(cosf(a) * mps, sinf(a) * mps, 0.f);
}

void WolfWeatherPartSource::update(const F32 dt)
{
    if (mMode == NONE) return;
    mAge += dt;
    if (!mImagep)
    {
        mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    mPosAgent = cam;
    updateLanding(cam, mMode == RAIN ? RAIN_BOX_XY_M : SNOW_BOX_XY_M, mMode == RAIN ? RAIN_TOP_M : SNOW_TOP_M);
    // The level's rate at the profile's density. The viewer's own particle cap
    // (RenderMaxPartCount) is still the ceiling — shouldAddPart() below refuses when it is near
    // — so a 300% blizzard sheds particles rather than the rest of the scene doing so.
    const F32 base_rate = (mMode == RAIN) ? RAIN_RATE_BY_LEVEL[mLevel] : SNOW_RATE_BY_LEVEL[mLevel];
    const F32 rate = base_rate * ((F32)mProfile.mDensity / 100.f);
    mCarry += rate * llmin(dt, 0.1f);
    S32 n = (S32)mCarry;
    mCarry -= (F32)n;
    // Never more than a frame's fair share when the cap is near (shouldAddPart is probabilistic).
    n = llmin(n, 120);
    for (S32 i = 0; i < n; ++i)
    {
        if (!LLViewerPartSim::getInstance()->shouldAddPart()) break;
        emit(cam);
    }
}

void WolfWeatherPartSource::emit(const LLVector3& cam)
{
    LLViewerPart* part = new LLViewerPart();
    part->init(this, mImagep, NULL);
    part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
    part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
    part->mStartGlow = 0.f;
    part->mEndGlow = 0.f;
    part->mGlow = LLColor4U(0, 0, 0, 0);
    part->mLastUpdateTime = 0.f;
    const F32 size_k = (F32)mProfile.mSize / 100.f;
    const F32 vel_k = (F32)mProfile.mVelocity / 100.f;
    // The profile's colour: the kind's neutral colour moved towards the chosen tint, scaled by
    // brightness. WolfWeatherProfile::drawColor is the one place that arithmetic lives, so the
    // tab's swatch and the falling rain cannot drift apart.
    const LLColor4 draw = mProfile.drawColor();
    const LLVector3 w = wind();
    if (mMode == RAIN)
    {
        part->mPosAgent = cam + LLVector3(ll_frand(2.f * RAIN_BOX_XY_M) - RAIN_BOX_XY_M,
                                          ll_frand(2.f * RAIN_BOX_XY_M) - RAIN_BOX_XY_M,
                                          RAIN_BOTTOM_M + ll_frand(RAIN_TOP_M - RAIN_BOTTOM_M));
        // THE GOLDEN RULE (Paul: "rain and snow must never go into houses"). Nothing spawns
        // under a roof; a drop above one lives exactly long enough to REACH it and no longer.
        const F32 land = landingZ(cam, part->mPosAgent.mV[VX], part->mPosAgent.mV[VY], RAIN_BOX_XY_M);
        if (part->mPosAgent.mV[VZ] <= land) { delete part; return; }
        // A near-vertical fall leaning on the wind, every drop at its own speed.
        const F32 speed = RAIN_SPEED_BY_LEVEL[mLevel] * vel_k * (0.85f + ll_frand(0.3f));
        part->mVelocity = LLVector3(w.mV[VX] + ll_frand(1.6f) - 0.8f,
                                    w.mV[VY] + ll_frand(1.6f) - 0.8f,
                                    -speed);
        part->mAccel = LLVector3::zero;
        part->mMaxAge = llmin(RAIN_AGE_S, (part->mPosAgent.mV[VZ] - land) / speed);
        // A streak, not a dot: oriented along the velocity. 7 cm wide — the first cut's 2.5 cm
        // was below a pixel at any distance and the rain was invisible (Paul: "snowing worked
        // ... raining did not"; snow is 9 cm). Size scales both, so heavier rain is fatter as
        // well as longer.
        part->mScale.set(0.07f * size_k, 0.8f * size_k);
        part->mStartScale = part->mScale;
        part->mEndScale = part->mScale;
        part->mStartColor = LLColor4(draw.mV[VRED], draw.mV[VGREEN], draw.mV[VBLUE], 0.65f);
        part->mEndColor = LLColor4(draw.mV[VRED], draw.mV[VGREEN], draw.mV[VBLUE], 0.5f);
        part->mColor = part->mStartColor;
        // [2026-09-12] NOT emissive. LL_PART_EMISSIVE_MASK means "instead of being lit"
        // (llpartdata.h:115), so rain glowed its own colour at midnight and under cover, which
        // reads as falling light rather than water. Lit, it takes the sky and the region's own
        // lighting like everything else does.
        part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK | LLViewerPart::LL_PART_FOLLOW_VELOCITY_MASK;
    }
    else
    {
        part->mPosAgent = cam + LLVector3(ll_frand(2.f * SNOW_BOX_XY_M) - SNOW_BOX_XY_M,
                                          ll_frand(2.f * SNOW_BOX_XY_M) - SNOW_BOX_XY_M,
                                          SNOW_BOTTOM_M + ll_frand(SNOW_TOP_M - SNOW_BOTTOM_M));
        // THE GOLDEN RULE again: no flake is born below the roof above it, and none outlives
        // the fall to its own landing height.
        const F32 land = landingZ(cam, part->mPosAgent.mV[VX], part->mPosAgent.mV[VY], SNOW_BOX_XY_M);
        if (part->mPosAgent.mV[VZ] <= land) { delete part; return; }
        // Slow, each flake its own speed and a sideways drift on the profile's wind; the
        // LL_PART_WIND_MASK below lets the region wind push it as well.
        const F32 speed = SNOW_SPEED_BY_LEVEL[mLevel] * vel_k * (0.6f + ll_frand(0.8f));
        part->mVelocity = LLVector3(w.mV[VX] + ll_frand(1.0f) - 0.5f,
                                    w.mV[VY] + ll_frand(1.0f) - 0.5f,
                                    -speed);
        part->mAccel = LLVector3::zero;
        part->mMaxAge = llmin(SNOW_AGE_S, (part->mPosAgent.mV[VZ] - land) / speed);
        part->mScale.set(0.09f * size_k, 0.09f * size_k);
        part->mStartScale = part->mScale;
        part->mEndScale = part->mScale;
        part->mStartColor = LLColor4(draw.mV[VRED], draw.mV[VGREEN], draw.mV[VBLUE], 0.95f);
        part->mEndColor = LLColor4(draw.mV[VRED], draw.mV[VGREEN], draw.mV[VBLUE], 0.85f);
        part->mColor = part->mStartColor;
        // [2026-09-12] NOT emissive, for the same reason as the rain above: snow that lights
        // itself is glowing white confetti at night instead of snow.
        part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK | LLViewerPart::LL_PART_WIND_MASK;
    }
    part->mParameter = 0.f;
    LLViewerPartSim::getInstance()->addPart(part);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfWeather
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfWeather::WolfWeather() {}
WolfWeather::~WolfWeather() {}

void WolfWeather::toggle(Mode m)
{
    mUser = (mUser == m) ? Mode::NONE : m;
    apply();
}

void WolfWeather::setLevel(Mode m, S32 level)
{
    level = llclamp(level, 1, 4);
    if (m == Mode::RAIN) mUserRainLevel = level; else if (m == Mode::SNOW) mUserSnowLevel = level;
    mUser = m;
    apply();
}

void WolfWeather::clear()
{
    mUser = Mode::NONE;
    apply();
}

/**
 * THE ONE PLACE THAT DECIDES WHAT THE SKY DOES. [WEATHER 2026-09-12]
 *
 * Most specific first:
 *   0. an unsaved edit in About Land > Weather (setPreview) — not weather, the EDITOR showing
 *      its own author what they are about to save, and seen by nobody else
 *   1. a parcel prim's description (wolfrain / wolfsnow) — a script beats everything real
 *   2. the REGION setting from About Land > Weather (wolfregionweather.cpp)
 *   3. the resident's own Weather menu choice
 *
 * A region row of kind "clear" is a deliberate "no weather here" and outranks the menu; a
 * region with NO ROW at all has no opinion and the menu still applies. Those are different
 * things, and conflating them would make "turn the rain off for everyone" impossible.
 *
 * THE REGION'S LOOK is the house style even when the region is not forcing the weather:
 * brightness, colour, movement speed, density/velocity/size and the sound layers are the estate
 * owner's art direction for this place, and a resident who turns their own rain on in it should
 * get that rain. Only WHETHER it rains and HOW HARD come from the precedence above.
 *
 * One particle source for the session, made when first needed; setDead() when nothing falls so
 * the simulation drops it, and a fresh one when the weather comes back.
 */
// static
bool WolfWeather::enabled()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfWeatherEnabled", true);
    return on;
}

void WolfWeather::toggleEnabled()
{
    gSavedSettings.setBOOL("WolfWeatherEnabled", !enabled());
    apply();
}

void WolfWeather::apply()
{
    // THE SWITCH FIRST. Off is off: no region weather, no parcel weather, no menu choice, and
    // no preview — see WolfWeather::enabled().
    if (!enabled())
    {
        mActive = WolfWeatherProfile();
        mActiveSource = "off";
        if (mSource.notNull())
        {
            mSource->setMode(WolfWeatherPartSource::NONE);
            mSource->setDead();
            mSource = nullptr;
        }
        WolfWeatherSound::instance().apply(mActive);
        return;
    }

    WolfRegionWeather& rw = WolfRegionWeather::instance();
    const bool on_grid = rw.onGrid();
    WolfWeatherProfile prof = on_grid ? rw.stored() : WolfWeatherProfile();

    if (mHavePreview)
    {
        prof = mPreview;
        mActiveSource = "preview";
    }
    else
    {
        WolfWeatherProfile region;
        const bool region_forces = rw.forcedProfile(region);
        if (mForcedFound)
        {
            // A parcel prim's description carries the WHOLE profile, so its look and sound come
            // with it rather than being taken from the region underneath.
            prof = mForcedProfile;
            mActiveSource = "parcel";
        }
        else if (region_forces)
        {
            prof.mKind  = region.mKind;
            prof.mLevel = region.mLevel;
            mActiveSource = "region";
        }
        else
        {
            prof.mKind  = (mUser == Mode::RAIN) ? WolfWeatherProfile::RAIN
                        : (mUser == Mode::SNOW) ? WolfWeatherProfile::SNOW
                                                : WolfWeatherProfile::CLEAR;
            prof.mLevel = (mUser == Mode::RAIN) ? mUserRainLevel : mUserSnowLevel;
            mActiveSource = "menu";
        }
        prof.mEnabled = true;
        // A region tint is chosen for ONE kind. Falling back to the other kind's neutral colour
        // stops a snow tint turning a resident's rain white, and vice versa.
        if (!on_grid || rw.stored().mKind != prof.mKind)
        {
            prof.mTint = WolfWeatherProfile::neutralColor(prof.mKind);
        }
    }
    prof.clampAll();
    mActive = prof;

    const Mode want = modeOf(prof.mKind);
    if (want == Mode::NONE)
    {
        if (mSource.notNull())
        {
            mSource->setMode(WolfWeatherPartSource::NONE);
            mSource->setDead();
            mSource = nullptr;
        }
        WolfWeatherSound::instance().apply(prof);
        return;
    }
    if (mSource.isNull() || mSource->isDead())
    {
        mSource = new WolfWeatherPartSource();
        LLViewerPartSim::getInstance()->addPartSource(mSource);
    }
    mSource->setMode(want);
    mSource->setLevel(prof.mLevel);
    mSource->setProfile(prof);
    WolfWeatherSound::instance().apply(prof);
}

std::string WolfWeather::overriddenBy() const
{
    if (mForcedFound) return "parcel";
    WolfWeatherProfile ignored;
    if (WolfRegionWeather::instance().forcedProfile(ignored)) return "region";
    return std::string();
}

void WolfWeather::onRegionWeatherChanged()
{
    apply();
}

void WolfWeather::setPreview(const WolfWeatherProfile* p)
{
    mHavePreview = (p != nullptr);
    if (p) mPreview = *p;
    apply();
}

void WolfWeather::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextSweep)
    {
        mNextSweep = now + SWEEP_INTERVAL_SECS;
        sweep();
    }
    // [WEATHER 2026-09-12] The region's own answer, and the sound that goes with the sky.
    WolfRegionWeather::instance().idle();
    WolfWeatherSound::instance().idle();
}

// Source: fswolfwater.cpp sweep — every prim in draw distance is handed to the harvester,
// answered descriptions are tested for the keyword. Here the prim must also stand in the
// agent's parcel (LLViewerParcelMgr::inAgentParcel, the parcel bitmap test the sim gave us).
void WolfWeather::sweep()
{
    if (!gAgent.getRegion()) return;
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    const F32 range_sq = (F32)draw_distance * (F32)draw_distance;
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();
    WolfObjectProps& props = WolfObjectProps::instance();
    LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();

    // [WEATHER 2026-09-12] A prim in the parcel now carries a WHOLE profile, not just a level:
    //   wolfrain3 bright=60 col=#8fb4e6 move=180 dens=170 amb=thunder near=heavy vol=85
    // The strongest keyword in the parcel wins (clear over rain over snow — see
    // WolfWeatherProfile::fromDescription), and at equal kind the higher level wins, so a
    // builder can put a heavier prim in a corner without fighting the others.
    bool found = false;
    WolfWeatherProfile parcel_profile;
    S32 n_in_range = 0, n_known = 0;
    auto rank = [](WolfWeatherProfile::Kind k) -> S32
    {
        return k == WolfWeatherProfile::CLEAR ? 3 : (k == WolfWeatherProfile::RAIN ? 2 : 1);
    };
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp || objectp->isDead() || objectp->isAvatar()) continue;
        if (objectp->isHUDAttachment() || objectp->isAttachment()) continue;
        if (objectp->getPCode() != LL_PCODE_VOLUME) continue;
        if ((objectp->getPositionAgent() - camera_pos).lengthSquared() > range_sq) continue;
        ++n_in_range;
        props.want(objectp);
        const WolfObjectProps::Props* known = props.get(objectp->getID());
        if (!known) continue;
        ++n_known;
        if (!matches(known->mDescriptionLower, KEYWORD_RAIN)
            && !matches(known->mDescriptionLower, KEYWORD_SNOW)
            && !matches(known->mDescriptionLower, KEYWORD_CLEAR)) continue;
        if (!parcels->inAgentParcel(objectp->getPositionGlobal())) continue;
        WolfWeatherProfile candidate;
        if (!candidate.fromDescription(known->mDescriptionLower)) continue;
        if (!found
            || rank(candidate.mKind) > rank(parcel_profile.mKind)
            || (candidate.mKind == parcel_profile.mKind && candidate.mLevel > parcel_profile.mLevel))
        {
            parcel_profile = candidate;
        }
        found = true;
    }
    const Mode want = found ? modeOf(parcel_profile.mKind) : Mode::NONE;
    const S32 want_level = found ? parcel_profile.mLevel : 2;
    // A parcel prim's own look/sound settings travel with it, so re-applying on ANY change —
    // not just kind and level — is what makes "dens=200" in a description do anything.
    const bool changed = (want != mForced) || (want_level != mForcedLevel)
                       || (found && !(parcel_profile == mForcedProfile))
                       || (!found && mForcedFound);
    if (changed)
    {
        mForced = want;
        mForcedLevel = want_level;
        mForcedProfile = parcel_profile;
        mForcedFound = found;
        LL_INFOS("WolfWeather") << "parcel weather: " << (want == Mode::RAIN ? "rain" : want == Mode::SNOW ? "snow" : "none")
                                << " (user choice " << (mUser == Mode::RAIN ? "rain" : mUser == Mode::SNOW ? "snow" : "none") << ")" << LL_ENDL;
        apply();
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (effective() != Mode::NONE && now >= mNextStats)
    {
        mNextStats = now + STATS_INTERVAL_SECS;
        LL_INFOS("WolfWeather") << (effective() == Mode::RAIN ? "rain" : "snow") << (mForced != Mode::NONE ? " (parcel)" : " (menu)")
                                << ", " << n_in_range << " prims in range, " << n_known << " descriptions known" << LL_ENDL;
    }
}
