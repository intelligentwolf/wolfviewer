/**
 * @file wolfregionweather.h
 * @brief WolfViewer: the region's weather — About Land > Weather.
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

#ifndef WOLF_REGION_WEATHER_H
#define WOLF_REGION_WEATHER_H

#include "llhandle.h"
#include "llsd.h"
#include "llpanel.h"
#include "llsingleton.h"
#include "llviewerparcelmgr.h"
#include "wolfweatherprofile.h"

#include <string>

class LLButton;
class LLCheckBoxCtrl;
class LLColorSwatchCtrl;
class LLComboBox;
class LLRadioGroup;
class LLSliderCtrl;
class LLFloater;
class LLTextBox;

/**
 * The region's weather, stored on the grid (php/weather.php, table robust.wolf_weather).
 *
 * Weather already had two sources and neither was the region: a resident's own Weather menu, and
 * a scripted prim forcing it on its parcel through "wolfrain"/"wolfsnow" in the description.
 * This is the third, and the one an estate owner reaches for — rain across a whole region, seen
 * by everyone in it.
 *
 * PRECEDENCE, most specific first, applied in wolfweather.cpp:
 *   1. a parcel prim's description  — a script beats everything
 *   2. THIS region setting
 *   3. the resident's own Weather menu choice
 *
 * A region with NO ROW has kind "" — which is NOT the same as "clear". Empty means nobody has
 * set this region's weather and the resident's own choice still applies; "clear" means the owner
 * has deliberately turned it off for everyone.
 *
 * Deliberately shaped like WolfWaveZones: same API host, same auth headers, same optimistic
 * concurrency on `version`.
 */
class WolfRegionWeather : public LLSingleton<WolfRegionWeather>
{
    LLSINGLETON(WolfRegionWeather);

public:
    /** Ask the grid about the region the agent is in. Cheap and idempotent; throttled inside. */
    void refresh(bool force = false);

    /** Called every frame from WolfWeather::idle; re-asks on a region change or the poll clock. */
    void idle();

    /** True when this region is known to the grid — the only case the tab exists. */
    bool onGrid() const { return mHaveRow; }

    /**
     * The region's stored profile, complete. Usable even when the region has no opinion about
     * WHAT falls: the look and sound fields are the estate owner's art direction and
     * wolfweather.cpp applies them to a resident's own weather too.
     */
    const WolfWeatherProfile& stored() const { return mProfile; }

    /** True when the region is IMPOSING weather; then `out` is what it imposes. */
    bool forcedProfile(WolfWeatherProfile& out) const;

    S32 version() const { return mVersion; }
    const std::string& regionName() const { return mRegionName; }
    /** "" when the region has no row's worth of opinion about what falls. */
    const std::string& kind() const { return mKind; }

    /** Save. The grid checks the caller holds the region; this is the courtesy copy. */
    void save(const WolfWeatherProfile& profile);

    /**
     * The region pushed a change at us (a script called wolfSetRegionWeather). Takes the row as
     * the grid now has it, without a round trip. grid.weather is still the truth — this only
     * saves waiting for the next poll. Source: the GenericMessage "WolfWeather".
     */
    void applyPush(const LLSD& row);

    bool saving() const { return mSaving; }
    const std::string& lastError() const { return mLastError; }
    /** Bumped whenever the answer changes, so a panel can notice without polling fields. */
    S32 generation() const { return mGeneration; }

    static const char* API_URL;

private:
    static void fetchCoro(std::string region_id);
    static void saveCoro(std::string body);
    void applyToWeather();

    bool               mHaveRow = false;
    WolfWeatherProfile mProfile;
    std::string        mKind;         // "" = no opinion; NOT the same as "clear"
    S32                mVersion = 0;
    std::string        mRegionName;
    std::string        mFetchedFor;   // region uuid already asked about
    bool               mFetching = false;
    bool               mSaving = false;
    std::string        mLastError;
    S32                mGeneration = 0;
    F32                mNextPoll = 0.f;
};

/**
 * About Land > Weather — the region weather control panel.
 *
 * Built to the design Paul gave, and the same panel as the web viewer's
 * (wolfstorm/js/ui/floaters/land_weather_tab.js), control for control:
 *
 *   General Weather Control   Apply Preset, Use Inventory Preset
 *   Global Controls           Current Weather Type, Apply Global Weather
 *   Weather Dynamics          Precipitation Brightness, Precipitation Colour,
 *                             Weather Movement Speed
 *   Sound                     Enable/Disable Sound, Sound Volume
 *   Particle Effects          the intensity ladder, Particle density, Velocity, Size
 *   Sound Presets             the ambience bed, the near layer, Reset
 *
 * LIVE PREVIEW: moving anything shows it in THIS viewer at once through
 * WolfWeather::setPreview — you judge rain by looking at it. Apply writes it to the grid for
 * everyone; Revert drops the preview. Closing the floater drops it too, so an abandoned edit
 * never sticks.
 *
 * Source: wolfwavezones.h WolfPanelLandWaves for the About Land panel shape.
 */
class WolfPanelLandWeather : public LLPanel
{
public:
    WolfPanelLandWeather(LLParcelSelectionHandle& parcel);
    ~WolfPanelLandWeather();
    bool postBuild() override;
    void refresh() override;
    /** The grid's answers land asynchronously; LLFloaterLand::refresh only runs on parcel changes. */
    void draw() override;

private:
    void onApplyPreset();
    void onInventoryPreset();
    void onControlChanged();       ///< any slider / combo / check: update the edit and preview
    void onKindChanged();
    void onReset();
    void onApply();
    void onRevert();

    void readControls();           ///< controls -> mEdit
    void writeControls();          ///< mEdit -> controls
    void updatePreview();
    void setStatus(const std::string& msg, bool error);
    /** Region-level rights — the same rule php/weather.php enforces. A courtesy, not the check. */
    bool canEdit() const;

    WolfWeatherProfile mEdit;      ///< the working copy, edited until Apply or Revert
    WolfWeatherProfile mStored;    ///< the grid's row, to know whether anything is unsaved
    S32  mShownGeneration = -1;
    bool mWriting = false;         ///< writeControls() is setting values: ignore the commits

    LLComboBox*        mPreset = nullptr;
    LLComboBox*        mKind = nullptr;
    LLCheckBoxCtrl*    mEnabled = nullptr;
    LLRadioGroup*      mLadder = nullptr;
    LLSliderCtrl*      mBrightness = nullptr;
    LLColorSwatchCtrl* mTint = nullptr;
    LLSliderCtrl*      mTintAmount = nullptr;
    LLSliderCtrl*      mMoveSpeed = nullptr;
    LLSliderCtrl*      mDensity = nullptr;
    LLSliderCtrl*      mVelocity = nullptr;
    LLSliderCtrl*      mSize = nullptr;
    LLCheckBoxCtrl*    mSound = nullptr;
    LLSliderCtrl*      mVolume = nullptr;
    LLComboBox*        mAmbience = nullptr;
    LLComboBox*        mNearLayer = nullptr;
    LLButton*          mApply = nullptr;
    LLTextBox*         mRegionLine = nullptr;
    LLTextBox*         mStatus = nullptr;
    LLTextBox*         mParticlesTitle = nullptr;
    /** The notecard picker, one at a time; a handle so a closed window is not a dangling one. */
    LLHandle<LLFloater> mPickerHandle;
};

#endif // WOLF_REGION_WEATHER_H
