/**
 * @file wolfadventureplay.h
 * @brief WolfViewer: Wolf Adventure, the player's rolls and dice, the turn tracker and the quiz.
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

#ifndef WOLF_ADVENTURE_PLAY_H
#define WOLF_ADVENTURE_PLAY_H

#include <string>

#include "llfloater.h"
#include "lluictrl.h"
#include "wolffloaterroleplay.h"

namespace WolfGameDraw { struct Palette; }

/**
 * [WOLF ADVENTURE P4 2026-10-16] HUD_MESSAGES_V1.md §3.9 (the roll prompt and the dice card), §3.10 (the
 * turn tracker) and §3.11 (the quiz), drawn with the wolfGame kit as every adventure plate (§3: custom-drawn
 * plates, Cinzel titles, no emoji, 4.5:1 text, colour never alone). One view class with three faces, one per
 * window (wolf_adventure_roll, wolf_adventure_turns, wolf_adventure_quiz); none of them is modal and none
 * takes the keyboard by itself (as the cards: a player's chat and movement keys are never taken from them).
 */
class WolfAdventurePlayView : public LLUICtrl
{
public:
    enum EMode { MODE_ROLL = 0, MODE_TURNS, MODE_QUIZ };
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventurePlayView(const Params& p);
    void setMode(EMode m) { mMode = m; mLaidOutWidth = -1; }
    EMode mode() const { return mMode; }

    /** The size the face needs at `width` (0 = nothing to show). */
    S32 measure(S32 width);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;
    /** The quiz's text box takes typed keys while it has the keyboard. */
    bool acceptsTextInput() const override { return mMode == MODE_QUIZ && mFieldFocus && hasFocus(); }

private:
    F32 layout(F32 W, F32 H, bool draw);
    F32 faceRoll(F32 W, F32 H, bool draw, const WolfGameDraw::Palette& p);
    F32 faceTurns(F32 W, F32 H, bool draw, const WolfGameDraw::Palette& p);
    F32 faceQuiz(F32 W, F32 H, bool draw, const WolfGameDraw::Palette& p);
    void press(S32 id, S32 arg);
    U32 flags(S32 id, S32 arg, bool enabled) const;
    void addHit(bool draw, F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool enabled = true);

    EMode mMode = MODE_ROLL;
    WolfGameHits mHits;
    bool mKeyboardNav = false;
    bool mFieldFocus = false;           // the quiz's text answer has the keyboard
    bool mPickTarget = false;           // [WOLF STORY FIRST M4 2026-10-17] Attack pressed: choosing whom to attack
    std::string mFieldItem;             // the item the text box belongs to
    U32 mDiceSeq = 0;                   // the dice card the tumble is for
    F64 mTumbleStart = 0.0;
    // the last draw's layout, for measure (fit runs every frame before the draw)
    S32 mLaidOutWidth = -1;
    S32 mLaidOutHeight = 0;
    U32 mLaidOutGen = 0;
};

class WolfFloaterAdventurePlay : public LLFloater
{
public:
    WolfFloaterAdventurePlay(const LLSD& key);
    bool postBuild() override;
    void draw() override;
    void onClose(bool app_quitting) override;
    /** Size to the content and stand where §3.9-3.11 put it. */
    void fit();

private:
    WolfAdventurePlayView* mView = nullptr;
};

#endif // WOLF_ADVENTURE_PLAY_H
