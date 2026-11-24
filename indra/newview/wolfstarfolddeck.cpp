/**
 * @file wolfstarfolddeck.cpp
 * @brief WolfViewer: Starfold's cockpit, Station window and guide. See wolfstarfolddeck.h.
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

#include "wolfstarfolddeck.h"

#include <cmath>
#include <map>

#include "llagent.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llquaternion.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llwindow.h"
#include "wolfgrid.h"
#include "wolfstarfold.h"

static LLDefaultChildRegistry::Register<WolfStarfoldDeck> r_wolf_starfold_deck("wolf_starfold_deck");

namespace
{
    // ── the look (design.html "The cockpit": amber and cyan instruments on deep blue; GAME TUNING) ───────────────────────
    const LLColor4 C_TEXT(0.93f, 0.95f, 1.f, 1.f);
    const LLColor4 C_DIM(0.66f, 0.72f, 0.85f, 1.f);
    const LLColor4 C_AMBER(1.f, 0.74f, 0.28f, 1.f);
    const LLColor4 C_CYAN(0.38f, 0.90f, 1.f, 1.f);
    const LLColor4 C_RED(1.f, 0.40f, 0.34f, 1.f);
    const LLColor4 C_GREEN(0.46f, 1.f, 0.62f, 1.f);
    const LLColor4 C_VIOLET(0.80f, 0.56f, 1.f, 1.f);
    const LLColor4 C_BUTTON(0.10f, 0.17f, 0.32f, 1.f);
    const LLColor4 C_BUTTON_LIT(0.30f, 0.22f, 0.06f, 1.f);
    const LLColor4 C_PRIMARY(0.12f, 0.36f, 0.48f, 1.f);
    const LLColor4 C_EDGE(0.38f, 0.52f, 0.78f, 1.f);

    // the backgrounds a pilot can choose (WolfStarfoldTheme 0..3; design: "a background the user can choose")
    struct Theme { LLColor4 hi, lo; const char* name; };
    const Theme THEMES[4] = {
        { LLColor4(0.06f, 0.12f, 0.26f, 0.96f), LLColor4(0.02f, 0.04f, 0.10f, 0.96f), "Deep blue" },
        { LLColor4(0.15f, 0.07f, 0.24f, 0.96f), LLColor4(0.05f, 0.02f, 0.09f, 0.96f), "Nebula" },
        { LLColor4(0.06f, 0.06f, 0.07f, 0.94f), LLColor4(0.01f, 0.01f, 0.02f, 0.94f), "Black glass" },
        { LLColor4(0.f, 0.f, 0.f, 0.f), LLColor4(0.f, 0.f, 0.f, 0.f), "No panel" },
    };
    S32 theme()
    {
        static LLCachedControl<S32> t(gSavedSettings, "WolfStarfoldTheme", 0);
        return llclamp((S32)t, 0, 3);
    }

    // The picture's own units: the band is 1600 x 250 (as the cab's 1600 x 304, a little lower for a game).
    const F32 PIC_W = 1600.f, PIC_H = 250.f;

    // ---- primitives, view-local, untextured (Source: wolftraincab.cpp's, after wolfhelmhud.cpp's) ----
    void noTex() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }
    void vtx(F32 x, F32 y, const LLColor4& c) { gGL.color4fv(c.mV); gGL.vertex2f(x, y); }
    void rectF(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(r, b, c); vtx(r, t, c);
        vtx(l, b, c); vtx(r, t, c); vtx(l, t, c);
        gGL.end();
    }
    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bot)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bot); vtx(r, b, bot); vtx(r, t, top);
        vtx(l, b, bot); vtx(r, t, top); vtx(l, t, top);
        gGL.end();
    }
    void quadF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, F32 x4, F32 y4, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
        vtx(x1, y1, c); vtx(x3, y3, c); vtx(x4, y4, c);
        gGL.end();
    }
    void lineW(F32 x1, F32 y1, F32 x2, F32 y2, F32 w, const LLColor4& c)
    {
        const F32 dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quadF(x1 + nx, y1 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, x2 + nx, y2 + ny, c);
    }
    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c); rectF(l, t - w, r, t, c); rectF(l, b, l + w, t, c); rectF(r - w, b, r, t, c);
    }
    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 40)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * F_PI * (F32)i / (F32)seg, a1 = 2.f * F_PI * (F32)(i + 1) / (F32)seg;
            vtx(cx, cy, c); vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c); vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }
    // an arc `deg` clockwise from 12 o'clock (y up), `w` thick (Source: wolftraincab.cpp pt / clockArc)
    void pt(F32 cx, F32 cy, F32 r, F32 deg, F32& x, F32& y)
    {
        const F32 a = deg * DEG_TO_RAD;
        x = cx + r * sinf(a);
        y = cy + r * cosf(a);
    }
    void clockArc(F32 cx, F32 cy, F32 r, F32 w, F32 d0, F32 d1, const LLColor4& c, S32 seg = 48)
    {
        for (S32 i = 0; i < seg; ++i)
        {
            F32 x0, y0, x1, y1, x2, y2, x3, y3;
            const F32 a = d0 + (d1 - d0) * (F32)i / (F32)seg, b = d0 + (d1 - d0) * (F32)(i + 1) / (F32)seg;
            pt(cx, cy, r - w * 0.5f, a, x0, y0); pt(cx, cy, r + w * 0.5f, a, x1, y1);
            pt(cx, cy, r + w * 0.5f, b, x2, y2); pt(cx, cy, r - w * 0.5f, b, x3, y3);
            quadF(x0, y0, x1, y1, x2, y2, x3, y3, c);
        }
    }
    void ring(F32 cx, F32 cy, F32 r, F32 w, const LLColor4& c) { clockArc(cx, cy, r, w, 0.f, 360.f, c, 64); }

    // Source: wolftraincab.cpp face() (itself wolfhelmhud.cpp) - the B612 face (fonts.xml Avionics0..11) at the largest size
    // whose line fits px.
    const LLFontGL* face(bool bold, F32 px)
    {
        static std::map<S32, std::vector<const LLFontGL*>> cache;
        const S32 key = bold ? 1 : 0;
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                if (const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("B612", llformat("Avionics%d", i), bold ? LLFontGL::BOLD : LLFontGL::NORMAL))) v.push_back(f);
            }
            if (v.empty()) v.push_back(LLFontGL::getFontSansSerif());
            it = cache.emplace(key, v).first;
        }
        const LLFontGL* best = it->second.front();
        for (const LLFontGL* f : it->second) if ((F32)f->getLineHeight() <= px) best = f;
        return best;
    }
    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        WolfHudScale::text(face(bold, llmax(7.f, px * 1.25f)), s, x, y, c, h, LLFontGL::VCENTER);
    }
    F32 textW(const std::string& s, F32 px, bool bold)
    {
        const LLFontGL* f = face(bold, llmax(7.f, px * 1.25f));
        return f ? f->getWidthF32(utf8str_to_wstring(s).c_str()) : 0.f;
    }
    // words cut to `maxw` with "..." (as wolftraincab.cpp fitW)
    std::string fit(const std::string& s, F32 px, bool bold, F32 maxw)
    {
        if (textW(s, px, bold) <= maxw) return s;
        LLWString w = utf8str_to_wstring(s);
        while (!w.empty() && textW(wstring_to_utf8str(w) + "...", px, bold) > maxw) w.pop_back();
        return wstring_to_utf8str(w) + "...";
    }

    // our own labels and names (plain words): upper case for the cockpit's lettering
    std::string upper(std::string s)
    {
        LLStringUtil::toUpper(s);
        return s;
    }

    std::string distance(F32 m)
    {
        if (m >= 1000.f) return llformat("%.1f km", m / 1000.f);
        return llformat("%d m", (S32)llround(m));
    }
    std::string thousands(S64 n)
    {
        std::string d = llformat("%lld", (long long)llabs(n));
        for (S32 i = (S32)d.size() - 3; i > 0; i -= 3) d.insert((size_t)i, ",");
        return n < 0 ? "-" + d : d;
    }

    // The guide (design: "Guide cards with pictures, Back and Next"; Paul 2026-11-11 paul-games-must-look-fun: cards with Back /
    // Next and dots). British English, one idea per line (ui-ux-design, cognitive load).
    struct Card { const char* title; const char* l1; const char* l2; const char* l3; };
    const Card CARDS[] = {
        { "Welcome aboard your Cub", "The Cub is a small ship that does a bit of everything. Take your time: nothing here is timed.",
          "The region flies the ship; your buttons and keys say what you want it to do.",
          "Every button has a key as well, but you never need the keys." },
        { "Flying", "THROTTLE: drag the bar, or press W and S (or Page Up and Page Down).",
          "Turn with the arrow keys, roll with Q and E - or hold the left mouse button on the sky and point where to go.",
          "Shift and the arrows slide the ship sideways, up and down." },
        { "Fly there", "Press STATION, BELT or WORMHOLE and the ship flies there by itself and stops safely.",
          "You can also click them on the round radar, then FLY THERE.", "Touch any turning key and you are flying it yourself again." },
        { "Mining and selling", "Near the asteroid belt press MINE (or M). A green beam fills your hold with ore.",
          "Then fly to the station and press DOCK. Your ship is repaired as you dock.",
          "In the Station window press SELL ALL ORE for Starmarks." },
        { "Glasswing raiders", "Violet Glasswings may come for you. FIRE (or hold Space) shoots your target; NEXT TARGET (Tab) picks another.",
          "Your shield takes hits first and comes back by itself. Armour and hull are repaired at the station.",
          "If your hull runs out, a station tug brings you home. Near the station is a safe harbour." },
        { "Steady, Drift and the wormhole", "STEADY flies where the nose points. DRIFT (Z) lets the ship slide on as you turn.",
          "Fly into the cyan ring of the wormhole to cross to another region's space.",
          "LEAVE SHIP stands you up. The guide is always on the ? button." },
    };
    const S32 CARD_COUNT = (S32)(sizeof(CARDS) / sizeof(CARDS[0]));

    F32 bottomToolbarTop(LLView* view)
    {
        // above the bottom toolbar, as the cab sits (Source: wolftraincab.cpp draw)
        F32 tb = 0.f;
        if (gToolBarView)
        {
            if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
            {
                if (bp->isInVisibleChain())
                {
                    const LLRect sr = bp->calcScreenRect();
                    S32 lx = 0, ly = 0;
                    view->screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                    tb = llclamp((F32)ly, 0.f, (F32)view->getRect().getHeight() * 0.4f);
                }
            }
        }
        return tb;
    }
}

WolfStarfoldDeck::WolfStarfoldDeck(const Params& p) : LLUICtrl(p)
{
}

WolfStarfoldDeck::~WolfStarfoldDeck()
{
}

void WolfStarfoldDeck::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ l, b, r, t, id, tip });
}

const WolfStarfoldDeck::Hit* WolfStarfoldDeck::hitAt(S32 x, S32 y) const
{
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if ((F32)x >= it->mL && (F32)x <= it->mR && (F32)y >= it->mB && (F32)y <= it->mT) return &*it;
    }
    return nullptr;
}

void WolfStarfoldDeck::button(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& label, bool lit, const std::string& tip, F32 u,
                              bool primary)
{
    const bool down = mPressed == id;
    const LLColor4 fill = down ? C_EDGE : (lit ? C_BUTTON_LIT : (primary ? C_PRIMARY : C_BUTTON));
    rectG(l, b, r, t, fill * 1.25f, fill);
    frameW(l, b, r, t, llmax(1.f, 2.f * u), lit ? C_AMBER : C_EDGE);
    const F32 px = llmin((t - b) * 0.36f, 22.f * u);
    T((l + r) * 0.5f, (b + t) * 0.5f, fit(label, px, true, (r - l) - 10.f * u), px, lit ? C_AMBER : C_TEXT, true);
    addHit(l, b, r, t, id, tip);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// draw
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfStarfoldDeck::draw()
{
    WolfStarfold& sf = WolfStarfold::instance();
    mHits.clear();
    mBandDrawn = false;
    const bool flying = sf.piloting();
    const bool band = sf.panelShown() && !(sf.stationShown() && !flying);
    const bool station = sf.stationShown() && !flying;
    const bool guide = sf.guideShown() && WolfGrid::isOnWolfTerritoriesConfirmed() && sf.spaceHere();
    const bool tab = flying && sf.panelHidden();
    const bool setup = sf.setupShown();
    if (!setup) mClearArmed = false;
    if (!band && !station && !guide && !tab && !setup && sf.notes().empty())
    {
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        mPressed = H_NONE;
        mResize.hide();
        if (flying) sf.setPointer(false, 0.f, 0.f);
        return;
    }
    LLGLSUIDefault gls_ui;
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const F32 bottom = bottomToolbarTop(this);
    // the size: the window's width at the pilot's chosen scale, at most a third of the height (as the cab and the deck)
    F32 u = W / PIC_W * mResize.scale();
    u = llmin(u, (H * 0.34f) / PIC_H);
    u = llmax(u, 0.3f);
    F32 top_of_world = bottom;
    if (band && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (flying) drawCockpit(bottom, W, u);
        else drawGround(bottom, W, u);
        top_of_world = mPanelT;
    }
    else
    {
        mResize.hide();
    }
    if (tab)
    {
        // hidden by the pilot: a tab at the bottom centre brings the cockpit back (as the cab's)
        const F32 w = 260.f * u, h = 44.f * llmax(u, 1.f);
        button(W * 0.5f - w * 0.5f, bottom + 4.f, W * 0.5f + w * 0.5f, bottom + 4.f + h, H_SHOW_TAB,
               std::string(WolfStarfold::gameName()) + ": SHOW COCKPIT", false, "Bring the cockpit back", u);
        top_of_world = bottom + 8.f + h;
    }
    // the view above the band: point and fly takes its centre and half-size
    mWorldCx = W * 0.5f;
    mWorldCy = (top_of_world + H) * 0.5f;
    mWorldHalfW = llmax(1.f, W * 0.5f);
    mWorldHalfH = llmax(1.f, (H - top_of_world) * 0.5f);
    drawNotes(top_of_world + 10.f * u, W, llmax(u, 0.8f));
    if (station) drawStation(top_of_world, W, H, llmax(u, 0.8f));
    else if (setup) drawSetup(top_of_world, W, H, llmax(u, 0.8f));
    if (guide) drawGuide(top_of_world, W, H, llmax(u, 0.8f));
    if (mBandDrawn) mResize.draw();
}

void WolfStarfoldDeck::drawNotes(F32 above, F32 width, F32 u)
{
    // the region's words (k=msg), newest at the bottom, above the panel; they go after a while (WolfStarfold idle) but never
    // ask anything of the pilot in that time (design: "Nothing on the screen is timed against the player")
    const auto& notes = WolfStarfold::instance().notes();
    F32 y = above;
    const F32 px = 20.f * u, line = px * 1.7f;
    for (auto it = notes.rbegin(); it != notes.rend(); ++it)
    {
        const std::string s = fit(it->mText, px, true, width * 0.8f);
        const F32 w = textW(s, px, true) + 28.f * u;
        rectF(width * 0.5f - w * 0.5f, y, width * 0.5f + w * 0.5f, y + line, LLColor4(0.02f, 0.04f, 0.10f, 0.82f));
        T(width * 0.5f, y + line * 0.5f, s, px, C_TEXT, true);
        y += line + 4.f * u;
    }
}

void WolfStarfoldDeck::drawCockpit(F32 bottom, F32 W, F32 u)
{
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Ship& s = sf.ship();
    const F32 bandH = PIC_H * u;
    const F32 ox = (W - PIC_W * u) * 0.5f;   // the picture centred when the window is wider than it scales to
    auto X = [&](F32 x) { return ox + x * u; };
    auto Y = [&](F32 y) { return bottom + y * u; };
    mPanelB = bottom;
    mPanelT = bottom + bandH;
    mBandDrawn = true;
    mResize.place(mPanelB, mPanelT, W);
    const Theme& th = THEMES[theme()];
    if (th.hi.mV[VALPHA] > 0.f)
    {
        rectG(0.f, mPanelB, W, mPanelT, th.hi, th.lo);
        rectF(0.f, mPanelT - 2.f * u, W, mPanelT, C_EDGE * 0.8f);
    }

    // ── LEFT: the ship - shield, armour and hull as three arcs, with words and numbers ─────────────────────────────────
    {
        const F32 cx = X(130.f), cy = Y(120.f);
        struct Bar { const char* name; F32 v, full;   // not "max": <windows.h> defines max LLColor4 c; F32 r; };
        const Bar bars[3] = { { "SHIELD", s.mShield, s.mShieldMax, C_CYAN, 92.f }, { "ARMOUR", s.mArmour, s.mArmourMax, C_AMBER, 72.f },
                              { "HULL", s.mHull, s.mHullMax, C_RED, 52.f } };
        for (const Bar& b : bars)
        {
            const F32 f = llclamp(b.v / llmax(b.full, 1.f), 0.f, 1.f);
            clockArc(cx, cy, b.r * u, 13.f * u, -135.f, 135.f, LLColor4(b.c.mV[0] * 0.22f, b.c.mV[1] * 0.22f, b.c.mV[2] * 0.22f, 1.f));
            if (f > 0.f) clockArc(cx, cy, b.r * u, 13.f * u, -135.f, -135.f + 270.f * f, b.c);
        }
        T(cx, cy + 4.f * u, s.mClass.empty() ? std::string("CUB") : upper(s.mClass), 22.f * u, C_TEXT, true);
        // the figures, words and numbers (never colour alone: WCAG 1.4.1)
        const F32 lx = X(250.f);
        T(lx, Y(196.f), llformat("SHIELD %d", (S32)llround(s.mShield)), 21.f * u, C_CYAN, true, LLFontGL::LEFT);
        T(lx, Y(162.f), llformat("ARMOUR %d", (S32)llround(s.mArmour)), 21.f * u, C_AMBER, true, LLFontGL::LEFT);
        T(lx, Y(128.f), llformat("HULL %d", (S32)llround(s.mHull)), 21.f * u, C_RED, true, LLFontGL::LEFT);
        T(lx, Y(88.f), llformat("HOLD %d / %d", (S32)floorf(s.mHold), (S32)llround(s.mHoldMax)), 19.f * u,
          s.mHold >= s.mHoldMax - 0.5f && s.mHoldMax > 0.f ? C_AMBER : C_TEXT, true, LLFontGL::LEFT);
        T(lx, Y(58.f), thousands(s.mCredits) + " " + WolfStarfold::currency(), 17.f * u, C_GREEN, false, LLFontGL::LEFT);
        if (s.mSafe) T(lx, Y(28.f), "SAFE HARBOUR", 16.f * u, C_GREEN, true, LLFontGL::LEFT);
        else if (s.mAir) T(lx, Y(28.f), "IN THE AIR - CLIMBING", 16.f * u, C_AMBER, true, LLFontGL::LEFT);
    }

    // ── CENTRE: speed, the throttle, the mode, the four big buttons and Fly there ──────────────────────────────────────
    {
        // the throttle bar: drag it, or W / S
        const F32 tl = X(470.f), tr = X(520.f), tb = Y(26.f), tt = Y(214.f);
        mThrB = tb; mThrT = tt;
        rectF(tl, tb, tr, tt, LLColor4(0.03f, 0.06f, 0.14f, 1.f));
        frameW(tl, tb, tr, tt, 2.f * u, C_EDGE);
        const F32 thr = sf.throttle();
        const F32 zero = sf.drift() ? (tb + tt) * 0.5f : tb;   // Drift has reverse: the bar's middle is stop
        const F32 at = sf.drift() ? zero + (tt - tb) * 0.5f * thr : tb + (tt - tb) * thr;
        rectF(tl + 4.f * u, llmin(zero, at), tr - 4.f * u, llmax(zero, at), thr < 0.f ? C_RED : C_AMBER);
        rectF(tl - 6.f * u, at - 4.f * u, tr + 6.f * u, at + 4.f * u, C_TEXT);
        addHit(tl - 14.f * u, tb, tr + 14.f * u, tt, H_THROTTLE, "Throttle: drag up for faster (W and S, or Page Up and Page Down)");
        T((tl + tr) * 0.5f, Y(236.f), "THROTTLE", 13.f * u, C_DIM, true);

        // speed and the throttle in words
        T(X(640.f), Y(196.f), "SPEED", 15.f * u, C_DIM, true);
        T(X(640.f), Y(158.f), llformat("%d m/s", (S32)llround(s.mSpeed)), 36.f * u, C_TEXT, true);
        T(X(640.f), Y(124.f), llformat("THROTTLE %d%%", (S32)llround(thr * 100.f)), 15.f * u, C_AMBER, false);
        // the mode switch (design: "a big switch on the cockpit")
        button(X(560.f), Y(28.f), X(720.f), Y(96.f), H_MODE, sf.drift() ? "DRIFT" : "STEADY", sf.drift(),
               "Steady flies where the nose points; Drift keeps sliding as you turn (Z)", u);

        // the four big buttons (design: FIRE, MINE, NEXT TARGET, DOCK)
        const F32 bl = 750.f, bw = 150.f, gap = 12.f;
        button(X(bl), Y(140.f), X(bl + bw), Y(214.f), H_FIRE, "FIRE", sf.firing(), "Fire at your target (hold Space)", u);
        button(X(bl + bw + gap), Y(140.f), X(bl + 2 * bw + gap), Y(214.f), H_MINE, "MINE", sf.mining(),
               "Mine the nearest rock in reach (M)", u);
        button(X(bl), Y(60.f), X(bl + bw), Y(130.f), H_NEXT, "NEXT TARGET", false, "Pick the next Glasswing (Tab)", u);
        button(X(bl + bw + gap), Y(60.f), X(bl + 2 * bw + gap), Y(130.f), H_DOCK, "DOCK", false, "Dock at the station (within 150 m)", u);
        // Fly there: one row of the places this region has
        const WolfStarfold::System& sys = sf.system();
        F32 fx = bl;
        const F32 fw = 74.f;
        auto fly = [&](bool have, EHit id, const char* label, const char* where, const char* tip)
        {
            if (!have) return;
            button(X(fx), Y(12.f), X(fx + fw), Y(50.f), id, label, sf.flyingTo() == where, tip, u);
            fx += fw + 4.f;
        };
        fly(sys.mHaveStation, H_FLY_STATION, "STATION", "station", "Fly to the station and stop there");
        fly(sys.mHaveBelt, H_FLY_BELT, "BELT", "belt", "Fly to the asteroid belt and stop at its edge");
        fly(sys.mHaveWormhole, H_FLY_WORMHOLE, "WORMHOLE", "wormhole", "Fly into the wormhole");
        if (!sf.flyingTo().empty()) button(X(fx), Y(12.f), X(fx + fw), Y(50.f), H_FLY_STOP, "STOP", false, "Stop flying there", u);

        // the target, in words (design: "TARGET Glasswing raider · 640 m / its shield 40 · armour 100")
        const F32 tx = X(1080.f);
        rectF(tx - 8.f * u, Y(126.f), X(1248.f), Y(226.f), LLColor4(0.02f, 0.03f, 0.08f, 0.75f));
        if (s.mTarget.notNull())
        {
            T(tx, Y(206.f), "TARGET", 14.f * u, C_VIOLET, true, LLFontGL::LEFT);
            T(tx, Y(182.f), "Glasswing " + distance(s.mTargetM), 17.f * u, C_TEXT, true, LLFontGL::LEFT);
            T(tx, Y(158.f), llformat("shield %d  armour %d", (S32)llround(s.mTargetShield), (S32)llround(s.mTargetArmour)), 14.f * u, C_DIM,
              false, LLFontGL::LEFT);
            T(tx, Y(138.f), llformat("hull %d", (S32)llround(s.mTargetHull)), 14.f * u, C_DIM, false, LLFontGL::LEFT);
        }
        else
        {
            T(tx, Y(206.f), "NO TARGET", 14.f * u, C_DIM, true, LLFontGL::LEFT);
            T(tx, Y(178.f), s.mAliens.empty() ? "No Glasswings near" : llformat("%d Glasswing%s near", (S32)s.mAliens.size(),
              s.mAliens.size() == 1 ? "" : "s"), 15.f * u, s.mAliens.empty() ? C_DIM : C_VIOLET, false, LLFontGL::LEFT);
        }
        // the small buttons: guide, panel background, hide, leave
        const F32 sx = 1080.f, sw = 80.f;
        button(X(sx), Y(66.f), X(sx + sw), Y(112.f), H_HELP, "?", sf.guideShown(), "The guide: how to fly, mine, sell and fight", u);
        button(X(sx + sw + 8.f), Y(66.f), X(sx + 2 * sw + 8.f), Y(112.f), H_THEME, "PANEL", false,
               std::string("Change the panel's background (now: ") + THEMES[theme()].name + ")", u);
        button(X(sx), Y(12.f), X(sx + sw), Y(58.f), H_HIDE, "HIDE", false, "Hide the cockpit (a tab brings it back)", u);
        button(X(sx + sw + 8.f), Y(12.f), X(sx + 2 * sw + 8.f), Y(58.f), H_LEAVE, "LEAVE SHIP", false, "Stand up and leave the ship", u);
    }

    // ── RIGHT: the radar ───────────────────────────────────────────────────────────────────────────────────────────────
    drawRadar(X(1440.f), Y(130.f), 104.f * u, u);
    if (!mPicked.empty())
    {
        button(X(1290.f), Y(4.f), X(1590.f), Y(24.f) + 16.f * u, H_FLY_PICKED, "FLY THERE: " + upper(mPicked), false,
               "Fly to the place you picked on the radar", u, true);
    }
}

void WolfStarfoldDeck::drawRadar(F32 cx, F32 cy, F32 r, F32 u)
{
    // A flat round radar, nose up (design: "contacts above or below carry an up or down arrow and the word"). Its range: 1.5 km.
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Ship& s = sf.ship();
    const WolfStarfold::System& sys = sf.system();
    const F32 RANGE = 1500.f;
    circleF(cx, cy, r, LLColor4(0.01f, 0.05f, 0.08f, 0.95f), 56);
    ring(cx, cy, r, 2.f * u, C_CYAN * 0.8f);
    ring(cx, cy, r * 0.5f, 1.f * u, C_CYAN * 0.35f);
    lineW(cx - r, cy, cx + r, cy, 1.f * u, C_CYAN * 0.25f);
    lineW(cx, cy - r, cx, cy + r, 1.f * u, C_CYAN * 0.25f);
    T(cx + r - 6.f * u, cy + r - 4.f * u, "1.5 km", 11.f * u, C_DIM, false, LLFontGL::RIGHT);
    // our ship
    quadF(cx, cy + 9.f * u, cx + 6.f * u, cy - 6.f * u, cx, cy - 2.f * u, cx - 6.f * u, cy - 6.f * u, C_AMBER);
    LLViewerObject* ship = gObjectList.findObject(s.mRoot);
    LLViewerRegion* region = gAgent.getRegion();
    if (!ship || !region) return;
    LLViewerObject* root = ship->getRootEdit() ? ship->getRootEdit() : ship;
    const LLVector3 me = root->getPositionAgent();
    const LLQuaternion inv = ~root->getRotationRegion();
    auto place = [&](const LLVector3& agent, F32& x, F32& y, F32& dz) -> bool
    {
        const LLVector3 rel = (agent - me) * inv;   // the ship's frame: +X ahead, +Y left (SpaceMath: the nose is +X)
        dz = agent.mV[VZ] - me.mV[VZ];
        F32 fx = rel.mV[VX], fy = rel.mV[VY];
        const F32 d = sqrtf(fx * fx + fy * fy);
        const bool inside = d <= RANGE;
        if (!inside) { fx *= RANGE / d; fy *= RANGE / d; }
        x = cx - fy / RANGE * r * 0.94f;   // left of the nose is left on the scope
        y = cy + fx / RANGE * r * 0.94f;
        return inside;
    };
    auto updown = [&](F32 x, F32 y, F32 dz)
    {
        if (fabsf(dz) < 50.f) return;
        const bool up = dz > 0.f;
        const F32 a = 6.f * u, yy = y + (up ? 14.f : -14.f) * u;
        quadF(x - a, yy - (up ? a : -a) * 0.5f, x + a, yy - (up ? a : -a) * 0.5f, x, yy + (up ? a : -a), x, yy + (up ? a : -a), C_TEXT);
        T(x + 10.f * u, yy, up ? "above" : "below", 10.f * u, C_TEXT, false, LLFontGL::LEFT);
    };
    const F32 hit = 22.f * u;   // 44 px across at normal size (design: "Every target is at least 44 px")
    F32 x, y, dz;
    if (sys.mHaveBelt)
    {
        place(region->getPosAgentFromRegion(sys.mBeltCentre), x, y, dz);
        const F32 br = llmin(r * 0.95f, sys.mBeltR / RANGE * r);
        ring(x, y, llmax(br, 6.f * u), 4.f * u, LLColor4(0.62f, 0.50f, 0.38f, mPicked == "belt" ? 1.f : 0.6f));
        T(x, y, "BELT", 11.f * u, C_DIM, true);
        addHit(x - hit, y - hit, x + hit, y + hit, H_RADAR_BELT, "The asteroid belt: click, then FLY THERE");
    }
    if (sys.mHaveStation)
    {
        place(region->getPosAgentFromRegion(sys.mStation), x, y, dz);
        rectF(x - 7.f * u, y - 7.f * u, x + 7.f * u, y + 7.f * u, mPicked == "station" ? C_AMBER : C_TEXT);
        T(x, y - 16.f * u, "STATION", 10.f * u, C_TEXT, true);
        updown(x, y, dz);
        addHit(x - hit, y - hit, x + hit, y + hit, H_RADAR_STATION, "The station: click, then FLY THERE");
    }
    if (sys.mHaveWormhole)
    {
        place(region->getPosAgentFromRegion(sys.mWormhole), x, y, dz);
        ring(x, y, 8.f * u, 3.f * u, mPicked == "wormhole" ? C_AMBER : C_CYAN);
        T(x, y - 18.f * u, "WORMHOLE", 10.f * u, C_CYAN, true);
        updown(x, y, dz);
        addHit(x - hit, y - hit, x + hit, y + hit, H_RADAR_WORMHOLE, "The wormhole to " + sys.mWormholeTo + ": click, then FLY THERE");
    }
    for (size_t i = 0; i < s.mAliens.size() && i < 6; ++i)
    {
        LLViewerObject* a = gObjectList.findObject(s.mAliens[i]);
        if (!a) continue;
        place(a->getPositionAgent(), x, y, dz);
        const bool tg = s.mAliens[i] == s.mTarget || s.mAliens[i] == sf.selected();
        quadF(x, y + 8.f * u, x + 7.f * u, y - 6.f * u, x, y - 3.f * u, x - 7.f * u, y - 6.f * u, C_VIOLET);
        if (tg) frameW(x - 11.f * u, y - 11.f * u, x + 11.f * u, y + 11.f * u, 2.f * u, C_VIOLET);
        updown(x, y, dz);
        addHit(x - hit, y - hit, x + hit, y + hit, (EHit)(H_ALIEN_0 + (S32)i), "A Glasswing raider: click to make it your target");
    }
}

void WolfStarfoldDeck::drawGround(F32 bottom, F32 W, F32 u)
{
    // Not flying: a lower band that says where space starts here and offers LAUNCH (design: "Launch" from a pad or the station)
    WolfStarfold& sf = WolfStarfold::instance();
    const F32 h = 120.f * u;
    mPanelB = bottom;
    mPanelT = bottom + h;
    mBandDrawn = true;
    mResize.place(mPanelB, mPanelT, W);
    const Theme& th = THEMES[theme() == 3 ? 0 : theme()];
    rectG(0.f, mPanelB, W, mPanelT, th.hi, th.lo);
    rectF(0.f, mPanelT - 2.f * u, W, mPanelT, C_EDGE * 0.8f);
    const F32 ox = (W - PIC_W * u) * 0.5f;
    auto X = [&](F32 x) { return ox + x * u; };
    auto Y = [&](F32 y) { return bottom + y * u; };
    T(X(40.f), Y(84.f), upper(WolfStarfold::gameName()), 30.f * u, C_AMBER, true, LLFontGL::LEFT);
    T(X(40.f), Y(50.f), llformat("Space starts %s above the sea here.", distance(sf.system().mHeight).c_str()), 18.f * u, C_TEXT,
      false, LLFontGL::LEFT);
    T(X(40.f), Y(22.f), "Go to a launch pad or the station, then press LAUNCH.", 16.f * u, C_DIM, false, LLFontGL::LEFT);
    button(X(1080.f), Y(16.f), X(1300.f), Y(100.f), H_LAUNCH, "LAUNCH", false, "Bring out your Cub and climb to space", u, true);
    button(X(1320.f), Y(16.f), X(1430.f), Y(100.f), H_HELP, "GUIDE", sf.guideShown(), "How to fly, mine, sell and fight", u);
    button(X(1450.f), Y(16.f), X(1560.f), Y(100.f), H_CLOSE, "CLOSE", false, "Close this panel (Wolf World > Starfold opens it)", u);
}

void WolfStarfoldDeck::drawStation(F32 above, F32 W, F32 H, F32 u)
{
    // The Station window (design "Stations": Market, Repairs, Launch in stage 1), centred over the world, drawn as the cockpit is.
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Station& st = sf.station();
    const F32 w = llmin(W - 32.f, 720.f * u), h = llmin(H - above - 32.f, 460.f * u);
    const F32 l = W * 0.5f - w * 0.5f, r = l + w;
    const F32 b = above + (H - above - h) * 0.5f, t = b + h;
    rectG(l, b, r, t, THEMES[0].hi, THEMES[0].lo);
    frameW(l, b, r, t, 3.f * u, C_EDGE);
    addHit(l, b, r, t, H_NONE, "");   // the window takes its clicks: none fall through to the world
    const F32 pad = 28.f * u;
    T(l + pad, t - 36.f * u, "STATION", 30.f * u, C_AMBER, true, LLFontGL::LEFT);
    T(l + pad, t - 72.f * u, "Docked. Your ship is repaired: shield, armour and hull are full.", 17.f * u, C_GREEN, false, LLFontGL::LEFT);
    // the market: ore, in store, price, value (numbers right-aligned so they stack: ui-ux-design typography)
    const F32 c1 = l + pad, c2 = l + w * 0.55f, c3 = l + w * 0.72f, c4 = r - pad;
    F32 y = t - 120.f * u;
    T(c1, y, "MARKET", 16.f * u, C_DIM, true, LLFontGL::LEFT);
    T(c2, y, "IN STORE", 14.f * u, C_DIM, true, LLFontGL::RIGHT);
    T(c3, y, "PRICE", 14.f * u, C_DIM, true, LLFontGL::RIGHT);
    T(c4, y, "VALUE", 14.f * u, C_DIM, true, LLFontGL::RIGHT);
    S64 total = 0;
    for (const WolfStarfold::Ore& o : st.mOres)
    {
        y -= 36.f * u;
        const S64 held = (S64)floorf(o.mHeld);   // SpaceEngine.Sell sells whole units (Math.Floor)
        total += held * o.mPrice;
        T(c1, y, o.mTitle, 19.f * u, C_TEXT, false, LLFontGL::LEFT);
        T(c2, y, thousands(held), 19.f * u, C_TEXT, false, LLFontGL::RIGHT);
        T(c3, y, llformat("%d", o.mPrice), 19.f * u, C_TEXT, false, LLFontGL::RIGHT);
        T(c4, y, thousands(held * o.mPrice), 19.f * u, C_TEXT, false, LLFontGL::RIGHT);
    }
    if (st.mOres.empty())
    {
        y -= 36.f * u;
        T(c1, y, "No ore in store. Mine at the belt, then come back to sell.", 17.f * u, C_DIM, false, LLFontGL::LEFT);
    }
    y -= 48.f * u;
    T(c1, y, std::string("Your ") + WolfStarfold::currency() + ": " + thousands(st.mCredits), 20.f * u, C_GREEN, true, LLFontGL::LEFT);
    // the actions: SELL (only with something to sell), CLOSE apart from LAUNCH (the primary, bottom right)
    const F32 bh = 60.f * u, bb = b + pad;
    button(l + pad, bb, l + pad + 260.f * u, bb + bh, H_SELL, total > 0 ? "SELL ALL ORE (" + thousands(total) + ")" : "SELL ALL ORE",
           false, total > 0 ? "Sell everything in store at these prices" : "You have no ore in store", u);
    button(r - pad - 200.f * u, bb, r - pad, bb + bh, H_STATION_LAUNCH, "LAUNCH", false, "Leave the station in your Cub", u, true);
    button(r - pad - 200.f * u - 160.f * u, bb, r - pad - 220.f * u, bb + bh, H_STATION_CLOSE, "CLOSE", false,
           "Close this window (you stay docked)", u);
}

void WolfStarfoldDeck::drawGuide(F32 above, F32 W, F32 H, F32 u)
{
    WolfStarfold& sf = WolfStarfold::instance();
    const S32 c = llclamp(sf.guideCard(), 0, CARD_COUNT - 1);
    const Card& card = CARDS[c];
    const F32 w = llmin(W - 32.f, 820.f * u), h = llmin(H - above - 32.f, 330.f * u);
    const F32 l = W * 0.5f - w * 0.5f, r = l + w;
    const F32 b = above + (H - above - h) * 0.5f, t = b + h;
    rectG(l, b, r, t, LLColor4(0.07f, 0.13f, 0.28f, 0.98f), LLColor4(0.02f, 0.04f, 0.11f, 0.98f));
    frameW(l, b, r, t, 3.f * u, C_AMBER);
    addHit(l, b, r, t, H_NONE, "");
    const F32 pad = 28.f * u;
    T(l + pad, t - 40.f * u, card.title, 28.f * u, C_AMBER, true, LLFontGL::LEFT);
    // the lines, wrapped at whole words to the card
    F32 y = t - 92.f * u;
    for (const char* line : { card.l1, card.l2, card.l3 })
    {
        std::string rest = line, cur;
        const F32 px = 18.f * u, maxw = w - 2.f * pad;
        while (!rest.empty())
        {
            const size_t sp = rest.find(' ');
            const std::string word = rest.substr(0, sp);
            rest = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            const std::string trial = cur.empty() ? word : cur + " " + word;
            if (textW(trial, px, false) > maxw && !cur.empty())
            {
                T(l + pad, y, cur, px, C_TEXT, false, LLFontGL::LEFT);
                y -= px * 1.55f;
                cur = word;
            }
            else cur = trial;
        }
        if (!cur.empty()) { T(l + pad, y, cur, px, C_TEXT, false, LLFontGL::LEFT); y -= px * 1.55f; }
        y -= 8.f * u;
    }
    // dots, Back, Next / Done, Close (Back on the left, the forward action bottom right)
    for (S32 i = 0; i < CARD_COUNT; ++i)
        circleF(W * 0.5f + ((F32)i - (F32)(CARD_COUNT - 1) * 0.5f) * 22.f * u, b + 40.f * u, (i == c ? 6.f : 4.f) * u, i == c ? C_AMBER : C_DIM, 20);
    const F32 bh = 52.f * u;
    if (c > 0) button(l + pad, b + 14.f * u, l + pad + 140.f * u, b + 14.f * u + bh, H_GUIDE_BACK, "BACK", false, "The card before", u);
    button(r - pad - 160.f * u, b + 14.f * u, r - pad, b + 14.f * u + bh, c + 1 < CARD_COUNT ? H_GUIDE_NEXT : H_GUIDE_CLOSE,
           c + 1 < CARD_COUNT ? "NEXT" : "DONE", false, c + 1 < CARD_COUNT ? "The next card" : "Close the guide", u, true);
    button(r - pad - 46.f * u, t - pad - 34.f * u, r - pad + 4.f * u, t - pad + 10.f * u, H_GUIDE_CLOSE, "X", false, "Close the guide", u);
}

void WolfStarfoldDeck::drawSetup(F32 above, F32 W, F32 H, F32 u)
{
    // The setup helper (design.html "The region owner's switch": "Let's set up your space! Fly up to where you want your station
    // ... press Rez station. Next, the asteroid belt ... step by step until it says Your space is ready"; Paul's rule
    // paul-games-need-setup-helper: "stand there, press Rez"). The region does the rezzing (SpaceSetup.cs); this draws the steps.
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Setup& su = sf.setup();
    const F32 w = llmin(W - 32.f, 760.f * u), h = llmin(H - above - 32.f, 470.f * u);
    const F32 l = W - w - 24.f * u, r = l + w;   // to the right, so the owner still sees where they stand
    const F32 b = above + (H - above - h) * 0.5f, t = b + h;
    rectG(l, b, r, t, THEMES[0].hi, THEMES[0].lo);
    frameW(l, b, r, t, 3.f * u, C_EDGE);
    addHit(l, b, r, t, H_NONE, "");
    const F32 pad = 26.f * u;
    T(l + pad, t - 34.f * u, std::string("SET UP ") + upper(WolfStarfold::gameName()) + " HERE", 26.f * u, C_AMBER, true, LLFontGL::LEFT);
    const F32 line = su.mHave ? su.mHeight : sf.system().mHeight;
    T(l + pad, t - 68.f * u, llformat("Space starts at %d m. Fly up past it, stand where you want each part, press its button.",
      (S32)llround(line)), 15.f * u, C_TEXT, false, LLFontGL::LEFT);
    T(l + pad, t - 92.f * u, su.mOn ? std::string("Space is ON here.") : std::string("Space is OFF here: switch it on from the Space page or say \"space on\" on the channel."),
      15.f * u, su.mOn ? C_GREEN : C_AMBER, false, LLFontGL::LEFT);
    struct Step { const char* id; const char* title; const char* how; EHit hit; };
    const Step steps[4] = {
        { "station", "1. Station", "In space, where pilots dock and sell.", H_SETUP_STATION },
        { "belt", "2. Asteroid belt", "In space: a ring of rocks 300 m round the spot.", H_SETUP_BELT },
        { "wormhole", "3. Wormhole", "In space: joins the open network (Space page).", H_SETUP_WORMHOLE },
        { "pad", "4. Launch pad", "Anywhere: pilots stand on it and press Launch.", H_SETUP_PAD },
    };
    F32 y = t - 140.f * u;
    for (const Step& st : steps)
    {
        const bool done = su.has(st.id);
        const bool next = su.mNext == st.id;
        // done: a tick and the word (never colour alone)
        T(l + pad, y, done ? "DONE" : (next ? "NEXT" : ""), 14.f * u, done ? C_GREEN : C_AMBER, true, LLFontGL::LEFT);
        T(l + pad + 70.f * u, y + 10.f * u, st.title, 19.f * u, C_TEXT, true, LLFontGL::LEFT);
        T(l + pad + 70.f * u, y - 14.f * u, st.how, 14.f * u, C_DIM, false, LLFontGL::LEFT);
        if (!done)
            button(r - pad - 190.f * u, y - 24.f * u, r - pad, y + 24.f * u, st.hit, std::string("REZ ") + upper(st.title + 3), next,
                   std::string("Place the ") + (st.title + 3) + " where you are now", u, next);
        y -= 66.f * u;
    }
    if (su.mNext == "done") T(l + pad, y + 18.f * u, "Your space is ready.", 18.f * u, C_GREEN, true, LLFontGL::LEFT);
    const F32 bh = 52.f * u, bb = b + 16.f * u;
    button(l + pad, bb, l + pad + 300.f * u, bb + bh, H_SETUP_CLEAR, mClearArmed ? "PRESS AGAIN TO REMOVE" : "REMOVE HELPER'S PARTS", mClearArmed,
           "Removes only what this helper placed, never your own markers", u);
    button(r - pad - 150.f * u, bb, r - pad, bb + bh, H_SETUP_CLOSE, "CLOSE", false, "Close the helper", u, true);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// the mouse
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfStarfoldDeck::press(EHit id, S32 x, S32 y)
{
    WolfStarfold& sf = WolfStarfold::instance();
    if (id != H_SETUP_CLEAR) mClearArmed = false;   // any other press disarms the remove
    switch (id)
    {
        case H_FIRE: sf.fire(!sf.firing()); break;
        case H_MINE: sf.mine(!sf.mining()); break;
        case H_NEXT: sf.nextTarget(); break;
        case H_DOCK: sf.dock(); break;
        case H_MODE: sf.setDrift(!sf.drift()); break;
        case H_THROTTLE: dragThrottle(y); break;
        case H_FLY_STATION: sf.flyTo("station"); break;
        case H_FLY_BELT: sf.flyTo("belt"); break;
        case H_FLY_WORMHOLE: sf.flyTo("wormhole"); break;
        case H_FLY_STOP: sf.flyTo("stop"); break;
        case H_FLY_PICKED: if (!mPicked.empty()) sf.flyTo(mPicked); mPicked.clear(); break;
        case H_RADAR_STATION: mPicked = "station"; break;
        case H_RADAR_BELT: mPicked = "belt"; break;
        case H_RADAR_WORMHOLE: mPicked = "wormhole"; break;
        case H_HELP: sf.showGuide(!sf.guideShown()); break;
        case H_THEME: gSavedSettings.setS32("WolfStarfoldTheme", (theme() + 1) % 4); break;
        case H_HIDE: sf.togglePanel(); break;
        case H_SHOW_TAB: sf.togglePanel(); break;
        case H_LEAVE: sf.leave(); break;
        case H_LAUNCH: sf.launch(); break;
        case H_CLOSE: sf.togglePanel(); break;
        case H_SELL: sf.sell(); break;
        case H_STATION_LAUNCH: sf.launch(); break;
        case H_STATION_CLOSE: sf.closeStation(); break;
        case H_GUIDE_BACK: sf.setGuideCard(llmax(0, sf.guideCard() - 1)); break;
        case H_GUIDE_NEXT: sf.setGuideCard(llmin(CARD_COUNT - 1, sf.guideCard() + 1)); break;
        case H_GUIDE_CLOSE: sf.showGuide(false); gSavedSettings.setBOOL("WolfStarfoldGuideSeen", true); break;
        case H_WORLD: steer(x, y); break;
        case H_SETUP_STATION: sf.setupRez("station"); break;
        case H_SETUP_BELT: sf.setupRez("belt"); break;
        case H_SETUP_WORMHOLE: sf.setupRez("wormhole"); break;
        case H_SETUP_PAD: sf.setupRez("pad"); break;
        case H_SETUP_CLEAR:
            if (mClearArmed) { sf.setupClear(); mClearArmed = false; }
            else mClearArmed = true;
            break;
        case H_SETUP_CLOSE: sf.closeSetup(); break;
        default:
            if (id >= H_ALIEN_0 && id <= H_ALIEN_LAST)
            {
                const size_t i = (size_t)(id - H_ALIEN_0);
                if (i < sf.ship().mAliens.size()) sf.selectTarget(sf.ship().mAliens[i]);
            }
            break;
    }
}

void WolfStarfoldDeck::dragThrottle(S32 y)
{
    WolfStarfold& sf = WolfStarfold::instance();
    const F32 f = llclamp(((F32)y - mThrB) / llmax(mThrT - mThrB, 1.f), 0.f, 1.f);
    sf.setThrottle(sf.drift() ? f * 2.f - 1.f : f);
}

void WolfStarfoldDeck::steer(S32 x, S32 y)
{
    // point and fly (design: "the ship turns gently towards the pointer"): the pointer's offset from the middle of the view,
    // half way to the edge is a full turn
    const F32 yaw = -((F32)x - mWorldCx) / (mWorldHalfW * 0.5f);
    const F32 pitch = ((F32)y - mWorldCy) / (mWorldHalfH * 0.5f);
    WolfStarfold::instance().setPointer(true, llclamp(pitch, -1.f, 1.f), llclamp(yaw, -1.f, 1.f));
}

bool WolfStarfoldDeck::handleMouseDown(S32 x, S32 y, MASK mask)
{
    WolfStarfold& sf = WolfStarfold::instance();
    if (mBandDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        mPressed = hit->mId;
        if (hit->mId != H_NONE)
        {
            gFocusMgr.setMouseCapture(this);
            press(hit->mId, x, y);
        }
        return true;
    }
    if (mBandDrawn && (F32)y >= mPanelB && (F32)y <= mPanelT) return true;   // the panel itself: not the world behind it
    // piloting, on the world above the panel: point and fly while the button is held
    if (sf.piloting() && !sf.panelHidden() && (F32)y > mPanelT)
    {
        mPressed = H_WORLD;
        gFocusMgr.setMouseCapture(this);
        steer(x, y);
        return true;
    }
    return false;
}

bool WolfStarfoldDeck::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE || mResize.dragging();
    mResize.end();
    if (mPressed == H_WORLD) WolfStarfold::instance().setPointer(false, 0.f, 0.f);
    if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
    mPressed = H_NONE;
    return was || hitAt(x, y) != nullptr || (mBandDrawn && (F32)y >= mPanelB && (F32)y <= mPanelT);
}

bool WolfStarfoldDeck::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (mPressed == H_THROTTLE) { dragThrottle(y); getWindow()->setCursor(UI_CURSOR_HAND); return true; }
        if (mPressed == H_WORLD) { steer(x, y); getWindow()->setCursor(UI_CURSOR_CROSS); return true; }
        if (mResize.dragging()) { mResize.drag(y); getWindow()->setCursor(UI_CURSOR_SIZENS); return true; }
    }
    if (mBandDrawn && mResize.onGrip(x, y))
    {
        mResize.hovered();
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        getWindow()->setCursor(hit->mId == H_NONE ? UI_CURSOR_ARROW : UI_CURSOR_HAND);
        return true;
    }
    return mBandDrawn && (F32)y >= mPanelB && (F32)y <= mPanelT;
}

bool WolfStarfoldDeck::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mBandDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();
        return true;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        // a quick second click counts as another press (FIRE / MINE toggle back, a second NEXT TARGET), as a single would
        if (hit->mId != H_NONE && hit->mId != H_THROTTLE) press(hit->mId, x, y);
        return true;
    }
    return mBandDrawn && (F32)y >= mPanelB && (F32)y <= mPanelT;
}

bool WolfStarfoldDeck::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mBandDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());
        return true;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        if (!hit->mTip.empty()) LLToolTipMgr::instance().show(hit->mTip);
        return true;
    }
    return false;
}

void WolfStarfoldDeck::onMouseCaptureLost()
{
    if (mPressed == H_WORLD) WolfStarfold::instance().setPointer(false, 0.f, 0.f);
    mPressed = H_NONE;
    mResize.end();
}
