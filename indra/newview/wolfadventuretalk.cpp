/**
 * @file wolfadventuretalk.cpp
 * @brief WolfViewer: Wolf Adventure's talk panel - a conversation with a story character
 *        (HUD_MESSAGES_V1.md §2.4 `talk`, §3.2 the talk panel, §4 the verbs). See wolfadventurehud.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfadventurehud.h"

#include <cmath>

#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llrender.h"
#include "lltooltip.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureTalkView> r_wolf_adventure_talk_view("wolf_adventure_talk_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // The same inks as the Roleplay window and the Studio: 4.5:1 or better on the game's cards.
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24

    // Source: HUD_MESSAGES_V1.md §3.2 "about 46% of the window wide"; the panel never shrinks
    // below what two reply columns need, nor grows past a readable line (~75 characters).
    const F32 WIDTH_FRACTION = 0.46f;
    const S32 MIN_W = 380, MAX_W = 760;
    const F32 PAD = 12.f, PORTRAIT = 64.f;
    // Source: HUD_MESSAGES_V1.md §6 2.5.8 "reply and option buttons 32 high".
    const F32 REPLY_H = 32.f;
    // the panel's line, at most this many wrapped lines (the 1000-character cap, §1.3)
    const S32 MAX_LINE_ROWS = 14;

    enum ETalkHit { TH_CLOSE = 1, TH_REPLY, TH_FIELD, TH_SEND, TH_GIVE, TH_GIVE_ITEM, TH_QTY_LESS, TH_QTY_MORE, TH_GIVE_OK, TH_GIVE_CANCEL, TH_PLATE };

    // The talk panel's anchor, from the game HUD (screen coordinates; see setTalkAnchor).
    bool sAnchorSet = false, sAnchorTop = false;
    S32 sAnchorX = 0, sAnchorY = 0;

    std::string initials(const std::string& s)
    {
        LLWString w = utf8str_to_wstring(s);
        LLWString out;
        bool start = true;
        for (llwchar c : w)
        {
            if (c == ' ') { start = true; continue; }
            if (start) out.push_back(c);
            start = false;
            if (out.size() >= 2) break;
        }
        return wstring_to_utf8str(out);
    }

    const LLFontGL* italicFont()
    {
        // Source: llfloatercamerapresets.cpp - a font's descriptor with its style changed, through
        // LLFontGL::getFont; the plain face when the italic one is missing.
        static const LLFontGL* f = nullptr;
        if (!f)
        {
            const LLFontGL* base = LLFontGL::getFontSansSerif();
            LLFontDescriptor d(base->getFontDesc());
            d.setStyle(LLFontGL::ITALIC);
            const LLFontGL* it = LLFontGL::getFont(d);
            f = it ? it : base;
        }
        return f;
    }

    /** A loudspeaker: the speaker glyph of §3.2 ("a speaker glyph while voiced"). */
    void speakerGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        rect(cx - s * 0.5f, cy - s * 0.18f, cx - s * 0.22f, cy + s * 0.18f, c);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(c.mV);
        gGL.vertex2f(cx - s * 0.22f, cy - s * 0.18f); gGL.vertex2f(cx + s * 0.08f, cy - s * 0.45f); gGL.vertex2f(cx + s * 0.08f, cy + s * 0.45f);
        gGL.vertex2f(cx - s * 0.22f, cy - s * 0.18f); gGL.vertex2f(cx + s * 0.08f, cy + s * 0.45f); gGL.vertex2f(cx - s * 0.22f, cy + s * 0.18f);
        gGL.end();
        arc(cx + s * 0.1f, cy, s * 0.25f, 1.5f, -0.9f, 0.9f, c, 12);
        arc(cx + s * 0.1f, cy, s * 0.45f, 1.5f, -0.9f, 0.9f, c, 16);
    }

    /** The warning triangle beside an error (an icon AND the words: colour never alone). */
    void warnGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(c.mV);
        gGL.vertex2f(cx - s * 0.5f, cy - s * 0.42f); gGL.vertex2f(cx + s * 0.5f, cy - s * 0.42f); gGL.vertex2f(cx, cy + s * 0.48f);
        gGL.end();
    }

    S32 worldW() { return gViewerWindow ? gViewerWindow->getWorldViewWidthScaled() : 1024; }
    S32 worldH() { return gViewerWindow ? gViewerWindow->getWorldViewHeightScaled() : 768; }

    /** Bytes left in the box (§1.2: 720 bytes in all), the counter's number. */
    S32 bytesLeft(const std::string& box) { return (S32)WolfAdventure::MAX_TEXT_BYTES - (S32)box.size(); }

    /** The held bag items (WolfGame::items with a count), for Give. */
    std::vector<const WolfGame::Item*> heldItems()
    {
        std::vector<const WolfGame::Item*> out;
        for (const WolfGame::Item& it : WolfGame::instance().items())
        {
            // BRAIN_API_V1.md §6.2 give: item names the brain takes are [a-z0-9_]{1,24}
            if (it.mQty > 0 && !it.mName.empty() && it.mName.size() <= 24) out.push_back(&it);
        }
        return out;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// windows
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfAdventureUI
{
    bool talkAnchor(bool& from_top, S32& x, S32& y)
    {
        if (!sAnchorSet) return false;
        from_top = sAnchorTop;
        x = sAnchorX;
        y = sAnchorY;
        return true;
    }

    void setTalkAnchor(bool from_top, S32 x, S32 y)
    {
        sAnchorSet = true;
        sAnchorTop = from_top;
        sAnchorX = x;
        sAnchorY = y;
    }

    HeldFloater& talkWindow()
    {
        static HeldFloater held("wolf_adventure_talk");
        return held;
    }

    void syncTalk()
    {
        HeldFloater& held = talkWindow();
        const WolfAdventure::Talk& t = WolfAdventure::instance().talk();
        WolfFloaterAdventureTalk* f = static_cast<WolfFloaterAdventureTalk*>(held.get());
        if (!t.mOn || !WolfGrid::isOnWolfTerritories())
        {
            if (f && f->getVisible()) f->closeFloater();
            return;
        }
        if (!f || !f->getVisible())
        {
            // never takes the keyboard by itself, as game dialogs and cards (wolfadventurehud.cpp
            // syncCards): the player's chat or movement keys are not taken from them
            f = LLFloaterReg::showTypedInstance<WolfFloaterAdventureTalk>("wolf_adventure_talk", LLSD(), false);
            held.hold(f);
            if (f) f->fit();
        }
    }

    bool escapeFocusedTalk()
    {
        WolfFloaterAdventureTalk* f = LLFloaterReg::findTypedInstance<WolfFloaterAdventureTalk>("wolf_adventure_talk");
        if (!f || !f->getVisible() || !f->viewHasFocus()) return false;
        return f->escape();
    }

    LLColor4 moodColour(const std::string& band, const Palette& p)
    {
        // Source: HUD_MESSAGES_V1.md §2.4 mood.band and the §3.5 bands (Hostile / Wary / Neutral /
        // Friendly / Trusted). Rim colours (non-text, 3:1 against the plate, §6 1.4.11); the word is
        // always printed beside the portrait, so the colour never carries the mood alone.
        if (band == "hostile") return LLColor4(0.937f, 0.267f, 0.267f, 1.f);   // #ef4444
        if (band == "wary") return LLColor4(0.984f, 0.573f, 0.235f, 1.f);      // #fb923c
        if (band == "friendly") return LLColor4(0.290f, 0.871f, 0.502f, 1.f);  // #4ade80
        if (band == "trusted") return LLColor4(0.376f, 0.647f, 0.980f, 1.f);   // #60a5fa
        return p.mDim;
    }

    F32 talkFace(const WolfAdventure::Talk& t, F32 l, F32 r, F32 top, const Palette& p, bool draw, F32 right_reserve)
    {
        const LLFontGL* body = LLFontGL::getFontSansSerif();
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        const LLFontGL* name_font = display(18.f);
        const F32 lh = (F32)body->getLineHeight(), slh = (F32)small->getLineHeight(), nlh = (F32)name_font->getLineHeight();
        const F32 tl = l + PORTRAIT + 12.f;   // the text column, right of the portrait
        const F32 tw = r - tl;
        F32 y = top;

        // ---- the header: name and title; the AI tag and the speaker glyph on the right ----
        F32 tags_w = 0.f;
        // [WOLF ADVENTURE P4 2026-10-16] §5.6 "Show AI tag" (never hidden in class play)
        const bool ai = t.mHasLine && t.mAi && !t.mTyping && WolfAdventure::instance().aiTagShown(t.mAdv);
        if (ai) tags_w += bold->getWidthF32("AI") + 14.f + 6.f;
        if (t.mVoiced && !t.mTyping) tags_w += 22.f;
        if (draw)
        {
            const std::string name = t.mName.empty() ? std::string("Story character") : t.mName;
            textL(name_font, name, tl, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, tw - tags_w - right_reserve - 6.f);
            F32 x = r - right_reserve;
            if (t.mVoiced && !t.mTyping)
            {
                speakerGlyph(x - 10.f, y - nlh * 0.5f, 16.f, p.mAccent);
                x -= 22.f;
            }
            if (ai)
            {
                // §2.4 "a small AI tag in the corner, so players know what they are talking to"
                const F32 w = bold->getWidthF32("AI") + 14.f;
                chamfer(x - w, y - nlh * 0.5f - slh * 0.5f - 2.f, x, y - nlh * 0.5f + slh * 0.5f + 2.f, 3.f, p.mAccent, p.mAccent);
                textL(bold, "AI", x - w * 0.5f, y - nlh * 0.5f, p.mOnAccent, LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
        }
        y -= nlh;
        if (!t.mTitle.empty())
        {
            if (draw) textL(small, t.mTitle, tl, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, tw);
            y -= slh;
        }
        y -= 6.f;

        // ---- the portrait, on the left, with the mood rim and word under it ----
        if (draw)
        {
            const F32 pt = top, pb = top - PORTRAIT;
            icon(t.mPortrait, l, pb, PORTRAIT, p, initials(t.mName.empty() ? std::string("?") : t.mName));
            if (t.mHasMood)
            {
                const LLColor4 rim = moodColour(t.mMoodBand, p);
                chamferEdge(l - 2.f, pb - 2.f, l + PORTRAIT + 2.f, pt + 2.f, 6.f, 2.f, rim);
                const std::string word = !t.mMoodLabel.empty() ? t.mMoodLabel : t.mMoodBand;
                if (!word.empty())
                {
                    disc(l + 5.f, pb - 4.f - slh * 0.5f, 3.5f, rim, 12);
                    textL(small, word, l + 12.f, pb - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, PORTRAIT + 10.f);
                }
            }
        }
        F32 portrait_bottom = top - PORTRAIT - (t.mHasMood ? slh + 6.f : 0.f);

        // ---- the notice (first talk only, §2.4) ----
        if (!t.mNotice.empty())
        {
            for (const std::string& line : wrap(small, t.mNotice, tw, 4))
            {
                if (draw) textL(small, line, tl, y, p.mDim);
                y -= slh;
            }
            y -= 4.f;
        }

        // ---- what the player said last ----
        if (!t.mYouSaid.empty())
        {
            const std::vector<std::string> said = wrap(small, "You: " + t.mYouSaid, tw, 3);
            for (const std::string& line : said)
            {
                if (draw) textL(small, line, tl, y, p.mDim);
                y -= slh;
            }
            y -= 3.f;
        }

        // ---- the line, or thinking..., or waiting ----
        if (t.mTyping || t.mWaiting)
        {
            // §3.2 "<name> is thinking..." (three dots plus the words); the dots step, never flash
            const S32 dots = 1 + (S32)fmod(now() * 2.0, 3.0);
            const std::string who = t.mName.empty() ? std::string("They") : t.mName;
            const std::string words = t.mTyping ? who + " is thinking" : "Waiting for " + who;
            if (draw) textL(body, words + std::string((size_t)dots, '.'), tl, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, tw);
            y -= lh;
        }
        else if (t.mHasLine)
        {
            if (!t.mEmote.empty())
            {
                // §3.2 "emote text in italics"
                for (const std::string& line : wrap(italicFont(), t.mEmote, tw, 3))
                {
                    if (draw) textL(italicFont(), line, tl, y, p.mDim);
                    y -= (F32)italicFont()->getLineHeight();
                }
            }
            for (const std::string& line : wrap(body, t.mText, tw, MAX_LINE_ROWS))
            {
                if (draw) textL(body, line, tl, y, p.mText);
                y -= lh;
            }
            if (!t.mCites.empty())
            {
                // §2.4 cites: "Source: ..." under the line, in small type (accuracy mode)
                y -= 2.f;
                for (const WolfAdventure::TalkCite& c : t.mCites)
                {
                    const std::string s = "Source: " + c.mTitle + (c.mWhere.empty() ? std::string() : ", " + c.mWhere);
                    for (const std::string& line : wrap(small, s, tw, 2))
                    {
                        if (draw) textL(small, line, tl, y, p.mDim);
                        y -= slh;
                    }
                }
            }
        }
        else if (t.mError.empty())
        {
            if (draw) textL(body, "...", tl, y, p.mDim);
            y -= lh;
        }
        if (!t.mHint.empty())
        {
            y -= 3.f;
            for (const std::string& line : wrap(bold, "Hint: " + t.mHint, tw, 3))
            {
                if (draw) textL(bold, line, tl, y, p.mAccent);
                y -= slh;
            }
        }
        if (!t.mError.empty())
        {
            // an action that could not go: an icon and the words, kept until the next action
            y -= 3.f;
            const std::vector<std::string> err = wrap(small, t.mError, tw - 18.f, 3);
            if (draw) warnGlyph(tl + 6.f, y - slh * 0.5f, 11.f, WARN_INK);
            for (const std::string& line : err)
            {
                if (draw) textL(small, line, tl + 18.f, y, DANGER_INK);
                y -= slh;
            }
        }
        return llmin(y, portrait_bottom) - 6.f;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the view
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventureTalkView::WolfAdventureTalkView(const Params& p)
:   LLUICtrl(p)
{
}

S32 WolfAdventureTalkView::measure(S32 width) const
{
    const WolfAdventure::Talk& t = WolfAdventure::instance().talk();
    // the box and the bag picker change the height too: measured every time they could have
    const U32 seq = t.mSeq * 4u + (mGiveOpen ? 1u : 0u) + (mBox.empty() ? 0u : 2u);
    if (seq != mMeasuredSeq || width != mMeasuredWidth)
    {
        mMeasuredSeq = seq;
        mMeasuredWidth = width;
        mMeasured = (S32)ceilf(layout((F32)width, 0.f, false));
    }
    return mMeasured;
}

F32 WolfAdventureTalkView::layout(F32 W, F32 H, bool draw) const
{
    const WolfAdventure::Talk& t = WolfAdventure::instance().talk();
    // [WOLF ADVENTURE AUDIT 2026-10-17] the adventure's own colours (HUD_MESSAGES_V1.md §2.1 look)
    const Palette p = WolfAdventureUI::lookPalette(gamePalette(), t.mAdv);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const F32 slh = (F32)small->getLineHeight();
    // measured from 0 down; drawn from H down (the same arithmetic)
    const F32 top = draw ? H : 0.f;
    if (draw)
    {
        mHits.clear();
        frame(0.f, 0.f, W, H, p, 1.f, 10.f);
        mHits.add(0.f, 0.f, W, H, TH_PLATE, 0, std::string(), false);
    }
    F32 y = top - PAD;
    // the source, small, and the X
    if (draw)
    {
        textL(bold, caps(t.mSrc.empty() ? std::string("Conversation") : t.mSrc), PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD - 30.f);
        const F32 xc = W - PAD - 4.f, yc = y - slh * 0.5f;
        closeX(xc, yc, 5.f, mHits.isHover(TH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(TH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
        mHits.add(xc - 12.f, yc - 12.f, xc + 12.f, yc + 12.f, TH_CLOSE, 0, "End the conversation (Escape)");
    }
    y -= slh + 6.f;
    y = WolfAdventureUI::talkFace(t, PAD, W - PAD, y, p, draw);

    // ---- the replies: up to 6 buttons in two columns (§3.2) ----
    const bool busy = t.mTyping || t.mWaiting;
    if (!t.mReplies.empty())
    {
        const F32 gap = 8.f, cw = (W - 2.f * PAD - gap) * 0.5f;
        const S32 n = (S32)t.mReplies.size();
        for (S32 i = 0; i < n; ++i)
        {
            const S32 col = i % 2, row = i / 2;
            const F32 bl = PAD + (F32)col * (cw + gap), bt = y - (F32)row * (REPLY_H + 6.f);
            if (draw)
            {
                U32 flags = busy ? BTN_DISABLED : 0;
                if (!busy && mHits.isHover(TH_REPLY, i)) flags |= BTN_HOVER;
                if (!busy && mHits.isPressed(TH_REPLY, i)) flags |= BTN_PRESSED;
                if (!busy && mKeyboardNav && hasFocus() && mHits.isFocused(TH_REPLY, i)) flags |= BTN_FOCUS;
                button(bl, bt - REPLY_H, bl + cw, bt, std::string(), p, flags);
                const F32 kx = textL(bold, llformat("%d.", i + 1), bl + 10.f, bt - REPLY_H * 0.5f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
                textL(body, t.mReplies[i].mLabel, kx + 6.f, bt - REPLY_H * 0.5f, busy ? p.mDim : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, bl + cw - kx - 14.f);
                mHits.add(bl, bt - REPLY_H, bl + cw, bt, TH_REPLY, i,
                          busy ? std::string("Wait for the answer") : "Say: " + t.mReplies[i].mLabel + llformat(" (key %d)", i + 1), true, !busy);
            }
        }
        y -= (F32)((n + 1) / 2) * (REPLY_H + 6.f) + 2.f;
    }

    // ---- the text box, Send, the counter; Give (§3.2) ----
    const F32 bh = 30.f;
    if (t.mFree || t.mGive)
    {
        const F32 give_w = t.mGive ? 76.f : 0.f;
        const F32 send_w = t.mFree ? 72.f : 0.f;
        if (t.mFree)
        {
            const F32 fl = PAD, fr = W - PAD - send_w - 6.f - (t.mGive ? give_w + 6.f : 0.f);
            if (draw)
            {
                const bool focused = hasFocus() && mBoxFocus;
                chamfer(fl, y - bh, fr, y, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
                chamferEdge(fl + 0.5f, y - bh + 0.5f, fr - 0.5f, y - 0.5f, 4.f, focused ? 2.f : 1.f, focused ? p.mAccent : p.mDim);
                const F32 cy = y - bh * 0.5f;
                F32 x = fl + 8.f;
                // the end of the text stays in view while typing (only what fits, from the right)
                std::string shown = mBox;
                const F32 room = fr - fl - 18.f;
                while (!shown.empty() && body->getWidthF32(shown) > room)
                {
                    LLWString w = utf8str_to_wstring(shown);
                    w.erase(0, 1);
                    shown = wstring_to_utf8str(w);
                }
                if (mBox.empty()) textL(body, "Say something...", x, cy, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, room);
                else x = textL(body, shown, x, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
                if (focused && fmod(now(), 1.0) < 0.6) rect(x + 1.f, cy - (F32)body->getLineHeight() * 0.45f, x + 2.f, cy + (F32)body->getLineHeight() * 0.45f, p.mText);
                mHits.add(fl, y - bh, fr, y, TH_FIELD, 0, "Type what you say to " + (t.mName.empty() ? std::string("them") : t.mName) + ", then Enter");
                const F64 wait = WolfAdventure::instance().talkSayWait();
                const bool can = !busy && !mBox.empty() && wait <= 0.0;
                U32 flags = can ? 0 : BTN_DISABLED;
                if (can && mHits.isHover(TH_SEND, 0)) flags |= BTN_HOVER;
                if (can && mHits.isPressed(TH_SEND, 0)) flags |= BTN_PRESSED;
                if (can && mKeyboardNav && hasFocus() && mHits.isFocused(TH_SEND, 0)) flags |= BTN_FOCUS;
                button(fr + 6.f, y - bh, fr + 6.f + send_w, y, "Say", p, flags | (can ? BTN_PRIMARY : 0));
                mHits.add(fr + 6.f, y - bh, fr + 6.f + send_w, y, TH_SEND, 0, busy ? std::string("Wait for the answer") : std::string("Say it (Enter)"), true, can);
            }
        }
        if (t.mGive && draw)
        {
            const F32 gl = W - PAD - give_w;
            U32 flags = busy ? BTN_DISABLED : 0;
            if (!busy && mHits.isHover(TH_GIVE, 0)) flags |= BTN_HOVER;
            if (!busy && mHits.isPressed(TH_GIVE, 0)) flags |= BTN_PRESSED;
            if (!busy && mKeyboardNav && hasFocus() && mHits.isFocused(TH_GIVE, 0)) flags |= BTN_FOCUS;
            button(gl, y - bh, W - PAD, y, mGiveOpen ? "Give..." : "Give", p, flags | (mGiveOpen ? BTN_ACTIVE : 0));
            mHits.add(gl, y - bh, W - PAD, y, TH_GIVE, 0, "Give them something from your bag", true, !busy);
        }
        y -= bh + 4.f;
        if (t.mFree)
        {
            // §3.2 "a counter that stops at 720 bytes, shown as characters left"
            const S32 left = bytesLeft(mBox);
            if (draw) textL(small, llformat("%d characters left", left), PAD, y, left < 40 ? WARN_INK : p.mDim, LLFontGL::LEFT, LLFontGL::TOP);
            y -= slh + 2.f;
        }
    }

    // ---- the bag picker (Give) ----
    if (mGiveOpen && t.mGive)
    {
        const std::vector<const WolfGame::Item*> held = heldItems();
        if (draw) mGiveItems.clear();
        if (held.empty())
        {
            const std::string none = WolfGame::instance().items().empty()
                ? "Your bag is empty. (Items are kept while Game Mode is on.)"
                : "Nothing in your bag can be given.";
            if (draw) textL(small, none, PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
            y -= slh + 6.f;
        }
        else
        {
            F32 x = PAD;
            for (size_t i = 0; i < held.size() && i < 12; ++i)
            {
                const WolfGame::Item& it = *held[i];
                const std::string label = (it.mLabel.empty() ? it.mName : it.mLabel) + llformat(" (%lld)", (long long)it.mQty);
                const F32 w = llmin(220.f, bold->getWidthF32(label) + 22.f);
                if (x > PAD && x + w > W - PAD) { x = PAD; y -= 30.f; }
                if (draw)
                {
                    const bool on = mGiveItem == it.mName;
                    U32 flags = on ? BTN_ACTIVE : 0;
                    if (mHits.isHover(TH_GIVE_ITEM, (S32)i)) flags |= BTN_HOVER;
                    if (mKeyboardNav && hasFocus() && mHits.isFocused(TH_GIVE_ITEM, (S32)i)) flags |= BTN_FOCUS;
                    button(x, y - 24.f, x + w, y, label, p, flags);
                    mHits.add(x, y - 24.f, x + w, y, TH_GIVE_ITEM, (S32)i, "Give " + (it.mLabel.empty() ? it.mName : it.mLabel) + (on ? " (chosen)" : ""));
                    mGiveItems.push_back(it.mName);
                }
                x += w + 6.f;
            }
            y -= 32.f;
            if (draw)
            {
                S64 have = 0;
                for (const WolfGame::Item* it : held) if (it->mName == mGiveItem) have = it->mQty;
                const bool chosen = !mGiveItem.empty() && have > 0;
                textL(small, chosen ? llformat("How many: %d", mGiveQty) : std::string("Choose what to give."), PAD, y - 12.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
                const F32 sx = PAD + 120.f;
                button(sx, y - 24.f, sx + 30.f, y, "-", p, (chosen && mGiveQty > 1 ? 0 : BTN_DISABLED) | (mHits.isHover(TH_QTY_LESS, 0) ? BTN_HOVER : 0));
                mHits.add(sx, y - 24.f, sx + 30.f, y, TH_QTY_LESS, 0, "One fewer", true, chosen && mGiveQty > 1);
                button(sx + 36.f, y - 24.f, sx + 66.f, y, "+", p, (chosen && mGiveQty < have ? 0 : BTN_DISABLED) | (mHits.isHover(TH_QTY_MORE, 0) ? BTN_HOVER : 0));
                mHits.add(sx + 36.f, y - 24.f, sx + 66.f, y, TH_QTY_MORE, 0, "One more", true, chosen && mGiveQty < have);
                const bool can = chosen && !busy;
                button(W - PAD - 190.f, y - 26.f, W - PAD - 100.f, y, "Give", p, (can ? BTN_PRIMARY : BTN_DISABLED) | (mHits.isHover(TH_GIVE_OK, 0) ? BTN_HOVER : 0));
                mHits.add(W - PAD - 190.f, y - 26.f, W - PAD - 100.f, y, TH_GIVE_OK, 0, "Give it", true, can);
                button(W - PAD - 90.f, y - 26.f, W - PAD, y, "Cancel", p, mHits.isHover(TH_GIVE_CANCEL, 0) ? BTN_HOVER : 0);
                mHits.add(W - PAD - 90.f, y - 26.f, W - PAD, y, TH_GIVE_CANCEL, 0, "Close the bag");
            }
            y -= 32.f;
        }
    }
    y -= PAD - 4.f;
    return top - y;
}

void WolfAdventureTalkView::draw()
{
    const WolfAdventure::Talk& t = WolfAdventure::instance().talk();
    if (t.mNpc != mBoxNpc)
    {
        // a new conversation: the box, the bag and the focus start again
        mBoxNpc = t.mNpc;
        mBox.clear();
        mGiveOpen = false;
        mGiveItem.clear();
        mGiveQty = 1;
        mBoxFocus = false;
        mHits.setFocus(t.mReplies.empty() ? (t.mFree ? (S32)TH_FIELD : (S32)TH_CLOSE) : (S32)TH_REPLY, 0);
    }
    LLGLSUIDefault gls_ui;
    layout((F32)getRect().getWidth(), (F32)getRect().getHeight(), true);
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureTalkView::send()
{
    if (mBox.empty()) return;
    if (WolfAdventure::instance().talkSay(mBox)) mBox.clear();
}

bool WolfAdventureTalkView::escape()
{
    if (mBoxFocus && !mBox.empty())
    {
        mBox.clear();   // §3.2 Escape closes the panel "when no text is in the box": first it empties it
        return true;
    }
    if (mGiveOpen)
    {
        mGiveOpen = false;
        return true;
    }
    WolfAdventure::instance().talkClose();
    return true;
}

void WolfAdventureTalkView::press(S32 id, S32 arg)
{
    WolfAdventure& wa = WolfAdventure::instance();
    if (id != TH_FIELD) mBoxFocus = false;
    switch (id)
    {
    case TH_CLOSE: wa.talkClose(); break;
    case TH_REPLY: wa.talkReply(arg); break;
    case TH_FIELD:
        mBoxFocus = true;
        mHits.setFocus(TH_FIELD, 0);
        setFocus(true);
        break;
    case TH_SEND: send(); break;
    case TH_GIVE:
        mGiveOpen = !mGiveOpen;
        if (mGiveOpen) { mGiveItem.clear(); mGiveQty = 1; }
        break;
    case TH_GIVE_ITEM:
        if (arg >= 0 && arg < (S32)mGiveItems.size())
        {
            mGiveItem = mGiveItems[arg];
            mGiveQty = 1;
        }
        break;
    case TH_QTY_LESS: mGiveQty = llmax(1, mGiveQty - 1); break;
    case TH_QTY_MORE:
    {
        S64 have = 0;
        for (const WolfGame::Item* it : heldItems()) if (it->mName == mGiveItem) have = it->mQty;
        // never below 1, also when the item has just gone from the inventory (have 0): talkGive sends this
        mGiveQty = (S32)llmax<S64>(1, llmin<S64>(have, (S64)mGiveQty + 1));
        break;
    }
    case TH_GIVE_OK:
        if (!mGiveItem.empty() && wa.talkGive(mGiveItem, mGiveQty))
        {
            mGiveOpen = false;
            mGiveItem.clear();
            mGiveQty = 1;
        }
        break;
    case TH_GIVE_CANCEL: mGiveOpen = false; break;
    default: break;
    }
}

bool WolfAdventureTalkView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;
    if (h->mId == TH_PLATE) return true;    // the plate itself: a click on it is the panel's, not the world's
    mKeyboardNav = false;
    setFocus(true);
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAdventureTalkView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    // read what was pressed before the release clears it (wolffloaterroleplay.cpp's [FIX 2026-10-09])
    const S32 id = mHits.pressedId(), arg = mHits.pressedArg();
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == id && h->mArg == arg && h->mEnabled) press(id, arg);
    return true;
}

void WolfAdventureTalkView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
}

void WolfAdventureTalkView::onFocusLost()
{
    mBoxFocus = false;
    LLUICtrl::onFocusLost();
}

bool WolfAdventureTalkView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == TH_PLATE) h = nullptr;
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(h->mId == TH_FIELD ? UI_CURSOR_IBEAM : (h->mEnabled ? UI_CURSOR_HAND : UI_CURSOR_ARROW));
    return true;
}

bool WolfAdventureTalkView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfAdventureTalkView::handleKeyHere(KEY key, MASK mask)
{
    if (mBoxFocus)
    {
        if (key == KEY_BACKSPACE && mask == MASK_NONE)
        {
            LLWString w = utf8str_to_wstring(mBox);
            if (!w.empty()) w.pop_back();
            mBox = wstring_to_utf8str(w);
            return true;
        }
        if (key == KEY_RETURN && mask == MASK_NONE)
        {
            send();
            return true;
        }
        if (key == 'V' && (mask & MASK_CONTROL))
        {
            LLWString pasted;
            if (LLClipboard::instance().pasteFromClipboard(pasted))
            {
                std::string add = wstring_to_utf8str(pasted);
                for (char& c : add) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                mBox = WolfAdventure::cutUtf8(mBox + add, WolfAdventure::MAX_TEXT_BYTES);
            }
            return true;
        }
        // printable keys are the text's (they arrive as characters); the digit keys must not
        // reach the action bar while typing (§3.2 Keyboard)
        if (mask == MASK_NONE || mask == MASK_SHIFT)
        {
            if (key != KEY_TAB && key != KEY_ESCAPE && key < KEY_SPECIAL) return true;
        }
    }
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        const WolfGameHits::Hit* f = mHits.focused();
        mBoxFocus = f && f->mId == TH_FIELD;
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_ESCAPE:
        escape();
        return true;
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled)
        {
            mKeyboardNav = true;
            press(f->mId, f->mArg);
        }
        return true;
    }
    case KEY_LEFT:
    case KEY_UP:
    case KEY_RIGHT:
    case KEY_DOWN:
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_RIGHT || key == KEY_DOWN);
        mBoxFocus = mHits.focused() && mHits.focused()->mId == TH_FIELD;
        return true;
    default:
        return false;
    }
}

bool WolfAdventureTalkView::handleUnicodeCharHere(llwchar uni_char)
{
    if (mBoxFocus)
    {
        if (uni_char < 0x20 || uni_char == 0x7f) return true;
        LLWString one;
        one.push_back(uni_char);
        const std::string add = wstring_to_utf8str(one);
        // §1.2 at most 720 bytes: the box stops there (it never cuts a character)
        if (mBox.size() + add.size() <= WolfAdventure::MAX_TEXT_BYTES) mBox += add;
        return true;
    }
    if (uni_char == ' ')
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled) press(f->mId, f->mArg);
        return true;
    }
    // §3.2: the number keys pick a reply while the box is not being typed in
    if (uni_char >= '1' && uni_char <= '6')
    {
        const WolfAdventure::Talk& t = WolfAdventure::instance().talk();
        const S32 i = (S32)(uni_char - '1');
        if (i < (S32)t.mReplies.size() && !t.mTyping && !t.mWaiting) press(TH_REPLY, i);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventureTalk::WolfFloaterAdventureTalk(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureTalk::postBuild()
{
    mView = getChild<WolfAdventureTalkView>("view");
    // In front of the drag handle, as WolfFloaterRoleplay::postBuild explains.
    setBackgroundVisible(false);
    return true;
}

bool WolfFloaterAdventureTalk::viewHasFocus() const
{
    return mView && gFocusMgr.childHasKeyboardFocus(this);
}

void WolfFloaterAdventureTalk::fit()
{
    if (!mView || !getParent()) return;
    const S32 W = worldW(), H = worldH();
    S32 w = llclamp((S32)(WIDTH_FRACTION * (F32)W), MIN_W, llmin(MAX_W, llmax(MIN_W, W - 32)));
    const S32 h = llclamp(mView->measure(w), 120, llmax(160, H - 60));
    // Source: HUD_MESSAGES_V1.md §3.2 Where - above the game HUD, left of centre; with the HUD
    // hidden, above the bottom toolbar; top right under the notices when the bottom belongs to the
    // flight deck or the car dashboard (§3.1). The game HUD publishes where (setTalkAnchor).
    S32 left = 0, top = 0;
    S32 ax = W - 16, ay = 80;
    bool from_top = false;
    if (sAnchorSet)
    {
        getParent()->screenPointToLocal(sAnchorX, sAnchorY, &ax, &ay);
        from_top = sAnchorTop;
    }
    if (from_top)
    {
        left = ax - w;
        top = ay;
    }
    else
    {
        // centred a little left of the middle; never past the objective line on the right
        left = W / 2 - (S32)(0.62f * (F32)w);
        if (left + w > ax - 8) left = ax - 8 - w;
        if (left < 16)
        {
            left = 16;
            w = llmax(MIN_W, llmin(w, ax - 8 - left));
        }
        top = ay + h;
    }
    left = llclamp(left, 0, llmax(0, W - w));
    top = llclamp(top, h, llmax(h, H - 10));
    const LLRect want(left, top, left + w, top - h);
    if (want != getRect())
    {
        reshape(w, h);
        setRect(want);
        mView->setRect(LLRect(0, h, w, 0));
    }
}

void WolfFloaterAdventureTalk::draw()
{
    if (!WolfGrid::isOnWolfTerritories() || !WolfAdventure::instance().talk().mOn)
    {
        closeFloater();
        return;
    }
    fit();
    LLFloater::draw();
}

void WolfFloaterAdventureTalk::onClose(bool app_quitting)
{
    // closed from outside (the floater manager, logout): the talk ends too, so the panel and the
    // region never disagree about whether a conversation is going on
    if (!app_quitting && WolfAdventure::instanceExists() && WolfAdventure::instance().talk().mOn) WolfAdventure::instance().talkClose();
    LLFloater::onClose(app_quitting);
}
