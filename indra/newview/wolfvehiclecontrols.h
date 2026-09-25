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

    /** Registers the per-frame drive step (once; LLFloaterMove::postBuild). */
    void    init();

    /** Mouse driving (see the .cpp header). A world click the UI did not take: true when the
     *  Vehicle tab has consumed it (left = accelerator, right = brake). */
    bool    handleWorldMouse(EMouseClickType click, bool down);
    /** Any button-up, wherever the pointer is, ends that button's pedal. */
    void    handleMouseUpAnywhere(EMouseClickType click);
    /** Once a frame from LLViewerWindow::updateUI: did the hover reach the world, and where. */
    void    noteMouseOverWorld(bool over, S32 x);
    /** The mouse over the world is steering; `turn` is -1..1 (for drawing the wheel). */
    bool    mouseSteering(F32& turn);

    /** Pedal state for drawing, from every source (mouse, keyboard, on-screen). */
    bool    accelDown();
    bool    brakeDown();
    void    setScreenPedal(bool brake, bool held);

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
 * Accelerator or brake pedal (wolf_brake="true"). Held, it drives in the lever's direction
 * (accelerator) or against it (brake) — the per-frame work is WolfVehicle's drive step, so a
 * held pedal, a held mouse button over the world and Up / W all end up in one place. It also
 * lights whenever any of those has it down.
 */
class WolfPedal : public LLButton
{
public:
    struct Params : public LLInitParam::Block<Params, LLButton::Params>
    {
        Optional<bool> wolf_brake;
        Params() : wolf_brake("wolf_brake", false) { changeDefault(label, ""); }
    };
    WolfPedal(const Params& p);

    bool    handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool    handleMouseUp(S32 x, S32 y, MASK mask) override;
    void    onMouseCaptureLost() override;
    void    draw() override;

private:
    bool    mBrake;
};

#endif // WOLF_VEHICLECONTROLS_H
