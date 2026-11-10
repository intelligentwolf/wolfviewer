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
#include "llquaternion.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <map>
#include <vector>

class LLVertexBuffer;
class LLViewerObject;
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
 *  LANDING  [BIRDS LANDING 2026-11-02] gulls settle on the sea (floating on the drawn waves, WolfBoatRock::drawnWaveHeight),
 *           on the shore and on a few roof ridges, wall tops and railings (found by a budgeted downward ray, RAYS_PER_SEC);
 *           small flocks drop to open flat land to peck and hop, and lift together. Anyone or anything coming near
 *           (avatars, moving vehicles and boats, low planes, the camera) flushes them with a call; they settle further off.
 *           Moderate rain keeps them down; at dusk or when the owner turns birds off they take off and go like the rest.
 *           README "Landing" (region-terrain-work/birds-2026-11-02).
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
    /** [BIRDS LANDING] Where a bird is between the air and the ground. */
    enum Land : U8 { AIR = 0, APPROACH = 1, DOWN = 2, TAKEOFF = 3 };
    /** [BIRDS LANDING] What it lands on. */
    enum Spot : U8 { SPOT_NONE = 0, SPOT_WATER = 1, SPOT_SHORE = 2, SPOT_GRASS = 3, SPOT_PERCH = 4 };
    /** [BIRDS LANDING] What it does down there. */
    enum Act : U8 { ACT_REST = 0, ACT_MOVE = 1, ACT_PECK = 2, ACT_PREEN = 3 };
    /** How many birds are down (on water, land or a perch) now (for the log and tests). */
    S32 landedCount() const;

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
        // [BIRDS LANDING 2026-11-02]
        U8 mLand = AIR;
        U8 mSpot = SPOT_NONE;
        U8 mAct = ACT_REST;
        bool mAlarm = false;     // flushed: climbing away fast
        bool mRising = false;    // just off the ground or water, not yet up to the 1.5 m floor (keepClear: climbs to it)
        F32 mRiseT = 0.f;        // seconds since it took off, while rising
        F64 mWaveT = 0.0;        // floatZ: when the drawn water under it was last sampled (0 = never)
        F32 mWaveZ = 0.f;        // ... that sample, and its rate of change since the one before
        F32 mWaveRate = 0.f;
        LLVector3 mSpotPos;      // agent metres: the surface point it lands on (feet, or the waterline)
        F32 mYaw = 0.f;          // heading on the ground or water, radians from +X
        F32 mTurn = 0.f;         // its turn rate while paddling, rad/s
        F32 mPitch = 0.f;        // head down (pecking, preening), radians
        F32 mActFor = 0.f;       // seconds left of this activity (or of the approach, as a time-out)
        F32 mDownFor = 0.f;      // gulls: seconds left down before it takes off by itself
        F32 mNextLand = 0.f;     // gulls: seconds of flight before it looks for somewhere to settle
        F32 mRun = 0.f;          // take-off run along the water: seconds left
        F32 mAlarmFor = 0.f;     // seconds left of the alarm climb
        F32 mStep = 0.f;         // walking / hopping / pecking phase
        F32 mLift = 0.f;         // hop arc height now, metres
        LLPointer<LLViewerObject> mPerch;   // the prim it stands on (SPOT_PERCH)
        LLVector3 mPerchAt;      // that prim's position when it landed (agent metres)
        LLQuaternion mPerchRot;  // ... and its rotation
    };

    /** [BIRDS LANDING] A free perch the rays found: the top of a prim, near-horizontal, open to the sky. */
    struct Perch
    {
        LLPointer<LLViewerObject> mObject;
        LLVector3 mAt;           // agent metres: the surface point
        LLVector3 mObjectAt;     // the prim's position then (agent)
        LLQuaternion mObjectRot;
        F64 mFound = 0.0;
    };
    /** [BIRDS LANDING] A prim worth aiming a perch ray at (from scanBuilds): region metres, so no rebase is needed. */
    struct PerchCandidate
    {
        LLVector3 mRegionPos;    // its centre, region metres
        F32 mHalfX = 0.f, mHalfY = 0.f, mTop = 0.f;   // half its footprint, and its top (region metres)
    };
    /** [BIRDS LANDING] Something that flushes birds: an avatar, a moving vehicle or boat, a low plane, the camera. */
    enum ThreatKind : U8 { THREAT_PERSON = 0, THREAT_VEHICLE = 1, THREAT_AIRCRAFT = 2, THREAT_CAMERA = 3 };
    struct Threat
    {
        LLVector3 mPos;          // agent metres
        U8 mKind = THREAT_PERSON;
    };

    struct Flock
    {
        Kind mKind = GULL;
        U8 mZone = 0;            // [BIRDS ZONES 2026-11-10/] WolfRegionWeather::BirdsZone it was born in: its species
        LLVector3 mCentre;       // agent metres, at the surface
        LLVector3 mTarget;       // where the flock is heading (small, starling)
        LLVector3 mExit;         // where it flies off to when leaving
        F64 mRetarget = 0.0;     // when the target or centre moves next
        F64 mNextCall = 0.0;
        F32 mLoop = 0.f;         // starlings: the phase of their wheeling
        bool mLeaving = false;
        bool mRoosting = false;  // starlings: going down into the roost
        // [BIRDS LANDING 2026-11-02] small flocks land and lift together
        bool mDown = false;      // feeding on the ground (or coming down to)
        F32 mSettleIn = 0.f;     // seconds of flight before the flock drops to feed
        F32 mFeedFor = 0.f;      // seconds of feeding left before the whole flock lifts
        F32 mWary = 0.f;         // seconds after a flush before it lands again
        LLVector3 mPatch;        // agent metres: where it feeds
        LLVector3 mFlushedFrom;  // agent metres: what flushed it last (it settles away from there)
        bool mFlushed = false;
    };

    void reset();
    U8 zoneOf(const Bird& b) const { return mFlocks[(size_t)b.mFlock].mZone; }   // [BIRDS ZONES 2026-11-10/]
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
    // [BIRDS LANDING 2026-11-02]
    bool takeRay();
    S32 spotClear(const LLVector3& surface);          // 1 open to the sky, 0 something over it, -1 no ray left this second
    F32 slopeAt(const LLVector3& agent) const;
    bool shoreAt(const LLVector3& agent) const;
    bool grassAt(const LLVector3& agent) const;
    bool threatNear(const LLVector3& agent, Kind kind, F32 mult) const;   // something within mult x its flush distance
    static F32 flushRadius(U8 threat, Kind kind);
    bool perchStill(const Bird& b) const;             // its perch is there and has not moved
    void leave(Flock& f, const LLVector3& cam);       // the flock flies off out of view
    F32 standHeight(const Bird& b, Kind kind) const;  // the body's centre over the surface it is down on
    F32 waterSurfaceZ(const LLVector3& agent) const;  // the drawn water there (rest level + waves)
    F32 floatZ(Bird& b, const LLVector3& agent) const; // waterSurfaceZ for one bird, sampled at most every WAVE_SAMPLE_S
    void seekPerch(F64 now);
    void chooseLanding(Bird& b, Flock& f, S32 index, F64 now);
    void settleFlock(Flock& f, S32 flock_index);
    void liftFlock(Flock& f, S32 flock_index, bool alarm, const LLVector3& from);
    void takeOff(Bird& b, bool alarm, const LLVector3& from);
    void flush(S32 index, const LLVector3& from);
    void flushCheck(F64 now);
    void moveApproach(Bird& b, Flock& f, F32 dt);
    void moveDown(Bird& b, Flock& f, F32 dt);
    void moveTakeOff(Bird& b, Flock& f, F32 dt);
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

    // [BIRDS LANDING 2026-11-02]
    std::vector<PerchCandidate> mPerchCands;       // the prims worth a perch ray (scanBuilds)
    std::vector<PerchCandidate> mPerchCandsScan;   // ... the scan in progress
    std::vector<Perch> mPerches;                   // free perches found, not yet taken
    std::vector<Threat> mThreats;                  // what could flush a bird, as of the last flushCheck
    std::vector<LLVector3d> mAvatarScratch;
    std::map<LLUUID, LLVector3d> mMoverLast;       // moving roots' global positions at the last flushCheck (their speed)
    F64 mMoverLastT = 0.0;
    F32 mRayTokens = 0.f;                          // the ray budget (RAYS_PER_SEC, at most RAY_BURST)
    F64 mNextFlush = 0.0;
    F64 mNextPerchRay = 0.0;
    bool mSitTight = false;                        // moderate rain: down birds stay down
    // frame cost (idle + buildMesh), averaged for the log line
    F64 mCostIdleUs = 0.0, mCostMeshUs = 0.0;
    F64 mCostFlushUs = 0.0, mCostPerchUs = 0.0, mCostMoveUs = 0.0;   // parts of mCostIdleUs
    S32 mCostFrames = 0;

    LLPointer<LLVertexBuffer> mVB;
    std::vector<S32> mVisible;     // buildMesh: the birds in view this frame
};

#endif // WOLF_BIRDS_H
