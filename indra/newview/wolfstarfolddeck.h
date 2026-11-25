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
#include "v3math.h"
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
    /** [STARBASES 2026-11-25] For LEAP rigs (WolfStarfold.Do "hits"): logs every press target drawn this frame - its id, its tip
     *  and its middle in window GL pixels (LLWindow listener mouseDown x/y) - so a rig clicks the real buttons and the radar
     *  through the window's own mouse path, never by calling the orders itself. */
    static void logHits();

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;
    // [WOLFTREK v11 2026-11-25] the rename field (custom-drawn, as wolfadventuretalk.cpp's text box): typed letters while it has
    // the keyboard, Enter saves, Escape cancels
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;

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
        // [STARBASES 2026-11-24] the Starbase window (drawBase)
        H_BASE_OPEN, H_BASE_CLOSE, H_BASE_TAB_0, H_BASE_TAB_LAST = H_BASE_TAB_0 + 3,
        H_BASE_FOUND, H_BASE_SPOT, H_BASE_RESTORE, H_BASE_HOME, H_BASE_FLY, H_BASE_DOCKAT,
        H_BASE_DOCK_ALL, H_BASE_DOCK_FRIENDS, H_BASE_DOCK_GROUP, H_BASE_DOCK_ME,
        H_BASE_PACK, H_BASE_SCRAP, H_BASE_PREV, H_BASE_NEXT, H_BASE_TURN, H_BASE_BUILD, H_BASE_CANCEL,
        H_BASE_OWN_CHECK, H_BASE_PAGE_PREV, H_BASE_PAGE_NEXT,
        H_BASE_CARD_0, H_BASE_CARD_LAST = H_BASE_CARD_0 + 7,
        H_BASE_ROWA_0, H_BASE_ROWA_LAST = H_BASE_ROWA_0 + 11,   // a row's first button (Put it back, Place it, Attach)
        H_BASE_ROWB_0, H_BASE_ROWB_LAST = H_BASE_ROWB_0 + 11,   // its second (Take down, Send to my base)
        H_BASE_ROWC_0, H_BASE_ROWC_LAST = H_BASE_ROWC_0 + 11,   // a row's third (Make seat)
        H_BASE_GHOST_0, H_BASE_GHOST_LAST = H_BASE_GHOST_0 + 11, // a ghost in the world: click to choose it
        H_RADAR_BASE_0, H_RADAR_BASE_LAST = H_RADAR_BASE_0 + 23,  // a starbase on the radar (BaseRules.CAP_MAX 24)
        // [WOLFTREK v11 2026-11-25] the automatic approach's STOP, and renaming the station and the starbase
        H_BANNER_STOP, H_STATION_RENAME, H_BASE_RENAME, H_NAME_FIELD, H_NAME_SAVE, H_NAME_CANCEL,
        // [WOLFTREK v11 stage B] the Station window's tabs and its ship cards (SpaceRules.Fleet: 5 classes; room for 8)
        H_STAB_0, H_STAB_LAST = H_STAB_0 + 2,
        H_SHIP_BUY_0, H_SHIP_BUY_LAST = H_SHIP_BUY_0 + 7,
        H_SHIP_USE_0, H_SHIP_USE_LAST = H_SHIP_USE_0 + 7,
        H_SHIP_NO_0, H_SHIP_NO_LAST = H_SHIP_NO_0 + 7,
        H_SHIP_YES_0, H_SHIP_YES_LAST = H_SHIP_YES_0 + 7,
        H_SHIP_PAGE_PREV, H_SHIP_PAGE_NEXT,
        // [WOLFTREK v11 stage C] the Hangar's own designs (ShipBuilds.MAX_DESIGNS 10, OwnShipsList at most 12 builds)
        H_HANGAR_SHIPS, H_HANGAR_OWN, H_OWN_LIST, H_FLY_STANDARD, H_OWN_PAGE_PREV, H_OWN_PAGE_NEXT,
        H_DESIGN_USE_0, H_DESIGN_USE_LAST = H_DESIGN_USE_0 + 9,
        H_DESIGN_FORGET_0, H_DESIGN_FORGET_LAST = H_DESIGN_FORGET_0 + 9,
        H_DESIGN_NO_0, H_DESIGN_NO_LAST = H_DESIGN_NO_0 + 9,
        H_DESIGN_YES_0, H_DESIGN_YES_LAST = H_DESIGN_YES_0 + 9,
        H_BUILD_USE_0, H_BUILD_USE_LAST = H_BUILD_USE_0 + 11,
    };

private:
    struct Hit { F32 mL, mB, mR, mT; EHit mId; std::string mTip; };
    std::vector<Hit> mHits;
    F32 mPanelB = 0.f, mPanelT = 0.f;     // the band, view-local, this frame
    bool mBandDrawn = false;
    S32 mPressed = H_NONE;
    std::string mPicked;                  // a place picked on the radar: "station", "belt", "wormhole", "base:<id>"
    std::string mDockBase;                // [STARBASES] the base last picked on the radar: DOCK docks there when it is nearer
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
    // [STARBASES 2026-11-24] the Starbase window (design.html#bases "Building it: the Starbase window") and its ghosts in the world
    void drawBase(F32 bottom, F32 width, F32 height, F32 u);
    void drawBaseHome(F32 l, F32 b, F32 r, F32 t, F32 u);
    void drawBaseAdd(F32 l, F32 b, F32 r, F32 t, F32 u);
    void drawBaseStore(F32 l, F32 b, F32 r, F32 t, F32 u);
    void drawBaseOwn(F32 l, F32 b, F32 r, F32 t, F32 u);
    void drawBasePicture(F32 cx, F32 cy, F32 size, F32 u);
    void drawGhosts(F32 u);
    void pressBase(EHit id);
    /** Agent point -> view-local x, y over the world view (the camera's basis and field of view, as drawHud); false when it is
     *  behind the eye (x, y then hold its left / up offsets). */
    bool projectAgent(const LLVector3& p, F32& x, F32& y) const;
    std::vector<std::string> mRowIds;    // the ids the rows' buttons act on this frame (a part or an object)
    S32 mBasePage = 0;                   // the list's page (rows that do not fit go on the next)
    std::string mArmed;                  // an order pressed once that needs a second press: "pack", "scrap", "down:<part>"
    std::string mCard;                   // the module card chosen on the Add tab
    std::vector<std::string> mCardIds;   // the kinds the cards show this frame
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

    // [WOLFTREK v11 2026-11-25] the automatic approach's banner over the view (design.html#bay: "the cockpit shows Automatic
    // docking"), and the rename field in the Station window (the station, its owner) and the Starbase window (the base, its owner)
    void drawBanner(F32 u);
    enum EEdit { EDIT_NONE = 0, EDIT_STATION, EDIT_BASE };
    EEdit mEdit = EDIT_NONE;
    std::string mEditText;               // what is typed (UTF-8)
    std::string mEditError;              // why the name cannot be saved (WolfStarfold::checkName), shown under the field
    void startEdit(EEdit what, const std::string& now);
    void saveEdit();
    void cancelEdit();
    /** The field with SAVE and CANCEL across l..r, b..t. */
    void drawEditRow(F32 l, F32 b, F32 r, F32 t, F32 u);

    // [WOLFTREK v11 stage B] the Shipyard and the Hangar (design.html#fleet; as WolfStorm 9fbd3d4 wolf_starfold_hud.js _ships)
    S32 mStationTab = 0;                 // 0 MARKET, 1 SHIPYARD, 2 HANGAR
    std::string mBuyAsk;                 // the class whose "Buy the ... for ...?" is showing
    std::vector<std::string> mShipIds;   // the classes the cards' buttons act on this frame (by slot)
    S32 mShipPage = 0;
    void drawShips(F32 l, F32 b, F32 r, F32 t, F32 u);
    void pressShip(EHit id);
    // [WOLFTREK v11 stage C] the Hangar's YOUR OWN DESIGNS (design.html#ownships; as WolfStorm 55ef93e _ownDesigns)
    bool mHangarOwn = false;             // the Hangar shows your own designs (else the ships you own)
    std::string mForgetAsk;              // the design whose "Forget your design ...?" is showing
    std::vector<std::string> mDesignIds, mBuildIds;   // what the rows' buttons act on this frame (by slot)
    std::string mStandardClass;          // FLY THE STANDARD <class>: the class whose design flies next
    S32 mOwnPage = 0;
    void drawOwnDesigns(F32 l, F32 b, F32 r, F32 t, F32 u);
};

#endif // WOLF_STARFOLDDECK_H
