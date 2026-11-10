/**
 * @file wolfleaves.h
 * @brief WolfViewer: a few leaves falling from the trees whose description says "wolftree" (ticket 718).
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

#ifndef WOLF_LEAVES_H
#define WOLF_LEAVES_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "wolfleavesseason.h"

#include <map>
#include <vector>

class LLVertexBuffer;

/**
 * [FALLING LEAVES 2026-11-10] Ticket 718, Jimmy Olsen: "A possibility of making subtle fallen leaves particles coming from the
 * trees in a region? Maybe an option, its set the tree up the effect with "wolftree" on description field? ... Also the
 * particles varies color accordingly to season, and depending on location selected, no particles at all at winter?"
 * Paul approved it on 10 Nov ("Do").
 * Persona: a resident walking through a Wolf park in autumn. Story: "Under the trees a few red and brown leaves drift down,
 * swing as they fall and settle on the grass; in the summer only the odd green one, in the winter none."
 *
 * Drawn by each viewer for itself (as the birds and the bubbles are): no objects, scripts or messages of its own. The same
 * rules and figures as WolfStorm's js/world/wolf_leaves.js; README region-terrain-work/falling-leaves-2026-11-10/README.md.
 *
 *  KINDS    a broadleaf drops leaves (oak-lobed or oval, coloured by the season); a conifer or other evergreen only the odd
 *           brown needle (Paul 2026-11-10). "wolftree pine" / "wolftree conifer" (and spruce, fir, cedar ...) or, failing a word
 *           in the description, the object's own name decides; plain "wolftree" is a broadleaf (wolfleavesseason.h treeKind).
 *  TREES    any prim (not an attachment) whose description contains KEYWORD, case-insensitive, within TREE_REACH_M of the
 *           camera, nearest MAX_TREES. The description comes from WolfObjectProps, the store wolfwater already fills for
 *           every prim near the camera (no new kind of request: want() is idempotent, and a prim within reach of the leaves
 *           is already wanted by FSWolfWater::sweep). A linkset root carrying the word brings its whole linkset's box.
 *  WHEN     Wolf regions (visitors too, WolfGrid::showsWolfRegion), the resident's own WolfViewerFallingLeaves (World > Wolf
 *           World > Falling Leaves, on by default), not while snow falls on screen (WolfWeather), and the season
 *           (wolfleavesseason.h: the region's place and the day its sky shows, from weather.php's autoEnv block): none in
 *           winter.
 *  LEAVES   born in the top half of the tree's box, they fall at a leaf's slow speed swinging from side to side and rocking
 *           as they go, carried a little by the region's wind; they settle on the ground, the water or the tree's own base
 *           (a tree on a platform drops them on the platform), lie a few seconds and shrink away.
 *  COST     at most WolfViewerFallingLeavesMax (lowered by WolfFrameGovernor) leaves, one vertex buffer, one draw call in
 *           the deferred pass; the trees are looked for every SWEEP_S, nothing per frame but the leaves themselves.
 */
class WolfLeaves : public LLSingleton<WolfLeaves>
{
    LLSINGLETON(WolfLeaves);
    ~WolfLeaves();

public:
    /** The keyword, matched case-insensitively anywhere in a description. */
    static const std::string KEYWORD;
    /** Does this (already lower-cased) description ask for leaves? */
    static bool matches(const std::string& description_lower);

    /** Once a frame (llappviewer.cpp idle, beside WolfSwimBubbles): the trees, the season, new leaves, falling, settling. */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfSwimBubbles): the leaves as lit opaque geometry. */
    void renderDeferred();
    /** How many leaves there are now (for the log and tests). */
    S32 count() const { return (S32)mLeaves.size(); }

private:
    struct Tree
    {
        LLUUID mId;
        LLVector3 mMin, mMax;   // agent metres: the box of the prim (and its linkset when it is the root)
        F32 mSize = 1.f;        // how many leaves it drops compared with a tree of TREE_AREA_M2 under its crown
        F32 mDist = 0.f;        // from the camera when last swept
        bool mWhole = false;    // a one-prim tree or a linkset root: its box reaches the trunk's foot (a floor for leaves)
        WolfLeavesSeason::TreeKind mKind = WolfLeavesSeason::BROADLEAF;   // a conifer drops the odd brown needle only
    };

    struct Leaf
    {
        LLVector3 mCentre;      // agent metres: the point the leaf swings about as it falls
        LLVector3 mPos;         // drawn position this frame
        LLVector3 mSwing;       // unit, horizontal: the way it swings
        F32 mFall = 1.f;        // m/s down
        F32 mAmp = 0.3f;        // swing, metres either side
        F32 mOmega = 2.f;       // swing, radians a second
        F32 mPhase = 0.f;
        F32 mYaw = 0.f;         // which way the leaf points, radians
        F32 mYawRate = 0.f;     // its slow turn, radians a second
        F32 mLen = 0.1f, mWid = 0.06f;
        F32 mFloor = 0.f;       // agent z it cannot fall below (the tree's base)
        F32 mGround = 0.f;      // agent z it settles at here (ground, water or floor), as last sampled
        F64 mGroundT = 0.0;     // when that was sampled
        F32 mAge = 0.f;
        F32 mRest = 0.f;        // seconds lying still left (while settled)
        F32 mShrink = 1.f;      // 1 .. 0 as it goes
        bool mSettled = false;
        F32 mTilt = 0.f;        // lying: a small tilt so not every leaf is flat
        U8 mColour[3] = { 0, 0, 0 };
        U8 mShape = WolfLeavesSeason::OAK;   // WolfLeavesSeason::Shape
    };

    void reset();
    void rebase(const LLVector3& delta);
    void sweep(F64 now);
    void updateSeason(F64 now);
    void spawn(const Tree& t);
    F32 groundAt(const LLVector3& p, F32 floor) const;
    void move(F32 dt, F64 now);
    bool ensureBuffer();
    void buildMesh(S32& verts);

    std::vector<Tree> mTrees;
    std::map<LLUUID, F32> mCarry;   // per tree: the part of a leaf owed (emission accumulates across frames)
    std::vector<Leaf> mLeaves;
    std::vector<S32> mVisible;
    WolfLeavesSeason::Season mSeason;
    bool mSnowing = false;
    LLVector3d mLastOrigin;
    F64 mLastIdle = 0.0;
    F64 mNextSweep = 0.0;
    F64 mNextSeason = 0.0;
    F64 mLastLog = 0.0;
    S32 mLastSeasonKind = -1;
    // the minute's figures for the log line
    F64 mCostIdleUs = 0.0, mCostMeshUs = 0.0;
    S32 mCostFrames = 0, mPeakLeaves = 0, mPeakTrees = 0, mSwept = 0, mKnown = 0;

    LLPointer<LLVertexBuffer> mVB;
};

#endif // WOLF_LEAVES_H
