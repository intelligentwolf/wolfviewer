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
#include "v3dmath.h"
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
    static const char* gameName() { return "WolfTrek"; }   // [WOLFTREK 2026-11-25] Paul: residents read WolfTrek ("Starfold" is a Steam game); code names stay Starfold
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
        // [WOLFTREK v11 2026-11-25] SpaceEngine.SendSystem sn= (the station's name, only once AlphaWolf or the owner gave it one)
        // and bay=1 (the station has a fly-in bay: SpaceBay.cs ReadBay found bay_pad_<n> prims and a bay_mouth)
        std::string mStationName;
        bool        mBay = false;
        // [WOLFTREK v11 stage D] SpaceEngine.SendSystem whs= (SpaceWormholes.cs WhsField): stability 0-100, size s|l, minutes to live
        // (-1 never), closing; without it (an older region, or the owner's own named link: "100;l;-1;0") a stable large hole
        S32         mWhStab = 100;
        char        mWhSize = 'l';
        S32         mWhLife = -1;
        bool        mWhClosing = false;
    };
    /** [WOLFTREK v11 stage D] The region's wandering wormhole as k=sig told this pilot (SpaceWormholes.cs SendSig): its id (16 hex,
     *  AlphaWolf space.rs), its place (rounded to 200 m until found), found, and the region it leads to (once found). */
    struct Signal { bool mHave = false; std::string mId; LLVector3 mPos; bool mFound = false; std::string mRegion; };
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
        // [WOLFTREK v11 2026-11-25] SpaceEngine.Status: cid= the class id (always), seq=in|out while the bay's automatic approach or
        // departure flies the ship (SpaceBay.cs StartSeq .. EndSeq / CancelSeq); empty when the pilot has the controls
        std::string mClassId;
        std::string mSeq;
    };
    struct Ore { std::string mId, mTitle; S32 mPrice = 0; F32 mHeld = 0.f; };
    /** [WOLFTREK v11 stage B] One class of the fleet as k=docked lists it (SpaceFleet.cs FleetArgs): fleet=<id>;<title>;<price>;<owned>;
     *  <active>;<top speed m/s>;<turn deg/s>;<hold>;<shield>;<armour>;<hull>;<guns>;<mass>;<pad m>;<role> (the role the rest). */
    struct ShipInfo
    {
        std::string mId, mTitle, mRole;
        S64  mPrice = 0;
        bool mOwned = false, mActive = false;
        F32  mSpeed = 0.f, mTurn = 0.f, mHold = 0.f, mShield = 0.f, mArmour = 0.f, mHull = 0.f, mMass = 0.f, mPad = 0.f;
        S32  mGuns = 0;
    };
    /** [WOLFTREK v11 stage C] A saved design (SpaceShipDesigns.cs DesignArgs): design=<id: a Guid>;<name>;<class id>;<prims>;<active>. */
    struct Design { std::string mId, mName, mClass; S32 mPrims = 0; bool mActive = false; };
    /** k=bship (SpaceShipDesigns.cs OwnShipsList): o=<object, 32 hex>;<class or ->;<prims>;<ok 1|0>;<name>;<reason (the rest)>. */
    struct Build { std::string mId, mClass, mName, mReason; S32 mPrims = 0; bool mOk = false; };
    struct Builds { bool mAsked = false, mAsking = false; std::vector<Build> mItems; std::string mText; };
    /** k=docked (SpaceEngine.SendDocked). */
    struct Station
    {
        bool             mDocked = false;
        S64              mCredits = 0;
        std::vector<Ore> mOres;
        // [WOLFTREK v11 2026-11-25] SpaceEngine.SendDocked: sn= the station's name, ship= and pad= the pilot's ship on its pad
        // (only when it stands on a pad, not when it went below by the lift)
        std::string      mName;
        LLUUID           mShip;
        S32              mPad = -1;
        std::vector<ShipInfo> mFleet;   // [WOLFTREK v11 stage B] the Shipyard's classes, in its order (none: no Shipyard here)
        std::vector<Design>   mDesigns; // [WOLFTREK v11 stage C] the pilot's saved designs
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
    const Signal& signal() const { return mSig; }
    /** [WOLFTREK v11 stage D] SCAN: SF1|scan|on=1 (SpaceWormholes.cs StartScan: in space; k=scan p=0..100 [found=<id>]). */
    void startScan();
    bool scanning() const { return mScanP >= 0; }
    S32  scanProgress() const { return mScanP; }
    /** The last scan's result in words (the region's k=msg after p=100, else ours), kept until the next scan. */
    const std::string& scanResult() const { return mScanResult; }
    /** A wormhole's state in words and colour (design.html#wormholes2: cyan stable, amber over half its mass carried, red close to
     *  closing; always in words too), as WolfStorm: "Stable" (> 50), "Over half spent" (11-50), "Nearly spent" (<= 10), "Closing". */
    static std::string wormholeWords(S32 stab, bool closing);
    static LLColor3 wormholeColour(S32 stab, bool closing);
    /** The wormhole in one plain sentence (the radar's tooltip), as WolfStorm's whDescribe. */
    static std::string wormholeSentence(S32 stab, char size, S32 life, bool closing, const std::string& region);
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
    /** [WOLFTREK v11] The bay's automatic approach (seq=in) or departure (seq=out) flies the ship: the keys and the pointer send
     *  nothing (SpaceEngine.OnChat ignores them during it; STOP during an approach hands the controls back). */
    bool automatic() const { return piloting() && !mShip.mSeq.empty(); }
    bool automaticIn() const { return automatic() && mShip.mSeq == "in"; }
    /** The pad the region gave this pilot for the approach or departure (k=bay ph=in|out pad=; 0 the lift, -1 not said). */
    S32 bayPad() const { return mBayPad; }
    /** STOP during the automatic approach: SF1|fly|to=stop (SpaceEngine.OnChat -> SpaceBay.cs CancelSeq). */
    void stopApproach();
    /** The station's name as the region last said it (k=docked sn=, else k=sys sn=); empty while it has none. */
    const std::string& stationName() const;
    /** [WOLFTREK v11] RENAME STATION (the Station window): the region's owner or an estate manager (SpaceNames.cs RenameStation
     *  checks W.IsRegionOwner; the button follows the setup helper's rule, LLViewerRegion::canManageEstate). */
    bool canRenameStation() const;
    /** SF1|name|station=<name> (SpaceEngine.OnChat "name"); the name is checked first by checkName. */
    void renameStation(const std::string& name);
    /** The region's last answer to a station rename while the Station window is open ("Renaming..." until it comes). */
    const std::string& stationAnswer() const { return mStationAnswer; }
    /** Source: NameFilter.cs Check - the same character rules, so a name the region would refuse for its letters is said at
     *  once: spaces folded, 3 to 32 characters, letters, digits, spaces, apostrophes and hyphens, at least two letters. The word
     *  filter stays the region's. Returns the tidy name, or empty with `why` the plain reason. Any character above ASCII is left
     *  to the region (its char.IsLetterOrDigit is Unicode-wide), so this never refuses what the region would take. */
    static std::string checkName(const std::string& raw, std::string& why);
    /** [WOLFTREK v11 stage B] The fleet the region last listed (k=docked fleet=), kept while flying for the gauges; empty when none. */
    const std::vector<ShipInfo>& fleet() const { return mFleet; }
    /** SF1|ship|buy=<id> / SF1|ship|use=<id> (SpaceFleet.cs ShipVerb, docked only); the answer is a k=msg, kept in the Station window. */
    void buyShip(const std::string& id);
    void useShip(const std::string& id);
    /** [WOLFTREK v11 stage C] SpaceShipDesigns.cs DesignVerb: own=1 (answered by k=bship), register=<object>, use=design:<id>,
     *  forget=<id>. The answers are k=msg in the Station window's answer line; a fresh k=docked follows a success. */
    void listOwnShips();
    void registerShip(const std::string& object);
    void useDesign(const std::string& id);
    void forgetDesign(const std::string& id);
    const Builds& builds() const { return mBuilds; }
    /** A class's title by its id: the fleet= line's, else SpaceRules.cs's five, else the id. */
    std::string classTitle(const std::string& id) const;
    /** A message of the viewer's own for the Station window's answer line (it stays until the next order or answer). */
    void stationSay(const std::string& text) { mStationAnswer = text; }
    /** [WOLFTREK v11] The hangar view: this pilot's docked ship framed on its pad, turning slowly, until the avatar or the camera
     *  moves (design.html#bay "The camera frames your ship on its pad, as when you look at an object, and turns slowly round it
     *  until you move"). */
    bool hangarView() const { return mHangarOn; }
    /** One line for a LEAP rig (WolfStarfold.Do "state"): the banner, the hangar view, the camera mode, the station's name. */
    void logState() const;
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
    /** [WOLFTREK v11 2026-11-25] A key the cockpit uses while piloting (W S Q E Z M, Space, Tab, the arrows, Page Up / Down,
     *  no modifier but Shift). llviewerwindow.cpp handleKey: "letter keys start chat" (LetterKeysAffectsMovementNotFocusChatBar
     *  off, as Firestorm mode sets it) does not take these from the cockpit. */
    bool cockpitKey(KEY key, MASK mask) const;
    /** [WOLFTREK v11 2026-11-25] Once a frame, just before the AgentUpdate is sent (llappviewer.cpp idle): the movement the
     *  user's keys, the joystick or a "go here" autopilot asked for this frame. LLAgent::resetControlFlags clears them right
     *  after the send, before idle() runs, so the hangar view reads them from here. */
    void noteAgentControls();

private:
    void say(const std::string& verb_and_args, bool control = false);
    bool sendAllowed(bool control);
    void parseSystem(const LLUUID& region_id, const std::vector<std::pair<std::string, std::string>>& kv);
    void parseStatus(const std::vector<std::pair<std::string, std::string>>& kv);
    void parseBeam(const std::vector<std::pair<std::string, std::string>>& kv);
    void parseDocked(const std::vector<std::pair<std::string, std::string>>& kv);
    void parseBay(const std::vector<std::pair<std::string, std::string>>& kv);
    void startHangar(const LLUUID& ship);
    void beginHangarCamera();
    void hangarIdle();
    void endHangar(const std::string& why, bool walked = false);
    bool hangarFocusIsShip() const;
    void aimHangarCamera();
    static U32 hangarMoveFlags();
    static std::string flagNames(U32 flags);
    void releaseKeys();
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
    std::string mStationAnswer;      // the region's answer to a station rename or a ship order (k=msg), shown in the Station window
    std::vector<ShipInfo> mFleet;    // [WOLFTREK v11 stage B] the last k=docked fleet= lines
    Builds  mBuilds;                 // [WOLFTREK v11 stage C] the last k=bship (your ship builds nearby)
    F64     mStationAskedAt = -100.0;

    // [WOLFTREK v11] the bay
    S32     mBayPad = -1;            // k=bay ph=in|out pad=
    LLUUID  mLastShip;               // the root of the ship this avatar last flew here (k=bay ph=landed for it frames it)
    // the hangar view
    bool    mHangarOn = false;
    LLUUID  mHangarShip;             // the ship framed, or waited for (its object not here yet)
    LLUUID  mHangarDoneFor;          // framed once per docking: k=docked comes again on every sale (cleared by a launch, the lift, a new region)
    F64     mHangarWaitUntil = -1.0; // waiting for the ship's object until then
    F64     mHangarAt = 0.0;         // the view began
    LLVector3d mHangarFocus;         // the ship's middle, global
    bool    mHangarSettled = false;  // the camera has reached the view: its distance, height and bearing are the baseline
    F32     mHangarDist = 0.f, mHangarElev = 0.f, mHangarAz = 0.f;   // expected from the baseline on (the bearing grows as it turns)
    LLVector3d mHangarAvatarAt;      // where the avatar stands (a jump of more than 1 m: the region placed it: hold and aim again)
    F64     mHangarHoldUntil = 0.0;  // nothing but a new key, a launch or the ship ends the view before this
    F64     mHangarMeasureAt = 0.0;  // the camera has reached the aimed view by then: the baseline is taken
    bool    mHangarAim = false;      // aim at the ship again when the hold ends
    U32     mHangarKeys0 = 0;        // movement keys already down when the view began (from the cockpit): not the user's new input
    F64     mHangarKeysSeenAt = 0.0; // when one of those was last seen down
    U32     mAgentKeys = 0;          // this frame's movement flags as sent (noteAgentControls): the user's own input
    bool    mHangarTurnLogged = false;   // the avatar's own body-turn flags were seen (logged once a view: not the user's)

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
    // [WOLFTREK v11 stage D] the wandering wormhole and the scan
    Signal  mSig;
    S32     mScanP = -1;             // -1 not scanning, else the region's last p=
    F64     mScanAt = -100.0;        // the scan was asked for / last heard
    bool    mScanHeard = false;      // a k=scan came for this scan
    F64     mScanEndAt = -100.0;     // p=100 came: the region's k=msg just after it is the result
    std::string mScanResult;
};

#endif // WOLF_STARFOLD_H
