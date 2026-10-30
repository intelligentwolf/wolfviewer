/**
 * @file wolffloaterterraform.h
 * @brief WolfViewer: the Terraform window - Level, Ramp, Terrace, Smooth, Road, River and Beach with a preview, the mole plate
 *        and Undo / Redo.
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

#ifndef WOLF_FLOATER_TERRAFORM_H
#define WOLF_FLOATER_TERRAFORM_H

#include <string>
#include <vector>

#include "lleditmenuhandler.h"
#include "llfloater.h"
#include "v3math.h"
#include "wolffloatertunnels.h"   // WolfRouteHost, WolfToolTunnel: the Tunnel tool's pen, reused
#include "wolfterraform.h"

class LLButton;
class LLCheckBoxCtrl;
class LLPanel;
class LLRadioGroup;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;
class WolfDigProgress;

/**
 * [WOLF TERRAFORM 2026-10-23] The window (floater_wolf_terraform.xml; persona, story and flow in its header and wolfterraform.h).
 * Opened from the Build window's land tools (Terraform...), Build > Terraform... and Wolf World > Build & Land > Terraform...,
 * on Wolf Territories only (the same WolfGrid.IsOnWolfTerritories gate as the Cave Builder and the Tunnel tool).
 * The clicks on the ground are the Tunnel tool's pen (WolfToolTunnel) drawing for this window (WolfRouteHost): click to add a
 * point, drag a marker to move it, Ctrl+click to take one out or put one in, Backspace to take the last back. The preview (red
 * cut, green fill, grey stripes on land this viewer does not know to be yours) is drawn in the world from the plan, never on the
 * ground itself. Edit > Undo / Redo (Ctrl+Z / Ctrl+Y) are the window's history while it is the edit handler.
 */
class WolfFloaterTerraform : public LLFloater, public WolfRouteHost, public LLEditMenuHandler
{
public:
    explicit WolfFloaterTerraform(const LLSD& key);
    ~WolfFloaterTerraform() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onFocusReceived() override;
    void draw() override;

    // WolfRouteHost (the pen)
    void clickAt(F32 x, F32 y) override;
    void moveClick(S32 k, F32 x, F32 y, bool done) override;
    void undoPoint() override;
    void removeClick(S32 k) override;
    void insertClickNear(F32 x, F32 y) override;
    void setStatus(const std::string& text, bool error) override;
    void penStopped() override;
    F32 planeHeight() const override;
    const std::vector<LLVector2>& clicks() const override { return mClicks; }
    bool routeEditing() const override;
    std::string noGroundText(bool underground) const override;
    std::string escapeText() const override;
    F32 cursorRadius() const override;
    std::string markerLabel(size_t k) const override;
    std::string routeHint() const override;
    void renderRouteExtras(LLViewerRegion* rgn) override;
    bool joinClicks() const override;
    bool markersMovable() const override { return !mPicking; }

    // LLEditMenuHandler: Edit > Undo / Redo while this window is the edit handler
    void undo() override;
    bool canUndo() const override;
    void redo() override;
    bool canRedo() const override;

private:
    void pickTool(WolfTerraform::ETool tool);
    WolfTerraform::Params readParams() const;
    void changed();                 // a click or a setting changed: the plan is worked out again (at most every PLAN_SECS)
    void replan();
    void buildPreview();
    void updateControls();
    void fillHistory();
    void relayout();
    void onApply();
    void refuse(const std::string& why);
    void onUndo();
    void onRedo();
    void keepPen();
    bool penAllowed() const;
    void paintBurrow();
    void paintTile(LLButton* b, WolfTerraform::ETool tool) const;
    void clearDrawing();
    /** [ticket 355] The brush shape row as the tool needs it: the choice for the box tools, a note for the line tools. */
    void updateShapeControls();
    bool roundBrush() const;        // Round is chosen and the tool is a box tool

    WolfTerraform::ETool mTool = WolfTerraform::TOOL_LEVEL;
    std::vector<LLVector2> mClicks;
    WolfTerraform::Plan mPlan;
    bool mPlanDirty = true;
    F64 mPlanAt = 0.0;
    bool mPicking = false;          // the next click gives Level's height
    bool mPenWanted = false;
    bool mStartPending = false;     // onOpen ran: the Build window is closed in the first draw()
    LLUUID mRegionAt;               // the region the drawing is on
    U32 mShownSerial = 0;
    bool mSawJob = false;
    size_t mShownSteps = (size_t)-1, mShownDone = (size_t)-1;
    U32 mShownHistoryVersion = 0;
    LLUUID mShownHistoryRegion;

    // the preview, region metres: triangles by colour, the box's outline
    std::vector<LLVector3> mCut, mFill, mNotMine, mOutline;

    LLButton* mTiles[WolfTerraform::TOOL_COUNT] = {};
    LLPanel* mSimple[WolfTerraform::TOOL_COUNT] = {};
    LLPanel* mMore[WolfTerraform::TOOL_COUNT] = {};
    LLTextBox *mHint = nullptr, *mDrawInfo = nullptr, *mSummary = nullptr, *mStatus = nullptr, *mHistoryInfo = nullptr;
    LLTextBox *mRampInfo = nullptr, *mBeachInfo = nullptr;
    LLButton *mUndoPoint = nullptr, *mStartOver = nullptr, *mApply = nullptr, *mStop = nullptr, *mUndo = nullptr, *mRedo = nullptr;
    LLButton *mLevelPick = nullptr, *mLevelWater = nullptr;
    LLCheckBoxCtrl *mMoreCheck = nullptr, *mLevelUse = nullptr, *mRampTyped = nullptr, *mRiverSea = nullptr;
    LLSpinCtrl *mLevelHeight = nullptr, *mRampWidth = nullptr, *mRampShoulder = nullptr, *mRampA = nullptr, *mRampB = nullptr;
    LLSpinCtrl *mTerraceStep = nullptr, *mRoadWidth = nullptr, *mRoadShoulder = nullptr, *mRoadMaxGrade = nullptr;
    LLSpinCtrl *mRiverWidth = nullptr, *mRiverDepth = nullptr, *mRiverBanks = nullptr;
    LLSpinCtrl *mBeachSlope = nullptr, *mBeachAbove = nullptr, *mBeachBelow = nullptr, *mBeachEdge = nullptr;
    LLSliderCtrl *mLevelEdge = nullptr, *mTerraceSharp = nullptr, *mTerraceEdge = nullptr, *mSmoothRadius = nullptr, *mSmoothStrength = nullptr;
    LLRadioGroup* mRoadGrade = nullptr;
    LLRadioGroup* mShape = nullptr;             // [ticket 355] "square" / "round"
    LLSliderCtrl* mRoundSoft = nullptr;         // [ticket 355] Soft rim %, Round only
    LLTextBox* mShapeNote = nullptr;            // [ticket 355] the line tools' note in its place
    LLScrollListCtrl* mHistory = nullptr;
    WolfDigProgress* mDig = nullptr;
};

#endif // WOLF_FLOATER_TERRAFORM_H
