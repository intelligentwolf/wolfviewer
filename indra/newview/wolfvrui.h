/**
 * @file wolfvrui.h
 * @brief WolfViewer VR in the headset browser: the in-VR action bar's viewer side - each button runs the viewer's own
 *        toolbar command, and the bar's state (which are on, which cannot be used, sitting, flying, favourites) goes
 *        back to the headset page.
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

#ifndef WOLF_VRUI_H
#define WOLF_VRUI_H

#include <string>

// [VR CONTROLS 2026-11-04] Paul: "there's no controls in full 3D mode; in WolfStorm there are lots of controls that
// work". The headset page draws WolfStorm's VR action bar (wolfstorm/js/vr/vr_hud.js) and sends the button pressed;
// this runs the SAME command the desktop toolbar button runs (app_settings/commands.xml execute_function and its
// parameters, through LLUICtrl::CommitCallbackRegistry, as LLToolBar does), so the bar and the toolbar cannot do
// different things. Floaters it opens appear on the UI layer in the headset (wolfvrscreen.cpp captureLayer).
namespace WolfVRUi
{
    /** One button from the page: a commands.xml command name on the bar's list, or "home", "sit" (sit on the ground
     *  or stand up), "fav" (arg = a favourite landmark's inventory item id). Anything else is ignored. Main thread. */
    void run(const std::string& name, const std::string& arg);
    /** Sends the bar's state to the page when it changed (and every few seconds). Main thread, from the stream's pump. */
    void pump();
}

#endif // WOLF_VRUI_H
