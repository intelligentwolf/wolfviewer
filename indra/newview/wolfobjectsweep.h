/**
 * @file wolfobjectsweep.h
 * @brief WolfViewer: ONE shared, incremental pass over the object list for every Wolf feature that looks at every prim.
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

#ifndef WOLF_OBJECT_SWEEP_H
#define WOLF_OBJECT_SWEEP_H

#include "llsingleton.h"
#include "stdtypes.h"

#include <unordered_set>
#include <vector>

class LLViewerObject;

/**
 * [FAR DRAW PERF 2026-11-14] Paddington Station, draw distance 4096 m, ~98,000 prims in 22 regions (W107): eight Wolf
 * features each walked the WHOLE object list on their own timer, every 1 to 2.5 s, each in one frame - about 12 ms a frame on
 * average, delivered as 15-30 ms hitches (perf: WolfWaterClearance 3.9 %, WolfNaturalWater 2.6 %, WolfWeather 1.6 %,
 * FSWolfWater 1.5 %, WolfObjectProps 1.4 %, WolfDryVolumes 0.9 %, WolfLeaves 0.6 %, WolfBoatRock 0.5 % of the main thread).
 *
 * Now there is one pass, walked a slice a frame inside FRAME_BUDGET_SECS of wall time, and each feature is a Client of it:
 *
 *   request(c)   the feature wants a pass (its own timer fired, its own switches and grid gates allow it). It joins the NEXT
 *                pass; a pass starts as soon as any client waits, so the pass runs continuously only while someone wants one.
 *   beginPass()  the client's pass begins: it clears what it collects.
 *   beginSlice() once in each frame of its pass, before that frame's visits: it reads what is per frame (the camera, the draw
 *                distance, its region's origin) and can notice that what its pass depends on changed (and spoil the pass).
 *   visit(o)     one object. Only objects that every client skips anyway are left out here: dead ones, anything that is not a
 *                prim (LL_PCODE_VOLUME: avatars, trees, grass, terrain patches, water planes, particle groups) and attachments
 *                (isAttachment / isHUDAttachment). Each client still applies the rest of its own original filter.
 *   endPass()    every object of the pass has been offered.
 *   cancel(c)    the feature's switch went off or it was reset: its waiting request and its share of the running pass are
 *                dropped, no endPass.
 *
 * EVERY OBJECT ALIVE AT THE PASS START IS VISITED EXACTLY ONCE. gObjectList compacts itself (LLViewerObjectList::cleanDeadObjects,
 * llviewerobjectlist.cpp: a dead entry is swapped with one from the END of the list), so an index cursor kept across frames
 * would skip the live objects moved forward and could see others twice. The pass therefore takes a SNAPSHOT of the list's raw
 * pointers when it starts (copyObjectPointers: no object is touched, no reference count changes - copying 98k LLPointers would
 * write to 98k objects in one frame), and walks that. The pointers stay safe to read because an object can only be freed after
 * cleanDeadObjects (or killAllObjects) takes it out of mObjects, and both tell this class first (noteRemoved / noteAllRemoved):
 * a removed entry is skipped without being read. An object made during a pass is seen by the NEXT pass (latency: at most one
 * pass, logged below); one that dies during a pass is skipped (isDead()).
 *
 * TIMING. A feature asks for a pass when its own interval has run (unchanged constants) and the pass it asked for last is over,
 * so it is refreshed every max(its interval, one pass). How long a pass takes is in the log line below; each feature notes
 * next to its interval what the extra latency means for it. With WolfLogFrameRate on, one line every LOG_INTERVAL_SECS:
 * passes, objects a pass, frames a pass, ms a frame.
 * [review 2] A pass is kept near TARGET_PASS_SECS (1 s) at any frame rate by a progress floor that may take a frame past
 * FRAME_BUDGET_SECS at a low frame rate; the clearance fields and the weather can also ask for walkNow (the old one-frame
 * walk, with its one-time hitch) where waiting for a pass would show a resident something wrong.
 */
class WolfObjectSweep : public LLSingleton<WolfObjectSweep>
{
    LLSINGLETON(WolfObjectSweep);
    ~WolfObjectSweep();

public:
    /** The lead's brief for W107 (Paul, 14 Nov: "a budgeted slice of the object list per frame"): at most 1 ms of wall time a
     *  frame for the visits. */
    static constexpr F64 FRAME_BUDGET_SECS = 0.001;
    /** The clock is read every this many objects offered (a visit is far more than a clock read; the overrun is at most this
     *  many visits). */
    static constexpr S32 CLOCK_EVERY = 8;
    /** The pass's own log line (WolfLogFrameRate only), as FSWolfWater's "sweep:" line: every 10 s. */
    static constexpr F64 LOG_INTERVAL_SECS = 10.0;
    /** [FAR DRAW PERF 2026-11-14, review 2] The progress floor: whatever the 1 ms budget allows, each frame offers at least
     *  ceil(snapshot size x this frame's time / TARGET_PASS_SECS) objects, so at a low frame rate a pass still takes about
     *  TARGET_PASS_SECS and never far more than the shortest client interval: WolfWaterClearance::OBJECT_INTERVAL_SECS and
     *  WolfCloudClearance::INTERVAL_SECS, both 1 s (wolfwaterfield.h, wolfcloudclearance.h). A frame that needs more than
     *  the budget to meet the floor goes over it (counted in the log line). */
    static constexpr F64 TARGET_PASS_SECS = 1.0;

    class Client
    {
    public:
        virtual ~Client() = default;
        virtual void beginPass() = 0;
        virtual void beginSlice() {}
        virtual void visit(LLViewerObject* objectp) = 0;
        virtual void endPass() = 0;
    };

    /** Join the next pass. Nothing if the client already waits for one or is in the running one. */
    void request(Client* client);
    /** Drop the client's waiting request and its share of the running pass (its endPass is not called). */
    void cancel(Client* client);
    /** Waiting for a pass, or in the running one. */
    bool busy(const Client* client) const;
    /** In the running pass (beginPass has been called, endPass not yet). */
    bool inRunningPass(const Client* client) const;

    /** Once a frame from LLAppViewer::idle(): start a pass if one is wanted, then visit until the frame's budget is spent. */
    void step();

    /** LLViewerObjectList::cleanDeadObjects, before it drops the list's reference to a dead object. */
    void noteRemoved(const LLViewerObject* objectp);
    /** LLViewerObjectList::killAllObjects, before it clears the list: the running pass starts again for its clients. */
    void noteAllRemoved();

    /** [FAR DRAW PERF 2026-11-14, review 2] One whole walk of the object list for one client, now, in this frame - the old
     *  one-frame sweep - for the few moments a feature cannot wait for a pass (a clearance field with nothing to show yet,
     *  a weather pass spoiled twice running). The client's share of a running pass is dropped first; beginPass, one
     *  beginSlice, the same filtered visits and endPass are called as for a pass. */
    void walkNow(Client* client);

private:
    void startPass();
    void finishPass();
    void logStats(F64 now);

    std::vector<Client*> mWaiting;
    std::vector<Client*> mActive;
    std::vector<LLViewerObject*> mSnapshot;
    std::unordered_set<const LLViewerObject*> mRemoved;   // taken out of gObjectList since the snapshot: never read
    size_t mCursor = 0;
    bool mRunning = false;

    // the log line's figures
    U32 mPassFrames = 0;
    U32 mPassVisited = 0;
    F64 mPassSeconds = 0.0;
    F64 mPassMaxFrame = 0.0;
    F64 mPassSnapshotSecs = 0.0;
    U32 mLastObjects = 0, mLastVisited = 0, mLastFrames = 0, mLastClients = 0;
    F64 mLastSeconds = 0.0, mLastMaxFrame = 0.0, mLastSnapshotSecs = 0.0, mLastWall = 0.0;
    F64 mPassStartWall = 0.0;
    U32 mPassesSinceLog = 0;
    U32 mPassOverBudget = 0, mLastOverBudget = 0;   // frames the progress floor took past FRAME_BUDGET_SECS
    U32 mWalksSinceLog = 0;                         // walkNow calls
    F64 mNextLog = 0.0;
};

#endif // WOLF_OBJECT_SWEEP_H
