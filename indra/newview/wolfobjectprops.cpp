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
#include <cmath>   // [FAR DRAW PERF 2026-11-14/] ceil (pruneSlice)
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
void WolfObjectProps::want(LLViewerObject* objectp, bool keep_fresh)
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

    Want w;
    if (mProps.count(id))
    {
        // Answered. Asked again only for a caller that is acting on it (keep_fresh, see the
        // header), once the answer is old enough to be suspect, and only ever with budget
        // nothing new wanted.
        if (!keep_fresh)
        {
            return;   // [W107 PIPELINE 2026-11-14/] before the ask record is looked up: nearly every call ends here
        }
        auto rec = mAsked.find(id);
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
        auto rec = mAsked.find(id);
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
    pruneDeadObjects();
}

void WolfObjectProps::pruneDeadObjects()
{
    // Keep live answers and exhausted retry histories. Clearing either would trigger
    // fresh network requests for objects we already know or have stopped asking.
    const auto prune = [](auto& entries)
    {
        for (auto it = entries.begin(); it != entries.end();)
        {
            LLViewerObject* objectp = gObjectList.findObject(it->first);
            if (!objectp || objectp->isDead())
            {
                it = entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };
    prune(mProps);
    prune(mAsked);
    prune(mWants);
    mPruneNextProps = 0;
    mPruneNextAsked = 0;
}

// [FAR DRAW PERF 2026-11-14] See the header. The same test as pruneDeadObjects, a share of each map a frame.
// [W107 PIPELINE 2026-11-14] The maps are hash maps now, so the place the last frame stopped is a bucket index and a frame's
// share is counted in buckets: bucket_count x this frame's time / SWEEP_INTERVAL_SECS of them (at least one), so the whole
// table - every entry - is still looked at about once every SWEEP_INTERVAL_SECS. A rehash between two frames moves the
// entries to other buckets: that round may then look at an entry twice or reach it one round late, which is the same
// "up to SWEEP_INTERVAL_SECS later" the header already describes.
void WolfObjectProps::pruneSlice()
{
    const F32 dt = LLFrameTimer::getFrameDeltaTimeF32();
    std::vector<LLUUID> dead;
    const auto slice = [dt, &dead](auto& entries, size_t& next)
    {
        if (entries.empty())
        {
            next = 0;
            return;
        }
        // Source: std::unordered_map - erase() never rehashes, so bucket_count() holds for the whole loop.
        const size_t buckets = entries.bucket_count();
        const F64 share = (F64)buckets * (F64)llmax(dt, 0.f) / SWEEP_INTERVAL_SECS;
        size_t count = (size_t)llclamp(ceil(share), 1.0, (F64)buckets);
        if (next >= buckets)
        {
            next = 0;   // the table was rebuilt smaller
        }
        while (count-- > 0)
        {
            // the dead keys of this bucket first, erased after its walk: erasing an element ends the iterators to it,
            // and the bucket's own iterator would be standing on it
            dead.clear();
            for (auto it = entries.begin(next); it != entries.end(next); ++it)
            {
                LLViewerObject* objectp = gObjectList.findObject(it->first);
                if (!objectp || objectp->isDead())
                {
                    dead.push_back(it->first);
                }
            }
            for (const LLUUID& id : dead)
            {
                entries.erase(id);
            }
            if (++next >= buckets)
            {
                next = 0;   // round again
            }
        }
    };
    slice(mProps, mPruneNextProps);
    slice(mAsked, mPruneNextAsked);
}

// [FAR DRAW PERF 2026-11-14] The interest list only (pruneDeadObjects' third map), whole, before each drain.
void WolfObjectProps::pruneWants()
{
    for (auto it = mWants.begin(); it != mWants.end();)
    {
        LLViewerObject* objectp = gObjectList.findObject(it->first);
        if (!objectp || objectp->isDead())
        {
            it = mWants.erase(it);
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

    // [FAR DRAW PERF 2026-11-14] the answers and ask records a share a frame (pruneSlice, see the header); the interest list
    // whole before each drain, as before
    pruneSlice();

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextDrain)
    {
        return;
    }
    mNextDrain = now + SWEEP_INTERVAL_SECS;
    pruneWants();
    drain();

    // One line every STATS_INTERVAL_SECS, so "the name never arrived" can be told apart
    // from "it was never asked for" without rebuilding anything.
    if (now >= mNextStatsLog)
    {
        mNextStatsLog = now + STATS_INTERVAL_SECS;
        LL_INFOS("WolfObjectProps") << "props: " << mProps.size() << " objects answered, "
                                    << mAsked.size() << " asked, " << mLastSent
                                    << " request(s) sent last drain, " << mSentTotal
                                    // <WolfViewer 2026-09-26> these totals are never reset
                                    // (reset() clears only the per-region queues), so they
                                    // are since login — the old label said "this region".
                                    << " sent since login (" << mRefreshedTotal
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
        // No region yet (or any more): LLSelectMgr::requestObjectPropertiesFamily sends to
        // object->getRegion()->getHost() with no null check (llselectmgr.cpp:6182-6183), so an
        // object whose region is still connecting crashed here at login (15 Nov, STATE_PRECACHE,
        // neighbours connecting). It is dropped from this drain only; the sweep wants it again on
        // its next pass (want() re-inserts every pass), by when its region is known.
        if (!objectp || objectp->isDead() || !objectp->getRegion())
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
