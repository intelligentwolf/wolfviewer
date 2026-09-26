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
#include "llenvironment.h"   // <WolfViewer 2026-09-18/> auroraAmount(): the sun
#include "llvovolume.h"      // <WolfViewer 2026-09-20/> sWolfRoofProbe

#include <cmath>           // cosf / sinf for the wind direction

#include "llagent.h"
#include "llframetimer.h"
#include "llimagegl.h"       // <WolfViewer 2026-09-18/> the roof grid texture
#include "llglslshader.h"
#include "llshadermgr.h"     // <WolfViewer 2026-09-18/> WOLF_SHELTER_MAP
#include "llrender.h"
#include "llviewerregion.h"
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
#include "wolfgrid.h"            // <WolfViewer 2026-09-26/> snow cover region settle
#include "wolfobjectprops.h"
#include "wolfregionweather.h"
#include "wolfweathersound.h"
#include "wolflightning.h"      // [LIGHTNING 2026-09-13]

const std::string WolfWeather::KEYWORD_RAIN("wolfrain");
const std::string WolfWeather::KEYWORD_SNOW("wolfsnow");
const std::string WolfWeather::KEYWORD_CLEAR("wolfclear");

namespace
{
    // Source: wolfstorm environment_manager.js createRainSystem / createSnowSystem — the same
    // shapes, scaled to what the viewer's particle cap (RenderMaxPartCount, default 16384)
    // can carry: precipitation alive around the camera shares the scene's particle budget.
    // Four levels (Paul: "create 4 levels of rain and 4 levels of snow"): spawn rate per
    // second and fall speed, index 1..4 = light, moderate (default), heavy, torrential / blizzard.
    constexpr F32 RAIN_RATE_BY_LEVEL[5]  = { 0.f, 220.f, 500.f, 900.f, 1500.f };
    constexpr F32 RAIN_SPEED_BY_LEVEL[5] = { 0.f, 11.f, 14.f, 17.f, 20.f };
    constexpr F32 SNOW_RATE_BY_LEVEL[5]  = { 0.f, 70.f, 160.f, 320.f, 2240.f };
    constexpr F32 SNOW_SPEED_BY_LEVEL[5] = { 0.f, 1.1f, 1.6f, 2.1f, 3.0f };
    constexpr F32 RAIN_RATE_PER_S   = 700.f;   // (kept for reference; the tables above are used)
    constexpr F32 RAIN_BOX_XY_M     = 28.f;    // half-width of the box round the camera
    // <WolfViewer 2026-09-20> how far above the camera a roof probe starts: any building or
    // sky platform up to this height over your head is a roof. wolfstorm environment_manager.js same.
    constexpr F32 ROOF_PROBE_ABOVE_M = 300.f;
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
    // [2026-09-13] THE CAMERA'S OWN CELL IS RE-RAYED EVERY FRAME, before the round-robin and in
    // addition to it. The round-robin refreshes 6 of 144 cells a frame, so a full sweep is 24
    // frames — walk through a door and the rain kept falling on you until the grid caught up,
    // because your cell still held the height it had outdoors. Whatever is above your head is
    // now never more than one frame stale, and stepping under a roof stops the weather on you
    // at once. Source: wolfstorm environment_manager.js _updateLanding (cameraCell), the web
    // viewer's own fix, ported for parity (Paul: "NO WEATHER CAN ENTER A BUILDING").
    const S32 camera_cell = (LANDING_N / 2) * LANDING_N + (LANDING_N / 2);
    for (S32 r = -1; r < LANDING_RAYS_PER_FRAME; ++r)
    {
        S32 k;
        if (r < 0)
        {
            k = camera_cell;
        }
        else
        {
            k = mLandingNext;
            mLandingNext = (mLandingNext + 1) % (LANDING_N * LANDING_N);
        }
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
        // <WolfViewer 2026-09-20> The ray starts ROOF_PROBE_ABOVE_M above the camera, not just
        // above the weather box (top + 20 m, i.e. ~38 m): a mesh hall or a sculpted tower
        // roof higher than that was never on the ray, the ray hit the floor instead, and it
        // rained indoors (Paul: "snow is falling inside buildings and rain if they have mesh
        // or sculpty rooves"). And LLVOVolume::sWolfRoofProbe lets the probe see prims whose
        // click action is "Ignore", which the pick raycast otherwise skips.
        const LLVector3 s3(wx, wy, cam.mV[VZ] + ROOF_PROBE_ABOVE_M), e3(wx, wy, cam.mV[VZ] - 60.f);
        start.load3(s3.mV);
        end.load3(e3.mV);
        // pick_transparent true: a transparent prim is most often a window, and rain must not
        // fall THROUGH a roof that happens to carry alpha. pick_unselectable true: builds are
        // often locked / no-select.
        LLVOVolume::sWolfRoofProbe = true;
        const bool roof_hit = gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, NULL, NULL, NULL, &hit, NULL, NULL, NULL);
        LLVOVolume::sWolfRoofProbe = false;
        if (roof_hit)
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

// [LIGHTNING 2026-09-13] The camera's cell is re-rayed every frame (updateLanding above), so
// this is at most one frame stale. The ray starts above the weather box and stops at the first
// surface; a landing height ABOVE the camera can only be a roof (or a bridge, a deck) over it.
bool WolfWeatherPartSource::cameraUnderRoof() const
{
    if (!mLandingInit) return false;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const S32 camera_cell = (LANDING_N / 2) * LANDING_N + (LANDING_N / 2);
    return mLandingZ[camera_cell] > cam.mV[VZ];
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
    // — so extreme weather still respects the resident's chosen rendering budget.
    const F32 base_rate = (mMode == RAIN) ? RAIN_RATE_BY_LEVEL[mLevel] : SNOW_RATE_BY_LEVEL[mLevel];
    const F32 rate = base_rate * ((F32)mProfile.mDensity / 100.f);
    mCarry += rate * llclamp(dt, 0.f, 0.1f);
    S32 n = (S32)mCarry;
    mCarry -= (F32)n;
    // The clamped profile and time step bound work to at most 8960 attempts a frame (blizzard
    // 2240/s x density 4000 % x dt 0.1 s; it was 2240 while density stopped at 1000 %).
    // A fixed 120-per-frame cap made the same dense profile thinner at lower FPS.
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

// <WolfViewer 2026-09-18> Source: wolfweather.js setUserFog - World > Weather > Fog, the
// resident's own fog (0..100); like the rain and snow choice it applies only when nothing else
// is deciding the weather.
void WolfWeather::setUserFog(S32 percent)
{
    mUserFog = llclamp(percent, WolfWeatherProfile::FOG_MIN, WolfWeatherProfile::FOG_MAX);
    apply();
}

// <WolfViewer 2026-09-18> Source: wolfweather.js setUserAurora.
void WolfWeather::setUserAurora(S32 percent)
{
    mUserAurora = llclamp(percent, WolfWeatherProfile::AURORA_MIN, WolfWeatherProfile::AURORA_MAX);
    apply();
}

// Source: render_manager.js _applyWeatherAurora - the northern lights are a NIGHT sky: the
// profile's amount faded in as the sun goes from just above the horizon (0.05) to well below it
// (-0.12), so a region can leave aurora set and it simply appears after dusk. WolfStorm's fog
// mix hides the aurora in the shader; here the sky's fog is in the vertex haze, which the added
// light bypasses, so what the fog lets through is folded in with (1 - fog)^2.
F32 WolfWeather::auroraAmount()
{
    if (!instanceExists()) return 0.f;
    const WolfWeatherProfile& p = instance().mActive;
    if (p.mAurora <= 0) return 0.f;
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky) return 0.f;
    const F32 sun_z = sky->getSunDirection().mV[VZ];
    F32 night = llclamp((0.05f - sun_z) / 0.17f, 0.f, 1.f);
    night = night * night * (3.f - 2.f * night);
    const F32 clear_air = 1.f - (F32)p.mFog / 100.f;
    return ((F32)p.mAurora / 100.f) * night * clear_air * clear_air;
}

S32 WolfWeather::auroraColorMode()
{
    if (!instanceExists()) return 0;
    return WolfWeatherProfile::auroraColorMode(instance().mActive.mAuroraColor);
}

// Source: render_manager.js _applyWeatherFog - the fog in the air right now as an extinction
// coefficient, 1/metre, 0 for none. exp(-sigma * visibility) = 0.05, so sigma = ln(20) /
// visibility. Read every frame by LLSettingsVOSky::applyToUniforms; mActive is already the
// outcome of the whole precedence rule AND the Weather switch (off -> a default profile, fog 0),
// so every way the weather ends is also how the fog ends.
F32 WolfWeather::fogExtinction()
{
    if (!instanceExists()) return 0.f;
    const S32 fog = instance().mActive.mFog;
    return fog > 0 ? 2.9957f / WolfWeatherProfile::fogVisibility(fog) : 0.f;
}

void WolfWeather::clear()
{
    mUser = Mode::NONE;
    mUserAurora = 0;
    mUserFog = 0;   // Source: menu_handler.js clearWeather - "Clear Weather" clears the air as well as the sky
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

    const WolfWeatherProfile* preview = mPreviews.active();
    WolfWeatherProfile parcel;
    WolfWeatherProfile region;
    const bool parcel_forces = rw.parcelForcedProfile(parcel);
    const WolfWeatherSource selected = wolfWeatherSource(preview != nullptr, mForcedFound,
                                                         parcel_forces, rw.forcedProfile(region));
    if (selected == WolfWeatherSource::PREVIEW)
    {
        prof = *preview;
        mActiveSource = "preview";
    }
    else
    {
        if (selected == WolfWeatherSource::PRIM)
        {
            // A parcel prim's description carries the WHOLE profile, so its look and sound come
            // with it rather than being taken from the region underneath.
            prof = mForcedProfile;
            mActiveSource = "parcel";
        }
        else if (selected == WolfWeatherSource::PARCEL)
        {
            // Source: php/weather.php parcel_payload(): `applies` already combines the estate
            // switch, row existence and enabled. Explicit clear is still a forcing profile.
            prof = parcel;
            mActiveSource = "parcel";
        }
        else if (selected == WolfWeatherSource::REGION)
        {
            prof.mKind  = region.mKind;
            prof.mLevel = region.mLevel;
            prof.mAurora = region.mAurora;   // Source: wolfweather.js _applyPrecedence
            prof.mFog   = region.mFog;   // Source: wolfweather.js _applyPrecedence - the region decides, so its fog
            mActiveSource = "region";
        }
        else
        {
            prof.mKind  = (mUser == Mode::RAIN) ? WolfWeatherProfile::RAIN
                        : (mUser == Mode::SNOW) ? WolfWeatherProfile::SNOW
                                                : WolfWeatherProfile::CLEAR;
            prof.mLevel = (mUser == Mode::RAIN) ? mUserRainLevel : mUserSnowLevel;
            // Source: wolfweather.js _applyPrecedence - the menu decides, so the MENU's fog.
            // Never the stored region row's: `prof` began as rw.stored(), the region's LOOK,
            // which is used even when that row is switched off, and its fog would seep into a
            // resident's own rain.
            prof.mFog   = mUserFog;
            prof.mAurora = mUserAurora;   // same rule as the fog
            mActiveSource = "menu";
        }
        prof.mEnabled = true;
        // Source: parcel-weather-work/plan.md whole-profile precedence. Prim and parcel profiles
        // carry their own art direction. Only region/menu fallback borrows region art direction,
        // so only those sources normalize a tint chosen for another precipitation kind.
        if (wolfWeatherUsesRegionArtDirection(selected)
            && (!on_grid || rw.stored().mKind != prof.mKind))
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
    if (WolfRegionWeather::instance().parcelForcedProfile(ignored)) return "parcel";
    if (WolfRegionWeather::instance().forcedProfile(ignored)) return "region";
    return std::string();
}

void WolfWeather::onRegionWeatherChanged()
{
    apply();
}

WolfWeather::preview_owner_t WolfWeather::acquirePreviewOwner()
{
    return ++mNextPreviewOwner;
}

void WolfWeather::setPreview(preview_owner_t owner, const WolfWeatherProfile* p)
{
    // Source: parcel-weather-work/plan.md: simultaneous Region and About Land editors own
    // independent previews; closing one must leave the other's active preview intact.
    if (p) mPreviews.set(owner, *p);
    else   mPreviews.clear(owner);
    apply();
}

void WolfWeather::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextSweep)
    {
        mNextSweep = now + SWEEP_INTERVAL_SECS;
        sweep();
        ++mSweepCount;   // <WolfViewer 2026-09-26/> snow cover region settle
    }
    // [WEATHER 2026-09-12] The region's own answer, and the sound that goes with the sky.
    WolfRegionWeather::instance().idle();
    WolfWeatherSound::instance().idle();
    // [LIGHTNING 2026-09-13] Forks during a thunderstorm; the thunder is timed from them.
    WolfLightning::instance().idle();
    updateSnowCover(now);   // <WolfViewer 2026-09-18/> what the snow leaves on the ground
}

// <WolfViewer 2026-09-18> ─── snow on the ground ─────────────────────────────────────────────
// Source: wolfstorm environment_manager.js SNOW_COVER_SECS / SNOW_MELT_SECS / snowCoverStep.
F32 WolfWeather::snowCoverStep(F32 cover, bool snowing, S32 level, F32 dt)
{
    static const F32 COVER_SECS[5] = { 0.f, 480.f, 300.f, 200.f, 120.f };   // light .. blizzard (2026-09-18 eve: faster, Paul could not see it)
    static const F32 MELT_SECS = 480.f;
    if (snowing) return llmin(1.f, cover + dt / COVER_SECS[llclamp(level, 1, 4)]);
    return llmax(0.f, cover - dt / MELT_SECS);
}

// Source: environment_manager.js sheltered - the first thing above the ground is well above it.
bool WolfWeather::sheltered(F32 landing_z, F32 ground_z)
{
    return landing_z > -1e8f && landing_z > ground_z + SHELTER_HEADROOM_M;
}

void WolfWeather::updateSnowCover(F64 now)
{
    const F32 dt = mCoverLast > 0.0 ? (F32)llclamp(now - mCoverLast, 0.0, 0.5) : 0.f;
    mCoverLast = now;
    const bool snowing = mActive.mKind == WolfWeatherProfile::SNOW && mSource.notNull() && !mSource->isDead();

    // <WolfViewer 2026-09-26> Region change: hold the cover until the destination's weather is
    // known, then keep it (snowing there too) or clear it (not). See mCoverRegion in the header.
    LLViewerRegion* regionp = gAgent.getRegion();
    const U64 handle = regionp ? regionp->getHandle() : 0;
    if (handle != mCoverRegion)
    {
        if (mSnowCover <= 0.f)
        {
            mCoverRegion = handle;   // nothing on the ground to carry or clear
            mCoverPending = false;
        }
        else if (!mCoverPending)
        {
            mCoverPending = true;
            mCoverPendingSince = now;
            mCoverPendingSweep = mSweepCount;
        }
    }
    if (mCoverPending)
    {
        const bool answered = !WolfGrid::isWolfTerritories()
                           || WolfRegionWeather::instance().answeredThisVisit();
        const bool swept = mSweepCount > mCoverPendingSweep;   // the parcel prims were looked at
        if (!(answered && swept) && now - mCoverPendingSince < COVER_SETTLE_MAX_SECS)
        {
            return;   // held: not building, not melting
        }
        mCoverPending = false;
        mCoverRegion = handle;
        if (!snowing)
        {
            LL_INFOS("WolfWeather") << "snow cover cleared: it does not snow in the region we moved to ("
                                    << (S32)(mSnowCover * 100.f) << "% left behind)" << LL_ENDL;
            mSnowCover = 0.f;
            return;
        }
        LL_INFOS("WolfWeather") << "snow cover kept: it snows in the region we moved to too" << LL_ENDL;
    }
    // </WolfViewer>

    const F32 before = mSnowCover;
    mSnowCover = snowCoverStep(mSnowCover, snowing, mActive.mLevel, dt);
    if (mSnowCover > 0.f && gAgent.getRegion()) updateShelter();
    // One line at each tenth: the cover is building (or melting) and how much of the roof grid
    // is sheltered — the two things that cannot be seen from outside.
    if ((S32)(mSnowCover * 10.f) != (S32)(before * 10.f))
    {
        S32 sheltered = 0;
        for (U8 v : mShelterData) if (v == 0) ++sheltered;
        LL_INFOS("WolfWeather") << "snow cover " << (S32)(mSnowCover * 100.f) << "% (" << (snowing ? "snowing" : "melting")
                                << " level " << mActive.mLevel << "), " << sheltered << " of " << mShelterData.size()
                                << " roof cells sheltered" << LL_ENDL;
    }
}

// Source: WolfWeatherPartSource::updateLanding (the precipitation's roof test) and
// environment_manager.js updateSnowCover: the camera's own cell every frame, the rest round
// robin; each cell = the higher of the land and the first thing a ray from above hits.
void WolfWeather::updateShelter()
{
    if (!mShelterInit)
    {
        for (F32& z : mShelterZ) z = -1e9f;
        mShelterData.assign((size_t)SHELTER_N * SHELTER_N, 255);
        mShelterInit = true;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    mShelterCam = cam;
    const F32 cell = (2.f * SHELTER_HALF_M) / (F32)SHELTER_N;
    const S32 camera_cell = (SHELTER_N / 2) * SHELTER_N + (SHELTER_N / 2);
    for (S32 r = -1; r < SHELTER_RAYS_PER_FRAME; ++r)
    {
        S32 k;
        if (r < 0) k = camera_cell;
        else { k = mShelterNext; mShelterNext = (mShelterNext + 1) % (SHELTER_N * SHELTER_N); }
        const S32 cx = k % SHELTER_N, cy = k / SHELTER_N;
        const F32 wx = cam.mV[VX] - SHELTER_HALF_M + ((F32)cx + 0.5f) * cell;
        const F32 wy = cam.mV[VY] - SHELTER_HALF_M + ((F32)cy + 0.5f) * cell;
        const LLVector3 probe(wx, wy, cam.mV[VZ]);
        const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(probe);
        F32 landing = -1e9f;
        LLVector4a start, end, hit;
        const LLVector3 s3(wx, wy, cam.mV[VZ] + ROOF_PROBE_ABOVE_M), e3(wx, wy, cam.mV[VZ] - 60.f);   // <WolfViewer 2026-09-20/> same reach as updateLanding
        start.load3(s3.mV);
        end.load3(e3.mV);
        LLVOVolume::sWolfRoofProbe = true;   // <WolfViewer 2026-09-20/>
        const bool roof_hit = gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, NULL, NULL, NULL, &hit, NULL, NULL, NULL);
        LLVOVolume::sWolfRoofProbe = false;
        if (roof_hit)
        {
            landing = hit.getF32ptr()[2];
        }
        mShelterZ[k] = landing;
        const U8 open = sheltered(landing, ground) ? 0 : 255;
        if (mShelterData[k] != open) { mShelterData[k] = open; mShelterDirty = true; }
    }
}

void WolfWeather::bindSnowCover(LLGLSLShader* shader, LLViewerRegion* regionp)
{
    static LLStaticHashedString s_cover("wolf_snow_cover");
    static LLStaticHashedString s_origin("wolf_shelter_origin");
    static LLStaticHashedString s_size("wolf_shelter_size");
    if (!shader) return;
    if (mSnowCover <= 0.f || !regionp || !mShelterInit)
    {
        shader->uniform1f(s_cover, 0.f);
        return;
    }
    if (!mShelterTex || mShelterDirty)
    {
        if (!mShelterTex) LLImageGL::generateTextures(1, &mShelterTex);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mShelterTex);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_R8,
                                  SHELTER_N, SHELTER_N, GL_RED, GL_UNSIGNED_BYTE, mShelterData.data(), false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        mShelterDirty = false;
    }
    // The grid sits in agent space round the camera; the shaders work in REGION metres.
    const LLVector3 origin = regionp->getOriginAgent();
    const S32 unit = shader->enableTexture(LLShaderMgr::WOLF_SHELTER_MAP);
    if (unit > -1) gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, mShelterTex);
    else LL_WARNS_ONCE("WolfWeather") << "snow cover: shader '" << shader->mName << "' has no wolfShelterMap sampler - the roof grid cannot be applied" << LL_ENDL;
    if (!mBindLogged)
    {
        mBindLogged = true;
        LL_INFOS("WolfWeather") << "snow cover bound to '" << shader->mName << "': sampler unit " << unit << ", texture " << mShelterTex
                                << ", cover " << mSnowCover << ", origin " << (mShelterCam.mV[VX] - SHELTER_HALF_M - origin.mV[VX]) << "," << (mShelterCam.mV[VY] - SHELTER_HALF_M - origin.mV[VY]) << LL_ENDL;
    }
    shader->uniform1f(s_cover, mSnowCover);
    shader->uniform2f(s_origin, mShelterCam.mV[VX] - SHELTER_HALF_M - origin.mV[VX],
                                mShelterCam.mV[VY] - SHELTER_HALF_M - origin.mV[VY]);
    shader->uniform1f(s_size, 2.f * SHELTER_HALF_M);
}

void WolfWeather::unbindSnowCover(LLGLSLShader* shader)
{
    if (shader) shader->disableTexture(LLShaderMgr::WOLF_SHELTER_MAP);
}
// </WolfViewer>

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
