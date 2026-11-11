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
#include "wolffishingoverlay.h"   // [FISHING HUD 2026-11-11/]
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
        "fishing"   // [FISHING HUD 2026-11-11/]
    };

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
        // [FISHING HUD 2026-11-11] on while the Fishing HUD shows (wolffishingoverlay.cpp)
        case T_FISHING:   return WolfFishingSession::instanceExists() && WolfFishingSession::instance().hudWanted();   // (on, or waiting for a vehicle panel)
        default:          return false;
    }
}

// Source: lltoolbarview.cpp postBuild before 2026-11-10 - each icon's commit callback, unchanged.
void WolfFloaterHuds::onTile(ETile t)
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        return;   // the tiles are disabled off Wolf (refreshTiles); a press that slips through does nothing, as the icons
    }
    // Switching a HUD ON closes this window so the HUD can be seen (it sits over the bottom of the screen); switching
    // one OFF leaves it open for the next choice.
    const bool was_on = tileOn(t);
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
        // [FISHING HUD 2026-11-11] the same as Wolf World > Fishing (on) and the HUD's CLOSE (off: the rod's touch menu)
        case T_FISHING:   WolfFishingSession::toggleHud(); break;   // on; off; or, while waiting, cancel the wait
        default:          return;
    }
    if (!was_on)
    {
        closeFloater();
    }
}

void WolfFloaterHuds::refreshTiles()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        if (!mTiles[i] || !mStates[i]) continue;
        const ETile t = static_cast<ETile>(i);
        const bool on = wolf && tileOn(t);
        mTiles[i]->setEnabled(wolf);
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
        const std::string msg = wolf ? std::string() : std::string("Wolf HUDs work on Wolf Territories regions only.");
        if (mStatus->getText() != msg) mStatus->setText(msg);
    }
}
