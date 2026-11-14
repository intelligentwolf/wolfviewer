/**
 * @file wolfobjectsweep.cpp
 * @brief WolfViewer: ONE shared, incremental pass over the object list (see wolfobjectsweep.h).
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

#include "wolfobjectsweep.h"

#include <algorithm>
#include <cmath>

#include "llframetimer.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

WolfObjectSweep::WolfObjectSweep()
{
}

WolfObjectSweep::~WolfObjectSweep()
{
    // The clients are singletons too; nothing of theirs is called from here (no endPass at shutdown).
    mWaiting.clear();
    mActive.clear();
    mSnapshot.clear();
    mRemoved.clear();
}

void WolfObjectSweep::request(Client* client)
{
    if (!client || busy(client))
    {
        return;
    }
    mWaiting.push_back(client);
}

void WolfObjectSweep::cancel(Client* client)
{
    mWaiting.erase(std::remove(mWaiting.begin(), mWaiting.end(), client), mWaiting.end());
    mActive.erase(std::remove(mActive.begin(), mActive.end(), client), mActive.end());
}

bool WolfObjectSweep::busy(const Client* client) const
{
    return std::find(mWaiting.begin(), mWaiting.end(), client) != mWaiting.end() || inRunningPass(client);
}

bool WolfObjectSweep::inRunningPass(const Client* client) const
{
    return std::find(mActive.begin(), mActive.end(), client) != mActive.end();
}

void WolfObjectSweep::noteRemoved(const LLViewerObject* objectp)
{
    // Only the running pass's snapshot can hold the pointer; outside a pass nothing is kept.
    if (mRunning && objectp)
    {
        mRemoved.insert(objectp);
    }
}

void WolfObjectSweep::noteAllRemoved()
{
    if (!mRunning)
    {
        return;
    }
    // Every pointer of the snapshot may be gone. The running pass is dropped (no endPass: it did not see everything) and its
    // clients wait for a new one, in front of the others, so none of them is left "busy" with a pass that will never end.
    std::vector<Client*> again;
    again.swap(mActive);
    again.insert(again.end(), mWaiting.begin(), mWaiting.end());
    mWaiting.swap(again);
    mSnapshot.clear();
    mRemoved.clear();
    mCursor = 0;
    mRunning = false;
}

void WolfObjectSweep::walkNow(Client* client)
{
    if (!client)
    {
        return;
    }
    cancel(client);
    ++mWalksSinceLog;
    client->beginPass();
    client->beginSlice();
    // All in this frame, so the list cannot be compacted under the index (the old sweeps' own loop: getObject is null for a
    // dead entry, llviewerobjectlist.h); the same objects are left out as in a pass (step()).
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (objectp && objectp->getPCode() == LL_PCODE_VOLUME && !objectp->isAttachment() && !objectp->isHUDAttachment())
        {
            client->visit(objectp);
        }
    }
    client->endPass();
}

void WolfObjectSweep::startPass()
{
    mActive.swap(mWaiting);
    mWaiting.clear();
    LLTimer snap;
    // Raw pointers only: copyObjectPointers reads LLPointer::get() and touches no object (wolfobjectsweep.h).
    gObjectList.copyObjectPointers(mSnapshot);
    mPassSnapshotSecs = snap.getElapsedTimeF64();
    mRemoved.clear();
    mCursor = 0;
    mRunning = true;
    mPassFrames = 0;
    mPassOverBudget = 0;
    mPassVisited = 0;
    mPassSeconds = 0.0;
    mPassMaxFrame = 0.0;
    mPassStartWall = LLFrameTimer::getElapsedSeconds();
    for (Client* c : mActive)
    {
        c->beginPass();
    }
}

void WolfObjectSweep::finishPass()
{
    // The figures of the pass that just ended, for the log line.
    mLastObjects = (U32)mSnapshot.size();
    mLastVisited = mPassVisited;
    mLastFrames = mPassFrames;
    mLastClients = (U32)mActive.size();
    mLastSeconds = mPassSeconds;
    mLastMaxFrame = mPassMaxFrame;
    mLastOverBudget = mPassOverBudget;
    mLastSnapshotSecs = mPassSnapshotSecs;
    mLastWall = LLFrameTimer::getElapsedSeconds() - mPassStartWall;
    ++mPassesSinceLog;

    std::vector<Client*> done;
    done.swap(mActive);   // an endPass may request again: that goes to mWaiting, for the next pass
    mSnapshot.clear();
    mRemoved.clear();
    mCursor = 0;
    mRunning = false;
    for (Client* c : done)
    {
        c->endPass();
    }
}

void WolfObjectSweep::step()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    logStats(now);
    if (!mRunning && mWaiting.empty())
    {
        return;
    }
    // One clock for the whole frame's share: the snapshot and the clients' beginPass (a pass's first frame), beginSlice and
    // the visits all count against FRAME_BUDGET_SECS.
    LLTimer frame;
    if (!mRunning)
    {
        startPass();
    }
    if (mActive.empty())
    {
        // Every client of this pass was cancelled: drop it.
        mSnapshot.clear();
        mRemoved.clear();
        mCursor = 0;
        mRunning = false;
        return;
    }

    const F64 budget = FRAME_BUDGET_SECS;
    // the progress floor (TARGET_PASS_SECS): this frame's share of the snapshot whatever the budget says
    const F64 frame_dt = (F64)llmax(LLFrameTimer::getFrameDeltaTimeF32(), 0.f);
    const size_t floor_objects = (size_t)ceil((F64)mSnapshot.size() * frame_dt / TARGET_PASS_SECS);
    const size_t cursor_at_start = mCursor;
    for (Client* c : mActive)
    {
        c->beginSlice();
    }
    // A client's visit never calls request / cancel (the features only do that from their own idle), so mActive is stable here.
    const size_t n = mSnapshot.size();
    const bool any_removed = !mRemoved.empty();
    S32 since_clock = 0;
    while (mCursor < n)
    {
        LLViewerObject* objectp = mSnapshot[mCursor++];
        // A removed entry may already be freed: test the pointer before reading anything through it.
        if (!objectp || (any_removed && mRemoved.count(objectp)))
        {
            continue;
        }
        // What every client skips (wolfobjectsweep.h): dead, not a prim, worn.
        if (!objectp->isDead() && objectp->getPCode() == LL_PCODE_VOLUME && !objectp->isAttachment() && !objectp->isHUDAttachment())
        {
            ++mPassVisited;
            for (Client* c : mActive)
            {
                c->visit(objectp);
            }
        }
        if (++since_clock >= CLOCK_EVERY)
        {
            since_clock = 0;
            if (frame.getElapsedTimeF64() >= budget && mCursor - cursor_at_start >= floor_objects)
            {
                break;
            }
        }
    }
    const F64 spent = frame.getElapsedTimeF64();
    if (spent > budget)
    {
        ++mPassOverBudget;
    }
    mPassSeconds += spent;
    mPassMaxFrame = llmax(mPassMaxFrame, spent);
    ++mPassFrames;
    if (mCursor >= n)
    {
        finishPass();
    }
}

void WolfObjectSweep::logStats(F64 now)
{
    if (now < mNextLog)
    {
        return;
    }
    mNextLog = now + LOG_INTERVAL_SECS;
    static LLCachedControl<bool> log_rate(gSavedSettings, "WolfLogFrameRate", false);
    if (!log_rate || mPassesSinceLog == 0)
    {
        mPassesSinceLog = 0;
        mWalksSinceLog = 0;
        return;
    }
    LL_INFOS("WolfObjectSweep") << "object sweep: " << mPassesSinceLog << " pass(es) in " << LOG_INTERVAL_SECS << " s; last: "
                                << mLastObjects << " objects (" << mLastVisited << " prims visited) for " << mLastClients
                                << " feature(s) over " << mLastFrames << " frame(s), " << mLastWall << " s, "
                                << (mLastFrames ? mLastSeconds * 1000.0 / mLastFrames : 0.0) << " ms a frame (most "
                                << mLastMaxFrame * 1000.0 << " ms, snapshot " << mLastSnapshotSecs * 1000.0 << " ms, "
                                << mLastOverBudget << " frame(s) over " << FRAME_BUDGET_SECS * 1000.0
                                << " ms to keep the pass near " << TARGET_PASS_SECS << " s); " << mWalksSinceLog
                                << " whole walk(s) for one feature" << LL_ENDL;
    mWalksSinceLog = 0;
    mPassesSinceLog = 0;
}
