/**
 * @file wolfloadingnotice.h
 * @brief WolfViewer: "Loading Wolf water..." at the bottom left of the world view while the Wolf sea or sky is on its way.
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

#ifndef WOLF_LOADING_NOTICE_H
#define WOLF_LOADING_NOTICE_H

#include "lluictrl.h"

// [WOLF VISITOR SEES 2026-10-18] Paul, on the Wolf sea taking a moment to appear after arriving: "the delay is okay if you
// tell the user like a 'Loading Wolf Water' message at the bottom left". One line over the world view (main_view.xml, every
// skin that has its own), drawn only on a region that has itself said it is Wolf (WolfGrid::isOnWolfRegionConfirmed), only
// while something is still to come, and never for longer than TIMEOUT_S after an arrival. Takes no clicks.
class WolfLoadingNotice : public LLUICtrl
{
public:
    // its own Params type: LLUICtrlFactory::registerWidget refuses one Params type under two widget names
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfLoadingNotice(const Params& p);
    void draw() override;

    static constexpr F64 TIMEOUT_S = 20.0;

private:
    std::string mRegionId;     // the agent region the arrival clock below is for
    F64 mArrivedAt = 0.0;
};

#endif // WOLF_LOADING_NOTICE_H
