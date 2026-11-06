/**
 * @file wolfskihud.cpp
 * @brief WolfViewer: the Ski HUD. See wolfskihud.h and wolfskis.h.
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

#include "wolfskihud.h"

#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llwindow.h"
#include "wolfskis.h"

static LLDefaultChildRegistry::Register<WolfSkiHud> r_wolf_ski_hud("wolf_ski_hud");

namespace
{
    const F32 PI_F = 3.14159265f;

    // Night blue and ice; status colours as the dashboard's (ISO 2575, wolfdashboard.cpp): amber caution, red act now.
    // Text is white or ice on navy (contrast above 10:1), the piste chips carry their word as well as their colour.
    const LLColor4 NAVY_HI(0.09f, 0.17f, 0.28f, 0.94f), NAVY_LO(0.02f, 0.05f, 0.09f, 0.94f);
    const LLColor4 INSET_ON(0.f, 0.f, 0.f, 0.42f);
    // [SKIS 2026-11-06] Paul: the background must be able to go. With it off each read-out keeps its own backing, darker
    // and more opaque, so white text stays readable over bright snow (navy 0.02/0.05/0.09 at 78 %: well over 7:1 for SNOW).
    const LLColor4 INSET_OFF(0.02f, 0.05f, 0.09f, 0.78f);
    LLColor4 INSET = INSET_ON;                         // this frame's (draw() sets it from WolfSkiHudBackground)
    const LLColor4 ICE(0.62f, 0.88f, 1.00f, 1.f), ICE_DIM(0.38f, 0.55f, 0.68f, 1.f);
    const LLColor4 SNOW(0.97f, 0.98f, 1.00f, 1.f);
    const LLColor4 GREY(0.66f, 0.72f, 0.78f, 1.f);
    const LLColor4 GOLD(1.00f, 0.82f, 0.25f, 1.f);
    const LLColor4 AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 RED(1.00f, 0.25f, 0.20f, 1.f);
    const LLColor4 GREEN(0.30f, 0.90f, 0.45f, 1.f);
    // piste markers (European classification; the chip colours as the trail maps draw them)
    const LLColor4 P_BLUE(0.15f, 0.45f, 0.95f, 1.f), P_RED(0.88f, 0.15f, 0.15f, 1.f), P_BLACK(0.04f, 0.04f, 0.05f, 1.f);

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    // ---- primitives, view-local, untextured (Source: wolfhelmhud.cpp's, after wolfdashboard.cpp's) ----
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

    void triF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
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
        const F32 dx = x2 - x1, dy = y2 - y1;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quadF(x1 + nx, y1 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, x2 + nx, y2 + ny, c);
    }

    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c);
        rectF(l, t - w, r, t, c);
        rectF(l, b, l + w, t, c);
        rectF(r - w, b, r, t, c);
    }

    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 40)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * (F32)i / (F32)seg, a1 = 2.f * PI_F * (F32)(i + 1) / (F32)seg;
            vtx(cx, cy, c);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }

    // A lit dome, brighter to the upper left (Source: wolfhelmhud.cpp circleShaded).
    void circleShaded(F32 cx, F32 cy, F32 rad, const LLColor4& hi, const LLColor4& lo, S32 seg = 48)
    {
        noTex();
        const F32 hx = cx - rad * 0.3f, hy = cy + rad * 0.3f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * (F32)i / (F32)seg, a1 = 2.f * PI_F * (F32)(i + 1) / (F32)seg;
            vtx(hx, hy, hi);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), lo);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), lo);
        }
        gGL.end();
    }

    // A stroked arc, radians counter-clockwise from +x (Source: wolfhelmhud.cpp arcW).
    void arcW(F32 cx, F32 cy, F32 rad, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48)
    {
        noTex();
        const F32 h = w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * (F32)i / (F32)seg, u1 = a0 + (a1 - a0) * (F32)(i + 1) / (F32)seg;
            const F32 c0 = cosf(u0), s0 = sinf(u0), c1 = cosf(u1), s1 = sinf(u1);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c0, cy + (rad + h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c); vtx(cx + (rad - h) * c1, cy + (rad - h) * s1, c);
        }
        gGL.end();
    }

    void roundRectF(F32 l, F32 b, F32 r, F32 t, F32 rad, const LLColor4& top, const LLColor4& bot)
    {
        // Source: wolfhelmhud.cpp roundRectF
        rad = llmin(rad, (r - l) * 0.5f, (t - b) * 0.5f);
        const F32 h = llmax(0.001f, t - b);
        auto col = [&](F32 y) { return lerp(bot, top, llclamp((y - b) / h, 0.f, 1.f)); };
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b + rad, col(b + rad)); vtx(r, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad));
        vtx(l, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad)); vtx(l, t - rad, col(t - rad));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t)); vtx(l + rad, t, col(t));
        vtx(l + rad, b, col(b)); vtx(r - rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad));
        vtx(l + rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad)); vtx(l + rad, b + rad, col(b + rad));
        const F32 cxs[4] = { r - rad, l + rad, l + rad, r - rad };
        const F32 cys[4] = { t - rad, t - rad, b + rad, b + rad };
        for (S32 k = 0; k < 4; ++k)
        {
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 a0 = PI_F * 0.5f * ((F32)k + (F32)i / 6.f), a1 = PI_F * 0.5f * ((F32)k + (F32)(i + 1) / 6.f);
                const F32 x0 = cxs[k] + rad * cosf(a0), y0 = cys[k] + rad * sinf(a0);
                const F32 x1 = cxs[k] + rad * cosf(a1), y1 = cys[k] + rad * sinf(a1);
                vtx(cxs[k], cys[k], col(cys[k])); vtx(x0, y0, col(y0)); vtx(x1, y1, col(y1));
            }
        }
        gGL.end();
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER, LLFontGL::VAlign v = LLFontGL::VCENTER)
    {
        WolfHudScale::text(f, s, x, y, c, h, v);   // sharp at the size the rider set (wolfhudscale.h)
    }

    // Source: wolfhelmhud.cpp face() - the B612 face (fonts.xml Avionics0..11) at the largest size whose line fits px.
    const LLFontGL* face(const char* name, U8 style, F32 px)
    {
        static std::map<std::string, std::vector<const LLFontGL*>> cache;
        const std::string key = std::string(name) + (style ? "B" : "R");
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                if (const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, llformat("Avionics%d", i), style)))
                {
                    v.push_back(f);
                }
            }
            if (v.empty())
            {
                LL_WARNS("WolfSkiHud") << "font " << name << " missing, using the default" << LL_ENDL;
                v.push_back(LLFontGL::getFontSansSerif());
            }
            it = cache.emplace(key, v).first;
        }
        const LLFontGL* best = it->second.front();
        for (const LLFontGL* f : it->second)
        {
            if ((F32)f->getLineHeight() <= px) best = f;
        }
        return best;
    }

    // ════════════════════════════════════════════════════════════════════════════════════
    // THE LAYOUT, in the dashboards' frame (Source: wolfhelmhud.cpp "THE LAYOUT"): 1600 x 900 with the panel from y 596 to
    // 900 (y down), scaled by u. The speed keeps its distance from the left edge (L), the run stays centred (C), the
    // style and the buttons keep theirs from the right edge (R). Too narrow for one row: the centre goes on a row above.
    // ════════════════════════════════════════════════════════════════════════════════════
    struct Anchor { F32 ox = 0.f, top0 = 900.f, u = 1.f; };
    struct Geo
    {
        F32 W = 1600.f;
        Anchor L, C, R, cur;
        F32 bandB = 0.f;
        F32 panelTop = 304.f;
    } g;
    const F32 C_L = 600.f, C_R = 1000.f, R_L = 1040.f;   // the centre and right parts' edges, mock units (left: 0..560)
    void use(const Anchor& a) { g.cur = a; }
    F32 X(F32 x) { return g.cur.ox + x * g.cur.u; }
    F32 YM(F32 y) { return g.cur.top0 - y * g.cur.u; }
    F32 S(F32 v) { return v * g.cur.u; }

    // a point at `deg` clockwise from 12 o'clock (y up)
    void pt(F32 cx, F32 cy, F32 r, F32 deg, F32& x, F32& y)
    {
        const F32 a = deg * DEG_TO_RAD;
        x = cx + r * sinf(a);
        y = cy + r * cosf(a);
    }
    void clockArc(F32 cx, F32 cy, F32 r, F32 w, F32 d0, F32 d1, const LLColor4& c, S32 seg = 32)
    {
        arcW(cx, cy, r, w, (90.f - d1) * DEG_TO_RAD, (90.f - d0) * DEG_TO_RAD, c, seg);
    }

    const LLFontGL* font(F32 mock_px, bool bold = false, bool mono = false)
    {
        return face(mono ? "B612Mono" : "B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(mock_px) * 1.25f));
    }

    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, bool mono = false,
           LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        text(font(px, bold, mono), s, x, y, c, h, LLFontGL::VCENTER);
    }

    // Centred text that must fit `maxw` (picture units): smaller a size at a time down to `min_px`, then cut with "...".
    // Source: wolfhelmhud.cpp fitT.
    void fitT(F32 x, F32 y, std::string str, F32 px, F32 min_px, F32 maxw, const LLColor4& c, bool bold = false)
    {
        F32 size = px;
        const LLFontGL* f = font(size, bold);
        while (f && f->getWidthF32(str) > maxw && size > min_px)
        {
            size -= 1.f;
            f = font(size, bold);
        }
        if (f && f->getWidthF32(str) > maxw)
        {
            while (str.size() > 4 && f->getWidthF32(str + "...") > maxw)
            {
                str.pop_back();
                while (!str.empty() && ((unsigned char)str.back() & 0xC0) == 0x80) str.pop_back();
                if (!str.empty() && (unsigned char)str.back() >= 0xC0) str.pop_back();
            }
            str += "...";
        }
        text(f, str, x, y, c, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    struct Ctx
    {
        std::function<void(F32, F32, F32, F32, WolfSkiHud::EHit, const std::string&)> hit;
        S32 pressed;
    };

    void button(Ctx& c, F32 l, F32 b, F32 r, F32 t, const std::string& label, WolfSkiHud::EHit id, const std::string& tip,
                F32 px, const LLColor4& face_hi, const LLColor4& face_lo)
    {
        const bool down = c.pressed == id;
        roundRectF(l, b, r, t, (t - b) * 0.25f, down ? face_lo : face_hi, down ? face_hi : face_lo);
        frameW(l, b, r, t, 1.2f, alpha(SNOW, 0.55f));
        T((l + r) * 0.5f, (b + t) * 0.5f, label, px, SNOW, true);
        c.hit(l, b, r, t, id, tip);
    }

    std::string clockText(F32 secs)
    {
        const S32 tenths = (S32)llround(secs * 10.f);
        return llformat("%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
    }

    std::string thousands(S32 n)
    {
        std::string s = llformat("%d", n);
        for (S32 i = (S32)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
        return s;
    }

    // The steepness as a trail map marks it. Source: Wikipedia "Piste" (European classification): blue runs are not
    // steeper than 25 % (14.0 degrees) and red ones not than 40 % (21.8 degrees); black ones may be. Under 5 degrees
    // (9 %) it says "gentle": not a marker, our own word for where a beginner glides.
    void piste(F32 deg, std::string& word, LLColor4& chip)
    {
        const F32 pct = tanf(deg * DEG_TO_RAD) * 100.f;
        if (deg < 5.f) { word = "GENTLE"; chip = GREEN; }
        else if (pct <= 25.f) { word = "BLUE"; chip = P_BLUE; }
        else if (pct <= 40.f) { word = "RED"; chip = P_RED; }
        else { word = "BLACK"; chip = P_BLACK; }
    }

    std::string modeWords(const WolfSkis::Shown& s, WolfSkis::EKind kind)
    {
        if (s.mWiped || s.mMode == "wipe") return "WIPE OUT - GETTING UP";
        if (s.mInAir) return "IN THE AIR!";
        if (s.mMode == "water") return "ON THE WATER";
        if (s.mMode == "ready") return "READY - PRESS AN ARROW KEY TO GO";   // held still until the first key (ski_template.lsl go())
        if (!s.mRunning) return s.mHaveLast ? "RUN OVER - PUSH OFF FOR ANOTHER" : "PUSH OFF TO START (UP, OR POINT DOWNHILL)";
        if (s.mMode == "tuck") return "TUCKED - FASTER, WIDER TURNS";
        if (s.mMode == "brake") return kind == WolfSkis::KIND_SNOWBOARD ? "HEEL BRAKE" : "SNOWPLOUGH";
        if (s.mMode == "push") return kind == WolfSkis::KIND_SNOWBOARD ? "PUSHING ALONG" : "SKATING";
        if (s.mMode == "carve_l" || s.mMode == "carve_r") return s.mCombo >= 2 ? llformat("CARVING  x%d", s.mCombo) : std::string("CARVING");
        return "RUNNING";
    }

    struct HelpLine { S32 kind; const char* s; };   // 0 heading, 1 line
}

WolfSkiHud::WolfSkiHud(const Params& p) : LLUICtrl(p) {}
WolfSkiHud::~WolfSkiHud() {}

void WolfSkiHud::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ llmin(l, r), llmin(b, t), llmax(l, r), llmax(b, t), id, tip });
}

const WolfSkiHud::Hit* WolfSkiHud::hitAt(S32 x, S32 y) const
{
    const F32 px = (F32)x / mHitScale, py = (F32)y / mHitScale;
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (px >= it->mL && px <= it->mR && py >= it->mB && py <= it->mT)
        {
            return &*it;
        }
    }
    return nullptr;
}

bool WolfSkiHud::onPanel(S32 x, S32 y) const
{
    if (!mDrawn) return false;
    if (hitAt(x, y)) return true;
    // Source: wolfhelmhud.cpp onPanel - with the background on the whole band is solid; off, only the controls, and
    // clicks between the read-outs reach the world
    if (!gSavedSettings.getBOOL("WolfSkiHudBackground")) return false;
    return (F32)y >= mPanelB && (F32)y <= mPanelT;
}

void WolfSkiHud::press(EHit id)
{
    WolfSkis& sk = WolfSkis::instance();
    switch (id)
    {
    case H_UNITS:    sk.setMph(!sk.mph()); break;
    case H_TAKE_OFF: sk.takeOff(); break;
    case H_HIDE:     sk.showHud(false); break;
    case H_PANEL:    gSavedSettings.setBOOL("WolfSkiHudBackground", !gSavedSettings.getBOOL("WolfSkiHudBackground")); break;
    case H_HELP:     mShowHelp = !mShowHelp; break;
    default: break;
    }
}

void WolfSkiHud::draw()
{
    WolfSkis& sk = WolfSkis::instance();
    mHits.clear();
    mDrawn = false;
    if (!sk.hudShowing() || !WolfSkis::onWolf())
    {
        mShowHelp = false;
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        mPressed = H_NONE;
        mResize.hide();
        return;
    }
    if (!gSavedSettings.getBOOL("WolfSkiHudHelpSeen"))
    {
        gSavedSettings.setBOOL("WolfSkiHudHelpSeen", true);
        mShowHelp = true;   // the keys show by themselves the first time (as the dashboards' guides)
    }

    // the band, as wolfhelmhud.cpp draw() (but always one row, below): above the bottom toolbar, 30 % of the window
    // (170..380 px), scaled by the size
    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    F32 screen_bottom = 0.f;
    if (gToolBarView)
    {
        if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
        {
            if (bp->isInVisibleChain())
            {
                const LLRect sr = bp->calcScreenRect();
                S32 lx = 0, ly = 0;
                screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                screen_bottom = llclamp((F32)ly, 0.f, Hv * 0.4f);
            }
        }
    }
    const F32 zoom = mResize.scale();
    const F32 W = screen_w / zoom;
    const F32 bottom = screen_bottom / zoom;
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    // [SKIS 2026-11-06] Paul: "we'd lose half the screen" on a small one. The family's sizes are 30 % of the window,
    // 170..380 px (wolfhelmhud.cpp / wolfdashboard.cpp:2629) and 34 %, 200..430 px (wolfflightdeck.cpp:710); this HUD takes
    // 26 %, 120..330 px, and never more than 30 % of what is above the toolbar - so 720 high is at most ~190 px (26 %)
    // and a 1440-high window 330 px (23 %). Narrow windows shrink it further (one row, below); the size handle
    // (WolfSkiHudScale, wolfhudscale.h) takes it down to 0.4 of that.
    const F32 want_h = llclamp(Hv * ui * 0.26f, 120.f, 330.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.30f);
    const F32 uH = llmin(want_h, max_h) / 304.f;
    const F32 one = llmin(uH, W / 1600.f);
    g.W = W;
    g.bandB = bottom;
    // [SKIS 2026-11-06] always ONE row, however narrow the window: the helm's two-row layout put the run's panel in the
    // middle of the screen, over the rider (session skis2, a 1100 x 734 window); narrower just draws it all smaller
    g.L.u = g.C.u = g.R.u = one;
    g.L.top0 = g.C.top0 = g.R.top0 = bottom + 900.f * one;
    g.L.ox = 0.f;
    g.C.ox = W * 0.5f - 800.f * one;
    g.R.ox = W - 1600.f * one;
    g.panelTop = g.C.top0 - 596.f * g.C.u;
    mPanelB = screen_bottom;
    mPanelT = g.panelTop * zoom;
    mDrawn = true;

    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;
    zoomed.emplace(zoom);
    Ctx c{ [this](F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip) { addHit(l, b, r, t, id, tip); }, mPressed };
    const WolfSkis::Shown& s = sk.shown();
    const bool mph = sk.mph();
    const F32 k = mph ? 2.23694f : 3.6f;          // m/s -> mph (1 mph = 0.44704 m/s exactly) / km/h
    const char* unit = mph ? "MPH" : "KM/H";
    const F64 t = now();
    const bool bg = gSavedSettings.getBOOL("WolfSkiHudBackground");
    INSET = bg ? INSET_ON : INSET_OFF;

    // the band: night blue, a frosted top edge - or nothing at all, the read-outs floating over the world (PANEL)
    use(g.L);
    if (bg)
    {
        rectG(0.f, g.bandB, W, g.panelTop, NAVY_HI, NAVY_LO);
        rectF(0.f, g.panelTop - S(4.f), W, g.panelTop, alpha(ICE, 0.35f));
        rectF(0.f, g.panelTop - S(2.f), W, g.panelTop - S(1.f), alpha(SNOW, 0.8f));
    }

    // ── LEFT: the speedometer and the steepness ──
    use(g.L);
    {
        const F32 full = mph ? 90.f : 140.f;      // the dial's full scale (about the tucked top speed, tuning.py)
        const F32 v = s.mSpeed * k;
        mNeedle += (v - mNeedle) * llclamp((F32)LLFrameTimer::getFrameDeltaTimeF32() * 6.f, 0.f, 1.f);
        const F32 cx = X(170.f), cy = YM(752.f), r = S(124.f);
        circleF(cx, cy, r * 1.08f, INSET, 64);
        auto ang = [&](F32 x) { return -135.f + 270.f * llclamp(x / full, 0.f, 1.02f); };
        // the band of the dial: ice to gold to red as it gets fast
        clockArc(cx, cy, r * 0.9f, S(10.f), -135.f, ang(full * 0.5f), alpha(ICE, 0.35f), 32);
        clockArc(cx, cy, r * 0.9f, S(10.f), ang(full * 0.5f), ang(full * 0.75f), alpha(GOLD, 0.45f), 16);
        clockArc(cx, cy, r * 0.9f, S(10.f), ang(full * 0.75f), 135.f, alpha(RED, 0.5f), 16);
        clockArc(cx, cy, r * 0.9f, S(10.f), -135.f, ang(mNeedle), mNeedle > full * 0.75f ? RED : mNeedle > full * 0.5f ? GOLD : ICE, 48);
        const S32 step = mph ? 10 : 20;
        for (S32 m = 0; m <= (S32)full; m += step / 2)
        {
            F32 x0, y0, x1, y1;
            const bool major = m % step == 0;
            pt(cx, cy, r * (major ? 0.70f : 0.76f), ang((F32)m), x0, y0);
            pt(cx, cy, r * 0.80f, ang((F32)m), x1, y1);
            lineW(x0, y0, x1, y1, major ? 2.f : 1.f, alpha(SNOW, 0.8f));
            if (major)
            {
                F32 tx, ty;
                pt(cx, cy, r * 0.58f, ang((F32)m), tx, ty);
                T(tx, ty, llformat("%d", m), 10.f, GREY, false, true);
            }
        }
        // top speed this run: a gold tick on the dial
        if (s.mTop > 0.5f)
        {
            F32 x0, y0, x1, y1;
            pt(cx, cy, r * 0.82f, ang(s.mTop * k), x0, y0);
            pt(cx, cy, r * 1.0f, ang(s.mTop * k), x1, y1);
            lineW(x0, y0, x1, y1, S(4.f), GOLD);
        }
        {
            F32 nx, ny;
            pt(cx, cy, r * 0.84f, ang(mNeedle), nx, ny);
            lineW(cx, cy, nx, ny, S(4.f), SNOW);
            circleShaded(cx, cy, S(10.f), SNOW, ICE_DIM, 20);
        }
        T(cx, cy - S(40.f), llformat("%.0f", v), 34.f, SNOW, true, true);
        roundRectF(cx - S(34.f), cy - S(84.f), cx + S(34.f), cy - S(62.f), S(6.f), alpha(ICE, 0.25f), alpha(ICE, 0.12f));
        T(cx, cy - S(73.f), unit, 10.f, ICE, true);
        addHit(cx - S(40.f), cy - S(88.f), cx + S(40.f), cy - S(58.f), H_UNITS,
               mph ? "Speeds in miles an hour. Click for kilometres an hour." : "Speeds in kilometres an hour. Click for miles an hour.");
        if (!bg) roundRectF(cx - S(80.f), YM(898.f), cx + S(80.f), YM(876.f), S(6.f), INSET, INSET);
        T(cx, YM(888.f), s.mTop > 0.1f ? llformat("TOP %.0f %s", s.mTop * k, unit) : std::string("TOP --"), 11.f, GOLD, true);

        // the steepness, as the piste markers show it, with a little slope you can see
        const F32 bx = 330.f;
        roundRectF(X(bx), YM(724.f), X(550.f), YM(612.f), S(8.f), INSET, INSET);
        T(X(bx + 12.f), YM(628.f), "STEEPNESS", 9.f, GREY, true, false, LLFontGL::LEFT);
        std::string word;
        LLColor4 chip;
        piste(s.mSlopeDeg, word, chip);
        {
            // the slope drawn at its angle (doubled, so a gentle one still reads), a skier dot on it
            const F32 ox = X(bx + 18.f), oy = YM(708.f), len = S(84.f);
            const F32 a = llmin(s.mSlopeDeg * 2.f, 70.f) * DEG_TO_RAD;
            const F32 ex = ox + len * cosf(a), ey = oy + len * sinf(a);
            triF(ox, oy, ex, oy, ex, ey, alpha(SNOW, 0.85f));
            lineW(ox, oy, ex, ey, S(2.f), chip.mV[VRED] + chip.mV[VGREEN] + chip.mV[VBLUE] < 0.3f ? GREY : chip);
            circleF(ox + (ex - ox) * 0.6f, oy + (ey - oy) * 0.6f + S(5.f), S(4.5f), RED, 12);
        }
        T(X(bx + 200.f), YM(658.f), s.mSlopeKnown ? llformat("%.0f\xC2\xB0", s.mSlopeDeg) : std::string("--"), 24.f, SNOW, true, true, LLFontGL::RIGHT);
        roundRectF(X(bx + 112.f), YM(714.f), X(bx + 208.f), YM(688.f), S(5.f), chip, chip);
        frameW(X(bx + 112.f), YM(714.f), X(bx + 208.f), YM(688.f), 1.f, alpha(SNOW, 0.7f));
        T(X(bx + 160.f), YM(701.f), word, 10.f, SNOW, true);

        // air time: live while flying, else this run's longest
        roundRectF(X(bx), YM(856.f), X(550.f), YM(736.f), S(8.f), s.mInAir ? alpha(GOLD, 0.25f) : INSET, INSET);
        T(X(bx + 12.f), YM(752.f), s.mInAir ? "IN THE AIR" : "BIGGEST AIR", 9.f, s.mInAir ? GOLD : GREY, true, false, LLFontGL::LEFT);
        const F32 air = s.mInAir ? s.mAirNow : s.mBigAir;
        T(X(bx + 110.f), YM(800.f), air > 0.f ? llformat("%.1f s", air) : std::string("--"), 26.f, s.mInAir ? GOLD : SNOW, true, true);
        T(X(bx + 110.f), YM(840.f), s.mBestAir > 0.f ? llformat("best here %.1f s", s.mBestAir) : std::string("Page Up (E) to jump"), 9.f, GREY);
    }

    // ── CENTRE: the run ──
    use(g.C);
    {
        roundRectF(X(C_L), YM(642.f), X(C_R), YM(606.f), S(8.f), INSET, INSET);
        const std::string mw = modeWords(s, sk.kind());
        const LLColor4 mc = s.mWiped ? AMBER : s.mInAir ? GOLD : s.mRunning ? ICE : SNOW;
        fitT(X((C_L + C_R) * 0.5f), YM(624.f), mw, 13.f, 9.f, S(C_R - C_L - 20.f), mc, true);
        if (!sk.answered())
        {
            fitT(X((C_L + C_R) * 0.5f), YM(652.f), "(these skis do not talk to the HUD: jumps and tricks are not scored)", 8.f, 7.f,
                 S(C_R - C_L - 20.f), GREY);
        }
        // the clock (its own backing when the band is off)
        if (!bg) roundRectF(X(C_L + 70.f), YM(756.f), X(C_R - 70.f), YM(664.f), S(10.f), INSET, INSET);
        const F32 secs = s.mRunning ? s.mRunSecs : (s.mHaveLast ? s.mLastSecs : 0.f);
        T(X((C_L + C_R) * 0.5f), YM(706.f), clockText(secs), 44.f, s.mRunning ? SNOW : alpha(SNOW, 0.75f), true, true);
        T(X((C_L + C_R) * 0.5f), YM(746.f), s.mRunning ? "RUN TIME" : (s.mHaveLast ? "LAST RUN" : "RUN TIME"), 9.f, GREY, true);
        // vertical and top speed of the run
        const F32 vert = s.mRunning ? s.mVertical : (s.mHaveLast ? s.mLastVertical : 0.f);
        roundRectF(X(C_L + 20.f), YM(860.f), X(C_L + 190.f), YM(770.f), S(8.f), INSET, INSET);
        T(X(C_L + 105.f), YM(788.f), "VERTICAL", 9.f, GREY, true);
        T(X(C_L + 105.f), YM(824.f), llformat("%.0f m", vert), 22.f, SNOW, true, true);
        roundRectF(X(C_R - 190.f), YM(860.f), X(C_R - 20.f), YM(770.f), S(8.f), INSET, INSET);
        T(X(C_R - 105.f), YM(788.f), "HEADING", 9.f, GREY, true);
        {
            static const char* const PTS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            S32 i = ((S32)llround(s.mHeading / 45.f)) % 8;
            if (i < 0) i += 8;
            T(X(C_R - 132.f), YM(824.f), PTS[i], 20.f, ICE, true);
            // a small compass needle pointing the way they face (up = north)
            const F32 ccx = X(C_R - 64.f), ccy = YM(822.f), cr = S(24.f);
            arcW(ccx, ccy, cr, S(2.f), 0.f, 2.f * PI_F, alpha(SNOW, 0.6f), 32);
            F32 hx, hy, bx2, by2;
            pt(ccx, ccy, cr * 0.85f, s.mHeading, hx, hy);
            pt(ccx, ccy, cr * 0.5f, s.mHeading + 180.f, bx2, by2);
            lineW(bx2, by2, hx, hy, S(3.f), ICE);
            circleF(hx, hy, S(3.5f), RED, 10);
            T(ccx, ccy + cr + S(8.f), "N", 8.f, GREY, true);
        }
        if (!bg) roundRectF(X(C_L + 10.f), YM(898.f), X(C_R - 10.f), YM(878.f), S(6.f), INSET, INSET);
        T(X((C_L + C_R) * 0.5f), YM(888.f),
          sk.kind() == WolfSkis::KIND_SNOWBOARD ? "Left/Right carve  -  Down brake  -  Up push  -  E jump  -  C tuck"
                                                : "Left/Right carve  -  Down snowplough  -  Up skate  -  E jump  -  C tuck",
          9.f, GREY);
    }

    // ── RIGHT: style, the best here, the buttons ──
    use(g.R);
    {
        roundRectF(X(R_L), YM(760.f), X(R_L + 300.f), YM(606.f), S(10.f), alpha(GOLD, 0.14f), INSET);
        T(X(R_L + 14.f), YM(622.f), "STYLE", 9.f, GOLD, true, false, LLFontGL::LEFT);
        const S32 score = s.mRunning ? s.mScore : (s.mHaveLast ? s.mLastScore : 0);
        T(X(R_L + 150.f), YM(676.f), thousands(score), 38.f, SNOW, true, true);
        if (s.mCombo >= 2 && s.mRunning)
        {
            const F32 pulse = 0.75f + 0.25f * sinf((F32)fmod(t, 10.0) * 8.f);
            roundRectF(X(R_L + 220.f), YM(752.f), X(R_L + 292.f), YM(718.f), S(8.f), alpha(GOLD, pulse), alpha(AMBER, pulse));
            T(X(R_L + 256.f), YM(735.f), llformat("x%d", s.mCombo), 15.f, NAVY_LO, true);
        }
        T(X(R_L + 14.f), YM(736.f), s.mRunning ? "points this run" : (s.mHaveLast ? (s.mLastBest ? "last run - a new best!" : "last run") : "jump, spin, carve"),
          9.f, s.mLastBest && !s.mRunning ? GOLD : GREY, false, false, LLFontGL::LEFT);
        // the best here, on this computer
        if (!bg) roundRectF(X(R_L), YM(820.f), X(R_L + 300.f), YM(768.f), S(8.f), INSET, INSET);
        T(X(R_L + 14.f), YM(782.f), "BEST HERE", 9.f, GREY, true, false, LLFontGL::LEFT);
        const std::string best = s.mBestScore > 0 || s.mBestTop > 0.f
            ? llformat("%s pts   %.0f %s   %.1f s air", thousands(s.mBestScore).c_str(), s.mBestTop * k, unit, s.mBestAir)
            : std::string("no runs here yet");
        fitT(X(R_L + 150.f), YM(806.f), best, 11.f, 8.f, S(290.f), SNOW, true);

        // the buttons: big and clear (memory paul-simple-for-older-disabled-users)
        button(c, X(R_L + 320.f), YM(700.f), X(1588.f), YM(612.f), sk.kind() == WolfSkis::KIND_SNOWBOARD ? "TAKE IT OFF" : "TAKE THEM OFF",
               H_TAKE_OFF, "Stand up and take them off. They go back to your inventory, ready for next time.", 14.f,
               LLColor4(0.20f, 0.42f, 0.66f, 0.97f), LLColor4(0.08f, 0.18f, 0.32f, 0.97f));
        button(c, X(R_L + 320.f), YM(780.f), X(R_L + 395.f), YM(716.f), "KEYS", H_HELP, "Show the keys and how the score works", 11.f,
               LLColor4(0.16f, 0.26f, 0.38f, 0.97f), LLColor4(0.06f, 0.11f, 0.18f, 0.97f));
        // Source: wolfhelmhud.cpp PANEL (WolfHelmHudBackground) - the same switch, for this HUD's own setting
        button(c, X(R_L + 401.f), YM(780.f), X(R_L + 476.f), YM(716.f), "PANEL", H_PANEL,
               bg ? "Background on: click to float the read-outs over the world" : "Background off: click to put the panel back", 11.f,
               bg ? LLColor4(0.20f, 0.42f, 0.66f, 0.97f) : LLColor4(0.16f, 0.26f, 0.38f, 0.97f), LLColor4(0.06f, 0.11f, 0.18f, 0.97f));
        button(c, X(R_L + 482.f), YM(780.f), X(1588.f), YM(716.f), "HIDE", H_HIDE,
               "Hide this HUD while you ride (Wolf World > Skis and Snowboards... brings it back)", 12.f,
               LLColor4(0.16f, 0.26f, 0.38f, 0.97f), LLColor4(0.06f, 0.11f, 0.18f, 0.97f));
        if (bg) T(X(1588.f), YM(860.f), sk.kind() == WolfSkis::KIND_SNOWBOARD ? "WOLF SNOWBOARD" : "WOLF SKIS", 10.f, ICE_DIM, true, false, LLFontGL::RIGHT);
    }

    drawPopups(g.panelTop, W);
    if (mShowHelp) drawHelp(g.panelTop, W);

    zoomed.reset();
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mResize.draw();
}

// The call-outs: big and friendly above the band's middle, floating up and fading over WolfSkis' 3 s.
void WolfSkiHud::drawPopups(F32 band_top, F32 width)
{
    const std::vector<WolfSkis::Popup>& pops = WolfSkis::instance().popups();
    if (pops.empty()) return;
    use(g.C);
    const F64 t = now();
    F32 y = band_top + S(60.f);
    for (auto it = pops.rbegin(); it != pops.rend(); ++it)
    {
        const F32 age = (F32)(t - it->mAt);
        const F32 fade = age < 2.f ? 1.f : llmax(0.f, 1.f - (age - 2.f));
        const F32 pop = age < 0.15f ? 0.6f + age / 0.15f * 0.4f : 1.f;      // a quick swell as it arrives
        const F32 rise = llmin(age, 3.f) * S(10.f);
        const LLColor4 col = it->mTone == 1 ? GOLD : it->mTone == 2 ? AMBER : SNOW;
        const F32 cx = width * 0.5f;
        const F32 w = S(560.f), h = S(it->mSub.empty() ? 50.f : 76.f);
        roundRectF(cx - w * 0.5f, y + rise, cx + w * 0.5f, y + rise + h, S(14.f), alpha(NAVY_HI, 0.85f * fade), alpha(NAVY_LO, 0.85f * fade));
        frameW(cx - w * 0.5f, y + rise, cx + w * 0.5f, y + rise + h, 1.5f, alpha(col, 0.7f * fade));
        fitT(cx, y + rise + h - S(26.f), it->mText, 26.f * pop, 14.f, w - S(20.f), alpha(col, fade), true);
        if (!it->mSub.empty()) fitT(cx, y + rise + S(18.f), it->mSub, 12.f, 8.f, w - S(20.f), alpha(SNOW, 0.9f * fade));
        y += h + S(10.f);
    }
}

void WolfSkiHud::drawHelp(F32 band_top, F32 width)
{
    static const HelpLine LINES[] =
    {
        { 0, "RIDING" },
        { 1, "Point them downhill and you go: the steeper, the faster." },
        { 1, "Left / Right (A / D): carve a turn - at speed, or pivot when slow." },
        { 1, "Down (S): brake - a snowplough on skis, a heel-side skid on the board." },
        { 1, "Up (W): push along on the flat.   Page Up / E: jump.   Page Down / C: tuck." },
        { 1, "Left / Right in the air: spin.  Land along your skis, or you wipe out (and get up)." },
        { 0, "STYLE POINTS" },
        { 1, "100 a second in the air, more for a 180, 360 or 540 - landed cleanly." },
        { 1, "Linked carved turns build a combo: left, right, left... x2, x3 ..." },
        { 1, "Every second faster than 72 km/h (45 mph). Your best here is kept on this computer." },
        { 0, "Click KEYS again to close this." },
    };
    use(g.C);
    const F32 w = S(760.f), lh = S(22.f);
    const F32 h = lh * (F32)(sizeof(LINES) / sizeof(LINES[0])) + S(20.f);
    const F32 l = width * 0.5f - w * 0.5f, b = band_top + S(14.f);
    roundRectF(l, b, l + w, b + h, S(12.f), alpha(NAVY_HI, 0.96f), alpha(NAVY_LO, 0.96f));
    frameW(l, b, l + w, b + h, 1.5f, alpha(ICE, 0.7f));
    F32 y = b + h - S(20.f);
    for (const HelpLine& hl : LINES)
    {
        T(l + S(20.f), y, hl.s, hl.kind == 0 ? 12.f : 11.f, hl.kind == 0 ? ICE : SNOW, hl.kind == 0, false, LLFontGL::LEFT);
        y -= lh;
    }
}

bool WolfSkiHud::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return true;   // the panel itself: not a click on the world behind it
    }
    mPressed = hit->mId;
    gFocusMgr.setMouseCapture(this);
    press(hit->mId);
    return true;
}

bool WolfSkiHud::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE || mResize.dragging() || mResize.onGrip(x, y);
    mResize.end();
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    mPressed = H_NONE;
    return was || onPanel(x, y);
}

bool WolfSkiHud::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mResize.dragging())
    {
        mResize.drag(y);
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && mDrawn && mResize.onGrip(x, y))
    {
        mResize.hovered();
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && !onPanel(x, y))
    {
        return false;
    }
    getWindow()->setCursor(hitAt(x, y) ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfSkiHud::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfSkiHud::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    return handleMouseDown(x, y, mask);
}

bool WolfSkiHud::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        if (!hit->mTip.empty()) LLToolTipMgr::instance().show(hit->mTip);
    }
    return true;
}

void WolfSkiHud::onMouseCaptureLost()
{
    mResize.end();
    mPressed = H_NONE;
    LLUICtrl::onMouseCaptureLost();
}
