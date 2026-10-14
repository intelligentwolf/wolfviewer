/**
 * @file wolffloaterbuilder.h
 * @brief WolfViewer: the Wolf Builder floater - the grid, snapping to your own grid origin, floating
 *        object names, and the selection card. The machinery is wolfbuilder.cpp.
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

#ifndef WOLF_FLOATERBUILDER_H
#define WOLF_FLOATERBUILDER_H

#include "llfloater.h"
#include "llframetimer.h"

class LLCheckBoxCtrl;
class LLComboBox;
class LLSpinCtrl;
class LLTextBox;

// [WOLF BUILDER 2026-10-13] Paul: "another Wolf Builder floater with controls like a grid, easier
// snapping to defined grids, floating object names (all as options), floating number of prims and
// location like a card - imagine if you will like a builders helper". Opened from the Build floater's
// Builders Mode row, Build > Wolf Builder... and Wolf World > Wolf Builder...
//
// Every switch is a persisted setting the check box is bound to (control_name), so a tick is the
// whole wiring: the setting changes, and wolfbuilder.cpp reads it each frame. The grids draw only
// with Builders Mode (WolfBuildersMode) on, so this floater has that box too, and greys the grid
// switches out with a line saying why while it is off. The spacing and size
// combos write GridResolution and GridDrawSize, which the Build floater's Options... window shows
// too. The origin spinners write WolfBuilderGridOrigin. The card refreshes a few times a second from
// LLSelectMgr and the parcel, and says "Nothing selected." when there is nothing.
class WolfFloaterBuilder : public LLFloater
{
public:
    WolfFloaterBuilder(const LLSD& key);
    ~WolfFloaterBuilder() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void onCommitBuildersMode();
    void onCommitSpacing();
    void onCommitSize();
    void onCommitOrigin();
    void onSetOriginFromSelection();
    void onGridOnObject();
    void refreshGridControls();
    void refreshDrawSwitches();
    void refreshCard();
    void setStatus(const std::string& text, bool error);

    LLCheckBoxCtrl* mBuildersMode = nullptr;
    LLTextBox*  mBuildersModeHint = nullptr;
    LLComboBox* mSpacing = nullptr;
    LLComboBox* mSize = nullptr;
    LLSpinCtrl* mOriginX = nullptr;
    LLSpinCtrl* mOriginY = nullptr;
    LLSpinCtrl* mOriginZ = nullptr;
    LLTextBox*  mNamesCount = nullptr;
    LLTextBox*  mCardSelection = nullptr;
    LLTextBox*  mCardImpact = nullptr;
    LLTextBox*  mCardPosition = nullptr;
    LLTextBox*  mCardRegion = nullptr;
    LLTextBox*  mCardParcel = nullptr;
    LLTextBox*  mCardRegionPrims = nullptr;
    LLTextBox*  mStatus = nullptr;
    LLFrameTimer mRefreshTimer;
};

#endif // WOLF_FLOATERBUILDER_H
