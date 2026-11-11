/**
 * @file wolffloaterhuds.cpp
 * @brief WolfViewer: Wolf HUDs - one big illustrated tile per Wolf HUD, opened by the HUDs button.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "wolffloaterhuds.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "wolfgame.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfskis.h"

namespace
{
    // Source: floater_wolf_huds.xml tile_<id> / state_<id>; the order is ETile's.
    const char* const TILE_IDS[WolfFloaterHuds::T_COUNT] =
    {
        "game", "car", "skis", "plane", "spaceship", "boat", "ship", "speedboat", "submarine",
        "greedy", "roleplay", "compass", "speed"   // [W104 FEATFIX 2026-11-11]
    };

    // [W104 FEATFIX 2026-11-11 A7] each tile's name as floater_wolf_huds.xml name_<id> shows it, for the status line
    const char* const TILE_NAMES[WolfFloaterHuds::T_COUNT] =
    {
        "Game Mode", "The Car Dashboard", "Winter Fun", "The Flight Deck", "The Spaceship HUD", "The Sailing Deck",
        "The Captain's Helm", "The Speedboat helm", "The Submarine helm",
        "Greedy", "The Roleplay console", "Tessa's Compass", "Xenon's WolfSpeed"
    };

    // [W104 FEATFIX 2026-11-11] Wolf Territories only, as their menu items (menu_viewer.xml on_visible WolfGrid.IsOnWolfTerritories);
    // Tessa's Compass works on every grid (its menu item has no gate, floater_wolf_compass.xml "Every grid")
    const bool TILE_WOLF_ONLY[WolfFloaterHuds::T_COUNT] =
    {
        true, true, true, true, true, true, true, true, true,
        true, true, false, true
    };

    // [W104 FEATFIX 2026-11-11] the windows these tiles show and hide (menu_viewer.xml WolfGreedy / WolfRoleplay / WolfCompass /
    // WolfSpeed: Floater.Show / Floater.Toggle of these names)
    const char* floaterOf(WolfFloaterHuds::ETile t)
    {
        switch (t)
        {
            case WolfFloaterHuds::T_GREEDY:   return "wolf_greedy";
            case WolfFloaterHuds::T_ROLEPLAY: return "wolf_roleplay";
            case WolfFloaterHuds::T_COMPASS:  return "wolf_compass";
            case WolfFloaterHuds::T_SPEED:    return "wolf_speed";
            default:                          return nullptr;
        }
    }

    // Source: lltoolbarview.cpp postBuild before 2026-11-10 (the Game icon's `run` lambda): the same command as
    // World > Game Mode, so its refusals and the forced-region lock behave the same wherever it is pressed.
    void runCommand(const char* fn)
    {
        if (LLUICtrl::CommitCallbackRegistry::ptr_value_t cb = LLUICtrl::CommitCallbackRegistry::getValue(fn))
        {
            (*cb)(nullptr, LLSD());
        }
    }
}

WolfFloaterHuds::WolfFloaterHuds(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterHuds::postBuild()
{
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        const ETile t = static_cast<ETile>(i);
        mTiles[i] = getChild<LLButton>(std::string("tile_") + TILE_IDS[i]);
        mTiles[i]->setCommitCallback([this, t](LLUICtrl*, const LLSD&) { onTile(t); });
        mStates[i] = getChild<LLTextBox>(std::string("state_") + TILE_IDS[i]);
    }
    mStatus = getChild<LLTextBox>("status");
    refreshTiles();
    return true;
}

void WolfFloaterHuds::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mNote.clear();   // [A7/]
    refreshTiles();
    // Keyboard: Tab moves between the tiles, Enter or Space presses one (LLButton::handleKeyHere). No tile is given focus
    // on open: in the WolfViewer skins a focused button is drawn green, which would read as "this one is on".
}

void WolfFloaterHuds::draw()
{
    // The HUDs change from everywhere (their own close buttons, the Wolf World menu, a seat, leaving Wolf): read them
    // every frame while this shows, as lltoolbarview.cpp draw() lit the old icons.
    refreshTiles();
    LLFloater::draw();
}

// Source: lltoolbarview.cpp draw() before 2026-11-10 - each icon's setToggleState test, unchanged.
bool WolfFloaterHuds::tileOn(ETile t)
{
    switch (t)
    {
        case T_GAME:
        {
            const WolfGame& g = WolfGame::instance();
            return g.gameModeShownOn() && g.hudShowing();
        }
        case T_CAR:       return WolfDrive::instance().showingDesign(WolfDrive::TYPE_CAR);
        case T_SKIS:      return WolfSkis::instance().riding();
        case T_PLANE:
        {
            const WolfFlight& wf = WolfFlight::instance();
            return wf.active() && !wf.deckHidden() && !wf.sailing();
        }
        case T_SPACESHIP: return WolfDrive::instance().showingDesign(WolfDrive::TYPE_SPACESHIP);
        case T_BOAT:
        {
            const WolfFlight& wf = WolfFlight::instance();
            return wf.active() && !wf.deckHidden() && wf.sailing();
        }
        case T_SHIP:      return WolfHelm::instance().hudShowing();
        case T_SPEEDBOAT: return WolfDrive::instance().showingDesign(WolfDrive::TYPE_SPEEDBOAT);
        case T_SUBMARINE: return WolfDrive::instance().showingDesign(WolfDrive::TYPE_SUBMARINE);
        case T_GREEDY:
        case T_ROLEPLAY:
        case T_COMPASS:
        case T_SPEED:     return LLFloaterReg::instanceVisible(floaterOf(t));   // as their menu ticks (Floater.Visible)
        default:          return false;
    }
}

// Source: lltoolbarview.cpp postBuild before 2026-11-10 - each icon's commit callback, unchanged.
void WolfFloaterHuds::onTile(ETile t)
{
    if (!WolfGrid::isOnWolfTerritories() && TILE_WOLF_ONLY[t])
    {
        return;   // the tiles are disabled off Wolf (refreshTiles); a press that slips through does nothing, as the icons
    }
    // Switching a HUD ON closes this window so the HUD can be seen (it sits over the bottom of the screen); switching
    // one OFF leaves it open for the next choice.
    mNote.clear();
    const bool was_on = tileOn(t);
    const bool skis_window_was_open = LLFloaterReg::instanceVisible("wolf_skis");
    switch (t)
    {
        case T_GAME:
        {
            // [GAME HUD 2026-10-09] with Game Mode off the tile switches it on (World > Game Mode, so its refusals say
            // why); with it on, it shows or hides the game HUD and the game keeps running while it is hidden.
            WolfGame& g = WolfGame::instance();
            if (g.gameModeShownOn()) g.toggleHud();
            else runCommand("WolfGame.ToggleGameMode");
            break;
        }
        case T_CAR:       WolfDrive::instance().toggleDesign(WolfDrive::TYPE_CAR); break;
        case T_SKIS:      LLFloaterReg::toggleInstance("wolf_skis"); break;
        case T_PLANE:     WolfFlight::instance().toggleDeck(false); break;
        case T_SPACESHIP: WolfDrive::instance().toggleDesign(WolfDrive::TYPE_SPACESHIP); break;
        case T_BOAT:      WolfFlight::instance().toggleDeck(true); break;
        case T_SHIP:      WolfHelm::instance().toggleHud(); break;
        case T_SPEEDBOAT: WolfDrive::instance().toggleDesign(WolfDrive::TYPE_SPEEDBOAT); break;
        case T_SUBMARINE: WolfDrive::instance().toggleDesign(WolfDrive::TYPE_SUBMARINE); break;
        case T_GREEDY:
        case T_ROLEPLAY:
        case T_COMPASS:
        case T_SPEED:     LLFloaterReg::toggleInstance(floaterOf(t)); break;   // Floater.Toggle, as the menu
        default:          return;
    }
    if (was_on)
    {
        return;
    }
    // [W104 FEATFIX 2026-11-11 A7] Did it come on? Only then is this window closed. A HUD can refuse (a gate, Game Mode
    // locked by the region, a window that closes itself): the window then stays open with the tile still OFF and says so,
    // instead of the press seeming lost. Started but not on yet counts as on: Game Mode connecting, the Winter Fun window
    // (its tile is lit by riding, the window is where they choose; pressing it with that window open closed it).
    bool came_on = tileOn(t);
    if (t == T_GAME)
    {
        const WolfGame& g = WolfGame::instance();
        came_on = came_on || g.gameModeShownOn() || g.gameModeConnecting();
    }
    else if (t == T_SKIS)
    {
        came_on = came_on || skis_window_was_open || LLFloaterReg::instanceVisible("wolf_skis");
    }
    if (came_on)
    {
        closeFloater();
        return;
    }
    mNote = std::string(TILE_NAMES[t]) + " did not come on. If a message popped up, it says why; otherwise try again in a moment.";
    refreshTiles();
}

void WolfFloaterHuds::refreshTiles()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        if (!mTiles[i] || !mStates[i]) continue;
        const ETile t = static_cast<ETile>(i);
        const bool usable = wolf || !TILE_WOLF_ONLY[i];   // [W104 FEATFIX 2026-11-11/] Tessa's Compass works on every grid
        const bool on = usable && tileOn(t);
        mTiles[i]->setEnabled(usable);
        mTiles[i]->setToggleState(on);
        // The word as well as the light (never colour alone): ON / OFF, and what the old tooltips said beside it.
        std::string state = on ? "ON" : "OFF";
        bool running = on;   // the filled dot and the green word: the HUD shows, or its mode runs with it hidden
        if (wolf && t == T_GAME)
        {
            // Source: lltoolbarview.cpp draw() before 2026-11-10, the Game icon's tooltip states
            const WolfGame& g = WolfGame::instance();
            const bool mode_on = g.gameModeShownOn();
            if (g.gameModeConnecting() && !g.forced()) state = "Connecting...";
            else if (mode_on && !on) { state = "ON - HUD hidden"; running = true; }
            if (mode_on && g.forced()) state += " (locked on here)";
        }
        else if (wolf && (t == T_PLANE || t == T_BOAT))
        {
            // the mode runs on with its deck hidden (WolfFlight::toggleDeck): say so rather than a bare OFF
            const WolfFlight& wf = WolfFlight::instance();
            const bool mine = wf.active() && (t == T_BOAT) == wf.sailing();
            if (mine && wf.deckHidden()) { state = (t == T_BOAT) ? "Sailing - deck hidden" : "Flying - deck hidden"; running = true; }
        }
        // In the WolfViewer skins a lit (selected) button is only a lighter grey, so the state line says it plainly: a filled
        // dot and light green words when on, a hollow dot and the label colour when off. LtGreen (colors.xml, 0.79 1 0.79)
        // keeps well above 4.5:1 on the lit tile's grey.
        state = std::string(running ? "\xE2\x97\x8F " : "\xE2\x97\x8B ") + state;   // U+25CF / U+25CB
        if (mStates[i]->getText() != state)
        {
            mStates[i]->setText(state);
            mStates[i]->setColor(LLUIColorTable::instance().getColor(running ? "LtGreen" : "LabelTextColor"));
        }
    }
    if (mStatus)
    {
        // [W104 FEATFIX 2026-11-11 A7] after a press that did not switch a HUD on: why the window stayed open
        const std::string msg = wolf ? mNote : std::string("Wolf HUDs work on Wolf Territories regions only (Tessa's Compass works everywhere).");
        if (mStatus->getText() != msg)
        {
            mStatus->setText(msg);
            mStatus->setColor(LLUIColorTable::instance().getColor(msg.empty() ? "LabelTextColor" : "AlertCautionTextColor"));
        }
    }
}
