/**
 * @file wolfvrui.cpp
 * @brief WolfViewer VR in the headset browser: the in-VR action bar's viewer side. See wolfvrui.h.
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

#include "wolfvrui.h"

#include "llagent.h"
#include "llcommandmanager.h"
#include "llfavoritesbar.h"
#include "llinventorymodel.h"
#include "lluictrl.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"
#include "wolfvrstream.h"

#include <boost/json.hpp>

#include <algorithm>

namespace
{
    // The commands the bar may run: the WolfStorm bar's actions (wolfstorm/js/ui/hud_model.js ACTIONS) mapped to this
    // viewer's commands.xml names, plus the Wolf ones a resident needs in a headset (ship helm, car dashboard). Only
    // these: any device on the home network can reach the page while VR is on, so it cannot run arbitrary commands.
    const char* const kCommands[] = {
        "chat", "speak", "voice", "wolf_dictate", "wolf_sharescreen",   // comms
        "move", "view", "fly", "groundsit",                             // move
        "people", "radar", "search", "map", "minimap", "wolf_gamemode", "wolf_experiences",   // world
        "inventory", "appearance", "animationoverride", "gestures",     // self
        "snapshot", "quickprefs", "wolf_eep", "wolf_weather",           // system
        "wolf_helm", "wolf_dashboard",                                  // Wolf vehicles
    };

    bool allowed(const std::string& name)
    {
        return std::any_of(std::begin(kCommands), std::end(kCommands), [&](const char* c) { return name == c; });
    }

    // LLToolBar::createButton (lltoolbar.cpp) resolves a command's functions through the callback registries by name.
    bool callEnable(const std::string& fn, const LLSD& param, bool dflt)
    {
        if (fn.empty()) return dflt;
        LLUICtrl::enable_callback_t* f = LLUICtrl::EnableCallbackRegistry::getValue(fn);
        return f ? (*f)(nullptr, param) : dflt;
    }

    void callCommit(const std::string& fn, const LLSD& param)
    {
        if (fn.empty()) return;
        if (LLUICtrl::commit_callback_t* f = LLUICtrl::CommitCallbackRegistry::getValue(fn)) (*f)(nullptr, param);
        else LL_WARNS("WolfVR") << "action bar: no function " << fn << LL_ENDL;
    }

    LLCommand* command(const std::string& name) { return LLCommandManager::instance().getCommand(LLCommandId(name)); }

    std::string gLastState;
    F64 gLastSent = 0;
}

namespace WolfVRUi
{
void run(const std::string& name, const std::string& arg)
{
    if (name == "home")
    {
        gAgent.teleportHome();
        return;
    }
    if (name == "mic")
    {
        // [VR AUDIO 2026-11-04] the headset page's "Use the headset microphone for voice" (Headset Settings has it too)
        gSavedSettings.setBOOL("WolfVRHeadsetMic", arg == "1");
        return;
    }
        if (name == "sit")
    {
        // one slot for the pair (hud_model.js _sitOrStand): stand when seated on anything, else sit on the ground
        if (isAgentAvatarValid() && gAgentAvatarp->isSitting()) gAgent.standUp();
        else if (LLCommand* c = command("groundsit")) callCommit(c->executeFunctionName(), c->executeParameters());
        return;
    }
    if (name == "fav")
    {
        LLUUID id;
        if (!LLUUID::validate(arg) || !id.set(arg, false)) return;
        // only an item in the Favorites folder (what the bar listed), never any inventory item the page names
        const LLUUID fav = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getParentUUID() != fav || item->getType() != LLAssetType::AT_LANDMARK) return;
        gAgent.teleportViaLandmark(item->getAssetUUID());
        return;
    }
    if (!allowed(name)) return;
    LLCommand* c = command(name);
    if (!c) return;
    if (!callEnable(c->isEnabledFunctionName(), c->isEnabledParameters(), true)) return;
    // A command with a "stop" function is press-and-hold on the desktop (speak: Agent.PressMicrophone /
    // ReleaseMicrophone). A trigger pull cannot be held on a menu, so the bar LATCHES it, as WolfStorm's Speak slot does.
    if (!c->executeStopFunctionName().empty() && callEnable(c->isRunningFunctionName(), c->isRunningParameters(), false))
    {
        callCommit(c->executeStopFunctionName(), c->executeStopParameters());
        return;
    }
    callCommit(c->executeFunctionName(), c->executeParameters());
}

void pump()
{
    if (WolfVRStream::mode().empty()) { gLastState.clear(); return; }
    namespace json = boost::json;
    json::object o;
    o["t"] = "ui";
    json::array on, off;
    for (const char* name : kCommands)
    {
        LLCommand* c = command(name);
        if (!c || !callEnable(c->isEnabledFunctionName(), c->isEnabledParameters(), true)) { off.emplace_back(name); continue; }
        if (callEnable(c->isRunningFunctionName(), c->isRunningParameters(), false)) on.emplace_back(name);
    }
    o["on"] = std::move(on);
    o["off"] = std::move(off);
    o["wolf"] = WolfGrid::isOnWolfTerritories();
    LLViewerRegion* region = gAgent.getRegion();
    o["region"] = region ? region->getName() : std::string();
    // 0 standing, 1 on the ground, 2 on an object (a seat, a ship's helm, a plane): the page lowers the UI layer to a
    // dashboard when seated on something
    int sit = 0;
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting())
    {
        sit = gAgentAvatarp->getParent() ? 2 : 1;
    }
    o["sit"] = sit;
    o["fly"] = gAgent.getFlying();
    // [VR HEADSETS 2026-11-04] the headset chosen in Headset Settings ("auto" = the page works it out)
    o["headset"] = gSavedSettings.getString("WolfVRHeadset");
    o["mic"] = gSavedSettings.getBOOL("WolfVRHeadsetMic");   // [VR AUDIO] the page's microphone button
    // the favourites bar's landmarks, in its order (LLFavoritesOrderStorage), first 6 (vr_hud.js _favorites)
    json::array favs;
    const LLUUID fav = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
    if (fav.notNull())
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(fav, cats, items);
        if (items)
        {
            std::vector<LLViewerInventoryItem*> lm;
            for (const auto& it : *items)
            {
                if (it && it->getType() == LLAssetType::AT_LANDMARK) lm.push_back(it.get());
            }
            std::stable_sort(lm.begin(), lm.end(), [](LLViewerInventoryItem* a, LLViewerInventoryItem* b) {
                return LLFavoritesOrderStorage::instance().getSortIndex(a->getUUID()) <
                       LLFavoritesOrderStorage::instance().getSortIndex(b->getUUID());
            });
            for (size_t k = 0; k < lm.size() && k < 6; ++k)
            {
                json::object f;
                f["id"] = lm[k]->getUUID().asString();
                f["n"] = lm[k]->getName();
                favs.push_back(std::move(f));
            }
        }
    }
    o["fav"] = std::move(favs);
    const std::string s = json::serialize(o);
    const F64 now = LLTimer::getTotalSeconds();
    if (s == gLastState && now - gLastSent < 2.0) return;   // again every 2 s: a page that just connected gets it soon
    gLastState = s;
    gLastSent = now;
    WolfVRStream::sendText(s);
}
}
