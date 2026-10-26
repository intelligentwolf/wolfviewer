/**
 * @file wolffloaterenvironment.h
 * @brief WolfViewer: World > Wolf World > Wolf Environment — the owner's window.
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

#ifndef WOLF_FLOATER_ENVIRONMENT_H
#define WOLF_FLOATER_ENVIRONMENT_H

#include "llfloater.h"
#include "llsd.h"

#include "llsettingsdaycycle.h"

class LLButton;
class LLCheckBoxCtrl;
class LLColorSwatchCtrl;
class LLComboBox;
class LLLineEditor;
class LLRadioGroup;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTabContainer;
class LLTextBox;
class LLTextureCtrl;

/**
 * [WOLF ENV 2026-10-12] The Wolf Environment: one plain description of a region's (or one
 * parcel's) sky, set up here in plain terms (format: wolfstorm php/wolf_env_lib.php). The region
 * bakes it into ordinary EEP for every viewer (WolfSim WolfEnvironment.cs); WolfViewer also draws
 * what EEP cannot (wolfenvironmentsky.cpp, phases 2-4).
 *
 *   persona/story  a region owner, or a tenant on their own parcel, wants to give their land a look
 *                  (London's real weather, or a green alien sky with two moons), see it first on
 *                  their own screen, then save it for everyone — in a couple of minutes.
 *   flow           scope (this region / this parcel) -> mode -> a look -> Basics (place, time,
 *                  weather, cover, haze, mood, water) -> optional Sky / Suns & moons / Zones ->
 *                  Preview (only you) -> Save for everyone.
 *
 *   GET  php/wolf_environment.php?region=&x=&y=   the region's and this parcel's documents, presets,
 *                                                 whether you may change them
 *   POST {action: "check"|"save"|"remove", region, scope, x, y, version, doc}
 *   preview: the checked entry to the region's WolfEnvironmentPreview capability, which answers the
 *            baked EEP; shown as this viewer's own environment (ENV_LOCAL) until stopped.
 *
 * WHO MAY SAVE is decided by the web (wolf_env_lib.php: region holders; the parcel's owner or a
 * group member with the environment power); `you` in the GET is only shown here. Wolf grid only:
 * the menu item is gone elsewhere and the window closes itself on a hypergrid jump.
 */
class WolfFloaterEnvironment : public LLFloater
{
public:
    explicit WolfFloaterEnvironment(const LLSD& key);
    ~WolfFloaterEnvironment() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    static const char* API_URL;
    static const char* AUTO_ENV_URL;

private:
    // the network
    void fetch();
    void post(const std::string& action);
    void startPreview();
    void stopPreview();
    void onSearch();
    static void getCoro(LLHandle<LLFloater> handle, std::string url, U64 serial);
    static void postCoro(LLHandle<LLFloater> handle, std::string url, std::string body, std::string kind, U64 serial);
    static void previewCoro(LLHandle<LLFloater> handle, std::string cap, std::string body, U64 serial);
    static void zonesCoro(LLHandle<LLFloater> handle);
    static void clockCoro(LLHandle<LLFloater> handle, std::string url, std::string region_id);

    // [WOLF ENV 2026-10-12] the time test slider (as Jimmy's tab: wolfautoenvironment.cpp preview time)
    void applyTime();             ///< the slider -> the sky on this screen (the draft's while it is previewed)
    void endTime();               ///< off, and the sky back to now
    void showTime();              ///< the readout under the slider
    bool timeOn() const;
    F64  timeAhead() const;       ///< seconds ahead, 0 when off
    void onGetReply(const LLSD& reply, bool ok, const std::string& why);
    void onPostReply(const std::string& kind, const LLSD& reply, bool ok, bool conflict, const std::string& why);

    // the document <-> the controls
    void applyReply(const LLSD& reply);
    void writeControls();
    void writeBody();
    void writeZone();
    void readControls();
    void readBody();
    void readZone();
    void fillBodies();
    void fillZones();
    void fillTimeZones();
    void updateVisibility();
    void showNow();
    void onChanged();
    void onPresetPicked();
    void onScopePicked();
    void onRevert();
    void onRemoveAsked();
    void showErrors(const LLSD& errors);
    void setStatus(const std::string& msg, bool error);
    bool canEdit() const;
    LLSD& scopeEntry();           ///< the reply's entry for the current scope (may be undefined)
    LLSD& bodyList(const std::string& kind);
    std::string regionId() const;

    LLSD        mReply;           ///< the last GET reply
    LLSD        mDoc;             ///< the draft being edited
    std::string mScope = "region";
    S32         mVersion = 0;     ///< the stored row's version for this scope (0 = none)
    F32         mAtX = 0.f, mAtY = 0.f;   ///< where the agent stood when this scope was read (names the parcel)
    std::string mFetchedFor;
    S32         mParcelLocalId = -1;
    bool        mHave = false;
    bool        mDirty = false;
    bool        mWriting = false;
    bool        mBusy = false;
    /// [WolfViewer 2026-10-26] what holds mBusy: "fetch", "search", "save", "remove" or "check" (the preview's own validation,
    /// which never locks the editor - ticket #241), and the serial of that request (only it may clear mBusy)
    std::string mBusyKind;
    U64         mBusySerial = 0;
    /** The editor is locked only while a request the person started (a read, a search, a save, a remove) is out. */
    bool        editLocked() const { return mBusy && mBusyKind != "check"; }
    bool        mPreviewOn = false;
    bool        mPreviewPending = false;
    bool        mPreviewBusy = false;   ///< the region is baking a preview (separate from mBusy, the web)
    LLSD        mPreviewEntry;          ///< the checked draft being previewed (for the native renderer)
    F64         mPreviewDue = 0.0;
    U64         mSerial = 0;
    U64         mPreviewSerial = 0;
    S32         mBodyIndex = -1;  ///< selected row in the bodies list: index into mBodyRows
    S32         mZoneIndex = -1;
    LLSD        mBodyRows;        ///< [ [kind, index], ... ] behind the bodies list
    LLSD        mResults;         ///< place search results
    LLSD        mClockCfg;        ///< auto_environment.php's config for this region (the sky clock: offsets, label)
    std::string mClockFor;        ///< the region mClockCfg was read for
    bool        mTimeOwned = false;   ///< this window started the ENV_LOCAL time preview (not Jimmy's tab)
    LLSettingsDay::ptr_t mPreviewDay; ///< the baked draft's cycle while it is previewed (run ahead by the slider)
    S32         mPreviewDayLength = 86400;
    S32         mPreviewDayOffset = 0;
    LLCheckBoxCtrl*    mTimeOn = nullptr;
    LLSliderCtrl*      mTime = nullptr;
    LLTextBox*         mTimeText = nullptr;

    LLComboBox*        mScopeCombo = nullptr;
    LLTextBox*         mWho = nullptr;
    LLTextBox*         mStatus = nullptr;
    LLTabContainer*    mTabs = nullptr;
    LLRadioGroup*      mMode = nullptr;
    LLButton*          mPreviewBtn = nullptr;
};

#endif // WOLF_FLOATER_ENVIRONMENT_H
