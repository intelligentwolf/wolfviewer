/**
 * @file wolfobjectprops.cpp
 * @brief WolfViewer: the ONE throttled source of object NAMES and DESCRIPTIONS.
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

#include "llviewerprecompiledheaders.h"

#include "wolfobjectprops.h"

#include <algorithm>
#include <vector>

#include "llagent.h"
#include "llframetimer.h"
#include "llselectmgr.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"

// Source: wolfstorm/js/world/object_props_harvester.js ObjectPropsHarvester.

WolfObjectProps::WolfObjectProps()
{
}

WolfObjectProps::~WolfObjectProps()
{
}

// Source: fswolfwater.cpp (2026-09-03) noteDescription() — stored even when empty.
void WolfObjectProps::note(const LLUUID& object_id, const std::string& name, const std::string& description)
{
    if (object_id.isNull())
    {
        return;
    }
    Props& p = mProps[object_id];
    p.mName = name;
    p.mDescription = description;
    p.mDescriptionLower = description;
    LLStringUtil::toLower(p.mDescriptionLower);
    p.mReceivedAt = LLFrameTimer::getElapsedSeconds();
}

const WolfObjectProps::Props* WolfObjectProps::get(const LLUUID& object_id) const
{
    auto it = mProps.find(object_id);
    return (it == mProps.end()) ? nullptr : &it->second;
}

// Source: object_props_harvester.js want()
void WolfObjectProps::want(LLViewerObject* objectp)
{
    if (!objectp || objectp->isDead())
    {
        return;
    }
    const LLUUID& id = objectp->getID();
    if (id.isNull())
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    auto rec = mAsked.find(id);

    Want w;
    if (mProps.count(id))
    {
        // Answered. Only worth asking again once the answer is old enough to be suspect,
        // and only ever with budget nothing new wanted.
        if (rec != mAsked.end() && (now - rec->second.mSentAt) < REFRESH_SECS)
        {
            return;
        }
        w.mPriority = PRIORITY_REFRESH;
    }
    else
    {
        // Never answered. Give up after MAX_TRIES unanswered attempts — the request goes
        // out reliable, so silence means the object is gone, not that the packet was lost.
        if (rec != mAsked.end() && rec->second.mTries >= MAX_TRIES)
        {
            return;
        }
        w.mPriority = objectp->isRoot() ? PRIORITY_ROOT : PRIORITY_CHILD;
    }
    // Nearest first (3D, like the harvester's own ordering).
    const LLVector3 delta = objectp->getPositionAgent() - LLViewerCamera::getInstance()->getOrigin();
    w.mDistSq = delta.lengthSquared();
    mWants[id] = w;
}

// Source: object_props_harvester.js reset() — clears _asked and _wants only ("UUIDs are
// global rather than region-scoped, so an object that comes back will simply be
// re-wanted"); answers live on the objects there, so the viewer prunes its own store of
// the ones whose objects have gone.
void WolfObjectProps::reset()
{
    mAsked.clear();
    mWants.clear();
    mNextDrain = 0.0;
    for (auto it = mProps.begin(); it != mProps.end();)
    {
        LLViewerObject* objectp = gObjectList.findObject(it->first);
        if (!objectp || objectp->isDead())
        {
            it = mProps.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void WolfObjectProps::idle()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        return;
    }
    if (agent_region->getHandle() != mRegionHandle)
    {
        reset();
        mRegionHandle = agent_region->getHandle();
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextDrain)
    {
        return;
    }
    mNextDrain = now + SWEEP_INTERVAL_SECS;
    drain();

    // One line every STATS_INTERVAL_SECS, so "the name never arrived" can be told apart
    // from "it was never asked for" without rebuilding anything.
    if (now >= mNextStatsLog)
    {
        mNextStatsLog = now + STATS_INTERVAL_SECS;
        LL_INFOS("WolfObjectProps") << "props: " << mProps.size() << " objects answered, "
                                    << mAsked.size() << " asked, " << mLastSent
                                    << " request(s) sent last drain, " << mSentTotal
                                    << " sent this region (" << mRefreshedTotal
                                    << " refresh) over " << mDrains << " drains" << LL_ENDL;
    }
}

// Source: object_props_harvester.js update()
void WolfObjectProps::drain()
{
    mLastSent = 0;
    if (mWants.empty())
    {
        return;
    }
    std::map<LLUUID, Want> wants;
    wants.swap(mWants);

    const F64 now = LLFrameTimer::getElapsedSeconds();

    struct Queued
    {
        LLUUID mId;
        S32    mPriority;
        F32    mDistSq;
    };
    std::vector<Queued> queue;
    queue.reserve(wants.size());
    for (const auto& kv : wants)
    {
        const bool refresh = kv.second.mPriority == PRIORITY_REFRESH;
        // A request already in flight: leave it alone until the reply window closes.
        // Refreshes have no in-flight state to protect (the object is already answered),
        // and want() has already applied the REFRESH_SECS gate to them.
        if (!refresh)
        {
            auto rec = mAsked.find(kv.first);
            if (rec != mAsked.end() && (now - rec->second.mSentAt) < RETRY_SECS)
            {
                continue;
            }
        }
        queue.push_back({ kv.first, kv.second.mPriority, kv.second.mDistSq });
    }
    // Roots before children before refreshes, nearest first inside each class.
    std::sort(queue.begin(), queue.end(),
              [](const Queued& a, const Queued& b)
              {
                  if (a.mPriority != b.mPriority)
                  {
                      return a.mPriority < b.mPriority;
                  }
                  return a.mDistSq < b.mDistSq;
              });

    const S32 budget = llmin((S32)queue.size(), REQUEST_BUDGET);
    for (S32 i = 0; i < budget; ++i)
    {
        const Queued& q = queue[i];
        LLViewerObject* objectp = gObjectList.findObject(q.mId);
        if (!objectp || objectp->isDead())
        {
            continue;
        }
        const bool refresh = q.mPriority == PRIORITY_REFRESH;
        Ask& ask = mAsked[q.mId];
        ask.mSentAt = now;
        // A refresh restarts the attempt count: the object demonstrably exists and
        // demonstrably replies, so the give-up counter from its first fetch is spent
        // history rather than evidence.
        ask.mTries = refresh ? 1 : (ask.mTries + 1);

        // Medium 5 RequestObjectPropertiesFamily, one ObjectID per packet.
        LLSelectMgr::getInstance()->requestObjectPropertiesFamily(objectp);
        ++mLastSent;
        ++mSentTotal;
        if (refresh)
        {
            ++mRefreshedTotal;
        }
    }
    ++mDrains;
}
