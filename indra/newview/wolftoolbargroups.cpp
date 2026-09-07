/**
 * @file wolftoolbargroups.cpp
 * @brief WolfViewer: toolbar GROUP buttons — one button per purpose, a menu of the commands.
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

#include "wolftoolbargroups.h"

#include "llcommandmanager.h"
#include "llmenugl.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"      // gMenuHolder
#include "wolfgrid.h"

LLHandle<LLContextMenu> WolfToolbarGroups::sMenuHandle;

// Source: wolfstorm/js/ui/toolbar_groups.js GROUPS — the web viewer's five groups. The
// members here are this viewer's own commands for the same jobs (app_settings/commands.xml);
// Speak and Chat stay standalone on the bar for the same reason as there: pressed all the
// time, and Speak is a hold-to-talk button (execute_stop_function) a menu cannot hold.
const std::vector<WolfToolbarGroups::Group>& WolfToolbarGroups::groups()
{
    static const std::vector<Group> sGroups = {
        { "group_voice",   { "voice", "wolf_dictate", "wolf_readaloud", "wolf_sharescreen", "gestures" } },
        { "group_move",    { "move", "view", "fly", "groundsit", "mouselook_view" } },
        { "group_world",   { "people", "search", "map", "minimap", "destinations", "radar", "snapshot", "landmark_here", "teleport_history" } },
        { "group_me",      { "appearance", "inventory", "animationoverride", "profile", "picks" } },
        { "group_display", { "quickprefs", "preferences", "phototools" } },
    };
    return sGroups;
}

const WolfToolbarGroups::Group* WolfToolbarGroups::find(const std::string& name)
{
    for (const Group& g : groups())
    {
        if (name == g.mName) return &g;
    }
    return nullptr;
}

bool WolfToolbarGroups::memberListed(const std::string& member_name)
{
    // Wolf Territories-only commands are not shown off-grid at all (lltoolbarview.cpp rule).
    if (member_name.rfind("wolf_", 0) == 0 && !WolfGrid::isWolfTerritories()) return false;
    LLCommand* cmd = LLCommandManager::instance().getCommand(member_name);
    if (!cmd) return false;
    // A command without an execute_function is driven by a control_name (<FS:Zi>); the
    // toolbar button binds it as a checkbox, which a menu item built here cannot mirror.
    if (cmd->executeFunctionName().empty()) return false;
    return LLUICtrl::CommitCallbackRegistry::getValue(cmd->executeFunctionName()) != nullptr;
}

// Source: lltoolbar.cpp:1162-1190 — a toolbar button executes its command through the
// CommitCallbackRegistry by execute_function name with execute_parameters; this does the
// identical lookup for a menu item. The spawning button is passed as the ctrl argument, as
// the toolbar passes its button.
void WolfToolbarGroups::runMember(LLUICtrl* ctrl, const LLSD& member_name)
{
    LLCommand* cmd = LLCommandManager::instance().getCommand(member_name.asString());
    if (!cmd) return;
    LLUICtrl::commit_callback_t* fn = LLUICtrl::CommitCallbackRegistry::getValue(cmd->executeFunctionName());
    if (!fn) return;
    (*fn)(ctrl, cmd->executeParameters());
}

// Source: lltoolbar.cpp:1193-1206 — is_running_function / is_running_parameters through the
// EnableCallbackRegistry; the button's toggle state is read from it every frame.
bool WolfToolbarGroups::memberRunning(LLUICtrl* ctrl, const LLSD& member_name)
{
    LLCommand* cmd = LLCommandManager::instance().getCommand(member_name.asString());
    if (!cmd || cmd->isRunningFunctionName().empty()) return false;
    LLUICtrl::enable_callback_t* fn = LLUICtrl::EnableCallbackRegistry::getValue(cmd->isRunningFunctionName());
    if (!fn) return false;
    return (*fn)(ctrl, cmd->isRunningParameters());
}

// Source: lltoolbar.cpp:1146-1160 — is_enabled_function / is_enabled_parameters.
bool WolfToolbarGroups::memberEnabled(LLUICtrl* ctrl, const LLSD& member_name)
{
    LLCommand* cmd = LLCommandManager::instance().getCommand(member_name.asString());
    if (!cmd) return false;
    if (cmd->isEnabledFunctionName().empty()) return true;
    LLUICtrl::enable_callback_t* fn = LLUICtrl::EnableCallbackRegistry::getValue(cmd->isEnabledFunctionName());
    if (!fn) return true;
    return (*fn)(ctrl, cmd->isEnabledParameters());
}

bool WolfToolbarGroups::isRunning(LLUICtrl* ctrl, const LLSD& group_name)
{
    const Group* g = find(group_name.asString());
    if (!g) return false;
    for (const char* member : g->mMembers)
    {
        if (memberListed(member) && memberRunning(ctrl, LLSD(member))) return true;
    }
    return false;
}

// Source: fspanelpreferenceuisounds.cpp:79-92 — an LLContextMenu built in code from
// LLMenuItem*GL::Params and parented to gMenuHolder; llviewermenu.cpp:12366 — shown with
// LLMenuGL::showPopup(spawning_view, menu, x, y) in the spawning view's coordinates.
LLContextMenu* WolfToolbarGroups::build(const Group& group)
{
    LLContextMenu::Params menu_params;
    menu_params.name(std::string("wolf_toolbar_") + group.mName);
    menu_params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);

    for (const char* member : group.mMembers)
    {
        if (!memberListed(member)) continue;
        LLCommand* cmd = LLCommandManager::instance().getCommand(member);
        LLMenuItemCheckGL::Params p;
        p.name(member);
        p.label(LLTrans::getString(cmd->labelRef()));
        p.on_click.function_name = "WolfToolbarGroups.Run";
        p.on_click.parameter = LLSD(member);
        p.on_check.function_name = "WolfToolbarGroups.MemberRunning";
        p.on_check.parameter = LLSD(member);
        p.on_enable.function_name = "WolfToolbarGroups.MemberEnabled";
        p.on_enable.parameter = LLSD(member);
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCheckGL>(p));
    }
    gMenuHolder->addChild(menu);
    return menu;
}

void WolfToolbarGroups::open(LLUICtrl* ctrl, const LLSD& group_name)
{
    const Group* g = find(group_name.asString());
    if (!g || !ctrl) return;

    // One popup at a time; the previous one is torn down rather than left in the holder.
    if (LLContextMenu* old_menu = sMenuHandle.get())
    {
        old_menu->hide();
        old_menu->die();
    }
    LLContextMenu* menu = build(*g);
    sMenuHandle = menu->getHandle();
    if (menu->getChildList()->empty()) return;

    // LLContextMenu::show, NOT LLMenuGL::showPopup alone. Source: llmenugl.cpp:4263
    // `LLContextMenu::setVisible(bool visible) { if (!visible) hide(); }` — a context menu
    // ignores setVisible(true), which is all showPopup does to reveal it (MEASURED 2026-09-07:
    // the menu was built and positioned and never drawn — "i click them and nothing
    // happens"). show() takes SCREEN coordinates for the menu's top-left, arranges it, opens
    // it UPWARD when it would run past the bottom of the screen (llmenugl.cpp show(): "Open
    // upwards if menu extends past bottom") — which a bottom-toolbar button always does —
    // and sets the view visible itself. The toolbar's own right-click menu takes the same
    // path (lltoolbar.cpp:500).
    LLRect screen;
    ctrl->localRectToScreen(ctrl->getLocalRect(), &screen);
    menu->show(screen.mLeft, screen.mTop, ctrl);
}
