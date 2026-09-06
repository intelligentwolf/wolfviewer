/**
 * @file wolfobjectprops.h
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

#ifndef WOLF_OBJECTPROPS_H
#define WOLF_OBJECTPROPS_H

#include <map>
#include <string>

#include "llsingleton.h"
#include "lluuid.h"

class LLViewerObject;

// Source: wolfstorm/js/world/object_props_harvester.js ObjectPropsHarvester — the same
// registry, the same budget classes, the same retry policy, kept in step.
//
// ── WHY THIS EXISTS ──────────────────────────────────────────────────────────────────
// Neither the name nor the description of a prim is in ObjectUpdate. The template is
// explicit about it (message_template.msg ObjectUpdate): the ObjectData block carries
// Text, NameValue, MediaURL, ExtraParams and TextureEntry, and no Name, and no Description.
// Both strings exist in exactly two messages:
//
//   ObjectProperties        Medium  9 — sent on SELECT, and selecting freezes the
//                                       object's physics sim-side, so it is unusable as
//                                       a bulk query.
//   ObjectPropertiesFamily  Medium 10 — the reply to RequestObjectPropertiesFamily
//                                       (Medium 5), whose ObjectData is a SINGLE block:
//                                       one ObjectID per request packet. OpenSim handles
//                                       it at LLClientView.cs HandleRequestObjectPropertiesFamily.
//
// So there is no bulk path. Anything that wants to know what a prim is CALLED or what its
// description SAYS has to ask, one packet per prim, and pay for it. Stock Firestorm only
// ever asks about the object under the cursor or in a selection (LLSelectMgr::
// requestObjectPropertiesFamily) and keeps the answer in a select node, which is why this
// class carries its own store.
//
// ── WHY IT IS SHARED ─────────────────────────────────────────────────────────────────
// Two features need this, and before this class they would each have kept their own
// "already asked" bookkeeping and each have sent their own packet for the same prim:
//
//   - boat rock  (wolfboatrock.cpp nameVerdict) — is this floating thing a boat?
//   - wolfwater  (fswolfwater.cpp)              — is this prim described "wolfwater"?
//
// One registry, one budget, one retry policy. Callers declare interest with want() and
// read the answer with get(); they never send anything themselves. The answers are written
// by LLSelectMgr's ObjectProperties and ObjectPropertiesFamily handlers through note().
//
// ── THE BUDGET ───────────────────────────────────────────────────────────────────────
// A fixed number of requests per sweep, handed out in three priority classes and, within
// each class, nearest first:
//
//   0  ROOT PRIMS not yet fetched
//   1  CHILD PRIMS not yet fetched
//   2  REFRESHES — prims already fetched, last asked more than REFRESH_SECS ago
//
// ROOTS BEFORE CHILDREN. Most prims in a region are linkset children, but the things a
// feature cares about identifying — a boat, a water surface — are overwhelmingly ROOT
// prims. NEAREST FIRST: the budget goes to what the user can actually see. REFRESHES ARE
// LEFTOVERS: a description is editable in-world at any moment and the sim announces the
// change to nobody, so a refresh is only ever sent with budget that nothing new wanted —
// refreshes add no packets at all, they only fill idle slots.
//
// Nothing is persisted across sessions. On a region change the ask log and the interest
// list are dropped — every pending request refers to the old circuit — but an ANSWER is
// kept while its object still exists: UUIDs are global, a description does not change
// because the agent crossed a border, and a hull sailing across one must not stop rocking
// for want of a name it already has. Answers for objects that are gone (a teleport
// rebuilds the object list) are pruned at the same time, which bounds the store.
class WolfObjectProps : public LLSingleton<WolfObjectProps>
{
    LLSINGLETON(WolfObjectProps);
    ~WolfObjectProps();

public:
    /** What the sim last told us about an object. */
    struct Props
    {
        std::string mName;
        std::string mDescription;
        F64         mReceivedAt = 0.0;
    };

    // Source: fswolfwater.cpp (2026-09-03) request constants — 48 per 1.5 s is about 32/s
    // of a Medium-frequency packet, the same order the hover tooltip already produces when
    // a user sweeps the mouse across a crowded scene, and bounded: an object is asked once
    // and then not again for two minutes, so this is a burst on arrival, not a rate.
    // (object_props_harvester.js ships 24; WolfViewer was raised to 48 on 2026-09-03
    // because roots-first over several hundred root prims at 16/s left a prim unasked for
    // the better part of a minute.)
    static constexpr F64 SWEEP_INTERVAL_SECS = 1.5;
    static constexpr S32 REQUEST_BUDGET = 48;
    /** How long to wait for a reply before the single retry. */
    static constexpr F64 RETRY_SECS = 30.0;
    /** Attempts per object, inclusive of the first, before giving up on it. */
    static constexpr S32 MAX_TRIES = 2;
    /** How stale a known answer must be before spare budget may re-ask. */
    static constexpr F64 REFRESH_SECS = 120.0;
    /** How often the sweep reports what it is doing. */
    static constexpr F64 STATS_INTERVAL_SECS = 10.0;

    /**
     * Record what the sim just sent. Called from LLSelectMgr's ObjectProperties and
     * ObjectPropertiesFamily handlers, which are the only two places one arrives. An EMPTY
     * string is recorded as emphatically as a non-empty one — that is how a feature keyed
     * on a description is switched back off when a builder clears the keyword.
     */
    void note(const LLUUID& object_id, const std::string& name, const std::string& description);

    /** The answer for an object, or nullptr if the sim has not told us yet. */
    const Props* get(const LLUUID& object_id) const;

    /**
     * Declare interest in an object's name/description. Idempotent and cheap — safe to
     * call every sweep for every candidate. Objects that are fully answered and still
     * fresh, or that have exhausted their attempts, are dropped here rather than queued.
     */
    void want(LLViewerObject* objectp);

    /** Every frame from LLAppViewer::idle(); drains the interest list within budget. */
    void idle();

    /** Region change: drop asks and interest, prune answers whose objects are gone. */
    void reset();

private:
    void drain();

    // Priority classes for the request queue, best first.
    static constexpr S32 PRIORITY_ROOT    = 0;
    static constexpr S32 PRIORITY_CHILD   = 1;
    static constexpr S32 PRIORITY_REFRESH = 2;

    struct Ask
    {
        F64 mSentAt = 0.0;
        S32 mTries  = 0;
    };
    struct Want
    {
        F32 mDistSq   = 0.f;
        S32 mPriority = PRIORITY_ROOT;
    };

    /** object id -> what the sim last said. */
    std::map<LLUUID, Props> mProps;
    /** object id -> when we last asked, and how many times running. */
    std::map<LLUUID, Ask> mAsked;
    /** Interest registered since the last drain. */
    std::map<LLUUID, Want> mWants;

    F64 mNextDrain = 0.0;
    F64 mNextStatsLog = 0.0;
    U64 mRegionHandle = 0;

    // Diagnostics.
    S32 mSentTotal = 0;
    S32 mRefreshedTotal = 0;
    S32 mDrains = 0;
    S32 mLastSent = 0;
};

#endif // WOLF_OBJECTPROPS_H
