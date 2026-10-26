/**
 * @file wolffloateragecheck.cpp
 * @brief WolfViewer: Wolf World > Age Check... - the resident's age check status and the "Verify my age" link
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

#include "wolffloateragecheck.h"

#include "llagent.h"
#include "llbutton.h"
#include "lliconctrl.h"
#include "lltextbox.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "wolfadultcontent.h"
#include "wolfgrid.h"

WolfFloaterAgeCheck::WolfFloaterAgeCheck(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterAgeCheck::postBuild()
{
    // Source: PROTOCOL.md §5 "Verify my age" opens WolfAgeCheckURL; §6 the region types page.
    getChild<LLButton>("verify_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLWeb::loadURLExternal(WolfAdultContent::instance().ageCheckURL());
    });
    getChild<LLButton>("region_types_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLWeb::loadURLExternal(WolfAdultContent::instance().regionTypesURL());
    });
    return true;
}

void WolfFloaterAgeCheck::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE 2026-10-17] a Wolf-only window opened off Wolf (a saved toolbar, a link) says so and closes.
    if (!WolfGrid::isWolfTerritories())
    {
        WolfGrid::noteWolfOnly(getString("feature_name"));
        closeFloater();
        return;
    }
    refresh();
    mRefreshTimer.reset();
}

void WolfFloaterAgeCheck::draw()
{
    // The status changes only on arrival at a region (PROTOCOL.md §3a); once a second is plenty to follow a teleport
    // while the window is open.
    if (mRefreshTimer.getElapsedTimeF32() > 1.f)
    {
        refresh();
        mRefreshTimer.reset();
    }
    LLFloater::draw();
}

void WolfFloaterAgeCheck::refresh()
{
    WolfAdultContent& ac = WolfAdultContent::instance();
    const WolfAdultContent::EVerified state = ac.ageVerified();

    std::string status;
    switch (state)
    {
    case WolfAdultContent::EVerified::Verified:
        status = getString("status_verified");
        break;
    case WolfAdultContent::EVerified::NotVerified:
        status = getString("status_not_verified");
        break;
    case WolfAdultContent::EVerified::Unknown:
    default:
        status = getString("status_unknown");
        break;
    }
    getChild<LLTextBox>("status_text")->setText(status);
    // A state from an earlier region (this one did not say) is labelled as such.
    getChild<LLTextBox>("status_note")->setText(getString(
        state == WolfAdultContent::EVerified::Unknown ? "note_unknown"
        : (ac.ageVerifiedFromThisRegion() ? "note_this_region" : "note_earlier_region")));

    // A verified resident needs no "Verify my age"; the same page shows their status, so the button opens it as
    // "My age check" then.
    LLButton* verify = getChild<LLButton>("verify_btn");
    verify->setLabel(getString(state == WolfAdultContent::EVerified::Verified ? "verify_label_done" : "verify_label"));

    // The region the resident stands on: its type, with the Adult Content badge when it is one.
    const LLViewerRegion* region = gAgent.getRegion();
    std::string region_line;
    bool adult_content = false;
    if (region && WolfGrid::isOnWolfTerritories())
    {
        adult_content = WolfAdultContent::isAdultContentRegion(region);
        LLStringUtil::format_map_t args;
        args["[REGION]"] = region->getName();
        args["[TYPE]"] = WolfAdultContent::accessString(region->getSimAccess(), adult_content);
        region_line = getString("region_line", args);
    }
    else
    {
        region_line = getString("region_not_wolf");
    }
    getChild<LLIconCtrl>("region_icon")->setValue(region && WolfGrid::isOnWolfTerritories()
        ? WolfAdultContent::accessIcon(region->getSimAccess(), adult_content) : std::string());
    getChild<LLTextBox>("region_text")->setText(region_line);
}
