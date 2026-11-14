/**
 * @file wolffloaterweatherprops.h
 * @brief WolfViewer: the Weather Props window - the switch, a tick per prop and what is happening now (ticket 847).
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
#ifndef WOLF_FLOATERWEATHERPROPS_H
#define WOLF_FLOATERWEATHERPROPS_H

#include "llfloater.h"

class LLButton;
class LLTextBox;

// [WEATHER PROPS 2026-11-13] Ticket 847. The switch and the ticks are settings (floater_wolf_weather_props.xml control_name:
// WolfViewerWeatherProps, WolfViewerWeatherPropsUmbrella / Shovel / Rake); the rules that act on them are wolfweatherprops.cpp,
// which also writes the status line this window shows. Wolf Territories only: off it the window closes itself (the menu item
// and the Wolf HUDs tile are gone there too).
class WolfFloaterWeatherProps : public LLFloater
{
public:
    WolfFloaterWeatherProps(const LLSD& key);

    bool postBuild() override;
    void draw() override;

private:
    LLTextBox* mStatus = nullptr;
    LLButton* mRetry = nullptr;
};

#endif // WOLF_FLOATERWEATHERPROPS_H
