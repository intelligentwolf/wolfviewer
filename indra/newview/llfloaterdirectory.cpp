/**
 * @file llfloaterdirectory.cpp
 * @brief The legacy "Search" floater
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2025, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterdirectory.h"

#include "llpaneldirevents.h"
#include "llpaneleventinfo.h"
#include "llpaneldirland.h"
#include "llpaneldirpeople.h"
#include "llpaneldirgroups.h"
#include "llpaneldirplaces.h"
#include "llpaneldirclassified.h"
#include "llpaneldirweb.h"
#include "llscrollbar.h"
#include "llviewercontrol.h"
#include "llpanelavatar.h"
#include "llpanelclassified.h"
#include "llpanelgroup.h"
#include "llpanelplaces.h"
#include "llpanelprofile.h"

#include "lltabcontainer.h"

// <FS:TJ> Always navigate if the SearchURL has changed
#include "lfsimfeaturehandler.h"
#include "llviewernetwork.h"
// </FS:TJ>

LLFloaterDirectory::LLFloaterDirectory(const std::string& name)
:   LLFloater(name),
    mPanelAvatarp(nullptr),
    mPanelGroupp(nullptr),
    mPanelPlacep(nullptr),
    mPanelClassifiedp(nullptr),
    mPanelEventp(nullptr),
    // <FS:Ansariel> Add "open profile" button
    mOpenProfileBtn(nullptr),
    mDirectoryTabs(nullptr)
{
}

LLFloaterDirectory::~LLFloaterDirectory()
{
}

bool LLFloaterDirectory::postBuild()
{
    const std::vector<std::string> panel_names = {
        "panel_dir_classified",
        "panel_dir_events",
        "panel_dir_places",
        "panel_dir_land",
        "panel_dir_people",
        "panel_dir_groups" };

    for (const std::string& panel_name : panel_names)
    {
        if (LLPanelDirBrowser* panel_tab = findChild<LLPanelDirBrowser>(panel_name))
        {
            panel_tab->setFloaterDirectory(this);
        }
    }
    findChild<LLPanelDirWeb>("panel_dir_web")->setFloaterDirectory(this);

    mPanelAvatarp = findChild<LLPanelProfileSecondLife>("panel_profile_secondlife");
    mPanelAvatarp->setAllowEdit(false);
    mPanelGroupp = findChild<LLPanelGroup>("panel_group_info_sidetray");
    mPanelGroupp->hideBackBtn();
    mPanelPlacep = findChild<LLPanelPlaces>("panel_places");
    mPanelPlacep->hideBackBtn();
    mPanelClassifiedp = findChild<LLPanelClassifiedInfo>("panel_classified_info");
    mPanelClassifiedp->setBackgroundVisible(false);
    mPanelEventp = findChild<LLPanelEventInfo>("panel_event_info");

    // <FS:Ansariel> Add "open profile" button
    mOpenProfileBtn = getChild<LLButton>("open_profile_btn");
    mOpenProfileBtn->setCommitCallback([&](LLUICtrl*, const LLSD&) {
        auto* currentDirBrowserPanel = dynamic_cast<LLPanelDirBrowser*>(mDirectoryTabs->getCurrentPanel());
        if (currentDirBrowserPanel)
            currentDirBrowserPanel->openProfile();
        });
    mDirectoryTabs = getChild<LLTabContainer>("Directory Tabs");
    mDirectoryTabs->setCommitCallback([&](LLUICtrl*, const LLSD&) { updateProfileButtonVisibility(); });

    // </FS:Ansariel>
    // <FS:Wolf> [SEARCH HUB 2026-11-14] Paul: "it should default to the web tab to start with". Search opens on the
    // Web tab, which shows the grid's search page (the Wolf Territories hub), instead of the tab used last time
    // (Firestorm restored the FSLastSearchTab setting here; index 0 = People in floater_directory.xml, so a new
    // resident started on People). Built once per session (single_instance + reuse, llfloater.cpp mReuseInstance),
    // so within a session the window still keeps the tab the resident is on. A caller that asks for a tab
    // (onOpen key "tab"/"query") still gets it.
    if (!mDirectoryTabs->selectTabByName("panel_dir_web"))
    {
        mDirectoryTabs->selectFirstTab();
    }
    // </FS:Wolf>

    // <FS:TJ> Always navigate if the SearchURL has changed
    mLastSearchURL = LLGridManager::getInstance()->isInSecondLife() ? gSavedSettings.getString("SearchURL") : gSavedSettings.getString("SearchURLOpenSim");
    // </FS:TJ>

    return true;
}

// <FS:PP> FIRE-36483 Menu, navbar and toolbar button must open the same search window
void LLFloaterDirectory::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    if (auto currentPanel = mDirectoryTabs->getCurrentPanel())
        currentPanel->focusFirstItem();

    if (key.has("tab") && key["tab"].asString() == "groups")
    {
        mDirectoryTabs->selectTabByName("panel_dir_groups");
        return;
    }

    // <FS:TJ> Always navigate if the SearchURL has changed
    std::string search_url = LFSimFeatureHandler::instance().searchURL();
    if (search_url.empty())
    {
        search_url = LLGridManager::getInstance()->isInSecondLife() ? gSavedSettings.getString("SearchURL") : gSavedSettings.getString("SearchURLOpenSim");
    }

    if (!key.has("query") && mLastSearchURL == search_url)
    {
        return;
    }
    mLastSearchURL = search_url;

    LLPanelDirWeb* panel_dir_web = findChild<LLPanelDirWeb>("panel_dir_web");
    if (!panel_dir_web)
    {
        return;
    }

    const std::string category = key.has("category") ? key["category"].asString() : "standard";
    const std::string query = key["query"].asString();
    const std::string collection = key.has("collection") ? key["collection"].asString() : "";

    if (key.has("query"))
    {
        mDirectoryTabs->selectTabByName("panel_dir_web");
    }
    panel_dir_web->navigateToSearchPage(category, query, collection);
}
// </FS:PP>

// <FS:Wolf> [SEARCH HUB 2026-11-14] onClose no longer saves FSLastSearchTab: Search always starts on the Web tab
// (postBuild above), so nothing reads the saved tab and the setting is gone.

void LLFloaterDirectory::hideAllDetailPanels()
{
    if (mPanelAvatarp) mPanelAvatarp->setVisible(false);
    if (mPanelGroupp) mPanelGroupp->setVisible(false);
    if (mPanelPlacep) mPanelPlacep->setVisible(false);
    if (mPanelClassifiedp) mPanelClassifiedp->setVisible(false);
    if (mPanelEventp) mPanelEventp->setVisible(false);

    mOpenProfileBtn->setVisible(false); // <FS:Ansariel> Add "open profile" button
}

// <FS:Ansariel> Add "open profile" button
void LLFloaterDirectory::updateProfileButtonVisibility()
{
    std::string selected_tab = mDirectoryTabs->getCurrentPanel()->getName();
    if (selected_tab == "panel_dir_people")
        mOpenProfileBtn->setVisible(mPanelAvatarp->getVisible());
    else if (selected_tab == "panel_dir_groups")
        mOpenProfileBtn->setVisible(mPanelGroupp->getVisible());
    else
        mOpenProfileBtn->setVisible(false);
}
// </FS:Ansariel>
