/**
 * @file wolfgamehud.h
 * @brief WolfViewer: Wolf Roleplay on the world view - the game HUD across the bottom, notices, markers, the
 *        downed overlay and the screen effects - and the drawing kit every game surface uses.
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

#ifndef WOLF_GAME_HUD_H
#define WOLF_GAME_HUD_H

#include <string>
#include <vector>

#include "llfontgl.h"
#include "llrect.h"
#include "lluictrl.h"
#include "v4color.h"
#include "wolfgame.h"
#include "wolffloaterroleplay.h"

class LLViewerTexture;

/**
 * [WOLF GAME UI 2026-10-09] The game interface's own drawing kit. Paul: "it should be like the
 * airplane and sailing interfaces ... model it on REAL games like Eve online, other mmorpgs".
 * Like the flight deck (wolfflightdeck.cpp), nothing here is a stock widget: every frame, gauge,
 * slot and button is drawn, so the game HUD (WolfGameHUD), the Roleplay window
 * (WolfGameConsole) and game dialogs (WolfGameDialogView) look like one game in every skin.
 *
 * Two palettes (VIEWER_SPEC.md §3 Themes): "wolves" (night blue, moon silver, wolf-eye amber) and
 * "plain" (steel, white, ice blue). Content that is not the game's own (verified 0) is always
 * drawn plain. Text sits on a palette's card or base, which keep it at 4.5:1 or better even with
 * a white sky or a white picture behind them (the figures are worked in wolfgamehud.cpp).
 */
namespace WolfGameDraw
{
    struct Palette
    {
        bool     mWolves = false;
        LLColor4 mBaseTop, mBaseBottom;   // the frame's fill, a vertical gradient
        LLColor4 mCard;                   // the plates text sits on
        LLColor4 mText, mDim, mAccent;    // body text, secondary text, headers / active / focus
        LLColor4 mEdge;                   // the frame's hairline and corner brackets
        LLColor4 mTrack;                  // empty part of a bar or gauge
        LLColor4 mButtonTop, mButtonBottom, mButtonEdge;
        LLColor4 mOnAccent;               // text on an accent-filled control
    };
    const Palette& wolvesPalette();
    const Palette& plainPalette();
    /** The game's own palette: wolves when the game's theme is "wolves". */
    const Palette& gamePalette();
    /** `themed` = may wear the game's theme (the game's own content, of a "wolves" game). */
    const Palette& palette(bool themed);

    // The "wolves" colours (VIEWER_SPEC.md §3 Themes), for code that names them directly.
    extern const LLColor4 WOLF_NIGHT;      // #0f1626
    extern const LLColor4 WOLF_NIGHT_2;    // #141d31
    extern const LLColor4 WOLF_SILVER;     // #e6ebf5
    extern const LLColor4 WOLF_SILVER_DIM; // #aab4c8
    extern const LLColor4 WOLF_AMBER;      // #f5a524
    extern const LLColor4 CHIP_FILL;       // the Down / Dead chip, #b91c1c

    // ---- lettering ----
    /** Cinzel Bold (fonts.xml "WolfDisplay") at the largest size whose line fits `px`;
     *  the viewer's bold sans if that font is missing. Titles, names and big numbers. */
    const LLFontGL* display(F32 px);
    /** One line of text, cut with "..." past `max_w` (0 = no limit). Returns the right edge. */
    F32 textL(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::LEFT, LLFontGL::VAlign v = LLFontGL::TOP, F32 max_w = 0.f);
    /** Upper-case ASCII letters (other characters unchanged): small-caps headers and labels. */
    std::string caps(const std::string& s);

    // ---- shapes (view-local coordinates, no texture) ----
    void rect(F32 l, F32 b, F32 r, F32 t, const LLColor4& c);
    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bottom);
    void lineW(F32 x1, F32 y1, F32 x2, F32 y2, F32 w, const LLColor4& c);
    void disc(F32 cx, F32 cy, F32 r, const LLColor4& c, S32 seg = 40);
    /** A stroked arc; angles in radians counter-clockwise from +x. */
    void arc(F32 cx, F32 cy, F32 r, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48);
    /** A rectangle with its four corners cut at 45 degrees by `cut` (the plate shape). */
    void chamfer(F32 l, F32 b, F32 r, F32 t, F32 cut, const LLColor4& top, const LLColor4& bottom);
    void chamferEdge(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 w, const LLColor4& c);
    /**
     * A game frame (EVE Photon's framed panels): the palette's gradient plate, a hairline edge,
     * bright corner brackets and a soft outer glow. `alpha` scales the fill (floater fade).
     * With `fill` false only the edge, brackets and glow are drawn (a picture is the fill).
     */
    void frame(F32 l, F32 b, F32 r, F32 t, const Palette& p, F32 alpha = 1.f, F32 cut = 10.f, bool fill = true);
    /** A plate for text, inset in a frame. */
    void card(F32 l, F32 b, F32 r, F32 t, const Palette& p, F32 cut = 6.f);

    /**
     * A ring gauge (EVE's capacitor and shield / armour / hull arcs): a 270-degree track open at
     * the bottom, filled clockwise from the lower left by `frac`, with a bright leading tick.
     */
    void gauge(F32 cx, F32 cy, F32 r, F32 w, F32 frac, const LLColor4& colour, const LLColor4& track);

    enum EButton { BTN_HOVER = 1, BTN_PRESSED = 2, BTN_FOCUS = 4, BTN_DISABLED = 8, BTN_PRIMARY = 16, BTN_DANGER = 32, BTN_ACTIVE = 64 };
    /** A drawn button; the label is centred and cut with "..." when too long. */
    void button(F32 l, F32 b, F32 r, F32 t, const std::string& label, const Palette& p, U32 flags,
                const LLFontGL* font = nullptr);
    /** The keyboard focus ring: 2 px of accent just outside the rectangle. */
    void focusRing(F32 l, F32 b, F32 r, F32 t, const Palette& p);
    void closeX(F32 cx, F32 cy, F32 s, const LLColor4& c);
    void chevron(F32 cx, F32 cy, bool down, const LLColor4& c);

    // ---- emblems ----
    /** The paw-print motif, `size` px tall, centred at (cx, cy). */
    void paw(F32 cx, F32 cy, F32 size, const LLColor4& c);
    /** A crescent moon, `r` px radius, centred at (cx, cy), cut against `bg`. */
    void crescent(F32 cx, F32 cy, F32 r, const LLColor4& c, const LLColor4& bg);
    /** A sitting wolf howling, facing left, in the size x size box from (l, b). */
    void wolf(F32 l, F32 b, F32 size, const LLColor4& c);
    /** A coin (the wallet's icon): a lit disc with a rim and a paw stamped on it. */
    void coin(F32 cx, F32 cy, F32 r, const Palette& p);
    /** A drawstring bag (the inventory's icon), `s` px tall. */
    void bag(F32 cx, F32 cy, F32 s, const LLColor4& c);
    /** True when an icon field holds a texture UUID (wolfgame.cpp iconStr), not a text glyph. */
    bool isTextureIcon(const std::string& icon);
    /**
     * [WOLF GAME ICONS 2026-10-09] A game icon in its frame, the size x size square from (l, b).
     * A texture UUID is the picture (a neutral empty frame while it loads - Paul: "PROPER
     * GRAPHICS", never an emoji); another game's short text glyph sits centred in the same frame;
     * with no icon, `fallback` (an initial) in the display face. `framed` false: no frame.
     */
    void icon(const std::string& icon, F32 l, F32 b, F32 size, const Palette& p,
              const std::string& fallback = std::string(), bool framed = true);
    /**
     * [WOLF GAME UI 2026-10-09] One of the bundled photographs (textures.xml WolfGame_*; sources
     * in skins/default/textures/wolfgame/CREDITS.txt), loaded on first use; null until it has.
     */
    LLViewerTexture* photo(const std::string& name);
    /** Shutdown: let go of every texture the game UI holds, before the texture list goes. */
    void releaseTextures();
    /** The wallet's coin: the game's currency icon when it has one, else the drawn coin. */
    void walletIcon(F32 cx, F32 cy, F32 r, const Palette& p);
    /** [SECURITY] the "Unverified" mark: caution icon + word, from (left, top). */
    void unverifiedMark(F32 left, F32 top, const LLFontGL* font, F32* right_out);

    // ---- backgrounds ----
    /** The wolves theme's own picture: night sky, stars, a full moon, pines, a howling wolf;
     *  inside the chamfered shape (l, b, r, t, cut). */
    void nightScene(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 alpha);
    /** The plain theme's own picture: dark steel with a faint grid. */
    void steelScene(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 alpha);
    /** A texture filling the chamfered shape (cover: cropped, never stretched), tinted. False
     *  while the texture has nothing to draw yet. */
    bool imageCover(LLViewerTexture* tex, F32 l, F32 b, F32 r, F32 t, F32 cut, const LLColor4& tint);
    /** A texture filling a disc (the portrait). False while it has nothing to draw yet. */
    bool imageDisc(LLViewerTexture* tex, F32 cx, F32 cy, F32 r, const LLColor4& tint);

    // ---- numbers and text ----
    /** "1,234,567" - thousands separators, no locale. */
    std::string thousands(S64 v);
    /** Text with every digit in a cell as wide as the widest digit (tabular figures),
     *  right-aligned at `right`, top at `y`. Returns the left edge. */
    F32 tabularRight(const LLFontGL* font, const std::string& text, F32 right, F32 y, const LLColor4& c);
    F32 tabularWidth(const LLFontGL* font, const std::string& text);
    /** Words to lines no wider than `width`; '\n' always breaks. */
    std::vector<std::string> wrap(const LLFontGL* font, const std::string& text, F32 width, S32 max_lines = 0);
    /** The colour mixed toward white until its relative luminance reaches `min_lum`. */
    LLColor4 readable(const LLColor4& c, F32 min_lum = 0.33f);

    /** One stat as a bar: icon + label left, "v / m" right (tabular), a `bar_h` bar under them. */
    S32 statRowHeight(S32 bar_h = 6);
    void drawStatRow(const WolfGame::Stat& s, F32 left, F32 top, F32 width, const Palette& p, S32 bar_h = 6);
    /** The wallet's words: "100 Bones" - the money's name; its symbol only when it has no name
     *  (a symbol may be an emoji, and the wallet's picture is walletIcon). */
    std::string walletText(S64 balance, const std::string& symbol, const std::string& name);
    /** The wallet line the game shows now, or "" when the game has no money. */
    std::string currentWallet();
}

/**
 * The portrait (the game's logo, or the theme's emblem) with up to three stats as rings round it
 * and the level badge in the rings' gap - WoW's player portrait with EVE's arcs. Drawn by the
 * Roleplay window's hero block (the game HUD's portrait has the XP ring instead). `r` is the portrait's radius; the
 * rings reach r * 1.5.
 */
void wolfGamePortrait(F32 cx, F32 cy, F32 r, const WolfGameDraw::Palette& p);

/**
 * [WOLF GAME 2026-10-09] The always-on layer, between the world and the floaters (main_view.xml,
 * beside wolf_flight_deck): the UI stays usable over every overlay (§3.7). Clicks fall through to
 * the world except on what it draws.
 *
 * [GAME HUD 2026-10-09] Paul: "i was expecting a hud like affair across the bottom like eve online
 * or wow ... also make them scale with the UI sometimes the icons are tiny". The game's screen is
 * one HUD across the bottom, on whatever the bottom toolbar leaves (wolfflightdeck.cpp
 * layoutAndDraw): left, the portrait with the XP ring and the first three stats as orbs (Diablo /
 * WoW); centre, the 12 action slots - always, the empty ones drawn - and the weapon slots, with the
 * cast bar and the buffs above; right, the first 8 bag slots and the wallet; along its top edge
 * the name plate and the buttons that open the Roleplay window's pages, and PANEL. Sizes come
 * from the window's width in UI units (so UI Scale scales it too): an action slot is
 * width / 31, 40 to 64. Too narrow for one row, the action bar takes a row of its own on top.
 * The target frame stays top-centre. One bottom panel at a time: WolfGame::hudShowing.
 */
class WolfGameHUD : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfGameHUD(const Params& p);
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

private:
    void drawEffects(const LLRect& r);
    void drawDowned(const LLRect& r);
    void drawMarkers(const LLRect& r);
    void drawNotices(const LLRect& r, S32 top_pad, S32 right_pad);
    // [GAME HUD] the HUD across the bottom
    void drawBottomHud(const LLRect& r);
    void drawActionSlot(S32 i, F32 x, F32 b, F32 S);
    void drawEmptySlot(F32 x, F32 b, F32 S, const std::string& key);
    void drawWeaponSlot(S32 idx, F32 x, F32 b, F32 S);
    void drawOrb(const WolfGame::Stat& s, S32 i, F32 cx, F32 cy, F32 R);
    void drawPortraitXP(F32 cx, F32 cy, F32 R);
    // [COMBAT 2026-10-09] VIEWER_SPEC.md §5.3-5.6
    void drawBuffs(F32 cx, F32 bottom, F32 size);
    void drawTarget(const LLRect& r, S32 top_pad);
    void drawCastBar(F32 cx, F32 bottom, F32 width, F32 height);
    void drawCombatText(const LLRect& r);
    void drawCombatError(const LLRect& r);
    /** Above the viewer's bottom toolbar (or its hide strip). */
    F32 bottomPad() const;
    /** A left click on the world in Game Mode chooses the target (§5.5); root-view coordinates. */
    void pickTarget(S32 sx, S32 sy);
    S32 mClickX = 0, mClickY = 0;     // the press, root-view coordinates
    LLCoordGL mClickMouse;            // the mouse then (gViewerWindow's own coordinates)
    F64 mClickAt = 0.0;               // 0 = no press waiting

    enum EHit
    {
        H_ACTION = 1, H_CAST_CANCEL, H_TARGET_CLEAR, H_BUFF, H_WEAPON, H_TARGET,
        H_EMPTY,          // an empty action or bag slot: its tooltip only
        H_PLATE,          // the HUD's panel: not a hole to the world
        H_PORTRAIT, H_ORB, H_BAG, H_WALLET,
        H_PAGE,           // a button that opens a Roleplay window page (arg: WolfGameConsole::EPage)
        H_PANEL_BG,       // PANEL: the HUD's background on / off
        H_GUIDE_OK, H_GUIDE_HELP, H_GUIDE_CARD,
        H_GAMEMODE_SW     // [2026-10-09] the Game Mode on / off switch on the top edge
    };
    WolfGameHits mCombatHits;
    struct NoticeHit { LLRect mRect, mClose; U32 mSerial; };
    std::vector<NoticeHit> mNoticeHits;
};

#endif // WOLF_GAME_HUD_H
