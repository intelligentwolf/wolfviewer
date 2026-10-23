/**
 * @file wolfterraform.cpp
 * @brief WolfViewer: the Terraform tools' plans, history and jobs.
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

// [WOLF TERRAFORM 2026-10-23] See wolfterraform.h.

#include "llviewerprecompiledheaders.h"

#include "wolfterraform.h"

#include <cmath>

#include "llagent.h"
#include "llcoros.h"
#include "llparcel.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltoolbrush.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "wolfcave.h"
#include "wolffloatertunnels.h"
#include "wolfgrid.h"
#include "wolfterrainwriter.h"

using namespace WolfTerraformMath;

namespace WolfTerraform
{
    bool isBoxTool(ETool t) { return t == TOOL_LEVEL || t == TOOL_TERRACE || t == TOOL_SMOOTH || t == TOOL_BEACH; }
    bool isRouteTool(ETool t) { return t == TOOL_ROAD || t == TOOL_RIVER; }
    S32 minClicks(ETool) { return 2; }
    S32 maxClicks(ETool t) { return isRouteTool(t) ? 0 : 2; }

    bool knownHeight(LLViewerRegion* rgn, S32 x, S32 y, F32& z)
    {
        if (!rgn) return false;
        const LLSurface& land = rgn->getLand();
        const S32 w = (S32)rgn->getWidth(), gpp = land.getGridsPerPatchEdge(), ppe = land.getPatchesPerEdge();
        // one sample a metre (the region's cell x, y is the surface's grid point x, y): checked by makePlan as well
        if (land.getMetersPerGrid() != 1.f || gpp <= 0 || ppe <= 0 || x < 0 || y < 0 || x >= w || y >= w) return false;
        // Source: llsurface.cpp:659-673 getZ - the patch that owns the point; a patch made but never sent its heights reads 0
        // (llsurfacepatch.h:182 mHasReceivedData "has the patch EVER received height data?").
        const LLSurfacePatch* patch = land.findPatch(llmin(x / gpp, ppe - 1), llmin(y / gpp, ppe - 1));
        if (!patch || !patch->getHasReceivedData()) return false;
        z = land.getZ(x, y);
        return true;
    }

    std::string timeText(size_t floods)
    {
        const F32 per = WolfTerrainWriter::measuredSecsPerFlood();
        if (per <= 0.f) return "the time shows once this viewer has measured its first job";
        const S32 secs = (S32)std::ceil((F32)floods * per + WolfTerrainWriter::READBACK_SECS);
        if (secs < 60) return llformat("about %d s", secs);
        return llformat("about %d min %d s", secs / 60, secs % 60);
    }

    namespace
    {
        /** The ground at the cell nearest a click (a click on the region's far edge rounds onto its last cell). */
        F32 groundAt(const Grid& g, const LLVector2& p)
        {
            if (!g.w || !g.h) return WolfTerraformMath::nan();
            return g.at(llclamp((S32)ll_round(p.mV[0]), g.x0, g.x0 + g.w - 1), llclamp((S32)ll_round(p.mV[1]), g.y0, g.y0 + g.h - 1));
        }
        std::string pct(F32 grade) { return llformat("%d%%", (S32)ll_round(grade * 100.f)); }
    }

    Plan makePlan(ETool tool, const std::vector<LLVector2>& clicks, const Params& p)
    {
        Plan plan;
        plan.mTool = tool;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) { plan.mError = "You are not in a region yet."; return plan; }
        plan.mRegion = rgn->getRegionID();
        const S32 W = (S32)rgn->getWidth();
        plan.mRegionW = W;
        if (rgn->getLand().getMetersPerGrid() != 1.f)
        {
            plan.mError = "This region's ground is not one height a metre, so the Terraform tools cannot shape it.";
            return plan;
        }
        // The viewer's own estate rule for the land tools (lltoolbrush.cpp:684-689 canTerraformRegion: estate managers, or a region
        // without Block Terraform).
        if (!LLToolBrushLand::getInstance()->canTerraformRegion(rgn))
        {
            plan.mError = "This region does not allow terraforming (Block Terraform is on): only its estate owner and managers can change the ground.";
            return plan;
        }
        if ((S32)clicks.size() < minClicks(tool)) return plan;   // not drawn yet: the window says what to click

        // the area the tool writes, and the ground it needs round it
        Box area{ 0, 0, -1, -1 };
        S32 margin = 1;   // the ground a cell past the area (sample() between cells)
        const F32 water = rgn->getWaterHeight();
        Polyline line(std::vector<F32>{});
        if (isRouteTool(tool))
        {
            std::vector<F32> xy;
            for (const LLVector2& c : clicks) { xy.push_back(c.mV[0]); xy.push_back(c.mV[1]); }
            line = Polyline(xy);
            if (!line.usable()) { plan.mError = "The points are all on one spot - click further along."; return plan; }
            const F32 reach = tool == TOOL_ROAD ? p.roadWidth * 0.5f + p.roadShoulder : p.riverWidth * 0.5f + p.riverBanks;
            area = line.bounds(reach);
        }
        else if (tool == TOOL_RAMP)
        {
            const F32 reach = p.rampWidth * 0.5f + p.rampShoulder;
            area = Box{ (S32)std::floor(llmin(clicks[0].mV[0], clicks[1].mV[0]) - reach), (S32)std::floor(llmin(clicks[0].mV[1], clicks[1].mV[1]) - reach),
                        (S32)std::ceil(llmax(clicks[0].mV[0], clicks[1].mV[0]) + reach), (S32)std::ceil(llmax(clicks[0].mV[1], clicks[1].mV[1]) + reach) };
        }
        else
        {
            const Box b = boxOf(clicks[0].mV[0], clicks[0].mV[1], clicks[1].mV[0], clicks[1].mV[1]);
            const F32 edge = tool == TOOL_LEVEL ? p.levelEdge : tool == TOOL_TERRACE ? p.terraceEdge : 0.f;
            const S32 grow = (S32)std::ceil(llmax(0.f, edge));
            area = Box{ b.x0 - grow, b.y0 - grow, b.x1 + grow, b.y1 + grow };
            if (tool == TOOL_SMOOTH) margin = llclamp(p.smoothRadius, 1, SMOOTH_RADIUS_MAX) + 1;
        }
        const Box clip{ llmax(0, area.x0), llmax(0, area.y0), llmin(W - 1, area.x1), llmin(W - 1, area.y1) };
        if (clip.x1 < clip.x0 || clip.y1 < clip.y0) { plan.mError = "That is outside this region."; return plan; }
        const S32 side_w = clip.x1 - clip.x0 + 1, side_h = clip.y1 - clip.y0 + 1;
        if (side_w > MAX_SIDE_M || side_h > MAX_SIDE_M)
        {
            plan.mError = llformat("That covers %d x %d m; one job shapes at most %d m a side. Make it smaller, or do it in parts.",
                                   side_w, side_h, MAX_SIDE_M);
            return plan;
        }
        // The ground this viewer holds (never changed here). A cell it has not been sent is unknown: its heights could not be
        // kept for Undo, so the job is refused rather than guessed.
        Grid ground = areaGrid(clip, margin, W);
        S32 unknown = 0;
        for (S32 y = ground.y0; y < ground.y0 + ground.h; ++y)
            for (S32 x = ground.x0; x < ground.x0 + ground.w; ++x)
            {
                F32 z;
                if (knownHeight(rgn, x, y, z)) ground.ref(x, y) = z;
                else if (x >= clip.x0 && x <= clip.x1 && y >= clip.y0 && y <= clip.y1) ++unknown;
            }
        if (unknown > 0)
        {
            plan.mError = llformat("%d m² of that ground has not reached this viewer yet, so its heights could not be kept for Undo. "
                                   "Go nearer to it, wait a moment, then try again.", unknown);
            return plan;
        }

        switch (tool)
        {
        case TOOL_LEVEL:
        {
            const Box b = boxOf(clicks[0].mV[0], clicks[0].mV[1], clicks[1].mV[0], clicks[1].mV[1]);
            const F32 h = has(p.levelHeight) ? p.levelHeight : groundAt(ground, clicks[0]);
            plan.mTarget = level(ground, b, h, llmax(0.f, p.levelEdge), W);
            plan.mLabel = llformat("Level %d x %d m to %.2f m", b.x1 - b.x0 + 1, b.y1 - b.y0 + 1, h);
            break;
        }
        case TOOL_RAMP:
        {
            RampShape r;
            r.ax = clicks[0].mV[0]; r.ay = clicks[0].mV[1];
            r.bx = clicks[1].mV[0]; r.by = clicks[1].mV[1];
            r.az = has(p.rampHeightA) ? p.rampHeightA : groundAt(ground, clicks[0]);
            r.bz = has(p.rampHeightB) ? p.rampHeightB : groundAt(ground, clicks[1]);
            r.width = llmax(1.f, p.rampWidth);
            r.shoulder = llmax(0.f, p.rampShoulder);
            const F32 g = grade(r.ax, r.ay, r.az, r.bx, r.by, r.bz);
            const F32 len = dist_vec(clicks[0], clicks[1]);
            if (len < 1.f) { plan.mError = "The two ends are on one spot - click the far end further away."; return plan; }
            plan.mTarget = ramp(ground, r, W);
            plan.mLabel = llformat("Ramp %.0f m (%s)", len, pct(g).c_str());
            // Source: wolffloatertunnels.h WolfTunnels::WALK_GRADE_WARN (DESIGN.md 3.2 #2 "warns over the walk grade")
            if (g > WolfTunnels::WALK_GRADE_WARN)
                plan.mNotes.push_back(llformat("It rises %s - steeper than %s, the Tunnel tool's walking limit: people may slide on it.",
                                               pct(g).c_str(), pct(WolfTunnels::WALK_GRADE_WARN).c_str()));
            break;
        }
        case TOOL_TERRACE:
        {
            const Box b = boxOf(clicks[0].mV[0], clicks[0].mV[1], clicks[1].mV[0], clicks[1].mV[1]);
            const F32 step = llmax(0.25f, p.terraceStep);
            plan.mTarget = terrace(ground, b, step, llclamp(p.terraceSharpness, 0.f, 1.f), llmax(0.f, p.terraceEdge), W);
            plan.mLabel = llformat("Terraces %.1f m apart, %d x %d m", step, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
            break;
        }
        case TOOL_SMOOTH:
        {
            const Box b = boxOf(clicks[0].mV[0], clicks[0].mV[1], clicks[1].mV[0], clicks[1].mV[1]);
            plan.mTarget = smooth(ground, b, llclamp(p.smoothRadius, 1, SMOOTH_RADIUS_MAX), llclamp(p.smoothStrength, 0.f, 1.f), W);
            plan.mLabel = llformat("Smooth %d x %d m", b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
            break;
        }
        case TOOL_ROAD:
        {
            Profile gp;
            if (!groundProfile(ground, line, gp)) { plan.mError = "Part of the route's ground has not reached this viewer yet - go nearer, then try again."; return plan; }
            const F32 max_grade = llmax(0.01f, p.roadMaxGrade);
            const Profile prof = roadProfile(gp, llmax(1.f, p.roadWidth), p.roadFollow, max_grade);
            plan.mTarget = road(ground, line, prof, llmax(1.f, p.roadWidth), llmax(0.f, p.roadShoulder), W);
            plan.mLabel = llformat("Road %.0f m", line.len);
            const F32 g = steepest(prof);
            if (!p.roadFollow && g > max_grade + 1e-4f)
                plan.mNotes.push_back(llformat("The even grade is %s - steeper than your most (%s). Choose Follow the ground, or move an end.",
                                               pct(g).c_str(), pct(max_grade).c_str()));
            else if (g > WolfTunnels::WALK_GRADE_WARN)
                plan.mNotes.push_back(llformat("Its steepest part rises %s - steeper than %s, the Tunnel tool's walking limit.",
                                               pct(g).c_str(), pct(WolfTunnels::WALK_GRADE_WARN).c_str()));
            break;
        }
        case TOOL_RIVER:
        {
            Profile gp;
            if (!groundProfile(ground, line, gp)) { plan.mError = "Part of the route's ground has not reached this viewer yet - go nearer, then try again."; return plan; }
            const F32 depth = llmax(0.25f, p.riverDepth);
            const Profile bed = riverBed(gp, llmax(1.f, p.riverWidth), depth, p.riverToSea ? water - depth : nan());
            plan.mTarget = river(ground, line, bed, llmax(1.f, p.riverWidth), depth, llmax(0.f, p.riverBanks), W);
            plan.mLabel = llformat("River %.0f m", line.len);
            if (!p.riverToSea && bed.z.back() > water)
                plan.mNotes.push_back(llformat("Its bed ends at %.1f m, above the water (%.1f m): tick Run into the sea to carry it down to the water.",
                                               bed.z.back(), water));
            break;
        }
        case TOOL_BEACH:
        {
            const Box b = boxOf(clicks[0].mV[0], clicks[0].mV[1], clicks[1].mV[0], clicks[1].mV[1]);
            plan.mTarget = beach(ground, b, water, llmax(0.01f, p.beachSlope), llmax(0.f, p.beachAbove), llmax(0.f, p.beachBelow),
                                 llmax(0.f, p.beachEdge), W);
            if (plan.mTarget.defined() == 0)
            {
                plan.mError = llformat("There is no shore in that box: the water here is at %.1f m. Draw the box across the water's edge.", water);
                return plan;
            }
            plan.mLabel = llformat("Beach %d x %d m", b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
            break;
        }
        default:
            plan.mError = "Pick a tool.";
            return plan;
        }

        // the region clamps a flood's height to its limits (FlattenArea.cs:47-52): the target says so too, so the read-back agrees
        Grid& t = plan.mTarget;
        plan.mBefore = Grid(t.x0, t.y0, t.w, t.h);
        LLViewerParcelOverlay* overlay = rgn->getParcelOverlay();
        const bool manager = rgn->canManageEstate();
        for (S32 y = t.y0; y < t.y0 + t.h; ++y)
            for (S32 x = t.x0; x < t.x0 + t.w; ++x)
            {
                F32& v = t.ref(x, y);
                if (!has(v)) continue;
                v = llclamp(v, MIN_TERRAIN_M, MAX_TERRAIN_M);
                const F32 g = ground.at(x, y);
                plan.mBefore.ref(x, y) = g;
                const F32 d = v - g;
                if (fabsf(d) <= WRITE_SKIP_M) continue;
                ++plan.mCells;
                plan.mMaxCut = llmax(plan.mMaxCut, -d);
                plan.mMaxFill = llmax(plan.mMaxFill, d);
                // Who may change a cell is the region's call (PermissionsModule.cs:2028-2067 CanTerraformLand: estate owner or
                // manager, a parcel with Edit Terrain for everyone, its owner, its group with the power). This viewer knows only
                // its own land for sure (the parcel overlay, llviewerparceloverlay.h ownership: PARCEL_SELF).
                if (!manager && (!overlay || overlay->ownership(LLVector3((F32)x + 0.5f, (F32)y + 0.5f, 0.f)) != PARCEL_SELF)) ++plan.mNotYours;
            }
        plan.mRuns = planRuns(t, plan.mBefore, WRITE_BAND_M, WRITE_SKIP_M);
        if (plan.mRuns.empty())
        {
            plan.mError = "Nothing to change: the ground there is already like that.";
            return plan;
        }
        if ((S32)plan.mRuns.size() > MAX_FLOODS)
        {
            plan.mError = llformat("That needs %d floods (the ground is set a strip at a time); one job sends at most %d. "
                                   "Make the area smaller or do it in parts.", (S32)plan.mRuns.size(), MAX_FLOODS);
            return plan;
        }
        // Undo writes the old ground back, which can take far more floods than the change did (a smooth over rough ground):
        // a job is only allowed when its Undo fits in one job too (MAX_FLOODS is a hard limit for every write)
        const size_t undo_floods = planRuns(plan.mBefore, t, WRITE_BAND_M, WRITE_SKIP_M).size();
        if ((S32)undo_floods > MAX_FLOODS)
        {
            plan.mError = llformat("That could not be undone in one job (Undo would need %d floods, the most is %d). Make the area smaller "
                                   "or do it in parts.", (S32)undo_floods, MAX_FLOODS);
            plan.mRuns.clear();
            return plan;
        }
        return plan;
    }

    // ---- the jobs and the history --------------------------------------------------------------------------------------------

    const std::vector<Step>& Jobs::steps(const LLUUID& region) const
    {
        static const std::vector<Step> none;
        auto it = mHistory.find(region);
        return it == mHistory.end() ? none : it->second.mSteps;
    }

    size_t Jobs::done(const LLUUID& region) const
    {
        auto it = mHistory.find(region);
        return it == mHistory.end() ? 0 : it->second.mDone;
    }

    namespace
    {
        /** The floods writing `target` back would take from the ground as this viewer has it now (what Undo / Redo plan). */
        size_t floodsFromNow(const Grid& target)
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            Grid now(target.x0, target.y0, target.w, target.h);
            for (S32 y = target.y0; y < target.y0 + target.h; ++y)
                for (S32 x = target.x0; x < target.x0 + target.w; ++x)
                {
                    F32 z;
                    if (has(target.at(x, y)) && knownHeight(rgn, x, y, z)) now.ref(x, y) = z;
                }
            return planRuns(target, now, WRITE_BAND_M, WRITE_SKIP_M).size();
        }
    }

    size_t Jobs::undoFloods(const LLUUID& region) const
    {
        auto it = mHistory.find(region);
        if (it == mHistory.end() || it->second.mDone == 0) return 0;
        return floodsFromNow(it->second.mSteps[it->second.mDone - 1].mBefore);
    }

    size_t Jobs::redoFloods(const LLUUID& region) const
    {
        auto it = mHistory.find(region);
        if (it == mHistory.end() || it->second.mDone >= it->second.mSteps.size()) return 0;
        return floodsFromNow(it->second.mSteps[it->second.mDone].mAfter);
    }

    std::string Jobs::startBlocker(const LLUUID& region) const
    {
        if (mBusy) return "A Terraform job is still running - wait for it to finish (or press Stop), then try again.";
        // both write the ground with the same floods; two writers at once would drop each other's (TerrainModule.cs:1792, :1946)
        if (WolfCaves::instance().busy() || WolfTerrainWriter::busy())
            return "The Cave Builder is changing the ground right now - wait for it to finish, then try again.";
        // [WOLF GRID GATE] anything that SENDS to the region uses the Confirmed gate (wolfgrid.h isOnWolfTerritoriesConfirmed)
        if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return "Terraform works on Wolf Territories regions only.";
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != region) return "You are on another region now - these steps belong to the region they were made on.";
        return std::string();
    }

    void Jobs::start(const std::string& line)
    {
        mBusy = true;
        mCancel = false;
        mStatus = line;
        mProgress = 0.f;
    }

    void Jobs::finish(bool ok, const std::string& text)
    {
        mBusy = false;
        mCancel = false;
        mLastOk = ok;
        mLastText = text;
        mStatus = text;
        mProgress = ok ? 1.f : -1.f;
        ++mSerial;
        if (!ok) LL_WARNS("WolfTerraform") << text << LL_ENDL;
    }

    // Paul's no-eternal-logs rule (DESIGN.md 3.4 "Undo": capped, gone on restart): at most MAX_STEPS a region, MAX_HISTORY_BYTES
    // in all, the oldest step going first.
    void Jobs::trim(U64 keep_seq)
    {
        for (auto& kv : mHistory)
        {
            History& h = kv.second;
            while (h.mSteps.size() > MAX_STEPS && h.mSteps.front().mSeq != keep_seq)
            {
                h.mSteps.erase(h.mSteps.begin());
                if (h.mDone > 0) --h.mDone;
            }
        }
        for (;;)
        {
            size_t total = 0;
            History* oldest = nullptr;
            U64 oldest_seq = 0;
            for (auto& kv : mHistory)
                for (const Step& s : kv.second.mSteps)
                {
                    total += s.bytes();
                    if (!oldest || s.mSeq < oldest_seq) { oldest = &kv.second; oldest_seq = s.mSeq; }
                }
            // the step a job is about always stays
            if (total <= MAX_HISTORY_BYTES || !oldest || oldest_seq == keep_seq) break;
            oldest->mSteps.erase(oldest->mSteps.begin());
            if (oldest->mDone > 0) --oldest->mDone;
        }
    }

    Step* Jobs::findStep(const LLUUID& region, U64 seq)
    {
        auto it = mHistory.find(region);
        if (it == mHistory.end()) return nullptr;
        for (Step& s : it->second.mSteps) if (s.mSeq == seq) return &s;
        return nullptr;
    }

    bool Jobs::apply(const Plan& plan, std::string& why_not)
    {
        why_not = startBlocker(plan.mRegion);
        if (why_not.empty() && !plan.ok()) why_not = plan.mError.empty() ? std::string("Draw it on the ground first.") : plan.mError;
        if (!why_not.empty()) return false;
        // The step is kept BEFORE anything is written, so a job stopped part way can still be undone.
        History& h = mHistory[plan.mRegion];
        h.mSteps.resize(h.mDone);   // a new step drops what could have been redone
        Step st;
        st.mLabel = plan.mLabel;
        st.mSeq = mNextSeq++;
        // every cell it may write (a run can cover a cell that already stood near its target and move it by up to WRITE_SKIP_M +
        // WRITE_BAND_M / 2); Undo's own plan then leaves out the cells that are still within WRITE_SKIP_M of these
        st.mBefore = plan.mBefore;
        h.mSteps.push_back(st);
        h.mDone = h.mSteps.size();
        ++mHistoryVersion;
        trim(st.mSeq);
        static const char* VERB[TOOL_COUNT] = { "Levelling the ground", "Grading the ramp", "Cutting the terraces", "Smoothing the ground",
                                                "Laying the road", "Carving the river bed", "Shaping the shore" };
        const std::string verb = VERB[plan.mTool];
        start(verb + "...");
        const U64 seq = st.mSeq;
        LLCoros::instance().launch("WolfTerraform apply", [this, plan, verb, seq]() { runCoro(KIND_APPLY, plan.mRegion, plan, verb, seq); });
        return true;
    }

    namespace
    {
        /** The heights now of `cells`' cells (those with a value), from this viewer's ground; false when one is not known. */
        bool heightsNow(LLViewerRegion* rgn, const Grid& cells, Grid& out)
        {
            out = Grid(cells.x0, cells.y0, cells.w, cells.h);
            for (S32 y = cells.y0; y < cells.y0 + cells.h; ++y)
                for (S32 x = cells.x0; x < cells.x0 + cells.w; ++x)
                {
                    if (!has(cells.at(x, y))) continue;
                    F32 z;
                    if (!knownHeight(rgn, x, y, z)) return false;
                    out.ref(x, y) = z;
                }
            return true;
        }
    }

    bool Jobs::undo(const LLUUID& region, std::string& why_not)
    {
        why_not = startBlocker(region);
        auto it = mHistory.find(region);
        if (why_not.empty() && (it == mHistory.end() || it->second.mDone == 0)) why_not = "There is nothing to undo here.";
        if (!why_not.empty()) return false;
        Step& st = it->second.mSteps[it->second.mDone - 1];
        if ((S32)floodsFromNow(st.mBefore) > MAX_FLOODS)
        {
            why_not = llformat("Undo of \"%s\" would need more than %d floods now, the most one job sends: the ground there has changed too much since.",
                               st.mLabel.c_str(), MAX_FLOODS);
            return false;
        }
        // Redo will put back what the step made: the ground as it is now, read before Undo writes over it
        Grid now;
        if (!heightsNow(gAgent.getRegion(), st.mBefore, now))
        {
            why_not = "Part of that ground has not reached this viewer - go nearer to it, then press Undo again.";
            return false;
        }
        // a retry of a stopped Undo keeps the after heights read the first time (the ground now is half put back)
        if (!st.mUndoPartial) st.mAfter = now;
        Plan plan;
        plan.mRegion = region;
        plan.mTarget = st.mBefore;
        plan.mBefore = now;
        plan.mLabel = st.mLabel;
        const U64 seq = st.mSeq;
        trim(seq);   // (st may have moved: only the copies above are used from here)
        start("Undoing \"" + plan.mLabel + "\": putting the ground back...");
        const std::string verb = "Undoing \"" + plan.mLabel + "\": putting the ground back";
        LLCoros::instance().launch("WolfTerraform undo", [this, region, plan, verb, seq]() { runCoro(KIND_UNDO, region, plan, verb, seq); });
        return true;
    }

    bool Jobs::redo(const LLUUID& region, std::string& why_not)
    {
        why_not = startBlocker(region);
        auto it = mHistory.find(region);
        if (why_not.empty() && (it == mHistory.end() || it->second.mDone >= it->second.mSteps.size())) why_not = "There is nothing to redo here.";
        if (!why_not.empty()) return false;
        Step& st = it->second.mSteps[it->second.mDone];
        if (st.mAfter.cells() == 0) { why_not = "That step cannot be redone (its heights were not kept)."; return false; }
        if ((S32)floodsFromNow(st.mAfter) > MAX_FLOODS)
        {
            why_not = llformat("Redo of \"%s\" would need more than %d floods now, the most one job sends.", st.mLabel.c_str(), MAX_FLOODS);
            return false;
        }
        Grid now;
        if (!heightsNow(gAgent.getRegion(), st.mAfter, now))
        {
            why_not = "Part of that ground has not reached this viewer - go nearer to it, then press Redo again.";
            return false;
        }
        // Undo after this Redo puts back the ground as it is now; a retry of a stopped Redo keeps the first reading
        if (!st.mRedoPartial) st.mBefore = now;
        Plan plan;
        plan.mRegion = region;
        plan.mTarget = st.mAfter;
        plan.mBefore = now;
        plan.mLabel = st.mLabel;
        const U64 seq = st.mSeq;
        trim(seq);
        start("Redoing \"" + plan.mLabel + "\"...");
        const std::string verb = "Redoing \"" + plan.mLabel + "\"";
        LLCoros::instance().launch("WolfTerraform redo", [this, region, plan, verb, seq]() { runCoro(KIND_REDO, region, plan, verb, seq); });
        return true;
    }

    void Jobs::runCoro(EKind kind, LLUUID region, Plan plan, std::string verb, U64 seq)
    {
        const Grid& target = plan.mTarget;
        std::vector<Point> pts;
        for (S32 y = target.y0; y < target.y0 + target.h; ++y)
            for (S32 x = target.x0; x < target.x0 + target.w; ++x)
                if (has(target.at(x, y))) pts.push_back(Point{ x, y, target.at(x, y) });
        bool first = true;
        S32 pass = 0;   // which pass (counted by the planner, so an empty first pass still counts)
        auto here = [region]() -> LLViewerRegion*
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            return rgn && rgn->getRegionID() == region && WolfGrid::isOnWolfTerritoriesConfirmed() ? rgn : nullptr;
        };
        WolfTerrainWriter::Hooks hooks;
        hooks.cancelled = [this]() { return mCancel; };
        hooks.sendRegion = here;
        hooks.readRegion = here;
        hooks.label = plan.mLabel;   // [WOLF TERRAFORM 2026-10-23 Phase 3/] the region's undo history names the step
        hooks.sent = [this, &pass, verb](S32 n, S32 total)
        {
            // [WOLF TERRAFORM 2026-10-23 Phase 3] the region's WolfTerraform cap: no floods, no planner passes - one request per
            // 256 m block (wolfterraincap.h)
            if (pass == 0)
            {
                mProgress = (F32)n / (F32)llmax(1, total);
                mStatus = llformat("%s: the region has set %d of %d block%s", verb.c_str(), n, total, total == 1 ? "" : "s");
                return;
            }
            mProgress = (pass == 1 ? 0.f : 0.9f) + (pass == 1 ? 0.9f : 0.1f) * (F32)n / (F32)llmax(1, total);
            if (n == total) mStatus = verb + ": checking the ground took it...";
            else if (pass == 1) mStatus = llformat("%s: %d of %d floods", verb.c_str(), n, total);
            else mStatus = llformat("%s: going over %d place%s again (%d of %d)", verb.c_str(), total, total == 1 ? "" : "s", n, total);
        };
        const Grid before = plan.mBefore;
        const WolfTerrainWriter::Result res = WolfTerrainWriter::write(pts, [&first, &pass, &before, &target](const std::vector<Point>& list)
        {
            ++pass;
            Grid g(target.x0, target.y0, target.w, target.h);
            for (const Point& p : list) g.ref(p.x, p.y) = p.z;
            // the first pass leaves out what already stands right; the second pass sends every point that did not take
            std::vector<Run> runs = planRuns(g, first ? before : Grid(), WRITE_BAND_M, WRITE_SKIP_M);
            first = false;
            return runs;
        }, hooks);

        auto markStopped = [this, region, seq]()
        {
            if (Step* s = findStep(region, seq)) s->mStopped = true;
            ++mHistoryVersion;
        };
        // an Undo / Redo that did not finish: pressing it again goes on from its first readings
        auto markPartial = [this, kind, region, seq](bool partial)
        {
            if (Step* s = findStep(region, seq)) (kind == KIND_UNDO ? s->mUndoPartial : s->mRedoPartial) = partial;
            ++mHistoryVersion;
        };
        const std::string label = "\"" + plan.mLabel + "\"";
        if (res.stoppedFirst)
        {
            const bool asked = mCancel;
            const std::string why = asked ? "Stopped as you asked" : "Stopped: you left the region (or it stopped answering)";
            if (kind == KIND_APPLY)
            {
                markStopped();
                finish(false, why + " part way through " + label + ". Undo puts back what it changed.");
            }
            else if (kind == KIND_UNDO) { markPartial(true); finish(false, why + " part way through undoing " + label + ". Press Undo again to finish putting it back."); }
            else { markPartial(true); finish(false, why + " part way through redoing " + label + ". Press Redo again to finish it."); }
            return;
        }
        // Stop pressed during the second pass (going over the places that did not take): that is a stop, not a refusal
        if (mCancel && !res.left.empty())
        {
            if (kind == KIND_APPLY)
            {
                markStopped();
                finish(false, "Stopped as you asked while going over the places that did not take in " + label + ". Undo puts back what it changed.");
            }
            else
            {
                markPartial(true);
                finish(false, std::string("Stopped as you asked part way through ") + (kind == KIND_UNDO ? "undoing " : "redoing ") + label
                              + (kind == KIND_UNDO ? ". Press Undo again to finish putting it back." : ". Press Redo again to finish it."));
            }
            return;
        }
        // the history moves on once the job went through (a stopped Undo / Redo can simply be pressed again)
        auto it = mHistory.find(region);
        if (it != mHistory.end())
        {
            if (kind == KIND_UNDO && it->second.mDone > 0) --it->second.mDone;
            if (kind == KIND_REDO && it->second.mDone < it->second.mSteps.size()) ++it->second.mDone;
        }
        if (kind != KIND_APPLY) markPartial(false);
        if (!res.left.empty() && !here())
        {
            // the read-back could not be made (the writer then counts every point as not taken): say that, not "refused"
            finish(false, "All of " + label + " was sent, but you left the region before the ground could be checked. Come back and look: "
                          "Undo still puts back what it changed.");
            if (kind == KIND_APPLY) markStopped();
            return;
        }
        if (!res.left.empty() && res.viaCap && !res.reason.empty())
        {
            // [WOLF TERRAFORM 2026-10-23 Phase 3] through the WolfTerraform cap the region says exactly what it did not set and why
            // (the parcels and their owners, or the estate's limits): its words, as they are
            const std::string what = kind == KIND_APPLY ? label + " is done" : kind == KIND_UNDO ? "Undo of " + label + " is done" : "Redo of " + label + " is done";
            finish(false, llformat("%s, but %d m² did not change. The region says: %s", what.c_str(), (S32)res.left.size(), res.reason.c_str()));
            if (kind == KIND_APPLY) markStopped();
            return;
        }
        if (!res.left.empty())
        {
            // [2026-10-23 Paul's Terrace] Every cell that still looks off is said, never dropped - and its cause only when that
            // cause was possible: the land's rights (PermissionsModule.cs:2028-2067: never for an estate owner / manager or
            // on the agent's own parcel), the estate's raise / lower limits (TerrainModule.cs:1421-1489, read from the region:
            // never when they span the whole range), else the region skipped the flood while busy, or this viewer's rounded
            // view (LayerData, wolflayerdata.h) cannot confirm it. Never blame rights when the agent holds the region.
            S32 rights = 0, limits = 0, none = 0;
            for (U8 w : res.why)
            {
                if (w & WolfTerrainWriter::MAY_RIGHTS) ++rights;
                if (w & WolfTerrainWriter::MAY_LIMITS) ++limits;
                if (!w) ++none;
            }
            const Point& ex = res.left.front();
            LLViewerRegion* rgn = here();
            F32 got = 0.f;
            const bool known = rgn && knownHeight(rgn, ex.x, ex.y, got);
            const std::string what = kind == KIND_APPLY ? label + " is done" : kind == KIND_UNDO ? "Undo of " + label + " is done" : "Redo of " + label + " is done";
            std::string text = llformat("%s, but %d m² still look%s off (for example at %d, %d: wanted %.2f m, this viewer sees %s m). ",
                                        what.c_str(), (S32)res.left.size(), res.left.size() == 1 ? "s" : "", ex.x, ex.y, ex.z,
                                        known ? llformat("%.2f", got).c_str() : "?");
            if (rights > 0)
                text += llformat("%d m² of it is on land this viewer does not know to be yours: its owner decides whether you may change "
                                 "the ground there (About Land > Options > Edit Terrain). ", rights);
            if (limits > 0)
            {
                if (res.limitsKnown)
                    text += llformat("The estate only allows raising the ground %.1f m above and lowering it %.1f m below its baked shape "
                                     "(World > Region/Estate > Terrain), which may have held %d m² of it. ", res.raiseLimit, -res.lowerLimit, limits);
                else
                    text += llformat("The estate's terrain raise / lower limits may have held %d m² of it (this viewer could not read them). ", limits);
            }
            if (none > 0)
                text += llformat("%d m² could not have been refused here: the region may have skipped a flood while it was busy, or this "
                                 "viewer's view of the ground is too rounded to confirm it (terrain arrives compressed). Look at it in the world: "
                                 "Undo still puts back all of it. ", none);
            if (rights == 0 && limits == 0)
            {
                // nothing the user's rights or the estate held: a note, not a failure
                finish(true, text);
                return;
            }
            finish(false, text);
            if (kind == KIND_APPLY) markStopped();
            return;
        }
        if (kind == KIND_APPLY) finish(true, "Done: " + plan.mLabel + ".");
        else if (kind == KIND_UNDO) finish(true, "Undone: " + plan.mLabel + ".");
        else finish(true, "Redone: " + plan.mLabel + ".");
    }
}
