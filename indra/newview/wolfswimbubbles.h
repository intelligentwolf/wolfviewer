/**
 * @file wolfswimbubbles.h
 * @brief WolfViewer: a few small bubbles rising from the mouth of every avatar under the water, with a quiet bubbling
 *        (ticket 346, Jimmy Olsen's idea).
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

#ifndef WOLF_SWIM_BUBBLES_H
#define WOLF_SWIM_BUBBLES_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <map>
#include <vector>

class LLVertexBuffer;
class LLVOAvatar;
class LLVOWater;

/**
 * [SWIM BUBBLES 2026-11-02] Ticket 346, Jimmy Olsen: "When Avatar swimming, can u add effects like tiny bubble rezzing
 * from mouth? Maybe some bubble sounds too? (shouldnt be loud anyway)". Paul approved it on 2 Nov.
 * Persona: a resident diving off a Wolf coast with friends. Story: "Under the water I see a few small bubbles rise from my
 * mouth, and from my friends' mouths, wobble up and pop at the surface, and hear a quiet bubbling now and then."
 *
 * Drawn by each viewer for the avatars it sees (like the birds, region-terrain-work/birds-2026-11-02/README.md): no
 * objects, no scripts, no messages, nothing on the grid. README: region-terrain-work/swim-bubbles-2026-11-02/README.md.
 *
 *  WHO      every avatar within REACH_M of the camera (the resident's own too), not animated objects; Wolf regions only
 *           (WolfGrid::showsWolfRegion, as swimming) and the resident's own WolfViewerSwimBubbles (World > Wolf World >
 *           Bubbles Under Water, on by default).
 *  UNDER    its mouth at least ENTER_BELOW under the real water there, any of:
 *             - the sea: the region's water height plus the Wolf waves as drawn (WolfBoatRock::drawnWaveHeight);
 *             - the water a builder put on a prim ("wolfwater", FSWolfWater) or painted on the ground (WolfTerrainPaint):
 *               inside the body of one of their water planes (its footprint, from its surface down its depth);
 *           and never inside an air-filled volume (a sea tunnel or a boat's hull, WolfDryVolumes::contains).
 *  MOUTH    the head joint (mHead) moved forward to the front of the head: the frontmost point of the avatar's own
 *           rigged mesh on the head (LLVOAvatar::mJointRiggingInfoTab, what the viewer already measures for its bounding
 *           box), or the Mouth attachment point's for an avatar with none, plus the breath emitter's 5 cm ahead - so a
 *           wolf's or a dragon's snout is followed as well as a human face.
 *  BUBBLES  a puff of a few every few seconds (a breath), more often while swimming fast; each rises at its terminal
 *           speed, wobbles on a helix, grows as the pressure falls, and pops at the surface.
 *  SOUND    a quiet bubbling with each puff (CC0, app_settings/wolfbubbles/SOURCES.md) at the mouth, at the Ambient
 *           volume, heard only by a listener under the water too.
 *  COST     at most MAX_BUBBLES (lowered by WolfFrameGovernor), one vertex buffer, one draw call in the deferred pass.
 */
class WolfSwimBubbles : public LLSingleton<WolfSwimBubbles>
{
    LLSINGLETON(WolfSwimBubbles);
    ~WolfSwimBubbles();

public:
    /** Once a frame (llappviewer.cpp idle, after WolfBirds): who is under, puffs, rising, popping, sounds. */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfBirds): the bubbles as lit opaque geometry. */
    void renderDeferred();
    /** How many bubbles there are now (for the log and tests). */
    S32 count() const { return (S32)mBubbles.size(); }

private:
    /** Which water a bubble rises through. */
    struct Water
    {
        LLPointer<LLVOWater> mPlane;   // null: the sea (the region's water height and the drawn waves)
    };

    struct Bubble
    {
        LLVector3 mCentre;      // agent metres: the helix's axis point (the bubble wobbles round it)
        LLVector3 mDrift;       // m/s: what is left of the puff's own push (decays)
        F32 mR0 = 0.004f;       // radius when it left the mouth, metres
        F32 mP0 = 0.f;          // the absolute pressure there, Pa (Boyle: r = r0 (p0 / p)^(1/3))
        F32 mR = 0.004f;        // radius now
        F32 mPhase = 0.f;       // helix phase, radians
        F32 mTurn = 1.f;        // +1 / -1: which way it spirals
        F32 mAge = 0.f;         // seconds since it left the mouth
        F32 mPop = 0.f;         // seconds of popping left (while mPopping)
        bool mPopping = false;  // reached the surface (or left its water): swelling, then gone
        F32 mSurface = 0.f;     // agent z of the surface above it, as last sampled
        F64 mSurfaceT = 0.0;    // when that was sampled
        Water mWater;
        LLVector3 mPos;         // drawn position this frame (centre + helix)
    };

    /** What is kept per avatar: when it breathes next and where its mouth is on its head. */
    struct Diver
    {
        F64 mSeen = 0.0;
        F64 mNextPuff = 0.0;    // 0 = not under the water (the first puff comes soon after it goes under)
        S32 mBurstLeft = 0;     // bubbles of this puff still to leave the mouth
        F64 mBurstNext = 0.0;
        F64 mNextRig = 0.0;     // when the mouth's forward distance is measured again (while under the water)
        F32 mForward = 0.f;     // mHead frame: the front of the head (x), metres; 0 = not measured (the Mouth point)
        bool mUnder = false;    // puffing (hysteresis: see OUT_KEEP_S)
        F64 mOutSince = 0.0;    // when the mouth came out of the water while puffing (0 = it is in)
    };

    void reset();
    void rebase(const LLVector3& delta);
    /** The mouth in agent metres, and the head's forward direction. */
    /** measure: read the front of the head from the avatar's rigged mesh again (only while it is under the water). */
    bool mouthOf(LLVOAvatar* av, Diver& d, bool measure, LLVector3& mouth, LLVector3& forward);
    /** Is this agent point under the water: the surface over it (agent z) and which water. */
    bool underWater(const LLVector3& p, F32& surface, Water& water) const;
    /** The surface of this bubble's water over (x, y) now; false when it has left that water (out of a pool's footprint). */
    bool surfaceOf(const Water& w, const LLVector3& p, F32& surface) const;
    void puffSound(const LLVector3& mouth, bool self, F64 now);
    void emit(const LLVector3& mouth, const LLVector3& forward, F32 surface, const Water& water, S32 cap);
    void move(F32 dt, F64 now);
    bool ensureBuffer();
    void buildMesh(S32& verts);

    std::vector<Bubble> mBubbles;               // oldest first
    std::map<LLUUID, Diver> mDivers;
    std::vector<LLVOWater*> mPlanes;            // this frame's prim and painted water planes
    std::vector<LLVector3> mPlaneCentre;        // ... their agent positions
    std::vector<F32> mPlaneRadius2;             // ... and the squared radius of a sphere round each one's body
    std::vector<S32> mVisible;                  // buildMesh: the bubbles in view this frame
    LLVector3d mLastOrigin;
    F64 mLastIdle = 0.0;
    F64 mNextOtherSound = 0.0;                  // another avatar's puff is heard at most every OTHER_SOUND_GAP_S
    F64 mLastLog = 0.0;
    // frame cost (idle + buildMesh), summed for the minute's log line
    F64 mCostIdleUs = 0.0, mCostMeshUs = 0.0;
    S32 mCostFrames = 0, mPeakBubbles = 0, mPeakDivers = 0;

    LLPointer<LLVertexBuffer> mVB;
};

#endif // WOLF_SWIM_BUBBLES_H
