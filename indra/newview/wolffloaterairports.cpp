/**
 * @file wolffloaterairports.cpp
 * @brief WolfViewer: the Airports window. See wolffloaterairports.h.
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

#include "wolffloaterairports.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llbutton.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llscrolllistctrl.h"
#include "llsearcheditor.h"
#include "lltextbox.h"
#include "llurldispatcher.h"
#include "llweb.h"
#include "wolfairports.h"
#include "wolfflight.h"

namespace
{
    const F32 NM = 1852.f;          // Source: wolfflightdeck.cpp NM
    const S32 MAX_ROWS = 300;       // a scroll list this long is still quick; the search narrows it

    std::string distanceText(F64 metres)
    {
        const F32 nm = (F32)(metres / NM);
        return nm < 10.f ? llformat("%.1f NM", nm) : llformat("%d NM", (S32)llround(nm));
    }
}

WolfFloaterAirports::WolfFloaterAirports(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterAirports::postBuild()
{
    mSearch = getChild<LLSearchEditor>("search");
    mList = getChild<LLScrollListCtrl>("airports");
    mSummary = getChild<LLTextBox>("summary");
    mFly = getChild<LLButton>("fly");
    mShowMap = getChild<LLButton>("show_map");
    mTeleport = getChild<LLButton>("teleport");

    mSearch->setKeystrokeCallback([this](LLUICtrl*, const LLSD&) { fill(); });
    mSearch->setCommitCallback([this](LLUICtrl*, const LLSD&) { fill(); });
    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mList->setDoubleClickCallback([this]() { onFly(); });
    mFly->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFly(); });
    mShowMap->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShowMap(); });
    mTeleport->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTeleport(); });
    getChild<LLButton>("website")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        // Source: the deck's former AIRPORTS button - the grid's airports page
        LLWeb::loadURL("https://www.wolf-grid.com/index.php?f=ap");
    });
    return true;
}

void WolfFloaterAirports::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // <WolfViewer 2026-10-11> Paul: "refresh the airports window every time it opens" - a new list from the
    // website each time (draw() fills again when it arrives), the one held meanwhile
    WolfAirports::instance().refreshNow();
    fill();
    mSearch->setFocus(true);
}

void WolfFloaterAirports::draw()
{
    // The directory arrives a moment after the window opens: fill the list again then.
    WolfAirports& airports = WolfAirports::instance();
    airports.refresh();
    if (airports.loaded() && mFilledGeneration != airports.generation())
    {
        fill();
    }
    updateButtons();
    LLFloater::draw();
}

void WolfFloaterAirports::fill()
{
    WolfAirports& airports = WolfAirports::instance();
    const LLVector3d here = gAgent.getPositionGlobal();
    const S32 keep = mList->getSelectedValue().asInteger();
    mList->deleteAllItems();
    if (!airports.loaded())
    {
        mSummary->setText(std::string("Loading the airports list..."));
        return;
    }
    mFilledGeneration = airports.generation();

    std::string query = mSearch->getText();
    LLStringUtil::trim(query);
    std::vector<const WolfAirports::Airport*> found;
    if (query.empty())
    {
        // nothing typed: every airport, nearest first
        for (const WolfAirports::Airport& a : airports.all())
        {
            found.push_back(&a);
        }
        std::sort(found.begin(), found.end(), [&here](const WolfAirports::Airport* l, const WolfAirports::Airport* r)
        {
            return dist_vec_squared(l->mGlobal, here) < dist_vec_squared(r->mGlobal, here);
        });
    }
    else
    {
        found = airports.search(query, here);
    }

    S32 rows = 0;
    // Paul: "i put in a region name it should still use that for flying as well as airports" - what was typed,
    // as a region, first (value -1); the map server says whether there is one when it is flown
    if (!query.empty())
    {
        LLSD row;
        row["value"] = -1;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = "Fly to the region \"" + query + "\"";
        row["columns"][0]["font"]["style"] = "BOLD";
        row["columns"][1]["column"] = "region";
        row["columns"][1]["value"] = query;
        row["columns"][2]["column"] = "distance";
        row["columns"][2]["value"] = std::string();
        mList->addElement(row);
    }
    for (const WolfAirports::Airport* a : found)
    {
        if (rows >= MAX_ROWS)
        {
            break;
        }
        LLSD row;
        row["value"] = a->mId;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = a->mName;
        row["columns"][1]["column"] = "region";
        row["columns"][1]["value"] = a->mRegion;
        row["columns"][2]["column"] = "distance";
        const F64 dx = a->mGlobal.mdV[VX] - here.mdV[VX], dy = a->mGlobal.mdV[VY] - here.mdV[VY];
        row["columns"][2]["value"] = distanceText(sqrt(dx * dx + dy * dy));
        mList->addElement(row);
        ++rows;
    }
    if (keep)
    {
        mList->selectByValue(LLSD(keep));
    }
    if (found.empty())
    {
        mSummary->setText(query.empty() ? std::string("The airports directory has no airports yet.")
                                        : std::string("No airport matches \"") + query + "\" - fly to it as a region, the first line.");
    }
    else if ((S32)found.size() > rows)
    {
        mSummary->setText(llformat("%d airports - showing the first %d. Type more of the name to narrow it.", (S32)found.size(), rows));
    }
    else
    {
        mSummary->setText(query.empty() ? llformat("%d airports, nearest first.", rows)
                                        : llformat("%d found.", rows));
    }
    updateButtons();
}

void WolfFloaterAirports::updateButtons()
{
    const bool picked = mList->getFirstSelected() != nullptr;
    WolfFlight& f = WolfFlight::instance();
    const bool can_fly = picked && f.active() && !f.sailing();
    mFly->setEnabled(can_fly);
    mFly->setToolTip(!f.active() || f.sailing()
        ? std::string("Start Flight Mode first (World > Flight Mode) and sit in your aircraft; then Fly there sets the autopilot's route.")
        : std::string("Set this airport as the autopilot's destination and fly the route there; it lands on the runway it finds."));
    mShowMap->setEnabled(picked);
    mTeleport->setEnabled(picked);
}

void WolfFloaterAirports::onFly()
{
    const S32 id = mList->getSelectedValue().asInteger();
    WolfFlight& f = WolfFlight::instance();
    if (!id || !f.active() || f.sailing())
    {
        return;
    }
    if (id == -1)
    {
        std::string region = mSearch->getText();
        LLStringUtil::trim(region);
        f.flyToRegion(region);
        mSummary->setText("Flying to the region " + region + ". Engage CMD on the deck if the autopilot is off.");
        return;
    }
    f.flyToAirport(id);
    if (const WolfAirports::Airport* a = WolfAirports::instance().byId(id))
    {
        mSummary->setText("Flying to " + a->mName + " (" + a->mRegion + "). Engage CMD on the deck if the autopilot is off.");
    }
}

// <WolfViewer 2026-10-11> Paul: "add a teleport to the airports list so i can tp to the runway". To the airport's point - where
// its owner stood for "Copy SLurl", on or beside the runway (wolfairports.cpp NEAR_EXACT_M) - at its height when the
// directory has one, else the region's own ground (z 0: the region puts an arrival on the land). A region typed by name:
// its middle, by the same app/teleport SLURL the chat bar's own teleport command uses (chatbar_as_cmdline.cpp).
void WolfFloaterAirports::onTeleport()
{
    const S32 id = mList->getSelectedValue().asInteger();
    if (id == -1)
    {
        std::string region = mSearch->getText();
        LLStringUtil::trim(region);
        if (region.empty())
        {
            return;
        }
        LLURLDispatcher::dispatch("secondlife:///app/teleport/" + LLWeb::escapeURL(region) + "/128/128/0", "clicked", NULL, true);
        mSummary->setText("Teleporting to the region " + region + "...");
        return;
    }
    const WolfAirports::Airport* a = WolfAirports::instance().byId(id);
    if (!a)
    {
        return;
    }
    LLVector3d to = a->mGlobal;
    if (!a->mHasZ)
    {
        to.mdV[VZ] = 0.0;
    }
    gAgent.teleportViaLocation(to);
    mSummary->setText("Teleporting to " + a->mName + " (" + a->mRegion + ")...");
}

void WolfFloaterAirports::onShowMap()
{
    if (mList->getSelectedValue().asInteger() == -1)
    {
        // a region by name: the World Map's own search finds it
        std::string region = mSearch->getText();
        LLStringUtil::trim(region);
        LLFloaterReg::showInstance("world_map", LLSD("center"));
        if (LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance())
        {
            map->trackURL(region, 128, 128, 0);
        }
        return;
    }
    const WolfAirports::Airport* a = WolfAirports::instance().byId(mList->getSelectedValue().asInteger());
    if (!a)
    {
        return;
    }
    if (LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance())
    {
        map->trackLocation(a->mGlobal);
    }
    LLFloaterReg::showInstance("world_map", LLSD("center"));
}
