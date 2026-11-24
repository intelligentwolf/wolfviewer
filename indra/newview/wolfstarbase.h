/**
 * @file wolfstarbase.h
 * @brief WolfViewer: Starfold's player starbases - what the region says about them, and the Starbase window's orders.
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

#ifndef WOLF_STARBASE_H
#define WOLF_STARBASE_H

#include <string>
#include <utility>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

// Source: region-terrain-work/space-game-2026-11-24/design.html#bases (Paul approved 24 Nov ~17:00Z) and the wire in that
// folder's README.md "Player starbases" (wolf-space/src/SpaceBases.cs BaseVerb, SendBase, SendBaseStatus, SendBasesList, Ghost;
// the own-builds verbs own/attach/send/place/putback/core added 24 Nov evening). The region decides everything: this keeps what it
// said and sends what the resident wants, `SF1|base|...` on the Starfold channel (WolfStarfold::intent). Nothing is polled: the
// window asks for its base when it opens (status=1), and a part's build time is a finish time worked out from the region's
// "seconds left" when it arrived ("Build time is never a countdown against you ... Nothing ticks on the server meanwhile").
class WolfStarbase : public LLSingleton<WolfStarbase>
{
    LLSINGLETON(WolfStarbase);
    ~WolfStarbase();

public:
    /** k=base kit=<kind>;<price>;<build minutes>;<prims>;<sx,sy,sz>;<lane m>;<title>: what can be bought here now. */
    struct Kit
    {
        std::string mKind, mTitle;
        S32 mPrice = 0, mMinutes = 0, mPrims = 0;
        S32 mSize[3] = { 1, 1, 1 };
        F32 mLane = 0.f;
    };
    /** k=bpart p=<part>;<kind>;<state>;<cx,cy,cz>;<rot>;<ready_s>[;<own>;<sx,sy,sz>;<powered>]. */
    struct Part
    {
        std::string mId, mKind, mState;   // state: building, fine, off, stored, paused
        S32 mCell[3] = { 0, 0, 0 };
        S32 mRot = 0;
        F64 mReadyAt = 0.0;               // this viewer's clock (LLFrameTimer seconds) when the build is done
        bool mOwn = false, mPowered = true;
        S32 mSize[3] = { 1, 1, 1 };
        bool building() const;            // still being built (its finish time not reached)
    };
    /** k=bases b=<id>;x,y,z;yaw;owner name;dock - every base here. */
    struct Known { std::string mId, mOwner, mDock; LLVector3 mPos; S32 mYaw = 0; };
    /** k=bghost g=cx,cy,cz,rot. */
    struct Ghost { S32 mCell[3] = { 0, 0, 0 }; S32 mRot = 0; };
    /** k=bown o=<id>;<kind>;<prims>;<metres>;<ok>;<name>;<reason>;<sx,sy,sz>. */
    struct Own
    {
        std::string mId, mKind, mName, mReason;
        S32 mPrims = 0;
        F32 mMetres = -1.f;
        bool mOk = false;
        S32 mSize[3] = { 1, 1, 1 };
    };

    /** A WolfSpace GenericMessage about bases (k=bases, base, bpart, bghost, bown); false when it is not one. */
    bool receive(const std::string& kind, const std::vector<std::pair<std::string, std::string>>& kv);
    /** k=msg: the region's answer. Kept for the window while it is open (the window shows the latest, never only as a note). */
    void noteAnswer(const std::string& text);
    /** A new region (or space gone): everything about the old one is forgotten. */
    void reset();
    /** Once a frame: a part finished while the window is open asks the region once (the celebration, the Working state). */
    void idle();

    // ── the window ───────────────────────────────────────────────────────────────────────────────────────────────────
    enum ETab { TAB_BASE = 0, TAB_ADD, TAB_STORE, TAB_OWN, TAB_COUNT };
    /** Wolf login, a region with space on: the window may open (the cockpit's BASE, the Station window, Wolf World). */
    bool available() const;
    bool shown() const { return mShown && available(); }
    void show(bool on);
    void toggle() { show(!shown()); }
    ETab tab() const { return mTab; }
    void setTab(ETab t);

    // ── what the region said ─────────────────────────────────────────────────────────────────────────────────────────
    bool known() const { return mKnown; }          // a k=base came since the window opened in this region
    bool hasBase() const { return mHas; }
    const std::string& baseId() const { return mId; }
    const LLVector3& basePos() const { return mPos; }   // region metres
    S32 baseYaw() const { return mYaw; }
    const std::string& baseState() const { return mState; }
    const std::string& dock() const { return mDock; }
    S32 primsUsed() const { return mPrimsUsed; }
    S32 primsMax() const { return mPrimsMax; }
    bool haveStarmarks() const { return mStarmarks >= 0; }
    S64 starmarks() const { return mStarmarks; }
    // no base here (has=0)
    bool regionOpen() const { return mOpen; }
    S32 regionCount() const { return mCount; }
    S32 regionCap() const { return mCap; }
    const std::string& regionPrims() const { return mRegionPrims; }
    S32 corePrice() const { return mCorePrice; }
    const std::vector<Kit>& kits() const { return mKits; }
    const Kit* kit(const std::string& kind) const;
    const std::vector<Part>& parts() const { return mParts; }
    const Part* part(const std::string& id) const;
    const std::vector<Known>& bases() const { return mBases; }
    bool basesOpen() const { return mBasesOn; }
    const std::vector<Own>& own() const { return mOwn; }
    bool ownHere() const { return mOwnHere; }
    bool ownAsked() const { return mOwnAsked; }
    const std::string& ownText() const { return mOwnText; }
    const std::string& answer() const { return mAnswer; }
    /** A part finished in the last few seconds (the window's short celebration), else empty. */
    std::string justFinished() const;

    // ── choosing a spot (ghosts) ─────────────────────────────────────────────────────────────────────────────────────
    /** What the ghosts are for: a kit kind ("dock"), "own:<object id>" or "part:<part id>"; empty when none are shown. */
    const std::string& ghostFor() const { return mGhostFor; }
    bool ghostsArrived() const { return mGhostsIn; }
    const std::vector<Ghost>& ghosts() const { return mGhosts; }
    const std::string& ghostText() const { return mGhostText; }
    /** The spots are the ghosts' distinct cells; each spot's turns are the ghosts at that cell. */
    S32 spotCount() const;
    S32 spot() const { return mSpot; }
    S32 turnCount() const;
    const Ghost* chosen() const;
    void nextSpot(S32 step);
    void nextTurn();
    /** Choose the ghost `i` (a click on it in the world). */
    void choose(size_t i);
    /** The ghosts' size in cells along the module's own X, Y and Z (a kit's, a part's, an own build's). */
    void ghostSize(S32 out[3]) const;
    /** The kit kind of what the ghosts are for (own builds and stored parts carry theirs), for the price and the title. */
    std::string ghostKind() const;
    void clearGhosts();

    /** Source: BaseRules.cs CELL_M - a cell of the frame is 10 m; cell (i,j,k) covers [10i, 10i+10] in the base's frame. */
    static constexpr F32 CELL_M = 10.f;

    // ── the geometry (Source: wolf-space BaseRules.cs CellTurn, BasePlacement.cs Footprint / PartPose) ─────────────────
    /** CellTurn.Apply: where the module's own cell offset (i, j, k) goes under turn `rot` (0..23). */
    static void turn(S32 rot, const S32 in[3], S32 out[3]);
    /** CellTurn.Front: the direction the module's front (its own +X) faces under `rot`. */
    static void front(S32 rot, S32 out[3]);
    /** The cells a module of `size` fills with its anchor at `at` turned by `rot` (BasePlacement.Footprint). */
    static std::vector<std::vector<S32>> footprint(const S32 size[3], const S32 at[3], S32 rot);
    /** A point in the base's own frame (metres) to region metres: the base's centre plus its quarter-turn yaw (PartPose). */
    LLVector3 toRegion(const LLVector3& local) const;

    // ── the orders (each one `SF1|base|...` unless it says otherwise) ─────────────────────────────────────────────────
    void askStatus();
    void found();
    void findSpot();
    void restore();
    void home();
    void flyHome();                 // SF1|fly|to=base (piloting only)
    void dockHere();                // SF1|dock|at=<id> (piloting only)
    void setDock(const std::string& who);   // all, friends, group, me
    void pack();
    void scrap(bool sure);
    void takeDown(const std::string& part);
    void putBack(const std::string& part);
    void buyCore(const std::string& part);
    /** "Make this a seat" (design.html#bases: own quarters need one seat; wolf-space 1.0.8 seat=<part>|prim=<link>): the prim the
     *  resident has selected with the build tools, by its link number (root 1). Says plainly when nothing is selected. */
    void makeSeat(const std::string& part);
    void showGhosts(const std::string& what);   // a kit kind, "own:<id>" or "part:<id>"
    void build();                   // add / attach / place at the chosen ghost
    void attachAsIs(const std::string& object);
    void placeAsIs(const std::string& part);   // a stored decoration: the region picks a free spot in the frame
    void sendToBase(const std::string& object);
    void askOwn();

private:
    bool order(const std::string& args, bool exempt = false);
    void parseBase(const std::vector<std::pair<std::string, std::string>>& kv);
    void parsePart(const std::vector<std::pair<std::string, std::string>>& kv);

    bool mShown = false;
    ETab mTab = TAB_BASE;
    LLUUID mRegion;                 // the region all this is about
    // k=base
    bool mKnown = false, mHas = false;
    std::string mId, mState, mDock, mRaids;
    LLVector3 mPos;
    S32 mYaw = 0, mPrimsUsed = 0, mPrimsMax = 0;
    S64 mStarmarks = -1;
    bool mOpen = false;
    S32 mCount = 0, mCap = 0, mCorePrice = 0;
    std::string mRegionPrims;
    std::vector<Kit> mKits;
    std::vector<Part> mParts;
    // k=bases
    std::vector<Known> mBases;
    bool mBasesOn = false;
    // k=bghost
    std::string mGhostFor;
    bool mGhostsIn = false;
    std::vector<Ghost> mGhosts;
    std::string mGhostText;
    S32 mSpot = 0, mTurn = 0;
    // k=bown
    std::vector<Own> mOwn;
    bool mOwnHere = false, mOwnAsked = false;
    std::string mOwnText;
    // answers and pacing
    std::string mAnswer;
    F64 mOrderedAt = -100.0;        // the last order that counts against the region's one-a-second
    F64 mAskedAt = -100.0;          // the last order of any kind (an answer within a while is the window's)
    std::vector<std::pair<std::string, F64>> mFinished;   // parts seen to finish: id, when
    F64 mReaskAt = -1.0;
};

#endif // WOLF_STARBASE_H
