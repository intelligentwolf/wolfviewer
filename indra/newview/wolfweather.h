/**
 * @file wolfweather.h
 * @brief WolfViewer: weather — rain and snow, from the World > Weather menu or from prims
 *        described "wolfrain" / "wolfsnow" in the agent's parcel.
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

#ifndef WOLF_WEATHER_H
#define WOLF_WEATHER_H

#include <string>

#include "llpointer.h"
#include "llsingleton.h"
#include "llviewerpartsource.h"
#include "wolfweatherprofile.h"

// Source: wolfstorm/js/world/wolfweather.js + environment_manager.js rain/snow (2026-09-10).
//
// Paul: "add to wolfstorm and wolfviewer a weather menu, so we want to do rain, and snow to
// start with that a user can turn on and off ... then we need scripts to be able to control
// it so if there is an object in a parcel that has wolfrain in the description it rains, if it
// has wolfsnow in the description it snows".
//
// Two inputs, one output. The USER picks rain / snow / clear in World > Weather. A PARCEL
// forces rain or snow while the agent stands in it and a prim inside it (draw distance,
// LLViewerParcelMgr::inAgentParcel) carries the keyword in its description — read through the
// shared WolfObjectProps harvester exactly as fswolfwater.cpp reads "wolfwater", so a script
// controls the weather with llSetObjectDesc. The parcel wins while it lasts; the user's choice
// comes back when the agent leaves or the keyword goes. Both keywords in one parcel: rain.
//
// The weather itself is one LLViewerPartSource that follows the camera: rain as thin,
// velocity-aligned streaks falling fast through a box round the camera, snow as small soft
// white points falling slowly and drifting on the region wind. It rides the viewer's own
// particle simulation (LLViewerPartSim) and its particle cap, so a heavy scene sheds weather
// before anything else.
class WolfWeatherPartSource : public LLViewerPartSource
{
public:
    enum Mode { NONE, RAIN, SNOW };

    WolfWeatherPartSource();
    void setMode(Mode m) { mMode = m; }
    Mode mode() const { return mMode; }
    /** Intensity 1..4 (light, moderate, heavy, torrential / blizzard). */
    void setLevel(S32 level) { mLevel = llclamp(level, 1, 4); }
    S32  level() const { return mLevel; }
    /**
     * [WEATHER 2026-09-12] The whole profile. The level tables are the BASE figures for a band
     * and the profile's density / velocity / size are percentages OF THEM, so "Heavy Rain at
     * 130% density" is one thing rather than a fifth level with its own table.
     */
    void setProfile(const WolfWeatherProfile& p) { mProfile = p; }
    const WolfWeatherProfile& profile() const { return mProfile; }
    void update(const F32 dt) override;

    /** The roof test grid: LANDING_N x LANDING_N cells over the weather box round the camera. */
    static constexpr S32 LANDING_N = 12;
    static constexpr S32 LANDING_RAYS_PER_FRAME = 6;

private:
    void emit(const LLVector3& camera_pos);
    void updateLanding(const LLVector3& camera_pos, F32 half_xy, F32 top);
    F32  landingZ(const LLVector3& camera_pos, F32 x, F32 y, F32 half_xy) const;
    /** The wind the weather leans on, from the profile's movement speed. Slowly turning, so
        rain does not fall in one fixed diagonal for the whole session. */
    LLVector3 wind() const;
    Mode mMode = NONE;
    S32  mLevel = 2;
    WolfWeatherProfile mProfile;
    F32  mAge = 0.f;     // seconds this source has been alive, for the wind's rotation
    F32  mCarry = 0.f;   // fractional particles carried to the next frame
    F32  mLandingZ[LANDING_N * LANDING_N];   // agent-space landing height per cell (-1e9 = nothing below)
    S32  mLandingNext = 0;
    bool mLandingInit = false;
};

class WolfWeather : public LLSingleton<WolfWeather>
{
    LLSINGLETON(WolfWeather);
    ~WolfWeather();

public:
    using Mode = WolfWeatherPartSource::Mode;

    static constexpr F64 SWEEP_INTERVAL_SECS = 2.0;
    static constexpr F64 STATS_INTERVAL_SECS = 30.0;
    static const std::string KEYWORD_RAIN;
    static const std::string KEYWORD_SNOW;
    /** "wolfclear": no weather HERE, even when the region is raining. A cave, a covered market. */
    static const std::string KEYWORD_CLEAR;

    /**
     * [WEATHER 2026-09-12] THE WEATHER SWITCH. Paul: "allow the user to have a weather on off
     * button rather than the snow rain buttons ... so the user can turn off the region weather
     * like how they turn on and off the environment".
     *
     * Off means NOTHING falls for this resident — not the region's weather, not a parcel's, not
     * their own menu choice, and not an unsaved preview either. It is an opt-out from the whole
     * feature, the same shape as declining the shared environment, and it outranks every rule
     * below because a switch that something else can override is not a switch.
     *
     * Stored in WolfWeatherEnabled, so it survives a restart.
     */
    static bool enabled();
    void toggleEnabled();

    /** Menu / toolbar: the user's choice. Toggling the mode that is on turns the weather off. */
    void toggle(Mode m);
    /**
     * [WEATHER 2026-09-12] The REGION's weather changed (wolfregionweather.cpp). Re-decides
     * what falls, using the precedence below.
     */
    void onRegionWeatherChanged();
    /**
     * LIVE PREVIEW for About Land > Weather: an unsaved edit shown to its author only. It
     * outranks everything, including a parcel prim, because it is not weather — it is the
     * editor showing the owner what they are about to save. Pass nullptr to put the rule back.
     */
    void setPreview(const WolfWeatherProfile* p);
    /** What is actually falling, and which rule chose it ("parcel" / "region" / "menu"). */
    const WolfWeatherProfile& activeProfile() const { return mActive; }
    const std::string& activeSource() const { return mActiveSource; }
    /** Menu: pick a level 1..4 for a mode and turn it on at that level. */
    void setLevel(Mode m, S32 level);
    void clear();
    /** Menu tick: is this mode what is ACTUALLY falling now (preview, parcel, region or user)? */
    bool isOn(Mode m) const { return modeOf(mActive.mKind) == m; }
    /** Menu tick: is this mode on at this level? */
    bool isLevel(Mode m, S32 level) const { return isOn(m) && mActive.mLevel == level; }
    /**
     * True while something other than the resident's own menu decides the weather, so the
     * Weather menu can say so instead of silently doing nothing.
     * @return "parcel", "region" or "" — never "preview", which is the editor's own doing.
     */
    std::string overriddenBy() const;
    /** Kept for callers that only ask "is the menu locked out". */
    bool forced() const { return !overriddenBy().empty(); }
    static Mode modeOf(WolfWeatherProfile::Kind k)
    {
        return k == WolfWeatherProfile::RAIN ? Mode::RAIN
             : k == WolfWeatherProfile::SNOW ? Mode::SNOW : Mode::NONE;
    }
    /** Every frame from LLAppViewer::idle(): the parcel sweep and the emitter's mode. */
    void idle();

private:
    Mode effective() const { return mForced != Mode::NONE ? mForced : mUser; }
    S32  effectiveLevel() const { return mForced != Mode::NONE ? mForcedLevel : (mUser == Mode::RAIN ? mUserRainLevel : mUserSnowLevel); }
    void apply();
    void sweep();

    WolfWeatherProfile mActive;          ///< what is falling right now, whole
    std::string        mActiveSource;    ///< "preview" / "parcel" / "region" / "menu"
    bool               mHavePreview = false;
    WolfWeatherProfile mPreview;

    Mode mUser = Mode::NONE;
    S32  mUserRainLevel = 2;
    S32  mUserSnowLevel = 2;
    Mode mForced = Mode::NONE;
    S32  mForcedLevel = 2;
    /** The whole profile a parcel prim asked for, so its look and sound travel with it. */
    WolfWeatherProfile mForcedProfile;
    bool mForcedFound = false;
    F64  mNextSweep = 0.0;
    F64  mNextStats = 0.0;
    LLPointer<WolfWeatherPartSource> mSource;
};

#endif // WOLF_WEATHER_H
