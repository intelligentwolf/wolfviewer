/**
 * @file wolfadventureconsole.cpp
 * @brief WolfViewer: Wolf Adventure in the Roleplay window - the Journal page, the honour bar on
 *        Stats, and the adventure settings on Look. Part of WolfGameConsole (wolffloaterroleplay.h).
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

#include "wolffloaterroleplay.h"

#include <cmath>

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "wolfadventure.h"
#include "wolfadventurehud.h"
#include "wolfgame.h"
#include "wolfgamehud.h"

using namespace WolfGameDraw;

namespace
{
    // The same inks as the rest of the window (wolffloaterroleplay.cpp): 4.5:1 or better on its cards.
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80

    // §3.1 Entries: filter chips All / Clues / Notes / Lore / Done, by the entry's `kind`.
    struct Filter { const char* mKind; const char* mLabel; };
    const Filter FILTERS[] = { { "", "All" }, { "clue", "Clues" }, { "note", "Notes" }, { "lore", "Lore" }, { "done", "Done" } };
    const S32 FILTER_COUNT = (S32)(sizeof(FILTERS) / sizeof(FILTERS[0]));
    // How many "Adventures here" rows, and open parties on each, the page lists.
    const size_t MAX_HERE_ROWS = 20, MAX_PARTY_BUTTONS = 3;
    // Join arguments: row * JOIN_ROW + k; k 0 = Join (the service picks), 1..3 an open party, 99 Start a party.
    const S32 JOIN_ROW = 100, JOIN_NEW = 99;
    // A class code is short (BRAIN_API_V1.md class_create returns it); the field holds at most this.
    const size_t CODE_MAX = 32;

    std::string purposeWord(const std::string& p)
    {
        // ADVENTURE_PACK_V1.md §2 purpose: adventure, education, experience, event (as words, §3.1).
        if (p == "education") return "Education";
        if (p == "experience") return "Experience";
        if (p == "event") return "Event";
        return "Adventure";
    }

    std::string ratingWord(const std::string& r)
    {
        // ADVENTURE_PACK_V1.md §2 rating: general, moderate, adult.
        if (r == "moderate") return "Rated Moderate";
        if (r == "adult") return "Rated Adult";
        return "Rated General";
    }

    /** An entry's kind as a word beside it (§3.1 Entries; the kinds of the filter chips). */
    std::string kindWord(const std::string& k)
    {
        if (k == "clue") return "Clue";
        if (k == "note" || k.empty()) return "Note";
        if (k == "lore") return "Lore";
        if (k == "done") return "Done";
        std::string w = k;
        w[0] = (char)toupper((unsigned char)w[0]);
        return w;
    }

    std::string nameOf(const std::string& uuid)
    {
        if (!LLUUID::validate(uuid)) return std::string();
        LLAvatarName av;
        // queues a lookup when not known yet; the next frame shows it
        if (LLAvatarNameCache::get(LLUUID(uuid), &av)) return av.getDisplayName();
        return std::string();
    }

    /** The first letter, whole (a multi-byte letter is never cut) - an initial for a picture-less box. */
    std::string firstLetter(const std::string& s, const char* fallback)
    {
        const LLWString w = utf8str_to_wstring(s);
        return w.empty() ? std::string(fallback) : wstring_to_utf8str(w.substr(0, 1));
    }

    void tick(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        lineW(cx - s * 0.5f, cy, cx - s * 0.1f, cy - s * 0.4f, 2.f, c);
        lineW(cx - s * 0.1f, cy - s * 0.4f, cx + s * 0.55f, cy + s * 0.45f, 2.f, c);
    }

    /** §3.5: honour (big), the faction bars (smaller) and the characters' opinions (a list with words). */
    F32 repBlock(const WolfAdventure::Rep& rep, F32 l, F32 r, F32 top, const Palette& p)
    {
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        const F32 slh = (F32)small->getLineHeight();
        F32 y = top;
        if (rep.mHonour.mOn) y = WolfAdventureUI::meterBar(rep.mHonour, l, r, y, p, true) - 8.f;
        for (const WolfAdventure::Meter& f : rep.mFactions) y = WolfAdventureUI::meterBar(f, l, r, y, p, false) - 6.f;
        if (!rep.mCast.empty())
        {
            textL(bold, caps("What they think of you"), l, y, p.mDim);
            y -= slh + 3.f;
            for (const WolfAdventure::Opinion& o : rep.mCast)
            {
                textL(small, o.mName, l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.55f);
                const std::string v = (o.mValue > 0 ? "+" : "") + std::to_string(o.mValue) + "  " + WolfAdventureUI::opinionWord(o.mValue);
                textL(small, v, r, y, p.mText, LLFontGL::RIGHT, LLFontGL::TOP);
                y -= slh + 1.f;
            }
            y -= 4.f;
        }
        return y;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// Stats: honour (§3.5) and Look: the adventure settings (§5.6)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfGameConsole::honourSection(F32 l, F32 r, F32 top, const Palette& p)
{
    const WolfAdventure::Adventure* a = WolfAdventure::instance().current();
    if (!a || !a->mRep.mValid) return top;
    const WolfAdventure::Rep& rep = a->mRep;
    if (!rep.mHonour.mOn && rep.mFactions.empty() && rep.mCast.empty()) return top;
    F32 y = sectionHead(l, r, top, a->mQuest.mSrc.empty() ? std::string("Reputation") : "Reputation - " + a->mQuest.mSrc, p);
    y = repBlock(rep, l, r, y, p);
    hit(l, y, r, top, H_INFO, 900, "Your honour and reputation in this adventure", false);
    return y - 8.f;
}

F32 WolfGameConsole::adventureLookSection(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    F32 y = sectionHead(l, r, top, "Adventures", p);
    const bool cam = WolfAdventure::storyCamera();
    const bool still = WolfAdventure::reduceMotion();
    const F32 bw = llmin(220.f, (r - l) * 0.45f);
    button(l, y - 28.f, l + bw, y, cam ? "Story camera: On" : "Story camera: Off", p, flagsFor(H_ADV_STORYCAM, 0) | (cam ? BTN_ACTIVE : 0));
    hit(l, y - 28.f, l + bw, y, H_ADV_STORYCAM, 0,
        cam ? "Story camera is on: an adventure may turn your camera for a moment (any key or the mouse gives it back). Click to turn it off."
            : "Story camera is off: adventures never move your camera. Click to turn it on.");
    paragraph(l + bw + 10.f, r, y - 2.f, "An adventure may turn the camera to show you something; any key or mouse movement gives it back at once.", p.mDim, small);
    y -= 40.f;
    button(l, y - 28.f, l + bw, y, still ? "Reduce motion: On" : "Reduce motion: Off", p, flagsFor(H_ADV_MOTION, 0) | (still ? BTN_ACTIVE : 0));
    hit(l, y - 28.f, l + bw, y, H_ADV_MOTION, 0,
        still ? "Reduce motion is on: bars and titles appear without sliding and camera moves jump straight to the view. Click to turn it off."
              : "Reduce motion is off. Click to make bars and titles appear without sliding, and camera moves jump to the view.");
    paragraph(l + bw + 10.f, r, y - 2.f, "Bars and titles appear without sliding; camera moves jump straight to the view.", p.mDim, small);
    y -= 40.f;
    // [WOLF ADVENTURE P4 2026-10-16] §5.6 "Character voices volume" and "Show AI tag (default on, cannot be hidden in classes)"
    {
        const S32 pct = (S32)llround(WolfAdventure::voiceVolume() * 100.f);
        const F32 sb = 30.f;
        button(l, y - 28.f, l + sb, y, "-", p, flagsFor(H_ADV_VOICEVOL, 0) | (pct <= 0 ? BTN_DISABLED : 0));
        hit(l, y - 28.f, l + sb, y, H_ADV_VOICEVOL, 0, "Character voices quieter (10% a press)");
        textL(LLFontGL::getFontSansSerif(), llformat("Character voices: %d%%", pct), l + sb + 8.f, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, bw - 2.f * sb - 16.f);
        button(l + bw - sb, y - 28.f, l + bw, y, "+", p, flagsFor(H_ADV_VOICEVOL, 1) | (pct >= 100 ? BTN_DISABLED : 0));
        hit(l + bw - sb, y - 28.f, l + bw, y, H_ADV_VOICEVOL, 1, "Character voices louder (10% a press)");
        paragraph(l + bw + 10.f, r, y - 2.f, "The volume of story characters' voices this viewer plays (the Studio's previews and Talk to them now). "
                  "Voices in the world come mixed into the region's voice chat: use the voice volume for those.", p.mDim, small);
        y -= 48.f;
        const bool tag = gSavedPerAccountSettings.getBOOL("WolfAdventureShowAiTag");
        button(l, y - 28.f, l + bw, y, tag ? "Show AI tag: On" : "Show AI tag: Off", p, flagsFor(H_ADV_AITAG, 0) | (tag ? BTN_ACTIVE : 0));
        hit(l, y - 28.f, l + bw, y, H_ADV_AITAG, 0,
            tag ? "Lines written by AI carry a small AI tag. Click to hide it (it always shows in class play)."
                : "The AI tag is hidden (it always shows in class play). Click to show it again.");
        paragraph(l + bw + 10.f, r, y - 2.f, "Marks lines a character's AI wrote. It always shows in a class.", p.mDim, small);
    }
    return y - 40.f;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the Journal (§3.1)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGameConsole::onJournalOpened()
{
    WolfAdventure& wa = WolfAdventure::instance();
    // §4: hello "when the Journal opens" - the brain answers with this player's quest and rep. Opening and closing
    // it again and again sends at most one in 3 s: the answer to the last one is still on its way or here.
    const F64 t = LLFrameTimer::getTotalSeconds();
    if (t - mJournalHelloAt >= 3.0)
    {
        mJournalHelloAt = t;
        wa.sendHello();
    }
    const WolfAdventure::Adventure* sel = mJournalAdv.empty() ? nullptr : wa.find(mJournalAdv);
    if (!mJournalHere && (!sel || !sel->mHasQuest))
    {
        const WolfAdventure::Adventure* cur = wa.current();
        mJournalAdv = cur ? cur->mQuest.mAdv : std::string();
    }
    // the page itself comes with the `journal` verb (§4; BRAIN_API_V1.md §6.3)
    if (!mJournalHere && !mJournalAdv.empty()) wa.sendJournalPage(mJournalAdv, 0);
    if (!wa.hereCurrent() && !wa.hereLoading()) wa.refreshHere();
}

void WolfGameConsole::showAdventuresHere()
{
    mJournalHere = true;
    WolfAdventure& wa = WolfAdventure::instance();
    if (!wa.hereLoading()) wa.refreshHere();
}

void WolfGameConsole::textField(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, S32 id, S32 arg,
                                const Palette& p)
{
    const bool focused = hasFocus() && mHits.isFocused(id, arg);
    chamfer(l, b, r, t, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
    // the edge at 3:1 or better (§6 1.4.11); the accent and a thicker line when it has the keyboard
    chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, 4.f, focused ? 2.f : 1.f, focused ? p.mAccent : p.mDim);
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 cy = (b + t) * 0.5f;
    F32 x = l + 8.f;
    if (text.empty()) textL(f, placeholder, x, cy, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 16.f);
    else x = textL(f, text, x, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 16.f);
    if (focused && fmod(LLFrameTimer::getTotalSeconds(), 1.0) < 0.6)
    {
        rect(x + 1.f, cy - (F32)f->getLineHeight() * 0.45f, x + 2.f, cy + (F32)f->getLineHeight() * 0.45f, p.mText);
    }
    hit(l, b, r, t, id, arg, "Type or paste the class code your teacher gave you, then Enter");
}

F32 WolfGameConsole::pageJournal(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    if (!mClassCodesLoaded)
    {
        mClassCodesLoaded = true;
        const LLSD codes = gSavedPerAccountSettings.getLLSD("WolfAdventureClassCodes");
        if (codes.isMap())
        {
            for (LLSD::map_const_iterator it = codes.beginMap(); it != codes.endMap(); ++it)
            {
                if (it->second.isString()) mClassCodes[it->first] = WolfGame::cleanText(it->second.asString(), CODE_MAX);
            }
        }
    }
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    mJournalTabs = wa.playing();
    if (!mJournalHere && std::find(mJournalTabs.begin(), mJournalTabs.end(), mJournalAdv) == mJournalTabs.end())
    {
        const WolfAdventure::Adventure* cur = wa.current();
        mJournalAdv = cur ? cur->mQuest.mAdv : std::string();
    }
    const bool here = mJournalHere || mJournalAdv.empty();
    F32 y = top;
    // §3.1 "Several adventures: a selector at the top lists the adventures the player is in, plus
    // Adventures here"
    {
        const F32 ch = 26.f, gap = 6.f;
        F32 x = l;
        auto chip = [&](const std::string& label, S32 arg, bool on, const std::string& tip)
        {
            const F32 w = llmin(200.f, bold->getWidthF32(label) + 24.f);
            if (x > l && x + w > r) { x = l; y -= ch + gap; }
            button(x, y - ch, x + w, y, label, p, flagsFor(H_ADV_TAB, arg) | (on ? BTN_ACTIVE : 0));
            hit(x, y - ch, x + w, y, H_ADV_TAB, arg, tip);
            x += w + gap;
        };
        for (size_t i = 0; i < mJournalTabs.size(); ++i)
        {
            const WolfAdventure::Adventure* a = wa.find(mJournalTabs[i]);
            const std::string title = a && !a->mQuest.mSrc.empty() ? a->mQuest.mSrc : std::string("Adventure");
            chip(title, (S32)i, !here && mJournalTabs[i] == mJournalAdv, title + ": its journal");
        }
        chip("Adventures here", -1, here, "The adventures on this region: join one, start a party, or leave");
        y -= ch + 12.f;
    }
    if (here) return journalHere(l, r, y, p);
    return journalAdventure(l, r, y, mJournalAdv, p);
}

F32 WolfGameConsole::journalAdventure(F32 l, F32 r, F32 top, const std::string& adv, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Adventure* a = wa.find(adv);
    if (!a) return top;
    const WolfAdventure::Quest& q = a->mQuest;
    const WolfAdventure::Journal& j = a->mJournal;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const F32 slh = (F32)small->getLineHeight(), lh = (F32)body->getLineHeight();
    F32 y = top;

    // ---- 1. Header: the title, the party chip, rating and purpose as words, Ready ----
    const std::string title = !j.mTitle.empty() ? j.mTitle : (!q.mSrc.empty() ? q.mSrc : std::string("Adventure"));
    const bool party = q.mParty.mOn;
    const F32 ready_w = 130.f;
    // [WOLF ADVENTURE AUDIT 2026-10-17] the header in the adventure's own look (HUD_MESSAGES_V1.md §2.1): its logo, and its
    // accent title on its panel colour when it has them
    F32 tx = l;
    const Palette lp = WolfAdventureUI::lookPalette(p, adv);
    const F32 th = (F32)display(22.f)->getLineHeight();
    if (q.mLook.mColours) rect(l - 4.f, y - th - 4.f, r - (party ? ready_w + 6.f : 0.f), y + 4.f, lp.mCard);
    if (!q.mLook.mLogo.empty())
    {
        icon(q.mLook.mLogo, l, y - th, th, p, std::string(), true);
        tx = l + th + 8.f;
    }
    textL(display(22.f), title, tx, y, lp.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - tx - (party ? ready_w + 10.f : 0.f));
    if (party)
    {
        // §3.1 "Ready (a toggle with its state as a word)"
        const bool ready = q.mParty.mReady;
        button(r - ready_w, y - 26.f, r, y, ready ? "Ready: Yes" : "Ready: No", p, flagsFor(H_ADV_READY, 0) | (ready ? BTN_ACTIVE : 0));
        hit(r - ready_w, y - 26.f, r, y, H_ADV_READY, 0,
            ready ? "You are marked ready. Click to say you are not ready yet." : "Click to tell your party you are ready");
    }
    y -= (F32)display(22.f)->getLineHeight() + 2.f;
    std::string line;
    if (j.mValid) line = purposeWord(j.mPurpose) + " \xC2\xB7 " + ratingWord(j.mRating);
    if (party)
    {
        std::string leader = q.mParty.mLeader == gAgentID.asString() ? std::string("you") : nameOf(q.mParty.mLeader);
        if (leader.empty()) leader = "loading...";
        const std::string chip = q.mParty.mSize <= 1 ? "On your own" : llformat("Party of %d \xC2\xB7 Leader: ", q.mParty.mSize) + leader;
        line = line.empty() ? chip : line + "  \xC2\xB7  " + chip;
        if (q.mParty.mWaiting > 0) line += llformat(" \xC2\xB7 %d not ready", q.mParty.mWaiting);
    }
    if (!line.empty())
    {
        textL(small, line, l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 4.f;
    }
    if (q.mPaused || q.mGm)
    {
        std::string state;
        if (q.mPaused) state = q.mPausedText.empty() ? std::string("Paused: the story is paused.") : "Paused: " + q.mPausedText;
        if (q.mGm) state += (state.empty() ? std::string() : std::string("  ")) + "A host is running this story live.";
        y = paragraph(l, r, y, state, p.mAccent, bold) - 4.f;
    }
    if (j.mEndedOn || q.mEnded.mOn)
    {
        // [WORDING 2026-10-18] how it ended: the journal's own `ended`, else the quest's (as the web Journal, j.ended || q.ended);
        // never "Finished" on a loss
        const bool fromJ = j.mEndedOn;
        const std::string ek = fromJ ? j.mEndedKind : q.mEnded.mKind;
        const std::string et = fromJ ? j.mEndedTitle : q.mEnded.mTitle, ex = fromJ ? j.mEndedText : q.mEnded.mText;
        y = paragraph(l, r, y, (ek == "win" ? "You won: " : ek == "lose" ? "You lost: " : "The end: ") + (et.empty() ? title : et),
                      ek == "lose" ? DANGER_INK : OK_INK, bold);
        if (!ex.empty()) y = paragraph(l, r, y, ex, p.mText);
        y -= 4.f;
    }
    y -= 6.f;

    // ---- [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 4: the game card and the main quest (journal goal, main, card) ----
    {
        const bool rp = j.mShape == "roleplay" || q.mShape == "roleplay";
        const std::string goal = !j.mGoal.empty() ? j.mGoal : q.mGoal;
        const S32 of = j.mMainOf > 0 ? j.mMainOf : q.mMainOf, step = j.mMainOf > 0 ? j.mMainStep : q.mMainStep;
        const std::string mainTitle = j.mMainOf > 0 ? j.mMainTitle : q.mMainTitle;
        if (j.mHasCard || !goal.empty() || of > 0)
        {
            y = sectionHead(l, r, y, rp ? "Your part" : "Your game", p);
            if (!j.mYouAre.empty()) y = paragraph(l, r, y, "You are: " + j.mYouAre, p.mText) - 2.f;
            if (!goal.empty()) y = paragraph(l, r, y, (rp ? "About: " : "Goal: ") + goal, p.mText) - 2.f;
            if (!rp && !j.mWin.empty()) y = paragraph(l, r, y, "You win when: " + j.mWin, p.mText) - 2.f;
            if (!rp && j.mHasCard) y = paragraph(l, r, y, j.mLose.empty() ? std::string("You can't lose - just keep trying.") : "You lose when: " + j.mLose, p.mText) - 2.f;
            if (of > 0) y = paragraph(l, r, y, llformat("Main quest %d/%d", step, of) + (mainTitle.empty() ? std::string() : ": " + mainTitle), p.mAccent, bold) - 2.f;
            y -= 6.f;
        }
    }

    // ---- 2. Now: the objective card, Show on map / Teleport, the cards waiting for a choice ----
    y = sectionHead(l, r, y, "Now", p);
    {
        const std::string objective = !q.mObjective.empty() ? q.mObjective : (!j.mObjective.empty() ? j.mObjective : std::string("Nothing to do right now."));
        const std::vector<std::string> lines = wrap(body, objective, r - l - 24.f, 6);
        const bool marker = q.mMarker.mOn;
        const F32 ch = 12.f + (F32)lines.size() * lh + (marker ? slh + 8.f + 28.f : 0.f) + 10.f;
        card(l, y - ch, r, y, p, 6.f);
        F32 cy = y - 10.f;
        for (const std::string& ln : lines)
        {
            textL(body, ln, l + 12.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            cy -= lh;
        }
        if (marker)
        {
            const F32 metres = WolfAdventure::markerDistance(q);
            std::string where = (q.mMarker.mLabel.empty() ? std::string("The marker") : q.mMarker.mLabel) + " - " + q.mMarker.mRegion;
            if (metres >= 0.f) where += " - " + thousands((S64)llround(metres)) + " m away";
            textL(small, where, l + 12.f, cy - 2.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            cy -= slh + 8.f;
            const F32 bw = 130.f;
            button(l + 12.f, cy - 26.f, l + 12.f + bw, cy, "Show on map", p, flagsFor(H_ADV_MAP, 0));
            hit(l + 12.f, cy - 26.f, l + 12.f + bw, cy, H_ADV_MAP, 0, "Show the objective on the world map (asks first)");
            button(l + 20.f + bw, cy - 26.f, l + 20.f + 2.f * bw, cy, "Teleport", p, flagsFor(H_ADV_TP, 0));
            hit(l + 20.f + bw, cy - 26.f, l + 20.f + 2.f * bw, cy, H_ADV_TP, 0, "Teleport to the objective (asks first)");
        }
        y -= ch + 8.f;
    }
    mWaitingIds.clear();
    for (const WolfAdventure::Card* c : wa.waitingCards())
    {
        if (c->mAdv != adv) continue;
        const S32 arg = (S32)mWaitingIds.size();
        mWaitingIds.push_back({ c->mAdv, c->mId });
        const F32 bw = 110.f;
        textL(bold, "Waiting for your choice:", l, y - 4.f, p.mAccent);
        textL(body, c->mTitle.empty() ? std::string("A decision") : c->mTitle, l + bold->getWidthF32("Waiting for your choice:") + 8.f, y - 3.f,
              p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - bw - 180.f);
        button(r - bw, y - 24.f, r, y, "Choose...", p, flagsFor(H_ADV_CARD, arg));
        hit(r - bw, y - 24.f, r, y, H_ADV_CARD, arg, "Bring this card back to choose");
        y -= 30.f;
    }
    if (!q.mDone.empty())
    {
        textL(bold, caps("Done"), l, y, p.mDim);
        y -= slh + 2.f;
        for (const std::string& d : q.mDone)
        {
            tick(l + 6.f, y - slh * 0.5f, 10.f, OK_INK);
            textL(small, d, l + 18.f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 18.f);
            y -= slh + 1.f;
        }
        y -= 6.f;
    }
    if (!j.mValid)
    {
        return paragraph(l, r, y - 4.f, "The journal is on its way from the story...", p.mDim) - 8.f;
    }

    // ---- 3. Chapters: a vertical timeline (done, current, locked); done chapters expand ----
    mChapterActs.clear();
    if (!j.mChapters.empty())
    {
        y = sectionHead(l, r, y, "Chapters", p);
        bool has_acts = false;
        for (const WolfAdventure::Entry& e : j.mEntries) has_acts = has_acts || !e.mAct.empty();
        for (size_t i = 0; i < j.mChapters.size(); ++i)
        {
            const WolfAdventure::Chapter& c = j.mChapters[i];
            const S32 arg = (S32)i;
            mChapterActs.push_back(c.mAct);
            const F32 cx = l + 9.f, cy = y - 10.f;
            if (i + 1 < j.mChapters.size()) rect(cx - 1.f, y - 30.f, cx + 1.f, cy - 7.f, p.mTrack);
            const bool done = c.mState == "done", cur = c.mState == "current";
            // shape AND word carry the state (§3: colour never alone)
            if (cur) { disc(cx, cy, 7.f, p.mAccent, 20); }
            else if (done) { disc(cx, cy, 7.f, p.mTrack, 20); tick(cx, cy, 8.f, OK_INK); }
            else { arc(cx, cy, 6.f, 1.5f, 0.f, 2.f * F_PI, p.mDim, 20); }
            const std::string word = cur ? "Now" : done ? "Done" : "Locked";
            // §3.1 note: a done chapter expands only when the entries say which chapter they belong to
            // (the optional `act`, an additive change); without it there is nothing to put under it.
            const bool expandable = done && has_acts;
            const bool open = expandable && mOpenChapters.count(c.mAct) > 0;
            // [WORDING 2026-10-18] a chapter with no title is "Chapter N", never its raw id (as the web Journal)
            textL(cur ? bold : body, c.mTitle.empty() ? llformat("Chapter %d", (S32)i + 1) : c.mTitle, l + 24.f, y - 2.f, done || cur ? p.mText : p.mDim,
                  LLFontGL::LEFT, LLFontGL::TOP, r - l - 120.f);
            textL(small, word, r - (expandable ? 20.f : 0.f), y - 3.f, cur ? p.mAccent : p.mDim, LLFontGL::RIGHT, LLFontGL::TOP);
            if (expandable) chevron(r - 8.f, y - 9.f, !open, p.mText);
            if (expandable)
            {
                hit(l, y - 22.f, r, y, H_ADV_CHAPTER, arg, open ? "Hide what happened in this chapter" : "Show what happened in this chapter");
            }
            y -= 24.f;
            if (open)
            {
                bool any = false;
                for (const WolfAdventure::Entry& e : j.mEntries)
                {
                    if (e.mAct != c.mAct) continue;
                    any = true;
                    y = paragraph(l + 24.f, r, y, kindWord(e.mKind) + ": " + e.mText, p.mText, small) - 2.f;
                }
                if (!any) y = paragraph(l + 24.f, r, y, "No entries from this chapter on this page of the journal.", p.mDim, small) - 2.f;
                y -= 4.f;
            }
        }
        y -= 6.f;
    }

    // ---- 4. Entries: filter chips, the list newest first, paging ----
    y = sectionHead(l, r, y, "Entries", p);
    {
        F32 x = l;
        for (S32 i = 0; i < FILTER_COUNT; ++i)
        {
            const F32 w = bold->getWidthF32(FILTERS[i].mLabel) + 22.f;
            const bool on = mEntryFilter == FILTERS[i].mKind;
            button(x, y - 22.f, x + w, y, FILTERS[i].mLabel, p, flagsFor(H_ADV_FILTER, i) | (on ? BTN_ACTIVE : 0));
            hit(x, y - 22.f, x + w, y, H_ADV_FILTER, i, std::string("Show ") + (i == 0 ? "every entry" : FILTERS[i].mLabel) + (on ? " (showing now)" : ""));
            x += w + 6.f;
        }
        y -= 30.f;
    }
    bool any_entry = false;
    for (const WolfAdventure::Entry& e : j.mEntries)
    {
        if (!mEntryFilter.empty() && e.mKind != mEntryFilter) continue;
        any_entry = true;
        textL(bold, kindWord(e.mKind), l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, 60.f);
        y = paragraph(l + 64.f, r, y, e.mText, p.mText, small) - 6.f;
    }
    if (!any_entry) y = paragraph(l, r, y, mEntryFilter.empty() ? "No entries yet. Clues and notes appear here as the story goes on."
                                                                 : "No entries of this kind on this page.", p.mDim) - 6.f;
    if (j.mPages > 1)
    {
        const F32 bw = 100.f;
        const bool prev = j.mPage > 0, next = j.mPage + 1 < j.mPages;
        button(l, y - 24.f, l + bw, y, "Previous", p, flagsFor(H_ADV_PAGE, -1, prev));
        hit(l, y - 24.f, l + bw, y, H_ADV_PAGE, -1, "Newer entries", true, prev);
        textL(small, llformat("Page %d of %d", j.mPage + 1, j.mPages), (l + r) * 0.5f, y - 12.f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
        button(r - bw, y - 24.f, r, y, "Next", p, flagsFor(H_ADV_PAGE, 1, next));
        hit(r - bw, y - 24.f, r, y, H_ADV_PAGE, 1, "Older entries", true, next);
        y -= 32.f;
    }
    y -= 4.f;

    // ---- 5. Handouts: a grid of thumbnails, the title under each ----
    mHandoutIds.clear();
    if (!j.mHandouts.empty())
    {
        y = sectionHead(l, r, y, "Handouts", p);
        const F32 S = 96.f, cell = S + 14.f, row_h = S + slh + 12.f;
        const S32 cols = llmax(1, (S32)((r - l) / cell));
        for (size_t i = 0; i < j.mHandouts.size(); ++i)
        {
            const WolfAdventure::Handout& h = j.mHandouts[i];
            const S32 col = (S32)i % cols, row = (S32)i / cols;
            const F32 hl = l + (F32)col * cell, ht = y - (F32)row * row_h;
            mHandoutIds.push_back(h.mId);
            icon(h.mTexture, hl, ht - S, S, p, firstLetter(h.mTitle, "?"));
            if (mHits.isHover(H_ADV_HANDOUT, (S32)i) || (mKeyboardNav && hasFocus() && mHits.isFocused(H_ADV_HANDOUT, (S32)i)))
            {
                focusRing(hl, ht - S, hl + S, ht, p);
            }
            textL(small, h.mTitle, hl + S * 0.5f, ht - S - 3.f, p.mText, LLFontGL::HCENTER, LLFontGL::TOP, cell - 4.f);
            hit(hl, ht - S - slh - 4.f, hl + S, ht, H_ADV_HANDOUT, (S32)i, (h.mTitle.empty() ? std::string("A handout") : h.mTitle) + " - click to open it");
        }
        y -= (F32)(((S32)j.mHandouts.size() + cols - 1) / cols) * row_h + 6.f;
    }

    // ---- 6. Your progress: journal flags, reputation and honour, quizzes ----
    const bool rep = a->mRep.mValid && (a->mRep.mHonour.mOn || !a->mRep.mFactions.empty() || !a->mRep.mCast.empty());
    if (!j.mFlags.empty() || rep || !j.mQuizzes.empty())
    {
        y = sectionHead(l, r, y, "Your progress", p);
        for (const WolfAdventure::Row& f : j.mFlags)
        {
            textL(small, f.mLabel, l, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.6f);
            textL(small, f.mValue, r, y, p.mText, LLFontGL::RIGHT, LLFontGL::TOP, (r - l) * 0.35f);
            y -= slh + 2.f;
        }
        if (!j.mFlags.empty()) y -= 6.f;
        if (rep) y = repBlock(a->mRep, l, r, y, p) - 4.f;
        mQuizRowIds.clear();
        for (const WolfAdventure::QuizRow& qz : j.mQuizzes)
        {
            const bool done = qz.mState == "done";
            const std::string state = done ? (qz.mScore.empty() ? std::string("Done") : "Done - " + qz.mScore) : std::string("Open");
            textL(small, qz.mTitle.empty() ? std::string("Quiz") : qz.mTitle, l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.6f);
            if (done) textL(small, state, r, y, OK_INK, LLFontGL::RIGHT, LLFontGL::TOP);
            else
            {
                // [WOLF ADVENTURE AUDIT 2026-10-17] an open quiz can always be opened again (it was a dead end once closed)
                const S32 arg = (S32)mQuizRowIds.size();
                mQuizRowIds.push_back({ adv, qz.mId });
                const F32 bw = bold->getWidthF32("Open the quiz") + 24.f;
                button(r - bw, y - 22.f, r, y + 2.f, "Open the quiz", p, flagsFor(H_ADV_QUIZ, arg, !qz.mId.empty()));
                hit(r - bw, y - 22.f, r, y + 2.f, H_ADV_QUIZ, arg, "Open this quiz again (your answers so far are kept)", true, !qz.mId.empty());
                y -= 10.f;
            }
            y -= slh + 2.f;
        }
        y -= 6.f;
    }

    // ---- 7. Conversations (when saved), Credits and Attribution (§3.1). [WOLF ADVENTURE P2 2026-10-16]
    //      Conversations come from the brain's chatlog {adventure} (p2brief: the caller's own lines). ----
    if (j.mConversations || mConvoAdv == adv) y = conversationsSection(l, r, y, adv, p);
    if (!j.mCredits.empty())
    {
        y = sectionHead(l, r, y, "Made by", p);   // [WORDING 2026-10-18] "credits" is money now
        for (const WolfAdventure::Row& c : j.mCredits)
        {
            textL(small, c.mLabel, l, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.4f);
            textL(small, c.mValue, l + (r - l) * 0.42f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.58f);
            y -= slh + 2.f;
        }
        y -= 6.f;
    }
    if (!j.mAttribution.empty())
    {
        y = sectionHead(l, r, y, "Based on", p);   // [WORDING 2026-10-18]
        for (const WolfAdventure::Row& at : j.mAttribution)
        {
            if (!at.mLabel.empty())
            {
                textL(bold, at.mLabel, l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
                y -= slh + 1.f;
            }
            y = paragraph(l, r, y, at.mValue, p.mDim, small) - 6.f;
        }
    }
    // Leave this adventure (asks first)
    {
        const F32 bw = 180.f;
        button(l, y - 28.f, l + bw, y, "Leave this adventure", p, flagsFor(H_ADV_LEAVE, -1) | BTN_DANGER);
        hit(l, y - 28.f, l + bw, y, H_ADV_LEAVE, -1, "Leave this adventure and its party (asks first)");
        y -= 36.f;
    }
    return y;
}

F32 WolfGameConsole::journalHere(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (wa.playing().empty())
    {
        // §3.1 the empty state, in its own words
        y = emptyCard(l, r, y, "No adventure yet", "No adventure yet. Adventures on this region are listed below, or look in Wolf Experiences.", p);
    }
    // the refresh button sits on the section's own line
    {
        const F32 bw = 90.f;
        const bool can = !wa.hereLoading();
        button(r - bw, y - 22.f, r, y, can ? "Refresh" : "Looking...", p, flagsFor(H_ADV_REFRESH, 0, can));
        hit(r - bw, y - 22.f, r, y, H_ADV_REFRESH, 0, "Ask the grid again for the adventures on this region", true, can);
    }
    y = sectionHead(l, r - 100.f, y, "Adventures here", p);
    if (!wa.hereCurrent() && !wa.hereLoading() && wa.hereError().empty())
    {
        wa.refreshHere();   // the list is for another region (or none yet)
    }
    if (wa.hereLoading()) y = paragraph(l, r, y, "Looking for adventures on this region...", p.mDim) - 6.f;
    if (!wa.hereError().empty()) y = paragraph(l, r, y, "Could not list them: " + wa.hereError(), DANGER_INK) - 6.f;
    const LLSD& list = wa.here();
    mHereIds.clear();
    if (!wa.hereLoading() && wa.hereError().empty() && (!list.isArray() || list.size() == 0))
    {
        y = paragraph(l, r, y, "No adventures run on this region right now. Wolf World > Wolf Experiences lists places to go.", p.mDim) - 6.f;
    }
    if (!list.isArray()) return y;
    for (size_t i = 0; i < (size_t)list.size() && i < MAX_HERE_ROWS; ++i)
    {
        const LLSD& row = list[(LLSD::Integer)i];
        const S32 arg = (S32)i;
        const std::string id = WolfGame::cleanText(row["id"].asString(), 64);
        mHereIds.push_back(id);     // by row, so the rows' hit arguments stay the list's indexes
        if (id.empty()) continue;   // nothing to join or keep a class code for
        const std::string title = WolfGame::cleanText(row["title"].asString(), 64);
        const std::string purpose = row["purpose"].asString();
        const std::string summary = WolfGame::cleanText(row["summary"].asString(), 500, true);
        const std::string now_line = WolfGame::cleanText(row["now"].asString(), 240);
        // a texture uuid only (§1.3 "Texture UUIDs"); anything else would draw as text
        std::string cover = row["cover"].asString();
        if (cover.size() != 36 || !LLUUID::validate(cover) || LLUUID(cover).isNull()) cover.clear();
        // [WOLF ADVENTURE AUDIT 2026-10-17] the adventure's logo (BRAIN_API_V1.md §10.1 adventures_here `logo`) is the list's
        // square picture when it has one; its cover otherwise
        std::string logo = row["logo"].asString();
        if (logo.size() != 36 || !LLUUID::validate(logo) || LLUUID(logo).isNull()) logo.clear();
        if (!logo.empty()) cover = logo;
        const bool playing = row["mine"]["playing"].asInteger() != 0;
        const std::string mode = row["party"]["mode"].asString();
        const S32 max_size = row["party"]["maxSize"].asInteger();
        // the card: cover, title, words, summary, now, then the buttons - measured first, so the
        // card is drawn under its content
        const F32 IS = 64.f, tl = l + IS + 22.f, tw = r - tl - 10.f;
        const LLFontGL* tf = display(17.f);
        const std::string words = purposeWord(purpose) + " \xC2\xB7 " + ratingWord(row["rating"].asString());
        const std::vector<std::string> sum = wrap(small, summary, tw, 3);
        const std::vector<std::string> nowl = wrap(small, now_line, tw, 2);
        const std::string mine_obj = WolfGame::cleanText(row["mine"]["objective"].asString(), 240);
        struct Btn { std::string mLabel, mTip; S32 mId, mArg; bool mDanger; };
        std::vector<Btn> btns;
        if (playing)
        {
            btns.push_back({ "Open journal", "Show this adventure's journal", H_ADV_OPEN, arg, false });
            btns.push_back({ "Leave", "Leave this adventure and its party (asks first)", H_ADV_LEAVE, arg, true });
        }
        else
        {
            // [WOLF ADVENTURE AUDIT 2026-10-17] it always starts a new party (no party is sent: wolfadventure_lib.php wa_join 'new')
            btns.push_back({ mode == "shared" ? std::string("Join") : std::string("Play it"),
                             mode == "shared" ? std::string("Join the story on this region") : std::string("Play it (starts your own party; the Join buttons beside it join someone else's)"),
                             H_ADV_JOIN, arg * JOIN_ROW, false });
            const LLSD& parties = row["parties"];
            size_t shown = 0;
            if (mode != "shared" && parties.isArray())
            {
                for (size_t k = 0; k < (size_t)parties.size() && shown < MAX_PARTY_BUTTONS; ++k)
                {
                    const LLSD& pt = parties[(LLSD::Integer)k];
                    if (pt["open"].asInteger() == 0) continue;
                    const std::string leader = WolfGame::cleanText(pt["leaderName"].asString(), 40);
                    const S32 size = pt["size"].asInteger();
                    std::string label = "Join " + (leader.empty() ? std::string("a party") : leader + "'s party");
                    label += max_size > 0 ? llformat(" (%d/%d)", size, max_size) : llformat(" (%d)", size);
                    btns.push_back({ label, "Join this party", H_ADV_JOIN, arg * JOIN_ROW + 1 + (S32)k, false });
                    ++shown;
                }
            }
            if (mode == "party" || mode == "both")
            {
                btns.push_back({ "Start a party", "Start a new party that others can join", H_ADV_JOIN, arg * JOIN_ROW + JOIN_NEW, false });
            }
        }
        S32 btn_rows = btns.empty() ? 0 : 1;
        {
            F32 x = tl;
            for (const Btn& b : btns)
            {
                const F32 w = bold->getWidthF32(b.mLabel) + 26.f;
                if (x > tl && x + w > r - 10.f) { x = tl; ++btn_rows; }
                x += w + 6.f;
            }
        }
        // [WOLF ADVENTURE AUDIT 2026-10-17] a teacher may run a class on any published adventure (wolfadventure_gm_lib.php
        // class open: owner or teacher grant, any purpose), so the class code goes on every card not being played
        const bool education = !playing;
        const F32 text_h = 10.f + (F32)tf->getLineHeight() + 1.f + slh + 3.f + (F32)(sum.size() + nowl.size()) * slh
                         + (playing && !mine_obj.empty() ? slh + 2.f : 0.f) + 6.f;
        const F32 card_h = llmax(IS + 20.f, text_h + (F32)btn_rows * 32.f + (education ? 34.f : 0.f) + 6.f);
        const F32 card_top = y;
        card(l, card_top - card_h, r, card_top, p, 6.f);
        icon(cover, l + 10.f, card_top - 10.f - IS, IS, p, firstLetter(title, "A"));
        F32 cy = card_top - 10.f;
        textL(tf, title.empty() ? std::string("Adventure") : title, tl, cy, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, tw);
        cy -= (F32)tf->getLineHeight() + 1.f;
        textL(small, words, tl, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, tw);
        cy -= slh + 3.f;
        for (const std::string& ln : sum)
        {
            textL(small, ln, tl, cy, p.mText);
            cy -= slh;
        }
        for (const std::string& ln : nowl)
        {
            textL(small, ln, tl, cy, p.mDim);
            cy -= slh;
        }
        if (playing && !mine_obj.empty())
        {
            textL(bold, "You are playing: " + mine_obj, tl, cy - 2.f, OK_INK, LLFontGL::LEFT, LLFontGL::TOP, tw);
            cy -= slh + 2.f;
        }
        cy -= 6.f;
        F32 x = tl;
        for (const Btn& b : btns)
        {
            const F32 w = bold->getWidthF32(b.mLabel) + 26.f;
            if (x > tl && x + w > r - 10.f) { x = tl; cy -= 32.f; }
            const bool can = !mJoinBusy || b.mId == H_ADV_OPEN;
            button(x, cy - 26.f, x + w, cy, b.mLabel, p, flagsFor(b.mId, b.mArg, can) | (b.mDanger ? BTN_DANGER : 0));
            hit(x, cy - 26.f, x + w, cy, b.mId, b.mArg, b.mTip, true, can);
            x += w + 6.f;
        }
        if (!btns.empty()) cy -= 32.f;
        if (education)
        {
            // §3.1 "the class-code field" - remembered per adventure (§6 3.3.7), may be pasted (3.3.8)
            const F32 fw = llmax(80.f, llmin(200.f, r - tl - 140.f));
            textField(tl, cy - 26.f, tl + fw, cy, mClassCodes[id], "Class code", H_ADV_CODE, arg, p);
            const bool can = !mJoinBusy;
            button(tl + fw + 8.f, cy - 26.f, tl + fw + 120.f, cy, "Join class", p, flagsFor(H_ADV_CLASSJOIN, arg, can));
            hit(tl + fw + 8.f, cy - 26.f, tl + fw + 120.f, cy, H_ADV_CLASSJOIN, arg, "Join your class with the code your teacher gave you", true, can);
        }
        y = card_top - card_h - 8.f;
    }
    return y;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [WOLF ADVENTURE P2 2026-10-16] Conversations (HUD_MESSAGES_V1.md §2.2 `conversations`, §3.1 item 7;
// BRAIN_API_V1.md §10.3: the player sees their own lines and the replies, and may delete them)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGameConsole::loadConversations(const std::string& adv)
{
    if (adv.empty()) return;
    mConvoAdv = adv;
    mConvoLoading = true;
    mConvoError.clear();
    const U32 serial = ++mConvoSerial;
    LLSD body;
    body["adventure"] = adv;
    LLHandle<WolfGameConsole> handle = getDerivedHandle<WolfGameConsole>();
    // p2brief: chatlog {adventure} -> {lines:[{id, who, cast, castName, text, flags, at}]} newest first, at most 200
    WolfAdventure::api("chatlog", body, [handle, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfGameConsole* self = handle.get();
        if (serial != self->mConvoSerial) return;   // another adventure was asked for since
        self->mConvoLoading = false;
        if (ok) self->mConvo = reply["lines"].isArray() ? reply["lines"] : LLSD::emptyArray();
        else self->mConvoError = error;
    });
}

F32 WolfGameConsole::conversationsSection(F32 l, F32 r, F32 top, const std::string& adv, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = sectionHead(l, r, top, "Conversations", p);
    const bool open = mConvoAdv == adv;
    {
        const F32 bw = 170.f;
        button(l, y - 26.f, l + bw, y, open ? "Hide conversations" : "Show conversations", p, flagsFor(H_ADV_CONVO, 0, !mConvoLoading));
        hit(l, y - 26.f, l + bw, y, H_ADV_CONVO, 0, open ? "Hide what you and the characters said" : "Read what you and the characters said in this adventure", true, !mConvoLoading);
        y -= 34.f;
    }
    y = paragraph(l, r, y, "Characters may be AI. Conversations are kept for 30 days; this adventure's hosts (and your teacher in a class) can read them.",
                  p.mDim, small) - 6.f;
    if (!open) return y - 4.f;
    if (mConvoLoading) return paragraph(l, r, y, "Loading your conversations...", p.mDim) - 8.f;
    if (!mConvoError.empty()) return paragraph(l, r, y, "Could not load them: " + mConvoError, DANGER_INK) - 8.f;
    mConvoLineIds.clear();
    if (!mConvo.isArray() || mConvo.size() == 0)
    {
        y = paragraph(l, r, y, "Nothing saved yet: talk to a character and your conversation appears here.", p.mDim) - 6.f;
    }
    else
    {
        // grouped by character, newest first within each (the lines come newest first)
        std::vector<std::string> order;
        std::map<std::string, std::vector<S32>> by;
        for (S32 i = 0; i < (S32)mConvo.size() && i < 200; ++i)
        {
            const std::string c = WolfGame::cleanText(mConvo[i]["cast"].asString(), 64);
            if (!by.count(c)) order.push_back(c);
            by[c].push_back(i);
        }
        for (const std::string& c : order)
        {
            const std::vector<S32>& rows = by[c];
            std::string who = WolfGame::cleanText(mConvo[rows.front()]["castName"].asString(), 64);
            if (who.empty()) who = c.empty() ? std::string("The story") : c;
            textL(display(16.f), who, l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
            y -= (F32)display(16.f)->getLineHeight() + 2.f;
            for (S32 i : rows)
            {
                const LLSD& ln = mConvo[i];
                const std::string kind = ln["who"].asString();
                const bool mine = kind == "player";
                const std::string speaker = mine ? std::string("You") : (kind == "gm" ? who + " (host)" : (kind == "system" ? std::string("Story") : who));
                const std::string text = WolfGame::cleanText(ln["text"].asString(), 1000, true);
                const F32 rep_w = mine ? 0.f : 70.f;
                textL(bold, speaker, l, y, mine ? p.mText : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, 140.f);
                const F32 ty = y;
                y = paragraph(l + 146.f, r - rep_w - 6.f, y, text, p.mText, small) - 4.f;
                if (!mine)
                {
                    const S64 id = ln["id"].asInteger();
                    const S32 arg = (S32)mConvoLineIds.size();
                    mConvoLineIds.push_back(id);
                    button(r - rep_w, ty - 20.f, r, ty, "Report", p, flagsFor(H_ADV_REPORT, arg));
                    hit(r - rep_w, ty - 20.f, r, ty, H_ADV_REPORT, arg, "Report this line to the grid's moderators (asks first)");
                    y = llmin(y, ty - 24.f);
                }
            }
            y -= 6.f;
        }
    }
    const F32 bw = 230.f;
    button(l, y - 26.f, l + bw, y, "Delete my conversations...", p, flagsFor(H_ADV_CONVO_DEL, 0) | BTN_DANGER);
    hit(l, y - 26.f, l + bw, y, H_ADV_CONVO_DEL, 0, "Delete your lines and the answers in this adventure (asks first)");
    button(r - 200.f, y - 26.f, r, y, "Make them forget me...", p, flagsFor(H_ADV_FORGET, 0) | BTN_DANGER);
    hit(r - 200.f, y - 26.f, r, y, H_ADV_FORGET, 0, "Delete what the characters remember about you (asks first)");
    return y - 40.f;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// input
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGameConsole::joinAdventure(const std::string& adv, const std::string& party, const std::string& code, const std::string& title)
{
    if (mJoinBusy)
    {
        setStatus("Wait for the last request to finish.", true);
        return;
    }
    mJoinBusy = true;
    const std::string name = title.empty() ? std::string("the adventure") : title;
    setStatus("Joining " + name + "...", false);
    // Source: BRAIN_API_V1.md §10.1 join {adventure, party?|"new", code?}.
    LLSD body;
    body["adventure"] = adv;
    if (!party.empty()) body["party"] = party;
    if (!code.empty()) body["code"] = code;
    LLHandle<WolfGameConsole> handle = getDerivedHandle<WolfGameConsole>();
    WolfAdventure::api("join", body, [handle, name](bool ok, const LLSD&, const std::string& error)
    {
        if (ok && WolfAdventure::instanceExists())
        {
            // the hands bring the quest on the next hello; the list shows "playing"
            WolfAdventure::instance().sendHello();
            WolfAdventure::instance().refreshHere();
        }
        if (handle.isDead()) return;
        WolfGameConsole* self = handle.get();
        self->mJoinBusy = false;
        if (ok) self->setStatus("You joined " + name + ". Your objective shows on screen and in this Journal.", false);
        else self->setStatus("Could not join " + name + ": " + error, true);
    });
}

bool WolfGameConsole::pressAdventure(S32 id, S32 arg)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const std::string not_connected = "Not sent: the viewer is not connected to a Wolf Territories region.";
    const WolfAdventure::Adventure* sel = wa.find(mJournalAdv);
    switch (id)
    {
    case H_ADV_TAB:
        mScroll[PAGE_JOURNAL] = 0.f;
        if (arg < 0)
        {
            mJournalHere = true;
            if (!wa.hereLoading()) wa.refreshHere();
        }
        else if (arg < (S32)mJournalTabs.size())
        {
            mJournalHere = false;
            mJournalAdv = mJournalTabs[arg];
            wa.sendJournalPage(mJournalAdv, 0);
        }
        return true;
    case H_ADV_READY:
        if (sel && sel->mHasQuest)
        {
            const bool want = !sel->mQuest.mParty.mReady;
            if (wa.sendReady(mJournalAdv, want)) setStatus(want ? "You told your party you are ready." : "You told your party you are not ready.", false);
            else setStatus(not_connected, true);
        }
        return true;
    case H_ADV_MAP:
    case H_ADV_TP:
        mConfirmAdv = mJournalAdv;
        askConfirm(id == H_ADV_MAP ? CONFIRM_ADV_MAP : CONFIRM_ADV_TP);
        return true;
    case H_ADV_QUIZ:   // [WOLF ADVENTURE AUDIT 2026-10-17] the quiz plate again
        if (arg >= 0 && arg < (S32)mQuizRowIds.size()) wa.quizReopen(mQuizRowIds[arg].first, mQuizRowIds[arg].second);
        return true;
    case H_ADV_CARD:
        if (arg >= 0 && arg < (S32)mWaitingIds.size())
        {
            wa.bringBack(mWaitingIds[arg].first, mWaitingIds[arg].second);
            WolfAdventureUI::focusCard();
        }
        return true;
    case H_ADV_CHAPTER:
        if (arg >= 0 && arg < (S32)mChapterActs.size())
        {
            const std::string& act = mChapterActs[arg];
            if (mOpenChapters.count(act)) mOpenChapters.erase(act);
            else mOpenChapters.insert(act);
        }
        return true;
    case H_ADV_FILTER:
        if (arg >= 0 && arg < FILTER_COUNT) mEntryFilter = FILTERS[arg].mKind;
        return true;
    case H_ADV_PAGE:
        if (sel && sel->mJournal.mValid)
        {
            const S32 page = llclamp(sel->mJournal.mPage + arg, 0, sel->mJournal.mPages - 1);
            if (wa.sendJournalPage(mJournalAdv, page)) setStatus(llformat("Asking for page %d...", page + 1), false);
            else setStatus(not_connected, true);
        }
        return true;
    case H_ADV_HANDOUT:
        if (arg >= 0 && arg < (S32)mHandoutIds.size()) WolfAdventureUI::showHandout(mJournalAdv, mHandoutIds[arg]);
        return true;
    case H_ADV_JOIN:
    {
        const S32 row = arg / JOIN_ROW, k = arg % JOIN_ROW;
        const LLSD& list = wa.here();
        if (row < 0 || row >= (S32)mHereIds.size() || !list.isArray() || row >= (S32)list.size()) return true;
        const LLSD& item = list[row];
        if (WolfGame::cleanText(item["id"].asString(), 64) != mHereIds[row]) return true;   // the list changed under the click
        std::string party;
        if (k == JOIN_NEW) party = "new";
        else if (k > 0)
        {
            const LLSD& parties = item["parties"];
            if (!parties.isArray() || k - 1 >= (S32)parties.size()) return true;
            party = WolfGame::cleanText(parties[k - 1]["party"].asString(), 64);
        }
        joinAdventure(mHereIds[row], party, std::string(), WolfGame::cleanText(item["title"].asString(), 64));
        return true;
    }
    case H_ADV_CODE:
        if (arg >= 0 && arg < (S32)mHereIds.size())
        {
            mCodeAdv = mHereIds[arg];
            mHits.setFocus(H_ADV_CODE, arg);
            mKeyboardNav = true;
            setFocus(true);
        }
        return true;
    case H_ADV_CLASSJOIN:
        if (arg >= 0 && arg < (S32)mHereIds.size())
        {
            const std::string adv = mHereIds[arg];
            const std::string code = mClassCodes[adv];
            if (code.empty())
            {
                setStatus("Type the class code your teacher gave you first.", true);
                mCodeAdv = adv;
                mHits.setFocus(H_ADV_CODE, arg);
                return true;
            }
            // §6 3.3.7: remembered per adventure for next time
            LLSD codes = gSavedPerAccountSettings.getLLSD("WolfAdventureClassCodes");
            if (!codes.isMap()) codes = LLSD::emptyMap();
            codes[adv] = code;
            gSavedPerAccountSettings.setLLSD("WolfAdventureClassCodes", codes);
            const LLSD& list = wa.here();
            const std::string title = list.isArray() && arg < (S32)list.size() ? WolfGame::cleanText(list[arg]["title"].asString(), 64) : std::string();
            joinAdventure(adv, std::string(), code, title);
        }
        return true;
    case H_ADV_LEAVE:
        mConfirmAdv = arg >= 0 ? (arg < (S32)mHereIds.size() ? mHereIds[arg] : std::string()) : mJournalAdv;
        if (!mConfirmAdv.empty()) askConfirm(CONFIRM_ADV_LEAVE);
        return true;
    case H_ADV_OPEN:
        if (arg >= 0 && arg < (S32)mHereIds.size())
        {
            mJournalHere = false;
            mJournalAdv = mHereIds[arg];
            mScroll[PAGE_JOURNAL] = 0.f;
            wa.sendJournalPage(mJournalAdv, 0);
        }
        return true;
    case H_ADV_REFRESH:
        wa.refreshHere();
        return true;
    case H_ADV_CONVO:
        if (mConvoAdv == mJournalAdv && !mConvoLoading)
        {
            mConvoAdv.clear();   // the second click hides them
            mConvo = LLSD();
        }
        else
        {
            loadConversations(mJournalAdv);
        }
        return true;
    case H_ADV_CONVO_DEL:
        mConfirmAdv = mJournalAdv;
        askConfirm(CONFIRM_ADV_CONVO_DEL);
        return true;
    case H_ADV_FORGET:
        mConfirmAdv = mJournalAdv;
        askConfirm(CONFIRM_ADV_FORGET);
        return true;
    case H_ADV_REPORT:
        if (arg >= 0 && arg < (S32)mConvoLineIds.size())
        {
            mConfirmAdv = mJournalAdv;
            mConfirmLine = mConvoLineIds[arg];
            askConfirm(CONFIRM_ADV_REPORT);
        }
        return true;
    case H_ADV_STORYCAM:
    {
        const bool on = !WolfAdventure::storyCamera();
        gSavedPerAccountSettings.setBOOL("WolfAdventureStoryCamera", on);
        setStatus(on ? "Story camera on." : "Story camera off: adventures will not move your camera.", false);
        return true;
    }
    case H_ADV_MOTION:
    {
        const bool on = !WolfAdventure::reduceMotion();
        gSavedPerAccountSettings.setBOOL("WolfAdventureReduceMotion", on);
        setStatus(on ? "Reduce motion on." : "Reduce motion off.", false);
        return true;
    }
    case H_ADV_VOICEVOL:
    {
        const F32 v = llclamp(WolfAdventure::voiceVolume() + (arg == 1 ? 0.1f : -0.1f), 0.f, 1.f);
        gSavedPerAccountSettings.setF32("WolfAdventureVoiceVolume", (F32)llround(v * 10.f) / 10.f);
        setStatus(llformat("Character voices at %d%%.", (S32)llround(v * 100.f)), false);
        return true;
    }
    case H_ADV_AITAG:
    {
        const bool on = !gSavedPerAccountSettings.getBOOL("WolfAdventureShowAiTag");
        gSavedPerAccountSettings.setBOOL("WolfAdventureShowAiTag", on);
        setStatus(on ? "The AI tag shows on lines written by AI." : "The AI tag is hidden (it still shows in class play).", false);
        return true;
    }
    default:
        return false;
    }
}

bool WolfGameConsole::adventureConfirmText(std::string& title, std::string& body, std::string& yes, std::string& no) const
{
    const WolfAdventure::Adventure* a = WolfAdventure::instance().find(mConfirmAdv);
    switch (mConfirm)
    {
    case CONFIRM_ADV_MAP:
    {
        const std::string what = a && !a->mQuest.mMarker.mLabel.empty() ? a->mQuest.mMarker.mLabel : std::string("the objective");
        title = "Show " + what + " on the world map?";
        body = "The map shows where it is" + (a ? " on " + a->mQuest.mMarker.mRegion : std::string()) + ". You choose whether to teleport from there.";
        yes = "Show on map";
        no = "Cancel";
        return true;
    }
    case CONFIRM_ADV_TP:
    {
        const std::string what = a && !a->mQuest.mMarker.mLabel.empty() ? a->mQuest.mMarker.mLabel : std::string("the objective");
        title = "Teleport to " + what + "?";
        body = a ? llformat("You will be taken to %s (%d, %d, %d). Your party is not moved with you.", a->mQuest.mMarker.mRegion.c_str(),
                            (S32)llround(a->mQuest.mMarker.mPos.mV[VX]), (S32)llround(a->mQuest.mMarker.mPos.mV[VY]),
                            (S32)llround(a->mQuest.mMarker.mPos.mV[VZ]))
                 : std::string("You will be taken to the objective.");
        yes = "Teleport";
        no = "Stay here";
        return true;
    }
    case CONFIRM_ADV_LEAVE:
    {
        std::string name = a && !a->mQuest.mSrc.empty() ? a->mQuest.mSrc : std::string();
        if (name.empty())
        {
            const LLSD& list = WolfAdventure::instance().here();
            for (size_t i = 0; list.isArray() && i < (size_t)list.size(); ++i)
            {
                if (WolfGame::cleanText(list[(LLSD::Integer)i]["id"].asString(), 64) == mConfirmAdv)
                {
                    name = WolfGame::cleanText(list[(LLSD::Integer)i]["title"].asString(), 64);
                }
            }
        }
        title = "Leave " + (name.empty() ? std::string("this adventure") : name) + "?";
        body = "You leave the adventure and your party. You can join again from Adventures here.";
        yes = "Leave";
        no = "Stay";
        return true;
    }
    case CONFIRM_ADV_CONVO_DEL:
        // Source: BRAIN_API_V1.md §10.3 "The player ... may delete them"
        title = "Delete your conversations?";
        body = "Your lines and the characters' answers in this adventure are deleted from the grid. It cannot be undone.";
        yes = "Delete";
        no = "Keep them";
        return true;
    case CONFIRM_ADV_FORGET:
        // Source: BRAIN_API_V1.md §10.3 - memories are deleted "at once when the player deletes them from the Journal"
        title = "Make the characters forget you?";
        body = "What this adventure's characters remember about you is deleted. They will greet you as a stranger.";
        yes = "Forget me";
        no = "Cancel";
        return true;
    case CONFIRM_ADV_REPORT:
        // Source: BRAIN_API_V1.md §5.5 chatlog_report - the line is kept, with names, for moderation
        title = "Report this line?";
        body = "The line is kept, with names, for the grid's moderators to look at.";
        yes = "Report";
        no = "Cancel";
        return true;
    default:
        return false;
    }
}

void WolfGameConsole::confirmAdventure(EConfirm what)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Adventure* a = wa.find(mConfirmAdv);
    if (what == CONFIRM_ADV_MAP || what == CONFIRM_ADV_TP)
    {
        if (!a || !a->mQuest.mMarker.mOn)
        {
            setStatus("The objective has no marker now.", true);
            return;
        }
        const WolfAdventure::QuestMarker& m = a->mQuest.mMarker;
        LLVector3d global;
        const bool known = WolfAdventure::markerGlobal(a->mQuest, global);
        if (what == CONFIRM_ADV_MAP)
        {
            // the world map finds the region by name and tracks the point; Teleport is on the map
            LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance();
            if (!map)
            {
                setStatus("The world map is not available.", true);
                return;
            }
            LLFloaterReg::showInstance("world_map", "center");
            if (known) map->trackLocation(global);
            else map->trackURL(m.mRegion, (S32)llround(m.mPos.mV[VX]), (S32)llround(m.mPos.mV[VY]), (S32)llround(m.mPos.mV[VZ]));
            return;
        }
        if (!known)
        {
            setStatus("Where " + m.mRegion + " is is not known yet - use Show on map, then Teleport there.", true);
            return;
        }
        gAgent.teleportViaLocation(global);
        setStatus("Teleporting to " + (m.mLabel.empty() ? m.mRegion : m.mLabel) + "...", false);
        return;
    }
    if (what == CONFIRM_ADV_CONVO_DEL || what == CONFIRM_ADV_FORGET || what == CONFIRM_ADV_REPORT)
    {
        const std::string adv = mConfirmAdv;
        LLSD body;
        std::string action, done_text;
        if (what == CONFIRM_ADV_REPORT)
        {
            action = "chatlog_report";
            body["line"] = (LLSD::Integer)mConfirmLine;   // a wolfadv_chatlog id (BIGINT; fits 32 bits for years)
            done_text = "Reported. The grid's moderators can see the line.";
        }
        else
        {
            action = what == CONFIRM_ADV_CONVO_DEL ? "chatlog_delete" : "memory_delete";
            body["adventure"] = adv;
            done_text = what == CONFIRM_ADV_CONVO_DEL ? "Your conversations in this adventure were deleted." : "The characters have forgotten you.";
        }
        setStatus("Sending...", false);
        LLHandle<WolfGameConsole> handle = getDerivedHandle<WolfGameConsole>();
        WolfAdventure::api(action, body, [handle, adv, done_text, what](bool ok, const LLSD&, const std::string& error)
        {
            if (handle.isDead()) return;
            WolfGameConsole* self = handle.get();
            self->setStatus(ok ? done_text : "Not done: " + error, !ok);
            if (ok && what == CONFIRM_ADV_CONVO_DEL && self->mConvoAdv == adv) self->loadConversations(adv);
        });
        return;
    }
    if (what == CONFIRM_ADV_LEAVE)
    {
        const std::string adv = mConfirmAdv;
        setStatus("Leaving...", false);
        LLSD body;
        body["adventure"] = adv;
        LLHandle<WolfGameConsole> handle = getDerivedHandle<WolfGameConsole>();
        // Source: BRAIN_API_V1.md §10.1 leave {adventure}; the hands then send quest clear (§2.1).
        WolfAdventure::api("leave", body, [handle](bool ok, const LLSD&, const std::string& error)
        {
            if (ok && WolfAdventure::instanceExists())
            {
                WolfAdventure::instance().sendHello();
                WolfAdventure::instance().refreshHere();
            }
            if (handle.isDead()) return;
            handle.get()->setStatus(ok ? std::string("You left the adventure.") : "Could not leave: " + error, !ok);
        });
    }
}

bool WolfGameConsole::adventureKey(KEY key, MASK mask)
{
    if (mCodeAdv.empty()) return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (!f || f->mId != H_ADV_CODE)
    {
        mCodeAdv.clear();
        return false;
    }
    std::string& code = mClassCodes[mCodeAdv];
    if (key == KEY_BACKSPACE && mask == MASK_NONE)
    {
        LLWString w = utf8str_to_wstring(code);
        if (!w.empty()) w.pop_back();
        code = wstring_to_utf8str(w);
        return true;
    }
    if (key == KEY_RETURN && mask == MASK_NONE)
    {
        pressAdventure(H_ADV_CLASSJOIN, f->mArg);
        return true;
    }
    if (key == 'V' && (mask & MASK_CONTROL))
    {
        // §6 3.3.8: the class code may be pasted
        LLWString pasted;
        if (LLClipboard::instance().pasteFromClipboard(pasted))
        {
            std::string add = WolfGame::cleanText(wstring_to_utf8str(pasted), CODE_MAX);
            LLStringUtil::trim(add);
            code = WolfGame::cleanText(code + add, CODE_MAX);
        }
        return true;
    }
    return false;
}

bool WolfGameConsole::adventureChar(llwchar c)
{
    if (mCodeAdv.empty()) return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (!f || f->mId != H_ADV_CODE) return false;
    if (c < 0x20 || c == 0x7f) return true;
    std::string& code = mClassCodes[mCodeAdv];
    LLWString w = utf8str_to_wstring(code);
    if (w.size() < CODE_MAX) w.push_back(c);
    code = wstring_to_utf8str(w);
    return true;
}
