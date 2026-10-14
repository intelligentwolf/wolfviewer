/**
 * @file wolflandsandbox.h
 * @brief WolfViewer: About Land > Sandbox - auto return and most prims per person for a parcel.
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

#ifndef WOLF_LAND_SANDBOX_H
#define WOLF_LAND_SANDBOX_H

#include "llpanel.h"
#include "llframetimer.h"
#include "llsafehandle.h"

class LLButton;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLParcelSelection;
class LLTextBox;

// <WolfViewer 2026-10-13> Paul: "add a function to the land floater, with autoreturn, maximum prims
// per user for Sand Box mode - so it makes sure that a user can't put down more than x prims".
// The panel only asks and shows; the region does the work and every check (WolfSim
// WolfSandboxModule.cs - its summary is the wire format both sides follow).
class WolfPanelLandSandbox final : public LLPanel
{
public:
    explicit WolfPanelLandSandbox(LLSafeHandle<LLParcelSelection>& parcel);
    ~WolfPanelLandSandbox() override;

    bool postBuild() override;
    void refresh() override;
    /** Answers arrive asynchronously and a silent region times out: both are noticed here. */
    void draw() override;
    void onVisibilityChange(bool visible) override;

    /** The region's answer (WolfSandboxDispatchHandler, wolflandsandbox.cpp). */
    static void receive(const LLUUID& region_id, const std::vector<std::string>& strings);

private:
    void request();                      ///< "get" for the selected parcel
    void onApply();
    void onRevert();
    void onEdited();
    void handleReply(const LLUUID& region_id, const std::vector<std::string>& strings);
    void writeControls();                ///< the region's values -> the controls
    void updateEnabled();
    void setStatus(const std::string& msg, bool error);
    void setFieldError(LLTextBox* box, LLLineEditor* field, const std::string& msg);
    bool sendToRegion(const std::vector<std::string>& params);

    LLSafeHandle<LLParcelSelection>& mParcel;

    // which parcel the panel is showing: the selection's region and the parcel's local id
    LLUUID mRegionId;
    S32 mLocalId = -1;

    // the region's last answer
    bool mKnown = false;                 ///< the region has answered for this parcel
    bool mOn = false;
    S32  mMaxPrims = 0;                  ///< 0 = never set
    S32  mMinutes = 0;                   ///< the parcel's auto return now (LandData.OtherCleanTime)
    bool mCanEdit = false;
    S32  mMine = 0;

    bool mDirty = false;                 ///< the controls hold edits not yet applied
    bool mWriting = false;               ///< writeControls() is setting values: not an edit
    bool mGetPending = false;
    bool mSetPending = false;
    bool mUnsupported = false;           ///< the region never answered: no WolfSandboxModule there
    LLFrameTimer mPendingTimer;

    LLCheckBoxCtrl* mEnabled = nullptr;
    LLLineEditor*   mMaxPrimsEdit = nullptr;
    LLLineEditor*   mMinutesEdit = nullptr;
    LLTextBox*      mMaxPrimsError = nullptr;
    LLTextBox*      mMinutesError = nullptr;
    LLTextBox*      mYours = nullptr;
    LLTextBox*      mBuildHint = nullptr;
    LLTextBox*      mStatus = nullptr;
    LLButton*       mApply = nullptr;
    LLButton*       mRevert = nullptr;
};

#endif // WOLF_LAND_SANDBOX_H
