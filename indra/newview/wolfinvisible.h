/**
 * @file wolfinvisible.h
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

#ifndef WOLF_INVISIBLE_H
#define WOLF_INVISIBLE_H

// <WolfViewer 2026-10-11> Paul: "create a new Invisible Mode, this can be turned on before login from
// the Wolf World menu or after login that basically makes the user invisible to IM's and Friend
// requests, like busy mode but show them offline when someone messages them."
// The hiding is done by Wolf Territories' servers (the WolfSim module and the login service); the
// viewer only says what the user chose:
//   - at login, request parameter "wolf_invisible" = "1" (lllogininstance.cpp constructAuthParams);
//   - to every Wolf region the agent arrives in, and whenever the switch changes while logged in,
//     GenericMessage method "wolfinvisible" with one parameter "1" or "0" (idle() below).
// The switch is the global setting WolfInvisibleMode (Wolf World menu, and the login screen's).
// Nothing visible to others is used: no Busy / Do Not Disturb, no animation.
namespace WolfInvisible
{
    /** Tell the current region the Invisible Mode state when it has not been told yet (arrival,
     *  region change, switch changed). Wolf Territories only. From LLAppViewer idle. */
    void idle();
}
// </WolfViewer 2026-10-11>

#endif // WOLF_INVISIBLE_H
