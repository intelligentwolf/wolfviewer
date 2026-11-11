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

class LLButton;
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
    enum ETile { T_GAME = 0, T_CAR, T_SKIS, T_PLANE, T_SPACESHIP, T_BOAT, T_SHIP, T_SPEEDBOAT, T_SUBMARINE,
                 T_GREEDY, T_ROLEPLAY, T_COMPASS, T_SPEED, T_COUNT };

    /** Lit as its old icon was (the HUD shows). */
    static bool tileOn(ETile t);

private:
    void onTile(ETile t);
    void refreshTiles();

    LLButton*  mTiles[T_COUNT] = { nullptr };
    LLTextBox* mStates[T_COUNT] = { nullptr };
    LLTextBox* mStatus = nullptr;
    std::string mNote;   // [W104 FEATFIX 2026-11-11 A7] the status line's words after a press that did not switch a HUD on
};

#endif // WOLF_FLOATERHUDS_H
