/**
 * @file wolfautoenvironment.h
 * @brief WolfViewer: Region / Estate > Jimmy Olsen's Automatic Environment.
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

#ifndef WOLF_AUTO_ENVIRONMENT_H
#define WOLF_AUTO_ENVIRONMENT_H

#include "llpanel.h"
#include "llsd.h"
#include "lluuid.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;

/**
 * Jimmy Olsen's Automatic Environment — the region follows a real place on Earth.
 *
 * The region server (WolfSim WolfAutoEnvironmentModule) builds the sky: sun, moon (with its real
 * phase, drawn by lldrawpoolwlsky.cpp / moonF.glsl) and stars where they really are, clouds and
 * haze from the real weather. The web (php/weather.php via php/auto_env_lib.php) serves the real
 * rain, snow, fog and thunder. This tab chooses the place and what follows it.
 *
 *   GET  php/auto_environment.php?region=<uuid>   settings + the place's current observation
 *   GET  php/auto_environment.php?timezones=1     the zones the sky clock can be set to
 *   POST {region, version, enabled, skyOn, weatherOn, placeName, lat, lon, clock}   save
 *   POST {action: "search", q}                     place search (OpenStreetMap Nominatim)
 *
 * WHO MAY CHANGE IT: the region owner only (Paul 2026-10-03), or a god — the same rule
 * auto_environment.php enforces. Everyone else sees it read-only.
 */
class WolfPanelRegionAutoEnvironment : public LLPanel
{
public:
    WolfPanelRegionAutoEnvironment();
    ~WolfPanelRegionAutoEnvironment();

    bool postBuild() override;
    void refresh() override;
    void draw() override;

    static const char* API_URL;

private:
    void fetch();
    void onSearch();
    void onResultPicked();
    void onApply();
    void onRevert();
    void onChanged();
    void onPreviewChanged();
    void showPreviewTime();
    void onVisibilityChange(bool visible) override;
    void writeControls();
    void showNow();
    void setStatus(const std::string& msg, bool error);
    bool canEdit() const;
    std::string regionId() const;

    void fillClocks();
    void onClockSearchCommit();
    static void zonesCoro(LLHandle<LLPanel> handle);
    static void fetchCoro(LLHandle<LLPanel> handle, std::string region_id, U64 serial);
    static void postCoro(LLHandle<LLPanel> handle, std::string body, std::string region_id, bool search, U64 serial);
    void applyReply(const LLSD& reply);

    LLSD        mConfig;          ///< the grid's settings, as last read
    LLSD        mObs;             ///< the place's observation, as last read
    LLUUID      mRegionOwner;     ///< the region's owner, who may save (auto_environment.php regionOwner)
    bool        mHave = false;
    bool        mDirty = false;
    bool        mWriting = false;
    bool        mBusy = false;
    std::string mFetchedFor;
    F64         mNextFetch = 0.0;
    F64         mNextNow = 0.0;
    U64         mSerial = 0;

    LLCheckBoxCtrl*   mEnabled = nullptr;
    LLCheckBoxCtrl*   mSky = nullptr;
    LLCheckBoxCtrl*   mWeather = nullptr;
    LLLineEditor*     mSearch = nullptr;
    LLScrollListCtrl* mResults = nullptr;
    LLLineEditor*     mPlace = nullptr;
    LLSpinCtrl*       mLat = nullptr;
    LLSpinCtrl*       mLon = nullptr;
    LLComboBox*       mClock = nullptr;     ///< [SKY CLOCK 2026-10-04] the clock the sky runs on
    LLLineEditor*     mClockSearch = nullptr;   ///< filters mClock's zones (Paul 10-04: "we need a searchable box")
    LLTextBox*        mNow = nullptr;
    LLTextBox*        mStatus = nullptr;
    LLCheckBoxCtrl*   mPreviewOn = nullptr;
    LLSliderCtrl*     mPreview = nullptr;
    LLTextBox*        mPreviewTime = nullptr;
    LLSD              mResultData;
};

#endif // WOLF_AUTO_ENVIRONMENT_H
