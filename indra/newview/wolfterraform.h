/**
 * @file wolfterraform.h
 * @brief WolfViewer: the Terraform tools' plans (target heights from the viewer's own ground), their history (Undo / Redo) and
 *        the jobs that write them through WolfTerrainWriter.
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

#ifndef WOLF_TERRAFORM_H
#define WOLF_TERRAFORM_H

#include <map>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v2math.h"
#include "wolfterraformmath.h"

class LLViewerRegion;

/**
 * [WOLF TERRAFORM 2026-10-23] Paul: "where is the extra terraforming tools you know we were going to enhance it based on other
 * terraforming applications". Design: firestorm/region-terrain-work/terraforming-tools/DESIGN.md, phases 1 and 2.
 * Persona: Jimmy, a region owner building a sim, who wants level pads, ramps, roads, rivers and beaches fast, and to take a
 * mistake back, without learning an expert tool. Story: "I pick a tool, click or draw on the ground, see what will change, press
 * Apply, watch it work, and can take it back."
 *
 * Every tool works its target heights out here, in the viewer, from the ground the viewer already holds (LLSurface; never
 * changed here - the region's terrain stays the only truth), then the job writes them with ModifyLand flatten floods
 * (WolfTerrainWriter), under the region's own ModifyLand rights: each cell is checked by the region (PermissionsModule.cs:
 * 2028-2067 CanTerraformLand) and a cell it refuses is left as it was and reported. Before writing, the heights it will
 * overwrite are kept for Undo. No server change.
 */
namespace WolfTerraform
{
    enum ETool { TOOL_LEVEL, TOOL_RAMP, TOOL_TERRACE, TOOL_SMOOTH, TOOL_ROAD, TOOL_RIVER, TOOL_BEACH, TOOL_COUNT };
    /** Level, Terrace, Smooth and Beach work in a box between two clicked corners; Ramp between two clicked ends; Road and River
     *  along a line of clicked points. */
    bool isBoxTool(ETool t);
    bool isRouteTool(ETool t);
    /** How many clicks the tool needs at least, and at most (route tools: no most, 0). */
    S32 minClicks(ETool t);
    S32 maxClicks(ETool t);

    /** Every tool's settings (the window's controls). NaN heights: from the ground where it was clicked. */
    struct Params
    {
        F32 levelHeight = WolfTerraformMath::nan();
        F32 levelEdge = 0.f;
        F32 rampWidth = 6.f, rampShoulder = 3.f;
        F32 rampHeightA = WolfTerraformMath::nan(), rampHeightB = WolfTerraformMath::nan();
        F32 terraceStep = 2.f, terraceSharpness = 0.8f, terraceEdge = 2.f;
        S32 smoothRadius = 8;
        F32 smoothStrength = 0.7f;
        F32 roadWidth = 6.f, roadShoulder = 4.f, roadMaxGrade = 0.25f;
        bool roadFollow = true;
        F32 riverWidth = 6.f, riverDepth = 1.5f, riverBanks = 4.f;
        bool riverToSea = false;
        F32 beachSlope = 0.1f, beachAbove = 2.f, beachBelow = 2.f, beachEdge = 4.f;
        // [2026-10-29 ticket 355] the box tools' brush shape (wolfterraformmath.h Oval): false = Square, the whole box (as
        // before); true = Round, the circle (oval) inside it, its rim eased over roundSoft (0..1) of the radius
        bool round = false;
        F32 roundSoft = 1.f;
    };

    /** What a tool will do: its target, the heights it will overwrite, the floods, and the plain-words summary. */
    struct Plan
    {
        ETool mTool = TOOL_LEVEL;
        std::string mLabel;                     // the history's name for it ("Ramp 52 m")
        std::string mError;                     // why it cannot be applied (shown; Apply greyed); empty = it can
        std::vector<std::string> mNotes;        // warnings that do not stop it (a steep grade)
        LLUUID mRegion;
        S32 mRegionW = 0;
        WolfTerraformMath::Grid mTarget;        // NaN = left alone
        WolfTerraformMath::Grid mBefore;        // the ground now, where mTarget has a value
        std::vector<WolfTerraformMath::Run> mRuns;
        S32 mCells = 0;                         // cells it changes (more than WRITE_SKIP_M)
        S32 mNotYours = 0;                      // cells on land this viewer does not know to be yours (the region decides)
        F32 mMaxCut = 0.f, mMaxFill = 0.f;
        bool ok() const { return mError.empty() && !mRuns.empty(); }
    };

    // [2026-10-23 Paul's Beach "its pretty slow"] A run is flooded at the middle of its targets' band, so no cell is more than
    // WRITE_BAND_M / 2 off, and a cell already within WRITE_SKIP_M of its target is left alone. 0.1 m / 0.05 m (was the Cave
    // Builder's 0.04 / 0.02): every viewer sees the ground through LayerData's rounding, which the exact model of it
    // (wolflayerdata.h, tests/wolfterraformmath_test.cpp) puts at about 0.04-0.06 m on a smooth 16 m patch and tenths of a metre
    // on a stepped one, so 5 cm is below what anyone can see, and an avatar's step is far larger. It halves the floods of a
    // sloping job: on a 10 % slope each metre differs by 0.1 m, and two neighbours can now share a flood.
    constexpr F32 WRITE_BAND_M = 0.1f;
    constexpr F32 WRITE_SKIP_M = 0.05f;
    // The window's own limits for one job through the writer (DESIGN.md 3.1: the writer is for local edits; "whole-region
    // operations require the module", Phase 3): the box or route's area at most MAX_SIDE_M a side, at most MAX_FLOODS floods.
    constexpr S32 MAX_SIDE_M = 512;
    constexpr S32 MAX_FLOODS = 6000;
    constexpr S32 SMOOTH_RADIUS_MAX = 32;   // DESIGN.md 3.2 #7 "a chosen radius (up to 32 m)"

    /** Work out what `tool` does with these clicks (region metres, the agent's region) and settings. */
    Plan makePlan(ETool tool, const std::vector<LLVector2>& clicks, const Params& p);
    /** "about 40 s" from this viewer's measured writer speed, or a plain note that no time can be given yet. */
    std::string timeText(size_t floods);

    /** One step of the history: the heights it overwrote, and (once it has been undone) the heights it had made. */
    struct Step
    {
        std::string mLabel;
        U64 mSeq = 0;
        bool mStopped = false;                  // the job stopped part way (Undo still puts back what it changed)
        // an Undo / Redo of this step stopped part way: pressing it again must not take the half-way ground as the step's own
        // after (Undo) or before (Redo) heights
        bool mUndoPartial = false, mRedoPartial = false;
        WolfTerraformMath::Grid mBefore, mAfter;
        size_t bytes() const { return (mBefore.cells() + mAfter.cells()) * sizeof(F32); }
    };

    /**
     * The jobs and the history. One job at a time in this viewer, and none while a Cave Builder job runs (both write the ground).
     * The history is this viewer's, kept in memory only (gone on restart), per region, at most MAX_STEPS steps a region and
     * MAX_HISTORY_BYTES in all (the oldest go first): Paul's no-eternal-logs rule.
     */
    class Jobs : public LLSingleton<Jobs>
    {
        LLSINGLETON_EMPTY_CTOR(Jobs);
    public:
        static constexpr size_t MAX_STEPS = 20;                       // DESIGN.md 3.7 phase 1: "20 steps or 32 MB"
        static constexpr size_t MAX_HISTORY_BYTES = 32u * 1024u * 1024u;

        bool busy() const { return mBusy; }
        /** Stop at the next safe point (between floods). */
        void cancel() { if (mBusy) mCancel = true; }
        bool cancelling() const { return mBusy && mCancel; }
        const std::string& status() const { return mStatus; }
        F32 progress() const { return mProgress; }
        /** Bumped when a job ends; lastOk / lastText say how. */
        U32 serial() const { return mSerial; }
        bool lastOk() const { return mLastOk; }
        /** Bumped whenever a step changes (the window's history list follows it). */
        U32 historyVersion() const { return mHistoryVersion; }
        const std::string& lastText() const { return mLastText; }

        /** Why a job cannot start now, or empty. */
        std::string startBlocker(const LLUUID& region) const;
        bool apply(const Plan& plan, std::string& why_not);
        bool undo(const LLUUID& region, std::string& why_not);
        bool redo(const LLUUID& region, std::string& why_not);
        /** The region's steps, oldest first, and how many of them are done (the rest can be redone). */
        const std::vector<Step>& steps(const LLUUID& region) const;
        size_t done(const LLUUID& region) const;
        /** How many floods Undo / Redo of the next step would take (for its time), 0 when there is none. */
        size_t undoFloods(const LLUUID& region) const;
        size_t redoFloods(const LLUUID& region) const;

    private:
        struct History { std::vector<Step> mSteps; size_t mDone = 0; };
        enum EKind { KIND_APPLY, KIND_UNDO, KIND_REDO };
        void start(const std::string& line);
        void runCoro(EKind kind, LLUUID region, Plan plan, std::string verb, U64 seq);
        void finish(bool ok, const std::string& text);
        /** Drop the oldest steps over the caps, never the step `keep_seq` (the one a job is about). */
        void trim(U64 keep_seq);
        Step* findStep(const LLUUID& region, U64 seq);

        std::map<LLUUID, History> mHistory;
        U64 mNextSeq = 1;
        U32 mHistoryVersion = 0;
        bool mBusy = false;
        bool mCancel = false;
        std::string mStatus;
        F32 mProgress = -1.f;
        U32 mSerial = 0;
        bool mLastOk = true;
        std::string mLastText;
    };

    /** The ground this viewer holds at cell (x, y) of `rgn`: false where its patch has not arrived (its heights would be 0). */
    bool knownHeight(LLViewerRegion* rgn, S32 x, S32 y, F32& z);
}

#endif // WOLF_TERRAFORM_H
