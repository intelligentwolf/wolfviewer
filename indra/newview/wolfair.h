/**
 * @file wolfair.h
 * @brief WolfViewer: Wolf Air - the link to a warplane's helm (or a Wolf Dogfight kit) for the flight deck's dogfight band.
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

#ifndef WOLF_AIR_H
#define WOLF_AIR_H

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4color.h"

// Paul, 2026-11-01: "our next project is aircraft, being able to do dogfights in the sky".
//
// The protocol is region-terrain-work/aircraft-dogfight-2026-11-01/a-research/DESIGN.md s2 ("HUD protocol WolfAir1"),
// the ships' WolfHelm1 (wolfhelm.cpp) for aircraft:
//   plane -> viewer  llRegionSayTo(avatar, 0, "WolfAir1|<kind>|k=v|...") - kinds hi, st, hit, kill, down, respawn - only to
//                    a viewer that said hello while seated on her. Taken out of chat here (onObjectChat, called from
//                    llviewermessage.cpp) when it comes from the object the agent sits on.
//   viewer -> plane  chat SHOUTED on the plane's channel (7 until her "hi" names her own): "hud hello 1", "hud bye",
//                    "hud fire on" / "hud fire off", "hud dogfight on" / "hud dogfight off", "hud bail".
//   region -> viewer GenericMessage "WolfAir" (DESIGN.md s3.8, the wolf-air plugin): e=kill|k=<killer>|v=<victim>|w=<weapon>,
//                    the kill feed for every Wolf viewer on the region.
//                    [WOLF AIR RADAR 2026-11-02] e=contacts|s=|p=|me=|mv=|dk=|rch=|c=... the air radar's contacts, only to the
//                    people aboard an aircraft in dogfight mode (wolf-air 1.7.4, c-wolf-air/src/AirRadar.cs; README "1.7.4").
// Wolf Territories only: off it nothing is said, nothing is read and the band does not show.
class WolfAir : public LLSingleton<WolfAir>
{
    LLSINGLETON(WolfAir);
    ~WolfAir();

public:
    // Source: DESIGN.md s3.4 - the eight hit boxes, in the order "st" sends "dmg=" (s2).
    static const S32 PART_COUNT = 8;
    enum EPart { ENGINE = 0, FUEL, COCKPIT, FUSELAGE, WING_LEFT, WING_RIGHT, TAIL, FIN };
    static const char* partName(S32 part);     // "ENG" ... for the band

    struct State
    {
        // hi
        bool        mHi = false;
        std::string mPlane, mClass;
        S32         mChannel = 7;
        F32         mStall = 0.f, mVne = 0.f, mGmax = 0.f;    // m/s, m/s, g
        S32         mGuns = 0, mCannon = 0;                     // how many of each
        bool        mWolfAir = false;                           // the region has the wolf-air plugin
        // st
        bool        mHaveState = false;
        S32         mThrottle = 0;          // 0..100
        bool        mEngine = false, mGear = true, mFlaps = false;
        F32         mG = 1.f, mAoa = 0.f;
        bool        mStalled = false;
        S32         mFuel = 100;            // percent
        S32         mAmmoGuns = 0, mAmmoCannon = 0;
        S32         mHeatGuns = 0, mHeatCannon = 0;            // percent
        std::array<S32, PART_COUNT> mDmg{};  // health left per part, percent (100 = whole)
        bool        mFire = false, mSmoke = false;
        std::string mDogfight = "off";      // the plugin's state word: off on switching_off:<s> down wreck:<s>
        bool        mSafe = false;
        std::string mSay;
        // [WOLF AIR JETS 2026-11-02] helm 1.5.x st fields. Source: aircraft-dogfight-2026-11-01/b-flight-model/lsl/
        // wolf_plane_helm_template.lsl HudState(): "|gnd=" always; "|mach=|rpm=|rh=|ab=" only for a JET; "|noz=|h2o=|wet=|kg="
        // only for a VTOL. rh = gReheat (reheat lit), ab = gBrake (the AIRBRAKES, not the afterburner), rpm = gSpool x 100,
        // noz = gNoz in degrees, h2o = gWaterS (seconds of water left at full throttle), wet = gWet (water switched on),
        // kg = gMass (her mass now).
        bool        mJet = false;           // a "mach=" field has been seen
        F32         mMach = 0.f;
        S32         mRpm = 0;               // spool, percent
        bool        mReheat = false, mAirbrake = false;
        bool        mVtol = false;          // a "noz=" field has been seen
        S32         mNozzle = 0;            // degrees, 0 aft .. 98 forward of down
        S32         mWaterS = 0;
        bool        mWet = false;
        S32         mMassKg = 0;
        // [WOLF AIR JETS 2026-11-02] kit 1.3.0 fields. Source: e-lsl-commands/lsl/wolf_dogfight.lsl HudFields():
        // "|msl=<ir>/<radar>|cm=<flares>/<chaff>|warn=<0/1>" (counts from air.fire ["count"], AirMissiles.cs Count).
        S32         mIr = 0, mRadar = 0, mFlares = 0, mChaff = 0;
        bool        mWarn = false;
        // events (frame seconds)
        std::string mHitBy, mHitPart;
        S32         mHitDmg = 0;
        F64         mHitAt = -100.0;
        std::string mDownBy;
        F64         mDownAt = -100.0;
        F64         mRespawnAt = -100.0;
        // [WOLF AIR JETS] "warn|<text>" (wolf_dogfight.lsl Happened, kind missile_warning; the words are AirMissiles.cs:95
        // "MISSILE! <type> from <name> - release <chaff|flares> and break")
        std::string mWarnText;
        F64         mWarnAt = -100.0;
    };

    // ---- [WOLF AIR JETS 2026-11-02] the stores: the weapon the FIRE button and fire key let go ----
    enum EWeapon { WPN_GUN = 0, WPN_IR, WPN_RADAR };
    // A missile type's seeker figures. Source: aircraft-dogfight-2026-11-01/d-planes/generator/jet_weapons.py TYPES
    // (cone_deg and range_m; the plugin's lock test is AirMissiles.cs Launch: the target nearest the nose within the cone
    // and the reach). The class's missiles: d-planes/classes/<id>.json "guns" (group "missile" = infra-red, "radar").
    struct MissileType { const char* mId; const char* mTitle; F32 mConeDeg; F32 mRangeM; };
    /** The class's infra-red or radar missile, or null when she carries none (or the class is not one of ours). */
    static const MissileType* missileOf(const std::string& cls, bool radar);
    /** The class has reheat (thrust_ab_n > thrust_dry_n in its class file). */
    static bool hasReheat(const std::string& cls);
    /** Harrier: her hover limit at sea level in kg, wet or dry; 0 for a class that cannot hover. */
    static F32 hoverLimitKg(const std::string& cls, bool wet);

    struct Seeker
    {
        bool        mTarget = false;        // a warplane is inside the cone and the reach
        std::string mName;
        F32         mOffDeg = 0.f, mRangeM = 0.f;
        LLVector3   mPosAgent;              // where she is (for the diamond)
    };

    // ---- [WOLF AIR BOMBERS] the crew lines, DESIGN_BOMBERS.md s9 ----
    struct CrewSeat { std::string mSeat, mRole, mName; };
    struct Turret
    {
        bool mHave = false;
        std::string mSeat, mStop;
        F32 mZeroAz = 0.f, mAz = 0.f, mEl = 0.f, mAzMin = -180.f, mAzMax = 180.f, mElMin = -90.f, mElMax = 90.f, mRate = 0.f;
        S32 mAmmo = 0, mHeat = 0;
        bool mFire = false;
    };
    // DESIGN_BOMBERS.md s10 "eng": the helm's per-engine line (multi-engine classes only)
    static constexpr S32 MAX_ENGINES = 8;
    struct Engine { S32 mRpm = 0; bool mRun = false, mFeathered = false; S32 mPower = 100; bool mFire = false, mBottle = true; bool mReheat = false; };
    struct Engines
    {
        bool mHave = false;
        S32 mCount = 0;
        std::array<Engine, MAX_ENGINES> mE{};
        bool mAP = false, mAsym = false;
        S32 mTrim = 0;                      // -100..100
        bool mConcorde = false;             // rh/nose/cg seen
        F32 mNose = 0.f, mCg = 0.f;
    };
    struct Bombsight
    {
        bool mHave = false;
        S32 mDoors = 0;                     // 0 shut, 1 open, 2 opening
        F32 mOpenS = 0.f;
        S32 mLeft = 0, mLoad = 0, mStick = 1, mSpacingMs = 0;
        F32 mTtr = -1.f, mDrift = 0.f, mTargetM = 0.f;
        std::string mSteer, mTarget, mLastImpact;
        F64 mImpactAt = -100.0;
        bool mSafe = false;
    };

    // ---- [WOLF AIR RADAR 2026-11-02] the air radar (wolf-air 1.7.4 AirRadar.cs; c-wolf-air/README.md "wolf-air 1.7.4") ----
    // Paul on Dogfight: "i just saw a spitfire but nothing on my radar" - "thats the problem its hard to spot them".
    struct Contact
    {
        LLUUID      mId;
        LLVector3d  mPosGlobal;             // where the look put her (dead-reckoned from here on its velocity)
        LLVector3   mVel;                   // m/s
        char        mRel = 'n';             // 'h' hostile, 't' target drone, 'n' neutral (AirRadar.cs View)
        std::string mName, mPilot;
        F64         mAt = 0.0;              // when the look came (frame seconds)
        S32         mSeq = 0;
        bool        mNeighbour = false;     // seen by this viewer on ANOTHER region (the plugin cannot see there): neutral
    };
    static const S32 RADAR_RANGE_COUNT = 4;
    static F32 radarRangeM();               // WolfAirRadarRange: 1, 2, 5 or 10 km
    static void cycleRadarRange();
    static S32 targetKey();                 // WolfAirTargetKey: 0 none, 1 End
    static const char* targetKeyName(S32 key);
    /** A look has come within RADAR_STALE seconds (the plugin is sending: dogfight mode on, wolf-air 1.7.4 or later). */
    bool radarLive() const;
    const std::vector<Contact>& contacts() const { return mContacts; }
    /** Where a contact is now: the object itself when this viewer holds it, else the look's position moved on its velocity. */
    LLVector3d contactPos(const Contact& c) const;
    LLVector3 contactVel(const Contact& c) const;
    /** Her own aircraft: position, velocity and the nose's heading (radians, counter-clockwise from east). */
    bool ownPose(LLVector3d& pos, LLVector3& vel, F32& heading) const;
    /** The TGT button and the target key: the nearest hostile or target, then the next nearest, round again. */
    void cycleTarget();
    const Contact* target() const;
    /** What the last TGT press said, for the scope ("" after a few seconds). */
    std::string targetNote() const;
    /** The direction (agent frame, unit) the guns must point to hit the selected target, and the time of flight. */
    bool leadDirection(LLVector3& dir, F32& tof) const;
    bool haveGun() const { return mGunMv > 0.f; }
    F32 gunReach() const { return mGunReach; }

    struct Kill
    {
        std::string mKiller, mVictim, mHow;
        F64 mAt = 0.0;
    };

    void idle();

    /** A chat line from an object (llviewermessage.cpp). True when it was a WolfAir1 line: then it is shown nowhere else. */
    bool onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text);
    /** GenericMessage "WolfAir" from a region. */
    void receive(const LLUUID& region_id, const std::vector<std::string>& strings);

    /** Seated on a plane that answered: the flight deck shows its dogfight band. */
    bool active() const;
    bool hailing() const;
    const State& state() const { return mState; }
    const std::deque<Kill>& kills() const { return mKills; }
    /** In dogfight mode (any state but "off"). */
    bool inDogfight() const;
    /** "down" or "wreck:<s>". */
    bool isDown() const;
    /** The plane's root's nose (+X) in agent space, for the gunsight; false when not in view. */
    bool noseAxis(LLVector3& origin, LLVector3& axis) const;
    /** [WOLF AIR BOMBERS] the turret's bore in agent space (s9 tur: zaz + az, el in the root frame); false when unknown. */
    bool turretAxis(LLVector3& origin, LLVector3& axis) const;
    const std::string& lastError() const { return mError; }
    bool noAnswer() const;

    // ---- commands (DESIGN.md s2) ----
    void fire(bool on);             // held: "hud fire on", repeated every second while held; let go: "hud fire off"
    bool firing() const { return mFiring; }
    void dogfight(bool on);
    void bail();                    // the band asks first (BAIL then CONFIRM)

    // [WOLF AIR JETS 2026-11-02] the stores and the jet's switches (the helm's and kit's own words, sent on her channel)
    EWeapon weapon() const { return mWeapon; }
    bool weaponCarried(EWeapon w) const;
    void selectWeapon(EWeapon w);
    void trigger(bool down);        // the FIRE button / fire key: guns while held, one missile per press
    void decoy(bool flare);         // "hud flare" / "hud chaff"
    void reheat();                  // "reheat"
    void airbrake();                // "brakes"
    void nozzles(S32 deg);          // "nozzles <deg>" ("hover" at 90)
    void water();                   // "water"
    const Seeker& seeker() const { return mSeeker; }
    /** A missile ordered: what became of it, for the band ("" when nothing to say). */
    std::string launchNote(LLColor4& col) const;
    /** The missile warning is live (warn=1 or a warn line in the last 6 s). */
    bool warning() const;

    // [WOLF AIR BOMBERS] crew (DESIGN_BOMBERS.md s9)
    const std::vector<CrewSeat>& crew() const { return mCrew; }
    const std::string& mySeat() const { return mMySeat; }
    bool isCrew() const { return !mMySeat.empty() && mMySeat != "pilot"; }
    const Turret& turret() const { return mTurret; }
    const Seeker& turretCue() const { return mTurretCue; }
    const Engines& engines() const { return mEng; }
    /** The engine panel is this seat's: the pilot, the flight engineer or the co-pilot (s1 seats; s10 panel "engines"). */
    bool enginePanel() const;
    // s10 orders
    void engineOrder(S32 i, const char* what);    // "feather", "unfeather", "extinguish", "start", "stop"
    void autopilot(bool on);
    void rudderTrim(bool on);
    void droop(F32 deg);
    void concordeReheat(bool on);
    void cgTrim(const char* dir);
    const Bombsight& bombsight() const { return mBomb; }
    void changeSeat(const std::string& seat);
    void bombDoors();
    void bombRelease();
    void bombStick(S32 n);          // 0 = all

    // [WOLF AIR PREVIEW] the deck drawn from recorded lines at the login screen (WolfAirPreviewFile); nothing is sent
    bool previewing() const { return mPreview; }
    void previewTick();

    static bool onWolf();
    // The fire key the pilot chose on the CDU CONTROLS page (WolfAirFireKey): 0 none, 1 Space, 2 Insert.
    static S32 fireKey();
    static const char* fireKeyName(S32 key);

private:
    void reset(const LLUUID& seat);
    void send(const std::string& text);
    void sayHello();
    bool order(const std::string& words);   // true when it was said
    void parseHi(const std::vector<std::string>& f);
    void parseState(const std::vector<std::string>& f);
    void parseHit(const std::vector<std::string>& f);
    void parseKill(const std::vector<std::string>& f);
    void parseDown(const std::vector<std::string>& f);
    void addKill(const std::string& killer, const std::string& victim, const std::string& how);
    void pollFireKey();
    void handleLine(const std::string& text);
    void parseCrew(const std::vector<std::string>& f);
    void parseTurret(const std::vector<std::string>& f);
    void parseBomb(const std::vector<std::string>& f);
    void parseEngines(const std::vector<std::string>& f);
    void parsePreview(const std::vector<std::string>& f);
    // [WOLF AIR RADAR]
    void receiveContacts(const LLUUID& region_id, const std::vector<std::string>& strings);
    void scanNeighbours();
    void ageContacts();
    void pollTargetKey();
    void scanSeeker();
    void warnTone();
    bool previewWanted();

    State       mState;
    std::deque<Kill> mKills;
    LLUUID      mSeatRoot;
    bool        mPlaneDesc = false;         // the seat's root says "wolfvehicle:warplane"
    bool        mDescKnown = false;
    S32         mHelloTries = 0;
    F64         mHelloAt = 0.0;
    F64         mLastLineAt = 0.0;
    F64         mLastCommandAt = 0.0;
    bool        mStateAsked = false;
    bool        mDeckOpened = false;        // Flight Mode switched on for this seat already
    bool        mFiring = false;
    bool        mKeyFiring = false;         // held by the fire key (not the band's button)
    F64         mFireSentAt = 0.0;
    std::string mError;
    S32         mOnWolf = -1;
    // [WOLF AIR JETS]
    EWeapon     mWeapon = WPN_GUN;
    bool        mTriggerDown = false;
    Seeker      mSeeker;
    F64         mSeekerAt = 0.0;
    F64         mLaunchAt = -100.0;     // a missile ordered then
    S32         mLaunchCount = 0;       // what she had of it then
    bool        mLaunchRadar = false;
    bool        mWarnWas = false;
    F64         mToneAt = -100.0;
    S32         mToneState = 0;         // 0 not made, 1 made, -1 could not be made
    // [WOLF AIR BOMBERS]
    std::vector<CrewSeat> mCrew;
    std::string mMySeat;
    Turret      mTurret;
    Seeker      mTurretCue;
    F64         mTurretCueAt = 0.0;
    Bombsight   mBomb;
    Engines     mEng;
    // [WOLF AIR RADAR]
    std::vector<Contact> mContacts;
    F64         mRadarAt = -100.0;      // the last look
    S32         mLookSeq = -1, mLookParts = 0;
    U32         mLookGot = 0;           // bit i: part i+1 of mLookSeq has come
    F32         mGunMv = 0.f, mGunDrag = 0.f, mGunReach = 0.f;
    LLUUID      mTarget;
    std::string mTargetNote;
    F64         mTargetNoteAt = -100.0;
    bool        mTargetKeyDown = false;
    F64         mNeighbourAt = 0.0;
    mutable F32 mLastHeading = 0.f;     // held through a loop or a vertical climb (the nose has no heading then)
    // [WOLF AIR PREVIEW]
    bool        mPreview = false;
    S32         mPreviewChecked = 0;
    std::vector<std::pair<F64, std::string> > mPreviewLines;
    size_t      mPreviewNext = 0;
    F64         mPreviewStart = 0.0;
    F32         mPreviewVS = 0.f, mPreviewGS = 0.f, mPreviewAGL = 0.f;
public:
    /** The preview's stand-ins for the flight data the hover cue reads (vertical speed, ground speed, height). */
    void previewFlight(F32& vs, F32& gs, F32& agl) const { vs = mPreviewVS; gs = mPreviewGS; agl = mPreviewAGL; }
};

#endif // WOLF_AIR_H
