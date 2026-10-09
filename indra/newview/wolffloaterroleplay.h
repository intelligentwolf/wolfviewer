/**
 * @file wolffloaterroleplay.h
 * @brief WolfViewer: Wolf Roleplay windows - the Roleplay window (the drawn game console), game
 *        dialogs and the game's web page.
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

#ifndef WOLF_FLOATER_ROLEPLAY_H
#define WOLF_FLOATER_ROLEPLAY_H

#include <functional>
#include <string>
#include <vector>

#include "llfloater.h"
#include "lluictrl.h"

class LLFontGL;
class LLMediaCtrl;
class LLTextBox;
class LLViewerTexture;

namespace WolfGameDraw { struct Palette; }

/**
 * [WOLF GAME UI 2026-10-09] The player's choice of picture behind the Roleplay window (Paul:
 * "users can set the background image"): the theme's own, a texture dragged from inventory, or a
 * picture file on this computer (loaded as a local texture, lllocalbitmaps.cpp - the texture
 * picker's own mechanism). Per account (settings_per_account.xml WolfGameBg*).
 */
namespace WolfGameBackground
{
    enum ESource { SRC_THEME = 0, SRC_TEXTURE, SRC_FILE };
    ESource source();
    /** The picture to draw, or null for the theme's own (or while it loads). */
    LLViewerTexture* texture();
    /** A few words for the Look page: "Theme picture", "Texture from your inventory", a file name. */
    std::string describe();
    void useTexture(const LLUUID& asset_id);
    /** Loads `path` as a local texture; false (and `why`) when it cannot be read. */
    bool useFile(const std::string& path, std::string& why);
    void reset();
    F32 opacity();
    void setOpacity(F32 v);
    S32 tint();
    void setTint(S32 index);
    S32 tintCount();
    const char* tintName(S32 index);
    LLColor4 tintColour(S32 index);
    /** Shutdown: drop the fetched texture and the local bitmap. */
    void release();
}

/**
 * Clickable things on a drawn surface, rebuilt every frame, with keyboard focus that survives the
 * rebuild (by id and argument): Tab / Shift-Tab move it, Enter or Space press it. Shared by the
 * console and the dialog view.
 */
class WolfGameHits
{
public:
    struct Hit
    {
        F32 mL = 0.f, mB = 0.f, mR = 0.f, mT = 0.f;
        S32 mId = 0;
        S32 mArg = 0;
        std::string mTip;
        bool mFocusable = true;
        bool mEnabled = true;
    };
    void clear() { mHits.clear(); }
    void add(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable = true, bool enabled = true);
    const Hit* at(S32 x, S32 y) const;
    const Hit* focused() const;
    bool isFocused(S32 id, S32 arg) const { return mFocusId == id && mFocusArg == arg; }
    bool isHover(S32 id, S32 arg) const { return mHoverId == id && mHoverArg == arg; }
    bool isPressed(S32 id, S32 arg) const { return mPressId == id && mPressArg == arg; }
    void setFocus(S32 id, S32 arg) { mFocusId = id; mFocusArg = arg; }
    /** Tab: the next (or previous) focusable, enabled hit; false when there is none. */
    bool moveFocus(bool forward);
    /** The focused hit vanished (a page changed): the first focusable one instead. */
    void keepFocusValid();
    void setHover(const Hit* h) { mHoverId = h ? h->mId : 0; mHoverArg = h ? h->mArg : 0; }
    void setPressed(const Hit* h) { mPressId = h ? h->mId : 0; mPressArg = h ? h->mArg : 0; }
    S32 pressedId() const { return mPressId; }
    S32 pressedArg() const { return mPressArg; }
    const std::vector<Hit>& all() const { return mHits; }

private:
    std::vector<Hit> mHits;
    S32 mFocusId = 0, mFocusArg = 0;
    S32 mHoverId = 0, mHoverArg = 0;
    S32 mPressId = 0, mPressArg = 0;
};

/**
 * [WOLF GAME UI 2026-10-09] The Roleplay window's whole face (floater_wolf_roleplay.xml). Paul:
 * "COMPLETELY WRONG ... it should be like the airplane and sailing interfaces, look online model it
 * on REAL games like Eve online, other mmorpgs it should attractive and users can set the
 * background image". Drawn like the flight deck (wolfflightdeck.cpp), with the kit in
 * wolfgamehud.cpp:
 *   hero        the portrait with stat rings and level badge (WoW player portrait + EVE arcs), the
 *               avatar's name, the game, the wallet (Paul: "where are the tokens"), and the
 *               Game Mode switch (§3.2, OFF by default)
 *   tabs        Stats (ring gauges, the game's info panel, markers elsewhere) - Combat (weapon
 *               cards, the armour paper doll, abilities, effects; §5.3) - Bag (a slot grid,
 *               always shown, empty slots too, the game's other items at 0: Albion's slots, count
 *               bottom-right) - Pets - Game (logo, description, rules, website) - Look (the
 *               background picture) - Help (opens by itself the first time Game Mode comes on)
 *   action bar  the game's buttons, their source and "Unverified" when not the game's own
 *   footer      Leave this game, Clear screen effects, and a status line for every action's result
 * Drop a texture from inventory anywhere on it to make that the background.
 */
class WolfGameConsole : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfGameConsole(const Params& p);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;
    void onMouseCaptureLost() override;

    enum EPage { PAGE_STATS = 0, PAGE_COMBAT, PAGE_BAG, PAGE_PETS, PAGE_GAME, PAGE_LOOK, PAGE_HELP, PAGE_COUNT };
    void showPage(EPage page);
    EPage page() const { return mPage; }
    /** Escape: the question inside the window takes it (its safe answer), else the window closes. */
    void escape();
    /** A line at the foot for an action's result: kept until the next action when `error`
     *  (errors never vanish by themselves), a few seconds otherwise. */
    void setStatus(const std::string& text, bool error);

private:
    enum EHit
    {
        H_NONE = 0, H_CLOSE, H_GAMEMODE, H_TAB, H_ACTION, H_LEAVE, H_CLEARFX, H_MARKER_MAP, H_WEBSITE,
        H_BG_FILE, H_BG_RESET, H_BG_OPACITY, H_BG_TINT, H_CONFIRM_YES, H_CONFIRM_NO, H_SLOT, H_PET, H_SCROLLBAR,
        H_EQUIP, H_UNEQUIP, H_RELOAD, H_ABILITY, H_GEAR, H_INFO
    };
    enum EConfirm { CONFIRM_NONE = 0, CONFIRM_LEAVE, CONFIRM_MAP };

    void layoutAndDraw();
    F32 drawHero(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 drawTabs(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** The game's own buttons (§3.3) from `bottom` up; returns their top. */
    F32 drawActions(F32 l, F32 r, F32 bottom);
    void drawFooter(F32 l, F32 r, F32 bottom, const WolfGameDraw::Palette& p);
    void drawPage(F32 l, F32 b, F32 r, F32 t, const WolfGameDraw::Palette& p);
    F32 pageStats(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageCombat(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageHelp(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 weaponCard(F32 l, F32 r, F32 top, S32 slot_index, const WolfGameDraw::Palette& p);
    F32 armourSlot(F32 l, F32 top, F32 w, S32 slot_index, bool right_aligned, const WolfGameDraw::Palette& p);
    F32 paragraph(F32 l, F32 r, F32 top, const std::string& text, const LLColor4& colour, const LLFontGL* font = nullptr);
    F32 pageBag(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pagePets(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageGame(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageLook(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 emptyCard(F32 l, F32 r, F32 top, const std::string& head, const std::string& body, const WolfGameDraw::Palette& p);
    F32 sectionHead(F32 l, F32 r, F32 top, const std::string& text, const WolfGameDraw::Palette& p);
    void drawConfirm(const WolfGameDraw::Palette& p);
    void drawBackground(F32 W, F32 H, F32 cut, F32 alpha, const WolfGameDraw::Palette& p);

    /** Adds a hit, clipped to the page while the page is being drawn. */
    void hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable = true, bool enabled = true);
    U32 flagsFor(S32 id, S32 arg, bool enabled = true) const;
    void press(S32 id, S32 arg);
    void setOpacityFromX(S32 x);
    void askConfirm(EConfirm what, const std::string& marker_id = std::string());
    void onGameMode();
    void onAction(S32 index);
    void onClearEffects();
    void onWebsite();
    void onChooseFile();
    void showMarkerOnMap(const std::string& id);
    LLFloater* floater();

    WolfGameHits mHits;
    EPage mPage = PAGE_STATS;
    F32 mScroll[PAGE_COUNT] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
    F32 mContentH = 0.f;
    LLRect mPageRect;              // where the page shows (local)
    bool mInPage = false;          // hits being added belong to the page (clip them)
    bool mKeyboardNav = false;     // the focus ring shows once the keyboard is used
    bool mDraggingScroll = false;
    bool mDraggingSlider = false;
    F32 mSliderL = 0.f, mSliderR = 1.f;
    F32 mScrollGrabY = 0.f, mScrollGrabStart = 0.f;
    F64 mDragOverUntil = 0.0;      // a texture is being dragged over: say so
    EConfirm mConfirm = CONFIRM_NONE;
    std::string mConfirmMarker;
    std::string mStatus;
    bool mStatusError = false;
    F64 mStatusUntil = 0.0;
    std::vector<std::string> mMarkerIds;   // the markers listed on the Stats page, by row
};

/**
 * World > Roleplay, the toolbar's Roleplay button, or the game HUD's buttons (§3.3, §3.12). A floater
 * only for what floaters give (moving, resizing, layering, closing): its face is the console.
 */
class WolfFloaterRoleplay : public LLFloater
{
public:
    WolfFloaterRoleplay(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    /**
     * [GAME HUD 2026-10-09] The HUD's buttons (Character, Combat, Bag, Pets, Game, Look, Help):
     * open the window on that page. `toggle`: when it already shows that page, close it instead
     * (WoW's character / bag keys).
     */
    static void showPage(WolfGameConsole::EPage page, bool toggle);
    /** The window is open on `page`. */
    static bool showingPage(WolfGameConsole::EPage page);
    /** Escape while this window has the keyboard (WolfGame::handleEscape): WolfGameConsole::escape. */
    static bool escapeFocused();

private:
    WolfGameConsole* mConsole = nullptr;
    /** Opened from the game HUD: the window does not take the keyboard (the arrow keys keep
     *  walking the avatar until the player clicks into the window). */
    static bool sOpenQuietly;
};

/**
 * [WOLF GAME UI 2026-10-09] A game dialog's (or the web prompt's) drawn face: a game frame, the
 * title in the display face, the source with "Unverified" (icon + word) when not the game's own -
 * and then the plain palette, never the game's theme - the text, and the buttons three a row.
 * Its height follows the text; the floater sizes itself to it.
 */
class WolfGameDialogView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfGameDialogView(const Params& p);
    void setContent(const std::string& title, const std::string& src, bool verified, const std::string& text,
                    const std::vector<std::string>& buttons, std::function<void(const std::string&)> on_button);
    /** The height this content needs at `width`. */
    S32 measure(S32 width) const;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;

private:
    F32 layout(F32 W, F32 H, bool draw) const;
    void press(S32 id, S32 arg);
    std::string mTitle, mSrc, mText;
    bool mVerified = true;
    std::vector<std::string> mButtons;
    std::function<void(const std::string&)> mOnButton;
    mutable WolfGameHits mHits;
    bool mKeyboardNav = false;
};

/**
 * A wolfGameDialog (§3.5): placed like llDialog, top-right, under any script dialogs already
 * there; not modal; one floater per dialog id. A button sends ["dialog", id, label]; the close X
 * or Escape sends nothing.
 */
class WolfFloaterGameDialog : public LLFloater
{
public:
    WolfFloaterGameDialog(const LLSD& key);
    bool postBuild() override;
    void draw() override;   // [WOLF GRID GATE] closes itself off Wolf

    /** The "dialog" message: {id, title, text, buttons:[label...], src, verified}. */
    static void showDialog(const LLSD& msg);
    static void closeAll();
    /** Escape while one of these has the keyboard focus. */
    static bool closeFocused();

private:
    void build(const LLSD& msg);
    void place();
    std::string mDialogId;
    WolfGameDialogView* mView = nullptr;
};

/**
 * The game's web page (§3.8): https only, one at a time. "panel" docks on the right, "window"
 * is an ordinary floater, "overlay" covers the world view with a close X (Escape closes it,
 * WolfGame::handleEscape). The page is untrusted content in the viewer's media browser.
 */
class WolfFloaterGameWeb : public LLFloater
{
public:
    WolfFloaterGameWeb(const LLSD& key);
    bool postBuild() override;
    void draw() override;   // [WOLF GRID GATE] closes itself off Wolf
    void onClose(bool app_quitting) override;

    /** verified = false (any script but the game's own): a prompt first, the page only on Open,
     *  and never as an overlay. */
    static void showPage(const std::string& url, const std::string& mode, S32 w, S32 h, const std::string& src,
                         bool verified);
    static void closePage();
    static bool closeOverlayIfFocusAllows();

private:
    static void openPage(const std::string& url, const std::string& mode, S32 w, S32 h, const std::string& src,
                         bool verified);
    static void closeWindows();
    static U32 sPageSerial;   // bumped by every web / webclose: an old prompt's Open does nothing
    LLMediaCtrl* mBrowser = nullptr;
    LLTextBox*   mHostLabel = nullptr;   // overlay: the page's real host, always shown
    std::string  mMode;
};

/**
 * [SECURITY 2026-10-09] "<host> (from <src>, unverified) wants to open a page" - Open / Not now.
 * Drawn plain text (the dialog view never parses links), so neither name can become a clickable
 * SLURL; the page loads only on Open.
 */
class WolfFloaterGameWebPrompt : public LLFloater
{
public:
    WolfFloaterGameWebPrompt(const LLSD& key);
    bool postBuild() override;
    void draw() override;   // [WOLF GRID GATE] closes itself off Wolf
    static void ask(const std::string& host, const std::string& src, std::function<void()> open);
    /** Escape while the prompt has the keyboard: "Not now" (nothing opens). */
    static bool closeFocused();

private:
    std::function<void()> mOpen;
    WolfGameDialogView* mView = nullptr;
};

#endif // WOLF_FLOATER_ROLEPLAY_H
