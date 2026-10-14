/**
 * @file wolfenvironmentstate.h
 * @brief WolfViewer: the Wolf Environment in force where the agent is (phases 2-4 read it).
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

#ifndef WOLF_ENVIRONMENT_STATE_H
#define WOLF_ENVIRONMENT_STATE_H

#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"
#include "v3color.h"
#include "v3math.h"
#include <string>
#include <vector>

/**
 * [WOLF ENV 2026-10-12] One Wolf Environment document (wolfstorm php/wolf_env_lib.php, format 1),
 * read TOLERANTLY: every number clamped to the format's range, every unknown value replaced by
 * Earth's, so whatever reaches the renderer is finite and in range (a stored row the web never
 * validated cannot make NaN or a blinding sky). The same reading as WolfSim's
 * WolfEnvironment.cs ReadDoc for the fields drawn here; what the region bakes into EEP alone (the
 * place, the clock, the water, star brightness, the world's light) is not read. Percentages are
 * kept as fractions (x / 100).
 */
struct WolfEnvBody
{
    std::string mId;
    bool     mHasColor = false;
    LLColor3 mColor = LLColor3::white;
    F32      mSize = 1.f;          // x Earth's sun / moon (planets: x the moon)
    F32      mBrightness = 1.f;
    LLUUID   mTexture;
    enum Mode { REAL, FIXED, ORBIT } mMode = REAL;
    F32      mAzimuth = 0.f, mElevation = 45.f, mPeriodHours = 24.f, mTilt = 0.f, mPhase = 0.f;
    bool     mHasLit = false;      // moons: a fixed lit part
    F32      mLit = 0.5f;
    bool     mRings = false;       // planets
    LLColor3 mRingColor = LLColor3(0.82f, 0.75f, 0.63f);
};

struct WolfEnvZone
{
    std::string mId, mName;
    enum Shape { CIRCLE, RECT, ABOVE } mShape = CIRCLE;
    F32 mX = 0.f, mY = 0.f, mRadius = 1.f, mX1 = 0.f, mY1 = 0.f, mX2 = 0.f, mY2 = 0.f, mAltitude = 1000.f, mEdge = 0.f;
    bool mHasClouds = false;     F32 mClouds = 0.f;
    bool mHasHaze = false;       F32 mHaze = 0.f;
    bool mHasFogColor = false;   LLColor3 mFogColor = LLColor3::white;
    bool mHasFogGlow = false;    F32 mFogGlow = 0.f;
    bool mHasLight = false;      LLColor3 mLight = LLColor3::white;
    bool mHasLightStrength = false; F32 mLightStrength = 1.f;
    bool mHasMood = false;       LLColor3 mTint = LLColor3::white; F32 mTintStrength = 0.f;

    /** 1 inside the shape, fading to 0 across the soft edge; "above" zones by height only.
     *  Source: WolfEnvironment.cs ZoneStrength (the same rule the server bakes parcels by). */
    F32 strengthAt(const LLVector3& region_pos) const;
};

struct WolfEnvDoc
{
    bool mValid = false;
    enum Mode { WOLF, ON_TOP, OFF } mMode = OFF;
    bool mHasClouds = false; F32 mClouds = 0.f;
    F32  mHaze = 0.f;
    LLColor3 mTint = LLColor3::white; F32 mTintStrength = 0.f;
    // the sky block (wolf_env_lib.php we_earth_sky)
    bool mHasRayleigh = false, mHasMie = false, mHasOzone = false;
    LLColor3 mRayleigh = LLColor3::white, mMie = LLColor3::white, mOzone = LLColor3::white;
    F32 mRayleighDensity = 1.f, mMieDensity = 1.f, mMieAnisotropy = 0.8f, mOzoneDensity = 1.f;
    F32 mPlanetKm = 6360.f, mAtmosphereKm = 60.f;
    std::vector<WolfEnvBody> mSuns, mMoons, mPlanets;
    bool mHasStarColor = false; LLColor3 mStarColor = LLColor3::white;
    F32 mNebula = 0.f; LLColor3 mNebulaColor = LLColor3(0.38f, 0.25f, 0.63f);
    bool mHasCloudColor = false; LLColor3 mCloudColor = LLColor3::white;
    F32 mCloudEmission = 0.f;
    std::string mCloudStyle = "natural";
    bool mHasFogColor = false; LLColor3 mFogColor = LLColor3::white;
    F32 mFogGlow = 0.f;
    std::vector<WolfEnvZone> mZones;

    // the weather the web resolved for this scope (entry obs): what the volumetric clouds follow
    bool  mHaveWeather = false;
    F32   mWeatherCloud = 0.3f, mWeatherLow = 0.f, mWeatherMid = 0.f, mWeatherHigh = 0.f, mWeatherFog = 0.f;
    F32   mWindSpeed = 3.f, mWindFrom = 240.f;
    std::string mWeatherKind = "clear";
    S32   mWeatherLevel = 2;
    // [WolfViewer 2026-10-13, step 3 genera] the rest of the observation (auto_env_lib.php:346-384): what the
    // genus mapping needs (wolfcloudgenus.h Weather)
    bool  mHaveTemperature = false; F32 mTemperature = 15.f;   // air_temperature C
    F32   mHumidity = 60.f;                                     // relative_humidity %
    F32   mPrecipitation = 0.f;                                 // precipitation_amount mm, next 1 h
    F32   mThunderChance = 0.f;                                 // probability_of_thunder %
    std::string mWeatherSymbol;                                 // symbol_code
    struct HourlyRow { F64 t = 0.0; F32 low = 0.f, medium = 0.f, high = 0.f; };
    std::vector<HourlyRow> mHourly;                             // the forecast rows [unix time, cloud, low, medium, high, ...]

    static WolfEnvDoc fromEntry(const LLSD& entry);
};

/**
 * Which document is in force for this viewer, now: the parcel's own (when it has one and the estate
 * shows parcel environments), else the region's; or the editor's draft while its preview is on.
 * Wolf grid only, read from php/wolf_environment.php (public GET), again on every region or parcel
 * change and every two minutes. Nothing here draws; wolfenvironmentrender.cpp does.
 */
class WolfEnvironmentState : public LLSingleton<WolfEnvironmentState>
{
    LLSINGLETON(WolfEnvironmentState);
    ~WolfEnvironmentState();

public:
    /** Called once a frame (WolfEnvironmentRender::frameUpdate): reads again when due. */
    void idle();

    /** The document in force; mValid false when there is none (or off the Wolf grid). */
    const WolfEnvDoc& active() const;

    /** The editor's draft while it is previewed (wolffloaterenvironment.cpp); an undefined entry ends it. */
    void setPreviewEntry(const LLSD& entry);
    bool previewing() const { return mPreview.mValid; }

    /** [WOLF ENV 2026-10-12] How far ahead the Wolf window's time slider shows the sky (seconds, 0 = now),
     *  for what the native renderer places by the clock itself (fixed / orbiting bodies). */
    void setTimeAhead(F64 seconds) { mTimeAhead = seconds; }
    F64  timeAhead() const { return mTimeAhead; }

    /** Read again on the next frame (the editor saved or removed a document): the 120 s wait and the
     *  5 s parcel guard are skipped, and a read still in flight - which may carry the document as it
     *  was before the save - is dropped (its serial no longer matches). */
    void refresh();

private:
    static void fetchCoro(std::string url, std::string region_id, U64 serial);
    void applyReply(const LLSD& reply, const std::string& region_id);

    WolfEnvDoc  mRegion, mParcel, mPreview, mNone;
    // [WOLF ENV 2026-10-12] the region's AUTOMATIC environment as a document (php/wolf_environment.php
    // `autoEnv`, only when the region has no stored one), drawn natively only with the opt-in
    // WolfEnvironmentNativeSky
    WolfEnvDoc  mAuto;
    mutable WolfEnvDoc mEepOnly;   // [WolfViewer round 13] WolfEnvironmentAllRegions: a document of the region's EEP alone (active())
    bool        mUseParcel = false;
    std::string mFetchedFor;
    S32         mParcelLocalId = -1;
    F64         mNextFetch = 0.0;
    bool        mBusy = false;
    U64         mSerial = 0;
    F64         mLastFetch = -1.0e9;
    F64         mTimeAhead = 0.0;
};

#endif // WOLF_ENVIRONMENT_STATE_H
