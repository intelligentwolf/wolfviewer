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
    void setHorn(bool held) { mHornHeld = held; }
    bool horn() const { return mHornHeld; }
    bool mph() const;
    void setMph(bool on);

    /** What WolfDrive sends while driving() (see the class comment). */
    F32 sendPower() const;
    F32 sendBrake() const;
    S32 sendGear() const;
    U32 sendButtons() const;

    /** Up / W one step more, Down / S one step less (wolfdrive.cpp noteScanKey, while driving()). */
    void noteKey(KEY key, bool key_down, bool repeat);

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
    S32  mNotch = NOTCH_STOP;
    F32  mPower = 0.f, mBrake = 1.f;
    S32  mRev = 1;
    bool mHornHeld = false;
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
