/**
 * @file wolftraincab.h
 * @brief WolfViewer: the Train Cab - driving a Wolf Rail train (the wolf-rail plugin's train.driver), and its HUD.
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

#ifndef WOLF_TRAINCAB_H
#define WOLF_TRAINCAB_H

#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluictrl.h"
#include "lluuid.h"
#include "wolfhudscale.h"

// Paul, 2026-11-13: "make them drivable with a train driving hud"; "the trains etc will use our new commands and server
// side stuff". The design is region-terrain-work/wolf-trains-drivable-2026-11-13/DESIGN.md section 2; the region's side
// is the wolf-rail plugin 1.1 (region-terrain-work/wolf-rail/src/Cab.cs, RailEngine.cs SendCab):
//   region -> viewer  GenericMessage "WolfTrainCab", each parameter "key=value": train, state (on / off), kind (steam,
//                     diesel, electric, highspeed, shunter, tram or empty), mode (driver, easy, script, timetable), speed
//                     and line (m/s), power, brake, rev (-1 / 0 / 1), stop (station, stop or empty), stopm (-1: none),
//                     ahead (m), protect (0 / 1), region, note. Sent only to the avatar the engine's script made its
//                     driver (train.driver), while they sit on the engine: once a second until this viewer sends
//                     controls, then four a second.
//   viewer -> region  the controls every Wolf viewer already sends for vehicles: GenericMessage "wolfdrive" "in"
//                     (wolfdrive.cpp WolfDrive::sendAnalogue): throttle = power, brake, gear = the reverser (1 forward,
//                     0 neutral, -1 reverse), buttons = horn (4) and, while Easy driving is on, ASSIST (4096). The
//                     region reads them itself (IWolfDriveModule.TryGetInput) - no script in between.
// [wolf-rail 1.2] AUTO (Paul 13 Nov: "trains should have an auto mode as well btw so they can just run"): the cab's AUTO
// button holds CRUISE (256) for a moment with both levers let go (Cab.cs BTN_AUTO, EngineDriver.ReadDriver: the bit
// coming on switches Auto on, or off). [client review 13 Nov, W3] The press leaves the driver's levers as they are: only once
// the region says mode=auto does the cab put them to their let-go position (and send them so); a region that does not switch
// Auto on (a lever held: "Let both levers go, then press AUTO.") finds them exactly as the driver had them. Taking a lever
// (a notch, a drag, Up / Down) in Auto takes the train back.
// [wolf-rail 1.2, Paul 13 Nov D-b] POINTS is one press: STAGE (512) held a moment; the region changes the next points a driver
// may set and says the new setting in the note. [D-c] Easy driving has FORWARD and BACK: BACK sends the reverser at R (the
// gear the driver's mode sends for REVERSE), and the region backs the train slowly (only from a stand).
// [wolf-rail 1.3, stage V] (region-terrain-work/wolf-rail/DESIGN-terminus.md section 5) the region adds lead=, ends=, veh=,
// couple=, shunt= to every cab message and walk= to the state=off after CHANGE ENDS; the SHUNT buttons CHANGE ENDS (ENGINE
// 16384), COUPLE (ANCHOR 2048) and UNCOUPLE (LANDING_GEAR 8192) are one press each and show only when the region says they
// can act. The turntable (train.turntable) is a script call for a train nobody drives: the cab has nothing for it.
// Wolf Territories only: off it nothing is read, nothing is sent and the cab does not show.
class WolfTrain : public LLSingleton<WolfTrain>
{
    LLSINGLETON(WolfTrain);
    ~WolfTrain();

public:
    /** What the region last said (the keys above). */
    struct Cab
    {
        std::string mTrain, mKind, mMode, mStop, mRegion, mNote;
        F32  mSpeed = 0.f, mLine = 0.f, mPower = 0.f, mBrake = 1.f;
        S32  mRev = 0;
        F32  mStopM = -1.f, mAhead = 0.f;
        bool mProtect = false;
        // [wolf-rail 1.2] backing (back=1: "ahead" is then the line behind); the next signal (sig=aspect|metres) and the
        // next points (pts=name|setting|metres|driver) on the line laid (RailEngine.SendCab)
        bool mBack = false;
        std::string mSig;               // "red", "yellow", "green", or empty: none in sight
        F32  mSigM = -1.f;
        std::string mPts, mPtsSet;      // the points' name and setting ("left", "right", "auto" = as laid)
        F32  mPtsM = -1.f;
        bool mPtsDriver = false;        // a driver may change them from the cab ("points <name> driver")
        // [wolf-rail 1.3, stage V] Source: wolf-rail RailEngine.cs SendCab :1215-1218 (DESIGN-terminus.md 5.2). A region
        // before 1.3 never sends these keys: every one keeps its "not said" value and nothing new is shown.
        bool mHasLead = false;          // lead= was sent
        bool mLead = true;              // lead=1 the cab is at the leading end, 0 the train is propelled (RailEngine.Lead)
        bool mEnds = false;             // ends=1 a change of ends could be made now (RailEngine.EndsNow)
        S32  mVeh = -1;                 // veh= the vehicles behind the head (TrainRecord.Vehicles.Count); -1 not sent
        // couple= "ahead|metres|name" or "behind|..." (RailCouple.cs CoupleSeen: "ahead" = towards the cab's front), or empty
        std::string mCoupleSide, mCoupleName;
        F32  mCoupleM = -1.f;
        bool mShunt = false;            // shunt=1 this driver may couple and uncouple (train.driver [.., "shunt"])
    };

    void receive(const LLUUID& invoice, const std::vector<std::string>& strings);
    void idle();

    /** The region made this avatar the driver of the engine they sit on, and says so: the controls go out. */
    bool driving() const;
    /** The HUD is up (driving, or opened by hand from Wolf World to read the guide). */
    bool shown() const;
    void toggleShown();
    /** Driving with the cab hidden by the driver (not given way to another panel): the view draws a small tab to bring it back. */
    bool hiddenByDriver() const { return driving() && mHidden && !mYielded; }
    const Cab& cab() const { return mCab; }
    static bool onWolf();

    // ── the controls ──────────────────────────────────────────────────────────────────────────────────────────
    enum ENotch { NOTCH_STOP = 0, NOTCH_SLOW, NOTCH_GO, NOTCH_COUNT };
    bool easy() const;
    void setEasy(bool on);
    S32  notch() const { return mNotch; }
    void setNotch(S32 n);
    F32  power() const { return mPower; }
    F32  brake() const { return mBrake; }
    void setPower(F32 v);
    void setBrake(F32 v);
    S32  reverser() const { return mRev; }
    void setReverser(S32 r);
    /** [wolf-rail 1.2, D-c] Easy driving's direction: FORWARD (false) or BACK (true, slow speed only). */
    bool easyBack() const { return mEasyBack; }
    void setEasyBack(bool back);
    void setHorn(bool held) { mHornHeld = held; }
    /** [wolf-rail 1.2] Auto: shown and sent while the region says mode=auto (or the driver has just let it go here). */
    bool autoOn() const { return mAutoWant; }
    /** AUTO pressed, the region's answer not yet heard (at most AUTO_GRACE_S): the levers are still the driver's. */
    bool autoAsking() const;
    void pressAuto();
    /** [wolf-rail 1.2] The POINTS button: the next points ahead that drivers may change, within the region's reach
     *  (RailEngine.DRIVER_POINTS_M). One press sends the press (Paul 13 Nov, D-b: no question first). */
    bool canChangePoints() const;
    void pressPoints();
    /** [wolf-rail 1.3, stage V] The SHUNT buttons (DESIGN-terminus.md 5.3 / 5.4), each one press like POINTS: CHANGE ENDS
     *  (ENGINE 16384, Cab.cs BTN_ENDS) while the region says ends=1; COUPLE (ANCHOR 2048, BTN_COUPLE) with shunt=1 and stock
     *  in couple=; UNCOUPLE (LANDING_GEAR 8192, BTN_UNCOUPLE) with shunt=1 and veh > 0. */
    bool canChangeEnds() const;
    bool canCouple() const;
    bool canUncouple() const;
    void pressEnds();
    void pressCouple();
    void pressUncouple();
    /** COUPLE joins towards the cab's front with the reverser at FORWARD, else behind (wolf-rail RailCouple.cs CabCouple):
     *  true while the way the cab is set to go is not the way to the stock in couple=. */
    bool coupleWrongWay() const;
    /** [stage V] After CHANGE ENDS the region says state=off with walk=<vehicle> and its note: the driver has
     *  wolf-rail Cab.CAB_WALK_MS (120 s) to sit in that cab (RailEngine.cs CommitEnds :904-910). */
    bool walkPending() const;
    const std::string& walkNote() const { return mWalkNote; }
    void dismissWalk() { mWalkUntil = -1.0; mWalkNote.clear(); }
    /** [stage V] The last refusal the region gave a SHUNT press (isRefusal), kept until the driver closes it: the region's
     *  own note goes after 4 s (EngineDriver.cs Note :1895-1899). */
    const std::string& alert() const { return mAlert; }
    void dismissAlert() { mAlert.clear(); }
    bool horn() const { return mHornHeld; }
    bool mph() const;
    void setMph(bool on);

    /** What WolfDrive sends while driving() (see the class comment). */
    F32 sendPower() const;
    F32 sendBrake() const;
    S32 sendGear() const;
    U32 sendButtons() const;

    /** The cab's keys, while driving() (wolfdrive.cpp noteScanKey: unmodified keys only, never one the UI took - so typing
     *  in chat never reaches them, llviewerinput.cpp mKeyHandledByUI). Watch-only like every Wolf HUD key: the key's own
     *  binding still runs. Up / W one step more, Down / S one step less; F5 the reverser (forward, neutral, reverse in turn);
     *  (in Easy driving: FORWARD and BACK in turn); F2 Easy / driver's mode; F3 mph / km/h; F4 show / hide the cab; [1.2] F8
     *  AUTO, F9 POINTS (one press); [wolf-rail 1.3, stage V] F6 CHANGE ENDS, F7 COUPLE (DESIGN-terminus.md 5.4).
     *  F2-F9 have no binding in any mode
     *  (key_bindings.xml first_person / third_person / sitting) and are no menu's shortcut unmodified (only F1 is,
     *  menu_viewer.xml; F2-F9 appear there only with control|alt); every free letter is a driving action's default key
     *  (wolfdrive.cpp ACTION_DEFAULT_KEYS) or a movement key. */
    static bool isCabKey(KEY key);
    void noteKey(KEY key, bool key_down, bool repeat);
    /** [stage V] Shift+F7 UNCOUPLE (the design's key: a split of the train is not one plain key away). WolfDrive::noteScanKey
     *  watches unmodified keys only, so it hands this one combination over by itself. Shift+F7 is no binding
     *  (key_bindings.xml has no F key) and no menu's shortcut (menu_viewer.xml has F keys only with control|alt). */
    static bool isCabShiftKey(KEY key) { return key == KEY_F7; }
    void noteShiftKey(KEY key, bool key_down, bool repeat);

private:
    void close();
    void claimBottom();

    Cab mCab;
    bool mOpen = false;             // the region said state=on for this seat
    bool mHidden = false;           // driving, but hidden: by the driver (X), or given way to another panel
    bool mYielded = false;          // hidden because another panel took the bottom: it comes back when that one goes
    bool mByHand = false;           // opened from Wolf World without driving (the guide, "sit in a cab")
    LLUUID mSeat;                   // the root the avatar sat on when the region opened the cab
    F64  mLastAt = 0.0;             // when the region last said anything
    // [client review 13 Nov, W3] the levers of a cab that closed only because the regions went quiet (a long hand-over at a
    // border): a cab that opens again for the same seat and train within REOPEN_KEEP_S keeps them, not "fresh"
    // [stage V] walk: kept by CHANGE ENDS for the cab at the other end - any seat of the same train within CAB_WALK_S
    // (DESIGN-terminus.md 5.4: "keep the levers (the existing mKept rule) for CAB_WALK_MS")
    struct Levers { LLUUID seat; std::string train; F64 at = -1.0; S32 notch = 0; F32 power = 0.f, brake = 1.f; S32 rev = 1; bool easyBack = false; bool walk = false; };
    void keepLevers(bool walk);
    void beginWalk(const std::string& train, const std::string& note, const std::string& walk);
    Levers mKept;
    S32  mNotch = NOTCH_STOP;
    F32  mPower = 0.f, mBrake = 1.f;
    S32  mRev = 1;
    bool mEasyBack = false;         // [wolf-rail 1.2, D-c] Easy driving set to BACK
    bool mHornHeld = false;
    // [wolf-rail 1.2] Auto: on as the region says (mode=auto), when the driver last let it go here (an off press, a lever),
    // the on press waiting for the region's answer (until mAutoAskUntil), and the AUTO button's press on the wire (CRUISE
    // held until mAutoPulseUntil)
    bool mAutoWant = false;
    F64  mAutoChosenAt = -1.0e9;
    F64  mAutoAskUntil = -1.0;
    F64  mAutoPulseUntil = -1.0;
    void takeBack();
    void autoLetGo();
    // [wolf-rail 1.2] the POINTS press on the wire (STAGE held until mPointsPulseUntil)
    F64  mPointsPulseUntil = -1.0;
    // [stage V] the SHUNT presses on the wire (ENGINE / ANCHOR / LANDING_GEAR held until these), the walk after CHANGE
    // ENDS and the refusal kept for the driver to read
    F64  mEndsPulseUntil = -1.0, mCouplePulseUntil = -1.0, mUncouplePulseUntil = -1.0;
    std::string mWalkNote;
    F64  mWalkUntil = -1.0;
    std::string mAlert;
};

/** The Train Cab across the bottom of the screen (main_view.xml wolf_train_cab), in the dashboards' family: all drawn
 *  here, sized by dragging its top edge (WolfTrainCabScale), one bottom panel at a time. */
class WolfTrainCabView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfTrainCabView(const Params& p);
    ~WolfTrainCabView() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    enum EHit
    {
        H_NONE = 0,
        H_NOTCH_STOP, H_NOTCH_SLOW, H_NOTCH_GO,
        H_POWER, H_BRAKE,
        H_REV_FWD, H_REV_N, H_REV_REV,
        H_HORN, H_MODE, H_UNITS, H_HELP, H_HELP_BACK, H_HELP_NEXT, H_HELP_CLOSE, H_CLOSE, H_SHOW_TAB,
        H_AUTO, H_POINTS, H_EASY_FWD, H_EASY_BACK,
        H_ENDS, H_COUPLE, H_UNCOUPLE, H_ALERT_OK, H_WALK,   // [wolf-rail 1.3, stage V]
    };

private:
    struct Hit { F32 mL, mB, mR, mT; EHit mId; std::string mTip; };
    std::vector<Hit> mHits;
    F32 mPanelB = 0.f, mPanelT = 0.f;
    F32 mHitScale = 1.f;
    bool mDrawn = false;
    WolfHudResize mResize{ "WolfTrainCabScale" };
    S32 mPressed = H_NONE;
    F32 mLeverB = 0.f, mLeverT = 1.f;   // the levers' slot this frame, picture units (for dragging)
    F32 mHelpL = 0.f, mHelpB = 0.f, mHelpR = 0.f, mHelpT = 0.f;   // the guide card this frame, view units (its clicks are ours)
    F32 mTabL = 0.f, mTabB = 0.f, mTabR = 0.f, mTabT = 0.f;       // the "Train Cab" tab while hidden by the driver, view units
    F32 mAlertL = 0.f, mAlertB = 0.f, mAlertR = 0.f, mAlertT = 0.f;   // [stage V] the refusal strip above the panel, view units
    bool mShowAlert = false;
    bool mShowHelp = false;
    S32 mHelpPage = 0;

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void press(EHit id, S32 y);
    void dragLever(S32 y);
    void drawHelp(F32 top, F32 W);
};

#endif // WOLF_TRAINCAB_H
