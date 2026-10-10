/**
 * @file wolfdashboard.h
 * @brief WolfViewer: the car / truck / bike dashboard drawn across the bottom of the screen.
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

#ifndef WOLF_DASHBOARD_H
#define WOLF_DASHBOARD_H

#include <string>
#include <vector>

#include "lluictrl.h"

class LLFontGL;

// Paul, 2026-10-09: "I want a proper car dashboard, with a steering wheel, and everything look
// flash with gears proper speedo proper rev counter ... model it on a real car dashboard", "3
// modes, Car, Truck and Bike", and an option to hide the background.
//
// One control the width of the screen in main_view.xml, like the flight deck (wolfflightdeck.cpp,
// the pattern: everything is drawn here, nothing is a stock widget). Three designs, each after a
// real vehicle researched for it (the sources are in wolfdrive.cpp and on each draw function):
//   CAR    a Porsche 911 (992) cluster: the analogue rev counter in the middle, flanked by a
//          speedometer and an information screen, red needles (BMW M), ISO 2575 warning lamps
//   TRUCK  a Scania R cluster: low-revving rev counter with its green economy band, speedometer
//          with the 90 km/h limiter, range-splitter gear display, dual-needle air pressure,
//          fuel and AdBlue, retarder, the tachograph's driving time; a big flat wheel
//   BIKE   a BMW S 1000 RR / Ducati Panigale TFT: bar rev counter along the top with the shift
//          light, the gear in the middle, digital speed, lean angle; handlebars, not a wheel
// The values are WolfDrive::shown(): the vehicle script's (wolfDashboard) or the viewer's model.
class WolfDashboard : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfDashboard(const Params& p);
    ~WolfDashboard() override;

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
        H_WHEEL, H_THROTTLE, H_BRAKE,
        H_SEL_0, H_SEL_1, H_SEL_2, H_SEL_3, H_SEL_4, H_SEL_5, H_SEL_6, H_SEL_7,
        H_SHIFT_UP, H_SHIFT_DOWN, H_BOX_MODE,
        H_TYPE_CAR, H_TYPE_TRUCK, H_TYPE_BIKE,
        H_UNITS, H_PANEL, H_CONTROLS, H_TRIP, H_CLOSE, H_MAP, H_HELP,
        H_LIGHTS, H_IND_L, H_IND_R, H_HANDBRAKE, H_HORN, H_HAZARDS, H_CRUISE,
        // [GEAR STICK 2026-10-10] the manual gate's slots: R, N, then gears 1..12; the truck's
        // range collar (LOW / HIGH) and splitter rocker (L / H) on the knob
        H_GATE_0, H_GATE_1, H_GATE_2, H_GATE_3, H_GATE_4, H_GATE_5, H_GATE_6, H_GATE_7,
        H_GATE_8, H_GATE_9, H_GATE_10, H_GATE_11, H_GATE_12, H_GATE_13,
        H_RANGE, H_SPLIT,
        H_MINI,   // <WolfViewer 2026-10-10/> shrink to the mini bar / bring the dashboard back
    };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame: the panel's solid band
    bool mDrawn = false;

    S32 mPressed = H_NONE;
    S32 mPressX = 0;
    F32 mWheelR = 1.f;
    bool mShowHelp = false;
    S32  mHelpCheckedType = -1;         // which design's first-time guide was last considered

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void drawMiniBar(F32 bottom);   // <WolfViewer 2026-10-10/> WolfDashboardMini
    void press(EHit id);
    void release();
    void drawHelp(F32 top);
};

#endif // WOLF_DASHBOARD_H
