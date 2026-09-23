/**
 * @file wolfnearbyregions.h
 * @brief WolfViewer: the regions the viewer is connected to, the agent's own first, nearest next.
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

#ifndef WOLF_NEARBYREGIONS_H
#define WOLF_NEARBYREGIONS_H

#include <algorithm>
#include <vector>

#include "llagent.h"
#include "llviewerregion.h"
#include "llworld.h"

// [2026-09-23] Paul: "the region next to the one i'm in needs to show its water and paint on
// the ground textures even if i'm not in that region".
//
// Terrain paint and wave zones used to GUESS their neighbours' handles from the agent region's
// corners (x + {0, width, -256, -width, 256}, the same for y). That only finds a neighbour
// whose corner happens to sit on one of those offsets. From WolfFest 2026 (2048 m at grid
// 4944,5048) it asked for 6 regions and missed West World 99 and 100 (4944,5044 and
// 4948,5044), which touch its southern edge; stepping into West World 99 found 8. The viewer
// already knows exactly which regions are next to it: every one it is connected to is in
// LLWorld's region list, whatever its size or where along an edge it sits.
namespace WolfNearbyRegions
{
    // Metres between two region footprints (0 when they touch or overlap), from the global
    // origin and width LLWorld holds for each (llviewerregion.h getOriginGlobal / getWidth).
    inline F64 gapMetres(const LLViewerRegion* a, const LLViewerRegion* b)
    {
        const LLVector3d ao = a->getOriginGlobal(), bo = b->getOriginGlobal();
        const F64 aw = a->getWidth(), bw = b->getWidth();
        const F64 dx = llmax(0.0, llmax(bo.mdV[VX] - (ao.mdV[VX] + aw), ao.mdV[VX] - (bo.mdV[VX] + bw)));
        const F64 dy = llmax(0.0, llmax(bo.mdV[VY] - (ao.mdV[VY] + aw), ao.mdV[VY] - (bo.mdV[VY] + bw)));
        return sqrt(dx * dx + dy * dy);
    }

    // The agent's region, then every other live region nearest first, at most max_count.
    inline std::vector<LLViewerRegion*> regions(size_t max_count)
    {
        std::vector<LLViewerRegion*> out;
        LLViewerRegion* agent = gAgent.getRegion();
        if (!agent || max_count == 0) return out;
        std::vector<std::pair<F64, LLViewerRegion*>> others;
        for (LLViewerRegion* rgn : LLWorld::getInstance()->getRegionList())
        {
            if (!rgn || !rgn->isAlive() || rgn == agent) continue;
            others.emplace_back(gapMetres(agent, rgn), rgn);
        }
        std::stable_sort(others.begin(), others.end(),
                         [](const auto& l, const auto& r) { return l.first < r.first; });
        out.push_back(agent);
        for (const auto& o : others)
        {
            if (out.size() >= max_count) break;
            out.push_back(o.second);
        }
        return out;
    }

    // Their handles, in the same order.
    inline std::vector<U64> handles(size_t max_count)
    {
        std::vector<U64> out;
        for (LLViewerRegion* rgn : regions(max_count)) out.push_back(rgn->getHandle());
        return out;
    }
}

#endif // WOLF_NEARBYREGIONS_H
