/**
 * @file wolfinvisible.cpp
 * @brief WolfViewer: Invisible Mode - appear offline to friends, instant messages and friend requests.
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

#include "wolfinvisible.h"

#include "llagent.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerregion.h"
#include "wolfgrid.h"

// <WolfViewer 2026-10-11> Paul: Invisible Mode (wolfinvisible.h).
namespace
{
    // The method the WolfSim module listens for; its one parameter is "1" (invisible) or "0".
    const std::string METHOD_TO_SIM("wolfinvisible");

    // The region last told, and what it was told. Cleared on every region change, so the region
    // arrived in is told even when it is one the agent was in before (a new presence there).
    LLUUID sToldRegion;
    bool sToldValue = false;
    // Whether any region has been told "1" this session: the "off" note is only shown after an "on".
    bool sToldOnThisSession = false;
    // The login region gets the note too, once, so a user who switched it on long ago is reminded.
    bool sToldThisSession = false;
    // <WolfViewer 2026-10-11> the login these belong to: a logout and a new login in the same run (the
    // session id changes, llstartup.cpp) starts them again, so the reminder shows at the new login.
    LLUUID sSession;

    boost::signals2::connection sRegionChanged;
}

void WolfInvisible::idle()
{
    // Source: wolfgamehud.cpp idle - the same "logged in and in a region" test before sending.
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion())
    {
        return;
    }
    if (!sRegionChanged.connected())
    {
        // Source: llagent.cpp LLAgent::setRegion fires this at the end of every region switch
        // (login region, teleport, crossing) - the same hook wolfgame.cpp's constructor uses.
        sRegionChanged = gAgent.addRegionChangedCallback([]() { sToldRegion.setNull(); });
    }
    // A teleport in flight: wait for the arrival, then tell the region arrived in.
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        return;
    }
    // [WOLF GRID GATE] Only to a region that has itself said it is Wolf Territories
    // (wolfgrid.h isOnWolfTerritoriesConfirmed, the check wolfgame.cpp send() and wolfdrive.cpp
    // canSend() use for sending). Until its features arrive nothing is recorded, so the next frames
    // try again; a foreign region never hears of it.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }

    if (gAgentSessionID != sSession)
    {
        sSession = gAgentSessionID;
        sToldRegion.setNull();
        sToldValue = false;
        sToldOnThisSession = false;
        sToldThisSession = false;
    }

    static LLCachedControl<bool> invisible(gSavedSettings, "WolfInvisibleMode", false);
    const bool want = invisible;
    const LLUUID region = gAgent.getRegion()->getRegionID();
    if (region == sToldRegion && want == sToldValue)
    {
        return;
    }
    const bool changed = !sToldThisSession || want != sToldValue;
    sToldRegion = region;
    sToldValue = want;

    // Source: llviewergenericmessage.cpp send_generic_message - goes to the agent's current region
    // (gAgent.sendReliableMessage); each parameter goes out with its NUL, the byte OpenSim's
    // Util.FieldToString removes.
    send_generic_message(METHOD_TO_SIM, { want ? "1" : "0" });

    // A note the user can see, not only a log line: at login when it is on, and whenever it is
    // switched while logged in. Region changes alone say nothing.
    if (changed)
    {
        if (want)
        {
            LLNotificationsUtil::add("WolfInvisibleOn");
            sToldOnThisSession = true;
        }
        else if (sToldOnThisSession)
        {
            LLNotificationsUtil::add("WolfInvisibleOff");
        }
    }
    sToldThisSession = true;
    LL_INFOS("WolfInvisible") << "told region " << region << " invisible=" << (want ? "1" : "0") << LL_ENDL;
}
// </WolfViewer 2026-10-11>
