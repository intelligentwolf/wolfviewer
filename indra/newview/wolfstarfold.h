/**
 * @file wolfstarfold.h
 * @brief WolfViewer: Starfold - the space game above opted-in Wolf regions (the viewer's side).
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

#ifndef WOLF_STARFOLD_H
#define WOLF_STARFOLD_H

#include <deque>
#include <string>
#include <vector>

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3color.h"
#include "v3math.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;
class LLViewerTexture;
class WolfStarfoldBeamSource;

// Paul, 24 Nov 2026: Starfold, our own space game - "space high above regions whose owner opts in, ships, a station, an
// asteroid belt, Glasswing aliens with lasers, and wormholes". The design is region-terrain-work/space-game-2026-11-24/
// design.html (approved 24 Nov); the wire is that folder's README.md and the region's side is the wolf-space plugin
// (wolf-space/src/SpaceEngine.cs, SpaceRules.cs). The region is the authority: this viewer sends what the pilot WANTS and
// draws what the region SAYS; it never sends a position, a hit, a balance or a price.
//
//   viewer -> region  avatar chat on channel -73653 (SpaceRules.CHANNEL), "SF1|<verb>|k=v|..." (SpaceRules.INTENT_PREFIX;
//                     SpaceEngine.OnChat): hello, launch, ctl|t=|p=|y=|r=|sy=|sz=|m=s/d (only when an input changes),
//                     fire|on=1|tg=<alien root>, fire|on=0, target, mine|on=1, mine|on=0, fly|to=station/belt/wormhole/stop,
//                     dock, sell, leave. At most SpaceRules.MAX_INTENTS_PER_S (20) a second: the region drops the rest.
//   region -> viewer  GenericMessage "WolfSpace" (SpaceRules.METHOD), one "key=value" per parameter (SpaceEngine.SendSystem,
//                     Status, Beam, SendDocked, Disable, Jump, Mine, Refill, KillAlien; SceneSpaceWorld.Cut: each cut at
//                     250 characters): k=sys, k=st, k=beam, k=rock, k=boom, k=docked, k=msg, k=end, k=jump.
//
// What is DRAWN follows the region underfoot (WolfGrid::showsWolfRegion: Hypergrid visitors see the sky too; Paul 24 Nov);
// what is SENT and the cockpit need a Wolf login (WolfGrid::isOnWolfTerritoriesConfirmed), as every Wolf tool. Off the Wolf
// grid nothing is read, drawn, sent or watched.
//
// The game's name is said ONLY by gameName() (Paul 24 Nov: one constant per part, so a rename takes minutes); the wire never
// carries it.
class WolfStarfold : public LLSingleton<WolfStarfold>
{
    LLSINGLETON(WolfStarfold);
    ~WolfStarfold();

public:
    /** The game's name, its currency, the ship and the alien, for everything a resident reads. */
    static const char* gameName() { return "Starfold"; }
    static const char* currency() { return "Starmarks"; }

    // ── what the region said ─────────────────────────────────────────────────────────────────────────────────────────
    /** k=sys (SpaceEngine.SendSystem). Positions are the region's own metres. */
    struct System
    {
        bool        mHave = false;
        LLUUID      mRegion;            // the region that said it (the message's invoice)
        bool        mOn = false;
        F32         mHeight = 0.f;      // the space line, region metres
        U32         mSeed = 1;          // BeltSeed(region id): the belt, the planet and the sky's look
        bool        mHaveBelt = false;
        LLVector3   mBeltCentre;
        F32         mBeltR = 0.f, mBeltW = 0.f, mBeltT = 40.f;
        S32         mBeltN = 0;
        bool        mHaveStation = false;
        LLVector3   mStation;
        bool        mHaveWormhole = false;
        LLVector3   mWormhole;
        std::string mWormholeTo;
    };
    /** k=st (SpaceEngine.Status): this avatar's ship, twice a second while it flies. */
    struct Ship
    {
        LLUUID      mRoot;
        std::string mClass;
        F32         mShield = 0.f, mArmour = 0.f, mHull = 0.f;
        F32         mShieldMax = 100.f, mArmourMax = 100.f, mHullMax = 100.f;   // SpaceRules.Cub
        F32         mSpeed = 0.f, mThrottle = 0.f;
        bool        mDrift = false, mAir = false, mSafe = false;
        F32         mHold = 0.f, mHoldMax = 0.f;
        S64         mCredits = 0;
        LLUUID      mTarget;            // tg=: the Glasswing the ship fires at
        F32         mTargetM = 0.f, mTargetShield = 0.f, mTargetArmour = 0.f, mTargetHull = 0.f;
        std::vector<LLUUID> mAliens;    // al=: the Glasswings near, nearest first
        // [TARGET BOX 2026-11-24] ap= / tgp= (wolf-space SpaceEngine.Status, README wire k=st): their region positions, so the
        // radar and the target box work beyond the objects this viewer has been sent (the far clip decides those: at the normal
        // 128-256 m a Glasswing 600 m off is not in the object list)
        std::vector<LLVector3> mAlienPos;   // the same order as mAliens; empty when the region did not send ap=
        bool        mHaveTargetPos = false;
        LLVector3   mTargetPos;             // region metres
    };
    struct Ore { std::string mId, mTitle; S32 mPrice = 0; F32 mHeld = 0.f; };
    /** k=docked (SpaceEngine.SendDocked). */
    struct Station
    {
        bool             mDocked = false;
        S64              mCredits = 0;
        std::vector<Ore> mOres;
    };
    struct Rock { LLVector3 mPos; F32 mSize = 0.f; U8 mOre = 0; bool mLeft = true; };
    /** A beam the region said is on (k=beam on=1): its shooter, its target (or a rock), its kind ('p', 'a', 'm'). */
    struct Beam { LLUUID mFrom, mTo; char mKind = 'p'; S32 mRock = -1; F64 mSince = 0.0; };
    struct Note { std::string mText; F64 mAt = 0.0; };
    /** k=setup (SpaceSetup.cs SendSetup), to the owner only: what is in place, the space height, on, the next step. */
    struct Setup
    {
        bool mHave = false;
        std::vector<std::string> mHas;   // of "station", "belt", "wormhole", "pad"
        F32  mHeight = 0.f;
        bool mOn = false;
        std::string mNext;               // the first missing step, or "done"
        bool has(const std::string& part) const;
    };

    /** GenericMessage "WolfSpace" from a region (invoice = that region's id). */
    void receive(const LLUUID& region_id, const std::vector<std::string>& strings);
    /** Once a frame (llappviewer.cpp idle): hello, the controls, sounds, the beams' source. */
    void idle();

    // ── where we are ─────────────────────────────────────────────────────────────────────────────────────────────────
    /** The region underfoot has space on (its last k=sys said on=1), and it is a Wolf region this viewer may draw on. */
    bool spaceHere() const;
    const System& system() const { return mSys; }
    /** This avatar flies a ship (the region's k=st came for it, and no k=end since). */
    bool piloting() const;
    const Ship& ship() const { return mShip; }
    const Station& station() const { return mStation; }
    const std::vector<Rock>& rocks() const { return mRocks; }
    const std::vector<Beam>& beams() const { return mBeams; }
    const std::deque<Note>& notes() const { return mNotes; }
    /** The space line above the sea (metres; -1 when there is no space here). */
    F32 lineAboveSea() const;
    /** The camera's height above the sea, metres. */
    static F32 cameraAboveSea();
    /** 0 below the line's 300 m edge, 1 at the line and above (README "client look rules": s). */
    F32 spaceFade() const;

    // ── the sky (README "client look rules") ─────────────────────────────────────────────────────────────────────────
    /** The height the altitude sky reads (wolfaltitudesky.cpp): `a` itself unless space is here, else remapped to 50 km at the line. */
    F32 skyAltitude(F32 a) const;
    /** How much of the air above the camera is gone (0..1): wolftk/skyF.glsl uWolfVacuum. */
    F32 skyVacuum() const;
    /** The highest a cloud layer's top may reach, metres above the sea; -1 none (wolfenvironmentrender.cpp). */
    F32 cloudCapAboveSea() const;
    /** The nebula's colour and strength (0..1) for the Wolf sky (wolfenvironmentrender.cpp uNebula / uNebulaColor). */
    F32 nebula(LLColor3& colour) const;
    /** The planet's uniforms (wolfStarfoldSkyF.glsl) on a sky program; `lum` the sky's units for a white surface in full sun. */
    void bindSkyUniforms(LLGLSLShader& shader, F32 lum, const LLVector3& sun_dir_world) const;
    /** [STARFOLD 2026-11-24, Paul: "i was expecting to be actually in space"] the space itself for the Wolf dome (wolftk/skyF.glsl
     *  sf_space_on, sf_space_map): the fade above the line (0 in a reflection-probe face) and the all-sky map of the Wolf space sky
     *  (WolfSpaceSky::paintSphere, made the first time it is wanted) on texture unit `unit`. The sampler is set to `unit` on every
     *  call, drawn or not, so it never sits on unit 0 beside another sampler type (wolfenvironmentrender.cpp "THE MAGENTA SKY"). */
    void bindSpaceSky(LLGLSLShader& shader, S32 unit);
    /** The stars' brightness above the line by day, in the frame's units (WolfEnvironmentRender::starTint): 0 below the line's edge. */
    F32 spaceStarFloor() const;

    // ── drawing ──────────────────────────────────────────────────────────────────────────────────────────────────────
    /** The belt's rocks as lit geometry (pipeline.cpp, the deferred pass beside the birds). */
    void renderDeferred();
    /** The wormhole and the jump's tunnel over the finished frame (pipeline.cpp renderFinalize). True when it drew dst. */
    bool renderLens(LLRenderTarget* src, LLRenderTarget* dst, LLVertexBuffer* screen_triangle);
    /** A rock's or an object's place in agent space now; false when it is not known. */
    bool beamEnd(const LLUUID& id, S32 rock, LLVector3& agent) const;
    /** A Glasswing's place in agent space: its object when this viewer has it, else the region's last word (k=st ap= / tgp=). */
    bool alienAt(const LLUUID& id, LLVector3& agent) const;

    // ── the pilot's intents ──────────────────────────────────────────────────────────────────────────────────────────
    bool canLaunch() const;         // a Wolf login, space here, not flying
    void launch();
    /** wolf-space 1.0.4 "goto": the region takes the avatar to the nearest pad ("pad") or onto the station deck ("station"),
     *  past the landing point; it answers by k=msg. Not while flying (the region says to use Fly there). */
    void goTo(const std::string& where);
    void fire(bool on);
    bool firing() const { return mFireWanted; }
    void nextTarget();
    void selectTarget(const LLUUID& alien) { mSelected = alien; }
    const LLUUID& selected() const { return mSelected; }
    void mine(bool on);
    bool mining() const;
    void flyTo(const std::string& where);   // "station", "belt", "wormhole", "stop", "base" (the pilot's own starbase)
    /** An intent from another Starfold part (the Starbase window, wolfstarbase.cpp): "base|...", "dock|at=...". Sent as every
     *  intent is (a Wolf login, the one-second budget). */
    void intent(const std::string& verb_and_args);
    const std::string& flyingTo() const { return mFlyTo; }
    void dock();
    void sell();
    void leave();
    void setDrift(bool drift);
    bool drift() const { return mDrift; }
    /** The throttle the pilot set: 0..1 in Steady, -1..1 in Drift (the cockpit's lever and the keys). */
    void setThrottle(F32 t);
    F32  throttle() const { return mThrottle; }
    /** Point and fly (the cockpit, the mouse held on the world): -1..1 each, 0 when let go. */
    void setPointer(bool held, F32 pitch, F32 yaw);
    /** Close the Station window (the ship stays docked: the next Launch leaves from the station). */
    void closeStation() { mStationShown = false; }
    bool stationShown() const { return mStation.mDocked && mStationShown && spaceHere(); }
    void showStation() { mStationShown = true; }

    // ── the setup helper (the owner's; SpaceSetup.cs): it works while space is off too ─────────────────────────────────────
    /** The region has the space plugin (it answered with k=sys) and this avatar may manage its estate: the helper is offered. */
    bool canSetUp() const;
    bool setupShown() const { return mSetupShown && canSetUp(); }
    void toggleSetup();
    void closeSetup() { mSetupShown = false; }
    const Setup& setup() const { return mSetup; }
    void setupRez(const std::string& part);   // "station", "belt", "wormhole", "pad"
    void setupClear();

    // ── the cockpit's own state ──────────────────────────────────────────────────────────────────────────────────────
    /** Wolf World > Starfold: the cockpit shown (flying), or the ground panel (Launch, the guide). */
    bool panelShown() const;
    void togglePanel();
    bool panelHidden() const { return mHidden; }
    bool guideShown() const { return mGuide; }
    /** Closing the guide by any way (X, DONE, the ? button, WolfStarfold.Do guide:0) counts as read: it no longer opens by
     *  itself on the next ship (WolfStarfoldGuideSeen). */
    void showGuide(bool on);
    S32  guideCard() const { return mGuideCard; }
    void setGuideCard(S32 c) { mGuideCard = c; }
    /** The jump's tunnel: 0..1. */
    F32 tunnel() const;

    /** The space keys while piloting (llviewerinput.cpp scanKey, after the UI's own keys). True: the key is the cockpit's
     *  and is not passed on to its sitting binding (the arrows and W/S would otherwise swing the camera round the seat). */
    bool noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat);

private:
    void say(const std::string& verb_and_args, bool control = false);
    bool sendAllowed(bool control);
    void parseSystem(const LLUUID& region_id, const std::vector<std::pair<std::string, std::string>>& kv);
    void parseStatus(const std::vector<std::pair<std::string, std::string>>& kv);
    void parseBeam(const std::vector<std::pair<std::string, std::string>>& kv);
    void parseDocked(const std::vector<std::pair<std::string, std::string>>& kv);
    void makeBelt();
    void addNote(const std::string& text);
    void endShip();
    void sendControls();
    void sounds();
    void claimBottom();
    void resetRegion();
    LLUUID agentRegionId() const;

    System  mSys;
    Ship    mShip;
    Station mStation;
    std::vector<Rock> mRocks;
    bool    mRocksDirty = true;      // the rocks' vertex buffer must be written again
    std::vector<Beam> mBeams;
    std::deque<Note>  mNotes;

    bool    mPiloting = false;
    F64     mLastStatusAt = -100.0;
    LLUUID  mHelloRegion;            // the region a hello went to (or a k=sys came from)
    F64     mArrivedAt = -100.0;
    LLUUID  mLastRegion;

    // the pilot's inputs and what was last sent
    F32     mThrottle = 0.f;
    bool    mDrift = false;
    F32     mKeyPitch = 0.f, mKeyYaw = 0.f, mKeyRoll = 0.f, mKeySlideY = 0.f, mKeySlideZ = 0.f;
    bool    mThrUp = false, mThrDown = false;
    bool    mPointerHeld = false;
    F32     mPointerPitch = 0.f, mPointerYaw = 0.f;
    F32     mSent[7] = { -9.f, -9.f, -9.f, -9.f, -9.f, -9.f, -9.f };   // t p y r sy sz m
    F64     mSentAt = -100.0;
    std::deque<F64> mSendTimes;      // the last second's intents
    bool    mFireWanted = false;
    bool    mMineWanted = false;
    LLUUID  mSelected;
    std::string mFlyTo;
    // held keys (KEY codes) this frame: the scan keeps each true until its key goes up
    bool    mKeyHeld[16] = {};
    KEY     mKeyOf[16] = {};         // the key holding each
    LLUUID  mAskedRegion;            // the hello: where, when, how many times
    F64     mAskedAt = -100.0;
    S32     mAsks = 0;
    F64     mThrottleKeyAt = 0.0;

    // UI
    bool    mHidden = false;         // flying, hidden by the pilot
    bool    mByHand = false;         // opened from Wolf World while not flying
    bool    mGuide = false;
    S32     mGuideCard = 0;
    bool    mStationShown = true;
    Setup   mSetup;
    bool    mSetupShown = false;

    // the jump
    F64     mJumpAt = -100.0;
    LLUUID  mJumpFromRegion;
    bool    mJumpTpSeen = false;     // a teleport began after k=jump
    F64     mJumpEndAt = -1.0;       // the tunnel fades from here: no teleport began, or it ended without landing

    // sounds
    S32     mSoundState = 0;         // 0 not written, 1 written, -1 failed
    F64     mHumAt = -100.0, mHitAt = -100.0;

    // drawing
    LLPointer<WolfStarfoldBeamSource> mBeamSource;
    LLPointer<LLVertexBuffer> mRockVB;
    U32     mRockVerts = 0;
    LLVector3 mRockOrigin;           // the region's agent origin the buffer was written for
    LLPointer<LLViewerTexture> mSpaceMap;   // bindSpaceSky: the all-sky map, made once
};

#endif // WOLF_STARFOLD_H
