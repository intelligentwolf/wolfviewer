/**
 * @file wolfbirds.h
 * @brief WolfViewer: wild birds over Wolf Territories regions - gulls and terns over the sea, small flocks over open land,
 *        starling murmurations at dusk in winter (ticket 445, Maximus Lear's idea).
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

#ifndef WOLF_BIRDS_H
#define WOLF_BIRDS_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <vector>

class LLVertexBuffer;
class LLViewerRegion;

/**
 * The birds are drawn by each viewer, for itself: no objects, no scripts and no messages on the region, so 2000
 * residents cost the grid nothing but the `birds` flag weather.php already sends with the weather each minute
 * (region-terrain-work/birds-2026-11-02/README.md "Design").
 *
 *  WHETHER  the region allows them: WolfRegionWeather::birdsAllowed() (the owner's Region/Estate choice, or the default:
 *           on under Parks or Marinas, sea and coast only with no category, off under any other category;
 *           wolf_ambient_lib.php wa_birds_default), an Earth sky (not a black space sky), a camera not up in a skybox,
 *           and the resident's own WolfViewerBirds.
 *  WHERE    from the ground the viewer already has: gulls and terns over water below the region's water height and its
 *           coasts, small flocks over land; never over cells crowded with builds (built()).
 *  WHEN     by the sun of the sky on screen: none at night (they roost), fewer at dawn and dusk; starlings gather and
 *           wheel at dusk from October to March (April to September south of the equator) and then drop into the roost.
 *  WEATHER  the weather on screen (WolfWeather::activeProfile): fewer in rain or snow, none in a downpour, a blizzard or a
 *           hurricane over the region.
 *  COST     at most WolfViewerBirdsMax (160) birds, lowered by the frame budget (WolfFrameGovernor); one vertex buffer,
 *           one draw call in the deferred pass, birds outside the view not written.
 */
class WolfBirds : public LLSingleton<WolfBirds>
{
    LLSINGLETON(WolfBirds);
    ~WolfBirds();

public:
    /** Once a frame (llappviewer.cpp idle, after WolfFarGround): decide, move, call. */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfFarGround): the birds as lit opaque geometry. */
    void renderDeferred();
    /** How many birds fly now (for the log and tests). */
    S32 count() const { return (S32)mBirds.size(); }

    enum Kind : U8 { GULL = 0, TERN = 1, SMALL = 2, STARLING = 3, KIND_COUNT = 4 };

private:
    struct Bird
    {
        LLVector3 mPos;          // agent metres
        LLVector3 mVel;          // m/s
        F32 mBank = 0.f;         // radians, + = left wing down
        F32 mPhase = 0.f;        // wingbeat phase, radians
        F32 mFlapFor = 0.f;      // seconds of flapping left (gull, small: then a glide or a bound)
        F32 mGlideFor = 0.f;     // seconds of gliding left
        F32 mSpeed = 10.f;       // its own cruising speed
        F32 mSize = 1.f;         // its own scale (0.9 .. 1.1)
        // gulls: the circle it soars on, round the flock's centre
        LLVector3 mOrbitOffset;
        F32 mOrbitR = 20.f;
        F32 mOrbitDir = 1.f;
        F32 mAlt = 20.f;         // metres above the surface it keeps to
        // terns: patrol, hover, plunge
        LLVector3 mWaypoint;
        S32 mState = 0;          // 0 patrol, 1 hover, 2 dive, 3 climb
        F32 mStateFor = 0.f;
        S32 mFlock = 0;          // index into mFlocks
    };

    struct Flock
    {
        Kind mKind = GULL;
        LLVector3 mCentre;       // agent metres, at the surface
        LLVector3 mTarget;       // where the flock is heading (small, starling)
        LLVector3 mExit;         // where it flies off to when leaving
        F64 mRetarget = 0.0;     // when the target or centre moves next
        F64 mNextCall = 0.0;
        F32 mLoop = 0.f;         // starlings: the phase of their wheeling
        bool mLeaving = false;
        bool mRoosting = false;  // starlings: going down into the roost
    };

    void reset();
    bool wanted(LLViewerRegion*& region) const;
    void direct(LLViewerRegion* region, F64 now);
    bool spawnFlock(LLViewerRegion* region, Kind kind, S32 count, F64 now);
    void rebase(const LLVector3& delta);   // the agent frame moved: shift every stored position with it
    bool pickPlace(LLViewerRegion* region, Kind kind, F32 min_m, F32 max_m, LLVector3& out) const;
    void move(F32 dt, F64 now);
    void moveGull(Bird& b, Flock& f, F32 dt);
    void moveTern(Bird& b, Flock& f, F32 dt);
    void moveFlocking(Bird& b, Flock& f, S32 index, F32 dt);
    void steer(Bird& b, const LLVector3& desired_vel, F32 max_accel, F32 dt);
    void keepClear(Bird& b, F32 dt);
    void calls(F64 now);
    void playCall(Kind kind, const LLVector3& at_agent, F32 dist);

    // the place
    F32 surfaceZ(const LLVector3& agent) const;       // max(land, water)
    bool overWater(const LLVector3& agent) const;
    bool built(const LLVector3& agent) const;
    bool inRegion(LLViewerRegion* region, const LLVector3& agent, F32 margin) const;
    void scanBuilds(LLViewerRegion* region, F64 now);

    // the look
    void buildMesh(S32& verts);
    bool ensureBuffer();

    std::vector<Bird>  mBirds;
    std::vector<Flock> mFlocks;
    LLUUID mRegionId;
    LLVector3 mLastCamera;
    LLVector3d mLastOrigin;        // gAgent.getAgentOriginGlobal() the positions are relative to
    F64 mNextDirect = 0.0;
    F64 mLastIdle = 0.0;
    F64 mLastLog = 0.0;

    // built cells: CELL_M squares over the agent's region, an object count per cell
    std::vector<U16> mBuilt;
    std::vector<U16> mBuiltScan;   // the scan in progress (spread over frames)
    S32 mCells = 0;                // cells per side
    S32 mScanIndex = -1;           // next gObjectList index of the scan in progress, -1 = none
    F64 mNextScan = 0.0;
    bool mHaveBuilt = false;

    LLPointer<LLVertexBuffer> mVB;
    std::vector<S32> mVisible;     // buildMesh: the birds in view this frame
};

#endif // WOLF_BIRDS_H
