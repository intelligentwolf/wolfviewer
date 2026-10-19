/**
 * @file wolfviewerkey.h
 * @brief WolfViewer: the per-login key that tells Wolf Territories regions this login is WolfViewer's.
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

#ifndef WOLF_VIEWER_KEY_H
#define WOLF_VIEWER_KEY_H

// [COPYBOT EXEMPT 2026-10-19] Paul: "wolf territories grid should NEVER do copybot checking against wolfstorm and
// wolfviewer". The regions' copybot guard (WolfSim WolfCopybotGuard.cs) never trusts the viewer's name - a copybot
// logged in as "WolfViewer-Release" on 10-08 - so this viewer proves its login instead:
//   1. after login on Wolf Territories it asks wolfstorm.app/php/wolfviewer_key.php (X-Wolf-Agent + X-Wolf-Session,
//      checked there against the grid's presence service) for a key bound to the agent AND this session (6 h);
//   2. it gives that key to every region it has a circuit with (the one it stands in and its neighbours, whose
//      textures and meshes it also fetches: lltexturefetch.cpp LLTextureFetchWorker::getRegion uses the texture's
//      host region), GenericMessage "wolfviewerkey", and again to any region it meets later;
//   3. it renews the key at half its life and hands the new one round.
// The region then leaves this agent's downloads uncounted for this session. Wolf Territories logins only: a hypergrid
// visitor's session is never sent to us (wolfgrid.h showsWolfRegion), so visitors keep the guard.
namespace WolfViewerKey
{
    /** Fetch / renew the key and hand it to every region not yet given it. From LLAppViewer idle. */
    void idle();
}

#endif // WOLF_VIEWER_KEY_H
