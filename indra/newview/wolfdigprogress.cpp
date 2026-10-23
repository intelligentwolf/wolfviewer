/**
 * @file wolfdigprogress.cpp
 * @brief WolfViewer: the painted "working" plate of the Tunnel tool and the Cave Builder.
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

// [WOLF TUNNELS 2026-10-23] See wolfdigprogress.h.

#include "llviewerprecompiledheaders.h"

#include "wolfdigprogress.h"

#include <cmath>
#include <vector>

#include "llbutton.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "lltexteditor.h"
#include "lluictrlfactory.h"
#include "wolfburrow.h"
#include "wolfpaint.h"

using namespace WolfPaint;

namespace
{
    // ---- palette: the burrow (fixed colours, not the skin's - a painted plate, as the uploaders; every line of text sits on
    // a plate drawn under it, so light and dark skins read the same). Contrast (WCAG 2.x relative luminance), worked out for
    // each text pair (region-terrain-work/cave-lights/check/dig_contrast.py):
    //   AMBER #ffbd52 (the word) on PLATE_TOP #2e1f14: 9.6:1, on PLATE_BOTTOM #170f0a: 11.4:1
    //   CREAM #f2ebd6 (the step line) on PLATE_TOP: 13.4:1
    //   MUTED #d1c2a8 (the percent) on PLATE_TOP: 9.1:1
    //   GREEN #73d98c (done) on PLATE_TOP: 9.1:1
    //   ERR_INK #ffb385 on ERR_PLATE #2e0d0f: 10.2:1 (the same pair as llfloaterwolfavatars.cpp:79)
    const LLColor4 AMBER(1.f, 0.74f, 0.32f, 1.f);
    const LLColor4 AMBER_DEEP(0.86f, 0.54f, 0.16f, 1.f);
    const LLColor4 CREAM(0.95f, 0.92f, 0.84f, 1.f);
    const LLColor4 MUTED(0.82f, 0.76f, 0.66f, 1.f);
    const LLColor4 GREEN(0.45f, 0.85f, 0.55f, 1.f);
    const LLColor4 PLATE_TOP(0.18f, 0.12f, 0.08f, 1.f);
    const LLColor4 PLATE_BOTTOM(0.09f, 0.06f, 0.04f, 1.f);
    const LLColor4 EDGE(0.55f, 0.40f, 0.24f, 1.f);
    const LLColor4 TRACK(0.05f, 0.035f, 0.02f, 1.f);
    const LLColor4 ERR_INK(1.f, 0.70f, 0.52f, 1.f);
    const LLColor4 ERR_PLATE(0.18f, 0.05f, 0.06f, 1.f);
    const LLColor4 ERR_EDGE(0.85f, 0.30f, 0.25f, 1.f);

    /** How long a finished job's tick stays (the window's status line keeps the words after). */
    constexpr F64 DONE_SECS = 6.0;
    /** Each fun word stays this long, fading in and out at its ends. */
    constexpr F32 WORD_SECS = 1.8f;

    /** The words for what the step is doing, picked by the step's own line (the jobs' status lines in wolffloatertunnels.cpp,
     *  wolfcave.cpp and wolfcavelights.cpp). They only colour the plate: the step line and the bar under them are the facts. */
    const std::vector<std::string>& wordsFor(const std::string& line)
    {
        static const std::vector<std::string> SAVE = { "Surveying...", "Checking the deeds...", "Marking out the ground..." };
        static const std::vector<std::string> ROCK = { "Quarrying...", "Chiselling the rock...", "Shaping the stone...", "Moling..." };
        static const std::vector<std::string> REZ = { "Heaving the rock in...", "Lowering it into place...", "Tamping it down..." };
        static const std::vector<std::string> DIG = { "Tunnelling...", "Digging...", "Moling...", "Burrowing...", "Shovelling...",
                                                      "Shoring up the roof..." };
        static const std::vector<std::string> LIGHT = { "Lighting the torches...", "Hanging the lanterns...", "Striking a light...",
                                                        "Trimming the wicks..." };
        static const std::vector<std::string> DARK = { "Snuffing the torches...", "Taking the lamps down...", "Blowing out the flames..." };
        static const std::vector<std::string> FILL = { "Filling it in...", "Backfilling...", "Patting the earth flat...", "Tidying up..." };
        // [WOLF TERRAFORM 2026-10-23] the Terraform window's jobs (wolfterraform.cpp Jobs: each line starts with its verb), DESIGN.md
        // 3.6 "Grading...", "Levelling...", "Laying the road...", "Carving the river bed...", "Cutting the terraces..."
        static const std::vector<std::string> LEVEL = { "Levelling...", "Flattening...", "Making a pad..." };
        static const std::vector<std::string> RAMP = { "Grading...", "Grading the ramp...", "Raking the slope..." };
        static const std::vector<std::string> TERRACE = { "Cutting the terraces...", "Stepping the hill...", "Cutting shelves..." };
        static const std::vector<std::string> SMOOTH = { "Smoothing...", "Raking it smooth...", "Softening the bumps..." };
        static const std::vector<std::string> ROAD = { "Laying the road...", "Grading...", "Rolling it flat..." };
        static const std::vector<std::string> RIVER = { "Carving the river bed...", "Digging the channel...", "Letting the water through..." };
        static const std::vector<std::string> SHORE = { "Shaping the shore...", "Raking the sand...", "Smoothing the beach..." };
        static const std::vector<std::string> REDO = { "Redoing...", "Doing it again...", "Moling..." };
        std::string l = line;
        LLStringUtil::toLower(l);
        auto starts = [&l](const char* w) { return l.rfind(w, 0) == 0; };
        if (starts("levelling the ground")) return LEVEL;
        if (starts("grading the ramp")) return RAMP;
        if (starts("cutting the terraces")) return TERRACE;
        if (starts("smoothing the ground")) return SMOOTH;
        if (starts("laying the road")) return ROAD;
        if (starts("carving the river bed")) return RIVER;
        if (starts("shaping the shore")) return SHORE;
        if (starts("undoing \"")) return FILL;
        if (starts("redoing \"")) return REDO;
        auto has = [&l](const char* w) { return l.find(w) != std::string::npos; };
        if (has("light") && (has("taking") || has("remov") || has("away"))) return DARK;
        if (has("placing") || has("lighting") || has("flame") || has("lit:")) return LIGHT;
        if (has("leftover") || has("removing the walkable") || has("old rock") || has("taking the rock") || has("ground back")
            || has("forgetting") || has("deleting") || has("taken away")) return FILL;
        if (has("saving") || has("rights")) return SAVE;
        if (has("making the rock")) return ROCK;
        if (has("putting the tunnel down") || has("rezzing")) return REZ;
        return DIG;
    }

    /** The warning mark an error carries (a shape and a word, not only a colour). Source: wolffloatermeshupload.cpp warningMark. */
    void warningMark(F32 x, F32 y, F32 r)
    {
        disc(x, y, r, ERR_EDGE, ERR_EDGE);
        line(x, y + r * 0.55f, x, y - r * 0.15f, llmax(1.5f, r * 0.28f), ERR_PLATE);
        disc(x, y - r * 0.50f, llmax(1.f, r * 0.15f), ERR_PLATE, ERR_PLATE);
    }
}

WolfDigProgress::WolfDigProgress(const Params& p) : LLView(p) {}

WolfDigProgress* WolfDigProgress::add(LLFloater* owner, const LLRect& rect)
{
    Params p;
    p.name("wolf_dig_progress");
    p.rect(rect);
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    p.mouse_opaque(true);   // the controls it covers are words only; clicks on the plate go nowhere else
    p.visible(false);
    WolfDigProgress* v = new WolfDigProgress(p);

    // the reason a job stopped: read-only but selectable, so it can be copied (Ctrl+C or the context menu)
    LLTextEditor::Params ep;
    ep.name("wolf_dig_error");
    ep.rect(LLRect(0, 40, 200, 0));
    ep.read_only(true);
    ep.wrap(true);
    ep.bg_visible(false);
    ep.border_visible(false);
    ep.allow_scroll(true);
    ep.text_readonly_color(LLUIColor(ERR_INK));
    ep.text_color(LLUIColor(ERR_INK));
    ep.font(LLFontGL::getFontSansSerifSmall());
    ep.tab_stop(true);
    v->mError = LLUICtrlFactory::create<LLTextEditor>(ep);
    v->addChild(v->mError);

    LLButton::Params bp;
    bp.name("wolf_dig_ok");
    bp.label("OK");
    bp.rect(LLRect(0, 20, 56, 0));
    bp.tool_tip("Put the message away (select its words first to copy them)");
    v->mOk = LLUICtrlFactory::create<LLButton>(bp);
    v->mOk->setClickedCallback([v](LLUICtrl*, const LLSD&) { v->hide(); });
    v->addChild(v->mOk);

    owner->addChild(v);   // in front of the controls it lies over
    v->layoutChildren();
    return v;
}

void WolfDigProgress::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLView::reshape(width, height, called_from_parent);
    layoutChildren();
}

void WolfDigProgress::layoutChildren()
{
    if (!mError || !mOk) return;
    const LLRect r = getLocalRect();
    mOk->setShape(LLRect(r.mRight - 64, r.mBottom + 26, r.mRight - 8, r.mBottom + 6));
    mError->setShape(LLRect(r.mLeft + 30, r.mTop - 6, r.mRight - 8, r.mBottom + 30));
    const bool failed = mMode == MODE_FAILED;
    mError->setVisible(failed);
    mOk->setVisible(failed);
}

void WolfDigProgress::showWork(const std::string& line, F32 fraction)
{
    mMode = MODE_WORK;
    mLine = line;
    mFraction = fraction < 0.f ? -1.f : llclamp(fraction, 0.f, 1.f);
    setVisible(true);
    layoutChildren();
}

void WolfDigProgress::showDone(const std::string& line)
{
    mMode = MODE_DONE;
    mLine = line;
    mDoneAt = LLFrameTimer::getElapsedSeconds();
    setVisible(true);
    layoutChildren();
}

void WolfDigProgress::showFailed(const std::string& text)
{
    mMode = MODE_FAILED;
    mLine = text;
    if (mError) mError->setText(text);
    setVisible(true);
    layoutChildren();
}

void WolfDigProgress::hide()
{
    mMode = MODE_HIDDEN;
    setVisible(false);
    layoutChildren();
}

/** The burrow scene (wolfburrow.cpp drawMoleScene, shared with the windows' headers). */
void WolfDigProgress::drawMole(const LLRect& pit, F32 t, bool lit) const
{
    WolfBurrow::drawMoleScene(pit, t, lit);
}

void WolfDigProgress::draw()
{
    if (mMode == MODE_HIDDEN) return;
    if (mMode == MODE_DONE && LLFrameTimer::getElapsedSeconds() - mDoneAt > DONE_SECS)
    {
        hide();
        return;
    }
    const F32 t = animSeconds();
    const LLRect r = getLocalRect();
    untextured();
    if (mMode == MODE_FAILED)
    {
        fillRounded(r, allCorners(4.f), ERR_PLATE, ERR_PLATE);
        strokeRounded(r, allCorners(4.f), 0.f, 1.f, ERR_EDGE, ERR_EDGE);
        warningMark((F32)r.mLeft + 16.f, (F32)r.mTop - 16.f, 9.f);
        LLView::draw();   // the reason (selectable) and OK
        return;
    }
    // the plate
    LLRect shadow = r;
    shadow.translate(1, -2);
    fillRounded(shadow, allCorners(5.f), LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.45f));
    fillRounded(r, allCorners(5.f), PLATE_TOP, PLATE_BOTTOM);
    strokeRounded(r, allCorners(5.f), 0.f, 1.f, EDGE, withAlpha(EDGE, 0.6f));
    // the burrow on the left
    const S32 side = llmin(r.getHeight() - 10, 96);
    const LLRect pit(r.mLeft + 5, r.mTop - 5, r.mLeft + 5 + (S32)(side * 1.3f), r.mTop - 5 - side);
    const bool done = mMode == MODE_DONE;
    const std::vector<std::string>& words = wordsFor(mLine);
    const bool lit = &words == &wordsFor("lighting");
    drawMole(pit, done ? 0.f : t, lit);
    if (done)
    {
        disc((F32)pit.mRight - 12.f, (F32)pit.mTop - 12.f, 9.f, GREEN, GREEN);
        checkMark((F32)pit.mRight - 12.f, (F32)pit.mTop - 12.f, 10.f, 2.f, PLATE_BOTTOM);
    }
    // the words on the right: the step's fun word (big), the step's own line, then the bar
    const F32 tx = (F32)pit.mRight + 10.f, tw = (F32)r.mRight - 10.f - tx;
    const F32 top = (F32)r.mTop;
    if (done)
    {
        const LLFontGL* f = fitDisplayFont(28.f, tw, { "Done!" });
        text(f, "Done!", tx, top - 18.f, GREEN, LLFontGL::LEFT, tw);
    }
    else
    {
        const S32 n = llmax(1, (S32)words.size());
        const S32 k = (S32)floorf(t / WORD_SECS) % n;
        const F32 in = fmodf(t, WORD_SECS);
        const F32 a = llclamp(llmin(in, WORD_SECS - in) / 0.25f, 0.f, 1.f);
        const F32 ww = tw - 22.f;   // room for the turning arc at the right
        const LLFontGL* f = fitDisplayFont(28.f, ww, words);
        text(f, words[k], tx, top - 18.f, withAlpha(AMBER, a), LLFontGL::LEFT, ww);
    }
    const LLFontGL* sf = LLFontGL::getFontSansSerifSmall();
    const bool counted = !done && mFraction >= 0.f;
    const std::string pct = counted ? llformat("%d%%", (S32)floorf(mFraction * 100.f + 0.5f)) : std::string();
    const F32 pct_w = pct.empty() ? 0.f : sf->getWidthF32(pct) + 8.f;
    // the step line can be two lines' worth: it is cut to the room left (the window's status line keeps it whole)
    text(sf, mLine, tx, top - 42.f, CREAM, LLFontGL::LEFT, tw - pct_w);
    if (!pct.empty()) text(sf, pct, (F32)r.mRight - 10.f, top - 42.f, MUTED, LLFontGL::RIGHT);
    if (!done)
    {
        untextured();
        const LLRect bar((S32)tx, (S32)(top - 56.f), r.mRight - 10, (S32)(top - 64.f));
        fillRounded(bar, allCorners(3.f), TRACK, TRACK);
        const F32 bl = (F32)bar.mLeft + 1.f, br = (F32)bar.mRight - 1.f;
        if (counted)
        {
            const F32 x1 = bl + (br - bl) * mFraction;
            if (x1 > bl + 1.f) fillRounded(LLRect((S32)bl, bar.mTop - 1, (S32)x1, bar.mBottom + 1), allCorners(2.f), AMBER, AMBER_DEEP);
        }
        else
        {
            // a step that cannot be counted: a band sweeps (no number is made up for it)
            const F32 len = (br - bl) * 0.3f, k = fmodf(t * 0.6f, 1.f);
            const F32 x0 = bl - len + (br - bl + len) * k;
            const F32 a0 = llmax(bl, x0), a1 = llmin(br, x0 + len);
            if (a1 > a0) box(a0, (F32)bar.mTop - 1.f, a1, (F32)bar.mBottom + 1.f, AMBER, AMBER_DEEP);
        }
        // a small turning arc beside the word (the mole says it too, but an arc reads as "busy" everywhere)
        const F32 ax = (F32)r.mRight - 16.f, ay = top - 16.f, ang = fmodf(t * 4.f, F_TWO_PI);
        ring(ax, ay, 5.f, 7.f, withAlpha(EDGE, 0.6f));
        arcBand(ax, ay, 5.f, 7.f, ang, ang + 1.8f, withAlpha(AMBER, 0.f), AMBER);
    }
    LLView::draw();
}
