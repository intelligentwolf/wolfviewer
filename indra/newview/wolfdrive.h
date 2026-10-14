/**
 * @file wolfdrive.h
 * @brief WolfViewer driving: wheel / pedal / gamepad input, the analogue controls sent to WolfSim,
 *        the car / truck / bike dashboard's data and the viewer's own gearbox model.
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

#ifndef WOLF_DRIVE_H
#define WOLF_DRIVE_H

#include <set>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "llkeyboard.h"

class LLViewerJoystick;
class LLViewerObject;

// Paul, 2026-10-09: "the car dashboard should be complete now do that add the proper stuff
// please", then "no that vehicle tab can go I want a proper car dashboard ... look flash with
// gears proper speedo proper rev counter", then "3 modes, Car, Truck and Bike".
//
// THREE PARTS, ONE STATE:
//   1. CONTROLS. A wheel, pedals or a gamepad (LLViewerJoystick: libndofdev - SDL2 on Linux,
//      DirectInput on Windows, HID on macOS), the keyboard, and the dashboard's own on-screen
//      wheel, pedals and lever all feed one set of values: steer, throttle, brake, clutch, the
//      gear selector and the buttons. They always become the SAME control flags the keyboard
//      sends (gAgent.moveAt / moveYaw, PageUp / PageDown taps for the lever), so every vehicle
//      script drives exactly as it does from the keys.
//   0. WOLF TERRITORIES ONLY, ALL OF IT (Paul 2026-10-09: "other grids can't use these interfaces
//      they are only for wolf"). Off Wolf WolfDrive is inert: no dashboard, menu items, toolbar
//      button or Driving Controls; noteScanKey watches nothing; joystickStep returns false so the
//      controller is stock LLViewerJoystick; idle does nothing after the gate; no message goes out.
//      Leaving Wolf closes everything (refreshDashGate -> goInert); settings are kept for the way back.
//   2. ANALOGUE TO SCRIPTS, Wolf Territories only. While seated, the values go to WolfSim as
//      GenericMessage "wolfdrive" (WolfDriveModule.cs) - on change, at most 10 a second, and once
//      a second unchanged - where a vehicle script reads them with wolfDriveInput().
//   3. THE DASHBOARD (wolfdashboard.cpp), Wolf Territories only. A vehicle script drives it with
//      wolfDashboard() (GenericMessage "WolfDashboard"); whatever it does not send, the viewer's
//      own model fills in from the vehicle's real velocity (see WolfDrive::model in the .cpp).
// <WolfViewer 2026-10-11> w65 Space Rabbit adds two designs to the same state, to the protocol in
// region-terrain-work/wolfvehicles/PROTOCOL.md: the SPEEDBOAT helm (Paul: it "goes next to the sailing hud") and the
// SPACESHIP HUD (Paul: "spaceships are basically aircraft", "rocket mode for flying spacecraft is cool").
class WolfDrive : public LLSingleton<WolfDrive>
{
    LLSINGLETON(WolfDrive);
    ~WolfDrive();

public:
    // <WolfViewer 2026-10-11> w65 Space Rabbit: TYPE_SPEEDBOAT and TYPE_SPACESHIP added (PROTOCOL.md, region-terrain-work/
    // wolfvehicles). The saved value (WolfDashboardType, WolfDashboardVehicleTypes) is this number, so new
    // designs go on the end.
    // TYPE_SUBMARINE: Paul, "add another hud for driving a submarine and lsl commands" (the same day).
    enum EType { TYPE_CAR = 0, TYPE_TRUCK, TYPE_BIKE, TYPE_SPEEDBOAT, TYPE_SPACESHIP, TYPE_SUBMARINE, TYPE_COUNT };
    /** Car, truck and bike: the designs with wheels, a gearbox and the road telltales. */
    static bool isRoad(EType t) { return t == TYPE_CAR || t == TYPE_TRUCK || t == TYPE_BIKE; }
    /** "car" (any road design), "speedboat", "spaceship", "submarine" - the menu's parameter; anything else is the car. */
    static EType designFromName(const std::string& name);

    // The gear selector, as sent to WolfSim and returned by wolfDriveInput.
    // Source: WolfDriveModule.cs GEAR_P .. GEAR_D and LSL_Constants.cs WOLF_DRIVE_GEAR_*.
    static constexpr S32 GEAR_P = -2, GEAR_R = -1, GEAR_N = 0, GEAR_MAX = 18, GEAR_D = 99;

    // The buttons bitmask. Source: LSL_Constants.cs WOLF_DRIVE_BTN_* (WolfDriveModule BUTTON_MASK).
    enum EButton
    {
        BTN_GEAR_UP = 1, BTN_GEAR_DOWN = 2, BTN_HORN = 4, BTN_LIGHTS = 8,
        BTN_IND_LEFT = 16, BTN_IND_RIGHT = 32, BTN_HANDBRAKE = 64, BTN_HAZARDS = 128, BTN_CRUISE = 256,
        // <WolfViewer 2026-10-11> w65: region-terrain-work/wolfvehicles/PROTOCOL.md "Viewer to script" (WOLF_DRIVE_BTN_STAGE ..)
        BTN_STAGE = 512, BTN_BOOST = 1024, BTN_ANCHOR = 2048, BTN_ASSIST = 4096, BTN_LANDING_GEAR = 8192, BTN_ENGINE = 16384,
        BTN_DIVE = 32768, BTN_SURFACE = 65536, BTN_PERISCOPE = 131072, BTN_SONAR = 262144,   // the submarine's
    };
    static constexpr U32 BUTTON_MASK = 0x7FFFF; // PROTOCOL.md: "BUTTON_MASK goes from 0x1FF to 0x7FFFF" (WolfDriveModule.cs)
    // [WOLF RETARDER 2026-10-12] The truck's retarder controls: the 8th "in" parameter, a whole number 0..127
    // (WolfDriveModule.cs RETARDER_MAX / RETARDER_STALK_MASK / RETARDER_STALK_MAX): bits 0-2 the stalk 0 (off) .. 5,
    // then the four switches below. Never in the buttons parameter: a region from before 2026-10-12 clamps that to
    // BUTTON_MASK (so a high bit would read as every button held) and reads only the first 7 parameters.
    static constexpr S32 RETARDER_STALK_MAX = 5;
    static constexpr U32 RETARDER_W_DSC = 8, RETARDER_W_AUT = 16, RETARDER_W_DSC_UP = 32, RETARDER_W_DSC_DOWN = 64;
    /** One design's vehicle: the real one it is modelled on (sources in wolfdrive.cpp). */
    struct Spec
    {
        const char* mName;
        S32  mGears;                 // forward gears
        const F32* mRatios;          // mGears of them, first gear first
        F32  mReverse;
        F32  mFinal;                 // everything between the gearbox output and the wheel
        F32  mTyreCirc;              // m, the driven wheel's rolling circumference
        F32  mIdle;                  // the model's idle speed (a model choice, see the .cpp)
        F32  mLaunch;                // engine speed while the clutch slips pulling away (model)
        F32  mUpLight, mUpFull;      // AUTO: upshift rpm at no throttle / full throttle (model)
        F32  mDownLight, mDownFull;  // AUTO: downshift rpm (model)
        F32  mLimiter;               // the engine's maximum speed
        F32  mDialRpm;               // the rev counter's full scale
        F32  mRedline;
        F32  mGreenLo, mGreenHi;     // the economy band (truck), 0 for none
        F32  mDialKmh, mDialMph;     // the speedometer's full scale (0: digital only)
        F32  mDialKn;                // <WolfViewer 2026-10-11/> the same in knots (units=kn; the speedboat's own)
        F32  mGovernorKmh;           // a fitted speed limiter, 0 for none
        F32  mRangeKm;               // a full tank's range, for the model's fuel gauge
        F32  mShiftSecs;             // AUTO: the automatic's time between shifts (model)
    };
    static const Spec& spec(EType t);

    // The actions: one per EButton bit, in bit order. Each can be triggered by a key of the
    // driver's choosing, a controller button (Driving Controls) and the dashboard's own button,
    // and each sends the vehicle what the driver set for it - like the flight / sailing decks'
    // CONTROLS page (wolfflight.cpp keySetting, WolfFlightGearCommand / Channel).
    enum EAction
    {
        ACT_GEAR_UP = 0, ACT_GEAR_DOWN, ACT_HORN, ACT_LIGHTS, ACT_IND_LEFT, ACT_IND_RIGHT,
        ACT_HANDBRAKE, ACT_HAZARDS, ACT_CRUISE,
        // <WolfViewer 2026-10-11> w65: bits 9..14 (BTN_STAGE .. BTN_ENGINE)
        ACT_STAGE, ACT_BOOST, ACT_ANCHOR, ACT_ASSIST, ACT_LANDING_GEAR, ACT_ENGINE,
        ACT_DIVE, ACT_SURFACE, ACT_PERISCOPE, ACT_SONAR,   // bits 15..18, the submarine's
        // [WOLF RETARDER 2026-10-12] the truck's retarder stalk (one stage more / less braking), downhill speed
        // control on / off and its set speed + / - (held), and AUT. These six have NO button bit on the wire: what
        // they set goes out as the 8th "in" parameter (RETARDER_W_*). Their index is only the action's own bit in
        // the held-actions mask (ACTION_MASK).
        ACT_RETARDER_UP, ACT_RETARDER_DOWN, ACT_RETARDER_DSC, ACT_RETARDER_DSC_UP, ACT_RETARDER_DSC_DOWN, ACT_RETARDER_AUT,
        ACTION_COUNT
    };
    static constexpr U32 ACTION_MASK = (1u << ACTION_COUNT) - 1;   // every action's bit (the buttons are the low 19)
    static const char* actionSetting(S32 action);   // "WolfDriveButtonGearUp" ...: the controller button
    static const char* actionLabel(S32 action);
    static const char* actionId(S32 action);        // "gear_up" ...: the key in WolfDriveActions

    /** What an action sends the vehicle, besides the wolfDriveInput bit and the dashboard lamp. */
    enum EOut { OUT_NONE = 0, OUT_PAGE_UP, OUT_PAGE_DOWN, OUT_CHAT, OUT_COUNT };
    static const char* outLabel(S32 out);
    struct ActionCfg
    {
        std::string mKey;        // LLKeyboard::stringFromKey(key, false); "" = no key
        S32         mOut = OUT_NONE;
        std::string mCmd;        // OUT_CHAT: what is said
        S32         mChannel = 0;   // OUT_CHAT: on this channel (0 = aloud)
    };
    static ActionCfg actionDefault(S32 action);
    ActionCfg actionCfg(S32 action) const;          // the saved choice (setting WolfDriveActions)
    void setActionCfg(S32 action, const ActionCfg& cfg);
    void resetActions();
    /** A press of an action from the dashboard's own button (the paddles, the lamp buttons). */
    void pressAction(S32 action);

    // ---- per frame ----
    void idle();
    /** From LLViewerJoystick::scanJoystick after the device was read: true when the controller
     *  is driving this frame, so the joystick must not also move the avatar or the flycam. */
    bool joystickStep(LLViewerJoystick* js);
    /** From LLViewerInput::scanKey, before the key's binding runs. Only watches. */
    void noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat);

    // ---- the controller mapping (the Driving Controls floater) ----
    enum EAxisRole { AXIS_STEER = 0, AXIS_THROTTLE, AXIS_BRAKE, AXIS_CLUTCH, AXIS_ROLE_COUNT };
    static const char* axisSetting(S32 role);          // "WolfDriveSteerAxis" ...
    static const char* axisInvertSetting(S32 role);
    static const char* axisFullRangeSetting(S32 role); // the pedal rests at -1 (not used for steering)
    /** The role's value from the controller now, after the mapping and the dead zone:
     *  steering -1..1, pedals 0..1. 0 when unmapped or no controller. */
    F32  hardwareValue(S32 role) const;
    bool hardwareButton(S32 action) const;
    bool controllerReady() const;      // a controller is enabled and read
    bool hardwareDriving() const { return mHardwareDriving; }

    // ---- the values, from every source ----
    F32  steer() const { return mSteer; }
    F32  throttle() const { return mThrottle; }
    F32  brake() const { return mBrake; }
    F32  clutch() const { return mClutch; }
    S32  selector() const { return mSelector; }        // GEAR_* or 1..GEAR_MAX
    U32  buttonsHeld() const { return mButtons; }
    bool actionHeld(S32 action) const { return action >= 0 && action < ACTION_COUNT && (mActionsHeld & (1u << action)) != 0; }

    // ---- [WOLF RETARDER 2026-10-12] the truck's retarder controls (sources in wolfdrive.cpp) ----
    /** The stalk the driver set, 0 (off) .. RETARDER_STALK_MAX. */
    S32  retarderStalk() const { return mRetarderStalk; }
    void setRetarderStalk(S32 pos);
    /** Downhill speed control switched on (the vehicle's script holds the speed it was switched on at). */
    bool retarderDsc() const { return mRetarderDsc; }
    void setRetarderDsc(bool on);
    /** AUT: the brake pedal brings the retarder in before the wheel brakes. Remembered (WolfDriveRetarderAut). */
    bool retarderAut() const;
    void setRetarderAut(bool on);
    /** The 8th "in" parameter now (0 off the truck design). */
    U32  retarderWord() const;

    // ---- the dashboard's own controls ----
    void setScreenSteer(F32 v, bool held);
    void setScreenPedal(bool brake, bool held);
    void setScreenButton(U32 button, bool held);
    /** Put the selector in `g`; when seated, one PageUp / PageDown tap per position stepped goes
     *  to the vehicle's script (the old Vehicle tab's lever). */
    void setSelector(S32 g);
    /** This vehicle type's selector positions, lowest first: what the lever shows. */
    /** The lever as drawn, for the design and its gearbox mode. */
    static std::vector<S32> selectorPositions(EType t, bool auto_box);
    /** Every lever position in Page Up / Page Down order (MANUAL car / truck R, N, 1..; AUTO the
     *  automatic's P R N D / R N A; bike 1, N, 2..6). */
    static std::vector<S32> gearSequence(EType t, bool auto_box);
    /** Car / truck gearbox: AUTO (true) or MANUAL (false, the default); the bike is always manual.
     *  Remembered per design (WolfDashboardAutoCar / WolfDashboardAutoTruck). */
    bool autoBox() const;
    void setAutoBox(bool on);
    static const char* autoBoxSetting(EType t);
    static std::string gearLabel(S32 selector);

    // ---- the dashboard ----
    bool dashboardOn() const { return mDashOn; }
    /** The menu, the toolbar button and the dashboard's own close button. Off Wolf it refuses,
     *  saying nothing (there is no dashboard there at all). */
    void requestDashboard(bool on);
    EType type() const { return mType; }
    /** The driver's own choice of design on the dashboard: remembered for this vehicle (by its
     *  name) and as the default for the next. A script's "type" still wins while it sends one. */
    void chooseType(EType t);
    /** <WolfViewer 2026-10-11> The toolbar icons and Wolf World items (car, speedboat, spaceship): the dashboard in
     *  that design, or closed when it already shows it. TYPE_CAR stands for the road designs (the last one used). */
    void toggleDesign(EType t);
    bool showingDesign(EType t) const;
    /** <WolfViewer 2026-10-11> Nearby chat from an object (llviewermessage.cpp process_chat_from_simulator): the
     *  unmodified speedboat's "Speed = N" when it comes from the vehicle sat on. Only watches. */
    void noteObjectChat(const LLUUID& source, S32 chat_type, const std::string& text);

    /** What the instruments show this frame: the script's values where it sent them, the
     *  viewer's model for the rest. Every field is in SI units or percent. */
    struct Shown
    {
        bool  mValid = false;
        bool  mScripted = false;     // a script sent values for this vehicle
        F32   mSpeed = 0.f;          // m/s, the vehicle's real horizontal speed
        F32   mRpm = 0.f;
        F32   mMaxRpm = 8000.f;      // the rev counter's full scale
        F32   mRedline = 7400.f;
        F32   mMaxSpeed = 330.f;     // the speedometer's full scale, in the display units
        bool  mMph = false;          // mUnits == UNITS_MPH (the road designs' switch)
        // <WolfViewer 2026-10-11> w65: the display units (units=kn, the speedboat's knots, the spaceship's m/s)
        enum EUnits { UNITS_KMH = 0, UNITS_MPH, UNITS_KN, UNITS_MS };
        S32   mUnits = UNITS_KMH;
        std::string mGear;           // "P", "R", "N", "D", "3", "8H" ...
        S32   mGearNumber = 0;       // the gear the model or the script is in (0 none)
        bool  mManual = false;       // M / sequential: the driver picks the gear
        bool  mAutoBox = false;      // car / truck in AUTO (the dashboard's switch)
        F32   mFuel = 100.f;         // percent
        F32   mAdBlue = 100.f;
        F32   mTemp = 90.f;          // coolant, degrees C
        F32   mAir1 = 8.f, mAir2 = 8.f;   // bar
        S32   mIndicators = 0;       // 0 off, 1 left, 2 right, 3 hazard
        S32   mLights = 0;           // 0 off, 1 dipped, 2 main beam
        bool  mHandbrake = false;
        S32   mRetarder = 0;             // the stage braking now, the script's (the stalk is WolfDrive::retarderStalk)
        bool  mHasDscSpeed = false;      // [WOLF RETARDER 2026-10-12] the script holds a downhill speed
        F32   mDscSpeed = 0.f;           //   km/h, as sent (dsc_speed)
        bool  mCruise = false;
        F32   mDriveTime = 0.f;      // seconds driven since the last break
        F64   mOdoKm = 0.0;
        F64   mTripKm = 0.0;
        F32   mLean = 0.f;           // degrees, + = leaning right
        F32   mMaxLeanL = 0.f, mMaxLeanR = 0.f;
        std::vector<std::string> mWarnings;
        // <WolfViewer 2026-10-11> w65 Space Rabbit: the speedboat and spaceship. Measured: the attitude of the
        // seat's frame (as mLean), the vehicle's velocity, the land and water under it.
        F32   mHeading = 0.f;        // degrees true, 0 north, clockwise
        F32   mPitch = 0.f;          // degrees, + nose / bow up
        F32   mRoll = 0.f;           // degrees, + right side down
        F32   mTrack = 0.f;          // the direction of travel over the ground (heading when slow)
        F32   mSpeed3D = 0.f;        // m/s, the whole velocity
        F32   mVS = 0.f;             // m/s, + up
        F32   mFPA = 0.f;            // flight path angle, degrees (where the velocity points, up / down)
        // the flight-path marker: where the velocity points, in degrees from the nose in the craft's own frame
        // (+x right, +y up); not valid below 1 m/s or when going backwards
        F32   mFpmX = 0.f, mFpmY = 0.f;
        bool  mFpmValid = false;
        bool  mFuelScripted = false; // the fuel figure is the script's (the spaceship shows "--" otherwise)
        F32   mAltMSL = 0.f;         // m above the region's water
        F32   mAGL = 0.f;            // m above the ground or water under it, whichever is higher
        F32   mDepth = 0.f;          // m of water under the hull: water height minus land height (<= 0: aground)
        F32   mApogee = 0.f;         // m above the water: z + vz^2 / 2g while climbing, else the height now
        F32   mThrottlePct = 0.f;    // -100..100 (script throttle, or the driver's keys / pedals)
        bool  mThrottleScripted = false;
        bool  mHasTrim = false;      S32 mTrim = 0;
        bool  mAnchor = false;
        bool  mEngine = true;
        bool  mAssist = true;
        bool  mLandingGear = true;
        bool  mRocket = false;       // mode=rocket
        S32   mStage = 0, mStages = 0;
        bool  mHasBoost = false;     F32 mBoost = 0.f;
        bool  mHasShield = false;    F32 mShield = 0.f;
        bool  mHasHull = false;      F32 mHull = 0.f;
        bool  mHasHeat = false;      F32 mHeat = 0.f;
        S32   mStepMax = 0;          // speedboat: the steps the "3/6" is out of
        bool  mStepFromChat = false; // speedboat: the step is the boat's own "Speed = N"
        bool  mOnPlane = false, mPlaning = false;   // speedboat: on the plane / climbing onto it
        // the submarine (PROTOCOL.md ballast .. periscope); measured: the depth below the water and the sea bed
        F32   mSubDepth = 0.f;       // m below the water surface (water height minus z; < 0: above it)
        F32   mSeabed = 0.f;         // m from the hull down to the land under it (z minus land height)
        F32   mDepthRate = 0.f;      // m/min, + = going deeper
        bool  mHasBallast = false;   S32 mBallast = 0;
        bool  mHasMaxDepth = false;  S32 mMaxDepth = 0;
        bool  mHasOxygen = false;    F32 mOxygen = 0.f;
        bool  mHasBattery = false;   F32 mBattery = 0.f;
        bool  mHasPlanes = false;    S32 mPlanes = 0;
        bool  mPeriscope = false;
    };
    const Shown& shown() const { return mShown; }
    void resetTrip();
    std::string vehicleName() const { return mVehicleName; }

    static bool onWolf();    // lenient: for showing UI
    static bool canSend();   // confirmed: for anything sent to the region

private:
    // the script's values for the vehicle being driven
    struct Script
    {
        LLUUID mVehicle;             // root of what the agent sat on when they arrived
        F64    mAt = 0.0;
        bool   mHasType = false;     EType mType = TYPE_CAR;
        bool   mHasUnits = false;    S32 mUnits = Shown::UNITS_KMH;
        bool   mHasGear = false;     std::string mGear;
        bool   mHasRpm = false;      F32 mRpm = 0.f;
        bool   mHasMaxRpm = false;   F32 mMaxRpm = 0.f;
        bool   mHasRedline = false;  F32 mRedline = 0.f;
        bool   mHasMaxSpeed = false; F32 mMaxSpeed = 0.f;
        bool   mHasFuel = false;     F32 mFuel = 0.f;
        bool   mHasAdBlue = false;   F32 mAdBlue = 0.f;
        bool   mHasTemp = false;     F32 mTemp = 0.f;
        bool   mHasAir1 = false;     F32 mAir1 = 0.f;
        bool   mHasAir2 = false;     F32 mAir2 = 0.f;
        bool   mHasInd = false;      S32 mInd = 0;
        bool   mHasLights = false;   S32 mLights = 0;
        bool   mHasHandbrake = false; bool mHandbrake = false;
        bool   mHasRetarder = false; S32 mRetarder = 0;
        bool   mHasDscSpeed = false; F32 mDscSpeed = 0.f;   // [WOLF RETARDER 2026-10-12/]
        bool   mHasCruise = false;   bool mCruise = false;
        bool   mHasDriveTime = false; F32 mDriveTime = 0.f;
        bool   mHasOdo = false;      F64 mOdo = 0.0;
        bool   mHasWarnings = false; std::vector<std::string> mWarnings;
        // <WolfViewer 2026-10-11> w65: PROTOCOL.md "New keys"
        bool   mHasThrottle = false; S32 mThrottle = 0;
        bool   mHasTrim = false;     S32 mTrim = 0;
        bool   mHasAnchor = false;   bool mAnchor = false;
        bool   mHasEngine = false;   bool mEngine = false;
        bool   mHasMode = false;     bool mRocket = false;
        bool   mHasStage = false;    S32 mStage = 0;
        bool   mHasStages = false;   S32 mStages = 0;
        bool   mHasBoost = false;    F32 mBoost = 0.f;
        bool   mHasShield = false;   F32 mShield = 0.f;
        bool   mHasHull = false;     F32 mHull = 0.f;
        bool   mHasHeat = false;     F32 mHeat = 0.f;
        bool   mHasAssist = false;   bool mAssist = false;
        bool   mHasLandingGear = false; bool mLandingGear = false;
        bool   mHasBallast = false;  S32 mBallast = 0;
        bool   mHasMaxDepth = false; S32 mMaxDepth = 0;
        bool   mHasOxygen = false;   F32 mOxygen = 0.f;
        bool   mHasBattery = false;  F32 mBattery = 0.f;
        bool   mHasPlanes = false;   S32 mPlanes = 0;
        bool   mHasPeriscope = false; bool mPeriscope = false;
    };

public:
    /** The "WolfDashboard" handler (gGenericDispatcher). */
    void receiveDashboard(const LLUUID& invoice, const std::vector<std::string>& strings);

private:
    void readMapping();
    F32  mapAxis(S32 role, F32 raw) const;
    void combine(F32 dt);
    void applyControlFlags();
    void edgeButtons(U32 now_held);
    void doAction(S32 action);
    void shiftPicture(S32 dir);
    void sendAnalogue();
    void model(F32 dt);
    void watchSeat();
    void refreshDashGate();
    void goInert();   // off Wolf: drop every held input, the seat and the script's values
    void loadTypeFor(const std::string& name);
    void queueTaps(S32 steps);
    void tapStep();

    // controller
    bool mHardwareDriving = false;
    F32  mHw[AXIS_ROLE_COUNT] = { 0.f, 0.f, 0.f, 0.f };
    U32  mHwButtons = 0;
    F64  mHwSeen = 0.0;

    // the driver's own action keys (ActionCfg::mKey), held: the bits, and when each was last seen
    U32  mKeyButtons = 0;
    F64  mKeySeen[ACTION_COUNT] = {};
    KEY  mActionKey[ACTION_COUNT] = {};
    bool mActionKeysDirty = true;
    bool mReopenOnWolf = false;    // the dashboard was up when Wolf was left: back on return
    mutable ActionCfg mActionCache[ACTION_COUNT];
    mutable bool mActionCacheValid = false;
    bool mOwnCruise = false;

    // keyboard mirror (the old Vehicle tab's, wolfvehiclecontrols.cpp noteScanKey)
    bool mKbdAccel = false, mKbdBrake = false, mKbdLeft = false, mKbdRight = false;
    F64  mKbdAccelSeen = 0.0, mKbdBrakeSeen = 0.0, mKbdSteerSeen = 0.0;

    // on-screen
    bool mScreenSteerHeld = false;
    F32  mScreenSteer = 0.f;
    bool mScreenAccel = false, mScreenBrake = false;
    U32  mScreenButtons = 0;
    U32  mScreenLatched = 0;
    F64  mScreenPedalSince = 0.0;
    S32  mScreenPedalDir = 0;

    // combined
    F32  mSteer = 0.f, mThrottle = 0.f, mBrake = 0.f, mClutch = 0.f;
    S32  mSelector = GEAR_N;
    S32  mManualGear = 0;        // AUTO, in D / A: 0 the automatic; > 0 the driver took a gear (M)
    S32  mModelGear = 1;         // AUTO: the automatic's gear
    F64  mShiftAt = 0.0;         // AUTO: when it last shifted
    U32  mButtons = 0;           // the buttons held (BUTTON_MASK): the wire, the lamps
    U32  mActionsHeld = 0;       // every action held (ACTION_MASK): the edges, the retarder's + / - held
    U32  mPrevButtons = 0;       // last frame's mActionsHeld
    // [WOLF RETARDER 2026-10-12] the stalk and the downhill switch: a new seat starts with both off
    S32  mRetarderStalk = 0;
    bool mRetarderDsc = false;

    // lever taps to the script: +1 PageUp, -1 PageDown (old Vehicle tab tapIdle)
    std::vector<S32> mTapQueue;
    S32  mTapDir = 0;
    F64  mTapPhaseStart = 0.0;

    // the driver's own lamps when no script says (toggled by the buttons)
    S32  mOwnIndicators = 0;
    S32  mOwnLights = 0;
    bool mOwnHandbrake = false;
    // <WolfViewer 2026-10-11> w65: the same for the new buttons (MODEL starting states: anchor up, engine running,
    // flight assist on as Elite Dangerous starts, landing gear down as a craft sits on its pad)
    bool mOwnAnchor = false;
    bool mOwnEngine = true;
    bool mOwnAssist = true;
    bool mOwnLandingGear = true;
    bool mOwnPeriscope = false;      // the submarine's periscope, down to start
    // the unmodified speedboat's "Speed = N" (noteObjectChat), for the seat it came from
    S32    mChatStep = -1;
    LLUUID mChatStepSeat;
    EType  mLastRoadType = TYPE_CAR;   // what the car icon / Car Dashboard opens

    // sending
    F64  mLastSent = 0.0;
    std::string mLastSentKey;
    bool mWasSending = false;

    // seat and vehicle
    LLUUID mSeatRoot;
    LLUUID mHelloSeat, mHelloRegion;   // the seat and region the last "hello" went for
    std::string mVehicleName;
    bool  mNameKnown = false;
    bool  mAutoOpened = false;
    bool  mAutoDone = false;     // this seat's automatic opening has happened (or was refused)
    bool  mSeated = false;

    // dashboard
    bool  mDashOn = false;
    bool  mSyncing = false;
    EType mType = TYPE_CAR;
    Script mScript;
    Shown  mShown;
    S32    mOnWolf = -1;
    F64    mLastGate = 0.0;

    // model
    F32  mModelRpm = 0.f;
    F32  mFuelUsedKm = 0.f;      // km driven since sitting (the model's fuel)
    F32  mDriving = 0.f;         // seconds driven
    F64  mOdoKm = 0.0;
    F64  mTripKm = 0.0;
    F32  mMaxLeanL = 0.f, mMaxLeanR = 0.f;
};

#endif // WOLF_DRIVE_H
