/**
 * @file wolfhelm.h
 * @brief WolfViewer: the Captain's Helm - the link to a sailing ship's helm script, for the Captain's Helm HUD.
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

#ifndef WOLF_HELM_H
#define WOLF_HELM_H

#include <array>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"

// Paul, 2026-10-28, sailing HMS Wolfy at The Caribbean: "can we add a pirate ship control hud to the viewer".
//
// The protocol is region-terrain-work/pirate-ship/DESIGN.md s20 (the ship's side: lsl/hms_wolfy_helm_template.lsl,
// "the Captain's Helm HUD"). In short:
//   ship -> viewer  llRegionSayTo(avatar, 0, "WolfHelm1|<kind>|...") - kinds hi, st, bt, fired - only to a viewer that
//                   said hello while seated on her (so a stock viewer never gets a line). Taken out of chat here
//                   (onObjectChat, called from llviewermessage.cpp) when it comes from the object the agent sits on.
//   viewer -> ship  chat SHOUTED on the ship's channel (7): "hud hello 1", "hud bye", "hud helm <deg>", the ship's own
//                   orders ("plain sail", "drop anchor", "wind region"...) and "hud battle|fire ..." for the guns.
// Wolf Territories only: off it nothing is said, nothing is read and the HUD does not show.
class WolfHelm : public LLSingleton<WolfHelm>
{
    LLSINGLETON(WolfHelm);
    ~WolfHelm();

public:
    static const S32 SECTION_COUNT = 7;   // DESIGN.md s20.2 bt: bow, stern, port, starboard, waterline, rudder, rigging

    /** What the ship last said (DESIGN.md s20.2), plus the viewer's own measures. */
    struct State
    {
        // hi
        bool        mHi = false;            // the ship answered: it speaks the protocol
        std::string mShip;
        S32         mChannel = 7;
        F32         mNoGo = 67.5f;
        F32         mHelmMax = 35.f;
        F32         mHullKn = 0.f;
        bool        mNaval = false;
        // st
        bool        mHaveState = false;
        LLUUID      mCaptain;
        S32         mSail = 0;              // 0 Furled .. 3 All sail
        bool        mAnchor = false;
        bool        mAstern = false;
        F32         mKn = 0.f;              // through the water, - astern
        F32         mHdg = 0.f;
        F32         mHelm = 0.f;            // degrees, + port
        F32         mHeel = 0.f;
        S32         mWind = 0;              // 0 Wolf weather, 1 region, 2 fixed
        bool        mLight = true;
        F32         mTws = 0.f, mEff = 0.f, mTwf = 0.f, mTwa = 180.f;
        bool        mPortTack = false;
        std::string mPos;                   // calm irons close beam broad run
        S32         mStanding = 0;
        std::string mSrc, mStop, mSay;
        // bt (the naval script's 7201, passed on)
        bool        mHaveBattle = false;
        std::string mMode;                  // off on switching_off silenced sinking sunk
        F64         mLockUntil = 0.0;       // switching_off: when it may be switched off; silenced: when it may fire
        std::array<F32, SECTION_COUNT> mNow{}, mMax{};
        F64         mPortReadyAt = 0.0, mStbdReadyAt = 0.0;   // frame seconds when loaded
        F32         mPortFull = 0.f, mStbdFull = 0.f;         // the reload ring's full circle (seconds)
        F32         mSailFactor = 1.f, mTurnFactor = 1.f;
        bool        mStruck = false;
        F64         mProtectedUntil = 0.0;
        // fired
        F64         mPortFiredAt = -100.0, mStbdFiredAt = -100.0;
    };

    void idle();

    /** A chat line from an object (llviewermessage.cpp). True when it was the ship's line to this HUD: then it is not
     *  shown anywhere else. */
    bool onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text);

    // ---- the HUD ----
    bool hudShowing() const { return mShown; }
    /** Wolf World > Captain's Helm, the toolbar button, the corner icon, the HUD's X. */
    void toggleHud();
    void showHud(bool on);
    bool seatedOnShip() const { return mSeatRoot.notNull(); }
    bool shipAnswered() const { return mState.mHi; }
    bool isCaptain() const;
    /** Waiting for the ship's answer: a hello is still to go or was sent moments ago. */
    bool hailing() const;
    const State& state() const { return mState; }
    /** The ship's heading each frame from its root's rotation (the bow is the root's +X: DESIGN.md s3); the last
     *  "hdg" when the root is not in view. */
    F32 heading() const;
    /** The helm angle to draw: a drag in progress at once, else the ship's. */
    F32 helmShown() const;
    /** Seconds until a side is loaded (0 = ready); its fraction loaded 0..1. */
    F32 reloadLeft(bool starboard) const;
    F32 reloadFraction(bool starboard) const;
    F32 lockLeft() const;
    F32 protectedLeft() const;
    /** A command went and no line came back since: the HUD says so (accessible feedback, never console only). */
    bool noAnswer() const;
    const std::string& lastError() const { return mError; }

    // ---- commands to the ship (DESIGN.md s20.3) ----
    void order(const std::string& words);                 // an order the ship already knows ("plain sail")
    void setHelm(F32 degrees);                            // hud helm <deg>, sent at most 5 a second
    void battle(bool on);
    void fire(bool starboard);
    void setFixedWind(F32 knots, F32 from_deg);           // wind <knots> <from>, sent once the steppers rest

    static bool onWolf();

private:
    void reset(const LLUUID& seat);
    void sayHello();
    void send(const std::string& text);
    void parseHi(const std::vector<std::string>& f);
    void parseState(const std::vector<std::string>& f);
    void parseBattle(const std::vector<std::string>& f);
    void parseFired(const std::vector<std::string>& f);
    void claimBottom();
    void refreshGate();

    State       mState;
    LLUUID      mSeatRoot;
    bool        mShipDesc = false;          // the seat's root says "wolfvehicle:warship"
    bool        mDescKnown = false;
    S32         mHelloTries = 0;
    F64         mHelloAt = 0.0;
    F64         mLastLineAt = 0.0;
    F64         mLastCommandAt = 0.0;
    bool        mShown = false;
    bool        mAutoOpened = false;
    bool        mAutoDone = false;          // closed by hand on this seat: not opened again by itself
    F32         mHelmWant = 0.f;
    bool        mHelmPending = false;
    bool        mStateAsked = false;        // "hi" came without a state: asked once more
    F64         mHelmSentAt = 0.0;
    bool        mWindPending = false;
    F32         mWindKn = 0.f, mWindFrom = 0.f;
    F64         mWindChangedAt = 0.0;
    std::string mError;
    S32         mOnWolf = -1;
    F64         mLastGate = 0.0;
};

#endif // WOLF_HELM_H
