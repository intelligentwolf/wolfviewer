/**
 * @file wolfgreedyhud.h
 * @brief WolfViewer: the Greedy HUD - the dice game along the bottom of the screen.
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

#ifndef WOLF_GREEDYHUD_H
#define WOLF_GREEDYHUD_H

#include <string>
#include <vector>

#include "lluictrl.h"
#include "wolfhudscale.h"

/**
 * [W104 FEATFIX 2026-11-11] Paul: "the greedy thing needs to be a HUD not a floater, so users can play it while wandering
 * around the grid, with the same hiding of the background etc."
 * Persona: Margaret, 62, walking round a market region with friends, UI Size 1.2. Story: "I switch Greedy on, the dice sit
 * along the bottom of the screen, I press ROLL and BANK as I wander, and the world stays in view."
 * Flow: Wolf World > Greedy, or the Wolf HUDs window -> the HUD (the game this login is in is loaded) -> NEW GAME / PRACTICE /
 * JOIN (the window's list of games) -> ROLL (the dice tumble and land) -> ROLL again or BANK -> "You banked 350!" / "Bust!".
 * HOW TO PLAY opens the guide's cards (the Greedy window). LESS shrinks it to one row; CLOSE switches it off.
 *
 * One control the width of the screen in main_view.xml ("wolf_greedy_hud"), in the dashboard family exactly as the Ski HUD
 * (wolfskihud.cpp; the Fishing HUD of branch fishing-overlay is built the same way): the 1600 x 304 mock band with left /
 * centre / right anchors above the bottom toolbar, its height a share of the window times UI Size, the night-blue band with
 * the frosted top edge or (BACKGROUND off, per resident: WolfGreedyHudBackground) only the controls and outlined words over the
 * world, B612 lettering, the same rounded buttons, the same size handle on its top edge (WolfGreedyHudScale, wolfhudscale.h).
 * Left: the Greedy logo and the scoreboard. Centre: the state, the six 3D dice (they tumble when a roll lands; a click lands
 * them), this turn's points, the moment ("Bust!", "You banked 350!") and the message area (newest first; a problem in amber
 * stays until something newer happens - no big boxes over the scenery). Right: ROLL and BANK, big; NEW GAME / PRACTICE / JOIN
 * or LEAVE / PASS; HOW TO PLAY, BACKGROUND, LESS / MORE, CLOSE.
 * The game itself is WolfGreedyGame (wolffloatergreedy.h), shared with the Greedy window. Draws nothing and takes no clicks
 * unless it is switched on and the agent is on Wolf Territories; outside the band every click goes to the world.
 */
class WolfGreedyHud : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfGreedyHud(const Params& p);
    ~WolfGreedyHud() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    enum EHit { H_NONE = 0, H_ROLL, H_BANK, H_NEW, H_PRACTICE, H_JOIN, H_LEAVE, H_PASS, H_HELP, H_WINDOW, H_PANEL, H_LESS,
                H_CLOSE, H_DICE };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;             // in the picture's units (see mHitScale)
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame
    WolfHudResize mResize{ "WolfGreedyHudScale" };
    F32 mHitScale = 1.f;
    bool mDrawn = false;
    S32 mPressed = H_NONE;
    bool mOtherWasUp = false;           // [review] another bottom panel was showing last frame (WolfGreedyGame::vehiclePanelUp)

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void press(EHit id);
};

#endif // WOLF_GREEDYHUD_H
