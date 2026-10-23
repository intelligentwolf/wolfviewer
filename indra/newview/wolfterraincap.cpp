/**
 * @file wolfterraincap.cpp
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

// [WOLF TERRAFORM 2026-10-23] See wolfterraincap.h. The POST is the tunnel tool's (wolffloatertunnels.cpp capPost:
// LLCoreHttpUtil::HttpCoroutineAdapter::postAndSuspend, LLSD XML both ways).

#include "llviewerprecompiledheaders.h"

#include "wolfterraincap.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "llcorehttputil.h"
#include "lleventcoro.h"
#include "llsurface.h"
#include "lltimer.h"
#include "lluuid.h"
#include "llviewerregion.h"

#include "wolfterraincapmath.h"

using namespace WolfTerraformMath;
using namespace WolfTerrainCapMath;

std::string WolfTerrainCap::url(LLViewerRegion* rgn)
{
    return rgn ? rgn->getCapability(CAP) : std::string();
}

namespace
{
    enum class Sent { SET, REFUSED, NO_ANSWER };

    /** One batch to the region, waiting out busy answers (WolfTerraformModule: "busy" + "retry_after"). */
    Sent post(const std::string& url, const LLSD& body, LLSD& answer, std::string& words, const WolfTerrainWriter::Hooks& hooks)
    {
        for (int t = 0; t < WolfTerrainCap::BUSY_TRIES; ++t)
        {
            auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTerraform", LLCore::HttpRequest::DEFAULT_POLICY_ID);
            LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
            answer = adapter->postAndSuspend(request, url, body);
            LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(answer[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
            if (!status)
            {
                LL_WARNS("WolfTerraform") << "the region's WolfTerraform did not answer: " << status.toString() << LL_ENDL;
                return Sent::NO_ANSWER;
            }
            words = answer["message"].asString();
            if (answer["success"].asBoolean()) return Sent::SET;
            if (!answer["busy"].asBoolean()) return Sent::REFUSED;
            if (hooks.cancelled && hooks.cancelled()) return Sent::REFUSED;
            const F32 wait = llclamp((F32)answer["retry_after"].asInteger(), 0.5f, WolfTerrainCap::BUSY_MAX_WAIT_SECS);
            llcoro::suspendUntilTimeout(wait);
        }
        return Sent::REFUSED;   // still busy after BUSY_TRIES waits: `words` says why
    }
}

bool WolfTerrainCap::write(const std::vector<Point>& pts, const WolfTerrainWriter::Hooks& hooks,
                           WolfTerrainWriter::Result& out, std::vector<Point>& rest)
{
    rest.clear();
    LLViewerRegion* first = hooks.sendRegion ? hooks.sendRegion() : nullptr;
    if (url(first).empty()) return false;

    std::vector<Batch> batches = makeBatches(pts);
    // the tiles of one job share a group, so the region keeps them as one undo step (WolfTerrainHistory.Open)
    const std::string group = LLUUID::generateNewID().asString();
    const std::string label = hooks.label.empty() ? std::string("WolfViewer terrain") : hooks.label;
    std::vector<std::string> words;
    std::vector<bool> settled(batches.size(), false);   // set by the region (so worth waiting for on the viewer's ground)
    for (size_t i = 0; i < batches.size(); ++i)
    {
        LLViewerRegion* rgn = hooks.sendRegion ? hooks.sendRegion() : nullptr;
        if ((hooks.cancelled && hooks.cancelled()) || !rgn)
        {
            // the same as a flood pass stopped part way (WolfTerrainWriter::write): every point counts as not done
            out.stoppedFirst = true;
            out.left = pts;
            out.why.assign(pts.size(), (U8)(WolfTerrainWriter::MAY_RIGHTS | WolfTerrainWriter::MAY_LIMITS));
            return true;
        }
        const std::string u = url(rgn);
        Batch& b = batches[i];
        LLSD body;
        body["op"] = "apply";
        body["x"] = b.x0;
        body["y"] = b.y0;
        body["w"] = b.w;
        body["h"] = b.h;
        body["heights"] = LLSD::Binary(encode(b.heights));
        body["group"] = group;
        body["label"] = label;
        LLSD answer;
        std::string said;
        const Sent sent = u.empty() ? Sent::NO_ANSWER : post(u, body, answer, said, hooks);
        if (sent == Sent::NO_ANSWER)
        {
            // the cap went (a restart) or stopped answering: the rest goes by floods, nothing is lost
            for (size_t k = i; k < batches.size(); ++k) rest.insert(rest.end(), batches[k].pts.begin(), batches[k].pts.end());
            break;
        }
        if (sent == Sent::SET)
        {
            std::vector<U8> codes;
            const std::vector<Point> off = notSet(b, answer["cells"].asBinary(), codes);
            out.left.insert(out.left.end(), off.begin(), off.end());
            for (U8 c : codes) out.why.push_back(c == CELL_LIMIT ? (U8)WolfTerrainWriter::MAY_LIMITS : (U8)WolfTerrainWriter::MAY_RIGHTS);
            settled[i] = off.size() < b.pts.size();
        }
        else
        {
            out.left.insert(out.left.end(), b.pts.begin(), b.pts.end());
            out.why.insert(out.why.end(), b.pts.size(), (U8)(WolfTerrainWriter::MAY_RIGHTS | WolfTerrainWriter::MAY_LIMITS));
            if (said.empty()) said = "the region refused the change";
        }
        if (!said.empty() && std::find(words.begin(), words.end(), said) == words.end()) words.push_back(said);
        if (hooks.sent) hooks.sent((S32)(i + 1), (S32)batches.size());
    }

    // The region has the heights now; its LayerData for the changed patches follows within a moment. Wait for the viewer's own
    // ground to show them (at most SETTLE_MAX_SECS), so the next step plans from the new ground. Nothing is sent again: the
    // region's answer is the truth for what it set.
    std::unordered_set<long long> notTaken;
    for (const Point& q : out.left) notTaken.insert(((long long)q.x << 32) | (unsigned)q.y);
    LLTimer waited;
    while (waited.getElapsedTimeF32() < SETTLE_MAX_SECS && !(hooks.cancelled && hooks.cancelled()))
    {
        LLViewerRegion* rgn = hooks.readRegion ? hooks.readRegion() : nullptr;
        if (!rgn) break;
        bool all = true;
        for (size_t i = 0; i < batches.size() && all; ++i)
        {
            if (!settled[i]) continue;
            for (const Point& p : batches[i].pts)
            {
                if (fabsf(rgn->getLand().resolveHeightRegion((F32)p.x, (F32)p.y) - p.z) > WolfTerrainWriter::HEIGHT_TOLERANCE
                    && !notTaken.count(((long long)p.x << 32) | (unsigned)p.y))
                {
                    all = false;
                    break;
                }
            }
        }
        if (all) break;
        llcoro::suspendUntilTimeout(SETTLE_STEP_SECS);
    }

    for (size_t i = 0; i < words.size() && i < 3; ++i) out.reason += (i ? " " : "") + words[i];
    if (words.size() > 3) out.reason += llformat(" (and %d more)", (S32)words.size() - 3);
    return true;
}
