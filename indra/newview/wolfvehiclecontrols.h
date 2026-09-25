/**
 * @file wolfvehiclecontrols.h
 * @brief The Move floater's Vehicle tab: steering wheel, accelerator pedal, D / N / R lever.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026, IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

#ifndef WOLF_VEHICLECONTROLS_H
#define WOLF_VEHICLECONTROLS_H

#include "llbutton.h"

// Source: WolfStorm js/input/touch_controls.js (Vehicle tab) — the web viewer's version of
// the same controls. Firestorm has no driving controls; everything here drives the agent
// through the same LLAgent calls the Move floater's own buttons and the keyboard use.
namespace WolfVehicle
{
    // Lowest gear first (touch_controls.js GEARS): PageUp steps up, PageDown steps down.
    enum EGear { GEAR_R = 0, GEAR_N = 1, GEAR_D = 2 };

    /** The Move floater is open on its Vehicle tab — the keyboard hooks apply only then. */
    bool    active();

    EGear   gear();
    /** Put the lever in `g`. When `send_to_vehicle` and seated, one PageUp / PageDown tap per
     *  gear stepped goes to the vehicle's script (touch_controls.js _shiftTo). */
    void    setGear(EGear g, bool send_to_vehicle);
    void    shiftGear(S32 steps, bool send_to_vehicle);

    /** The pedal clicked down: it stays down (driving every frame from the idle loop)
     *  until clicked again, the tab changes or the floater closes. */
    void    setPedalLatched(bool on);
    bool    pedalLatched();

    /** Keyboard state for drawing: Up / W holding the pedal, Left / Right steering. */
    void    noteKeyboardPedal(bool held);
    bool    keyboardPedalHeld();
    void    noteKeyboardSteer(S32 dir, bool held);   // dir -1 left, +1 right
    S32     keyboardSteerDir();                       // -1, 0 or +1

    /** Called from LLViewerInput::scanKey for every key before its binding runs. Returns
     *  true when the Vehicle tab has consumed the key; `redirect` is set to another key whose
     *  binding should run instead (Up / W in R runs Down / S). */
    bool    handleScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level,
                          bool repeat, KEY& redirect);
}

/**
 * Steering wheel. Grab it anywhere and drag sideways: horizontal travel from the press turns
 * it (full lock, 90 degrees, at one radius), vertical travel is ignored, so it only ever
 * steers left or right. Past the 0.25 dead zone it turns the agent at Firestorm's own yaw
 * rate scaled by how far it is turned (lljoystickbutton.cpp LLJoystickAgentTurn wolf_analog);
 * released, it springs back to centre. Held Left / Right keys show as full lock.
 */
class WolfSteeringWheel : public LLButton
{
public:
    struct Params : public LLInitParam::Block<Params, LLButton::Params>
    {
        Params() { changeDefault(label, ""); }
    };
    WolfSteeringWheel(const Params& p);

    bool    handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool    handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool    handleHover(S32 x, S32 y, MASK mask) override;
    void    draw() override;

private:
    static void onHeldDown(void* userdata);
    F32     radius() const;

    S32     mPressX;
    F32     mTurn;      // -1 (full left) .. +1 (full right), pointer only
};

/**
 * Accelerator pedal. Click it and it LATCHES down, driving the agent in the lever's direction
 * — forward in D, backward in R, nothing in N — with the stock nudge ramp (llviewerinput.cpp
 * agent_push_forwardbackward); click again and it comes up. It latches rather than needing
 * to be held because a mouse is one pointer: held, it could never steer at the same time
 * (Paul, 2026-09-26). A scripted vehicle gets CONTROL_FWD / CONTROL_BACK and ramps its own
 * speed while the pedal is down. Up / W held on the keyboard also shows it pressed.
 */
class WolfPedal : public LLButton
{
public:
    struct Params : public LLInitParam::Block<Params, LLButton::Params>
    {
        Params() { changeDefault(label, ""); }
    };
    WolfPedal(const Params& p);

    bool    handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool    handleMouseUp(S32 x, S32 y, MASK mask) override;
    void    draw() override;
};

#endif // WOLF_VEHICLECONTROLS_H
