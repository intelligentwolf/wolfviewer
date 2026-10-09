/**
 * @file wolfdashboard.cpp
 * @brief WolfViewer: the car / truck / bike dashboard. See wolfdashboard.h.
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

#include "wolfdashboard.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>

#include "llagent.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
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
#include "wolfdrive.h"

static LLDefaultChildRegistry::Register<WolfDashboard> r_wolf_dashboard("wolf_car_dashboard");

namespace
{
    const F32 PI_F = 3.14159265f;
    const F32 KMH = 3.6f;
    const F32 MPH = 2.2369363f;

    // ISO 2575 colours (researched 2026-10-09): red = act now, amber = caution, green / blue = a
    // system is on (blue for main beam).
    const LLColor4 C_RED(1.00f, 0.13f, 0.10f, 1.f);
    const LLColor4 C_AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 C_GREEN(0.20f, 0.95f, 0.30f, 1.f);
    const LLColor4 C_BLUE(0.20f, 0.45f, 1.00f, 1.f);
    const LLColor4 C_WHITE(0.96f, 0.96f, 0.96f, 1.f);
    const LLColor4 C_GREY(0.62f, 0.64f, 0.67f, 1.f);
    const LLColor4 C_DIM(0.30f, 0.31f, 0.34f, 1.f);
    const LLColor4 C_OFF(0.16f, 0.16f, 0.18f, 1.f);
    const LLColor4 C_BLACK(0.f, 0.f, 0.f, 1.f);
    const LLColor4 C_NEEDLE(0.95f, 0.12f, 0.08f, 1.f);       // BMW M: red needles
    const LLColor4 C_TEAL(0.25f, 0.85f, 0.90f, 1.f);
    const LLColor4 C_ORANGE(1.00f, 0.50f, 0.05f, 1.f);

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    bool blink(F32 period = 0.8f) { return fmod(now(), (F64)period) < period * 0.5f; }
    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    // ---- primitives, view-local, untextured (after wolfflightdeck.cpp's) ----
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

    void ellipseF(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& c, S32 seg = 40)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + rx * cosf(a0), cy + ry * sinf(a0), c);
            vtx(cx + rx * cosf(a1), cy + ry * sinf(a1), c);
        }
        gGL.end();
    }

    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 40) { ellipseF(cx, cy, rad, rad, c, seg); }

    // A lit dome: brighter toward the upper left, like a light above.
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

    // A stroked elliptical arc, angles in radians counter-clockwise from +x.
    void arcE(F32 cx, F32 cy, F32 rx, F32 ry, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48)
    {
        noTex();
        const F32 h = w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
            const F32 c0 = cosf(u0), s0 = sinf(u0), c1 = cosf(u1), s1 = sinf(u1);
            vtx(cx + (rx - h) * c0, cy + (ry - h) * s0, c); vtx(cx + (rx + h) * c0, cy + (ry + h) * s0, c); vtx(cx + (rx + h) * c1, cy + (ry + h) * s1, c);
            vtx(cx + (rx - h) * c0, cy + (ry - h) * s0, c); vtx(cx + (rx + h) * c1, cy + (ry + h) * s1, c); vtx(cx + (rx - h) * c1, cy + (ry - h) * s1, c);
        }
        gGL.end();
    }

    void arcW(F32 cx, F32 cy, F32 rad, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48)
    {
        arcE(cx, cy, rad, rad, w, a0, a1, c, seg);
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
        if (!f || s.empty()) return;
        f->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // Source: wolfflightdeck.cpp avionics() - the B612 face (fonts.xml Avionics0..11) at the
    // largest size whose line fits px.
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
                LL_WARNS("WolfDashboard") << "font " << name << " missing, using the default" << LL_ENDL;
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


    /** The telltale's own word and its ISO 2575 colour (red: act now, amber: caution). */
    void warningLook(const std::string& w, std::string& label, LLColor4& col)
    {
        col = C_AMBER;
        label = w;
        if (w == "engine")        { label = "ENGINE"; }
        else if (w == "oil")      { label = "OIL"; col = C_RED; }
        else if (w == "battery")  { label = "BATT"; col = C_RED; }
        else if (w == "brake")    { label = "BRAKE"; col = C_RED; }
        else if (w == "abs")      { label = "ABS"; }
        else if (w == "temp")     { label = "TEMP"; col = C_RED; }
        else if (w == "tyre")     { label = "TYRE"; }
        else if (w == "airbag")   { label = "AIRBAG"; col = C_RED; }
        else if (w == "seatbelt") { label = "BELT"; col = C_RED; }
        else if (w == "fuel")     { label = "FUEL"; }
        else if (w == "adblue")   { label = "AdBlue"; }
        else if (w == "air")      { label = "AIR"; col = C_RED; }
        else if (w == "door")     { label = "DOOR"; col = C_RED; }
        else if (w == "esp")      { label = "ESP"; }
    }

    // ════════════════════════════════════════════════════════════════════════════════════
    // THE LAYOUT. Paul approved the mockup (2026-10-09, scratchpad mockups/dashboards.html):
    // every position below is that drawing's, in its 1600 x 900 frame with the panel from y 596
    // to 900 (y down), scaled by u to the panel drawn here. Each part has an ANCHOR: the wheel
    // keeps its distance from the left edge (L), the cluster stays centred (C), the controls keep
    // theirs from the right edge (R). A window too narrow for one row at a readable size puts the
    // cluster on a row of its own above the wheel and controls. Sizes are UI units, and the panel's
    // height follows the UI scale (Paul 2026-10-09: "make them scale with the UI"), so at UI
    // scale 1.5 everything is half as big again, as the rest of the interface is.
    // ════════════════════════════════════════════════════════════════════════════════════
    struct Anchor { F32 ox = 0.f, top0 = 900.f, u = 1.f; };
    struct Geo
    {
        F32 W = 1600.f;
        Anchor L, C, R, cur;
        F32 us = 1.f;          // the strip of buttons above the panel
        F32 bandB = 0.f;       // the panel's bottom and top, view-local y
        F32 panelTop = 304.f;
        bool compact = false;
    } g;
    // Below this the figures and lamps get too small to read: a narrow window goes to two rows
    // rather than shrinking past it, and the strip's buttons never go under their own minimum.
    const F32 U_MIN = 0.75f;
    const F32 US_MIN = 0.85f, US_MAX = 1.3f;
    // the lamp row's narrowest, in mockup units: 7 buttons of 34 ("LAMPS" in 10 px bold) and 6 gaps of 5
    const F32 LAMP_MIN_W = 7.f * 34.f + 6.f * 5.f;
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
    // an arc between two clock angles (d0 < d1), as arcW wants it
    void clockArc(F32 cx, F32 cy, F32 r, F32 w, F32 d0, F32 d1, const LLColor4& c, S32 seg = 32)
    {
        arcW(cx, cy, r, w, (90.f - d1) * DEG_TO_RAD, (90.f - d0) * DEG_TO_RAD, c, seg);
    }

    const LLFontGL* font(F32 mock_px, bool bold = false, bool mono = false)
    {
        return face(mono ? "B612Mono" : "B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(mock_px) * 1.25f));
    }

    // text at mock coordinates
    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, bool mono = false,
           LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        text(font(px, bold, mono), s, x, y, c, h, LLFontGL::VCENTER);
    }

    const LLColor4 CHROME_HI(0.85f, 0.87f, 0.89f, 1.f), CHROME_LO(0.17f, 0.18f, 0.20f, 1.f);
    const LLColor4 FACE_HI(0.114f, 0.125f, 0.149f, 1.f), FACE_LO(0.012f, 0.012f, 0.016f, 1.f);
    const LLColor4 TFACE_HI(0.133f, 0.157f, 0.188f, 1.f), TFACE_LO(0.027f, 0.035f, 0.043f, 1.f);
    const LLColor4 TRUCK_NEEDLE(1.f, 0.54f, 0.086f, 1.f);

    void chromeDisc(F32 cx, F32 cy, F32 r) { circleShaded(cx, cy, r, CHROME_HI, CHROME_LO, 64); }

    struct Dial
    {
        F32 cx, cy, r, max, major, label_every, minor, div, red, green_lo, green_hi, value, mark;
        std::string unit;
        bool truck;
        LLColor4 needle;
        F32 num_px;
    };

    // A round analogue gauge after the mockup's dial(): chrome ring, dark face, green / red arcs,
    // ticks and figures from -135 to +135 degrees, a glowing needle with a tail, a chrome hub
    // and a touch of glass.
    void drawDial(const Dial& o)
    {
        auto ang = [&](F32 v) { return -135.f + 270.f * llclamp(v / llmax(1.f, o.max), 0.f, 1.04f); };
        chromeDisc(o.cx, o.cy, o.r * 1.09f);
        circleF(o.cx, o.cy, o.r * 1.03f, LLColor4(0.02f, 0.02f, 0.024f, 1.f), 64);
        circleShaded(o.cx, o.cy, o.r, o.truck ? TFACE_HI : FACE_HI, o.truck ? TFACE_LO : FACE_LO, 64);
        if (o.green_hi > o.green_lo)
        {
            clockArc(o.cx, o.cy, o.r * 0.86f, o.r * 0.07f, ang(o.green_lo), ang(o.green_hi), C_GREEN);
        }
        if (o.red > 0.f && o.red < o.max)
        {
            clockArc(o.cx, o.cy, o.r * 0.86f, o.r * 0.09f, ang(o.red), ang(o.max), C_RED);
        }
        F32 x0, y0, x1, y1;
        if (o.minor > 0.f)
        {
            for (F32 v = 0.f; v <= o.max + 0.01f; v += o.minor)
            {
                const F32 q = v / o.major;
                if (fabsf(q - llround(q)) < 0.001f) continue;
                pt(o.cx, o.cy, o.r * 0.88f, ang(v), x0, y0);
                pt(o.cx, o.cy, o.r * 0.95f, ang(v), x1, y1);
                lineW(x0, y0, x1, y1, llmax(1.f, o.r * 0.012f), (o.red > 0.f && v >= o.red) ? C_RED : LLColor4(0.79f, 0.8f, 0.82f, 1.f));
            }
        }
        const LLFontGL* nf = face("B612", LLFontGL::BOLD, o.num_px * 1.25f);
        for (F32 m = 0.f; m <= o.max + 0.01f; m += o.major)
        {
            const bool red = o.red > 0.f && m >= o.red;
            pt(o.cx, o.cy, o.r * 0.78f, ang(m), x0, y0);
            pt(o.cx, o.cy, o.r * 0.96f, ang(m), x1, y1);
            lineW(x0, y0, x1, y1, llmax(2.f, o.r * 0.03f), red ? C_RED : C_WHITE);
            const F32 q = o.label_every > 0.f ? m / o.label_every : 0.f;
            if (o.label_every <= 0.f || fabsf(q - llround(q)) < 0.001f)
            {
                F32 nx, ny;
                pt(o.cx, o.cy, o.r * 0.62f, ang(m), nx, ny);
                text(nf, llformat("%d", (S32)llround(m / o.div)), nx, ny, red ? C_RED : C_WHITE);
            }
        }
        if (o.mark > 0.f)
        {
            F32 ax, ay, bx, by, cx2, cy2;
            pt(o.cx, o.cy, o.r, ang(o.mark), ax, ay);
            pt(o.cx, o.cy, o.r * 0.9f, ang(o.mark) - 3.f, bx, by);
            pt(o.cx, o.cy, o.r * 0.9f, ang(o.mark) + 3.f, cx2, cy2);
            triF(ax, ay, bx, by, cx2, cy2, C_AMBER);
        }
        if (!o.unit.empty())
        {
            text(face("B612", LLFontGL::NORMAL, o.r * 0.11f), o.unit, o.cx, o.cy - o.r * 0.30f, C_GREY);
        }
        // the needle, its glow under it
        const F32 a = ang(o.value) * DEG_TO_RAD;
        const F32 sx = sinf(a), sy = cosf(a);           // along the needle
        const F32 nx = -sy, ny = sx;                    // across it
        const F32 w = llmax(3.f, o.r * 0.035f);
        for (S32 pass = 0; pass < 2; ++pass)
        {
            const F32 ww = pass == 0 ? w * 2.4f : w;
            const LLColor4 c = pass == 0 ? alpha(o.needle, 0.25f) : o.needle;
            const F32 tx = o.cx - sx * o.r * 0.18f, ty = o.cy - sy * o.r * 0.18f;
            const F32 hx = o.cx + sx * o.r * 0.90f, hy = o.cy + sy * o.r * 0.90f;
            quadF(tx + nx * ww, ty + ny * ww, tx - nx * ww, ty - ny * ww,
                  hx - nx * ww * 0.3f, hy - ny * ww * 0.3f, hx + nx * ww * 0.3f, hy + ny * ww * 0.3f, c);
        }
        chromeDisc(o.cx, o.cy, o.r * 0.11f);
        circleF(o.cx, o.cy, o.r * 0.07f, LLColor4(0.043f, 0.043f, 0.051f, 1.f), 24);
        ellipseF(o.cx, o.cy + o.r * 0.45f, o.r * 0.8f, o.r * 0.5f, LLColor4(1.f, 1.f, 1.f, 0.05f), 40);
    }

    // a small sweep (fuel, coolant), after the mockup's mini()
    void drawMini(F32 cx, F32 cy, F32 r, F32 frac, const std::string& lo, const std::string& hi, const std::string& name,
                  F32 warn_lo, F32 warn_hi)
    {
        auto ang = [](F32 t) { return -60.f + 120.f * t; };
        clockArc(cx, cy, r, llmax(1.5f, r * 0.1f), -60.f, 60.f, LLColor4(0.79f, 0.8f, 0.82f, 1.f), 24);
        if (warn_lo > 0.f) clockArc(cx, cy, r, llmax(2.f, r * 0.16f), -60.f, ang(warn_lo), C_RED, 8);
        if (warn_hi > 0.f) clockArc(cx, cy, r, llmax(2.f, r * 0.16f), ang(warn_hi), 60.f, C_RED, 8);
        F32 x0, y0, x1, y1;
        for (S32 i = 0; i <= 4; ++i)
        {
            pt(cx, cy, r * 0.75f, ang(i / 4.f), x0, y0);
            pt(cx, cy, r * 1.03f, ang(i / 4.f), x1, y1);
            lineW(x0, y0, x1, y1, 1.5f, LLColor4(0.79f, 0.8f, 0.82f, 1.f));
        }
        pt(cx, cy, r * 1.02f, ang(llclamp(frac, 0.f, 1.f)), x1, y1);
        lineW(cx, cy, x1, y1, llmax(2.f, r * 0.11f), C_NEEDLE);
        circleF(cx, cy, llmax(2.f, r * 0.15f), C_GREY, 16);
        const LLFontGL* f = face("B612", LLFontGL::NORMAL, llmax(7.f, r * 0.55f));
        pt(cx, cy, r + r * 0.5f, -60.f, x0, y0);
        pt(cx, cy, r + r * 0.5f, 60.f, x1, y1);
        text(f, lo, x0, y0 - r * 0.15f, C_GREY);
        text(f, hi, x1, y1 - r * 0.15f, C_GREY);
        text(f, name, cx, cy - r * 0.6f, C_GREY);
    }

    // ---- telltales (ISO 2575 colours) ----
    void arrowLamp(F32 x, F32 y, F32 s, bool left, bool on)
    {
        const F32 d = left ? -1.f : 1.f;
        const LLColor4 c = on ? C_GREEN : C_OFF;
        triF(x + d * s, y, x + d * s * 0.1f, y + s * 0.75f, x + d * s * 0.1f, y - s * 0.75f, c);
        rectF(llmin(x - d * s * 0.9f, x + d * s * 0.1f), y - s * 0.3f, llmax(x - d * s * 0.9f, x + d * s * 0.1f), y + s * 0.3f, c);
        if (on) circleF(x, y, s * 1.2f, alpha(C_GREEN, 0.12f), 20);
    }
    void beamLamp(F32 x, F32 y, F32 s, bool main_beam, bool on)
    {
        const LLColor4 c = on ? (main_beam ? C_BLUE : C_GREEN) : C_OFF;
        const F32 w = llmax(1.5f, s * 0.12f);
        clockArc(x - s * 0.15f, y, s * 0.45f, w, 180.f, 360.f, c, 14);
        lineW(x - s * 0.15f, y + s * 0.45f, x - s * 0.15f, y - s * 0.45f, w, c);
        for (S32 i = -1; i <= 1; ++i)
        {
            const F32 yy = y + i * s * 0.3f;
            lineW(x + s * 0.05f, yy, x + s * 0.6f, main_beam ? yy : yy - s * 0.2f, llmax(1.5f, s * 0.1f), c);
        }
    }
    void parkLamp(F32 x, F32 y, F32 s, bool on)
    {
        const LLColor4 c = on ? C_RED : C_OFF;
        arcW(x, y, s * 0.36f, llmax(1.5f, s * 0.08f), 0.f, 2.f * PI_F, c, 24);
        clockArc(x, y, s * 0.52f, llmax(1.5f, s * 0.07f), -140.f, -40.f, c, 10);
        clockArc(x, y, s * 0.52f, llmax(1.5f, s * 0.07f), 40.f, 140.f, c, 10);
        text(face("B612", LLFontGL::BOLD, s * 0.55f), "P", x, y, c);
    }
    F32 wordLamp(F32 x, F32 y, const std::string& word, const LLColor4& colour, bool on)
    {
        const F32 w = S(10.f + word.size() * 9.f);
        roundRectF(x, y - S(11.f), x + w, y + S(11.f), S(4.f), alpha(on ? colour : C_OFF, on ? 0.22f : 0.5f), alpha(on ? colour : C_OFF, on ? 0.22f : 0.5f));
        T(x + w * 0.5f, y, word, 13.f, on ? colour : LLColor4(0.235f, 0.24f, 0.26f, 1.f), true);
        return w;
    }

    /** The lamp row: indicators, dipped, main beam, handbrake, cruise, then every warning in its
     *  ISO 2575 colour. `warnings_only`: the bike shows its indicators and beams by the screen. */
    void telltales(F32 x0, F32 x1, F32 y, const WolfDrive::Shown& s, bool dark, bool warnings_only)
    {
        if (dark)
        {
            roundRectF(x0, y - S(18.f), x1, y + S(18.f), S(10.f), LLColor4(0.008f, 0.008f, 0.012f, 0.85f), LLColor4(0.008f, 0.008f, 0.012f, 0.85f));
        }
        const bool flash = blink(0.7f);
        F32 x = x0 + S(16.f);
        if (!warnings_only)
        {
            const bool left = s.mIndicators == 1 || s.mIndicators == 3;
            const bool right = s.mIndicators == 2 || s.mIndicators == 3;
            arrowLamp(x0 + S(24.f), y, S(13.f), true, left && flash);
            arrowLamp(x1 - S(24.f), y, S(13.f), false, right && flash);
            x = x0 + S(56.f);
            beamLamp(x, y, S(26.f), false, s.mLights == 1); x += S(34.f);
            beamLamp(x, y, S(26.f), true, s.mLights == 2);  x += S(40.f);
            parkLamp(x, y, S(30.f), s.mHandbrake);          x += S(28.f);
            x += wordLamp(x, y, "CRUISE", C_GREEN, s.mCruise) + S(10.f);
        }
        for (const std::string& w : s.mWarnings)
        {
            std::string label;
            LLColor4 col;
            warningLook(w, label, col);
            if (x + S(10.f + label.size() * 9.f) > x1 - S(48.f)) break;
            x += wordLamp(x, y, label, col, true) + S(8.f);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════

WolfDashboard::WolfDashboard(const Params& p)
:   LLUICtrl(p)
{
}

WolfDashboard::~WolfDashboard()
{
}

void WolfDashboard::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ llmin(l, r), llmin(b, t), llmax(l, r), llmax(b, t), id, tip });
}

const WolfDashboard::Hit* WolfDashboard::hitAt(S32 x, S32 y) const
{
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (x >= it->mL && x <= it->mR && y >= it->mB && y <= it->mT)
        {
            return &*it;
        }
    }
    return nullptr;
}

// With the background on the whole panel is solid (a click on it is not a click on the world);
// with it off only the controls are, so the gaps between the gauges belong to the world.
bool WolfDashboard::onPanel(S32 x, S32 y) const
{
    if (!mDrawn)
    {
        return false;
    }
    if (hitAt(x, y))
    {
        return true;
    }
    if (!gSavedSettings.getBOOL("WolfDashboardBackground"))
    {
        return false;
    }
    return y >= mPanelB && y <= mPanelT;
}

namespace
{
    // ---- the controls (after the mockup's button / strip / pedals / selector / lampButtons) ----
    struct Ctx
    {
        WolfDashboard* self;
        std::function<void(F32, F32, F32, F32, WolfDashboard::EHit, const std::string&)> hit;
        S32 pressed;
    };

    void button(Ctx& c, F32 l, F32 b, F32 r, F32 t, const std::string& label, bool lit, WolfDashboard::EHit id,
                const std::string& tip, F32 px = 13.f)
    {
        const bool down = c.pressed == id;
        roundRectF(l, b, r, t, (t - b) * 0.22f,
                   down ? LLColor4(0.09f, 0.09f, 0.10f, 0.96f) : LLColor4(0.227f, 0.239f, 0.263f, 0.96f),
                   down ? LLColor4(0.16f, 0.16f, 0.18f, 0.96f) : LLColor4(0.086f, 0.09f, 0.102f, 0.96f));
        frameW(l, b, r, t, 1.2f, lit ? C_AMBER : LLColor4(0.337f, 0.353f, 0.384f, 1.f));
        if (lit)
        {
            rectF(l + S(4.f), t - S(5.5f), r - S(4.f), t - S(3.f), C_AMBER);   // the lit bar: never colour of text alone
        }
        T((l + r) * 0.5f, (b + t) * 0.5f, label, px, lit ? C_AMBER : C_WHITE, true);
        c.hit(l, b, r, t, id, tip);
    }

    /** The buttons above the panel, right-aligned, in their own size (never under US_MIN), wrapping
     *  onto another line above when the window is too narrow. Returns the top of the highest line. */
    F32 strip(Ctx& c, const WolfDrive::Shown& s, WolfDrive::EType ty, const std::string& vehicle)
    {
        Anchor sa;
        sa.u = g.us;
        use(sa);
        const bool bg = gSavedSettings.getBOOL("WolfDashboardBackground");
        struct B { F32 w; std::string label; bool lit; WolfDashboard::EHit id; std::string tip; };
        const B bs[] =
        {
            { 30.f, "X", false, WolfDashboard::H_CLOSE, "Close the dashboard. World > Car Dashboard (or the toolbar button) brings it back." },
            { 70.f, "HELP", false, WolfDashboard::H_HELP, "How the dashboard works, setting up a wheel and keys, and wolfDashboard for vehicle makers" },
            { 120.f, "CONTROLS", false, WolfDashboard::H_CONTROLS, "Driving Controls: a steering wheel, pedals or gamepad, and the keys for the horn, lamps and gears" },
            { 70.f, "MAP", false, WolfDashboard::H_MAP, "World Map, centred on where you are driving" },
            { 92.f, "PANEL", bg, WolfDashboard::H_PANEL, bg ? "Background on: click to float the instruments over the world" : "Background off: click to put the panel back" },
            { 92.f, "TRIP 0", false, WolfDashboard::H_TRIP, "Reset the trip meter (and the bike's lean record)" },
            { 92.f, s.mMph ? "MPH" : "KM/H", false, WolfDashboard::H_UNITS, s.mMph ? "Speed in miles per hour: click for km/h" : "Speed in km/h: click for miles per hour" },
            { 92.f, "BIKE", ty == WolfDrive::TYPE_BIKE, WolfDashboard::H_TYPE_BIKE, "Motorbike dashboard (superbike TFT, handlebars, sequential gears)" },
            { 92.f, "TRUCK", ty == WolfDrive::TYPE_TRUCK, WolfDashboard::H_TYPE_TRUCK, "Truck dashboard (Scania style, 12-speed range-splitter: manual or automatic, air brakes)" },
            { 92.f, "CAR", ty == WolfDrive::TYPE_CAR, WolfDashboard::H_TYPE_CAR, "Car dashboard (Porsche 911 style, 8 gears: manual or automatic)" },
        };
        const F32 margin = S(14.f), gap = S(8.f), h = S(30.f), line = S(38.f);
        F32 x = g.W - margin;
        F32 bot = g.panelTop + S(8.f);
        S32 lines = 1;
        for (const B& b : bs)
        {
            const F32 w = S(b.w);
            if (x - w < margin && x < g.W - margin)
            {
                // no room: a new line above, from the right again
                x = g.W - margin;
                bot += line;
                ++lines;
            }
            button(c, x - w, bot, x, bot + h, b.label, b.lit, b.id, b.tip);
            x -= w + gap;
        }
        const F32 top = bot + h;
        // which vehicle, and whose numbers: on the line's left, when everything fitted on one line
        if (lines == 1)
        {
            std::string who = vehicle;
            if (who.size() > 32) who = who.substr(0, 32) + "...";
            const std::string label = (who.empty() ? std::string() : who + "  -  ")
                                    + (s.mScripted ? "values from the vehicle's script" : "viewer's own model");
            const LLFontGL* f = font(13.f);
            const F32 lw = f ? f->getWidthF32(label) : S(200.f);
            const F32 lr = llmin(margin + lw + S(24.f), x - gap);
            if (lr > margin + S(80.f))
            {
                const F32 lb = g.panelTop + S(8.f);
                roundRectF(margin, lb, lr, lb + h, S(6.f), LLColor4(0.f, 0.f, 0.f, 0.55f), LLColor4(0.f, 0.f, 0.f, 0.55f));
                text(f, label, margin + S(12.f), lb + h * 0.5f, s.mScripted ? C_GREEN : C_GREY, LLFontGL::LEFT, LLFontGL::VCENTER);
            }
        }
        return top;
    }

    // the pedals (car / truck) or the throttle and brake (bike), each with how far it is pressed
    void pedals(Ctx& c, F32 xl, F32 xr, F32 ytop, F32 h_mock, F32 brake, F32 accel, bool bike)
    {
        const F32 w = xr - xl, h = S(h_mock);
        const F32 pw = w * 0.42f;
        struct P { F32 l; F32 v; bool brk; const char* name; WolfDashboard::EHit id; const char* tip; };
        const P ps[2] =
        {
            { xl, brake, true, "BRAKE", WolfDashboard::H_BRAKE,
              "Brake: hold it (or Down / S, or your brake pedal). Held on its own it is the back key, as on the keyboard." },
            { xr - pw, accel, false, bike ? "THROTTLE" : "ACCEL", WolfDashboard::H_THROTTLE,
              "Accelerator: hold it (or Up / W, or your pedal / trigger)" },
        };
        for (const P& p : ps)
        {
            const F32 sink = p.v * h * 0.05f;
            const F32 top = ytop - sink, bot = ytop - h * 0.84f + sink;
            const LLColor4 col = p.v > 0.f ? (p.brk ? LLColor4(0.55f, 0.14f, 0.13f, 1.f) : LLColor4(0.227f, 0.416f, 0.659f, 1.f))
                                           : LLColor4(0.29f, 0.306f, 0.333f, 1.f);
            roundRectF(p.l - 1.f, bot - 1.f, p.l + pw + 1.f, top + 1.f, pw * 0.18f, LLColor4(0.04f, 0.04f, 0.045f, 1.f), LLColor4(0.04f, 0.04f, 0.045f, 1.f));
            roundRectF(p.l, bot, p.l + pw, top, pw * 0.18f, col, col * 0.55f);
            for (S32 k = 1; k < 7; ++k)
            {
                const F32 y = top - k * h * 0.11f;
                if (y < bot + S(6.f)) break;
                roundRectF(p.l + pw * 0.18f, y - S(3.f), p.l + pw * 0.82f, y, S(1.5f), LLColor4(0.78f, 0.8f, 0.82f, 0.45f), LLColor4(0.78f, 0.8f, 0.82f, 0.45f));
            }
            const F32 bx = p.brk ? p.l + pw + S(5.f) : p.l - S(10.f);
            const F32 btop = ytop, bbot = ytop - h * 0.84f;
            rectF(bx, bbot, bx + S(5.f), btop, C_OFF);
            rectF(bx, bbot, bx + S(5.f), bbot + (btop - bbot) * llclamp(p.v, 0.f, 1.f), p.brk ? C_RED : C_GREEN);
            T(p.l + pw * 0.5f, ytop - h * 0.93f, p.name, 11.f, C_GREY);
            c.hit(p.l, bbot, p.l + pw, btop, p.id, p.tip);
        }
    }

    void selector(Ctx& c, F32 xl, F32 xr, F32 ytop, F32 ybot, WolfDrive::EType ty, const char* under)
    {
        WolfDrive& d = WolfDrive::instance();
        const bool ab = d.autoBox();
        const std::vector<S32> pos = WolfDrive::selectorPositions(ty, ab);
        const S32 n = (S32)pos.size();
        const F32 w = xr - xl, h = ytop - ybot;
        const F32 col_r = xl + w * 0.55f;
        const F32 cell = h / (F32)n;
        roundRectF(xl, ybot, col_r, ytop, (col_r - xl) * 0.2f, LLColor4(0.043f, 0.043f, 0.051f, 1.f), LLColor4(0.043f, 0.043f, 0.051f, 1.f));
        frameW(xl, ybot, col_r, ytop, 1.f, LLColor4(0.227f, 0.239f, 0.263f, 1.f));
        S32 shown = d.selector();
        if (ty != WolfDrive::TYPE_BIKE && !ab && shown >= 1) shown = WolfDrive::GEAR_D;   // MANUAL: the forward-gears slot
        for (S32 i = 0; i < n && i < 8; ++i)
        {
            const S32 idx = n - 1 - i;                    // highest at the top
            const S32 gsel = pos[idx];
            const F32 ct = ytop - cell * i, cb = ct - cell;
            const bool on = shown == gsel;
            std::string label = WolfDrive::gearLabel(gsel);
            // MANUAL car / truck: the forward-gears slot shows the gear held (1 when in R or N);
            // AUTO truck: Opticruise's A
            if (ty != WolfDrive::TYPE_BIKE && gsel == WolfDrive::GEAR_D)
            {
                label = !ab ? llformat("%d", llmax(1, d.selector())) : ty == WolfDrive::TYPE_TRUCK ? std::string("A") : std::string("D");
            }
            if (on)
            {
                roundRectF(xl + S(4.f), cb + S(3.f), col_r - S(4.f), ct - S(3.f), cell * 0.2f, LLColor4(0.933f, 0.94f, 0.95f, 1.f), LLColor4(0.8f, 0.81f, 0.83f, 1.f));
            }
            T((xl + col_r) * 0.5f, (cb + ct) * 0.5f, label, cell > S(40.f) ? 20.f : 16.f,
              on ? C_BLACK : (gsel == WolfDrive::GEAR_R ? LLColor4(0.9f, 0.4f, 0.36f, 1.f) : C_GREY), true);
            const std::string what = gsel == WolfDrive::GEAR_D ? (ab ? label + " (the automatic)" : std::string("a forward gear (first, or the gear you are in)"))
                                   : gsel == WolfDrive::GEAR_P ? std::string("P (park)")
                                   : gsel == WolfDrive::GEAR_R ? std::string("R (reverse)")
                                   : gsel == WolfDrive::GEAR_N ? std::string("N (neutral)") : "gear " + label;
            c.hit(xl, cb, col_r, ct, (WolfDashboard::EHit)(WolfDashboard::H_SEL_0 + idx),
                  "Put it in " + what + ". Seated, each step is one Page Up / Page Down to the vehicle's script.");
        }
        const F32 bl = col_r + w * 0.07f;
        const F32 mid = (ybot + ytop) * 0.5f;
        button(c, bl, mid + S(4.f), xr, ytop, "+", false, WolfDashboard::H_SHIFT_UP,
               ab ? "Gear up: in D / A takes a gear yourself (M) (Page Up, or your gear-up key or button)"
                  : "Gear up: one gear, staying in top gear at the top (Page Up, or your gear-up key or button)", 20.f);
        button(c, bl, ybot, xr, mid - S(4.f), "-", false, WolfDashboard::H_SHIFT_DOWN,
               ab ? "Gear down: in D / A takes a gear yourself (M) (Page Down, or your gear-down key or button)"
                  : ty == WolfDrive::TYPE_BIKE ? "Gear down: one gear (Page Down, or your gear-down key or button)"
                                               : "Gear down: one gear; below first is N, then R (Page Down, or your gear-down key or button)", 20.f);
        T((bl + xr) * 0.5f, ybot - S(14.f), under, 10.f, C_GREY);
        if (ty != WolfDrive::TYPE_BIKE)
        {
            // the gearbox mode, above the lever (Paul: "select automatic or manual gears default to manual")
            const F32 mt = ytop + S(30.f), mb = ytop + S(6.f);
            button(c, xl, mb, xr, mt, ab ? "AUTO" : "MANUAL", ab, WolfDashboard::H_BOX_MODE,
                   ab ? "Gearbox: AUTOMATIC - it picks the gear from the speed; + / - take one yourself (M). Click for MANUAL."
                      : "Gearbox: MANUAL - Page Up / Page Down (or + / -) step one gear; past top you stay in top, below 1 is N then R. Click for AUTOMATIC.",
                   11.f);
        }
    }

    /** " (key H)" when the driver gave the action a key (Driving Controls), else nothing. */
    std::string keyHint(S32 action)
    {
        const std::string k = WolfDrive::instance().actionCfg(action).mKey;
        return k.empty() ? std::string() : " (key " + k + ")";
    }

    /** The lamp / horn row. Its bottom-right corner sits where the design puts it, but it is never
     *  narrower than LAMP_MIN_W mockup units (so "LAMPS" fits) nor smaller than the strip's size
     *  (g.us, the UI-scale minimum): on a small panel it grows up and to the left instead of
     *  shrinking with the gauges (Paul: "sometimes the icons are tiny"). wolf_dashboard.js lampButtons. */
    void lampButtons(Ctx& c, F32 xl_in, F32 xr_in, F32 ytop_in, const WolfDrive::Shown& s, U32 held)
    {
        const S32 N = 7;
        const Anchor was = g.cur;
        const F32 k = llmax(was.u, g.us);
        const F32 rw = llmax((xr_in - xl_in) / was.u, LAMP_MIN_W);
        const F32 bottom = ytop_in - S(30.f);
        Anchor lb;
        lb.u = k;
        lb.ox = xr_in - rw * k;
        lb.top0 = bottom + 30.f * k;
        use(lb);
        const F32 xl = X(0.f), xr = X(rw), ytop = YM(0.f);
        const F32 gap = S(5.f), bw = (xr - xl - gap * (N - 1)) / N, h = S(30.f);
        const bool horn_builtin = WolfDrive::instance().actionCfg(WolfDrive::ACT_HORN).mOut == WolfDrive::OUT_NONE;
        struct B { std::string l; bool lit; WolfDashboard::EHit id; std::string tip; };
        const B bs[N] =
        {
            { "<", s.mIndicators == 1, WolfDashboard::H_IND_L, "Left indicator (on / off)" + keyHint(WolfDrive::ACT_IND_LEFT) },
            { s.mLights == 2 ? "MAIN" : "LAMPS", s.mLights > 0, WolfDashboard::H_LIGHTS, "Lights: off, dipped, main beam" + keyHint(WolfDrive::ACT_LIGHTS) },
            { "HAZ", s.mIndicators == 3, WolfDashboard::H_HAZARDS, "Hazard lights (on / off)" + keyHint(WolfDrive::ACT_HAZARDS) },
            { "P", s.mHandbrake, WolfDashboard::H_HANDBRAKE, "Handbrake (on / off)" + keyHint(WolfDrive::ACT_HANDBRAKE) },
            { "CRZ", s.mCruise, WolfDashboard::H_CRUISE, "Cruise control (on / off)" + keyHint(WolfDrive::ACT_CRUISE) },
            { "HORN", (held & WolfDrive::BTN_HORN) != 0, WolfDashboard::H_HORN,
              (horn_builtin ? std::string("Horn: none set - the built-in horn, heard in-world (Wolf Territories)")
                            : std::string("Horn: sends what you set in Driving Controls")) + keyHint(WolfDrive::ACT_HORN) },
            { ">", s.mIndicators == 2, WolfDashboard::H_IND_R, "Right indicator (on / off)" + keyHint(WolfDrive::ACT_IND_RIGHT) },
        };
        for (S32 i = 0; i < N; ++i)
        {
            const F32 x = xl + i * (bw + gap);
            button(c, x, ytop - h, x + bw, ytop, bs[i].l, bs[i].lit, bs[i].id, bs[i].tip, 10.f);
        }
        use(was);
    }

    // ---- wheels ----
    void spoke(F32 cx, F32 cy, F32 a_deg, F32 r_in, F32 r_out, F32 w_in, F32 w_out, const LLColor4& top, const LLColor4& bot)
    {
        const F32 a = a_deg * DEG_TO_RAD;
        const F32 ux = sinf(a), uy = cosf(a), nx = -uy, ny = ux;
        const F32 x0 = cx + ux * r_in, y0 = cy + uy * r_in, x1 = cx + ux * r_out, y1 = cy + uy * r_out;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0 + nx * w_in, y0 + ny * w_in, top); vtx(x0 - nx * w_in, y0 - ny * w_in, bot); vtx(x1 - nx * w_out, y1 - ny * w_out, bot);
        vtx(x0 + nx * w_in, y0 + ny * w_in, top); vtx(x1 - nx * w_out, y1 - ny * w_out, bot); vtx(x1 + nx * w_out, y1 + ny * w_out, top);
        gGL.end();
    }

    /** A steering wheel facing the driver. Car: flat-bottomed, three spokes, stitched leather,
     *  the yellow 12 o'clock stripe. Truck (Paul: "truck use round steering wheel"): round, big,
     *  two spokes swept down and a large boss. `turn` -1..1, drawn up to `lock_deg`. */
    void wheel(F32 cx, F32 cy, F32 R, F32 turn, bool truck)
    {
        const F32 rot = turn * (truck ? 120.f : 90.f);
        const F32 rim = R * (truck ? 0.11f : 0.14f);
        const F32 flat = truck ? 2.f : cosf(25.f * DEG_TO_RAD);   // the flat bottom's chord (car only)
        // shadow, then the rim: leather, lit from above
        const S32 seg = 96;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 d0 = 360.f * i / seg, d1 = 360.f * (i + 1) / seg;
            F32 r0 = R, r1 = R;
            // the flat bottom: within 25 degrees either side of 6 o'clock, follow the chord
            auto chord = [&](F32 d) { const F32 c = cosf(d * DEG_TO_RAD); return (c < -flat) ? R * flat / -c : R; };
            r0 = chord(d0); r1 = chord(d1);
            const F32 a0 = (d0 + rot) * DEG_TO_RAD, a1 = (d1 + rot) * DEG_TO_RAD;
            const F32 l0 = 0.5f + 0.5f * cosf(d0 * DEG_TO_RAD), l1 = 0.5f + 0.5f * cosf(d1 * DEG_TO_RAD);
            const LLColor4 c0 = lerp(LLColor4(0.02f, 0.02f, 0.024f, 1.f), LLColor4(0.23f, 0.23f, 0.25f, 1.f), l0);
            const LLColor4 c1 = lerp(LLColor4(0.02f, 0.02f, 0.024f, 1.f), LLColor4(0.23f, 0.23f, 0.25f, 1.f), l1);
            const F32 s0 = sinf(a0), k0 = cosf(a0), s1 = sinf(a1), k1 = cosf(a1);
            vtx(cx + (r0 - rim) * s0, cy + (r0 - rim) * k0, c0); vtx(cx + r0 * s0, cy + r0 * k0, c0); vtx(cx + r1 * s1, cy + r1 * k1, c1);
            vtx(cx + (r0 - rim) * s0, cy + (r0 - rim) * k0, c0); vtx(cx + r1 * s1, cy + r1 * k1, c1); vtx(cx + (r1 - rim) * s1, cy + (r1 - rim) * k1, c1);
        }
        gGL.end();
        if (!truck)
        {
            // the stitching on the top of the rim
            for (F32 d = -70.f; d < 70.f; d += 4.f)
            {
                F32 x0, y0, x1, y1;
                pt(cx, cy, R - rim * 0.5f, d + rot, x0, y0);
                pt(cx, cy, R - rim * 0.5f, d + 2.2f + rot, x1, y1);
                lineW(x0, y0, x1, y1, llmax(1.f, R * 0.012f), LLColor4(0.71f, 0.23f, 0.18f, 0.9f));
            }
        }
        const LLColor4 sp_hi(0.353f, 0.369f, 0.396f, 1.f), sp_lo(0.145f, 0.153f, 0.17f, 1.f);
        const F32 hub = R * (truck ? 0.34f : 0.28f);
        if (truck)
        {
            spoke(cx, cy, -100.f + rot, hub * 0.9f, R - rim * 0.5f, R * 0.11f, R * 0.07f, sp_hi, sp_lo);
            spoke(cx, cy, 100.f + rot, hub * 0.9f, R - rim * 0.5f, R * 0.11f, R * 0.07f, sp_hi, sp_lo);
        }
        else
        {
            spoke(cx, cy, -90.f + rot, hub * 0.9f, R - rim * 0.6f, R * 0.13f, R * 0.09f, sp_hi, sp_lo);
            spoke(cx, cy, 90.f + rot, hub * 0.9f, R - rim * 0.6f, R * 0.13f, R * 0.09f, sp_hi, sp_lo);
            spoke(cx, cy, 180.f + rot, hub * 0.9f, R * flat - rim * 0.6f, R * 0.11f, R * 0.07f, sp_hi, sp_lo);
        }
        chromeDisc(cx, cy, hub);
        circleShaded(cx, cy, hub * 0.87f, LLColor4(0.10f, 0.105f, 0.12f, 1.f), LLColor4(0.03f, 0.03f, 0.035f, 1.f), 48);
        text(face("B612", LLFontGL::BOLD, hub * 0.32f), truck ? "GRIFFIN" : "WOLF", cx, cy, LLColor4(0.75f, 0.77f, 0.8f, 1.f));
        // the 12 o'clock mark: a stripe on the car, a stud on the truck
        F32 mx, my;
        pt(cx, cy, R - rim * 0.5f, rot, mx, my);
        if (truck)
        {
            circleF(mx, my, rim * 0.28f, LLColor4(0.75f, 0.77f, 0.8f, 1.f), 12);
        }
        else
        {
            F32 x0, y0, x1, y1;
            pt(cx, cy, R - rim, rot, x0, y0);
            pt(cx, cy, R, rot, x1, y1);
            lineW(x0, y0, x1, y1, R * 0.09f, LLColor4(0.957f, 0.76f, 0.10f, 1.f));
        }
    }

    /** The bike's handlebars from the saddle, after the mockup's handlebars(): the top yoke, fork
     *  caps, clip-ons, switchgear, ribbed grips, bar-end weights, brake (right) and clutch (left)
     *  levers that move with the brake and clutch, the brake reservoir and a throttle collar.
     *  Drawn in the mockup's own units around (cx, cy), scaled by k, turned with the steering. */
    void handlebars(F32 cx, F32 cy, F32 w_mock, F32 k, F32 turn, F32 brake, F32 clutch, F32 throttle)
    {
        const F32 a = -turn * 25.f * DEG_TO_RAD;
        const F32 ca = cosf(a), sa = sinf(a);
        const F32 hw = w_mock * 0.5f;
        // mockup-local (x right, y DOWN) to the view
        auto P = [&](F32 x, F32 y, F32& ox, F32& oy) { const F32 yy = -y; ox = cx + k * (x * ca - yy * sa); oy = cy + k * (x * sa + yy * ca); };
        auto quad = [&](F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, F32 x4, F32 y4, const LLColor4& c)
        {
            F32 a1, b1, a2, b2, a3, b3, a4, b4;
            P(x1, y1, a1, b1); P(x2, y2, a2, b2); P(x3, y3, a3, b3); P(x4, y4, a4, b4);
            quadF(a1, b1, a2, b2, a3, b3, a4, b4, c);
        };
        auto box = [&](F32 l, F32 t, F32 r, F32 b, const LLColor4& c) { quad(l, t, r, t, r, b, l, b, c); };
        auto line = [&](F32 x1, F32 y1, F32 x2, F32 y2, F32 w, const LLColor4& c)
        {
            F32 a1, b1, a2, b2;
            P(x1, y1, a1, b1); P(x2, y2, a2, b2);
            lineW(a1, b1, a2, b2, w * k, c);
        };
        auto disc = [&](F32 x, F32 y, F32 r, const LLColor4& hi, const LLColor4& lo)
        {
            F32 px, py;
            P(x, y, px, py);
            circleShaded(px, py, r * k, hi, lo, 28);
        };
        // the top yoke
        quad(-hw * 0.42f, -22.f, hw * 0.42f, -22.f, hw * 0.34f, 26.f, -hw * 0.34f, 26.f, LLColor4(0.169f, 0.18f, 0.2f, 1.f));
        line(-hw * 0.38f, -20.f, hw * 0.38f, -20.f, 2.f, LLColor4(0.43f, 0.45f, 0.48f, 1.f));
        disc(0.f, 2.f, 14.f, CHROME_HI, CHROME_LO);
        disc(0.f, 2.f, 7.f, LLColor4(0.1f, 0.11f, 0.125f, 1.f), LLColor4(0.1f, 0.11f, 0.125f, 1.f));
        for (S32 side = -1; side <= 1; side += 2)
        {
            const F32 d = (F32)side;
            const F32 fx = d * hw * 0.30f, fy = -4.f, ex = d * hw * 0.98f, ey = 10.f;
            // the clip-on bar
            quad(fx, fy - 7.f, ex, ey - 6.f, ex, ey + 6.f, fx, fy + 7.f, LLColor4(0.6f, 0.63f, 0.66f, 1.f));
            line(fx, fy - 7.f, ex, ey - 6.f, 1.5f, LLColor4(0.84f, 0.86f, 0.88f, 1.f));
            // the fork cap and its gold preload adjuster
            disc(fx, fy, 19.f, CHROME_HI, CHROME_LO);
            disc(fx, fy, 12.f, LLColor4(0.9f, 0.74f, 0.3f, 1.f), LLColor4(0.55f, 0.42f, 0.12f, 1.f));
            // switchgear housing and its buttons
            const F32 sx = d * hw * 0.55f;
            box(sx - 22.f, -11.f, sx + 22.f, 21.f, LLColor4(0.067f, 0.07f, 0.078f, 1.f));
            box(sx - 12.f, -6.f, sx - 2.f, 1.f, side < 0 ? LLColor4(0.79f, 0.8f, 0.82f, 1.f) : LLColor4(0.82f, 0.23f, 0.18f, 1.f));
            box(sx + 2.f, -6.f, sx + 12.f, 1.f, LLColor4(0.357f, 0.376f, 0.408f, 1.f));
            // the grip, ribbed, and the bar-end weight
            const F32 gx0 = d * hw * 0.66f, gx1 = d * hw * 0.97f;
            box(llmin(gx0, gx1), -4.f, llmax(gx0, gx1), 26.f, LLColor4(0.043f, 0.043f, 0.051f, 1.f));
            for (S32 r = 1; r < 9; ++r)
            {
                const F32 gx = gx0 + (gx1 - gx0) * r / 9.f;
                line(gx, -2.f, gx, 24.f, 2.f, LLColor4(0.15f, 0.157f, 0.176f, 1.f));
            }
            box(side > 0 ? gx1 : gx1 - 12.f, 0.f, side > 0 ? gx1 + 12.f : gx1, 22.f, LLColor4(0.7f, 0.72f, 0.75f, 1.f));
            // the lever ahead of the grip, pulled in by how far it is pressed
            const F32 pull = side > 0 ? brake : clutch;
            const F32 lx0 = d * hw * 0.60f, ly0 = -10.f, lx1 = d * hw * 1.02f, ly1 = -30.f + 22.f * pull;
            const LLColor4 lc = (side > 0 && pull > 0.f) ? C_RED : LLColor4(0.765f, 0.78f, 0.8f, 1.f);
            line(lx0, ly0, (lx0 + lx1) * 0.5f, (ly0 + ly1) * 0.5f - 4.f, 7.f, lc);
            line((lx0 + lx1) * 0.5f, (ly0 + ly1) * 0.5f - 4.f, lx1, ly1, 7.f, lc);
            disc(lx0, ly0, 7.f, LLColor4(0.43f, 0.45f, 0.48f, 1.f), LLColor4(0.169f, 0.18f, 0.2f, 1.f));
        }
        // the front brake reservoir, right
        box(hw * 0.40f, -58.f, hw * 0.40f + 22.f, -24.f, LLColor4(0.227f, 0.239f, 0.263f, 1.f));
        box(hw * 0.40f + 3.f, -44.f, hw * 0.40f + 19.f, -28.f, LLColor4(0.725f, 0.64f, 0.416f, 0.75f));
        // the throttle collar: lit while it is turned
        box(hw * 0.66f, -6.f, hw * 0.66f + 6.f, 28.f, throttle > 0.f ? C_GREEN : C_OFF);
    }

    /** The guide's words, per design (the HELP button, and by itself the first time each design
     *  is used - after the flight deck's guide, wolfflightdeck.cpp drawHelp). */
    struct HelpLine { S32 kind; const char* s; };   // 0 heading, 1 text, 2 indented, 3 gap, 4 code
    const HelpLine HELP_DRIVE[] =
    {
        { 0, "THE DASHBOARD" },
        { 1, "Opens by itself when you sit in a vehicle that is being driven," },
        { 2, "or from World > Car Dashboard, or its toolbar button. X closes it." },
        { 1, "CAR / TRUCK / BIKE: three designs. Your pick is kept for this" },
        { 2, "vehicle; a vehicle's script can choose one too." },
        { 1, "KM/H / MPH: the speed units.  TRIP 0: resets the trip meter." },
        { 1, "PANEL: the background on or off. Off, only the instruments and" },
        { 2, "controls are drawn and clicks in the gaps reach the world." },
        { 1, "MAP: the World Map, centred on you.  HELP: this guide." },
        { 3, "" },
        { 0, "GEARS" },
        { 1, "Car (8 gears) and truck (12): the button over the lever picks" },
        { 2, "MANUAL (the start) or AUTO, kept for each design." },
        { 1, "MANUAL: Page Up / + a gear up, Page Down / - a gear down; past" },
        { 2, "top you stay in top, below 1 is N then R. M by the gear." },
        { 1, "AUTO: car P R N D, truck R N A; it picks the gear (D / A)," },
        { 2, "and + / - take one yourself (M); click D / A to hand back." },
        { 2, "The truck shows the range (LOW / HIGH) and split (L / H)." },
        { 1, "Bike: sequential 1 N 2 3 4 5 6 with - / +." },
        { 1, "Each step sends Page Up / Page Down to the vehicle, as the keys do." },
        { 1, "The speed is the vehicle's own; the revs and gear are the" },
        { 2, "vehicle's script's, or the viewer's own model when it sends none." },
    };
    const HelpLine HELP_SETUP[] =
    {
        { 0, "DRIVING" },
        { 1, "Keys as always: Up / W, Down / S, Left / Right, Page Up / Down." },
        { 1, "Or drag the wheel (handlebars) and hold the pedals on screen." },
        { 3, "" },
        { 0, "A WHEEL, PEDALS OR A GAMEPAD" },
        { 1, "1  Plug it in. In Joystick Configuration choose it and enable it." },
        { 1, "2  CONTROLS (above) opens Driving Controls. For each control" },
        { 2, "press Detect and move it; the bar shows what is read." },
        { 1, "3  Tick 'Drive with this controller whenever I sit on anything'." },
        { 1, "Pedals that rest in the middle, or one combined pedal axis," },
        { 2, "are options there." },
        { 3, "" },
        { 0, "HORN, LAMPS, GEARS: YOUR KEYS" },
        { 1, "In Driving Controls each action has a key (Set key), a wheel" },
        { 2, "button (Detect) and what it sends the vehicle: nothing, Page" },
        { 2, "Up / Down, or a chat command on a channel. Reset puts them back." },
        { 1, "Horn with nothing set plays the built-in horn for the car," },
        { 2, "truck or bike, heard in-world by everyone near (Wolf only)." },
        { 2, "A vehicle with a horn of its own: set Horn to send its command." },
        { 3, "" },
        { 0, "FOR VEHICLE MAKERS" },
        { 1, "wolfDriveInput(driver) gives the exact wheel and pedals:" },
        { 4, "[steer, throttle, brake, clutch, gear, buttons, age]" },
        { 1, "wolfDashboard(driver, [...]) drives this dashboard:" },
        { 4, "wolfDashboard(llAvatarOnSitTarget(), [\"type\",\"car\"," },
        { 4, "    \"rpm\", 3200, \"gear\", \"3\", \"fuel\", 74]);" },
        { 1, "Both only work for a script in the vehicle the driver sits on." },
        { 2, "Other viewers send keys only: keep a control() event as well." },
        { 3, "" },
        { 1, "Click anywhere above the dashboard to close.  HELP reopens it." },
    };
}

void WolfDashboard::drawHelp(F32 top)
{
    const F32 W = (F32)getRect().getWidth(), Hv = (F32)getRect().getHeight();
    const F32 avail_h = Hv - top;
    if (avail_h < 120.f)
    {
        return;
    }
    const F32 cw = llmin(W * 0.92f, 1500.f);
    const F32 ch = avail_h * 0.92f;
    const F32 l = (W - cw) * 0.5f, r = l + cw, t = Hv - avail_h * 0.04f, b = t - ch;
    roundRectF(l - 2.f, b - 2.f, r + 2.f, t + 2.f, 10.f, LLColor4(0.3f, 0.315f, 0.335f, 1.f), LLColor4(0.03f, 0.03f, 0.035f, 1.f));
    roundRectF(l, b, r, t, 9.f, LLColor4(0.05f, 0.06f, 0.08f, 0.96f), LLColor4(0.02f, 0.025f, 0.035f, 0.96f));
    const S32 n1 = (S32)(sizeof(HELP_DRIVE) / sizeof(HELP_DRIVE[0]));
    const S32 n2 = (S32)(sizeof(HELP_SETUP) / sizeof(HELP_SETUP[0]));
    const S32 rows = llmax(n1, n2) + 2;
    const F32 lh = llmin((ch - 20.f) / rows, 26.f);
    const LLFontGL* head = face("B612", LLFontGL::BOLD, lh * 0.95f);
    const LLFontGL* body = face("B612", LLFontGL::NORMAL, lh * 0.82f);
    const LLFontGL* code = face("B612Mono", LLFontGL::NORMAL, lh * 0.8f);
    static const char* const TITLES[] = { "CAR DASHBOARD", "TRUCK DASHBOARD", "BIKE DASHBOARD" };
    text(head, TITLES[llclamp((S32)WolfDrive::instance().type(), 0, 2)], (l + r) * 0.5f, t - lh * 0.8f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
    auto column = [&](const HelpLine* lines, S32 n, F32 x)
    {
        F32 y = t - lh * 1.8f;
        for (S32 i = 0; i < n; ++i)
        {
            switch (lines[i].kind)
            {
            case 0: text(head, lines[i].s, x, y, C_TEAL, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 1: text(body, lines[i].s, x + lh * 0.5f, y, C_WHITE, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 2: text(body, lines[i].s, x + lh * 1.6f, y, C_GREY, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 4: text(code, lines[i].s, x + lh * 1.2f, y, C_GREEN, LLFontGL::LEFT, LLFontGL::TOP); break;
            default: break;
            }
            y -= (lines[i].kind == 3) ? lh * 0.5f : lh;
        }
    };
    column(HELP_DRIVE, n1, l + cw * 0.03f);
    column(HELP_SETUP, n2, l + cw * 0.52f);
    rectF(l + cw * 0.505f, b + lh, l + cw * 0.505f + 1.f, t - lh * 1.8f, C_DIM);
}

void WolfDashboard::draw()
{
    WolfDrive& d = WolfDrive::instance();
    mHits.clear();
    mDrawn = false;
    if (!d.dashboardOn() || !WolfDrive::onWolf())
    {
        mShowHelp = false;
        // a press that was going on when it closed (or Wolf was left) lets go: no stale hold
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        if (mPressed != H_NONE)
        {
            release();
        }
        return;
    }
    const WolfDrive::EType ty = d.type();
    // The guide opens by itself the first time each design is used (after the flight deck's).
    if ((S32)ty != mHelpCheckedType)
    {
        mHelpCheckedType = (S32)ty;
        static const char* const SEEN[] = { "WolfDashboardHelpCar", "WolfDashboardHelpTruck", "WolfDashboardHelpBike" };
        const char* setting = SEEN[llclamp((S32)ty, 0, 2)];
        if (!gSavedSettings.getBOOL(setting))
        {
            gSavedSettings.setBOOL(setting, true);
            mShowHelp = true;
        }
    }

    const F32 W = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    // Source: wolfflightdeck.cpp layoutAndDraw - sit on whatever the bottom toolbar leaves.
    F32 bottom = 0.f;
    if (gToolBarView)
    {
        if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
        {
            if (bp->isInVisibleChain())
            {
                const LLRect sr = bp->calcScreenRect();
                S32 lx = 0, ly = 0;
                screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                bottom = llclamp((F32)ly, 0.f, Hv * 0.4f);
            }
        }
    }
    // The panel is the mockup's 304 units tall. Its size is chosen in pixels (30% of the window,
    // 170 to 380) and turned into UI units with the UI scale, so a bigger UI makes it bigger; it
    // never takes more than 55% of what the toolbar leaves.
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    const F32 want_h = llclamp(Hv * ui * 0.30f, 170.f, 380.f);
    const F32 max_h = llmax(60.f, (Hv - bottom) * 0.55f);
    const F32 uH = llmin(want_h, max_h) / 304.f;
    const F32 one = llmin(uH, W / 1600.f);
    g.W = W;
    g.bandB = bottom;
    if (one >= llmin(uH, U_MIN))
    {
        // one row: the mockup as drawn
        g.compact = false;
        g.L.u = g.C.u = g.R.u = one;
        g.L.top0 = g.C.top0 = g.R.top0 = bottom + 900.f * one;
        g.L.ox = 0.f;
        g.C.ox = W * 0.5f - 800.f * one;
        g.R.ox = W - 1600.f * one;
    }
    else
    {
        // a narrow window: the wheel and controls on the bottom row, the cluster on its own above
        g.compact = true;
        const F32 row_h = llmin(want_h, (Hv - bottom) * 0.30f);
        const F32 u2 = llmin(row_h / 304.f, W / 920.f);
        const F32 u1 = llmin(row_h / 304.f, W / 860.f);
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
    mPanelB = bottom;
    mPanelT = g.panelTop;   // the strip's top is added once it is laid out
    mDrawn = true;


    LLGLSUIDefault gls_ui;
    const WolfDrive::Shown& s = d.shown();
    const bool bg = gSavedSettings.getBOOL("WolfDashboardBackground");
    Ctx c{ this, [this](F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip) { addHit(l, b, r, t, id, tip); }, mPressed };
    const F32 kmh = s.mSpeed * (s.mMph ? MPH : KMH);

    use(g.C);
    if (ty == WolfDrive::TYPE_CAR)
    {
        // CAR - after the Porsche 911 (992) cluster (Motor1: "a classic-looking analog tachometer
        // sits in the center of a digital gauge cluster ... flanked by a pair of LCD screens"),
        // red needles and 330 km/h after the BMW M3 (F80).
        if (bg)
        {
            rectG(0.f, g.bandB, W, g.panelTop, LLColor4(0.11f, 0.11f, 0.122f, 1.f), LLColor4(0.024f, 0.024f, 0.028f, 1.f));
            for (F32 x = S(6.f); x < W; x += S(13.f))
            {
                rectF(x, g.panelTop - S(10.f) - 0.8f, x + S(7.f), g.panelTop - S(10.f) + 0.8f, LLColor4(0.66f, 0.22f, 0.17f, 0.8f));
            }
            roundRectF(X(455.f), YM(900.f), X(1145.f), YM(616.f), S(60.f), LLColor4(0.07f, 0.075f, 0.086f, 1.f), LLColor4(0.012f, 0.012f, 0.016f, 1.f));
        }
        const F32 cy = YM(778.f);
        drawDial({ X(588.f), cy, S(82.f), s.mMaxSpeed, s.mMaxSpeed > 250.f ? 30.f : s.mMaxSpeed > 120.f ? 20.f : 10.f,
                   s.mMaxSpeed > 250.f ? 60.f : 0.f, s.mMaxSpeed > 120.f ? 10.f : 5.f, 1.f, 0.f, 0.f, 0.f, kmh, 0.f,
                   s.mMph ? "mph" : "km/h", false, C_NEEDLE, S(13.f) });
        T(X(588.f), YM(822.f), llformat("%d", (S32)llround(kmh)), 21.f, C_WHITE, true, true);
        drawDial({ X(800.f), cy, S(104.f), s.mMaxRpm, 1000.f, 0.f, 250.f, 1000.f, s.mRedline, 0.f, 0.f, s.mRpm, 0.f,
                   "x1000/min", false, C_NEEDLE, S(21.f) });
        const bool shift_now = s.mRpm >= s.mRedline * 0.97f;
        if (s.mManual && s.mGearNumber > 0)
        {
            T(X(800.f), YM(828.f), llformat("%d", s.mGearNumber), 46.f, shift_now && blink(0.25f) ? C_RED : C_WHITE, true);
            T(X(830.f), YM(838.f), "M", 18.f, C_AMBER, true);
        }
        else
        {
            T(X(800.f), YM(828.f), s.mGear, 46.f, shift_now && blink(0.25f) ? C_RED : C_WHITE, true);
            if (s.mGear == "D" && s.mGearNumber > 0)
            {
                T(X(830.f), YM(838.f), llformat("%d", s.mGearNumber), 18.f, C_GREY, true);
            }
        }
        // the information screen
        chromeDisc(X(1012.f), cy, S(89.f));
        circleF(X(1012.f), cy, S(84.f), LLColor4(0.02f, 0.02f, 0.024f, 1.f), 64);
        circleShaded(X(1012.f), cy, S(82.f), FACE_HI, FACE_LO, 64);
        drawMini(X(974.f), YM(780.f), S(22.f), s.mFuel / 100.f, "E", "F", "FUEL", 0.12f, 0.f);
        drawMini(X(1050.f), YM(780.f), S(22.f), (s.mTemp - 40.f) / 100.f, "40", "140", "TEMP", 0.f, 0.8f);
        T(X(1012.f), YM(812.f), llformat("%07.0f km", s.mOdoKm), 13.f, C_WHITE, false, true);
        T(X(1012.f), YM(830.f), llformat("TRIP %6.1f", s.mTripKm), 11.f, C_GREY, false, true);
        const time_t now_t = time(nullptr);
        struct tm lt;
#if LL_WINDOWS
        localtime_s(&lt, &now_t);
#else
        localtime_r(&now_t, &lt);
#endif
        T(X(1012.f), YM(728.f), llformat("%02d:%02d", lt.tm_hour, lt.tm_min), 11.f, C_GREY, false, true);
        telltales(X(485.f), X(1115.f), YM(642.f), s, !bg, false);
        use(g.L);
        mWheelR = S(124.f);
        wheel(X(220.f), YM(761.f), S(124.f), d.steer(), false);
        addHit(X(96.f), YM(885.f), X(344.f), YM(637.f), H_WHEEL, "Steering wheel: drag left or right (or Left / Right, A / D, or your own wheel)");
        use(g.R);
        selector(c, X(1175.f), X(1267.f), YM(632.f), YM(832.f), ty, "gears");
        pedals(c, X(1310.f), X(1580.f), YM(666.f), 172.f, d.brake(), d.throttle(), false);
        lampButtons(c, X(1310.f), X(1580.f), YM(858.f), s, d.buttonsHeld());
    }
    else if (ty == WolfDrive::TYPE_TRUCK)
    {
        use(g.C);
        // TRUCK - after the Scania R cluster (the sources are in wolfdrive.cpp): green economy
        // band and red line, the 90 km/h limiter, GRS905 range and split, dual-needle air, fuel
        // and AdBlue, the retarder, the tachograph's driving time. A big ROUND wheel.
        const WolfDrive::Spec& sp = WolfDrive::spec(WolfDrive::TYPE_TRUCK);
        if (bg)
        {
            rectG(0.f, g.bandB, W, g.panelTop, LLColor4(0.29f, 0.314f, 0.345f, 1.f), LLColor4(0.106f, 0.118f, 0.133f, 1.f));
            rectF(0.f, g.panelTop - S(8.f), W, g.panelTop, LLColor4(0.043f, 0.047f, 0.055f, 1.f));
            roundRectF(X(400.f), YM(904.f), X(1230.f), YM(610.f), S(26.f), LLColor4(0.035f, 0.04f, 0.047f, 1.f), LLColor4(0.035f, 0.04f, 0.047f, 1.f));
        }
        const F32 cy = YM(780.f);
        drawDial({ X(520.f), cy, S(98.f), s.mMaxRpm, 500.f, 0.f, 100.f, 100.f, s.mRedline, sp.mGreenLo, sp.mGreenHi, s.mRpm, 0.f,
                   "x100/min", true, TRUCK_NEEDLE, S(17.f) });
        T(X(520.f), YM(828.f), llformat("%d", (S32)llround(s.mRpm)), 16.f, C_WHITE, true, true);
        const F32 gov = sp.mGovernorKmh > 0.f ? (s.mMph ? sp.mGovernorKmh / 1.609344f : sp.mGovernorKmh) : 0.f;
        drawDial({ X(990.f), cy, S(98.f), s.mMaxSpeed, s.mMaxSpeed > 100.f ? 20.f : 10.f, 0.f, 5.f, 1.f, 0.f, 0.f, 0.f, kmh, gov,
                   s.mMph ? "mph" : "km/h", true, TRUCK_NEEDLE, S(17.f) });
        T(X(990.f), YM(828.f), llformat("%d", (S32)llround(kmh)), 16.f, C_WHITE, true, true);
        // the centre screen
        const F32 dl = X(642.f), dr = X(868.f), dt = YM(664.f), db = YM(882.f);
        roundRectF(dl, db, dr, dt, S(10.f), LLColor4(0.02f, 0.027f, 0.043f, 1.f), LLColor4(0.043f, 0.063f, 0.094f, 1.f));
        frameW(dl, db, dr, dt, 1.f, LLColor4(0.227f, 0.247f, 0.278f, 1.f));
        const bool automatic = s.mAutoBox && !s.mManual;
        T(X(755.f), YM(682.f), automatic ? "A   AUTOMATIC" : "M   MANUAL", 12.f, C_TEAL, true);
        T(X(728.f), YM(734.f), s.mGear, 60.f, C_WHITE, true);
        if (s.mGearNumber >= 1 && s.mGearNumber <= 12)
        {
            // GRS905: gears 1-6 the low range, 7-12 the high; odd the low split, even the high.
            T(X(810.f), YM(720.f), s.mGearNumber <= 6 ? "LOW" : "HIGH", 12.f, C_GREY);
            T(X(810.f), YM(744.f), (s.mGearNumber % 2) ? "L" : "H", 24.f, C_TEAL, true);
            T(X(810.f), YM(764.f), "SPLIT", 9.f, C_GREY);
        }
        rectF(dl + S(12.f), YM(780.f), dr - S(12.f), YM(780.f) + 1.f, LLColor4(0.15f, 0.19f, 0.23f, 1.f));
        T(dl + S(14.f), YM(798.f), "RETARDER", 10.f, C_GREY, false, false, LLFontGL::LEFT);
        for (S32 i = 0; i < 5; ++i)
        {
            const F32 x = X(740.f + i * 24.f);
            roundRectF(x, YM(804.f), x + S(18.f), YM(792.f), S(2.f), i < s.mRetarder ? C_BLUE : C_OFF, i < s.mRetarder ? C_BLUE : C_OFF);
        }
        const F32 allowed = 4.5f * 3600.f;   // EU drivers' hours: 4.5 h, then a 45-minute break
        const F32 left = allowed - s.mDriveTime;
        auto clock = [](F32 secs) { const S32 m = (S32)(llmax(0.f, secs) / 60.f); return llformat("%d:%02d", m / 60, m % 60); };
        T(dl + S(14.f), YM(822.f), "DRIVE", 10.f, C_GREY, false, false, LLFontGL::LEFT);
        T(dr - S(14.f), YM(822.f), clock(s.mDriveTime), 14.f, C_WHITE, false, true, LLFontGL::RIGHT);
        T(dl + S(14.f), YM(842.f), "BREAK IN", 10.f, C_GREY, false, false, LLFontGL::LEFT);
        T(dr - S(14.f), YM(842.f), left > 0.f ? clock(left) : std::string("DUE"), 14.f,
          left > 15.f * 60.f ? C_GREEN : (blink() ? C_AMBER : C_OFF), false, true, LLFontGL::RIGHT);
        T(X(755.f), YM(866.f), llformat("%07.0f km   TRIP %.1f", s.mOdoKm, s.mTripKm), 11.f, C_GREY, false, true);
        // air pressure: circuit 1 green, circuit 2 red
        const F32 ax = X(1150.f), ay = YM(698.f), ar = S(46.f);
        chromeDisc(ax, ay, ar * 1.09f);
        circleShaded(ax, ay, ar, TFACE_HI, TFACE_LO, 48);
        for (S32 v = 0; v <= 12; v += 2)
        {
            const F32 aa = -135.f + 270.f * v / 12.f;
            F32 x0, y0, x1, y1;
            pt(ax, ay, ar * 0.8f, aa, x0, y0);
            pt(ax, ay, ar * 0.95f, aa, x1, y1);
            lineW(x0, y0, x1, y1, 2.f, C_WHITE);
            if (v % 4 == 0)
            {
                pt(ax, ay, ar * 0.6f, aa, x0, y0);
                T(x0, y0, llformat("%d", v), 10.f, C_WHITE, true);
            }
        }
        const F32 airs[2] = { s.mAir1, s.mAir2 };
        const LLColor4 airc[2] = { C_GREEN, C_RED };
        for (S32 i = 0; i < 2; ++i)
        {
            F32 x1, y1;
            pt(ax, ay, ar * (i ? 0.8f : 0.9f), -135.f + 270.f * llclamp(airs[i] / 12.f, 0.f, 1.f), x1, y1);
            lineW(ax, ay, x1, y1, 3.f, airc[i]);
        }
        chromeDisc(ax, ay, S(5.f));
        T(ax, ay - S(30.f), "AIR  bar", 9.f, C_GREY);
        auto bar = [&](F32 x, F32 frac, const LLColor4& col, const char* name)
        {
            const F32 l = X(x), r = X(x + 22.f), t = YM(778.f), b = YM(862.f);
            rectF(l, b, r, t, C_OFF);
            rectF(l, b, r, b + (t - b) * llclamp(frac, 0.f, 1.f), frac < 0.1f ? C_RED : col);
            for (S32 k = 1; k < 4; ++k) rectF(l, b + (t - b) * k / 4.f, l + S(8.f), b + (t - b) * k / 4.f + 1.5f, C_BLACK);
            frameW(l - 1.f, b - 1.f, r + 1.f, t + 1.f, 1.f, LLColor4(0.5f, 0.52f, 0.55f, 1.f));
            T((l + r) * 0.5f, YM(875.f), name, 10.f, C_GREY);
        };
        bar(1112.f, s.mFuel / 100.f, C_WHITE, "FUEL");
        bar(1166.f, s.mAdBlue / 100.f, C_BLUE, "AdBlue");
        telltales(X(425.f), X(1205.f), YM(636.f), s, !bg, false);
        use(g.L);
        mWheelR = S(140.f);
        wheel(X(200.f), YM(748.f), S(140.f), d.steer(), true);
        addHit(X(60.f), YM(888.f), X(340.f), YM(608.f), H_WHEEL, "Steering wheel: drag left or right (or Left / Right, A / D, or your own wheel)");
        use(g.R);
        selector(c, X(1255.f), X(1347.f), YM(632.f), YM(812.f), ty, "gears");
        pedals(c, X(1378.f), X(1580.f), YM(666.f), 172.f, d.brake(), d.throttle(), false);
        lampButtons(c, X(1255.f), X(1580.f), YM(858.f), s, d.buttonsHeld());
    }
    else
    {
        use(g.C);
        // BIKE - after the BMW S 1000 RR / Ducati Panigale V4 TFT: the 8:3 screen, the bar rev
        // counter along the top ("the tachometer moves on a horizontal scale positioned in the
        // highest part of the instrument ... the gear engaged is in the center"), shift lights,
        // speed, lean angle. Handlebars, not a wheel.
        if (bg)
        {
            rectF(0.f, g.bandB, W, g.panelTop, LLColor4(0.043f, 0.043f, 0.051f, 1.f));
            for (F32 x = 0.f; x < W; x += S(8.f))
            {
                rectF(x, g.bandB, x + S(4.f), g.panelTop, LLColor4(1.f, 1.f, 1.f, 0.02f));
            }
            // the fairing's inside, round the screen
            noTex();
            gGL.begin(LLRender::TRIANGLES);
            const LLColor4 fc(0.024f, 0.024f, 0.028f, 0.9f);
            const S32 seg = 32;
            for (S32 i = 0; i < seg; ++i)
            {
                const F32 t0 = (F32)i / seg, t1 = (F32)(i + 1) / seg;
                auto q = [&](F32 t, F32& x, F32& y)
                {
                    // the two quadratic curves of the mockup's fairing path, joined at the top
                    if (t < 0.5f) { const F32 u = t * 2.f; x = (1 - u) * (1 - u) * 400.f + 2 * u * (1 - u) * 440.f + u * u * 800.f;
                                    y = (1 - u) * (1 - u) * 900.f + 2 * u * (1 - u) * 620.f + u * u * 608.f; }
                    else { const F32 u = (t - 0.5f) * 2.f; x = (1 - u) * (1 - u) * 800.f + 2 * u * (1 - u) * 1160.f + u * u * 1200.f;
                           y = (1 - u) * (1 - u) * 608.f + 2 * u * (1 - u) * 620.f + u * u * 900.f; }
                };
                F32 x0, y0, x1, y1;
                q(t0, x0, y0); q(t1, x1, y1);
                vtx(X(800.f), YM(900.f), fc); vtx(X(x0), YM(y0), fc); vtx(X(x1), YM(y1), fc);
            }
            gGL.end();
        }
        const F32 sl = X(530.f), sr = X(1070.f), st = YM(636.f), sb = YM(838.f);
        roundRectF(sl - S(14.f), sb - S(14.f), sr + S(14.f), st + S(16.f), S(18.f), LLColor4(0.08f, 0.082f, 0.094f, 1.f), LLColor4(0.08f, 0.082f, 0.094f, 1.f));
        frameW(sl - S(14.f), sb - S(14.f), sr + S(14.f), st + S(16.f), 1.f, LLColor4(0.165f, 0.173f, 0.192f, 1.f));
        rectG(sl, sb, sr, st, LLColor4(0.02f, 0.027f, 0.043f, bg ? 1.f : 0.88f), LLColor4(0.043f, 0.063f, 0.094f, bg ? 1.f : 0.88f));
        // shift lights: on through the last tenth below the red line, all flashing at it (design)
        const F32 frac_shift = (s.mRpm - s.mRedline * 0.90f) / llmax(1.f, s.mRedline * 0.10f);
        for (S32 i = 0; i < 10; ++i)
        {
            LLColor4 col = C_OFF;
            if (frac_shift >= (i + 1) / 10.f) col = i < 4 ? C_GREEN : i < 8 ? C_AMBER : C_RED;
            if (s.mRpm >= s.mRedline * 0.985f) col = blink(0.12f) ? C_BLUE : C_OFF;
            circleF(X(602.f + i * 44.f), YM(628.f), S(5.f), col, 14);
            if (col != C_OFF) circleF(X(602.f + i * 44.f), YM(628.f), S(9.f), alpha(col, 0.2f), 14);
        }
        // the bar rev counter: segments rising toward the limiter
        const F32 bl = X(552.f), br = X(1048.f), bt = YM(648.f), bb = YM(688.f);
        const F32 maxr = llmax(1000.f, s.mMaxRpm);
        auto xr = [&](F32 v) { return bl + (br - bl) * llclamp(v / maxr, 0.f, 1.f); };
        const S32 segs = 60;
        for (S32 i = 0; i < segs; ++i)
        {
            const F32 v0 = maxr * i / segs, v1 = maxr * (i + 1) / segs;
            const bool lit = v0 < s.mRpm;
            const LLColor4 col = v0 >= s.mRedline ? C_RED : v0 >= s.mRedline - 1500.f ? C_ORANGE : C_WHITE;
            const F32 hgt = (bt - bb) * (0.35f + 0.65f * i / (segs - 1));
            rectF(xr(v0) + 1.f, bb, xr(v1) - 1.f, bb + hgt, lit ? col : LLColor4(0.11f, 0.125f, 0.153f, 1.f));
        }
        for (F32 v = 0.f; v <= maxr + 1.f; v += 1000.f)
        {
            rectF(xr(v), bb - S(7.f), xr(v) + 1.f, bb - S(2.f), C_GREY);
            const S32 kk = (S32)llround(v / 1000.f);
            if (kk % 2 == 0 || v + 1000.f > maxr)
            {
                T(xr(v), bb - S(15.f), llformat("%d", kk), 11.f, v >= s.mRedline ? C_RED : C_GREY, true);
            }
        }
        T(br, bb - S(30.f), "x1000/min", 9.f, C_GREY, false, false, LLFontGL::RIGHT);
        // speed, gear, lean
        const F32 my = YM(758.f);
        T(X(642.f), my, llformat("%d", (S32)llround(kmh)), 60.f, C_WHITE, true, true);
        T(X(642.f), YM(796.f), s.mMph ? "mph" : "km/h", 12.f, C_GREY);
        roundRectF(X(752.f), YM(804.f), X(848.f), YM(712.f), S(10.f), LLColor4(0.051f, 0.082f, 0.133f, 1.f), LLColor4(0.051f, 0.082f, 0.133f, 1.f));
        frameW(X(752.f), YM(804.f), X(848.f), YM(712.f), 1.f, LLColor4(0.15f, 0.25f, 0.37f, 1.f));
        T(X(800.f), YM(756.f), s.mGear, 72.f, s.mGear == "N" ? C_GREEN : C_WHITE, true);
        T(X(800.f), YM(792.f), "GEAR", 10.f, C_GREY);
        const F32 lx = X(958.f), ly = YM(780.f), lr = S(58.f);
        clockArc(lx, ly, lr, S(9.f), -90.f, 90.f, LLColor4(0.165f, 0.19f, 0.22f, 1.f));
        for (S32 dg = -60; dg <= 60; dg += 15)
        {
            F32 x0, y0, x1, y1;
            pt(lx, ly, lr - S(11.f), (F32)dg, x0, y0);
            pt(lx, ly, lr - S(4.f), (F32)dg, x1, y1);
            lineW(x0, y0, x1, y1, 1.5f, C_GREY);
        }
        const F32 lean = llclamp(s.mLean, -60.f, 60.f);
        if (fabsf(lean) > 0.5f)
        {
            clockArc(lx, ly, lr, S(9.f), llmin(0.f, lean), llmax(0.f, lean), C_TEAL);
        }
        F32 mx, my2;
        pt(lx, ly, lr, -llmin(60.f, s.mMaxLeanL), mx, my2);
        circleF(mx, my2, S(5.f), C_AMBER, 12);
        pt(lx, ly, lr, llmin(60.f, s.mMaxLeanR), mx, my2);
        circleF(mx, my2, S(5.f), C_AMBER, 12);
        T(lx, ly + S(16.f), llformat("%d", (S32)llround(fabsf(s.mLean))), 26.f, C_WHITE, true, true);
        T(lx, ly - S(10.f), llformat("LEAN  L%d  R%d", (S32)llround(s.mMaxLeanL), (S32)llround(s.mMaxLeanR)), 10.f, C_GREY);
        // the bottom row
        const F32 ry = YM(820.f);
        rectF(sl + S(16.f), ry + S(16.f), sr - S(16.f), ry + S(17.f), LLColor4(0.106f, 0.141f, 0.188f, 1.f));
        roundRectF(X(552.f), ry - S(6.f), X(662.f), ry + S(6.f), S(2.f), C_OFF, C_OFF);
        roundRectF(X(552.f), ry - S(6.f), X(552.f + 110.f * llclamp(s.mFuel / 100.f, 0.f, 1.f)), ry + S(6.f), S(2.f),
                   s.mFuel < 10.f ? C_AMBER : C_WHITE, s.mFuel < 10.f ? C_AMBER : C_WHITE);
        T(X(672.f), ry, "FUEL", 10.f, C_GREY, false, false, LLFontGL::LEFT);
        T(X(780.f), ry, llformat("%d C", (S32)llround(s.mTemp)), 13.f, s.mTemp > 110.f ? C_RED : C_GREY, false, true);
        T(X(1048.f), ry, llformat("ODO %06.0f   TRIP %.1f", s.mOdoKm, s.mTripKm), 12.f, C_GREY, false, true, LLFontGL::RIGHT);
        // indicators and beams beside the screen; the warnings under it
        const bool flash = blink(0.7f);
        arrowLamp(X(484.f), YM(666.f), S(15.f), true, (s.mIndicators == 1 || s.mIndicators == 3) && flash);
        arrowLamp(X(1116.f), YM(666.f), S(15.f), false, (s.mIndicators == 2 || s.mIndicators == 3) && flash);
        beamLamp(X(480.f), YM(718.f), S(32.f), false, s.mLights == 1);
        beamLamp(X(1112.f), YM(718.f), S(32.f), true, s.mLights == 2);
        telltales(X(650.f), X(950.f), YM(872.f), s, !bg, true);
        use(g.L);
        mWheelR = S(232.f);
        handlebars(X(255.f), YM(761.f), 320.f, S(1.45f), d.steer(), d.brake(), d.clutch(), d.throttle());
        addHit(X(20.f), YM(840.f), X(490.f), YM(660.f), H_WHEEL, "Handlebars: drag left or right to steer (or Left / Right, A / D, or your controller)");
        use(g.R);
        selector(c, X(1240.f), X(1332.f), YM(626.f), YM(826.f), ty, "shift");   // ends above the lamp row
        pedals(c, X(1372.f), X(1580.f), YM(666.f), 172.f, d.brake(), d.throttle(), true);
        lampButtons(c, X(1372.f), X(1580.f), YM(858.f), s, d.buttonsHeld());
    }
    mPanelT = strip(c, s, ty, d.vehicleName());
    use(g.C);

    // (Paul 2026-10-09: no "sit on a vehicle" banner - the instruments simply rest until you do.)
    if (mShowHelp)
    {
        drawHelp(mPanelT);
    }
}

//-----------------------------------------------------------------------------
// input
//-----------------------------------------------------------------------------

void WolfDashboard::press(EHit id)
{
    WolfDrive& d = WolfDrive::instance();
    switch (id)
    {
    case H_THROTTLE:   d.setScreenPedal(false, true); break;
    case H_BRAKE:      d.setScreenPedal(true, true); break;
    case H_SHIFT_UP:   d.pressAction(WolfDrive::ACT_GEAR_UP); break;
    case H_BOX_MODE:   d.setAutoBox(!d.autoBox()); break;
    case H_SHIFT_DOWN: d.pressAction(WolfDrive::ACT_GEAR_DOWN); break;
    case H_TYPE_CAR:   d.chooseType(WolfDrive::TYPE_CAR); break;
    case H_TYPE_TRUCK: d.chooseType(WolfDrive::TYPE_TRUCK); break;
    case H_TYPE_BIKE:  d.chooseType(WolfDrive::TYPE_BIKE); break;
    case H_UNITS:      gSavedSettings.setBOOL("WolfDashboardMph", !gSavedSettings.getBOOL("WolfDashboardMph")); break;
    case H_PANEL:      gSavedSettings.setBOOL("WolfDashboardBackground", !gSavedSettings.getBOOL("WolfDashboardBackground")); break;
    case H_CONTROLS:   LLFloaterReg::toggleInstanceOrBringToFront("wolf_drive_controls"); break;
    case H_TRIP:       d.resetTrip(); break;
    case H_CLOSE:      d.requestDashboard(false); break;
    case H_HELP:       mShowHelp = !mShowHelp; break;
    case H_MAP:
    {
        // Source: wolfflightdeck.cpp press() H_MAP - the World Map, opened and fitted to an area
        // (LLFloaterWorldMap::wolfShowArea). Driving has no route: the area is round the vehicle.
        LLFloaterReg::showInstance("world_map");
        LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance();
        if (map && gAgent.getRegion())
        {
            map->wolfShowArea(gAgent.getPositionGlobal(), 2048.0);
        }
        break;
    }
    case H_LIGHTS:     d.setScreenButton(WolfDrive::BTN_LIGHTS, true); break;
    case H_IND_L:      d.setScreenButton(WolfDrive::BTN_IND_LEFT, true); break;
    case H_IND_R:      d.setScreenButton(WolfDrive::BTN_IND_RIGHT, true); break;
    case H_HANDBRAKE:  d.setScreenButton(WolfDrive::BTN_HANDBRAKE, true); break;
    case H_HORN:       d.setScreenButton(WolfDrive::BTN_HORN, true); break;
    case H_HAZARDS:    d.setScreenButton(WolfDrive::BTN_HAZARDS, true); break;
    case H_CRUISE:     d.setScreenButton(WolfDrive::BTN_CRUISE, true); break;
    default:
        if (id >= H_SEL_0 && id <= H_SEL_7)
        {
            const std::vector<S32> pos = WolfDrive::selectorPositions(d.type(), d.autoBox());
            const S32 i = id - H_SEL_0;
            if (i < (S32)pos.size()) d.setSelector(pos[i]);
        }
        break;
    }
}

void WolfDashboard::release()
{
    WolfDrive& d = WolfDrive::instance();
    d.setScreenPedal(false, false);
    d.setScreenPedal(true, false);
    d.setScreenSteer(0.f, false);
    d.setScreenButton(WolfDrive::BTN_LIGHTS | WolfDrive::BTN_IND_LEFT | WolfDrive::BTN_IND_RIGHT
                      | WolfDrive::BTN_HANDBRAKE | WolfDrive::BTN_HORN | WolfDrive::BTN_HAZARDS | WolfDrive::BTN_CRUISE, false);
    mPressed = H_NONE;
}

bool WolfDashboard::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mShowHelp && y > mPanelT)
    {
        mShowHelp = false;   // the guide is over the world: a click puts it away
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return true;   // the panel itself (background on): not a click on the world behind it
    }
    mPressed = hit->mId;
    gFocusMgr.setMouseCapture(this);
    if (hit->mId == H_WHEEL)
    {
        mPressX = x;
        WolfDrive::instance().setScreenSteer(0.f, true);
        return true;
    }
    press(hit->mId);
    return true;
}

bool WolfDashboard::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE;
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    release();
    return was || onPanel(x, y);
}

bool WolfDashboard::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mPressed == H_WHEEL)
    {
        // Source: the old Vehicle tab's wheel (wolfvehiclecontrols.cpp handleHover): horizontal
        // travel from the press, full lock at one radius.
        WolfDrive::instance().setScreenSteer(llclamp((F32)(x - mPressX) / llmax(24.f, mWheelR), -1.f, 1.f), true);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    if (!hasMouseCapture() && !onPanel(x, y))
    {
        return false;
    }
    getWindow()->setCursor(hitAt(x, y) ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfDashboard::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return onPanel(x, y);   // no world context menu through the panel
}

bool WolfDashboard::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!onPanel(x, y))
    {
        return false;
    }
    return handleMouseDown(x, y, mask);
}

bool WolfDashboard::handleToolTip(S32 x, S32 y, MASK mask)
{
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

void WolfDashboard::onMouseCaptureLost()
{
    release();
    LLUICtrl::onMouseCaptureLost();
}
