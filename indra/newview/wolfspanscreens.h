/**
 * @file wolfspanscreens.h
 * @brief WolfViewer: spread the viewer window across every screen (Wolf World > Spread Across All
 *        Screens).
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

#ifndef WOLF_SPANSCREENS_H
#define WOLF_SPANSCREENS_H

// [SPAN SCREENS 2026-10-10] Paul: "can we add an option to make the viewer spread across all
// available screens?", "wouldnt you just get the width of the total screens and resize the window
// to it?", "make sure everyone gets the feature".
//
// The window code does the work where the window can place itself (LLWindow::spanAllScreens:
// Windows, macOS, Linux on X11). Hyprland (native Wayland or XWayland) is asked over its socket to
// float the window over the whole desktop, as Paul's SUPER+ALT+W script does. On any other Wayland
// desktop a window cannot place itself, so the viewer offers to restart through XWayland, where the
// X11 way works. The choice is kept in WolfSpanAllScreens and put back at the next start.
namespace WolfSpanScreens
{
    void toggle();                  // the menu item
    bool isOn();                    // the menu tick: the window is spread now
    bool active();                  // do not save this window size or place as the normal one
    void startup();                 // after the window opens: spread it again if it was
}

#endif // WOLF_SPANSCREENS_H
