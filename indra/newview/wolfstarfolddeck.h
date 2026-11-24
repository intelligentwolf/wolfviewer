/**
 * @file wolfstarfolddeck.h
 * @brief WolfViewer: Starfold's cockpit - the bottom display, the Station window and the guide, all drawn here.
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

#ifndef WOLF_STARFOLDDECK_H
#define WOLF_STARFOLDDECK_H

#include <string>
#include <vector>

#include "lluictrl.h"
#include "lluuid.h"
#include "wolfhudscale.h"

// Source: region-terrain-work/space-game-2026-11-24/design.html "The cockpit": "A full-width bottom display in the flight-deck
// family (wolfflightdeck.cpp), custom-drawn, scaling with the user's UI size, with a background the user can choose ... Only
// one bottom panel shows at a time. Left: shield, armour and hull as three arcs, always with words and numbers as well as
// colour. Centre: speed, throttle, the Steady or Drift mode and four big buttons. Right: a flat round radar; contacts above or
// below carry an up or down arrow and the word. Every target is at least 44 px at normal UI size. The look is ours: amber and
// cyan instruments on deep blue, B612 lettering as on the flight deck ... Guide cards with pictures, Back and Next explain
// everything on the first flight. Nothing on the screen is timed against the player."
// And "Stations": "Docked, the Station window opens: Market, Repairs, Fitting, Hangar and Launch" - stage 1 has the Market,
// the repair (done by the region on docking: SpaceEngine.SendDocked) and Launch; Fitting and Hangar come with stage 2's ships.
//
// Who and what (ui-ux-design): a resident of 35 to 65, perhaps with unsteady hands, wants to fly to the belt, mine, sell and
// fight off a raider without fast reactions. The flow: LAUNCH -> (the climb) -> BELT (Fly there) -> MINE -> STATION -> DOCK ->
// SELL ALL ORE -> LAUNCH. Every one of those is one big button; the keys are a second way, never the only way.
//
// [GLASS ARC 2026-11-24] Paul chose design "A - Glass Arc" (~/.cache/shadow-scratch/starfold-client/mockups/
// starfold_hud_A_glass_arc.png): while flying, the full-width band is now one slim curved translucent console along the bottom
// (the same arcs, words, numbers and big targets), a holographic elliptical radar with contacts on stalks, and over the view a
// reticle, a pitch ladder, a heading tape and an amber box round the target. The ground panel and the windows stay solid.
//
// One control the size of the main view (main_view.xml "wolf_starfold_deck"), after wolftraincab.cpp: it draws nothing and
// takes no clicks unless WolfStarfold says the cockpit, the ground panel, the Station window or the guide is up.
class WolfStarfoldDeck : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfStarfoldDeck(const Params& p);
    ~WolfStarfoldDeck() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    enum EHit
    {
        H_NONE = 0,
        H_FIRE, H_MINE, H_NEXT, H_DOCK, H_MODE, H_THROTTLE,
        H_FLY_STATION, H_FLY_BELT, H_FLY_WORMHOLE, H_FLY_STOP, H_FLY_PICKED,
        H_HELP, H_THEME, H_HIDE, H_LEAVE, H_SHOW_TAB,
        H_LAUNCH, H_CLOSE, H_GOTO_PAD, H_GOTO_STATION,
        H_SELL, H_STATION_LAUNCH, H_STATION_CLOSE,
        H_GUIDE_BACK, H_GUIDE_NEXT, H_GUIDE_CLOSE,
        H_RADAR_STATION, H_RADAR_BELT, H_RADAR_WORMHOLE,
        H_ALIEN_0, H_ALIEN_LAST = H_ALIEN_0 + 5,
        H_SETUP_STATION, H_SETUP_BELT, H_SETUP_WORMHOLE, H_SETUP_PAD, H_SETUP_CLEAR, H_SETUP_CLOSE,
        H_WORLD,        // the sky above the panel while piloting: point and fly
    };

private:
    struct Hit { F32 mL, mB, mR, mT; EHit mId; std::string mTip; };
    std::vector<Hit> mHits;
    F32 mPanelB = 0.f, mPanelT = 0.f;     // the band, view-local, this frame
    bool mBandDrawn = false;
    S32 mPressed = H_NONE;
    std::string mPicked;                  // a place picked on the radar: "station", "belt", "wormhole"
    WolfHudResize mResize{ "WolfStarfoldDeckScale" };
    F32 mWorldCx = 0.f, mWorldCy = 0.f, mWorldHalfW = 1.f, mWorldHalfH = 1.f;   // the view above the band, for point and fly

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    void press(EHit id, S32 x, S32 y);
    void dragThrottle(S32 x);
    void steer(S32 x, S32 y);
    F32 mThrB = 0.f, mThrT = 1.f;         // the throttle bar's travel, view-local

    void drawCockpit(F32 bottom, F32 width, F32 u);
    void drawGround(F32 bottom, F32 width, F32 u);
    void drawStation(F32 bottom, F32 width, F32 height, F32 u);
    void drawGuide(F32 bottom, F32 width, F32 height, F32 u);
    void drawSetup(F32 bottom, F32 width, F32 height, F32 u);
    bool mClearArmed = false;            // CLEAR pressed once: the second press removes the helper's parts
    void drawNotes(F32 above, F32 width, F32 u);
    void drawRadar(F32 cx, F32 cy, F32 rx, F32 ry, F32 u);
    // the Glass Arc (Paul's design "A", 24 Nov): the console's curved top edge, the view overlay, the capsules, the hint line
    F32 arcTop(F32 x) const;
    bool onPanel(S32 x, S32 y) const;    // on the console (under its curve) or the ground band: not the world behind
    void drawHud(F32 u);
    void capsule(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& label, bool lit, const std::string& tip, F32 u, bool dim = false);
    void showHint(const std::string& text);
    F32 mArcW = 0.f, mArcU = 1.f;
    bool mArcOn = false;
    std::string mHint;                   // a word for a press that could not act, shown for a few seconds
    F64 mHintUntil = 0.0;
    LLUUID mClosingFor;                  // the target the closing speed is for
    F32 mClosing = 0.f, mLastRange = 0.f;
    F64 mLastRangeAt = 0.0;
    void button(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& label, bool lit, const std::string& tip, F32 u,
                bool primary = false);
};

#endif // WOLF_STARFOLDDECK_H
