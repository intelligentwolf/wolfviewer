/**
 * @file wolffloaterdrivecontrols.cpp
 * @brief WolfViewer: Driving Controls. See wolffloaterdrivecontrols.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaterdrivecontrols.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llprogressbar.h"
#include "indra_constants.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"

namespace
{
    const F64 DETECT_SECONDS = 10.0;
    // How far an axis must move from where it rested to count as "the one you moved". Axes read
    // -1..1 (llviewerjoystick.cpp updateStatus: axes / axes_max), so 0.5 is a quarter of travel.
    const F32 DETECT_TRAVEL = 0.5f;
    // A resting value this close to -1 or +1 is a pedal that rests at an end of its axis.
    const F32 REST_AT_END = 0.7f;

    const char* const ROLE_NAMES[WolfDrive::AXIS_ROLE_COUNT] = { "steer", "throttle", "brake", "clutch" };
    const char* const ROLE_ASK[WolfDrive::AXIS_ROLE_COUNT] =
    {
        "Turn the steering wheel (or stick) all the way to the RIGHT now...",
        "Press the accelerator (or trigger) all the way down now...",
        "Press the brake all the way down now...",
        "Press the clutch all the way down now...",
    };
    // Unmodified keys with a default job of their own (app_settings/key_bindings.xml, mask="NONE"):
    // driving and flying (W A S D E C F, the arrows, Space, Home, R / T roll), chat (ENTER
    // start_chat) and gestures (DIVIDE start_gesture). Allowed, with a warning that they do both;
    // the dashboard only watches keys (WolfDrive::noteScanKey), it never takes them away.
    bool busyKey(KEY k)
    {
        return k == 'W' || k == 'A' || k == 'S' || k == 'D' || k == 'E' || k == 'C' || k == 'F'
            || k == 'R' || k == 'T'
            || k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT
            || k == ' ' || k == KEY_HOME || k == KEY_RETURN || k == KEY_DIVIDE;
    }

    /** A channel typed by the driver: digits only, 0 to 2147483646 (2147483647 is CHAT_CHANNEL_DEBUG,
     *  indra_constants.h, which is not a vehicle's). Empty means 0, said aloud. */
    bool parseChannel(const std::string& text, S32& out)
    {
        if (text.empty())
        {
            out = 0;
            return true;
        }
        if (text.size() > 10)
        {
            return false;
        }
        S64 v = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9') return false;
            v = v * 10 + (ch - '0');
        }
        if (v > (S64)CHAT_CHANNEL_DEBUG - 1)
        {
            return false;
        }
        out = (S32)v;
        return true;
    }

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
}

WolfFloaterDriveControls::WolfFloaterDriveControls(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterDriveControls::~WolfFloaterDriveControls()
{
}

bool WolfFloaterDriveControls::postBuild()
{
    mDevice = getChild<LLTextBox>("device");
    mStatus = getChild<LLTextBox>("status");
    for (S32 r = 0; r < WolfDrive::AXIS_ROLE_COUNT; ++r)
    {
        const std::string n = ROLE_NAMES[r];
        mAxisCombo[r] = getChild<LLComboBox>(n + "_axis");
        mAxisCombo[r]->add("None", LLSD::Integer(-1));
        for (S32 a = 0; a < MAX_JOYSTICK_AXES; ++a)
        {
            mAxisCombo[r]->add(llformat("Axis %d", a), LLSD::Integer(a));
        }
        mAxisCombo[r]->setCommitCallback([r](LLUICtrl* c, const LLSD&)
        {
            gSavedSettings.setS32(WolfDrive::axisSetting(r), c->getValue().asInteger());
        });
        mInvert[r] = getChild<LLCheckBoxCtrl>(n + "_invert");
        mInvert[r]->setCommitCallback([r](LLUICtrl* c, const LLSD&)
        {
            gSavedSettings.setBOOL(WolfDrive::axisInvertSetting(r), c->getValue().asBoolean());
        });
        if (r != WolfDrive::AXIS_STEER)
        {
            mFull[r] = getChild<LLCheckBoxCtrl>(n + "_full");
            mFull[r]->setCommitCallback([r](LLUICtrl* c, const LLSD&)
            {
                gSavedSettings.setBOOL(WolfDrive::axisFullRangeSetting(r), c->getValue().asBoolean());
            });
        }
        mBar[r] = getChild<LLProgressBar>(n + "_bar");
        mValue[r] = getChild<LLTextBox>(n + "_value");
        getChild<LLButton>(n + "_detect")->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { startDetect(r); });
    }
    for (S32 a = 0; a < WolfDrive::ACTION_COUNT; ++a)
    {
        const std::string n = WolfDrive::actionId(a);
        mButtonCombo[a] = getChild<LLComboBox>(n + "_button");
        mButtonCombo[a]->add("None", LLSD::Integer(-1));
        for (S32 b = 0; b < MAX_JOYSTICK_BUTTONS; ++b)
        {
            mButtonCombo[a]->add(llformat("Button %d", b), LLSD::Integer(b));
        }
        mButtonCombo[a]->setCommitCallback([a](LLUICtrl* c, const LLSD&)
        {
            gSavedSettings.setS32(WolfDrive::actionSetting(a), c->getValue().asInteger());
        });
        mButtonLit[a] = getChild<LLTextBox>(n + "_lit");
        getChild<LLButton>(n + "_detect")->setCommitCallback([this, a](LLUICtrl*, const LLSD&) { startDetect(100 + a); });
        mKeyText[a] = getChild<LLTextBox>(n + "_key");
        getChild<LLButton>(n + "_setkey")->setCommitCallback([this, a](LLUICtrl*, const LLSD&) { startKeyCapture(a); });
        mOutCombo[a] = getChild<LLComboBox>(n + "_out");
        for (S32 o = 0; o < WolfDrive::OUT_COUNT; ++o)
        {
            mOutCombo[a]->add(WolfDrive::outLabel(o), LLSD::Integer(o));
        }
        mOutCombo[a]->setCommitCallback([this, a](LLUICtrl*, const LLSD&) { saveAction(a); });
        mCmdEdit[a] = getChild<LLLineEditor>(n + "_cmd");
        mCmdEdit[a]->setCommitOnFocusLost(true);
        mCmdEdit[a]->setCommitCallback([this, a](LLUICtrl*, const LLSD&) { saveAction(a); });
        mChanEdit[a] = getChild<LLLineEditor>(n + "_ch");
        mChanEdit[a]->setCommitOnFocusLost(true);
        mChanEdit[a]->setCommitCallback([this, a](LLUICtrl*, const LLSD&) { saveAction(a); });
    }
    getChild<LLButton>("reset_actions")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        WolfDrive::instance().resetActions();
        mCapture = -1;
        refreshFromSettings();
        setStatus("Keys, buttons and what each action sends are back to their defaults.", false);
    });
    getChild<LLButton>("joystick_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLFloaterReg::showInstance("pref_joystick");
    });
    refreshFromSettings();
    return true;
}

void WolfFloaterDriveControls::onOpen(const LLSD& key)
{
    // [WOLF ONLY] Paul 2026-10-09: "other grids can't use these interfaces they are only for wolf".
    // The menu item is gone off Wolf; any other way in closes again before touching the controller.
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    if (!LLViewerJoystick::getInstance()->isJoystickInitialized())
    {
        // Source: llfloaterjoystick.cpp constructor - the same first look for a device.
        LLViewerJoystick::getInstance()->init(false);
    }
    refreshFromSettings();
    mDetect = -1;
    setStatus("Pick each control, or press Detect beside it and move that control.", false);
}

void WolfFloaterDriveControls::refreshFromSettings()
{
    for (S32 r = 0; r < WolfDrive::AXIS_ROLE_COUNT; ++r)
    {
        mAxisCombo[r]->selectByValue(LLSD::Integer(llclamp(gSavedSettings.getS32(WolfDrive::axisSetting(r)), -1, MAX_JOYSTICK_AXES - 1)));
        mInvert[r]->set(gSavedSettings.getBOOL(WolfDrive::axisInvertSetting(r)));
        if (mFull[r]) mFull[r]->set(gSavedSettings.getBOOL(WolfDrive::axisFullRangeSetting(r)));
    }
    for (S32 a = 0; a < WolfDrive::ACTION_COUNT; ++a)
    {
        mButtonCombo[a]->selectByValue(LLSD::Integer(llclamp(gSavedSettings.getS32(WolfDrive::actionSetting(a)), -1, MAX_JOYSTICK_BUTTONS - 1)));
    }
    refreshActions();
}

void WolfFloaterDriveControls::refreshActions()
{
    WolfDrive& d = WolfDrive::instance();
    for (S32 a = 0; a < WolfDrive::ACTION_COUNT; ++a)
    {
        const WolfDrive::ActionCfg c = d.actionCfg(a);
        mKeyText[a]->setText(a == mCapture ? std::string("press a key") : (c.mKey.empty() ? std::string("none") : c.mKey));
        mOutCombo[a]->selectByValue(LLSD::Integer(c.mOut));
        mCmdEdit[a]->setText(c.mCmd);
        mChanEdit[a]->setText(llformat("%d", c.mChannel));
        const bool chat = c.mOut == WolfDrive::OUT_CHAT;
        mCmdEdit[a]->setEnabled(chat);
        mChanEdit[a]->setEnabled(chat);
        if (a == WolfDrive::ACT_HORN)
        {
            mOutCombo[a]->setToolTip(c.mOut == WolfDrive::OUT_NONE
                ? std::string("Horn: none set - uses the built-in horn (car, truck or bike, by the dashboard's design), played in-world on Wolf Territories")
                : std::string("What the horn sends the vehicle"));
        }
    }
}

void WolfFloaterDriveControls::saveAction(S32 action)
{
    WolfDrive& d = WolfDrive::instance();
    WolfDrive::ActionCfg c = d.actionCfg(action);
    c.mOut = mOutCombo[action]->getValue().asInteger();
    c.mCmd = mCmdEdit[action]->getText();
    LLStringUtil::trim(c.mCmd);
    S32 ch = 0;
    std::string chs = mChanEdit[action]->getText();
    LLStringUtil::trim(chs);
    if (!parseChannel(chs, ch))
    {
        setStatus(std::string("The channel for ") + WolfDrive::actionLabel(action)
                  + " must be a whole number from 0 (said aloud) to 2147483646. Nothing was saved.", true);
        refreshActions();
        return;
    }
    c.mChannel = ch;
    d.setActionCfg(action, c);
    refreshActions();
    if (c.mOut == WolfDrive::OUT_CHAT && c.mCmd.empty())
    {
        setStatus(std::string(WolfDrive::actionLabel(action)) + ": type the command the vehicle listens for.", true);
    }
}

void WolfFloaterDriveControls::startKeyCapture(S32 action)
{
    mCapture = action;
    gFocusMgr.setKeyboardFocus(this);   // the next key comes here (handleKeyHere), not to the world
    refreshActions();
    setStatus(std::string("Press the key you want for ") + WolfDrive::actionLabel(action)
              + ". Escape cancels, Backspace removes the key.", false);
}

bool WolfFloaterDriveControls::handleKeyHere(KEY key, MASK mask)
{
    if (mCapture < 0)
    {
        return LLFloater::handleKeyHere(key, mask);
    }
    const S32 a = mCapture;
    if (key == KEY_SHIFT || key == KEY_CONTROL || key == KEY_ALT)
    {
        return true;   // a modifier on its own: wait for the key
    }
    if (mask != MASK_NONE && key != KEY_ESCAPE && key != KEY_BACKSPACE && key != KEY_DELETE)
    {
        // still waiting: the driver can press the key on its own straight away
        setStatus("Use a key on its own: Shift, Ctrl and Alt combinations are the camera's and the viewer's. "
                  "Press another key, or Escape to cancel.", true);
        return true;
    }
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)
    {
        // they already shift: the vehicle gets them as CONTROL_UP / CONTROL_DOWN and the picture
        // follows them (WolfDrive::noteScanKey), so as an action key they would shift twice
        setStatus("Page Up and Page Down are already the gear keys. Press another key, or Escape to cancel.", true);
        return true;
    }
    mCapture = -1;
    WolfDrive& d = WolfDrive::instance();
    WolfDrive::ActionCfg c = d.actionCfg(a);
    if (key == KEY_ESCAPE)
    {
        setStatus("Nothing was changed.", false);
    }
    else if (key == KEY_BACKSPACE || key == KEY_DELETE)
    {
        c.mKey.clear();
        d.setActionCfg(a, c);
        setStatus(std::string(WolfDrive::actionLabel(a)) + " has no key now.", false);
    }
    else
    {
        c.mKey = LLKeyboard::stringFromKey(key, false);
        // one key, one action: take it off any other action that had it
        std::string moved;
        for (S32 o = 0; o < WolfDrive::ACTION_COUNT; ++o)
        {
            if (o == a) continue;
            WolfDrive::ActionCfg oc = d.actionCfg(o);
            if (oc.mKey == c.mKey)
            {
                oc.mKey.clear();
                d.setActionCfg(o, oc);
                moved += std::string(moved.empty() ? "" : ", ") + WolfDrive::actionLabel(o);
            }
        }
        d.setActionCfg(a, c);
        std::string msg = std::string(WolfDrive::actionLabel(a)) + " is now the " + c.mKey + " key";
        if (!moved.empty()) msg += " (taken from " + moved + ")";
        msg += busyKey(key) ? " - it has its own job too (driving, the camera, chat or gestures), so it will do both." : ".";
        setStatus(msg, busyKey(key));
    }
    refreshActions();
    return true;
}

void WolfFloaterDriveControls::onFocusLost()
{
    if (mCapture >= 0)
    {
        mCapture = -1;
        refreshActions();
        setStatus("Setting the key was cancelled.", false);
    }
    LLFloater::onFocusLost();
}

void WolfFloaterDriveControls::setStatus(const std::string& text, bool problem)
{
    mStatus->setText(text);
    mStatus->setColor(problem ? LLColor4(1.f, 0.72f, 0.2f, 1.f) : LLColor4(0.85f, 0.88f, 0.92f, 1.f));
}

void WolfFloaterDriveControls::startDetect(S32 what)
{
    WolfDrive& d = WolfDrive::instance();
    if (!d.controllerReady())
    {
        setStatus("No controller is enabled. Open Joystick Configuration, pick your wheel or gamepad, then try again.", true);
        return;
    }
    LLViewerJoystick* js = LLViewerJoystick::getInstance();
    for (S32 i = 0; i < MAX_JOYSTICK_AXES; ++i) mBaseAxes[i] = js->getJoystickAxis(i);
    for (S32 i = 0; i < MAX_JOYSTICK_BUTTONS; ++i) mBaseButtons[i] = js->getJoystickButton(i) != 0;
    mDetect = what;
    mDetectStart = now();
    if (what >= 100)
    {
        setStatus(std::string("Press the button you want for ") + WolfDrive::actionLabel(what - 100) + " now...", false);
    }
    else
    {
        setStatus(std::string("Leave every control at rest, then: ") + ROLE_ASK[what], false);
    }
}

void WolfFloaterDriveControls::stepDetect()
{
    if (mDetect < 0)
    {
        return;
    }
    if (now() - mDetectStart > DETECT_SECONDS)
    {
        mDetect = -1;
        setStatus("Nothing moved in 10 seconds - nothing was changed. Is the right controller picked in Joystick Configuration?", true);
        return;
    }
    LLViewerJoystick* js = LLViewerJoystick::getInstance();
    if (mDetect >= 100)
    {
        const S32 action = mDetect - 100;
        const S32 n = (S32)llmin((U32)MAX_JOYSTICK_BUTTONS, js->getNumOfJoystickButtons());
        for (S32 b = 0; b < n; ++b)
        {
            if (js->getJoystickButton(b) && !mBaseButtons[b])
            {
                gSavedSettings.setS32(WolfDrive::actionSetting(action), b);
                mDetect = -1;
                refreshFromSettings();
                setStatus(llformat("%s is now button %d.", WolfDrive::actionLabel(action), b), false);
                return;
            }
        }
        return;
    }
    const S32 role = mDetect;
    const S32 n = (S32)llmin((U32)MAX_JOYSTICK_AXES, js->getNumOfJoystickAxes());
    S32 best = -1;
    F32 best_d = DETECT_TRAVEL;
    for (S32 a = 0; a < n; ++a)
    {
        const F32 dlt = fabsf(js->getJoystickAxis(a) - mBaseAxes[a]);
        if (dlt > best_d)
        {
            best_d = dlt;
            best = a;
        }
    }
    if (best < 0)
    {
        return;
    }
    const F32 rest = mBaseAxes[best];
    const F32 moved = js->getJoystickAxis(best) - rest;
    gSavedSettings.setS32(WolfDrive::axisSetting(role), best);
    std::string how;
    if (role == WolfDrive::AXIS_STEER)
    {
        // asked for RIGHT: an axis that went negative is reversed
        gSavedSettings.setBOOL(WolfDrive::axisInvertSetting(role), moved < 0.f);
        how = moved < 0.f ? " (reversed)" : "";
    }
    else
    {
        // A pedal resting at one end travels the whole axis; one resting in the middle, half.
        const bool at_end = fabsf(rest) > REST_AT_END;
        const bool invert = moved < 0.f;
        gSavedSettings.setBOOL(WolfDrive::axisInvertSetting(role), invert);
        gSavedSettings.setBOOL(WolfDrive::axisFullRangeSetting(role), at_end);
        how = std::string(at_end ? " (rests at the end of its travel" : " (rests in the middle") + (invert ? ", reversed)" : ")");
    }
    mDetect = -1;
    refreshFromSettings();
    static const char* const role_label[] = { "Steering", "Accelerator", "Brake", "Clutch" };
    setStatus(llformat("%s is now axis %d%s. Check the bar moves the right way.", role_label[role], best, how.c_str()), false);
}

void WolfFloaterDriveControls::draw()
{
    WolfDrive& d = WolfDrive::instance();
    const bool ready = d.controllerReady();
    if (ready)
    {
        const std::string name = LLViewerJoystick::getInstance()->getDescription();
        mDevice->setText("Controller: " + (name.empty() ? std::string("(unnamed)") : name)
                         + (d.hardwareDriving() ? "  -  driving now" : ""));
        mDevice->setColor(LLColor4(0.6f, 1.f, 0.6f, 1.f));
    }
    else
    {
        mDevice->setText(std::string("No controller enabled - pick one in Joystick Configuration."));
        mDevice->setColor(LLColor4(1.f, 0.72f, 0.2f, 1.f));
    }
    for (S32 r = 0; r < WolfDrive::AXIS_ROLE_COUNT; ++r)
    {
        const F32 v = d.hardwareValue(r);
        if (r == WolfDrive::AXIS_STEER)
        {
            mBar[r]->setValue(50.f + v * 50.f);
            mValue[r]->setText(v == 0.f ? std::string("centre") : llformat("%s %d%%", v < 0.f ? "left" : "right", (S32)llround(fabsf(v) * 100.f)));
        }
        else
        {
            mBar[r]->setValue(v * 100.f);
            mValue[r]->setText(llformat("%d%%", (S32)llround(v * 100.f)));
        }
    }
    const bool combined = gSavedSettings.getBOOL("WolfDriveCombinedPedals");
    mAxisCombo[WolfDrive::AXIS_BRAKE]->setEnabled(!combined);
    mInvert[WolfDrive::AXIS_BRAKE]->setEnabled(!combined);
    mFull[WolfDrive::AXIS_BRAKE]->setEnabled(!combined);
    for (S32 a = 0; a < WolfDrive::ACTION_COUNT; ++a)
    {
        const bool on = d.hardwareButton(a);
        mButtonLit[a]->setText(std::string(on ? "PRESSED" : ""));
    }
    stepDetect();
    LLFloater::draw();
}
