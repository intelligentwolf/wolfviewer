/**
 * @file wolffloateragecheck.h
 * @brief WolfViewer: Wolf World > Age Check... - the resident's age check status and the "Verify my age" link
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

#ifndef WOLF_FLOATERAGECHECK_H
#define WOLF_FLOATERAGECHECK_H

#include "llfloater.h"
#include "llframetimer.h"

// [ADULT CONTENT 2026-10-26] Paul: residents need a way to see their verification status, plus a "Verify my age"
// entry, under Wolf World (his rule: every Wolf feature there). Who and why: a Wolf resident wants to know whether they
// can enter Adult Content regions and, if not, start the age check in one click.
// The status is SimulatorFeatures "WolfAgeVerified" (region-terrain-work/adult-content/PROTOCOL.md §3a/§4), which the
// region sends on arrival only: the window says so, and points to wolf-grid.com where the status is always current.
// There is deliberately no web lookup of it (§4). Wolf Territories logins only (the menu item is gone elsewhere).
class WolfFloaterAgeCheck : public LLFloater
{
public:
    WolfFloaterAgeCheck(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refresh() override;
    LLFrameTimer mRefreshTimer;
};

#endif // WOLF_FLOATERAGECHECK_H
