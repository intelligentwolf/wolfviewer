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
#include "llscrollcontainer.h"   // [HUDS TILES 2 2026-11-14/] tiles_scroll
#include "lltextbox.h"
#include "lluicolortable.h"
#include "wolfdrive.h"
#include "wolffishingoverlay.h"   // [FISHING HUD 2026-11-11/]
#include "wolfflight.h"
#include "wolfgame.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfskis.h"
#include "wolftraincab.h"   // [HUDS TRAIN 2026-11-13/] WolfTrain: the Train Cab
#include "wolffloatergreedy.h"   // [W104 FEATFIX 2026-11-11/] WolfGreedyGame: the Greedy HUD
#include "wolfweatherprops.h"   // [WEATHER PROPS 2026-11-13/] the switch, for the tile's state line

namespace
{
    // Source: floater_wolf_huds.xml tile_<id> / state_<id>; the order is ETile's.
    const char* const TILE_IDS[WolfFloaterHuds::T_COUNT] =
    {
        "game", "car", "skis", "plane", "spaceship", "boat", "ship", "speedboat", "submarine",
        "greedy", "roleplay", "compass", "speed",   // [W104 FEATFIX 2026-11-11]
        "fishing",   // [FISHING HUD 2026-11-11/]
        "train",   // [HUDS TRAIN 2026-11-13/] floater_wolf_huds.xml tile_train / state_train
        "weatherprops", "tutorials",   // [WEATHER PROPS / TUTORIALS 2026-11-13/] floater_wolf_huds.xml row 6
        "trainbuilder", "track"   // [HUDS TILES 2 2026-11-14/] floater_wolf_huds.xml tile_trainbuilder (row 6 right) / tile_track (row 7)
    };

    // [W104 FEATFIX 2026-11-11 A7] each tile's name as floater_wolf_huds.xml name_<id> shows it, for the status line
    const char* const TILE_NAMES[WolfFloaterHuds::T_COUNT] =
    {
        "Game Mode", "The Car Dashboard", "Winter Fun", "The Flight Deck", "The Spaceship HUD", "The Sailing Deck",
        "The Captain's Helm", "The Speedboat helm", "The Submarine helm",
        "Greedy", "The Roleplay console", "Tessa's Compass", "Xenon's WolfSpeed",
        "Fishing",   // [W104 MERGE 2026-11-11] floater_wolf_huds.xml name_fishing
        "The Train Cab",   // [HUDS TRAIN 2026-11-13/] floater_wolf_huds.xml name_train
        "Weather Props", "Tutorial Videos",   // [WEATHER PROPS / TUTORIALS 2026-11-13/] floater_wolf_huds.xml name_weatherprops / name_tutorials
        "The Train Builder", "Lay Railway Track"   // [HUDS TILES 2 2026-11-14/] floater_wolf_huds.xml name_trainbuilder / name_track
    };

    // [W104 FEATFIX 2026-11-11] Wolf Territories only, as their menu items (menu_viewer.xml on_visible WolfGrid.IsOnWolfTerritories);
    // Tessa's Compass works on every grid (its menu item has no gate, floater_wolf_compass.xml "Every grid")
    const bool TILE_WOLF_ONLY[WolfFloaterHuds::T_COUNT] =
    {
        true, true, true, true, true, true, true, true, true,
        true, true, false, true,
        true,   // [W104 MERGE 2026-11-11] Fishing: Wolf Territories only, as its menu item (menu_viewer.xml WolfWorldFishing on_visible WolfGrid.IsOnWolfTerritories)
        true,   // [HUDS TRAIN 2026-11-13] the Train Cab: Wolf Territories only, as its menu item (menu_viewer.xml WolfTrainCab on_visible WolfGrid.IsOnWolfTerritories) and WolfTrain::onWolf (wolftraincab.cpp)
        true, true,   // [WEATHER PROPS / TUTORIALS 2026-11-13] Wolf Territories only, as their menu items (menu_viewer.xml WolfWeatherProps / WolfTutorials on_visible WolfGrid.IsOnWolfTerritories)
        true, true   // [HUDS TILES 2 2026-11-14] Wolf Territories only, as their menu items (menu_viewer.xml WolfWorldTrainBuilder / WolfVehiclesTrainBuilder / WolfWorldTrack on_visible WolfGrid.IsOnWolfTerritories)
    };

    // [W104 FEATFIX 2026-11-11] the windows these tiles show and hide (menu_viewer.xml WolfRoleplay / WolfCompass / WolfSpeed:
    // Floater.Toggle of these names); Greedy is a HUD (WolfGreedyGame)
    const char* floaterOf(WolfFloaterHuds::ETile t)
    {
        switch (t)
        {
            case WolfFloaterHuds::T_ROLEPLAY: return "wolf_roleplay";
            case WolfFloaterHuds::T_COMPASS:  return "wolf_compass";
            case WolfFloaterHuds::T_SPEED:    return "wolf_speed";
            case WolfFloaterHuds::T_WEATHERPROPS: return "wolf_weather_props";   // [WEATHER PROPS 2026-11-13/] menu_viewer.xml WolfWeatherProps
            case WolfFloaterHuds::T_TUTORIALS:    return "wolf_tutorials";       // [TUTORIALS 2026-11-13/] menu_viewer.xml WolfTutorials
            // [HUDS TILES 2 2026-11-14/] menu_viewer.xml WolfWorldTrainBuilder / WolfWorldTrack Floater.Show parameters
            // (llviewerfloaterreg.cpp "wolf_train_builder" WolfFloaterTrainBuilder, "wolf_track" WolfFloaterTrack)
            case WolfFloaterHuds::T_TRAINBUILDER: return "wolf_train_builder";
            case WolfFloaterHuds::T_TRACK:        return "wolf_track";
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
    // [HUDS TILES 2 2026-11-14] the tiles scroll (floater_wolf_huds.xml tiles_scroll / tiles_panel). Tabbing to a tile below
    // the fold brings it into view, as Wolf Experiences' cards (wolffloaterexperiences.cpp): a tile's rect is in tiles_panel's
    // coordinates, which are the scroll's document coordinates (llscrollcontainer.cpp scrollToShowRect).
    mScroll = getChild<LLScrollContainer>("tiles_scroll");
    mFullHeight = getRect().getHeight();
    LLHandle<LLFloater> fh = getHandle();
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        LLButton* tile = mTiles[i];
        tile->setFocusReceivedCallback([fh, tile](LLFocusableElement*)
        {
            if (WolfFloaterHuds* self = static_cast<WolfFloaterHuds*>(fh.get()))
            {
                if (self->mScroll) self->mScroll->scrollToShowRect(tile->getRect());
            }
        });
    }
    refreshTiles();
    return true;
}

void WolfFloaterHuds::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mNote.clear();   // [A7/]
    refreshTiles();
    // [HUDS TILES 2 2026-11-14] from the top every time (as WolfStorm's window), fitted to the floaters' area as it is now
    if (mScroll) mScroll->goToTop();
    mFitRoom = -1;
    fitToArea();
    // Keyboard: Tab moves between the tiles, Enter or Space presses one (LLButton::handleKeyHere). No tile is given focus
    // on open: in the WolfViewer skins a focused button is drawn green, which would read as "this one is on".
}

void WolfFloaterHuds::draw()
{
    // The HUDs change from everywhere (their own close buttons, the Wolf World menu, a seat, leaving Wolf): read them
    // every frame while this shows, as lltoolbarview.cpp draw() lit the old icons.
    refreshTiles();
    // [HUDS TILES 2 2026-11-14] the floaters' area changed while this shows (the viewer window resized, the UI size changed,
    // a toolbar shown or hidden): fit again
    if (gFloaterView && gFloaterView->getSnapRect().getHeight() != mFitRoom)
    {
        fitToArea();
    }
    LLFloater::draw();
}

// [HUDS TILES 2 2026-11-14] With seven rows the window (floater_wolf_huds.xml, 920 high in UI units) is taller than the
// floaters' area on many screens at a larger UI size. LLFloaterView::adjustToFitScreen (llfloater.cpp:3242-3307) only
// shrinks resizable floaters and this one is not, so it is done here: never taller than the area (getSnapRect, the space
// between the toolbars, in the same UI units as this window's rect, so it follows the UI size), never shorter than the
// intro, one row of tiles with its gap and the status line (64 + 116 + 52, floater_wolf_huds.xml; the row's 8-unit gap
// covers the title bar's few units of header stretch, llfloater.cpp), and never taller than every tile
// needs. The tiles scroll inside (tiles_scroll follows all; the status line follows the bottom). Then the window is brought
// wholly inside the area (adjustToFitScreen, allow_partial_outside false, snap_in_toolbars true, as wolffloaterbuilder.cpp).
void WolfFloaterHuds::fitToArea()
{
    if (!gFloaterView || getParent() != gFloaterView || isMinimized() || mFullHeight <= 0)
    {
        return;
    }
    const S32 room = gFloaterView->getSnapRect().getHeight();
    mFitRoom = room;
    constexpr S32 MIN_HEIGHT = 64 + 116 + 52;
    const S32 want = llmax(MIN_HEIGHT, llmin(room, mFullHeight));
    const LLRect r = getRect();
    if (want != r.getHeight())
    {
        LLRect fitted;
        fitted.setLeftTopAndSize(r.mLeft, r.mTop, r.getWidth(), want);   // the top stays where it is
        setShape(fitted);
    }
    gFloaterView->adjustToFitScreen(this, false, true);
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
        // [W104 FEATFIX 2026-11-11] Greedy is a HUD now (wolfgreedyhud.h), as its menu tick (WolfGreedy.HudOn)
        case T_GREEDY:    return WolfGreedyGame::instanceExists() && WolfGreedyGame::instance().hudWanted();   // (on, or waiting for a vehicle panel)
        case T_ROLEPLAY:
        case T_COMPASS:
        case T_SPEED:     return LLFloaterReg::instanceVisible(floaterOf(t));   // as their menu ticks (Floater.Visible)
        // [WEATHER PROPS / TUTORIALS 2026-11-13] lit while their window shows, as the three above
        case T_WEATHERPROPS:
        case T_TUTORIALS:
        case T_TRAINBUILDER:   // [HUDS TILES 2 2026-11-14/] lit while its window shows, as the two above
        case T_TRACK:     return LLFloaterReg::instanceVisible(floaterOf(t));
        // [FISHING HUD 2026-11-11] on while the Fishing HUD shows (wolffishingoverlay.cpp)
        case T_FISHING:   return WolfFishingSession::instanceExists() && WolfFishingSession::instance().hudWanted();   // (on, or waiting for a vehicle panel)
        // [HUDS TRAIN 2026-11-13] on while the Train Cab shows, as its menu tick (llviewermenu.cpp WolfTrain.IsShown ->
        // wolftraincab.cpp WolfTrain::shown: driving and not hidden, or opened by hand to read the guide)
        case T_TRAIN:     return WolfTrain::instance().shown();
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
        case T_GREEDY:    WolfGreedyGame::toggleHud(); break;   // WolfGreedy.ToggleHud, as the menu
        case T_ROLEPLAY:
        case T_COMPASS:
        case T_SPEED:     LLFloaterReg::toggleInstance(floaterOf(t)); break;   // Floater.Toggle, as the menu
        // [WEATHER PROPS / TUTORIALS 2026-11-13] their windows, as Wolf World > Sky & Weather > Weather Props... / Tutorial Videos...
        case T_WEATHERPROPS:
        case T_TUTORIALS:
        // [HUDS TILES 2 2026-11-14] their windows: on opens it, as Floater.Show of their menu items (menu_viewer.xml
        // WolfWorldTrainBuilder / WolfWorldTrack); off (the tile lit, the window showing) hides it, as the tiles above
        case T_TRAINBUILDER:
        case T_TRACK:     LLFloaterReg::toggleInstance(floaterOf(t)); break;
        // [FISHING HUD 2026-11-11] the same as Wolf World > Fishing (on) and the HUD's CLOSE (off: the rod's touch menu)
        case T_FISHING:   WolfFishingSession::toggleHud(); break;   // on; off; or, while waiting, cancel the wait
        // [HUDS TRAIN 2026-11-13] the same as Wolf World > Vehicles & Travel > Train Cab (llviewermenu.cpp WolfTrain.Toggle ->
        // wolftraincab.cpp WolfTrain::toggleShown). Not in an engine's cab the Train Cab still opens, and says in its own
        // panel "Sit in the cab of a Wolf Rail engine to drive it." with the guide behind its ? (WolfTrainCabView::draw).
        case T_TRAIN:     WolfTrain::instance().toggleShown(); break;
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
        else if (wolf && t == T_TRAIN)
        {
            // [HUDS TRAIN 2026-11-13] still the driver with the cab not showing (wolftraincab.cpp WolfTrain::toggleShown /
            // idle: hidden with its X or F4, or given way to another bottom panel): say so rather than a bare OFF
            const WolfTrain& tr = WolfTrain::instance();
            if (tr.driving() && !on) { state = "Driving - cab hidden"; running = true; }
        }
        else if (wolf && t == T_WEATHERPROPS)
        {
            // [WEATHER PROPS 2026-11-13] the props carry on with the window closed: say so rather than a bare OFF
            if (WolfWeatherProps::enabled() && !on) { state = "Props on - window closed"; running = true; }
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
