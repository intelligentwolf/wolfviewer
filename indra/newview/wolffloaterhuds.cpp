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
#include "lliconctrl.h"   // [T958 2026-11-21/] a hidden tile's picture, dimmed while choosing
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"   // [T958 2026-11-21/] gSavedPerAccountSettings WolfHudsHidden
#include "wolfdrive.h"
#include "wolffishingoverlay.h"   // [FISHING HUD 2026-11-11/]
#include "wolfflight.h"
#include "wolfgame.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfskis.h"
#include "wolftraincab.h"   // [HUDS TRAIN 2026-11-13/] WolfTrain: the Train Cab
#include "wolfstarfold.h"   // [STARFOLD 2026-11-24/] WolfStarfold: the cockpit / Launch panel
#include "wolffloatergreedy.h"   // [W104 FEATFIX 2026-11-11/] WolfGreedyGame: the Greedy HUD
#include "wolfweatherprops.h"   // [WEATHER PROPS 2026-11-13/] the switch, for the tile's state line

#include <algorithm>    // [HUDS SMALL 2026-11-14/] std::find, std::sort (the grid in reading order)
#include <functional>   // [HUDS SMALL 2026-11-14/] std::greater

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
        "trainbuilder", "track",   // [HUDS TILES 2 2026-11-14/] floater_wolf_huds.xml tile_trainbuilder (row 6 right) / tile_track (row 7)
        "starfold"   // [STARFOLD 2026-11-24/] floater_wolf_huds.xml tile_starfold (row 7 middle)
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
        "The Train Builder", "Lay Railway Track",   // [HUDS TILES 2 2026-11-14/] floater_wolf_huds.xml name_trainbuilder / name_track
        "WolfTrek"   // [STARFOLD 2026-11-24/] [WOLFTREK 2026-11-25] the game's name is WolfStarfold::gameName(); this window's XML says it too (name_starfold)
    };

    // [HUDS SMALL 2026-11-14] the status line's two kinds of words, here once so fitToArea can measure the tallest
    // (they were written in onTile [W104 FEATFIX A7] and refreshTiles, unchanged)
    const char* const NOT_ON_WOLF_STATUS = "Wolf HUDs work on Wolf Territories regions only (Tessa's Compass works everywhere).";
    // [T958 2026-11-21] the words while choosing and when tiles are hidden (plain, British; the resident is 35-65)
    const char* const CHOOSE_INTRO = "Choosing: press a picture to show or hide it in this window. Hidden ones stay in the Wolf World menu. Press Done when you have finished.";
    std::string hiddenWords(S32 n)
    {
        return n == 1 ? std::string("1 tile is hidden here. Choose tiles... brings it back.")
                      : llformat("%d tiles are hidden here. Choose tiles... brings them back.", n);
    }
    const char* const ALL_HIDDEN = "Every tile is hidden here. Press Choose tiles... to show some again.";
    std::string choosingWords(S32 n)
    {
        return n == 0 ? std::string("Every tile is showing. Press a picture to hide it here.")
                      : llformat("%d hidden here. Show all brings every tile back.", n);
    }

    std::string pressNote(WolfFloaterHuds::ETile t)
    {
        return std::string(TILE_NAMES[t]) + " did not come on. If a message popped up, it says why; otherwise try again in a moment.";
    }

    // [W104 FEATFIX 2026-11-11] Wolf Territories only, as their menu items (menu_viewer.xml on_visible WolfGrid.IsOnWolfTerritories);
    // Tessa's Compass works on every grid (its menu item has no gate, floater_wolf_compass.xml "Every grid")
    const bool TILE_WOLF_ONLY[WolfFloaterHuds::T_COUNT] =
    {
        true, true, true, true, true, true, true, true, true,
        true, true, false, true,
        true,   // [W104 MERGE 2026-11-11] Fishing: Wolf Territories only, as its menu item (menu_viewer.xml WolfWorldFishing on_visible WolfGrid.IsOnWolfTerritories)
        true,   // [HUDS TRAIN 2026-11-13] the Train Cab: Wolf Territories only, as its menu item (menu_viewer.xml WolfTrainCab on_visible WolfGrid.IsOnWolfTerritories) and WolfTrain::onWolf (wolftraincab.cpp)
        true, true,   // [WEATHER PROPS / TUTORIALS 2026-11-13] Wolf Territories only, as their menu items (menu_viewer.xml WolfWeatherProps / WolfTutorials on_visible WolfGrid.IsOnWolfTerritories)
        true, true,   // [HUDS TILES 2 2026-11-14] Wolf Territories only, as their menu items (menu_viewer.xml WolfWorldTrainBuilder / WolfVehiclesTrainBuilder / WolfWorldTrack on_visible WolfGrid.IsOnWolfTerritories)
        true   // [STARFOLD 2026-11-24] Wolf Territories only, as its menu item (menu_viewer.xml WolfStarfold on_visible WolfStarfold.Available)
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
        mPics[i] = findChild<LLIconCtrl>(std::string("pic_") + TILE_IDS[i]);   // [T958 2026-11-21/]
    }
    mStatus = getChild<LLTextBox>("status");
    // [T958 2026-11-21] Choose tiles... (a toggle: Done while choosing) and Show all
    mChooseBtn = getChild<LLButton>("choose_btn");
    mShowAllBtn = getChild<LLButton>("show_all_btn");
    mChooseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setChoosing(mChooseBtn->getToggleState()); });
    mShowAllBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { showAll(); });
    // [HUDS TILES 2 2026-11-14] the tiles scroll (floater_wolf_huds.xml tiles_scroll / tiles_panel). Tabbing to a tile below
    // the fold brings it into view, as Wolf Experiences' cards (wolffloaterexperiences.cpp): a tile's rect is in tiles_panel's
    // coordinates, which are the scroll's document coordinates (llscrollcontainer.cpp scrollToShowRect).
    mScroll = getChild<LLScrollContainer>("tiles_scroll");
    // [HUDS SMALL 2026-11-14] the grid and margins as built (floater_wolf_huds.xml, after the header stretch), for fitToArea
    mIntro = getChild<LLTextBox>("intro");
    mPanel = getChild<LLView>("tiles_panel");
    {
        const LLRect win = getRect(), intro = mIntro->getRect(), scroll = mScroll->getRect(), status = mStatus->getRect();
        const LLRect panel = mPanel->getRect();
        mXmlWidth = win.getWidth();
        mPanelXmlWidth = panel.getWidth();
        mIntroXmlWidth = intro.getWidth();
        mStatusXmlWidth = status.getWidth();
        mScrollRightPad = win.getWidth() - scroll.mRight;
        mTopToIntro = win.getHeight() - intro.mTop;
        mIntroToScroll = intro.mBottom - scroll.mTop;
        mScrollToStatus = scroll.mBottom - status.mTop;
        mStatusBottom = status.mBottom;
        mIntroMinH = intro.getHeight();
        mStatusMinH = status.getHeight();
        mIntroToButtons = intro.mBottom - mChooseBtn->getRect().mTop;   // [T958 2026-11-21/] 6, as built
        mIntroWords = mIntro->getText();
        mGrid.clear();
        std::vector<S32> lefts, tops;
        for (S32 i = 0; i < T_COUNT; ++i)
        {
            const std::string id = TILE_IDS[i];
            TileParts tp;
            tp.tile = static_cast<ETile>(i);   // [T958 2026-11-21/]
            tp.views[0] = mTiles[i];
            tp.views[1] = getChild<LLView>("pic_" + id);
            tp.views[2] = getChild<LLView>("name_" + id);
            tp.views[3] = getChild<LLView>("desc_" + id);
            tp.views[4] = mStates[i];
            const LLRect tr = mTiles[i]->getRect();
            for (int k = 0; k < TileParts::N; ++k)
            {
                const LLRect vr = tp.views[k]->getRect();
                tp.dx[k] = vr.mLeft - tr.mLeft;
                tp.dtop[k] = tr.mTop - vr.mTop;
            }
            mGrid.push_back(tp);
            if (std::find(lefts.begin(), lefts.end(), tr.mLeft) == lefts.end()) lefts.push_back(tr.mLeft);
            if (std::find(tops.begin(), tops.end(), tr.mTop) == tops.end()) tops.push_back(tr.mTop);
        }
        // reading order: the top row first (panel rects count up from the bottom), each row left to right
        std::sort(mGrid.begin(), mGrid.end(), [](const TileParts& a, const TileParts& b)
        {
            const LLRect ra = a.views[0]->getRect(), rb = b.views[0]->getRect();
            return ra.mTop != rb.mTop ? ra.mTop > rb.mTop : ra.mLeft < rb.mLeft;
        });
        std::sort(lefts.begin(), lefts.end());
        std::sort(tops.begin(), tops.end(), std::greater<S32>());
        mGridOk = lefts.size() >= 2 && tops.size() >= 2;
        if (mGridOk)
        {
            mXmlCols = (S32)lefts.size();
            mColStep = lefts[1] - lefts[0];   // 264: a 256-wide tile and its 8 gap
            mRowStep = tops[0] - tops[1];     // 116: a 108-high tile and its 8 gap
            mFirstLeft = lefts[0];
            mFirstTopGap = panel.getHeight() - tops[0];
            mTileH = mTiles[0]->getRect().getHeight();
        }
    }
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
    // [T958 2026-11-21] the resident's choice as saved; a window opens out of choosing
    loadHidden();
    mChoosing = false;
    if (mChooseBtn) mChooseBtn->setToggleState(false);
    if (mShowAllBtn) mShowAllBtn->setVisible(false);
    refreshTiles();
    // [HUDS TILES 2 2026-11-14] from the top every time (as WolfStorm's window), fitted to the floaters' area as it is now
    if (mScroll) mScroll->goToTop();
    mFitW = mFitH = -1;
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
    // a toolbar shown or hidden): fit again. [HUDS SMALL 2026-11-14] its width as well: a viewer window made narrower at the
    // same height (tiled beside another window) changes only that
    if (gFloaterView)
    {
        const LLRect area = gFloaterView->getSnapRect();
        if (area.getWidth() != mFitW || area.getHeight() != mFitH)
        {
            fitToArea();
        }
    }
    LLFloater::draw();
}

// [HUDS TILES 2 2026-11-14] With seven rows the window (floater_wolf_huds.xml, 920 high in UI units) is taller than the
// floaters' area on many screens at a larger UI size. LLFloaterView::adjustToFitScreen (llfloater.cpp:3242-3307) only
// shrinks resizable floaters and this one is not, so it is done here: never taller than the area (getSnapRect, the space
// between the toolbars, in the same UI units as this window's rect, so it follows the UI size), never shorter than the
// intro, one row of tiles with its gap and the status line, and never taller than every tile needs. The tiles scroll
// inside. Then the window is brought wholly inside the area (adjustToFitScreen, allow_partial_outside false,
// snap_in_toolbars true, as wolffloaterbuilder.cpp).
// [HUDS SMALL 2026-11-14] Paul (W106 beta): "the huds menu on a smaller screen needs to scroll". His viewer window, made
// small (WolfViewer.log "Handling a resize event: 640x779"), is 474 UI units wide at UI size 1.35, and the window stayed
// 824 wide: only its height was fitted, so the right-hand column and the scroll bar (at the window's right edge) were past
// the edge of the screen, and adjustToFitScreen cannot shrink it (not resizable, above). Now the width is fitted too: as
// many of the XML's three columns as the area's width takes (824 for three, 560 for two, 296 for one: each column is a
// tile and its gap, mColStep), the same tiles in the same reading order, so every tile is reached by scrolling up and down
// only. The intro and the status line take the window's width less their XML margins and grow to their words' height
// (narrower, they wrap to more lines).
void WolfFloaterHuds::fitToArea()
{
    if (!gFloaterView || getParent() != gFloaterView || isMinimized() || !mGridOk)
    {
        return;
    }
    const LLRect area = gFloaterView->getSnapRect();
    mFitW = area.getWidth();
    mFitH = area.getHeight();

    // columns: the most that fit the area's width, one at the least (a window narrower than one tile cannot be helped)
    S32 cols = mXmlCols;
    while (cols > 1 && mXmlWidth - (mXmlCols - cols) * mColStep > mFitW)
    {
        --cols;
    }
    const S32 narrower = (mXmlCols - cols) * mColStep;
    const S32 width = mXmlWidth - narrower;
    // [T958 2026-11-21] the tiles laid out: every one while choosing, else the ones not hidden (the rest close up); a hidden
    // tile's parts are not shown at all, so Tab skips them too
    std::vector<const TileParts*> shown;
    for (const TileParts& tp : mGrid)
    {
        const bool show = mChoosing || !mHidden[tp.tile];
        for (int p = 0; p < TileParts::N; ++p) tp.views[p]->setVisible(show);
        if (show) shown.push_back(&tp);
    }
    const S32 tiles = (S32)shown.size();
    const S32 rows = llmax(1, (tiles + cols - 1) / cols);   // [T958/] one empty row when every tile is hidden
    const S32 panel_h = mFirstTopGap + (rows - 1) * mRowStep + mTileH;   // 804 for three columns, as the XML

    // the tiles, in reading order, cols to a row; each part keeps its XML offset from its tile
    mPanel->reshape(mPanelXmlWidth - narrower, panel_h);
    for (S32 k = 0; k < tiles; ++k)
    {
        const S32 left = mFirstLeft + (k % cols) * mColStep;
        const S32 top = panel_h - mFirstTopGap - (k / cols) * mRowStep;
        const TileParts& tp = *shown[k];
        for (int p = 0; p < TileParts::N; ++p)
        {
            const LLRect vr = tp.views[p]->getRect();
            LLRect moved;
            moved.setLeftTopAndSize(left + tp.dx[p], top - tp.dtop[p], vr.getWidth(), vr.getHeight());
            tp.views[p]->setShape(moved);
        }
    }

    // the intro's and the status line's heights at this width (never less than the XML gives them)
    const S32 intro_w = mIntroXmlWidth - narrower;
    const S32 status_w = mStatusXmlWidth - narrower;
    // [T958 2026-11-21] the intro's words for choosing, measured like the XML's
    const std::string intro_words = mChoosing ? std::string(CHOOSE_INTRO) : mIntroWords;
    if (mIntro->getText() != intro_words) mIntro->setText(intro_words);
    mIntro->reshape(intro_w, mIntroMinH);
    const S32 intro_h = llmax(mIntroMinH, mIntro->getTextPixelHeight());
    const S32 status_h = statusHeight(status_w);

    const S32 above = mTopToIntro + intro_h + mIntroToScroll;      // the title bar, the intro and its gap
    const S32 below = mScrollToStatus + status_h + mStatusBottom;  // the gap, the status line and the bottom margin
    const S32 full = above + panel_h + below;
    const S32 least = above + mRowStep + below;                     // one whole row with its gap
    const S32 height = llmax(least, llmin(mFitH, full));

    const LLRect r = getRect();
    LLRect fitted;
    fitted.setLeftTopAndSize(r.mLeft, r.mTop, width, height);   // the top left stays where it is
    if (fitted != r)
    {
        setShape(fitted);
    }
    // the intro, the tiles' scroll and the status line placed for this size (the follows flags alone would keep the XML's
    // intro and status heights)
    const LLRect intro_r = mIntro->getRect(), status_r = mStatus->getRect(), scroll_r = mScroll->getRect();
    mIntro->setShape(LLRect(intro_r.mLeft, height - mTopToIntro, intro_r.mLeft + intro_w, height - mTopToIntro - intro_h));
    // [T958 2026-11-21] Choose tiles... / Show all: their XML gap under the intro, however many lines it wrapped to
    for (LLButton* b : { mChooseBtn, mShowAllBtn })
    {
        if (!b) continue;
        LLRect br = b->getRect();
        br.setLeftTopAndSize(br.mLeft, height - mTopToIntro - intro_h - mIntroToButtons, br.getWidth(), br.getHeight());
        b->setShape(br);
    }
    mStatus->setShape(LLRect(status_r.mLeft, mStatusBottom + status_h, status_r.mLeft + status_w, mStatusBottom));
    const LLRect scroll_new(scroll_r.mLeft, height - above, width - mScrollRightPad, below);
    mScroll->setShape(scroll_new);
    // setShape reshapes only when the rect changed; the document (tiles_panel) may have changed alone, so the scroll bars
    // are worked out again here either way (LLScrollContainer::reshape: doc and page sizes, updateScroll)
    mScroll->reshape(scroll_new.getWidth(), scroll_new.getHeight());

    gFloaterView->adjustToFitScreen(this, false, true);
}

// [HUDS SMALL 2026-11-14] The status line's words change while the window shows (refreshTiles): its height is the tallest
// of every message it can show at this width, so a message never runs past it. Measured with the line itself (its font
// and wrapping), then its own words put back.
S32 WolfFloaterHuds::statusHeight(S32 width)
{
    const std::string now = mStatus->getText();
    mStatus->reshape(width, mStatusMinH);
    S32 h = mStatusMinH;
    mStatus->setText(std::string(NOT_ON_WOLF_STATUS));
    h = llmax(h, mStatus->getTextPixelHeight());
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        mStatus->setText(pressNote(static_cast<ETile>(i)));
        h = llmax(h, mStatus->getTextPixelHeight());
    }
    // [T958 2026-11-21] and the choosing / hidden words (the widest counts)
    for (const std::string& w : { hiddenWords(T_COUNT - 1), std::string(ALL_HIDDEN), choosingWords(0), choosingWords(T_COUNT) })
    {
        mStatus->setText(w);
        h = llmax(h, mStatus->getTextPixelHeight());
    }
    mStatus->setText(now);
    return h;
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
        // [STARFOLD 2026-11-24] on while the cockpit or the Launch panel shows, as its menu tick (WolfStarfold.IsShown)
        case T_STARFOLD:  return WolfStarfold::instance().panelShown();
        default:          return false;
    }
}

// Source: lltoolbarview.cpp postBuild before 2026-11-10 - each icon's commit callback, unchanged.
void WolfFloaterHuds::onTile(ETile t)
{
    // [T958 2026-11-21] choosing: a press shows or hides the tile here, never its HUD (on any grid: it is only this window)
    if (mChoosing)
    {
        setHidden(t, !mHidden[t]);
        return;
    }
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
        // [STARFOLD 2026-11-24] the same as Wolf World > Vehicles & Travel > Starfold (Space) (WolfStarfold.Toggle); only where
        // the region's space is on (refreshTiles greys the tile elsewhere and says why)
        case T_STARFOLD:
            if (!WolfStarfold::instance().spaceHere()) return;
            WolfStarfold::instance().togglePanel();
            break;
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
    mNote = pressNote(t);   // [HUDS SMALL 2026-11-14/] the words unchanged, now in pressNote
    refreshTiles();
}

void WolfFloaterHuds::refreshTiles()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        if (!mTiles[i] || !mStates[i]) continue;
        const ETile t = static_cast<ETile>(i);
        bool usable = wolf || !TILE_WOLF_ONLY[i];   // [W104 FEATFIX 2026-11-11/] Tessa's Compass works on every grid
        // [STARFOLD 2026-11-24] Starfold only where the region's space is on, as its menu item (WolfStarfold.Available): greyed
        // elsewhere, the state line saying why (refreshTiles' words, below)
        const bool noSpace = t == T_STARFOLD && wolf && !WolfStarfold::instance().spaceHere();
        if (noSpace) usable = false;
        const bool on = usable && tileOn(t);
        mTiles[i]->setEnabled(usable || mChoosing);   // [T958/] choosing works off Wolf too: it is only this window
        mTiles[i]->setToggleState(on && !mChoosing);
        // [T958 2026-11-21] choosing: the line says whether it shows here (words, not colour alone), a hidden one's picture dimmed
        if (mPics[i]) mPics[i]->setColor(LLColor4(1.f, 1.f, 1.f, (mChoosing && mHidden[i]) ? 0.35f : 1.f));
        if (mChoosing)
        {
            const std::string choose = mHidden[i] ? std::string("\xE2\x97\x8B Hidden here") : std::string("\xE2\x9C\x93 Shown here");   // U+25CB / U+2713
            if (mStates[i]->getText() != choose)
            {
                mStates[i]->setText(choose);
                mStates[i]->setColor(LLUIColorTable::instance().getColor(mHidden[i] ? "WolfHudKitTextDim" : "LtGreen"));
            }
            continue;
        }
        // The word as well as the light (never colour alone): ON / OFF, and what the old tooltips said beside it.
        std::string state = on ? "ON" : (noSpace ? "Space isn't on here" : "OFF");
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
        // The state line says it plainly as well as the lit key (never colour alone): a filled dot and light green words when
        // on, a hollow dot and the kit's dim words when off. [HUD KIT 2026-11-25] the tiles are Gunmetal Console keys whatever
        // the skin (floater_wolf_huds.xml, WolfHudKit_Key / KeyLit): LtGreen (colors.xml, 0.79 1 0.79) on the key cap's
        // lightest middle as rendered (76, 75, 78) is 7.7:1, WolfHudKitTextDim 5.6:1.
        state = std::string(running ? "\xE2\x97\x8F " : "\xE2\x97\x8B ") + state;   // U+25CF / U+25CB
        if (mStates[i]->getText() != state)
        {
            mStates[i]->setText(state);
            mStates[i]->setColor(LLUIColorTable::instance().getColor(running ? "LtGreen" : "WolfHudKitTextDim"));
        }
    }
    if (mStatus)
    {
        // [W104 FEATFIX 2026-11-11 A7] after a press that did not switch a HUD on: why the window stayed open
        // [T958 2026-11-21] then, in the plain label colour: choosing, or how many tiles are hidden here
        std::string msg = wolf ? mNote : std::string(NOT_ON_WOLF_STATUS);   // [HUDS SMALL 2026-11-14/] words unchanged
        bool caution = !msg.empty();
        if (mChoosing && mNote.empty())
        {
            msg = choosingWords(hiddenCount());
            caution = false;
        }
        else if (msg.empty() && hiddenCount() > 0)
        {
            msg = hiddenCount() == T_COUNT ? std::string(ALL_HIDDEN) : hiddenWords(hiddenCount());
        }
        if (mStatus->getText() != msg)
        {
            mStatus->setText(msg);
            mStatus->setColor(LLUIColorTable::instance().getColor(caution ? "WolfHudKitAmber" : "WolfHudKitTextDim"));
        }
    }
}

// ---- [T958 2026-11-21] which tiles show here ------------------------------------------------------------------------------

void WolfFloaterHuds::loadHidden()
{
    for (bool& h : mHidden) h = false;
    // the TILE_IDS of the hidden ones; an id this viewer does not have (a later or older tile) is left alone in the setting
    const LLSD ids = gSavedPerAccountSettings.getLLSD("WolfHudsHidden");
    if (!ids.isArray()) return;
    for (LLSD::array_const_iterator it = ids.beginArray(); it != ids.endArray(); ++it)
    {
        const std::string id = it->asString();
        for (S32 i = 0; i < T_COUNT; ++i)
        {
            if (id == TILE_IDS[i]) mHidden[i] = true;
        }
    }
}

void WolfFloaterHuds::saveHidden() const
{
    // the ids this viewer knows, as chosen, plus any it does not know that were already there (kept for the viewer that does)
    LLSD out = LLSD::emptyArray();
    const LLSD old = gSavedPerAccountSettings.getLLSD("WolfHudsHidden");
    if (old.isArray())
    {
        for (LLSD::array_const_iterator it = old.beginArray(); it != old.endArray(); ++it)
        {
            const std::string id = it->asString();
            bool known = false;
            for (S32 i = 0; i < T_COUNT; ++i) known = known || id == TILE_IDS[i];
            if (!known && !id.empty()) out.append(id);
        }
    }
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        if (mHidden[i]) out.append(std::string(TILE_IDS[i]));
    }
    gSavedPerAccountSettings.setLLSD("WolfHudsHidden", out);
}

S32 WolfFloaterHuds::hiddenCount() const
{
    S32 n = 0;
    for (bool h : mHidden) n += h ? 1 : 0;
    return n;
}

void WolfFloaterHuds::setHidden(ETile t, bool hidden)
{
    mHidden[t] = hidden;
    saveHidden();   // at once: nothing to press to keep it
    refreshTiles();
}

void WolfFloaterHuds::setChoosing(bool on)
{
    mChoosing = on;
    mNote.clear();
    if (mChooseBtn) mChooseBtn->setToggleState(on);
    if (mShowAllBtn) mShowAllBtn->setVisible(on);
    refreshTiles();
    mFitW = mFitH = -1;   // the tiles laid out and the intro's words change: fit again
    fitToArea();
}

void WolfFloaterHuds::showAll()
{
    for (bool& h : mHidden) h = false;
    saveHidden();
    refreshTiles();
}
