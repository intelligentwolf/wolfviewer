/**
 * @file wolffloateradventureguide.cpp
 * @brief WolfViewer: the Adventure Guide. See wolffloateradventureguide.h.
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

#include "wolffloateradventureguide.h"

#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "lltooltip.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "wolffloateradventurestudio.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureGuideView> r_wolf_adventure_guide_view("wolf_adventure_guide_view");

using namespace WolfGameDraw;

namespace
{
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    // The website guide (gridmanager/wolfstoryguide.php ?p=<slug>; the slugs are wsg_pages() in wolfstoryguide_lib.php).
    const char* GUIDE_URL = "https://www.wolf-grid.com/story-guide?p=";

    struct Help { const char* mTitle; const char* mText; const char* mExample; const char* mSlug; };

    // One per Start here step (WolfAdventureStudioView::EWizStep order).
    const Help STEP_HELP[WolfAdventureStudioView::W_COUNT] = {
        { "1. What kind", "Choose the kind closest to your idea. You get a short story that already works - three or four steps, a character "
          "and a prize - and you change it into your own. Give it a name players will see, and say who it is for.",
          "Treasure hunt, named \"The Lost Gold of Wolf Creek\", rated General.", "first-adventure" },
        { "2. Where", "An adventure runs on one or more regions. Stand on your land and press Use this region: on land you hold it is "
          "ready at once. On someone else's land the holder is asked and must say yes; you can keep working while you wait.",
          "Standing on Sandbox, press Use this region (Sandbox).", "you-need" },
        { "3. The story", "The story is a list of steps. Each step is one thing to do (it shows in the players' Journal), and it is done "
          "when they reach a place, talk to someone, touch an object, or all press Ready. Then the next step starts; after the last "
          "one the story ends.", "Step 1: Find the old well - done when they reach a place (the well).", "plan" },
        { "4. Characters", "Characters stand in the world and talk. Give each a name (two words), say who they are, and write what they "
          "say. With written lines they say exactly that, for free. With AI they talk freely in character, and each talk turn costs a "
          "little from your credits, up to a daily cap you set.", "Ma Dalton, \"Runs the saloon\": \"Howdy, stranger. Heard about the gold?\"",
          "characters" },
        { "5. Places", "Every place the story uses must be put in the world. Walk to it and press Use my position; for a place players "
          "reach, set how big the circle is. While this step is open the places are drawn in the world so you can check them.",
          "Walk to the well, press Use my position, make it 6 metres across.", "places-triggers" },
        { "6. How it ends", "Write what players read when they finish. You can give a prize: money or an item from the region's game. "
          "Prizes only move on games you own or whose owner linked your adventure.", "\"You found the gold!\" and 50 money.", "quests-rewards" },
        { "7. The look", "Make it yours: a cover picture and a logo for the Adventures list, a title card players see when they start, and "
          "the colours of their story panel. Pick a colour set, or your own two colours - the Studio tells you if they are hard to read.",
          "A wanted-poster cover, the Frontier colours, title card \"The Lost Gold\".", "first-adventure" },
        { "8. Test Play", "Test Play runs your story for you alone, on your region. Walk the steps as a player would, or press Fire to "
          "pretend a step happened. Try it with and without AI. Nothing a test gives is real.",
          "Start the test, walk to the well, see step 2 appear in your Journal.", "test-play" },
        { "9. Publish", "Publishing puts the story live for everyone on its regions. Anything marked Must fix has to be fixed first - "
          "press Fix beside it. You can change it and publish again whenever you like; players carry on at their next step.",
          "Fix \"Step 2 needs a place\", then Publish.", "publish" } };

    // One per Studio page (WolfAdventureStudioView::EPage order; Start here is drawn from STEP_HELP).
    const Help PAGE_HELP[] = {
        { "Adventures and regions", "Your adventures in full (duplicate, delete, quick make) and any requests to run adventures on your regions.",
          "Open \"Lone Wolf's Wild West\" to carry on where you left off.", "first-adventure" },
        { "Import", "Bring in a story you already wrote: a Wolf Story notecard (and its Look notecards) or a pack file. Nothing is saved "
          "until you press Use as draft.", "Drop the STORY notecard here, choose the region, press Import.", "import-export" },
        { "Outline", "The whole story: acts hold scenes, scenes hold beats (a beat is one step). Select one to add after it, move it or "
          "open it. A new beat goes into the chain after the selected one. The flow shows where each beat leads.",
          "Select Beat 2, press Add beat: the new beat sits between beat 2 and beat 3.", "plan" },
        { "Cast", "Each character's sheet: name and look, who they are, what they know, their secrets, lines, voice, their day, and "
          "what they may do. Use what I'm wearing saves your outfit as their look.", "Ma Dalton: \"Knows who robbed the stage, never tells the sheriff.\"",
          "characters" },
        { "Places", "Spots, areas, paths and chests, put in the world with the mouse while this page is open. Escape stops placing.",
          "Draw circle round the saloon door: players arriving there finish the step.", "places-triggers" },
        { "Story", "One scene or beat in detail: its stage, what happens when it starts, its triggers (each reads as a sentence), "
          "decisions and hints.", "When a player talks to Ma Dalton -> then go to \"Find the map\".", "places-triggers" },
        { "Rules", "The rules preset (dice, stats and checks) closest to your story, and your changes to it. Changed values are marked.",
          "Wild West: grit, draw and luck checks.", "rules" },
        { "Lore", "What characters know (story cards), cited facts (source cards), encounters, rewards, handouts, learning goals and "
          "quizzes.", "A handout: the treasure map picture.", "quests-rewards" },
        { "Test Play", "Play the draft from any beat, alone. The overlay shows where the story is, its flags, and Fire buttons for every "
          "trigger.", "Start from \"Find the map\", fire \"talked to Ma\".", "test-play" },
        { "Publish", "Checks, regions, hosts and teachers, history, and Publish.", "Request Sandbox, then Publish.", "publish" },
        { "AI and voice", "AI characters or written lines only, the daily caps, voices, and what was spent today.",
          "AI characters, at most 100 credits a day.", "characters" },
        { "Cast talk", "Give each character a voice and talk to them now, on any beat, to hear how they sound.",
          "Ask Ma Dalton \"What did you hear?\" on beat 2.", "voices" },
        { "Book and assist", "Give it a book: a public-domain text or your own outline becomes a draft to edit, with a quote first. "
          "The assistant gives you ideas only you see.", "Paste a short story, 3 acts, Get a quote.", "import-export" },
        { "Run", "Run it live: take control from the AI Director, pause, and hand it back.", "Take control, start the showdown, hand it back.",
          "run-live" },
        // [WOLF STORY FIRST 2026-10-17] SP_WIZARD (its steps have their own help above); [SIMPLE STUDIO 2026-10-17] the three pages
        { "Start here", "The step-by-step pages, with no AI.", "Step 1: what kind of adventure.", "first-adventure" },
        { "Your story", "Type an idea, paste a story, or load a book, then press Build my game. You see what it costs before anything is charged.",
          "\"A Wild West bank robbery where the players can side with the outlaws or the sheriff.\"", "first-adventure" },
        { "Your characters", "Check each character's name, who they are and the first thing they say. Use what I'm wearing gives them your look; "
          "pick a voice and press Hear them.", "Ma Dalton, \"Runs the saloon\": \"Howdy, stranger. Heard about the gold?\"", "characters" },
        { "Gameplay", "Each box is one moment of your story. Press Next place, walk to where it happens and press Place it here, then tell "
          "the helper what happens there. When every box is placed, press Play from the start.", "Walk to the old well, press Place it here, type "
          "\"the sheriff is waiting here\".", "places-triggers" } };
    const S32 PAGE_HELP_COUNT = (S32)(sizeof(PAGE_HELP) / sizeof(PAGE_HELP[0]));

    // [SIMPLE STUDIO 2026-10-17] the three steps (SIMPLE_STUDIO_SPEC.md), as the Guide shows them
    struct Step { const char* mTitle; const char* mLine; };
    const Step STEPS[3] = {
        { "Your story", "Type an idea, paste a story or load a book, then press Build my game." },
        { "Your characters", "Check their names and what they say; dress them in what you wear." },
        { "Gameplay", "Walk your land: Next place, Place it here, say what happens, then Play from the start." } };
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the view
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfAdventureGuideView::WolfAdventureGuideView(const Params& p)
:   LLUICtrl(p)
{
}

WolfAdventureStudioView* WolfAdventureGuideView::studio() const
{
    WolfFloaterAdventureStudio* f = LLFloaterReg::findTypedInstance<WolfFloaterAdventureStudio>("wolf_adventure_studio");
    return f && f->getVisible() ? f->view() : nullptr;
}

void WolfAdventureGuideView::draw()
{
    mHits.clear();
    const Palette& p = gamePalette();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    LLGLSUIDefault gls_ui;
    frame(0.f, 0.f, W, H, p, 1.f, 14.f);
    const F32 M = 14.f;
    // the title band (under the floater's drag strip), with Close
    textL(bold, "GUIDE", M + 2.f, H - 12.f, p.mDim);
    {
        const F32 xc = W - M - 8.f, yc = H - 19.f;
        closeX(xc, yc, 5.f, mHits.isHover(GH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(GH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
        mHits.add(xc - 11.f, yc - 11.f, xc + 11.f, yc + 11.f, GH_CLOSE, 0, "Hide the Guide (the Studio's Guide button shows it again)");
    }
    WolfAdventureStudioView* sv = studio();
    const F32 l = M + 4.f, r = W - M - 4.f, top = H - 40.f, bottom = 12.f;
    const LLRect clip_r((S32)M, (S32)top, (S32)(W - M), (S32)bottom);
    F32 y = top + mScroll;
    {
        LLLocalClipRect clip(clip_r);
        auto paraL = [&](const std::string& text, const LLColor4& c, const LLFontGL* f)
        {
            for (const std::string& line : wrap(f, text, r - l, 40)) { textL(f, line, l, y, c); y -= (F32)f->getLineHeight(); }
        };
        // ---- the three steps ----
        textL(display(18.f), "Three steps to your game", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= (F32)display(18.f)->getLineHeight() + 8.f;
        if (!sv) paraL("Open the Adventure Studio (Wolf World > Adventure Studio) to see where you are.", p.mDim, small);
        const S32 here = sv ? sv->simpleStep() : -1;
        for (S32 i = 0; i < 3; ++i)
        {
            const std::vector<std::string> lines = wrap(small, STEPS[i].mLine, r - l - 36.f, 3);
            const F32 rh = 8.f + slh + (F32)lines.size() * slh + 8.f;
            const bool is_here = here == i;
            if (is_here) rect(l, y - rh, r, y, fade(p.mAccent, 0.22f));
            else if (mHits.isHover(GH_STEP, i)) rect(l, y - rh, r, y, fade(p.mAccent, 0.12f));
            // the number in a ring, the step's name, its one line; "you are here" in words (never colour alone)
            disc(l + 12.f, y - 8.f - slh * 0.5f, 9.f, is_here ? p.mAccent : fade(p.mDim, 0.6f));
            textL(bold, llformat("%d", i + 1), l + 12.f, y - 8.f - slh * 0.5f, p.mBaseTop, LLFontGL::HCENTER, LLFontGL::VCENTER);
            textL(is_here ? bold : small, STEPS[i].mTitle, l + 30.f, y - 8.f, is_here ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 110.f);
            if (is_here) textL(small, "you are here", r - 4.f, y - 8.f, p.mAccent, LLFontGL::RIGHT, LLFontGL::TOP);
            F32 ly = y - 8.f - slh;
            for (const std::string& ln : lines) { textL(small, ln, l + 30.f, ly, p.mDim); ly -= slh; }
            const bool can = sv && sv->simpleCanGo(i);
            mHits.add(l, llmax(y - rh, (F32)bottom), r, llmin(y, top), GH_STEP, i, can ? llformat("Go to %s", STEPS[i].mTitle) : std::string("Build your game first (Your story)"), true, can);
            if (mKeyboardNav && hasFocus() && mHits.isFocused(GH_STEP, i)) focusRing(l, y - rh, r, y, p);
            y -= rh + 4.f;
        }
        y -= 8.f;
        // ---- this page: its help (every page has some; the three pages' is short, an expert page's says what it is for) ----
        const S32 step = sv ? sv->guideStep() : -1;
        const S32 page = sv ? sv->guidePage() : 0;
        const Help* h = step >= 0 && step < WolfAdventureStudioView::W_COUNT ? &STEP_HELP[step]
                      : (page >= 0 && page < PAGE_HELP_COUNT ? &PAGE_HELP[page] : &PAGE_HELP[0]);
        if (sv)
        {
            textL(bold, caps("This page"), l, y, p.mAccent);
            rect(l, y - slh - 3.f, r, y - slh - 2.f, fade(p.mAccent, 0.3f));
            y -= slh + 10.f;
            textL(display(18.f), h->mTitle, l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
            y -= (F32)display(18.f)->getLineHeight() + 6.f;
            paraL(h->mText, p.mText, LLFontGL::getFontSansSerif());
            y -= 8.f;
            {
                const std::vector<std::string> lines = wrap(small, h->mExample, r - l - 20.f, 4);
                const F32 eh = 10.f + slh + (F32)lines.size() * slh + 8.f;
                card(l, y - eh, r, y, p, 4.f);
                textL(bold, "EXAMPLE", l + 10.f, y - 8.f, p.mAccent);
                F32 ey = y - 8.f - slh;
                for (const std::string& ln : lines) { textL(small, ln, l + 10.f, ey, p.mText); ey -= slh; }
                y -= eh + 10.f;
            }
        }
        {
            const std::string lab = "Read more on the website";
            const F32 bw = (F32)bold->getWidthF32(lab) + 24.f;
            const std::string url = std::string(GUIDE_URL) + h->mSlug;
            button(l, y - 28.f, l + bw, y, lab, p, (mHits.isHover(GH_WEB, 0) ? BTN_HOVER : 0) | (mHits.isPressed(GH_WEB, 0) ? BTN_PRESSED : 0)
                   | (mKeyboardNav && hasFocus() && mHits.isFocused(GH_WEB, 0) ? BTN_FOCUS : 0));
            mHits.add(l, y - 28.f, l + bw, y, GH_WEB, 0, "Opens " + url + " in your web browser");
            y -= 40.f;
        }
    }
    mContentH = top + mScroll - y + 8.f;
    const F32 view_h = top - bottom;
    mScroll = llclamp(mScroll, 0.f, llmax(0.f, mContentH - view_h));
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureGuideView::press(S32 id, S32 arg)
{
    switch (id)
    {
    case GH_CLOSE:
        // [SIMPLE STUDIO 2026-10-17] shut until the Studio's Guide button is pressed again (nothing remembered)
        if (LLFloater* f = getParentByType<LLFloater>()) f->closeFloater();
        break;
    case GH_WEB:
    {
        WolfAdventureStudioView* sv = studio();
        const S32 step = sv ? sv->guideStep() : -1;
        const S32 page = sv ? sv->guidePage() : 0;
        const Help* h = step >= 0 && step < WolfAdventureStudioView::W_COUNT ? &STEP_HELP[step]
                      : (page >= 0 && page < PAGE_HELP_COUNT ? &PAGE_HELP[page] : &PAGE_HELP[0]);
        LLWeb::loadURLExternal(std::string(GUIDE_URL) + h->mSlug);
        break;
    }
    case GH_STEP:
        if (WolfAdventureStudioView* sv = studio()) sv->goSimple(arg);
        break;
    default:
        break;
    }
}

bool WolfAdventureGuideView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;
    setFocus(true);
    mKeyboardNav = false;
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAdventureGuideView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    const S32 pid = mHits.pressedId(), parg = mHits.pressedArg();
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == pid && h->mArg == parg && h->mEnabled) press(pid, parg);
    return true;
}

void WolfAdventureGuideView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
}

bool WolfAdventureGuideView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(h->mEnabled ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfAdventureGuideView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    mScroll = llmax(0.f, mScroll + (F32)clicks * 40.f);
    return true;
}

bool WolfAdventureGuideView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfAdventureGuideView::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled) { mKeyboardNav = true; press(f->mId, f->mArg); }
        return true;
    }
    case KEY_ESCAPE:
        press(GH_CLOSE, 0);   // the same as its close button: hidden until the Studio's Guide button
        return true;
    case KEY_UP: mScroll = llmax(0.f, mScroll - 40.f); return true;
    case KEY_DOWN: mScroll += 40.f; return true;
    default: return false;
    }
}

bool WolfAdventureGuideView::handleUnicodeCharHere(llwchar uni_char)
{
    if (uni_char != ' ') return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (f && f->mEnabled) { mKeyboardNav = true; press(f->mId, f->mArg); }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventureGuide::WolfFloaterAdventureGuide(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureGuide::postBuild()
{
    // in front of the drag handle, as WolfFloaterRoleplay::postBuild explains
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterAdventureGuide::draw()
{
    // Wolf Territories only, as the Studio (HUD_MESSAGES_V1.md §5.1)
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}
