/**
 * @file wolffloaterterraform.cpp
 * @brief WolfViewer: the Terraform window.
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

// [WOLF TERRAFORM 2026-10-23] See wolffloaterterraform.h, wolfterraform.h and floater_wolf_terraform.xml.

#include "llviewerprecompiledheaders.h"

#include "wolffloaterterraform.h"

#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "lldraghandle.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llpanel.h"
#include "llparcel.h"
#include "llradiogroup.h"
#include "llrender.h"
#include "llscrolllistctrl.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "llsurface.h"
#include "lltextbox.h"
#include "lltoolmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "wolfburrow.h"
#include "wolfdigprogress.h"
#include "wolfgrid.h"
#include "wolfpaint.h"
#include "wolfterrainwriter.h"

using namespace WolfTerraform;
using WolfTerraformMath::Grid;
using WolfTerraformMath::has;

namespace
{
    // the window's own: the plan is worked out again at most this often while a setting or a marker is being moved, seconds
    constexpr F64 PLAN_SECS = 0.2;
    // the preview draws at most about this many squares (a bigger area is drawn in blocks), so a 512 m job stays smooth to draw
    constexpr S32 PREVIEW_SQUARES = 40000;
    // a second click closer than this to the last is the second press of a double-click (the Tunnel tool's SEG_MIN_M,
    // wolffloatertunnels.cpp)
    constexpr F32 SAME_CLICK_M = 0.5f;
    // Base and More options heights (floater_wolf_terraform.xml: 620 high - 596 until the brush shape row, ticket 355 - the More
    // options panels 60 high at its foot)
    constexpr S32 BASE_H = 620, MORE_H = 64;
    // [ticket 355] the brush shape row's values (floater_wolf_terraform.xml tf_shape radio_item value=) and its settings
    // (app_settings/settings.xml WolfTerraformBrushShape, WolfTerraformRoundSoft)
    const char* const SHAPE_SQUARE = "square";
    const char* const SHAPE_ROUND = "round";
    // the round outline's pieces round the whole rim (one every few metres would show corners on a small circle)
    constexpr S32 OVAL_PARTS = 96;

    const char* const TOOL_KEY[TOOL_COUNT] = { "level", "ramp", "terrace", "smooth", "road", "river", "beach" };
    const char* const TOOL_WORD[TOOL_COUNT] = { "Level", "Ramp", "Terrace", "Smooth", "Road", "River", "Beach" };

    // the preview's colours (in the world, not text): DESIGN.md 3.3 "red where ground will be cut, green where it will be filled,
    // and grey stripes over cells your rights do not cover" - and the summary line says it in words too (never colour alone)
    const LLColor4 PREVIEW_CUT(0.95f, 0.25f, 0.20f, 1.f);
    const LLColor4 PREVIEW_FILL(0.30f, 0.85f, 0.40f, 1.f);
    const LLColor4 PREVIEW_NOT_MINE(0.60f, 0.60f, 0.60f, 1.f);
    // the tiles' pictures (decoration on the stone, not text)
    const LLColor4 TILE_WATER(0.35f, 0.62f, 0.90f, 1.f);
    const LLColor4 TILE_SAND(0.90f, 0.80f, 0.55f, 1.f);
    const LLColor4 TILE_EARTH(0.55f, 0.40f, 0.24f, 1.f);
}

WolfFloaterTerraform::WolfFloaterTerraform(const LLSD& key) : LLFloater(key) {}

WolfFloaterTerraform::~WolfFloaterTerraform()
{
    if (LLEditMenuHandler::gEditMenuHandler == this) LLEditMenuHandler::gEditMenuHandler = nullptr;
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
}

bool WolfFloaterTerraform::postBuild()
{
    for (S32 t = 0; t < TOOL_COUNT; ++t)
    {
        mTiles[t] = getChild<LLButton>(std::string("tf_tool_") + TOOL_KEY[t]);
        mSimple[t] = getChild<LLPanel>(std::string("tf_simple_") + TOOL_KEY[t]);
        mMore[t] = getChild<LLPanel>(std::string("tf_more_") + TOOL_KEY[t]);
        const ETool tool = (ETool)t;
        mTiles[t]->setClickedCallback([this, tool](LLUICtrl*, const LLSD&) { pickTool(tool); });
    }
    mHint = getChild<LLTextBox>("tf_hint");
    mDrawInfo = getChild<LLTextBox>("tf_draw_info");
    mSummary = getChild<LLTextBox>("tf_summary");
    mStatus = getChild<LLTextBox>("tf_status");
    mHistoryInfo = getChild<LLTextBox>("tf_history_info");
    mRampInfo = getChild<LLTextBox>("tf_ramp_info");
    mBeachInfo = getChild<LLTextBox>("tf_beach_info");
    mUndoPoint = getChild<LLButton>("tf_undo_point");
    mStartOver = getChild<LLButton>("tf_start_over");
    mApply = getChild<LLButton>("tf_apply");
    mStop = getChild<LLButton>("tf_stop");
    mUndo = getChild<LLButton>("tf_undo");
    mRedo = getChild<LLButton>("tf_redo");
    mLevelPick = getChild<LLButton>("tf_level_pick");
    mLevelWater = getChild<LLButton>("tf_level_water");
    mMoreCheck = getChild<LLCheckBoxCtrl>("tf_more_check");
    mLevelUse = getChild<LLCheckBoxCtrl>("tf_level_use");
    mRampTyped = getChild<LLCheckBoxCtrl>("tf_ramp_typed");
    mRiverSea = getChild<LLCheckBoxCtrl>("tf_river_sea");
    mLevelHeight = getChild<LLSpinCtrl>("tf_level_height");
    mRampWidth = getChild<LLSpinCtrl>("tf_ramp_width");
    mRampShoulder = getChild<LLSpinCtrl>("tf_ramp_shoulder");
    mRampA = getChild<LLSpinCtrl>("tf_ramp_a");
    mRampB = getChild<LLSpinCtrl>("tf_ramp_b");
    mTerraceStep = getChild<LLSpinCtrl>("tf_terrace_step");
    mRoadWidth = getChild<LLSpinCtrl>("tf_road_width");
    mRoadShoulder = getChild<LLSpinCtrl>("tf_road_shoulder");
    mRoadMaxGrade = getChild<LLSpinCtrl>("tf_road_maxgrade");
    mRiverWidth = getChild<LLSpinCtrl>("tf_river_width");
    mRiverDepth = getChild<LLSpinCtrl>("tf_river_depth");
    mRiverBanks = getChild<LLSpinCtrl>("tf_river_banks");
    mBeachSlope = getChild<LLSpinCtrl>("tf_beach_slope");
    mBeachAbove = getChild<LLSpinCtrl>("tf_beach_above");
    mBeachBelow = getChild<LLSpinCtrl>("tf_beach_below");
    mBeachEdge = getChild<LLSpinCtrl>("tf_beach_edge");
    mLevelEdge = getChild<LLSliderCtrl>("tf_level_edge");
    mTerraceSharp = getChild<LLSliderCtrl>("tf_terrace_sharp");
    mTerraceEdge = getChild<LLSliderCtrl>("tf_terrace_edge");
    mSmoothRadius = getChild<LLSliderCtrl>("tf_smooth_radius");
    mSmoothStrength = getChild<LLSliderCtrl>("tf_smooth_strength");
    mRoadGrade = getChild<LLRadioGroup>("tf_road_grade");
    mHistory = getChild<LLScrollListCtrl>("tf_history");
    mRoadGrade->setValue("follow");
    // [ticket 355] the brush shape, as it was left (Square, the whole box, unless Round was chosen)
    mShape = getChild<LLRadioGroup>("tf_shape");
    mRoundSoft = getChild<LLSliderCtrl>("tf_round_soft");
    mShapeNote = getChild<LLTextBox>("tf_shape_note");
    mShape->setValue(gSavedSettings.getString("WolfTerraformBrushShape") == SHAPE_ROUND ? SHAPE_ROUND : SHAPE_SQUARE);
    mRoundSoft->setValue(llclamp(gSavedSettings.getF32("WolfTerraformRoundSoft"), 0.f, 100.f));
    mShape->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("WolfTerraformBrushShape", mShape->getValue().asString() == SHAPE_ROUND ? SHAPE_ROUND : SHAPE_SQUARE);
        updateShapeControls();
        changed();
    });
    mRoundSoft->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setF32("WolfTerraformRoundSoft", llclamp(mRoundSoft->getValueF32(), 0.f, 100.f));
        changed();
    });

    // every setting works the plan out again
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mLevelUse, mLevelHeight, mLevelEdge, mRampWidth, mRampShoulder, mRampTyped, mRampA,
                                                          mRampB, mTerraceStep, mTerraceSharp, mTerraceEdge, mSmoothRadius, mSmoothStrength,
                                                          mRoadWidth, mRoadGrade, mRoadShoulder, mRoadMaxGrade, mRiverWidth, mRiverDepth,
                                                          mRiverSea, mRiverBanks, mBeachSlope, mBeachAbove, mBeachBelow, mBeachEdge })
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { changed(); });
    mLevelPick->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        mPicking = mLevelPick->getToggleState();
        setStatus(mPicking ? "Click on the ground: the box goes to the height there." : std::string(), false);
    });
    mLevelWater->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        // DESIGN.md 3.2 #1: "water + 0.5 m for a beach line" (LLViewerRegion::getWaterHeight, the region's water)
        mLevelHeight->set(ll_round((rgn->getWaterHeight() + 0.5f) * 100.f) / 100.f);
        mLevelUse->set(true);
        changed();
    });
    mUndoPoint->setClickedCallback([this](LLUICtrl*, const LLSD&) { undoPoint(); });
    mStartOver->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        clearDrawing();
        setStatus("The points are cleared - click on the ground again.", false);
    });
    mApply->setClickedCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
    mStop->setClickedCallback([](LLUICtrl*, const LLSD&) { Jobs::instance().cancel(); });
    mUndo->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    mRedo->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRedo(); });
    mMoreCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { relayout(); });

    // the painted working plate over the summary and the status line while a job runs or after it stopped (wolfdigprogress.cpp)
    {
        LLRect area = mSummary->getRect();
        area.unionWith(mStatus->getRect());
        mDig = WolfDigProgress::add(this, area);
    }
    // the painted window behind every control (wolfburrow.cpp)
    WolfBurrow::Sheet::add(this, [this]() { paintBurrow(); });
    if (getDragHandle()) sendChildToBack(getDragHandle());   // empty paper still drags the window
    pickTool(TOOL_LEVEL);
    relayout();
    return true;
}

void WolfFloaterTerraform::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] checked on open, as the Tunnel tool (no polling): a Wolf tool, Wolf Territories only
    if (!WolfGrid::isOnWolfTerritories())
    {
        setStatus("Terraform works on Wolf Territories regions only.", true);
        mPenWanted = false;
        updateControls();
        return;
    }
    LLEditMenuHandler::gEditMenuHandler = this;   // Edit > Undo / Redo (Ctrl+Z / Ctrl+Y) are this window's history
    mPenWanted = true;
    mStartPending = true;
    // one drawing tool at a time: the Tunnels window uses the same pen
    if (LLFloaterReg::instanceVisible("wolf_tunnels"))
    {
        LLFloaterReg::hideInstance("wolf_tunnels");
        setStatus("The Tunnels window was closed: it draws with the same pen.", false);
    }
    changed();
    fillHistory();
    updateControls();
}

void WolfFloaterTerraform::onClose(bool app_quitting)
{
    // nothing waits and nothing asks at quit (llappviewer.cpp idleShutdown waits only for modal dialogs and visible floaters)
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
    if (LLEditMenuHandler::gEditMenuHandler == this) LLEditMenuHandler::gEditMenuHandler = nullptr;
    mPenWanted = false;
    mPicking = false;
    if (mLevelPick) mLevelPick->setToggleState(false);
    (void)app_quitting;
}

void WolfFloaterTerraform::onFocusReceived()
{
    LLFloater::onFocusReceived();
    LLEditMenuHandler::gEditMenuHandler = this;
}

// ---- the tools ----------------------------------------------------------------------------------------------------------------

void WolfFloaterTerraform::pickTool(ETool tool)
{
    const bool was_route = isRouteTool(mTool), now_route = isRouteTool(tool);
    mTool = tool;
    // the clicks stay when the new tool takes them the same way (two corners or two ends; a line), else a new drawing
    if (was_route != now_route) mClicks.clear();
    if (!now_route && mClicks.size() > 2) mClicks.resize(2);
    if (tool != TOOL_LEVEL && mPicking) { mPicking = false; mLevelPick->setToggleState(false); }
    for (S32 t = 0; t < TOOL_COUNT; ++t)
    {
        mTiles[t]->setToggleState(t == tool);
        mSimple[t]->setVisible(t == tool);
    }
    updateShapeControls();
    relayout();
    changed();
}

bool WolfFloaterTerraform::roundBrush() const
{
    return isBoxTool(mTool) && mShape && mShape->getValue().asString() == SHAPE_ROUND;
}

void WolfFloaterTerraform::updateShapeControls()
{
    if (!mShape) return;
    const bool box = isBoxTool(mTool);
    mShape->setEnabled(box && !Jobs::instance().busy());
    mRoundSoft->setVisible(box && roundBrush());
    mShapeNote->setVisible(!box);
}

WolfTerraform::Params WolfFloaterTerraform::readParams() const
{
    WolfTerraform::Params p;
    p.levelHeight = mLevelUse->get() ? mLevelHeight->getValueF32() : WolfTerraformMath::nan();
    p.levelEdge = mLevelEdge->getValueF32();
    p.rampWidth = mRampWidth->getValueF32();
    p.rampShoulder = mRampShoulder->getValueF32();
    p.rampHeightA = mRampTyped->get() ? mRampA->getValueF32() : WolfTerraformMath::nan();
    p.rampHeightB = mRampTyped->get() ? mRampB->getValueF32() : WolfTerraformMath::nan();
    p.terraceStep = mTerraceStep->getValueF32();
    p.terraceSharpness = mTerraceSharp->getValueF32();
    p.terraceEdge = mTerraceEdge->getValueF32();
    p.smoothRadius = (S32)ll_round(mSmoothRadius->getValueF32());
    p.smoothStrength = mSmoothStrength->getValueF32() / 100.f;
    p.roadWidth = mRoadWidth->getValueF32();
    p.roadShoulder = mRoadShoulder->getValueF32();
    p.roadMaxGrade = mRoadMaxGrade->getValueF32() / 100.f;
    p.roadFollow = mRoadGrade->getValue().asString() != "even";
    p.riverWidth = mRiverWidth->getValueF32();
    p.riverDepth = mRiverDepth->getValueF32();
    p.riverBanks = mRiverBanks->getValueF32();
    p.riverToSea = mRiverSea->get();
    p.beachSlope = mBeachSlope->getValueF32() / 100.f;
    p.beachAbove = mBeachAbove->getValueF32();
    p.beachBelow = mBeachBelow->getValueF32();
    p.beachEdge = mBeachEdge->getValueF32();
    p.round = roundBrush();
    p.roundSoft = llclamp(mRoundSoft->getValueF32(), 0.f, 100.f) / 100.f;
    return p;
}

void WolfFloaterTerraform::changed()
{
    mPlanDirty = true;
}

void WolfFloaterTerraform::replan()
{
    mPlanDirty = false;
    mPlanAt = LLFrameTimer::getElapsedSeconds();
    mPlan = makePlan(mTool, mClicks, readParams());
    buildPreview();
    // the summary: what it does, in words as well as colour (DESIGN.md 3.3)
    std::string s;
    if (!mPlan.mError.empty()) s = mPlan.mError;
    else if (mPlan.ok())
    {
        s = mPlan.mLabel + ". ";
        if (mPlan.mMaxCut > WRITE_SKIP_M && mPlan.mMaxFill > WRITE_SKIP_M)
            s += llformat("Cuts up to %.1f m (red) and fills up to %.1f m (green)", mPlan.mMaxCut, mPlan.mMaxFill);
        else if (mPlan.mMaxCut > WRITE_SKIP_M) s += llformat("Cuts up to %.1f m (red)", mPlan.mMaxCut);
        else s += llformat("Fills up to %.1f m (green)", mPlan.mMaxFill);
        s += llformat(" over %d m²: %d flood%s, %s.", mPlan.mCells, (S32)mPlan.mRuns.size(), mPlan.mRuns.size() == 1 ? "" : "s",
                      timeText(mPlan.mRuns.size()).c_str());
        if (mPlan.mNotYours > 0)
            s += llformat(" %d m² (grey stripes) is not land this viewer knows to be yours: the region decides, and leaves it as it is if you may not change it.",
                          mPlan.mNotYours);
        for (const std::string& n : mPlan.mNotes) s += " " + n;
    }
    mSummary->setText(s);
    mSummary->setColor(mPlan.mError.empty() ? WolfBurrow::CREAM : WolfBurrow::ERR_INK);
    // the tools' own lines
    if (mTool == TOOL_RAMP && mClicks.size() >= 2 && mPlan.mError.empty() && mPlan.mTarget.cells())
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        F32 za = 0.f, zb = 0.f;
        const WolfTerraform::Params p = readParams();
        const bool a_ok = has(p.rampHeightA) ? (za = p.rampHeightA, true)
                        : knownHeight(rgn, (S32)ll_round(mClicks[0].mV[0]), (S32)ll_round(mClicks[0].mV[1]), za);
        const bool b_ok = has(p.rampHeightB) ? (zb = p.rampHeightB, true)
                        : knownHeight(rgn, (S32)ll_round(mClicks[1].mV[0]), (S32)ll_round(mClicks[1].mV[1]), zb);
        const F32 g = WolfTerraformMath::grade(mClicks[0].mV[0], mClicks[0].mV[1], za, mClicks[1].mV[0], mClicks[1].mV[1], zb);
        mRampInfo->setText(a_ok && b_ok ? llformat("%.0f m long, %.1f m to %.1f m: %d%% grade", dist_vec(mClicks[0], mClicks[1]), za, zb,
                                                   (S32)ll_round(g * 100.f)) : std::string());
    }
    else mRampInfo->setText(std::string());
    LLViewerRegion* rgn = gAgent.getRegion();
    mBeachInfo->setText(rgn ? llformat("The water here is at %.1f m.", rgn->getWaterHeight()) : std::string());
}

// The preview: a sheet at the target heights over the cells it changes - red where it cuts, green where it fills, grey where the
// land is not known to be yours - in blocks of `step` cells so a big job stays light to draw. Never on the ground itself
// (LLSurface is not touched: the region's terrain stays the only truth).
void WolfFloaterTerraform::buildPreview()
{
    mCut.clear();
    mFill.clear();
    mNotMine.clear();
    mOutline.clear();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const LLSurface& land = rgn->getLand();
    // the box tools' outline on the ground (the route tools' line is the pen's)
    if (isBoxTool(mTool) && mClicks.size() >= 2)
    {
        const WolfTerraformMath::Box b = WolfTerraformMath::boxOf(mClicks[0].mV[0], mClicks[0].mV[1], mClicks[1].mV[0], mClicks[1].mV[1]);
        const F32 x0 = (F32)b.x0 - 0.5f, x1 = (F32)b.x1 + 0.5f, y0 = (F32)b.y0 - 0.5f, y1 = (F32)b.y1 + 0.5f;
        if (roundBrush())
        {
            // [ticket 355] the round brush's rim (wolfterraformmath.h ovalOf: the circle or oval touching the box's sides)
            const WolfTerraformMath::Oval o = WolfTerraformMath::ovalOf(b);
            const F32 tau = 6.2831853f;
            for (S32 i = 0; i < OVAL_PARTS; ++i)
            {
                const F32 a0 = tau * (F32)i / (F32)OVAL_PARTS, a1 = tau * (F32)(i + 1) / (F32)OVAL_PARTS;
                const F32 px0 = o.cx + o.a * cosf(a0), py0 = o.cy + o.b * sinf(a0), px1 = o.cx + o.a * cosf(a1), py1 = o.cy + o.b * sinf(a1);
                mOutline.emplace_back(px0, py0, land.resolveHeightRegion(px0, py0) + 0.15f);
                mOutline.emplace_back(px1, py1, land.resolveHeightRegion(px1, py1) + 0.15f);
            }
        }
        auto edge = [&](F32 ax, F32 ay, F32 bx, F32 by)
        {
            const S32 parts = llclamp((S32)(llmax(fabsf(bx - ax), fabsf(by - ay)) / 2.f), 1, 512);
            for (S32 i = 0; i < parts; ++i)
            {
                const F32 u0 = (F32)i / parts, u1 = (F32)(i + 1) / parts;
                const F32 px0 = ax + (bx - ax) * u0, py0 = ay + (by - ay) * u0, px1 = ax + (bx - ax) * u1, py1 = ay + (by - ay) * u1;
                mOutline.emplace_back(px0, py0, land.resolveHeightRegion(px0, py0) + 0.15f);
                mOutline.emplace_back(px1, py1, land.resolveHeightRegion(px1, py1) + 0.15f);
            }
        };
        if (!roundBrush())
        {
            edge(x0, y0, x1, y0);
            edge(x1, y0, x1, y1);
            edge(x1, y1, x0, y1);
            edge(x0, y1, x0, y0);
        }
    }
    const Grid& t = mPlan.mTarget;
    const Grid& g = mPlan.mBefore;
    if (!t.cells() || mPlan.mRegion != rgn->getRegionID()) return;
    const S32 step = llmax(1, (S32)std::ceil(std::sqrt((F32)llmax(1, mPlan.mCells) / (F32)PREVIEW_SQUARES)));
    LLViewerParcelOverlay* overlay = rgn->getParcelOverlay();
    const bool manager = rgn->canManageEstate();
    for (S32 y = t.y0; y < t.y0 + t.h; y += step)
        for (S32 x = t.x0; x < t.x0 + t.w; x += step)
        {
            const F32 tv = t.at(x, y), gv = g.at(x, y);
            if (!has(tv) || !has(gv) || fabsf(tv - gv) <= WRITE_SKIP_M) continue;
            // the block's corners at their target height (the ground where a corner is left alone)
            const F32 h = (F32)step * 0.5f;
            auto zat = [&](S32 cx, S32 cy)
            {
                const F32 a = t.at(cx, cy);
                return has(a) ? a : land.resolveHeightRegion((F32)cx, (F32)cy);
            };
            const LLVector3 p0((F32)x - h, (F32)y - h, zat(x, y)), p1((F32)x + h, (F32)y - h, zat(x + step, y));
            const LLVector3 p2((F32)x + h, (F32)y + h, zat(x + step, y + step)), p3((F32)x - h, (F32)y + h, zat(x, y + step));
            std::vector<LLVector3>& out = tv < gv ? mCut : mFill;
            out.push_back(p0); out.push_back(p1); out.push_back(p2);
            out.push_back(p0); out.push_back(p2); out.push_back(p3);
            // grey stripes (every other diagonal band of blocks) where the land is not known to be yours
            const bool not_mine = !manager && (!overlay || overlay->ownership(LLVector3((F32)x + 0.5f, (F32)y + 0.5f, 0.f)) != PARCEL_SELF);
            if (not_mine && (((x + y) / (2 * step)) % 2) == 0)
            {
                const LLVector3 lift(0.f, 0.f, 0.05f);
                mNotMine.push_back(p0 + lift); mNotMine.push_back(p1 + lift); mNotMine.push_back(p2 + lift);
                mNotMine.push_back(p0 + lift); mNotMine.push_back(p2 + lift); mNotMine.push_back(p3 + lift);
            }
        }
}

// Source: wolffloatertunnels.cpp WolfToolTunnel::render (the preview tube: faint through the ground, stronger in the open;
// LLGLDepthTest(enabled, write); LLGLDisable(GL_CULL_FACE)). Called by the pen's render with the blend already on.
void WolfFloaterTerraform::renderRouteExtras(LLViewerRegion* rgn)
{
    if (!rgn || !getVisible() || isMinimized() || rgn->getRegionID() != mRegionAt) return;
    auto draw = [&](const std::vector<LLVector3>& v, U8 mode)
    {
        if (v.empty()) return;
        gGL.begin(mode);
        for (const LLVector3& p : v) gGL.vertex3fv(rgn->getPosAgentFromRegion(p).mV);
        gGL.end();
    };
    LLGLDisable cull(GL_CULL_FACE);
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);   // through the ground: faint (a cut lies under it)
        gGL.color4fv(WolfPaint::withAlpha(PREVIEW_CUT, 0.18f).mV);
        draw(mCut, LLRender::TRIANGLES);
        gGL.color4fv(WolfPaint::withAlpha(PREVIEW_FILL, 0.12f).mV);
        draw(mFill, LLRender::TRIANGLES);
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);    // in the open: stronger
        gGL.color4fv(WolfPaint::withAlpha(PREVIEW_CUT, 0.40f).mV);
        draw(mCut, LLRender::TRIANGLES);
        gGL.color4fv(WolfPaint::withAlpha(PREVIEW_FILL, 0.40f).mV);
        draw(mFill, LLRender::TRIANGLES);
        gGL.color4fv(WolfPaint::withAlpha(PREVIEW_NOT_MINE, 0.55f).mV);
        draw(mNotMine, LLRender::TRIANGLES);
    }
    LLGLDepthTest depth(GL_FALSE);
    gGL.setLineWidth(2.f);
    gGL.color4f(1.f, 1.f, 1.f, 0.85f);
    draw(mOutline, LLRender::LINES);
    gGL.setLineWidth(1.f);
}

// ---- the pen's calls (WolfRouteHost) -----------------------------------------------------------------------------------------

bool WolfFloaterTerraform::routeEditing() const
{
    return getVisible() && !isMinimized() && WolfGrid::isOnWolfTerritories();
}

void WolfFloaterTerraform::clickAt(F32 x, F32 y)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    if (mPicking)
    {
        mPicking = false;
        mLevelPick->setToggleState(false);
        F32 z;
        if (!knownHeight(rgn, (S32)ll_round(x), (S32)ll_round(y), z)) { setStatus("That ground has not reached this viewer yet - click nearer.", true); return; }
        mLevelHeight->set(ll_round(z * 100.f) / 100.f);
        mLevelUse->set(true);
        setStatus(llformat("Level to %.2f m: the ground where you clicked.", z), false);
        changed();
        return;
    }
    if (Jobs::instance().busy()) { setStatus("Wait for the job to finish before drawing the next one.", true); return; }
    if (mRegionAt != rgn->getRegionID()) { mClicks.clear(); mRegionAt = rgn->getRegionID(); }
    const LLVector2 p(x, y);
    if (!mClicks.empty() && dist_vec(mClicks.back(), p) < SAME_CLICK_M) return;   // the second press of a double-click
    const S32 most = maxClicks(mTool);
    if (most > 0 && (S32)mClicks.size() >= most)
    {
        setStatus(isBoxTool(mTool) ? "The box has both corners - drag a corner to change it, or press Start over."
                                   : "The ramp has both ends - drag an end to move it, or press Start over.", false);
        return;
    }
    mClicks.push_back(p);
    setStatus(std::string(), false);
    changed();
}

void WolfFloaterTerraform::moveClick(S32 k, F32 x, F32 y, bool done)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || k < 0 || (size_t)k >= mClicks.size()) return;
    const F32 w = rgn->getWidth();
    mClicks[k].set(llclamp(x, 0.f, w - 1.f), llclamp(y, 0.f, w - 1.f));
    changed();
    if (done) replan();
}

void WolfFloaterTerraform::undoPoint()
{
    if (mClicks.empty()) return;
    mClicks.pop_back();
    setStatus(mClicks.empty() ? std::string("No points left - click on the ground to start.") : std::string("The last point was taken back."), false);
    changed();
}

void WolfFloaterTerraform::removeClick(S32 k)
{
    if (k < 0 || (size_t)k >= mClicks.size()) return;
    if (isRouteTool(mTool) && mClicks.size() <= 2)
    {
        setStatus("A line needs its two ends - drag them instead, or press Undo last point.", true);
        return;
    }
    mClicks.erase(mClicks.begin() + k);
    setStatus(isRouteTool(mTool) ? llformat("Point %d was taken out.", k + 1) : std::string("That corner was taken out - click where it goes."), false);
    changed();
}

// Source: wolffloatertunnels.cpp WolfFloaterTunnels::insertClickNear (the piece of the line nearest the click gets the new point).
void WolfFloaterTerraform::insertClickNear(F32 x, F32 y)
{
    if (!isRouteTool(mTool)) { setStatus("Ctrl+click puts a point into a road or a river's line.", false); return; }
    if (mClicks.size() < 2) { clickAt(x, y); return; }
    size_t best = 1;
    F32 best_d = 1e30f;
    const LLVector2 q(x, y);
    for (size_t i = 1; i < mClicks.size(); ++i)
    {
        const LLVector2 a = mClicks[i - 1], v = mClicks[i] - mClicks[i - 1];
        const F32 l2 = v.lengthSquared();
        const F32 u = l2 > 1e-9f ? llclamp(((q - a) * v) / l2, 0.f, 1.f) : 0.f;
        const F32 d = dist_vec(a + v * u, q);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best_d > llmax(cursorRadius() * 2.f, 6.f)) { setStatus("Ctrl+click on the line to put a point there.", true); return; }
    if (dist_vec(q, mClicks[best - 1]) < SAME_CLICK_M || dist_vec(q, mClicks[best]) < SAME_CLICK_M) return;
    mClicks.insert(mClicks.begin() + best, q);
    setStatus(llformat("A point was put in between point %d and point %d - drag it where you want it.", (S32)best, (S32)best + 1), false);
    changed();
}

void WolfFloaterTerraform::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(error ? WolfBurrow::ERR_INK : WolfBurrow::CREAM);   // fixed colours on the painted plate
}

// A refusal stays on screen and can be copied: the red plate (never dismissed by itself) and the status line.
void WolfFloaterTerraform::refuse(const std::string& why)
{
    setStatus(why, true);
    if (mDig) mDig->showFailed(why);
}

void WolfFloaterTerraform::penStopped()
{
    updateControls();
}

// Source: wolffloatertunnels.cpp WolfFloaterTunnels::planeHeight (from under the ground a click lands on a level plane).
F32 WolfFloaterTerraform::planeHeight() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return 0.f;
    if (!mClicks.empty()) return rgn->getLand().resolveHeightRegion(mClicks.back().mV[0], mClicks.back().mV[1]);
    return rgn->getPosRegionFromAgent(gAgent.getPositionAgent()).mV[VZ] - 1.f;   // about where the avatar stands
}

std::string WolfFloaterTerraform::noGroundText(bool underground) const
{
    return underground ? "Point at the ground inside this region (you are under the ground)."
                       : "Click on the ground of the region you are in - Terraform shapes one region at a time.";
}

std::string WolfFloaterTerraform::escapeText() const
{
    return "You are drawing on the ground. Backspace takes the last point back; closing the Terraform window stops.";
}

F32 WolfFloaterTerraform::cursorRadius() const
{
    const WolfTerraform::Params p = readParams();
    switch (mTool)
    {
    case TOOL_RAMP: return llmax(0.5f, p.rampWidth * 0.5f);
    case TOOL_ROAD: return llmax(0.5f, p.roadWidth * 0.5f);
    case TOOL_RIVER: return llmax(0.5f, p.riverWidth * 0.5f);
    default: return 0.5f;
    }
}

std::string WolfFloaterTerraform::markerLabel(size_t k) const
{
    if (mTool == TOOL_RAMP) return k == 0 ? "A" : "B";
    if (isBoxTool(mTool)) return k == 0 ? "Corner 1" : "Corner 2";
    if (k == 0) return mTool == TOOL_RIVER ? "Source" : "Start";
    if (mClicks.size() >= 2 && k + 1 == mClicks.size()) return mTool == TOOL_RIVER ? "Mouth" : "End";
    return llformat("%d", (S32)k + 1);
}

bool WolfFloaterTerraform::joinClicks() const
{
    return !isBoxTool(mTool);
}

std::string WolfFloaterTerraform::routeHint() const
{
    const size_t n = mClicks.size();
    if (mPicking) return "Click on the ground to take its height for Level.";
    if (Jobs::instance().busy()) return Jobs::instance().status();
    switch (mTool)
    {
    case TOOL_LEVEL:
    case TOOL_TERRACE:
    case TOOL_SMOOTH:
    case TOOL_BEACH:
    {
        static const char* WHAT[TOOL_COUNT] = { "level", "", "terrace", "smooth", "", "", "make a beach in (across the water's edge)" };
        if (roundBrush())
        {
            // [ticket 355] the round brush is drawn by the box it fits in
            if (n == 0) return llformat("Click one corner of a box round the circle to %s.", WHAT[mTool]);
            if (n == 1) return "Now click the opposite corner: the circle fits inside the box.";
            return "Drag a corner to change the circle. Red will be cut, green filled: press Apply in the Terraform window.";
        }
        if (n == 0) return llformat("Click one corner of the area to %s.", WHAT[mTool]);
        if (n == 1) return "Now click the opposite corner.";
        return "Drag a corner to change the box. Red will be cut, green filled: press Apply in the Terraform window.";
    }
    case TOOL_RAMP:
        if (n == 0) return "Click where the ramp starts (A).";
        if (n == 1) return "Now click where it ends (B).";
        return "Drag A or B to move an end. Press Apply in the Terraform window.";
    default:
        if (n == 0) return mTool == TOOL_RIVER ? "Click where the river starts (its source, the high end)." : "Click where the road starts.";
        if (n == 1) return "Click the bends and the far end. Ctrl+click on the line adds a point, on a marker takes it out.";
        return "Keep clicking to go on, drag a marker to move it. Press Apply in the Terraform window.";
    }
}

// ---- Edit > Undo / Redo --------------------------------------------------------------------------------------------------------

void WolfFloaterTerraform::undo() { onUndo(); }
void WolfFloaterTerraform::redo() { onRedo(); }

bool WolfFloaterTerraform::canUndo() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn && !Jobs::instance().busy() && Jobs::instance().done(rgn->getRegionID()) > 0;
}

bool WolfFloaterTerraform::canRedo() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn && !Jobs::instance().busy() && Jobs::instance().done(rgn->getRegionID()) < Jobs::instance().steps(rgn->getRegionID()).size();
}

void WolfFloaterTerraform::onApply()
{
    // worked out again now, from the ground as it is now (the preview may be a moment old)
    replan();
    std::string why;
    if (!Jobs::instance().apply(mPlan, why)) { refuse(why); return; }
    // the drawing goes (the job has its own copy): the preview would show the old ground against the new
    mClicks.clear();
    mSawJob = true;
    setStatus(Jobs::instance().status(), false);
    changed();
    updateControls();
}

void WolfFloaterTerraform::onUndo()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    std::string why;
    if (!rgn || !Jobs::instance().undo(rgn->getRegionID(), why)) { refuse(why.empty() ? std::string("You are not in a region yet.") : why); return; }
    mSawJob = true;
    updateControls();
}

void WolfFloaterTerraform::onRedo()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    std::string why;
    if (!rgn || !Jobs::instance().redo(rgn->getRegionID(), why)) { refuse(why.empty() ? std::string("You are not in a region yet.") : why); return; }
    mSawJob = true;
    updateControls();
}

void WolfFloaterTerraform::clearDrawing()
{
    mClicks.clear();
    mPicking = false;
    mLevelPick->setToggleState(false);
    changed();
}

// ---- the window ---------------------------------------------------------------------------------------------------------------

// Source: wolffloatertunnels.cpp WolfFloaterTunnels::penAllowed / keepPen.
bool WolfFloaterTerraform::penAllowed() const
{
    if (!mPenWanted || !getVisible() || isMinimized() || !WolfGrid::isOnWolfTerritories() || LLApp::isExiting()) return false;
    // the Tunnels window draws with the same pen
    if (LLFloaterReg::instanceVisible("wolf_tunnels")) return false;
    return gAgent.getRegion() != nullptr;
}

void WolfFloaterTerraform::keepPen()
{
    if (!WolfToolTunnel::instanceExists() && !mPenWanted) return;
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (!penAllowed())
    {
        if (t->pen() != WolfToolTunnel::PEN_NONE && t->hostIs(this)) t->cancel();
        return;
    }
    if (t->current()) return;
    // Not while something else has the mouse: the Build window (its focus drops the pen: llfloatertools.cpp onFocusReceived ->
    // lltoolmgr.cpp setCurrentTool clears the transient tool), another transient tool, a mouse capture, mouselook.
    if (gFloaterTools && gFloaterTools->getVisible()) return;
    if (LLToolMgr::getInstance()->usingTransientTool() || gFocusMgr.getMouseCapture() || gAgentCamera.cameraMouselook()) return;
    t->begin(getHandle(), WolfToolTunnel::PEN_ROUTE);
}

void WolfFloaterTerraform::draw()
{
    if (mStartPending && !LLApp::isExiting())
    {
        // Here and not in onOpen: the window may be opened by the Build window's own Terraform... button, and that window's focus
        // would put the pen down at once (wolffloatertunnels.cpp WolfFloaterTunnels::draw, the same).
        mStartPending = false;
        if (gFloaterTools && gFloaterTools->getVisible())
        {
            gFloaterTools->closeFloater();
            setStatus("The Build window was closed so that your clicks draw on the ground.", false);
        }
    }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && rgn->getRegionID() != mRegionAt && !Jobs::instance().busy())
    {
        if (!mClicks.empty() && mRegionAt.notNull()) setStatus("You are on another region now: the drawing was put away.", true);
        mRegionAt = rgn->getRegionID();
        mClicks.clear();
        changed();
        fillHistory();
    }
    if (mPlanDirty && LLFrameTimer::getElapsedSeconds() - mPlanAt >= PLAN_SECS) replan();
    Jobs& jobs = Jobs::instance();
    if (jobs.busy() && mDig) mDig->showWork(jobs.status(), jobs.progress());
    if (jobs.busy() && mStatus->getText() != jobs.status()) setStatus(jobs.status(), false);
    if (mShownSerial != jobs.serial())
    {
        mShownSerial = jobs.serial();
        setStatus(jobs.lastText(), !jobs.lastOk());
        // a failure stays on the red plate, selectable and copyable, until OK (never dismissed by itself)
        if (mDig && mSawJob) { if (jobs.lastOk()) mDig->showDone(jobs.lastText()); else mDig->showFailed(jobs.lastText()); }
        mSawJob = false;
        changed();   // the ground has changed: the next preview is worked out from it
    }
    fillHistory();
    updateControls();
    keepPen();
    LLFloater::draw();
}

void WolfFloaterTerraform::updateControls()
{
    const bool busy = Jobs::instance().busy();
    const bool on_wolf = WolfGrid::isOnWolfTerritories();
    mApply->setEnabled(on_wolf && !busy && mPlan.ok());
    mStop->setVisible(busy);
    mStop->setEnabled(busy && !Jobs::instance().cancelling());
    mUndoPoint->setEnabled(!mClicks.empty());
    mStartOver->setEnabled(!mClicks.empty() || mPicking);
    mUndo->setEnabled(on_wolf && canUndo());
    mRedo->setEnabled(on_wolf && canRedo());
    for (LLButton* b : mTiles) b->setEnabled(!busy);
    updateShapeControls();
    mLevelPick->setEnabled(!busy);
    mLevelWater->setEnabled(!busy);
    mHint->setText(on_wolf ? routeHint() : std::string("Terraform works on Wolf Territories regions only."));
    mDrawInfo->setText(mClicks.empty() ? std::string() : llformat("%d point%s", (S32)mClicks.size(), mClicks.size() == 1 ? "" : "s"));
}

// The history list and the line beside Undo: what Undo / Redo would do, and their time, or why they are greyed (DESIGN.md 0:
// Undo must be honest).
void WolfFloaterTerraform::fillHistory()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLUUID region = rgn ? rgn->getRegionID() : LLUUID::null;
    const Jobs& jobs = Jobs::instance();
    const std::vector<Step>& steps = jobs.steps(region);
    const size_t done = jobs.done(region);
    // (the version: a step marked partly done changes neither count)
    if (steps.size() == mShownSteps && done == mShownDone && region == mShownHistoryRegion && jobs.historyVersion() == mShownHistoryVersion) return;
    mShownHistoryVersion = jobs.historyVersion();
    mShownSteps = steps.size();
    mShownDone = done;
    mShownHistoryRegion = region;
    mHistory->deleteAllItems();
    for (size_t i = 0; i < steps.size(); ++i)
    {
        LLSD row;
        row["columns"][0]["column"] = "label";
        row["columns"][0]["value"] = steps[i].mLabel;
        row["columns"][1]["column"] = "state";
        row["columns"][1]["value"] = i >= done ? "undone" : steps[i].mStopped ? "partly done" : "done";
        mHistory->addElement(row);
    }
    std::string info;
    if (done > 0) info = "Undo: " + steps[done - 1].mLabel + ", " + timeText(jobs.undoFloods(region)) + ".";
    else if (done < steps.size()) info = "Redo: " + steps[done].mLabel + ".";
    else info = "Nothing to undo yet: each Apply is kept here (this region, this session, up to 20 steps).";
    mHistoryInfo->setText(info);
}

// Base height with More options closed; More options adds MORE_H at the foot (the window grows down, its top stays).
// Source: wolffloatertunnels.cpp WolfFloaterTunnels::relayout.
void WolfFloaterTerraform::relayout()
{
    const bool more = mMoreCheck->get();
    for (S32 t = 0; t < TOOL_COUNT; ++t) mMore[t]->setVisible(more && t == mTool);
    const S32 want = BASE_H + (more ? MORE_H : 0);
    if (getRect().getHeight() != want)
    {
        LLRect r = getRect();
        r.mBottom = r.mTop - want;
        setShape(r);
    }
    for (S32 t = 0; t < TOOL_COUNT; ++t) mMore[t]->setOrigin(0, want - BASE_H - mMore[t]->getRect().getHeight());
    if (gFloaterView && getParent() == gFloaterView)
    {
        const LLRect snap = gFloaterView->getSnapRect();
        const LLRect r = getRect();
        if (r.mBottom < snap.mBottom)
        {
            const S32 up = llmin(snap.mBottom - r.mBottom, llmax(0, snap.mTop - r.mTop));
            if (up > 0) translate(0, up);
        }
    }
}

// ---- the paint ----------------------------------------------------------------------------------------------------------------

// Who and why: Jimmy wants the tool he is using, what to click next and the main action to stand out. The paper, the header with
// the mole (digging while a job runs), a plate per step under its heading, painted tiles with a small picture and a word, and
// every button's face by weight (Apply amber, Stop and Start over with a red rule). Source: wolffloatertunnels.cpp paintBurrow.
void WolfFloaterTerraform::paintBurrow()
{
    using namespace WolfBurrow;
    const LLRect all = getLocalRect();
    const LLRect client(0, all.mTop - getHeaderHeight(), all.mRight, 0);
    drawSheet(client);
    const S32 L = 4, R = all.mRight - 4;
    drawHeader(LLRect(L, client.mTop - 3, R, client.mTop - 45), "TERRAFORM", "Pick a tool, click on the ground, see it, Apply.",
               Jobs::instance().busy() ? WolfPaint::animSeconds() : 0.f);
    auto wide = [R](LLRect r) { if (!r.isEmpty()) { r.mLeft = L; r.mRight = R; } return r; };
    std::vector<LLView*> first(std::begin(mTiles), std::end(mTiles));
    for (LLView* v : { (LLView*)mHint, (LLView*)mUndoPoint, (LLView*)mStartOver, (LLView*)mDrawInfo, (LLView*)mShape }) first.push_back(v);
    drawSection(wide(around(first, this, 4, 16)), "1  PICK A TOOL, THEN CLICK ON THE GROUND");
    drawSection(wide(around({ mSimple[mTool] }, this, 2, 16)), "2  SETTINGS");
    drawSection(wide(around({ mSummary, mApply, mStop, mStatus }, this, 4, 16)), "3  SEE IT, THEN APPLY");
    drawSection(wide(around({ mHistory, mUndo, mRedo, mHistoryInfo }, this, 4, 16)), "4  HISTORY");
    drawSection(wide(around({ mMoreCheck }, this, 4, 0)), std::string());
    if (mMore[mTool]->getVisible()) drawSection(wide(around({ mMore[mTool] }, this, 2, 16)), "MORE OPTIONS");
    for (S32 t = 0; t < TOOL_COUNT; ++t) paintTile(mTiles[t], (ETool)t);
    drawButtonFace(mApply, this, WEIGHT_PRIMARY);
    for (LLButton* b : { mStop, mStartOver }) drawButtonFace(b, this, WEIGHT_DANGER);
    for (LLButton* b : { mUndoPoint, mUndo, mRedo, mLevelPick, mLevelWater }) drawButtonFace(b, this, WEIGHT_SECONDARY);
}

// A tile: the stone face (amber rule when it is the tool), a small picture of what the tool does and its word under it (CREAM on
// stone 8.1:1 or more, burrow_contrast.py; DIS when greyed, 6.7:1).
void WolfFloaterTerraform::paintTile(LLButton* b, ETool tool) const
{
    using namespace WolfPaint;
    WolfBurrow::drawButtonFace(b, this, WolfBurrow::WEIGHT_SECONDARY);
    if (!b || !b->isInVisibleChain()) return;
    const LLRect r = rectIn(b, this);
    const bool on = b->isInEnabledChain();
    const LLColor4 ink = on ? WolfBurrow::CREAM : WolfBurrow::DIS;
    const LLColor4 acc = on ? WolfBurrow::AMBER : WolfBurrow::DIS;
    const F32 l = (F32)r.mLeft + 9.f, rr = (F32)r.mRight - 9.f, bt = (F32)r.mBottom + 21.f, tp = (F32)r.mTop - 8.f;
    const F32 w = rr - l, h = tp - bt, mid = (l + rr) * 0.5f;
    untextured();
    switch (tool)
    {
    case TOOL_LEVEL:   // hills cut to a level pad
        line(l, bt + h * 0.3f, l + w * 0.2f, bt + h * 0.75f, 2.f, ink);
        line(l + w * 0.2f, bt + h * 0.75f, l + w * 0.3f, bt + h * 0.45f, 2.f, ink);
        line(l + w * 0.3f, bt + h * 0.45f, l + w * 0.7f, bt + h * 0.45f, 3.f, acc);
        line(l + w * 0.7f, bt + h * 0.45f, l + w * 0.85f, bt + h * 0.85f, 2.f, ink);
        line(l + w * 0.85f, bt + h * 0.85f, rr, bt + h * 0.4f, 2.f, ink);
        break;
    case TOOL_RAMP:    // a wedge between two levels
        tri(l, bt, rr, bt, rr, tp - h * 0.1f, withAlpha(TILE_EARTH, on ? 1.f : 0.5f));
        line(l, bt, rr, tp - h * 0.1f, 2.f, acc);
        disc(l + 2.f, bt + 2.f, 2.5f, ink, ink);
        disc(rr - 2.f, tp - h * 0.1f, 2.5f, ink, ink);
        break;
    case TOOL_TERRACE: // steps
        for (S32 i = 0; i < 4; ++i)
        {
            const F32 x0 = l + w * 0.25f * (F32)i, x1 = x0 + w * 0.25f, y = bt + h * 0.25f * (F32)(i + 1);
            line(x0, y, x1, y, 2.f, acc);
            if (i < 3) line(x1, y, x1, y + h * 0.25f, 2.f, ink);
        }
        break;
    case TOOL_SMOOTH:  // a rough line becoming a smooth one
        for (S32 i = 0; i < 12; ++i)
        {
            const F32 u0 = (F32)i / 12.f, u1 = (F32)(i + 1) / 12.f;
            auto yat = [&](F32 u) { const F32 rough = (1.f - u) * (i % 2 ? 0.18f : -0.18f); return bt + h * (0.5f + 0.25f * sinf(u * 6.28f) * u + rough * (1.f - u)); };
            line(l + w * u0, yat(u0), l + w * u1, yat(u1), 2.f, u0 < 0.5f ? ink : acc);
        }
        break;
    case TOOL_ROAD:    // a road going away, with a dashed middle
        line(l + w * 0.1f, bt, mid - w * 0.08f, tp, 2.f, ink);
        line(rr - w * 0.1f, bt, mid + w * 0.08f, tp, 2.f, ink);
        dashed(mid, bt, mid, tp, 2.f, 4.f, 3.f, acc);
        break;
    case TOOL_RIVER:   // a winding stream
        for (S32 i = 0; i < 12; ++i)
        {
            const F32 u0 = (F32)i / 12.f, u1 = (F32)(i + 1) / 12.f;
            line(mid + sinf(u0 * 6.28f) * w * 0.25f, bt + h * u0, mid + sinf(u1 * 6.28f) * w * 0.25f, bt + h * u1, 4.f - 2.f * u0,
                 withAlpha(TILE_WATER, on ? 1.f : 0.5f));
        }
        break;
    case TOOL_BEACH:   // sand sloping into the water
        tri(l, bt + h * 0.75f, rr, bt, l, bt, withAlpha(TILE_SAND, on ? 1.f : 0.5f));
        box(l + w * 0.45f, bt + h * 0.35f, rr, bt, withAlpha(TILE_WATER, on ? 0.8f : 0.4f), withAlpha(TILE_WATER, on ? 0.8f : 0.4f));
        line(l + w * 0.45f, bt + h * 0.35f, rr, bt + h * 0.35f, 2.f, acc);
        break;
    default:
        break;
    }
    text(LLFontGL::getFontSansSerifSmallBold(), TOOL_WORD[tool], mid, (F32)r.mBottom + 11.f, ink, LLFontGL::HCENTER, (F32)r.getWidth() - 4.f);
}
