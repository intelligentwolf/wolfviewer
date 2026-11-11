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

    enum ETile { T_GAME = 0, T_CAR, T_SKIS, T_PLANE, T_SPACESHIP, T_BOAT, T_SHIP, T_SPEEDBOAT, T_SUBMARINE,
                 T_FISHING,   // [FISHING HUD 2026-11-11] Paul: the HUDs window lists every on-screen HUD
                 T_COUNT };

    /** Lit as its old icon was (the HUD shows). */
    static bool tileOn(ETile t);

private:
    void onTile(ETile t);
    void refreshTiles();

    LLButton*  mTiles[T_COUNT] = { nullptr };
    LLTextBox* mStates[T_COUNT] = { nullptr };
    LLTextBox* mStatus = nullptr;
};

#endif // WOLF_FLOATERHUDS_H
