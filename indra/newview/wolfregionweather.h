/**
 * @file wolfregionweather.h
 * @brief WolfViewer: region and occupied-parcel weather state and editors.
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
#include "wolfweatherstate.h"

#include <string>

class LLButton;
class LLCheckBoxCtrl;
class LLColorSwatchCtrl;
class LLComboBox;
class LLRadioGroup;
class LLSliderCtrl;
class LLFloater;
class LLTextBox;

// Source: weather.php:383-505 and grid_rights.php:298-337. A draft belongs to the exact server
// row revision and, for parcels, the accepted point that resolved its stable UUID.
struct WolfWeatherSaveTarget
{
    std::string mEditorTarget;
    std::string mRegionId;
    std::string mParcelId;
    S32 mVersion = 0;
    F32 mX = 0.f;
    F32 mY = 0.f;
    WolfWeatherAsyncGate::generation_t mVisitGeneration = 0;
};

/**
 * The region and occupied parcel weather, stored by php/weather.php in grid.weather and
 * grid.weather_parcel.
 *
 * Weather already had two sources and neither was the region: a resident's own Weather menu, and
 * a scripted prim forcing it on its parcel through "wolfrain"/"wolfsnow" in the description.
 * This is the third, and the one an estate owner reaches for — rain across a whole region, seen
 * by everyone in it.
 *
 * PRECEDENCE, most specific first, applied in wolfweather.cpp:
 *   1. a parcel prim's description  — a script beats everything
 *   2. THIS parcel's stored setting
 *   3. THIS region setting
 *   4. the resident's own Weather menu choice
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
    ~WolfRegionWeather();

public:
    /** Ask the grid about the region the agent is in. Cheap and idempotent; throttled inside. */
    void refresh(bool force = false);

    /** Called every frame from WolfWeather::idle; re-asks on a region change or the poll clock. */
    void idle();

    /** True when this region is known to the grid — the only case the tab exists. */
    bool onGrid() const { return mHaveRow; }
    /** <WolfViewer 2026-09-26> The grid has answered (row, no row, or error) since the agent
     *  arrived in this region — WolfWeather waits for it before deciding what happens to the
     *  snow already on the ground. */
    bool answeredThisVisit() const { return mAnsweredValid && mRegionGate.accepts(mAnsweredGeneration); }

    /**
     * The region's stored profile, complete. Usable even when the region has no opinion about
     * WHAT falls: the look and sound fields are the estate owner's art direction and
     * wolfweather.cpp applies them to a resident's own weather too.
     */
    const WolfWeatherProfile& stored() const { return mProfile; }

    /** True when the region is IMPOSING weather; then `out` is what it imposes. */
    bool forcedProfile(WolfWeatherProfile& out) const;
    /** Source: weather.php parcel_payload(): applies is authoritative, including explicit clear. */
    bool parcelForcedProfile(WolfWeatherProfile& out) const;

    S32 version() const { return mVersion; }
    const std::string& regionName() const { return mRegionName; }
    /** "" when the region has no row's worth of opinion about what falls. */
    const std::string& kind() const { return mKind; }
    bool allowParcel() const { return mAllowParcel; }

    bool haveParcel() const { return mHaveParcel; }
    const WolfWeatherProfile& parcelStored() const { return mParcelProfile; }
    const std::string& parcelKind() const { return mParcelKind; }
    const std::string& parcelName() const { return mParcelName; }
    const std::string& parcelId() const { return mParcelId; }
    bool parcelGroupOwned() const { return mParcelGroupOwned; }
    bool parcelApplies() const { return mParcelApplies; }
    std::string regionTarget() const;
    std::string parcelTarget() const;
    WolfWeatherSaveTarget regionSaveTarget() const;
    WolfWeatherSaveTarget parcelSaveTarget() const;

    /** Save. The grid checks the caller holds the region; this is the courtesy copy. */
    U64 saveRegion(const WolfWeatherProfile& profile, bool allow_parcel,
                   const WolfWeatherSaveTarget& captured_target);
    U64 saveParcel(const WolfWeatherProfile& profile, const WolfWeatherSaveTarget& captured_target);
    const WolfWeatherSaveResult& regionSaveResult() const { return mRegionSaveResult; }
    const WolfWeatherSaveResult& parcelSaveResult() const { return mParcelSaveResult; }

    /**
     * The region pushed a change at us (a script called wolfSetRegionWeather). Takes the row as
     * the grid now has it, without a round trip. grid.weather is still the truth — this only
     * saves waiting for the next poll. Source: the GenericMessage "WolfWeather".
     */
    void applyPush(const LLSD& row);

    const std::string& lastError() const { return mLastError; }
    /** Bumped whenever the answer changes, so a panel can notice without polling fields. */
    S32 generation() const { return mGeneration; }

    static const char* API_URL;

private:
    static void fetchCoro(std::string region_id, bool include_parcel, F32 x, F32 y,
                          WolfWeatherAsyncGate::generation_t region_generation,
                          WolfWeatherAsyncGate::generation_t parcel_generation, U64 fetch_serial);
    static void saveCoro(std::string body, std::string scope, std::string region_id,
                         std::string parcel_id, WolfWeatherAsyncGate::generation_t target_generation,
                         U64 save_serial);
    void applyToWeather();
    void onRegionChanged();
    void onParcelChanged();
    void invalidateParcel();
    void finishFetch();
    void recomputeParcelApplies();

    bool               mHaveRow = false;
    WolfWeatherProfile mProfile;
    std::string        mKind;         // "" = no opinion; NOT the same as "clear"
    bool               mAllowParcel = true;
    S32                mVersion = 0;
    std::string        mRegionName;
    std::string        mFetchedFor;   // region uuid already asked about
    std::string        mAttemptedFor; // target whose retry clock is already running
    S32                mFetchesInFlight = 0;
    bool               mFetchPending = false;
    bool               mFetchPendingForce = false;
    bool               mAwaitingParcel = true;
    bool               mSavingRegion = false;
    bool               mSavingParcel = false;
    std::string        mLastError;
    S32                mGeneration = 0;
    F32                mNextPoll = 0.f;
    bool               mHaveParcel = false;
    WolfWeatherProfile mParcelProfile;
    std::string        mParcelKind;
    std::string        mParcelName;
    std::string        mParcelId;
    bool               mParcelGroupOwned = false;
    bool               mParcelApplies = false;
    S32                mParcelVersion = 0;
    F32                mParcelX = 0.f;
    F32                mParcelY = 0.f;
    WolfWeatherAsyncGate mRegionGate;
    WolfWeatherAsyncGate::generation_t mAnsweredGeneration = 0;   // <WolfViewer 2026-09-26/>
    bool mAnsweredValid = false;
    WolfWeatherAsyncGate mParcelGate;
    U64                mNextFetchSerial = 0;
    U64                mAcceptedFetchSerial = 0;
    U64                mRegionSaveSerial = 0;
    U64                mParcelSaveSerial = 0;
    WolfWeatherSaveResult mRegionSaveResult;
    WolfWeatherSaveResult mParcelSaveResult;
    boost::signals2::connection mRegionChangedConnection;
    boost::signals2::connection mParcelChangedConnection;

public:
    bool fetching() const { return mFetchesInFlight > 0; }
    bool savingRegion() const { return mSavingRegion; }
    bool savingParcel() const { return mSavingParcel; }
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
class WolfPanelWeather : public LLPanel
{
public:
    enum Scope { REGION, PARCEL };
    explicit WolfPanelWeather(Scope scope);
    ~WolfPanelWeather();
    bool postBuild() override;
    void refresh() override;
    /** The grid's answers land asynchronously; LLFloaterLand::refresh only runs on parcel changes. */
    void draw() override;
    void onVisibilityChange(bool visible) override;
    void clearPreview();

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
    std::string currentTarget() const;
    WolfWeatherSaveTarget currentSaveTarget() const;
    const WolfWeatherProfile& currentStored() const;
    bool currentAllowParcel() const;
    bool dirty() const;
    void adoptTarget(bool announce_discard);
    void cancelPresetLoad();
    void markEdited();

    WolfWeatherProfile mEdit;      ///< the working copy, edited until Apply or Revert
    WolfWeatherProfile mStored;    ///< the grid's row, to know whether anything is unsaved
    S32  mShownGeneration = -1;
    bool mWriting = false;         ///< writeControls() is setting values: ignore the commits
    Scope mScope;
    std::string mTarget;
    WolfWeatherSaveTarget mSaveTarget;
    bool mEditAllowParcel = true;
    bool mStoredAllowParcel = true;
    WolfWeatherEditGate mEditGate;
    U64 mEditRevision = 0;
    U64 mSubmittedEditRevision = 0;
    U64 mSubmittedSaveSerial = 0;
    WolfWeatherProfile mSubmittedProfile;
    bool mSubmittedAllowParcel = true;
    bool mSavePending = false;
    U64 mPreviewOwner = 0;
    boost::signals2::connection mPanelRegionChangedConnection;
    boost::signals2::connection mPanelParcelChangedConnection;

    LLComboBox*        mPreset = nullptr;
    LLComboBox*        mKind = nullptr;
    LLCheckBoxCtrl*    mEnabled = nullptr;
    LLCheckBoxCtrl*    mUseRegion = nullptr;
    LLCheckBoxCtrl*    mAllowParcel = nullptr;
    LLRadioGroup*      mLadder = nullptr;
    LLSliderCtrl*      mBrightness = nullptr;
    LLSliderCtrl* mAurora = nullptr;
    LLComboBox*   mAuroraColor = nullptr;   // <WolfViewer 2026-09-18/> Source: land_weather_tab.js wx-acol       // <WolfViewer 2026-09-18/> Source: land_weather_tab.js wx-aurora
    LLSliderCtrl* mFog = nullptr;          // <WolfViewer 2026-09-18/> Source: land_weather_tab.js wx-fog
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

class WolfPanelRegionWeather final : public WolfPanelWeather
{
public:
    WolfPanelRegionWeather() : WolfPanelWeather(REGION) {}
};

class WolfPanelLandWeather final : public WolfPanelWeather
{
public:
    explicit WolfPanelLandWeather(LLParcelSelectionHandle&) : WolfPanelWeather(PARCEL) {}
};

#endif // WOLF_REGION_WEATHER_H
