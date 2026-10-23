/**
 * @file wolfterrainwriter.cpp
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

// [WOLF TERRAFORM 2026-10-23] See wolfterrainwriter.h. Moved out of wolfcave.cpp WolfCaves::shape (git 5c44e59bbf, lines
// 1035-1127): the floods, their order, their fields and the read-back are the same; the Cave Builder keeps its own merge
// (WolfTerraformMath::caveRuns) and its status line (the `sent` hook).

#include "llviewerprecompiledheaders.h"

#include "wolfterrainwriter.h"
#include "wolfterraincap.h"

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#include "llagent.h"
#include "lleventcoro.h"
#include "llparcel.h"
#include "llregioninfomodel.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerparceloverlay.h"
#include "message.h"
#include "wolflayerdata.h"

using namespace WolfTerraformMath;

S32 WolfTerrainWriter::sWriting = 0;

namespace
{
    /** Counts a write in and out (busy()), whatever way the write ends. */
    struct Writing
    {
        S32& mCount;
        explicit Writing(S32& count) : mCount(count) { ++mCount; }
        ~Writing() { --mCount; }
    };
}

F32 WolfTerrainWriter::measuredSecsPerFlood()
{
    static LLCachedControl<F32> secs(gSavedSettings, "WolfTerraformSecsPerFlood", 0.f);
    return llmax(0.f, (F32)secs);
}

// Source: wolfcave.cpp (before 2026-10-23) WolfCaves::shape `send` lambda, lines 1066-1100.
bool WolfTerrainWriter::send(const std::vector<Run>& runs, const Hooks& hooks)
{
    Writing writing(sWriting);
    LLTimer took;
    S32 n = 0;
    for (const Run& r : runs)
    {
        // Cancel stops between floods: every flood already sent is whole, and the caller records where the job stood.
        if (hooks.cancelled && hooks.cancelled()) return false;
        LLViewerRegion* rgn = hooks.sendRegion ? hooks.sendRegion() : nullptr;
        if (!rgn) return false;
        // Source: lltoolbrush.cpp:309-344 modifyLandInSelectionGlobal — the same message.
        const Flood f = floodOf(r);
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ModifyLand);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ModifyBlock);
        msg->addU8Fast(_PREHASH_Action, ACTION_FLATTEN);
        msg->addU8Fast(_PREHASH_BrushSize, (U8)0);
        msg->addF32Fast(_PREHASH_Seconds, FLATTEN_SECONDS);
        msg->addF32Fast(_PREHASH_Height, f.height);
        msg->nextBlockFast(_PREHASH_ParcelData);
        msg->addS32Fast(_PREHASH_LocalID, -1);
        msg->addF32Fast(_PREHASH_West, f.west);
        msg->addF32Fast(_PREHASH_South, f.south);
        msg->addF32Fast(_PREHASH_East, f.east);
        msg->addF32Fast(_PREHASH_North, f.north);
        msg->nextBlock("ModifyBlockExtended");
        msg->addF32("BrushSize", 1.f);
        // Reliable (lltoolbrush.cpp sends a brush dab unreliably; a dropped flood here would be a hole the read-back then has
        // to find).
        msg->sendReliable(rgn->getHost());
        ++n;
        if (hooks.sent) hooks.sent(n, (S32)runs.size());
        llcoro::suspendUntilTimeout(MODIFY_GAP_SECS);
    }
    // the writer's real speed on this viewer (DESIGN.md 3.1 "Writer speed": measured, never assumed)
    if (n >= MEASURE_MIN_RUNS) gSavedSettings.setF32("WolfTerraformSecsPerFlood", took.getElapsedTimeF32() / (F32)n);
    return true;
}

namespace
{
    typedef std::pair<S32, S32> PatchKey;
    /** A patch's 256 decoded heights (row by row, as wolflayerdata.h), false when its data has not arrived. */
    bool patchHeights(LLViewerRegion* rgn, const PatchKey& pk, float* out)
    {
        const LLSurface& land = rgn->getLand();
        const S32 w = (S32)rgn->getWidth(), gpp = land.getGridsPerPatchEdge(), ppe = land.getPatchesPerEdge();
        // one sample a metre and 16 to a patch, the sizes wolflayerdata.h models (OpenSim Constants.TerrainPatchSize = 16)
        if (land.getMetersPerGrid() != 1.f || gpp != WolfLayerData::PATCH || ppe <= 0) return false;
        if (pk.first < 0 || pk.second < 0 || pk.first >= ppe || pk.second >= ppe) return false;
        if ((pk.first + 1) * WolfLayerData::PATCH > w || (pk.second + 1) * WolfLayerData::PATCH > w) return false;
        const LLSurfacePatch* patch = land.findPatch(pk.first, pk.second);
        if (!patch || !patch->getHasReceivedData()) return false;
        for (S32 j = 0; j < WolfLayerData::PATCH; ++j)
            for (S32 i = 0; i < WolfLayerData::PATCH; ++i)
                out[j * WolfLayerData::PATCH + i] = land.getZ(pk.first * WolfLayerData::PATCH + i, pk.second * WolfLayerData::PATCH + j);
        return true;
    }
    PatchKey patchOf(S32 x, S32 y) { return PatchKey(x / WolfLayerData::PATCH, y / WolfLayerData::PATCH); }
    U64 cellKey(S32 x, S32 y) { return ((U64)(U32)x << 32) | (U32)y; }
}

// Source: wolfcave.cpp (before 2026-10-23) WolfCaves::shape, lines 1101-1127: the first pass, the read-back after READBACK_SECS,
// one more pass for the points that did not take, the read-back again. [2026-10-23 Paul's Terrace] "Did not take" is judged by
// the LayerData model (wolflayerdata.h judge) instead of HEIGHT_TOLERANCE alone, which called a terraced patch's ordinary
// coding error (0.18 m at 784, 409) a refusal.
WolfTerrainWriter::Result WolfTerrainWriter::writeFloods(const std::vector<Point>& pts, const Planner& plan, const Hooks& hooks)
{
    Writing writing(sWriting);
    Result out;
    // The estate's raise / lower limits, for the report: anyone may ask (llfloaterregioninfo.cpp:405-414 "Must allow anyone to
    // request the RegionInfo data"); the answer lands in LLRegionInfoModel (llviewerregion.cpp:1191-1199, llregioninfomodel.cpp
    // :142-152), long before the read-back READBACK_SECS later. Only to the agent's own region (sendReliableMessage).
    LLViewerRegion* first_rgn = hooks.readRegion ? hooks.readRegion() : nullptr;
    if (first_rgn && first_rgn == gAgent.getRegion())
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessage("RequestRegionInfo");
        msg->nextBlock("AgentData");
        msg->addUUID("AgentID", gAgent.getID());
        msg->addUUID("SessionID", gAgent.getSessionID());
        gAgent.sendReliableMessage();
    }
    // What this viewer decoded for every patch the write touches, before it (the "kept" explanation of judge()).
    std::map<PatchKey, std::vector<float>> before;
    if (first_rgn)
    {
        for (const Point& p : pts)
        {
            const PatchKey pk = patchOf(p.x, p.y);
            if (before.count(pk)) continue;
            std::vector<float> h(256);
            if (patchHeights(first_rgn, pk, h.data())) before[pk] = h;
            else before[pk] = std::vector<float>();
        }
    }
    // The height each cell was last sent.
    std::unordered_map<U64, F32> sent;
    auto sendPlan = [&](const std::vector<Point>& list) -> bool
    {
        const std::vector<Run> runs = plan(list);
        for (const Run& r : runs)
            for (S32 x = r.x0; x <= r.x1; ++x)
                for (S32 y = r.y0; y <= r.y1; ++y) sent[cellKey(x, y)] = floodOf(r).height;
        return send(runs, hooks);
    };
    auto misses = [&]() -> std::vector<Point>
    {
        llcoro::suspendUntilTimeout(READBACK_SECS);   // LayerData for the changed patches
        std::vector<Point> left;
        LLViewerRegion* rgn = hooks.readRegion ? hooks.readRegion() : nullptr;
        if (!rgn) return pts;
        // the points not within HEIGHT_TOLERANCE, by patch
        std::map<PatchKey, std::vector<size_t>> doubtful;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const Point& p = pts[i];
            if (fabsf(rgn->getLand().resolveHeightRegion((F32)p.x, (F32)p.y) - p.z) > HEIGHT_TOLERANCE) doubtful[patchOf(p.x, p.y)].push_back(i);
        }
        S32 judged = 0;
        for (const auto& kv : doubtful)
        {
            // the model costs about 32 patch round trips: give the frame back now and then on a big job
            if (++judged % 8 == 0)
            {
                llcoro::suspend();
                rgn = hooks.readRegion ? hooks.readRegion() : nullptr;
                if (!rgn) return pts;
            }
            float decoded[256], was[256], sent_h[256];
            const auto b = before.find(kv.first);
            const bool can_judge = b != before.end() && b->second.size() == 256 && patchHeights(rgn, kv.first, decoded);
            if (!can_judge)
            {
                for (size_t i : kv.second) left.push_back(pts[i]);
                continue;
            }
            std::memcpy(was, b->second.data(), sizeof was);
            const S32 px0 = kv.first.first * WolfLayerData::PATCH, py0 = kv.first.second * WolfLayerData::PATCH;
            for (S32 j = 0; j < WolfLayerData::PATCH; ++j)
                for (S32 i = 0; i < WolfLayerData::PATCH; ++i)
                {
                    const auto it = sent.find(cellKey(px0 + i, py0 + j));
                    sent_h[j * WolfLayerData::PATCH + i] = it != sent.end() ? it->second : WolfTerraformMath::nan();
                }
            // a point the planner left out (it already stood within its skip) was not written: judge() counts it as taken
            bool took[256];
            WolfLayerData::judge(decoded, sent_h, was, HEIGHT_TOLERANCE, took);
            for (size_t i : kv.second)
                if (!took[(pts[i].y - py0) * WolfLayerData::PATCH + (pts[i].x - px0)]) left.push_back(pts[i]);
        }
        return left;
    };
    if (!sendPlan(pts))
    {
        out.stoppedFirst = true;
        out.left = pts;
        out.why.assign(pts.size(), (U8)(MAY_RIGHTS | MAY_LIMITS));
        return out;
    }
    out.left = misses();
    if (!out.left.empty() && !(hooks.cancelled && hooks.cancelled()) && sendPlan(out.left)) out.left = misses();
    // why each point left may not have taken
    LLViewerRegion* rgn = hooks.readRegion ? hooks.readRegion() : nullptr;
    const LLRegionInfoModel& info = LLRegionInfoModel::instance();
    out.limitsKnown = rgn && !info.mSimName.empty() && info.mSimName == rgn->getName();
    out.raiseLimit = info.mTerrainRaiseLimit;
    out.lowerLimit = info.mTerrainLowerLimit;
    // Source: RegionSettings.cs:355-356 FullTerrainRaiseLimit = MaxTerrainHeightmap - MinTerrainHeightmap = 4000 - -100 = 4100:
    // limits that wide can never hold a height the region keeps anyway (Constants.cs:165-166)
    const bool limits_bind = !out.limitsKnown || out.raiseLimit < 4100.f || out.lowerLimit > -4100.f;
    const bool manager = rgn && rgn->canManageEstate();
    LLViewerParcelOverlay* overlay = rgn ? rgn->getParcelOverlay() : nullptr;
    for (const Point& p : out.left)
    {
        U8 why = limits_bind ? (U8)MAY_LIMITS : (U8)0;
        if (!manager && (!overlay || overlay->ownership(LLVector3((F32)p.x + 0.5f, (F32)p.y + 0.5f, 0.f)) != PARCEL_SELF)) why |= MAY_RIGHTS;
        out.why.push_back(why);
    }
    return out;
}

// [WOLF TERRAFORM 2026-10-23 Phase 3] DESIGN.md 3.1 B: "Route 1, the WolfTerraform cap (when the region offers it)" - the region
// sets the heights in one request per 256 m block and says which cells it did not set and why (wolfterraincap.h). "Route 2,
// the fallback (any OpenSim grid, including Wolf regions before the module)": writeFloods, unchanged - for the whole job when
// the region has no cap, or for the points the cap could not send when it stopped answering part way.
WolfTerrainWriter::Result WolfTerrainWriter::write(const std::vector<Point>& pts, const Planner& plan, const Hooks& hooks)
{
    Writing writing(sWriting);
    Result viaCap;
    std::vector<Point> rest;
    if (!WolfTerrainCap::write(pts, hooks, viaCap, rest)) return writeFloods(pts, plan, hooks);
    viaCap.viaCap = true;
    if (viaCap.stoppedFirst || rest.empty()) return viaCap;
    LL_WARNS("WolfTerraform") << "the region's WolfTerraform stopped answering; " << rest.size() << " point(s) go by floods" << LL_ENDL;
    Result floods = writeFloods(rest, plan, hooks);
    if (floods.stoppedFirst)
    {
        floods.left.insert(floods.left.end(), viaCap.left.begin(), viaCap.left.end());
        floods.why.insert(floods.why.end(), viaCap.why.begin(), viaCap.why.end());
        floods.reason = viaCap.reason;
        return floods;
    }
    viaCap.left.insert(viaCap.left.end(), floods.left.begin(), floods.left.end());
    viaCap.why.insert(viaCap.why.end(), floods.why.begin(), floods.why.end());
    viaCap.limitsKnown = floods.limitsKnown;
    viaCap.raiseLimit = floods.raiseLimit;
    viaCap.lowerLimit = floods.lowerLimit;
    return viaCap;
}
