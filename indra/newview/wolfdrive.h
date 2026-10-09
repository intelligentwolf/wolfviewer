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
class WolfDrive : public LLSingleton<WolfDrive>
{
    LLSINGLETON(WolfDrive);
    ~WolfDrive();

public:
    enum EType { TYPE_CAR = 0, TYPE_TRUCK, TYPE_BIKE, TYPE_COUNT };

    // The gear selector, as sent to WolfSim and returned by wolfDriveInput.
    // Source: WolfDriveModule.cs GEAR_P .. GEAR_D and LSL_Constants.cs WOLF_DRIVE_GEAR_*.
    static constexpr S32 GEAR_P = -2, GEAR_R = -1, GEAR_N = 0, GEAR_MAX = 18, GEAR_D = 99;

    // The buttons bitmask. Source: LSL_Constants.cs WOLF_DRIVE_BTN_* (WolfDriveModule BUTTON_MASK).
    enum EButton
    {
        BTN_GEAR_UP = 1, BTN_GEAR_DOWN = 2, BTN_HORN = 4, BTN_LIGHTS = 8,
        BTN_IND_LEFT = 16, BTN_IND_RIGHT = 32, BTN_HANDBRAKE = 64, BTN_HAZARDS = 128, BTN_CRUISE = 256,
    };
    static constexpr U32 BUTTON_MASK = 0x1FF;   // WolfDriveModule.cs BUTTON_MASK
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
        ACT_HANDBRAKE, ACT_HAZARDS, ACT_CRUISE, ACTION_COUNT
    };
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
        bool  mMph = false;
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
        S32   mRetarder = 0;
        bool  mCruise = false;
        F32   mDriveTime = 0.f;      // seconds driven since the last break
        F64   mOdoKm = 0.0;
        F64   mTripKm = 0.0;
        F32   mLean = 0.f;           // degrees, + = leaning right
        F32   mMaxLeanL = 0.f, mMaxLeanR = 0.f;
        std::vector<std::string> mWarnings;
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
        bool   mHasUnits = false;    bool mMph = false;
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
        bool   mHasCruise = false;   bool mCruise = false;
        bool   mHasDriveTime = false; F32 mDriveTime = 0.f;
        bool   mHasOdo = false;      F64 mOdo = 0.0;
        bool   mHasWarnings = false; std::vector<std::string> mWarnings;
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
    U32  mButtons = 0;
    U32  mPrevButtons = 0;

    // lever taps to the script: +1 PageUp, -1 PageDown (old Vehicle tab tapIdle)
    std::vector<S32> mTapQueue;
    S32  mTapDir = 0;
    F64  mTapPhaseStart = 0.0;

    // the driver's own lamps when no script says (toggled by the buttons)
    S32  mOwnIndicators = 0;
    S32  mOwnLights = 0;
    bool mOwnHandbrake = false;

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
