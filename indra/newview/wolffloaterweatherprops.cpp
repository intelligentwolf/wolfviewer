/**
 * @file wolffloaterweatherprops.cpp
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
#include "llviewerprecompiledheaders.h"

#include "wolffloaterweatherprops.h"

#include "llbutton.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "wolfgrid.h"
#include "wolfweatherprops.h"

WolfFloaterWeatherProps::WolfFloaterWeatherProps(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterWeatherProps::postBuild()
{
    mStatus = getChild<LLTextBox>("status");
    mRetry = getChild<LLButton>("retry");
    mRetry->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfWeatherProps::instance().retry(); });
    return true;
}

void WolfFloaterWeatherProps::draw()
{
    // Wolf Territories only (paul-wolf-features-wolf-grid-only): no window reachable elsewhere
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    const WolfWeatherProps& wp = WolfWeatherProps::instance();
    if (mStatus && mStatus->getText() != wp.statusText())
    {
        mStatus->setText(wp.statusText());
        // the words as well as the colour; a problem in the caution colour, as the Wolf HUDs window's status line
        // (wolffloaterhuds.cpp refreshTiles: AlertCautionTextColor / LabelTextColor)
        mStatus->setColor(LLUIColorTable::instance().getColor(wp.statusIsError() ? "AlertCautionTextColor" : "LabelTextColor"));
    }
    if (mRetry) mRetry->setVisible(wp.canRetry());
    LLFloater::draw();
}
