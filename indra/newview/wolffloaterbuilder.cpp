/**
 * @file wolffloaterbuilder.cpp
 * @brief WolfViewer: the Wolf Builder floater. See wolffloaterbuilder.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaterbuilder.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"        // instanceVisible
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llparcelselection.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"     // LLGridManager
#include "llviewerobject.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "wolfbuilder.h"

namespace
{
    // Paul: "easier snapping to defined grids" - the spacings a builder reaches for. GridResolution is
    // the snap grid's single step (settings.xml "Size of single grid step (meters)").
    const F32 SPACINGS[] = { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f };
    // How far the drawn grid reaches from its centre. GridDrawSize ("Visible extent of 2D snap grid").
    const F32 SIZES[] = { 8.f, 12.f, 16.f, 24.f, 32.f, 48.f, 64.f };

    const F32 REFRESH_SECS = 0.25f;

    std::string with_commas(S32 n)
    {
        std::string s;
        LLResMgr::getInstance()->getIntegerString(s, n);
        return s;
    }

    // Fill a combo with the presets, select the one the setting holds, or add it as a custom entry
    // (the Build floater's Options... window lets a builder type any value).
    void fill_combo(LLComboBox* combo, const F32* presets, size_t count, F32 current, const char* unit)
    {
        combo->removeall();
        S32 match = -1;
        for (size_t i = 0; i < count; ++i)
        {
            combo->add(llformat("%g %s", presets[i], unit), LLSD(presets[i]));
            if (match < 0 && fabsf(presets[i] - current) < 0.0005f)
            {
                match = (S32)i;
            }
        }
        if (match < 0)
        {
            combo->add(llformat("%g %s (custom)", current, unit), LLSD(current));
            match = (S32)count;
        }
        combo->setCurrentByIndex(match);
    }
}

WolfFloaterBuilder::WolfFloaterBuilder(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterBuilder::~WolfFloaterBuilder()
{
}

bool WolfFloaterBuilder::postBuild()
{
    mBuildersMode = getChild<LLCheckBoxCtrl>("builders_mode");
    mBuildersModeHint = getChild<LLTextBox>("builders_mode_hint");
    mSpacing = getChild<LLComboBox>("grid_spacing");
    mSize = getChild<LLComboBox>("grid_size");
    mOriginX = getChild<LLSpinCtrl>("origin_x");
    mOriginY = getChild<LLSpinCtrl>("origin_y");
    mOriginZ = getChild<LLSpinCtrl>("origin_z");
    mNamesCount = getChild<LLTextBox>("names_count");
    mCardSelection = getChild<LLTextBox>("card_selection");
    mCardImpact = getChild<LLTextBox>("card_impact");
    mCardPosition = getChild<LLTextBox>("card_position");
    mCardRegion = getChild<LLTextBox>("card_region");
    mCardParcel = getChild<LLTextBox>("card_parcel");
    mCardRegionPrims = getChild<LLTextBox>("card_region_prims");
    mStatus = getChild<LLTextBox>("status");

    mBuildersMode->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitBuildersMode, this));
    mSpacing->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitSpacing, this));
    mSize->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitSize, this));
    mOriginX->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitOrigin, this));
    mOriginY->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitOrigin, this));
    mOriginZ->setCommitCallback(boost::bind(&WolfFloaterBuilder::onCommitOrigin, this));
    getChild<LLButton>("origin_from_selection")->setCommitCallback(boost::bind(&WolfFloaterBuilder::onSetOriginFromSelection, this));
    getChild<LLButton>("object_grid_btn")->setCommitCallback(boost::bind(&WolfFloaterBuilder::onGridOnObject, this));

    refreshGridControls();
    refreshDrawSwitches();
    refreshCard();
    setStatus("", false);
    return true;
}

void WolfFloaterBuilder::onOpen(const LLSD& key)
{
    // [WolfViewer 2026-10-13] Paul: "i cant see the builders mode floater". Its saved rect (save_rect) can be off the
    // window - e.g. saved while the viewer spanned three screens. Source: llfloater.cpp:1690, the same call LLFloater
    // uses to bring a floater fully on screen (llfloater.h:606 adjustToFitScreen).
    if (gFloaterView)
        gFloaterView->adjustToFitScreen(this, false, true);
    refreshGridControls();
    refreshDrawSwitches();
    refreshCard();
    mRefreshTimer.reset();
}

void WolfFloaterBuilder::draw()
{
    if (mRefreshTimer.getElapsedTimeF32() >= REFRESH_SECS)
    {
        mRefreshTimer.reset();
        refreshCard();
        // The Build floater's Builders Mode box writes the same setting.
        refreshDrawSwitches();
        // The Options... window or the Build floater may have changed the grid step underneath us.
        const F32 spacing = gSavedSettings.getF32("GridResolution");
        const F32 size = gSavedSettings.getF32("GridDrawSize");
        if (fabsf((F32)mSpacing->getValue().asReal() - spacing) > 0.0005f || fabsf((F32)mSize->getValue().asReal() - size) > 0.0005f)
        {
            refreshGridControls();
        }
    }
    LLFloater::draw();
}

// The box is bound to WolfBuildersMode (control_name), and llcheckboxctrl.cpp:169-177 onCommit writes
// the setting before this runs. What this adds is the sky, as the Build floater's own box does
// (llfloatertools.cpp onCommitWolfBuildersMode / applyWolfBuildersMode): Builders Mode's Midday is
// for while the Build floater is open (llfloatertools.cpp onOpen applies it, onClose restores), so
// with that floater open it goes on now; with it closed, only the grids change now and the sky
// follows when it opens. A refusal (RLV @setenv, the legacy preset missing) unticks the setting -
// and so both boxes - and says why in the same must-read dialog the Build floater uses.
void WolfFloaterBuilder::onCommitBuildersMode()
{
    const bool on = gSavedSettings.getBOOL("WolfBuildersMode");
    if (on)
    {
        if (LLFloaterReg::instanceVisible("build"))
        {
            std::string why_not;
            if (!WolfBuilder::instance().applyBuildersMode(why_not))
            {
                gSavedSettings.setBOOL("WolfBuildersMode", false);
                setStatus("Builders Mode could not set the Midday sky. " + why_not, true);
                LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Builders Mode could not set the Midday sky. " + why_not));
            }
            else
            {
                setStatus("Builders Mode on: Midday sky, and the grids you tick draw.", false);
            }
        }
        else
        {
            setStatus("Builders Mode on: the grids you tick draw. Midday comes with the Build floater.", false);
        }
    }
    else
    {
        const bool had_sky = WolfBuilder::instance().buildersModeApplied();
        WolfBuilder::instance().restoreEnvironment();
        setStatus(had_sky ? "Builders Mode off: your own sky is back, and the grids are hidden."
                          : "Builders Mode off: the grids are hidden.", false);
    }
    refreshDrawSwitches();
}

// Source: llselectmgr.cpp:1564 - the world grid's step is GridResolution, read on every getGrid.
void WolfFloaterBuilder::onCommitSpacing()
{
    const F32 spacing = (F32)mSpacing->getValue().asReal();
    if (spacing <= 0.f)
    {
        setStatus("The grid spacing must be more than zero.", true);
        refreshGridControls();
        return;
    }
    gSavedSettings.setF32("GridResolution", spacing);
    setStatus(llformat("Grid spacing %g m.", spacing), false);
}

// Source: llmaniptranslate.cpp:1898 - the snap grid's drawn extent is GridDrawSize.
void WolfFloaterBuilder::onCommitSize()
{
    const F32 size = (F32)mSize->getValue().asReal();
    if (size <= 0.f)
    {
        setStatus("The grid size must be more than zero.", true);
        refreshGridControls();
        return;
    }
    gSavedSettings.setF32("GridDrawSize", size);
    setStatus(llformat("Grid drawn %g m across.", size), false);
}

void WolfFloaterBuilder::onCommitOrigin()
{
    const LLVector3 origin(mOriginX->get(), mOriginY->get(), mOriginZ->get());
    gSavedSettings.setVector3("WolfBuilderGridOrigin", origin);
    setStatus(llformat("Grid origin %.2f, %.2f, %.2f from the region corner.", origin.mV[VX], origin.mV[VY], origin.mV[VZ]), false);
}

// The selection's centre (llselectmgr.h:903 getSelectionCenterGlobal) in metres from the corner of the
// region the avatar is in (llviewerregion.h:340 getPosRegionFromAgent), which is the frame the snap
// origin is applied in (WolfBuilder::snapOriginAgent). Nothing selected: the avatar's own position.
void WolfFloaterBuilder::onSetOriginFromSelection()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        setStatus("Not in a region yet.", true);
        return;
    }
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLVector3 agent_pos;
    bool from_selection = false;
    if (!selection->isEmpty() && selection->getSelectType() == SELECT_TYPE_WORLD)
    {
        agent_pos = gAgent.getPosAgentFromGlobal(LLSelectMgr::getInstance()->getSelectionCenterGlobal());
        from_selection = true;
    }
    else
    {
        agent_pos = gAgent.getPositionAgent();
    }
    const LLVector3 origin = region->getPosRegionFromAgent(agent_pos);
    gSavedSettings.setVector3("WolfBuilderGridOrigin", origin);
    refreshGridControls();
    setStatus(llformat("%s: grid origin set to %.2f, %.2f, %.2f.",
        from_selection ? "From the selection" : "Nothing selected, so from where you stand",
        origin.mV[VX], origin.mV[VY], origin.mV[VZ]), false);
}

// Paul 10-13: "we need to be able to select an object and do 'show grid on object' so we can see the
// grid in relation to the object". The button is the explicit form of the check box: it needs an
// in-world selection (getSelectType SELECT_TYPE_WORLD, the only kind WolfBuilder::renderObjectGrid
// draws for) and says so when there is none, then switches WolfBuilderObjectGrid on and names the
// ruler frame the grid follows (LLSelectMgr::getGridMode, llselectmgr.h:155-160).
void WolfFloaterBuilder::onGridOnObject()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection->isEmpty())
    {
        setStatus("Select an object first (Build, then click it), then press Grid on object.", true);
        return;
    }
    if (selection->getSelectType() != SELECT_TYPE_WORLD)
    {
        setStatus("That is an attachment or HUD; the grid on object is for objects in-world.", true);
        return;
    }
    gSavedSettings.setBOOL("WolfBuilderObjectGrid", true);
    const EGridMode mode = LLSelectMgr::getInstance()->getGridMode();
    const char* frame = mode == GRID_MODE_LOCAL ? "Local" : mode == GRID_MODE_REF_OBJECT ? "Reference" : "World";
    setStatus(llformat("Grid on the object, %s grid. It follows the selection.", frame), false);
}

void WolfFloaterBuilder::refreshGridControls()
{
    fill_combo(mSpacing, SPACINGS, sizeof(SPACINGS) / sizeof(SPACINGS[0]), gSavedSettings.getF32("GridResolution"), "m");
    fill_combo(mSize, SIZES, sizeof(SIZES) / sizeof(SIZES[0]), gSavedSettings.getF32("GridDrawSize"), "m");
    const LLVector3 origin = gSavedSettings.getVector3("WolfBuilderGridOrigin");
    mOriginX->setValue(origin.mV[VX]);
    mOriginY->setValue(origin.mV[VY]);
    mOriginZ->setValue(origin.mV[VZ]);
}

// The grids draw only with Builders Mode on (wolfbuilder.cpp renderGrid). Paul 10-14 ticked "show the
// grid on the ground" with it off and saw nothing and no reason; now the switches that only work with
// it are greyed out while it is off, and the line beside the Builders Mode box says to tick it.
// Spacing, size and the origin stay live: they set the snap grid the Build floater uses whether or
// not anything is drawn.
void WolfFloaterBuilder::refreshDrawSwitches()
{
    const bool on = gSavedSettings.getBOOL("WolfBuildersMode");
    getChildView("grid_show")->setEnabled(on);
    getChildView("object_grid_show")->setEnabled(on);
    getChildView("object_grid_btn")->setEnabled(on);
    getChildView("object_grid_all_faces")->setEnabled(on);
    getChildView("names_show")->setEnabled(on);   // [WolfViewer 2026-10-14] the names need Builders Mode too (WolfBuilder::idle)
    getChildView("names_radius")->setEnabled(on);
    mBuildersModeHint->setText(on ? std::string("On: the grids and names you tick show.")
                                  : std::string("Tick it to show the grids and names."));
    mBuildersModeHint->setColor(LLUIColorTable::instance().getColor(on ? "LabelTextColor" : "AlertCautionTextColor"));
}

// The card. Counts: llfloatertools.cpp:706-713 (getObjectCount prims, getRootObjectCount objects,
// getSelectedLinksetCost land impact) and :791-797 (on OpenSim a zero cost means the prim count).
// Parcel room: llfloaterland.cpp:1429-1447 (parcel capacity x bonus, region capacity, both capped by
// the region's max tasks). Which parcel: the Build floater selects the parcel under the selection
// (llfloatertools.cpp:727 selectParcelAt -> the floating parcel selection); with nothing selected,
// the parcel the avatar stands on (llviewerparcelmgr.cpp:563 getAgentParcel).
void WolfFloaterBuilder::refreshCard()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    const bool have_selection = !selection->isEmpty();

    if (!have_selection)
    {
        mCardSelection->setText(std::string("Nothing selected."));
        mCardImpact->setText(std::string(""));
        mCardPosition->setText(std::string(""));
    }
    else
    {
        const S32 prim_count = selection->getObjectCount();
        const S32 link_count = selection->getRootObjectCount();
        const F32 link_cost = selection->getSelectedLinksetCost();
        S32 impact = (S32)link_cost;
#ifdef OPENSIM
        if (LLGridManager::getInstance()->isInOpenSim())
        {
            impact = link_cost ? (S32)link_cost : prim_count;
        }
#endif
        mCardSelection->setText(llformat("%s object%s, %s prim%s",
            with_commas(link_count).c_str(), link_count == 1 ? "" : "s",
            with_commas(prim_count).c_str(), prim_count == 1 ? "" : "s"));
        mCardImpact->setText("Land impact " + with_commas(impact));

        if (selection->getSelectType() == SELECT_TYPE_WORLD)
        {
            const LLVector3d center = LLSelectMgr::getInstance()->getSelectionCenterGlobal();
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(center);
            if (region)
            {
                const LLVector3 pos = region->getPosRegionFromAgent(gAgent.getPosAgentFromGlobal(center));
                mCardPosition->setText(llformat("At %.2f, %.2f, %.2f in %s", pos.mV[VX], pos.mV[VY], pos.mV[VZ], region->getName().c_str()));
            }
            else
            {
                mCardPosition->setText(std::string("Position: not in a region you can see."));
            }
        }
        else
        {
            mCardPosition->setText(std::string("An attachment or HUD: no world position."));
        }
    }

    LLViewerRegion* agent_region = gAgent.getRegion();
    mCardRegion->setText(agent_region ? "Region " + agent_region->getName() : std::string("Region: not connected"));

    LLParcel* parcel = nullptr;
    if (have_selection)
    {
        LLParcelSelectionHandle floating = LLViewerParcelMgr::getInstance()->getFloatingParcelSelection();
        if (floating)
        {
            parcel = floating->getParcel();
        }
    }
    if (!parcel)
    {
        parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    }
    if (!parcel)
    {
        mCardParcel->setText(std::string("Parcel prims: waiting for the parcel."));
        mCardRegionPrims->setText(std::string(""));
    }
    else
    {
        S32 max = ll_round(parcel->getMaxPrimCapacity() * parcel->getParcelPrimBonus());
        S32 total = parcel->getPrimCount();
        S32 sw_max = parcel->getSimWideMaxPrimCapacity();
        S32 sw_total = parcel->getSimWidePrimCount();
        if (agent_region)
        {
            const S32 max_tasks_per_region = (S32)agent_region->getMaxTasks();
            sw_max = llmin(sw_max, max_tasks_per_region);
            max = llmin(max, max_tasks_per_region);
        }
        mCardParcel->setText(llformat("Parcel \"%s\": %s of %s prims used, %s free",
            parcel->getName().c_str(), with_commas(total).c_str(), with_commas(max).c_str(), with_commas(llmax(0, max - total)).c_str()));
        mCardRegionPrims->setText(llformat("Owner's land in this region: %s of %s prims used",
            with_commas(sw_total).c_str(), with_commas(sw_max).c_str()));
    }

    const S32 tags = WolfBuilder::instance().nameTagCount();
    mNamesCount->setText(gSavedSettings.getBOOL("WolfBuilderNames") && gSavedSettings.getBOOL("WolfBuildersMode")
        ? llformat("%d name%s showing", tags, tags == 1 ? "" : "s")
        : std::string(""));
}

void WolfFloaterBuilder::setStatus(const std::string& text, bool error)
{
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}
