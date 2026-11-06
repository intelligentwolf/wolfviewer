/**
 * @file wolfskihud.h
 * @brief WolfViewer: the Ski HUD - speed, steepness, the run's clock, air time and a style score, across the bottom.
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

#ifndef WOLF_SKI_HUD_H
#define WOLF_SKI_HUD_H

#include <string>
#include <vector>

#include "lluictrl.h"
#include "wolfhudscale.h"

// [SKIS 2026-11-06] Paul: "add a new hud for it that will be fun". One control the width of the screen in main_view.xml,
// in the dashboard family (wolfhelmhud.cpp: the same 1600 x 304 mock frame with left / centre / right anchors - always in
// one row - the size handle, B612 lettering; everything drawn here, no stock widgets), after the ski-tracker apps and the trick-score
// call-outs of snow games: a big speedometer, the steepness with its piste colour, the run's clock and vertical, the air
// time, the style score with its combo, the best here, a compass, and big friendly pop-ups ("Big air! 1.8 s").
// Night blue and ice. Its numbers are WolfSkis' (wolfskis.h). Wolf Territories only, and only while riding.
class WolfSkiHud : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfSkiHud(const Params& p);
    ~WolfSkiHud() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    enum EHit { H_NONE = 0, H_UNITS, H_TAKE_OFF, H_HIDE, H_HELP, H_PANEL };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;             // in the picture's units (see mHitScale)
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame
    WolfHudResize mResize{ "WolfSkiHudScale" };
    F32 mHitScale = 1.f;
    bool mDrawn = false;
    S32 mPressed = H_NONE;
    bool mShowHelp = false;
    F32 mNeedle = 0.f;                  // the speedometer needle, eased toward the speed (display units)

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void press(EHit id);
    void drawPopups(F32 band_top, F32 width);
    void drawHelp(F32 band_top, F32 width);
};

#endif // WOLF_SKI_HUD_H
