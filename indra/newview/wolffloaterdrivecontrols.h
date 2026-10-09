/**
 * @file wolffloaterdrivecontrols.h
 * @brief WolfViewer: Driving Controls - map a wheel, pedals or a gamepad to driving.
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

#ifndef WOLF_FLOATERDRIVECONTROLS_H
#define WOLF_FLOATERDRIVECONTROLS_H

#include "llfloater.h"
#include "llviewerjoystick.h"
#include "wolfdrive.h"

class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLProgressBar;
class LLTextBox;

// Who: a driver with a steering wheel and pedals (or a gamepad). What: "tell the viewer which
// axis is my wheel and which my pedals, see it work, then drive". The flow, top to bottom: the
// controller and whether it drives -> each axis (pick it or press Detect and move it) with a live
// bar -> the dead zone -> the buttons -> the status line, which says every result and every
// problem in words. Wolf Territories only, like all of WolfDrive: off Wolf it cannot be opened.
class WolfFloaterDriveControls : public LLFloater
{
public:
    WolfFloaterDriveControls(const LLSD& key);
    ~WolfFloaterDriveControls() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onFocusLost() override;

private:
    void refreshFromSettings();
    void startDetect(S32 what);     // 0..AXIS_ROLE_COUNT-1 an axis role, 100 + n button action n
    void stepDetect();
    void setStatus(const std::string& text, bool problem);
    void refreshActions();
    void saveAction(S32 action);
    void startKeyCapture(S32 action);

    LLComboBox*     mAxisCombo[WolfDrive::AXIS_ROLE_COUNT] = {};
    LLCheckBoxCtrl* mInvert[WolfDrive::AXIS_ROLE_COUNT] = {};
    LLCheckBoxCtrl* mFull[WolfDrive::AXIS_ROLE_COUNT] = {};
    LLProgressBar*  mBar[WolfDrive::AXIS_ROLE_COUNT] = {};
    LLTextBox*      mValue[WolfDrive::AXIS_ROLE_COUNT] = {};
    LLComboBox*     mButtonCombo[WolfDrive::ACTION_COUNT] = {};
    LLTextBox*      mButtonLit[WolfDrive::ACTION_COUNT] = {};
    LLTextBox*      mKeyText[WolfDrive::ACTION_COUNT] = {};
    LLComboBox*     mOutCombo[WolfDrive::ACTION_COUNT] = {};
    LLLineEditor*   mCmdEdit[WolfDrive::ACTION_COUNT] = {};
    LLLineEditor*   mChanEdit[WolfDrive::ACTION_COUNT] = {};
    S32             mCapture = -1;   // the action whose key is being set, or -1
    LLTextBox*      mDevice = nullptr;
    LLTextBox*      mStatus = nullptr;

    S32  mDetect = -1;
    F64  mDetectStart = 0.0;
    F32  mBaseAxes[MAX_JOYSTICK_AXES] = {};
    bool mBaseButtons[MAX_JOYSTICK_BUTTONS] = {};
};

#endif // WOLF_FLOATERDRIVECONTROLS_H
