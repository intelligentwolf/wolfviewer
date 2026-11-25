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
#include "llclipboard.h"
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
#include "wolfstarbase.h"
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

namespace { WolfStarfoldDeck* sDeck = nullptr; }   // the one deck (main_view.xml "wolf_starfold_deck")

WolfStarfoldDeck::WolfStarfoldDeck(const Params& p) : LLUICtrl(p)
{
    sDeck = this;
}

WolfStarfoldDeck::~WolfStarfoldDeck()
{
    if (sDeck == this) sDeck = nullptr;
}

// static
void WolfStarfoldDeck::logHits()
{
    if (!sDeck) { LL_INFOS("WolfStarfold") << "hits: no deck" << LL_ENDL; return; }
    // view-local UI units -> window GL pixels: LLView::localPointToScreen gives the root view's (UI) coordinates, and
    // LLViewerWindow::handleMouseDown divides window GL pixels by the UI scale (llviewerwindow.cpp: x / mDisplayScale.mV[VX])
    const LLVector2 scale = gViewerWindow ? gViewerWindow->getDisplayScale() : LLVector2(1.f, 1.f);
    LL_INFOS("WolfStarfold") << "hits: " << sDeck->mHits.size() << " (display scale " << scale.mV[VX] << ")" << LL_ENDL;
    for (const Hit& h : sDeck->mHits)
    {
        S32 sx = 0, sy = 0;
        sDeck->localPointToScreen(ll_round((h.mL + h.mR) * 0.5f), ll_round((h.mB + h.mT) * 0.5f), &sx, &sy);
        LL_INFOS("WolfStarfold") << "hit " << (S32)h.mId << " gl " << ll_round((F32)sx * scale.mV[VX]) << "," << ll_round((F32)sy * scale.mV[VY])
                                 << " tip " << h.mTip << LL_ENDL;
    }
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
    const bool basewin = WolfStarbase::instance().shown();   // [STARBASES 2026-11-24] the Starbase window
    if (!setup) mClearArmed = false;
    if (!basewin) mArmed.clear();
    // [WOLFTREK v11] the rename field goes with its window (the Station window closed or left, the Starbase window off its own base)
    if (mEdit == EDIT_STATION && !(station && !basewin && sf.canRenameStation())) cancelEdit();
    if (mEdit == EDIT_BASE && !(basewin && WolfStarbase::instance().tab() == WolfStarbase::TAB_BASE && WolfStarbase::instance().known()
                                && WolfStarbase::instance().hasBase())) cancelEdit();
    if (!band && !station && !guide && !tab && !setup && !basewin && sf.notes().empty())
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
    if (sf.automatic()) drawBanner(llmax(u, 0.8f));   // shown with the cockpit hidden too
    if (basewin) drawBase(top_of_world, W, H, llmax(u, 0.8f));   // opened from the Station window too: it goes over it
    else if (station) drawStation(top_of_world, W, H, llmax(u, 0.8f));
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
        // [WOLFTREK v11] the station by its name (design.html#names: "the radar's station label and the Fly there button ('Fly to
        // Kestrel Reach')"); with a fly-in bay DOCK and FLY THERE work from anywhere here (SpaceBay.cs BayDock: the autopilot to
        // the approach point, then the automatic approach)
        const std::string sname = sf.stationName();
        const std::string stationWords = sname.empty() ? std::string("the station") : sname;
        const std::string dockTip = sf.system().mBay ? "Dock at " + stationWords + ": your ship flies in by itself from anywhere here"
                                                     : "Dock at " + stationWords + " (within 150 m)";
        std::string flyLabel = "FLY THERE";
        if (mPicked == "station" && !sname.empty()) flyLabel = "FLY TO " + upper(sname);
        // the FLY THERE capsule grows with a name, up to 230 (the centre row then still clears the gauges' words and the radar)
        const F32 flyW = llclamp(textW(flyLabel, 20.f * u, true) / llmax(u, 0.01f) + 34.f, 160.f, 230.f);
        const std::string flyTip = mPicked.empty() ? std::string("Pick a place on the radar first, then FLY THERE")
                                 : (mPicked == "station" ? "Fly to " + stationWords + (sf.system().mBay ? " and dock in its bay" : "")
                                                         : std::string("Fly to the place you picked on the radar"));
        struct Big { EHit id; std::string label; F32 w; bool lit; bool dim; std::string tip; };
        const Big bigs[5] = {
            { H_FIRE, "FIRE", 120.f, sf.firing(), false, "Fire at your target (hold Space)" },
            { H_MINE, "MINE", 120.f, sf.mining(), false, "Mine the nearest rock in reach (M)" },
            { H_NEXT, "NEXT TARGET", 176.f, false, false, "Pick the next Glasswing (Tab)" },
            { H_DOCK, "DOCK", 120.f, false, false, dockTip },
            { H_FLY_PICKED, flyLabel, flyW, !mPicked.empty(), mPicked.empty(), flyTip } };
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
        auto capsuleSmall = [&](F32 w, EHit id, const std::string& label, bool lit, const std::string& tip, bool leftward)
        {
            if (leftward) { capsule(X(fx - w), Y(6.f), X(fx), Y(40.f), id, label, lit, tip, u); fx -= w + 8.f; }
            else { capsule(X(fx), Y(6.f), X(fx + w), Y(40.f), id, label, lit, tip, u); fx += w + 8.f; }
        };
        const WolfStarfold::System& sys = sf.system();
        if (!sf.flyingTo().empty()) capsuleSmall(70.f, H_FLY_STOP, "STOP", false, "Stop flying there", true);
        if (sys.mHaveWormhole) capsuleSmall(118.f, H_FLY_WORMHOLE, "WORMHOLE", sf.flyingTo() == "wormhole", "Fly into the wormhole", true);
        if (sys.mHaveBelt) capsuleSmall(72.f, H_FLY_BELT, "BELT", sf.flyingTo() == "belt", "Fly to the asteroid belt and stop at its edge", true);
        if (sys.mHaveStation) capsuleSmall(96.f, H_FLY_STATION, "STATION", sf.flyingTo() == "station",
                                           sys.mBay ? "Fly to " + stationWords + " and dock in its bay" : "Fly to " + stationWords + " and stop there", true);
        capsuleSmall(96.f, H_MODE, sf.drift() ? "DRIFT" : "STEADY", sf.drift(), "Steady flies where the nose points; Drift keeps sliding as you turn (Z)", true);
        fx = 815.f;
        capsuleSmall(44.f, H_HELP, "?", sf.guideShown(), "The guide: how to fly, mine, sell and fight", false);
        capsuleSmall(84.f, H_THEME, "PANEL", false, std::string("Change the console's glass (now: ") + THEMES[theme()].name + ")", false);
        capsuleSmall(72.f, H_HIDE, "HIDE", false, "Hide the cockpit (a tab brings it back)", false);
        capsuleSmall(124.f, H_LEAVE, "LEAVE SHIP", false, "Stand up and leave the ship", false);
        // [STARBASES 2026-11-24] design.html#bases: "in the cockpit (a Base button)"
        capsuleSmall(76.f, H_BASE_OPEN, "BASE", WolfStarbase::instance().shown(), "Your starbase: found it, build on it, go home", false);
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
        // [WOLFTREK v11] its name (k=sys sn=) once it has one
        const std::string sname = sf.stationName();
        labels.push_back({ m.x, m.head + 14.f * u, (sname.empty() ? std::string("STATION") : upper(sname)) + height(m.dz), c, mPicked == "station" ? 0 : 1 });
        addHit(m.x - hit, m.head - hit, m.x + hit, m.head + hit, H_RADAR_STATION,
               (sname.empty() ? std::string("The station") : sname + ", the station") + ": click, then FLY THERE or DOCK");
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
    // [STARBASES 2026-11-24] the starbases here (k=bases, SpaceBases.cs SendBasesList: "so the radar and Fly there know them"): a
    // small diamond each, yours labelled once the Starbase window has learned which it is; click one, then FLY THERE or DOCK
    {
        const WolfStarbase& sb = WolfStarbase::instance();
        for (size_t i = 0; i < sb.bases().size() && i < 24; ++i)
        {
            const WolfStarbase::Known& kb = sb.bases()[i];
            const Mark m = place(region->getPosAgentFromRegion(kb.mPos));
            const bool picked = mPicked == "base:" + kb.mId, mineBase = !sb.baseId().empty() && kb.mId == sb.baseId();
            const LLColor4 c = picked ? C_AMBER : (mineBase ? C_GREEN : C_DIM);
            stalk(m, c);
            const F32 d = 6.f * u;
            quadF(m.x, m.head + d, m.x + d, m.head, m.x, m.head - d, m.x - d, m.head, c);
            // [WOLFTREK v11] its own name with its owner's (design.html#names: "The radar ... use the base's name together with
            // its owner's name"); without a name, as before
            std::string what;
            if (mineBase) what = kb.mName.empty() ? std::string("YOUR BASE") : upper(kb.mName) + " (YOURS)";
            else if (kb.mName.empty()) what = upper(kb.mOwner.empty() ? std::string("STARBASE") : kb.mOwner);
            else what = upper(kb.mName) + (kb.mOwner.empty() ? std::string() : " (" + upper(kb.mOwner) + ")");
            if (picked || mineBase) labels.push_back({ m.x, m.head + 14.f * u, what + height(m.dz), c, picked ? 0 : 2 });
            std::string who = mineBase ? std::string("Your starbase") : (kb.mOwner.empty() ? std::string("A starbase") : kb.mOwner + "'s starbase");
            if (!kb.mName.empty()) who = kb.mName + ", " + (mineBase ? std::string("your starbase") : who);
            addHit(m.x - hit, m.head - hit, m.x + hit, m.head + hit, (EHit)(H_RADAR_BASE_0 + (S32)i), who + ": click, then FLY THERE or DOCK");
        }
    }
    for (size_t i = 0; i < s.mAliens.size() && i < 6; ++i)
    {
        LLVector3 at;   // its object, or the region's position for it (ap=) when it is beyond what this viewer was sent
        if (!sf.alienAt(s.mAliens[i], at)) continue;
        const Mark m = place(at);
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
    // ([WOLFTREK v11] not while the bay's automatic approach or departure flies: its banner stands there, drawBanner)
    if (level && !sf.automatic())
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
    // (its object when this viewer has it, else the region's tgp= / ap= position: at the normal far clip the Glasswing is often
    // not in the object list at all - the rig, 24 Nov: none reached it at RenderFarClip 512)
    LLVector3 tp;
    if (!sf.alienAt(s.mTarget, tp))
    {
        mClosingFor.setNull();
        return;
    }
    const F32 range = (tp - me).length();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mClosingFor != s.mTarget)
    {
        // which position the box stands on (a test reads this: the far clip decides whether the object is here at all)
        LL_INFOS("WolfStarfold") << "target box: " << (gObjectList.findObject(s.mTarget) ? "the object" : "the region's position (tgp/ap)")
                                 << ", " << (S32)llround((tp - me).length()) << " m" << LL_ENDL;
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
        button(X(890.f), Y(16.f), X(1060.f), Y(100.f), H_GOTO_STATION, "TO THE STATION", false,
               sf.stationName().empty() ? std::string("Take me onto the station") : "Take me onto " + sf.stationName(), u);
    button(X(1080.f), Y(16.f), X(1300.f), Y(100.f), H_LAUNCH, "LAUNCH", false, "Bring out your Cub and climb to space", u, true);
    button(X(1320.f), Y(16.f), X(1430.f), Y(100.f), H_HELP, "GUIDE", sf.guideShown(), "How to fly, mine, sell and fight", u);
    button(X(1450.f), Y(16.f), X(1560.f), Y(100.f), H_CLOSE, "CLOSE", false, std::string("Close this panel (Wolf World > ") + WolfStarfold::gameName() + " (Space) opens it)", u);
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
    // [WOLFTREK v11] the title is the station's name (design.html#names "the Station window's title"); its owner renames it here
    // (an inline field in the title's place: Enter saves, Escape cancels; the region's answer stays in the window)
    const std::string sname = sf.stationName();
    const F32 tbh = llmax(52.f * u, 44.f), rt = t - 14.f * u, rb = rt - tbh;
    if (mEdit == EDIT_STATION) drawEditRow(l + pad, rb, r - pad, rt, u);
    else
    {
        const bool owner = sf.canRenameStation();
        const F32 rw = 230.f * u, room = (r - pad) - (l + pad) - (owner ? rw + 12.f * u : 0.f);
        T(l + pad, (rb + rt) * 0.5f, fit(sname.empty() ? std::string("STATION") : sname, 30.f * u, true, room), 30.f * u, C_AMBER, true, LLFontGL::LEFT);
        if (owner)
            button(r - pad - rw, rb, r - pad, rt, H_STATION_RENAME, "RENAME STATION", false,
                   "Give this station a new name (3 to 32 letters; it must not be in use anywhere on the grid)", u);
    }
    {
        // under the title: why a typed name cannot be saved (said next to the field, never only a passing note), else where you are
        const F32 ly = rb - 20.f * u;
        if (mEdit == EDIT_STATION && !mEditError.empty())
            T(l + pad, ly, fit("Not saved: " + mEditError, 17.f * u, true, w - 2.f * pad), 17.f * u, C_RED, true, LLFontGL::LEFT);
        else
        {
            std::string where = sname.empty() ? std::string("Docked") : "Docked at " + sname;
            if (st.mPad > 0) where += llformat(", pad %d", st.mPad);
            where += sname.empty() && st.mPad <= 0 ? ". Your ship is repaired: shield, armour and hull are full." : ". Your ship is repaired.";
            T(l + pad, ly, fit(where, 17.f * u, false, w - 2.f * pad), 17.f * u, C_GREEN, false, LLFontGL::LEFT);
        }
    }
    // the market: ore, in store, price, value (numbers right-aligned so they stack: ui-ux-design typography)
    const F32 c1 = l + pad, c2 = l + w * 0.55f, c3 = l + w * 0.72f, c4 = r - pad;
    F32 y = rb - 62.f * u;
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
    // [STARBASES 2026-11-24] design.html#bases: the Starbase window opens "in the Station window" too (on the Starmarks line, right)
    {
        const F32 sbh = llmax(52.f * u, 44.f);
        button(c4 - 200.f * u, y - sbh * 0.5f, c4, y + sbh * 0.5f, H_BASE_OPEN, "STARBASE", false, "Your starbase: found it, build on it, go home", u);
    }
    // the actions: SELL (only with something to sell), CLOSE apart from LAUNCH (the primary, bottom right)
    const F32 bh = 60.f * u, bb = b + pad;
    button(l + pad, bb, l + pad + 260.f * u, bb + bh, H_SELL, total > 0 ? "SELL ALL ORE (" + thousands(total) + ")" : "SELL ALL ORE",
           false, total > 0 ? "Sell everything in store at these prices" : "You have no ore in store", u);
    button(r - pad - 200.f * u, bb, r - pad, bb + bh, H_STATION_LAUNCH, "LAUNCH", false, "Leave the station in your Cub", u, true);
    button(r - pad - 200.f * u - 160.f * u, bb, r - pad - 220.f * u, bb + bh, H_STATION_CLOSE, "CLOSE", false,
           "Close this window (you stay docked)", u);
    // [WOLFTREK v11] the region's answer to a rename (k=msg), kept above the buttons until the next rename
    if (!sf.stationAnswer().empty())
    {
        const F32 px = 16.f * u, ab = bb + bh + 12.f * u, at = ab + px * 2.2f;
        rectF(l + pad, ab, r - pad, at, LLColor4(0.18f, 0.12f, 0.02f, 0.92f));
        frameW(l + pad, ab, r - pad, at, llmax(1.f, 1.5f * u), C_AMBER);
        T(l + pad + 12.f * u, (ab + at) * 0.5f, fit(sf.stationAnswer(), px, true, w - 2.f * pad - 24.f * u), px, C_AMBER, true, LLFontGL::LEFT);
    }
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
// [STARBASES 2026-11-24] the Starbase window
// Source: design.html#bases "Building it: the Starbase window" (Paul approved 24 Nov ~17:00Z): "One window, in the cockpit (a Base
// button), in the Station window, and under Wolf World > Starfold > My starbase ... It is a game screen, not a settings dialog: our
// own starbase logo, a lit picture of your base that turns slowly, the building drones, and a short celebration when a module is
// finished"; "Glowing outlines (ghosts) appear on your base wherever that module fits. Press Next spot until you like it, then Build
// it here"; "Plain refusals ... Never an error code"; "Every button is a large target with words; nothing is timed".
// Who and what (ui-ux-design): a resident of 35 to 65, perhaps with unsteady hands, wants a home in space: found it, add a ring and
// quarters, let friends dock, go home. Flow: MY BASE (Found / Find me a spot) -> ADD A MODULE (card -> ghosts -> Next spot / Turn it
// round -> Build it here) -> MY BASE (ready in about N minutes; Go home). The STOREROOM and YOUR OWN BUILDS tabs are one list each.
// Every target is at least 44 px; the region's answer stays on the window until the next order (never only a passing note).
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: wolf-space BaseRules.cs:43-55 (Modules: each kind's Title) and design.html#bases "Build your own modules" ("decor":
    // decoration). The region's kit= titles win; these name a kind the region did not list (an own build's kind).
    std::string kindTitle(const std::string& kind)
    {
        if (const WolfStarbase::Kit* k = WolfStarbase::instance().kit(kind)) return k->mTitle;
        static const std::pair<const char*, const char*> NAMES[] = {
            { "core", "Core" }, { "dock", "Docking ring" }, { "quarters", "Living quarters" }, { "hangar", "Hangar" },
            { "refinery", "Refinery" }, { "market", "Market stall" }, { "shield", "Shield generator" }, { "turret", "Turret" },
            { "decor", "Decoration" } };
        for (const auto& n : NAMES) if (kind == n.first) return n.second;
        return kind;
    }

    // the kinds' colours in the picture and the list (always with words beside them: never colour alone)
    LLColor4 kindColour(const std::string& kind, bool own)
    {
        if (own) return C_VIOLET;
        if (kind == "core") return C_AMBER;
        if (kind == "dock") return C_CYAN;
        if (kind == "quarters") return C_GREEN;
        return C_TEXT;
    }

    // words wrapped at whole words to maxw, from y downwards (as drawGuide); the y under the last line
    F32 para(F32 x, F32 y, const std::string& text, F32 px, F32 maxw, const LLColor4& c, bool bold = false, S32 maxLines = 5)
    {
        std::string rest = text, cur;
        S32 lines = 0;
        while (!rest.empty() && lines < maxLines)
        {
            const size_t sp = rest.find(' ');
            const std::string word = rest.substr(0, sp);
            rest = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            const std::string trial = cur.empty() ? word : cur + " " + word;
            if (textW(trial, px, bold) > maxw && !cur.empty())
            {
                T(x, y, cur, px, c, bold, LLFontGL::LEFT);
                y -= px * 1.5f;
                ++lines;
                cur = word;
            }
            else cur = trial;
        }
        if (!cur.empty() && lines < maxLines) { T(x, y, fit(cur, px, bold, maxw), px, c, bold, LLFontGL::LEFT); y -= px * 1.5f; }
        return y;
    }

    // "about N minutes" (design: "Ready in about 20 minutes. You don't need to wait here.")
    std::string aboutMinutes(F64 seconds)
    {
        const S32 m = (S32)ceil(llmax(seconds, 0.0) / 60.0);
        return m <= 1 ? std::string("about a minute") : llformat("about %d minutes", m);
    }

    F32 targetH(F32 u) { return llmax(52.f * u, 44.f); }   // design: "Every target is at least 44 px"
}

bool WolfStarfoldDeck::projectAgent(const LLVector3& p, F32& x, F32& y) const
{
    // Source: drawHud's projection (itself WolfStarfold::renderLens): the camera basis, LLCamera's vertical field of view and
    // aspect, onto the world view's rectangle in this view's coordinates
    if (!gViewerWindow) return false;
    LLRect world = gViewerWindow->getWorldViewRectScaled();
    const LLRect mine = calcScreenRect();
    world.translate(-mine.mLeft, -mine.mBottom);
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 tan_half = tanf(cam->getView() * 0.5f), aspect = cam->getAspect();
    const LLVector3 d = p - cam->getOrigin();
    const F32 f = d * cam->getAtAxis();
    const F32 lx = -(d * cam->getLeftAxis()), uy = d * cam->getUpAxis();
    if (f <= 0.5f) { x = lx; y = uy; return false; }
    x = (F32)world.mLeft + ((lx / f) / (tan_half * aspect) * 0.5f + 0.5f) * (F32)world.getWidth();
    y = (F32)world.mBottom + ((uy / f) / tan_half * 0.5f + 0.5f) * (F32)world.getHeight();
    return true;
}

void WolfStarfoldDeck::drawGhosts(F32 u)
{
    // "Ghosts cost nothing. They are drawn by the viewer ... see-through and glowing. Nothing is rezzed until you press Build."
    // Each ghost is its module's cells (BasePlacement.Footprint) on the base's 10 m frame, turned with the base's quarter-turn yaw
    // (PartPose); the chosen one amber, the others cyan. A click on one chooses it ("Clicking a ghost in the world chooses it too").
    WolfStarbase& sb = WolfStarbase::instance();
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || !sb.hasBase()) return;
    const F32 C = WolfStarbase::CELL_M;
    auto agentOf = [&](F32 x, F32 y, F32 z) { return region->getPosAgentFromRegion(sb.toRegion(LLVector3(x, y, z))); };
    const F32 lw = llmax(1.5f, 2.f * u);
    // the frame itself, faint: where a base may grow (BaseRules.cs FRAME_MIN_XY..FRAME_MAX_XY, FRAME_MIN_Z..FRAME_MAX_Z: -3..2
    // and -2..1 cells, 60 x 60 x 40 m round the base's centre)
    auto box = [&](F32 x0, F32 y0, F32 z0, F32 x1, F32 y1, F32 z1, const LLColor4& edge, const LLColor4& fill)
    {
        LLVector3 c[8];
        F32 sx[8], sy[8];
        bool ok[8];
        for (S32 i = 0; i < 8; ++i)
        {
            c[i] = agentOf((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0);
            ok[i] = projectAgent(c[i], sx[i], sy[i]);
        }
        static const S32 E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        static const S32 F[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
        if (fill.mV[VALPHA] > 0.f)
            for (const auto& f : F)
                if (ok[f[0]] && ok[f[1]] && ok[f[2]] && ok[f[3]])
                    quadF(sx[f[0]], sy[f[0]], sx[f[1]], sy[f[1]], sx[f[2]], sy[f[2]], sx[f[3]], sy[f[3]], fill);
        for (const auto& e : E)
            if (ok[e[0]] && ok[e[1]]) lineW(sx[e[0]], sy[e[0]], sx[e[1]], sy[e[1]], lw, edge);
    };
    box(-3.f * C, -3.f * C, -2.f * C, 3.f * C, 3.f * C, 2.f * C, LLColor4(0.38f, 0.90f, 1.f, 0.22f), LLColor4(0.f, 0.f, 0.f, 0.f));
    if (sb.ghostFor().empty() || !sb.ghostsArrived()) return;
    S32 size[3];
    sb.ghostSize(size);
    const WolfStarbase::Ghost* pick = sb.chosen();
    const F32 pulse = 0.75f + 0.25f * sinf((F32)LLFrameTimer::getElapsedSeconds() * 3.f);
    const std::vector<WolfStarbase::Ghost>& gs = sb.ghosts();
    // the others first, the chosen one last (on top)
    for (S32 pass = 0; pass < 2; ++pass)
    {
        for (size_t i = 0; i < gs.size(); ++i)
        {
            const bool chosen = &gs[i] == pick;
            if (chosen != (pass == 1)) continue;
            const LLColor4 edge = chosen ? LLColor4(1.f, 0.74f, 0.28f, pulse) : LLColor4(0.38f, 0.90f, 1.f, 0.65f);
            const LLColor4 fill = chosen ? LLColor4(1.f, 0.74f, 0.28f, 0.20f) : LLColor4(0.38f, 0.90f, 1.f, 0.07f);
            LLVector3 mid;
            const std::vector<std::vector<S32>> cells = WolfStarbase::footprint(size, gs[i].mCell, gs[i].mRot);
            for (const std::vector<S32>& c : cells)
            {
                // a cell round the camera would fill the whole view (rig 24 Nov 23:07Z, the ship inside the frame): its fill fades
                // as the eye comes within 40 m of its centre; the edges stay
                const F32 near_m = (agentOf(((F32)c[0] + 0.5f) * C, ((F32)c[1] + 0.5f) * C, ((F32)c[2] + 0.5f) * C) - LLViewerCamera::getInstance()->getOrigin()).length();
                LLColor4 cellFill = fill;
                cellFill.mV[VALPHA] *= llclamp((near_m - 8.f) / 32.f, 0.1f, 1.f);
                box((F32)c[0] * C, (F32)c[1] * C, (F32)c[2] * C, (F32)(c[0] + 1) * C, (F32)(c[1] + 1) * C, (F32)(c[2] + 1) * C, edge, cellFill);
                mid += LLVector3(((F32)c[0] + 0.5f) * C, ((F32)c[1] + 0.5f) * C, ((F32)c[2] + 0.5f) * C);
            }
            if (cells.empty()) continue;
            mid /= (F32)cells.size();
            F32 x, y;
            if (!projectAgent(agentOf(mid.mV[VX], mid.mV[VY], mid.mV[VZ]), x, y)) continue;
            const F32 hit = llmax(22.f * u, 22.f);
            addHit(x - hit, y - hit, x + hit, y + hit, (EHit)(H_BASE_GHOST_0 + (S32)i), chosen ? "The chosen spot" : "Click to choose this spot");
            if (!chosen) continue;
            T(x, y, "HERE", 16.f * u, C_AMBER, true);
            // the docking lane in front of a module that needs one (kit= lane metres; BasePlacement.LaneCells: from its front face)
            const WolfStarbase::Kit* k = sb.kit(sb.ghostKind());
            if (k && k->mLane > 0.f)
            {
                S32 fr[3];
                WolfStarbase::front(gs[i].mRot, fr);
                const LLVector3 dir((F32)fr[0], (F32)fr[1], (F32)fr[2]);
                const LLVector3 a = mid + dir * (C * 0.5f), bpt = a + dir * k->mLane;
                F32 ax, ay, bx, by;
                if (projectAgent(agentOf(a.mV[VX], a.mV[VY], a.mV[VZ]), ax, ay) && projectAgent(agentOf(bpt.mV[VX], bpt.mV[VY], bpt.mV[VZ]), bx, by))
                {
                    for (S32 d = 0; d < 8; ++d)   // dashed: a lane is kept clear, nothing is built there
                    {
                        const F32 p0 = (F32)d / 8.f, p1 = p0 + 0.6f / 8.f;
                        lineW(ax + (bx - ax) * p0, ay + (by - ay) * p0, ax + (bx - ax) * p1, ay + (by - ay) * p1, lw, C_AMBER);
                    }
                    T(bx, by + 14.f * u, llformat("%d m CLEAR LANE", (S32)llround(k->mLane)), 13.f * u, C_AMBER, true);
                }
            }
        }
    }
}

void WolfStarfoldDeck::drawBasePicture(F32 cx, F32 cy, F32 size, F32 u)
{
    // "a lit picture of your base that turns slowly, the building drones": the parts' cells seen from above and to the side,
    // turning once a minute or so, lit from the upper left; a part being built pulses and has its drones circling it
    WolfStarbase& sb = WolfStarbase::instance();
    const F32 t = (F32)LLFrameTimer::getElapsedSeconds();
    const F32 a = t * 0.12f, ca = cosf(a), sa = sinf(a);
    const F32 k = size / 80.f;   // pixels a metre: the 60 m frame with room round it
    ellipseF(cx, cy - size * 0.30f, size * 0.46f, size * 0.12f, LLColor4(0.04f, 0.16f, 0.27f, 0.55f));
    ellipseLine(cx, cy - size * 0.30f, size * 0.46f, size * 0.12f, llmax(1.f, 1.5f * u), C_CYAN * 0.5f);
    auto P = [&](F32 x, F32 y, F32 z, F32& px, F32& py)
    {
        px = cx + (x * ca - y * sa) * k * 0.9f;
        py = cy + (x * sa + y * ca) * k * 0.32f + z * k * 0.8f;
    };
    struct Cube { F32 depth; S32 c[3]; LLColor4 col; bool building; };
    std::vector<Cube> cubes;
    const F32 C = WolfStarbase::CELL_M;
    for (const WolfStarbase::Part& p : sb.parts())
    {
        if (p.mState == "stored" || p.mState == "off") continue;
        const bool building = p.building();
        LLColor4 col = kindColour(p.mKind, p.mOwn);
        for (const std::vector<S32>& c : WolfStarbase::footprint(p.mSize, p.mCell, p.mRot))
        {
            const F32 x = ((F32)c[0] + 0.5f) * C, y = ((F32)c[1] + 0.5f) * C;
            cubes.push_back({ x * sa + y * ca, { c[0], c[1], c[2] }, col, building });
        }
    }
    // far first (painter's order): depth grows away from the eye
    std::sort(cubes.begin(), cubes.end(), [](const Cube& l, const Cube& r) { return l.depth > r.depth; });
    const F32 pulse = 0.55f + 0.35f * sinf(t * 2.5f);
    for (const Cube& q : cubes)
    {
        const F32 x0 = (F32)q.c[0] * C, y0 = (F32)q.c[1] * C, z0 = (F32)q.c[2] * C, x1 = x0 + C, y1 = y0 + C, z1 = z0 + C;
        F32 px[8], py[8];
        for (S32 i = 0; i < 8; ++i) P((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0, px[i], py[i]);
        const F32 alpha = q.building ? pulse : 0.95f;
        const LLColor4 side(q.col.mV[0] * 0.45f, q.col.mV[1] * 0.45f, q.col.mV[2] * 0.45f, alpha);
        const LLColor4 side2(q.col.mV[0] * 0.30f, q.col.mV[1] * 0.30f, q.col.mV[2] * 0.30f, alpha);
        const LLColor4 top(q.col.mV[0] * 0.85f, q.col.mV[1] * 0.85f, q.col.mV[2] * 0.85f, alpha);
        static const S32 SIDES[4][4] = { { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
        S32 n = 0;
        for (const auto& f : SIDES)
            quadF(px[f[0]], py[f[0]], px[f[1]], py[f[1]], px[f[2]], py[f[2]], px[f[3]], py[f[3]], (n++ & 1) ? side : side2);
        quadF(px[4], py[4], px[5], py[5], px[7], py[7], px[6], py[6], top);
        for (S32 e : { 4, 5, 7, 6 })
        {
            const S32 nx = e == 4 ? 5 : e == 5 ? 7 : e == 7 ? 6 : 4;
            lineW(px[e], py[e], px[nx], py[nx], llmax(1.f, 1.2f * u), LLColor4(1.f, 1.f, 1.f, 0.35f * alpha));
        }
        if (q.building)
        {
            // the building drones: three small lights circling the cell
            for (S32 d = 0; d < 3; ++d)
            {
                const F32 da = t * 1.7f + (F32)d * 2.0944f;
                F32 dx, dy;
                P(x0 + C * 0.5f + cosf(da) * C * 0.8f, y0 + C * 0.5f + sinf(da) * C * 0.8f, z1 + 2.f, dx, dy);
                circleF(dx, dy, 3.f * u, C_AMBER, 10);
            }
        }
    }
    if (cubes.empty()) T(cx, cy, "Your parts are in the storeroom", 14.f * u, C_DIM, false);
}

void WolfStarfoldDeck::drawBase(F32 above, F32 W, F32 H, F32 u)
{
    WolfStarbase& sb = WolfStarbase::instance();
    mRowIds.assign(12, std::string());
    mCardIds.clear();
    drawGhosts(u);
    const F32 w = llmin(W - 32.f, 860.f * u), h = llmin(H - above - 32.f, 660.f * u);
    const F32 l = W - w - 24.f * u, r = l + w;   // to the right, so the base and its ghosts stay in view on the left
    const F32 b = above + (H - above - h) * 0.5f, t = b + h;
    rectG(l, b, r, t, SOLID.hi, SOLID.lo);
    frameW(l, b, r, t, 3.f * u, C_EDGE);
    addHit(l, b, r, t, H_NONE, "");   // the window takes its clicks: none fall through to the world
    const F32 pad = 24.f * u, bh = targetH(u);
    // the starbase logo: the wolf-ear mark in a ring (our own; design: "our own starbase logo")
    ring(l + pad + 22.f * u, t - 40.f * u, 22.f * u, 2.5f * u, C_AMBER);
    wolfMark(l + pad + 2.f * u, t - 58.f * u, 1.f * u, C_AMBER);
    T(l + pad + 56.f * u, t - 30.f * u, "STARBASE", 28.f * u, C_AMBER, true, LLFontGL::LEFT);
    T(l + pad + 56.f * u, t - 58.f * u, sb.hasBase() ? "Your own home in space." : "Found your own home in space.", 15.f * u, C_DIM, false, LLFontGL::LEFT);
    button(r - pad - bh, t - pad - bh + 8.f * u, r - pad, t - pad + 8.f * u, H_BASE_CLOSE, "X", false, "Close the Starbase window", u);
    // the tabs
    S32 stored = 0;
    for (const WolfStarbase::Part& p : sb.parts()) if (p.mState == "stored" || p.mState == "off") ++stored;
    const char* labels[4] = { "MY BASE", "ADD A MODULE", "STOREROOM", "YOUR OWN BUILDS" };
    const char* tips[4] = { "Your starbase: found it, go home, who may dock", "Buy a module and choose where it goes",
                            "Parts waiting to go back on your base", "Things you built yourself with starbase in their description" };
    const F32 tabT = t - 84.f * u, tabB = tabT - bh, gap = 8.f * u, tw = (w - 2.f * pad - 3.f * gap) / 4.f;
    for (S32 i = 0; i < 4; ++i)
    {
        const F32 x0 = l + pad + (F32)i * (tw + gap);
        std::string lab = labels[i];
        if (i == 2 && stored > 0) lab += llformat(" (%d)", stored);
        button(x0, tabB, x0 + tw, tabT, (EHit)(H_BASE_TAB_0 + i), lab, (S32)sb.tab() == i, tips[i], u);
    }
    // the bottom row and, over it, the region's answer (kept until the next order: never only a passing note)
    const F32 bb = b + pad;
    F32 contentB = bb + bh + 16.f * u;
    const std::string done = sb.justFinished();
    if (!done.empty())
    {
        // the celebration (design: "a short celebration when a module is finished")
        const F32 cy = contentB + 22.f * u;
        rectF(l + pad, cy - 20.f * u, r - pad, cy + 20.f * u, LLColor4(0.05f, 0.25f, 0.12f, 0.9f));
        const F32 tt = (F32)LLFrameTimer::getElapsedSeconds();
        for (S32 s = 0; s < 6; ++s)
            circleF(l + pad + 30.f * u + (F32)s * 18.f * u, cy + 8.f * u * sinf(tt * 4.f + (F32)s), 3.f * u, C_AMBER, 8);
        // [STARBASES 2026-11-25] in a sentence the kit's title is lower case and "living quarters" is plural (the rig showed "Your
        // Living quarters is ready!"); WolfStorm's window says the same (wolf_starbase_window.js)
        std::string said = done;
        if (!said.empty()) said[0] = (char)tolower((unsigned char)said[0]);
        const bool plural = said.size() >= 8 && said.compare(said.size() - 8, 8, "quarters") == 0;
        T((l + r) * 0.5f, cy, "Your " + said + (plural ? " are ready!" : " is ready!"), 20.f * u, C_GREEN, true);
        contentB = cy + 28.f * u;
    }
    // a press that waits for a second one says plainly what it will do (design: "a plain 'Are you sure?' that names what will go")
    std::string answer = sb.answer();
    if (mArmed == "pack") answer = "Packing up returns every part to you and keeps everything you bought. Press PACK UP again to pack it up.";
    else if (mArmed == "scrap") answer = "Scrapping removes your starbase and all its parts for good, and returns half of what they cost. Press SCRAP again to be sure.";
    else if (mArmed.rfind("down:", 0) == 0) answer = "Taking it down returns all its cost within 10 minutes of buying it, or three quarters after. Press again to take it down.";
    if (!answer.empty())
    {
        const F32 px = 16.f * u, top = contentB + px * 3.2f;
        rectF(l + pad, contentB, r - pad, top, LLColor4(0.18f, 0.12f, 0.02f, 0.92f));
        frameW(l + pad, contentB, r - pad, top, llmax(1.f, 1.5f * u), C_AMBER);
        para(l + pad + 12.f * u, top - px * 1.0f, answer, px, w - 2.f * pad - 24.f * u, C_AMBER, true, 2);
        contentB = top + 8.f * u;
    }
    const F32 ct = tabB - 16.f * u;
    switch (sb.tab())
    {
        case WolfStarbase::TAB_BASE: drawBaseHome(l + pad, contentB, r - pad, ct, u); break;
        case WolfStarbase::TAB_ADD: drawBaseAdd(l + pad, contentB, r - pad, ct, u); break;
        case WolfStarbase::TAB_STORE: drawBaseStore(l + pad, contentB, r - pad, ct, u); break;
        default: drawBaseOwn(l + pad, contentB, r - pad, ct, u); break;
    }
    // the bottom row: choosing a spot, or the tab's own orders
    struct Btn { EHit id; std::string label; std::string tip; bool lit; bool primary; F32 wt; };
    std::vector<Btn> row;
    if (!sb.ghostFor().empty())
    {
        const std::string kind = sb.ghostKind();
        const WolfStarbase::Kit* k = sb.kit(kind);
        std::string act = "BUILD IT HERE";
        if (sb.ghostFor().rfind("own:", 0) == 0) act = "ATTACH IT HERE";
        else if (sb.ghostFor().rfind("part:", 0) == 0) act = "PLACE IT HERE";
        else if (k) act += " - " + thousands(k->mPrice);
        row.push_back({ H_BASE_PREV, "PREVIOUS", "The spot before", false, false, 1.f });
        row.push_back({ H_BASE_NEXT, "NEXT SPOT", "Show the next place it fits", false, false, 1.f });
        row.push_back({ H_BASE_TURN, "TURN IT ROUND", "Turn it to face another way in this spot", false, false, 1.1f });
        row.push_back({ H_BASE_BUILD, act, "Use the amber spot", false, true, 1.5f });
        row.push_back({ H_BASE_CANCEL, "CANCEL", "Stop choosing a spot", false, false, 0.9f });
    }
    else
    {
        switch (sb.tab())
        {
            case WolfStarbase::TAB_BASE:
                if (!sb.known()) break;
                if (sb.hasBase())
                {
                    row.push_back({ H_BASE_HOME, "GO HOME", "Take me into my quarters (or the core)", false, true, 1.f });
                    if (WolfStarfold::instance().piloting())
                    {
                        row.push_back({ H_BASE_FLY, "FLY THERE", "Fly my ship to my starbase", false, false, 1.f });
                        row.push_back({ H_BASE_DOCKAT, "DOCK", "Dock at my starbase (fly close first)", false, false, 0.8f });
                    }
                    row.push_back({ H_BASE_PACK, mArmed == "pack" ? "PRESS AGAIN TO PACK" : "PACK UP", "Pack my starbase away, with everything in it", mArmed == "pack", false, 1.2f });
                    row.push_back({ H_BASE_SCRAP, mArmed == "scrap" ? "PRESS AGAIN TO SCRAP" : "SCRAP", "Remove my starbase for good (half its cost back)", mArmed == "scrap", false, 1.2f });
                }
                else
                {
                    row.push_back({ H_BASE_FOUND, "FOUND MY STARBASE HERE", "Found it where my ship (or I) am now", false, true, 1.7f });
                    row.push_back({ H_BASE_SPOT, "FIND ME A SPOT", "Find the nearest good spot (and fly me there)", false, false, 1.2f });
                    row.push_back({ H_BASE_RESTORE, "BRING MY BASE BACK", "Put my packed-up starbase here", false, false, 1.4f });
                }
                break;
            case WolfStarbase::TAB_STORE:
                if (sb.known() && !sb.hasBase())
                    row.push_back({ H_BASE_RESTORE, "BRING MY BASE BACK", "Put my packed-up starbase here", false, true, 1.f });
                break;
            case WolfStarbase::TAB_OWN:
                row.push_back({ H_BASE_OWN_CHECK, "CHECK AGAIN", "Look again for my builds nearby", false, false, 1.f });
                break;
            default: break;
        }
    }
    if (!row.empty())
    {
        F32 total = 0.f;
        for (const Btn& bt : row) total += bt.wt;
        const F32 avail = w - 2.f * pad - (F32)(row.size() - 1) * gap;
        F32 x = l + pad;
        for (const Btn& bt : row)
        {
            const F32 bw = avail * bt.wt / total;
            button(x, bb, x + bw, bb + bh, bt.id, bt.label, bt.lit, bt.tip, u, bt.primary);
            x += bw + gap;
        }
    }
}

void WolfStarfoldDeck::drawBaseHome(F32 l, F32 b, F32 r, F32 t, F32 u)
{
    WolfStarbase& sb = WolfStarbase::instance();
    const F32 w = r - l;
    if (!sb.known())
    {
        T(l, t - 20.f * u, "Asking the starbase office...", 18.f * u, C_DIM, false, LLFontGL::LEFT);
        return;
    }
    if (!sb.hasBase())
    {
        F32 y = t - 16.f * u;
        y = para(l, y, "You have no starbase in this region yet.", 21.f * u, w, C_TEXT, true);
        y = para(l, y - 6.f * u, sb.regionOpen() ? llformat("Starbases are open here: %d of %d bases so far.", sb.regionCount(), sb.regionCap())
                                                 : std::string("This region's owner has not opened it to player starbases."),
                 17.f * u, w, sb.regionOpen() ? C_GREEN : C_AMBER);
        const WolfStarbase::Kit* core = sb.kit("core");
        const std::string cost = core ? llformat("Founding costs %s %s: the core, built in about %d minutes (%d prims).", thousands(core->mPrice).c_str(),
                                                 WolfStarfold::currency(), core->mMinutes, core->mPrims)
                                      : llformat("Founding costs %s %s: the core.", thousands(sb.corePrice()).c_str(), WolfStarfold::currency());
        y = para(l, y - 6.f * u, cost, 17.f * u, w, C_TEXT);
        y = para(l, y - 10.f * u, "Fly to an open part of space, well clear of the station, the belt and the wormhole, then press Found my "
                 "starbase here. If the spot is not allowed, this window says what is in the way. Find me a spot flies you to the nearest good one.",
                 16.f * u, w, C_DIM);
        para(l, y - 6.f * u, "Packed your base up before? Bring my base back puts it here, with everything in it.", 16.f * u, w, C_DIM);
        return;
    }
    // the picture on the left, the facts and who may dock on the right (the picture no taller than that column, so the parts
    // keep their room on a short window: rig 24 Nov 23:05Z, 1080 lines, the answer lay over the first part)
    const F32 pic = llmin(llmin(180.f * u, (t - b) * 0.5f), 96.f * u + targetH(u));
    drawBasePicture(l + pic * 0.5f, t - pic * 0.55f, pic, u);
    const F32 x0 = l + pic + 20.f * u, cw = r - x0;
    // [WOLFTREK v11] the base's own name (k=base name=) with RENAME (its owner's: this tab is always the asker's own base,
    // SpaceBases.cs SendBase); renaming puts the field in the name's place, Enter saves, Escape cancels
    const F32 nbh = targetH(u);
    F32 y = t - 4.f * u;
    if (mEdit == EDIT_BASE) drawEditRow(x0, y - nbh, r, y, u);
    else
    {
        std::string state = sb.baseName().empty() ? std::string("Your starbase") : sb.baseName();
        if (sb.baseState() == "packing") state += " is being packed away";
        const F32 rw = 150.f * u;
        T(x0, y - nbh * 0.5f, fit(state, 21.f * u, true, cw - rw - 12.f * u), 21.f * u, C_TEXT, true, LLFontGL::LEFT);
        button(r - rw, y - nbh, r, y, H_BASE_RENAME, "RENAME", false, "Give your starbase a new name (3 to 32 letters)", u);
    }
    y -= nbh + 18.f * u;
    if (mEdit == EDIT_BASE && !mEditError.empty())
    {
        // why the typed name cannot be saved, next to the field (in the facts line's place while it shows)
        T(x0, y, fit("Not saved: " + mEditError, 16.f * u, true, cw), 16.f * u, C_RED, true, LLFontGL::LEFT);
        y -= 28.f * u;
    }
    else
    {
        // one line of facts: how far, the prims, the Starmarks
        std::string facts;
        if (LLViewerRegion* region = gAgent.getRegion())
            facts = distance((region->getPosRegionFromAgent(gAgent.getPositionAgent()) - sb.basePos()).length()) + " from you.  ";
        facts += llformat("Prims %d of %d.", sb.primsUsed(), sb.primsMax());
        if (sb.haveStarmarks()) facts += "  " + thousands(sb.starmarks()) + " " + WolfStarfold::currency() + ".";
        T(x0, y, fit(facts, 16.f * u, false, cw), 16.f * u, C_TEXT, false, LLFontGL::LEFT);
        y -= 28.f * u;
    }
    T(x0, y, "Who may dock at your docking ring:", 16.f * u, C_DIM, true, LLFontGL::LEFT);
    y -= 12.f * u;
    {
        const F32 bh = targetH(u), gap = 6.f * u, bw = (cw - 3.f * gap) / 4.f;
        const struct { EHit id; const char* label; const char* who; const char* tip; } D[4] = {
            { H_BASE_DOCK_ALL, "EVERYONE", "all", "Any pilot may dock" }, { H_BASE_DOCK_FRIENDS, "FRIENDS", "friends", "Only my friends may dock" },
            { H_BASE_DOCK_GROUP, "MY GROUP", "group", "Only members of my base's group may dock" }, { H_BASE_DOCK_ME, "ONLY ME", "me", "Nobody else may dock" } };
        for (S32 i = 0; i < 4; ++i)
            button(x0 + (F32)i * (bw + gap), y - bh, x0 + (F32)i * (bw + gap) + bw, y, D[i].id, D[i].label, sb.dock() == D[i].who, D[i].tip, u);
        y -= bh + 8.f * u;
    }
    // the parts on the base (the storeroom has its own tab)
    std::vector<const WolfStarbase::Part*> rows;
    for (const WolfStarbase::Part& p : sb.parts()) if (p.mState != "stored") rows.push_back(&p);
    F32 ly = llmin(y, t - pic - 8.f * u);
    const F32 rowH = targetH(u) + 12.f * u;
    // rows that fit; when they do not all fit, the page line (a button's height) comes off the room first
    S32 per = (S32)((ly - b) / rowH);
    if (per < (S32)rows.size()) per = (S32)((ly - b - targetH(u) - 6.f * u) / rowH);
    if (per < 1)
    {
        // no room for a row (a very short window): the parts in one line of words instead, never drawn over the answer
        std::string sum;
        for (const WolfStarbase::Part* p : rows) sum += (sum.empty() ? "" : ", ") + kindTitle(p->mKind) + (p->building() ? " (being built)" : "");
        if (!sum.empty() && ly - b > 16.f * u) T(l, ly - 10.f * u, fit("Parts: " + sum, 14.f * u, false, r - l), 14.f * u, C_DIM, false, LLFontGL::LEFT);
        return;
    }
    const S32 pages = llmax(1, ((S32)rows.size() + per - 1) / per);
    mBasePage = llclamp(mBasePage, 0, pages - 1);
    if (pages > 1)
    {
        const F32 bh = targetH(u);
        T(l, ly - bh * 0.5f, llformat("Parts, page %d of %d", mBasePage + 1, pages), 15.f * u, C_DIM, true, LLFontGL::LEFT);
        button(r - 2.f * bh - 6.f * u, ly - bh, r - bh - 6.f * u, ly, H_BASE_PAGE_PREV, "<", false, "The page before", u);
        button(r - bh, ly - bh, r, ly, H_BASE_PAGE_NEXT, ">", false, "The next page", u);
        ly -= bh + 6.f * u;
    }
    const F32 tnow = (F32)LLFrameTimer::getElapsedSeconds();
    for (S32 i = 0; i < per && mBasePage * per + i < (S32)rows.size(); ++i)
    {
        const WolfStarbase::Part& p = *rows[(size_t)(mBasePage * per + i)];
        const F32 top = ly - (F32)i * rowH, mid = top - rowH * 0.5f;
        rectF(l, top - rowH + 4.f * u, r, top, LLColor4(0.03f, 0.07f, 0.16f, 0.75f));
        rectF(l, top - rowH + 4.f * u, l + 5.f * u, top, kindColour(p.mKind, p.mOwn));
        std::string title = kindTitle(p.mKind) + (p.mOwn ? " (your own build)" : "");
        std::string status;
        LLColor4 sc = C_GREEN;
        const bool gameplay = p.mKind != "decor";
        if (p.building())
        {
            status = "Being built: ready in " + aboutMinutes(p.mReadyAt - LLFrameTimer::getElapsedSeconds()) + ". You don't need to wait here.";
            sc = C_AMBER;
            for (S32 d = 0; d < 3; ++d)   // the drones at work
                circleF(l + 22.f * u + cosf(tnow * 2.f + (F32)d * 2.1f) * 9.f * u, mid + sinf(tnow * 2.f + (F32)d * 2.1f) * 9.f * u, 2.5f * u, C_AMBER, 8);
        }
        else if (p.mState == "off") { status = "Moved off your starbase, so its work has paused."; sc = C_AMBER; }
        else if (p.mState == "paused") { status = "Paused: the region's message says why."; sc = C_AMBER; }
        else if (!gameplay) status = "Decoration";
        else if (!p.mPowered) { status = "Decoration until you buy its core."; sc = C_AMBER; }
        else status = "Working";
        const F32 tx = l + 40.f * u;
        // the buttons on the right: Put it back (off) or Buy its core (an own shell with none), and Take down (not the core)
        const F32 bh = targetH(u), bw = 170.f * u;
        F32 bx = r - 8.f * u;
        const S32 slot = i;
        mRowIds[(size_t)slot] = p.mId;
        if (p.mKind != "core")
        {
            const bool armed = mArmed == "down:" + p.mId;
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWB_0 + slot), armed ? "PRESS AGAIN" : "TAKE DOWN", armed,
                   "Take it down (all its cost back within 10 minutes of buying, three quarters after)", u);
            bx -= bw + 8.f * u;
        }
        if (p.mState == "off")
        {
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWA_0 + slot), "PUT IT BACK", false, "Snap it back to its old spot", u);
            bx -= bw + 8.f * u;
        }
        else if (p.mOwn && gameplay && !p.mPowered)
        {
            const WolfStarbase::Kit* k = sb.kit(p.mKind);
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWA_0 + slot), k ? "BUY CORE " + thousands(k->mPrice) : std::string("BUY ITS CORE"),
                   false, "Buy this kind's core so your own build does its work", u);
            bx -= bw + 8.f * u;
            // own quarters need a seat first (design.html#bases: "choose a prim and press Make this a seat")
            if (p.mKind == "quarters")
            {
                button(bx - bw * 0.8f, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWC_0 + slot), "MAKE SEAT", false,
                       "Make the prim you have selected (Edit, Edit linked) the seat of your quarters", u);
                bx -= bw * 0.8f + 8.f * u;
            }
        }
        const F32 room = bx - tx - 8.f * u;
        T(tx, mid + 10.f * u, fit(title, 18.f * u, true, room), 18.f * u, C_TEXT, true, LLFontGL::LEFT);
        T(tx, mid - 13.f * u, fit(status, 14.f * u, false, room), 14.f * u, sc, false, LLFontGL::LEFT);
    }
}

void WolfStarfoldDeck::drawBaseAdd(F32 l, F32 b, F32 r, F32 t, F32 u)
{
    WolfStarbase& sb = WolfStarbase::instance();
    const F32 w = r - l;
    if (!sb.known()) { T(l, t - 20.f * u, "Asking the starbase office...", 18.f * u, C_DIM, false, LLFontGL::LEFT); return; }
    if (!sb.hasBase())
    {
        para(l, t - 16.f * u, "Found your starbase first, on the My base tab. Then come back here to add modules to it.", 18.f * u, w, C_TEXT);
        return;
    }
    // the cards: every module the region sells here now, but the core (design: "Choose a module from big picture cards")
    std::vector<const WolfStarbase::Kit*> kits;
    for (const WolfStarbase::Kit& k : sb.kits()) if (k.mKind != "core") kits.push_back(&k);
    if (kits.empty())
    {
        para(l, t - 16.f * u, "No modules can be bought here just now.", 18.f * u, w, C_DIM);
        return;
    }
    const F32 gap = 12.f * u, cw = llmin(250.f * u, (w - gap * 2.f) / 3.f), ch = 150.f * u;
    F32 x = l, y = t;
    for (size_t i = 0; i < kits.size() && i < 8; ++i)
    {
        const WolfStarbase::Kit& k = *kits[i];
        if (x + cw > r + 1.f) { x = l; y -= ch + gap; }
        const bool lit = mCard == k.mKind;
        rectG(x, y - ch, x + cw, y, lit ? C_BUTTON_LIT * 1.3f : C_BUTTON * 1.25f, lit ? C_BUTTON_LIT : C_BUTTON);
        frameW(x, y - ch, x + cw, y, llmax(1.f, 2.f * u), lit ? C_AMBER : C_EDGE);
        // its little picture: the module's cells
        const LLColor4 col = kindColour(k.mKind, false);
        const F32 cell = 16.f * u;
        for (S32 a = 0; a < llclamp(k.mSize[0], 1, 3); ++a)
            for (S32 bb = 0; bb < llclamp(k.mSize[1], 1, 3); ++bb)
            {
                const F32 px = x + cw - 18.f * u - (F32)(a + 1) * cell - (F32)bb * cell * 0.5f, py = y - 18.f * u - cell - (F32)bb * cell * 0.5f;
                rectF(px, py, px + cell - 2.f * u, py + cell - 2.f * u, col * 0.85f);
            }
        T(x + 14.f * u, y - 24.f * u, fit(k.mTitle, 19.f * u, true, cw - 90.f * u), 19.f * u, lit ? C_AMBER : C_TEXT, true, LLFontGL::LEFT);
        T(x + 14.f * u, y - 58.f * u, thousands(k.mPrice) + " " + WolfStarfold::currency(), 17.f * u, C_GREEN, true, LLFontGL::LEFT);
        T(x + 14.f * u, y - 84.f * u, llformat("Built in about %d minutes", k.mMinutes), 14.f * u, C_TEXT, false, LLFontGL::LEFT);
        T(x + 14.f * u, y - 106.f * u, llformat("%d prims, %d x %d x %d cells", k.mPrims, k.mSize[0], k.mSize[1], k.mSize[2]), 14.f * u, C_TEXT, false, LLFontGL::LEFT);
        if (k.mLane > 0.f) T(x + 14.f * u, y - 128.f * u, llformat("Needs a %d m clear lane", (S32)llround(k.mLane)), 14.f * u, C_DIM, false, LLFontGL::LEFT);
        addHit(x, y - ch, x + cw, y, (EHit)(H_BASE_CARD_0 + (S32)i), "Choose the " + k.mTitle + " and see where it fits");
        mCardIds.push_back(k.mKind);
        x += cw + gap;
    }
    y -= ch + 18.f * u;
    // where it fits
    if (sb.ghostFor().empty() || sb.ghostFor() != mCard)
    {
        para(l, y, "Choose a card to see where it fits on your base. Glowing outlines show the places; nothing is built until you press "
             "Build it here.", 16.f * u, w, C_DIM);
        return;
    }
    if (!sb.ghostsArrived()) { para(l, y, "Finding the places it fits...", 17.f * u, w, C_DIM); return; }
    if (sb.ghosts().empty())
    {
        para(l, y, sb.ghostText().empty() ? std::string("There is no room for that on your starbase.") : sb.ghostText(), 17.f * u, w, C_AMBER, true);
        return;
    }
    const S32 n = sb.spotCount(), turns = sb.turnCount();
    std::string line = n == 1 ? std::string("It fits in 1 place.") : llformat("It fits in %d places. Spot %d of %d", n, sb.spot() + 1, n);
    if (turns > 1) line += llformat(", turned %d ways", turns);
    if (n != 1 || turns > 1) line += ".";
    y = para(l, y, line + " The amber outline in space is where it will go.", 17.f * u, w, C_TEXT, true);
    if (const WolfStarbase::Kit* k = sb.kit(mCard))
    {
        if (sb.haveStarmarks() && sb.starmarks() < k->mPrice)
            para(l, y - 4.f * u, "You need " + thousands(k->mPrice - sb.starmarks()) + " more " + WolfStarfold::currency() +
                 ". A full hold of ore sells for 200 to 600 at the station.", 16.f * u, w, C_AMBER);
    }
}

void WolfStarfoldDeck::drawBaseStore(F32 l, F32 b, F32 r, F32 t, F32 u)
{
    WolfStarbase& sb = WolfStarbase::instance();
    const F32 w = r - l;
    if (!sb.known()) { T(l, t - 20.f * u, "Asking the starbase office...", 18.f * u, C_DIM, false, LLFontGL::LEFT); return; }
    if (!sb.hasBase())
    {
        para(l, t - 16.f * u, "Your storeroom travels with your starbase. If you packed your base up, bring it back here first: everything "
             "you bought comes back with it.", 18.f * u, w, C_TEXT);
        return;
    }
    std::vector<const WolfStarbase::Part*> rows;
    for (const WolfStarbase::Part& p : sb.parts()) if (p.mState == "stored" || p.mState == "off") rows.push_back(&p);
    F32 y = para(l, t - 14.f * u, "Nothing you bought is ever lost. A part that was returned, taken or moved off your base waits here until you "
                 "place it again.", 16.f * u, w, C_DIM) - 6.f * u;
    if (rows.empty()) { para(l, y, "Your storeroom is empty.", 18.f * u, w, C_TEXT, true); return; }
    const F32 rowH = targetH(u) + 12.f * u, bh = targetH(u), bw = 190.f * u;
    S32 per = (S32)((y - b) / rowH);
    if (per < (S32)rows.size()) per = (S32)((y - b - bh - 6.f * u) / rowH);   // the page line's room first
    per = llmax(1, per);
    const S32 pages = llmax(1, ((S32)rows.size() + per - 1) / per);
    mBasePage = llclamp(mBasePage, 0, pages - 1);
    if (pages > 1)
    {
        T(l, y - bh * 0.5f, llformat("Page %d of %d", mBasePage + 1, pages), 15.f * u, C_DIM, true, LLFontGL::LEFT);
        button(r - 2.f * bh - 6.f * u, y - bh, r - bh - 6.f * u, y, H_BASE_PAGE_PREV, "<", false, "The page before", u);
        button(r - bh, y - bh, r, y, H_BASE_PAGE_NEXT, ">", false, "The next page", u);
        y -= bh + 6.f * u;
    }
    for (S32 i = 0; i < per && mBasePage * per + i < (S32)rows.size(); ++i)
    {
        const WolfStarbase::Part& p = *rows[(size_t)(mBasePage * per + i)];
        const F32 top = y - (F32)i * rowH, mid = top - rowH * 0.5f;
        rectF(l, top - rowH + 4.f * u, r, top, LLColor4(0.03f, 0.07f, 0.16f, 0.75f));
        rectF(l, top - rowH + 4.f * u, l + 5.f * u, top, kindColour(p.mKind, p.mOwn));
        mRowIds[(size_t)i] = p.mId;
        F32 bx = r - 8.f * u;
        if (p.mState == "off")
        {
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWB_0 + i), "PLACE IT AGAIN", false, "Choose a new spot for it", u);
            bx -= bw + 8.f * u;
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWA_0 + i), "PUT IT BACK", false, "Snap it back to its old spot", u, true);
        }
        else
        {
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWA_0 + i), "PLACE IT", false,
                   p.mKind == "decor" ? "Put it back inside your base's frame" : "See where it fits, then place it", u, true);
        }
        bx -= bw + 8.f * u;
        const F32 tx = l + 20.f * u, room = bx - tx;
        T(tx, mid + 10.f * u, fit(kindTitle(p.mKind) + (p.mOwn ? " (your own build)" : ""), 18.f * u, true, room), 18.f * u, C_TEXT, true, LLFontGL::LEFT);
        T(tx, mid - 13.f * u, fit(p.mState == "off" ? "Moved off your starbase, so its work has paused." : "In your storeroom.", 14.f * u, false, room),
          14.f * u, p.mState == "off" ? C_AMBER : C_DIM, false, LLFontGL::LEFT);
    }
}

void WolfStarfoldDeck::drawBaseOwn(F32 l, F32 b, F32 r, F32 t, F32 u)
{
    // design.html#bases "Build your own modules": "write starbase in its description ... the window's Your own builds tab lists
    // every object of yours nearby with the word, each with a green tick or a plain reason"; Attach where you can build, Send to
    // my base from anywhere else (the object is moved, never copied)
    WolfStarbase& sb = WolfStarbase::instance();
    const F32 w = r - l;
    F32 y = para(l, t - 14.f * u, "Build anything with the normal build tools and write starbase:quarters (or dock, hangar...) in its Description. "
                 "Just starbase makes it decoration. No scripts or contents, at most 64 prims.", 16.f * u, w, C_DIM) - 6.f * u;
    if (!sb.ownAsked()) { para(l, y, "Looking for your builds nearby...", 17.f * u, w, C_DIM); return; }
    if (sb.own().empty())
    {
        para(l, y, sb.ownText().empty() ? std::string("No builds of yours with starbase in the description were found near you.") : sb.ownText(),
             17.f * u, w, C_TEXT);
        return;
    }
    if (!sb.ownHere())
        y = para(l, y, "Your starbase is not in this region, so use Send to my base: it waits in your storeroom.", 15.f * u, w, C_AMBER) - 4.f * u;
    const F32 rowH = targetH(u) + 12.f * u, bh = targetH(u), bw = 200.f * u;
    S32 per = (S32)((y - b) / rowH);
    if (per < (S32)sb.own().size()) per = (S32)((y - b - bh - 6.f * u) / rowH);   // the page line's room first
    per = llmax(1, per);
    const S32 pages = llmax(1, ((S32)sb.own().size() + per - 1) / per);
    mBasePage = llclamp(mBasePage, 0, pages - 1);
    if (pages > 1)
    {
        T(l, y - bh * 0.5f, llformat("Page %d of %d", mBasePage + 1, pages), 15.f * u, C_DIM, true, LLFontGL::LEFT);
        button(r - 2.f * bh - 6.f * u, y - bh, r - bh - 6.f * u, y, H_BASE_PAGE_PREV, "<", false, "The page before", u);
        button(r - bh, y - bh, r, y, H_BASE_PAGE_NEXT, ">", false, "The next page", u);
        y -= bh + 6.f * u;
    }
    for (S32 i = 0; i < per && mBasePage * per + i < (S32)sb.own().size(); ++i)
    {
        const WolfStarbase::Own& o = sb.own()[(size_t)(mBasePage * per + i)];
        const F32 top = y - (F32)i * rowH, mid = top - rowH * 0.5f;
        rectF(l, top - rowH + 4.f * u, r, top, LLColor4(0.03f, 0.07f, 0.16f, 0.75f));
        rectF(l, top - rowH + 4.f * u, l + 5.f * u, top, o.mOk ? C_GREEN : C_AMBER);
        mRowIds[(size_t)i] = o.mId;
        F32 bx = r - 8.f * u;
        if (o.mOk)
        {
            button(bx - bw, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWB_0 + i), "SEND TO MY BASE", false,
                   "Move it (never copied) to your base's storeroom", u);
            bx -= bw + 8.f * u;
            if (sb.ownHere())
            {
                button(bx - bw * 0.7f, mid - bh * 0.5f, bx, mid + bh * 0.5f, (EHit)(H_BASE_ROWA_0 + i), "ATTACH", false,
                       o.mKind == "decor" ? "Make it part of your base, where it is" : "See where it fits on your base, then attach it", u, true);
                bx -= bw * 0.7f + 8.f * u;
            }
        }
        const F32 tx = l + 20.f * u, room = bx - tx;
        std::string facts = kindTitle(o.mKind) + llformat(", %d prims", o.mPrims);
        if (o.mMetres >= 0.f) facts += ", " + distance(o.mMetres) + " from your base";
        T(tx, mid + 12.f * u, fit((o.mName.empty() ? std::string("(no name)") : o.mName) + " - " + facts, 17.f * u, true, room), 17.f * u, C_TEXT, true, LLFontGL::LEFT);
        T(tx, mid - 12.f * u, fit(o.mOk ? std::string("Ready") : o.mReason, 14.f * u, false, room), 14.f * u, o.mOk ? C_GREEN : C_AMBER, !o.mOk, LLFontGL::LEFT);
    }
}

void WolfStarfoldDeck::pressBase(EHit id)
{
    WolfStarbase& sb = WolfStarbase::instance();
    const std::string armed = mArmed;
    mArmed.clear();   // any press disarms a waiting second press, but its own
    if (id >= H_BASE_TAB_0 && id <= H_BASE_TAB_LAST)
    {
        sb.setTab((WolfStarbase::ETab)(id - H_BASE_TAB_0));
        mBasePage = 0;
        mCard.clear();
        return;
    }
    if (id >= H_BASE_CARD_0 && id <= H_BASE_CARD_LAST)
    {
        const size_t i = (size_t)(id - H_BASE_CARD_0);
        if (i < mCardIds.size()) { mCard = mCardIds[i]; sb.showGhosts(mCard); }
        return;
    }
    if (id >= H_BASE_GHOST_0 && id <= H_BASE_GHOST_LAST) { sb.choose((size_t)(id - H_BASE_GHOST_0)); return; }
    if (id >= H_BASE_ROWC_0 && id <= H_BASE_ROWC_LAST)
    {
        const size_t slot = (size_t)(id - H_BASE_ROWC_0);
        if (slot < mRowIds.size() && !mRowIds[slot].empty()) sb.makeSeat(mRowIds[slot]);
        return;
    }
    const bool rowA = id >= H_BASE_ROWA_0 && id <= H_BASE_ROWA_LAST, rowB = id >= H_BASE_ROWB_0 && id <= H_BASE_ROWB_LAST;
    if (rowA || rowB)
    {
        const size_t slot = (size_t)(id - (rowA ? H_BASE_ROWA_0 : H_BASE_ROWB_0));
        const std::string rid = slot < mRowIds.size() ? mRowIds[slot] : std::string();
        if (rid.empty()) return;
        if (sb.tab() == WolfStarbase::TAB_OWN)
        {
            if (rowB) { sb.sendToBase(rid); return; }
            for (const WolfStarbase::Own& o : sb.own())
                if (o.mId == rid) { if (o.mKind == "decor") sb.attachAsIs(rid); else sb.showGhosts("own:" + rid); }
            return;
        }
        const WolfStarbase::Part* p = sb.part(rid);
        if (!p) return;
        if (sb.tab() == WolfStarbase::TAB_STORE)
        {
            if (rowA && p->mState == "off") { sb.putBack(rid); return; }
            if (p->mKind == "decor") sb.placeAsIs(rid); else sb.showGhosts("part:" + rid);
            return;
        }
        // My base: A puts an off part back or buys an own shell's core; B takes a part down (pressed twice)
        if (rowA) { if (p->mState == "off") sb.putBack(rid); else sb.buyCore(rid); return; }
        if (armed == "down:" + rid) sb.takeDown(rid);
        else mArmed = "down:" + rid;
        return;
    }
    switch (id)
    {
        case H_BASE_CLOSE: sb.show(false); break;
        case H_BASE_FOUND: sb.found(); break;
        case H_BASE_SPOT: sb.findSpot(); break;
        case H_BASE_RESTORE: sb.restore(); break;
        case H_BASE_HOME: sb.home(); break;
        case H_BASE_FLY: sb.flyHome(); break;
        case H_BASE_DOCKAT: sb.dockHere(); break;
        case H_BASE_DOCK_ALL: sb.setDock("all"); break;
        case H_BASE_DOCK_FRIENDS: sb.setDock("friends"); break;
        case H_BASE_DOCK_GROUP: sb.setDock("group"); break;
        case H_BASE_DOCK_ME: sb.setDock("me"); break;
        case H_BASE_PACK:
            if (armed == "pack") sb.pack(); else mArmed = "pack";
            break;
        case H_BASE_SCRAP:
            // design.html#bases: "the owner can scrap their own base from the window, after a plain 'Are you sure?' that names what
            // will go. Modules return 50 % of their price." The region asks again for sure=1 (SpaceBases.cs BaseVerb scrap).
            if (armed == "scrap") sb.scrap(true); else mArmed = "scrap";
            break;
        case H_BASE_PREV: sb.nextSpot(-1); break;
        case H_BASE_NEXT: sb.nextSpot(1); break;
        case H_BASE_TURN: sb.nextTurn(); break;
        case H_BASE_BUILD: sb.build(); mCard.clear(); break;
        case H_BASE_CANCEL: sb.clearGhosts(); mCard.clear(); break;
        case H_BASE_OWN_CHECK: sb.askOwn(); break;
        case H_BASE_PAGE_PREV: mBasePage = llmax(0, mBasePage - 1); break;
        case H_BASE_PAGE_NEXT: ++mBasePage; break;   // clamped when drawn
        default: break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// the mouse
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfStarfoldDeck::press(EHit id, S32 x, S32 y)
{
    WolfStarfold& sf = WolfStarfold::instance();
    if (id != H_SETUP_CLEAR) mClearArmed = false;   // any other press disarms the remove
    // [WOLFTREK v11] the bay's automatic approach or departure flies the ship (SpaceEngine.OnChat ignores these meanwhile): a
    // press says so in words instead of doing nothing (CLAUDE.md: visible feedback)
    if (sf.automatic() && (id == H_FIRE || id == H_MINE || id == H_NEXT || id == H_DOCK || id == H_MODE || id == H_THROTTLE
                           || id == H_FLY_STATION || id == H_FLY_BELT || id == H_FLY_WORMHOLE || id == H_FLY_STOP || id == H_FLY_PICKED))
    {
        showHint(sf.automaticIn() ? "Automatic docking: the ship flies itself. Press STOP to take the controls."
                                  : "Automatic departure: you have the controls once you are out of the bay.");
        return;
    }
    switch (id)
    {
        case H_BANNER_STOP: sf.stopApproach(); break;
        case H_STATION_RENAME: startEdit(EDIT_STATION, sf.stationName()); break;
        case H_BASE_RENAME: startEdit(EDIT_BASE, WolfStarbase::instance().baseName()); break;
        case H_NAME_FIELD: setFocus(true); break;
        case H_NAME_SAVE: saveEdit(); break;
        case H_NAME_CANCEL: cancelEdit(); break;
        case H_FIRE: sf.fire(!sf.firing()); break;
        case H_MINE: sf.mine(!sf.mining()); break;
        case H_NEXT: sf.nextTarget(); break;
        case H_DOCK:
        {
            // [STARBASES 2026-11-24] a base picked on the radar and nearer than the station: dock there (SpaceBases.cs TryDockBase:
            // the owner at the core, others at a ready docking ring they are let into; the region says why not), else the station
            LLVector3 shipAt;
            bool base = false;
            if (!mDockBase.empty() && sf.beamEnd(sf.ship().mRoot, -1, shipAt))
            {
                for (const WolfStarbase::Known& kb : WolfStarbase::instance().bases())
                {
                    if (kb.mId != mDockBase) continue;
                    LLViewerRegion* region = gAgent.getRegion();
                    if (!region) break;
                    const F32 toBase = (region->getPosAgentFromRegion(kb.mPos) - shipAt).length();
                    const F32 toStation = sf.system().mHaveStation ? (region->getPosAgentFromRegion(sf.system().mStation) - shipAt).length() : 1e9f;
                    base = toBase < toStation;
                }
            }
            if (base) sf.intent("dock|at=" + mDockBase);
            else sf.dock();
            break;
        }
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
        case H_BASE_OPEN: WolfStarbase::instance().toggle(); break;
        default:
            if (id >= H_RADAR_BASE_0 && id <= H_RADAR_BASE_LAST)
            {
                const size_t i = (size_t)(id - H_RADAR_BASE_0);
                if (i < WolfStarbase::instance().bases().size())
                {
                    mDockBase = WolfStarbase::instance().bases()[i].mId;
                    mPicked = "base:" + mDockBase;
                }
                break;
            }
            if (id >= H_BASE_CLOSE && id <= H_BASE_GHOST_LAST) { pressBase(id); break; }
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
    if (sf.piloting() && !sf.automatic() && !sf.panelHidden() && !onPanel(x, y))
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
        // [STARBASES] never for the Starbase window or the radar's bases: Pack up, Scrap and Take down wait for a deliberate
        // second press, and a double click must not be taken as one
        if (hit->mId != H_NONE && hit->mId != H_THROTTLE && hit->mId < H_BASE_OPEN) press(hit->mId, x, y);
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

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// [WOLFTREK v11 2026-11-25] the automatic approach's banner, and the rename field
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfStarfoldDeck::drawBanner(F32 u)
{
    // design.html#bay: "the cockpit shows Automatic docking ... you need to touch nothing. The keys do nothing during the approach;
    // STOP hands you the controls again." Words, not colour alone; STOP only for an approach (SpaceEngine.OnChat: a departure
    // cannot be stopped); nothing on it counts down.
    WolfStarfold& sf = WolfStarfold::instance();
    if (!gViewerWindow) return;
    LLRect world = gViewerWindow->getWorldViewRectScaled();
    const LLRect mine = calcScreenRect();
    world.translate(-mine.mLeft, -mine.mBottom);
    const bool in = sf.automaticIn();
    std::string title = "AUTOMATIC DEPARTURE";
    if (in)
    {
        const S32 pad = sf.bayPad();
        title = pad > 0 ? llformat("AUTOMATIC DOCKING - pad %d", pad) : (pad == 0 ? std::string("AUTOMATIC DOCKING - the lift") : std::string("AUTOMATIC DOCKING"));
    }
    const std::string sname = sf.stationName().empty() ? std::string("the station") : sf.stationName();
    const std::string line = in ? "The ship flies itself into " + sname + " and lands. STOP gives you the controls."
                                : "The ship flies itself out of " + sname + ". You have the controls once you are clear.";
    const F32 px = 24.f * u, px2 = 16.f * u, bh = llmax(52.f * u, 44.f), gap = 16.f * u;
    const F32 stopW = in ? llmax(120.f * u, 44.f) : 0.f;
    const F32 textWide = llmax(textW(title, px, true), textW(line, px2, false));
    const F32 w = llmin((F32)world.getWidth() - 32.f, textWide + 48.f * u + (in ? stopW + gap : 0.f));
    const F32 h = llmax(bh + 20.f * u, px * 1.4f + px2 * 1.6f + 24.f * u);
    const F32 cx = (F32)world.getCenterX(), top = (F32)world.mTop - 18.f * u;
    const F32 l = cx - w * 0.5f, r = cx + w * 0.5f, b = top - h, t = top;
    pill(l, b, r, t, LLColor4(0.02f, 0.05f, 0.12f, 0.90f));
    pillLine(l, b, r, t, llmax(1.5f, 2.f * u), C_AMBER);
    addHit(l, b, r, t, H_NONE, "");   // the banner takes its clicks: none steer the ship (none would: the region flies it)
    const F32 tx = l + 24.f * u, room = (r - l) - 48.f * u - (in ? stopW + gap : 0.f);
    T(tx, t - 12.f * u - px * 0.7f, fit(title, px, true, room), px, C_AMBER, true, LLFontGL::LEFT);
    T(tx, b + 12.f * u + px2 * 0.8f, fit(line, px2, false, room), px2, C_TEXT, false, LLFontGL::LEFT);
    if (in)
    {
        const F32 sb = (b + t) * 0.5f - bh * 0.5f;
        capsule(r - 20.f * u - stopW, sb, r - 20.f * u, sb + bh, H_BANNER_STOP, "STOP", false, "Stop the automatic docking and take the controls", u);
    }
}

void WolfStarfoldDeck::startEdit(EEdit what, const std::string& now)
{
    mEdit = what;
    mEditText = now;
    mEditError.clear();
    setFocus(true);   // the typed letters come here (handleKeyHere / handleUnicodeCharHere), not to the world's keys
}

void WolfStarfoldDeck::cancelEdit()
{
    if (mEdit == EDIT_NONE) return;
    mEdit = EDIT_NONE;
    mEditText.clear();
    mEditError.clear();
    if (hasFocus()) setFocus(false);
}

void WolfStarfoldDeck::saveEdit()
{
    if (mEdit == EDIT_NONE) return;
    std::string why;
    const std::string name = WolfStarfold::checkName(mEditText, why);
    if (name.empty())
    {
        mEditError = why;   // the field stays open with the words as typed, the reason beside it
        return;
    }
    if (mEdit == EDIT_STATION) WolfStarfold::instance().renameStation(name);
    else WolfStarbase::instance().rename(name);
    cancelEdit();
}

void WolfStarfoldDeck::drawEditRow(F32 l, F32 b, F32 r, F32 t, F32 u)
{
    // the field, then SAVE (the primary) and CANCEL, each a big target (design: at least 44 px)
    const F32 bw = llmax(130.f * u, 44.f), gap = 8.f * u;
    const F32 fr = r - 2.f * (bw + gap);
    const bool focused = hasFocus();
    rectF(l, b, fr, t, LLColor4(0.01f, 0.03f, 0.08f, 0.96f));
    frameW(l, b, fr, t, focused ? llmax(2.f, 3.f * u) : llmax(1.f, 2.f * u), focused ? C_AMBER : (mEditError.empty() ? C_EDGE : C_RED));
    const F32 px = llmin((t - b) * 0.40f, 20.f * u), room = (fr - l) - 30.f * u, cy = (b + t) * 0.5f;
    F32 x = l + 12.f * u;
    if (mEditText.empty())
    {
        T(x, cy, fit("Type the new name", px, false, room), px, C_DIM, false, LLFontGL::LEFT);
    }
    else
    {
        // the end of the text stays in view while typing (as wolfadventuretalk.cpp's box: only what fits, from the right)
        LLWString w = utf8str_to_wstring(mEditText);
        while (w.size() > 1 && textW(wstring_to_utf8str(w), px, false) > room) w.erase(0, 1);
        const std::string shown = wstring_to_utf8str(w);
        T(x, cy, shown, px, C_TEXT, false, LLFontGL::LEFT);
        x += textW(shown, px, false);
    }
    if (focused && fmod(LLFrameTimer::getElapsedSeconds(), 1.0) < 0.6)
        rectF(x + 2.f * u, cy - px * 0.6f, x + 2.f * u + llmax(1.f, 2.f * u), cy + px * 0.6f, C_TEXT);
    addHit(l, b, fr, t, H_NAME_FIELD, "Type the new name (3 to 32 letters, numbers, spaces, apostrophes and hyphens), then press Enter or SAVE. Escape cancels.");
    button(fr + gap, b, fr + gap + bw, t, H_NAME_SAVE, "SAVE", false, "Save the new name (Enter)", u, true);
    button(r - bw, b, r, t, H_NAME_CANCEL, "CANCEL", false, "Keep the name as it is (Escape)", u);
}

bool WolfStarfoldDeck::handleKeyHere(KEY key, MASK mask)
{
    // Source: wolfadventuretalk.cpp WolfAdventureTalkView::handleKeyHere - Backspace, Enter, Ctrl+V, and every printable key is
    // the field's (its character arrives in handleUnicodeCharHere), so typed letters never reach the world's or the cockpit's
    // keys (LLViewerInput::scanKey bails on mKeyHandledByUI)
    if (mEdit == EDIT_NONE) return false;
    if (key == KEY_ESCAPE && mask == MASK_NONE) { cancelEdit(); return true; }
    if ((key == KEY_RETURN || key == KEY_PAD_RETURN) && mask == MASK_NONE) { saveEdit(); return true; }
    if (key == KEY_BACKSPACE && mask == MASK_NONE)
    {
        LLWString w = utf8str_to_wstring(mEditText);
        if (!w.empty()) w.pop_back();
        mEditText = wstring_to_utf8str(w);
        mEditError.clear();
        return true;
    }
    if (key == 'V' && (mask & MASK_CONTROL))
    {
        LLWString pasted;
        if (LLClipboard::instance().pasteFromClipboard(pasted))
        {
            LLWString w = utf8str_to_wstring(mEditText);
            for (llwchar c : pasted)
            {
                if (w.size() >= 40) break;   // a little over NameFilter.NAME_MAX (32): folded spaces may shorten it
                w.push_back(c < 0x20 ? (llwchar)' ' : c);
            }
            mEditText = wstring_to_utf8str(w);
            mEditError.clear();
        }
        return true;
    }
    if ((mask == MASK_NONE || mask == MASK_SHIFT) && key != KEY_TAB && key < KEY_SPECIAL) return true;
    return false;
}

bool WolfStarfoldDeck::handleUnicodeCharHere(llwchar uni_char)
{
    if (mEdit == EDIT_NONE) return false;
    if (uni_char < 0x20 || uni_char == 0x7f) return true;
    LLWString w = utf8str_to_wstring(mEditText);
    if (w.size() < 40) w.push_back(uni_char);
    mEditText = wstring_to_utf8str(w);
    mEditError.clear();
    return true;
}
