/**
 * @file wolfadventurehud.cpp
 * @brief WolfViewer: Wolf Adventure on screen. See wolfadventurehud.h.
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

#include "llagent.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llscriptfloater.h"
#include "lltooltip.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llrender2dutils.h"
#include "llviewertexturelist.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureCardView> r_wolf_adventure_card_view("wolf_adventure_card_view");
static LLDefaultChildRegistry::Register<WolfAdventureHandoutView> r_wolf_adventure_handout_view("wolf_adventure_handout_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    LLColor4 hex(U32 v, F32 a = 1.f) { return LLColor4(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, a); }

    /** WCAG relative luminance (the same formula as wolfgamehud.cpp relLum). */
    F32 lum(const LLColor4& c)
    {
        auto lin = [](F32 v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * lin(c.mV[VRED]) + 0.7152f * lin(c.mV[VGREEN]) + 0.0722f * lin(c.mV[VBLUE]);
    }

    /**
     * §3.3 "a plate in the theme (parchment-toned in wolves, steel in plain)". The parchment is
     * opaque, with the contrasts worked as wolfgamehud.cpp's palettes are (WCAG relative luminance):
     *   base #ddd0b0 (L 0.636): ink #2b2118 10.3:1, dim #5c4b36 5.4:1, accent #7a4e00 4.7:1
     *   buttons #cfbb8a (L 0.506): ink 8.3:1, dim 4.6:1
     *   the accent (focus ring, timer bar, brackets) is 4.7:1 against the plate (1.4.11 needs 3:1)
     * The plain theme's cards use the plain palette (its figures are in wolfgamehud.cpp).
     * Never BTN_PRIMARY or BTN_DANGER on parchment: those inks are for dark plates.
     */
    const Palette& cardPalette()
    {
        if (!WolfGame::instance().wolvesTheme()) return plainPalette();
        static Palette p;
        static bool init = false;
        if (!init)
        {
            init = true;
            p.mWolves = true;
            p.mBaseTop = hex(0xeadfc4, 0.98f);
            p.mBaseBottom = hex(0xddd0b0, 0.98f);
            p.mCard = hex(0xf3ead6, 0.95f);
            p.mText = hex(0x2b2118);
            p.mDim = hex(0x5c4b36);
            p.mAccent = hex(0x7a4e00);
            p.mEdge = hex(0x7a4e00, 0.55f);
            p.mTrack = hex(0xc4b183);
            p.mButtonTop = hex(0xdccba0);
            p.mButtonBottom = hex(0xcfbb8a);
            p.mButtonEdge = hex(0x5c4b36, 0.6f);
            p.mOnAccent = hex(0xf3ead6);
        }
        return p;
    }

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

    /** "45 s", "2 min 5 s" - the time as words (§3.3 "a thin bar plus 45 s in words"). */
    std::string secondsText(F64 s)
    {
        const S32 n = (S32)ceil(llmax(0.0, s));
        if (n < 60) return llformat("%d s", n);
        return llformat("%d min %d s", n / 60, n % 60);
    }

    /** The pause glyph: two bars (with the word beside it, never alone). */
    void pauseGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        rect(cx - s * 0.45f, cy - s * 0.5f, cx - s * 0.12f, cy + s * 0.5f, c);
        rect(cx + s * 0.12f, cy - s * 0.5f, cx + s * 0.45f, cy + s * 0.5f, c);
    }

    void triangle(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(c.mV);
        gGL.vertex2f(x1, y1); gGL.vertex2f(x2, y2); gGL.vertex2f(x3, y3);
        gGL.end();
    }

    /** A picture fitted whole inside the box (contain), centred. False while it has nothing to draw. */
    bool imageContain(LLViewerTexture* tex, F32 l, F32 b, F32 r, F32 t)
    {
        if (!tex || r <= l || t <= b) return false;
        const F32 iw = (F32)llmax(1, tex->getFullWidth()), ih = (F32)llmax(1, tex->getFullHeight());
        const F32 bw = r - l, bh = t - b;
        F32 w = bw, h = bw * ih / iw;
        if (h > bh) { h = bh; w = bh * iw / ih; }
        const F32 x = l + (bw - w) * 0.5f, y = b + (bh - h) * 0.5f;
        // cover of a box with the picture's own shape = the whole picture
        return imageCover(tex, x, y, x + w, y + h, 0.f, LLColor4::white);
    }

    /** The world view's size in UI units. */
    S32 worldW() { return gViewerWindow ? gViewerWindow->getWorldViewWidthScaled() : 1024; }
    S32 worldH() { return gViewerWindow ? gViewerWindow->getWorldViewHeightScaled() : 768; }

    enum ECardHit { CH_CLOSE = 1, CH_OPTION, CH_MORE, CH_CONTINUE };
    enum EHandoutHit { HH_CLOSE = 1 };
    const F32 PEEK = 7.f;   // how much of each older card shows above the top one (§3.3 "stack")
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// windows
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfAdventureUI
{
    LLFloater* HeldFloater::get()
    {
        if (LLFloater* f = mHandle.get()) return f;
        const F64 t = now();
        if (t - mLookedAt < 1.0) return nullptr;
        mLookedAt = t;
        LLFloater* f = LLFloaterReg::findInstance(mName);
        hold(f);
        return f;
    }

    WolfGameDraw::Palette lookPalette(const WolfGameDraw::Palette& base, const std::string& adv)
    {
        if (!WolfAdventure::instanceExists()) return base;
        const WolfAdventure::Adventure* a = WolfAdventure::instance().find(adv);
        if (!a || !a->mQuest.mLook.mColours) return base;
        const WolfAdventure::Look& lk = a->mQuest.mLook;
        auto withA = [](LLColor4 c, F32 alpha) { c.mV[VALPHA] = alpha; return c; };
        auto mix = [](const LLColor4& x, const LLColor4& y, F32 t)
        {
            return LLColor4(x.mV[VRED] + (y.mV[VRED] - x.mV[VRED]) * t, x.mV[VGREEN] + (y.mV[VGREEN] - x.mV[VGREEN]) * t,
                            x.mV[VBLUE] + (y.mV[VBLUE] - x.mV[VBLUE]) * t, 1.f);
        };
        WolfGameDraw::Palette p = base;
        const LLColor4 panel = withA(lk.mPanel, 1.f);
        const LLColor4 ink = WolfAdvStudio::inkOn(panel);
        // the fills keep the base's own see-through (the HUD over the world), in the adventure's colour
        p.mBaseTop = withA(mix(panel, ink, 0.06f), base.mBaseTop.mV[VALPHA]);
        p.mBaseBottom = withA(panel, base.mBaseBottom.mV[VALPHA]);
        p.mCard = withA(panel, llmax(base.mCard.mV[VALPHA], 0.92f));
        p.mText = ink;
        // secondary text: ink eased toward the panel, but never under WCAG 1.4.3's 4.5:1 (else the ink itself)
        LLColor4 dim = mix(panel, ink, 0.72f);
        if (WolfAdvStudio::contrast(dim, panel) < 4.5f) dim = ink;
        p.mDim = dim;
        p.mAccent = withA(lk.mAccent, 1.f);   // the brain checked accent on panel >= 4.5:1 (ADVENTURE_PACK_V1.md §15 rule 14)
        p.mEdge = withA(lk.mAccent, 0.7f);
        p.mTrack = withA(mix(panel, ink, 0.18f), 1.f);
        // a button's face eased toward the ink, unless that would take its text under 4.5:1 (a mid-grey panel)
        auto face = [&](F32 t) { const LLColor4 c = mix(panel, ink, t); return WolfAdvStudio::contrast(ink, c) >= 4.5f ? c : panel; };
        p.mButtonTop = face(0.14f);
        p.mButtonBottom = face(0.06f);
        if (WolfAdvStudio::contrast(ink, withA(p.mBaseTop, 1.f)) < 4.5f) p.mBaseTop = withA(panel, base.mBaseTop.mV[VALPHA]);
        p.mButtonEdge = withA(mix(panel, ink, 0.35f), 1.f);
        p.mOnAccent = WolfAdvStudio::inkOn(withA(lk.mAccent, 1.f));
        return p;
    }

    void syncCards()
    {
        static HeldFloater held("wolf_adventure_card");
        const WolfAdventure::Card* top = WolfAdventure::instance().topCard();
        WolfFloaterAdventureCard* f = static_cast<WolfFloaterAdventureCard*>(held.get());
        if (!top)
        {
            if (f && f->getVisible()) f->closeFloater();
            return;
        }
        if (!f || !f->getVisible())
        {
            // Not modal and never takes the keyboard: like a game dialog (wolffloaterroleplay.cpp
            // WolfFloaterGameDialog::showDialog, after LLScriptFloater::show).
            f = LLFloaterReg::showTypedInstance<WolfFloaterAdventureCard>("wolf_adventure_card", LLSD(), false);
            held.hold(f);
            if (f) f->fit(true);
        }
    }

    void closeWindows()
    {
        LLFloaterReg::hideInstance("wolf_adventure_card");
        LLFloaterReg::hideInstance("wolf_adventure_talk");   // [WOLF ADVENTURE P2 2026-10-16/]
        LLFloaterReg::hideInstance("wolf_adventure_handout");
        LLFloaterReg::hideInstance("wolf_adventure_studio");
    }

    void focusCard()
    {
        // the player's own action (the Journal's Choose...): the card takes the keyboard, so Tab,
        // Enter and the number keys reach it (§6 2.1.1); a card never takes it by itself
        syncCards();
        if (WolfFloaterAdventureCard* f = LLFloaterReg::findTypedInstance<WolfFloaterAdventureCard>("wolf_adventure_card"))
        {
            f->takeKeyboard();
        }
    }

    bool escapeFocusedCard()
    {
        WolfFloaterAdventureCard* f = LLFloaterReg::findTypedInstance<WolfFloaterAdventureCard>("wolf_adventure_card");
        if (!f || !f->getVisible() || !gFocusMgr.childHasKeyboardFocus(f)) return false;
        const WolfAdventure::Card* c = WolfAdventure::instance().topCard();
        if (c) WolfAdventure::instance().putAway(c->mAdv, c->mId);
        return true;
    }

    void showHandout(const std::string& adv, const std::string& handout_id)
    {
        if (!WolfGrid::isOnWolfTerritories()) return;
        WolfFloaterAdventureHandout* f = LLFloaterReg::showTypedInstance<WolfFloaterAdventureHandout>("wolf_adventure_handout", LLSD(), true);
        if (f) f->show(adv, handout_id);
    }

    // ═══════════════════════════════════════════════════════════════════════════════════
    // the stage (§3.6)
    // ═══════════════════════════════════════════════════════════════════════════════════

    void drawStage(const LLRect& r, LLRect& title_rect)
    {
        title_rect = LLRect();
        const WolfAdventure& wa = WolfAdventure::instance();
        const F32 l = (F32)r.mLeft, b = (F32)r.mBottom, rr = (F32)r.mRight, t = (F32)r.mTop, H = (F32)r.getHeight();
        const F32 fd = wa.fadeNow();
        if (fd > 0.f) rect(l, b, rr, t, LLColor4(0.f, 0.f, 0.f, llclamp(fd, 0.f, 1.f)));
        // Source: HUD_MESSAGES_V1.md §2.6 letterbox - black bars top and bottom, each 11 % of the
        // window's height. The HUD and talk panel are drawn after this, so they stay above the bars.
        const F32 box = wa.letterboxNow();
        if (box > 0.f)
        {
            const F32 bh = floorf(0.11f * H * llclamp(box, 0.f, 1.f));
            rect(l, b, rr, b + bh, LLColor4::black);
            rect(l, t - bh, rr, t, LLColor4::black);
        }
        if (!wa.titleOn()) return;
        // §2.6 title: `text` large in Cinzel, `sub` under it, centred in the upper third - on the
        // game's card (silver on the wolves card is 10.0:1 even over a white sky, wolfgamehud.cpp).
        const F32 a = wa.titleAlpha();
        const Palette& p = gamePalette();
        const LLFontGL* big = display(llclamp(H * 0.07f, 28.f, 48.f));
        const LLFontGL* sub = display(20.f);
        const F32 tw = llmax(big->getWidthF32(wa.titleText()), wa.titleSub().empty() ? 0.f : sub->getWidthF32(wa.titleSub()));
        // [WOLF ADVENTURE AUDIT 2026-10-17] §2.6 title `image` (the adventure's title card): the picture fitted in the card's
        // upper part, the words below it on the card itself (the card colour keeps them at 4.5:1 whatever the picture is)
        LLViewerFetchedTexture* pic = nullptr;
        if (!wa.titleImage().empty())
        {
            pic = LLViewerTextureManager::getFetchedTexture(LLUUID(wa.titleImage()), FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_UI);
            if (pic) pic->addTextureStats(256.f * 256.f);
        }
        const F32 pic_h = pic ? llclamp(H * 0.22f, 96.f, 220.f) : 0.f;
        const F32 w = llmin(rr - l - 40.f, llmax(tw + 80.f, pic ? pic_h * 1.6f : 0.f));
        const F32 h = (F32)big->getLineHeight() + (wa.titleSub().empty() ? 0.f : (F32)sub->getLineHeight() + 6.f) + 36.f + (pic ? pic_h + 12.f : 0.f);
        const F32 cx = (l + rr) * 0.5f, cy = b + H * 0.72f - (pic ? pic_h * 0.5f : 0.f);
        const F32 pl = cx - w * 0.5f, pb = cy - h * 0.5f, pr = cx + w * 0.5f, pt = cy + h * 0.5f;
        chamfer(pl, pb, pr, pt, 12.f, fade(p.mCard, a), fade(p.mCard, a));
        chamferEdge(pl + 0.5f, pb + 0.5f, pr - 0.5f, pt - 0.5f, 12.f, 1.f, fade(p.mEdge, a));
        rect(pl + 30.f, pt - 8.f, pr - 30.f, pt - 7.f, fade(p.mAccent, 0.6f * a));
        rect(pl + 30.f, pb + 7.f, pr - 30.f, pb + 8.f, fade(p.mAccent, 0.6f * a));
        F32 y = pt - 18.f;
        if (pic)
        {
            // fitted, aspect kept once the picture's size is known (a square until then)
            const F32 iw0 = (F32)llmax(1, (S32)pic->getFullWidth()), ih0 = (F32)llmax(1, (S32)pic->getFullHeight());
            const F32 sc = llmin((w - 40.f) / iw0, pic_h / ih0);
            const F32 iw = pic->getFullWidth() > 0 ? iw0 * sc : pic_h, ih = pic->getFullHeight() > 0 ? ih0 * sc : pic_h;
            gl_draw_scaled_image((S32)(cx - iw * 0.5f), (S32)(y - ih), (S32)iw, (S32)ih, pic, LLColor4(1.f, 1.f, 1.f, a));
            y -= pic_h + 12.f;
        }
        textL(big, wa.titleText(), cx, y, fade(p.mText, a), LLFontGL::HCENTER, LLFontGL::TOP, w - 20.f);
        y -= (F32)big->getLineHeight() + 6.f;
        if (!wa.titleSub().empty()) textL(sub, wa.titleSub(), cx, y, fade(p.mAccent, a), LLFontGL::HCENTER, LLFontGL::TOP, w - 20.f);
        title_rect = LLRect((S32)pl, (S32)pt, (S32)pr, (S32)pb);
    }

    // ═══════════════════════════════════════════════════════════════════════════════════
    // the objective line (§3.1)
    // ═══════════════════════════════════════════════════════════════════════════════════

    bool drawObjective(F32 right, F32 y, bool from_top, LLRect& out)
    {
        out = LLRect();
        const WolfAdventure& wa = WolfAdventure::instance();
        const WolfAdventure::Adventure* a = wa.current();
        if (!a) return false;
        const WolfAdventure::Quest& q = a->mQuest;
        // [WOLF ADVENTURE AUDIT 2026-10-17] the adventure's own colours and logo (HUD_MESSAGES_V1.md §2.1 look)
        const Palette p = WolfAdventureUI::lookPalette(gamePalette(), q.mAdv);
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        const LLFontGL* body = LLFontGL::getFontSansSerif();
        const F32 slh = (F32)small->getLineHeight(), lh = (F32)body->getLineHeight();
        const F32 W = 340.f, PAD = 10.f;
        const F32 l = right - W;
        const bool logo = !q.mLook.mLogo.empty();
        const F32 LOGO = 36.f, logo_w = logo ? LOGO + 8.f : 0.f;
        std::string objective = q.mObjective;
        if (q.mEnded.mOn) objective = "Complete: " + (q.mEnded.mTitle.empty() ? q.mSrc : q.mEnded.mTitle);
        else if (objective.empty()) objective = "Nothing to do right now - the story will tell you.";
        const std::vector<std::string> lines = wrap(body, objective, W - 2.f * PAD - 6.f - logo_w, 2);
        const size_t waiting = wa.waitingCards().size();
        const bool status = (q.mPaused && !q.mEnded.mOn) || q.mGm || waiting > 0;
        const F32 h = llmax(PAD + slh + 3.f + lh * (F32)lines.size() + (status ? slh + 5.f : 0.f) + PAD, logo ? LOGO + 2.f * PAD : 0.f);
        const F32 b = from_top ? y - h : y, t = b + h;
        frame(l, b, right, t, p, 1.f, 8.f);
        // a gold rule down the left: the RDR2 objective line's mark
        rect(l + 4.f, b + 8.f, l + 6.f, t - 8.f, p.mAccent);
        F32 ty = t - PAD;
        // the logo at the top left (a texture uuid; a plain frame while it loads, wolfgamehud.cpp icon())
        if (logo) icon(q.mLook.mLogo, l + PAD + 4.f, t - PAD - LOGO, LOGO, p, std::string(), true);
        const F32 tl = l + PAD + 4.f + logo_w;
        // the chapter, small - and the distance to the marker in metres on the right
        std::string dist;
        const F32 metres = WolfAdventure::markerDistance(q);
        if (metres >= 0.f) dist = thousands((S64)llround(metres)) + " m";
        const F32 dist_w = dist.empty() ? 0.f : bold->getWidthF32(dist) + 8.f;
        const std::string head = !q.mChapter.empty() ? q.mChapter : (!q.mSrc.empty() ? q.mSrc : std::string("Adventure"));
        textL(bold, caps(head), tl, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, right - PAD - tl - dist_w);
        if (!dist.empty()) textL(bold, dist, right - PAD, ty, p.mAccent, LLFontGL::RIGHT, LLFontGL::TOP);
        ty -= slh + 3.f;
        for (const std::string& line : lines)
        {
            textL(body, line, tl, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, right - PAD - tl);
            ty -= lh;
        }
        if (status)
        {
            ty -= 3.f;
            F32 x = tl;
            if (q.mPaused && !q.mEnded.mOn)
            {
                pauseGlyph(x + 5.f, ty - slh * 0.5f, 9.f, p.mAccent);
                x = textL(bold, "Paused", x + 14.f, ty, p.mAccent) + 12.f;
            }
            if (q.mGm)
            {
                const F32 gw = bold->getWidthF32("GM") + 10.f;
                chamferEdge(x, ty - slh, x + gw, ty, 3.f, 1.f, p.mAccent);
                textL(bold, "GM", x + gw * 0.5f, ty - slh * 0.5f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
                x += gw + 10.f;
            }
            if (waiting > 0)
            {
                textL(bold, waiting == 1 ? std::string("1 choice waiting") : llformat("%d choices waiting", (S32)waiting), x, ty, p.mText,
                      LLFontGL::LEFT, LLFontGL::TOP, right - PAD - x);
            }
        }
        out = LLRect((S32)l, (S32)t, (S32)right, (S32)b);
        return true;
    }

    std::string objectiveTip()
    {
        const WolfAdventure::Adventure* a = WolfAdventure::instance().current();
        if (!a) return std::string();
        const WolfAdventure::Quest& q = a->mQuest;
        std::string tip = q.mSrc.empty() ? std::string("Adventure") : q.mSrc;
        if (!q.mChapter.empty()) tip += " - " + q.mChapter;
        if (!q.mObjective.empty()) tip += "\n" + q.mObjective;
        if (q.mPaused) tip += "\n" + (q.mPausedText.empty() ? std::string("The story is paused.") : q.mPausedText);
        if (q.mGm) tip += "\nA game master is running this story.";
        return tip + "\nClick for the Journal";
    }

    // ═══════════════════════════════════════════════════════════════════════════════════
    // reputation (§3.5)
    // ═══════════════════════════════════════════════════════════════════════════════════

    std::string opinionWord(S32 v)
    {
        // Source: HUD_MESSAGES_V1.md §3.5 - Hostile / Wary / Neutral / Friendly / Trusted, from
        // -100...-60...-20...20...60...100 (a design choice, used when the pack defines none).
        if (v < -60) return "Hostile";
        if (v < -20) return "Wary";
        if (v <= 20) return "Neutral";
        if (v <= 60) return "Friendly";
        return "Trusted";
    }

    std::string bandWord(const WolfAdventure::Meter& m)
    {
        if (m.mHasBand && !m.mBand.mLabel.empty()) return m.mBand.mLabel;
        std::string word;
        for (const WolfAdventure::Band& b : m.mBands)
        {
            if (m.mValue >= b.mFrom) word = b.mLabel;
        }
        return word;
    }

    F32 meterBar(const WolfAdventure::Meter& m, F32 l, F32 r, F32 top, const Palette& p, bool big)
    {
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        const F32 slh = (F32)small->getLineHeight();
        const std::string band = bandWord(m);
        F32 y = top;
        // the label row: the name on the left, the number and the band as words on the right
        const std::string value = std::to_string(m.mValue) + (band.empty() ? std::string() : " \xC2\xB7 " + band);
        textL(big ? bold : small, value, r, y, p.mText, LLFontGL::RIGHT, LLFontGL::TOP);
        textL(big ? display(17.f) : bold, m.mLabel, l, y + (big ? 2.f : 0.f), p.mText, LLFontGL::LEFT, LLFontGL::TOP,
              r - l - (big ? bold : small)->getWidthF32(value) - 12.f);
        y -= (big ? (F32)display(17.f)->getLineHeight() : slh) + 4.f;
        const F32 range = (F32)llmax(1, m.mMax - m.mMin);
        auto xOf = [&](S32 v) { return l + (r - l) * llclamp((F32)(v - m.mMin) / range, 0.f, 1.f); };
        const F32 bar_h = big ? 22.f : 12.f;
        const F32 bt = y - 10.f, bb = bt - bar_h;   // 10 px above the bar for the marker
        rect(l, bb, r, bt, p.mTrack);
        if (!m.mBands.empty())
        {
            // band segments in their colours, each band's label inside it on a pill (§3.5; §6 1.4.3
            // "a dark or light pill picked per band colour")
            for (size_t i = 0; i < m.mBands.size(); ++i)
            {
                const WolfAdventure::Band& bd = m.mBands[i];
                const S32 from = llmax(m.mMin, bd.mFrom);
                const S32 to = i + 1 < m.mBands.size() ? llmin(m.mMax, m.mBands[i + 1].mFrom) : m.mMax;
                if (to <= from) continue;
                const F32 x0 = xOf(from), x1 = xOf(to);
                const LLColor4 c = bd.mHasColour ? bd.mColour : (i % 2 ? p.mTrack : readable(p.mTrack, 0.12f));
                rect(x0, bb, x1, bt, c);
                if (i > 0) rect(x0, bb, x0 + 1.f, bt, LLColor4(0.f, 0.f, 0.f, 0.6f));
                if (big && !bd.mLabel.empty())
                {
                    const F32 lw = small->getWidthF32(bd.mLabel) + 8.f;
                    if (lw < x1 - x0 - 4.f)
                    {
                        const bool light = lum(c) > 0.18f;
                        const F32 cx = (x0 + x1) * 0.5f, cy = (bb + bt) * 0.5f;
                        rect(cx - lw * 0.5f, cy - slh * 0.5f, cx + lw * 0.5f, cy + slh * 0.5f,
                             light ? LLColor4(1.f, 1.f, 1.f, 0.85f) : LLColor4(0.f, 0.f, 0.f, 0.75f));
                        textL(small, bd.mLabel, cx, cy, light ? LLColor4(0.05f, 0.05f, 0.05f, 1.f) : LLColor4::white,
                              LLFontGL::HCENTER, LLFontGL::VCENTER);
                    }
                }
            }
        }
        else
        {
            // no bands defined: one plain bar with the marker and no segment labels (§2.5 note); a
            // tick at 0 so the side of zero reads at a glance
            const S32 zero = llclamp(0, m.mMin, m.mMax);
            const F32 x0 = xOf(zero);
            rect(x0, bb - 2.f, x0 + 1.f, bt + 2.f, p.mDim);
        }
        // the bar's edge at 3:1 or better against the plate (§6 1.4.11)
        chamferEdge(l - 0.5f, bb - 0.5f, r + 0.5f, bt + 0.5f, 0.f, 1.f, fade(p.mDim, 0.9f));
        // the marker at the value: a line through the bar (black under white, readable on any band)
        // and a pointer above it with the number
        const F32 mx = xOf(m.mValue);
        rect(mx - 2.f, bb - 3.f, mx + 2.f, bt + 3.f, LLColor4::black);
        rect(mx - 1.f, bb - 2.f, mx + 1.f, bt + 2.f, LLColor4::white);
        triangle(mx - 6.f, bt + 10.f, mx + 6.f, bt + 10.f, mx, bt + 3.f, p.mAccent);
        F32 bottom = bb - 4.f;
        if (big)
        {
            // "Honour: 15 · Drifter" under it (§3.5)
            textL(small, m.mLabel + ": " + std::to_string(m.mValue) + (band.empty() ? std::string() : " \xC2\xB7 " + band), l, bottom,
                  p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
            bottom -= slh + 2.f;
        }
        return bottom;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the card (§3.3)
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventureCardView::WolfAdventureCardView(const Params& p)
:   LLUICtrl(p)
{
}

S32 WolfAdventureCardView::measure(S32 width) const
{
    // measured again only when the cards change or the width does (it wraps the whole card)
    const U32 gen = WolfAdventure::instance().generation();
    if (gen != mMeasuredGen || width != mMeasuredWidth)
    {
        mMeasuredGen = gen;
        mMeasuredWidth = width;
        mMeasured = (S32)ceilf(layout((F32)width, 0.f, false));
    }
    return mMeasured;
}

void WolfAdventureCardView::takeKeyboard()
{
    mKeyboardNav = true;
    setFocus(true);
}

/**
 * Lays the top card out from the top down; with `draw` it also draws and records the hits.
 * Returns the height it needs. The image is at most 40 % of the card's height (§3.3).
 */
F32 WolfAdventureCardView::layout(F32 W, F32 H, bool draw) const
{
    const WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Card* c = wa.topCard();
    if (!c) return 0.f;
    const Palette& p = cardPalette();
    const LLFontGL* title_font = display(20.f);
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 lh = (F32)body->getLineHeight(), slh = (F32)small->getLineHeight(), tlh = (F32)title_font->getLineHeight();
    const F32 PAD = 16.f, cw = W - 2.f * PAD, top = H;
    // older cards still up (not put away) peek above this one: at most two (§3.3 "at most 3 visible")
    S32 under = 0;
    for (const WolfAdventure::Card& k : wa.cards())
    {
        if (!k.mWaiting && k.mId != c->mId) ++under;
    }
    under = llmin(under, 2);
    const std::vector<std::string> title = wrap(title_font, c->mTitle, cw - 26.f, 2);
    const std::vector<std::string> text = wrap(body, c->mText, cw, 30);
    const bool chosen = !c->mChosen.empty();
    const bool timed = c->mEndsAt > 0.0 && !chosen && !c->mOptions.empty();
    const bool more = WolfAdventure::moreTimeAllowed(*c);

    // everything but the picture, to size the picture against (§3.3 "at most 40 % of the height")
    F32 rest = PEEK * under + 12.f + slh + 6.f + tlh * (F32)title.size() + 4.f + (c->mSpeaker.empty() ? 0.f : 42.f)
             + lh * (F32)text.size() + 10.f;
    if (c->mOptions.empty()) rest += 32.f + 8.f;
    else
    {
        if (!c->mMayChoose || chosen) rest += slh + 6.f;
        for (const WolfAdventure::Option& o : c->mOptions) rest += (o.mDetail.empty() ? 32.f : 46.f) + 6.f;
    }
    if (timed) rest += 6.f + slh + 4.f + (more ? 32.f : 0.f);
    rest += PAD;
    F32 img_h = 0.f;
    if (!c->mImage.empty()) img_h = floorf(llmin(llmin(cw * 0.5f, 220.f), rest * 2.f / 3.f));
    const F32 total = rest + (img_h > 0.f ? img_h + 8.f : 0.f);
    if (!draw) return total;

    mHits.clear();
    // the older cards behind, a few pixels each above this one
    for (S32 k = under; k >= 1; --k)
    {
        const F32 inset = 10.f * (F32)k;
        frame(inset, top - PEEK * (F32)(under - k) - total + PEEK * (F32)under, W - inset, top - PEEK * (F32)(under - k), p, 0.85f, 10.f);
    }
    const F32 card_t = top - PEEK * (F32)under;
    frame(0.f, 0.f, W, card_t, p, 1.f, 10.f);
    F32 y = card_t - 12.f;
    // the source over the title, and the X
    textL(bold, caps(c->mSrc.empty() ? std::string("Adventure") : c->mSrc), PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cw - 30.f);
    {
        const F32 xc = W - PAD - 4.f, yc = y - slh * 0.5f;
        closeX(xc, yc, 5.f, mHits.isHover(CH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(CH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
        mHits.add(xc - 12.f, yc - 12.f, xc + 12.f, yc + 12.f, CH_CLOSE, 0,
                  c->mOptions.empty() ? std::string("Close this card (Escape)")
                                      : std::string("Put this card away (Escape) - it waits in the Journal until you choose"));
    }
    y -= slh + 6.f;
    for (const std::string& line : title)
    {
        textL(title_font, line, PAD, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw);
        y -= tlh;
    }
    y -= 4.f;
    if (!c->mSpeaker.empty())
    {
        icon(c->mPortrait, PAD, y - 36.f, 36.f, p, initials(c->mSpeaker));
        textL(bold, c->mSpeaker, PAD + 44.f, y - 18.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, cw - 44.f);
        y -= 42.f;
    }
    if (img_h > 0.f)
    {
        chamfer(PAD, y - img_h, W - PAD, y, 6.f, LLColor4(0.f, 0.f, 0.f, 0.25f), LLColor4(0.f, 0.f, 0.f, 0.25f));
        imageContain(iconTexture(c->mImage), PAD, y - img_h, W - PAD, y);
        chamferEdge(PAD + 0.5f, y - img_h + 0.5f, W - PAD - 0.5f, y - 0.5f, 6.f, 1.f, p.mEdge);
        y -= img_h + 8.f;
    }
    for (const std::string& line : text)
    {
        textL(body, line, PAD, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw);
        y -= lh;
    }
    y -= 10.f;
    if (c->mOptions.empty())
    {
        // §3.3 "A card with no options closes on any button or Escape"
        U32 flags = 0;
        if (mHits.isHover(CH_CONTINUE, 0)) flags |= BTN_HOVER;
        if (mHits.isPressed(CH_CONTINUE, 0)) flags |= BTN_PRESSED;
        if (mKeyboardNav && hasFocus() && mHits.isFocused(CH_CONTINUE, 0)) flags |= BTN_FOCUS;
        button(PAD, y - 32.f, W - PAD, y, "Continue", p, flags);
        mHits.add(PAD, y - 32.f, W - PAD, y, CH_CONTINUE, 0, "Close this card");
        y -= 32.f + 8.f;
    }
    else
    {
        if (chosen)
        {
            textL(bold, "Chosen: " + c->mChosen, PAD, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, cw);
            y -= slh + 6.f;
        }
        else if (!c->mMayChoose)
        {
            // §2.3 mayChoose 0: the options without buttons
            const std::string who = c->mWho == "vote" ? "Waiting for the vote" : "Waiting for the leader to choose";
            textL(bold, who, PAD, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cw);
            y -= slh + 6.f;
        }
        // §3.3 note: when the timer reaches 0 the options are disabled until the brain's close
        const bool time_up = c->mEndsAt > 0.0 && now() >= c->mEndsAt;
        const bool may = c->mMayChoose && !chosen && !time_up;
        for (size_t i = 0; i < c->mOptions.size(); ++i)
        {
            const WolfAdventure::Option& o = c->mOptions[i];
            const F32 oh = o.mDetail.empty() ? 32.f : 46.f;
            const S32 arg = (S32)i;
            const bool this_one = chosen && o.mLabel == c->mChosen;
            U32 flags = may ? 0 : BTN_DISABLED;
            if (this_one) flags = BTN_ACTIVE;
            if (may && mHits.isHover(CH_OPTION, arg)) flags |= BTN_HOVER;
            if (may && mHits.isPressed(CH_OPTION, arg)) flags |= BTN_PRESSED;
            if (may && mKeyboardNav && hasFocus() && mHits.isFocused(CH_OPTION, arg)) flags |= BTN_FOCUS;
            button(PAD, y - oh, W - PAD, y, std::string(), p, flags);
            const LLColor4 ink = may || this_one ? p.mText : p.mDim;
            // the number key, the label, the detail under it; the tally on the right in a vote
            std::string tally;
            if (c->mWho == "vote" && o.mTally >= 0) tally = o.mTally == 1 ? std::string("1 vote") : llformat("%d votes", o.mTally);
            if (this_one) tally = tally.empty() ? std::string("Chosen") : "Chosen - " + tally;
            const F32 tw = tally.empty() ? 0.f : bold->getWidthF32(tally) + 12.f;
            const F32 tx = PAD + 12.f;
            const F32 key_r = textL(bold, llformat("%d.", (S32)i + 1), tx, y - 16.f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
            textL(body, o.mLabel, key_r + 6.f, y - 16.f, ink, LLFontGL::LEFT, LLFontGL::VCENTER, W - PAD - 10.f - tw - key_r - 6.f);
            if (!o.mDetail.empty())
            {
                textL(small, o.mDetail, key_r + 6.f, y - 30.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - PAD - 10.f - tw - key_r - 6.f);
            }
            if (!tally.empty()) textL(bold, tally, W - PAD - 10.f, y - 16.f, ink, LLFontGL::RIGHT, LLFontGL::VCENTER);
            std::string tip = o.mLabel;
            if (!o.mDetail.empty()) tip += " - " + o.mDetail;
            tip += may ? llformat(" (key %d)", (S32)i + 1) : (this_one ? std::string(" - your choice") : std::string());
            mHits.add(PAD, y - oh, W - PAD, y, CH_OPTION, arg, tip, may, may);
            y -= oh + 6.f;
        }
    }
    if (timed)
    {
        // §3.3 a thin bar plus the seconds in words
        y -= 6.f;
        const F64 left = llmax(0.0, c->mEndsAt - now());
        const F32 frac = c->mSec > 0.f ? llclamp((F32)(left / (F64)c->mSec), 0.f, 1.f) : 0.f;
        const std::string words = left > 0.0 ? secondsText(left) + " left" : std::string("Time is up - waiting for the story");
        textL(small, words, W - PAD, y, p.mText, LLFontGL::RIGHT, LLFontGL::TOP);
        const F32 bar_r = W - PAD - small->getWidthF32(words) - 12.f;
        rect(PAD, y - slh * 0.5f - 2.f, bar_r, y - slh * 0.5f + 2.f, p.mTrack);
        rect(PAD, y - slh * 0.5f - 2.f, PAD + (bar_r - PAD) * frac, y - slh * 0.5f + 2.f, p.mAccent);
        y -= slh + 4.f;
        if (more)
        {
            U32 flags = 0;
            if (mHits.isHover(CH_MORE, 0)) flags |= BTN_HOVER;
            if (mHits.isPressed(CH_MORE, 0)) flags |= BTN_PRESSED;
            if (mKeyboardNav && hasFocus() && mHits.isFocused(CH_MORE, 0)) flags |= BTN_FOCUS;
            button(PAD, y - 26.f, PAD + 120.f, y, "More time", p, flags);
            mHits.add(PAD, y - 26.f, PAD + 120.f, y, CH_MORE, 0,
                      c->mWho == "vote" ? std::string("Ask for 60 more seconds (once in a vote)") : std::string("Ask for 60 more seconds"));
            y -= 32.f;
        }
    }
    return total;
}

void WolfAdventureCardView::draw()
{
    const WolfAdventure::Card* c = WolfAdventure::instance().topCard();
    if (c && c->mId != mCardId)
    {
        // a new card: the focus starts on its first answer
        mCardId = c->mId;
        mHits.setFocus(c->mOptions.empty() ? CH_CONTINUE : CH_OPTION, 0);
    }
    LLGLSUIDefault gls_ui;
    layout((F32)getRect().getWidth(), (F32)getRect().getHeight(), true);
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureCardView::press(S32 id, S32 arg)
{
    WolfAdventure& wa = WolfAdventure::instance();
    const WolfAdventure::Card* c = wa.topCard();
    if (!c) return;
    const std::string card = c->mId, adv = c->mAdv;
    switch (id)
    {
    case CH_CLOSE:
    case CH_CONTINUE:
        wa.putAway(adv, card);
        break;
    case CH_OPTION:
        if (arg >= 0 && arg < (S32)c->mOptions.size() && c->mMayChoose && c->mChosen.empty() && !(c->mEndsAt > 0.0 && now() >= c->mEndsAt))
        {
            wa.choose(adv, card, c->mOptions[arg].mId);
        }
        break;
    case CH_MORE:
        if (!wa.askMoreTime(adv, card))
        {
            WolfGame::instance().addNotice(c->mTitle.empty() ? std::string("More time") : c->mTitle,
                                           "More time was not asked for: the viewer is not connected to a Wolf Territories region.",
                                           WolfGame::NOTICE_DANGER);   // [WOLF ADVENTURE AUDIT 2026-10-17] a failure stays until read
        }
        break;
    default:
        break;
    }
}

bool WolfAdventureCardView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;   // the floater: drag it
    mKeyboardNav = false;
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAdventureCardView::handleMouseUp(S32 x, S32 y, MASK mask)
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

void WolfAdventureCardView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
}

bool WolfAdventureCardView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(h->mEnabled ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfAdventureCardView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfAdventureCardView::handleKeyHere(KEY key, MASK mask)
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
    case KEY_UP:
    case KEY_LEFT:
    case KEY_DOWN:
    case KEY_RIGHT:
        // §6 2.1.1 "arrow keys in option lists"
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_DOWN || key == KEY_RIGHT);
        return true;
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled) press(f->mId, f->mArg);
        return true;
    }
    case KEY_ESCAPE:
        press(CH_CLOSE, 0);
        return true;
    default:
        return false;
    }
}

bool WolfAdventureCardView::handleUnicodeCharHere(llwchar uni_char)
{
    if (uni_char == ' ')
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled) press(f->mId, f->mArg);
        return true;
    }
    // the number keys choose (the "1." on each option)
    if (uni_char >= '1' && uni_char <= '6')
    {
        const WolfAdventure::Card* c = WolfAdventure::instance().topCard();
        const S32 i = (S32)(uni_char - '1');
        if (c && i < (S32)c->mOptions.size()) press(CH_OPTION, i);
        return true;
    }
    return false;
}

void WolfFloaterAdventureCard::takeKeyboard()
{
    if (mView) mView->takeKeyboard();
}

WolfFloaterAdventureCard::WolfFloaterAdventureCard(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureCard::postBuild()
{
    mView = getChild<WolfAdventureCardView>("view");
    // In front of the drag handle, as WolfFloaterRoleplay::postBuild explains.
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterAdventureCard::fit(bool place)
{
    if (!mView) return;
    const WolfAdventure::Card* c = WolfAdventure::instance().topCard();
    if (!c) return;
    // §3.3 "at most 560 units wide"
    const S32 w = llclamp(worldW() - 32, 260, 560);
    const S32 h = llclamp(mView->measure(w), 80, llmax(120, worldH() - 40));
    if (h != mLastHeight || w != getRect().getWidth())
    {
        // keep the top where it is: the card grows downward (a timer row appearing does not lift it)
        const S32 top = getRect().mTop;
        mLastHeight = h;
        reshape(w, h);
        LLRect r = getRect();
        r.setLeftTopAndSize(r.mLeft, top, w, h);
        setRect(r);
        mView->setRect(LLRect(0, h, w, 0));
    }
    if (place || c->mId != mPlacedFor)
    {
        mPlacedFor = c->mId;
        // centred, in the upper third (its middle at 5/6 of the height, kept under the
        // navigation bar - llscriptfloater.cpp getTopPad, the pad script dialogs use)
        const S32 W = worldW(), H = worldH();
        const S32 top_pad = LLScriptFloaterManager::instance().getTopPad();
        const S32 top = llmax(h, llmin(H - top_pad - 12, H * 5 / 6 + h / 2));
        LLRect r;
        r.setLeftTopAndSize((W - w) / 2, top, w, h);
        setRect(r);
    }
}

void WolfFloaterAdventureCard::draw()
{
    if (!WolfGrid::isOnWolfTerritories() || !WolfAdventure::instance().topCard())
    {
        closeFloater();
        return;
    }
    fit(false);
    LLFloater::draw();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the handout viewer (§3.4)
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    const WolfAdventure::Handout* findHandout(const std::string& adv, const std::string& id)
    {
        const WolfAdventure::Adventure* a = WolfAdventure::instance().find(adv);
        if (!a) return nullptr;
        for (const WolfAdventure::Handout& h : a->mJournal.mHandouts)
        {
            if (h.mId == id) return &h;
        }
        return nullptr;
    }
}

WolfAdventureHandoutView::WolfAdventureHandoutView(const Params& p)
:   LLUICtrl(p)
{
}

void WolfAdventureHandoutView::setHandout(const std::string& adv, const std::string& id)
{
    mAdv = adv;
    mId = id;
    mScroll = 0.f;
    mHits.setFocus(HH_CLOSE, 0);
}

void WolfAdventureHandoutView::draw()
{
    mHits.clear();
    LLGLSUIDefault gls_ui;
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const Palette& p = gamePalette();
    const LLFontGL* title_font = display(19.f);
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 PAD = 14.f, lh = (F32)body->getLineHeight();
    frame(0.f, 0.f, W, H, p, 1.f, 10.f);
    const WolfAdventure::Handout* ho = findHandout(mAdv, mId);
    F32 y = H - 12.f;
    {
        const F32 xc = W - PAD - 4.f, yc = y - 9.f;
        closeX(xc, yc, 5.f, mHits.isHover(HH_CLOSE, 1) ? p.mAccent : p.mText);
        mHits.add(xc - 12.f, yc - 12.f, xc + 12.f, yc + 12.f, HH_CLOSE, 1, "Close (Escape)", false);
    }
    if (!ho)
    {
        textL(title_font, "Handout", PAD, y, p.mText);
        textL(body, "This handout is no longer in the Journal.", PAD, y - (F32)title_font->getLineHeight() - 8.f, p.mDim);
        y = 46.f;
    }
    else
    {
        textL(title_font, ho->mTitle.empty() ? std::string("Handout") : ho->mTitle, PAD, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, W - 2.f * PAD - 24.f);
        y -= (F32)title_font->getLineHeight() + 8.f;
        // the text takes what it needs, up to 40 % of the window; the picture the rest (§3.4 "the
        // largest size that fits the window")
        const std::vector<std::string> lines = wrap(body, ho->mText, W - 2.f * PAD - 8.f, 200);
        const F32 foot = 46.f;
        const F32 text_h = ho->mText.empty() ? 0.f : llmin((F32)lines.size() * lh + 8.f, (y - foot) * (ho->mTexture.empty() ? 1.f : 0.4f));
        const F32 img_t = y, img_b = y - (y - foot - text_h - (text_h > 0.f ? 8.f : 0.f));
        if (!ho->mTexture.empty() && img_t - img_b > 20.f)
        {
            if (!imageContain(iconTexture(ho->mTexture), PAD, img_b, W - PAD, img_t))
            {
                textL(small, "Loading the picture...", W * 0.5f, (img_b + img_t) * 0.5f, p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
            y = img_b - 8.f;
        }
        if (text_h > 0.f)
        {
            const LLRect clip((S32)PAD, (S32)y, (S32)(W - PAD), (S32)(y - text_h));
            card(PAD - 4.f, y - text_h, W - PAD + 4.f, y, p, 4.f);
            const F32 content = (F32)lines.size() * lh + 8.f;
            mTextOverflow = llmax(0.f, content - text_h);
            mScroll = llclamp(mScroll, 0.f, mTextOverflow);
            LLLocalClipRect cr(clip);
            F32 ty = y - 4.f + mScroll;
            for (const std::string& line : lines)
            {
                if (ty - lh < y - text_h - lh) break;
                if (ty <= y + lh) textL(body, line, PAD + 4.f, ty, p.mText);
                ty -= lh;
            }
        }
        else
        {
            mTextOverflow = 0.f;
        }
    }
    // the buttons along the foot: Close, and Show on map for a map when the quest has a marker
    const F32 bb = 10.f, bh = 28.f;
    U32 flags = 0;
    if (mHits.isHover(HH_CLOSE, 0)) flags |= BTN_HOVER;
    if (mHits.isPressed(HH_CLOSE, 0)) flags |= BTN_PRESSED;
    if (mKeyboardNav && hasFocus() && mHits.isFocused(HH_CLOSE, 0)) flags |= BTN_FOCUS;
    button(W - PAD - 100.f, bb, W - PAD, bb + bh, "Close", p, flags);
    mHits.add(W - PAD - 100.f, bb, W - PAD, bb + bh, HH_CLOSE, 0, "Close the handout (Escape). It stays in the Journal.");
    // §3.4 note: a handout carries no position, so a map handout shows its picture and text only.
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureHandoutView::press(S32 id)
{
    if (id == HH_CLOSE)
    {
        if (LLFloater* f = getParentByType<LLFloater>()) f->closeFloater();
    }
}

bool WolfAdventureHandoutView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;
    mKeyboardNav = false;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAdventureHandoutView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    const S32 id = mHits.pressedId(), arg = mHits.pressedArg();
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == id && h->mArg == arg) press(id);
    return true;
}

void WolfAdventureHandoutView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
}

bool WolfAdventureHandoutView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(UI_CURSOR_HAND);
    return true;
}

bool WolfAdventureHandoutView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfAdventureHandoutView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mTextOverflow <= 0.f) return false;
    mScroll = llclamp(mScroll + (F32)clicks * 30.f, 0.f, mTextOverflow);
    return true;
}

bool WolfAdventureHandoutView::handleKeyHere(KEY key, MASK mask)
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
    case KEY_ESCAPE: press(HH_CLOSE); return true;
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f) press(f->mId);
        return true;
    }
    case KEY_UP: mScroll = llclamp(mScroll - 30.f, 0.f, mTextOverflow); return true;
    case KEY_DOWN: mScroll = llclamp(mScroll + 30.f, 0.f, mTextOverflow); return true;
    default: return false;
    }
}

WolfFloaterAdventureHandout::WolfFloaterAdventureHandout(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureHandout::postBuild()
{
    mView = getChild<WolfAdventureHandoutView>("view");
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterAdventureHandout::show(const std::string& adv, const std::string& id)
{
    if (!mView) return;
    mView->setHandout(adv, id);
    mView->setFocus(true);
}

void WolfFloaterAdventureHandout::draw()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}
