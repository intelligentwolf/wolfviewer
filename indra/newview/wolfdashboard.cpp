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

#include "wolfhudscale.h"
#include "wolfvehiclerez.h"   // [HUD REZ 2026-10-12/]
#include "wolfdashboard.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <optional>

#include "llagent.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "llworld.h"
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

    // <WolfViewer 2026-10-11> w65: a speed in m/s in the shown units (units=kn and the spaceship's m/s besides km/h and
    // mph), and their name. 1 kn = 1852 m / 3600 s.
    const F32 KN = 3600.f / 1852.f;
    F32 speedIn(const WolfDrive::Shown& s, F32 ms)
    {
        switch (s.mUnits)
        {
        case WolfDrive::Shown::UNITS_MPH: return ms * MPH;
        case WolfDrive::Shown::UNITS_KN:  return ms * KN;
        case WolfDrive::Shown::UNITS_MS:  return ms;
        default:                          return ms * KMH;
        }
    }
    const char* unitName(const WolfDrive::Shown& s)
    {
        switch (s.mUnits)
        {
        case WolfDrive::Shown::UNITS_MPH: return "mph";
        case WolfDrive::Shown::UNITS_KN:  return "kn";
        case WolfDrive::Shown::UNITS_MS:  return "m/s";
        default:                          return "km/h";
        }
    }
    std::string unitUpper(const WolfDrive::Shown& s)
    {
        std::string u = unitName(s);
        LLStringUtil::toUpper(u);
        return u;
    }
    /** The speedboat's speed step as the helm writes it: "3/6" when it is one of the boat's steps, else as sent. */
    std::string stepText(const WolfDrive::Shown& s)
    {
        if (s.mGear.empty()) return "-";
        const bool digits = s.mGear.find_first_not_of("0123456789") == std::string::npos;
        if (digits && s.mStepMax > 0 && atoi(s.mGear.c_str()) <= s.mStepMax)
        {
            return s.mGear + llformat("/%d", s.mStepMax);
        }
        return s.mGear;
    }

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
        WolfHudScale::text(f, s, x, y, c, h, v);   // [HUD SIZE 2026-10-12/] sharp at the size the driver set
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
        // <WolfViewer 2026-10-11> w65 (PROTOCOL.md "New warnings words"): red where the craft is at risk now
        else if (w == "shield")   { label = "SHIELD"; }
        else if (w == "hull")     { label = "HULL"; col = C_RED; }
        else if (w == "heat")     { label = "HEAT"; col = C_RED; }
        else if (w == "stage")    { label = "STAGE"; }
        else if (w == "boost")    { label = "BOOST"; }
        else if (w == "anchor")   { label = "ANCHOR"; }
        else if (w == "depth")
        {
            // measured by the viewer: too shallow under a speedboat, past the safe depth in a submarine
            label = WolfDrive::instance().type() == WolfDrive::TYPE_SUBMARINE ? "TOO DEEP" : "SHALLOW";
            col = C_RED;
        }
        else if (w == "oxygen")   { label = "OXYGEN"; col = C_RED; }
        else if (w == "battery")  { label = "BATTERY"; }
        else if (w == "leak")     { label = "LEAK"; col = C_RED; }
        // [WOLF RETARDER 2026-10-12] held back by heat (Scania "Limiting at high temperatures"): a caution, amber
        else if (w == "retarder") { label = "RETARDER"; }
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
    // <WolfViewer 2026-10-11> the speedboat helm's navy (Mercury VesselView / Yamaha CL7 / Garmin gauge pages)
    const LLColor4 NAVY_HI(0.055f, 0.105f, 0.200f, 1.f), NAVY_LO(0.012f, 0.035f, 0.085f, 1.f);
    const LLColor4 NAVY_FACE_HI(0.07f, 0.13f, 0.24f, 1.f), NAVY_FACE_LO(0.01f, 0.03f, 0.08f, 1.f);
    const LLColor4 NAVY_SCREEN(0.008f, 0.024f, 0.060f, 1.f);
    const LLColor4 NAVY_LINE(0.16f, 0.28f, 0.42f, 1.f);
    const LLColor4 SEA_FACE_HI(0.05f, 0.16f, 0.13f, 1.f), SEA_FACE_LO(0.005f, 0.04f, 0.035f, 1.f);   // the submarine's

    void chromeDisc(F32 cx, F32 cy, F32 r) { circleShaded(cx, cy, r, CHROME_HI, CHROME_LO, 64); }

    struct Dial
    {
        F32 cx, cy, r, max, major, label_every, minor, div, red, green_lo, green_hi, value, mark;
        std::string unit;
        bool truck;
        LLColor4 needle;
        F32 num_px;
        bool navy = false;   // <WolfViewer 2026-10-11/> the speedboat's navy face
        bool sea = false;    // <WolfViewer 2026-10-11/> the submarine's dark green face
    };

    // A round analogue gauge after the mockup's dial(): chrome ring, dark face, green / red arcs,
    // ticks and figures from -135 to +135 degrees, a glowing needle with a tail, a chrome hub
    // and a touch of glass.
    void drawDial(const Dial& o)
    {
        auto ang = [&](F32 v) { return -135.f + 270.f * llclamp(v / llmax(1.f, o.max), 0.f, 1.04f); };
        chromeDisc(o.cx, o.cy, o.r * 1.09f);
        circleF(o.cx, o.cy, o.r * 1.03f, LLColor4(0.02f, 0.02f, 0.024f, 1.f), 64);
        circleShaded(o.cx, o.cy, o.r, o.sea ? SEA_FACE_HI : o.navy ? NAVY_FACE_HI : o.truck ? TFACE_HI : FACE_HI,
                     o.sea ? SEA_FACE_LO : o.navy ? NAVY_FACE_LO : o.truck ? TFACE_LO : FACE_LO, 64);
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
        // <WolfViewer 2026-10-10> Paul (330 km/h car dial): "make numbers smaller" - at the top of the dial neighbouring
        // figures sit side by side and "140 160 180" ran into each other. The figures are as big as the mockup asks
        // only while the widest one fits the chord to its neighbour on the figure circle (0.62 r); smaller otherwise.
        const LLFontGL* nf = face("B612", LLFontGL::BOLD, o.num_px * 1.25f);
        {
            const F32 step = o.label_every > 0.f ? o.label_every : o.major;
            const F32 gap_deg = 270.f * step / llmax(1.f, o.max);
            const F32 chord = 2.f * o.r * 0.62f * sinf(gap_deg * 0.5f * DEG_TO_RAD);
            const std::string widest = llformat("%d", (S32)llround(o.max / o.div));
            F32 px = o.num_px * 1.25f;
            while (nf && px > 6.f && nf->getWidthF32(widest) > chord * 0.85f)
            {
                px -= 1.f;
                nf = face("B612", LLFontGL::BOLD, px);
            }
        }
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
    // [HUD SIZE 2026-10-12] the view's point in the picture the hits were made in
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

// <WolfViewer 2026-10-10> The mini dashboard: one bar at the bottom - speed, gear, fuel, the indicator and lamp
// tell-tales, cruise and the first warning - with FULL and X. Everything beside it is the world's.
void WolfDashboard::drawMiniBar(F32 bottom)
{
    const WolfDrive::Shown& s = WolfDrive::instance().shown();
    const F32 W = (F32)getRect().getWidth();
    const F32 h = llclamp((F32)getRect().getHeight() * 0.055f, 36.f, 56.f);
    struct Cell { std::string label, value; LLColor4 col; EHit hit; std::string tip; F32 w; bool key; };
    std::vector<Cell> cells;
    const bool blink_on = fmod(LLFrameTimer::getElapsedSeconds(), 1.0) < 0.5;
    const WolfDrive::EType ty = WolfDrive::instance().type();
    if (ty == WolfDrive::TYPE_SPEEDBOAT)
    {
        // <WolfViewer 2026-10-11> w65: the speedboat helm's mini bar - speed, step, revs, depth, fuel, anchor
        const bool shallow = s.mDepth < 2.f;
        cells.push_back({ unitUpper(s), llformat("%d", (S32)llround(speedIn(s, s.mSpeed))), C_WHITE, H_UNITS, "Click to change the speed units", 1.2f, false });
        cells.push_back({ "STEP", stepText(s), C_WHITE, H_NONE, "", 0.9f, false });
        cells.push_back({ "RPM", llformat("%d", (S32)llround(s.mRpm / 10.f) * 10), s.mRpm >= s.mRedline ? C_RED : C_WHITE, H_NONE, "", 1.2f, false });
        cells.push_back({ "DEPTH", s.mDepth <= 0.f ? std::string("AGROUND") : llformat("%.1f m", s.mDepth), shallow ? C_RED : C_WHITE, H_NONE, "", 1.4f, false });
        cells.push_back({ "FUEL", llformat("%d%%", (S32)llround(s.mFuel)), s.mFuel < 15.f ? C_AMBER : C_WHITE, H_NONE, "", 1.0f, false });
        cells.push_back({ "ANCHOR", s.mAnchor ? "DOWN" : "UP", s.mAnchor ? C_AMBER : C_DIM, H_ANCHOR, "Anchor: click to send it up or down", 1.0f, false });
    }
    else if (ty == WolfDrive::TYPE_SUBMARINE)
    {
        // <WolfViewer 2026-10-11> w65: the submarine's - speed, depth, sea bed, battery, oxygen, periscope
        cells.push_back({ unitUpper(s), llformat("%.1f", speedIn(s, s.mSpeed3D)), C_WHITE, H_UNITS, "Click to change the speed units", 1.1f, false });
        cells.push_back({ "DEPTH", s.mSubDepth <= 0.f ? std::string("SURF") : llformat("%.0f m", s.mSubDepth),
                          s.mHasMaxDepth && s.mSubDepth > s.mMaxDepth ? C_RED : C_WHITE, H_NONE, "", 1.2f, false });
        cells.push_back({ "SEA BED", llformat("%.0f m", s.mSeabed), s.mSeabed < 2.f ? C_RED : s.mSeabed < 5.f ? C_AMBER : C_WHITE, H_NONE, "", 1.2f, false });
        cells.push_back({ "BATTERY", s.mHasBattery ? llformat("%d%%", (S32)llround(s.mBattery)) : std::string("--"), C_WHITE, H_NONE, "", 1.1f, false });
        cells.push_back({ "OXYGEN", s.mHasOxygen ? llformat("%d%%", (S32)llround(s.mOxygen)) : std::string("--"), s.mHasOxygen && s.mOxygen < 25.f ? C_AMBER : C_WHITE, H_NONE, "", 1.1f, false });
        cells.push_back({ "SCOPE", s.mPeriscope ? "UP" : "DOWN", s.mPeriscope ? C_AMBER : C_DIM, H_PERISCOPE, "Periscope: click to raise or lower", 1.0f, false });
    }
    else if (ty == WolfDrive::TYPE_SPACESHIP)
    {
        // <WolfViewer 2026-10-11> w65: the spaceship's - speed, altitude, vertical speed, throttle, stage, assist, gear
        cells.push_back({ unitUpper(s), llformat("%d", (S32)llround(speedIn(s, s.mSpeed3D))), C_TEAL, H_NONE, "", 1.2f, false });
        cells.push_back({ "ALT M", llformat("%d", (S32)llround(s.mAltMSL)), C_TEAL, H_NONE, "", 1.3f, false });
        cells.push_back({ "VS M/S", llformat("%+.1f", s.mVS), s.mVS < 0.f ? C_AMBER : C_TEAL, H_NONE, "", 1.2f, false });
        cells.push_back({ "THRUST", llformat("%d%%", (S32)llround(s.mThrottlePct)), C_TEAL, H_NONE, "", 1.1f, false });
        if (s.mStages > 0)
        {
            cells.push_back({ "STAGE", llformat("%d/%d", s.mStage, s.mStages), C_TEAL, H_NONE, "", 0.9f, false });
            if (s.mRocket)
            {
                cells.push_back({ "", "SEP", C_AMBER, H_STAGE, "Separate the stage (sends the stage button to the craft's script)", 0.8f, true });
            }
        }
        cells.push_back({ "ASSIST", s.mAssist ? "ON" : "OFF", s.mAssist ? C_TEAL : C_AMBER, H_ASSIST, "Flight assist: click to switch", 1.0f, false });
        cells.push_back({ "GEAR", s.mLandingGear ? "DOWN" : "UP", s.mLandingGear ? C_GREEN : C_DIM, H_LANDING_GEAR, "Landing gear: click to raise or lower", 1.0f, false });
    }
    else
    {
        cells.push_back({ "", (s.mIndicators == 1 || s.mIndicators == 3) && blink_on ? "<" : "", C_GREEN, H_IND_L, "Left indicator", 0.6f, false });
        cells.push_back({ unitUpper(s), llformat("%d", (S32)llround(speedIn(s, s.mSpeed))), C_WHITE, H_UNITS,
                          s.mMph ? "Click for km/h" : "Click for miles per hour", 1.2f, false });
        cells.push_back({ "GEAR", s.mGear.empty() ? std::string("-") : s.mGear, C_WHITE, H_NONE, "", 0.9f, false });
        cells.push_back({ "FUEL", llformat("%d%%", (S32)llround(s.mFuel)), s.mFuel < 15.f ? C_AMBER : C_WHITE, H_NONE, "", 1.0f, false });
        cells.push_back({ "LIGHTS", s.mLights == 2 ? "MAIN" : (s.mLights == 1 ? "ON" : "OFF"), s.mLights == 2 ? C_BLUE : (s.mLights ? C_GREEN : C_DIM),
                          H_LIGHTS, "Lamps: click to change", 1.0f, false });
        cells.push_back({ "CRUISE", s.mCruise ? "ON" : "OFF", s.mCruise ? C_GREEN : C_DIM, H_CRUISE, "Cruise control: click to switch", 1.0f, false });
    }
    if (!s.mWarnings.empty())
    {
        std::string label;
        LLColor4 col = C_AMBER;
        warningLook(s.mWarnings.front(), label, col);
        cells.push_back({ "WARNING", label, col, H_NONE, "", 1.8f, false });
    }
    if (WolfDrive::isRoad(ty))
    {
        cells.push_back({ "", (s.mIndicators == 2 || s.mIndicators == 3) && blink_on ? ">" : "", C_GREEN, H_IND_R, "Right indicator", 0.6f, false });
    }
    cells.push_back({ "", "REZ", WolfVehicleRez::pending() ? C_AMBER : C_WHITE, H_REZ, WolfVehicleRez::tip(WolfVehicleRez::typeForDesign(ty)), 0.8f, true });   // [HUD REZ 2026-10-12/]
    cells.push_back({ "", "INV", WolfHud::inventoryOpen() ? C_GREEN : C_WHITE, H_INVENTORY, WolfHud::inventoryTip(), 0.8f, true });   // [HUD INVENTORY 2026-10-12/]
    cells.push_back({ "", "FULL", C_WHITE, H_MINI, "Bring the whole dashboard back", 1.0f, true });
    cells.push_back({ "", "X", C_WHITE, H_CLOSE, "Close the dashboard", 0.6f, true });

    const F32 unit = h * 1.45f;
    F32 total = 0.f;
    for (const Cell& c : cells) total += c.w * unit;
    const F32 scale = llmin(1.f, (W - 16.f) / total);
    const F32 bw = total * scale;
    const F32 l = (W - bw) * 0.5f, r = l + bw, b = bottom + 4.f, t = b + h;
    mPanelB = bottom;
    mPanelT = t + 2.f;

    roundRectF(l - 3.f, b - 3.f, r + 3.f, t + 3.f, 6.f, C_GREY, C_OFF);
    roundRectF(l, b, r, t, 5.f, LLColor4(0.10f, 0.10f, 0.11f, 0.95f), LLColor4(0.03f, 0.03f, 0.04f, 0.95f));
    const LLFontGL* vf = face("B612", LLFontGL::BOLD, h * 0.42f);
    const LLFontGL* lf = face("B612", LLFontGL::NORMAL, h * 0.24f);
    const LLFontGL* kf = face("B612", LLFontGL::BOLD, h * 0.34f);
    F32 x = l;
    for (const Cell& c : cells)
    {
        const F32 cw = c.w * unit * scale;
        const F32 cl = x + 2.f, cr = x + cw - 2.f;
        if (c.key)
        {
            const bool pressed = mPressed == c.hit;
            roundRectF(cl + 2.f, b + 5.f - (pressed ? 1.f : 0.f), cr - 2.f, t - 5.f - (pressed ? 1.f : 0.f), 3.f,
                       LLColor4(0.22f, 0.23f, 0.25f, 1.f), LLColor4(0.12f, 0.12f, 0.14f, 1.f));
            text(kf, c.value, (cl + cr) * 0.5f, (b + t) * 0.5f - (pressed ? 1.f : 0.f), c.col);
        }
        else
        {
            rectF(cl, b + 4.f, cr, t - 4.f, C_BLACK);
            text(lf, c.label, (cl + cr) * 0.5f, t - 4.f - h * 0.17f, C_GREY);
            text(vf, c.value, (cl + cr) * 0.5f, b + 4.f + h * 0.30f, c.col);
        }
        if (c.hit != H_NONE)
        {
            addHit(cl, b, cr, t, c.hit, c.tip);
        }
        x += cw;
    }
}
// </WolfViewer>

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
    if (!gSavedSettings.getBOOL("WolfDashboardBackground") || gSavedSettings.getBOOL("WolfDashboardMini"))   // <WolfViewer 2026-10-10/> mini: only its controls
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
        // <WolfViewer 2026-10-11> w65: built per design - the speedboat's UNITS goes knots / km/h / mph, the spaceship
        // has neither UNITS nor TRIP (m/s and no odometer); BOAT and SHIP join the design buttons.
        const bool boat = ty == WolfDrive::TYPE_SPEEDBOAT || ty == WolfDrive::TYPE_SUBMARINE, ship = ty == WolfDrive::TYPE_SPACESHIP;
        std::vector<B> bs =
        {
            { 30.f, "X", false, WolfDashboard::H_CLOSE, "Close the dashboard. Wolf World > Car Dashboard / Speedboat Helm / Spaceship HUD (or the toolbar icons) bring it back." },
            { 70.f, "MINI", false, WolfDashboard::H_MINI, "Shrink the dashboard to one small bar; FULL on it brings it back" },
            { 70.f, "HELP", false, WolfDashboard::H_HELP, "How the dashboard works, setting up a wheel and keys, and wolfDashboard for vehicle makers" },
            { 120.f, "CONTROLS", false, WolfDashboard::H_CONTROLS, "Driving Controls: a steering wheel, pedals or gamepad, and the keys for the horn, lamps, gears and the boat / spaceship buttons" },
            { 70.f, "MAP", false, WolfDashboard::H_MAP, "World Map, centred on where you are" },
            { 120.f, "INVENTORY", WolfHud::inventoryOpen(), WolfDashboard::H_INVENTORY, WolfHud::inventoryTip() },   // [HUD INVENTORY 2026-10-12/]
            // [HUD REZ 2026-10-12] Paul: "a rez button on each hud that rezzes the vehicle" - lit while a rez is under way
            { 70.f, "REZ", WolfVehicleRez::pending(), WolfDashboard::H_REZ, WolfVehicleRez::tip(WolfVehicleRez::typeForDesign(ty)) },
            { 92.f, "PANEL", bg, WolfDashboard::H_PANEL, bg ? "Background on: click to float the instruments over the world" : "Background off: click to put the panel back" },
        };
        if (!ship)
        {
            bs.push_back({ 92.f, "TRIP 0", false, WolfDashboard::H_TRIP, "Reset the trip meter (and the bike's lean record)" });
        }
        if (boat)
        {
            const char* next = s.mUnits == WolfDrive::Shown::UNITS_KN ? "km/h" : s.mUnits == WolfDrive::Shown::UNITS_KMH ? "mph" : "knots";
            bs.push_back({ 92.f, s.mUnits == WolfDrive::Shown::UNITS_KN ? "KNOTS" : unitUpper(s), false, WolfDashboard::H_UNITS,
                           std::string("Speed in ") + unitName(s) + ": click for " + next });
        }
        else if (!ship)
        {
            bs.push_back({ 92.f, s.mMph ? "MPH" : "KM/H", false, WolfDashboard::H_UNITS, s.mMph ? "Speed in miles per hour: click for km/h" : "Speed in km/h: click for miles per hour" });
        }
        // [HUD SIZE 2026-10-12] Paul: "only the car hud needs extra buttons lorry, car, bike the others dont need them".
        // The speedboat, spaceship and submarine have none: Wolf World > Speedboat Helm / Spaceship HUD / Submarine
        // Helm, their toolbar icons (WolfDrive::toggleDesign) and the craft's own script ("type") choose them, and the
        // car icon or Wolf World > Car Dashboard goes back to the road design last used.
        if (WolfDrive::isRoad(ty))
        {
            bs.push_back({ 92.f, "BIKE", ty == WolfDrive::TYPE_BIKE, WolfDashboard::H_TYPE_BIKE, "Motorbike dashboard (superbike TFT, handlebars, sequential gears)" });
            bs.push_back({ 92.f, "TRUCK", ty == WolfDrive::TYPE_TRUCK, WolfDashboard::H_TYPE_TRUCK, "Truck dashboard (Scania style, 12-speed range-splitter: manual or automatic, air brakes)" });
            bs.push_back({ 92.f, "CAR", ty == WolfDrive::TYPE_CAR, WolfDashboard::H_TYPE_CAR, "Car dashboard (Porsche 911 style, 8 gears: manual or automatic)" });
        }
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

    // ════════════════════════════════════════════════════════════════════════════════════
    // [GEAR STICK 2026-10-10] Paul: "the car gear sticks and lorry gears sticks dont look like a
    // real gear shifts research on the internet and make that better". Drawn as the real things
    // look from the driver's seat, looking down at the console:
    //   CAR MANUAL   an H-pattern gate in a brushed plate (the 911's: R out to the left, then
    //                1/2, 3/4, 5/6 and 7/8 - Porsche put its 7-speed's seventh up and right of
    //                fifth), the lever in a leather gaiter, a round black knob with the shift
    //                pattern engraved on its top. The neutral rail runs across the middle.
    //   TRUCK MANUAL Scania GRS905 range-splitter: first is left and back, second right and
    //                forward, third straight back from second (R forward of first); the range
    //                collar under the knob (LOW 1-6 / HIGH 7-12) and the splitter rocker on the
    //                knob's front (L / H half-gear) - each gear is a slot, a range and a split.
    //   AUTOMATIC    a straight gated slot P R N D (truck Opticruise R N A) with the lit letter
    //                beside it, and the M gate beside D: forward + / back - takes a gear.
    // Every slot is a click (WolfDrive::setSelector: one Page Up / Page Down tap per step, as
    // the keys do); the knob glides to the gear it is in.
    // ════════════════════════════════════════════════════════════════════════════════════

    struct KnobPos { F32 x = -1.f, y = -1.f; };
    KnobPos sKnob[3];     // per vehicle type: where the knob is drawn, gliding to its slot

    void glideKnob(KnobPos& k, F32 tx, F32 ty)
    {
        if (k.x < 0.f) { k.x = tx; k.y = ty; return; }
        const F32 a = 1.f - expf(-llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.1f) / 0.05f);
        k.x += (tx - k.x) * a;
        k.y += (ty - k.y) * a;
    }

    void plate(F32 l, F32 b, F32 r, F32 t)
    {
        roundRectF(l, b, r, t, S(10.f), LLColor4(0.20f, 0.21f, 0.23f, 1.f), LLColor4(0.08f, 0.085f, 0.095f, 1.f));
        // brushed lines
        for (F32 y = b + S(6.f); y < t - S(4.f); y += S(3.f))
        {
            rectF(l + S(6.f), y, r - S(6.f), y + 1.f, LLColor4(1.f, 1.f, 1.f, 0.025f));
        }
        frameW(l, b, r, t, 1.2f, LLColor4(0.33f, 0.35f, 0.38f, 1.f));
    }

    // A slot cut in the plate, from (x0,y0) to (x1,y1): dark, with a light lip on one side.
    void slot(F32 x0, F32 y0, F32 x1, F32 y1)
    {
        const F32 w = S(5.f);
        lineW(x0, y0, x1, y1, w + S(2.f), LLColor4(0.34f, 0.36f, 0.39f, 1.f));
        lineW(x0, y0, x1, y1, w, LLColor4(0.01f, 0.01f, 0.012f, 1.f));
    }

    // The lever seen from above: the gaiter round the pivot, the shaft to the knob, the knob.
    void lever(F32 px, F32 py, F32 kx, F32 ky, F32 kr, bool truck, const std::string& top_label)
    {
        // leather gaiter: folded rings round the pivot, pulled towards the knob
        const F32 gr = kr * 1.7f;
        for (S32 i = 3; i >= 0; --i)
        {
            const F32 f = (F32)i / 3.f;
            const F32 cx = px + (kx - px) * (0.25f * (1.f - f)), cy = py + (ky - py) * (0.25f * (1.f - f));
            const F32 shade = 0.05f + 0.035f * (3 - i);
            circleF(cx, cy, gr * (0.55f + 0.45f * f), LLColor4(shade, shade, shade * 1.05f, 1.f), 32);
        }
        // shaft
        lineW(px, py, kx, ky, kr * 0.45f, LLColor4(0.05f, 0.05f, 0.055f, 1.f));
        // knob: a shaded dome, leather black (car) or the truck's grey
        circleF(kx + S(2.f), ky - S(2.f), kr, LLColor4(0.f, 0.f, 0.f, 0.5f), 40);
        circleShaded(kx, ky, kr, truck ? LLColor4(0.42f, 0.44f, 0.47f, 1.f) : LLColor4(0.30f, 0.30f, 0.32f, 1.f),
                     truck ? LLColor4(0.10f, 0.11f, 0.12f, 1.f) : LLColor4(0.02f, 0.02f, 0.025f, 1.f), 40);
        if (!top_label.empty())
        {
            T(kx, ky, top_label, llclamp(kr / g.cur.u * 0.9f, 9.f, 16.f), C_WHITE, true);
        }
    }

    // The engraved shift pattern on top of a car knob: R 1 3 5 7 over 2 4 6 8, in white lines.
    void knobPattern(F32 cx, F32 cy, F32 kr, S32 gears)
    {
        const S32 cols = (gears + 1) / 2 + 1;
        const F32 w = kr * 1.15f, h = kr * 0.9f;
        const LLColor4 ink(0.85f, 0.86f, 0.88f, 0.9f);
        const F32 lw = llmax(1.f, kr * 0.06f);
        lineW(cx - w * 0.5f, cy, cx + w * 0.5f, cy, lw, ink);
        for (S32 i = 0; i < cols; ++i)
        {
            const F32 x = cx - w * 0.5f + w * (F32)i / (F32)(cols - 1);
            lineW(x, cy, x, cy + h * 0.5f, lw, ink);
            if (i > 0) lineW(x, cy, x, cy - h * 0.5f, lw, ink);
        }
    }

    void gearShifter(Ctx& c, F32 xl, F32 xr, F32 ytop, F32 ybot, WolfDrive::EType ty)
    {
        WolfDrive& d = WolfDrive::instance();
        const bool ab = d.autoBox();
        const bool truck = ty == WolfDrive::TYPE_TRUCK;
        const S32 gears = WolfDrive::spec(ty).mGears;
        const S32 sel = d.selector();
        KnobPos& kp = sKnob[llclamp((S32)ty, 0, 2)];

        // the gearbox mode button above (as before), the +/- below
        const F32 mt = ytop + S(30.f), mb = ytop + S(6.f);
        button(c, xl, mb, xr, mt, ab ? "AUTO" : "MANUAL", ab, WolfDashboard::H_BOX_MODE,
               ab ? "Gearbox: AUTOMATIC - it picks the gear from the speed; + / - (or the M gate) take one yourself. Click for MANUAL."
                  : "Gearbox: MANUAL - click a gear in the gate, or Page Up / Page Down (+ / -) one gear at a time. Click for AUTOMATIC.",
               11.f);
        const F32 bh = S(22.f);
        const F32 pb = ybot + bh + S(6.f);     // the plate's bottom: the +/- row under it
        const F32 mid = (xl + xr) * 0.5f;
        button(c, xl, ybot, mid - S(3.f), ybot + bh, "-", false, WolfDashboard::H_SHIFT_DOWN,
               "Gear down: one gear (Page Down, or your gear-down key or button)", 16.f);
        button(c, mid + S(3.f), ybot, xr, ybot + bh, "+", false, WolfDashboard::H_SHIFT_UP,
               "Gear up: one gear (Page Up, or your gear-up key or button)", 16.f);
        plate(xl, pb, xr, ytop);

        const F32 w = xr - xl, h = ytop - pb;
        const F32 kr = llmin(w * 0.15f, S(15.f));

        if (ab)
        {
            // AUTOMATIC: P R N D (truck R N A) in a straight gate; the M gate beside D.
            const std::vector<S32> pos = WolfDrive::selectorPositions(ty, true);
            const S32 n = (S32)pos.size();
            const F32 gx = xl + w * 0.36f, top = ytop - h * 0.12f, bot = pb + h * 0.18f;
            slot(gx, top, gx, bot);
            const F32 mx = xl + w * 0.72f;
            slot(gx, bot, mx, bot);                                    // across to M
            slot(mx, bot + h * 0.18f, mx, bot - h * 0.06f);            // M: + forward, - back
            F32 tx = gx, tyy = bot;
            for (S32 i = 0; i < n; ++i)
            {
                const F32 y = top - (top - bot) * (F32)i / (F32)llmax(1, n - 1);
                const S32 gsel = pos[i];
                const bool on = sel == gsel;
                std::string label = WolfDrive::gearLabel(gsel);
                if (gsel == WolfDrive::GEAR_D && truck) label = "A";
                T(xl + w * 0.13f, y, label, 13.f, on ? (gsel == WolfDrive::GEAR_R ? C_RED : C_GREEN) : C_DIM, true);
                if (on) { tx = gx; tyy = y; }
                c.hit(xl, y - h * 0.08f, gx + w * 0.12f, y + h * 0.08f, (WolfDashboard::EHit)(WolfDashboard::H_SEL_0 + i),
                      "Put the lever in " + label + (gsel == WolfDrive::GEAR_D ? " - the automatic drives" : ""));
            }
            T(mx + w * 0.14f, bot + h * 0.16f, "+", 12.f, C_GREY, true);
            T(mx + w * 0.14f, bot - h * 0.04f, "-", 12.f, C_GREY, true);
            T(mx, bot - h * 0.14f, "M", 11.f, C_GREY, true);
            c.hit(mx - w * 0.1f, bot + h * 0.02f, mx + w * 0.22f, bot + h * 0.24f, WolfDashboard::H_SHIFT_UP, "M gate forward: a gear up, held by you");
            c.hit(mx - w * 0.1f, bot - h * 0.12f, mx + w * 0.22f, bot - h * 0.01f, WolfDashboard::H_SHIFT_DOWN, "M gate back: a gear down, held by you");
            glideKnob(kp, tx, tyy);
            lever(gx, (top + bot) * 0.5f, kp.x, kp.y, kr * 0.85f, truck, "");
            return;
        }

        // MANUAL
        const F32 rail = pb + h * 0.5f;
        const F32 up = ytop - h * 0.14f, down = pb + h * 0.14f;
        struct Gate { S32 sel; F32 x, y; };
        std::vector<Gate> gates;
        if (!truck)
        {
            // R 1 3 5 7 forward, 2 4 6 8 back; R out on the left (911 style)
            const S32 cols = (gears + 1) / 2 + 1;
            auto colx = [&](S32 i) { return xl + w * 0.12f + (w * 0.76f) * (F32)i / (F32)(cols - 1); };
            slot(colx(0), rail, colx(cols - 1), rail);
            gates.push_back({ WolfDrive::GEAR_R, colx(0), up });
            slot(colx(0), rail, colx(0), up);
            for (S32 gnum = 1; gnum <= gears; ++gnum)
            {
                const S32 col = (gnum + 1) / 2;
                const F32 y = (gnum % 2) ? up : down;
                slot(colx(col), rail, colx(col), y);
                gates.push_back({ gnum, colx(col), y });
            }
            gates.push_back({ WolfDrive::GEAR_N, colx((cols - 1) / 2), rail });
        }
        else
        {
            // GRS905: R forward-left, 1 back-left, 2 forward-right, 3 back-right; the range and the
            // split say which of the twelve
            const F32 x0 = xl + w * 0.3f, x1 = xl + w * 0.72f;
            slot(x0, rail, x1, rail);
            slot(x0, up, x0, down);
            slot(x1, up, x1, down);
            const S32 cur = sel >= 1 ? sel : 1;
            const S32 range = (cur - 1) / 6, split = (cur - 1) % 2;
            gates.push_back({ WolfDrive::GEAR_R, x0, up });
            for (S32 p = 1; p <= 3; ++p)
            {
                const S32 gnum = range * 6 + (p - 1) * 2 + split + 1;
                gates.push_back({ gnum, p == 1 ? x0 : x1, p == 2 ? up : down });
            }
            gates.push_back({ WolfDrive::GEAR_N, (x0 + x1) * 0.5f, rail });
        }

        F32 tx = gates.back().x, tyy = rail;   // N unless a gate matches
        for (const Gate& gt : gates)
        {
            bool on;
            std::string label;
            if (truck && gt.sel >= 1)
            {
                // the slot holds two of the twelve in this range: it is "on" for either split
                on = sel >= 1 && ((sel - 1) / 6 == (gt.sel - 1) / 6) && (((sel - 1) % 6) / 2 == ((gt.sel - 1) % 6) / 2);
                label = llformat("%d", ((gt.sel - 1) % 6) / 2 + 1);
            }
            else
            {
                on = sel == gt.sel;
                label = WolfDrive::gearLabel(gt.sel);
            }
            if (on) { tx = gt.x; tyy = gt.y; }
            if (gt.sel != WolfDrive::GEAR_N)
            {
                const F32 ly = gt.y + (gt.y > rail ? S(10.f) : -S(10.f));
                T(gt.x, ly, label, 11.f, on ? (gt.sel == WolfDrive::GEAR_R ? C_RED : C_WHITE) : C_DIM, true);
            }
            const S32 idx = gt.sel == WolfDrive::GEAR_R ? 0 : gt.sel == WolfDrive::GEAR_N ? 1 : 1 + gt.sel;
            const F32 hw = llmax(S(8.f), w * 0.09f);
            const std::string tip = gt.sel == WolfDrive::GEAR_R ? std::string("Reverse")
                                  : gt.sel == WolfDrive::GEAR_N ? std::string("Neutral")
                                  : truck ? llformat("Gear %d (this slot in the %s range, %s split)", gt.sel, gt.sel <= 6 ? "LOW" : "HIGH", ((gt.sel - 1) % 2) ? "H" : "L")
                                          : llformat("Gear %d", gt.sel);
            c.hit(gt.x - hw, gt.y - hw, gt.x + hw, gt.y + hw, (WolfDashboard::EHit)(WolfDashboard::H_GATE_0 + llclamp(idx, 0, 13)), tip + ": click to put it in");
        }
        glideKnob(kp, tx, tyy);
        const F32 px = (xl + xr) * 0.5f, py = rail;
        lever(px, py, kp.x, kp.y, kr, truck, std::string());
        if (!truck)
        {
            knobPattern(kp.x, kp.y, kr * 0.75f, gears);
        }
        else
        {
            // the range collar round the knob and the splitter rocker on its front
            const bool high = sel >= 7;
            const bool hsplit = sel >= 1 && ((sel - 1) % 2) == 1;
            arcW(kp.x, kp.y, kr + S(3.f), S(4.f), 0.f, 6.2831853f, high ? C_AMBER : C_TEAL, 40);
            const F32 rw = kr * 0.9f, rh = kr * 0.45f;
            roundRectF(kp.x - rw * 0.5f, kp.y - kr * 0.75f, kp.x + rw * 0.5f, kp.y - kr * 0.75f + rh, S(2.f),
                       LLColor4(0.15f, 0.15f, 0.17f, 1.f), LLColor4(0.06f, 0.06f, 0.07f, 1.f));
            T(kp.x, kp.y - kr * 0.75f + rh * 0.5f, hsplit ? "H" : "L", 9.f, C_WHITE, true);
            T(kp.x, kp.y + kr * 0.25f, high ? "HI" : "LO", 9.f, high ? C_AMBER : C_TEAL, true);
            c.hit(kp.x - rw * 0.5f, kp.y - kr * 0.8f, kp.x + rw * 0.5f, kp.y - kr * 0.75f + rh, WolfDashboard::H_SPLIT,
                  hsplit ? "Splitter H (the higher half-gear): click for L" : "Splitter L (the lower half-gear): click for H");
            c.hit(kp.x - kr - S(5.f), kp.y - S(2.f), kp.x + kr + S(5.f), kp.y + kr + S(5.f), WolfDashboard::H_RANGE,
                  high ? "Range collar: HIGH (gears 7-12). Click for LOW." : "Range collar: LOW (gears 1-6). Click for HIGH.");
            T(xl + w * 0.5f, pb + S(8.f), high ? "RANGE HIGH" : "RANGE LOW", 9.f, high ? C_AMBER : C_TEAL);
        }
    }

    /** " (key H)" when the driver gave the action a key (Driving Controls), else nothing. */
    std::string keyHint(S32 action)
    {
        const std::string k = WolfDrive::instance().actionCfg(action).mKey;
        return k.empty() ? std::string() : " (key " + k + ")";
    }

    /**
     * [WOLF RETARDER 2026-10-12] The truck's retarder stalk, beside the wheel (the real ones and their sources are in
     * wolfdrive.cpp, above WolfDrive::setRetarderStalk). Top to bottom:
     *   AUT      the brake pedal brings the retarder in first (lit while on; the driver's own setting)
     *   DSC      downhill speed control on / off, then its set speed (the script's dsc_speed) with - and + (held)
     *   0..5     the stalk: the knob sits in the position the DRIVER chose; the figures 1..5 light blue up to the
     *            stage the vehicle's script says is braking NOW (the "retarder" key), which can be less (the
     *            accelerator, low speed, heat) or more (the brake pedal with AUT, downhill speed control)
     *   RET      the retarder lamp: blue while it brakes, amber flashing while the script reports it held back by heat
     * Pulled DOWN is more braking, as on the Scania lever ("When you move the auxiliary brake lever downwards, the
     * braking effect increases gradually", Scania Next Gen Quick Guide).
     */
    void retarderStalk(Ctx& c, F32 xl, F32 xr, F32 ytop, F32 ybot, const WolfDrive::Shown& s)
    {
        WolfDrive& d = WolfDrive::instance();
        const F32 w = xr - xl;
        const F32 bh = S(20.f), gap = S(4.f);
        F32 y = ytop;
        button(c, xl, y - bh, xr, y, "AUT", d.retarderAut(), WolfDashboard::H_RET_AUT,
               std::string(d.retarderAut() ? "Retarder AUT: ON - pressing the brake pedal brings the retarder in first, before the wheel brakes. Click for OFF."
                                           : "Retarder AUT: OFF - the brake pedal works the wheel brakes only. Click for ON.")
               + keyHint(WolfDrive::ACT_RETARDER_AUT), 9.f);
        y -= bh + gap;
        button(c, xl, y - bh, xr, y, "DSC", d.retarderDsc(), WolfDashboard::H_RET_DSC,
               std::string(d.retarderDsc() ? "Downhill speed control: ON - the retarder holds the set speed downhill (the accelerator pauses it). Click for OFF."
                                           : "Downhill speed control: OFF. Click to hold the speed you are doing now, downhill, with the retarder.")
               + keyHint(WolfDrive::ACT_RETARDER_DSC), 9.f);
        y -= bh + gap;
        // the set speed, in the shown units (the script sends km/h)
        const std::string set = s.mHasDscSpeed ? llformat("%d", (S32)llround(speedIn(s, s.mDscSpeed / KMH))) : std::string("--");
        T((xl + xr) * 0.5f, y - S(8.f), set, 12.f, s.mHasDscSpeed ? C_TEAL : C_DIM, true, true);
        y -= S(17.f);
        const F32 half = (w - gap) * 0.5f;
        button(c, xl, y - bh, xl + half, y, "-", d.actionHeld(WolfDrive::ACT_RETARDER_DSC_DOWN), WolfDashboard::H_RET_DSC_DOWN,
               "Downhill speed control: set speed down (hold)" + keyHint(WolfDrive::ACT_RETARDER_DSC_DOWN), 14.f);
        button(c, xr - half, y - bh, xr, y, "+", d.actionHeld(WolfDrive::ACT_RETARDER_DSC_UP), WolfDashboard::H_RET_DSC_UP,
               "Downhill speed control: set speed up (hold)" + keyHint(WolfDrive::ACT_RETARDER_DSC_UP), 14.f);
        y -= bh + gap * 2.f;
        // the stalk's gate: six slots, 0 at the top
        const F32 lamp_h = S(18.f);
        const F32 gt = y, gb = ybot + lamp_h + gap;
        const F32 cell = (gt - gb) / 6.f;
        const F32 tx = xl + w * 0.30f;                     // the slot the knob runs in
        roundRectF(xl, gb, xr, gt, S(6.f), LLColor4(0.043f, 0.043f, 0.051f, 1.f), LLColor4(0.043f, 0.043f, 0.051f, 1.f));
        frameW(xl, gb, xr, gt, 1.f, LLColor4(0.227f, 0.239f, 0.263f, 1.f));
        lineW(tx, gt - cell * 0.5f, tx, gb + cell * 0.5f, S(5.f) + S(2.f), LLColor4(0.34f, 0.36f, 0.39f, 1.f));
        lineW(tx, gt - cell * 0.5f, tx, gb + cell * 0.5f, S(5.f), LLColor4(0.01f, 0.01f, 0.012f, 1.f));
        const S32 sel = d.retarderStalk();
        const S32 active = llclamp(s.mRetarder, 0, WolfDrive::RETARDER_STALK_MAX);
        static const char* const TORQUE[6] = { "off", "500 Nm", "1000 Nm", "1500 Nm", "2000 Nm", "3000 Nm" };
        for (S32 i = 0; i <= WolfDrive::RETARDER_STALK_MAX; ++i)
        {
            const F32 ct = gt - cell * i, cb = ct - cell, cy = (ct + cb) * 0.5f;
            const bool braking = i >= 1 && i <= active;
            if (braking)
            {
                roundRectF(xl + w * 0.56f, cb + S(2.f), xr - S(3.f), ct - S(2.f), S(3.f), alpha(C_BLUE, 0.35f), alpha(C_BLUE, 0.35f));
            }
            rectF(tx - S(6.f), cy - 0.5f, tx - S(3.f), cy + 0.5f, C_GREY);   // the notch
            T(xl + w * 0.75f, cy, llformat("%d", i), 11.f, braking ? C_BLUE : (i == sel ? C_WHITE : C_GREY), true);
            c.hit(xl, cb, xr, ct, (WolfDashboard::EHit)(WolfDashboard::H_RET_0 + i),
                  i == 0 ? "Retarder stalk: 0, off. Drag the stalk, click a position or use the scroll wheel over it."
                               + keyHint(WolfDrive::ACT_RETARDER_DOWN)
                         : llformat("Retarder stalk: position %d, a fixed %s - braking whenever the accelerator is up "
                                    "(Scania's five steps). Drag, click or scroll; blue: the stage braking now.", i, TORQUE[i])
                               + keyHint(i > sel ? WolfDrive::ACT_RETARDER_UP : WolfDrive::ACT_RETARDER_DOWN));
        }
        // the knob in the chosen slot (the lever seen from the driver's side)
        static F32 knob_y = -1.f;
        const F32 want = gt - cell * (sel + 0.5f);
        const F32 a = 1.f - expf(-llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.1f) / 0.05f);
        knob_y = knob_y < 0.f || knob_y > gt || knob_y < gb ? want : knob_y + (want - knob_y) * a;
        const F32 kr = llmin(cell * 0.42f, w * 0.22f);
        circleF(tx + S(1.5f), knob_y - S(1.5f), kr, LLColor4(0.f, 0.f, 0.f, 0.5f), 24);
        circleShaded(tx, knob_y, kr, LLColor4(0.42f, 0.44f, 0.47f, 1.f), LLColor4(0.10f, 0.11f, 0.12f, 1.f), 24);
        // the retarder lamp
        const bool hot = std::find(s.mWarnings.begin(), s.mWarnings.end(), std::string("retarder")) != s.mWarnings.end();
        const bool lit = hot ? blink(0.7f) : active > 0;
        const LLColor4 lc = hot ? C_AMBER : C_BLUE;
        roundRectF(xl, ybot, xr, ybot + lamp_h, S(4.f), alpha(lit ? lc : C_OFF, lit ? 0.22f : 0.5f), alpha(lit ? lc : C_OFF, lit ? 0.22f : 0.5f));
        T((xl + xr) * 0.5f, ybot + lamp_h * 0.5f, "RET", 11.f, lit ? lc : LLColor4(0.235f, 0.24f, 0.26f, 1.f), true);
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

    // ════════════════════════════════════════════════════════════════════════════════════
    // <WolfViewer 2026-10-11> w65 Space Rabbit: the SPEEDBOAT HELM and the SPACESHIP HUD. Both sit in the same panel
    // as the road designs (the mockup's 1600 x 304 band, anchors L / C / R, two rows in a narrow window) and take
    // the same WolfDrive::Shown values; their buttons send the wolfDriveInput bits of PROTOCOL.md.
    // ════════════════════════════════════════════════════════════════════════════════════

    // Source: wolfflightdeck.cpp wrap180
    F32 wrap180(F32 a)
    {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    // Source: wolfflightdeck.cpp struct Rot - a point turned about (cx, cy) by deg, counter-clockwise
    struct Rot
    {
        F32 mCx, mCy, mC, mS;
        Rot(F32 cx, F32 cy, F32 deg) : mCx(cx), mCy(cy), mC(cosf(deg * DEG_TO_RAD)), mS(sinf(deg * DEG_TO_RAD)) {}
        void p(F32 x, F32 y, F32& ox, F32& oy) const
        {
            ox = mCx + x * mC - y * mS;
            oy = mCy + x * mS + y * mC;
        }
    };

    std::string compassLabel(S32 deg)
    {
        switch (deg)
        {
        case 0:   return "N";
        case 90:  return "E";
        case 180: return "S";
        case 270: return "W";
        default:  return llformat("%d", deg);
        }
    }

    /** A heading tape, 50 degrees either side of the lubber line: ticks every 5, figures every 30, the course over
     *  the ground as a small diamond (when moving). Used by both designs, in their own colours. */
    void headingTape(F32 l, F32 r, F32 b, F32 t, F32 hdg, F32 trk, bool show_trk, const LLColor4& ink, const LLColor4& lubber,
                     const LLColor4& track, const LLColor4& bg)
    {
        if (bg.mV[VALPHA] > 0.f) rectF(l, b, r, t, bg);
        const F32 span = 50.f;
        const F32 ppd = (r - l) * 0.5f / span;
        const F32 cx = (l + r) * 0.5f, h = t - b;
        {
            LLLocalClipRect clip(WolfHudScale::clip(LLRect((S32)l, (S32)t, (S32)r, (S32)b)));
            const LLFontGL* f = font(11.f, true);
            for (S32 k = 0; k < 360; k += 5)
            {
                const F32 rel = wrap180((F32)k - hdg);
                if (fabsf(rel) > span + 6.f) continue;
                const F32 x = cx + rel * ppd;
                const bool major = k % 10 == 0;
                lineW(x, t, x, t - h * (major ? 0.32f : 0.18f), major ? 1.6f : 1.f, ink);
                if (k % 30 == 0)
                {
                    text(f, compassLabel(k), x, b + h * 0.32f, ink);
                }
            }
            if (show_trk)
            {
                const F32 rel = wrap180(trk - hdg);
                if (fabsf(rel) < span)
                {
                    const F32 x = cx + rel * ppd, y = b + h * 0.10f, d = h * 0.12f;
                    quadF(x, y - d, x + d, y, x, y + d, x - d, y, track);
                }
            }
        }
        triF(cx, t - h * 0.40f, cx - h * 0.18f, t + h * 0.02f, cx + h * 0.18f, t + h * 0.02f, lubber);
    }

    /** A labelled horizontal bar 0..100 %. `has` false: "--" and an empty track (the script sends none). */
    void percentBar(F32 l, F32 r, F32 y_label, F32 b, F32 t, const char* name, bool has, F32 v, const LLColor4& fill,
                    const LLColor4& ink, const LLColor4& track)
    {
        T(l, y_label, name, 10.f, ink, true, false, LLFontGL::LEFT);
        T(r, y_label, has ? llformat("%d%%", (S32)llround(v)) : std::string("--"), 10.f, has ? fill : alpha(ink, 0.5f), true, true, LLFontGL::RIGHT);
        rectF(l, b, r, t, track);
        if (has)
        {
            rectF(l, b, l + (r - l) * llclamp(v / 100.f, 0.f, 1.f), t, fill);
        }
        for (S32 k = 1; k < 10; ++k)
        {
            const F32 x = l + (r - l) * k / 10.f;
            rectF(x, b, x + 1.f, t, alpha(C_BLACK, 0.5f));
        }
        frameW(l, b, r, t, 1.f, alpha(ink, 0.6f));
    }

    // ---- THE SPEEDBOAT HELM ----
    // After the engine pages of Mercury VesselView, Yamaha's CL7 and Garmin's gauge screens: a navy panel, white
    // figures, a teal accent. The tachometer on the left (the Verado 300's full-throttle band green, red above it -
    // wolfdrive.cpp SPEEDBOAT), the throttle lever beside it (F / N / R), the speedometer in knots on the right with
    // the helm's buttons, and a centre screen: compass and heading, the speed step, ON PLANE, the depth under the
    // hull (red under 2 m), trim, fuel and the warnings.
    void drawSpeedboat(Ctx& c, const WolfDrive::Shown& s, bool bg, U32 held)
    {
        WolfDrive& d = WolfDrive::instance();
        const WolfDrive::Spec& sp = WolfDrive::spec(WolfDrive::TYPE_SPEEDBOAT);
        if (bg)
        {
            rectG(0.f, g.bandB, g.W, g.panelTop, NAVY_HI, NAVY_LO);
            rectF(0.f, g.panelTop - 2.f, g.W, g.panelTop, C_TEAL);
        }

        // ---- left: the tachometer, the throttle lever ----
        use(g.L);
        {
            // the green band is the Verado's own; a script with its own red line or scale has its own engine
            const bool own_engine = fabsf(s.mRedline - sp.mRedline) < 1.f && fabsf(s.mMaxRpm - sp.mDialRpm) < 1.f;
            Dial tach{ X(190.f), YM(762.f), S(118.f), s.mMaxRpm, 1000.f, 0.f, 500.f, 1000.f, s.mRedline,
                       own_engine ? sp.mGreenLo : 0.f, own_engine ? sp.mGreenHi : 0.f, s.mRpm, 0.f, "x1000 RPM", false, C_TEAL, S(18.f) };
            tach.navy = true;
            drawDial(tach);
            T(X(190.f), YM(824.f), s.mEngine ? llformat("%d", (S32)llround(s.mRpm / 10.f) * 10) : std::string("ENGINE OFF"), 14.f,
              s.mEngine ? (s.mRpm >= s.mRedline ? C_RED : C_WHITE) : C_AMBER, true, true);
        }
        {
            const F32 lx = X(385.f), ltop = YM(628.f), lbot = YM(812.f), lmid = (ltop + lbot) * 0.5f;
            roundRectF(X(338.f), lbot - S(12.f), X(432.f), ltop + S(12.f), S(10.f), NAVY_SCREEN, NAVY_SCREEN);
            frameW(X(338.f), lbot - S(12.f), X(432.f), ltop + S(12.f), 1.f, NAVY_LINE);
            lineW(lx, ltop, lx, lbot, S(6.f), LLColor4(0.f, 0.f, 0.f, 1.f));
            lineW(X(372.f), lmid, X(398.f), lmid, 1.5f, NAVY_LINE);
            const F32 thr = llclamp(s.mThrottlePct, -100.f, 100.f);
            T(X(414.f), ltop, "F", 13.f, thr > 0.f ? C_GREEN : C_DIM, true);
            T(X(414.f), lmid, "N", 13.f, thr == 0.f ? C_WHITE : C_DIM, true);
            T(X(414.f), lbot, "R", 13.f, thr < 0.f ? C_RED : C_DIM, true);
            const F32 hy = lmid + (thr / 100.f) * (ltop - lmid);
            lineW(lx, lmid, lx, hy, S(4.f), LLColor4(0.55f, 0.58f, 0.62f, 1.f));
            roundRectF(lx - S(20.f), hy - S(8.f), lx + S(12.f), hy + S(8.f), S(4.f), LLColor4(0.2f, 0.21f, 0.23f, 1.f), LLColor4(0.04f, 0.04f, 0.05f, 1.f));
            frameW(lx - S(20.f), hy - S(8.f), lx + S(12.f), hy + S(8.f), 1.f, C_TEAL);
            T(X(385.f), YM(836.f), llformat("%+d%%", (S32)llround(thr)), 11.f, s.mThrottleScripted ? C_TEAL : C_GREY, true, true);
            button(c, X(322.f), YM(880.f), X(383.f), YM(848.f), "AHEAD", d.throttle() > 0.f, WolfDashboard::H_THROTTLE,
                   "Ahead: hold it (or Up / W, or your accelerator)", 9.f);
            button(c, X(387.f), YM(880.f), X(448.f), YM(848.f), "ASTERN", d.brake() > 0.f, WolfDashboard::H_BRAKE,
                   "Astern: hold it (or Down / S, or your brake)", 9.f);
        }

        // ---- centre: the screen ----
        use(g.C);
        {
            const F32 sl = X(472.f), sr = X(1128.f), st = YM(610.f), sb = YM(890.f);
            roundRectF(sl, sb, sr, st, S(12.f), alpha(NAVY_SCREEN, bg ? 1.f : 0.9f), alpha(NAVY_SCREEN, bg ? 1.f : 0.9f));
            frameW(sl, sb, sr, st, 1.f, NAVY_LINE);
            headingTape(X(492.f), X(1108.f), YM(664.f), YM(620.f), s.mHeading, s.mTrack, s.mSpeed > 1.f, C_WHITE, C_TEAL, C_TEAL,
                        LLColor4(0.02f, 0.06f, 0.13f, 1.f));
            T(X(720.f), YM(690.f), "HDG", 11.f, C_TEAL, true);
            T(X(800.f), YM(690.f), llformat("%03d", (S32)llround(s.mHeading) % 360), 24.f, C_WHITE, true, true);
            T(X(890.f), YM(690.f), s.mSpeed > 1.f ? llformat("COG %03d", (S32)llround(s.mTrack) % 360) : std::string("COG ---"), 11.f, C_GREY, false, true);

            auto box = [&](F32 l, F32 r, const LLColor4& fill, const LLColor4& edge)
            {
                roundRectF(X(l), YM(794.f), X(r), YM(716.f), S(8.f), fill, fill);
                frameW(X(l), YM(794.f), X(r), YM(716.f), 1.f, edge);
            };
            const LLColor4 cell(0.03f, 0.07f, 0.15f, 1.f);
            // the speed step, with - / + (Page Down / Page Up to the boat, as the keys)
            box(492.f, 682.f, cell, NAVY_LINE);
            T(X(587.f), YM(728.f), s.mStepFromChat ? "SPEED STEP  (BOAT)" : "SPEED STEP", 10.f, C_TEAL, true);
            T(X(587.f), YM(762.f), stepText(s), 28.f, C_WHITE, true, true);
            button(c, X(498.f), YM(788.f), X(530.f), YM(746.f), "-", false, WolfDashboard::H_SHIFT_DOWN,
                   "A speed step down (Page Down, or your gear-down key or button)" + keyHint(WolfDrive::ACT_GEAR_DOWN), 16.f);
            button(c, X(644.f), YM(788.f), X(676.f), YM(746.f), "+", false, WolfDashboard::H_SHIFT_UP,
                   "A speed step up (Page Up, or your gear-up key or button)" + keyHint(WolfDrive::ACT_GEAR_UP), 16.f);
            // ON PLANE (the thresholds are wolfdrive.cpp's MODEL)
            const LLColor4 plane_col = s.mOnPlane ? C_GREEN : s.mPlaning ? C_AMBER : C_DIM;
            box(700.f, 900.f, s.mOnPlane || s.mPlaning ? alpha(plane_col, 0.22f) : cell, s.mOnPlane || s.mPlaning ? plane_col : NAVY_LINE);
            T(X(800.f), YM(742.f), s.mOnPlane ? "ON PLANE" : s.mPlaning ? "PLANING" : "DISPLACEMENT", s.mOnPlane ? 17.f : 14.f,
              s.mOnPlane || s.mPlaning ? plane_col : C_GREY, true);
            T(X(800.f), YM(774.f), llformat("BOW %+.1f   %.0f kn", s.mPitch, s.mSpeed * KN), 11.f, C_GREY, false, true);
            // the depth under the hull: water height minus the land's
            const bool shallow = s.mDepth < 2.f;
            const bool flash = shallow && blink(0.6f);
            box(918.f, 1108.f, shallow ? alpha(C_RED, flash ? 0.6f : 0.3f) : cell, shallow ? C_RED : NAVY_LINE);
            T(X(1013.f), YM(728.f), "DEPTH", 10.f, shallow ? C_WHITE : C_TEAL, true);
            T(X(1013.f), YM(764.f), s.mDepth <= 0.f ? std::string("AGROUND") : llformat("%.1f m", s.mDepth), s.mDepth <= 0.f ? 18.f : 24.f,
              C_WHITE, true, true);

            // trim (or, with no trim from the script, the bow's angle measured) and fuel
            const F32 tl = X(500.f), tr = X(780.f), tmid = (tl + tr) * 0.5f, tb = YM(840.f), tt = YM(826.f);
            T(tl, YM(810.f), s.mHasTrim ? "TRIM" : "BOW ANGLE", 10.f, C_TEAL, true, false, LLFontGL::LEFT);
            T(tr, YM(810.f), s.mHasTrim ? llformat("%+d", s.mTrim) : llformat("%+.1f", s.mPitch), 11.f, C_WHITE, true, true, LLFontGL::RIGHT);
            rectF(tl, tb, tr, tt, LLColor4(0.03f, 0.07f, 0.15f, 1.f));
            for (S32 k = -5; k <= 5; ++k)
            {
                const F32 x = tmid + (tr - tmid) * k / 5.f;
                rectF(x, tb, x + 1.f, k == 0 ? tt + S(4.f) : tt, k == 0 ? C_WHITE : NAVY_LINE);
            }
            const F32 tv = s.mHasTrim ? (F32)s.mTrim / 5.f : llclamp(s.mPitch / 10.f, -1.f, 1.f);
            const F32 tx = tmid + (tr - tmid) * llclamp(tv, -1.f, 1.f);
            triF(tx, tt, tx - S(7.f), tb - S(6.f), tx + S(7.f), tb - S(6.f), C_TEAL);
            T(tl, YM(852.f), s.mHasTrim ? "IN" : "DOWN", 8.f, C_GREY, false, false, LLFontGL::LEFT);
            T(tr, YM(852.f), s.mHasTrim ? "OUT" : "UP", 8.f, C_GREY, false, false, LLFontGL::RIGHT);
            percentBar(X(818.f), X(1100.f), YM(810.f), YM(842.f), YM(824.f), "FUEL", true, s.mFuel,
                       s.mFuel < 10.f ? C_RED : s.mFuel < 15.f ? C_AMBER : C_WHITE, C_TEAL, LLColor4(0.03f, 0.07f, 0.15f, 1.f));
            telltales(X(480.f), X(1120.f), YM(872.f), s, false, true);
        }

        // ---- right: the speedometer, the helm's buttons ----
        use(g.R);
        {
            const F32 spd = speedIn(s, s.mSpeed);
            const F32 maxs = llmax(10.f, s.mMaxSpeed);
            Dial speedo{ X(1300.f), YM(762.f), S(118.f), maxs, maxs > 100.f ? 20.f : 10.f, 0.f, maxs > 100.f ? 10.f : 5.f, 1.f,
                         0.f, 0.f, 0.f, spd, 0.f, unitName(s), false, C_TEAL, S(18.f) };
            speedo.navy = true;
            drawDial(speedo);
            T(X(1300.f), YM(824.f), llformat("%d", (S32)llround(spd)), 18.f, C_WHITE, true, true);
            const bool horn_builtin = d.actionCfg(WolfDrive::ACT_HORN).mOut == WolfDrive::OUT_NONE;
            struct HB { const char* label; bool lit; WolfDashboard::EHit id; std::string tip; };
            const HB hb[4] =
            {
                { "HORN", (held & WolfDrive::BTN_HORN) != 0, WolfDashboard::H_HORN,
                  (horn_builtin ? std::string("Horn: the built-in air horn, heard in-world by everyone near (Wolf Territories)")
                                : std::string("Horn: sends what you set in Driving Controls")) + keyHint(WolfDrive::ACT_HORN) },
                { s.mLights == 2 ? "MAIN" : "LIGHTS", s.mLights > 0, WolfDashboard::H_LIGHTS, "Navigation lights: off, on, bright" + keyHint(WolfDrive::ACT_LIGHTS) },
                { "ANCHOR", s.mAnchor, WolfDashboard::H_ANCHOR, std::string(s.mAnchor ? "Anchor down: click to weigh it" : "Anchor up: click to drop it")
                                                               + keyHint(WolfDrive::ACT_ANCHOR) },
                { "ENGINE", s.mEngine, WolfDashboard::H_ENGINE, std::string(s.mEngine ? "Engine running: click to stop it" : "Engine stopped: click to start it")
                                                               + keyHint(WolfDrive::ACT_ENGINE) },
            };
            for (S32 i = 0; i < 4; ++i)
            {
                const F32 top = 618.f + i * 66.f;
                button(c, X(1448.f), YM(top + 54.f), X(1586.f), YM(top), hb[i].label, hb[i].lit, hb[i].id, hb[i].tip, 12.f);
            }
        }
    }

    // ---- THE SPACESHIP HUD ----
    // After Elite Dangerous, Star Citizen and the Space Shuttle's glass cockpit: cyan lines on glass, amber for
    // caution, red for warning, magenta for a target (the apogee). Ship layout: the heading tape along the top,
    // the pitch ladder with the boresight (W) and the flight-path marker in the middle, speed and throttle on the
    // left with fuel / boost / shield / hull / heat, altitude above the water (the ground under it hatched) and
    // vertical speed on the right with the buttons. mode=rocket puts the launch figures in the middle instead:
    // altitude, vertical speed, apogee (z + vz^2 / 2g while climbing), stage N/M and SEPARATE.
    const LLColor4 SC_CYAN(0.30f, 0.92f, 1.00f, 1.f);
    const LLColor4 SC_MAGENTA(1.00f, 0.35f, 1.00f, 1.f);

    void drawSpaceship(Ctx& c, const WolfDrive::Shown& s, bool bg, U32 held)
    {
        const LLColor4 dim = alpha(SC_CYAN, 0.35f);
        const LLColor4 glass(0.f, 0.02f, 0.04f, 0.55f);
        if (bg)
        {
            rectG(0.f, g.bandB, g.W, g.panelTop, LLColor4(0.f, 0.03f, 0.06f, 0.55f), LLColor4(0.f, 0.01f, 0.03f, 0.75f));
            rectF(0.f, g.panelTop - 1.5f, g.W, g.panelTop, alpha(SC_CYAN, 0.6f));
        }

        // ---- left: speed tape, throttle, the systems' bars ----
        use(g.L);
        {
            const F32 tl = X(40.f), tr = X(118.f), tt = YM(620.f), tb = YM(882.f), cy = (tt + tb) * 0.5f;
            const F32 spd = speedIn(s, s.mSpeed3D);
            const F32 span = 100.f, ppu = (tt - tb) / span;
            rectF(tl, tb, tr, tt, glass);
            {
                LLLocalClipRect clip(WolfHudScale::clip(LLRect((S32)tl, (S32)tt, (S32)tr, (S32)tb)));
                for (S32 k = ((S32)(spd - span * 0.5f) / 5) * 5; k <= spd + span * 0.5f; k += 5)
                {
                    if (k < 0) continue;
                    const F32 y = cy + (k - spd) * ppu;
                    lineW(tr - S(12.f), y, tr, y, 1.2f, SC_CYAN);
                    if (k % 20 == 0) T(tr - S(16.f), y, llformat("%d", k), 10.f, SC_CYAN, false, true, LLFontGL::RIGHT);
                }
            }
            frameW(tl, tb, tr, tt, 1.f, dim);
            const F32 bh = S(13.f);
            rectF(tl - S(4.f), cy - bh, tr, cy + bh, LLColor4(0.f, 0.f, 0.f, 0.85f));
            frameW(tl - S(4.f), cy - bh, tr, cy + bh, 1.5f, SC_CYAN);
            T((tl + tr) * 0.5f - S(2.f), cy, llformat("%d", (S32)llround(spd)), 15.f, SC_CYAN, true, true);
            T((tl + tr) * 0.5f, YM(608.f), std::string("SPD ") + unitName(s), 9.f, SC_CYAN, true);
            // throttle: up from the middle ahead, down for reverse thrust
            const F32 bl = X(128.f), br = X(146.f);
            rectF(bl, tb, br, tt, glass);
            const F32 thr = llclamp(s.mThrottlePct, -100.f, 100.f);
            rectF(bl, cy, br, cy + (tt - cy) * thr / 100.f, thr >= 0.f ? SC_CYAN : C_AMBER);
            rectF(bl - S(3.f), cy - 0.75f, br + S(3.f), cy + 0.75f, C_WHITE);
            frameW(bl, tb, br, tt, 1.f, dim);
            T((bl + br) * 0.5f, YM(608.f), "THR", 9.f, SC_CYAN, true);
            T((bl + br) * 0.5f + S(8.f), YM(892.f), llformat("%d%%", (S32)llround(thr)), 9.f, thr < 0.f ? C_AMBER : SC_CYAN, true, true);
            // the systems
            auto low_bad = [](F32 v) { return v < 10.f ? C_RED : v < 25.f ? C_AMBER : SC_CYAN; };
            const F32 x0 = X(180.f), x1 = X(440.f);
            const struct { const char* name; bool has; F32 v; LLColor4 col; } bars[5] =
            {
                { "FUEL",   s.mFuelScripted, s.mFuel,   low_bad(s.mFuel) },
                { "BOOST",  s.mHasBoost,     s.mBoost,  low_bad(s.mBoost) },
                { "SHIELD", s.mHasShield,    s.mShield, low_bad(s.mShield) },
                { "HULL",   s.mHasHull,      s.mHull,   low_bad(s.mHull) },
                { "HEAT",   s.mHasHeat,      s.mHeat,   s.mHeat > 90.f ? C_RED : s.mHeat > 75.f ? C_AMBER : SC_CYAN },
            };
            for (S32 i = 0; i < 5; ++i)
            {
                const F32 y0 = 622.f + i * 52.f;
                percentBar(x0, x1, YM(y0), YM(y0 + 30.f), YM(y0 + 14.f), bars[i].name, bars[i].has, bars[i].v, bars[i].col, SC_CYAN, glass);
            }
        }

        // ---- centre ----
        use(g.C);
        headingTape(X(520.f), X(1080.f), YM(642.f), YM(606.f), s.mHeading, s.mTrack, s.mSpeed > 1.f, SC_CYAN, SC_CYAN, SC_CYAN,
                    LLColor4(0.f, 0.f, 0.f, 0.f));
        {
            const F32 bw = S(30.f);
            rectF(X(800.f) - bw, YM(668.f), X(800.f) + bw, YM(646.f), LLColor4(0.f, 0.f, 0.f, 0.85f));
            frameW(X(800.f) - bw, YM(668.f), X(800.f) + bw, YM(646.f), 1.2f, SC_CYAN);
            T(X(800.f), YM(657.f), llformat("%03d", (S32)llround(s.mHeading) % 360), 13.f, SC_CYAN, true, true);
        }
        if (!s.mRocket)
        {
            // the pitch ladder, the boresight and the flight-path marker
            const F32 cx = X(800.f), cy = YM(764.f), aw = S(230.f), ah = S(90.f);
            const F32 ppd = ah / 25.f;
            {
                LLLocalClipRect clip(WolfHudScale::clip(LLRect((S32)(cx - aw), (S32)(cy + ah), (S32)(cx + aw), (S32)(cy - ah))));
                // <WolfViewer 2026-10-11> + mRoll (right wing down) turns the world counter-clockwise: the same fix as the
                // flight deck's ADI (wolfflightdeck.cpp drawPFD), which this copied with the mirrored sign
                const Rot rot(cx, cy, s.mRoll);
                const F32 y0 = -s.mPitch * ppd;
                F32 x1, y1, x2, y2;
                rot.p(-aw * 3.f, y0, x1, y1);
                rot.p(-aw * 0.08f, y0, x2, y2);
                lineW(x1, y1, x2, y2, 1.8f, SC_CYAN);
                rot.p(aw * 0.08f, y0, x1, y1);
                rot.p(aw * 3.f, y0, x2, y2);
                lineW(x1, y1, x2, y2, 1.8f, SC_CYAN);
                for (S32 p = -85; p <= 85; p += 5)
                {
                    if (p == 0 || fabsf((F32)p - s.mPitch) > 30.f) continue;
                    const F32 y = y0 + p * ppd;
                    const bool ten = p % 10 == 0;
                    const F32 half = ten ? aw * 0.24f : aw * 0.12f, gap = aw * 0.07f;
                    const F32 tick = (p > 0 ? -1.f : 1.f) * ah * 0.06f;   // toward the horizon
                    for (S32 side = -1; side <= 1; side += 2)
                    {
                        rot.p(side * gap, y, x1, y1);
                        rot.p(side * half, y, x2, y2);
                        if (p > 0)
                        {
                            lineW(x1, y1, x2, y2, 1.3f, SC_CYAN);
                        }
                        else
                        {
                            // below the horizon dashed, as the Shuttle's and the fighters' ladders draw it
                            const F32 dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
                            const F32 dash = S(6.f);
                            for (F32 u = 0.f; u < len; u += dash * 2.f)
                            {
                                const F32 e = llmin(u + dash, len);
                                lineW(x1 + dx * u / len, y1 + dy * u / len, x1 + dx * e / len, y1 + dy * e / len, 1.3f, SC_CYAN);
                            }
                        }
                        F32 x3, y3;
                        rot.p(side * half, y + tick, x3, y3);
                        lineW(x2, y2, x3, y3, 1.3f, SC_CYAN);
                        if (ten)
                        {
                            rot.p(side * (half + aw * 0.07f), y, x3, y3);
                            text(font(9.f, true, true), llformat("%d", p), x3, y3, SC_CYAN);
                        }
                    }
                }
                // the bank pointer, turning with the ladder
                F32 px, py, qx, qy, ux, uy;
                rot.p(0.f, ah * 0.92f, px, py);
                rot.p(-aw * 0.025f, ah * 0.80f, qx, qy);
                rot.p(aw * 0.025f, ah * 0.80f, ux, uy);
                triF(px, py, qx, qy, ux, uy, SC_CYAN);
            }
            frameW(cx - aw, cy - ah, cx + aw, cy + ah, 1.f, alpha(SC_CYAN, 0.2f));
            // the fixed bank scale's ticks (10, 20, 30, 45, 60 each way) round the top
            {
                const F32 rr = ah * 0.95f;
                const F32 ticks[] = { -60.f, -45.f, -30.f, -20.f, -10.f, 0.f, 10.f, 20.f, 30.f, 45.f, 60.f };
                for (F32 a : ticks)
                {
                    const F32 ra = (90.f + a) * DEG_TO_RAD;
                    const F32 len = (a == 0.f || fabsf(a) == 30.f || fabsf(a) == 60.f) ? ah * 0.09f : ah * 0.05f;
                    lineW(cx + rr * cosf(ra), cy + rr * sinf(ra), cx + (rr + len) * cosf(ra), cy + (rr + len) * sinf(ra), 1.2f, dim);
                }
            }
            // the boresight: where the nose points
            {
                const F32 u = S(1.f);
                lineW(cx - 30.f * u, cy, cx - 13.f * u, cy, 2.f, SC_CYAN);
                lineW(cx - 13.f * u, cy, cx - 6.5f * u, cy - 8.f * u, 2.f, SC_CYAN);
                lineW(cx - 6.5f * u, cy - 8.f * u, cx, cy, 2.f, SC_CYAN);
                lineW(cx, cy, cx + 6.5f * u, cy - 8.f * u, 2.f, SC_CYAN);
                lineW(cx + 6.5f * u, cy - 8.f * u, cx + 13.f * u, cy, 2.f, SC_CYAN);
                lineW(cx + 13.f * u, cy, cx + 30.f * u, cy, 2.f, SC_CYAN);
            }
            // the flight-path marker: where the craft is really going (amber when off the box)
            if (s.mFpmValid)
            {
                F32 fx = cx + s.mFpmX * ppd, fy = cy + s.mFpmY * ppd;
                const bool off = fx < cx - aw || fx > cx + aw || fy < cy - ah || fy > cy + ah;
                fx = llclamp(fx, cx - aw + S(12.f), cx + aw - S(12.f));
                fy = llclamp(fy, cy - ah + S(12.f), cy + ah - S(12.f));
                const LLColor4 col = off ? C_AMBER : SC_CYAN;
                const F32 r = S(7.f);
                arcW(fx, fy, r, 1.8f, 0.f, 2.f * PI_F, col, 20);
                lineW(fx - r, fy, fx - r * 2.2f, fy, 1.8f, col);
                lineW(fx + r, fy, fx + r * 2.2f, fy, 1.8f, col);
                lineW(fx, fy + r, fx, fy + r * 2.f, 1.8f, col);
            }
            T(cx - aw + S(4.f), cy - ah + S(9.f), llformat("PITCH %+d  ROLL %+d", (S32)llround(s.mPitch), (S32)llround(s.mRoll)), 9.f, dim,
              false, true, LLFontGL::LEFT);
            T(cx + aw - S(4.f), cy - ah + S(9.f), llformat("FPA %+d", (S32)llround(s.mFPA)), 9.f, dim, false, true, LLFontGL::RIGHT);
        }
        else
        {
            // ROCKET: the launch figures
            const bool km = fabsf(s.mAltMSL) >= 10000.f;
            T(X(640.f), YM(680.f), "ALTITUDE", 11.f, SC_CYAN, true);
            T(X(640.f), YM(712.f), km ? llformat("%.2f km", s.mAltMSL / 1000.f) : llformat("%d m", (S32)llround(s.mAltMSL)), 30.f, SC_CYAN, true, true);
            T(X(640.f), YM(742.f), llformat("ABOVE GROUND %d m", (S32)llround(s.mAGL)), 10.f, dim, false, true);
            T(X(960.f), YM(680.f), "VERTICAL SPEED", 11.f, SC_CYAN, true);
            T(X(960.f), YM(712.f), llformat("%+.1f m/s", s.mVS), 30.f, s.mVS >= 0.f ? SC_CYAN : C_AMBER, true, true);
            const bool climbing = s.mVS > 0.f;
            const F32 apo = s.mApogee;
            T(X(960.f), YM(742.f), climbing ? (fabsf(apo) >= 10000.f ? llformat("APOGEE %.2f km", apo / 1000.f) : llformat("APOGEE %d m", (S32)llround(apo)))
                                            : std::string("APOGEE --"), 11.f, climbing ? SC_MAGENTA : dim, true, true);
            // the stage panel and SEPARATE
            roundRectF(X(560.f), YM(846.f), X(1040.f), YM(758.f), S(10.f), glass, glass);
            frameW(X(560.f), YM(846.f), X(1040.f), YM(758.f), 1.f, dim);
            T(X(640.f), YM(774.f), "STAGE", 11.f, SC_CYAN, true);
            T(X(640.f), YM(810.f), s.mStages > 0 ? llformat("%d/%d", s.mStage, s.mStages) : std::string("--"), 28.f, SC_CYAN, true, true);
            const bool more = s.mStages > 0 && s.mStage < s.mStages;
            button(c, X(740.f), YM(840.f), X(1030.f), YM(764.f), "SEPARATE", more, WolfDashboard::H_STAGE,
                   "Separate the stage: the craft's script drops the spent stage and fires the next" + keyHint(WolfDrive::ACT_STAGE), 24.f);
            T(X(800.f), YM(860.f), llformat("PITCH %+d   HDG %03d   SPD %d %s", (S32)llround(s.mPitch), (S32)llround(s.mHeading) % 360,
                                            (S32)llround(speedIn(s, s.mSpeed3D)), unitName(s)), 10.f, dim, false, true);
        }
        // the state lamps and the warnings along the bottom
        {
            const F32 y = YM(880.f);
            F32 x = X(520.f);
            x += wordLamp(x, y, s.mAssist ? "FA ON" : "FA OFF", s.mAssist ? SC_CYAN : C_AMBER, true) + S(8.f);
            x += wordLamp(x, y, s.mLandingGear ? "GEAR DOWN" : "GEAR UP", s.mLandingGear ? C_GREEN : SC_CYAN, true) + S(8.f);
            x += wordLamp(x, y, s.mEngine ? "ENGINE" : "ENGINE OFF", s.mEngine ? SC_CYAN : C_AMBER, true) + S(8.f);
            if (s.mStages > 0 && !s.mRocket)
            {
                x += wordLamp(x, y, llformat("STAGE %d/%d", s.mStage, s.mStages), SC_CYAN, true) + S(8.f);
            }
            telltales(x - S(8.f), X(1090.f), y, s, false, true);
        }

        // ---- right: altitude tape, vertical speed, the buttons ----
        use(g.R);
        {
            const F32 tl = X(1170.f), tr = X(1258.f), tt = YM(620.f), tb = YM(882.f), cy = (tt + tb) * 0.5f;
            const F32 alt = s.mAltMSL, span = 200.f, ppm = (tt - tb) / span;
            rectF(tl, tb, tr, tt, glass);
            {
                LLLocalClipRect clip(WolfHudScale::clip(LLRect((S32)tl, (S32)tt, (S32)tr, (S32)tb)));
                // the ground (or water) under the craft, hatched amber
                const F32 gy = cy + ((alt - s.mAGL) - alt) * ppm;
                if (gy > tb)
                {
                    const F32 top_y = llmin(gy, tt);
                    for (F32 k = tb - (tr - tl); k < top_y; k += S(8.f))
                    {
                        lineW(tl, k, tr, llmin(k + (tr - tl), top_y), 1.f, alpha(C_AMBER, 0.6f));
                    }
                    rectF(tl, top_y - 1.f, tr, top_y + 1.f, C_AMBER);
                }
                for (S32 k = ((S32)(alt - span * 0.5f) / 10) * 10; k <= alt + span * 0.5f; k += 10)
                {
                    const F32 y = cy + (k - alt) * ppm;
                    lineW(tl, y, tl + S(10.f), y, 1.2f, SC_CYAN);
                    if (k % 50 == 0) T(tl + S(14.f), y, llformat("%d", k), 10.f, SC_CYAN, false, true, LLFontGL::LEFT);
                }
            }
            frameW(tl, tb, tr, tt, 1.f, dim);
            const F32 bh = S(13.f);
            rectF(tl, cy - bh, tr + S(4.f), cy + bh, LLColor4(0.f, 0.f, 0.f, 0.85f));
            frameW(tl, cy - bh, tr + S(4.f), cy + bh, 1.5f, SC_CYAN);
            T((tl + tr) * 0.5f + S(2.f), cy, llformat("%d", (S32)llround(alt)), 15.f, SC_CYAN, true, true);
            T((tl + tr) * 0.5f, YM(608.f), "ALT m", 9.f, SC_CYAN, true);
            // vertical speed: +/- 50 m/s full scale (the design's choice)
            const F32 vl = X(1266.f), vr = X(1284.f);
            rectF(vl, tb, vr, tt, glass);
            const F32 vs = llclamp(s.mVS / 50.f, -1.f, 1.f);
            rectF(vl, cy, vr, cy + (tt - cy) * vs, s.mVS >= 0.f ? SC_CYAN : C_AMBER);
            rectF(vl - S(3.f), cy - 0.75f, vr + S(3.f), cy + 0.75f, C_WHITE);
            frameW(vl, tb, vr, tt, 1.f, dim);
            T((vl + vr) * 0.5f, YM(608.f), "VS", 9.f, SC_CYAN, true);

            // the buttons: BOOST (held), ASSIST, GEAR, ENGINE
            struct SB { const char* label; bool lit; WolfDashboard::EHit id; std::string tip; };
            const SB sb[4] =
            {
                { "BOOST", (held & WolfDrive::BTN_BOOST) != 0, WolfDashboard::H_BOOST, "Boost: hold it" + keyHint(WolfDrive::ACT_BOOST) },
                { "ASSIST", s.mAssist, WolfDashboard::H_ASSIST, std::string(s.mAssist ? "Flight assist on: click to switch it off" : "Flight assist off: click to switch it on")
                                                               + keyHint(WolfDrive::ACT_ASSIST) },
                { "GEAR", s.mLandingGear, WolfDashboard::H_LANDING_GEAR, std::string(s.mLandingGear ? "Landing gear down: click to raise it" : "Landing gear up: click to lower it")
                                                                        + keyHint(WolfDrive::ACT_LANDING_GEAR) },
                { "ENGINE", s.mEngine, WolfDashboard::H_ENGINE, std::string(s.mEngine ? "Engine running: click to stop it" : "Engine stopped: click to start it")
                                                               + keyHint(WolfDrive::ACT_ENGINE) },
            };
            for (S32 i = 0; i < 4; ++i)
            {
                const F32 l = (i % 2) ? 1458.f : 1310.f, top = (i / 2) ? 680.f : 620.f;
                button(c, X(l), YM(top + 50.f), X(l + 128.f), YM(top), sb[i].label, sb[i].lit, sb[i].id, sb[i].tip, 13.f);
            }
            const F32 lx = X(1314.f);
            T(lx, YM(754.f), llformat("ABOVE GROUND  %d m", (S32)llround(s.mAGL)), 11.f, s.mAGL < 20.f && s.mVS < -2.f ? C_AMBER : SC_CYAN, false, true, LLFontGL::LEFT);
            T(lx, YM(778.f), llformat("ABOVE SEA     %d m", (S32)llround(s.mAltMSL)), 11.f, SC_CYAN, false, true, LLFontGL::LEFT);
            T(lx, YM(802.f), llformat("VERT SPEED    %+.1f m/s", s.mVS), 11.f, s.mVS < 0.f ? C_AMBER : SC_CYAN, false, true, LLFontGL::LEFT);
            T(lx, YM(826.f), llformat("SPEED         %d %s", (S32)llround(speedIn(s, s.mSpeed3D)), unitName(s)), 11.f, SC_CYAN, false, true, LLFontGL::LEFT);
            if (s.mVS > 0.f)
            {
                const F32 apo = s.mApogee;
                T(lx, YM(850.f), fabsf(apo) >= 10000.f ? llformat("APOGEE        %.2f km", apo / 1000.f) : llformat("APOGEE        %d m", (S32)llround(apo)),
                  11.f, SC_MAGENTA, false, true, LLFontGL::LEFT);
            }
        }
    }

    // ---- THE SUBMARINE HELM ----
    // Paul, 2026-10-11: "add another hud for driving a submarine and lsl commands". A sonar room in dark green and
    // teal, after a submarine's ship-control panel (depth gauge, ballast, planes, the depth under the keel) and
    // the sonar screens of games like Silent Hunter and Barotrauma: the DEPTH gauge on the left (below the water
    // surface, measured; red past the script's max_depth), the ballast tanks and the dive planes beside it; in the
    // middle the heading tape over a SONAR scope - a sweep that paints the objects and avatars round the
    // submarine (within SONAR_RANGE, from what the viewer already knows: gObjectList) and the land that rises
    // above the submarine's depth (the coastline ring) - with the sea bed under the hull, the dive angle, battery,
    // oxygen and the throttle beside it; the speed in knots and the helm's buttons on the right.
    const LLColor4 SUB_HI(0.020f, 0.090f, 0.080f, 1.f), SUB_LO(0.f, 0.030f, 0.030f, 1.f);
    const LLColor4 SUB_SCREEN(0.004f, 0.035f, 0.028f, 1.f);
    const LLColor4 SUB_LINE(0.10f, 0.32f, 0.26f, 1.f);
    const LLColor4 SUB_GREEN(0.35f, 1.00f, 0.55f, 1.f);

    // The sonar's picture, refreshed SONAR_PERIOD apart (cheap: the nearest SONAR_MAX_CONTACTS roots, the land
    // sampled on SONAR_BEARINGS bearings). Design choices, not figures from any real sonar.
    const F32 SONAR_RANGE = 100.f;          // m
    const F64 SONAR_PERIOD = 0.25;          // s
    const size_t SONAR_MAX_CONTACTS = 48;
    const S32 SONAR_BEARINGS = 36;
    const F32 SONAR_STEP = 5.f;             // m between the land samples along a bearing
    struct SonarContact { F32 east, north, size; bool avatar; };
    struct SonarPicture
    {
        F64 at = -1.0;
        std::vector<SonarContact> contacts;
        F32 coast[SONAR_BEARINGS];          // m to the first land above the hull's depth on that bearing; < 0 none
    } sSonar;

    LLViewerObject* vehicleRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting()) return nullptr;
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent()) p = (LLViewerObject*)p->getParent();
        return p;
    }

    void refreshSonar()
    {
        const F64 t = now();
        if (sSonar.at >= 0.0 && t - sSonar.at < SONAR_PERIOD) return;
        sSonar.at = t;
        sSonar.contacts.clear();
        for (F32& c : sSonar.coast) c = -1.f;
        LLViewerObject* self = vehicleRoot();
        if (!self) return;
        const LLVector3d here = self->getPositionGlobal();
        // objects and avatars: root prims only (a linkset is one contact), never the submarine itself
        std::vector<std::pair<F32, SonarContact>> found;
        const S32 n = gObjectList.getNumObjects();
        for (S32 i = 0; i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o == self || o->getParent() || o->isAttachment() || o->isHUDAttachment()) continue;
            const LLVector3d d = o->getPositionGlobal() - here;
            const F32 east = (F32)d.mdV[VX], north = (F32)d.mdV[VY];
            const F32 dist = sqrtf(east * east + north * north);
            if (dist > SONAR_RANGE || dist < 0.5f) continue;
            const LLVector3 sc = o->getScale();
            found.push_back({ dist, { east, north, llmax(sc.mV[VX], sc.mV[VY]), o->isAvatar() } });
        }
        std::sort(found.begin(), found.end(), [](const std::pair<F32, SonarContact>& a, const std::pair<F32, SonarContact>& b) { return a.first < b.first; });
        for (size_t i = 0; i < found.size() && i < SONAR_MAX_CONTACTS; ++i) sSonar.contacts.push_back(found[i].second);
        // the land: along each bearing, the first sample where the ground stands higher than the hull
        LLWorld* world = LLWorld::getInstance();
        const F32 z = (F32)here.mdV[VZ];
        for (S32 b = 0; b < SONAR_BEARINGS; ++b)
        {
            const F32 a = 2.f * PI_F * b / SONAR_BEARINGS;   // clockwise from north
            for (F32 r = SONAR_STEP; r <= SONAR_RANGE; r += SONAR_STEP)
            {
                LLVector3d p = here;
                p.mdV[VX] += sinf(a) * r;
                p.mdV[VY] += cosf(a) * r;
                if (world->resolveLandHeightGlobal(p) >= z)
                {
                    sSonar.coast[b] = r;
                    break;
                }
            }
        }
    }

    void drawSonar(F32 cx, F32 cy, F32 R, F32 heading)
    {
        refreshSonar();
        circleF(cx, cy, R, SUB_SCREEN, 64);
        for (S32 k = 1; k <= 4; ++k)
        {
            arcW(cx, cy, R * k / 4.f, 1.f, 0.f, 2.f * PI_F, alpha(SUB_GREEN, k == 4 ? 0.6f : 0.25f), 64);
        }
        lineW(cx - R, cy, cx + R, cy, 1.f, alpha(SUB_GREEN, 0.2f));
        lineW(cx, cy - R, cx, cy + R, 1.f, alpha(SUB_GREEN, 0.2f));
        // the sweep: one turn every 4 s, heading up; what it passed glows and fades over the turn
        const F32 sweep = (F32)fmod(now() * 90.0, 360.0);   // degrees clockwise from the top of the scope
        auto glow = [&](F32 deg) { const F32 behind = fmodf(sweep - deg + 720.f, 360.f); return 1.f - behind / 360.f; };
        auto at = [&](F32 east, F32 north, F32& x, F32& y, F32& deg)
        {
            // world bearing to scope angle: heading up
            const F32 brg = atan2f(east, north) * RAD_TO_DEG;
            deg = fmodf(brg - heading + 720.f, 360.f);
            const F32 r = sqrtf(east * east + north * north) / SONAR_RANGE * R;
            x = cx + r * sinf(deg * DEG_TO_RAD);
            y = cy + r * cosf(deg * DEG_TO_RAD);
        };
        // the coastline ring: land above the hull's depth
        for (S32 b = 0; b < SONAR_BEARINGS; ++b)
        {
            if (sSonar.coast[b] < 0.f) continue;
            const F32 a0 = 360.f * b / SONAR_BEARINGS - 5.f, a1 = a0 + 10.f;
            const F32 deg_mid = fmodf(360.f * b / SONAR_BEARINGS - heading + 720.f, 360.f);
            const F32 rr = sSonar.coast[b] / SONAR_RANGE * R;
            clockArc(cx, cy, rr, llmax(2.f, R * 0.03f), a0 - heading, a1 - heading, alpha(C_AMBER, 0.35f + 0.65f * glow(deg_mid)), 6);
        }
        for (const SonarContact& k : sSonar.contacts)
        {
            F32 x, y, deg;
            at(k.east, k.north, x, y, deg);
            const F32 g = glow(deg);
            const F32 dot = llclamp(k.size / SONAR_RANGE * R, S(2.5f), S(7.f));
            circleF(x, y, dot, alpha(k.avatar ? C_WHITE : SUB_GREEN, 0.25f + 0.75f * g), 12);
        }
        // the sweep's beam
        F32 sx, sy;
        pt(cx, cy, R, sweep, sx, sy);
        lineW(cx, cy, sx, sy, 2.f, SUB_GREEN);
        for (S32 i = 1; i <= 6; ++i)
        {
            F32 tx, ty;
            pt(cx, cy, R, sweep - i * 4.f, tx, ty);
            lineW(cx, cy, tx, ty, 2.f, alpha(SUB_GREEN, 0.5f - i * 0.07f));
        }
        // the submarine at the centre, bow up
        triF(cx, cy + S(8.f), cx - S(4.f), cy - S(6.f), cx + S(4.f), cy - S(6.f), C_WHITE);
        T(cx, cy - R - S(8.f), llformat("SONAR  %d m", (S32)SONAR_RANGE), 9.f, alpha(SUB_GREEN, 0.8f), true);
    }

    void drawSubmarine(Ctx& c, const WolfDrive::Shown& s, bool bg, U32 held)
    {
        WolfDrive& d = WolfDrive::instance();
        if (bg)
        {
            rectG(0.f, g.bandB, g.W, g.panelTop, SUB_HI, SUB_LO);
            rectF(0.f, g.panelTop - 2.f, g.W, g.panelTop, C_TEAL);
        }

        // ---- left: the depth gauge, ballast and planes ----
        use(g.L);
        {
            // the scale: a quarter past the safe depth when the script gives one (rounded up to 10 m), else 100 m
            // (MODEL); red beyond max_depth
            const F32 safe = s.mHasMaxDepth ? (F32)s.mMaxDepth : 0.f;
            const F32 scale = s.mHasMaxDepth ? llmax(10.f, ceilf(safe * 1.25f / 10.f) * 10.f) : 100.f;
            const F32 major = scale > 500.f ? 100.f : scale > 200.f ? 50.f : scale > 50.f ? 10.f : 5.f;
            const bool too_deep = s.mHasMaxDepth && s.mSubDepth > safe;
            Dial depth{ X(190.f), YM(762.f), S(118.f), scale, major, major * 2.f, major / 2.f, 1.f, safe, 0.f, 0.f,
                        llmax(0.f, s.mSubDepth), 0.f, "DEPTH m", false, too_deep ? C_RED : C_TEAL, S(16.f) };
            depth.sea = true;
            drawDial(depth);
            T(X(190.f), YM(824.f), s.mSubDepth <= 0.f ? std::string("SURFACED") : llformat("%.1f m", s.mSubDepth), 14.f,
              too_deep ? C_RED : C_WHITE, true, true);
            const F32 rate = s.mDepthRate;
            T(X(190.f), YM(846.f), llformat("%s %.1f m/min", rate > 0.5f ? "DOWN" : rate < -0.5f ? "UP" : "LEVEL", fabsf(rate)), 10.f,
              fabsf(rate) > 0.5f ? SUB_GREEN : C_GREY, false, true);
        }
        {
            // the ballast tanks (flooded, from the top) and the dive planes (+ nose down)
            const F32 top = YM(628.f), bot = YM(846.f);
            const F32 bl = X(330.f), br = X(370.f);
            rectF(bl, bot, br, top, SUB_SCREEN);
            if (s.mHasBallast)
            {
                rectF(bl, top - (top - bot) * s.mBallast / 100.f, br, top, alpha(C_BLUE, 0.85f));
            }
            frameW(bl, bot, br, top, 1.f, SUB_LINE);
            T((bl + br) * 0.5f, YM(616.f), "BALLAST", 8.f, C_TEAL, true);
            T((bl + br) * 0.5f, YM(860.f), s.mHasBallast ? llformat("%d%%", s.mBallast) : std::string("--"), 10.f, C_WHITE, true, true);
            const F32 pl = X(400.f), pr = X(440.f), mid = (top + bot) * 0.5f;
            rectF(pl, bot, pr, top, SUB_SCREEN);
            for (S32 k = -2; k <= 2; ++k) rectF(pl, mid + (top - mid) * k / 2.f, pl + S(8.f), mid + (top - mid) * k / 2.f + 1.f, SUB_LINE);
            if (s.mHasPlanes)
            {
                const F32 y = mid - (top - mid) * s.mPlanes / 100.f;   // nose down drawn downward
                rectF(pl, llmin(mid, y), pr, llmax(mid, y), s.mPlanes > 0 ? alpha(C_AMBER, 0.8f) : alpha(SUB_GREEN, 0.8f));
            }
            rectF(pl - S(3.f), mid - 0.75f, pr + S(3.f), mid + 0.75f, C_WHITE);
            frameW(pl, bot, pr, top, 1.f, SUB_LINE);
            T((pl + pr) * 0.5f, YM(616.f), "PLANES", 8.f, C_TEAL, true);
            T((pl + pr) * 0.5f, YM(860.f), s.mHasPlanes ? llformat("%+d", s.mPlanes) : std::string("--"), 10.f, C_WHITE, true, true);
            T((pl + pr) * 0.5f, YM(878.f), "+ = DIVE", 7.f, C_GREY);
        }

        // ---- centre: heading, sonar, the boat's state ----
        use(g.C);
        headingTape(X(480.f), X(1120.f), YM(640.f), YM(606.f), s.mHeading, s.mTrack, s.mSpeed > 0.5f, SUB_GREEN, C_TEAL, C_TEAL,
                    alpha(SUB_SCREEN, 0.9f));
        T(X(800.f), YM(652.f), llformat("%03d", (S32)llround(s.mHeading) % 360), 12.f, C_WHITE, true, true);
        drawSonar(X(800.f), YM(774.f), S(104.f), s.mHeading);
        {
            auto box = [&](F32 t0, F32 t1, const LLColor4& fill, const LLColor4& edge)
            {
                roundRectF(X(482.f), YM(t1), X(676.f), YM(t0), S(8.f), fill, fill);
                frameW(X(482.f), YM(t1), X(676.f), YM(t0), 1.f, edge);
            };
            // the sea bed under the hull: amber under 5 m, red under 2 m
            const F32 bed = s.mSeabed;
            const LLColor4 bc = bed < 2.f ? C_RED : bed < 5.f ? C_AMBER : SUB_LINE;
            box(662.f, 728.f, bed < 5.f ? alpha(bc, bed < 2.f && blink(0.6f) ? 0.55f : 0.28f) : SUB_SCREEN, bc);
            T(X(579.f), YM(676.f), "SEA BED BELOW", 9.f, bed < 5.f ? C_WHITE : C_TEAL, true);
            T(X(579.f), YM(706.f), bed <= 0.f ? std::string("ON THE BOTTOM") : llformat("%.1f m", bed), bed <= 0.f ? 13.f : 20.f, C_WHITE, true, true);
            box(736.f, 802.f, SUB_SCREEN, SUB_LINE);
            T(X(579.f), YM(750.f), "DIVE ANGLE", 9.f, C_TEAL, true);
            T(X(579.f), YM(780.f), llformat("%+.1f", -s.mPitch), 20.f, fabsf(s.mPitch) > 20.f ? C_AMBER : C_WHITE, true, true);
            T(X(660.f), YM(780.f), -s.mPitch > 0.5f ? "DN" : s.mPitch > 0.5f ? "UP" : "", 9.f, C_GREY, true, false, LLFontGL::RIGHT);
            telltales(X(470.f), X(690.f), YM(830.f), s, false, true);
            T(X(579.f), YM(868.f), s.mPeriscope ? "PERISCOPE UP" : "PERISCOPE DOWN", 10.f, s.mPeriscope ? C_AMBER : C_GREY, true);
        }
        {
            const LLColor4 track = alpha(SUB_SCREEN, 1.f);
            auto low_bad = [](F32 v) { return v < 10.f ? C_RED : v < 25.f ? C_AMBER : SUB_GREEN; };
            percentBar(X(924.f), X(1118.f), YM(668.f), YM(698.f), YM(680.f), "BATTERY", s.mHasBattery, s.mBattery, low_bad(s.mBattery), C_TEAL, track);
            percentBar(X(924.f), X(1118.f), YM(716.f), YM(746.f), YM(728.f), "OXYGEN", s.mHasOxygen, s.mOxygen, low_bad(s.mOxygen), C_TEAL, track);
            // the throttle: ahead to the right, astern to the left
            const F32 tl = X(924.f), tr = X(1118.f), mid = (tl + tr) * 0.5f, tb = YM(796.f), tt = YM(778.f);
            const F32 thr = llclamp(s.mThrottlePct, -100.f, 100.f);
            T(tl, YM(766.f), "THROTTLE", 10.f, C_TEAL, true, false, LLFontGL::LEFT);
            T(tr, YM(766.f), thr > 0.f ? llformat("AHEAD %d%%", (S32)llround(thr)) : thr < 0.f ? llformat("ASTERN %d%%", (S32)llround(-thr)) : std::string("STOP"),
              10.f, thr < 0.f ? C_AMBER : C_WHITE, true, true, LLFontGL::RIGHT);
            rectF(tl, tb, tr, tt, track);
            rectF(llmin(mid, mid + (tr - mid) * thr / 100.f), tb, llmax(mid, mid + (tr - mid) * thr / 100.f), tt, thr < 0.f ? C_AMBER : SUB_GREEN);
            rectF(mid - 0.75f, tb - S(3.f), mid + 0.75f, tt + S(3.f), C_WHITE);
            frameW(tl, tb, tr, tt, 1.f, SUB_LINE);
            button(c, X(924.f), YM(838.f), X(1018.f), YM(806.f), "AHEAD", d.throttle() > 0.f, WolfDashboard::H_THROTTLE,
                   "Ahead: hold it (or Up / W, or your accelerator)", 10.f);
            button(c, X(1024.f), YM(838.f), X(1118.f), YM(806.f), "ASTERN", d.brake() > 0.f, WolfDashboard::H_BRAKE,
                   "Astern: hold it (or Down / S, or your brake)", 10.f);
            T(X(1021.f), YM(860.f), "Page Up / Page Down: up / down", 8.f, C_GREY);
        }

        // ---- right: speed, the buttons ----
        use(g.R);
        {
            const F32 spd = speedIn(s, s.mSpeed3D);
            const F32 maxs = llmax(10.f, s.mMaxSpeed);
            Dial speedo{ X(1278.f), YM(762.f), S(108.f), maxs, maxs > 100.f ? 20.f : maxs > 40.f ? 10.f : 5.f, 0.f, maxs > 40.f ? 5.f : 1.f, 1.f,
                         0.f, 0.f, 0.f, spd, 0.f, unitName(s), false, C_TEAL, S(16.f) };
            speedo.sea = true;
            drawDial(speedo);
            T(X(1278.f), YM(822.f), llformat("%.1f", spd), 16.f, C_WHITE, true, true);
            struct SB { const char* label; bool lit; WolfDashboard::EHit id; std::string tip; };
            const SB sb[6] =
            {
                { "DIVE", (held & WolfDrive::BTN_DIVE) != 0, WolfDashboard::H_DIVE, "Dive: hold it - flood the ballast and go down" + keyHint(WolfDrive::ACT_DIVE) },
                { "SURFACE", (held & WolfDrive::BTN_SURFACE) != 0, WolfDashboard::H_SURFACE, "Surface: hold it - blow the ballast and go up" + keyHint(WolfDrive::ACT_SURFACE) },
                { "PERISCOPE", s.mPeriscope, WolfDashboard::H_PERISCOPE, std::string(s.mPeriscope ? "Periscope up: click to lower it" : "Periscope down: click to raise it")
                                                                         + keyHint(WolfDrive::ACT_PERISCOPE) },
                { "PING", (held & WolfDrive::BTN_SONAR) != 0, WolfDashboard::H_SONAR, "Sonar ping (sent to the submarine's script)" + keyHint(WolfDrive::ACT_SONAR) },
                { s.mLights == 2 ? "MAIN" : "LIGHTS", s.mLights > 0, WolfDashboard::H_LIGHTS, "Lights: off, on, bright" + keyHint(WolfDrive::ACT_LIGHTS) },
                { "ENGINE", s.mEngine, WolfDashboard::H_ENGINE, std::string(s.mEngine ? "Motor running: click to stop it" : "Motor stopped: click to start it")
                                                               + keyHint(WolfDrive::ACT_ENGINE) },
            };
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 l = (i % 2) ? 1504.f : 1406.f, top = 618.f + (i / 2) * 50.f;
                button(c, X(l), YM(top + 42.f), X(l + 94.f), YM(top), sb[i].label, sb[i].lit, sb[i].id, sb[i].tip, 11.f);
            }
            T(X(1503.f), YM(784.f), s.mHasMaxDepth ? llformat("SAFE DEPTH %d m", s.mMaxDepth) : std::string("SAFE DEPTH --"), 10.f, C_TEAL, true, true);
            T(X(1503.f), YM(808.f), llformat("BELOW SURFACE %.1f m", llmax(0.f, s.mSubDepth)), 10.f, C_WHITE, false, true);
            T(X(1503.f), YM(832.f), llformat("SEA BED %.1f m", s.mSeabed), 10.f, s.mSeabed < 2.f ? C_RED : s.mSeabed < 5.f ? C_AMBER : C_WHITE, false, true);
        }
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
        { 3, "" },
        // [WOLF RETARDER 2026-10-12] the truck's stalk (wolfdrive.cpp has the sources)
        { 0, "TRUCK RETARDER (the stalk beside the wheel)" },
        { 1, "0 off, 1-5 five fixed steps of braking (500 to 3000 Nm): drag it," },
        { 2, "click, scroll over it, or ] / [. It brakes when you lift off the" },
        { 2, "accelerator; blue figures show the stage braking now." },
        { 1, "DSC holds the speed it was switched on at, downhill; - / + (or , / .)" },
        { 2, "change it, ; switches it. AUT: the brake pedal uses it first." },
        { 1, "RET lamp: blue braking, amber flashing when held back by heat." },
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
        { 2, "(the truck's retarder: buttons bits 19 up - PROTOCOL.md)" },   // [WOLF RETARDER 2026-10-12/]
        { 1, "wolfDashboard(driver, [...]) drives this dashboard:" },
        { 4, "wolfDashboard(llAvatarOnSitTarget(), [\"type\",\"car\"," },
        { 4, "    \"rpm\", 3200, \"gear\", \"3\", \"fuel\", 74]);" },
        { 1, "Both only work for a script in the vehicle the driver sits on." },
        { 2, "Other viewers send keys only: keep a control() event as well." },
        { 3, "" },
        { 1, "Click anywhere above the dashboard to close.  HELP reopens it." },
    };
    // <WolfViewer 2026-10-11> w65: the speedboat helm's and the spaceship HUD's own guides
    const HelpLine HELP_BOAT[] =
    {
        { 0, "THE SPEEDBOAT HELM" },
        { 1, "Opens by itself when a boat's script asks for it, or from" },
        { 2, "Wolf World > Speedboat Helm, or its icon beside the boat icon." },
        { 1, "Left: the rev counter (green: the engine's full-throttle band)." },
        { 1, "Right: the speed in knots (KNOTS above: km/h, mph)." },
        { 1, "Centre: compass and heading (COG: where you are going)," },
        { 2, "the speed step, ON PLANE, the depth under you (red under 2 m)," },
        { 2, "trim (or the bow's angle), fuel and warnings." },
        { 1, "Throttle lever: F ahead, N, R astern. AHEAD / ASTERN: hold." },
        { 3, "" },
        { 0, "SPEED STEPS" },
        { 1, "Page Up / + a step up, Page Down / - a step down." },
        { 1, "A boat that says \"Speed = 3\" aloud shows its own step here," },
        { 2, "marked BOAT." },
        { 1, "HORN: the air horn, heard by everyone near (Wolf only)." },
        { 1, "LIGHTS, ANCHOR, ENGINE: sent to the boat's script." },
    };
    const HelpLine HELP_BOAT_MAKERS[] =
    {
        { 0, "YOUR KEYS" },
        { 1, "CONTROLS (above) opens Driving Controls: a key, a controller" },
        { 2, "button and what it sends, for the horn, lights, anchor (N)" },
        { 2, "and engine (I). Reset puts them back." },
        { 3, "" },
        { 0, "FOR BOAT MAKERS" },
        { 1, "wolfDashboard(driver, [...]) drives this helm:" },
        { 4, "wolfDashboard(llAvatarOnSitTarget(), [\"type\",\"speedboat\"," },
        { 4, "    \"rpm\", 4200, \"gear\", \"3\", \"throttle\", 60, \"trim\", 2]);" },
        { 1, "Also: anchor, engine, fuel, units (kn kmh mph), warnings." },
        { 1, "wolfDriveInput(driver) gives the buttons held: 4 horn," },
        { 2, "8 lights, 2048 anchor, 16384 engine." },
        { 1, "With no script the helm reads the boat's own speed, heading," },
        { 2, "bow angle and the depth under it." },
        { 3, "" },
        { 1, "Click anywhere above the helm to close.  HELP reopens it." },
    };
    const HelpLine HELP_SHIP[] =
    {
        { 0, "THE SPACESHIP HUD" },
        { 1, "Opens by itself when a craft's script asks for it, or from" },
        { 2, "Wolf World > Spaceship HUD, or its icon beside the plane icon." },
        { 1, "Top: heading. Centre: the pitch ladder; the W is your nose," },
        { 2, "the circle is where you are really going (flight path)." },
        { 1, "Left: speed and throttle; fuel, boost, shield, hull, heat." },
        { 1, "Right: altitude above the water (the ground under it hatched)," },
        { 2, "vertical speed and the height above the ground." },
        { 1, "Cyan normal, amber caution, red warning, magenta target." },
        { 3, "" },
        { 0, "ROCKET MODE" },
        { 1, "A rocket's script switches to the launch layout: altitude," },
        { 2, "vertical speed, apogee (how high it will coast), stage N/M" },
        { 2, "and SEPARATE, which drops the spent stage." },
        { 1, "You fly with the craft's own keys; the HUD's buttons go to" },
        { 2, "its script: STAGE, BOOST (held), ASSIST, GEAR, ENGINE." },
    };
    const HelpLine HELP_SHIP_MAKERS[] =
    {
        { 0, "YOUR KEYS" },
        { 1, "CONTROLS (above) opens Driving Controls: J stage, Y boost," },
        { 2, "O flight assist, G landing gear, I engine - or your own keys" },
        { 2, "or controller buttons." },
        { 3, "" },
        { 0, "FOR CRAFT MAKERS" },
        { 1, "wolfDashboard(driver, [...]) drives this HUD:" },
        { 4, "wolfDashboard(pilot, [\"type\",\"spaceship\",\"mode\",\"rocket\"," },
        { 4, "    \"stage\", 1, \"stages\", 3, \"throttle\", 100]);" },
        { 1, "Also: fuel, boost, shield, hull, heat, assist, landing_gear," },
        { 2, "engine, warnings." },
        { 1, "wolfDriveInput(driver) buttons held: 512 stage, 1024 boost," },
        { 2, "4096 assist, 8192 landing gear, 16384 engine." },
        { 1, "Speed, attitude and heights come from the craft itself." },
        { 3, "" },
        { 1, "Click anywhere above the HUD to close.  HELP reopens it." },
    };
}

void WolfDashboard::drawHelp(F32 top)
{
    // <WolfViewer 2026-10-11> the submarine helm's guide (Paul: "add another hud for driving a submarine and lsl commands")
    static const HelpLine HELP_SUB[] =
    {
        { 0, "THE SUBMARINE HELM" },
        { 1, "Opens by itself when a submarine's script asks for it, or from" },
        { 2, "Wolf World > Submarine Helm, or its icon beside the speedboat's." },
        { 1, "Left: DEPTH below the surface (red past the safe depth), how" },
        { 2, "fast it changes, the ballast tanks and the dive planes." },
        { 1, "Centre: heading and the SONAR - objects (green), avatars" },
        { 2, "(white) and land higher than you (amber) within 100 m." },
        { 1, "SEA BED BELOW: amber under 5 m, red under 2 m." },
        { 1, "Right: speed in knots (KNOTS above: km/h, mph)." },
        { 3, "" },
        { 0, "DRIVING" },
        { 1, "Keys as always: Up / W ahead, Down / S astern, Left / Right," },
        { 2, "Page Up / Page Down up and down - any submarine works." },
        { 1, "DIVE and SURFACE: hold. PERISCOPE, PING, LIGHTS, ENGINE:" },
        { 2, "sent to the submarine's script (PING also pings here)." },
    };
    static const HelpLine HELP_SUB_MAKERS[] =
    {
        { 0, "YOUR KEYS" },
        { 1, "CONTROLS (above) opens Driving Controls: M dive, U surface," },
        { 2, "P periscope, Q sonar - or your own keys or controller buttons." },
        { 3, "" },
        { 0, "FOR SUBMARINE MAKERS" },
        { 1, "wolfDashboard(driver, [...]) drives this helm:" },
        { 4, "wolfDashboard(llAvatarOnSitTarget(), [\"type\",\"submarine\"," },
        { 4, "    \"ballast\", 60, \"max_depth\", 120, \"planes\", 20]);" },
        { 1, "Also: oxygen, battery, periscope, throttle, engine, lights," },
        { 2, "units, warnings (oxygen, battery, leak ...)." },
        { 1, "wolfDriveInput(driver) buttons held: 32768 dive, 65536" },
        { 2, "surface, 131072 periscope, 262144 sonar." },
        { 1, "The depth, sea bed and sonar come from the viewer itself." },
        { 3, "" },
        { 1, "Click anywhere above the helm to close.  HELP reopens it." },
    };
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
    // <WolfViewer 2026-10-11> each design's own pair of columns (the road designs share theirs)
    const WolfDrive::EType ty = WolfDrive::instance().type();
    const HelpLine* left = HELP_DRIVE;
    const HelpLine* right = HELP_SETUP;
    S32 n1 = (S32)(sizeof(HELP_DRIVE) / sizeof(HELP_DRIVE[0]));
    S32 n2 = (S32)(sizeof(HELP_SETUP) / sizeof(HELP_SETUP[0]));
    if (ty == WolfDrive::TYPE_SPEEDBOAT)
    {
        left = HELP_BOAT;   n1 = (S32)(sizeof(HELP_BOAT) / sizeof(HELP_BOAT[0]));
        right = HELP_BOAT_MAKERS; n2 = (S32)(sizeof(HELP_BOAT_MAKERS) / sizeof(HELP_BOAT_MAKERS[0]));
    }
    else if (ty == WolfDrive::TYPE_SUBMARINE)
    {
        left = HELP_SUB;    n1 = (S32)(sizeof(HELP_SUB) / sizeof(HELP_SUB[0]));
        right = HELP_SUB_MAKERS; n2 = (S32)(sizeof(HELP_SUB_MAKERS) / sizeof(HELP_SUB_MAKERS[0]));
    }
    else if (ty == WolfDrive::TYPE_SPACESHIP)
    {
        left = HELP_SHIP;   n1 = (S32)(sizeof(HELP_SHIP) / sizeof(HELP_SHIP[0]));
        right = HELP_SHIP_MAKERS; n2 = (S32)(sizeof(HELP_SHIP_MAKERS) / sizeof(HELP_SHIP_MAKERS[0]));
    }
    const S32 rows = llmax(n1, n2) + 2;
    const F32 lh = llmin((ch - 20.f) / rows, 26.f);
    const LLFontGL* head = face("B612", LLFontGL::BOLD, lh * 0.95f);
    const LLFontGL* body = face("B612", LLFontGL::NORMAL, lh * 0.82f);
    const LLFontGL* code = face("B612Mono", LLFontGL::NORMAL, lh * 0.8f);
    static const char* const TITLES[WolfDrive::TYPE_COUNT] = { "CAR DASHBOARD", "TRUCK DASHBOARD", "BIKE DASHBOARD", "SPEEDBOAT HELM", "SPACESHIP HUD", "SUBMARINE HELM" };
    text(head, TITLES[llclamp((S32)ty, 0, (S32)WolfDrive::TYPE_COUNT - 1)], (l + r) * 0.5f, t - lh * 0.8f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
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
    column(left, n1, l + cw * 0.03f);
    column(right, n2, l + cw * 0.52f);
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
        mResize.hide();   // [HUD SIZE 2026-10-12/]
        return;
    }
    const WolfDrive::EType ty = d.type();
    // The guide opens by itself the first time each design is used (after the flight deck's).
    if ((S32)ty != mHelpCheckedType)
    {
        mHelpCheckedType = (S32)ty;
        static const char* const SEEN[WolfDrive::TYPE_COUNT] = { "WolfDashboardHelpCar", "WolfDashboardHelpTruck", "WolfDashboardHelpBike",
                                                                 "WolfDashboardHelpSpeedboat", "WolfDashboardHelpSpaceship",
                                                                 "WolfDashboardHelpSubmarine" };   // <WolfViewer 2026-10-11/>
        const char* setting = SEEN[llclamp((S32)ty, 0, (S32)WolfDrive::TYPE_COUNT - 1)];
        if (!gSavedSettings.getBOOL(setting))
        {
            gSavedSettings.setBOOL(setting, true);
            mShowHelp = true;
        }
    }

    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    // Source: wolfflightdeck.cpp layoutAndDraw - sit on whatever the bottom toolbar leaves.
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
    // <WolfViewer 2026-10-10> Paul: "the huds allow them to be minimised to just minimal stuff and brought back up
    // again like a mini version of them" - MINI on the strip: one bar, FULL on it brings the dashboard back.
    if (gSavedSettings.getBOOL("WolfDashboardMini"))
    {
        LLGLSUIDefault gls_ui;
        mHitScale = 1.f;      // [HUD SIZE 2026-10-12] the bar keeps its own size, and has no size handle
        mResize.hide();
        drawMiniBar(screen_bottom);
        mDrawn = true;
        return;
    }
    // </WolfViewer>
    // [HUD SIZE 2026-10-12] Paul: "the huds are all much too big ... the user should be able to drag the top of the
    // huds (apart from gaming) down to reduce its size on the screen". Everything below is laid out as it always was
    // (its height from the window's, in pixels) in a picture as wide as the window over `zoom`, and drawn at `zoom`
    // (WolfHudScale::Scope): the same dashboard, smaller, still across the window and on the toolbar.
    const F32 zoom = mResize.scale();
    const F32 W = screen_w / zoom;
    const F32 bottom = screen_bottom / zoom;
    // The panel is the mockup's 304 units tall. Its size is chosen in pixels (30% of the window,
    // 170 to 380) and turned into UI units with the UI scale, so a bigger UI makes it bigger; it
    // never takes more than 55% of what the toolbar leaves.
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    const F32 want_h = llclamp(Hv * ui * 0.30f, 170.f, 380.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.55f);
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
        const F32 row_h = llmin(want_h, (Hv - screen_bottom) * 0.30f);
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
    mPanelB = screen_bottom;
    mPanelT = g.panelTop * zoom;   // the strip's top is added once it is laid out
    mDrawn = true;


    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;   // [HUD SIZE 2026-10-12] the dashboard itself, to the strip's top
    zoomed.emplace(zoom);
    const WolfDrive::Shown& s = d.shown();
    const bool bg = gSavedSettings.getBOOL("WolfDashboardBackground");
    Ctx c{ this, [this](F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip) { addHit(l, b, r, t, id, tip); }, mPressed };
    const F32 kmh = speedIn(s, s.mSpeed);   // <WolfViewer 2026-10-11/> in the shown units (the name is the road designs')

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
                   unitName(s), false, C_NEEDLE, S(13.f) });
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
        gearShifter(c, X(1175.f), X(1267.f), YM(632.f), YM(832.f), ty);   // [GEAR STICK 2026-10-10]
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
        const F32 gov = sp.mGovernorKmh > 0.f ? speedIn(s, sp.mGovernorKmh / KMH) : 0.f;   // <WolfViewer 2026-10-11/> any units
        drawDial({ X(990.f), cy, S(98.f), s.mMaxSpeed, s.mMaxSpeed > 100.f ? 20.f : 10.f, 0.f, 5.f, 1.f, 0.f, 0.f, 0.f, kmh, gov,
                   unitName(s), true, TRUCK_NEEDLE, S(17.f) });
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
        // [WOLF RETARDER 2026-10-12] the stages braking now filled blue (the script's), the stalk's own position
        // framed white; with downhill speed control on, its set speed in place of the word
        if (d.retarderDsc())
        {
            T(dl + S(14.f), YM(798.f), s.mHasDscSpeed ? llformat("DSC %d", (S32)llround(speedIn(s, s.mDscSpeed / KMH))) : std::string("DSC"),
              10.f, C_TEAL, true, false, LLFontGL::LEFT);
        }
        else
        {
            T(dl + S(14.f), YM(798.f), "RETARDER", 10.f, C_GREY, false, false, LLFontGL::LEFT);
        }
        for (S32 i = 0; i < 5; ++i)
        {
            const F32 x = X(740.f + i * 24.f);
            roundRectF(x, YM(804.f), x + S(18.f), YM(792.f), S(2.f), i < s.mRetarder ? C_BLUE : C_OFF, i < s.mRetarder ? C_BLUE : C_OFF);
            if (i + 1 == d.retarderStalk())
            {
                frameW(x - 1.5f, YM(804.f) - 1.5f, x + S(18.f) + 1.5f, YM(792.f) + 1.5f, 1.2f, C_WHITE);
            }
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
        // [WOLF RETARDER 2026-10-12] the retarder stalk beside the wheel: the strip of the left anchor between the
        // wheel's hit (to X 340) and the cluster (the centre anchor, from its X 400), free in both layouts
        retarderStalk(c, X(346.f), X(396.f), YM(606.f), YM(890.f), s);
        use(g.R);
        gearShifter(c, X(1255.f), X(1347.f), YM(632.f), YM(812.f), ty);   // [GEAR STICK 2026-10-10]
        pedals(c, X(1378.f), X(1580.f), YM(666.f), 172.f, d.brake(), d.throttle(), false);
        lampButtons(c, X(1255.f), X(1580.f), YM(858.f), s, d.buttonsHeld());
    }
    else if (ty == WolfDrive::TYPE_SPEEDBOAT)
    {
        drawSpeedboat(c, s, bg, d.buttonsHeld());   // <WolfViewer 2026-10-11/> w65
    }
    else if (ty == WolfDrive::TYPE_SPACESHIP)
    {
        drawSpaceship(c, s, bg, d.buttonsHeld());   // <WolfViewer 2026-10-11/> w65
    }
    else if (ty == WolfDrive::TYPE_SUBMARINE)
    {
        drawSubmarine(c, s, bg, d.buttonsHeld());   // <WolfViewer 2026-10-11/> w65
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
        T(X(642.f), YM(796.f), unitName(s), 12.f, C_GREY);
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
    mPanelT = strip(c, s, ty, d.vehicleName()) * zoom;
    use(g.C);
    // [HUD SIZE 2026-10-12] back to the view's own size: the clicks are mapped by mHitScale, the size handle and the
    // guide (over the world above) are drawn as they are
    zoomed.reset();
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mResize.draw();

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
    // [GEAR STICK 2026-10-10] the gate's slots, the truck's range collar and splitter
    case H_RANGE:
    {
        const S32 cur = d.selector() >= 1 ? d.selector() : 1;
        d.setSelector(cur <= 6 ? llmin(cur + 6, WolfDrive::spec(d.type()).mGears) : cur - 6);
        break;
    }
    case H_SPLIT:
    {
        const S32 cur = d.selector() >= 1 ? d.selector() : 1;
        d.setSelector(((cur - 1) % 2) ? cur - 1 : llmin(cur + 1, WolfDrive::spec(d.type()).mGears));
        break;
    }
    case H_TYPE_TRUCK: d.chooseType(WolfDrive::TYPE_TRUCK); break;
    case H_TYPE_BIKE:  d.chooseType(WolfDrive::TYPE_BIKE); break;
    case H_UNITS:
        if (d.type() == WolfDrive::TYPE_SPEEDBOAT || d.type() == WolfDrive::TYPE_SUBMARINE)
        {
            // <WolfViewer 2026-10-11> the speedboat and submarine: knots, km/h, mph, round again
            gSavedSettings.setS32("WolfDashboardBoatUnits", (llclamp(gSavedSettings.getS32("WolfDashboardBoatUnits"), 0, 2) + 1) % 3);
        }
        else
        {
            gSavedSettings.setBOOL("WolfDashboardMph", !gSavedSettings.getBOOL("WolfDashboardMph"));
        }
        break;
    // <WolfViewer 2026-10-11> w65: the new buttons (held while pressed, like the lamps; BOOST is the one a script
    // reads as held)
    case H_STAGE:          d.setScreenButton(WolfDrive::BTN_STAGE, true); break;
    case H_BOOST:          d.setScreenButton(WolfDrive::BTN_BOOST, true); break;
    case H_ANCHOR:         d.setScreenButton(WolfDrive::BTN_ANCHOR, true); break;
    case H_ASSIST:         d.setScreenButton(WolfDrive::BTN_ASSIST, true); break;
    case H_LANDING_GEAR:   d.setScreenButton(WolfDrive::BTN_LANDING_GEAR, true); break;
    case H_ENGINE:         d.setScreenButton(WolfDrive::BTN_ENGINE, true); break;
    case H_DIVE:           d.setScreenButton(WolfDrive::BTN_DIVE, true); break;
    case H_SURFACE:        d.setScreenButton(WolfDrive::BTN_SURFACE, true); break;
    case H_PERISCOPE:      d.setScreenButton(WolfDrive::BTN_PERISCOPE, true); break;
    case H_SONAR:          d.setScreenButton(WolfDrive::BTN_SONAR, true); break;
    // [WOLF RETARDER 2026-10-12] the switches through the actions' own edge (as a key or a wheel button would, so
    // what the driver set them to send goes too); + / - held while pressed; a stalk position is set straight
    case H_RET_AUT:        d.pressAction(WolfDrive::ACT_RETARDER_AUT); break;
    case H_RET_DSC:        d.pressAction(WolfDrive::ACT_RETARDER_DSC); break;
    case H_RET_DSC_UP:     d.setScreenButton(1u << WolfDrive::ACT_RETARDER_DSC_UP, true); break;
    case H_RET_DSC_DOWN:   d.setScreenButton(1u << WolfDrive::ACT_RETARDER_DSC_DOWN, true); break;
    case H_RET_0: case H_RET_1: case H_RET_2: case H_RET_3: case H_RET_4: case H_RET_5:
        if (WolfDrive::onWolf()) d.setRetarderStalk(id - H_RET_0);   // [WOLF ONLY]
        break;
    case H_PANEL:      gSavedSettings.setBOOL("WolfDashboardBackground", !gSavedSettings.getBOOL("WolfDashboardBackground")); break;
    case H_CONTROLS:   LLFloaterReg::toggleInstanceOrBringToFront("wolf_drive_controls"); break;
    case H_TRIP:       d.resetTrip(); break;
    case H_CLOSE:      d.requestDashboard(false); break;
    case H_MINI:       gSavedSettings.setBOOL("WolfDashboardMini", !gSavedSettings.getBOOL("WolfDashboardMini")); break;   // <WolfViewer 2026-10-10/>
    case H_HELP:       mShowHelp = !mShowHelp; break;
    case H_INVENTORY:  WolfHud::toggleInventory(); break;   // [HUD INVENTORY 2026-10-12/]
    case H_REZ:        WolfVehicleRez::rez(WolfVehicleRez::typeForDesign(d.type())); break;   // [HUD REZ 2026-10-12/]
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
        if (id >= H_GATE_0 && id <= H_GATE_13)
        {
            const S32 i = id - H_GATE_0;
            d.setSelector(i == 0 ? WolfDrive::GEAR_R : i == 1 ? WolfDrive::GEAR_N : i - 1);
            break;
        }
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
                      | WolfDrive::BTN_HANDBRAKE | WolfDrive::BTN_HORN | WolfDrive::BTN_HAZARDS | WolfDrive::BTN_CRUISE
                      | WolfDrive::BTN_STAGE | WolfDrive::BTN_BOOST | WolfDrive::BTN_ANCHOR | WolfDrive::BTN_ASSIST   // <WolfViewer 2026-10-11/>
                      | WolfDrive::BTN_LANDING_GEAR | WolfDrive::BTN_ENGINE
                      | WolfDrive::BTN_DIVE | WolfDrive::BTN_SURFACE | WolfDrive::BTN_PERISCOPE | WolfDrive::BTN_SONAR
                      | (1u << WolfDrive::ACT_RETARDER_DSC_UP) | (1u << WolfDrive::ACT_RETARDER_DSC_DOWN), false);   // [WOLF RETARDER 2026-10-12/]
    mPressed = H_NONE;
}

bool WolfDashboard::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // [HUD SIZE 2026-10-12] the top edge: a drag sizes the dashboard, and is never a press on a control
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
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
    // the edge's own up (the second of a double-click too) stays with the HUD, never the world under it
    const bool was = mPressed != H_NONE || mResize.dragging() || mResize.onGrip(x, y);
    mResize.end();   // [HUD SIZE 2026-10-12/]
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
        // [HUD SIZE 2026-10-12] mWheelR is the picture's: the travel too
        WolfDrive::instance().setScreenSteer(llclamp((F32)(x - mPressX) / mHitScale / llmax(24.f, mWheelR), -1.f, 1.f), true);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    // [WOLF RETARDER 2026-10-12] dragging the stalk: it follows the pointer from slot to slot (up and down)
    if (hasMouseCapture() && mPressed >= H_RET_0 && mPressed <= H_RET_5)
    {
        const S32 slot = retarderSlotAt(y);
        if (slot >= 0)
        {
            WolfDrive::instance().setRetarderStalk(slot);
            mPressed = H_RET_0 + slot;
        }
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    // [HUD SIZE 2026-10-12] Source: llresizebar.cpp handleHover - UI_CURSOR_SIZENS for an edge dragged up and down
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

bool WolfDashboard::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfDashboard::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();   // [HUD SIZE 2026-10-12/] double-click on the top edge: the default size
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    return handleMouseDown(x, y, mask);
}

// [WOLF RETARDER 2026-10-12] The scroll wheel over the retarder stalk steps it: towards you (clicks > 0, which
// llscrollbar.cpp:409 handleScrollWheel turns into changeLine(+), further down a list) is the stalk pulled down,
// one stage more braking. Anywhere else
// the wheel goes on to the world as before (the camera's zoom).
bool WolfDashboard::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!mDrawn || clicks == 0 || !WolfDrive::onWolf())
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit || hit->mId < H_RET_0 || hit->mId > H_RET_5)
    {
        return false;
    }
    WolfDrive& d = WolfDrive::instance();
    d.setRetarderStalk(d.retarderStalk() + clicks);
    return true;
}

S32 WolfDashboard::retarderSlotAt(S32 y) const
{
    const F32 py = (F32)y / mHitScale;   // [HUD SIZE 2026-10-12] the hits are in the picture's units
    S32 best = -1;
    F32 best_d = 0.f;
    for (const Hit& h : mHits)
    {
        if (h.mId < H_RET_0 || h.mId > H_RET_5) continue;
        const F32 dist = fabsf(py - (h.mB + h.mT) * 0.5f);
        if (best < 0 || dist < best_d)
        {
            best = h.mId - H_RET_0;
            best_d = dist;
        }
    }
    return best;
}

bool WolfDashboard::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());   // [HUD SIZE 2026-10-12/]
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

void WolfDashboard::onMouseCaptureLost()
{
    mResize.end();   // [HUD SIZE 2026-10-12/]
    release();
    LLUICtrl::onMouseCaptureLost();
}
