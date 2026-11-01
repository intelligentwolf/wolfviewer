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
#include "v3math.h"

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
        // events (frame seconds)
        std::string mHitBy, mHitPart;
        S32         mHitDmg = 0;
        F64         mHitAt = -100.0;
        std::string mDownBy;
        F64         mDownAt = -100.0;
        F64         mRespawnAt = -100.0;
    };

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
    const std::string& lastError() const { return mError; }
    bool noAnswer() const;

    // ---- commands (DESIGN.md s2) ----
    void fire(bool on);             // held: "hud fire on", repeated every second while held; let go: "hud fire off"
    bool firing() const { return mFiring; }
    void dogfight(bool on);
    void bail();                    // the band asks first (BAIL then CONFIRM)

    static bool onWolf();
    // The fire key the pilot chose on the CDU CONTROLS page (WolfAirFireKey): 0 none, 1 Space, 2 Insert.
    static S32 fireKey();
    static const char* fireKeyName(S32 key);

private:
    void reset(const LLUUID& seat);
    void send(const std::string& text);
    void sayHello();
    void order(const std::string& words);
    void parseHi(const std::vector<std::string>& f);
    void parseState(const std::vector<std::string>& f);
    void parseHit(const std::vector<std::string>& f);
    void parseKill(const std::vector<std::string>& f);
    void parseDown(const std::vector<std::string>& f);
    void addKill(const std::string& killer, const std::string& victim, const std::string& how);
    void pollFireKey();

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
};

#endif // WOLF_AIR_H
