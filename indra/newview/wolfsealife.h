/**
 * @file wolfsealife.h
 * @brief WolfViewer: dolphins and whales in the open sea of Wolf Territories regions whose owner has switched them on
 *        (ticket 499, IndigoQueen King's idea).
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

#ifndef WOLF_SEA_LIFE_H
#define WOLF_SEA_LIFE_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <set>
#include <string>
#include <vector>

class LLVertexBuffer;
class LLViewerObject;
class LLViewerRegion;

/**
 * Drawn by each viewer for itself, exactly as the birds are (wolfbirds.h): no objects, scripts or messages on the region,
 * so 2000 residents cost the grid nothing but the `sealife` block weather.php already sends with the weather each minute.
 * The shared spec, with every figure and its source: region-terrain-work/sea-life-2026-11-03/README.md. This is a port of
 * WolfStorm's js/world/wolf_sea_life.js (the same species table, state machine, sounds and spray), so the two viewers
 * show the same animals doing the same things.
 *
 *  WHETHER  WolfRegionWeather::seaLifeAllowed() (off unless the estate owner or a manager switched it on in Region/Estate),
 *           a camera not up in a skybox, and the resident's own WolfViewerSeaLife.
 *  WHERE    the open sea of the agent's region only: terrain at least the species' depth below the region's water height
 *           (no painted or prim water: those are lakes and pools); never into the shallows.
 *  WHAT     bottlenose dolphins in pods of 5-15 and alone (cruising and breathing, porpoising, riding the bow of a moving
 *           boat); humpback and minke whales alone or in small groups (a series of breaths, each with a blow, then a dive,
 *           the humpback lifting its flukes, a long time down). Fewer dolphins in polar seas (the grid's `dolphins`).
 *  SOUND    dolphins' whistles and clicks, whales' blows and humpback song, at the Ambient volume; under the water the
 *           whistles and the song are louder and carry much further. At most MAX_VOICES at once.
 *  COST     nothing while off; at most WolfViewerSeaLifeMax (60) animals, lowered by the frame budget (WolfFrameGovernor);
 *           one vertex buffer, one draw call in the deferred pass (the blows' spray included); animals out of view are not
 *           written; fewer rings beyond LOD_M.
 */
class WolfSeaLife : public LLSingleton<WolfSeaLife>
{
    LLSINGLETON(WolfSeaLife);
    ~WolfSeaLife();

public:
    /** Once a frame (llappviewer.cpp idle, after WolfBirds): decide, swim, sound. */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfBirds): the animals and their spray as lit opaque geometry. */
    void renderDeferred();
    /** How many animals swim now (for the log and tests). */
    S32 count() const { return (S32)mAnimals.size(); }

    enum Kind : U8 { DOLPHIN = 0, HUMPBACK = 1, MINKE = 2, KIND_COUNT = 3 };
    /** wolf_sea_life.js states: dolphins SWIM, BREATHE, LEAP; whales DOWN, RISE, SHALLOW, SURFACE, DIVE. */
    enum State : U8 { SWIM = 0, BREATHE = 1, LEAP = 2, W_DOWN = 3, W_RISE = 4, W_SHALLOW = 5, W_SURFACE = 6, W_DIVE = 7 };

private:
    struct Animal
    {
        S32 mGroup = 0;          // index into mGroups (the group's first animal in mAnimals is its leader)
        Kind mKind = DOLPHIN;
        F32 mSize = 1.f, mLen = 2.5f;
        LLVector3 mOff;          // its offset from the group's centre (agent axes, as wolf_sea_life.js `off`)
        LLVector3 mPos;          // agent metres: the centre of its body's axis
        F32 mHeading = 0.f, mPitch = 0.f, mSpeed = 0.f, mVz = 0.f, mPhase = 0.f;
        U8 mState = SWIM;
        F32 mStateFor = 0.f;     // seconds left of this state
        F32 mDepthT = 1.f;       // the depth it keeps while swimming
        S32 mBreaths = 0;
        bool mBlown = false, mWasAbove = false, mSynced = false;
        F32 mArcT = 0.f, mArcLen = 0.f, mArcZ0 = 0.f;   // BREATHE / DIVE: time in, length, start height
        LLVector3 mSlot;         // bow riding: its slot ahead of the bow (agent metres)
    };

    struct Group
    {
        S32 mId = 0;
        Kind mKind = DOLPHIN;
        LLVector3 mCentre;       // agent metres (z unused)
        F32 mHeading = 0.f, mTarget = 0.f, mRetarget = 0.f;
        bool mLeaving = false;
        bool mPorpoise = false;  // dolphins: 'porpoise' mode rather than 'cruise'
        F32 mModeFor = 0.f;
        F64 mNextCall = 0.0, mNextSong = 0.0;
        // bow riding
        LLPointer<LLViewerObject> mRide;
        F64 mRideUntil = 0.0;
        F32 mRideSpeed = 0.f, mRideHeading = 0.f;
    };

    /** One drop of a blow's spray (agent metres). */
    struct Drop
    {
        LLVector3 mPos, mVel;
        F32 mDec = 0.f, mAge = 0.f, mLife = 3.f, mSize = 0.5f, mRise = 0.6f;
    };

    void reset();
    void rebase(const LLVector3& delta);
    bool wanted(LLViewerRegion*& region) const;
    void direct(const LLVector3& cam);
    bool pickPlace(Kind kind, const LLVector3& cam, LLVector3& out, bool first) const;   // [SEA LIFE EASY 2026-11-06/] first
    bool spawn(Kind kind, S32 count, const LLVector3& cam, F64 now, bool first);
    void leaveGroup(Group& g, const LLVector3& cam);
    void moveGroup(Group& g, F32 dt);
    void steer(Animal& a, F32 dt);
    void moveDolphin(Animal& a, F32 dt, F64 now);
    void moveWhale(Animal& a, F32 dt, F64 now);
    void rideCheck(F64 now);
    void recentre(S32 group);
    void rideSlots(S32 group);
    void blow(const Animal& a, F64 now);
    void moveSpray(F32 dt);
    void groupSounds(F64 now, const LLVector3& cam);
    void sound(S32 which, const LLVector3& at, F32 mult, F64 now);
    void move(F32 dt, F64 now, const LLVector3& cam);

    // the place: the agent's region and ([SEA LIFE EASY 2026-11-06]) the neighbours round it whose sea may hold them
    F32 water() const;
    F32 depth(const LLVector3& agent) const;             // 0 on land, before the terrain has loaded, off every region or over
                                                         // a neighbour whose sea may not hold them
    /** [SEA LIFE EASY 2026-11-06] The region under `agent` other than the agent's (nullptr: the agent's own or none), and in
     *  `ok` whether its sea may hold them (WolfRegionWeather::seaLifeNear, and its water at the agent's region's height). */
    LLViewerRegion* neighbourAt(const LLVector3& agent, bool& ok) const;
    bool deepDisc(const LLVector3& agent, F32 need, F32 radius) const;
    bool inRegion(const LLVector3& agent, F32 margin) const;

    // the look
    void buildMesh(S32& verts);
    bool ensureBuffer();

    std::vector<Animal> mAnimals;
    std::vector<Group> mGroups;
    std::vector<Drop> mDrops;
    std::set<std::string> mRideTried;   // "boat id:group id", decided once each
    std::vector<F64> mVoiceEnds;        // when each sound playing now ends (MAX_VOICES)
    S32 mNextGroupId = 0;
    LLUUID mRegionId;
    LLVector3 mLastCamera;
    bool mHaveLastCamera = false;
    LLVector3d mLastOrigin;
    F64 mNextDirect = 0.0, mNextRide = 0.0;
    F64 mLastIdle = 0.0, mLastLog = 0.0;
    F64 mCostIdleUs = 0.0, mCostMeshUs = 0.0;
    S32 mCostFrames = 0;

    LLPointer<LLVertexBuffer> mVB;
};

#endif // WOLF_SEA_LIFE_H
