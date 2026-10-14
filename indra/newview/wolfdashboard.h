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
#include "wolfhudscale.h"

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
//   <WolfViewer 2026-10-11> w65 Space Rabbit adds two:
//   SPEEDBOAT  after Mercury VesselView / Yamaha CL7 / Garmin gauge pages: a navy panel, white figures, teal
//          accent; the tachometer (left), the speedometer in knots (right), and a centre screen with the
//          compass, ON PLANE, the depth under the hull, trim, fuel and the speed step; the throttle lever (F N R)
//   SPACESHIP  after Elite Dangerous / Star Citizen / Shuttle glass: cyan lines on glass, amber caution, red
//          warning, magenta target - heading tape, pitch ladder and flight-path marker, speed and throttle,
//          altitude and vertical speed, boost / shield / hull / heat; mode=rocket a launch layout (altitude,
//          vertical speed, apogee, stage N/M and a big SEPARATE button)
//   SUBMARINE  a sonar room in dark green and teal: the depth gauge (red past max_depth), ballast and dive planes,
//          the sea bed under the hull, a sonar scope of what the viewer knows round the boat, battery, oxygen,
//          throttle, speed in knots, DIVE / SURFACE (held), PERISCOPE, PING, LIGHTS, ENGINE
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
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;   // [WOLF RETARDER 2026-10-12/] the stalk
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
        H_INVENTORY,   // [HUD INVENTORY 2026-10-12/] the toolbar's Inventory (WolfHud::toggleInventory)
        H_REZ,         // [HUD REZ 2026-10-12/] rez this design's free Wolf vehicle and sit in it (WolfVehicleRez::rez)
        // <WolfViewer 2026-10-11> w65: the speedboat, spaceship and submarine buttons (WolfDrive::BTN_STAGE ..). [HUD SIZE
        // 2026-10-12] no design buttons for them any more (Paul: "only the car hud needs extra buttons lorry, car, bike
        // the others dont need them"): Wolf World, the toolbar icons and the craft's own script choose those designs.
        H_STAGE, H_BOOST, H_ANCHOR, H_ASSIST, H_LANDING_GEAR, H_ENGINE,
        H_DIVE, H_SURFACE, H_PERISCOPE, H_SONAR,   // the submarine's
        // [WOLF RETARDER 2026-10-12] the truck's retarder stalk: its six positions (click, drag along them, or the
        // scroll wheel over them), downhill speed control on / off and its set speed - / + (held), AUT
        H_RET_0, H_RET_1, H_RET_2, H_RET_3, H_RET_4, H_RET_5,
        H_RET_DSC, H_RET_DSC_DOWN, H_RET_DSC_UP, H_RET_AUT,
    };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;             // in the picture's units (see mHitScale)
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame: the panel's solid band
    // [HUD SIZE 2026-10-12] the size the driver drags the top edge to (WolfDashboardScale), and the scale this frame's
    // picture was drawn at: a point on the view is (x, y) / mHitScale in mHits (1 for the mini bar)
    WolfHudResize mResize{ "WolfDashboardScale" };
    F32 mHitScale = 1.f;
    bool mDrawn = false;

    S32 mPressed = H_NONE;
    S32 mPressX = 0;
    F32 mWheelR = 1.f;
    bool mShowHelp = false;
    S32  mHelpCheckedType = -1;         // which design's first-time guide was last considered

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    /** [WOLF RETARDER 2026-10-12] The stalk position under a view point while dragging (the nearest of the six,
     *  by height, from where the slots were drawn this frame), or -1 when the stalk was not drawn. */
    S32 retarderSlotAt(S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void drawMiniBar(F32 bottom);   // <WolfViewer 2026-10-10/> WolfDashboardMini
    void press(EHit id);
    void release();
    void drawHelp(F32 top);
};

#endif // WOLF_DASHBOARD_H
