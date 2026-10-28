/**
 * @file wolfhelmhud.h
 * @brief WolfViewer: the Captain's Helm HUD for sailing ships, drawn across the bottom of the screen.
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

#ifndef WOLF_HELM_HUD_H
#define WOLF_HELM_HUD_H

#include <string>
#include <vector>

#include "lluictrl.h"
#include "wolfhudscale.h"

// Paul, 2026-10-28: "can we add a pirate ship control hud to the viewer". One control the width of the screen in
// main_view.xml, in the dashboard family (wolfdashboard.cpp: the same 1600 x 304 mock frame with left / centre /
// right anchors, the same size handle, PANEL switch and B612 lettering; everything drawn here, no stock widgets).
// Dark oak and brass, a parchment compass card. The layout, left to right (DESIGN.md s20.5):
//   the ship's wheel (turns with the helm; drag it, double-click for midships) | the sail plan (each sail lit as set)
//   with FURL / BATTLE / PLAIN / ALL, ANCHOR, ASTERN | the status line over a heading-up compass (true wind arrow,
//   apparent wind, the no-go wedge) and the speed log in knots | the wind source (WOLF / REGION / FIXED, LIGHT AIRS)
//   and the battle section (BATTLE MODE with its lock, the flag, seven health bars, PORT / STARBOARD broadsides with
//   reload rings).
// Its data and commands are WolfHelm's (wolfhelm.h). Wolf Territories only.
class WolfHelmHud : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfHelmHud(const Params& p);
    ~WolfHelmHud() override;

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
        H_WHEEL,
        H_FURL, H_BATTLE_SAILS, H_PLAIN, H_ALL,
        H_ANCHOR, H_ASTERN,
        H_WIND_WEATHER, H_WIND_REGION, H_WIND_FIXED, H_LIGHT_AIRS,
        H_KN_DOWN, H_KN_UP, H_FROM_DOWN, H_FROM_UP,
        H_BATTLE_MODE, H_FIRE_PORT, H_FIRE_STBD,
        H_CLOSE, H_HELP, H_PANEL, H_AUTO,
    };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;             // in the picture's units (see mHitScale)
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame: the panel's band, up to the strip's top
    WolfHudResize mResize{ "WolfHelmHudScale" };
    F32 mHitScale = 1.f;
    bool mDrawn = false;
    S32 mPressed = H_NONE;
    S32 mPressX = 0;
    F32 mPressHelm = 0.f;               // the helm when the wheel was grabbed
    F32 mWheelR = 1.f;                  // the wheel's radius this frame, picture units
    F32 mWheelShown = 0.f;              // the wheel's drawn angle, eased toward the helm
    bool mShowHelp = false;
    std::string mRefused;               // why the last press did nothing (shown on the status line)
    F64 mRefusedAt = 0.0;

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void press(EHit id);
    void refuse(const std::string& why);
    void drawHelp(F32 top);
};

#endif // WOLF_HELM_HUD_H
