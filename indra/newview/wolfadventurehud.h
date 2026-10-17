/**
 * @file wolfadventurehud.h
 * @brief WolfViewer: Wolf Adventure on screen - the stage (letterbox, fade, chapter title), the
 *        objective line, vignette cards, the handout viewer and the honour bar.
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

#ifndef WOLF_ADVENTURE_HUD_H
#define WOLF_ADVENTURE_HUD_H

#include <functional>
#include <string>

#include "llfloater.h"
#include "llrect.h"
#include "lluictrl.h"
#include "wolfadventure.h"
#include "wolffloaterroleplay.h"

namespace WolfGameDraw { struct Palette; }

/**
 * [WOLF ADVENTURE P1 2026-10-16] HUD_MESSAGES_V1.md §3. Everything is drawn with the wolfGame kit
 * (wolfgamehud.h WolfGameDraw) in the game's theme, as VIEWER_SPEC.md §3 asks: custom-drawn plates,
 * Cinzel titles, no emoji, text at 4.5:1 or better, colour never alone.
 */
namespace WolfAdventureUI
{
    /**
     * A window the once-a-frame syncs drive (syncCards, syncTalk, syncPlay), held by its handle so a frame does
     * not search the floater registry by name. Only while it is not held (not built yet, or destroyed) is it
     * looked for, at most once a second: the syncs build it themselves (showTypedInstance) and hold it then.
     * The registry names one class per name (llviewerfloaterreg.cpp), so the caller casts to it.
     */
    struct HeldFloater
    {
        explicit HeldFloater(const char* name) : mName(name) {}
        LLFloater* get();
        void hold(LLFloater* f) { if (f) mHandle = f->getHandle(); }
        const char* mName;
        LLHandle<LLFloater> mHandle;
        F64 mLookedAt = -100.0;
    };

    /**
     * [WOLF ADVENTURE AUDIT 2026-10-17] HUD_MESSAGES_V1.md §2.1 `look`: `base` dressed in the adventure's own panel and
     * accent colours (white or black text on the panel, whichever reads better; secondary text kept at 4.5:1 or more),
     * or `base` itself when the adventure has none. For the objective plate, the Journal header and the talk panel.
     */
    WolfGameDraw::Palette lookPalette(const WolfGameDraw::Palette& base, const std::string& adv);

    /** Shows or hides the card window to match WolfAdventure::topCard (WolfAdventure::idle). */
    void syncCards();
    /** Off Wolf, logout: the card and handout windows and the Studio close (§5.1). */
    void closeWindows();
    /** The player's own action brought a card back: show it and give it the keyboard. */
    void focusCard();
    /** Escape while the card window has the keyboard: the card is put away (§5.5). */
    bool escapeFocusedCard();
    /** §3.4: one of an adventure's handouts in the handout viewer. */
    void showHandout(const std::string& adv, const std::string& handout_id);

    // ---- [WOLF ADVENTURE P2 2026-10-16] the talk panel (HUD_MESSAGES_V1.md §3.2) ----
    /** Shows or hides the talk window to match WolfAdventure::talk (WolfAdventure::idle). */
    void syncTalk();
    /** The talk window as syncTalk holds it (the roll plate stands above it). */
    HeldFloater& talkWindow();
    /** Escape while the talk window has the keyboard: the box empties, else the talk ends (§3.2). */
    bool escapeFocusedTalk();
    /**
     * Where the talk panel stands, published by the game HUD each frame (wolfgamehud.cpp
     * drawAdventure), in SCREEN coordinates: `from_top` false - the panel stands on `y` with its
     * right edge at most `x` (left of the objective line); true - it hangs from `y` with its right
     * edge at `x` (top right under the notices, when the bottom belongs to the flight deck or car).
     */
    void setTalkAnchor(bool from_top, S32 x, S32 y);
    /** [WOLF ADVENTURE P4 2026-10-16] The anchor setTalkAnchor last stored (screen coordinates);
     *  false before the HUD first published one. */
    bool talkAnchor(bool& from_top, S32& x, S32& y);

    // ---- [WOLF ADVENTURE P4 2026-10-16] wolfadventureplay.cpp: rolls and dice (§3.9), the turn tracker
    // (§3.10), the quiz (§3.11), and the GM Console's world overlays (§2.7) ----
    /** Shows or hides the roll, turns and quiz windows to match WolfAdventure (WolfAdventure::idle). */
    void syncPlay();
    /** What the GM Console wants drawn in the world while it is open (STUDIO §3 Overlays). */
    struct GmOverlay
    {
        bool mShowZones = false, mShowPaths = false, mShowNpcs = false;
        std::string mAdv;
        LLUUID mRegion;                                   // the region the places are on
        std::vector<WolfAdventure::GmArea> mAreas;        // the pack's areas there (when no gm layer came)
        std::vector<WolfAdventure::GmPath> mPaths;
        std::vector<WolfAdventure::GmNpc> mNpcs;          // from gm_state (when no gm layer came)
    };
    /** When what the GM Console shows changes; keepGmOverlay on the frames between (it stops when the console
     *  closes: §2.7 "only while open"). */
    void setGmOverlay(const GmOverlay& o);
    /** The frames between: the GM Console is still open and the overlay set last is still right. */
    void keepGmOverlay();
    /** Over the world, under the HUD (wolfgamehud.cpp draw, after the markers). */
    void drawGmOverlay(LLView* hud, const LLRect& r);
    /**
     * The talk panel's face (shared by the panel and the Studio's "Talk to them now", STUDIO §2.3
     * "using the real talk UI"): portrait with the mood rim and word, name and title, the AI tag
     * and speaker glyph, the notice, "You: ...", the line (emote in italics) or "<name> is
     * thinking...", sources, the hint, an error. From `top` down between `l` and `r`; returns the
     * bottom. `right_reserve` keeps the header clear of a close X. With `draw` false it measures.
     */
    F32 talkFace(const WolfAdventure::Talk& t, F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p, bool draw, F32 right_reserve = 0.f);
    /** A mood band's rim colour (§3.2 "the mood, as a word and a rim colour"). */
    LLColor4 moodColour(const std::string& band, const WolfGameDraw::Palette& p);

    /**
     * §3.6: the letterbox bars, the fade and the chapter title, over the world and under the HUD
     * (WolfGameHUD::draw calls this right after the screen effects). `title_rect` gets the title
     * card's rectangle (a click on it skips it, §2.6), or an empty one.
     */
    void drawStage(const LLRect& r, LLRect& title_rect);
    /**
     * §3.1 the objective line: a plate `width` wide whose right edge is `right`, standing on
     * `y` (from_top false) or hanging from it (from_top true). Returns false when there is no
     * adventure to show; `out` is its rectangle (a click opens the Journal).
     */
    bool drawObjective(F32 right, F32 y, bool from_top, LLRect& out);
    /** The objective line's tooltip. */
    std::string objectiveTip();

    /**
     * §3.5: a reputation bar from min to max - band segments in their colours with each band's
     * label inside it on a pill, a marker at the value with its number - and, when `big`, the
     * "Honour: 15 · Drifter" line under it. Returns the bottom.
     */
    F32 meterBar(const WolfAdventure::Meter& m, F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p, bool big);
    /** §3.5: a character's opinion as a word (Hostile / Wary / Neutral / Friendly / Trusted). */
    std::string opinionWord(S32 v);
    /** The band a value falls in, by the bands list when there is one, else the meter's own. */
    std::string bandWord(const WolfAdventure::Meter& m);
}

/**
 * §3.3 a vignette card's drawn face: the top card of the stack, with up to two older ones peeking
 * above it. Options one per row (32 high, `detail` as a second line), the tally next to each in a
 * vote, a timer as a bar plus seconds in words, "More time", "Chosen: <label>". Tab / Shift-Tab and
 * the arrow keys move between the buttons, Enter or Space presses, Escape puts the card away.
 */
class WolfAdventureCardView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureCardView(const Params& p);

    /** The height the top card needs at `width` (0 when there is no card). */
    S32 measure(S32 width) const;
    void takeKeyboard();

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;

private:
    F32 layout(F32 W, F32 H, bool draw) const;
    void press(S32 id, S32 arg);
    mutable WolfGameHits mHits;
    mutable U32 mMeasuredGen = 0;
    mutable S32 mMeasuredWidth = 0, mMeasured = 0;
    bool mKeyboardNav = false;
    std::string mCardId;           // the card the focus belongs to: a new card starts on its first button
};

class WolfFloaterAdventureCard : public LLFloater
{
public:
    WolfFloaterAdventureCard(const LLSD& key);
    bool postBuild() override;
    void draw() override;
    /** Size to the content and stand centred in the upper third (§3.3). */
    void fit(bool place);
    void takeKeyboard();

private:
    WolfAdventureCardView* mView = nullptr;
    S32 mLastHeight = 0;
    std::string mPlacedFor;        // the card id the window was last placed for
};

/**
 * [WOLF ADVENTURE P2 2026-10-16] §3.2 the talk panel's drawn face (Mass Effect / Witcher dialogue
 * panels): the speaker's portrait, the line, then numbered replies in two columns, a text box
 * when `free`, Give when `give`, and the X. Tab / Shift-Tab cycle, Enter activates, 1-6 pick a
 * reply while the box is not being typed in, Escape empties the box or ends the talk.
 */
class WolfAdventureTalkView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureTalkView(const Params& p);

    /** The height the panel needs at `width`. */
    S32 measure(S32 width) const;
    /** Escape: true when it was taken (the box emptied, or the talk ended). */
    bool escape();

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;
    void onFocusLost() override;
    /** While the box has the keyboard the panel is a text field: the action-bar keys 1-9, 0, -, =
     *  do not fire (wolfgame.cpp handleActionKey), and Escape reaches escape() first. */
    bool acceptsTextInput() const override { return mBoxFocus && hasFocus(); }

private:
    F32 layout(F32 W, F32 H, bool draw) const;
    void press(S32 id, S32 arg);
    void send();
    mutable WolfGameHits mHits;
    mutable U32 mMeasuredSeq = 0;
    mutable S32 mMeasuredWidth = 0, mMeasured = 0;
    bool mKeyboardNav = false;
    bool mBoxFocus = false;
    std::string mBox;                       // the free text being typed (at most 720 bytes, §1.2)
    LLUUID mBoxNpc;                         // the character the box was typed to (cleared on a new talk)
    bool mGiveOpen = false;                 // the bag picker under the panel
    std::string mGiveItem;
    S32 mGiveQty = 1;
    mutable std::vector<std::string> mGiveItems;   // the bag picker's rows, by hit arg
};

class WolfFloaterAdventureTalk : public LLFloater
{
public:
    WolfFloaterAdventureTalk(const LLSD& key);
    bool postBuild() override;
    void draw() override;
    void onClose(bool app_quitting) override;
    /** Size to the content and stand where the HUD says (§3.2 Where). */
    void fit();
    bool escape() { return mView && mView->escape(); }
    bool viewHasFocus() const;

private:
    WolfAdventureTalkView* mView = nullptr;
};

/** §3.4 the handout viewer's drawn face: the picture as large as fits, the title, the text. */
class WolfAdventureHandoutView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureHandoutView(const Params& p);
    void setHandout(const std::string& adv, const std::string& id);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    void onMouseCaptureLost() override;

private:
    void press(S32 id);
    std::string mAdv, mId;
    WolfGameHits mHits;
    bool mKeyboardNav = false;
    F32 mScroll = 0.f;
    F32 mTextOverflow = 0.f;
};

class WolfFloaterAdventureHandout : public LLFloater
{
public:
    WolfFloaterAdventureHandout(const LLSD& key);
    bool postBuild() override;
    void draw() override;
    void show(const std::string& adv, const std::string& id);

private:
    WolfAdventureHandoutView* mView = nullptr;
};

#endif // WOLF_ADVENTURE_HUD_H
