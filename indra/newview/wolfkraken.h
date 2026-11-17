/**
 * @file wolfkraken.h
 * @brief WolfViewer: now and then a Kraken rises in the open sea of Wolf Territories regions (ticket 823, Indi Moon's idea).
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

#ifndef WOLF_KRAKEN_H
#define WOLF_KRAKEN_H

#include "llpointer.h"
#include "llsingleton.h"
#include "v3dmath.h"
#include "v3math.h"

#include <map>
#include <string>
#include <vector>

class LLVertexBuffer;
class LLViewerObject;
class LLViewerRegion;

/**
 * Drawn by each viewer for itself, as the dolphins and whales are (wolfsealife.h): no objects, scripts or messages on the
 * region. Paul 2026-11-17: on "only if the user turns it off" - on by default, off for yourself (Wolf World > Kraken
 * Sightings, WolfViewerKraken) or for a region (Region/Estate > Region, weather.php `kraken`). A port of WolfStorm's
 * js/world/wolf_kraken.js (the same schedule, hash, places, timeline and look); the shared spec with every figure is
 * region-terrain-work/kraken-2026-11-17/README.md in the firestorm repository.
 *
 *  WHEN   a hash of (512 m cell, 15-minute UTC slot): the same sighting, at the same second, for every viewer near it.
 *  WHERE  the first of four fixed points in the cell that is deep open sea with no build near, in a region whose switch is
 *         on; a viewer that cannot judge a point yet (terrain not here, too far off) shows nothing rather than another one.
 *  WHAT   it churns the water, breaches, its arms rise one after another and wave, it roars, a long tentacle lashes the
 *         sea (or both reach for a passing boat and coil round it, for show), and it sinks in a whirlpool of foam.
 *         Nothing is touched: no object is moved, nobody is hurt.
 *  COST   a hash every 2 s while none is near; while one shows, one vertex buffer and one draw call (about 6 400
 *         triangles and the spray), only while its place is in view.
 */
class WolfKraken : public LLSingleton<WolfKraken>
{
    LLSINGLETON(WolfKraken);
    ~WolfKraken();

public:
    /** Once a frame (llappviewer.cpp idle, beside WolfSeaLife). */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfSeaLife). */
    void renderDeferred();
    /** Whether a sighting is on show (for the log and tests). */
    bool showing() const { return mHave; }

    static constexpr S32 CANDIDATES = 4, ARMS = 8, FEEDERS = 2, FOAM_N = 220;

private:
    struct Plan
    {
        std::string mKey;
        S32 mCx = 0, mCy = 0;
        U32 mSlot = 0;
        F64 mStart = 0.0;
        F64 mCand[CANDIDATES][2] = {};
    };
    struct ArmLook { F32 mEmerge = 0.f, mPhase = 0.f, mPhase2 = 0.f, mLen = 0.f; };
    struct FoamLook { F32 mRho = 0.f, mAlpha = 0.f, mSize = 0.f; };
    struct Look
    {
        F32 mFacing = 0.f, mSize = 1.f;
        S32 mPalette = 0;
        U32 mSeed = 0;
        ArmLook mArms[ARMS];
        FoamLook mFoam[FOAM_N];
    };
    /** A drop of spray or a bubble, relative to the spot at the water (z = height over the water). */
    struct Drop
    {
        LLVector3 mPos, mVel;
        F32 mAge = 0.f, mLife = 4.f, mSize = 0.5f;
        bool mBubble = false;
    };
    enum Judge { NO = 0, YES = 1, UNKNOWN = 2 };
    struct Decided { bool mSpot = false; F64 mGx = 0.0, mGy = 0.0; };

    // the schedule
    static bool plan(S32 cx, S32 cy, U32 slot, Plan& out);
    static void looks(S32 cx, S32 cy, U32 slot, Look& out);
    F64 utcNow();
    void check(const LLVector3d& cam_global, F64 utc);
    bool resolve(const Plan& p, const LLVector3d& cam_global, Decided& out);
    void begin(const Plan& p, F64 gx, F64 gy, F32 water, F32 depth, F64 utc, bool forced);
    bool force(const LLVector3d& cam_global);

    // the place
    Judge qualify(F64 gx, F64 gy, const LLVector3d& cam_global) const;
    bool depthAt(F64 gx, F64 gy, F32& depth, F32& water) const;   // false: not known yet
    bool regionOn(LLViewerRegion* r) const;
    bool buildNear(F64 gx, F64 gy, F32 water) const;
    LLViewerObject* findBoat() const;

    // the animation
    F32 crownZ(F32 t) const;
    F32 armOut(S32 i, F32 t) const;
    struct Frame { LLVector3 mC, mA, mF, mS; };
    Frame bodyFrame(F32 t) const;
    void armLine(S32 i, F32 t, const Frame& bf, std::vector<LLVector3>& line, std::vector<LLVector3>& suck);
    void feederPose(S32 i, F32 t, std::vector<LLVector3>& line, const LLVector3& base, const LLVector3& o, F32 len, F32 out);
    void spray(S32 n, const LLVector3& at, F32 vz_lo, F32 vz_hi, F32 out, F32 size_lo, F32 size_hi);
    void bubbles(S32 n, F32 radius);
    void moveDrops(F32 dt);
    void events(F64 prev_t, F64 t);
    void sound(S32 which, const LLVector3& rel);
    LLVector3 spotAgent() const;   // the spot at the water, agent metres

    // the look
    void buildMesh(F32 t, S32& verts);
    bool ensureBuffer();

    bool mHave = false;
    Plan mPlan;
    F64 mGx = 0.0, mGy = 0.0;
    F32 mWater = 20.f;
    F32 mDepth = 20.f;   // the water's depth at the spot (crownZ keeps the body above the sea bed)
    Look mLook;
    bool mForced = false;
    F64 mPrevT = 0.0;
    std::vector<Drop> mDrops;
    LLPointer<LLViewerObject> mBoat;
    bool mGrabDecided = false;
    std::map<std::string, Decided> mDecided;
    F64 mNextCheck = 0.0, mLastIdle = 0.0;
    F64 mUtcAnchor = 0.0, mElapsedAnchor = -1.0;
    F64 mCostUs = 0.0;
    S32 mCostFrames = 0;
    F64 mLastLog = 0.0;

    LLPointer<LLVertexBuffer> mVB;
};

#endif // WOLF_KRAKEN_H
