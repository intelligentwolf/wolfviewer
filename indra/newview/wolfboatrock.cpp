/**
 * @file wolfboatrock.cpp
 * @brief WolfViewer: client-side buoyancy — boats ride the drawn water.
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

#include "wolfboatrock.h"
#include "wolfgrid.h"   // <WolfViewer 2026-09-22/> Wolf Territories only

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <vector>

#include "llappviewer.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "lldrawpoolwater.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"   // <WolfViewer 2026-10-05/> the land under a hull past its region's edge
#include "pipeline.h"
#include "wolfobjectprops.h"
#include "wolfseastate.h"
#include "wolfwaterfield.h"
#include "wolfwavezones.h"   // [WAVES 2026-09-07]

// Source: wolfstorm/js/world/terrain/terrain_manager.js [ROCK 2026-08-15].

namespace
{
    // Source: terrain_manager.js:34-35
    //   static _NOT_BOAT_RE = /dock|boat ?house/i;
    //   static _BOAT_RE = /boat|jet ?ski|yacht|dinghy|dinghies|canoe|kayak|catamaran|gondola|\bships?\b|\bsail(?:ing|s|boats?)?\b/i;
    // <WolfViewer 2026-10-05> + "manager": the SF Sail boat system's "SFsail Manager" control box is
    // not a boat (KEEP IN STEP with WolfStorm terrain_manager.js _NOT_BOAT_RE).
    const std::regex NOT_BOAT_RE("dock|boat ?house|\\bmanager\\b", std::regex::ECMAScript | std::regex::icase);
    // <WolfViewer 2026-09-22> Boards ride the swell too (Paul asked for a surfboard). Word
    // boundaries throughout so "surface", "resurfaced" and "keyboard" are not boats.
    // KEEP IN STEP with WolfStorm terrain_manager.js _BOAT_RE.
    // <WolfViewer 2026-09-26> Paul: "purely by name anything with boat, ship, tug, etc": more
    // vessel words, whole words so "aircraft" is not a raft.
    const std::regex BOAT_RE("boat|jet ?ski|yacht|dinghy|dinghies|canoe|kayak|catamaran|gondola|\\bships?\\b|\\bsail(?:ing|s|boats?)?\\b"
                             "|\\bsurf ?boards?\\b|\\bpaddle ?boards?\\b|\\bbody ?boards?\\b|\\blong ?boards?\\b|\\bsurf\\b"
                             "|\\btugs?\\b|\\bferry\\b|\\bferries\\b|\\bbarges?\\b|\\btrawlers?\\b|\\bcruisers?\\b|\\brafts?\\b|\\bpunts?\\b"
                             "|\\bliners?\\b|\\bfreighters?\\b|\\btankers?\\b|\\bvessels?\\b|\\bschooners?\\b|\\bketch(?:es)?\\b|\\bsloops?\\b"
                             "|\\bgalleons?\\b|\\bfrigates?\\b|\\bhovercraft\\b|\\bpedalos?\\b"
                             // <WolfViewer 2026-10-05> Paul: "sf sailboat is a boat" - SF Sail boats are named
                             // sfsail, sfsail_4, sfsail_5... (Madrigal), no word boundary before "sail".
                             "|\\bsfsail"
                             // <WolfViewer 2026-10-05> Paul: "bouy should be another wave keyword" - buoys
                             // bob on the swell (and the common spelling "bouy").
                             "|\\bbuoys?\\b|\\bbouys?\\b",
                             std::regex::ECMAScript | std::regex::icase);
    // <WolfViewer 2026-09-22> The boards out of that list. A board PLANES: it rides the crest
    // and sits on the surface in the trough, it never goes under. A hull does go under --
    // the waterline band below reaches 3 m down for exactly that reason -- so the two cannot
    // share one rule. KEEP IN STEP with WolfStorm terrain_manager.js _BOARD_RE.
    const std::regex BOARD_RE("\\bsurf ?boards?\\b|\\bpaddle ?boards?\\b|\\bbody ?boards?\\b|\\blong ?boards?\\b|\\bsurf\\b",
                              std::regex::ECMAScript | std::regex::icase);

    // Source: terrain_manager.js updateFloaters() — the low-pass runs per 30 Hz logic
    // tick: bob += (target - bob) * 0.15, slopes * 0.12 ("hulls have inertia, and 30Hz
    // steps read as continuous"). The viewer's frame rate is whatever the GPU gives, so
    // the same filter is expressed as a time constant: 0.15 per 1/30 s is
    // tau = -(1/30) / ln(1 - 0.15) = 0.2051 s, 0.12 per 1/30 s is 0.2608 s, and the
    // per-frame factor is 1 - exp(-dt / tau), which is exactly 0.15 / 0.12 at 30 fps.
    const F32 BOB_TAU_SECS   = 0.2051f;
    const F32 SLOPE_TAU_SECS = 0.2608f;

    F32 lowpassAlpha(F32 dt, F32 tau)
    {
        return 1.f - expf(-llmax(dt, 0.f) / tau);
    }

    // Source: lldrawpoolwater.cpp:203 phase_time = LLFrameTimer::getElapsedSeconds() * 0.5f
    // (uploaded as WATER_TIME, the `time` uniform waterV.glsl phases the swell with).
    F32 waterTime()
    {
        return (F32)LLFrameTimer::getElapsedSeconds() * 0.5f;
    }
}

WolfBoatRock::WolfBoatRock()
{
}

WolfBoatRock::~WolfBoatRock()
{
    // The object list outlives this singleton at shutdown; dropping the references is all
    // that is wanted here, and nothing is marked moved on a pipeline that may be gone.
    mRockers.clear();
}

// Source: terrain_manager.js updateFloaters() — the tick: init, kill switch, sweep, step.
void WolfBoatRock::idle()
{
    // <WolfViewer 2026-09-22> Wolf Territories only (Paul 09-22: "I don't want people from
    // other grids getting a free ride on what we have created"). Off-grid this whole
    // subsystem is inert, which also saves the work it would otherwise do for nothing.
    if (!WolfGrid::isWolfTerritories()) return;
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerBoatRock", true);
    if (!enabled)
    {
        if (!mRockers.empty())
        {
            evictAll();
        }
        return;
    }
    if (LLPipeline::FreezeTime)
    {
        return;
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now > mNextSweep)
    {
        mNextSweep = now + SWEEP_INTERVAL_SECS;
        sweep();
    }
    if (!mRockers.empty())
    {
        step();
    }

    if (now >= mNextStatsLog)
    {
        mNextStatsLog = now + STATS_INTERVAL_SECS;
        LL_INFOS("WolfBoatRock") << "sweep: " << mStatBand << " root prims in the waterline band within "
                                 << RADIUS_M << "m, " << mStatQualify << " qualify, "
                                 << mRockers.size() << " rocking (cap " << MAX_ROCKERS << "), "
                                 << mStatWantName << " waiting for a name, "
                                 << mStatDenied << " denied" << LL_ENDL;
        // <WolfViewer 2026-10-06> How far the nearest rocking hulls really moved since the last line.
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        std::vector<std::pair<F32, Rocker*>> near_;
        for (auto& kv : mRockers)
        {
            if (kv.second.mObject.notNull() && !kv.second.mObject->isDead())
            {
                near_.push_back({ (kv.second.mObject->getPositionAgent() - cam).lengthSquared(), &kv.second });
            }
        }
        std::sort(near_.begin(), near_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < near_.size() && k < 8; ++k)
        {
            Rocker& r = *near_[k].second;
            const WolfObjectProps::Props* np = WolfObjectProps::instance().get(r.mObject->getID());
            LL_DEBUGS("WolfBoatRock") << "moves: [" << (np ? np->mName : std::string("?")) << "] gain " << r.mGain
                                      << " peak bob " << r.mPeakBob << " m, peak slope " << r.mPeakSlope
                                      << " at " << llround(sqrtf(near_[k].first)) << " m" << LL_ENDL;
            r.mPeakBob = 0.f;
            r.mPeakSlope = 0.f;
        }
    }
}

// Source: terrain_manager.js updateFloaters() — the 2.5 s sweep ([FIX 2026-08-24]: cap on
// the NEW set alone, candidates picked by DISTANCE not localId; [ROCK-NAME 2026-08-29]:
// name wants routed to the shared harvester).
void WolfBoatRock::sweep()
{
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();

    struct Candidate
    {
        LLViewerObject* mObject;
        Verdict         mVerdict;
        F32             mDistSq;
    };
    std::vector<Candidate> cands;
    WolfObjectProps& props = WolfObjectProps::instance();

    mStatBand = 0;
    mStatQualify = 0;
    mStatWantName = 0;
    mStatDenied = 0;

    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp)
        {
            continue;
        }
        const Verdict verdict = classify(objectp);
        if (verdict.mInBand)
        {
            ++mStatBand;
            if (!verdict.mRock && !verdict.mWantName)
            {
                ++mStatDenied;
                const WolfObjectProps::Props* np = props.get(objectp->getID());
                LL_DEBUGS("WolfBoatRock") << "still " << objectp->getID() << ": " << verdict.mWhy
                                          << " [" << (np ? np->mName : std::string("?")) << "] at "
                                          << objectp->getPositionRegion() << LL_ENDL;
            }
        }
        if (verdict.mWantName)
        {
            ++mStatWantName;
            // Names only arrive via ObjectPropertiesFamily, one packet per prim, through
            // the ONE shared harvester (wolfobjectprops.cpp). want() is idempotent and
            // drops anything already fetched, in flight or given up on.
            props.want(objectp);
        }
        if (!verdict.mRock)
        {
            continue;
        }
        const LLVector3 pa = objectp->getPositionAgent();
        const F32 ddx = pa.mV[VX] - camera_pos.mV[VX];
        const F32 ddy = pa.mV[VY] - camera_pos.mV[VY];
        cands.push_back({ objectp, verdict, ddx * ddx + ddy * ddy });
        ++mStatQualify;
    }
    // The budget must go to what the user can SEE: nearest 48.
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.mDistSq < b.mDistSq; });
    if (cands.size() > (size_t)MAX_ROCKERS)
    {
        cands.resize(MAX_ROCKERS);
    }

    std::set<const LLViewerObject*> keep;
    for (const Candidate& c : cands)
    {
        keep.insert(c.mObject);
        auto it = mRockers.find(c.mObject);
        if (it != mRockers.end())
        {
            it->second.mGain = c.mVerdict.mGain;
            it->second.mBoard = c.mVerdict.mBoard;   // <WolfViewer 2026-09-22/>
            hullExtents(c.mObject, it->second.mMin, it->second.mMax);   // <WolfViewer 2026-09-27/> prims can be linked or resized
        }
        else
        {
            Rocker r;
            r.mObject = c.mObject;
            r.mGain = c.mVerdict.mGain;
            r.mBoard = c.mVerdict.mBoard;   // <WolfViewer 2026-09-22/>
            hullExtents(c.mObject, r.mMin, r.mMax);   // <WolfViewer 2026-09-27/>
            mRockers[c.mObject] = r;
            LL_DEBUGS("WolfBoatRock") << "rocking " << c.mObject->getID() << " gain " << c.mVerdict.mGain
                                      << ": " << c.mVerdict.mWhy << LL_ENDL;
        }
    }
    for (auto it = mRockers.begin(); it != mRockers.end();)
    {
        if (keep.find(it->first) == keep.end())
        {
            auto dying = it++;
            evict(dying);
        }
        else
        {
            ++it;
        }
    }
}

// Source: terrain_manager.js updateFloaters() — the per-tick loop.
void WolfBoatRock::step()
{
    const F32 t = waterTime();
    const F32 dt = (F32)gFrameIntervalSeconds;
    const F32 bob_alpha = lowpassAlpha(dt, BOB_TAU_SECS);
    const F32 slope_alpha = lowpassAlpha(dt, SLOPE_TAU_SECS);
    const LLVector3 up(0.f, 0.f, 1.f);
    const Sea sea = seaNow();   // <WolfViewer 2026-09-27/> once per frame, not per measured point

    for (auto it = mRockers.begin(); it != mRockers.end();)
    {
        Rocker& r = it->second;
        LLViewerObject* objectp = r.mObject;
        if (!objectp || objectp->isDead() || objectp->mDrawable.isNull() || objectp->mDrawable->isDead())
        {
            // Gone: nothing to restore, the drawable is dead or never existed.
            auto dying = it++;
            mRockers.erase(dying);
            continue;
        }
        LLDrawable* drawablep = objectp->mDrawable;

        // Never fight the edit gizmo on a selected object. LLManip drives the drawable
        // undamped while dragging, so the offset comes off rather than riding on top of it.
        if (objectp->isSelected())
        {
            if (r.mApplied)
            {
                r.mTargetBob = 0.f;
                r.mTargetTilt.loadIdentity();
                // One more updateXform with a zero target clears the applied offset.
                if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
                {
                    gPipeline.markMoved(drawablep, true);
                }
            }
            ++it;
            continue;
        }

        Sample w = measureHull(r, objectp, sea, t);
        const Sample s = mooringSway(r, objectp, t);
        w.mZ += s.mZ;
        w.mSx += s.mSx;
        w.mSy += s.mSy;
        // gain: 1.0 physical/crewed, 0.6 parked floaters (classify())
        const F32 g = (r.mGain > 0.f) ? r.mGain : 1.f;
        r.mBob += (w.mZ * g - r.mBob) * bob_alpha;
        // <WolfViewer 2026-09-22> A BOARD PLANES. The bob is an offset from where the sim
        // put the object, and on the downstroke it goes NEGATIVE — correct for a hull, which
        // rides down into the trough (the waterline band reaches 3 m down for exactly that),
        // but it is what pushed a surfboard under the surface. A board rides the crest up and
        // sits on the surface in the trough: clamp the downstroke away, keep the lift.
        // Clamped after the low-pass so the smoothing cannot reintroduce a dip.
        if (r.mBoard && r.mBob < 0.f)
        {
            r.mBob = 0.f;
        }
        r.mSx += (w.mSx * g - r.mSx) * slope_alpha;
        r.mSy += (w.mSy * g - r.mSy) * slope_alpha;

        // <WolfViewer 2026-09-27> No tiltScale (6 m / hull radius) any more: the slope is
        // measured across the hull's own length and beam (measureHull), which is what made a
        // big vessel steadier than a dinghy in the first place.
        // n = normalize(-sx, -sy, 1); tiltQ = up -> n.
        LLVector3 n(-r.mSx, -r.mSy, 1.f);
        n.normalize();
        r.mTargetBob = r.mBob;
        r.mTargetTilt.shortestArc(up, n);
        r.mPeakBob = llmax(r.mPeakBob, fabsf(r.mBob));
        r.mPeakSlope = llmax(r.mPeakSlope, sqrtf(r.mSx * r.mSx + r.mSy * r.mSy));

        // The offset lands in LLDrawable::updateXform() (apply/removeApplied); all this
        // pass does is make sure updateXform runs for the hull this frame. Damped, and only
        // when nobody else has already queued it: markMoved(undamped) would trump a damped
        // ObjectUpdate queued earlier this frame, and markMoved(damped) on an already
        // queued drawable would clear an UNDAMPED that LLManip or a sim update asked for.
        if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
        {
            gPipeline.markMoved(drawablep, true);
        }
        ++it;
    }
}

// Source: terrain_manager.js _restoreFloater() — put a floater back on its authoritative
// transform when it stops rocking. Here the authoritative transform is what
// LLDrawable::updateXform() derives from the viewer object once no rocker exists for it, so
// restoring is: forget the rocker, run updateXform once more.
void WolfBoatRock::evict(std::map<const LLViewerObject*, Rocker>::iterator it)
{
    LLViewerObject* objectp = it->second.mObject;
    const bool applied = it->second.mApplied;
    mRockers.erase(it);
    if (applied && objectp && !objectp->isDead() && objectp->mDrawable.notNull() && !objectp->mDrawable->isDead())
    {
        LLDrawable* drawablep = objectp->mDrawable;
        if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
        {
            gPipeline.markMoved(drawablep, true);
        }
    }
}

void WolfBoatRock::evictAll()
{
    while (!mRockers.empty())
    {
        evict(mRockers.begin());
    }
}

// LLDrawable::updateXform() hooks. Source: terrain_manager.js updateFloaters():
//   const base = o._lastPos || o.position;  o.group.position.z = base.z + st.bob;
//   tmp.baseQ.set(br...); tmp.tiltQ.setFromUnitVectors(up, n);
//   o.group.quaternion.multiplyQuaternions(tmp.tiltQ, tmp.baseQ);
// three.js multiplyQuaternions(a, b) applies b then a; LL's `a * b` applies a then b
// (llquaternion.cpp operator*, and llvoavatar.cpp:9101 av_rot = av_rot * obj_rot), so the
// same "base first, then a world-space tilt" is base * tilt here.
void WolfBoatRock::removeApplied(const LLViewerObject* objectp, LLVector3& pos, LLQuaternion& rot) const
{
    if (mRockers.empty())
    {
        return;
    }
    auto it = mRockers.find(objectp);
    if (it == mRockers.end() || !it->second.mApplied)
    {
        return;
    }
    pos.mV[VZ] -= it->second.mAppliedBob;
    rot = rot * ~it->second.mAppliedTilt;
}

void WolfBoatRock::apply(const LLViewerObject* objectp, LLVector3& pos, LLQuaternion& rot)
{
    if (mRockers.empty())
    {
        return;
    }
    auto it = mRockers.find(objectp);
    if (it == mRockers.end())
    {
        return;
    }
    Rocker& r = it->second;
    pos.mV[VZ] += r.mTargetBob;
    rot = rot * r.mTargetTilt;
    r.mAppliedBob = r.mTargetBob;
    r.mAppliedTilt = r.mTargetTilt;
    r.mApplied = true;
}

// Source: terrain_manager.js _classifyFloater(o, om) — "Is this object a floater, and how
// hard should it rock?"
//
// The classification problem: boats sit IN the water — hulls BELOW the waterline — and
// most are non-physical until their script enables physics when someone sits. A pontoon
// floats at the surface too but must NOT rock. The client cannot read scripts, so:
//  - PHYSICAL + floating       -> rocks at full strength (free-floating by definition;
//    Source: object_flags.h FLAGS_USE_PHYSICS = 1<<0, LLViewerObject::flagUsePhysics()).
//  - someone ABOARD (a seated avatar is a child object) -> rocks at full strength — the
//    sat-on boat case even before the physics update round-trips.
//  - parked/static floaters    -> gentle rock (gain 0.6) when NAMED or DESCRIBED as a boat
//    or board ([ROCK-NAME 2026-08-29]). The wide-thin-PLATE and bottom-on-seabed gates were
//    removed 2026-09-26: they read the root prim only and refused boats built on a deck/hull
//    plate root; the name gate covers pontoons, docks and pilings.
WolfBoatRock::Verdict WolfBoatRock::classify(LLViewerObject* objectp) const
{
    Verdict v;
    auto no = [&v](const char* why) { v.mRock = false; v.mGain = 0.f; v.mWhy = why; return v; };

    if (!objectp || objectp->isDead() || objectp->mDrawable.isNull() || objectp->mDrawable->isDead())
    {
        return no("gone");
    }
    if (objectp->isAvatar() || objectp->isAttachment() || objectp->isHUDAttachment())
    {
        return no("avatar/attachment");
    }
    // The viewer's object list also holds water, sky, terrain patches, particle groups,
    // trees and grass; WolfStorm's holds prims and avatars only. A hull is a volume.
    if (objectp->getPCode() != LL_PCODE_VOLUME)
    {
        return no("not a volume");
    }
    if (!objectp->isRoot())
    {
        return no("child prim");
    }
    LLViewerRegion* regionp = objectp->getRegion();
    if (!regionp)
    {
        return no("no region");
    }
    // if (dx*dx + dy*dy > 256*256) return no('far') — camera XY distance, agent space.
    const LLVector3 pa = objectp->getPositionAgent();
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();
    {
        const F32 dx = pa.mV[VX] - camera_pos.mV[VX];
        const F32 dy = pa.mV[VY] - camera_pos.mV[VY];
        if (dx * dx + dy * dy > RADIUS_M * RADIUS_M)
        {
            return no("far");
        }
    }
    // Everything from here is in the OBJECT'S OWN region's frame: its water height, its
    // terrain, its baked fields. WolfStorm has one region and rejects anything past its
    // edge ("we cannot know the terrain outside our own region, so do not pretend to");
    // the viewer holds every neighbour's land, so a hull moored across the border is
    // classified against the land it is actually over.
    const LLVector3 p = objectp->getPositionRegion();
    const F32 wh = regionp->getWaterHeight();
    // Hulls ride BELOW the waterline — band reaches 3m down.
    // <WolfViewer 2026-10-01> ...or the HULL crosses the waterline. The band tests the ROOT
    // prim's centre, and a tall ship's centre is far above the water: Paul's 64 x 14 x 17.8 m
    // welcome ferry floats with its centre 6.5 m up (wolfferry build notes), so it was refused
    // before its name was ever read and sat dead still while 30 m surf rolled past (Paul 10-01:
    // "i want bigger boats to rock ... the boats should always respond to the waves"). A
    // linkset whose vertical span (hullVerticalSpan: root + linked prims, rotated) has water
    // above its bottom and air below its top is in the water whatever its root's height. The
    // name / physics gates below still decide whether it is a boat.
    // Source: terrain_manager.js _classifyFloater, same rule.
    if (p.mV[VZ] < wh - 3.0f || p.mV[VZ] > wh + 2.5f)
    {
        F32 zlo, zhi;
        hullVerticalSpan(objectp, zlo, zhi);
        if (!(zlo < wh && zhi > wh))
        {
            return no("outside waterline band");
        }
    }
    v.mInBand = true;
    const LLSurface& land = regionp->getLand();
    // Source: wolfwaterfield.cpp idle() — the land is not there until the grid is.
    if (land.getGridsPerEdge() < 4)
    {
        return no("terrain not loaded");
    }
    const F32 width = regionp->getWidth();
    F32 th;
    if (p.mV[VX] < 0.f || p.mV[VX] > width || p.mV[VY] < 0.f || p.mV[VY] > width)
    {
        // <WolfViewer 2026-10-05> Past this region's edge but still its own (the open sea,
        // WolfOpenSea.cs, or visiting land over a neighbour, WolfVisitingLand.cs). Paul: "if you
        // have waves out in opensea the boats are not going over them" - every hull out there was
        // refused here. Over a neighbour: that region's land, which the viewer holds. Over open
        // sea (no region there, the region allows it): deep water. Otherwise unknown, as before.
        const LLVector3d pg = regionp->getPosGlobalFromRegion(p);
        LLViewerRegion* over = LLWorld::getInstance()->getRegionFromPosGlobal(pg);
        if (over && over != regionp)
        {
            if (over->getLand().getGridsPerEdge() < 4)
            {
                return no("neighbour's terrain not loaded");
            }
            th = over->getLand().resolveHeightGlobal(pg);
        }
        else if (!over && regionp->getWolfOpenSeaMeters() > 0.f)
        {
            th = wh - 100.f;
        }
        else
        {
            return no("outside this region (terrain unknown)");
        }
    }
    else
    {
        th = land.resolveHeightRegion(p.mV[VX], p.mV[VY]);
    }
    if (th > wh - 0.4f)
    {
        // <WolfViewer 2026-10-06> The numbers behind the refusal (Madrigal: 372 of 400 in-band
        // objects refused here), so a shallow berth can be told from a hull on dry land.
        LL_DEBUGS("WolfBoatRock") << "not over water " << objectp->getID() << " at " << p
                                  << " ground " << th << " water " << wh << LL_ENDL;
        return no("not over water");
    }

    const bool physical = objectp->flagUsePhysics();
    bool crewed = false;
    {
        // A seated avatar is a child of the prim it sits on, which is a child of the root
        // when the seat is not the root itself: check both levels.
        LLViewerObject::const_child_list_t& kids = objectp->getChildren();
        for (LLViewerObject::child_list_t::const_iterator ki = kids.begin(); ki != kids.end() && !crewed; ++ki)
        {
            LLViewerObject* kid = *ki;
            if (!kid)
            {
                continue;
            }
            if (kid->isAvatar())
            {
                crewed = true;
                break;
            }
            LLViewerObject::const_child_list_t& grandkids = kid->getChildren();
            for (LLViewerObject::child_list_t::const_iterator gi = grandkids.begin(); gi != grandkids.end(); ++gi)
            {
                LLViewerObject* grandkid = *gi;
                if (grandkid && grandkid->isAvatar())
                {
                    crewed = true;
                    break;
                }
            }
        }
    }
    // [ROCK-NAME 2026-08-29] The object's NAME decides. A name containing "dock" is never
    // a boat (even crewed or physical — someone sat on a dock is still on a dock); a
    // static floater must be named OR DESCRIBED as a boat to rock; physical/crewed hulls
    // keep rocking regardless of an unknown name because physics floating is real
    // floating. Names are fetched by the sweep (mWantName) — until one lands the object
    // stays still, the safe default.
    const NameVerdict nv = nameVerdict(objectp);
    if (nv == NAME_NOT_BOAT)
    {
        return no("name says not a boat");
    }
    // <WolfViewer 2026-09-22/> carried through to idle(), which holds a board on the surface
    v.mBoard = (nv == NAME_BOARD);
    if (physical || crewed)
    {
        v.mRock = true;
        v.mGain = 1.0f;
        v.mWhy = physical ? "physical" : "crewed";
        return v;
    }
    // Parked/static floater.
    // <WolfViewer 2026-09-26> The pontoon-vs-hull "flat plate" gate is gone (WolfStorm
    // terrain_manager.js _classifyFloater the same): it measured the ROOT PRIM, and a boat
    // whose root is its deck or hull plate (wider than 5 m, 8x wider than tall) was refused
    // as a pontoon before its name was ever asked for, so a freshly rezzed boat sat still
    // until someone boarded it (Paul 09-26, log: "flat plate (pontoon/dock)"). The name gate
    // below does that job now: a parked floater rocks only when named or described as a
    // boat, and "dock" / "boathouse" names are refused above.
    // The "bottom on seabed" gate went too: it measured the root prim as well. Paul 09-26:
    // rock "purely by name ... that is on water" — the waterline band and the over-water test
    // above are the "on water"; the name or description is the rest.
    if (nv == NAME_UNKNOWN)
    {
        v.mRock = false;
        v.mGain = 0.f;
        v.mWantName = true;
        v.mWhy = "static floater, name not fetched yet";
        return v;
    }
    if (nv != NAME_BOAT && nv != NAME_BOARD)   // <WolfViewer 2026-09-26/> a parked board rocks too
    {
        return no("static floater with no boat word in name/description");
    }
    v.mRock = true;
    v.mGain = 0.6f;
    v.mWhy = "static floater named/described as a boat";
    return v;
}

// Source: terrain_manager.js _boatNameVerdict(o) — classify an object by its name OR
// description. The deny list stays NAME-only: a thing NAMED "dock" is a dock, but a boat
// whose description says "moored at the dock" must not be denied for it. Both strings
// arrive together in ObjectPropertiesFamily (WolfObjectProps::note from LLSelectMgr), so
// checking the description costs no extra round trip.
WolfBoatRock::NameVerdict WolfBoatRock::nameVerdict(const LLViewerObject* objectp) const
{
    const WolfObjectProps::Props* props = WolfObjectProps::instance().get(objectp->getID());
    if (!props)
    {
        return NAME_UNKNOWN;
    }
    const std::string& name = props->mName;
    const std::string& desc = props->mDescription;
    if (name.empty() && desc.empty())
    {
        return NAME_NOT_BOAT;
    }
    if (std::regex_search(name, NOT_BOAT_RE))
    {
        return NAME_NOT_BOAT;
    }
    if (std::regex_search(name, BOARD_RE) || std::regex_search(desc, BOARD_RE))
    {
        return NAME_BOARD;   // <WolfViewer 2026-09-22/> floats, but never dips under
    }
    if (std::regex_search(name, BOAT_RE) || std::regex_search(desc, BOAT_RE))
    {
        return NAME_BOAT;
    }
    return NAME_NOT_BOAT;
}

// <WolfViewer 2026-09-27> The sea state the water is drawn with this frame, read once per
// step(). The same reads sampleWave() made per hull before it became waveHeight().
WolfBoatRock::Sea WolfBoatRock::seaNow() const
{
    Sea sea;
    // The sea state the water was drawn with (lldrawpoolwater.cpp renderPostDeferred sets
    // it every frame from the region's EEP water, or the manual height when pinned).
    WolfSeaState st;
    LLDrawPoolWater* wp = nullptr;
    if (LLDrawPool* poolp = gPipeline.findPool(LLDrawPool::POOL_WATER))
    {
        wp = static_cast<LLDrawPoolWater*>(poolp);
        st = wp->getSeaState();
    }
    // Source: lldrawpoolwater.cpp:326-332 — waveFrequency = sea_from_region ?
    // mSeaState.mFrequency : max(0.001, WolfViewerWaterWaveScale); waveSpeed =
    // WolfViewerWaterWaveSpeed; :355 shoreWavesEnabled = WolfViewerWaterShoreField.
    // waveAmplitude (:453) is mSeaState.mAmplitude either way — the manual path writes the
    // pinned height into mSeaState.mAmplitude (:227).
    static LLCachedControl<bool> sea_from_region(gSavedSettings, "WolfViewerWaterSeaStateFromRegion", true);
    static LLCachedControl<F32> wave_scale(gSavedSettings, "WolfViewerWaterWaveScale", 0.1f);
    static LLCachedControl<F32> wave_speed(gSavedSettings, "WolfViewerWaterWaveSpeed", 1.f);
    static LLCachedControl<bool> shore_on(gSavedSettings, "WolfViewerWaterShoreField", true);
    sea.mAmp = st.mAmplitude;
    sea.mFreq = sea_from_region ? st.mFrequency : llmax(0.001f, (F32)wave_scale);
    sea.mSpeed = (F32)wave_speed;
    sea.mShoreOn = shore_on;

    // Source: lldrawpoolwater.cpp:309 WATER_WAVE_DIR1 = pwater->getWave1Dir() (raw: the
    // shader normalises it for dir1 and uses its LENGTH for the breaker's speedScale).
    sea.mDir1.set(1.04999f, -0.42000f);   // llsettingswater.cpp:107 default, like wolfseastate.h
    {
        LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
        if (pwater)
        {
            sea.mDir1 = pwater->getWave1Dir();
        }
    }

    // [WAVES 2026-09-10] the painted zones' calm ripple and small-wave scale (WolfWaveZones).
    const LLSD& zp = WolfWaveZones::instance().params();
    sea.mCalm = llclamp(zp.has("calmRipple") ? (F32)zp["calmRipple"].asReal() : 0.03f, 0.f, 0.1f);
    sea.mSmall = llclamp(zp.has("smallScale") ? (F32)zp["smallScale"].asReal() : WolfWaveZones::SMALL_SCALE_DEFAULT, 0.05f, 0.8f);

    if (wp)
    {
        sea.mSurfH = wp->getSurfHeight();
        sea.mSurfSet = wp->getSurfSetInterval();
        sea.mSurfLen = wp->getSurfLength();
    }
    return sea;
}

// Source: terrain_manager.js _waveSampleCPU(x, y, t, u) — CPU mirror of the shader's wave
// surface: the three dominant Gerstner swells (same arguments as the gerstnerWave calls,
// here waterV.glsl w1/w2/w3) plus the geometric shore breaker (same phase as the vertex
// stage) and the surf train. localAmp uses the GPU noise field's MEAN (0.5) — the
// per-position simplex variation, the chaos wavelength jitter and the spectral cascades are
// omitted. The distance fade (waveFade 320..1500 m) is 1 inside the 256 m sweep radius.
// <WolfViewer 2026-09-27> HEIGHT ONLY. It used to return a slope too, from analytic
// derivatives, and the breaker's was not the drawn water's: it stood in 0.5 for the seabed
// grade (typically 0.01-0.05), so in a shallow marina a hull rolled up to 45 degrees every
// few seconds (Paul 09-27, a customer's boat at Cape Cod "moving like crazy with the
// waves"). The slope is now measured from this height across the hull (measureHull).
//
// SPACES: waterV.glsl phases the swell on AGENT-space XY (`wxy = position.xy`) so
// neighbouring regions' water joins up, and reads the baked fields in the vertex's own
// REGION space (`regionXY = position.xy - wolfRegionOrigin`, lldrawpoolwater.cpp
// pushWaterPlanes binds each region's own field). The same two spaces here.
F32 WolfBoatRock::waveHeight(const Sea& sea, LLViewerRegion* regionp, F32 ax, F32 ay, F32 t) const
{
    F32 z = 0.f;
    const F32 amp = sea.mAmp;
    F32 rx = ax, ry = ay;   // baked fields: region space
    if (regionp)
    {
        const LLVector3 o = regionp->getOriginAgent();
        rx = ax - o.mV[VX];
        ry = ay - o.mV[VY];
    }
    const WolfWaterField::Field* field = regionp ? WolfWaterField::instance().get(regionp) : nullptr;
    auto exposure_at = [field](F32 x, F32 y) -> F32
    {
        return field ? WolfWaterField::exposureAt(*field, x, y) : 1.f;
    };
    auto smoothstep01 = [](F32 e0, F32 e1, F32 x)
    {
        const F32 u = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return u * u * (3.f - 2.f * u);   // GLSL smoothstep
    };

    // [SWELL-EXPOSURE 2026-08-31] Mirror of the vertex stage's swellScale = mix(0.15, 1.3,
    // exposure) — boats ride a calmer sea in rivers and harbours, a rolling one offshore.
    //
    // [FLOOR 2026-08-31 — MEASURED LIVE at Madrigal marina] ...but with a 0.7 FLOOR that
    // the water surface deliberately does NOT get. Without the floor the exposure scaling
    // silenced the boats at exactly the places boats moor: every marina hull sat at
    // exposure 0-0.23 -> scale ~0.15-0.4 -> peak bob 7-25 MILLIMETRES. Moored boats visibly
    // sway from harbour slop even on water that reads calm, so the ROCKER keeps >= 0.7 of
    // the base wave field while the drawn surface still flattens toward shore and rivers.
    // [WAVES 2026-09-10] ...times the painted zone's swell scale, exactly as the vertex stage
    // (waterV.glsl zoneScale — WolfWaveZones::zoneScale is the one mapping): a boat on an OFF
    // cell sits still (the default everywhere inside a region now), on a small-wave cell it
    // rides smallScale of the open sea, on open / surf cells the full swell.
    const F32 energy = field ? WolfWaterField::zoneAt(*field, rx, ry) : WolfWaveZones::OPEN_ENERGY;
    // <WolfViewer 2026-10-06> ...but never below HARBOUR_SLOP for the ROCKER. Paul at Madrigal marina:
    // "none of the boats appear to be rocking" / "i just dont see a lot of rocking" - the marina is
    // unpainted (OFF, zone 0), so the 09-10 zone rule multiplied every moored hull by 0 and undid the
    // 08-31 floor above. Moored boats sway from harbour slop even where the drawn water is flat;
    // the surface keeps the painted zones. Paul chose the fix 10-06; about a third of the open sea.
    // Mirror: wolfstorm terrain_manager.js _waveSampleCPU, same constant.
    const F32 HARBOUR_SLOP = 0.33f;
    const F32 zone_mul = llmax(WolfWaveZones::zoneScale(energy, amp, sea.mCalm, sea.mSmall), HARBOUR_SLOP);
    const F32 swell_scale = llmax(0.15f + 1.15f * exposure_at(rx, ry), 0.7f) * zone_mul;

    if (amp > 0.01f)
    {
        const F32 base_wl = 1.0f / (sea.mFreq + 0.001f);
        LLVector2 d1 = sea.mDir1;
        if (d1.length() < 1e-4f)
        {
            d1.set(1.04999f, -0.42000f);
        }
        F32 local_amp = amp * 0.5f * swell_scale;
        // <WolfViewer 2026-09-26> Mirror of waterV.glsl: the swell converges on the centre of
        // the region the water belongs to (the hull's region — its own water plane), phased on
        // the position relative to that centre, calmed within 32 m of the region edge and two
        // wavelengths of the centre (lldrawpoolwater.cpp wolfSwellCentre / wolfSwellHalf).
        F32 px = ax, py = ay;
        if (regionp)
        {
            const LLVector3 o = regionp->getOriginAgent();
            const F32 half = regionp->getWidth() * 0.5f;
            const F32 relx = ax - (o.mV[VX] + half), rely = ay - (o.mV[VY] + half);
            const F32 r = sqrtf(relx * relx + rely * rely);
            const F32 qx = fabsf(relx) - half, qy = fabsf(rely) - half;
            const F32 to_edge = (qx < 0.f && qy < 0.f) ? llmin(-qx, -qy)
                                                      : sqrtf(llmax(qx, 0.f) * llmax(qx, 0.f) + llmax(qy, 0.f) * llmax(qy, 0.f));
            local_amp *= smoothstep01(0.f, 32.f, to_edge) * smoothstep01(0.f, 4.f * base_wl, r);
            if (r > 0.5f)
            {
                d1.set(-relx / r, -rely / r);
            }
            px = relx;
            py = rely;
        }
        // Source: waterV.glsl:352-355 dir2/dir3 rotation constants (+35 / -60 degrees).
        const F32 d1x = d1.mV[VX], d1y = d1.mV[VY];
        const LLVector2 dir2(d1x * 0.819f - d1y * 0.574f, d1x * 0.574f + d1y * 0.819f);
        const LLVector2 dir3(d1x * 0.5f + d1y * 0.866f, -d1x * 0.866f + d1y * 0.5f);
        // Source: waterV.glsl gerstnerWave(): k = 2pi/wl, c = sqrt(9.8/k),
        // f = k * (dot(d, pos) - c * time * waveSpeed); z += A sin f.
        auto acc = [&](F32 wl, F32 A, const LLVector2& d)
        {
            const F32 k = 6.28318f / wl;
            const F32 c = sqrtf(9.8f / k);
            F32 len = d.length();
            if (len <= 0.f) len = 1.f;
            const F32 dx = d.mV[VX] / len, dy = d.mV[VY] / len;
            const F32 f = k * (dx * px + dy * py - c * t * sea.mSpeed);
            z += A * sinf(f);
        };
        // Source: waterV.glsl:359-361 w1/w2/w3 — the three dominant trains.
        acc(base_wl * 2.0f, local_amp * 0.40f, d1);
        acc(base_wl * 1.5f, local_amp * 0.30f, dir2);
        acc(base_wl * 1.2f, local_amp * 0.25f, dir3);
    }

    // Geometric shore breaker (waterV.glsl:614-646, Water.js vertex stage formula).
    F32 dtex[4];
    if (field && sea.mShoreOn && WolfWaterField::depthAt(*field, rx, ry, dtex))
    {
        const F32 g = dtex[1], b = dtex[2];
        const F32 conf = sqrtf(g * g + b * b);
        if (conf > 0.02f)
        {
            const F32 smooth_depth = llmax(field->mWaterLevel - dtex[3], 0.f);
            // waterV.glsl edgeFade — faded out over a 4% band INSIDE the field's edge so
            // nothing can pop a hull across the border (the drawn breaker fades the same way).
            // <WolfViewer 2026-09-27> Measured from the field's origin, as the shader's sduv
            // = (regionXY - depthOrigin) / depthRegionSize: on a region wider than 2048 m the
            // field is a camera window and rx / mSizeX was the wrong fraction of it.
            const F32 su = (rx - field->mX0) / field->mSizeX, sv = (ry - field->mY0) / field->mSizeY;
            const F32 edge_fade = smoothstep01(0.f, 0.04f, su) * (1.f - smoothstep01(0.96f, 1.f, su))
                                * smoothstep01(0.f, 0.04f, sv) * (1.f - smoothstep01(0.96f, 1.f, sv));
            const F32 shoal = (1.f - smoothstep01(0.5f, 8.0f, smooth_depth)) * llmin(conf * 1.5f, 1.f) * edge_fade;
            if (shoal > 0.01f)
            {
                const F32 speed_scale = llclamp(sea.mDir1.length() * 0.885f, 0.4f, 2.0f);
                const F32 phase = t * 4.5f * speed_scale + sqrtf(smooth_depth) * 8.0f;
                // [SWELL-EXPOSURE 2026-08-31] Mirror of the vertex stage's breaker feed:
                // exposure tapped ~45m seaward (-(g,b)/conf is the seaward unit vector),
                // sqrt-lifted, 0.35..1.0.
                // [FLOOR 2026-08-31] Same 0.7 rocker floor as the swell term above, for the
                // same reason: at a marina the BREAKER is the dominant part of a moored
                // hull's bob. The drawn water keeps the un-floored feed.
                const F32 feed = exposure_at(rx - (g / conf) * 45.f, ry - (b / conf) * 45.f);
                const F32 b_amp = llmin(0.10f + amp * 0.9f, 0.5f) * shoal
                                * llmax(0.35f + 0.65f * sqrtf(feed), 0.7f);
                const F32 br = sinf(phase);
                z += (br + 0.35f * br * br) * b_amp;
            }
        }
    }

    // [SURF 2026-09-07] The SURF TRAIN (waterV.glsl [SURF rev2]; terrain_manager.js
    // _surfSampleCPU, same numbers): the crest-referenced profile's height, so a boat in a
    // surf cell rides the wall the water draws. Horizontal push omitted.
    if (field && sea.mShoreOn && sea.mSurfH > 0.01f && field->mZoneTex)
    {
        const F32 surf_h = sea.mSurfH, surf_set = sea.mSurfSet, surf_len = sea.mSurfLen;
        // surf cells only (waterV.glsl surfZone). <WolfViewer 2026-10-01/> from the surf WEIGHT, as the shader now reads it
        const F32 surf_zone = smoothstep01(0.62f, 0.95f, 0.55f + 0.45f * WolfWaterField::surfWeightAt(*field, rx, ry));
        if (surf_zone > 0.001f)
        {
            const F32 dW = 80.f, dH = 80.f;   // <WolfViewer 2026-09-20/> waterV.glsl: 80 m either side, not 1/32 of the span
            // <WolfViewer 2026-09-20> travel = UP the distance-from-the-open-sea gradient (landward),
            // the old distance-to-land rule only where no open sea is in reach (waterV.glsl same).
            const F32 d_open = WolfWaterField::openDistanceAt(*field, rx, ry);
            const F32 ox = WolfWaterField::openDistanceAt(*field, rx + dW, ry) - WolfWaterField::openDistanceAt(*field, rx - dW, ry);
            const F32 oy = WolfWaterField::openDistanceAt(*field, rx, ry + dH) - WolfWaterField::openDistanceAt(*field, rx, ry - dH);
            const F32 ol = sqrtf(ox * ox + oy * oy);
            const F32 dist = WolfWaterField::distanceAt(*field, rx, ry);
            const F32 gx = WolfWaterField::distanceAt(*field, rx + dW, ry) - WolfWaterField::distanceAt(*field, rx - dW, ry);
            const F32 gy = WolfWaterField::distanceAt(*field, rx, ry + dH) - WolfWaterField::distanceAt(*field, rx, ry - dH);
            const F32 gl = sqrtf(gx * gx + gy * gy);
            // <WolfViewer 2026-09-21> The baked OPTICAL PATH and its gradient — the ray
            // direction and the wave phase, exactly as waterV.glsl reads them. A boat has to
            // rock on the sea that is drawn, so this block mirrors the shader term for term;
            // the shader's LATTICE BAND LIMIT is the one thing deliberately not mirrored,
            // because that is a drawing limit and the water really is under the hull.
            const F32 d_path = WolfWaterField::openPathAt(*field, rx, ry);
            const F32 px = WolfWaterField::openPathAt(*field, rx + dW, ry) - WolfWaterField::openPathAt(*field, rx - dW, ry);
            const F32 py = WolfWaterField::openPathAt(*field, rx, ry + dH) - WolfWaterField::openPathAt(*field, rx, ry - dH);
            const F32 pl = sqrtf(px * px + py * py);
            const bool have_open = d_open < 3000.f && ol > 1.f;
            const bool have_path = field->mSurfK0 > 0.f && d_open < 3000.f && pl > 1.f;
            const bool have_land = dist < 3000.f && gl > 1.f;
            F32 dx, dy, coord;
            if (have_path) { dx = px / pl; dy = py / pl; coord = d_open; }
            else if (have_open) { dx = ox / ol; dy = oy / ol; coord = d_open; }
            else if (have_land) { dx = -gx / gl; dy = -gy / gl; coord = -dist; }
            else
            {
                // <WolfViewer 2026-09-27> toward the FIELD's centre, waterV.glsl
                // normalize(depthOrigin + depthRegionSize * 0.5 - regionXY + vec2(0.001, 0.0)).
                const F32 cx = field->mX0 + field->mSizeX * 0.5f - rx + 0.001f, cy = field->mY0 + field->mSizeY * 0.5f - ry;
                F32 cl = sqrtf(cx * cx + cy * cy); if (cl <= 0.f) cl = 1.f;
                dx = cx / cl; dy = cy / cl; coord = rx * dx + ry * dy;
            }
            const F32 path = have_path ? d_path : coord;
            // depth: the smoothed height, continuing past the field's border and easing to 30 m
            // <WolfViewer 2026-09-27> the border is the FIELD's, as waterV.glsl sduv/over:
            // on a windowed field it is not (0, 0)-(mSizeX, mSizeY) in region space.
            F32 h = 30.f;
            F32 dt4[4];
            const F32 fx0 = field->mX0, fy0 = field->mY0;
            const F32 fx1 = fx0 + field->mSizeX, fy1 = fy0 + field->mSizeY;
            const F32 qx = llclamp(rx, fx0, fx1), qy = llclamp(ry, fy0, fy1);
            if (WolfWaterField::depthAt(*field, qx, qy, dt4))
            {
                const F32 over = llmax(llmax(fx0 - rx, rx - fx1), llmax(fy0 - ry, ry - fy1), 0.f);
                const F32 outside = smoothstep01(0.f, 64.f, over);
                const F32 h_in = llmax(field->mWaterLevel - dt4[3], 0.f);
                h = h_in + (30.f - h_in) * outside;
            }
            h = WolfWaterField::surfDepth(h, dist);   // <WolfViewer 2026-10-01/> real or reef depth, waterV.glsl same
            const F32 g9 = 9.81f;
            const F32 lambda = llmax(surf_len, 12.f * surf_h);
            // <WolfViewer 2026-09-21/> k0 deep-water, omega0 CONSTANT, k local by Guo (2002)
            const F32 k0 = field->mSurfK0 > 0.f ? field->mSurfK0 : 6.2831853f / llmax(lambda, 8.f);
            const F32 omega0 = sqrtf(g9 * k0);
            const F32 k = WolfWaterField::surfWavenumber(k0, h);
            const F32 set_ph = 6.2831853f * (t / llmax(surf_set, 10.f)) - path * (0.22f / lambda);
            const F32 set_env = 0.30f + 0.70f * smoothstep01(0.15f, 1.f, 0.5f + 0.5f * sinf(set_ph));
            const F32 crest_var = 0.85f + 0.15f * sinf((rx * -dy + ry * dx) * (1.1f / lambda) + t * 0.1f);
            const F32 ksh = llclamp(WolfWaterField::surfShoalGain(k0, k, h), 1.f, 1.8f);   // <WolfViewer 2026-10-01/> floor 1, waterV.glsl same
            F32 crest_h = llmin(surf_h * surf_zone * set_env * ksh * crest_var, surf_h * 1.15f);
            const F32 h_max = 0.78f * h;   // <WolfViewer 2026-10-01/> McCowan on the surf depth (real or reef), waterV.glsl same
            const F32 break_f = smoothstep01(0.7f, 1.15f, crest_h / llmax(h_max, 0.01f));
            crest_h = llmin(crest_h, h_max);
            crest_h *= smoothstep01(0.2f, 0.6f + 0.5f * surf_h, h);
            if (crest_h > 0.01f)
            {
                const F32 ph = k0 * path - omega0 * t;
                const F32 ph2 = ph + (0.30f + 0.45f * break_f) * sinf(ph);
                const F32 sn = sinf(ph2);
                const F32 up = 0.5f + 0.5f * sn;
                const F32 upk = powf(up, 1.6f + 1.2f * break_f);   // [SURF rev3] peaked crest
                const F32 prof = -0.25f + 1.25f * upk + 0.25f * break_f * upk * upk;
                const F32 tip = upk * upk * upk;
                const F32 lip = 0.55f * break_f * tip;
                z += crest_h * (prof - 0.35f * lip);
            }
        }
    }
    return z;
}

// <WolfViewer 2026-09-27> Paul: "it should measure the wave height at that point for that
// boat". The water's height is measured under the hull's middle and at its four ends — bow,
// stern, port, starboard, along the root prim's own axes over the whole linkset's footprint
// (hullExtents) — and the hull takes the mean as its heave and the end-to-end differences
// as its slope. A wave shorter than the hull lifts one end while it drops the other and so
// barely moves it, which is why a ferry rides steadier than a dinghy.
WolfBoatRock::Sample WolfBoatRock::measureHull(const Rocker& r, const LLViewerObject* objectp, const Sea& sea, F32 t) const
{
    Sample out;
    LLViewerRegion* regionp = objectp->getRegion();
    const LLVector3 pa = objectp->getPositionAgent();
    // Region and agent space share their axes, so the region rotation orients agent XY.
    const LLQuaternion rot = objectp->getRotationRegion();
    const LLVector3 axis_x = LLVector3::x_axis * rot;   // bow-stern
    const LLVector3 axis_y = LLVector3::y_axis * rot;   // beam
    const F32 mx = 0.5f * (r.mMin.mV[VX] + r.mMax.mV[VX]);
    const F32 my = 0.5f * (r.mMin.mV[VY] + r.mMax.mV[VY]);
    auto height = [&](F32 lx, F32 ly)
    {
        const LLVector3 p = pa + axis_x * lx + axis_y * ly;
        return waveHeight(sea, regionp, p.mV[VX], p.mV[VY], t);
    };
    const F32 z_mid   = height(mx, my);
    const F32 z_bow   = height(r.mMax.mV[VX], my);
    const F32 z_stern = height(r.mMin.mV[VX], my);
    const F32 z_port  = height(mx, r.mMax.mV[VY]);
    const F32 z_stbd  = height(mx, r.mMin.mV[VY]);
    out.mZ = (z_mid + z_bow + z_stern + z_port + z_stbd) * 0.2f;

    // The slopes along the two measured lines, then the surface gradient that has them:
    // g . u = s_u and g . v = s_v, u and v the lines' horizontal directions.
    const LLVector2 u(axis_x.mV[VX], axis_x.mV[VY]);
    const LLVector2 v(axis_y.mV[VX], axis_y.mV[VY]);
    const F32 lu = u.length(), lv = v.length();
    const F32 len_u = (r.mMax.mV[VX] - r.mMin.mV[VX]) * lu;   // horizontal bow-stern distance
    const F32 len_v = (r.mMax.mV[VY] - r.mMin.mV[VY]) * lv;
    if (len_u < 0.05f || len_v < 0.05f)
    {
        return out;   // the hull stands on end: no horizontal line to measure along, no tilt
    }
    const F32 s_u = (z_bow - z_stern) / len_u;
    const F32 s_v = (z_port - z_stbd) / len_v;
    const F32 ux = u.mV[VX] / lu, uy = u.mV[VY] / lu;
    const F32 vx = v.mV[VX] / lv, vy = v.mV[VY] / lv;
    const F32 det = ux * vy - uy * vx;
    if (fabsf(det) < 0.1f)
    {
        return out;   // the two lines are near parallel seen from above: no gradient from them
    }
    out.mSx = (s_u * vy - s_v * uy) / det;
    out.mSy = (s_v * ux - s_u * vx) / det;
    return out;
}

// <WolfViewer 2026-10-06> MOORING SWAY. Measured at Madrigal marina with the "moves:" debug
// line: the region's water is the calmest sea state (WolfSeaState MIN_INDEX 0.15 -> swell
// 0.22 * 0.15^1.5 = 1.3 cm), so after the harbour scaling the 35 rocking hulls moved 0.1-3 mm
// and tilted under 0.1 degree — rocking that nobody can see (Paul: "hardly any of my boats are
// rocking"). A moored hull is never that still: wind on the topsides and the wakes of passing
// boats roll and pitch it even on glassy water. This is that motion, independent of the sea
// state, added to the wave-following one: roll about the keel, pitch about the beam and a
// little heave, each a slow sine with a second, longer one beating against it so the motion
// never looks mechanical. Every hull's periods and phases come from its UUID, so neighbours
// never move in step. Scaled down for long hulls (a 6 m dinghy gets the full roll, a 24 m
// yacht a third) and by the rocker gain like the waves.
// Mirror: wolfstorm js/world/terrain/terrain_manager.js _mooringSway, same constants.
WolfBoatRock::Sample WolfBoatRock::mooringSway(const Rocker& r, const LLViewerObject* objectp, F32 t) const
{
    static const F32 ROLL_DEG = 2.5f;     // full-size roll, either side
    static const F32 PITCH_DEG = 0.9f;
    static const F32 HEAVE_M = 0.05f;
    static const F32 REF_LENGTH_M = 8.f;  // hulls up to this long sway fully

    Sample out;
    const LLUUID& id = objectp->getID();
    // Four bytes of the UUID per figure, 0..1.
    auto h = [&id](S32 i)
    {
        const U8* b = id.mData + (i * 4) % 16;
        return (F32)((b[0] << 8) | b[1]) / 65535.f;
    };
    const F32 TWO_PI = 6.28318f;
    const F32 roll_period = 5.0f + 2.0f * h(0);
    const F32 pitch_period = 3.8f + 1.4f * h(1);
    const F32 heave_period = 4.4f + 1.6f * h(2);
    const F32 ph = TWO_PI * h(3);

    const F32 length = llmax(r.mMax.mV[VX] - r.mMin.mV[VX], 0.5f);
    const F32 size = llclamp(REF_LENGTH_M / length, 0.35f, 1.f);

    const F32 roll = ROLL_DEG * DEG_TO_RAD * size
                   * (sinf(TWO_PI * t / roll_period + ph) + 0.35f * sinf(TWO_PI * t / (roll_period * 2.7f) + ph * 1.7f));
    const F32 pitch = PITCH_DEG * DEG_TO_RAD * size
                    * (sinf(TWO_PI * t / pitch_period + ph * 2.3f) + 0.3f * sinf(TWO_PI * t / (pitch_period * 3.1f) + ph * 0.6f));
    out.mZ = HEAVE_M * size * sinf(TWO_PI * t / heave_period + ph * 3.1f);

    // The slope a tilted plane has along the hull's own horizontal axes, as a world gradient
    // (measureHull's convention: n = normalize(-sx, -sy, 1)). Bow-stern carries the pitch,
    // the beam the roll.
    const LLQuaternion rot = objectp->getRotationRegion();
    LLVector2 u((LLVector3::x_axis * rot).mV[VX], (LLVector3::x_axis * rot).mV[VY]);
    LLVector2 v((LLVector3::y_axis * rot).mV[VX], (LLVector3::y_axis * rot).mV[VY]);
    if (u.length() < 0.1f || v.length() < 0.1f)
    {
        return out;   // standing on end: heave only
    }
    u.normalize();
    v.normalize();
    const F32 s_u = tanf(pitch);
    const F32 s_v = tanf(roll);
    out.mSx = s_u * u.mV[VX] + s_v * v.mV[VX];
    out.mSy = s_u * u.mV[VY] + s_v * v.mV[VY];
    return out;
}

// <WolfViewer 2026-09-27> The linkset's footprint in the root prim's own frame: the root's
// box and every linked prim's box corners (a child's position and rotation are relative to
// the root, LLXform). Seated avatars are children too and are not hull.
// <WolfViewer 2026-10-01> The linkset's vertical span in REGION metres: the eight corners of
// the root prim and of every linked prim (avatars excluded, as hullExtents), through the
// child's own rotation and offset into the root's frame, then the root's rotation and region
// position. Source: terrain_manager.js _hullVerticalSpan(), same corners.
void WolfBoatRock::hullVerticalSpan(const LLViewerObject* root, F32& zlo, F32& zhi)
{
    const LLVector3 rp = root->getPositionRegion();
    const LLQuaternion rr = root->getRotation();
    zlo = rp.mV[VZ];
    zhi = rp.mV[VZ];
    auto add_box = [&](const LLVector3& scale, const LLQuaternion& rot, const LLVector3& off)
    {
        const LLVector3 half = scale * 0.5f;
        for (S32 c = 0; c < 8; ++c)
        {
            const LLVector3 corner((c & 1) ? half.mV[VX] : -half.mV[VX],
                                   (c & 2) ? half.mV[VY] : -half.mV[VY],
                                   (c & 4) ? half.mV[VZ] : -half.mV[VZ]);
            const F32 z = ((corner * rot + off) * rr).mV[VZ] + rp.mV[VZ];
            zlo = llmin(zlo, z);
            zhi = llmax(zhi, z);
        }
    };
    add_box(root->getScale(), LLQuaternion(), LLVector3::zero);
    LLViewerObject::const_child_list_t& kids = root->getChildren();
    for (LLViewerObject::child_list_t::const_iterator ki = kids.begin(); ki != kids.end(); ++ki)
    {
        const LLViewerObject* kid = *ki;
        if (!kid || kid->isAvatar())
        {
            continue;
        }
        add_box(kid->getScale(), kid->getRotation(), kid->getPosition());
    }
}

void WolfBoatRock::hullExtents(const LLViewerObject* root, LLVector2& lo, LLVector2& hi)
{
    const LLVector3& rs = root->getScale();
    lo.set(-0.5f * rs.mV[VX], -0.5f * rs.mV[VY]);
    hi.set(0.5f * rs.mV[VX], 0.5f * rs.mV[VY]);
    LLViewerObject::const_child_list_t& kids = root->getChildren();
    for (LLViewerObject::child_list_t::const_iterator ki = kids.begin(); ki != kids.end(); ++ki)
    {
        const LLViewerObject* kid = *ki;
        if (!kid || kid->isAvatar())
        {
            continue;
        }
        const LLVector3 half = kid->getScale() * 0.5f;
        const LLVector3 cp = kid->getPosition();
        const LLQuaternion cr = kid->getRotation();
        for (S32 c = 0; c < 8; ++c)
        {
            const LLVector3 corner((c & 1) ? half.mV[VX] : -half.mV[VX],
                                   (c & 2) ? half.mV[VY] : -half.mV[VY],
                                   (c & 4) ? half.mV[VZ] : -half.mV[VZ]);
            const LLVector3 q = corner * cr + cp;
            lo.set(llmin(lo.mV[VX], q.mV[VX]), llmin(lo.mV[VY], q.mV[VY]));
            hi.set(llmax(hi.mV[VX], q.mV[VX]), llmax(hi.mV[VY], q.mV[VY]));
        }
    }
    // A sliver of a root with nothing linked still gets a measurable line.
    if (hi.mV[VX] - lo.mV[VX] < 0.5f) { const F32 c = 0.5f * (hi.mV[VX] + lo.mV[VX]); lo.mV[VX] = c - 0.25f; hi.mV[VX] = c + 0.25f; }
    if (hi.mV[VY] - lo.mV[VY] < 0.5f) { const F32 c = 0.5f * (hi.mV[VY] + lo.mV[VY]); lo.mV[VY] = c - 0.25f; hi.mV[VY] = c + 0.25f; }
}
