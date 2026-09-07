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
    const std::regex NOT_BOAT_RE("dock|boat ?house", std::regex::ECMAScript | std::regex::icase);
    const std::regex BOAT_RE("boat|jet ?ski|yacht|dinghy|dinghies|canoe|kayak|catamaran|gondola|\\bships?\\b|\\bsail(?:ing|s|boats?)?\\b",
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
                LL_DEBUGS("WolfBoatRock") << "still " << objectp->getID() << ": " << verdict.mWhy << LL_ENDL;
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
        }
        else
        {
            Rocker r;
            r.mObject = c.mObject;
            r.mGain = c.mVerdict.mGain;
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

        const Sample w = sampleWave(objectp, t);
        // gain: 1.0 physical/crewed, 0.6 parked floaters (classify())
        const F32 g = (r.mGain > 0.f) ? r.mGain : 1.f;
        r.mBob += (w.mZ * g - r.mBob) * bob_alpha;
        r.mSx += (w.mSx * g - r.mSx) * slope_alpha;
        r.mSy += (w.mSy * g - r.mSy) * slope_alpha;

        // Big vessels ride steadier than dinghies.
        const LLVector3& scale = objectp->getScale();
        const F32 radius = llmax(scale.mV[VX] > 0.f ? scale.mV[VX] : 1.f,
                                 scale.mV[VY] > 0.f ? scale.mV[VY] : 1.f);
        const F32 tilt_scale = llmin(1.f, 6.f / llmax(radius, 1.f));

        // n = normalize(-sx * tiltScale, -sy * tiltScale, 1); tiltQ = up -> n.
        LLVector3 n(-r.mSx * tilt_scale, -r.mSy * tilt_scale, 1.f);
        n.normalize();
        r.mTargetBob = r.mBob;
        r.mTargetTilt.shortestArc(up, n);

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
//  - parked/static floaters    -> gentle rock (gain 0.6), EXCEPT wide thin PLATES
//    (pontoons, docks, swim platforms: max(x,y) > 5m and >8x wider than tall) and anything
//    whose bottom touches the seabed (pilings, ramps) — those stay still — and only when
//    NAMED or DESCRIBED as a boat ([ROCK-NAME 2026-08-29]).
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
    if (p.mV[VZ] < wh - 3.0f || p.mV[VZ] > wh + 2.5f)
    {
        return no("outside waterline band");
    }
    v.mInBand = true;
    const LLSurface& land = regionp->getLand();
    // Source: wolfwaterfield.cpp idle() — the land is not there until the grid is.
    if (land.getGridsPerEdge() < 4)
    {
        return no("terrain not loaded");
    }
    const F32 width = regionp->getWidth();
    if (p.mV[VX] < 0.f || p.mV[VX] > width || p.mV[VY] < 0.f || p.mV[VY] > width)
    {
        return no("outside this region (terrain unknown)");
    }
    const F32 th = land.resolveHeightRegion(p.mV[VX], p.mV[VY]);
    if (th > wh - 0.4f)
    {
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
    if (physical || crewed)
    {
        v.mRock = true;
        v.mGain = 1.0f;
        v.mWhy = physical ? "physical" : "crewed";
        return v;
    }
    // Parked/static floater: pontoon-vs-hull shape gate.
    const LLVector3& scale = objectp->getScale();
    const F32 sx = scale.mV[VX] > 0.f ? scale.mV[VX] : 1.f;
    const F32 sy = scale.mV[VY] > 0.f ? scale.mV[VY] : 1.f;
    const F32 sz = scale.mV[VZ] > 0.f ? scale.mV[VZ] : 1.f;
    const F32 wide = llmax(sx, sy);
    if (wide > 5.f && wide / llmax(sz, 0.01f) > 8.f)
    {
        return no("flat plate (pontoon/dock)");
    }
    if (p.mV[VZ] - sz * 0.5f < th + 0.15f)
    {
        return no("bottom on seabed (anchored)");
    }
    if (nv == NAME_UNKNOWN)
    {
        v.mRock = false;
        v.mGain = 0.f;
        v.mWantName = true;
        v.mWhy = "static floater, name not fetched yet";
        return v;
    }
    if (nv != NAME_BOAT)
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
    if (std::regex_search(name, BOAT_RE) || std::regex_search(desc, BOAT_RE))
    {
        return NAME_BOAT;
    }
    return NAME_NOT_BOAT;
}

// Source: terrain_manager.js _waveSampleCPU(x, y, t, u) — CPU mirror of the shader's wave
// surface at the hull: the three dominant Gerstner swells (same arguments as the
// gerstnerWave/gerstnerSlope calls, here waterV.glsl w1/w2/w3) plus the geometric shore
// breaker (same phase as the vertex stage). localAmp uses the GPU noise field's MEAN (0.5)
// — the per-position simplex variation, the chaos wavelength jitter and the spectral
// cascades are omitted; hulls cannot visibly disagree with the surface over their own
// footprint. The distance fade (waveFade 320..1500 m) is 1 inside the 256 m sweep radius.
// Returns height offset and surface slope.
//
// SPACES: waterV.glsl phases the swell on AGENT-space XY (`wxy = position.xy`) so
// neighbouring regions' water joins up, and reads the baked fields in the vertex's own
// REGION space (`regionXY = position.xy - wolfRegionOrigin`, lldrawpoolwater.cpp
// pushWaterPlanes binds each region's own field). The same two spaces here.
WolfBoatRock::Sample WolfBoatRock::sampleWave(const LLViewerObject* objectp, F32 t) const
{
    Sample out;

    // The sea state the water was drawn with (lldrawpoolwater.cpp renderPostDeferred sets
    // it every frame from the region's EEP water, or the manual height when pinned).
    WolfSeaState st;
    if (LLDrawPool* poolp = gPipeline.findPool(LLDrawPool::POOL_WATER))
    {
        st = static_cast<LLDrawPoolWater*>(poolp)->getSeaState();
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
    const F32 amp = st.mAmplitude;
    const F32 freq = sea_from_region ? st.mFrequency : llmax(0.001f, (F32)wave_scale);
    const F32 speed = (F32)wave_speed;

    // Source: lldrawpoolwater.cpp:309 WATER_WAVE_DIR1 = pwater->getWave1Dir() (raw: the
    // shader normalises it for dir1 and uses its LENGTH for the breaker's speedScale).
    LLVector2 wd1(1.04999f, -0.42000f);   // llsettingswater.cpp:107 default, like wolfseastate.h
    {
        LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
        if (pwater)
        {
            wd1 = pwater->getWave1Dir();
        }
    }

    LLViewerRegion* regionp = objectp->getRegion();
    const LLVector3 pa = objectp->getPositionAgent();
    const LLVector3 pr = objectp->getPositionRegion();
    const F32 ax = pa.mV[VX], ay = pa.mV[VY];   // swell phase: agent space
    const F32 rx = pr.mV[VX], ry = pr.mV[VY];   // baked fields: region space
    const WolfWaterField::Field* field = regionp ? WolfWaterField::instance().get(regionp) : nullptr;
    auto exposure_at = [field](F32 x, F32 y) -> F32
    {
        return field ? WolfWaterField::exposureAt(*field, x, y) : 1.f;
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
    // [WAVES 2026-09-07] ...times the painted zone, exactly as the vertex stage
    // (waterV.glsl zoneEnergy): a boat in a surf cell rides the bigger swell.
    const F32 zone_mul = 0.1f + 1.5f * (field ? WolfWaterField::zoneAt(*field, rx, ry) : WolfWaveZones::OPEN_ENERGY);
    const F32 swell_scale = llmax(0.15f + 1.15f * exposure_at(rx, ry), 0.7f) * zone_mul;

    if (amp > 0.01f)
    {
        const F32 base_wl = 1.0f / (freq + 0.001f);
        LLVector2 d1 = wd1;
        if (d1.length() < 1e-4f)
        {
            d1.set(1.04999f, -0.42000f);
        }
        const F32 local_amp = amp * 0.5f * swell_scale;
        // Source: waterV.glsl:352-355 dir2/dir3 rotation constants (+35 / -60 degrees).
        const F32 d1x = d1.mV[VX], d1y = d1.mV[VY];
        const LLVector2 dir2(d1x * 0.819f - d1y * 0.574f, d1x * 0.574f + d1y * 0.819f);
        const LLVector2 dir3(d1x * 0.5f + d1y * 0.866f, -d1x * 0.866f + d1y * 0.5f);
        // Source: waterV.glsl gerstnerWave()/gerstnerSlope(): k = 2pi/wl, c = sqrt(9.8/k),
        // f = k * (dot(d, pos) - c * time * waveSpeed); z += A sin f; slope = d * A k cos f.
        auto acc = [&](F32 wl, F32 A, const LLVector2& d)
        {
            const F32 k = 6.28318f / wl;
            const F32 c = sqrtf(9.8f / k);
            F32 len = d.length();
            if (len <= 0.f) len = 1.f;
            const F32 dx = d.mV[VX] / len, dy = d.mV[VY] / len;
            const F32 f = k * (dx * ax + dy * ay - c * t * speed);
            out.mZ += A * sinf(f);
            const F32 s = A * k * cosf(f);
            out.mSx += dx * s;
            out.mSy += dy * s;
        };
        // Source: waterV.glsl:359-361 w1/w2/w3 — the three dominant trains.
        acc(base_wl * 2.0f, local_amp * 0.40f, d1);
        acc(base_wl * 1.5f, local_amp * 0.30f, dir2);
        acc(base_wl * 1.2f, local_amp * 0.25f, dir3);
    }

    // Geometric shore breaker (waterV.glsl:421-454, Water.js vertex stage formula).
    F32 dtex[4];
    if (field && shore_on && WolfWaterField::depthAt(*field, rx, ry, dtex))
    {
        const F32 g = dtex[1], b = dtex[2];
        const F32 conf = sqrtf(g * g + b * b);
        if (conf > 0.02f)
        {
            const F32 smooth_depth = llmax(field->mWaterLevel - dtex[3], 0.f);
            // smoothstep(0.5, 8.0, smoothDepth)
            const F32 sst = llclamp((smooth_depth - 0.5f) / 7.5f, 0.f, 1.f);
            // waterV.glsl:426-428 edgeFade — faded out over a 4% band INSIDE the region
            // edge so nothing can pop a hull across the border (the drawn breaker fades
            // the same way; WolfStorm's single-region mirror has no border to fade at).
            auto smoothstep01 = [](F32 e0, F32 e1, F32 x)
            {
                const F32 u = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
                return u * u * (3.f - 2.f * u);
            };
            const F32 su = rx / field->mSizeX, sv = ry / field->mSizeY;
            const F32 edge_fade = smoothstep01(0.f, 0.04f, su) * (1.f - smoothstep01(0.96f, 1.f, su))
                                * smoothstep01(0.f, 0.04f, sv) * (1.f - smoothstep01(0.96f, 1.f, sv));
            const F32 shoal = (1.f - sst * sst * (3.f - 2.f * sst)) * llmin(conf * 1.5f, 1.f) * edge_fade;
            if (shoal > 0.01f)
            {
                const F32 speed_scale = llclamp(wd1.length() * 0.885f, 0.4f, 2.0f);
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
                out.mZ += (br + 0.35f * br * br) * b_amp;
                // Slope along the travel direction — d/ds of sin(8*sqrt(d)) steepens as
                // depth shrinks, capped.
                const F32 sl = cosf(phase) * b_amp
                             * llmin(4.f / llmax(sqrtf(smooth_depth), 0.5f), 4.f) * 0.5f;
                out.mSx += (g / conf) * sl;
                out.mSy += (b / conf) * sl;
            }
        }
    }

    // [SURF 2026-09-07] The SURF TRAIN (waterV.glsl [SURF rev2]; terrain_manager.js
    // _surfSampleCPU, same numbers): the crest-referenced profile's height and slope, so a
    // boat in a surf cell rides the wall the water draws. Horizontal push omitted.
    F32 surf_h = 0.f, surf_set = 90.f, surf_len = 36.f;
    if (LLDrawPool* poolp = gPipeline.findPool(LLDrawPool::POOL_WATER))
    {
        LLDrawPoolWater* wp = static_cast<LLDrawPoolWater*>(poolp);
        surf_h = wp->getSurfHeight();
        surf_set = wp->getSurfSetInterval();
        surf_len = wp->getSurfLength();
    }
    if (field && shore_on && surf_h > 0.01f && field->mZoneTex)
    {
        auto smoothstep01 = [](F32 e0, F32 e1, F32 x)
        {
            const F32 u = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
            return u * u * (3.f - 2.f * u);
        };
        const F32 surf_zone = smoothstep01(0.62f, 0.95f, WolfWaterField::zoneAt(*field, rx, ry));   // surf cells only (waterV.glsl surfZone)
        if (surf_zone > 0.001f)
        {
            const F32 dW = field->mExpoSX / 32.f, dH = field->mExpoSY / 32.f;
            const F32 dist = WolfWaterField::distanceAt(*field, rx, ry);
            const F32 gx = WolfWaterField::distanceAt(*field, rx + dW, ry) - WolfWaterField::distanceAt(*field, rx - dW, ry);
            const F32 gy = WolfWaterField::distanceAt(*field, rx, ry + dH) - WolfWaterField::distanceAt(*field, rx, ry - dH);
            const F32 gl = sqrtf(gx * gx + gy * gy);
            const bool have_land = dist < 3000.f && gl > 1.f;
            F32 dx, dy, coord;
            if (have_land) { dx = -gx / gl; dy = -gy / gl; coord = -dist; }
            else
            {
                const F32 cx = field->mSizeX * 0.5f - rx + 0.001f, cy = field->mSizeY * 0.5f - ry;
                F32 cl = sqrtf(cx * cx + cy * cy); if (cl <= 0.f) cl = 1.f;
                dx = cx / cl; dy = cy / cl; coord = rx * dx + ry * dy;
            }
            // depth: the smoothed height, continuing past the border and easing to 30 m
            F32 h = 30.f;
            F32 dt4[4];
            const F32 qx = llclamp(rx, 0.f, field->mSizeX), qy = llclamp(ry, 0.f, field->mSizeY);
            if (WolfWaterField::depthAt(*field, qx, qy, dt4))
            {
                const F32 over = llmax(llmax(-rx, rx - field->mSizeX), llmax(-ry, ry - field->mSizeY), 0.f);
                const F32 outside = smoothstep01(0.f, 64.f, over);
                const F32 h_in = llmax(field->mWaterLevel - dt4[3], 0.f);
                h = h_in + (30.f - h_in) * outside;
            }
            const F32 g9 = 9.81f;
            const F32 lambda = llmax(surf_len, 12.f * surf_h);
            const F32 k = 6.2831853f / llmax(lambda, 8.f);
            const F32 tk = tanhf(k * llmax(h, 0.05f));
            const F32 omega = sqrtf(g9 * k * tk);
            const F32 set_ph = 6.2831853f * (t / llmax(surf_set, 10.f)) - coord * (0.22f / lambda);
            const F32 set_env = 0.30f + 0.70f * smoothstep01(0.15f, 1.f, 0.5f + 0.5f * sinf(set_ph));
            const F32 crest_var = 0.85f + 0.15f * sinf((rx * -dy + ry * dx) * (1.1f / lambda) + t * 0.1f);
            const F32 ksh = llclamp(1.f / sqrtf(llmax(tk, 0.05f)), 1.f, 1.8f);
            F32 crest_h = llmin(surf_h * surf_zone * set_env * ksh * crest_var, surf_h * 1.15f);
            const F32 h_max = 0.78f * (h + 0.8f * surf_h);
            const F32 break_f = smoothstep01(0.7f, 1.15f, crest_h / llmax(h_max, 0.01f));
            crest_h = llmin(crest_h, h_max);
            crest_h *= smoothstep01(0.2f, 0.6f + 0.5f * surf_h, h);
            if (crest_h > 0.01f)
            {
                const F32 kl = k / sqrtf(llmax(tk, 0.05f));
                const F32 ph = kl * coord - omega * t;
                const F32 ph2 = ph + (0.30f + 0.45f * break_f) * sinf(ph);
                const F32 sn = sinf(ph2), cs = cosf(ph2);
                const F32 up = 0.5f + 0.5f * sn;
                const F32 upk = powf(up, 1.6f + 1.2f * break_f);   // [SURF rev3] peaked crest
                const F32 prof = -0.25f + 1.25f * upk + 0.25f * break_f * upk * upk;
                const F32 tip = upk * upk * upk;
                const F32 lip = 0.55f * break_f * tip;
                out.mZ += crest_h * (prof - 0.35f * lip);
                const F32 sl2 = crest_h * 0.6f * kl * cs * (0.3f + 0.7f * upk);
                out.mSx += dx * sl2;
                out.mSy += dy * sl2;
            }
        }
    }
    return out;
}
