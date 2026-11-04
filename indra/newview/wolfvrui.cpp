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
#include "llfloater.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "lltexteditor.h"
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

    bool callCommit(const std::string& fn, const LLSD& param)
    {
        if (fn.empty()) return false;
        if (LLUICtrl::commit_callback_t* f = LLUICtrl::CommitCallbackRegistry::getValue(fn)) { (*f)(nullptr, param); return true; }
        LL_WARNS("WolfVR") << "action bar: no function " << fn << LL_ENDL;
        return false;
    }

    LLCommand* command(const std::string& name) { return LLCommandManager::instance().getCommand(LLCommandId(name)); }

    std::string gLastState;
    F64 gLastSent = 0;
}

namespace
{
    bool runCommand(const std::string& name, const std::string& arg, std::string& why);
}

namespace WolfVRUi
{
void run(const std::string& name, const std::string& arg)
{
    // [VR MENU FIX 2026-11-04] every command from the bar gets an answer the headset shows (done, or why not): a press
    // that silently did nothing is what Paul saw on the Quest 3 ("clicking any button on the menu does nothing")
    std::string why;
    const bool ok = runCommand(name, arg, why);
    namespace json = boost::json;
    json::object o;
    o["t"] = "acted";
    o["a"] = name;
    o["ok"] = ok;
    if (!ok) o["why"] = why;
    WolfVRStream::sendText(json::serialize(o));
}
}

namespace
{
bool runCommand(const std::string& name, const std::string& arg, std::string& why)
{
    if (name == "home")
    {
        gAgent.teleportHome();
        return true;
    }
    if (name == "hideavatar")
    {
        // [VR VIEW 2026-11-04] the bar's Hide Me / Show Me (Headset Settings has it too)
        gSavedSettings.setBOOL("WolfVRHideAvatar", !gSavedSettings.getBOOL("WolfVRHideAvatar"));
        return true;
    }
    if (name == "mic")
    {
        // [VR AUDIO 2026-11-04] the headset page's "Use the headset microphone for voice" (Headset Settings has it too)
        gSavedSettings.setBOOL("WolfVRHeadsetMic", arg == "1");
        return true;
    }
    if (name == "sit")
    {
        // one slot for the pair (hud_model.js _sitOrStand): stand when seated on anything, else sit on the ground
        if (isAgentAvatarValid() && gAgentAvatarp->isSitting()) gAgent.standUp();
        else if (LLCommand* c = command("groundsit"); !c || !callCommit(c->executeFunctionName(), c->executeParameters()))
        {
            why = "sitting is not available";
            return false;
        }
        return true;
    }
    if (name == "closefloater")
    {
        // [VR PANELS 2026-11-04] Paul: "there was no easy close on the mini map". A panel's own Close: only a window open
        // in the floater view now, found by the id its panel was sent with (wolfvrscreen.cpp sendPanels), closed as its
        // own close button would
        why = "that window is not open any more";
        if (!gFloaterView) return false;
        for (LLView* v : *gFloaterView->getChildList())
        {
            LLFloater* f = dynamic_cast<LLFloater*>(v);
            if (!f || !f->isShown()) continue;
            std::string id = f->getInstanceName();
            if (f->getKey().isDefined()) id += ":" + f->getKey().asString();
            LLStringUtil::truncate(id, 80);
            if (id != arg) continue;
            if (f->getInstanceName() == "wolf_vr_stream") { why = "that window stays on the desktop"; return false; }
            f->closeFloater();
            return true;
        }
        return false;
    }
    if (name == "fav")
    {
        LLUUID id;
        why = "that landmark is not in your favourites";
        if (!LLUUID::validate(arg) || !id.set(arg, false)) return false;
        // only an item in the Favorites folder (what the bar listed), never any inventory item the page names
        const LLUUID fav = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getParentUUID() != fav || item->getType() != LLAssetType::AT_LANDMARK) return false;
        gAgent.teleportViaLandmark(item->getAssetUUID());
        return true;
    }
    if (!allowed(name)) { why = "not a menu button"; return false; }
    LLCommand* c = command(name);
    if (!c) { why = "not in this viewer"; return false; }
    if (!callEnable(c->isEnabledFunctionName(), c->isEnabledParameters(), true)) { why = "cannot be used here just now"; return false; }
    // A command with a "stop" function is press-and-hold on the desktop (speak: Agent.PressMicrophone /
    // ReleaseMicrophone). A trigger pull cannot be held on a menu, so the bar LATCHES it, as WolfStorm's Speak slot does.
    if (!c->executeStopFunctionName().empty() && callEnable(c->isRunningFunctionName(), c->isRunningParameters(), false))
    {
        if (callCommit(c->executeStopFunctionName(), c->executeStopParameters())) return true;
        why = "not available in this viewer";
        return false;
    }
    if (callCommit(c->executeFunctionName(), c->executeParameters())) return true;
    why = "not available in this viewer";
    return false;
}
}

namespace WolfVRUi
{

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
    o["hide"] = gSavedSettings.getBOOL("WolfVRHideAvatar");   // [VR VIEW] the bar's Hide Me / Show Me
    // [VR KEYBOARD 2026-11-04] Paul: "NO KEYBOARD". The page's keyboard opens by itself while the viewer's typing goes to
    // a text field (nearby chat, an IM, search, the inventory filter), and closes again when it leaves one
    LLFocusableElement* kf = gFocusMgr.getKeyboardFocus();
    const LLLineEditor* le = dynamic_cast<LLLineEditor*>(kf);
    const LLTextEditor* te = dynamic_cast<LLTextEditor*>(kf);
    o["typing"] = (le && le->acceptsTextInput()) || (te && te->acceptsTextInput());   // not a read-only chat history
    o["lang"] = gSavedSettings.getString("Language");   // the keyboard's layout when the headset's own language is not one it has
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
