/**
 * @file wolfterrainwriter.h
 * @brief WolfViewer: the exact-height terrain writer shared by the Cave Builder and the Terraform window.
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

#ifndef WOLF_TERRAIN_WRITER_H
#define WOLF_TERRAIN_WRITER_H

#include <functional>
#include <string>
#include <vector>

#include "stdtypes.h"
#include "wolfterraformmath.h"

class LLViewerRegion;

/**
 * [WOLF TERRAFORM 2026-10-23] DESIGN.md 3.1 B: "WolfCaves::shape (wolfcave.cpp:1016-1128) moved into its own class, so caves and
 * every new tool share one writer". It sends each run as one ModifyLand flatten flood (Action 0, Seconds 25: the region SETS the
 * rectangle to the height, FlattenArea.cs:47-58), reliably, MODIFY_GAP_SECS apart (the region drops a ModifyLand that comes
 * within 20 ms of the last: TerrainModule.cs:1792-1798, :1946), then waits READBACK_SECS for the changed patches, reads the
 * ground back from the viewer's own terrain and sends what did not take once more. MODIFY_GAP_SECS stays at the Cave Builder's
 * 60 ms: the region's own rule is 20 ms after it finished the last one (TerrainModule.cs:1946) and only while its terrain update
 * does not hold the lock (:1792-1794 TryEnter), counted from when the packets ARRIVE, so the gap is the 20 ms plus a margin for
 * network jitter bunching them up; nothing in the source proves a smaller gap safe. Only ever called from a coroutine (it
 * suspends between floods). One write at a time in this viewer (busy()).
 */
class WolfTerrainWriter
{
public:
    // Source: wolfcave.cpp (before 2026-10-23) MODIFY_GAP_SECS, FLATTEN_SECONDS, ACTION_FLATTEN, HEIGHT_TOLERANCE and the 3 s
    // wait in shape's misses() - moved here unchanged.
    static constexpr F32 MODIFY_GAP_SECS = 0.06f;
    static constexpr F32 FLATTEN_SECONDS = 25.f;
    static constexpr U8 ACTION_FLATTEN = 0;          // TerrainModule.cs:61-69 StandardTerrainEffects.Flatten = 0
    static constexpr F32 HEIGHT_TOLERANCE = 0.06f;   // a read-back point this near its height is taken without the model
    static constexpr F32 READBACK_SECS = 3.f;        // LayerData for the changed patches

    struct Hooks
    {
        /** The region to send the next flood to, or null to stop (the job left it, or may not send now). */
        std::function<LLViewerRegion*()> sendRegion;
        /** The region to read the ground back from, or null. */
        std::function<LLViewerRegion*()> readRegion;
        /** Stop between floods. */
        std::function<bool()> cancelled;
        /** After each flood: how many of this pass are sent, of how many. */
        std::function<void(S32 sent, S32 total)> sent;
        /** [WOLF TERRAFORM 2026-10-23 Phase 3] What the job is, for the region's undo history (WolfTerraform cap only). */
        std::string label;
    };
    /** Turns the points still to write into floods (the Cave Builder its own column runs, the Terraform tools planRuns). */
    typedef std::function<std::vector<WolfTerraformMath::Run>(const std::vector<WolfTerraformMath::Point>&)> Planner;

    /** Why a point may not have taken (bits): MAY_RIGHTS - its land is not known to be this agent's and the agent does not
     *  manage the estate (PermissionsModule.cs:2028-2067 CanTerraformLand; llviewerregion.cpp:1039-1044 canManageEstate);
     *  MAY_LIMITS - the estate's raise / lower limits could hold it (TerrainModule.cs:1421-1489 LimitChannelChanges; they
     *  cannot when they span the whole height range, RegionSettings.cs:355-356 FullTerrainRaiseLimit = 4000 - -100). Neither:
     *  the region could not have refused it - it was dropped (the region skips a ModifyLand while its terrain update holds
     *  the lock, TerrainModule.cs:1243-1262 / :1792-1794), or this viewer's rounded view cannot confirm it. */
    enum { MAY_RIGHTS = 1, MAY_LIMITS = 2 };

    struct Result
    {
        bool stoppedFirst = false;                       // the first pass did not finish (cancelled, or the region went)
        std::vector<WolfTerraformMath::Point> left;      // the points still off after the second pass (all of them if stoppedFirst)
        std::vector<U8> why;                             // per point of `left`: MAY_* bits
        bool limitsKnown = false;                        // the region answered RequestRegionInfo for the limits below
        F32 raiseLimit = 0.f, lowerLimit = 0.f;          // metres from the region's baked ground (RegionInfo TerrainRaiseLimit / -Lower)
        // [WOLF TERRAFORM 2026-10-23 Phase 3] through the region's WolfTerraform cap (wolfterraincap.h): `why` is then exact
        // (the region's own answer per cell), and `reason` the region's words for what it did not set ("" when it set it all)
        bool viaCap = false;
        std::string reason;
    };

    /** Write `pts` (wolfcave.cpp shape: first pass, read-back, one more pass for the misses, read-back). The read-back judges a
     *  point by the LayerData model (wolflayerdata.h judge), not by a fixed tolerance alone. */
    static Result write(const std::vector<WolfTerraformMath::Point>& pts, const Planner& plan, const Hooks& hooks);

    /** Send these floods, MODIFY_GAP_SECS apart; false when stopped part way. */
    static bool send(const std::vector<WolfTerraformMath::Run>& runs, const Hooks& hooks);

    /** A write is going on (the Cave Builder and the Terraform window each refuse to start one then). */
    static bool busy() { return sWriting > 0; }

    /**
     * Seconds a flood took on this viewer, measured over the last pass of at least MEASURE_MIN_RUNS floods (the gap, the frame
     * the coroutine wakes on, the send), kept in WolfTerraformSecsPerFlood; 0 until one has been measured. The Terraform window
     * shows a time only from this (DESIGN.md 3.1: "Measure it before promising times in the UI").
     */
    static F32 measuredSecsPerFlood();

private:
    /** The flood route (every OpenSim region): what write() did before Phase 3, unchanged. */
    static Result writeFloods(const std::vector<WolfTerraformMath::Point>& pts, const Planner& plan, const Hooks& hooks);

    static constexpr S32 MEASURE_MIN_RUNS = 20;
    static S32 sWriting;
};

#endif // WOLF_TERRAIN_WRITER_H
