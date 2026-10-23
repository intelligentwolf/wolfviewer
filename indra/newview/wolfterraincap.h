/**
 * @file wolfterraincap.h
 * @brief WolfViewer: the terrain writer's fast route, the region's WolfTerraform capability.
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

#ifndef WOLF_TERRAIN_CAP_H
#define WOLF_TERRAIN_CAP_H

#include <string>
#include <vector>

#include "wolfterrainwriter.h"

class LLViewerRegion;

/**
 * [WOLF TERRAFORM 2026-10-23] DESIGN.md 3.1 B "Route 1, the WolfTerraform cap (when the region offers it)". Paul: "yeah do that
 * please". WolfTerrainWriter::write asks this first: when the region offers the "WolfTerraform" capability (WolfSim
 * WolfTerraformModule.cs; listed in llviewerregion.cpp capabilityNames), the points go as rectangles of heights, one request per
 * 256 m block (wolfterraincapmath.h), instead of one ModifyLand flood per run. The region checks every cell's rights and the
 * estate limits exactly as for ModifyLand, and answers which cells it did not set and why, in words.
 * A region without the cap (an older WolfSim, another grid, a paged region on whole-heightmap physics) has no URL, and the
 * writer's flood route is used unchanged.
 */
namespace WolfTerrainCap
{
    constexpr const char* CAP = "WolfTerraform";   // WolfTerraformModule.CAP
    constexpr int BUSY_TRIES = 30;                 // a busy answer (rate limit, terrain busy) is waited out this many times
    constexpr F32 BUSY_MAX_WAIT_SECS = 10.f;       // at most this long per wait, whatever retry_after says
    constexpr F32 SETTLE_STEP_SECS = 0.25f;        // after the last request: how often the viewer's own ground is looked at
    constexpr F32 SETTLE_MAX_SECS = 3.f;           // ... until it shows the new heights (LayerData), at most this long

    /** The region's WolfTerraform URL, or empty. */
    std::string url(LLViewerRegion* rgn);

    /**
     * Write `pts` through the capability (from a coroutine). True when the cap was used: `out` is the result (left = the points
     * the region refused or held at a limit, reason = its words), and `rest` the points it could not send because the cap stopped
     * answering part way (the writer floods those). False when the region has no cap: nothing was sent.
     */
    bool write(const std::vector<WolfTerraformMath::Point>& pts, const WolfTerrainWriter::Hooks& hooks,
               WolfTerrainWriter::Result& out, std::vector<WolfTerraformMath::Point>& rest);
}

#endif // WOLF_TERRAIN_CAP_H
