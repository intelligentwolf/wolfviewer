/**
 * @file wolfflight.h
 * @brief WolfViewer Flight Mode: the flight data, the autopilot and the aircraft's controls.
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

#ifndef WOLF_FLIGHT_H
#define WOLF_FLIGHT_H

#include <deque>
#include <vector>
#include <string>

#include "llsingleton.h"
#include "v3dmath.h"
#include "v3math.h"
#include "indra_constants.h"

// Paul, 2026-10-06: "World, Flight Mode, adds a flight control interface for flying planes
// including an AUTOPILOT where you can tell it which region and position you want to fly to
// and it will automatically fly you there ... it can just go straight there ... proper plane
// controls (will work for helicopter) ... make it look MEGA realistic", "it has to control
// the plane not the avatar", "its normally pgup pgdn arrow keys and then sometimes you have
// like G for raise and lower gear but maybe it could be settable".
//
// THE AUTOPILOT FLIES THE AIRCRAFT THROUGH ITS OWN SCRIPT. A scripted plane or helicopter
// reads the pilot's keys as control flags (llTakeControls / control event): the arrows and W/S
// arrive as CONTROL_FWD / BACK, the turn keys as CONTROL_ROT_LEFT / RIGHT, PgUp / PgDn as
// CONTROL_UP / DOWN, Shift+arrows as CONTROL_LEFT / RIGHT. The viewer sends those as
// AGENT_CONTROL_AT_POS / AT_NEG, YAW_POS / YAW_NEG, UP_POS / UP_NEG and LEFT_POS / LEFT_NEG
// (llagent.cpp moveAt / moveYaw / moveUp / moveLeft). The autopilot sets exactly those flags,
// level-held and pulse-width modulated, so the aircraft's script flies itself the way it
// would for a pilot on the keys. Which pair does what differs between aircraft, so the
// assignment is the pilot's to set on the CDU's CONTROLS page (defaults: arrows up/down pitch,
// left/right bank, PgUp / PgDn throttle), and the autopilot watches the response and turns a
// reversed pitch or bank round by itself (AUTO LEARN).
//
// It never moves the avatar or the vehicle directly: the only thing it touches is the keys.
class WolfFlight : public LLSingleton<WolfFlight>
{
    LLSINGLETON(WolfFlight);
    ~WolfFlight();

public:
    // ---- the measured state, once per frame ----
    struct Data
    {
        bool        mValid = false;
        bool        mSeated = false;      // sitting on something (the aircraft)
        std::string mVehicle;             // the root prim's name, when known
        std::string mRegion;
        LLVector3d  mPosGlobal;
        LLVector3   mPosRegion;
        LLVector3   mVel;                 // m/s, world (east, north, up)
        F32 mPitch = 0.f;                 // degrees, nose up +
        F32 mRoll = 0.f;                  // degrees, right wing down +
        F32 mHeading = 0.f;               // degrees true, 0 north, 90 east
        F32 mTrack = 0.f;                 // ground track, degrees
        F32 mGS = 0.f;                    // ground speed, m/s
        F32 mAirspeed = 0.f;              // |velocity|, m/s (SL air is still)
        F32 mVS = 0.f;                    // vertical speed, m/s
        F32 mAltMSL = 0.f;                // metres above the region's water level
        F32 mAGL = 0.f;                   // metres above the land below
        F32 mGround = 0.f;                // land height below, region metres
        F32 mWaterZ = 20.f;               // the region's water height
        F32 mSlip = 0.f;                  // sideslip, degrees, + = air from the right
        F32 mG = 1.f;                     // load factor
        F32 mFPA = 0.f;                   // flight path angle, degrees
        F32 mTurnRate = 0.f;              // degrees/s, + right
        F32 mRollRate = 0.f;              // degrees/s
        F32 mPitchRate = 0.f;             // degrees/s
        F32 mSpeedTrend = 0.f;            // airspeed change over 10 s, m/s
        LLVector3 mWind;                  // the sim's wind here, m/s
        F32 mTimeDilation = 1.f;
        // sailing
        F32 mDepth = 0.f;                 // water under the hull, m (water level - seabed)
        F32 mTWS = 0.f, mTWD = 0.f;       // true wind speed m/s, direction it comes FROM
        F32 mTWA = 0.f;                   // true wind angle off the bow, + = starboard
        F32 mTWDSteady = 0.f;             // the true wind averaged over ~15 s: what the sailing AP steers by
        F32 mTWSSteady = 0.f;
        F32 mAWS = 0.f, mAWA = 0.f;       // apparent wind (felt aboard)
    };

    // The route the autopilot follows: the start, any turning points, the destination.
    // A plane's is the straight line; a boat's goes round the land (planWaterRoute).
    struct Route
    {
        std::vector<LLVector3d> mPts;
        S32  mLeg = 1;                    // the leg being flown: from mPts[mLeg-1] to mPts[mLeg]
        bool mValid = false;
        bool mUnsurveyed = false;         // crosses regions whose land is not loaded yet
        bool mNoWay = false;              // no way round by water was found
        bool mShoreEnd = false;           // the destination is on land: ends at the water nearest it
        F64  mPlannedAt = 0.0;
    };

    enum ELateral { LAT_NONE = 0, LAT_HDG, LAT_LNAV, LAT_HOLD, LAT_WIND };
    enum EVertical { VERT_NONE = 0, VERT_ALT, VERT_VS, VERT_VNAV, VERT_HOVER };
    // A pair of keys the pilot holds, as the flags the viewer sends for them.
    enum EPair { PAIR_FWD_BACK = 0, PAIR_TURN, PAIR_UP_DOWN, PAIR_SLIDE, PAIR_NONE, PAIR_COUNT };
    enum ECraft { CRAFT_PLANE = 0, CRAFT_HELI };
    enum ECasLevel { CAS_MEMO = 0, CAS_ADVISORY, CAS_CAUTION, CAS_WARNING };
    struct Cas
    {
        std::string mText;
        ECasLevel   mLevel = CAS_MEMO;
        F64         mTime = 0.0;
    };
    struct Dest
    {
        bool        mValid = false;
        bool        mPending = false;     // waiting for the map server to name the region
        bool        mFailed = false;      // NOT IN DATABASE
        bool        mHasZ = false;
        std::string mRegion;
        LLVector3   mLocal;               // region metres
        LLVector3d  mGlobal;
        F64         mRequestedAt = 0.0;
    };

    // ---- Flight Mode itself (World > Flight Mode, setting WolfFlightMode) ----
    bool active() const { return mActive; }
    void setActive(bool on);
    /** World > Sailing Mode (WolfSailMode): the same deck with marine instruments. */
    bool sailing() const { return mActive && mSail; }
    void requestMode(bool sail, bool on);
    /** The bottom-right Plane / Boat icons: show that mode's deck, or hide it if it is the one
     *  showing. Hidden, the mode and its autopilot keep running (pictures without the panel). */
    void toggleDeck(bool sail);
    bool deckHidden() const { return mDeckHidden; }

    // ---- sailing autopilot ----
    F32 selTWA() const { return mSelTWA; }
    void setSelTWA(F32 v);
    void pressWIND();               // hold the selected wind angle
    void pressTACK();               // tack (or gybe) now
    static F32 tackAngle();
    S32 tack() const { return mTack; }

    // ---- the route ----
    const Route& route() const { return mRoute; }
    LLVector3d activeWaypoint() const;
    F32 routeTotal() const;         // metres along the whole route
    F32 routeFlown() const;         // metres along it so far
    void rebuildRoute();

    /** Every frame from LLAppViewer::idle(), after the agent update is sent. */
    void idle();
    const Data& data() const { return mData; }

    // ---- the autopilot (the glareshield) ----
    bool apEngaged() const { return mAP; }
    void engageAP();
    void disconnectAP(const std::string& why, bool by_pilot);
    bool atEngaged() const { return mAT; }
    void toggleAT();
    ELateral lateral() const { return mLateral; }
    EVertical vertical() const { return mVertical; }
    void pressLNAV();
    void pressHDG();
    void pressALT();
    void pressVS();
    void pressVNAV();
    F32 selSpeedKt() const { return mSelSpeedKt; }
    F32 selHeading() const { return mSelHeading; }
    F32 selAltFt() const { return mSelAltFt; }
    F32 selVsFpm() const { return mSelVsFpm; }
    void setSelSpeedKt(F32 v);
    void setSelHeading(F32 v);
    void setSelAltFt(F32 v);
    void setSelVsFpm(F32 v);
    /** Flight director: the pitch and bank the guidance wants (shown even with the AP off). */
    bool fdValid() const { return mFDValid; }
    F32 fdPitch() const { return mFDPitch; }
    F32 fdRoll() const { return mFDRoll; }
    /** What the autopilot is holding on each axis right now, -1..1 (the control synoptic). */
    F32 outPitch() const { return mOutPitch; }
    F32 outBank() const { return mOutBank; }
    F32 outThrottle() const { return mOutThrottle; }
    /** What the pilot's own keys are doing on each axis, -1 / 0 / 1. */
    S32 pilotPitch() const;
    S32 pilotBank() const;
    S32 pilotThrottle() const;

    // ---- the route ----
    const Dest& dest() const { return mDest; }
    /** Look the region up (map server) and fly there once known. Empty region = this one. */
    void setDestination(const std::string& region, const LLVector3& local, bool has_z);
    /** The destination set on the world map, if it has one. */
    bool setDestinationFromMap();
    void clearDestination();
    bool mapHasDestination() const;
    F32 destDistance() const;      // metres, horizontal
    F32 destBearing() const;       // degrees true
    F32 destEtaSeconds() const;    // < 0 unknown
    F32 cruiseAltFt() const { return mCruiseAltFt; }
    void setCruiseAltFt(F32 ft);

    // ---- crew alerting ----
    const std::deque<Cas>& cas() const { return mCas; }
    void postCas(const std::string& text, ECasLevel level);
    void clearCas(const std::string& text);
    bool masterWarning() const;
    bool masterCaution() const;
    void cancelMasters();

    // ---- the aircraft's own controls (CDU CONTROLS page; settings) ----
    static ECraft craft();
    static EPair pitchPair();
    // The helm / motor key settings for the current mode: Sailing Mode has its own (WolfSail*),
    // so a plane can never reverse a boat's helm. what: BankInvert, ThrottleInvert,
    // ThrottleSteps, BankPair, ThrottlePair.
    static const char* keySetting(const char* what);
    // A key's "reversed" setting as in use: the saved choice (CDU CTL page), turned round by any
    // reversal AUTO LEARN found this session. Learned reversals are not saved: every start is
    // clean (Paul: "all routes etc should be cleared on start of the viewer"); a learned flip
    // had kept his plane's bank reversed across restarts.
    static bool invert(const char* setting);
    static void setInvert(const char* setting, bool reversed);   // a pilot's own choice: saved, learned flip cleared
    static EPair bankPair();
    static EPair throttlePair();
    static const char* pairName(EPair p);
    void sendGearCommand();
    bool gearDown() const { return mGearDown; }
    F64 gearMovedAt() const { return mGearMovedAt; }
    void sendEngineCommand();
    void sendSailCommand();

    /** The deck's sidestick, -1..1: x + = bank right (helicopter: yaw right), y + = nose up
     *  (pulled back; helicopter: forward). Springs back to 0 when let go. */
    void setStick(F32 x, F32 y);
    F32 stickX() const { return mStickX; }
    F32 stickY() const { return mStickY; }
    /** The deck's spring-centred throttle lever, -1..1 (+ = more power / lift). */
    void setThrottleLever(F32 v);
    F32 throttleLever() const { return mThrottleLever; }

    /** From LLViewerInput::scanKey: the pilot's own keys (they disconnect the autopilot). */
    void noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat);

private:
    void measure(F32 dt);
    void guidance(F32 dt);
    void drive(F32 dt);
    void manualDrive();
    void autothrottle(F32 dt);
    void groundProximity();
    void syncModeSettings();
    void refuseOffGrid(bool sail);
    void clearTrip();   // destination, route and alerts gone: a mode entered or left starts clean
    F32  boatRay(const LLVector3& from, const LLVector3& dir, F32 reach);
    F32  terrainNeed(F32 track_deg, F32 speed, F32 z) const;
    void advanceRoute();
    void planWaterRoute();
    void sailAlarms();
    void sailGuidance(F32 dt);
    void sailDrive(F32 dt);
    F32 obstacleSeconds(const LLVector3& dir, F32 speed, F32 skip);
    bool mSail = false;
    bool mDeckHidden = false;
    bool mSyncing = false;
    Route mRoute;
    F32 mSelTWA = 45.f;
    S32 mTack = 0;                  // +1 starboard tack (wind on the starboard bow), -1 port
    F64 mLastTack = 0.0;
    F64 mAvoidUntil = 0.0;          // turning away from a shoal or an obstacle until then
    F32 mAvoidTurn = 0.f;
    bool mRouteDirty = false;
    F32 mBoatObstacle = 1e9f;
    F32 mWindAvgX = 0.f, mWindAvgY = 0.f;   // the sim's wind, averaged (measure)
    bool mHaveWindAvg = false;
    std::map<std::string, bool> mLearnedFlip;   // setting name -> reversed this session by AUTO LEARN
    F32 mTerrainNeed = 0.f;             // m/s of climb the land ahead calls for (terrainNeed)
    F64 mNextTerrainScan = 0.0;
    F32 mTerrainNeedSmooth = 0.f;
    S32 mTurnHeld = 0, mPitchHeld = 0;   // a plane's turn / pitch key held down (-1, 0, +1), not pulsed       // mTerrainNeed eased over ~2 s: what the climb follows
    F64 mNextFlightLog = 0.0;
    F32 mBoatClearL = 1e9f, mBoatClearR = 1e9f;   // metres clear ahead off the port / starboard bow
    F64 mNextBoatRay = 0.0;
    F32 mLastCourse = 0.f, mCourseRate = 0.f;   // a boat's course over the ground and its rate of turn
    F64 mCircleStart = 0.0;
    F32 mCircleTurn = 0.f;                      // degrees turned this minute (the circling check)
    F64 mNextSailLog = 0.0;
    F64 mNextObstacleRay = 0.0;
    F32 mObstacleImpact = 1e9f;
    F64 mPullUpUntil = 0.0, mTerrainUntil = 0.0, mNextAlarmSound = 0.0;
    void learn(F32 dt);
    void resolveDestination();
    void holdPair(EPair pair, F32 cmd, F32 phase_offset);
    void tapPair(EPair pair, S32 dir);
    F32 terrainFloorZ() const;
    F32 targetAltitudeZ() const;
    void applyToolbar(bool flight_on);

    bool mActive = false;
    bool mToolbarWasHidden = false;
    Data mData;
    bool mHaveLast = false;
    LLVector3 mLastVel;
    F32 mLastRoll = 0.f, mLastPitch = 0.f, mLastHeading = 0.f, mLastAirspeed = 0.f;
    F32 mHeadingOffset = 0.f;      // the pilot's seat faces this far off the nose (multiple of 90)
    F32 mOffsetVote = 0.f;
    F64 mOffsetVoteStart = 0.0;

    bool mAP = false;
    bool mAT = false;
    ELateral mLateral = LAT_NONE;
    EVertical mVertical = VERT_NONE;
    F32 mSelSpeedKt = 120.f;
    F32 mSelHeading = 0.f;
    F32 mSelAltFt = 500.f;
    F32 mSelVsFpm = 0.f;
    F32 mCruiseAltFt = 500.f;
    F32 mPitchTrim = 0.f;          // the integrator: the pitch that holds level flight
    bool mFDValid = false;
    F32 mFDPitch = 0.f, mFDRoll = 0.f;
    F32 mWantVS = 0.f;             // m/s the vertical guidance asks for
    F32 mVSf = 0.f;                // the vertical speed, filtered
    F32 mWantTrack = 0.f;          // degrees the lateral guidance asks for
    F32 mWantSpeed = 0.f;          // m/s
    F32 mOutPitch = 0.f, mOutBank = 0.f, mOutThrottle = 0.f;
    F64 mThrottleNextTap = 0.0;
    S32 mThrottleTapDir = 0;
    F64 mThrottleTapUntil = 0.0;
    S32 mThrottleStreak = 0;       // taps in one direction without the speed answering
    F32 mThrottleStreakSpeed = 0.f;
    bool mArrived = false;

    // AUTO LEARN: does holding "nose up" raise the nose, does "bank right" turn right?
    F32 mLearnPitchAcc = 0.f, mLearnPitchWeight = 0.f;
    F32 mLearnBankAcc = 0.f, mLearnBankWeight = 0.f;
    F32 mLearnLiftAcc = 0.f, mLearnLiftWeight = 0.f;
    F64 mLearnWindowStart = 0.0;
    F64 mLearnHoldUntil = 0.0;          // no learning while the craft settles after the AP engages
    S32 mPitchRevStreak = 0, mBankRevStreak = 0, mLiftRevStreak = 0;   // windows in a row that looked reversed

    // the deck's stick and throttle lever
    F32 mStickX = 0.f, mStickY = 0.f, mThrottleLever = 0.f;
    F64 mManualNextTap = 0.0;
    F64 mManualTapUntil = 0.0;
    S32 mManualTapDir = 0;

    // the pilot's keys, as seen by noteScanKey
    F64 mKeySeen[PAIR_COUNT][2] = {};

    Dest mDest;
    std::deque<Cas> mCas;
    bool mMasterWarning = false, mMasterCaution = false;
    bool mGearDown = true;
    F64 mGearMovedAt = -100.0;
    F64 mApOffFlashUntil = 0.0;

public:
    F64 apOffFlashUntil() const { return mApOffFlashUntil; }
};

#endif // WOLF_FLIGHT_H
