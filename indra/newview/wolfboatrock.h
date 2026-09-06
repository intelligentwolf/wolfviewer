/**
 * @file wolfboatrock.h
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

#ifndef WOLF_BOATROCK_H
#define WOLF_BOATROCK_H

#include <map>

#include "llpointer.h"
#include "llquaternion.h"
#include "llsingleton.h"
#include "v3math.h"

class LLViewerObject;

// Source: wolfstorm/js/world/terrain/terrain_manager.js [ROCK 2026-08-15] updateFloaters(),
// _classifyFloater(), _boatNameVerdict(), _restoreFloater(), _waveSampleCPU(),
// _swellExposureAt() — the same sweep, the same gates, the same numbers, kept in step.
//
// "Make the boats rock — anything basically sitting on the water, client side only."
//
// A 2.5 s sweep tags candidate floaters: ROOT prims near the camera whose position sits
// in the waterline band, over real water (terrain below the surface), and NOT
// bottom-anchored — a dock piling reaches the seabed, a hull does not. Every frame the
// floaters ride the SAME wave surface the shader draws (sampleWave() mirrors waterV.glsl's
// three dominant Gerstner trains and its geometric shore breaker), low-pass filtered so
// hulls read as having inertia. Big vessels tilt less than dinghies.
//
// HOW THE OFFSET IS APPLIED. WolfStorm writes the rocked transform into obj.group and
// leaves obj._lastPos/_lastRot (what the sim said) alone, re-deriving the group from them
// every tick. The viewer's equivalent split is LLViewerObject (authoritative position and
// rotation) against LLDrawable::mXform (what is rendered), copied in
// LLDrawable::updateXform(). That copy is where the offset goes: apply() adds this frame's
// bob and tilt to the drawable's target, removeApplied() takes last frame's back out of
// the previous xform before the damping compares the two. The viewer object is never
// touched, so interpolation, ObjectUpdate comparison, selection and the sim all see the
// hull exactly where it is. Linked prims render through the root drawable's world matrix
// (LLSpatialBridge, lldrawable.cpp getRenderMatrix()) and a seated avatar's root is slaved
// to its seat's drawable (llvoavatar.cpp updateRootPositionAndRotation), so the whole
// boat and everyone aboard follow for free — WolfStorm's _updateSeatedRiders() pass has
// no counterpart here because the scene graph already does it.
//
// The rocker only ever marks a hull's drawable moved; LLDrawable::updateMove() makes it
// active (geometry in object space under a spatial bridge), which is the same state every
// vehicle the sim drives is in, and the state WolfStorm reproduces by popping a rocking
// linkset out of its static batches.
class WolfBoatRock : public LLSingleton<WolfBoatRock>
{
    LLSINGLETON(WolfBoatRock);
    ~WolfBoatRock();

public:
    /** Every frame from LLAppViewer::idle(), BEFORE gPipeline.updateMove(). */
    void idle();

    /**
     * LLDrawable::updateXform() hooks, root drawables only. removeApplied() subtracts the
     * offset apply() put on this object last frame; apply() adds this frame's and records
     * it. Both are no-ops for anything that is not rocking.
     */
    void removeApplied(const LLViewerObject* objectp, LLVector3& pos, LLQuaternion& rot) const;
    void apply(const LLViewerObject* objectp, LLVector3& pos, LLQuaternion& rot);

    // Source: terrain_manager.js updateFloaters() — 2.5 s sweep, 48 rockers, 256 m.
    static constexpr F64 SWEEP_INTERVAL_SECS = 2.5;
    static constexpr S32 MAX_ROCKERS = 48;
    static constexpr F32 RADIUS_M = 256.f;
    static constexpr F64 STATS_INTERVAL_SECS = 10.0;

private:
    struct Verdict
    {
        bool        mRock = false;
        F32         mGain = 0.f;
        bool        mWantName = false;
        bool        mInBand = false;     // passed the waterline-band gate (diagnostics)
        const char* mWhy = "";
    };
    enum NameVerdict
    {
        NAME_UNKNOWN,   // properties not received yet
        NAME_BOAT,
        NAME_NOT_BOAT
    };
    struct Sample
    {
        F32 mZ = 0.f;   // height offset, metres
        F32 mSx = 0.f;  // surface slope dz/dx
        F32 mSy = 0.f;  // surface slope dz/dy
    };
    struct Rocker
    {
        LLPointer<LLViewerObject> mObject;
        // Low-passed wave state.
        F32 mBob = 0.f;
        F32 mSx = 0.f;
        F32 mSy = 0.f;
        F32 mGain = 1.f;
        // What idle() computed for this frame, what apply() put on the drawable.
        F32          mTargetBob = 0.f;
        LLQuaternion mTargetTilt;
        F32          mAppliedBob = 0.f;
        LLQuaternion mAppliedTilt;
        bool         mApplied = false;
    };

    void sweep();
    void step();
    /** Drop a rocker and put its hull back on the sim's transform. */
    void evict(std::map<const LLViewerObject*, Rocker>::iterator it);
    void evictAll();

    Verdict classify(LLViewerObject* objectp) const;
    NameVerdict nameVerdict(const LLViewerObject* objectp) const;
    Sample sampleWave(const LLViewerObject* objectp, F32 t) const;

    std::map<const LLViewerObject*, Rocker> mRockers;
    F64 mNextSweep = 0.0;
    F64 mNextStatsLog = 0.0;
    // Diagnostics from the last sweep.
    S32 mStatBand = 0;
    S32 mStatQualify = 0;
    S32 mStatWantName = 0;
    S32 mStatDenied = 0;
};

#endif // WOLF_BOATROCK_H
