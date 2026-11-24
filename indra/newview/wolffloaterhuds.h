/**
 * @file wolffloaterhuds.h
 * @brief WolfViewer: Wolf HUDs - one big illustrated tile per Wolf HUD, opened by the HUDs button.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#ifndef WOLF_FLOATERHUDS_H
#define WOLF_FLOATERHUDS_H

#include "llfloater.h"

#include <vector>

class LLButton;
class LLIconCtrl;
class LLScrollContainer;
class LLTextBox;

// [HUDS BUTTON 2026-11-10] Paul: "on the bottom left of the screen all those tiny buttons look messy - have a Huds button
// in the bottom right control that opens a colourful floater with each different hud as an illustrated button".
// Source: the nine show / hide icons this replaces, lltoolbarview.cpp postBuild / draw before this change (wolf_skis_toggle,
// wolf_game_toggle, wolf_car_toggle, wolf_plane_toggle, wolf_spaceship_toggle, wolf_boat_toggle, wolf_ship_toggle,
// wolf_speedboat_toggle, wolf_submarine_toggle): each tile runs the SAME command its icon ran and is lit by the same
// test, so nothing about what a HUD does changes - only where it is pressed. Same tiles, order and words as WolfStorm
// js/ui/floaters/floater_wolf_huds.js. Layout: floater_wolf_huds.xml.
class WolfFloaterHuds : public LLFloater
{
public:
    WolfFloaterHuds(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // [W104 FEATFIX 2026-11-11] Paul: "greedy the hud isn't on the hud menu, I think there are a lot of huds not on the floater
    // that should be": + Greedy, the Roleplay console, Tessa's Compass and Xenon's WolfSpeed (each a window shown and hidden
    // from the Wolf World menu). A new tile goes before T_COUNT, with its TILE_IDS / TILE_NAMES / TILE_WOLF_ONLY entries,
    // tileOn and onTile cases and its XML block (floater_wolf_huds.xml: the next free slot of the 3-wide grid).
    // [HUDS TILES 2 2026-11-14] the tiles are in tiles_panel inside tiles_scroll: a new row grows tiles_panel, the scroll, the
    // window by 116 and moves the status line 116 down; fitToArea keeps the window inside the floaters' area.
    enum ETile { T_GAME = 0, T_CAR, T_SKIS, T_PLANE, T_SPACESHIP, T_BOAT, T_SHIP, T_SPEEDBOAT, T_SUBMARINE,
                 T_GREEDY, T_ROLEPLAY, T_COMPASS, T_SPEED,
                 T_FISHING,   // [FISHING HUD 2026-11-11] Paul: the HUDs window lists every on-screen HUD ([W104 MERGE 2026-11-11] after featfix's tiles)
                 T_TRAIN,     // [HUDS TRAIN 2026-11-13] Paul (W106 test build): "the train hud is not on the huds menu?" - the Train Cab (wolftraincab.h)
                 T_WEATHERPROPS, T_TUTORIALS,   // [WEATHER PROPS / TUTORIALS 2026-11-13] ticket 847: their windows (row 6)
                 T_TRAINBUILDER, T_TRACK,       // [HUDS TILES 2 2026-11-14] the Train Builder and Lay Railway Track windows (row 6 right, row 7)
                 T_STARFOLD,                    // [STARFOLD 2026-11-24] Paul: "not on the hud menu it should be on there" (row 7 middle)
                 T_COUNT };

    /** Lit as its old icon was (the HUD shows). */
    static bool tileOn(ETile t);

private:
    void onTile(ETile t);
    void refreshTiles();
    // [HUDS TILES 2 2026-11-14] no taller than the floaters' area; the tiles scroll inside.
    // [HUDS SMALL 2026-11-14] and no wider: fewer columns of tiles when the area is narrower than the XML's three.
    void fitToArea();
    S32  statusHeight(S32 width);   // [HUDS SMALL 2026-11-14] the tallest the status line's words get at this width

    // [T958 2026-11-21] ticket 958 (Luke Koda): the resident picks which tiles show here (settings_per_account.xml
    // WolfHudsHidden, the TILE_IDS of the hidden ones). Choosing: every tile shows and a press shows or hides it; otherwise
    // the hidden ones are left out and the rest close up. A hidden tile's HUD still works (Wolf World menu, its own keys).
    void loadHidden();
    void saveHidden() const;
    void setHidden(ETile t, bool hidden);
    void setChoosing(bool on);
    void showAll();
    S32  hiddenCount() const;

    LLButton*  mTiles[T_COUNT] = { nullptr };
    LLTextBox* mStates[T_COUNT] = { nullptr };
    LLIconCtrl* mPics[T_COUNT] = { nullptr };   // [T958 2026-11-21/] pic_<id>, dimmed while choosing when hidden
    LLTextBox* mStatus = nullptr;
    LLScrollContainer* mScroll = nullptr;   // [HUDS TILES 2 2026-11-14] floater_wolf_huds.xml tiles_scroll

    // [HUDS SMALL 2026-11-14] the window as built from floater_wolf_huds.xml (after LLFloater's header stretch), so that
    // fitToArea can lay the same tiles out in fewer columns. Each tile is its button and four views beside it in
    // tiles_panel (picture, name, words, state line), kept at their XML offsets from the button.
    struct TileParts
    {
        static constexpr int N = 5;   // tile_<id>, pic_<id>, name_<id>, desc_<id>, state_<id>
        LLView* views[N] = { nullptr };
        S32 dx[N] = { 0 };     // left, from the tile's left
        S32 dtop[N] = { 0 };   // down, from the tile's top
        ETile tile = T_GAME;   // [T958 2026-11-21/] which tile these are (mGrid is sorted into reading order)
    };
    std::vector<TileParts> mGrid;   // reading order (rows top to bottom, each left to right), as the XML places them
    bool mGridOk = false;           // the XML gave at least two columns and two rows to measure the steps from
    LLTextBox* mIntro = nullptr;    // floater_wolf_huds.xml intro
    LLView* mPanel = nullptr;       // floater_wolf_huds.xml tiles_panel
    S32 mXmlCols = 0, mColStep = 0, mRowStep = 0, mTileH = 0, mFirstLeft = 0, mFirstTopGap = 0;
    S32 mXmlWidth = 0, mPanelXmlWidth = 0, mIntroXmlWidth = 0, mStatusXmlWidth = 0, mScrollRightPad = 0;
    S32 mTopToIntro = 0, mIntroToScroll = 0, mScrollToStatus = 0, mStatusBottom = 0, mIntroMinH = 0, mStatusMinH = 0;
    S32 mFitW = -1, mFitH = -1;     // the floaters' area fitToArea last fitted to
    std::string mNote;   // [W104 FEATFIX 2026-11-11 A7] the status line's words after a press that did not switch a HUD on
    // [T958 2026-11-21] floater_wolf_huds.xml choose_btn / show_all_btn, under the intro (mIntroToButtons below it, as built)
    LLButton* mChooseBtn = nullptr;
    LLButton* mShowAllBtn = nullptr;
    S32 mIntroToButtons = 0;
    std::string mIntroWords;           // the intro as the XML has it (choosing shows other words)
    bool mChoosing = false;
    bool mHidden[T_COUNT] = { false };
};

#endif // WOLF_FLOATERHUDS_H
