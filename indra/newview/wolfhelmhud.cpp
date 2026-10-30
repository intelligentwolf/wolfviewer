/**
 * @file wolfhelmhud.cpp
 * @brief WolfViewer: the Captain's Helm HUD. See wolfhelmhud.h, wolfhelm.h and
 *        region-terrain-work/pirate-ship/DESIGN.md s20.
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

#include "wolfhelmhud.h"

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
#include "wolfhelm.h"

static LLDefaultChildRegistry::Register<WolfHelmHud> r_wolf_helm_hud("wolf_helm_hud");

namespace
{
    const F32 PI_F = 3.14159265f;

    // Status colours as the dashboard's (ISO 2575, wolfdashboard.cpp): red act now, amber caution, green on.
    const LLColor4 C_RED(1.00f, 0.13f, 0.10f, 1.f);
    const LLColor4 C_AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 C_GREEN(0.20f, 0.95f, 0.30f, 1.f);
    const LLColor4 C_WHITE(0.96f, 0.96f, 0.96f, 1.f);
    const LLColor4 C_GREY(0.66f, 0.64f, 0.60f, 1.f);
    const LLColor4 C_DIM(0.36f, 0.33f, 0.30f, 1.f);
    const LLColor4 C_TEAL(0.25f, 0.85f, 0.90f, 1.f);
    // The ship's own: dark oak, brass, a parchment card (design, after a ship's binnacle and a captain's cabin).
    const LLColor4 OAK_HI(0.20f, 0.13f, 0.08f, 1.f), OAK_LO(0.055f, 0.033f, 0.02f, 1.f);
    const LLColor4 OAK_INSET(0.035f, 0.022f, 0.014f, 1.f);
    const LLColor4 BRASS(0.86f, 0.69f, 0.34f, 1.f);
    const LLColor4 BRASS_HI(1.00f, 0.89f, 0.58f, 1.f), BRASS_LO(0.40f, 0.28f, 0.11f, 1.f);
    const LLColor4 PARCH_HI(0.96f, 0.92f, 0.79f, 1.f), PARCH_LO(0.78f, 0.69f, 0.51f, 1.f);
    const LLColor4 INK(0.17f, 0.11f, 0.06f, 1.f);
    const LLColor4 WOOD_HI(0.55f, 0.33f, 0.16f, 1.f), WOOD_LO(0.26f, 0.14f, 0.06f, 1.f);
    const LLColor4 SAIL_SET(0.97f, 0.94f, 0.83f, 1.f);
    const LLColor4 SAIL_FURLED(0.45f, 0.39f, 0.31f, 1.f);

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    bool blink(F32 period = 0.8f) { return fmod(now(), (F64)period) < period * 0.5f; }
    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    // ---- primitives, view-local, untextured (after wolfdashboard.cpp's, itself after wolfflightdeck.cpp's) ----
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
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }

    // A lit dome: brighter toward the upper left (wolfdashboard.cpp circleShaded).
    void circleShaded(F32 cx, F32 cy, F32 rad, const LLColor4& hi, const LLColor4& lo, S32 seg = 48)
    {
        noTex();
        const F32 hx = cx - rad * 0.3f, hy = cy + rad * 0.3f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(hx, hy, hi);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), lo);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), lo);
        }
        gGL.end();
    }

    // A stroked arc, radians counter-clockwise from +x.
    void arcW(F32 cx, F32 cy, F32 rad, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48)
    {
        noTex();
        const F32 h = w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
            const F32 c0 = cosf(u0), s0 = sinf(u0), c1 = cosf(u1), s1 = sinf(u1);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c0, cy + (rad + h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c); vtx(cx + (rad - h) * c1, cy + (rad - h) * s1, c);
        }
        gGL.end();
    }

    void roundRectF(F32 l, F32 b, F32 r, F32 t, F32 rad, const LLColor4& top, const LLColor4& bot)
    {
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
                const F32 a0 = PI_F * 0.5f * (k + i / 6.f), a1 = PI_F * 0.5f * (k + (i + 1) / 6.f);
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
        WolfHudScale::text(f, s, x, y, c, h, v);   // sharp at the size the captain set (wolfhudscale.h)
    }

    // Source: wolfdashboard.cpp face() - the B612 face (fonts.xml Avionics0..11) at the largest size whose line fits px.
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
                LL_WARNS("WolfHelmHud") << "font " << name << " missing, using the default" << LL_ENDL;
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
    // THE LAYOUT, in the dashboards' frame (wolfdashboard.cpp "THE LAYOUT"): 1600 x 900 with the panel from y 596 to
    // 900 (y down), scaled by u. The wheel and sail plan keep their distance from the left edge (L), the compass and
    // log stay centred (C), the wind and battle sections keep theirs from the right edge (R). A window too narrow for
    // one row at a readable size puts the centre on a row of its own above.
    // ════════════════════════════════════════════════════════════════════════════════════
    struct Anchor { F32 ox = 0.f, top0 = 900.f, u = 1.f; };
    struct Geo
    {
        F32 W = 1600.f;
        Anchor L, C, R, cur;
        F32 us = 1.f;
        F32 bandB = 0.f;
        F32 panelTop = 304.f;
        bool compact = false;
    } g;
    // Source: wolfdashboard.cpp U_MIN, US_MIN, US_MAX.
    const F32 U_MIN = 0.75f;
    const F32 US_MIN = 0.85f, US_MAX = 1.3f;
    // The parts in mock units: L 0..612, C 612..1050, R 1056..1600 - side by side, never over each other, in one row.
    const F32 L_W = 612.f, C_L = 612.f, C_R = 1050.f, R_L = 1056.f;
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
    // A filled wedge between two clock angles.
    void clockWedge(F32 cx, F32 cy, F32 r, F32 d0, F32 d1, const LLColor4& c, S32 seg = 24)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            F32 x0, y0, x1, y1;
            pt(cx, cy, r, d0 + (d1 - d0) * i / seg, x0, y0);
            pt(cx, cy, r, d0 + (d1 - d0) * (i + 1) / seg, x1, y1);
            vtx(cx, cy, c); vtx(x0, y0, c); vtx(x1, y1, c);
        }
        gGL.end();
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

    // Centred text that must fit `maxw` (picture units): a size smaller at a time down to `min_px`, then cut with "...".
    // The ship's status lines run long ("In irons: the wind is 40° off the bow - turn her ...").
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
                while (!str.empty() && ((unsigned char)str.back() & 0xC0) == 0x80) str.pop_back();   // whole UTF-8 characters
                if (!str.empty() && (unsigned char)str.back() >= 0xC0) str.pop_back();
            }
            str += "...";
        }
        text(f, str, x, y, c, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    // An arrow from (x0, y0) to its head at (x1, y1).
    void arrow(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, F32 head, const LLColor4& c)
    {
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 1.f) return;
        const F32 ux = dx / len, uy = dy / len;
        lineW(x0, y0, x1 - ux * head * 0.8f, y1 - uy * head * 0.8f, w, c);
        triF(x1, y1, x1 - ux * head - uy * head * 0.55f, y1 - uy * head + ux * head * 0.55f,
             x1 - ux * head + uy * head * 0.55f, y1 - uy * head - ux * head * 0.55f, c);
    }

    // ---- controls ----
    struct Ctx
    {
        std::function<void(F32, F32, F32, F32, WolfHelmHud::EHit, const std::string&)> hit;
        S32 pressed;
    };

    void button(Ctx& c, F32 l, F32 b, F32 r, F32 t, const std::string& label, bool lit, WolfHelmHud::EHit id,
                const std::string& tip, F32 px = 12.f, bool enabled = true, const LLColor4& lit_col = C_AMBER)
    {
        const bool down = c.pressed == id;
        roundRectF(l, b, r, t, (t - b) * 0.22f,
                   down ? LLColor4(0.09f, 0.06f, 0.04f, 0.97f) : LLColor4(0.30f, 0.21f, 0.13f, 0.97f),
                   down ? LLColor4(0.16f, 0.11f, 0.07f, 0.97f) : LLColor4(0.11f, 0.07f, 0.045f, 0.97f));
        frameW(l, b, r, t, 1.2f, lit ? lit_col : enabled ? alpha(BRASS, 0.75f) : C_DIM);
        if (lit)
        {
            rectF(l + S(4.f), t - S(5.5f), r - S(4.f), t - S(3.f), lit_col);   // the lit bar: never colour of text alone
        }
        T((l + r) * 0.5f, (b + t) * 0.5f, label, px, !enabled ? C_DIM : lit ? lit_col : C_WHITE, true);
        c.hit(l, b, r, t, id, tip);
    }

    // ---- the ship's wheel: eight spokes with handles, a wooden rim, a brass hub; `turn` degrees, + = to port (anti-
    //      clockwise as you look at it, as a ship's wheel put over to port) ----
    void shipsWheel(F32 cx, F32 cy, F32 R, F32 turn, bool enabled)
    {
        const LLColor4 wood_hi = enabled ? WOOD_HI : alpha(WOOD_HI, 0.5f), wood_lo = enabled ? WOOD_LO : alpha(WOOD_LO, 0.5f);
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 a = -turn + i * 45.f;
            F32 x0, y0, x1, y1, x2, y2;
            pt(cx, cy, R * 0.18f, a, x0, y0);
            pt(cx, cy, R * 1.0f, a, x1, y1);
            pt(cx, cy, R * 1.24f, a, x2, y2);
            lineW(x0, y0, x1, y1, S(9.f), wood_lo);
            lineW(x0, y0, x1, y1, S(5.f), wood_hi);
            lineW(x1, y1, x2, y2, S(11.f), wood_lo);   // the handle
            lineW(x1, y1, x2, y2, S(6.f), wood_hi);
            circleF(x2, y2, S(6.f), wood_hi, 12);
            if (i == 0)
            {
                // the king spoke: brass-capped, so the wheel's turn reads at a glance (amidships = at the top)
                circleF(x2, y2, S(6.5f), BRASS, 12);
            }
        }
        arcW(cx, cy, R * 0.92f, S(16.f), 0.f, 2.f * PI_F, wood_lo, 64);
        arcW(cx, cy, R * 0.92f, S(10.f), 0.f, 2.f * PI_F, wood_hi, 64);
        arcW(cx, cy, R * 0.92f, S(1.5f), 0.f, 2.f * PI_F, alpha(BRASS, 0.7f), 64);
        circleShaded(cx, cy, R * 0.2f, BRASS_HI, BRASS_LO, 32);
        circleF(cx, cy, R * 0.07f, OAK_LO, 16);
    }

    // ---- the sail plan: the ship from the side, bow to the right; every sail of the sails script's table
    //      (hms_wolfy_sails.lsl SAILS: the lowest setting each is set at) drawn set or furled ----
    struct SailDraw { const char* name; S32 from; S32 mast; F32 y0, y1, w0, w1; };   // mast: 0 fore 1 main 2 mizzen
    // Source: out/frigate_pallas/lsl/hms_wolfy_sails.lsl SAILS (royal 3, topgallant 2, topsail 1, course 2 on the
    // fore and main; mizzen topsail 1; spanker 1; fore / main topmast staysails 2; jib 1; flying jib 3). The
    // heights (mock units, y down) are the drawing's.
    const SailDraw SQUARE[] =
    {
        { "fore course", 2, 0, 708.f, 744.f, 58.f, 66.f }, { "fore topsail", 1, 0, 672.f, 704.f, 46.f, 56.f },
        { "fore topgallant", 2, 0, 646.f, 668.f, 36.f, 44.f }, { "fore royal", 3, 0, 628.f, 643.f, 28.f, 34.f },
        { "main course", 2, 1, 704.f, 744.f, 64.f, 72.f }, { "main topsail", 1, 1, 664.f, 700.f, 50.f, 62.f },
        { "main topgallant", 2, 1, 636.f, 660.f, 40.f, 48.f }, { "main royal", 3, 1, 617.f, 633.f, 30.f, 38.f },
        { "mizzen topsail", 1, 2, 680.f, 708.f, 38.f, 46.f },
    };

    void sailPlan(F32 l, F32 r, S32 setting, bool enabled)
    {
        // masts at fixed fractions of the box (fore, main, mizzen), the hull along the bottom
        const F32 mx[3] = { X(l + (r - l) * 0.68f), X(l + (r - l) * 0.47f), X(l + (r - l) * 0.27f) };
        const F32 top_y[3] = { YM(624.f), YM(612.f), YM(650.f) };
        const F32 deck = YM(760.f);
        const LLColor4 spar = enabled ? LLColor4(0.36f, 0.24f, 0.13f, 1.f) : C_DIM;
        // the hull: a sheer line, a gun stripe, the waterline
        quadF(X(l + 6.f), YM(752.f), X(r - 2.f), YM(748.f), X(r - 30.f), YM(790.f), X(l + 22.f), YM(790.f), LLColor4(0.16f, 0.10f, 0.06f, 1.f));
        quadF(X(l + 8.f), YM(758.f), X(r - 8.f), YM(755.f), X(r - 14.f), YM(763.f), X(l + 10.f), YM(766.f), alpha(BRASS, 0.85f));
        for (S32 i = 0; i < 9; ++i)
        {
            const F32 gx = X(l + 40.f + i * (r - l - 90.f) / 8.f);
            rectF(gx, YM(770.f), gx + S(6.f), YM(764.f), LLColor4(0.05f, 0.03f, 0.02f, 1.f));   // the gun ports
        }
        rectF(X(l + 18.f), YM(792.f), X(r - 26.f), YM(789.f), alpha(C_TEAL, 0.6f));
        // the bowsprit and jib boom
        lineW(X(r - 6.f), YM(752.f), X(r + 34.f), YM(730.f), S(3.f), spar);
        for (S32 m = 0; m < 3; ++m)
        {
            lineW(mx[m], deck, mx[m], top_y[m], S(3.5f), spar);
        }
        auto sailCol = [&](S32 from) { return setting >= from ? (enabled ? SAIL_SET : alpha(SAIL_SET, 0.45f)) : SAIL_FURLED; };
        for (const SailDraw& s : SQUARE)
        {
            const F32 cx = mx[s.mast];
            const F32 yt = YM(s.y0), yb = YM(s.y1);
            const bool set = setting >= s.from;
            lineW(cx - S(s.w0 * 0.5f + 3.f), yt, cx + S(s.w0 * 0.5f + 3.f), yt, S(2.f), spar);   // the yard
            if (set)
            {
                // the set sail, its foot bellied a little
                quadF(cx - S(s.w0 * 0.5f), yt, cx + S(s.w0 * 0.5f), yt, cx + S(s.w1 * 0.5f), yb, cx - S(s.w1 * 0.5f), yb, sailCol(s.from));
                triF(cx - S(s.w1 * 0.5f), yb, cx + S(s.w1 * 0.5f), yb, cx, yb - S(4.f), sailCol(s.from));
                lineW(cx - S(s.w1 * 0.5f), yb, cx + S(s.w1 * 0.5f), yb, S(1.f), alpha(INK, 0.35f));
            }
            else
            {
                lineW(cx - S(s.w0 * 0.5f), yt - S(2.f), cx + S(s.w0 * 0.5f), yt - S(2.f), S(4.f), SAIL_FURLED);   // furled on the yard
            }
        }
        // fore-and-aft: the spanker (1) abaft the mizzen, the staysails (2) and jibs (1, 3) forward
        auto fore_aft = [&](F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, S32 from)
        {
            if (setting >= from) triF(x1, y1, x2, y2, x3, y3, sailCol(from));
            else lineW(x1, y1, x2, y2, S(2.5f), SAIL_FURLED);
        };
        if (setting >= 1)
        {
            quadF(mx[2] - S(2.f), YM(676.f), mx[2] - S(2.f), YM(742.f), X(l + 6.f), YM(746.f), X(l + 20.f), YM(690.f), sailCol(1));
        }
        else
        {
            lineW(mx[2], YM(744.f), X(l + 6.f), YM(746.f), S(4.f), SAIL_FURLED);   // the spanker brailed to its boom
        }
        fore_aft(mx[0] + S(2.f), YM(668.f), X(r - 6.f), YM(752.f), mx[0] + S(30.f), YM(744.f), 2);   // fore topmast staysail
        fore_aft(mx[1] + S(2.f), YM(662.f), mx[0] - S(4.f), YM(742.f), mx[1] + S(26.f), YM(742.f), 2);   // main topmast staysail
        fore_aft(mx[0] + S(2.f), YM(648.f), X(r + 12.f), YM(741.f), mx[0] + S(44.f), YM(738.f), 1);   // jib
        fore_aft(mx[0] + S(2.f), YM(630.f), X(r + 32.f), YM(731.f), mx[0] + S(58.f), YM(726.f), 3);   // flying jib
    }

    // A health bar: label, fill by now / max (green, amber under 50%, red under 25%), the figure.
    void healthBar(F32 l, F32 r, F32 y, const char* label, F32 cur, F32 mx)
    {
        const F32 frac = mx > 0.f ? llclamp(cur / mx, 0.f, 1.f) : 0.f;
        const LLColor4 col = mx <= 0.f ? C_DIM : frac < 0.25f ? C_RED : frac < 0.5f ? C_AMBER : C_GREEN;
        T(X(l), y, label, 10.f, C_GREY, false, false, LLFontGL::LEFT);
        const F32 bl = X(l + 70.f), br = X(r - 40.f);
        rectF(bl, y - S(5.f), br, y + S(5.f), LLColor4(0.f, 0.f, 0.f, 0.6f));
        if (mx > 0.f) rectF(bl, y - S(5.f), bl + (br - bl) * frac, y + S(5.f), col);
        frameW(bl, y - S(5.f), br, y + S(5.f), 1.f, alpha(BRASS, 0.5f));
        T(X(r), y, mx > 0.f ? llformat("%d%%", (S32)llround(frac * 100.f)) : std::string("-"), 10.f, mx > 0.f ? C_WHITE : C_DIM, false, true, LLFontGL::RIGHT);
    }

    // The broadside button: round, with the reload ring filling clockwise as the guns load; flashes when fired.
    void broadside(Ctx& c, F32 cx, F32 cy, F32 r, const char* label, F32 frac, F32 left, F64 fired_at, bool live,
                   WolfHelmHud::EHit id, const std::string& tip)
    {
        const bool down = c.pressed == id;
        const bool ready = live && left <= 0.f;
        const bool flash = now() - fired_at < 0.6;
        arcW(cx, cy, r * 1.18f, S(6.f), 0.f, 2.f * PI_F, LLColor4(0.f, 0.f, 0.f, 0.6f), 48);
        if (live) clockArc(cx, cy, r * 1.18f, S(6.f), 0.f, 360.f * frac, ready ? C_GREEN : C_AMBER, 48);
        circleShaded(cx, cy, r, down ? LLColor4(0.3f, 0.05f, 0.03f, 1.f) : flash ? LLColor4(1.f, 0.75f, 0.3f, 1.f) : ready ? LLColor4(0.62f, 0.10f, 0.06f, 1.f) : LLColor4(0.25f, 0.18f, 0.12f, 1.f),
                     LLColor4(0.08f, 0.03f, 0.02f, 1.f), 40);
        arcW(cx, cy, r, S(2.f), 0.f, 2.f * PI_F, live ? BRASS : C_DIM, 40);
        T(cx, cy + S(8.f), label, 11.f, live ? C_WHITE : C_DIM, true);
        T(cx, cy - S(9.f), !live ? "-" : ready ? "FIRE" : llformat("%.0f s", left), 10.f, ready ? C_WHITE : C_AMBER, true, true);
        c.hit(cx - r, cy - r, cx + r, cy + r, id, tip);
    }

    const char* const SECTION_NAMES[WolfHelm::SECTION_COUNT] = { "BOW", "STERN", "PORT", "STBD", "WATERLINE", "RUDDER", "RIGGING" };

    std::string pointOfSail(const WolfHelm::State& s)
    {
        const char* tack = s.mPortTack ? " - PORT TACK" : " - STARBOARD TACK";
        if (s.mPos == "calm") return "BECALMED";
        if (s.mPos == "irons") return "IN IRONS";
        if (s.mPos == "close") return std::string("CLOSE-HAULED") + tack;
        if (s.mPos == "beam") return std::string("BEAM REACH") + tack;
        if (s.mPos == "broad") return std::string("BROAD REACH") + tack;
        if (s.mPos == "run") return "RUNNING BEFORE THE WIND";
        return "";
    }

    std::string compassPoint(F32 deg)
    {
        static const char* const PTS[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
        S32 i = ((S32)llround(deg / 22.5f)) % 16;
        if (i < 0) i += 16;
        return PTS[i];
    }

    // British English, the read-out's own words for the helm (hms_wolfy_helm_template.lsl ReadOut "helm 20° to port").
    std::string helmWords(F32 helm)
    {
        const S32 d = (S32)llround(helm);
        if (d > 0) return llformat("PORT %d\xC2\xB0", d);
        if (d < 0) return llformat("STARBOARD %d\xC2\xB0", -d);
        return "MIDSHIPS";
    }

    struct HelpLine { S32 kind; const char* s; };   // 0 heading, 1 line, 2 more of it, 3 gap
}

WolfHelmHud::WolfHelmHud(const Params& p)
:   LLUICtrl(p)
{
}

WolfHelmHud::~WolfHelmHud()
{
}

void WolfHelmHud::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ llmin(l, r), llmin(b, t), llmax(l, r), llmax(b, t), id, tip });
}

const WolfHelmHud::Hit* WolfHelmHud::hitAt(S32 x, S32 y) const
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

// Source: wolfdashboard.cpp onPanel - with the background on the whole band is solid; off, only the controls.
bool WolfHelmHud::onPanel(S32 x, S32 y) const
{
    if (!mDrawn) return false;
    if (hitAt(x, y)) return true;
    if (!gSavedSettings.getBOOL("WolfHelmHudBackground")) return false;
    return y >= mPanelB && y <= mPanelT;
}

void WolfHelmHud::refuse(const std::string& why)
{
    mRefused = why;
    mRefusedAt = now();
}

void WolfHelmHud::draw()
{
    WolfHelm& h = WolfHelm::instance();
    mHits.clear();
    mDrawn = false;
    if (!h.hudShowing() || !WolfHelm::onWolf())
    {
        mShowHelp = false;
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        mPressed = H_NONE;
        mResize.hide();
        return;
    }
    if (!gSavedSettings.getBOOL("WolfHelmHudHelpSeen") && h.shipAnswered())
    {
        gSavedSettings.setBOOL("WolfHelmHudHelpSeen", true);
        mShowHelp = true;   // the guide opens by itself the first time (as the dashboards')
    }

    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    // Source: wolfdashboard.cpp draw - sit on whatever the bottom toolbar leaves.
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
    // Source: wolfdashboard.cpp draw - the panel's 304 units are 30% of the window (170..380 px) times the UI scale,
    // never more than 55% of what the toolbar leaves.
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    const F32 want_h = llclamp(Hv * ui * 0.30f, 170.f, 380.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.55f);
    const F32 uH = llmin(want_h, max_h) / 304.f;
    const F32 one = llmin(uH, W / 1600.f);
    g.W = W;
    g.bandB = bottom;
    if (one >= llmin(uH, U_MIN))
    {
        g.compact = false;
        g.L.u = g.C.u = g.R.u = one;
        g.L.top0 = g.C.top0 = g.R.top0 = bottom + 900.f * one;
        g.L.ox = 0.f;
        g.C.ox = W * 0.5f - 800.f * one;
        g.R.ox = W - 1600.f * one;
    }
    else
    {
        // narrow: the wheel, sail plan, wind and battle on the bottom row, the compass and log on their own above
        g.compact = true;
        const F32 row_h = llmin(want_h, (Hv - screen_bottom) * 0.30f);
        const F32 u2 = llmin(row_h / 304.f, W / (L_W + (1600.f - R_L)));
        const F32 u1 = llmin(row_h / 304.f, W / (C_R - C_L + 20.f));
        g.L.u = g.R.u = u2;
        g.L.top0 = g.R.top0 = bottom + 900.f * u2;
        g.L.ox = 0.f;
        g.R.ox = W - 1600.f * u2;
        const F32 row1_bottom = bottom + 304.f * u2;
        g.C.u = u1;
        g.C.top0 = row1_bottom + 900.f * u1;
        g.C.ox = W * 0.5f - 800.f * u1;
    }
    g.panelTop = g.C.top0 - 596.f * g.C.u;
    g.us = llclamp(llmax(g.C.u, US_MIN), US_MIN, US_MAX);
    mPanelB = screen_bottom;
    mPanelT = g.panelTop * zoom;
    mDrawn = true;

    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;
    zoomed.emplace(zoom);
    const bool bg = gSavedSettings.getBOOL("WolfHelmHudBackground");
    Ctx c{ [this](F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip) { addHit(l, b, r, t, id, tip); }, mPressed };
    const WolfHelm::State& s = h.state();
    const bool ready = h.shipAnswered() && s.mHaveState;
    const bool captain = h.isCaptain();
    const bool orders = ready && captain;   // only the captain gives the ship's orders (the helm script's rule)
    const F64 t = now();

    if (bg)
    {
        rectG(0.f, g.bandB, W, g.panelTop, OAK_HI, OAK_LO);
        // planking: a seam every so often, and a brass rail along the top
        use(g.L);
        for (F32 y = g.bandB + S(38.f); y < g.panelTop - S(10.f); y += S(38.f))
        {
            rectF(0.f, y, W, y + 1.f, LLColor4(0.f, 0.f, 0.f, 0.22f));
        }
        rectF(0.f, g.panelTop - S(4.f), W, g.panelTop, BRASS_LO);
        rectF(0.f, g.panelTop - S(2.f), W, g.panelTop - S(1.f), alpha(BRASS_HI, 0.8f));
    }

    // ── the empty states: no ship yet, or one that does not answer ──────────────────────────────
    if (!ready)
    {
        use(g.C);
        std::string msg, sub;
        LLColor4 col = C_WHITE;
        if (!h.lastError().empty())
        {
            msg = h.lastError();
            col = C_AMBER;
        }
        else if (!h.seatedOnShip())
        {
            msg = "Sit at the wheel of a ship to take the helm.";
            sub = "Right-click the wheel > Sit Here. The Captain's Helm opens by itself for a ship that speaks it (HMS Wolfy).";
        }
        else if (h.shipAnswered())
        {
            msg = "Reading the ship's log...";   // she answered; her state follows at once
        }
        else if (h.hailing())
        {
            msg = "Hailing the ship...";
            sub = "No answer yet: a Wolf ship's helm script answers within a few seconds. Other vessels do not speak the Captain's Helm.";
            col = C_AMBER;
        }
        else
        {
            msg = "This vessel does not speak the Captain's Helm.";
            sub = "Its helm script did not answer. Sailing Mode (Wolf World > Vehicles & Travel) works with any boat.";
        }
        const F32 room = g.W - 2.f * S(20.f);   // the empty states have the whole width
        fitT(g.W * 0.5f, YM(720.f), msg, 18.f, 12.f, room, col, true);
        if (!sub.empty()) fitT(g.W * 0.5f, YM(760.f), sub, 11.f, 8.f, room, C_GREY);
    }
    else
    {
        // ── LEFT: the wheel, the sail plan, its buttons ─────────────────────────────────────────
        use(g.L);
        const F32 helm = h.helmShown();
        // the wheel eases toward the helm (6 x the rudder: hard over, 35 degrees, is 210 degrees of wheel)
        const F32 want_turn = helm * 6.f;
        mWheelShown += (want_turn - mWheelShown) * llclamp((F32)LLFrameTimer::getFrameDeltaTimeF32() * 8.f, 0.f, 1.f);
        if (bg) circleF(X(176.f), YM(744.f), S(150.f), alpha(OAK_INSET, 0.55f), 48);
        // the rudder angle above it
        const F32 wcx = X(176.f), wcy = YM(744.f);
        mWheelR = S(104.f);
        shipsWheel(wcx, wcy, mWheelR, mWheelShown, orders);
        T(wcx, YM(886.f), helmWords(helm), 13.f, std::abs(helm) > 0.5f ? C_AMBER : C_WHITE, true);
        addHit(X(40.f), YM(872.f), X(312.f), YM(612.f), H_WHEEL,
               "The ship's wheel: drag left or right to put the helm over (it stays), double-click for midships. "
               "Left / Right (A / D) steer as before.");

        // sail plan box
        roundRectF(X(326.f), YM(796.f), X(604.f), YM(606.f), S(8.f), LLColor4(0.05f, 0.09f, 0.13f, 0.92f), LLColor4(0.10f, 0.18f, 0.24f, 0.92f));
        frameW(X(326.f), YM(796.f), X(604.f), YM(606.f), 1.f, alpha(BRASS, 0.6f));
        sailPlan(340.f, 562.f, s.mSail, true);
        static const char* const SETTINGS[4] = { "Furled", "Battle sails", "Plain sail", "All sail" };
        T(X(596.f), YM(618.f), SETTINGS[llclamp(s.mSail, 0, 3)], 10.f, BRASS, true, false, LLFontGL::RIGHT);
        const std::string capt_only = orders ? "" : " (the captain's order)";
        button(c, X(326.f), YM(838.f), X(392.f), YM(806.f), "FURL", s.mSail == 0, H_FURL, "Furl every sail" + capt_only, 11.f, orders);
        button(c, X(396.f), YM(838.f), X(462.f), YM(806.f), "BATTLE", s.mSail == 1, H_BATTLE_SAILS, "Battle sails: topsails, jib and spanker" + capt_only, 11.f, orders);
        button(c, X(466.f), YM(838.f), X(532.f), YM(806.f), "PLAIN", s.mSail == 2, H_PLAIN, "Plain sail: and the courses, topgallants and staysails" + capt_only, 11.f, orders);
        button(c, X(536.f), YM(838.f), X(604.f), YM(806.f), "ALL", s.mSail == 3, H_ALL, "All sail: and the royals and flying jib" + capt_only, 11.f, orders);
        button(c, X(326.f), YM(880.f), X(462.f), YM(846.f), s.mAnchor ? "WEIGH ANCHOR" : "DROP ANCHOR", s.mAnchor, H_ANCHOR,
               (s.mAnchor ? std::string("At anchor: weigh anchor to sail") : std::string("Let go the anchor: she is brought up short")) + capt_only, 10.f, orders);
        button(c, X(466.f), YM(880.f), X(604.f), YM(846.f), s.mAstern ? "STOP ASTERN" : "ASTERN", s.mAstern, H_ASTERN,
               (s.mAstern ? std::string("The boats stop towing") : std::string("The ship's boats tow her astern (sails furled first): to back her off a shoal or the edge")) + capt_only, 10.f, orders);

        // ── CENTRE: the status line, the compass, the speed log ────────────────────────────────
        use(g.C);
        std::string status;
        LLColor4 status_col = C_WHITE;
        if (h.noAnswer())
        {
            status = "No answer from the ship: is her helm script running? Shout /7 status to ask her.";
            status_col = C_RED;
        }
        else if (!mRefused.empty() && t - mRefusedAt < 4.0)
        {
            status = mRefused;
            status_col = C_AMBER;
        }
        else if (!s.mSay.empty())
        {
            status = s.mSay;
            status_col = (s.mStop.empty() && s.mPos != "irons") ? C_WHITE : (s.mStop.empty() ? C_AMBER : C_RED);
        }
        else
        {
            status = pointOfSail(s);
        }
        if (s.mStanding > 0) status += llformat("   (%d standing aboard: they must sit)", s.mStanding);
        roundRectF(X(C_L + 4.f), YM(636.f), X(C_R - 4.f), YM(604.f), S(6.f), LLColor4(0.f, 0.f, 0.f, 0.6f), LLColor4(0.f, 0.f, 0.f, 0.6f));
        fitT(X((C_L + C_R) * 0.5f), YM(620.f), status, 12.f, 9.f, S(C_R - C_L - 20.f), status_col, true);

        // the compass: heading-up, a parchment card in a brass ring
        const F32 hdg = h.heading();
        const F32 ccx = X(764.f), ccy = YM(768.f), cr = S(98.f);
        circleShaded(ccx, ccy, cr * 1.1f, BRASS_HI, BRASS_LO, 64);
        circleShaded(ccx, ccy, cr, PARCH_HI, PARCH_LO, 64);
        // the no-go wedge either side of the true wind: red, brighter while her bow is in it
        const F32 wind_rel = s.mTwf - hdg;
        const bool irons = s.mTwa < s.mNoGo && s.mEff >= 0.6f;   // 0.3 m/s = 0.58 kn: the script's "calm"
        clockWedge(ccx, ccy, cr * 0.97f, wind_rel - s.mNoGo, wind_rel + s.mNoGo, LLColor4(0.80f, 0.12f, 0.08f, irons ? 0.42f : 0.20f), 32);
        // ticks and the card's points, turned so her heading is at the top
        for (S32 b = 0; b < 360; b += 10)
        {
            F32 x0, y0, x1, y1;
            const bool major = b % 30 == 0;
            pt(ccx, ccy, cr * (major ? 0.84f : 0.9f), (F32)b - hdg, x0, y0);
            pt(ccx, ccy, cr * 0.97f, (F32)b - hdg, x1, y1);
            lineW(x0, y0, x1, y1, major ? 1.8f : 1.f, INK);
        }
        static const char* const CARD[4] = { "N", "E", "S", "W" };
        for (S32 k = 0; k < 4; ++k)
        {
            F32 x, y;
            pt(ccx, ccy, cr * 0.7f, k * 90.f - hdg, x, y);
            T(x, y, CARD[k], 14.f, k == 0 ? LLColor4(0.70f, 0.08f, 0.05f, 1.f) : INK, true);
        }
        // the ship at the centre, bow up
        triF(ccx, ccy + S(30.f), ccx - S(9.f), ccy + S(6.f), ccx + S(9.f), ccy + S(6.f), INK);
        quadF(ccx - S(9.f), ccy + S(6.f), ccx + S(9.f), ccy + S(6.f), ccx + S(7.f), ccy - S(26.f), ccx - S(7.f), ccy - S(26.f), INK);
        // the true wind: a brass-and-ink arrow from where it blows, toward the ship
        {
            F32 x0, y0, x1, y1;
            pt(ccx, ccy, cr * 1.02f, wind_rel, x0, y0);
            pt(ccx, ccy, cr * 0.42f, wind_rel, x1, y1);
            arrow(x0, y0, x1, y1, S(5.f), S(16.f), LLColor4(0.10f, 0.20f, 0.42f, 1.f));
        }
        // the apparent wind: what she feels, the true wind less her own way (thin, teal), when she is moving
        if (std::abs(s.mKn) > 0.3f && s.mTws > 0.1f)
        {
            // wind blowing toward (from + 180), boat moving toward hdg; the apparent wind is wind - boat
            const F32 wt = (s.mTwf + 180.f) * DEG_TO_RAD, ht = hdg * DEG_TO_RAD;
            const F32 ae = s.mTws * sinf(wt) - s.mKn * sinf(ht), an = s.mTws * cosf(wt) - s.mKn * cosf(ht);
            if (std::abs(ae) + std::abs(an) > 0.05f)
            {
                const F32 from = atan2f(-ae, -an) * RAD_TO_DEG;
                F32 x0, y0, x1, y1;
                pt(ccx, ccy, cr * 0.96f, from - hdg, x0, y0);
                pt(ccx, ccy, cr * 0.5f, from - hdg, x1, y1);
                arrow(x0, y0, x1, y1, S(2.5f), S(11.f), LLColor4(0.05f, 0.55f, 0.60f, 1.f));
            }
        }
        // the lubber line over the ring
        triF(ccx, ccy + cr * 1.02f, ccx - S(8.f), ccy + cr * 1.16f, ccx + S(8.f), ccy + cr * 1.16f, C_RED);
        T(X(C_L + 8.f), YM(652.f), "HEADING", 9.f, C_GREY, false, false, LLFontGL::LEFT);
        T(X(C_L + 8.f), YM(674.f), llformat("%03d\xC2\xB0", ((S32)llround(hdg)) % 360), 20.f, C_WHITE, true, true, LLFontGL::LEFT);
        T(X(C_L + 8.f), YM(694.f), compassPoint(hdg), 11.f, BRASS, true, false, LLFontGL::LEFT);
        // under the ring's lowest point (ccy - 1.1 cr = mock 876)
        T(X(C_L + 8.f), YM(888.f), llformat("WIND %.0f kn %s", s.mTws, compassPoint(s.mTwf).c_str()), 10.f, LLColor4(0.55f, 0.70f, 1.f, 1.f), true, false, LLFontGL::LEFT);

        // the speed log: 0..20 knots (more if her hull speed is higher), the hull speed in red
        const F32 lcx = X(966.f), lcy = YM(756.f), lr = S(74.f);
        const F32 full = llmax(20.f, std::ceil((s.mHullKn + 2.f) / 5.f) * 5.f);
        circleShaded(lcx, lcy, lr * 1.1f, BRASS_HI, BRASS_LO, 48);
        circleShaded(lcx, lcy, lr, LLColor4(0.10f, 0.08f, 0.06f, 1.f), LLColor4(0.02f, 0.015f, 0.01f, 1.f), 48);
        auto ang = [&](F32 v) { return -135.f + 270.f * llclamp(v / full, 0.f, 1.02f); };
        if (s.mHullKn > 0.f) clockArc(lcx, lcy, lr * 0.86f, S(5.f), ang(s.mHullKn), 135.f, C_RED);
        for (S32 k = 0; k <= (S32)full; ++k)
        {
            F32 x0, y0, x1, y1;
            const bool major = k % 5 == 0;
            pt(lcx, lcy, lr * (major ? 0.74f : 0.8f), ang((F32)k), x0, y0);
            pt(lcx, lcy, lr * 0.9f, ang((F32)k), x1, y1);
            lineW(x0, y0, x1, y1, major ? 1.8f : 1.f, C_WHITE);
            if (major)
            {
                F32 tx, ty;
                pt(lcx, lcy, lr * 0.58f, ang((F32)k), tx, ty);
                T(tx, ty, llformat("%d", k), 9.f, C_GREY, false, true);
            }
        }
        {
            F32 nx, ny;
            pt(lcx, lcy, lr * 0.86f, ang(std::abs(s.mKn)), nx, ny);
            lineW(lcx, lcy, nx, ny, S(3.f), BRASS_HI);
            circleShaded(lcx, lcy, S(8.f), BRASS_HI, BRASS_LO, 16);
        }
        T(lcx, lcy - S(32.f), llformat("%.1f", std::abs(s.mKn)), 16.f, C_WHITE, true, true);
        T(lcx, lcy - S(50.f), s.mKn < -0.05f ? "ASTERN" : "KNOTS", 9.f, s.mKn < -0.05f ? C_AMBER : C_GREY, true);
        T(lcx, YM(860.f), s.mHeel >= 0.5f || s.mHeel <= -0.5f ? llformat("HEEL %d\xC2\xB0 %s", (S32)llround(std::abs(s.mHeel)), s.mHeel > 0.f ? "STBD" : "PORT") : std::string("UPRIGHT"),
          9.f, C_GREY);
        T(lcx, YM(878.f), pointOfSail(s), 9.f, s.mPos == "irons" ? C_AMBER : BRASS, true);

        // ── RIGHT: the wind source, then the battle section ──────────────────────────────────────
        use(g.R);
        T(X(R_L + 10.f), YM(612.f), "WIND SHE SAILS ON", 9.f, C_GREY, false, false, LLFontGL::LEFT);
        button(c, X(R_L + 10.f), YM(654.f), X(R_L + 86.f), YM(624.f), "WOLF WX", s.mWind == 0, H_WIND_WEATHER,
               "Wolf weather: the real weather of this region's place" + capt_only, 10.f, orders);
        button(c, X(R_L + 90.f), YM(654.f), X(R_L + 162.f), YM(624.f), "REGION", s.mWind == 1, H_WIND_REGION,
               "The region's own wind (the one Sailing Mode's dial shows)" + capt_only, 10.f, orders);
        button(c, X(R_L + 166.f), YM(654.f), X(R_L + 236.f), YM(624.f), "FIXED", s.mWind == 2, H_WIND_FIXED,
               "A wind you set: knots and the bearing it blows from (the steppers below)" + capt_only, 10.f, orders);
        button(c, X(R_L + 10.f), YM(692.f), X(R_L + 120.f), YM(662.f), "LIGHT AIRS", s.mLight, H_LIGHT_AIRS,
               (s.mLight ? std::string("Light airs help on: under 8 knots of wind she sails as if it blew 8. Click for the real wind only")
                         : std::string("Light airs help off: she sails on the real wind only. Click to help her in light airs")) + capt_only, 10.f, orders, C_GREEN);
        T(X(R_L + 128.f), YM(670.f), llformat("%.0f kn from %03d\xC2\xB0", s.mTws, ((S32)llround(s.mTwf)) % 360), 11.f, C_WHITE, true, false, LLFontGL::LEFT);
        T(X(R_L + 128.f), YM(686.f), s.mEff > s.mTws + 0.2f ? llformat("sails as in %.0f kn", s.mEff) : std::string(), 9.f, C_GREEN, false, false, LLFontGL::LEFT);
        {
            std::string src = s.mSrc;
            if (src.size() > 44) src = src.substr(0, 44) + "...";
            T(X(R_L + 10.f), YM(706.f), src, 9.f, C_GREY, false, false, LLFontGL::LEFT);
        }
        if (s.mWind == 2)
        {
            // the fixed wind's steppers: knots 0..60 by 1, the bearing by 10 degrees
            button(c, X(R_L + 10.f), YM(744.f), X(R_L + 36.f), YM(718.f), "-", false, H_KN_DOWN, "One knot less" + capt_only, 13.f, orders);
            T(X(R_L + 64.f), YM(731.f), llformat("%.0f KN", s.mTws), 11.f, C_WHITE, true, true);
            button(c, X(R_L + 92.f), YM(744.f), X(R_L + 118.f), YM(718.f), "+", false, H_KN_UP, "One knot more" + capt_only, 13.f, orders);
            button(c, X(R_L + 126.f), YM(744.f), X(R_L + 152.f), YM(718.f), "-", false, H_FROM_DOWN, "Back the wind 10 degrees" + capt_only, 13.f, orders);
            T(X(R_L + 180.f), YM(731.f), llformat("%03d\xC2\xB0", ((S32)llround(s.mTwf)) % 360), 11.f, C_WHITE, true, true);
            button(c, X(R_L + 208.f), YM(744.f), X(R_L + 234.f), YM(718.f), "+", false, H_FROM_UP, "Veer the wind 10 degrees" + capt_only, 13.f, orders);
        }
        // crew / captain, and what the keys do
        T(X(R_L + 10.f), YM(872.f), captain ? "YOU HAVE THE HELM" : "CREW: THE CAPTAIN GIVES THE ORDERS", 10.f, captain ? C_GREEN : C_AMBER, true, false, LLFontGL::LEFT);
        T(X(R_L + 10.f), YM(888.f), "Keys: Left/Right helm, PgUp/PgDn sail", 9.f, C_GREY, false, false, LLFontGL::LEFT);

        // the battle section
        const F32 bl = 1316.f, br = 1592.f;
        roundRectF(X(bl - 6.f), YM(894.f), X(br), YM(604.f), S(8.f), LLColor4(0.07f, 0.03f, 0.02f, 0.88f), LLColor4(0.12f, 0.05f, 0.03f, 0.88f));
        frameW(X(bl - 6.f), YM(894.f), X(br), YM(604.f), 1.f, alpha(C_RED, 0.45f));
        T(X(bl + 4.f), YM(616.f), "BATTLE", 10.f, LLColor4(1.f, 0.45f, 0.35f, 1.f), true, false, LLFontGL::LEFT);
        if (!s.mNaval)
        {
            T(X((bl + br) * 0.5f), YM(730.f), "No guns fitted yet", 13.f, C_GREY, true);
            T(X((bl + br) * 0.5f), YM(752.f), "Battle mode comes with the naval script.", 9.f, C_DIM);
        }
        else
        {
            const bool on = s.mMode == "on" || s.mMode == "switching_off" || s.mMode == "silenced";
            const F32 lock = h.lockLeft();
            std::string label = on ? "BATTLE: ON" : "BATTLE: OFF";
            if (s.mMode == "switching_off" && lock > 0.f) label = llformat("ON - %d s", (S32)std::ceil(lock));
            if (s.mMode == "silenced") label = "SILENCED";
            if (s.mMode == "sinking") label = "SINKING";
            if (s.mMode == "sunk") label = "SUNK";
            button(c, X(bl + 2.f), YM(662.f), X(bl + 126.f), YM(626.f), label, on, H_BATTLE_MODE,
                   on ? "Battle mode is on: she can fire on, and be hit by, any other ship in battle mode. Click to switch it off "
                        "(it stays on for 60 s after, so no one can dodge a fight)"
                      : "Switch battle mode on: she can fire on, and be hit and sunk by, other ships in battle mode, on any region. "
                        "Ships with it off are never touched",
                   10.f, s.mHaveBattle && s.mMode != "sinking" && s.mMode != "sunk", C_RED);
            // the flag: the Jolly Roger flies while battle mode is on, a plain red pennant otherwise
            {
                const F32 fx = X(bl + 140.f), fb = YM(668.f), ft = YM(616.f);
                lineW(fx, fb, fx, ft, S(2.f), BRASS);
                const F32 wave = sinf((F32)t * 4.f) * S(3.f);
                if (on)
                {
                    quadF(fx, ft, fx + S(52.f), ft - wave, fx + S(52.f), ft - S(32.f) + wave, fx, ft - S(32.f), LLColor4(0.04f, 0.04f, 0.04f, 1.f));
                    const F32 kx = fx + S(26.f), ky = ft - S(14.f);
                    circleF(kx, ky + S(2.f), S(6.5f), C_WHITE, 16);              // the skull
                    rectF(kx - S(4.f), ky - S(6.f), kx + S(4.f), ky - S(2.f), C_WHITE);
                    circleF(kx - S(2.5f), ky + S(2.f), S(1.6f), LLColor4(0.04f, 0.04f, 0.04f, 1.f), 8);
                    circleF(kx + S(2.5f), ky + S(2.f), S(1.6f), LLColor4(0.04f, 0.04f, 0.04f, 1.f), 8);
                    lineW(kx - S(11.f), ky - S(13.f), kx + S(11.f), ky - S(5.f), S(2.5f), C_WHITE);   // the crossed bones
                    lineW(kx - S(11.f), ky - S(5.f), kx + S(11.f), ky - S(13.f), S(2.5f), C_WHITE);
                }
                else
                {
                    triF(fx, ft, fx + S(44.f), ft - S(8.f) - wave * 0.5f, fx, ft - S(16.f), LLColor4(0.55f, 0.10f, 0.08f, 1.f));
                }
            }
            // health: the seven sections
            for (S32 i = 0; i < WolfHelm::SECTION_COUNT; ++i)
            {
                healthBar(bl + 2.f, bl + 196.f, YM(686.f + i * 18.f), SECTION_NAMES[i],
                          s.mHaveBattle ? s.mNow[i] : 0.f, s.mHaveBattle ? s.mMax[i] : 0.f);
            }
            // the broadsides
            // firing in the 60 s switch-off keeps battle on (hms_wolfy_naval_template.lsl): those may fire too
            const bool can_fire = s.mHaveBattle && (s.mMode == "on" || s.mMode == "switching_off");
            broadside(c, X(br - 40.f), YM(706.f), S(30.f), "PORT", h.reloadFraction(false), h.reloadLeft(false), s.mPortFiredAt,
                      can_fire, H_FIRE_PORT, "Fire the port broadside (the guns on her left side). The ring fills as they reload");
            broadside(c, X(br - 40.f), YM(800.f), S(30.f), "STBD", h.reloadFraction(true), h.reloadLeft(true), s.mStbdFiredAt,
                      can_fire, H_FIRE_STBD, "Fire the starboard broadside (the guns on her right side). The ring fills as they reload");
            std::string note;
            LLColor4 note_col = C_GREY;
            if (!s.mHaveBattle) note = "Waiting for the gun deck's report...";
            else if (s.mMode == "sinking") { note = "SHE IS SINKING"; note_col = blink(0.5f) ? C_RED : C_WHITE; }
            else if (s.mMode == "sunk") { note = "SUNK - she will be restored"; note_col = C_RED; }
            else if (s.mMode == "silenced") { note = llformat("GUNS SILENCED %d s", (S32)std::ceil(lock)); note_col = C_AMBER; }
            else if (s.mStruck) { note = "COLOURS STRUCK"; note_col = C_AMBER; }
            else if (h.protectedLeft() > 0.f) { note = llformat("PROTECTED %d s", (S32)std::ceil(h.protectedLeft())); note_col = C_GREEN; }
            else if (s.mMode == "switching_off" && lock > 0.f) { note = llformat("LOCKED IN BATTLE %d s", (S32)std::ceil(lock)); note_col = C_AMBER; }
            else if (s.mMode == "on" && (s.mSailFactor < 0.99f || s.mTurnFactor < 0.99f))
            {
                note = llformat("DAMAGED: sails %d%%, helm %d%%", (S32)llround(s.mSailFactor * 100.f), (S32)llround(s.mTurnFactor * 100.f));
                note_col = C_AMBER;
            }
            T(X(bl + 2.f), YM(880.f), note, 10.f, note_col, true, false, LLFontGL::LEFT);
            // booty (2026-10-30): this avatar's hoard of gold doubloons, and the share just taken (gold, 30 s)
            if (s.mHaveBooty)
            {
                std::string hoard = llformat("%lld", (long long)s.mHoard);
                for (S32 k = (S32)hoard.size() - 3; k > 0; k -= 3) hoard.insert((size_t)k, ",");      // 12,345
                T(X(bl + 2.f), YM(822.f), "BOOTY", 10.f, BRASS, true, false, LLFontGL::LEFT);
                T(X(bl + 196.f), YM(822.f), hoard + (s.mRank > 0 ? llformat("  #%d", s.mRank) : std::string()), 10.f, BRASS_HI, true, true,
                  LLFontGL::RIGHT);
                if (s.mGot > 0 && now() - s.mGotAt < 30.0)
                {
                    const std::string took = llformat("+%d doubloons", s.mGot) + (s.mGotFrom.empty() ? std::string("!") : " from " + s.mGotFrom + "!");
                    fitT(X(bl + 99.f), YM(844.f), took, 10.f, 7.f, S(194.f), blink(0.6f) && now() - s.mGotAt < 6.0 ? C_WHITE : BRASS_HI, true);
                }
                else
                {
                    T(X(bl + 2.f), YM(844.f), "doubloons won in battle", 9.f, C_DIM, false, false, LLFontGL::LEFT);
                }
            }
        }
    }

    // ── the strip of buttons above the panel (wolfdashboard.cpp strip) ──────────────────────────
    {
        Anchor sa;
        sa.u = g.us;
        use(sa);
        const bool autoopen = gSavedSettings.getBOOL("WolfHelmHudAuto");
        struct B { F32 w; std::string label; bool lit; EHit id; std::string tip; };
        const std::vector<B> bs =
        {
            { 30.f, "X", false, H_CLOSE, "Close the Captain's Helm. Wolf World > Vehicles & Travel > Captain's Helm (Ships), the ship icon or its toolbar button bring it back." },
            { 70.f, "HELP", mShowHelp, H_HELP, "How the Captain's Helm works" },
            { 92.f, "PANEL", bg, H_PANEL, bg ? "Background on: click to float the instruments over the world" : "Background off: click to put the panel back" },
            { 92.f, "AUTO", autoopen, H_AUTO, autoopen ? "Opens by itself when you take a ship's helm: click to open it only by hand"
                                                         : "Opens only by hand: click to have it open by itself when you take a ship's helm" },
        };
        const F32 margin = S(14.f), gap = S(8.f), bh = S(30.f);
        F32 x = g.W - margin;
        const F32 bot = g.panelTop + S(8.f);
        for (const B& b : bs)
        {
            const F32 w = S(b.w);
            button(c, x - w, bot, x, bot + bh, b.label, b.lit, b.id, b.tip, 13.f);
            x -= w + gap;
        }
        // the ship's name and who you are aboard
        std::string who = s.mShip.empty() ? std::string("CAPTAIN'S HELM") : s.mShip;
        if (ready) who += captain ? "  -  you have the helm" : "  -  crew";
        const LLFontGL* f = font(13.f);
        const F32 lw = f ? f->getWidthF32(who) : S(200.f);
        const F32 lr = llmin(margin + lw + S(24.f), x - gap);
        if (lr > margin + S(80.f))
        {
            roundRectF(margin, bot, lr, bot + bh, S(6.f), LLColor4(0.f, 0.f, 0.f, 0.55f), LLColor4(0.f, 0.f, 0.f, 0.55f));
            text(f, who, margin + S(12.f), bot + bh * 0.5f, BRASS_HI, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        mPanelT = (bot + bh) * zoom;
    }

    zoomed.reset();
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mResize.draw();
    if (mShowHelp)
    {
        drawHelp(mPanelT);
    }
}

void WolfHelmHud::drawHelp(F32 top)
{
    static const HelpLine LEFT_COL[] =
    {
        { 0, "THE CAPTAIN'S HELM" },
        { 1, "Opens by itself when you take the wheel of a Wolf ship" },
        { 2, "(right-click the wheel > Sit Here); closes when you stand." },
        { 1, "Wheel (left): drag it to put the helm over - it stays." },
        { 2, "Double-click: midships. Left / Right keys steer as before." },
        { 1, "Sail plan: FURL, BATTLE, PLAIN, ALL - each sail lights as set." },
        { 2, "Page Up / Page Down make and shorten sail as before." },
        { 1, "ANCHOR and ASTERN (the boats tow her back off a shoal)." },
        { 3, "" },
        { 0, "THE COMPASS" },
        { 1, "Her bow is at the top. Blue arrow: the true wind; teal:" },
        { 2, "the wind she feels (apparent). Red wedge: she cannot sail" },
        { 2, "that close to the wind (67.5 degrees) - turn her out of it." },
        { 1, "Speed log: knots through the water, her hull speed in red." },
    };
    static const HelpLine RIGHT_COL[] =
    {
        { 0, "WIND" },
        { 1, "WOLF WX: the real weather. REGION: the region's wind (as" },
        { 2, "Sailing Mode's dial). FIXED: set knots and bearing yourself." },
        { 1, "LIGHT AIRS: under 8 knots she sails as if it blew 8." },
        { 3, "" },
        { 0, "BATTLE" },
        { 1, "BATTLE MODE on: she can fight any ship that also has it on," },
        { 2, "anywhere on the grid. It stays on 60 s after you switch it off." },
        { 1, "PORT / STBD fire a broadside; the ring fills as the guns reload." },
        { 1, "The bars: bow, stern, sides, waterline, rudder and rigging." },
        { 1, "BOOTY: your doubloons for ships your crew beat (Top Pirates)." },
        { 1, "Only the captain gives orders; the crew see the same helm." },
        { 1, "Typed orders still work: shout /7 status (Ctrl+Enter)." },
        { 1, "Click anywhere above the helm to close.  HELP reopens it." },
    };
    const F32 W = (F32)getRect().getWidth(), Hv = (F32)getRect().getHeight();
    const F32 avail_h = Hv - top;
    if (avail_h < 120.f) return;
    const F32 cw = llmin(W * 0.92f, 1500.f);
    const F32 ch = avail_h * 0.92f;
    const F32 l = (W - cw) * 0.5f, r = l + cw, t = Hv - avail_h * 0.04f, b = t - ch;
    roundRectF(l - 2.f, b - 2.f, r + 2.f, t + 2.f, 10.f, BRASS_LO, LLColor4(0.05f, 0.03f, 0.02f, 1.f));
    roundRectF(l, b, r, t, 9.f, LLColor4(0.10f, 0.065f, 0.04f, 0.97f), LLColor4(0.04f, 0.025f, 0.015f, 0.97f));
    const S32 n1 = (S32)(sizeof(LEFT_COL) / sizeof(LEFT_COL[0]));
    const S32 n2 = (S32)(sizeof(RIGHT_COL) / sizeof(RIGHT_COL[0]));
    const S32 rows = llmax(n1, n2) + 2;
    const F32 lh = llmin((ch - 20.f) / rows, 26.f);
    const LLFontGL* head = face("B612", LLFontGL::BOLD, lh * 0.95f);
    const LLFontGL* body = face("B612", LLFontGL::NORMAL, lh * 0.82f);
    text(head, "CAPTAIN'S HELM", (l + r) * 0.5f, t - lh * 0.8f, BRASS_HI, LLFontGL::HCENTER, LLFontGL::VCENTER);
    auto column = [&](const HelpLine* lines, S32 n, F32 x)
    {
        F32 y = t - lh * 1.8f;
        for (S32 i = 0; i < n; ++i)
        {
            switch (lines[i].kind)
            {
            case 0: text(head, lines[i].s, x, y, BRASS, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 1: text(body, lines[i].s, x + lh * 0.5f, y, C_WHITE, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 2: text(body, lines[i].s, x + lh * 1.6f, y, C_GREY, LLFontGL::LEFT, LLFontGL::TOP); break;
            default: break;
            }
            y -= (lines[i].kind == 3) ? lh * 0.5f : lh;
        }
    };
    column(LEFT_COL, n1, l + cw * 0.03f);
    column(RIGHT_COL, n2, l + cw * 0.52f);
    rectF(l + cw * 0.505f, b + lh, l + cw * 0.505f + 1.f, t - lh * 1.8f, C_DIM);
}

//-----------------------------------------------------------------------------
// input
//-----------------------------------------------------------------------------

void WolfHelmHud::press(EHit id)
{
    WolfHelm& h = WolfHelm::instance();
    const WolfHelm::State& s = h.state();
    const bool captain = h.isCaptain();
    auto captainOrder = [&](const std::string& words)
    {
        if (!captain)
        {
            refuse("Only the captain at the wheel gives the ship's orders.");
            return;
        }
        h.order(words);
    };
    switch (id)
    {
    // Source: hms_wolfy_helm_template.lsl Order - the words the ship already knows
    case H_FURL:          captainOrder("furl"); break;
    case H_BATTLE_SAILS:  captainOrder("battle sails"); break;
    case H_PLAIN:         captainOrder("plain sail"); break;
    case H_ALL:           captainOrder("all sail"); break;
    case H_ANCHOR:        captainOrder(s.mAnchor ? "weigh anchor" : "drop anchor"); break;
    case H_ASTERN:        captainOrder(s.mAstern ? "stop astern" : "astern"); break;
    case H_WIND_WEATHER:  captainOrder("wind weather"); break;
    case H_WIND_REGION:   captainOrder("wind region"); break;
    case H_LIGHT_AIRS:    captainOrder(s.mLight ? "light airs off" : "light airs on"); break;
    case H_WIND_FIXED:
    case H_KN_DOWN: case H_KN_UP: case H_FROM_DOWN: case H_FROM_UP:
    {
        if (!captain)
        {
            refuse("Only the captain at the wheel gives the ship's orders.");
            break;
        }
        F32 kn = s.mTws, from = s.mTwf;
        if (id == H_KN_DOWN) kn -= 1.f;
        else if (id == H_KN_UP) kn += 1.f;
        else if (id == H_FROM_DOWN) from -= 10.f;
        else if (id == H_FROM_UP) from += 10.f;
        h.setFixedWind(kn, from);   // FIXED: the wind she has now, held
        break;
    }
    // the guns: the naval script decides who may (DESIGN.md s20.3)
    case H_BATTLE_MODE:
    {
        if (!s.mHaveBattle || s.mMode == "sinking" || s.mMode == "sunk")
        {
            refuse(s.mHaveBattle ? "She cannot change battle mode while she is sinking or sunk." : "Waiting for the gun deck's report.");
            break;
        }
        const bool on = s.mMode == "on" || s.mMode == "switching_off" || s.mMode == "silenced";
        if (s.mMode == "switching_off" && h.lockLeft() > 0.f)
        {
            // already switching off: it goes off by itself when the lock runs out
            refuse(llformat("Battle mode goes off in %d s (it stays on 60 s, so no one can dodge a fight).", (S32)std::ceil(h.lockLeft())));
            break;
        }
        h.battle(!on);
        break;
    }
    case H_FIRE_PORT:
    case H_FIRE_STBD:
    {
        const bool stbd = id == H_FIRE_STBD;
        if (s.mMode == "silenced") { refuse(llformat("The guns are silenced for %d s.", (S32)std::ceil(h.lockLeft()))); break; }
        if (s.mMode != "on" && s.mMode != "switching_off") { refuse("Switch battle mode on to fire."); break; }
        if (h.reloadLeft(stbd) > 0.f) { refuse(llformat("The %s guns are reloading: %d s.", stbd ? "starboard" : "port", (S32)std::ceil(h.reloadLeft(stbd)))); break; }
        h.fire(stbd);
        break;
    }
    case H_CLOSE: h.showHud(false); break;
    case H_HELP:  mShowHelp = !mShowHelp; break;
    case H_PANEL: gSavedSettings.setBOOL("WolfHelmHudBackground", !gSavedSettings.getBOOL("WolfHelmHudBackground")); break;
    case H_AUTO:  gSavedSettings.setBOOL("WolfHelmHudAuto", !gSavedSettings.getBOOL("WolfHelmHudAuto")); break;
    default: break;
    }
}

bool WolfHelmHud::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (mDrawn && mShowHelp && y > mPanelT)
    {
        mShowHelp = false;
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
    if (hit->mId == H_WHEEL)
    {
        if (!WolfHelm::instance().isCaptain())
        {
            refuse("Only the captain at the wheel gives the ship's orders.");
            return true;
        }
        mPressX = x;
        mPressHelm = WolfHelm::instance().state().mHelm;
        return true;
    }
    press(hit->mId);
    return true;
}

bool WolfHelmHud::handleMouseUp(S32 x, S32 y, MASK mask)
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

bool WolfHelmHud::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mPressed == H_WHEEL && WolfHelm::instance().isCaptain())
    {
        // Source: wolfdashboard.cpp handleHover H_WHEEL - horizontal travel from the press, full lock at one radius.
        // Right is to starboard (the helm's minus).
        const WolfHelm& h = WolfHelm::instance();
        const F32 travel = (F32)(x - mPressX) / mHitScale / llmax(24.f, mWheelR);
        WolfHelm::instance().setHelm(mPressHelm - travel * h.state().mHelmMax);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
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

bool WolfHelmHud::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfHelmHud::handleDoubleClick(S32 x, S32 y, MASK mask)
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
    const Hit* hit = hitAt(x, y);
    if (hit && hit->mId == H_WHEEL)
    {
        if (WolfHelm::instance().isCaptain()) WolfHelm::instance().order("midships");
        else refuse("Only the captain at the wheel gives the ship's orders.");
        return true;
    }
    return handleMouseDown(x, y, mask);
}

bool WolfHelmHud::handleToolTip(S32 x, S32 y, MASK mask)
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
        if (!hit->mTip.empty())
        {
            LLToolTipMgr::instance().show(hit->mTip);
        }
    }
    return true;
}

void WolfHelmHud::onMouseCaptureLost()
{
    mResize.end();
    mPressed = H_NONE;
    LLUICtrl::onMouseCaptureLost();
}
