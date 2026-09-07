/**
 * @file wolftoolbargroups.h
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

#ifndef WOLF_TOOLBARGROUPS_H
#define WOLF_TOOLBARGROUPS_H

#include <string>
#include <vector>

#include "llhandle.h"
#include "llsd.h"

class LLUICtrl;
class LLContextMenu;

// Source: wolfstorm/js/ui/toolbar_groups.js (2026-09-07) — the same five groups, the same
// members, so the web viewer and this one read alike. Paul: "the toolbars have too many
// buttons on them we need to group buttons together by what they do".
//
// A group is an ordinary toolbar command (app_settings/commands.xml group_*): it has an
// icon, a label, sits on any toolbar, drags in the toybox like every other button. What it
// EXECUTES is a popup menu of its member commands — each item runs the member's own
// execute_function with its own parameters (lltoolbar.cpp:1162-1190 does the same lookup
// for a button), is ticked from the member's is_running_function (lltoolbar.cpp:1193-1206)
// and greyed from its is_enabled_function. The group button itself shows the running tick
// whenever any member is running, so a lit Nearby Voice or an open Map still reads from the
// bar. Members are never re-implemented here: a member with no execute_function (the
// <FS:Zi> control_name style) is simply not listed.
//
// Wolf Territories-only members (wolf_*) are left out of the menu on any other grid — the
// rule from lltoolbarview.cpp / llfloatertoybox.cpp, kept: they do not appear off-grid.
class WolfToolbarGroups
{
public:
    struct Group
    {
        const char*              mName;      // command name, commands.xml group_*
        std::vector<const char*> mMembers;   // member command names, menu order
    };

    /** The five groups, in toolbar order. */
    static const std::vector<Group>& groups();
    /** The group named `name`, or nullptr. */
    static const Group* find(const std::string& name);

    // Registered in llviewermenu.cpp:
    /** execute_function of a group command: pop the member menu above the button. */
    static void open(LLUICtrl* ctrl, const LLSD& group_name);
    /** is_running_function of a group command: any member running. */
    static bool isRunning(LLUICtrl* ctrl, const LLSD& group_name);
    /** Menu item callbacks (parameter = member command name). */
    static void runMember(LLUICtrl* ctrl, const LLSD& member_name);
    static bool memberRunning(LLUICtrl* ctrl, const LLSD& member_name);
    static bool memberEnabled(LLUICtrl* ctrl, const LLSD& member_name);

    /** True when this member may be listed at all (exists, executable, allowed on this grid). */
    static bool memberListed(const std::string& member_name);

private:
    static LLContextMenu* build(const Group& group);
    static LLHandle<LLContextMenu> sMenuHandle;
};

#endif // WOLF_TOOLBARGROUPS_H
