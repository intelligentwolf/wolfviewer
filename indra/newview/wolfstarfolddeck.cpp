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

#include <algorithm>
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
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
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

    // the console's glass a pilot can choose (WolfStarfoldTheme 0..3; design: "a background the user can choose"). Glass Arc
    // (make_mockups.py "glass": #16305e at 0.55 at the top to #050b1a at 0.85): a little more opaque here, so the words on it keep
    // 4.5:1 over a sunlit cloud below.
    struct Theme { LLColor4 hi, lo; const char* name; };
    const Theme THEMES[4] = {
        { LLColor4(0.086f, 0.188f, 0.369f, 0.62f), LLColor4(0.020f, 0.043f, 0.102f, 0.88f), "Navy glass" },
        { LLColor4(0.20f, 0.10f, 0.32f, 0.62f), LLColor4(0.05f, 0.02f, 0.09f, 0.88f), "Nebula glass" },
        { LLColor4(0.06f, 0.06f, 0.07f, 0.66f), LLColor4(0.f, 0.f, 0.01f, 0.90f), "Black glass" },
        { LLColor4(0.f, 0.f, 0.f, 0.f), LLColor4(0.f, 0.f, 0.f, 0.f), "No panel" },
    };
    // the windows (Station, setup helper) and the ground panel stay solid: they are read, not flown through
    const Theme SOLID = { LLColor4(0.06f, 0.12f, 0.26f, 0.96f), LLColor4(0.02f, 0.04f, 0.10f, 0.96f), "Deep blue" };
    S32 theme()
    {
        static LLCachedControl<S32> t(gSavedSettings, "WolfStarfoldTheme", 0);
        return llclamp((S32)t, 0, 3);
    }

    // The picture's own units: 1600 across. The Glass Arc's console is ARC_EDGE_H tall at the window's sides and ARC_CENTRE_H in
    // the middle; PIC_H is how tall the cockpit stands with the hold line and the radar's stalks over it (the size limit below).
    const F32 PIC_W = 1600.f, PIC_H = 230.f;
    const F32 ARC_EDGE_H = 128.f, ARC_CENTRE_H = 196.f;

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
    // a filled fan from d0 to d1 degrees (clockwise from 12 o'clock, as clockArc)
    void fan(F32 cx, F32 cy, F32 r, F32 d0, F32 d1, const LLColor4& c, S32 seg = 16)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            F32 x0, y0, x1, y1;
            pt(cx, cy, r, d0 + (d1 - d0) * (F32)i / (F32)seg, x0, y0);
            pt(cx, cy, r, d0 + (d1 - d0) * (F32)(i + 1) / (F32)seg, x1, y1);
            vtx(cx, cy, c); vtx(x0, y0, c); vtx(x1, y1, c);
        }
        gGL.end();
    }
    // a capsule (rounded ends of radius h / 2), filled once everywhere (no overlap, so a translucent fill stays even) ...
    void pill(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        const F32 rad = llmin((t - b) * 0.5f, (r - l) * 0.5f), cy = (b + t) * 0.5f;
        rectF(l + rad, b, r - rad, t, c);
        fan(l + rad, cy, rad, 180.f, 360.f, c);
        fan(r - rad, cy, rad, 0.f, 180.f, c);
    }
    // ... and its outline
    void pillLine(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        const F32 rad = llmin((t - b) * 0.5f, (r - l) * 0.5f), cy = (b + t) * 0.5f;
        rectF(l + rad, b, r - rad, b + w, c);
        rectF(l + rad, t - w, r - rad, t, c);
        clockArc(l + rad, cy, rad - w * 0.5f, w, 180.f, 360.f, c, 16);
        clockArc(r - rad, cy, rad - w * 0.5f, w, 0.f, 180.f, c, 16);
    }
    void ellipseF(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& c, S32 seg = 48)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * F_PI * (F32)i / (F32)seg, a1 = 2.f * F_PI * (F32)(i + 1) / (F32)seg;
            vtx(cx, cy, c); vtx(cx + rx * cosf(a0), cy + ry * sinf(a0), c); vtx(cx + rx * cosf(a1), cy + ry * sinf(a1), c);
        }
        gGL.end();
    }
    void ellipseLine(F32 cx, F32 cy, F32 rx, F32 ry, F32 w, const LLColor4& c, S32 seg = 64)
    {
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * F_PI * (F32)i / (F32)seg, a1 = 2.f * F_PI * (F32)(i + 1) / (F32)seg;
            lineW(cx + rx * cosf(a0), cy + ry * sinf(a0), cx + rx * cosf(a1), cy + ry * sinf(a1), w, c);
        }
    }
    // the wolf-ear chevron (make_mockups.py wolfmark: M x y l6 -10 l4 6 l4 -6 l6 10 l-10 8 Z, y down there, up here), at scale k
    void wolfMark(F32 x, F32 y, F32 k, const LLColor4& c)
    {
        const F32 px[6] = { 0.f, 6.f, 10.f, 14.f, 20.f, 10.f }, py[6] = { 0.f, 10.f, 4.f, 10.f, 0.f, -8.f };
        for (S32 i = 0; i < 6; ++i)
        {
            const S32 j = (i + 1) % 6;
            lineW(x + px[i] * 2.f * k, y + py[i] * 2.f * k + 16.f * k, x + px[j] * 2.f * k, y + py[j] * 2.f * k + 16.f * k, llmax(1.5f, 1.6f * k), c);
        }
    }

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
    // the label at its size, made smaller (to 60 %) before it is ever cut short ([rig 24 Nov]: "NEXT TARG...", "LEAVE ...")
    F32 px = llmin((t - b) * 0.36f, 22.f * u);
    const F32 room = (r - l) - 10.f * u, smallest = px * 0.6f;
    while (px > smallest && textW(label, px, true) > room) px *= 0.92f;
    T((l + r) * 0.5f, (b + t) * 0.5f, fit(label, px, true, room), px, lit ? C_AMBER : C_TEXT, true);
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
    mArcOn = false;
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
        if (flying)
        {
            mPanelB = bottom;                          // the overlay keeps clear of the console (drawCockpit sets the same)
            mPanelT = bottom + ARC_CENTRE_H * u;
            drawHud(u);
            drawCockpit(bottom, W, u);
        }
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

// ── the Glass Arc (Paul chose design "A - Glass Arc", 24 Nov: mock-up ~/.cache/shadow-scratch/starfold-client/mockups/
//    starfold_hud_A_glass_arc.png, make_mockups.py concept_a): one slim curved glass console along the bottom, the rest of the
//    screen left to space; over the view a reticle, a pitch ladder, a heading tape and the target box. Our own look: navy glass,
//    amber and cyan vector lines, the wolf-ear mark. All figures below are the picture's units (1600 across) and GAME TUNING. ──

// the console's top edge at view-local x: EDGE at the window's sides, CENTRE in the middle, a parabola between (the mock-up's
// "Q 960 850" curve)
F32 WolfStarfoldDeck::arcTop(F32 x) const
{
    if (!mArcOn) return mPanelT;   // the ground panel is a plain band
    const F32 t = mArcW > 0.f ? llclamp((x - mArcW * 0.5f) / (mArcW * 0.5f), -1.f, 1.f) : 0.f;
    return mPanelB + mArcU * (ARC_EDGE_H + (ARC_CENTRE_H - ARC_EDGE_H) * (1.f - t * t));
}

bool WolfStarfoldDeck::onPanel(S32 x, S32 y) const
{
    return mBandDrawn && (F32)y >= mPanelB && (F32)y <= arcTop((F32)x);
}

void WolfStarfoldDeck::capsule(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& label, bool lit, const std::string& tip, F32 u,
                               bool dim)
{
    // a fully rounded button (make_mockups.py btn: rx = h / 2, navy fill 0.75, a cyan outline; lit: amber on dark amber)
    const bool down = mPressed == id;
    const LLColor4 fill = down ? LLColor4(0.20f, 0.34f, 0.56f, 0.92f) : (lit ? LLColor4(0.23f, 0.16f, 0.03f, 0.85f) : LLColor4(0.05f, 0.13f, 0.27f, 0.80f));
    const LLColor4 edge = lit ? C_AMBER : (dim ? C_CYAN * 0.45f : C_CYAN);
    pill(l, b, r, t, fill);
    pillLine(l, b, r, t, llmax(1.5f, 2.f * u), edge);
    F32 px = llmin((t - b) * 0.42f, 20.f * u);
    const F32 room = (r - l) - (t - b) * 0.6f, smallest = px * 0.6f;
    while (px > smallest && textW(label, px, true) > room) px *= 0.92f;
    T((l + r) * 0.5f, (b + t) * 0.5f, fit(label, px, true, room), px, lit ? C_AMBER : (dim ? C_DIM : C_TEXT), true);
    // the target is at least 44 px tall and wide whatever the drawn size (design: "Every target is at least 44 px")
    const F32 cy = (b + t) * 0.5f, hh = llmax((t - b) * 0.5f, 22.f), cx = (l + r) * 0.5f, hw = llmax((r - l) * 0.5f, 22.f);
    addHit(cx - hw, cy - hh, cx + hw, cy + hh, id, tip);
}

void WolfStarfoldDeck::drawCockpit(F32 bottom, F32 W, F32 u)
{
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Ship& s = sf.ship();
    const F32 ox = (W - PIC_W * u) * 0.5f;   // the picture centred when the window is wider than it scales to
    auto X = [&](F32 x) { return ox + x * u; };
    auto Y = [&](F32 y) { return bottom + y * u; };
    mPanelB = bottom;
    mPanelT = bottom + ARC_CENTRE_H * u;
    mArcW = W;
    mArcU = u;
    mArcOn = true;
    mBandDrawn = true;
    mResize.place(mPanelB, mPanelT, W);

    // ── the glass: strips under the curve, darker towards the bottom (make_mockups.py "glass": #16305e at 0.55 to #050b1a at
    //    0.85, a little more opaque here so the words keep 4.5:1 over a bright cloud), a cyan edge with a faint glow ──
    const Theme& th = THEMES[theme()];
    const S32 STRIPS = 64;
    if (th.hi.mV[VALPHA] > 0.f)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < STRIPS; ++i)
        {
            const F32 x0 = W * (F32)i / (F32)STRIPS, x1 = W * (F32)(i + 1) / (F32)STRIPS;
            const F32 t0 = arcTop(x0), t1 = arcTop(x1);
            vtx(x0, mPanelB, th.lo); vtx(x1, mPanelB, th.lo); vtx(x1, t1, th.hi);
            vtx(x0, mPanelB, th.lo); vtx(x1, t1, th.hi); vtx(x0, t0, th.hi);
        }
        gGL.end();
    }
    for (S32 i = 0; i < STRIPS; ++i)
    {
        const F32 x0 = W * (F32)i / (F32)STRIPS, x1 = W * (F32)(i + 1) / (F32)STRIPS;
        lineW(x0, arcTop(x0), x1, arcTop(x1), 7.f * u, LLColor4(0.24f, 0.50f, 0.84f, 0.18f));
        lineW(x0, arcTop(x0), x1, arcTop(x1), llmax(1.5f, 2.f * u), LLColor4(0.37f, 0.82f, 1.f, 0.65f));
    }

    // ── LEFT: shield, armour and hull as three upper arcs (make_mockups.py arcgauge: 140 degrees over the top), with words and
    //    numbers (never colour alone: WCAG 1.4.1) ──
    {
        const F32 cx = X(150.f), cy = Y(24.f);
        struct Bar { const char* name; F32 v, full; LLColor4 c; F32 r; };   // "full", not "max": <windows.h> defines max
        const Bar bars[3] = { { "SHIELD", s.mShield, s.mShieldMax, C_CYAN, 100.f }, { "ARMOUR", s.mArmour, s.mArmourMax, C_AMBER, 82.f },
                              { "HULL", s.mHull, s.mHullMax, C_RED, 64.f } };
        for (const Bar& b : bars)
        {
            const F32 f = llclamp(b.v / llmax(b.full, 1.f), 0.f, 1.f);
            clockArc(cx, cy, b.r * u, 10.f * u, -72.f, 72.f, LLColor4(b.c.mV[0], b.c.mV[1], b.c.mV[2], 0.25f));
            if (f > 0.f) clockArc(cx, cy, b.r * u, 10.f * u, -72.f, -72.f + 144.f * f, b.c);
        }
        T(cx, Y(52.f), s.mClass.empty() ? std::string("CUB") : upper(s.mClass), 15.f * u, C_TEXT, true);
        if (s.mSafe) T(cx, Y(32.f), "SAFE HARBOUR", 12.f * u, C_GREEN, true);
        else if (s.mAir) T(cx, Y(32.f), "IN THE AIR - CLIMBING", 12.f * u, C_AMBER, true);
        const F32 lx = X(272.f);
        T(lx, Y(118.f), llformat("SHIELD %d", (S32)llround(s.mShield)), 19.f * u, C_CYAN, true, LLFontGL::LEFT);
        T(lx, Y(94.f), llformat("ARMOUR %d", (S32)llround(s.mArmour)), 19.f * u, C_AMBER, true, LLFontGL::LEFT);
        T(lx, Y(70.f), llformat("HULL %d", (S32)llround(s.mHull)), 19.f * u, LLColor4(1.f, 0.54f, 0.49f, 1.f), true, LLFontGL::LEFT);
    }

    // ── CENTRE: the speed, the throttle and the mode in words, the throttle strip, the five big capsules ──
    {
        const F32 thr = sf.throttle();
        T(X(800.f), Y(172.f), llformat("%d m/s", (S32)llround(s.mSpeed)), 30.f * u, C_TEXT, true);
        T(X(800.f), Y(148.f), llformat("THROTTLE %d%%   %s", (S32)llround(thr * 100.f), sf.drift() ? "DRIFT" : "STEADY"), 14.f * u, C_AMBER, false);
        // the throttle strip: drag along it (W and S, Page Up and Page Down as well); in Drift its middle is stop
        const F32 sl = X(560.f), sr = X(1040.f), sb = Y(118.f), st = Y(128.f);
        mThrB = sl; mThrT = sr;
        pill(sl, sb, sr, st, LLColor4(0.02f, 0.05f, 0.12f, 0.85f));
        pillLine(sl, sb, sr, st, llmax(1.f, 1.5f * u), C_CYAN * 0.6f);
        const F32 zero = sf.drift() ? (sl + sr) * 0.5f : sl;
        const F32 at = sf.drift() ? zero + (sr - sl) * 0.5f * thr : sl + (sr - sl) * thr;
        if (fabsf(at - zero) > 1.f) rectF(llmin(zero, at), sb + 2.f * u, llmax(zero, at), st - 2.f * u, thr < 0.f ? C_RED : C_AMBER);
        rectF(at - 3.f * u, sb - 6.f * u, at + 3.f * u, st + 6.f * u, C_TEXT);
        const F32 hy = (sb + st) * 0.5f, hh = llmax(19.f * u, 22.f);
        addHit(sl - 10.f * u, hy - hh, sr + 10.f * u, hy + hh, H_THROTTLE, "Throttle: drag right for faster (W and S, or Page Up and Page Down)");

        // FIRE, MINE, NEXT TARGET, DOCK, FLY THERE (make_mockups.py concept_a: one row, FIRE lit while firing)
        struct Big { EHit id; const char* label; F32 w; bool lit; bool dim; const char* tip; };
        const Big bigs[5] = {
            { H_FIRE, "FIRE", 120.f, sf.firing(), false, "Fire at your target (hold Space)" },
            { H_MINE, "MINE", 120.f, sf.mining(), false, "Mine the nearest rock in reach (M)" },
            { H_NEXT, "NEXT TARGET", 176.f, false, false, "Pick the next Glasswing (Tab)" },
            { H_DOCK, "DOCK", 120.f, false, false, "Dock at the station (within 150 m)" },
            { H_FLY_PICKED, "FLY THERE", 160.f, !mPicked.empty(), mPicked.empty(),
              mPicked.empty() ? "Pick a place on the radar first, then FLY THERE" : "Fly to the place you picked on the radar" } };
        F32 total = 0.f;
        for (const Big& b : bigs) total += b.w;
        total += 12.f * 4.f;
        F32 bx = 800.f - total * 0.5f;
        for (const Big& b : bigs)
        {
            capsule(X(bx), Y(46.f), X(bx + b.w), Y(96.f), b.id, b.label, b.lit, b.tip, u, b.dim);
            bx += b.w + 12.f;
        }

        // the small capsules: the mode and Fly there to the region's places on the left, the cockpit's own on the right
        F32 fx = 785.f;
        auto small = [&](F32 w, EHit id, const std::string& label, bool lit, const std::string& tip, bool leftward)
        {
            if (leftward) { capsule(X(fx - w), Y(6.f), X(fx), Y(40.f), id, label, lit, tip, u); fx -= w + 8.f; }
            else { capsule(X(fx), Y(6.f), X(fx + w), Y(40.f), id, label, lit, tip, u); fx += w + 8.f; }
        };
        const WolfStarfold::System& sys = sf.system();
        if (!sf.flyingTo().empty()) small(70.f, H_FLY_STOP, "STOP", false, "Stop flying there", true);
        if (sys.mHaveWormhole) small(118.f, H_FLY_WORMHOLE, "WORMHOLE", sf.flyingTo() == "wormhole", "Fly into the wormhole", true);
        if (sys.mHaveBelt) small(72.f, H_FLY_BELT, "BELT", sf.flyingTo() == "belt", "Fly to the asteroid belt and stop at its edge", true);
        if (sys.mHaveStation) small(96.f, H_FLY_STATION, "STATION", sf.flyingTo() == "station", "Fly to the station and stop there", true);
        small(96.f, H_MODE, sf.drift() ? "DRIFT" : "STEADY", sf.drift(), "Steady flies where the nose points; Drift keeps sliding as you turn (Z)", true);
        fx = 815.f;
        small(44.f, H_HELP, "?", sf.guideShown(), "The guide: how to fly, mine, sell and fight", false);
        small(84.f, H_THEME, "PANEL", false, std::string("Change the console's glass (now: ") + THEMES[theme()].name + ")", false);
        small(72.f, H_HIDE, "HIDE", false, "Hide the cockpit (a tab brings it back)", false);
        small(124.f, H_LEAVE, "LEAVE SHIP", false, "Stand up and leave the ship", false);
    }

    // ── RIGHT: the hold and Starmarks, the target in words, the holographic radar and the wolf-ear mark ──
    {
        const F32 rx = X(1400.f);
        auto backed = [&](F32 x, F32 y, const std::string& text, F32 px, const LLColor4& c, bool bold)
        {
            const F32 w = textW(text, px, bold) + 18.f * u, h = px * 1.6f;
            pill(x - w * 0.5f, y - h * 0.5f, x + w * 0.5f, y + h * 0.5f, LLColor4(0.01f, 0.03f, 0.08f, 0.62f));
            T(x, y, text, px, c, bold);
        };
        const bool full = s.mHold >= s.mHoldMax - 0.5f && s.mHoldMax > 0.f;
        backed(rx, Y(240.f), llformat("HOLD %d/%d   %s %s", (S32)floorf(s.mHold), (S32)llround(s.mHoldMax), thousands(s.mCredits).c_str(),
               WolfStarfold::currency()), 14.f * u, full ? C_AMBER : C_TEXT, false);
        if (s.mTarget.notNull())
        {
            backed(rx, Y(216.f), llformat("TARGET  shield %d  armour %d  hull %d", (S32)llround(s.mTargetShield), (S32)llround(s.mTargetArmour),
                   (S32)llround(s.mTargetHull)), 13.f * u, C_VIOLET, true);
        }
        else
        {
            backed(rx, Y(216.f), s.mAliens.empty() ? std::string("NO TARGET   no Glasswings near") : llformat("NO TARGET   %d Glasswing%s near",
                   (S32)s.mAliens.size(), s.mAliens.size() == 1 ? "" : "s"), 13.f * u, s.mAliens.empty() ? C_DIM : C_VIOLET, true);
        }
        drawRadar(rx, Y(80.f), 150.f * u, 56.f * u, u);
        wolfMark(X(1548.f), Y(152.f), 1.2f * u, C_AMBER);
    }

    // a word for a press that could not act (FLY THERE with nothing picked): shown, never only a sound (ARIA rules, CLAUDE.md)
    if (!mHint.empty() && LLFrameTimer::getElapsedSeconds() < mHintUntil)
    {
        const F32 px = 17.f * u, w = textW(mHint, px, true) + 30.f * u, h = px * 1.8f, y = mPanelT + 40.f * u;
        pill(W * 0.5f - w * 0.5f, y - h * 0.5f, W * 0.5f + w * 0.5f, y + h * 0.5f, LLColor4(0.02f, 0.04f, 0.10f, 0.88f));
        pillLine(W * 0.5f - w * 0.5f, y - h * 0.5f, W * 0.5f + w * 0.5f, y + h * 0.5f, llmax(1.f, 1.5f * u), C_AMBER);
        T(W * 0.5f, y, mHint, px, C_AMBER, true);
    }
}

void WolfStarfoldDeck::drawRadar(F32 cx, F32 cy, F32 rx, F32 ry, F32 u)
{
    // A holographic radar (make_mockups.py radar): a flat ellipse seen from above and behind, nose away from the pilot; each
    // contact stands on a stalk whose height is its height above or below the ship, and its label says the height in metres
    // (never the stalk alone). Its range: 1.5 km. Labels never sit on each other: a label that would overlap one already
    // placed moves up or down a line, and is left out (its mark stays) when there is no room.
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Ship& s = sf.ship();
    const WolfStarfold::System& sys = sf.system();
    const F32 RANGE = 1500.f;
    ellipseF(cx, cy, rx, ry, LLColor4(0.04f, 0.16f, 0.27f, 0.45f));
    ellipseLine(cx, cy, rx, ry, llmax(1.5f, 2.f * u), C_CYAN);
    ellipseLine(cx, cy, rx * 0.5f, ry * 0.5f, 1.f * u, C_CYAN * 0.45f);
    lineW(cx - rx, cy, cx + rx, cy, 1.f * u, LLColor4(0.38f, 0.90f, 1.f, 0.3f));
    lineW(cx, cy - ry, cx, cy + ry, 1.f * u, LLColor4(0.38f, 0.90f, 1.f, 0.3f));
    T(cx + rx * 0.78f, cy - ry - 8.f * u, "1.5 km", 11.f * u, C_DIM, false);
    // our ship (make_mockups.py: an amber triangle at the centre, nose up the scope)
    quadF(cx, cy + 9.f * u, cx + 7.f * u, cy - 5.f * u, cx, cy - 2.f * u, cx - 7.f * u, cy - 5.f * u, C_AMBER);
    LLViewerObject* ship = gObjectList.findObject(s.mRoot);
    LLViewerRegion* region = gAgent.getRegion();
    if (!ship || !region) return;
    LLViewerObject* root = ship->getRootEdit() ? ship->getRootEdit() : ship;
    const LLVector3 me = root->getPositionAgent();
    const LLQuaternion inv = ~root->getRotationRegion();
    const F32 STALK = 44.f * u;   // the longest stalk (a contact 1.5 km above or more)
    struct Mark { F32 x, y, head; F32 dz; };
    struct Lab { F32 x, y; std::string text; LLColor4 c; S32 pri; };   // pri: who gets the room first (lower first)
    auto place = [&](const LLVector3& agent) -> Mark
    {
        const LLVector3 rel = (agent - me) * inv;   // the ship's frame: +X ahead, +Y left (SpaceMath: the nose is +X)
        Mark m;
        m.dz = agent.mV[VZ] - me.mV[VZ];
        F32 fx = rel.mV[VX], fy = rel.mV[VY];
        const F32 d = sqrtf(fx * fx + fy * fy);
        if (d > RANGE) { fx *= RANGE / d; fy *= RANGE / d; }
        m.x = cx - fy / RANGE * rx * 0.94f;   // left of the nose is left on the scope
        m.y = cy + fx / RANGE * ry * 0.94f;
        m.head = m.y + llclamp(m.dz / RANGE * STALK, -STALK, STALK);
        return m;
    };
    auto stalk = [&](const Mark& m, const LLColor4& c)
    {
        circleF(m.x, m.y, 2.5f * u, LLColor4(c.mV[0], c.mV[1], c.mV[2], 0.5f), 12);   // its foot on the plane
        if (fabsf(m.head - m.y) > 1.f) lineW(m.x, m.y, m.x, m.head, llmax(1.f, 2.f * u), c);
    };
    std::vector<LLRect> placed;
    auto label = [&](F32 x, F32 y, const std::string& text, const LLColor4& c)
    {
        const F32 px = 11.f * u, w = textW(text, px, true), h = px * 1.4f;
        const F32 tries[5] = { 0.f, h, -h, 2.f * h, -2.f * h };
        for (F32 dy : tries)
        {
            const LLRect r((S32)(x - w * 0.5f - 2.f), (S32)(y + dy + h * 0.5f), (S32)(x + w * 0.5f + 2.f), (S32)(y + dy - h * 0.5f));
            bool clear = true;
            for (const LLRect& o : placed) if (r.overlaps(o)) { clear = false; break; }
            if (!clear) continue;
            placed.push_back(r);
            T(x, y + dy, text, px, c, true);
            return;
        }
    };
    auto height = [](F32 dz) { return fabsf(dz) < 50.f ? std::string() : llformat(" %+d m", (S32)llround(dz)); };
    const F32 hit = llmax(22.f * u, 22.f);   // 44 px across at normal size (design: "Every target is at least 44 px")
    // marks first, labels after (nearest the pilot's eye first: the picked place, then the station, the wormhole, the belt)
    std::vector<Lab> labels;
    if (sys.mHaveBelt)
    {
        const Mark m = place(region->getPosAgentFromRegion(sys.mBeltCentre));
        const F32 br = llmin(rx * 0.95f, sys.mBeltR / RANGE * rx);
        ellipseLine(m.x, m.y, llmax(br, 8.f * u), llmax(br * ry / rx, 3.f * u), 3.f * u,
                    LLColor4(0.69f, 0.54f, 0.38f, mPicked == "belt" ? 1.f : 0.75f));
        labels.push_back({ m.x, m.y - 12.f * u, "BELT" + height(m.dz), mPicked == "belt" ? C_AMBER : LLColor4(0.85f, 0.72f, 0.56f, 1.f), mPicked == "belt" ? 0 : 3 });
        addHit(m.x - hit, m.y - hit, m.x + hit, m.y + hit, H_RADAR_BELT, "The asteroid belt: click, then FLY THERE");
    }
    if (sys.mHaveStation)
    {
        const Mark m = place(region->getPosAgentFromRegion(sys.mStation));
        const LLColor4 c = mPicked == "station" ? C_AMBER : C_TEXT;
        stalk(m, c);
        rectF(m.x - 6.f * u, m.head - 6.f * u, m.x + 6.f * u, m.head + 6.f * u, c);
        labels.push_back({ m.x, m.head + 14.f * u, "STATION" + height(m.dz), c, mPicked == "station" ? 0 : 1 });
        addHit(m.x - hit, m.head - hit, m.x + hit, m.head + hit, H_RADAR_STATION, "The station: click, then FLY THERE");
    }
    if (sys.mHaveWormhole)
    {
        const Mark m = place(region->getPosAgentFromRegion(sys.mWormhole));
        const LLColor4 c = mPicked == "wormhole" ? C_AMBER : C_CYAN;
        stalk(m, c);
        ring(m.x, m.head, 7.f * u, 2.5f * u, c);
        labels.push_back({ m.x, m.head + 16.f * u, "WORMHOLE" + height(m.dz), c, mPicked == "wormhole" ? 0 : 2 });
        addHit(m.x - hit, m.head - hit, m.x + hit, m.head + hit, H_RADAR_WORMHOLE, "The wormhole to " + sys.mWormholeTo + ": click, then FLY THERE");
    }
    for (size_t i = 0; i < s.mAliens.size() && i < 6; ++i)
    {
        LLViewerObject* a = gObjectList.findObject(s.mAliens[i]);
        if (!a) continue;
        const Mark m = place(a->getPositionAgent());
        const bool tg = s.mAliens[i] == s.mTarget || s.mAliens[i] == sf.selected();
        stalk(m, C_VIOLET);
        circleF(m.x, m.head, 5.f * u, C_VIOLET, 16);
        if (tg)
        {
            ring(m.x, m.head, 10.f * u, 2.f * u, C_AMBER);
            labels.push_back({ m.x, m.head + 16.f * u, "GLASSWING" + height(m.dz), LLColor4(0.85f, 0.77f, 1.f, 1.f), 0 });
        }
        addHit(m.x - hit, m.head - hit, m.x + hit, m.head + hit, (EHit)(H_ALIEN_0 + (S32)i), "A Glasswing raider: click to make it your target");
    }
    std::stable_sort(labels.begin(), labels.end(), [](const Lab& a, const Lab& b) { return a.pri < b.pri; });
    for (const Lab& l : labels) label(l.x, l.y, l.text, l.c);
}

void WolfStarfoldDeck::drawHud(F32 u)
{
    // Over the view: the reticle where the nose points, the pitch ladder and the heading tape, and the amber box round the
    // targeted Glasswing with its range and closing speed (make_mockups.py reticle). Every point is projected as
    // WolfStarfold::renderLens places the wormhole: the camera basis (at, left, up), LLCamera's vertical field of view and
    // aspect, onto the world view's rectangle (LLViewerWindow::getWorldViewRectScaled, in this view's coordinates).
    WolfStarfold& sf = WolfStarfold::instance();
    const WolfStarfold::Ship& s = sf.ship();
    LLViewerObject* ship = gObjectList.findObject(s.mRoot);
    if (!ship || !gViewerWindow) return;
    LLViewerObject* root = ship->getRootEdit() ? ship->getRootEdit() : ship;
    const LLVector3 me = root->getPositionAgent();
    const LLVector3 fwd = LLVector3::x_axis * root->getRotationRegion();   // the nose is +X (SpaceMath)
    LLRect world = gViewerWindow->getWorldViewRectScaled();
    const LLRect mine = calcScreenRect();
    world.translate(-mine.mLeft, -mine.mBottom);
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin();
    const F32 tan_half = tanf(cam->getView() * 0.5f), aspect = cam->getAspect();
    // agent point -> view-local x, y; false when it is behind the eye (x, y then hold its left / up offsets, for an edge arrow)
    auto project = [&](const LLVector3& p, F32& x, F32& y) -> bool
    {
        const LLVector3 d = p - eye;
        const F32 f = d * cam->getAtAxis();
        const F32 lx = -(d * cam->getLeftAxis()), uy = d * cam->getUpAxis();
        if (f <= 0.5f) { x = lx; y = uy; return false; }
        x = (F32)world.mLeft + ((lx / f) / (tan_half * aspect) * 0.5f + 0.5f) * (F32)world.getWidth();
        y = (F32)world.mBottom + ((uy / f) / tan_half * 0.5f + 0.5f) * (F32)world.getHeight();
        return true;
    };
    const LLColor4 cyan(0.37f, 0.82f, 1.f, 0.9f);
    const F32 lw = llmax(1.5f, 2.f * u);

    // The reticle is the view's own middle (the chase camera does not look along the nose: the rig, 24 Nov 18:37Z, had the
    // nose's point off the screen), and the ladder is laid on the camera's heading, so both always stand where the eye looks;
    // where the nose points (3 km ahead) has its own small diamond when it is on the screen.
    const F32 rx = (F32)world.getCenterX(), ry = (F32)world.getCenterY();
    ring(rx, ry, 26.f * u, lw, cyan);
    lineW(rx - 60.f * u, ry, rx - 30.f * u, ry, lw, cyan);
    lineW(rx + 30.f * u, ry, rx + 60.f * u, ry, lw, cyan);
    lineW(rx, ry + 30.f * u, rx, ry + 44.f * u, lw, cyan);
    {
        F32 nx, ny;
        if (project(me + fwd * 3000.f, nx, ny) && nx > (F32)world.mLeft && nx < (F32)world.mRight && ny > mPanelT && ny < (F32)world.mTop)
        {
            const F32 d = 9.f * u;
            lineW(nx - d, ny, nx, ny + d, lw, C_AMBER); lineW(nx, ny + d, nx + d, ny, lw, C_AMBER);
            lineW(nx + d, ny, nx, ny - d, lw, C_AMBER); lineW(nx, ny - d, nx - d, ny, lw, C_AMBER);
        }
    }

    // the pitch ladder: the horizon-level lines at +-10 and +-20 degrees on the camera's heading (solid above, dashed below)
    LLVector3 hc(cam->getAtAxis().mV[VX], cam->getAtAxis().mV[VY], 0.f);
    LLVector3 h(fwd.mV[VX], fwd.mV[VY], 0.f);
    const bool level = h.normVec() > 0.1f;   // the nose not straight up or down: a heading makes sense
    if (hc.normVec() > 0.1f)
    {
        const LLVector3& h = hc;
        const LLVector3 side(-h.mV[VY], h.mV[VX], 0.f);
        for (S32 k : { -2, -1, 1, 2 })
        {
            const F32 th = (F32)k * 10.f * DEG_TO_RAD;
            const LLVector3 dir = h * cosf(th) + LLVector3::z_axis * sinf(th);
            F32 x0, y0, x1, y1;
            if (!project(eye + dir * 3000.f, x0, y0) || !project(eye + (dir + side * 0.02f) * 3000.f, x1, y1)) continue;
            F32 sx = x1 - x0, sy = y1 - y0;
            const F32 sl = sqrtf(sx * sx + sy * sy);
            if (sl < 1e-3f) continue;
            sx /= sl; sy /= sl;
            for (F32 side_sign : { -1.f, 1.f })
            {
                const F32 a = 90.f * u, b = 170.f * u;
                if (k > 0)
                {
                    lineW(x0 + sx * a * side_sign, y0 + sy * a * side_sign, x0 + sx * b * side_sign, y0 + sy * b * side_sign, lw, cyan);
                }
                else
                {
                    for (S32 d = 0; d < 5; ++d)   // dashed (make_mockups.py: "10 6")
                    {
                        const F32 p0 = a + (b - a) * (F32)d / 5.f, p1 = p0 + (b - a) / 5.f * 0.62f;
                        lineW(x0 + sx * p0 * side_sign, y0 + sy * p0 * side_sign, x0 + sx * p1 * side_sign, y0 + sy * p1 * side_sign, lw, cyan);
                    }
                }
            }
            T(x0 - sx * 190.f * u, y0 - sy * 190.f * u, llformat("%+d", k * 10), 14.f * u, cyan, true);
        }
    }

    // the heading tape across the top: compass degrees (the region's +y is north, +x east), ticks every 5, tall every 15
    if (level)
    {
        const F32 heading = fmodf(atan2f(h.mV[VX], h.mV[VY]) * RAD_TO_DEG + 360.f, 360.f);
        const F32 ty = (F32)world.mTop - 118.f * u, tcx = (F32)world.getCenterX(), per = 5.f * u;
        for (S32 d = (S32)floorf((heading - 60.f) / 5.f) * 5; d <= (S32)(heading + 60.f); d += 5)
        {
            const F32 x = tcx + ((F32)d - heading) * per;
            if (fabsf(x - tcx) > 60.f * per) continue;
            const bool tall = ((d % 15) + 15) % 15 == 0;
            lineW(x, ty, x, ty - (tall ? 18.f : 10.f) * u, llmax(1.f, 1.5f * u), LLColor4(0.37f, 0.82f, 1.f, 0.7f));
        }
        T(tcx, ty + 16.f * u, llformat("%03d", ((S32)llround(heading)) % 360), 20.f * u, cyan, true);
    }

    // the target: an amber box round it, "GLASSWING 640 m" over it and the closing speed under it; off the screen, an arrow at
    // the edge pointing the way to turn
    LLViewerObject* t = s.mTarget.notNull() ? gObjectList.findObject(s.mTarget) : nullptr;
    if (!t)
    {
        mClosingFor.setNull();
        return;
    }
    LLViewerObject* troot = t->getRootEdit() ? t->getRootEdit() : t;
    const LLVector3 tp = troot->getPositionAgent();
    const F32 range = (tp - me).length();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mClosingFor != s.mTarget)
    {
        mClosingFor = s.mTarget;
        mClosing = 0.f;
        mLastRange = range;
        mLastRangeAt = now;
    }
    else if (now - mLastRangeAt > 0.05)
    {
        // closing = how fast the range falls, smoothed over about half a second (the positions arrive in steps)
        const F32 dt = (F32)(now - mLastRangeAt);
        const F32 rate = -(range - mLastRange) / dt;
        mClosing += (rate - mClosing) * (1.f - expf(-dt / 0.5f));
        mLastRange = range;
        mLastRangeAt = now;
    }
    const std::string name = "GLASSWING  " + distance(range);
    const std::string closing = fabsf(mClosing) < 1.f ? std::string("holding range")
                              : (mClosing > 0.f ? llformat("closing %d m/s", (S32)llround(mClosing)) : llformat("opening %d m/s", (S32)llround(-mClosing)));
    F32 x, y;
    const bool front = project(tp, x, y);
    const bool onScreen = front && x >= (F32)world.mLeft && x <= (F32)world.mRight && y >= mPanelT && y <= (F32)world.mTop;
    if (onScreen)
    {
        const F32 hs = llclamp(9000.f / llmax(range, 1.f), 26.f, 70.f) * u, k = hs * 0.35f, bw = llmax(2.f, 2.5f * u);
        for (S32 sx : { -1, 1 })
            for (S32 sy : { -1, 1 })
            {
                const F32 cx = x + hs * (F32)sx, cy = y + hs * (F32)sy;
                lineW(cx, cy, cx - k * (F32)sx, cy, bw, C_AMBER);
                lineW(cx, cy, cx, cy - k * (F32)sy, bw, C_AMBER);
            }
        T(x, y + hs + 14.f * u, name, 17.f * u, C_AMBER, true);
        T(x, y - hs - 14.f * u, closing, 14.f * u, C_AMBER, false);
    }
    else
    {
        // the way to it from the middle of the view: on screen past an edge, or behind (its left / up offsets)
        const F32 mx = (F32)world.getCenterX(), my = ((F32)world.mTop + mPanelT) * 0.5f;
        F32 dx = front ? x - mx : x, dy = front ? y - my : y;
        const F32 dl = sqrtf(dx * dx + dy * dy);
        if (dl < 1e-3f) { dx = 0.f; dy = -1.f; } else { dx /= dl; dy /= dl; }
        const F32 reach = llmin((F32)world.getWidth() * 0.5f, ((F32)world.mTop - mPanelT) * 0.5f) * 0.82f;
        const F32 ax = mx + dx * reach, ay = my + dy * reach, a = 16.f * u;
        quadF(ax + dx * a, ay + dy * a, ax - dy * a * 0.6f, ay + dx * a * 0.6f, ax, ay, ax + dy * a * 0.6f, ay - dx * a * 0.6f, C_AMBER);
        T(ax - dx * 34.f * u, ay - dy * 34.f * u, name, 15.f * u, C_AMBER, true);
    }
}

void WolfStarfoldDeck::showHint(const std::string& text)
{
    mHint = text;
    mHintUntil = LLFrameTimer::getElapsedSeconds() + 4.0;
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
    rectG(0.f, mPanelB, W, mPanelT, SOLID.hi, SOLID.lo);
    rectF(0.f, mPanelT - 2.f * u, W, mPanelT, C_EDGE * 0.8f);
    const F32 ox = (W - PIC_W * u) * 0.5f;
    auto X = [&](F32 x) { return ox + x * u; };
    auto Y = [&](F32 y) { return bottom + y * u; };
    T(X(40.f), Y(84.f), upper(WolfStarfold::gameName()), 30.f * u, C_AMBER, true, LLFontGL::LEFT);
    T(X(40.f), Y(50.f), fit(llformat("Space starts %s above the sea here.", distance(sf.system().mHeight).c_str()), 18.f * u, false, 630.f * u),
      18.f * u, C_TEXT, false, LLFontGL::LEFT);
    T(X(40.f), Y(22.f), fit("Press LAUNCH where you stand, or go to the pad or the station first.", 16.f * u, false, 630.f * u), 16.f * u,
      C_DIM, false, LLFontGL::LEFT);
    // wolf-space 1.0.4 "goto": the region takes you there (past the landing point); its answer shows above the panel (k=msg)
    button(X(690.f), Y(16.f), X(880.f), Y(100.f), H_GOTO_PAD, "TO THE PAD", false, "Take me to the launch pad", u);
    if (sf.system().mHaveStation)
        button(X(890.f), Y(16.f), X(1060.f), Y(100.f), H_GOTO_STATION, "TO THE STATION", false, "Take me onto the station's deck", u);
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
    rectG(l, b, r, t, SOLID.hi, SOLID.lo);
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
    rectG(l, b, r, t, SOLID.hi, SOLID.lo);
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
        case H_THROTTLE: dragThrottle(x); break;
        case H_FLY_STATION: sf.flyTo("station"); break;
        case H_FLY_BELT: sf.flyTo("belt"); break;
        case H_FLY_WORMHOLE: sf.flyTo("wormhole"); break;
        case H_FLY_STOP: sf.flyTo("stop"); break;
        case H_FLY_PICKED:
            if (mPicked.empty()) showHint("Pick a place on the radar first, then press FLY THERE");
            else { sf.flyTo(mPicked); mPicked.clear(); }
            break;
        case H_RADAR_STATION: mPicked = "station"; break;
        case H_RADAR_BELT: mPicked = "belt"; break;
        case H_RADAR_WORMHOLE: mPicked = "wormhole"; break;
        case H_HELP: sf.showGuide(!sf.guideShown()); break;
        case H_THEME: gSavedSettings.setS32("WolfStarfoldTheme", (theme() + 1) % 4); break;
        case H_HIDE: sf.togglePanel(); break;
        case H_SHOW_TAB: sf.togglePanel(); break;
        case H_LEAVE: sf.leave(); break;
        case H_LAUNCH: sf.launch(); break;
        case H_GOTO_PAD: sf.goTo("pad"); break;
        case H_GOTO_STATION: sf.goTo("station"); break;
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

void WolfStarfoldDeck::dragThrottle(S32 x)
{
    // the Glass Arc's strip runs left (stop, or full reverse in Drift) to right (full ahead): mThrB / mThrT its two ends
    WolfStarfold& sf = WolfStarfold::instance();
    const F32 f = llclamp(((F32)x - mThrB) / llmax(mThrT - mThrB, 1.f), 0.f, 1.f);
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
    if (onPanel(x, y)) return true;   // the console itself (under its curved edge): not the world behind it
    // piloting, on the world above the panel: point and fly while the button is held
    if (sf.piloting() && !sf.panelHidden() && !onPanel(x, y))
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
    return was || hitAt(x, y) != nullptr || onPanel(x, y);
}

bool WolfStarfoldDeck::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (mPressed == H_THROTTLE) { dragThrottle(x); getWindow()->setCursor(UI_CURSOR_HAND); return true; }
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
    return onPanel(x, y);
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
    return onPanel(x, y);
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
