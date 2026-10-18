/**
 * @file wolfadventureplay.cpp
 * @brief WolfViewer: Wolf Adventure rolls and dice (HUD_MESSAGES_V1.md §3.9), the turn tracker (§3.10),
 *        the quiz (§3.11) and the GM Console's world overlays (§2.7).
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

#include "wolfadventureplay.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llrender.h"
#include "lltooltip.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "wolfadventure.h"
#include "wolfadventurehud.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventurePlayView> r_wolf_adventure_play_view("wolf_adventure_play_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    S32 worldW() { return gViewerWindow ? gViewerWindow->getWorldViewWidthScaled() : 1024; }
    S32 worldH() { return gViewerWindow ? gViewerWindow->getWorldViewHeightScaled() : 768; }

    // The inks the other adventure plates use (wolfadventurehud.cpp, wolfadventuretalk.cpp): 4.5:1 or better.
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24

    enum EPlayHit { PH_ROLL = 1, PH_ENDTURN, PH_CHOICE, PH_FIELD, PH_PREV, PH_NEXT, PH_SUBMIT, PH_CLOSE, PH_PLATE,
                    PH_MOVE, PH_TARGET };   // [WOLF STORY FIRST M4 2026-10-17] a battle move (arg: its index), an enemy to attack (arg: target index)

    // Source: HUD_MESSAGES_V1.md §6 2.5.8 "every button at least 24 x 24; reply and option buttons 32 high".
    const F32 OPTION_H = 32.f, PAD = 12.f;
    // §3.9 "a short tumble, then the faces": how long the tumble lasts (none under Reduce motion).
    const F64 TUMBLE_SEC = 0.6;

    const char* windowName(WolfAdventurePlayView::EMode m)
    {
        return m == WolfAdventurePlayView::MODE_ROLL ? "wolf_adventure_roll" : (m == WolfAdventurePlayView::MODE_TURNS ? "wolf_adventure_turns" : "wolf_adventure_quiz");
    }

    std::string outcomeWord(const std::string& o)
    {
        // Source: HUD_MESSAGES_V1.md §3.9 "the outcome as a word (Success / Failure / Critical / Fumble)".
        if (o == "success") return "Success";
        if (o == "failure") return "Failure";
        // [WORDING 2026-10-18] plain words, as the web HUD's OUTCOME_WORD
        if (o == "critical") return "Brilliant!";
        if (o == "fumble") return "Disaster!";
        return std::string();
    }

    bool goodOutcome(const std::string& o) { return o == "success" || o == "critical"; }

    bool wanted(WolfAdventurePlayView::EMode m)
    {
        const WolfAdventure& a = WolfAdventure::instance();
        if (m == WolfAdventurePlayView::MODE_ROLL) return a.roll().mOn || a.dice().mOn;
        if (m == WolfAdventurePlayView::MODE_TURNS) return a.turns().mOn;
        return a.quiz().mOn;
    }

    /** One choice's id is in a comma-separated answer. */
    bool picked(const std::string& answer, const std::string& id)
    {
        size_t start = 0;
        while (start <= answer.size())
        {
            const size_t end = answer.find(',', start);
            if (answer.substr(start, end == std::string::npos ? std::string::npos : end - start) == id) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    }

    std::string togglePick(const std::string& answer, const std::string& id, S32 max)
    {
        std::vector<std::string> ids;
        size_t start = 0;
        while (!answer.empty() && start <= answer.size())
        {
            const size_t end = answer.find(',', start);
            const std::string one = answer.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!one.empty()) ids.push_back(one);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        auto it = std::find(ids.begin(), ids.end(), id);
        if (max <= 1) ids.assign(1, id);                     // §3.11 radio buttons for max 1
        else if (it != ids.end()) ids.erase(it);              // check boxes: a second press unticks
        else if ((S32)ids.size() < max) ids.push_back(id);
        std::string out;
        for (const std::string& one : ids) out += (out.empty() ? "" : ",") + one;
        return out;
    }

    // ---- the GM overlay's state (wolfadventurehud.h setGmOverlay) ----
    WolfAdventureUI::GmOverlay sOverlay;
    F64 sOverlayAt = -100.0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// syncPlay and the GM overlay
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfAdventureUI
{
    void syncPlay()
    {
        static HeldFloater held[] = { HeldFloater(windowName(WolfAdventurePlayView::MODE_ROLL)),
                                      HeldFloater(windowName(WolfAdventurePlayView::MODE_TURNS)),
                                      HeldFloater(windowName(WolfAdventurePlayView::MODE_QUIZ)) };
        const bool wolf = WolfGrid::isOnWolfTerritories();
        for (WolfAdventurePlayView::EMode m : { WolfAdventurePlayView::MODE_ROLL, WolfAdventurePlayView::MODE_TURNS, WolfAdventurePlayView::MODE_QUIZ })
        {
            const char* name = windowName(m);
            HeldFloater& h = held[(S32)m];   // MODE_ROLL 0, MODE_TURNS 1, MODE_QUIZ 2 (wolfadventureplay.h EMode)
            WolfFloaterAdventurePlay* f = static_cast<WolfFloaterAdventurePlay*>(h.get());
            if (!wolf || !wanted(m))
            {
                if (f && f->getVisible()) f->closeFloater();
                continue;
            }
            if (!f || !f->getVisible())
            {
                // never takes the keyboard by itself (as the cards and the talk panel)
                f = LLFloaterReg::showTypedInstance<WolfFloaterAdventurePlay>(name, LLSD(), false);
                h.hold(f);
                if (f) f->fit();
            }
        }
    }

    void setGmOverlay(const GmOverlay& o)
    {
        sOverlay = o;
        sOverlayAt = now();
    }

    void keepGmOverlay()
    {
        sOverlayAt = now();
    }

    void drawGmOverlay(LLView* hud, const LLRect& r)
    {
        // Source: HUD_MESSAGES_V1.md §2.7 "All of it shows only while the GM Console is open": the console
        // sets (or keeps) the overlay every frame it draws; a second without it and nothing is drawn.
        if (!hud || now() - sOverlayAt > 1.0) return;
        if (!(sOverlay.mShowZones || sOverlay.mShowPaths || sOverlay.mShowNpcs)) return;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        WolfAdventure& wa = WolfAdventure::instance();
        const auto git = wa.gmLayers().find(sOverlay.mAdv);
        const WolfAdventure::GmLayers* layer = git != wa.gmLayers().end() ? &git->second : nullptr;
        // the brain's gm layers when they came (§2.7), else the pack's own places on this region
        const std::vector<WolfAdventure::GmArea>& areas = layer && layer->mHasAreas ? layer->mAreas : sOverlay.mAreas;
        const std::vector<WolfAdventure::GmPath>& paths = layer && layer->mHasPaths ? layer->mPaths : sOverlay.mPaths;
        const std::vector<WolfAdventure::GmNpc>& npcs = layer && layer->mHasNpcs ? layer->mNpcs : sOverlay.mNpcs;
        const bool places_here = (layer && (layer->mHasAreas || layer->mHasPaths)) || sOverlay.mRegion == rgn->getRegionID();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const Palette& p = gamePalette();
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        // region metres -> local HUD point (z 0 = on the ground, as ADVENTURE_PACK_V1.md §6 spots)
        auto project = [&](LLVector3 v, F32& x, F32& y) -> bool
        {
            if (v.mV[VZ] <= 0.f) v.mV[VZ] = rgn->getLandHeightRegion(v) + 0.3f;
            const LLVector3 agent = rgn->getPosAgentFromRegion(v);
            LLCoordGL s;
            if (!cam->projectPosAgentToScreen(agent, s, false)) return false;
            S32 lx = 0, ly = 0;
            hud->screenPointToLocal(s.mX, s.mY, &lx, &ly);
            x = (F32)lx;
            y = (F32)ly;
            return true;
        };
        auto label = [&](F32 x, F32 y, const std::string& text)
        {
            const F32 tw = small->getWidthF32(text), lh = (F32)small->getLineHeight();
            rect(x - tw * 0.5f - 4.f, y, x + tw * 0.5f + 4.f, y + lh + 4.f, LLColor4(0.f, 0.f, 0.f, 0.8f));
            textL(small, text, x, y + 2.f, p.mText, LLFontGL::HCENTER, LLFontGL::BOTTOM);
        };
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        const LLColor4 zone(p.mAccent.mV[VRED], p.mAccent.mV[VGREEN], p.mAccent.mV[VBLUE], 0.85f);
        if (sOverlay.mShowZones && places_here)
        {
            // "zones as translucent ground outlines in the accent colour, with their ids"
            for (const WolfAdventure::GmArea& a : areas)
            {
                std::vector<LLVector3> ring;
                if (a.mCircle)
                {
                    for (S32 i = 0; i <= 32; ++i)
                    {
                        const F32 ang = F_TWO_PI * (F32)i / 32.f;
                        ring.push_back(LLVector3(a.mCentre.mV[VX] + a.mRadius * cosf(ang), a.mCentre.mV[VY] + a.mRadius * sinf(ang), 0.f));
                    }
                }
                else
                {
                    ring = a.mPoly;
                    if (!ring.empty()) ring.push_back(ring.front());
                }
                F32 px = 0.f, py = 0.f, sx = 0.f, sy = 0.f, n = 0.f;
                bool prev = false;
                for (const LLVector3& v : ring)
                {
                    F32 x = 0.f, y = 0.f;
                    const bool ok = project(v, x, y);
                    if (ok && prev) lineW(px, py, x, y, 2.f, zone);
                    if (ok) { sx += x; sy += y; n += 1.f; }
                    prev = ok; px = x; py = y;
                }
                F32 cx = 0.f, cy = 0.f;
                LLVector3 c = a.mCircle ? a.mCentre : LLVector3::zero;
                if (!a.mCircle && !a.mPoly.empty()) { for (const LLVector3& v : a.mPoly) c += v; c /= (F32)a.mPoly.size(); }
                if (project(c, cx, cy)) label(cx, cy, "Zone " + a.mId);
                else if (n > 0.f) label(sx / n, sy / n, "Zone " + a.mId);
            }
        }
        if (sOverlay.mShowPaths && places_here)
        {
            // "paths as dotted lines with arrows"
            for (const WolfAdventure::GmPath& path : paths)
            {
                for (size_t i = 0; i + 1 < path.mPoints.size(); ++i)
                {
                    F32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
                    if (!project(path.mPoints[i], x0, y0) || !project(path.mPoints[i + 1], x1, y1)) continue;
                    const F32 dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
                    if (len < 1.f) continue;
                    for (F32 d = 0.f; d < len; d += 10.f)
                    {
                        const F32 e = llmin(len, d + 5.f);
                        lineW(x0 + dx * d / len, y0 + dy * d / len, x0 + dx * e / len, y0 + dy * e / len, 2.f, p.mText);
                    }
                    // an arrow head at the segment's middle, pointing along the path
                    const F32 ux = dx / len, uy = dy / len, mx = x0 + dx * 0.5f, my = y0 + dy * 0.5f;
                    lineW(mx, my, mx - ux * 8.f - uy * 5.f, my - uy * 8.f + ux * 5.f, 2.f, p.mAccent);
                    lineW(mx, my, mx - ux * 8.f + uy * 5.f, my - uy * 8.f - ux * 5.f, 2.f, p.mAccent);
                }
                F32 x = 0.f, y = 0.f;
                if (!path.mPoints.empty() && project(path.mPoints.front(), x, y)) label(x, y + 6.f, "Path " + path.mId);
            }
        }
        if (sOverlay.mShowNpcs)
        {
            // "NPC tags with handles": above each character standing in this region
            for (const WolfAdventure::GmNpc& n : npcs)
            {
                LLViewerObject* o = gObjectList.findObject(n.mNpc);
                if (!o || !o->isAvatar() || o->getRegion() != rgn) continue;
                LLVector3 pos = o->getPositionRegion();
                pos.mV[VZ] += 1.4f;
                F32 x = 0.f, y = 0.f;
                if (!project(pos, x, y)) continue;
                std::string text = (n.mName.empty() ? std::string("Character") : n.mName) + " \xC2\xB7 " + n.mHandle;
                if (!n.mPossessedBy.empty()) text += " \xC2\xB7 possessed";
                label(x, y + 10.f, text);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the view
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventurePlayView::WolfAdventurePlayView(const Params& p)
:   LLUICtrl(p)
{
}

U32 WolfAdventurePlayView::flags(S32 id, S32 arg, bool enabled) const
{
    U32 f = 0;
    if (!enabled) f |= BTN_DISABLED;
    if (mHits.isHover(id, arg)) f |= BTN_HOVER;
    if (mHits.isPressed(id, arg)) f |= BTN_PRESSED;
    if (mKeyboardNav && hasFocus() && mHits.isFocused(id, arg)) f |= BTN_FOCUS;
    return f;
}

void WolfAdventurePlayView::addHit(bool draw, F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool enabled)
{
    if (draw) mHits.add(l, b, r, t, id, arg, tip, true, enabled);
}

S32 WolfAdventurePlayView::measure(S32 width)
{
    // fit() measures every frame just before the draw lays the face out again. The draw's own height is kept
    // (layout returns the content height whatever H is: the faces return H - y + PAD, y counted down from H), so
    // the same width with nothing new from the region (WolfAdventure::generation) is not laid out a second time.
    const U32 gen = WolfAdventure::instance().generation();
    if (width == mLaidOutWidth && gen == mLaidOutGen) return mLaidOutHeight;
    return (S32)std::ceil(layout((F32)width, 4000.f, false));
}

void WolfAdventurePlayView::draw()
{
    mHits.clear();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    LLGLSUIDefault gls_ui;
    mLaidOutHeight = (S32)std::ceil(layout(W, H, true));
    mLaidOutWidth = getRect().getWidth();
    mLaidOutGen = WolfAdventure::instance().generation();
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

F32 WolfAdventurePlayView::layout(F32 W, F32 H, bool draw)
{
    const Palette& p = gamePalette();
    if (draw) frame(0.f, 0.f, W, H, p, 0.92f, 10.f);
    switch (mMode)
    {
    case MODE_ROLL: return faceRoll(W, H, draw, p);
    case MODE_TURNS: return faceTurns(W, H, draw, p);
    default: return faceQuiz(W, H, draw, p);
    }
}

// ---- §3.9 the roll prompt and the dice card ----
F32 WolfAdventurePlayView::faceRoll(F32 W, F32 H, bool draw, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Roll& rl = wa.roll();
    const WolfAdventure::Dice& dc = wa.dice();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* title = display(18.f);
    const F32 lh = (F32)body->getLineHeight(), slh = (F32)small->getLineHeight();
    F32 y = H - PAD;
    if (dc.mOn)
    {
        // §3.9 dice: the faces, kept bright and dropped dim AND struck through, the modifier, the total,
        // the DC, the outcome as a word with an icon, a GM note in italics. A short tumble first.
        if (dc.mSeq != mDiceSeq) { mDiceSeq = dc.mSeq; mTumbleStart = now(); }
        const bool tumbling = !WolfAdventure::reduceMotion() && now() - mTumbleStart < TUMBLE_SEC;
        if (draw) textL(bold, caps(dc.mByName.empty() ? std::string("Roll") : dc.mByName + " rolls"), PAD, y, p.mDim);
        y -= slh + 2.f;
        if (draw) textL(title, dc.mLabel.empty() ? std::string("Dice") : dc.mLabel, PAD, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
        y -= (F32)title->getLineHeight() + 6.f;
        const F32 die = 40.f;
        F32 x = PAD;
        for (size_t i = 0; i < dc.mDice.size(); ++i)
        {
            const WolfAdventure::Die& d = dc.mDice[i];
            if (x + die > W - PAD) { x = PAD; y -= die + 6.f; }
            if (draw)
            {
                const F32 a = d.mKept ? 1.f : 0.45f;
                chamfer(x, y - die, x + die, y, 6.f, LLColor4(0.f, 0.f, 0.f, 0.5f * a), LLColor4(0.f, 0.f, 0.f, 0.65f * a));
                chamferEdge(x + 0.5f, y - die + 0.5f, x + die - 0.5f, y - 0.5f, 6.f, d.mKept ? 2.f : 1.f, d.mKept ? p.mAccent : p.mDim);
                const S32 shown = tumbling ? (S32)(1 + ((S32)(now() * 23.0) + (S32)i * 7) % llmax(2, d.mSides)) : d.mValue;
                textL(display(20.f), llformat("%d", shown), x + die * 0.5f, y - die * 0.5f, d.mKept ? p.mText : p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER);
                textL(small, llformat("d%d", d.mSides), x + die * 0.5f, y - die - 2.f, p.mDim, LLFontGL::HCENTER, LLFontGL::TOP);
                if (!d.mKept) lineW(x + 6.f, y - die * 0.5f, x + die - 6.f, y - die * 0.5f, 2.f, p.mText);   // struck through
            }
            x += die + 8.f;
        }
        y -= die + slh + 8.f;
        if (!tumbling)
        {
            // [WORDING 2026-10-18] as the web HUD: "+2 = 15 - you needed 12"
            std::string sum = (dc.mMod != 0 ? llformat("%+d = ", dc.mMod) : std::string("= ")) + llformat("%d", dc.mTotal);
            if (dc.mHasDc) sum += llformat(" - you needed %d", dc.mDc);
            if (draw) textL(body, sum, PAD, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
            y -= lh + 4.f;
            const std::string word = outcomeWord(dc.mOutcome);
            if (!word.empty())
            {
                const bool good = goodOutcome(dc.mOutcome);
                if (draw)
                {
                    // the word AND an icon (never colour alone)
                    const F32 cy = y - lh * 0.5f;
                    if (good) { lineW(PAD + 1.f, cy, PAD + 5.f, cy - 4.f, 2.f, OK_INK); lineW(PAD + 5.f, cy - 4.f, PAD + 13.f, cy + 5.f, 2.f, OK_INK); }
                    else { lineW(PAD + 2.f, cy - 5.f, PAD + 12.f, cy + 5.f, 2.f, DANGER_INK); lineW(PAD + 2.f, cy + 5.f, PAD + 12.f, cy - 5.f, 2.f, DANGER_INK); }
                    textL(display(18.f), word, PAD + 20.f, y, good ? OK_INK : DANGER_INK);
                }
                y -= (F32)display(18.f)->getLineHeight() + 2.f;
            }
            if (!dc.mNote.empty())
            {
                for (const std::string& line : wrap(small, dc.mNote, W - 2.f * PAD, 3))
                {
                    if (draw) small->renderUTF8(line, 0, PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::ITALIC, LLFontGL::NO_SHADOW);
                    y -= slh;
                }
            }
            // [WORDING 2026-10-18] the result card says nothing of who sees it (the roll prompt did), as the web HUD
        }
        return H - y + PAD;
    }
    if (!rl.mOn) return 0.f;
    // §3.9 the roll prompt: the label, the dice with advantage as WORDS, the DC or "hidden", a big Roll button.
    if (draw) textL(bold, caps(rl.mSrc.empty() ? std::string("A check") : rl.mSrc), PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
    y -= slh + 2.f;
    if (draw) textL(title, rl.mLabel.empty() ? std::string("Roll") : rl.mLabel, PAD, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
    y -= (F32)title->getLineHeight() + 4.f;
    std::string what = rl.mDice.empty() ? std::string("1d20") : rl.mDice;
    // [WORDING 2026-10-18] the roll in plain words (no "advantage", "DC"), as the web HUD
    if (rl.mAdvantage > 0) what += " \xC2\xB7 roll twice, keep the best";
    else if (rl.mAdvantage < 0) what += " \xC2\xB7 roll twice, keep the worst";
    what += rl.mHasDc ? llformat(" \xC2\xB7 you need %d or more", rl.mDc) : std::string(" \xC2\xB7 the number you need is secret");
    if (!rl.mPublic) what += " \xC2\xB7 only you see it";
    if (draw) textL(body, what, PAD, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
    y -= lh + 6.f;
    if (rl.mEndsAt > 0.0 && rl.mSec > 0.f)
    {
        // a timer: a thin bar plus the seconds in words (§3.3's rule, used here as well)
        const F32 left = (F32)llmax(0.0, rl.mEndsAt - now());
        if (draw)
        {
            rect(PAD, y - 4.f, W - PAD, y, p.mTrack);
            rect(PAD, y - 4.f, PAD + (W - 2.f * PAD) * llclamp(left / rl.mSec, 0.f, 1.f), y, p.mAccent);
            textL(small, llformat("%d s", (S32)ceilf(left)), W - PAD, y - 6.f, p.mDim, LLFontGL::RIGHT, LLFontGL::TOP);
        }
        y -= 8.f + slh;
    }
    const bool mine = rl.mWho == gAgentID;
    if (mine && !rl.mSent)
    {
        const F32 bh = 40.f;
        if (draw) button(PAD, y - bh, W - PAD, y, "Roll", p, flags(PH_ROLL, 0, true) | BTN_PRIMARY, display(20.f));
        addHit(draw, PAD, y - bh, W - PAD, y, PH_ROLL, 0, "Roll the dice (Enter)");
        y -= bh + 6.f;
    }
    else
    {
        std::string who = rl.mWhoName;
        if (who.empty()) who = WolfAdventure::agentName(rl.mWho);
        const std::string text = mine ? std::string("Rolled - waiting for the result.")
                                      : "Waiting for " + (who.empty() ? std::string("another player") : who) + " to roll.";
        if (draw) textL(body, text, PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
        y -= lh + 6.f;
    }
    return H - y + PAD;
}

// ---- §3.10 the turn tracker ----
F32 WolfAdventurePlayView::faceTurns(F32 W, F32 H, bool draw, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Turns& t = wa.turns();
    if (!t.mOn) return 0.f;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = H - PAD;
    bool mine = false;
    std::string current_name;
    for (const WolfAdventure::TurnRow& r : t.mOrder)
    {
        if (r.mKey != t.mCurrent) continue;
        mine = r.mYou;
        current_name = r.mName;
    }
    const F32 left = t.mEndsAt > 0.0 ? (F32)llmax(0.0, t.mEndsAt - now()) : -1.f;
    std::string head = (t.mLabel.empty() ? std::string() : t.mLabel + " \xC2\xB7 ") + llformat("Round %d \xC2\xB7 ", t.mRound)
                     + (mine ? std::string("Your turn") : (current_name.empty() ? std::string("Waiting") : current_name + "'s turn"));
    if (left >= 0.f) head += llformat(" \xC2\xB7 %d s left", (S32)ceilf(left));
    if (draw) textL(display(17.f), head, W * 0.5f, y, mine ? p.mAccent : p.mText, LLFontGL::HCENTER, LLFontGL::TOP, W - 2.f * PAD - (mine ? 110.f : 0.f));
    y -= (F32)display(17.f)->getLineHeight() + 6.f;
    // [M4] what just happened, in words (the brain's last line: hits, misses, who fell or ran)
    if (!t.mLine.empty())
    {
        for (const std::string& ln : wrap(small, t.mLine, W - 2.f * PAD, 2)) { if (draw) textL(small, ln, W * 0.5f, y, p.mText, LLFontGL::HCENTER, LLFontGL::TOP); y -= slh; }
        y -= 4.f;
    }
    // portraits along the row: the current one larger (BG3's initiative bar, §3)
    const S32 n = (S32)t.mOrder.size();
    const F32 small_w = 92.f, big_w = 118.f, gap = 6.f;
    F32 total = 0.f;
    for (const WolfAdventure::TurnRow& r : t.mOrder) total += (r.mKey == t.mCurrent ? big_w : small_w) + gap;
    F32 x = llmax(PAD, (W - total) * 0.5f);
    const F32 row_h = 64.f + slh;
    for (S32 i = 0; i < n; ++i)
    {
        const WolfAdventure::TurnRow& r = t.mOrder[i];
        const bool cur = r.mKey == t.mCurrent;
        const F32 w = cur ? big_w : small_w;
        const F32 h = cur ? row_h : row_h - 8.f;
        if (x + w > W - PAD) break;   // the window grows to fit; past its width the rest are not drawn
        if (draw)
        {
            card(x, y - h, x + w, y, p, 6.f);
            const LLColor4 side = r.mSide == "enemy" ? DANGER_INK : (r.mSide == "party" ? OK_INK : p.mDim);
            chamferEdge(x + 0.5f, y - h + 0.5f, x + w - 0.5f, y - 0.5f, 6.f, cur ? 3.f : 1.f, cur ? p.mAccent : side);
            F32 ty = y - 6.f;
            textL(bold, r.mName.empty() ? std::string("?") : r.mName, x + 6.f, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, w - 12.f);
            ty -= slh;
            // the side as a word, not colour alone
            const std::string sw = r.mSide == "enemy" ? "Enemy" : (r.mSide == "party" ? (r.mYou ? "You" : "Party") : "Neutral");
            // [M4] a fighter who is out of it says so in words (down / ran off), as well as the dimmed bar
            const std::string st = r.mState == "down" ? " \xC2\xB7 down" : r.mState == "beaten" ? " \xC2\xB7 beaten" : r.mState == "fled" ? " \xC2\xB7 ran off" : r.mState == "defending" ? " \xC2\xB7 defending" : std::string();
            textL(small, sw + llformat(" \xC2\xB7 Order roll %d", r.mInit) + st, x + 6.f, ty, r.mState == "down" || r.mState == "beaten" || r.mState == "fled" ? WARN_INK : p.mDim, LLFontGL::LEFT, LLFontGL::TOP, w - 12.f);
            ty -= slh;
            if (r.mHasHp)
            {
                // HP as pips AND as text
                const F32 frac = llclamp((F32)r.mHp / (F32)r.mHpMax, 0.f, 1.f);
                rect(x + 6.f, ty - 6.f, x + w - 6.f, ty - 2.f, p.mTrack);
                rect(x + 6.f, ty - 6.f, x + 6.f + (w - 12.f) * frac, ty - 2.f, frac > 0.5f ? OK_INK : (frac > 0.2f ? WARN_INK : DANGER_INK));
                ty -= 8.f;
                textL(small, llformat("Health %d of %d", r.mHp, r.mHpMax), x + 6.f, ty, p.mText);
                ty -= slh;
            }
            if (!r.mCond.empty())
            {
                std::string c;
                for (const std::string& one : r.mCond) c += (c.empty() ? "" : ", ") + one;
                textL(small, c, x + 6.f, ty, WARN_INK, LLFontGL::LEFT, LLFontGL::TOP, w - 12.f);
            }
        }
        if (draw) mHits.add(x, y - h, x + w, y, PH_PLATE, i, r.mName + (cur ? " - their turn now" : ""), false);
        x += w + gap;
    }
    y -= row_h + 8.f;
    if (mine && !t.mActions.empty())
    {
        // [M4] a battle turn: the moves the brain allows now (COMBAT_V1.md §2.1), each a word on a button; Attack then asks
        // whom to attack when there is more than one enemy standing
        if (!mPickTarget)
        {
            const F32 bw = llmin(130.f, (W - 2.f * PAD - 6.f * ((F32)t.mActions.size() - 1.f)) / (F32)t.mActions.size());
            F32 bx = (W - (bw * (F32)t.mActions.size() + 6.f * ((F32)t.mActions.size() - 1.f))) * 0.5f;
            for (size_t k = 0; k < t.mActions.size(); ++k)
            {
                const std::string& a = t.mActions[k];
                const std::string lab = a == "attack" ? "Attack" : a == "defend" ? "Defend" : a == "item" ? llformat("Heal (%d left)", t.mDraughts) : "Flee";
                const std::string tip = a == "attack" ? "Attack an enemy" : a == "defend" ? "Defend: attacks on you are harder until your next turn"
                                      : a == "item" ? "Drink a healing draught" : "Try to get away (if you do, you are out of this fight)";
                if (draw) button(bx, y - 32.f, bx + bw, y, lab, p, flags(PH_MOVE, (S32)k, !t.mActSending) | (a == "attack" ? BTN_PRIMARY : 0));
                addHit(draw, bx, y - 32.f, bx + bw, y, PH_MOVE, (S32)k, tip + llformat(" (%d)", (S32)k + 1), !t.mActSending);
                bx += bw + 6.f;
            }
            y -= 40.f;
        }
        else
        {
            if (draw) textL(bold, "Attack whom?", PAD, y, p.mText);
            y -= slh + 4.f;
            for (size_t k = 0; k < t.mTargets.size(); ++k)
            {
                const WolfAdventure::TurnTarget& tv = t.mTargets[k];
                const std::string lab = tv.mName + llformat("  (%d/%d)", tv.mHp, tv.mHpMax);
                if (draw) button(PAD, y - 28.f, W - PAD, y, lab, p, flags(PH_TARGET, (S32)k, !t.mActSending));
                addHit(draw, PAD, y - 28.f, W - PAD, y, PH_TARGET, (S32)k, "Attack " + tv.mName + llformat(" (%d; Esc to go back)", (S32)k + 1), !t.mActSending);
                y -= 32.f;
            }
            y -= 6.f;
        }
    }
    else if (mine)
    {
        // §3.10 "On their own turn the player has End turn" (§6 2.2.1: the player ends it themselves)
        if (draw) button(W * 0.5f - 70.f, y - 32.f, W * 0.5f + 70.f, y, "End turn", p, flags(PH_ENDTURN, 0, true) | BTN_PRIMARY);
        addHit(draw, W * 0.5f - 70.f, y - 32.f, W * 0.5f + 70.f, y, PH_ENDTURN, 0, "End your turn (Enter)");
        y -= 40.f;
    }
    if (!mine) mPickTarget = false;
    return H - y + PAD;
}

// ---- §3.11 the quiz ----
F32 WolfAdventurePlayView::faceQuiz(F32 W, F32 H, bool draw, const Palette& p)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Quiz& q = wa.quiz();
    if (!q.mOn) return 0.f;
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 lh = (F32)body->getLineHeight(), slh = (F32)small->getLineHeight();
    F32 y = H - PAD;
    if (draw)
    {
        textL(bold, caps(q.mSrc.empty() ? std::string("Quiz") : q.mSrc), PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD - 30.f);
        const F32 xc = W - PAD - 8.f, yc = y - 8.f;
        closeX(xc, yc, 5.f, mHits.isHover(PH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(PH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
    }
    addHit(draw, W - PAD - 20.f, y - 20.f, W - PAD + 4.f, y + 4.f, PH_CLOSE, 0, "Put the quiz away (your answers so far are kept)");
    y -= slh + 2.f;
    if (draw) textL(display(20.f), q.mTitle.empty() ? std::string("Questions") : q.mTitle, PAD, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD);
    y -= (F32)display(20.f)->getLineHeight() + 6.f;
    if (q.mItems.empty())
    {
        if (draw) textL(body, "This quiz has no questions.", PAD, y, p.mDim);
        return H - y + lh + PAD;
    }
    if (q.mHasResult)
    {
        // §3.11 "After submitting, result shows which were correct (a tick AND the word)."
        const std::string line = llformat("Score %d of %d", q.mScore, q.mScoreMax) + (q.mPassed ? " \xC2\xB7 Passed" : " \xC2\xB7 Not passed yet");
        if (draw) textL(display(18.f), line, PAD, y, q.mPassed ? OK_INK : WARN_INK);
        y -= (F32)display(18.f)->getLineHeight() + 6.f;
        for (const WolfAdventure::QuizItem& it : q.mItems)
        {
            const auto mk = q.mMarked.find(it.mId);
            const S32 state = mk == q.mMarked.end() ? -1 : mk->second;
            const std::string word = state == 1 ? "Correct" : (state == 0 ? "Not correct" : "Waiting for your teacher");
            const LLColor4 c = state == 1 ? OK_INK : (state == 0 ? DANGER_INK : p.mDim);
            if (draw)
            {
                const F32 cy = y - lh * 0.5f;
                if (state == 1) { lineW(PAD + 1.f, cy, PAD + 5.f, cy - 4.f, 2.f, c); lineW(PAD + 5.f, cy - 4.f, PAD + 13.f, cy + 5.f, 2.f, c); }
                else if (state == 0) { lineW(PAD + 2.f, cy - 5.f, PAD + 12.f, cy + 5.f, 2.f, c); lineW(PAD + 2.f, cy + 5.f, PAD + 12.f, cy - 5.f, 2.f, c); }
                textL(bold, word, PAD + 20.f, y, c);
            }
            y -= slh;
            for (const std::string& l : wrap(small, it.mPrompt, W - 2.f * PAD - 20.f, 2))
            {
                if (draw) textL(small, l, PAD + 20.f, y, p.mText);
                y -= slh;
            }
            y -= 4.f;
        }
        return H - y + PAD;
    }
    const S32 n = (S32)q.mItems.size();
    const S32 idx = llclamp(q.mIndex, 0, n - 1);
    const WolfAdventure::QuizItem& it = q.mItems[idx];
    const auto ans_it = q.mAnswers.find(it.mId);
    const std::string answer = ans_it == q.mAnswers.end() ? std::string() : ans_it->second;
    if (draw) textL(small, llformat("Question %d of %d", idx + 1, n), PAD, y, p.mDim);
    y -= slh + 2.f;
    for (const std::string& l : wrap(body, it.mPrompt, W - 2.f * PAD, 8))
    {
        if (draw) textL(body, l, PAD, y, p.mText);
        y -= lh;
    }
    y -= 6.f;
    if (it.mType == "choice")
    {
        // §3.11 "radio buttons or check boxes for choice items (by max)"
        if (it.mMax > 1 && draw) textL(small, llformat("Choose up to %d.", it.mMax), PAD, y, p.mDim);
        if (it.mMax > 1) y -= slh + 2.f;
        for (size_t k = 0; k < it.mChoices.size(); ++k)
        {
            const WolfAdventure::QuizChoice& c = it.mChoices[k];
            const bool on = picked(answer, c.mId);
            if (draw)
            {
                button(PAD, y - OPTION_H, W - PAD, y, std::string(), p, flags(PH_CHOICE, (S32)k, true) | (on ? BTN_ACTIVE : 0));
                const F32 cx = PAD + 16.f, cy = y - OPTION_H * 0.5f;
                if (it.mMax <= 1)
                {
                    arc(cx, cy, 7.f, 1.5f, 0.f, F_TWO_PI, p.mText);
                    if (on) disc(cx, cy, 4.f, p.mAccent);
                }
                else
                {
                    chamferEdge(cx - 7.f, cy - 7.f, cx + 7.f, cy + 7.f, 1.f, 1.5f, p.mText);
                    if (on) { lineW(cx - 4.f, cy, cx - 1.f, cy - 3.f, 2.f, p.mAccent); lineW(cx - 1.f, cy - 3.f, cx + 5.f, cy + 4.f, 2.f, p.mAccent); }
                }
                textL(body, llformat("%d. ", (S32)k + 1) + c.mLabel, PAD + 32.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, W - 2.f * PAD - 40.f);
            }
            addHit(draw, PAD, y - OPTION_H, W - PAD, y, PH_CHOICE, (S32)k, (on ? "Chosen: " : "Choose: ") + c.mLabel);
            y -= OPTION_H + 4.f;
        }
    }
    else
    {
        // §3.11 "a text field for text items; a multi-line field for essays"
        const bool essay = it.mType == "essay";
        const S32 rows = essay ? 6 : 1;
        const F32 fh = essay ? lh * rows + 12.f : 30.f;
        const bool focused = mFieldFocus && hasFocus();
        if (draw)
        {
            chamfer(PAD, y - fh, W - PAD, y, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
            chamferEdge(PAD + 0.5f, y - fh + 0.5f, W - PAD - 0.5f, y - 0.5f, 4.f, focused ? 2.f : 1.f, focused ? p.mAccent : p.mDim);
            if (answer.empty())
            {
                textL(body, essay ? "Write your answer..." : "Type your answer...", PAD + 8.f, y - 6.f, p.mDim);
            }
            else
            {
                std::vector<std::string> lines = wrap(body, answer, W - 2.f * PAD - 16.f, 0);
                const size_t first = lines.size() > (size_t)rows ? lines.size() - (size_t)rows : 0;   // the end stays in view
                F32 ty = y - 6.f;
                F32 endx = PAD + 8.f;
                for (size_t k = first; k < lines.size(); ++k)
                {
                    endx = textL(body, lines[k], PAD + 8.f, ty, p.mText);
                    if (k + 1 < lines.size()) ty -= lh;
                }
                if (focused && fmod(now(), 1.0) < 0.6) rect(endx + 1.f, ty - lh + 2.f, endx + 2.f, ty - 2.f, p.mText);
            }
            if (answer.empty() && focused && fmod(now(), 1.0) < 0.6) rect(PAD + 8.f, y - 6.f - lh + 2.f, PAD + 9.f, y - 8.f, p.mText);
        }
        addHit(draw, PAD, y - fh, W - PAD, y, PH_FIELD, 0, essay ? "Your answer (your teacher reads it)" : "Your answer");
        y -= fh + 2.f;
        if (draw) textL(small, llformat("%d characters left", llmax(0, 2000 - (S32)utf8str_to_wstring(answer).size())), W - PAD, y, p.mDim, LLFontGL::RIGHT, LLFontGL::TOP);
        y -= slh + 4.f;
    }
    // a saved / not-saved word for this item (nothing is lost if the window closes)
    {
        const auto ok = q.mSentOk.find(it.mId);
        if (ok != q.mSentOk.end() && ok->second && draw) textL(small, "Saved.", PAD, y, OK_INK);
        if (ok != q.mSentOk.end() && ok->second) y -= slh + 2.f;
    }
    if (!q.mError.empty())
    {
        // errors stay visible until the next action
        for (const std::string& l : wrap(small, q.mError, W - 2.f * PAD, 3))
        {
            if (draw) textL(small, l, PAD, y, DANGER_INK);
            y -= slh;
        }
        y -= 4.f;
    }
    if (q.mSending)
    {
        if (draw) textL(body, "Sent. Waiting for your result...", PAD, y, p.mDim);
        y -= lh + 4.f;
    }
    // §3.11 Previous, Next and Submit
    const F32 bw = 110.f, bh = 32.f;
    if (draw) button(PAD, y - bh, PAD + bw, y, "Previous", p, flags(PH_PREV, 0, idx > 0));
    addHit(draw, PAD, y - bh, PAD + bw, y, PH_PREV, 0, "The previous question", idx > 0);
    if (idx + 1 < n)
    {
        if (draw) button(W - PAD - bw, y - bh, W - PAD, y, "Next", p, flags(PH_NEXT, 0, true) | BTN_PRIMARY);
        addHit(draw, W - PAD - bw, y - bh, W - PAD, y, PH_NEXT, 0, "Save this answer and go to the next question");
    }
    else
    {
        if (draw) button(W - PAD - bw, y - bh, W - PAD, y, "Submit", p, flags(PH_SUBMIT, 0, !q.mSending) | BTN_PRIMARY);
        addHit(draw, W - PAD - bw, y - bh, W - PAD, y, PH_SUBMIT, 0, "Send your answers", !q.mSending);
    }
    y -= bh + 4.f;
    return H - y + PAD;
}

// ---- input ----

void WolfAdventurePlayView::press(S32 id, S32 arg)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Quiz& q = wa.quiz();
    switch (id)
    {
    case PH_ROLL: wa.pressRoll(); break;
    case PH_ENDTURN: wa.pressEndTurn(); break;
    case PH_MOVE:   // [WOLF STORY FIRST M4 2026-10-17]
    {
        const WolfAdventure::Turns& t = wa.turns();
        if (arg < 0 || arg >= (S32)t.mActions.size()) break;
        const std::string move = t.mActions[arg];
        if (move == "attack")
        {
            if (t.mTargets.size() == 1) wa.combatAct("attack", t.mTargets[0].mKey);
            else if (!t.mTargets.empty()) mPickTarget = true;
        }
        else wa.combatAct(move, std::string());
        break;
    }
    case PH_TARGET:
    {
        const WolfAdventure::Turns& t = wa.turns();
        mPickTarget = false;
        if (arg >= 0 && arg < (S32)t.mTargets.size()) wa.combatAct("attack", t.mTargets[arg].mKey);
        break;
    }
    case PH_CHOICE:
    {
        if (!q.mOn || q.mItems.empty()) break;
        const WolfAdventure::QuizItem& it = q.mItems[llclamp(q.mIndex, 0, (S32)q.mItems.size() - 1)];
        if (arg < 0 || arg >= (S32)it.mChoices.size()) break;
        const auto a = q.mAnswers.find(it.mId);
        wa.setQuizAnswer(it.mId, togglePick(a == q.mAnswers.end() ? std::string() : a->second, it.mChoices[arg].mId, it.mMax));
        break;
    }
    case PH_FIELD:
        mFieldFocus = true;
        if (!q.mItems.empty()) mFieldItem = q.mItems[llclamp(q.mIndex, 0, (S32)q.mItems.size() - 1)].mId;
        setFocus(true);
        break;
    case PH_PREV: mFieldFocus = false; wa.quizGo(q.mIndex - 1); break;
    case PH_NEXT: mFieldFocus = false; wa.quizGo(q.mIndex + 1); break;
    case PH_SUBMIT: mFieldFocus = false; wa.quizSubmit(); break;
    case PH_CLOSE: mFieldFocus = false; wa.quizClose(); break;
    default: break;
    }
}

bool WolfAdventurePlayView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;
    if (h->mId != PH_FIELD) mFieldFocus = false;
    setFocus(true);
    mKeyboardNav = false;
    if (!h->mEnabled || !h->mFocusable) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAdventurePlayView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    const S32 pid = mHits.pressedId(), parg = mHits.pressedArg();
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == pid && h->mArg == parg && h->mEnabled) press(pid, parg);
    return true;
}

void WolfAdventurePlayView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
}

bool WolfAdventurePlayView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(h->mId == PH_FIELD ? UI_CURSOR_IBEAM : (h->mEnabled && h->mFocusable ? UI_CURSOR_HAND : UI_CURSOR_ARROW));
    return true;
}

bool WolfAdventurePlayView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfAdventurePlayView::handleKeyHere(KEY key, MASK mask)
{
    WolfAdventure& wa = WolfAdventure::instance();
    if (mMode == MODE_QUIZ && mFieldFocus)
    {
        const WolfAdventure::Quiz& q = wa.quiz();
        if (q.mOn && !q.mItems.empty())
        {
            const WolfAdventure::QuizItem& it = q.mItems[llclamp(q.mIndex, 0, (S32)q.mItems.size() - 1)];
            const auto a = q.mAnswers.find(it.mId);
            std::string text = a == q.mAnswers.end() ? std::string() : a->second;
            if (key == KEY_BACKSPACE && mask == MASK_NONE)
            {
                LLWString w = utf8str_to_wstring(text);
                if (!w.empty()) w.pop_back();
                wa.setQuizAnswer(it.mId, wstring_to_utf8str(w));
                return true;
            }
            if (key == KEY_RETURN && mask == MASK_NONE && it.mType == "essay")
            {
                wa.setQuizAnswer(it.mId, text + "\n");
                return true;
            }
            if (key == 'V' && (mask & MASK_CONTROL))
            {
                LLWString pasted;
                if (LLClipboard::instance().pasteFromClipboard(pasted)) wa.setQuizAnswer(it.mId, text + wstring_to_utf8str(pasted));
                return true;
            }
            if (key == KEY_ESCAPE && mask == MASK_NONE)
            {
                mFieldFocus = false;
                return true;
            }
        }
    }
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        const WolfGameHits::Hit* f = mHits.focused();
        mFieldFocus = f && f->mId == PH_FIELD;
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled && f->mFocusable)
        {
            mKeyboardNav = true;
            press(f->mId, f->mArg);
            return true;
        }
        // Enter alone: the plate's main button (Roll, End turn)
        if (mMode == MODE_ROLL) { wa.pressRoll(); return true; }
        // [M4] a battle turn has no End turn: Enter means nothing until a move is chosen (Tab, or 1-4)
        if (mMode == MODE_TURNS && wa.turns().mActions.empty()) { wa.pressEndTurn(); return true; }
        return true;
    }
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_UP:
    case KEY_DOWN:
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_RIGHT || key == KEY_DOWN);
        return true;
    case KEY_ESCAPE:
        if (mMode == MODE_QUIZ) { press(PH_CLOSE, 0); return true; }
        if (mPickTarget) { mPickTarget = false; return true; }   // [M4] back out of "Attack whom?"
        setFocus(false);
        return true;
    default:
        return false;
    }
}

bool WolfAdventurePlayView::handleUnicodeCharHere(llwchar uni_char)
{
    WolfAdventure& wa = WolfAdventure::instance();
    if (mMode == MODE_QUIZ && mFieldFocus)
    {
        const WolfAdventure::Quiz& q = wa.quiz();
        if (uni_char < 0x20 || uni_char == 0x7f || !q.mOn || q.mItems.empty()) return true;
        const WolfAdventure::QuizItem& it = q.mItems[llclamp(q.mIndex, 0, (S32)q.mItems.size() - 1)];
        const auto a = q.mAnswers.find(it.mId);
        LLWString w = utf8str_to_wstring(a == q.mAnswers.end() ? std::string() : a->second);
        if (w.size() < 2000) w.push_back(uni_char);   // §4 "text up to 2000 characters"
        wa.setQuizAnswer(it.mId, wstring_to_utf8str(w));
        return true;
    }
    // [WOLF STORY FIRST M4 2026-10-17] battle turn: digit N presses the Nth move button (or, while choosing whom to
    // attack, the Nth enemy) - the number shown in each button's tooltip
    if (mMode == MODE_TURNS && uni_char >= '1' && uni_char <= '9')
    {
        const WolfAdventure::Turns& t = wa.turns();
        const S32 n = (S32)(uni_char - '1');
        if (t.mActSending) return true;
        if (mPickTarget) { if (n < (S32)t.mTargets.size()) press(PH_TARGET, n); return true; }
        if (n < (S32)t.mActions.size()) press(PH_MOVE, n);
        return true;
    }
    if (uni_char == ' ')
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled && f->mFocusable) press(f->mId, f->mArg);
        return true;
    }
    // number keys pick a quiz choice while the quiz has the keyboard (as the cards' 1-6)
    if (mMode == MODE_QUIZ && uni_char >= '1' && uni_char <= '6')
    {
        press(PH_CHOICE, (S32)(uni_char - '1'));
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventurePlay::WolfFloaterAdventurePlay(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventurePlay::postBuild()
{
    mView = getChild<WolfAdventurePlayView>("view");
    const std::string& n = getName();
    mView->setMode(n == "wolf_adventure_turns" ? WolfAdventurePlayView::MODE_TURNS
                   : (n == "wolf_adventure_quiz" ? WolfAdventurePlayView::MODE_QUIZ : WolfAdventurePlayView::MODE_ROLL));
    setBackgroundVisible(false);   // in front of the drag handle, as WolfFloaterRoleplay::postBuild explains
    return true;
}

void WolfFloaterAdventurePlay::fit()
{
    if (!mView || !getParent()) return;
    const S32 W = worldW(), H = worldH();
    const WolfAdventurePlayView::EMode m = mView->mode();
    S32 w = 360;
    if (m == WolfAdventurePlayView::MODE_TURNS)
    {
        const S32 n = (S32)WolfAdventure::instance().turns().mOrder.size();
        w = llclamp(n * 98 + 160, 360, llmax(360, W - 40));
    }
    else if (m == WolfAdventurePlayView::MODE_QUIZ)
    {
        w = llmin(560, W - 32);   // §3.11 "a plate like the vignette" (§3.3 at most 560 wide)
    }
    const S32 h = llclamp(mView->measure(w), 60, llmax(80, H - 40));
    S32 left = (W - w) / 2, top = H - 40;
    if (m == WolfAdventurePlayView::MODE_ROLL)
    {
        // §3.9 "a small plate above the talk panel's position"
        bool from_top = false;
        S32 sx = 0, sy = 0, ax = W / 2, ay = 140;
        if (WolfAdventureUI::talkAnchor(from_top, sx, sy)) getParent()->screenPointToLocal(sx, sy, &ax, &ay);
        LLFloater* talk = WolfAdventureUI::talkWindow().get();
        if (talk && talk->getVisible()) ay = llmax(ay, talk->getRect().mTop + 8);
        else if (from_top) ay = 140;
        left = llclamp(W / 2 - w / 2 - W / 10, 16, llmax(16, W - w - 16));
        top = llclamp(ay + h + 8, h, H - 10);
    }
    else if (m == WolfAdventurePlayView::MODE_TURNS)
    {
        top = H - 130;   // §3.10 "along the top centre, under the target frame"
    }
    else
    {
        top = H - (S32)(0.12f * (F32)H);   // §3.3 the upper third
    }
    top = llclamp(top, h, H);
    const LLRect want(left, top, left + w, top - h);
    if (want != getRect())
    {
        reshape(w, h);
        setRect(want);
        mView->setRect(LLRect(0, h, w, 0));
    }
}

void WolfFloaterAdventurePlay::draw()
{
    if (!WolfGrid::isOnWolfTerritories() || !mView || !wanted(mView->mode()))
    {
        closeFloater();
        return;
    }
    fit();
    LLFloater::draw();
}

void WolfFloaterAdventurePlay::onClose(bool app_quitting)
{
    // a quiz closed from outside is put away (its answers so far are kept)
    if (!app_quitting && mView && mView->mode() == WolfAdventurePlayView::MODE_QUIZ && WolfAdventure::instanceExists() && WolfAdventure::instance().quiz().mOn)
    {
        WolfAdventure::instance().quizClose();
    }
    LLFloater::onClose(app_quitting);
}
