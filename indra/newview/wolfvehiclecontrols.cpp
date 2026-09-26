/**
 * @file wolfvehiclecontrols.cpp
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

// Source: WolfStorm js/input/touch_controls.js (Vehicle tab) and css/touch_controls.css.
//
// The Vehicle tab SHOWS the driving keys — Paul, 2026-09-26: "make it reflect key controls
// for now", after mouse driving and a latching pedal both proved worse than the keyboard every
// driving game uses. The keys behave exactly as on the other tabs; nothing is consumed or
// redirected (noteScanKey only watches, from LLViewerInput::scanKey):
//   Up / W            -> the accelerator lights       Down / S -> the brake lights
//   Left/Right, A / D -> the wheel turns to full lock
//   PageUp / PageDown -> the lever moves one gear; the key still does its usual job (jump /
//                        fly on foot; seated, CONTROL_UP / CONTROL_DOWN to the vehicle's
//                        script via key_bindings.xml <sitting> spin_over_sitting)
// Only unmodified keys: Shift / Ctrl / Alt combinations are camera and slide bindings.
//
// The on-screen controls drive through the same LLAgent calls as those keys:
//   wheel dragged     -> gAgent.moveYaw()          (llviewerinput.cpp agent_turn_left / _right)
//   accelerator held  -> gAgent.moveAt(+1)         (agent_push_forwardbackward)
//   brake held        -> gAgent.moveAt(-1); the brake wins if both are held
//   lever, seated     -> gAgent.moveUp(+1 / -1) held for one short tap per gear stepped — what
//                        PageUp / PageDown do on a vehicle; OpenSim hands a seated agent's
//                        UP_POS / UP_NEG to the script as CONTROL_UP / CONTROL_DOWN, where
//                        gearbox scripts listen. On foot the lever sends nothing.
// The lever is a picture of what was pressed, not the vehicle's real gearbox (the script owns
// that). Every session starts in D.

#include "llviewerprecompiledheaders.h"

#include "wolfvehiclecontrols.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llfloaterreg.h"
#include "llmoveview.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "llvoavatarself.h"

#include <deque>

static LLDefaultChildRegistry::Register<WolfSteeringWheel> r_wheel("wolf_steering_wheel");
static LLDefaultChildRegistry::Register<WolfPedal> r_pedal("wolf_pedal");

namespace
{
    // Source: touch_controls.js DEAD_ZONE (0.25) — the same as the analogue sticks
    // (lljoystickbutton.cpp WOLF_DEAD_ZONE).
    const F32 DEAD_ZONE = 0.25f;
    // Source: touch_controls.js WHEEL_LOCK_DEG.
    const F32 WHEEL_LOCK_DEG = 90.f;
    // Source: touch_controls.js SHIFT_TAP_MS — each tap holds the key this long, then waits
    // this long before the next, so the script sees every tap as its own press.
    const F32 SHIFT_TAP_SECONDS = 0.15f;
    // Source: llviewerinput.cpp NUDGE_TIME — the pedals ease in like the arrow keys.
    const F32 NUDGE_TIME = 0.25f;
    // A held key is scanned every frame (llkeyboardsdl2.cpp scanKeyboard); if its key-up is
    // lost (focus moved away) the drawn state still clears after this long.
    const F64 KEY_STALE_SECONDS = 0.25;

    WolfVehicle::EGear sGear = WolfVehicle::GEAR_D;   // every session starts in D

    bool sKbdPedal = false;
    F64  sKbdPedalSeen = 0.0;
    bool sKbdBrake = false;
    F64  sKbdBrakeSeen = 0.0;
    bool sKbdSteerLeft = false;
    bool sKbdSteerRight = false;
    F64  sKbdSteerSeen = 0.0;

    // Seated gear taps still to send: +1 PageUp, -1 PageDown.
    std::deque<S32> sTapQueue;
    S32  sTapDir = 0;          // the tap being sent now, 0 between taps
    F64  sTapPhaseStart = 0.0;
    bool sTapIdleRegistered = false;

    bool sScreenAccel = false;     // on-screen pedals held with the pointer
    bool sScreenBrake = false;
    S32  sDriveDir = 0;            // what they are doing: +1 / -1 / 0
    F64  sDriveSince = 0.0;
    bool sDriveIdleRegistered = false;

    F64 now()
    {
        return LLFrameTimer::getTotalSeconds();
    }

    bool agentSeated()
    {
        return isAgentAvatarValid() && gAgentAvatarp->isSitting();
    }

    LLFloaterMove* moveFloater()
    {
        return LLFloaterReg::findTypedInstance<LLFloaterMove>("moveview");
    }

    // One PageUp / PageDown press per queued gear: held SHIFT_TAP_SECONDS, then a gap the
    // same length. Runs from the idle loop so it keeps going whatever the floater does.
    void tapIdle(void*)
    {
        if (!agentSeated())
        {
            sTapQueue.clear();   // stood up mid-shift: nothing left to tell the vehicle
            sTapDir = 0;
        }
        const F64 t = now();
        if (sTapDir != 0)
        {
            if (t - sTapPhaseStart < SHIFT_TAP_SECONDS)
            {
                gAgent.moveUp(sTapDir);   // level-held, like agent_jump / agent_push_down
                return;
            }
            sTapDir = 0;                  // released: start the gap
            sTapPhaseStart = t;
            return;
        }
        if (t - sTapPhaseStart < SHIFT_TAP_SECONDS)
        {
            return;
        }
        if (sTapQueue.empty())
        {
            gIdleCallbacks.deleteFunction(tapIdle, nullptr);
            sTapIdleRegistered = false;
            return;
        }
        sTapDir = sTapQueue.front();
        sTapQueue.pop_front();
        sTapPhaseStart = t;
        gAgent.moveUp(sTapDir);
    }

    // Every frame while an on-screen pedal is held: what the Move floater's buttons do from
    // their held-down callbacks, in one place so the brake can win over the accelerator.
    void driveIdle(void*)
    {
        const S32 dir = sScreenBrake ? -1 : sScreenAccel ? 1 : 0;
        const F64 t = now();
        if (dir != sDriveDir)
        {
            sDriveDir = dir;
            sDriveSince = t;
        }
        if (dir == 0 || gAgent.isMovementLocked())
        {
            return;
        }
        // Source: llviewerinput.cpp agent_push_forwardbackward — a nudge first, then full.
        if (t - sDriveSince < NUDGE_TIME)
        {
            gAgent.moveAtNudge(dir);
        }
        else
        {
            gAgent.moveAt(dir);
        }
    }

    // Thick line as two triangles; gl_line_2d is one pixel wide.
    void drawBar(F32 x1, F32 y1, F32 x2, F32 y2, F32 half_width)
    {
        F32 dx = x2 - x1;
        F32 dy = y2 - y1;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.f)
        {
            return;
        }
        const F32 nx = -dy / len * half_width;
        const F32 ny = dx / len * half_width;
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(x1 + nx, y1 + ny);
        gGL.vertex2f(x1 - nx, y1 - ny);
        gGL.vertex2f(x2 - nx, y2 - ny);
        gGL.vertex2f(x1 + nx, y1 + ny);
        gGL.vertex2f(x2 - nx, y2 - ny);
        gGL.vertex2f(x2 + nx, y2 + ny);
        gGL.end();
    }
}

namespace WolfVehicle
{
    bool active()
    {
        LLFloaterMove* floater = moveFloater();
        return floater && floater->getVisible() && !floater->isMinimized()
            && floater->isVehicleStyle();
    }

    void init()
    {
        if (!sDriveIdleRegistered)
        {
            sDriveIdleRegistered = true;
            gIdleCallbacks.addFunction(driveIdle, nullptr);
        }
    }

    EGear gear()
    {
        return sGear;
    }

    void setGear(EGear g, bool send_to_vehicle)
    {
        const S32 steps = (S32)g - (S32)sGear;
        if (steps == 0)
        {
            return;
        }
        sGear = g;

        if (send_to_vehicle && agentSeated())
        {
            const S32 dir = steps > 0 ? 1 : -1;
            for (S32 n = 0; n < llabs(steps); ++n)
            {
                sTapQueue.push_back(dir);
            }
            if (!sTapIdleRegistered)
            {
                sTapIdleRegistered = true;
                sTapDir = 0;
                sTapPhaseStart = 0.0;   // no gap before the first tap
                gIdleCallbacks.addFunction(tapIdle, nullptr);
            }
        }

        if (LLFloaterMove* floater = moveFloater())
        {
            floater->refreshGearButtons();
        }
    }

    void shiftGear(S32 steps, bool send_to_vehicle)
    {
        const S32 g = llclamp((S32)sGear + steps, (S32)GEAR_R, (S32)GEAR_D);
        setGear((EGear)g, send_to_vehicle);
    }

    bool accelDown()
    {
        return sScreenAccel || (sKbdPedal && now() - sKbdPedalSeen < KEY_STALE_SECONDS);
    }

    bool brakeDown()
    {
        return sScreenBrake || (sKbdBrake && now() - sKbdBrakeSeen < KEY_STALE_SECONDS);
    }

    void setScreenPedal(bool brake, bool held)
    {
        (brake ? sScreenBrake : sScreenAccel) = held;
    }

    S32 keyboardSteerDir()
    {
        if (now() - sKbdSteerSeen >= KEY_STALE_SECONDS)
        {
            return 0;
        }
        return (sKbdSteerRight ? 1 : 0) - (sKbdSteerLeft ? 1 : 0);
    }

    void noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat)
    {
        if (mask != MASK_NONE || !active())
        {
            return;
        }
        // Down and up in one slow frame is a tap that has already ended.
        const bool held = key_level || (key_down && !key_up);
        const F64 t = now();

        if (key == KEY_UP || key == 'W')
        {
            sKbdPedal = held;
            sKbdPedalSeen = t;
        }
        else if (key == KEY_DOWN || key == 'S')
        {
            sKbdBrake = held;
            sKbdBrakeSeen = t;
        }
        else if (key == KEY_LEFT || key == 'A')
        {
            sKbdSteerLeft = held;
            sKbdSteerSeen = t;
        }
        else if (key == KEY_RIGHT || key == 'D')
        {
            sKbdSteerRight = held;
            sKbdSteerSeen = t;
        }
        else if ((key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && key_down && !repeat)
        {
            shiftGear(key == KEY_PAGE_UP ? 1 : -1, false);   // the key itself goes on as usual
        }
    }
}

//-----------------------------------------------------------------------------
// WolfSteeringWheel
//-----------------------------------------------------------------------------

WolfSteeringWheel::WolfSteeringWheel(const Params& p)
:   LLButton(p),
    mPressX(0),
    mTurn(0.f)
{
    // Every frame from the first, like the Move floater's own buttons (llmoveview.cpp
    // MOVE_BUTTON_DELAY = 0); the stock button template waits 0.5 s (widgets/button.xml).
    setHeldDownDelay(0.f);
    setHeldDownCallback(&WolfSteeringWheel::onHeldDown, this);
}

F32 WolfSteeringWheel::radius() const
{
    return llmax(24.f, llmin(getRect().getWidth(), getRect().getHeight()) * 0.5f);
}

bool WolfSteeringWheel::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mPressX = x;
    mTurn = 0.f;
    return LLButton::handleMouseDown(x, y, mask);
}

bool WolfSteeringWheel::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        // Source: touch_controls.js _bindWheel onMove — horizontal travel only.
        mTurn = llclamp((F32)(x - mPressX) / radius(), -1.f, 1.f);
    }
    return LLButton::handleHover(x, y, mask);
}

bool WolfSteeringWheel::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mTurn = 0.f;   // springs back to centre
    return LLButton::handleMouseUp(x, y, mask);
}

// static
void WolfSteeringWheel::onHeldDown(void* userdata)
{
    WolfSteeringWheel* self = (WolfSteeringWheel*)userdata;
    if (!self)
    {
        return;
    }
    // Past the dead zone, remapped to 0..1 like the sticks (LLJoystick::wolfAxis), times
    // Firestorm's yaw ramp — full lock is exactly the arrow keys' rate.
    const F32 v = self->mTurn;
    F32 axis = 0.f;
    if (v > DEAD_ZONE) axis = llmin(1.f, (v - DEAD_ZONE) / (1.f - DEAD_ZONE));
    else if (v < -DEAD_ZONE) axis = llmax(-1.f, (v + DEAD_ZONE) / (1.f - DEAD_ZONE));
    if (axis != 0.f)
    {
        // +axis is a turn to the right, i.e. negative yaw (LLJoystickAgentTurn::onHeldDown).
        gAgent.moveYaw(-LLFloaterMove::getYawRate(self->getHeldDownTime()) * axis);
    }
}

// Source: css/touch_controls.css .touch-wheel / .wheel-rim / .wheel-spoke / .wheel-hub /
// .wheel-mark, drawn with primitives in the analogue sticks' palette (WolfJoystick*).
void WolfSteeringWheel::draw()
{
    static LLUIColor rim_col    = LLUIColorTable::instance().getColor("WolfJoystickKnob",         LLColor4(0.922f, 0.961f, 1.f,    0.92f));
    static LLUIColor rim_active = LLUIColorTable::instance().getColor("WolfWheelRimActive",       LLColor4(0.612f, 0.812f, 1.f,    1.f));
    static LLUIColor face       = LLUIColorTable::instance().getColor("WolfJoystickBase",         LLColor4(0.078f, 0.102f, 0.149f, 0.42f));
    static LLUIColor spoke      = LLUIColorTable::instance().getColor("WolfWheelSpoke",           LLColor4(0.784f, 0.882f, 1.f,    0.85f));
    static LLUIColor hub        = LLUIColorTable::instance().getColor("WolfWheelHub",             LLColor4(0.235f, 0.392f, 0.706f, 0.92f));
    static LLUIColor mark       = LLUIColorTable::instance().getColor("WolfWheelMark",            LLColor4(0.961f, 0.690f, 0.255f, 1.f));

    // What turns the drawn wheel: dragging it, else the steering keys (full lock).
    const bool mouse = hasMouseCapture();
    const S32 kbd = mouse ? 0 : WolfVehicle::keyboardSteerDir();
    const F32 turn = mouse ? mTurn : (F32)kbd;
    const bool turning = mouse || kbd != 0;

    LLGLSUIDefault gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const F32 cx = getRect().getWidth() * 0.5f;
    const F32 cy = getRect().getHeight() * 0.5f;
    const F32 R = radius() - 1.f;
    const F32 rim_w = llmax(4.f, R * 0.18f);
    const F32 hub_r = R * 0.30f;

    gGL.pushUIMatrix();
    gGL.translateUI(cx, cy, 0.f);

    // face + rim
    gGL.color4fv(face.get().mV);
    gl_circle_2d(0.f, 0.f, R - rim_w, 48, true);
    const LLColor4 rc = (turning ? rim_active : rim_col).get();
    gl_washer_2d(R, R - rim_w, 64, rc, rc);

    // Clockwise on screen for a right turn; LLView's y grows up, so that is a negative angle.
    const F32 a = -turn * WHEEL_LOCK_DEG * DEG_TO_RAD;
    const F32 ca = cosf(a);
    const F32 sa = sinf(a);
    auto rot = [&](F32 x, F32 y, F32& ox, F32& oy) { ox = x * ca - y * sa; oy = x * sa + y * ca; };

    // spokes: left, right and bottom, hub to rim
    gGL.color4fv(spoke.get().mV);
    const F32 spoke_hw = llmax(2.f, R * 0.07f);
    const F32 in = hub_r;
    const F32 out = R - rim_w * 0.5f;
    const F32 dirs[3][2] = { { -1.f, 0.f }, { 1.f, 0.f }, { 0.f, -1.f } };
    for (const auto& d : dirs)
    {
        F32 x1, y1, x2, y2;
        rot(d[0] * in, d[1] * in, x1, y1);
        rot(d[0] * out, d[1] * out, x2, y2);
        drawBar(x1, y1, x2, y2, spoke_hw);
    }

    // hub
    gGL.color4fv(hub.get().mV);
    gl_circle_2d(0.f, 0.f, hub_r, 32, true);
    gGL.color4fv(rim_col.get().mV);
    gl_circle_2d(0.f, 0.f, hub_r, 32, false);

    // top-dead-centre marker on the rim, so the turn reads at a glance
    F32 mx, my;
    rot(0.f, R - rim_w * 0.5f, mx, my);
    gGL.color4fv(mark.get().mV);
    gl_circle_2d(mx, my, rim_w * 0.55f, 16, true);

    gGL.popUIMatrix();
}


//-----------------------------------------------------------------------------
// WolfPedal
//-----------------------------------------------------------------------------

WolfPedal::WolfPedal(const Params& p)
:   LLButton(p),
    mBrake(p.wolf_brake)
{
}

// Held with the pointer: driveIdle reads the flag every frame.
bool WolfPedal::handleMouseDown(S32 x, S32 y, MASK mask)
{
    WolfVehicle::setScreenPedal(mBrake, true);
    return LLButton::handleMouseDown(x, y, mask);
}

bool WolfPedal::handleMouseUp(S32 x, S32 y, MASK mask)
{
    WolfVehicle::setScreenPedal(mBrake, false);
    return LLButton::handleMouseUp(x, y, mask);
}

void WolfPedal::onMouseCaptureLost()
{
    WolfVehicle::setScreenPedal(mBrake, false);
    LLButton::onMouseCaptureLost();
}

// Source: css/touch_controls.css .touch-pedal / .touch-pedal-ribs / .active / .neutral.
void WolfPedal::draw()
{
    static LLUIColor body        = LLUIColorTable::instance().getColor("WolfPedalBody",        LLColor4(0.227f, 0.259f, 0.322f, 0.95f));
    static LLUIColor body_press  = LLUIColorTable::instance().getColor("WolfPedalBodyPressed", LLColor4(0.275f, 0.451f, 0.784f, 0.95f));
    static LLUIColor border      = LLUIColorTable::instance().getColor("WolfJoystickBorder",   LLColor4(0.627f, 0.784f, 1.f,    0.50f));
    static LLUIColor border_act  = LLUIColorTable::instance().getColor("WolfJoystickBorderActive", LLColor4(0.745f, 0.882f, 1.f, 0.90f));
    static LLUIColor ribs        = LLUIColorTable::instance().getColor("WolfPedalRibs",        LLColor4(0.824f, 0.882f, 0.961f, 0.55f));
    // css/touch_controls.css .touch-brake.active: red, so the two pedals never read alike.
    static LLUIColor brake_press = LLUIColorTable::instance().getColor("WolfBrakeBodyPressed", LLColor4(0.706f, 0.216f, 0.216f, 0.95f));
    static LLUIColor brake_rim   = LLUIColorTable::instance().getColor("WolfBrakeBorderActive", LLColor4(1.f, 0.745f, 0.745f, 0.90f));

    const bool pressed = mBrake ? WolfVehicle::brakeDown() : WolfVehicle::accelDown();
    const F32 alpha = 1.f;

    LLGLSUIDefault gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    // Pressed, the pad sinks and narrows a little (the CSS tips it away from the viewer).
    const S32 sink = pressed ? llmax(2, h / 16) : 0;
    const S32 inset = pressed ? llmax(1, w / 16) : 0;
    const S32 l = inset, r = w - 1 - inset, t = h - 1 - sink, b = 0;

    LLColor4 c = (pressed ? (mBrake ? brake_press : body_press) : body).get();
    c.mV[VALPHA] *= alpha;
    gl_rect_2d(l, t, r, b, c, true);
    c = (pressed ? (mBrake ? brake_rim : border_act) : border).get();
    c.mV[VALPHA] *= alpha;
    gl_rect_2d(l, t, r, b, c, false);

    // tread ribs across the pad
    c = ribs.get();
    c.mV[VALPHA] *= alpha;
    const S32 pad = llmax(4, (r - l) / 5);
    const S32 rib_h = llmax(2, h / 26);
    const S32 pitch = rib_h * 3;
    for (S32 y = t - pad; y - rib_h > b + pad; y -= pitch)
    {
        gl_rect_2d(l + pad, y, r - pad, y - rib_h, c, true);
    }
}
