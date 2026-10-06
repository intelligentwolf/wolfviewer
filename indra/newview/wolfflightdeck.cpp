/**
 * @file wolfflightdeck.cpp
 * @brief WolfViewer Flight Mode: the flight deck drawn across the bottom of the screen.
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

#include "wolfflightdeck.h"

#include <cmath>
#include <ctime>
#include <map>

#include "llagent.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llwindow.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmipmap.h"
#include "llviewertexture.h"
#include "wolfflight.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"

static LLDefaultChildRegistry::Register<WolfFlightDeck> r_wolf_flight_deck("wolf_flight_deck");

namespace
{
    const F32 KT = 1.943844f;
    const F32 FT = 3.280840f;
    const F32 FPM = 196.8504f;
    const F32 NM = 1852.f;
    const F32 PI_F = 3.14159265f;
    const F32 ND_RANGES_NM[] = { 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 20.f, 40.f };
    const S32 ND_RANGE_COUNT = 8;

    // ---- the flight deck's colours (a Boeing-style glass deck) ----
    const LLColor4 C_PANEL_TOP(0.255f, 0.275f, 0.300f, 1.f);
    const LLColor4 C_PANEL_BOT(0.180f, 0.195f, 0.215f, 1.f);
    const LLColor4 C_GLARE_TOP(0.035f, 0.035f, 0.040f, 1.f);
    const LLColor4 C_GLARE_BOT(0.090f, 0.092f, 0.100f, 1.f);
    const LLColor4 C_MCP_FACE_T(0.330f, 0.350f, 0.375f, 1.f);
    const LLColor4 C_MCP_FACE_B(0.255f, 0.272f, 0.295f, 1.f);
    const LLColor4 C_BEZEL(0.105f, 0.110f, 0.120f, 1.f);
    const LLColor4 C_BEZEL_HI(0.300f, 0.315f, 0.335f, 1.f);
    const LLColor4 C_BEZEL_LO(0.030f, 0.030f, 0.035f, 1.f);
    const LLColor4 C_SCREEN(0.008f, 0.012f, 0.020f, 1.f);
    const LLColor4 C_WHITE(0.95f, 0.95f, 0.95f, 1.f);
    const LLColor4 C_GREY(0.62f, 0.64f, 0.66f, 1.f);
    const LLColor4 C_DIM(0.35f, 0.37f, 0.40f, 1.f);
    const LLColor4 C_GREEN(0.20f, 1.00f, 0.25f, 1.f);
    const LLColor4 C_MAGENTA(1.00f, 0.35f, 1.00f, 1.f);
    const LLColor4 C_CYAN(0.25f, 0.90f, 1.00f, 1.f);
    const LLColor4 C_AMBER(1.00f, 0.72f, 0.00f, 1.f);
    const LLColor4 C_RED(1.00f, 0.16f, 0.12f, 1.f);
    const LLColor4 C_BLACK(0.f, 0.f, 0.f, 1.f);
    const LLColor4 C_SKY_HI(0.050f, 0.330f, 0.700f, 1.f);
    const LLColor4 C_SKY_LO(0.200f, 0.560f, 0.930f, 1.f);
    const LLColor4 C_GND_HI(0.560f, 0.340f, 0.120f, 1.f);
    const LLColor4 C_GND_LO(0.360f, 0.210f, 0.070f, 1.f);
    const LLColor4 C_TAPE(0.300f, 0.320f, 0.350f, 0.85f);
    const LLColor4 C_LED_DIGIT(1.00f, 0.66f, 0.10f, 1.f);
    const LLColor4 C_HUD(0.30f, 1.00f, 0.45f, 0.92f);
    const LLColor4 C_KNOB_T(0.200f, 0.205f, 0.215f, 1.f);
    const LLColor4 C_KNOB_B(0.060f, 0.062f, 0.068f, 1.f);
    const LLColor4 C_BTN_T(0.215f, 0.225f, 0.240f, 1.f);
    const LLColor4 C_BTN_B(0.120f, 0.125f, 0.135f, 1.f);
    const LLColor4 C_BAR_ON(0.25f, 1.00f, 0.35f, 1.f);
    const LLColor4 C_BAR_OFF(0.05f, 0.14f, 0.07f, 1.f);

    LLColor4 withAlpha(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    F64 now()
    {
        return LLFrameTimer::getTotalSeconds();
    }

    bool blink(F32 period = 1.f)
    {
        return fmod(now(), (F64)period) < period * 0.5f;
    }

    F32 wrap180(F32 a)
    {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    F32 wrap360(F32 a)
    {
        while (a >= 360.f) a -= 360.f;
        while (a < 0.f) a += 360.f;
        return a;
    }

    std::string fmt(const char* f, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, f);
        vsnprintf(buf, sizeof(buf), f, ap);
        va_end(ap);
        return std::string(buf);
    }

    // ---- primitives (view-local coordinates, no texture) ----
    void noTex()
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    void vtx(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

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
        if (len <= 0.0001f)
        {
            return;
        }
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quadF(x1 + nx, y1 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, x2 + nx, y2 + ny, c);
    }

    void dashW(F32 x1, F32 y1, F32 x2, F32 y2, F32 w, F32 dash, const LLColor4& c)
    {
        const F32 dx = x2 - x1, dy = y2 - y1;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f)
        {
            return;
        }
        for (F32 s = 0.f; s < len; s += dash * 2.f)
        {
            const F32 e = llmin(s + dash, len);
            lineW(x1 + dx * s / len, y1 + dy * s / len, x1 + dx * e / len, y1 + dy * e / len, w, c);
        }
    }

    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c);
        rectF(l, t - w, r, t, c);
        rectF(l, b, l + w, t, c);
        rectF(r - w, b, r, t, c);
    }

    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 32)
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

    // A lit dome: a radial gradient, brighter toward the upper left like a light above.
    void circleShaded(F32 cx, F32 cy, F32 rad, const LLColor4& hi, const LLColor4& lo, S32 seg = 40)
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

    // A stroked arc, angles in radians measured counter-clockwise from +x.
    void arcW(F32 cx, F32 cy, F32 rad, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 48)
    {
        noTex();
        const F32 ri = rad - w * 0.5f, ro = rad + w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
            const F32 c0 = cosf(u0), s0 = sinf(u0), c1 = cosf(u1), s1 = sinf(u1);
            vtx(cx + ri * c0, cy + ri * s0, c); vtx(cx + ro * c0, cy + ro * s0, c); vtx(cx + ro * c1, cy + ro * s1, c);
            vtx(cx + ri * c0, cy + ri * s0, c); vtx(cx + ro * c1, cy + ro * s1, c); vtx(cx + ri * c1, cy + ri * s1, c);
        }
        gGL.end();
    }

    void roundRectF(F32 l, F32 b, F32 r, F32 t, F32 rad, const LLColor4& top, const LLColor4& bot)
    {
        rad = llmin(rad, (r - l) * 0.5f, (t - b) * 0.5f);
        const F32 h = t - b;
        auto col = [&](F32 y) { return lerp(bot, top, llclamp((y - b) / h, 0.f, 1.f)); };
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        // centre band
        vtx(l, b + rad, col(b + rad)); vtx(r, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad));
        vtx(l, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad)); vtx(l, t - rad, col(t - rad));
        // top and bottom bands
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t)); vtx(l + rad, t, col(t));
        vtx(l + rad, b, col(b)); vtx(r - rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad));
        vtx(l + rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad)); vtx(l + rad, b + rad, col(b + rad));
        // corners
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

    // A slotted panel screw.
    void screw(F32 x, F32 y, F32 rad)
    {
        circleShaded(x, y, rad, LLColor4(0.55f, 0.56f, 0.58f, 1.f), LLColor4(0.18f, 0.18f, 0.20f, 1.f), 16);
        const F32 a = 0.6f + x * 0.013f;   // each screw turned its own way
        lineW(x - rad * 0.7f * cosf(a), y - rad * 0.7f * sinf(a), x + rad * 0.7f * cosf(a), y + rad * 0.7f * sinf(a),
              llmax(1.f, rad * 0.3f), LLColor4(0.08f, 0.08f, 0.09f, 1.f));
    }

    // ---- Sailing Mode's classic yacht look (Paul: "more sailingy ... perhaps we could have walnut?").
    // Polished lacquered brass cases on varnished wood, cream dials with plain black figures,
    // red to port and green to starboard: how ship's clocks, barometers and wind instruments are
    // made (Weems & Plath brass cases, teak / walnut mounts).
    const LLColor4 C_BRASS_HI(0.99f, 0.88f, 0.55f, 1.f);
    const LLColor4 C_BRASS(0.80f, 0.62f, 0.27f, 1.f);
    const LLColor4 C_BRASS_LO(0.42f, 0.29f, 0.10f, 1.f);
    const LLColor4 C_CREAM_HI(0.98f, 0.95f, 0.87f, 1.f);
    const LLColor4 C_CREAM_LO(0.86f, 0.80f, 0.66f, 1.f);
    const LLColor4 C_INK(0.09f, 0.08f, 0.07f, 1.f);
    const LLColor4 C_NAVY(0.10f, 0.20f, 0.48f, 1.f);
    const LLColor4 C_PORT(0.78f, 0.10f, 0.08f, 1.f);
    const LLColor4 C_STBD(0.08f, 0.55f, 0.20f, 1.f);
    const LLColor4 C_LCD_HI(0.70f, 0.75f, 0.63f, 1.f);
    const LLColor4 C_LCD_LO(0.58f, 0.63f, 0.52f, 1.f);
    const LLColor4 C_LCD_INK(0.08f, 0.11f, 0.08f, 1.f);

    // A ring of polished brass: light on the upper left, dark on the lower right.
    void brassRing(F32 cx, F32 cy, F32 rad, F32 width, S32 seg = 64)
    {
        noTex();
        const F32 ri = rad - width, ro = rad;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            // light from the upper left: brightest at 135 degrees
            const F32 l0 = 0.5f + 0.5f * cosf(a0 - 2.356f), l1 = 0.5f + 0.5f * cosf(a1 - 2.356f);
            const LLColor4 c0 = l0 > 0.5f ? lerp(C_BRASS, C_BRASS_HI, (l0 - 0.5f) * 2.f) : lerp(C_BRASS_LO, C_BRASS, l0 * 2.f);
            const LLColor4 c1 = l1 > 0.5f ? lerp(C_BRASS, C_BRASS_HI, (l1 - 0.5f) * 2.f) : lerp(C_BRASS_LO, C_BRASS, l1 * 2.f);
            vtx(cx + ri * cosf(a0), cy + ri * sinf(a0), c0); vtx(cx + ro * cosf(a0), cy + ro * sinf(a0), c0); vtx(cx + ro * cosf(a1), cy + ro * sinf(a1), c1);
            vtx(cx + ri * cosf(a0), cy + ri * sinf(a0), c0); vtx(cx + ro * cosf(a1), cy + ro * sinf(a1), c1); vtx(cx + ri * cosf(a1), cy + ri * sinf(a1), c1);
        }
        gGL.end();
        arcW(cx, cy, ri, 1.f, 0.f, 2.f * PI_F, C_BRASS_LO, seg);
    }

    // A brass frame round a rectangle (a polished strip with a lit top edge).
    void brassFrame(F32 l, F32 b, F32 r, F32 t, F32 width)
    {
        roundRectF(l, b, r, t, width * 0.9f, C_BRASS_HI, C_BRASS_LO);
        roundRectF(l + width * 0.35f, b + width * 0.35f, r - width * 0.35f, t - width * 0.35f, width * 0.6f, C_BRASS, C_BRASS_HI);
    }

    void brassScrew(F32 x, F32 y, F32 rad)
    {
        circleShaded(x, y, rad, C_BRASS_HI, C_BRASS_LO, 14);
        lineW(x - rad * 0.7f, y - rad * 0.2f, x + rad * 0.7f, y + rad * 0.2f, llmax(1.f, rad * 0.28f), C_BRASS_LO);
    }

    // Varnished walnut: planks with their grain, a dark seam between, the varnish's gloss on top.
    void walnut(F32 l, F32 b, F32 r, F32 t, F32 plank_h)
    {
        const LLColor4 hi(0.40f, 0.23f, 0.12f, 1.f), lo(0.26f, 0.14f, 0.07f, 1.f);
        S32 plank = 0;
        for (F32 pb = b; pb < t; pb += plank_h, ++plank)
        {
            const F32 pt = llmin(t, pb + plank_h);
            const F32 tone = 0.85f + 0.15f * sinf(plank * 2.7f);
            rectG(l, pb, r, pt, hi * tone, lo * tone);
            // grain: long wavering lines, a little darker and a little lighter by turns
            noTex();
            for (S32 g = 0; g < 14; ++g)
            {
                const F32 y0 = pb + (pt - pb) * (g + 0.5f) / 14.f;
                const F32 amp = (pt - pb) * (0.02f + 0.03f * fabsf(sinf(g * 1.3f + plank)));
                const F32 freq = 0.004f + 0.003f * fabsf(cosf(g * 0.7f + plank * 1.9f));
                const F32 ph = g * 2.1f + plank * 5.3f;
                const LLColor4 c = (g % 3 == 0) ? LLColor4(0.55f, 0.33f, 0.17f, 0.18f) : LLColor4(0.10f, 0.05f, 0.02f, 0.22f);
                F32 px = l, py = y0 + amp * sinf(ph);
                for (F32 x = l + 24.f; x <= r + 24.f; x += 24.f)
                {
                    const F32 y = y0 + amp * sinf(x * freq + ph) + amp * 0.4f * sinf(x * freq * 3.1f + ph * 0.7f);
                    lineW(px, py, x, y, 1.2f, c);
                    px = x; py = y;
                }
            }
            rectF(l, pt - 1.f, r, pt, LLColor4(0.05f, 0.02f, 0.01f, 0.7f));   // the seam
        }
        rectG(l, b, r, t, LLColor4(1.f, 0.95f, 0.85f, 0.10f), LLColor4(1.f, 1.f, 1.f, 0.f));   // varnish
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::LEFT, LLFontGL::VAlign v = LLFontGL::BASELINE)
    {
        if (!f || s.empty())
        {
            return;
        }
        f->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // The B612 face at the largest size whose line fits px (fonts.xml Avionics0..11).
    const LLFontGL* avionics(const char* name, U8 style, F32 px)
    {
        static std::map<std::string, std::vector<const LLFontGL*>> cache;
        const std::string key = std::string(name) + (style ? "B" : "R");
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, fmt("Avionics%d", i), style));
                if (f)
                {
                    v.push_back(f);
                }
            }
            if (v.empty())
            {
                LL_WARNS("WolfFlight") << "font " << name << " missing, using the default" << LL_ENDL;
                v.push_back(LLFontGL::getFontSansSerif());
            }
            it = cache.emplace(key, v).first;
        }
        const std::vector<const LLFontGL*>& v = it->second;
        const LLFontGL* best = v.front();
        for (const LLFontGL* f : v)
        {
            if ((F32)f->getLineHeight() <= px)
            {
                best = f;
            }
        }
        return best;
    }

    // Rotation about a centre, for the attitude sphere.
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

    std::string headingLabel(S32 deg)
    {
        deg = ((deg % 360) + 360) % 360;
        if (deg == 0) return "N";
        if (deg == 90) return "E";
        if (deg == 180) return "S";
        if (deg == 270) return "W";
        return fmt("%d", deg / 10);
    }

    bool parseFloat(const std::string& s, F32& out)
    {
        std::string t = s;
        LLStringUtil::trim(t);
        if (t.empty())
        {
            return false;
        }
        char* end = nullptr;
        const double v = strtod(t.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v))
        {
            return false;
        }
        out = (F32)v;
        return true;
    }

    std::vector<std::string> splitSlash(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s)
        {
            if (ch == '/')
            {
                out.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += ch;
            }
        }
        out.push_back(cur);
        return out;
    }

    std::string utcClock(bool seconds)
    {
        const time_t t = time(nullptr);
        struct tm g;
#if LL_WINDOWS
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        return seconds ? fmt("%02d:%02d:%02dZ", g.tm_hour, g.tm_min, g.tm_sec) : fmt("%02d%02dZ", g.tm_hour, g.tm_min);
    }
}

//-----------------------------------------------------------------------------

WolfFlightDeck::WolfFlightDeck(const Params& p)
:   LLUICtrl(p)
{
}

WolfFlightDeck::~WolfFlightDeck()
{
}

void WolfFlightDeck::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ l, b, r, t, id, tip });
}

const WolfFlightDeck::Hit* WolfFlightDeck::hitAt(S32 x, S32 y) const
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

void WolfFlightDeck::pickFonts(F32 unit)
{
    mF.mTiny      = avionics("B612", LLFontGL::NORMAL, unit * 0.62f);
    mF.mSmall     = avionics("B612", LLFontGL::NORMAL, unit * 0.78f);
    mF.mMed       = avionics("B612", LLFontGL::BOLD, unit * 1.0f);
    mF.mLarge     = avionics("B612", LLFontGL::BOLD, unit * 1.3f);
    mF.mMonoSmall = avionics("B612Mono", LLFontGL::NORMAL, unit * 0.8f);
    mF.mMonoMed   = avionics("B612Mono", LLFontGL::BOLD, unit * 1.05f);
    mF.mMonoLarge = avionics("B612Mono", LLFontGL::BOLD, unit * 1.35f);
    mF.mMonoHuge  = avionics("B612Mono", LLFontGL::BOLD, unit * 1.75f);
}

void WolfFlightDeck::draw()
{
    WolfFlight& f = WolfFlight::instance();
    mHits.clear();
    if (!f.active())
    {
        mPanelTop = mPanelBottom = 0.f;
        mFlightStart = 0.0;
        if (hasFocus())
        {
            setFocus(false);
        }
        return;
    }
    // Hidden from the bottom-right icon (WolfFlight::toggleDeck): nothing drawn, no clicks taken,
    // the mode and its autopilot carry on. Paul: "hide ... without quitting it to take pictures".
    if (f.deckHidden())
    {
        mPanelTop = mPanelBottom = 0.f;
        if (hasFocus())
        {
            setFocus(false);
        }
        return;
    }
    if (mFlightStart <= 0.0)
    {
        mFlightStart = now();
        syncRouteFromDest();
    }
    // Paul: "we need a help thing". Each mode's guide opens by itself the first time that mode is
    // used ("the sailing help should appear first time on sailing even if they have used the
    // airplane help already").
    {
        const bool sail_now = f.sailing();
        if (sail_now != mHelpForSail || !mHelpChecked)
        {
            syncRouteFromDest();   // the CDU forgets the other mode's destination with it
            mHelpChecked = true;
            mHelpForSail = sail_now;
            const char* setting = sail_now ? "WolfSailHelpShown" : "WolfFlightHelpShown";
            if (!gSavedSettings.getBOOL(setting))
            {
                gSavedSettings.setBOOL(setting, true);
                mShowHelp = true;
            }
        }
    }
    // An EXECuted route whose region the map server has just named: fly it.
    if (mArmRoute)
    {
        if (f.dest().mValid)
        {
            mArmRoute = false;
            f.pressLNAV();
            f.pressVNAV();
            syncRouteFromDest();
        }
        else if (!f.dest().mPending)
        {
            mArmRoute = false;
        }
    }
    // A knob held down keeps turning.
    if (mPressed != H_NONE && mPressStep != 0 && now() - mPressedAt > 0.45)
    {
        if (now() - mLastRepeat > 0.07)
        {
            mLastRepeat = now();
            knob((EHit)mPressed, mPressStep);
        }
    }
    layoutAndDraw();
}

void WolfFlightDeck::layoutAndDraw()
{
    const F32 W = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();

    // Sit on whatever the bottom toolbar leaves: its hide arrow's strip, or the whole bar if
    // the pilot brings it back with the arrow.
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
    F32 H = llclamp(Hv * 0.34f, 200.f, 430.f);
    H = llmin(H, (Hv - bottom) * 0.62f);
    const F32 top = bottom + H;
    mPanelBottom = bottom;
    mPanelTop = top;

    LLGLSUIDefault gls_ui;

    // the world above: head-up display (aircraft only), and the warnings whether or not the HUD is on
    const bool sail = WolfFlight::instance().sailing();
    if (!sail)
    {
        drawHUD(top);
    }
    drawWarnings(top);

    // the panel
    const F32 M = llclamp(H * 0.15f, 30.f, 54.f);
    if (sail)
    {
        walnut(0.f, bottom, W, top - M, (top - M - bottom) / 3.f);
        rectG(0.f, top - M - 3.f, W, top - M, C_BRASS_HI, C_BRASS_LO);   // a brass rubbing strip
    }
    else
    {
        rectG(0.f, bottom, W, top - M, C_PANEL_TOP, C_PANEL_BOT);
        // a faint panel seam every so often, as real panels are made in sections
        for (F32 x = W * 0.125f; x < W; x += W * 0.25f)
        {
            rectF(x, bottom, x + 1.f, top - M, LLColor4(0.f, 0.f, 0.f, 0.25f));
            rectF(x + 1.f, bottom, x + 2.f, top - M, LLColor4(1.f, 1.f, 1.f, 0.05f));
        }
    }
    drawGlareshield(0.f, top - M, W, top);

    // the instruments
    const F32 pad = H * 0.04f;
    const F32 rowB = bottom + pad;
    const F32 rowT = top - M - pad * 0.8f;
    F32 D = rowT - rowB;
    const F32 gap = 0.035f;
    // The instruments left to right, in widths of the display height D. Flight: clock, gear,
    // sidestick, PFD, ND, EICAS, CDU. Sailing: clock, helm, wind, speed and depth, chartplotter,
    // EICAS, CDU. The clock goes first when the screen is narrow.
    enum EPanel { P_CLOCK, P_GEAR, P_STICK, P_PFD, P_ND, P_EICAS, P_CDU, P_HELM, P_WIND, P_DATA };
    struct Panel { EPanel p; F32 w; };
    std::vector<Panel> panels;
    panels.push_back({ P_CLOCK, 0.50f });
    if (sail)
    {
        panels.push_back({ P_HELM, 0.62f });
        panels.push_back({ P_WIND, 1.0f });
        panels.push_back({ P_DATA, 0.72f });
    }
    else
    {
        panels.push_back({ P_GEAR, 0.36f });
        panels.push_back({ P_STICK, 0.56f });
        panels.push_back({ P_PFD, 1.0f });
    }
    panels.push_back({ P_ND, 1.0f });
    panels.push_back({ P_EICAS, 0.80f });
    panels.push_back({ P_CDU, 0.84f });
    auto unitsOf = [&]()
    {
        F32 u = gap * (panels.size() - 1);
        for (const Panel& pn : panels) u += pn.w;
        return u;
    };
    F32 units = unitsOf();
    F32 Dw = (W - 2.f * pad) / units;
    if (Dw < D * 0.9f)
    {
        panels.erase(panels.begin());   // no room for the clock
        units = unitsOf();
        Dw = (W - 2.f * pad) / units;
    }
    D = llmin(D, Dw);
    const F32 yb = rowB + ((rowT - rowB) - D) * 0.5f;
    const F32 yt = yb + D;
    F32 x = (W - units * D) * 0.5f;
    pickFonts(D * 0.055f);
    const bool plan = gSavedSettings.getBOOL("WolfFlightNDPlan");
    for (const Panel& pn : panels)
    {
        const F32 r = x + pn.w * D;
        switch (pn.p)
        {
        case P_CLOCK: drawClock(x, yb, r, yt); break;
        case P_GEAR:  drawGear(x, yb, r, yt); break;
        case P_STICK: drawStick(x, yb, r, yt); break;
        case P_PFD:   drawPFD(x, yb, r, yt); break;
        case P_ND:    if (plan) drawPlan(x, yb, r, yt); else drawND(x, yb, r, yt); break;
        case P_EICAS: drawEICAS(x, yb, r, yt); break;
        case P_CDU:   drawCDU(x, yb, r, yt); break;
        case P_HELM:  drawHelm(x, yb, r, yt); break;
        case P_WIND:  drawWindDial(x, yb, r, yt); break;
        case P_DATA:  drawSailData(x, yb, r, yt); break;
        }
        x = r + gap * D;
    }

    if (mShowHelp)
    {
        drawHelp(top);
    }
}

// A display's bezel: a dark rounded frame, a lit top edge, four screws; gives back the glass.
void WolfFlightDeck::drawBezel(F32 l, F32 b, F32 r, F32 t, F32 inset, F32& sl, F32& sb, F32& sr, F32& st)
{
    const F32 rad = inset * 0.9f;
    if (WolfFlight::instance().sailing())
    {
        roundRectF(l - 1.f, b - 3.f, r + 2.f, t, rad, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.45f));
        brassFrame(l, b, r, t, inset);
        sl = l + inset;
        sb = b + inset;
        sr = r - inset;
        st = t - inset;
        rectF(sl - 2.f, sb - 2.f, sr + 2.f, st + 2.f, C_BRASS_LO);
        rectF(sl, sb, sr, st, C_SCREEN);
        const F32 rs = llmax(2.f, inset * 0.22f);
        brassScrew(l + inset * 0.5f, b + inset * 0.5f, rs);
        brassScrew(r - inset * 0.5f, b + inset * 0.5f, rs);
        brassScrew(l + inset * 0.5f, t - inset * 0.5f, rs);
        brassScrew(r - inset * 0.5f, t - inset * 0.5f, rs);
        return;
    }
    roundRectF(l - 1.f, b - 2.f, r + 1.f, t + 1.f, rad, C_BEZEL_LO, C_BEZEL_LO);           // drop shadow
    roundRectF(l, b, r, t, rad, C_BEZEL_HI, C_BEZEL);
    roundRectF(l + 2.f, b + 2.f, r - 2.f, t - 2.f, rad * 0.8f, LLColor4(0.15f, 0.155f, 0.165f, 1.f), C_BEZEL);
    sl = l + inset;
    sb = b + inset;
    sr = r - inset;
    st = t - inset;
    rectF(sl - 2.f, sb - 2.f, sr + 2.f, st + 2.f, C_BEZEL_LO);
    rectF(sl, sb, sr, st, C_SCREEN);
    const F32 sr_ = llmax(2.f, inset * 0.22f);
    screw(l + inset * 0.5f, b + inset * 0.5f, sr_);
    screw(r - inset * 0.5f, b + inset * 0.5f, sr_);
    screw(l + inset * 0.5f, t - inset * 0.5f, sr_);
    screw(r - inset * 0.5f, t - inset * 0.5f, sr_);
}

//-----------------------------------------------------------------------------
// The glareshield: master lights and the autopilot's mode control panel
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawGlareshield(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const F32 M = t - b;
    // the anti-glare lip and the panel face
    rectG(l, b, r, t, C_GLARE_TOP, C_GLARE_BOT);
    rectF(l, t - 2.f, r, t, LLColor4(0.f, 0.f, 0.f, 1.f));
    rectF(l, b, r, b + 1.f, LLColor4(1.f, 1.f, 1.f, 0.08f));

    // The items, left to right, in units of the strip height.
    enum EKind { K_MASTER_W, K_MASTER_C, K_BUTTON, K_WINDOW, K_KNOB, K_WHEEL, K_GAP };
    struct Item { EKind k; F32 w; EHit id; std::string label; std::string value; bool lit; std::string tip; };
    std::vector<Item> items;
    const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;
    const WolfFlight::Data& d = f.data();
    const S32 range_idx = llclamp(gSavedSettings.getS32("WolfFlightNDRange"), 0, ND_RANGE_COUNT - 1);

    const bool sail = f.sailing();
    if (sail)
    {
        // A yacht's autopilot head: AUTO / STBY, TRACK (to the waypoint, tacking on the laylines),
        // HDG (a compass course), WIND (a wind angle), TACK; the motor's speed hold.
        items.push_back({ K_MASTER_W, 1.15f, H_MASTER_WARN, "WARNING", "", f.masterWarning(), "Master WARNING: press to cancel the warnings" });
        items.push_back({ K_MASTER_C, 1.15f, H_MASTER_CAUT, "CAUTION", "", f.masterCaution(), "Master CAUTION: press to cancel the cautions" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 0.95f, H_LNAV, "TRACK", "", f.lateral() == WolfFlight::LAT_LNAV,
                          "Track: sail to the destination set on the CDU, round the land, tacking on the laylines" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        items.push_back({ K_WINDOW, 1.05f, H_HDG_KNOB, "HEADING", fmt("%03d", (S32)llround(f.selHeading()) % 360), false, "Selected compass heading" });
        items.push_back({ K_KNOB, 0.85f, H_HDG_KNOB, "HDG", "", false, "Heading knob: scroll or click left / right (Shift: x10)" });
        items.push_back({ K_BUTTON, 0.95f, H_HDG_SEL, "HDG", "", f.lateral() == WolfFlight::LAT_HDG, "Steer the selected compass heading" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        {
            const S32 twa = (S32)llround(f.selTWA());
            items.push_back({ K_WINDOW, 1.15f, H_TWA_KNOB, "WIND ANGLE", fmt("%3d%c", llabs(twa), twa >= 0 ? 'S' : 'P'), false,
                              "Wind angle to hold, degrees off the bow (S = wind on the starboard side, P = port)" });
        }
        items.push_back({ K_KNOB, 0.85f, H_TWA_KNOB, "TWA", "", false, "Wind angle knob: scroll or click left / right" });
        items.push_back({ K_BUTTON, 0.95f, H_WIND, "WIND", "", f.lateral() == WolfFlight::LAT_WIND, "Hold this angle to the true wind (starts from the angle you are sailing now)" });
        items.push_back({ K_BUTTON, 0.95f, H_TACK, "TACK", "", false, "Tack (or gybe) through the wind to the other side" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 0.95f, H_AT, "MOTOR", "", f.atEngaged(), "Motor speed hold: keep the selected speed with the throttle keys" });
        items.push_back({ K_WINDOW, 1.05f, H_SPD_KNOB, "SPEED", fmt("%3d", (S32)llround(f.selSpeedKt())), false, "Motor speed to hold (knots)" });
        items.push_back({ K_KNOB, 0.85f, H_SPD_KNOB, "SPD", "", false, "Speed knob: scroll or click left / right (Shift: x10)" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 1.15f, H_CMD, "AUTO", "", f.apEngaged(), "Autopilot on / standby (the helm keys or the wheel also put it to standby)" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_KNOB, 0.85f, H_RANGE_KNOB, "RANGE", fmt("%g", ND_RANGES_NM[range_idx]), false, "Chartplotter range (nautical miles)" });
        items.push_back({ K_BUTTON, 0.95f, H_PLAN, "PLAN", "", gSavedSettings.getBOOL("WolfFlightNDPlan"), "Chartplotter: PLAN (the whole route, north up) or CHART (course up, around you)" });
        items.push_back({ K_BUTTON, 0.95f, H_MAP, "MAP", "", false, "World Map, zoomed to the whole trip: the planned route and where you are on it" });
        items.push_back({ K_BUTTON, 0.95f, H_WEB, "CLUB", "", false, "The Wolf Territories sailing club page on wolf-grid.com" });
        items.push_back({ K_BUTTON, 0.95f, H_HELP, "HELP", "", mShowHelp, "How to sail with Sailing Mode: the quick guide" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 1.0f, H_EXIT, "EXIT", "", false, "Leave Sailing Mode" });
    }
    else
    {
        items.push_back({ K_MASTER_W, 1.15f, H_MASTER_WARN, "WARNING", "", f.masterWarning(), "Master WARNING: press to cancel the warnings" });
        items.push_back({ K_MASTER_C, 1.15f, H_MASTER_CAUT, "CAUTION", "", f.masterCaution(), "Master CAUTION: press to cancel the cautions" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 0.95f, H_AT, heli ? "A/T SPD" : "A/T", "", f.atEngaged(),
                          heli ? "Autothrottle: hold the selected speed with the forward keys"
                               : "Autothrottle: hold the selected speed with the throttle keys" });
        items.push_back({ K_WINDOW, 1.25f, H_SPD_KNOB, "IAS", fmt("%3d", (S32)llround(f.selSpeedKt())), false, "Selected speed (knots): scroll, or click left/right of the knob" });
        items.push_back({ K_KNOB, 0.85f, H_SPD_KNOB, "SPD", "", false, "Speed knob: scroll or click left / right (Shift: x10)" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 0.95f, H_LNAV, "LNAV", "", f.lateral() == WolfFlight::LAT_LNAV || f.lateral() == WolfFlight::LAT_HOLD,
                          "Lateral navigation: fly straight to the destination set on the CDU" });
        items.push_back({ K_BUTTON, 0.95f, H_VNAV, "VNAV", "", f.vertical() == WolfFlight::VERT_VNAV || f.vertical() == WolfFlight::VERT_HOVER,
                          "Vertical navigation: cruise at CRZ ALT, descend to the destination's height" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        items.push_back({ K_WINDOW, 1.05f, H_HDG_KNOB, "HEADING", fmt("%03d", (S32)llround(f.selHeading()) % 360), false, "Selected heading" });
        items.push_back({ K_KNOB, 0.85f, H_HDG_KNOB, "HDG", "", false, "Heading knob: scroll or click left / right (Shift: x10)" });
        items.push_back({ K_BUTTON, 0.95f, H_HDG_SEL, "HDG SEL", "", f.lateral() == WolfFlight::LAT_HDG, "Heading select: fly the selected heading" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        items.push_back({ K_WINDOW, 1.45f, H_ALT_KNOB, "ALTITUDE", fmt("%5d", (S32)llround(f.selAltFt())), false, "Selected altitude (feet above the region's sea level)" });
        items.push_back({ K_KNOB, 0.85f, H_ALT_KNOB, "ALT", "", false, "Altitude knob: 100 ft a click (Shift: 1000)" });
        items.push_back({ K_BUTTON, 0.95f, H_ALT_HOLD, "ALT HOLD", "", f.vertical() == WolfFlight::VERT_ALT, "Climb or descend to the selected altitude and hold it" });
        items.push_back({ K_GAP, 0.25f, H_NONE, "", "", false, "" });
        {
            const S32 vs = (S32)llround(f.selVsFpm());
            items.push_back({ K_WINDOW, 1.35f, H_VS_WHEEL, "VERT SPEED", f.vertical() == WolfFlight::VERT_VS ? fmt("%+05d", vs) : std::string("     "), false, "Selected vertical speed (feet per minute)" });
        }
        items.push_back({ K_WHEEL, 0.45f, H_VS_WHEEL, "", "", false, "Vertical speed wheel: 100 ft/min a click" });
        items.push_back({ K_BUTTON, 0.95f, H_VS, "V/S", "", f.vertical() == WolfFlight::VERT_VS, "Vertical speed: climb or descend at the selected rate, level at the selected altitude" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 1.15f, H_CMD, "CMD", "", f.apEngaged(), "Autopilot engage / disengage (any flight key also disconnects it)" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_KNOB, 0.85f, H_RANGE_KNOB, "RANGE", fmt("%g", ND_RANGES_NM[range_idx]), false, "Navigation display range (nautical miles)" });
        items.push_back({ K_BUTTON, 0.95f, H_PLAN, "PLAN", "", gSavedSettings.getBOOL("WolfFlightNDPlan"), "Navigation display: PLAN (the whole route, north up) or MAP (heading up, around you)" });
        items.push_back({ K_BUTTON, 0.95f, H_HUD, "HUD", "", gSavedSettings.getBOOL("WolfFlightHUD"), "Head-up display over the view" });
        items.push_back({ K_BUTTON, 0.95f, H_MAP, "MAP", "", false, "World Map, zoomed to the whole trip: the planned route and where you are on it" });
        items.push_back({ K_BUTTON, 1.05f, H_WEB, "AIRPORTS", "", false, "The Wolf Territories airports page on wolf-grid.com" });
        items.push_back({ K_BUTTON, 0.95f, H_HELP, "HELP", "", mShowHelp, "How to fly with Flight Mode: the quick guide" });
        items.push_back({ K_GAP, 0.35f, H_NONE, "", "", false, "" });
        items.push_back({ K_BUTTON, 1.0f, H_EXIT, "EXIT", "", false, "Leave Flight Mode" });
    }

    F32 total = 0.f;
    for (const Item& it : items) total += it.w;
    const F32 u = llmin(M, (r - l - 2.f * M * 0.3f) / total);
    F32 x = l + ((r - l) - total * u) * 0.5f;
    const F32 face_b = b + M * 0.1f, face_t = t - M * 0.12f;
    // the MCP face plate behind the autopilot controls
    {
        F32 x0 = x, x1 = x;
        F32 acc = x;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i == 3) x0 = acc - u * 0.15f;
            acc += items[i].w * u;
            if (items[i].id == H_CMD) x1 = acc + u * 0.15f;
        }
        if (sail)
        {
            brassFrame(x0, face_b, x1, face_t, u * 0.08f);
            roundRectF(x0 + u * 0.08f, face_b + u * 0.08f, x1 - u * 0.08f, face_t - u * 0.08f, u * 0.06f,
                       LLColor4(0.30f, 0.17f, 0.09f, 1.f), LLColor4(0.20f, 0.11f, 0.05f, 1.f));   // a teak face
            brassScrew(x0 + u * 0.12f, (face_b + face_t) * 0.5f, u * 0.06f);
            brassScrew(x1 - u * 0.12f, (face_b + face_t) * 0.5f, u * 0.06f);
        }
        else
        {
            roundRectF(x0, face_b, x1, face_t, u * 0.12f, C_MCP_FACE_T, C_MCP_FACE_B);
            screw(x0 + u * 0.12f, (face_b + face_t) * 0.5f, u * 0.06f);
            screw(x1 - u * 0.12f, (face_b + face_t) * 0.5f, u * 0.06f);
        }
    }
    const LLFontGL* lbl = avionics("B612", LLFontGL::BOLD, u * 0.2f);
    const LLFontGL* digits = avionics("B612Mono", LLFontGL::BOLD, u * 0.44f);
    const LLFontGL* btn = avionics("B612", LLFontGL::BOLD, u * 0.22f);
    const LLFontGL* master = avionics("B612", LLFontGL::BOLD, u * 0.28f);

    for (const Item& it : items)
    {
        const F32 il = x, ir = x + it.w * u;
        const F32 cx = (il + ir) * 0.5f;
        switch (it.k)
        {
        case K_MASTER_W:
        case K_MASTER_C:
        {
            // An annunciator: a dark lens that lights red / amber and flashes until pressed.
            const bool warn = it.k == K_MASTER_W;
            const F32 bl = il + u * 0.08f, br = ir - u * 0.08f, bb = b + M * 0.16f, bt = t - M * 0.16f;
            const bool on = it.lit && blink(0.8f);
            const LLColor4 lens_on = warn ? C_RED : C_AMBER;
            roundRectF(bl - 2.f, bb - 2.f, br + 2.f, bt + 2.f, u * 0.08f, C_BEZEL_HI, C_BEZEL_LO);
            roundRectF(bl, bb, br, bt, u * 0.06f,
                       on ? lens_on : LLColor4(warn ? 0.22f : 0.20f, warn ? 0.05f : 0.14f, 0.03f, 1.f),
                       on ? lerp(lens_on, C_BLACK, 0.35f) : LLColor4(0.10f, 0.03f, 0.02f, 1.f));
            text(master, warn ? "MASTER" : "MASTER", cx, (bb + bt) * 0.5f + u * 0.02f, on ? C_BLACK : LLColor4(0.45f, 0.3f, 0.2f, 1.f), LLFontGL::HCENTER, LLFontGL::BOTTOM);
            text(master, it.label, cx, (bb + bt) * 0.5f - u * 0.02f, on ? C_BLACK : LLColor4(0.45f, 0.3f, 0.2f, 1.f), LLFontGL::HCENTER, LLFontGL::TOP);
            addHit(bl, bb, br, bt, it.id, it.tip);
            break;
        }
        case K_BUTTON:
        {
            // A square pushbutton with a light bar across its top, lit when the mode is on.
            const F32 s = llmin(it.w * u, M) * 0.78f;
            const F32 bl = cx - s * 0.5f * (it.w > 1.f ? it.w : 1.f), br = cx + s * 0.5f * (it.w > 1.f ? it.w : 1.f);
            const F32 bb = face_b + (face_t - face_b - s) * 0.5f, bt = bb + s;
            const bool pressed = mPressed == it.id;
            const F32 dy = pressed ? 1.f : 0.f;
            const bool classic = f.sailing();
            if (classic)
            {
                // an ivory key in a brass bezel; its lamp amber when on
                brassFrame(bl - 2.5f, bb - 3.f, br + 2.5f, bt + 2.f, 3.f);
                roundRectF(bl, bb - dy, br, bt - dy, s * 0.07f, C_CREAM_HI, C_CREAM_LO);
            }
            else
            {
                roundRectF(bl - 1.5f, bb - 2.5f, br + 1.5f, bt + 1.f, s * 0.08f, C_BEZEL_LO, C_BEZEL_LO);
                roundRectF(bl, bb - dy, br, bt - dy, s * 0.07f, C_BTN_T, C_BTN_B);
                rectF(bl + 1.f, bt - dy - 1.f, br - 1.f, bt - dy, LLColor4(1.f, 1.f, 1.f, 0.15f));
            }
            const F32 bar_t = bt - dy - s * 0.12f, bar_b = bar_t - s * 0.13f;
            const LLColor4 on = classic ? C_AMBER : C_BAR_ON;
            const LLColor4 off = classic ? LLColor4(0.45f, 0.35f, 0.20f, 1.f) : C_BAR_OFF;
            rectF(bl + s * 0.14f, bar_b, br - s * 0.14f, bar_t, it.lit ? on : off);
            if (it.lit)
            {
                rectF(bl + s * 0.10f, bar_b - 1.f, br - s * 0.10f, bar_t + 1.f, withAlpha(on, 0.25f));
            }
            text(btn, it.label, cx, bb - dy + s * 0.32f, classic ? C_INK : C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
            addHit(bl, bb, br, bt, it.id, it.tip);
            break;
        }
        case K_WINDOW:
        {
            // A recessed window with lit seven-segment-style digits, its legend above.
            const F32 wl = il + u * 0.06f, wr = ir - u * 0.06f;
            const F32 wb = face_b + (face_t - face_b) * 0.12f, wt = face_t - (face_t - face_b) * 0.36f;
            text(lbl, it.label, cx, face_t - (face_t - face_b) * 0.06f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
            rectF(wl - 2.f, wb - 2.f, wr + 2.f, wt + 2.f, C_BEZEL_LO);
            rectG(wl, wb, wr, wt, LLColor4(0.04f, 0.03f, 0.02f, 1.f), LLColor4(0.0f, 0.0f, 0.0f, 1.f));
            // unlit segments behind, like a real LED window
            std::string ghost(it.value.size(), '8');
            text(digits, ghost, cx, (wb + wt) * 0.5f, LLColor4(0.18f, 0.10f, 0.02f, 1.f), LLFontGL::HCENTER, LLFontGL::VCENTER);
            text(digits, it.value, cx, (wb + wt) * 0.5f, C_LED_DIGIT, LLFontGL::HCENTER, LLFontGL::VCENTER);
            addHit(wl, wb, wr, wt, it.id, it.tip);
            break;
        }
        case K_KNOB:
        {
            // A knurled knob; its pointer turns with the value it sets.
            const F32 rad = llmin(it.w * u, face_t - face_b) * 0.36f;
            const F32 cy = (face_b + face_t) * 0.5f - u * 0.04f;
            F32 turn = 0.f;
            if (it.id == H_SPD_KNOB) turn = f.selSpeedKt() * 9.f;
            else if (it.id == H_HDG_KNOB) turn = f.selHeading() * 4.f;
            else if (it.id == H_ALT_KNOB) turn = f.selAltFt() * 0.18f;
            else if (it.id == H_RANGE_KNOB) turn = range_idx * 40.f - 140.f;
            circleF(cx + 1.f, cy - 2.f, rad + 1.5f, LLColor4(0.f, 0.f, 0.f, 0.5f));
            if (f.sailing()) circleShaded(cx, cy, rad, C_BRASS_HI, C_BRASS_LO);
            else circleShaded(cx, cy, rad, C_KNOB_T, C_KNOB_B);
            for (S32 i = 0; i < 28; ++i)
            {
                const F32 a = 2.f * PI_F * i / 28.f + turn * DEG_TO_RAD;
                lineW(cx + rad * 0.86f * cosf(a), cy + rad * 0.86f * sinf(a), cx + rad * cosf(a), cy + rad * sinf(a), 1.f,
                      LLColor4(0.42f, 0.43f, 0.45f, 1.f));
            }
            circleShaded(cx, cy, rad * 0.62f, LLColor4(0.30f, 0.31f, 0.33f, 1.f), LLColor4(0.10f, 0.10f, 0.11f, 1.f));
            const F32 a = PI_F * 0.5f - turn * DEG_TO_RAD;
            lineW(cx + rad * 0.15f * cosf(a), cy + rad * 0.15f * sinf(a), cx + rad * 0.58f * cosf(a), cy + rad * 0.58f * sinf(a),
                  llmax(1.5f, rad * 0.1f), C_WHITE);
            if (!it.value.empty())
            {
                // the range knob's detent labels: the selected one beside it
                text(lbl, it.value, cx, face_t - u * 0.02f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
            }
            text(lbl, it.label, cx, face_b + u * 0.02f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);
            addHit(cx - rad, cy - rad, cx + rad, cy + rad, it.id, it.tip);
            break;
        }
        case K_WHEEL:
        {
            // The V/S thumbwheel, seen edge-on: ridges that roll with the value.
            const F32 wl = cx - u * 0.13f, wr = cx + u * 0.13f;
            const F32 wb = face_b + u * 0.08f, wt = face_t - u * 0.08f;
            rectF(wl - 2.f, wb - 2.f, wr + 2.f, wt + 2.f, C_BEZEL_LO);
            rectG(wl, wb, wr, wt, LLColor4(0.30f, 0.30f, 0.32f, 1.f), LLColor4(0.10f, 0.10f, 0.11f, 1.f));
            const F32 roll = fmodf(f.selVsFpm() / 100.f * 0.35f, 1.f);
            for (S32 i = -1; i < 9; ++i)
            {
                const F32 ph = (i + roll) / 8.f;
                if (ph < 0.f || ph > 1.f) continue;
                const F32 y = wb + (wt - wb) * (0.5f - 0.5f * cosf(ph * PI_F));
                lineW(wl + 1.f, y, wr - 1.f, y, 1.2f, LLColor4(0.05f, 0.05f, 0.05f, 1.f));
            }
            text(lbl, "UP", cx, wt + 1.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);
            addHit(wl - u * 0.1f, wb, wr + u * 0.1f, wt, it.id, it.tip);
            break;
        }
        default:
            break;
        }
        x = ir;
    }
}

//-----------------------------------------------------------------------------
// The clock
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawClock(F32 l, F32 b, F32 r, F32 t)
{
    const F32 w = r - l;
    F32 sl, sb, sr, st;
    // a round instrument in a square case
    const F32 cy = b + (t - b) * 0.62f, cx = (l + r) * 0.5f, rad = w * 0.44f;
    const bool ship = WolfFlight::instance().sailing();
    // in Sailing Mode a ship's clock: a round brass case, a cream face, black hands
    const LLColor4 ink = ship ? C_INK : C_WHITE;
    if (ship)
    {
        circleF(cx + 2.f, cy - 3.f, rad * 1.12f, LLColor4(0.f, 0.f, 0.f, 0.45f), 48);
        brassRing(cx, cy, rad * 1.12f, rad * 0.14f);
        circleShaded(cx, cy, rad * 0.985f, C_CREAM_HI, C_CREAM_LO, 48);
    }
    else
    {
        roundRectF(l, cy - w * 0.5f, r, cy + w * 0.5f, w * 0.08f, C_BEZEL_HI, C_BEZEL);
        circleF(cx, cy, rad + 2.f, C_BEZEL_LO);
        circleShaded(cx, cy, rad, LLColor4(0.06f, 0.06f, 0.07f, 1.f), LLColor4(0.01f, 0.01f, 0.012f, 1.f));
    }
    sl = l; sb = b; sr = r; st = t;
    const time_t tt = time(nullptr);
    struct tm g;
#if LL_WINDOWS
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    for (S32 i = 0; i < 60; ++i)
    {
        const F32 a = PI_F * 0.5f - 2.f * PI_F * i / 60.f;
        const F32 in = (i % 5 == 0) ? 0.80f : 0.88f;
        lineW(cx + rad * in * cosf(a), cy + rad * in * sinf(a), cx + rad * 0.95f * cosf(a), cy + rad * 0.95f * sinf(a),
              (i % 5 == 0) ? 2.f : 1.f, ink);
    }
    for (S32 h = 1; h <= 12; ++h)
    {
        const F32 a = PI_F * 0.5f - 2.f * PI_F * h / 12.f;
        text(mF.mSmall, fmt("%d", h), cx + rad * 0.66f * cosf(a), cy + rad * 0.66f * sinf(a), ink, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
    text(mF.mTiny, ship ? "SHIP'S TIME  UTC" : "UTC", cx, cy + rad * 0.32f, ship ? C_INK : C_GREY, LLFontGL::HCENTER, LLFontGL::VCENTER);
    const F32 sec = (F32)g.tm_sec;
    const F32 mins = g.tm_min + sec / 60.f;
    const F32 hrs = (g.tm_hour % 12) + mins / 60.f;
    auto hand = [&](F32 frac, F32 len, F32 width, const LLColor4& c)
    {
        const F32 a = PI_F * 0.5f - 2.f * PI_F * frac;
        lineW(cx - rad * 0.1f * cosf(a), cy - rad * 0.1f * sinf(a), cx + rad * len * cosf(a), cy + rad * len * sinf(a), width, c);
    };
    hand(hrs / 12.f, 0.5f, llmax(2.f, rad * 0.07f), ink);
    hand(mins / 60.f, 0.78f, llmax(1.5f, rad * 0.05f), ink);
    hand(sec / 60.f, 0.86f, 1.f, ship ? C_PORT : C_AMBER);
    if (ship) circleShaded(cx, cy, rad * 0.06f, C_BRASS_HI, C_BRASS_LO, 12);
    else circleF(cx, cy, rad * 0.05f, C_AMBER);

    // the digital readouts below: UTC, and ET (time in Flight Mode)
    const F32 db = b, dt = cy - w * 0.5f - w * 0.06f;
    const S32 et = (S32)(now() - mFlightStart);
    if (ship)
    {
        brassFrame(l + w * 0.04f, db, r - w * 0.04f, dt, w * 0.04f);
        rectG(l + w * 0.08f, db + 3.f, r - w * 0.08f, dt - 3.f, C_LCD_HI, C_LCD_LO);
        text(mF.mMonoSmall, utcClock(true), cx, db + (dt - db) * 0.68f, C_LCD_INK, LLFontGL::HCENTER, LLFontGL::VCENTER);
        text(mF.mMonoSmall, fmt("ET %02d:%02d", et / 3600, (et / 60) % 60), cx, db + (dt - db) * 0.28f, C_LCD_INK, LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }
    rectF(l + w * 0.06f, db, r - w * 0.06f, dt, C_BEZEL_LO);
    rectF(l + w * 0.08f, db + 2.f, r - w * 0.08f, dt - 2.f, C_SCREEN);
    text(mF.mMonoSmall, utcClock(true), cx, db + (dt - db) * 0.68f, C_GREEN, LLFontGL::HCENTER, LLFontGL::VCENTER);
    text(mF.mMonoSmall, fmt("ET %02d:%02d", et / 3600, (et / 60) % 60), cx, db + (dt - db) * 0.28f, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
}

//-----------------------------------------------------------------------------
// Landing gear lever and engine command
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawGear(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const F32 w = r - l, h = t - b;
    roundRectF(l, b, r, t, w * 0.06f, LLColor4(0.22f, 0.235f, 0.255f, 1.f), LLColor4(0.16f, 0.17f, 0.185f, 1.f));
    screw(l + w * 0.1f, t - w * 0.1f, w * 0.035f);
    screw(r - w * 0.1f, t - w * 0.1f, w * 0.035f);
    screw(l + w * 0.1f, b + w * 0.1f, w * 0.035f);
    screw(r - w * 0.1f, b + w * 0.1f, w * 0.035f);
    const F32 cx = (l + r) * 0.5f;

    // three gear lights: green when the gear was commanded down, red while it travels
    const bool moving = now() - f.gearMovedAt() < 6.0;
    const bool down = f.gearDown();
    const char* names[3] = { "NOSE", "LEFT", "RIGHT" };
    const F32 ly = t - h * 0.12f;
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 lx = l + w * (0.2f + 0.3f * i);
        const F32 s = w * 0.11f;
        LLColor4 c = LLColor4(0.06f, 0.08f, 0.06f, 1.f);
        if (moving) c = C_RED;
        else if (down) c = C_GREEN;
        rectF(lx - s - 1.f, ly - s - 1.f, lx + s + 1.f, ly + s + 1.f, C_BEZEL_LO);
        rectG(lx - s, ly - s, lx + s, ly + s, c, lerp(c, C_BLACK, 0.4f));
        text(mF.mTiny, names[i], lx, ly - s - 2.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
    }
    text(mF.mTiny, "GEAR CMD", cx, ly + w * 0.13f, C_GREY, LLFontGL::HCENTER, LLFontGL::BOTTOM);

    // the lever: a slot, UP above, DN below, the wheel-shaped knob at the end
    const F32 slot_t = t - h * 0.30f, slot_b = b + h * 0.30f;
    rectF(cx - w * 0.05f, slot_b, cx + w * 0.05f, slot_t, C_BEZEL_LO);
    text(mF.mSmall, "UP", cx + w * 0.12f, slot_t, C_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
    text(mF.mSmall, "DN", cx + w * 0.12f, slot_b, C_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
    F32 travel = down ? 0.f : 1.f;
    const F32 since = (F32)(now() - f.gearMovedAt());
    if (since < 0.35f)
    {
        const F32 u = since / 0.35f;
        travel = down ? 1.f - u : u;
    }
    const F32 ky = slot_b + (slot_t - slot_b) * travel;
    lineW(cx, (slot_b + slot_t) * 0.5f, cx, ky, w * 0.07f, LLColor4(0.75f, 0.76f, 0.78f, 1.f));
    const F32 kr = w * 0.17f;
    circleF(cx, ky - 2.f, kr + 1.f, LLColor4(0.f, 0.f, 0.f, 0.5f));
    circleShaded(cx, ky, kr, LLColor4(0.95f, 0.95f, 0.95f, 1.f), LLColor4(0.55f, 0.56f, 0.58f, 1.f));   // the white wheel
    arcW(cx, ky, kr * 0.75f, kr * 0.22f, 0.f, 2.f * PI_F, LLColor4(0.25f, 0.25f, 0.27f, 1.f));          // its tyre groove
    addHit(cx - kr * 1.4f, slot_b - kr, cx + kr * 1.4f, slot_t + kr, H_GEAR,
           "Landing gear: sends your gear command (" + gSavedSettings.getString("WolfFlightGearCommand") + ") — set it on the CDU CONTROLS page");

    // the engine command: a guarded pushbutton
    const F32 eb = b + h * 0.07f, et = b + h * 0.2f;
    const F32 el = l + w * 0.18f, er = r - w * 0.18f;
    const bool pressed = mPressed == H_ENGINE;
    roundRectF(el - 2.f, eb - 2.f, er + 2.f, et + 2.f, w * 0.04f, LLColor4(0.55f, 0.08f, 0.06f, 1.f), LLColor4(0.30f, 0.04f, 0.03f, 1.f));
    roundRectF(el, eb - (pressed ? 1.f : 0.f), er, et - (pressed ? 1.f : 0.f), w * 0.03f, C_BTN_T, C_BTN_B);
    text(mF.mTiny, "ENG", (el + er) * 0.5f, (eb + et) * 0.5f, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
    addHit(el, eb, er, et, H_ENGINE, "Engine: sends your engine command — set it on the CDU CONTROLS page");
}

//-----------------------------------------------------------------------------
// The sidestick and the throttle lever (Paul: "add a joystick please")
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawStick(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;
    const F32 w = r - l, h = t - b;
    roundRectF(l, b, r, t, w * 0.05f, LLColor4(0.22f, 0.235f, 0.255f, 1.f), LLColor4(0.16f, 0.17f, 0.185f, 1.f));
    screw(l + w * 0.06f, t - w * 0.06f, w * 0.022f);
    screw(r - w * 0.06f, t - w * 0.06f, w * 0.022f);
    screw(l + w * 0.06f, b + w * 0.06f, w * 0.022f);
    screw(r - w * 0.06f, b + w * 0.06f, w * 0.022f);

    // ---- the stick, seen from above: a round gate in a leather boot, the grip on top ----
    const F32 sw = w * 0.66f;
    const F32 cx = l + sw * 0.55f, cy = b + h * 0.58f;
    const F32 R = llmin(sw * 0.42f, h * 0.3f);
    mStickCx = cx; mStickCy = cy; mStickR = R;
    circleF(cx, cy, R * 1.18f, C_BEZEL_LO, 40);
    circleShaded(cx, cy, R * 1.12f, LLColor4(0.16f, 0.15f, 0.14f, 1.f), LLColor4(0.05f, 0.05f, 0.05f, 1.f), 40);
    // the boot's folds
    for (S32 i = 1; i <= 3; ++i)
    {
        arcW(cx, cy, R * 1.12f * i / 4.f, 1.f, 0.f, 2.f * PI_F, LLColor4(0.f, 0.f, 0.f, 0.45f), 40);
    }
    lineW(cx - R, cy, cx + R, cy, 1.f, LLColor4(1.f, 1.f, 1.f, 0.12f));
    lineW(cx, cy - R, cx, cy + R, 1.f, LLColor4(1.f, 1.f, 1.f, 0.12f));
    text(mF.mTiny, heli ? "FWD" : "DN", cx, cy + R * 1.2f, C_GREY, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    text(mF.mTiny, heli ? "AFT" : "UP", cx, cy - R * 1.2f, C_GREY, LLFontGL::HCENTER, LLFontGL::TOP);
    // where it is: the mouse's hand if it holds it, else the keys being pressed
    F32 sx = f.stickX(), sy = f.stickY();
    if (mPressed != H_STICK)
    {
        sx = (F32)f.pilotBank();
        sy = (F32)f.pilotPitch();   // + nose up (plane) / + forward (helicopter), as stickY
    }
    // y on the gate: a plane's nose-up is the stick pulled back (down the gate); a helicopter's
    // forward is pushed (up the gate)
    const F32 gy = heli ? sy : -sy;
    const F32 kx = cx + sx * R * 0.85f, ky = cy + gy * R * 0.85f;
    // the autopilot's hand, a green ring, when it is flying
    if (f.apEngaged())
    {
        const F32 ay = heli ? f.outPitch() : -f.outPitch();
        arcW(cx + f.outBank() * R * 0.85f, cy + ay * R * 0.85f, R * 0.3f, 2.f, 0.f, 2.f * PI_F, C_GREEN, 24);
    }
    circleF(kx + 2.f, ky - 3.f, R * 0.36f, LLColor4(0.f, 0.f, 0.f, 0.5f), 32);
    circleShaded(kx, ky, R * 0.34f, LLColor4(0.28f, 0.28f, 0.30f, 1.f), LLColor4(0.06f, 0.06f, 0.07f, 1.f), 32);
    // the grip's top: trim hat and the autopilot disconnect button (red)
    circleShaded(kx - R * 0.1f, ky + R * 0.08f, R * 0.08f, LLColor4(0.5f, 0.5f, 0.52f, 1.f), LLColor4(0.15f, 0.15f, 0.16f, 1.f), 16);
    circleShaded(kx + R * 0.13f, ky + R * 0.12f, R * 0.07f, LLColor4(0.95f, 0.25f, 0.2f, 1.f), LLColor4(0.45f, 0.05f, 0.04f, 1.f), 16);
    addHit(cx - R * 1.15f, cy - R * 1.15f, cx + R * 1.15f, cy + R * 1.15f, H_STICK,
           heli ? "Cyclic: drag to fly forward / back and yaw; let go and it centres. Moving it disconnects the autopilot."
                : "Sidestick: drag to pitch and bank (pull back = nose up); let go and it centres. Moving it disconnects the autopilot.");
    text(mF.mSmall, heli ? "CYCLIC" : "SIDESTICK", cx, b + h * 0.06f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);

    // ---- the throttle lever: spring-centred, + forward ----
    const F32 lx = l + sw + (w - sw) * 0.42f;
    const F32 lt = t - h * 0.14f, lb = b + h * 0.2f;
    mLeverCy = (lt + lb) * 0.5f;
    mLeverHalf = (lt - lb) * 0.5f;
    rectF(lx - w * 0.03f, lb, lx + w * 0.03f, lt, C_BEZEL_LO);
    for (S32 i = -4; i <= 4; ++i)
    {
        const F32 y = mLeverCy + mLeverHalf * i / 4.f;
        lineW(lx + w * 0.05f, y, lx + w * (i == 0 ? 0.11f : 0.08f), y, 1.f, C_GREY);
    }
    text(mF.mSmall, "+", lx, lt + 2.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    text(mF.mSmall, "-", lx, lb - 2.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
    F32 lv = f.throttleLever();
    if (mPressed != H_THROTTLE)
    {
        lv = (F32)f.pilotThrottle();
    }
    const F32 hy = mLeverCy + lv * mLeverHalf * 0.9f;
    const F32 hw = (w - sw) * 0.36f;
    rectF(lx - hw + 2.f, hy - h * 0.035f - 3.f, lx + hw + 2.f, hy + h * 0.035f - 3.f, LLColor4(0.f, 0.f, 0.f, 0.5f));
    roundRectF(lx - hw, hy - h * 0.035f, lx + hw, hy + h * 0.035f, h * 0.012f, LLColor4(0.75f, 0.76f, 0.78f, 1.f), LLColor4(0.38f, 0.39f, 0.41f, 1.f));
    lineW(lx - hw * 0.7f, hy, lx + hw * 0.7f, hy, 1.f, LLColor4(0.2f, 0.2f, 0.22f, 1.f));
    addHit(lx - hw * 1.2f, lb - h * 0.04f, lx + hw * 1.2f, lt + h * 0.04f, H_THROTTLE,
           heli ? "Collective: drag up to climb, down to descend; it springs back to the middle."
                : "Throttle: push up for more power, down for less; it springs back. Uses your throttle keys (CDU CONTROLS).");
    text(mF.mTiny, heli ? "COLL" : "THR", lx, b + h * 0.06f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);
}

void WolfFlightDeck::dragControl(S32 x, S32 y)
{
    WolfFlight& f = WolfFlight::instance();
    const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;
    if (mPressed == H_STICK)
    {
        const F32 dx = ((F32)x - mStickCx) / (mStickR * 0.85f);
        const F32 dy = ((F32)y - mStickCy) / (mStickR * 0.85f);
        // a plane: down the gate is pulled back, nose up (+); a helicopter: up the gate is forward (+);
        // a boat's wheel only turns
        if (f.sailing()) f.setStick(dx, 0.f);
        else f.setStick(dx, heli ? dy : -dy);
    }
    else if (mPressed == H_THROTTLE)
    {
        f.setThrottleLever(((F32)y - mLeverCy) / (mLeverHalf * 0.9f));
    }
}

//-----------------------------------------------------------------------------
// Sailing: the helm, the wind instrument, speed / depth / VMG
//-----------------------------------------------------------------------------

// The ship's wheel (drag it left / right; it centres when let go), the motor lever, and the
// SAIL and ENG command buttons.
void WolfFlightDeck::drawHelm(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const F32 w = r - l, h = t - b;
    // straight on the walnut; a brass plate for the rudder indicator and the buttons
    const F32 sw = w * 0.70f;
    const F32 cx = l + sw * 0.52f, cy = b + h * 0.60f;
    const F32 R = llmin(sw * 0.40f, h * 0.3f);
    mStickCx = cx; mStickCy = cy; mStickR = R;
    // how far the helm is over: the hand on it, else the helm keys, else the autopilot
    F32 helm = (F32)f.pilotBank();
    if (mPressed == H_STICK) helm = f.stickX();
    else if (f.apEngaged()) helm = f.outBank();
    const F32 turn = -helm * 120.f * DEG_TO_RAD;   // clockwise for starboard
    const LLColor4 wood_hi(0.62f, 0.38f, 0.18f, 1.f), wood_lo(0.32f, 0.17f, 0.07f, 1.f);
    // the handles beyond the rim, then the rim, the spokes, the brass hub
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 a = turn + PI_F * 0.5f + 2.f * PI_F * i / 8.f;
        const F32 x1 = cx + R * 0.95f * cosf(a), y1 = cy + R * 0.95f * sinf(a);
        const F32 x2 = cx + R * 1.28f * cosf(a), y2 = cy + R * 1.28f * sinf(a);
        lineW(x1, y1, x2, y2, R * 0.09f, wood_lo);
        circleShaded(x2, y2, R * 0.07f, wood_hi, wood_lo, 12);
        lineW(cx, cy, x1, y1, R * 0.06f, i == 0 ? LLColor4(0.85f, 0.70f, 0.30f, 1.f) : wood_lo);   // the king spoke, brass-bound
    }
    arcW(cx, cy, R * 0.92f, R * 0.13f, 0.f, 2.f * PI_F, wood_lo, 48);
    arcW(cx, cy, R * 0.94f, R * 0.05f, 0.f, 2.f * PI_F, wood_hi, 48);
    circleShaded(cx, cy, R * 0.16f, LLColor4(0.95f, 0.80f, 0.40f, 1.f), LLColor4(0.45f, 0.32f, 0.10f, 1.f), 20);
    // rudder angle indicator above
    {
        const F32 ry = t - h * 0.10f, rw = sw * 0.38f;
        brassFrame(cx - rw - 18.f, ry - 14.f, cx + rw + 18.f, ry + 16.f, 4.f);
        rectG(cx - rw - 14.f, ry - 10.f, cx + rw + 14.f, ry + 12.f, C_CREAM_HI, C_CREAM_LO);
        lineW(cx - rw, ry, cx + rw, ry, 1.5f, C_INK);
        for (S32 i = -2; i <= 2; ++i) lineW(cx + rw * i / 2.f, ry - 3.f, cx + rw * i / 2.f, ry + 3.f, 1.f, C_INK);
        const F32 px = cx + rw * llclamp(helm, -1.f, 1.f);
        triF(px, ry - 2.f, px - 5.f, ry - 10.f, px + 5.f, ry - 10.f, f.apEngaged() ? C_STBD : C_INK);
        text(mF.mTiny, "P", cx - rw - 4.f, ry, C_PORT, LLFontGL::RIGHT, LLFontGL::VCENTER);
        text(mF.mTiny, "S", cx + rw + 4.f, ry, C_STBD, LLFontGL::LEFT, LLFontGL::VCENTER);
        text(mF.mTiny, "RUDDER", cx, ry + 3.f, C_INK, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    }
    addHit(cx - R * 1.3f, cy - R * 1.3f, cx + R * 1.3f, cy + R * 1.3f, H_STICK,
           "Helm: drag left / right to steer (port / starboard); it centres when let go. Turning it puts the autopilot to standby.");

    // the motor lever
    const F32 lx = l + sw + (w - sw) * 0.45f;
    const F32 lt = t - h * 0.16f, lb = b + h * 0.32f;
    mLeverCy = (lt + lb) * 0.5f;
    mLeverHalf = (lt - lb) * 0.5f;
    brassFrame(lx - w * 0.05f, lb - 4.f, lx + w * 0.05f, lt + 4.f, 4.f);
    rectF(lx - w * 0.02f, lb, lx + w * 0.02f, lt, C_INK);
    text(mF.mTiny, "AHD", lx, lt + 6.f, C_CREAM_HI, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    text(mF.mTiny, "AST", lx, lb - 6.f, C_CREAM_HI, LLFontGL::HCENTER, LLFontGL::TOP);
    F32 lv = (mPressed == H_THROTTLE) ? f.throttleLever() : (F32)f.pilotThrottle();
    const F32 hy = mLeverCy + lv * mLeverHalf * 0.9f;
    const F32 hw = (w - sw) * 0.34f;
    roundRectF(lx - hw, hy - h * 0.03f, lx + hw, hy + h * 0.03f, h * 0.01f, LLColor4(0.85f, 0.15f, 0.12f, 1.f), LLColor4(0.45f, 0.06f, 0.05f, 1.f));
    addHit(lx - hw * 1.3f, lb - h * 0.04f, lx + hw * 1.3f, lt + h * 0.04f, H_THROTTLE,
           "Motor: push up for ahead, down for astern; it springs back. Uses your throttle keys (CDU CONTROLS).");

    // SAIL and ENG buttons along the bottom
    const F32 bb = b + h * 0.05f, bt = b + h * 0.17f;
    // The sail button is labelled with the word it says (Paul: "we need a raise button that puts
    // raise in the chat and enter ... that normally puts the sail up"): RAISE by default.
    std::string sail_label = gSavedSettings.getString("WolfSailCommand");
    LLStringUtil::toUpper(sail_label);
    if (sail_label.empty() || sail_label.size() > 8) sail_label = "SAIL";
    const struct { std::string label; EHit id; const char* tip; } btns[2] = {
        { sail_label, H_SAIL, "Says your sail command in open chat (RAISE puts the sail up on most boats); change it on the CDU CONTROLS page" },
        { "ENG", H_ENGINE, "Engine: sends your engine command (CDU CONTROLS page)" } };
    for (S32 i = 0; i < 2; ++i)
    {
        const F32 bl = l + w * (0.06f + 0.46f * i), br = bl + w * 0.40f;
        const bool pressed = mPressed == btns[i].id;
        brassFrame(bl - 3.f, bb - 3.f, br + 3.f, bt + 3.f, 3.f);
        roundRectF(bl, bb - (pressed ? 1.f : 0.f), br, bt - (pressed ? 1.f : 0.f), 3.f, C_CREAM_HI, C_CREAM_LO);
        text(mF.mSmall, btns[i].label, (bl + br) * 0.5f, (bb + bt) * 0.5f, C_INK, LLFontGL::HCENTER, LLFontGL::VCENTER);
        addHit(bl, bb, br, bt, btns[i].id, btns[i].tip);
    }
}

// A yacht's wind instrument: apparent wind needle, true wind pointer, close-hauled sectors.
void WolfFlightDeck::drawWindDial(F32 l, F32 b, F32 r, F32 t)
{
    const WolfFlight::Data& d = WolfFlight::instance().data();
    const F32 w = r - l, h = t - b;
    const F32 cx = (l + r) * 0.5f, cy = (b + t) * 0.5f;
    const F32 R = llmin(w, h) * 0.47f;
    // a brass-cased wind instrument with a cream dial
    circleF(cx + 3.f, cy - 4.f, R * 1.06f, LLColor4(0.f, 0.f, 0.f, 0.45f), 64);
    brassRing(cx, cy, R * 1.06f, R * 0.11f);
    circleShaded(cx, cy, R * 0.955f, C_CREAM_HI, C_CREAM_LO, 64);
    // screen angle for a wind angle off the bow (+ starboard, clockwise from the top)
    auto ang = [](F32 a) { return (90.f - a) * DEG_TO_RAD; };
    // close-hauled sectors: red to port, green to starboard
    arcW(cx, cy, R * 0.86f, R * 0.07f, ang(60.f), ang(20.f), C_STBD, 16);
    arcW(cx, cy, R * 0.86f, R * 0.07f, ang(-20.f), ang(-60.f), C_PORT, 16);
    for (S32 k = 0; k < 360; k += 10)
    {
        const F32 a = ang((F32)k);
        const F32 in = (k % 30 == 0) ? 0.80f : 0.86f;
        lineW(cx + R * in * cosf(a), cy + R * in * sinf(a), cx + R * 0.93f * cosf(a), cy + R * 0.93f * sinf(a), (k % 30 == 0) ? 2.f : 1.f, C_INK);
        if (k % 30 == 0 && k != 0)
        {
            const S32 lbl = k <= 180 ? k : 360 - k;
            text(mF.mSmall, fmt("%d", lbl), cx + R * 0.70f * cosf(a), cy + R * 0.70f * sinf(a), C_INK, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
    // the boat, bow up
    {
        const F32 s = R * 0.22f;
        const LLColor4 hull(0.55f, 0.50f, 0.42f, 1.f);
        triF(cx, cy + s * 1.2f, cx - s * 0.45f, cy + s * 0.2f, cx + s * 0.45f, cy + s * 0.2f, hull);
        quadF(cx - s * 0.45f, cy + s * 0.2f, cx + s * 0.45f, cy + s * 0.2f, cx + s * 0.35f, cy - s * 1.0f, cx - s * 0.35f, cy - s * 1.0f, hull);
    }
    if (!d.mValid || d.mTWS < 0.05f)
    {
        text(mF.mMed, "NO WIND", cx, cy - R * 0.45f, C_PORT, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
    else
    {
        // true wind: a cyan pointer with T
        const F32 ta = ang(d.mTWA);
        const F32 tx = cx + R * 0.78f * cosf(ta), ty = cy + R * 0.78f * sinf(ta);
        triF(tx, ty, cx + R * 0.62f * cosf(ta + 0.08f), cy + R * 0.62f * sinf(ta + 0.08f),
             cx + R * 0.62f * cosf(ta - 0.08f), cy + R * 0.62f * sinf(ta - 0.08f), C_NAVY);
        text(mF.mTiny, "T", cx + R * 0.56f * cosf(ta), cy + R * 0.56f * sinf(ta), C_NAVY, LLFontGL::HCENTER, LLFontGL::VCENTER);
        // apparent wind: the long black needle, its tip red, on a brass boss
        const F32 aa = ang(d.mAWA);
        lineW(cx - R * 0.15f * cosf(aa), cy - R * 0.15f * sinf(aa), cx + R * 0.66f * cosf(aa), cy + R * 0.66f * sinf(aa), llmax(2.f, R * 0.035f), C_INK);
        lineW(cx + R * 0.66f * cosf(aa), cy + R * 0.66f * sinf(aa), cx + R * 0.80f * cosf(aa), cy + R * 0.80f * sinf(aa), llmax(2.f, R * 0.035f), C_PORT);
        circleShaded(cx, cy, R * 0.06f, C_BRASS_HI, C_BRASS_LO, 16);
    }
    // the readouts
    const F32 kt = 1.943844f;
    text(mF.mTiny, "APPARENT", cx, cy + R * 0.42f, C_INK, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    text(mF.mMonoMed, fmt("%.1f KT", d.mAWS * kt), cx, cy + R * 0.40f, C_INK, LLFontGL::HCENTER, LLFontGL::TOP);
    text(mF.mMonoSmall, fmt("AWA %d%c", (S32)llround(fabsf(d.mAWA)), d.mAWA >= 0.f ? 'S' : 'P'), cx, cy - R * 0.30f, C_INK, LLFontGL::HCENTER, LLFontGL::TOP);
    text(mF.mMonoSmall, fmt("TRUE %.1f KT  %03d", d.mTWS * kt, (S32)llround(d.mTWD) % 360), cx, cy - R * 0.44f, C_NAVY, LLFontGL::HCENTER, LLFontGL::TOP);
}

// Speed, depth and VMG, each in its own black display like a mast-mounted repeater.
void WolfFlightDeck::drawSailData(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    const F32 h = t - b;
    const F32 kt = 1.943844f;
    const F32 draft = gSavedSettings.getF32("WolfSailDraft");
    struct Box { const char* title; std::string big; std::string small; LLColor4 col; };
    Box boxes[3];
    boxes[0] = { "BOAT SPEED  KT", fmt("%.1f", d.mGS * kt), fmt("SOG %.1f  COG %03d", d.mGS * kt, (S32)llround(d.mTrack) % 360), C_WHITE };
    {
        const bool shallow = d.mDepth < draft + 0.5f;
        boxes[1] = { "DEPTH  M", d.mDepth > 99.f ? fmt("%.0f", d.mDepth) : fmt("%.1f", d.mDepth),
                     shallow ? std::string("SHALLOW") : fmt("DRAFT %.1f", draft), shallow ? (blink(0.6f) ? C_RED : C_AMBER) : C_WHITE };
    }
    if (f.dest().mValid)
    {
        const LLVector3d wp = f.activeWaypoint();
        const F64 dx = wp.mdV[VX] - d.mPosGlobal.mdV[VX], dy = wp.mdV[VY] - d.mPosGlobal.mdV[VY];
        const F32 brg = (F32)(atan2(dx, dy) * RAD_TO_DEG);
        const F32 vmg = d.mGS * cosf((d.mTrack - brg) * DEG_TO_RAD);
        boxes[2] = { "VMG TO WPT  KT", fmt("%.1f", vmg * kt), fmt("HEEL %d%c", (S32)llround(fabsf(d.mRoll)), d.mRoll >= 0.f ? 'S' : 'P'), vmg < 0.f ? C_AMBER : C_WHITE };
    }
    else
    {
        const F32 vmg = d.mGS * cosf(d.mTWA * DEG_TO_RAD);
        boxes[2] = { "VMG WIND  KT", fmt("%.1f", vmg * kt), fmt("HEEL %d%c", (S32)llround(fabsf(d.mRoll)), d.mRoll >= 0.f ? 'S' : 'P'), C_WHITE };
    }
    const F32 bh = h / 3.f;
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 bt = t - bh * i - 2.f, bb = bt - bh + 4.f;
        // a brass-cased repeater with a grey-green LCD, as on classic yachts
        brassFrame(l, bb, r, bt, 7.f);
        rectG(l + 7.f, bb + 7.f, r - 7.f, bt - 7.f, C_LCD_HI, C_LCD_LO);
        const LLColor4 ink = (boxes[i].col == C_WHITE) ? C_LCD_INK : boxes[i].col;
        text(mF.mTiny, boxes[i].title, l + 11.f, bt - 9.f, C_LCD_INK, LLFontGL::LEFT, LLFontGL::TOP);
        text(mF.mMonoHuge, boxes[i].big, r - 12.f, (bb + bt) * 0.5f, ink, LLFontGL::RIGHT, LLFontGL::VCENTER);
        text(mF.mTiny, boxes[i].small, l + 11.f, bb + 9.f, C_LCD_INK, LLFontGL::LEFT, LLFontGL::BOTTOM);
    }
}

//-----------------------------------------------------------------------------
// PLAN: the whole route, north up, on the world map (Paul: "a map on plane and boat showing the
// route its planning to take and where it is on that route")
//-----------------------------------------------------------------------------

// World map tiles under a north-up view: centre (global) at the screen's middle, ppm pixels per
// metre. Source: llworldmapview.cpp drawMipmapLevel — the same tiles, levels and corners.
void WolfFlightDeck::drawTiles(F32 sl, F32 sb, F32 sr, F32 st, const LLVector3d& centre, F32 ppm)
{
    const F32 cx = (sl + sr) * 0.5f, cy = (sb + st) * 0.5f;
    const S32 level = llclamp(LLWorldMipmap::scaleToLevel(ppm * REGION_WIDTH_METERS), 1, LLWorldMipmap::MAP_LEVELS);
    const F64 tile_m = (F64)LLWorldMipmap::MAP_TILE_SIZE * (1 << (level - 1));
    const F64 x0 = centre.mdV[VX] - (cx - sl) / ppm, x1 = centre.mdV[VX] + (sr - cx) / ppm;
    const F64 y0 = centre.mdV[VY] - (cy - sb) / ppm, y1 = centre.mdV[VY] + (st - cy) / ppm;
    LLWorldMap* map = LLWorldMap::getInstance();
    for (F64 gy = y0 - tile_m; gy < y1 + tile_m; gy += tile_m)
    {
        for (F64 gx = x0 - tile_m; gx < x1 + tile_m; gx += tile_m)
        {
            U32 tx = 0, ty = 0;
            LLWorldMipmap::globalToMipmap(gx, gy, level, &tx, &ty);
            LLPointer<LLViewerFetchedTexture> tex = map->getObjectsTile(tx, ty, level, true);
            if (!tex || !tex->hasGLTexture())
            {
                continue;
            }
            const F32 left = cx + (F32)((F64)tx * REGION_WIDTH_METERS - centre.mdV[VX]) * ppm;
            const F32 bottom = cy + (F32)((F64)ty * REGION_WIDTH_METERS - centre.mdV[VY]) * ppm;
            const F32 right = left + (F32)tile_m * ppm, top = bottom + (F32)tile_m * ppm;
            gGL.getTexUnit(0)->bind(tex);
            tex->setAddressMode(LLTexUnit::TAM_CLAMP);
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            gGL.begin(LLRender::TRIANGLES);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(left, top);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(left, bottom);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(right, bottom);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(left, top);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(right, bottom);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(right, top);
            gGL.end();
        }
    }
    noTex();
}

void WolfFlightDeck::drawPlan(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    const WolfFlight::Route& route = f.route();
    const bool sail = f.sailing();
    F32 sl, sb, sr, st;
    drawBezel(l, b, r, t, (r - l) * 0.045f, sl, sb, sr, st);
    const F32 w = sr - sl, h = st - sb;
    addHit(sl, sb, sr, st, H_ND_SCREEN, sail ? "Click for the CHART view (course up, around you)" : "Click for the MAP view (heading up, around you)");
    LLLocalClipRect screen_clip(LLRect((S32)sl, (S32)st, (S32)sr, (S32)sb));
    text(mF.mSmall, "PLAN", sr - w * 0.03f, sb + h * 0.02f, C_GREEN, LLFontGL::RIGHT, LLFontGL::BOTTOM);
    if (!d.mValid || !route.mValid)
    {
        const char* why = route.mNoWay ? "NO WATER ROUTE" : (f.dest().mPending ? "LOOKING UP DESTINATION" : "NO ROUTE");
        text(mF.mMed, why, sl + w * 0.5f, sb + h * 0.55f, route.mNoWay ? C_AMBER : C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
        text(mF.mSmall, "Type a destination on the CDU and press ENTER", sl + w * 0.5f, sb + h * 0.45f, C_GREY, LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }
    // the map area; a plane gets the vertical profile under it
    const F32 mb = sail ? sb : sb + h * 0.30f;
    const F32 mt = st - h * 0.11f;
    // fit the route and the craft, with a margin
    F64 minx = d.mPosGlobal.mdV[VX], maxx = minx, miny = d.mPosGlobal.mdV[VY], maxy = miny;
    for (const LLVector3d& p : route.mPts)
    {
        minx = llmin(minx, p.mdV[VX]); maxx = llmax(maxx, p.mdV[VX]);
        miny = llmin(miny, p.mdV[VY]); maxy = llmax(maxy, p.mdV[VY]);
    }
    const F64 span = llmax(300.0, llmax(maxx - minx, (maxy - miny) * w / (mt - mb)));
    const F32 ppm = (F32)((w * 0.86) / span);
    const LLVector3d centre((minx + maxx) * 0.5, (miny + maxy) * 0.5, 0.0);
    const F32 cx = sl + w * 0.5f, cy = (mb + mt) * 0.5f;
    {
        LLLocalClipRect map_clip(LLRect((S32)sl, (S32)mt, (S32)sr, (S32)mb));
        drawTiles(sl, mb, sr, mt, centre, ppm);
        rectF(sl, mb, sr, mt, LLColor4(0.f, 0.f, 0.f, 0.30f));   // dimmed so the route reads
        auto toS = [&](const LLVector3d& p, F32& x, F32& y)
        {
            x = cx + (F32)(p.mdV[VX] - centre.mdV[VX]) * ppm;
            y = cy + (F32)(p.mdV[VY] - centre.mdV[VY]) * ppm;
        };
        // the legs: flown grey, the active one magenta, those to come white
        for (size_t i = 1; i < route.mPts.size(); ++i)
        {
            F32 x1, y1, x2, y2;
            toS(route.mPts[i - 1], x1, y1);
            toS(route.mPts[i], x2, y2);
            if ((S32)i < route.mLeg) lineW(x1, y1, x2, y2, 2.f, C_DIM);
            else if ((S32)i == route.mLeg) lineW(x1, y1, x2, y2, 3.f, C_MAGENTA);
            else lineW(x1, y1, x2, y2, 2.f, C_WHITE);
        }
        // the turning points
        for (size_t i = 0; i < route.mPts.size(); ++i)
        {
            F32 x, y;
            toS(route.mPts[i], x, y);
            const bool active = (S32)i == route.mLeg;
            const LLColor4 c = active ? C_MAGENTA : C_WHITE;
            const F32 s = 6.f;
            if (i == 0)
            {
                arcW(x, y, s, 1.5f, 0.f, 2.f * PI_F, C_GREEN, 16);
                text(mF.mTiny, "START", x + s + 2.f, y, C_GREEN, LLFontGL::LEFT, LLFontGL::VCENTER);
                continue;
            }
            quadF(x, y + s, x + s * 0.3f, y, x, y - s, x - s * 0.3f, y, c);
            quadF(x + s, y, x, y - s * 0.3f, x - s, y, x, y + s * 0.3f, c);
            const bool last = i == route.mPts.size() - 1;
            text(mF.mTiny, last ? f.dest().mRegion : fmt("WP%d", (S32)i), x + s + 2.f, y, c, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        // the craft: a white arrow pointing its heading
        {
            F32 x, y;
            toS(d.mPosGlobal, x, y);
            const F32 a = (90.f - d.mHeading) * DEG_TO_RAD, s = 9.f;
            const F32 fx = cosf(a), fy = sinf(a);
            if (sail)
            {
                // the boat: bow, beam and stern turned to the heading
                auto P = [&](F32 along, F32 across, F32& ox, F32& oy) { ox = x + fx * along + fy * across; oy = y + fy * along - fx * across; };
                F32 bx, by, l1x, l1y, r1x, r1y, l2x, l2y, r2x, r2y;
                P(s * 1.3f, 0.f, bx, by);
                P(s * 0.3f, -s * 0.45f, l1x, l1y); P(s * 0.3f, s * 0.45f, r1x, r1y);
                P(-s * 1.0f, -s * 0.38f, l2x, l2y); P(-s * 1.0f, s * 0.38f, r2x, r2y);
                triF(bx, by, l1x, l1y, r1x, r1y, C_WHITE);
                quadF(l1x, l1y, r1x, r1y, r2x, r2y, l2x, l2y, C_WHITE);
            }
            else
            {
                triF(x + fx * s, y + fy * s, x - fx * s * 0.7f - fy * s * 0.6f, y - fy * s * 0.7f + fx * s * 0.6f,
                     x - fx * s * 0.7f + fy * s * 0.6f, y - fy * s * 0.7f - fx * s * 0.6f, C_WHITE);
            }
            arcW(x, y, s * 1.6f, 1.f, 0.f, 2.f * PI_F, C_WHITE, 20);
        }
        // north
        text(mF.mMed, "N", sr - w * 0.05f, mt - 4.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
        triF(sr - w * 0.05f, mt - h * 0.075f, sr - w * 0.05f - 4.f, mt - h * 0.11f, sr - w * 0.05f + 4.f, mt - h * 0.11f, C_WHITE);
        // scale bar
        const F32 bar_m = (F32)pow(10.0, floor(log10(span * 0.25)));
        lineW(sl + w * 0.04f, mb + 10.f, sl + w * 0.04f + bar_m * ppm, mb + 10.f, 2.f, C_WHITE);
        text(mF.mTiny, bar_m >= 1000.f ? fmt("%g km", bar_m / 1000.f) : fmt("%g m", bar_m), sl + w * 0.04f, mb + 13.f, C_WHITE, LLFontGL::LEFT, LLFontGL::BOTTOM);
    }
    // progress along the route
    {
        const F32 total = f.routeTotal(), flown = f.routeFlown();
        const F32 pct = total > 1.f ? llclamp(flown / total, 0.f, 1.f) : 0.f;
        const F32 togo = llmax(0.f, total - flown);
        const S32 legs = (S32)route.mPts.size() - 1;
        text(mF.mSmall, fmt("WPT %d/%d   %s TO GO   %d%%", llmin(route.mLeg, legs), legs,
                             togo < NM * 10.f ? fmt("%.2f NM", togo / NM).c_str() : fmt("%.0f NM", togo / NM).c_str(), (S32)llround(pct * 100.f)),
             sl + w * 0.03f, st - h * 0.02f, C_WHITE, LLFontGL::LEFT, LLFontGL::TOP);
        const F32 by = st - h * 0.085f;
        rectF(sl + w * 0.03f, by, sr - w * 0.03f, by + 4.f, C_DIM);
        rectF(sl + w * 0.03f, by, sl + w * 0.03f + (w * 0.94f) * pct, by + 4.f, C_MAGENTA);
        if (route.mUnsurveyed)
        {
            text(mF.mTiny, "PART UNSURVEYED - REPLANS AS LAND LOADS", sr - w * 0.03f, by - 3.f, C_AMBER, LLFontGL::RIGHT, LLFontGL::TOP);
        }
    }
    if (sail)
    {
        return;
    }

    // ---- the vertical profile: land along the route, the planned climb / cruise / descent ----
    const F32 pb = sb + h * 0.03f, pt = sb + h * 0.27f;
    const F32 pl = sl + w * 0.10f, pr = sr - w * 0.03f;
    rectF(sl, pt + 2.f, sr, pt + 3.f, C_DIM);
    const F32 total = f.routeTotal();
    if (total < 1.f)
    {
        return;
    }
    const F32 FT_ = 3.280840f;
    const S32 N = 90;
    std::vector<F32> terr(N + 1, -1e9f);
    F32 lo = 1e9f, hi = -1e9f;
    // walk the legs to sample the land under the route
    {
        F32 acc = 0.f;
        S32 si = 0;
        for (size_t i = 1; i < route.mPts.size() && si <= N; ++i)
        {
            const LLVector3d a = route.mPts[i - 1], bpt = route.mPts[i];
            const F64 lx = bpt.mdV[VX] - a.mdV[VX], ly = bpt.mdV[VY] - a.mdV[VY];
            const F32 len = (F32)sqrt(lx * lx + ly * ly);
            while (si <= N && total * si / N <= acc + len + 0.001f)
            {
                const F32 u = len > 0.f ? (total * si / N - acc) / len : 0.f;
                const LLVector3d p(a.mdV[VX] + lx * u, a.mdV[VY] + ly * u, 0.0);
                if (LLWorld::getInstance()->getRegionFromPosGlobal(p))
                {
                    terr[si] = (llmax(LLWorld::getInstance()->resolveLandHeightGlobal(p), d.mWaterZ) - d.mWaterZ) * FT_;
                    lo = llmin(lo, terr[si]);
                    hi = llmax(hi, terr[si]);
                }
                ++si;
            }
            acc += len;
        }
    }
    const WolfFlight::Dest& dest = f.dest();
    const F32 cruise_ft = (f.vertical() == WolfFlight::VERT_VNAV) ? f.cruiseAltFt() : f.selAltFt();
    const F32 dest_ft = dest.mHasZ ? ((F32)dest.mGlobal.mdV[VZ] - d.mWaterZ) * FT_ : cruise_ft;
    const F32 cur_ft = d.mAltMSL * FT_;
    const F32 flown = f.routeFlown();
    lo = llmin(lo, llmin(0.f, llmin(dest_ft, cur_ft)));
    hi = llmax(hi, llmax(cruise_ft, llmax(cur_ft, dest_ft)));
    lo -= 50.f;
    hi += 300.f;
    auto px = [&](F32 along) { return pl + (pr - pl) * llclamp(along / total, 0.f, 1.f); };
    auto py = [&](F32 ft) { return pb + (pt - pb) * (ft - lo) / (hi - lo); };
    // the land
    for (S32 i = 0; i < N; ++i)
    {
        if (terr[i] < -1e8f || terr[i + 1] < -1e8f) continue;
        const F32 x1 = px(total * i / N), x2 = px(total * (i + 1) / N);
        quadF(x1, pb, x2, pb, x2, py(terr[i + 1]), x1, py(terr[i]), LLColor4(0.45f, 0.30f, 0.12f, 1.f));
    }
    // the planned path: from here up (or down) to cruise, then the descent to the destination
    const F32 tod_m = llmax(0.f, (cruise_ft - dest_ft) / FT_) * 12.f + 200.f;   // WolfFlight::targetAltitudeZ, a plane
    const F32 tod_at = llmax(flown, total - tod_m);
    const F32 climb_end = llmin(tod_at, flown + fabsf(cruise_ft - cur_ft) / FT_ * 8.f);
    lineW(px(flown), py(cur_ft), px(climb_end), py(cruise_ft), 2.f, C_MAGENTA);
    lineW(px(climb_end), py(cruise_ft), px(tod_at), py(cruise_ft), 2.f, C_MAGENTA);
    lineW(px(tod_at), py(cruise_ft), px(total), py(dest_ft), 2.f, C_MAGENTA);
    text(mF.mTiny, "T/D", px(tod_at), py(cruise_ft) + 3.f, C_GREEN, LLFontGL::HCENTER, LLFontGL::BOTTOM);
    // the aircraft
    {
        const F32 x = px(flown), y = py(cur_ft);
        triF(x + 8.f, y, x - 6.f, y + 5.f, x - 6.f, y - 5.f, C_WHITE);
    }
    // scale
    text(mF.mTiny, fmt("%d", (S32)llround(hi)), pl - 4.f, pt, C_WHITE, LLFontGL::RIGHT, LLFontGL::TOP);
    text(mF.mTiny, fmt("%d", (S32)llround(lo)), pl - 4.f, pb, C_WHITE, LLFontGL::RIGHT, LLFontGL::BOTTOM);
    text(mF.mTiny, "FT", pl - 4.f, (pb + pt) * 0.5f, C_CYAN, LLFontGL::RIGHT, LLFontGL::VCENTER);
}

//-----------------------------------------------------------------------------
// PFD
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawPFD(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    F32 sl, sb, sr, st;
    drawBezel(l, b, r, t, (r - l) * 0.045f, sl, sb, sr, st);
    const F32 w = sr - sl, h = st - sb;
    LLLocalClipRect screen_clip(LLRect((S32)sl, (S32)st, (S32)sr, (S32)sb));
    if (!d.mValid)
    {
        text(mF.mLarge, "ATT", sl + w * 0.5f, sb + h * 0.5f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }
    const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;

    // ---- the flight mode annunciator along the top ----
    {
        const F32 fb = st - h * 0.085f;
        const char* at = f.atEngaged() ? (heli ? "SPD" : "SPD") : "";
        const char* lat = "";
        switch (f.lateral())
        {
        case WolfFlight::LAT_HDG: lat = "HDG SEL"; break;
        case WolfFlight::LAT_LNAV: lat = "LNAV"; break;
        case WolfFlight::LAT_HOLD: lat = "HOLD"; break;
        default: break;
        }
        std::string vert;
        const F32 sel_err_ft = fabsf(d.mAltMSL * FT - f.selAltFt());
        switch (f.vertical())
        {
        case WolfFlight::VERT_ALT: vert = sel_err_ft > 150.f ? "FLCH" : "ALT HOLD"; break;
        case WolfFlight::VERT_VS: vert = "V/S"; break;
        case WolfFlight::VERT_VNAV: vert = "VNAV PTH"; break;
        case WolfFlight::VERT_HOVER: vert = "HOVER"; break;
        default: break;
        }
        const F32 cw = w * 0.22f;
        const F32 x0 = sl + w * 0.17f;
        const char* cols[3] = { at, lat, vert.c_str() };
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 cxm = x0 + cw * (i + 0.5f);
            if (i > 0)
            {
                rectF(x0 + cw * i, fb + 2.f, x0 + cw * i + 1.f, st - 2.f, C_DIM);
            }
            text(mF.mMed, cols[i], cxm, (fb + st) * 0.5f, C_GREEN, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
        // autopilot status, just under the annunciator (Boeing: "CMD"; flashing red when it trips)
        const F32 ay = fb - h * 0.045f;
        if (f.apEngaged())
        {
            text(mF.mLarge, "CMD", sl + w * 0.5f, ay, C_GREEN, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
        else if (now() < f.apOffFlashUntil())
        {
            if (blink(0.5f))
            {
                const F32 bw = w * 0.13f;
                rectF(sl + w * 0.5f - bw, ay - h * 0.025f, sl + w * 0.5f + bw, ay + h * 0.025f, C_RED);
                text(mF.mMed, "AP DISC", sl + w * 0.5f, ay, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
        }
        else if (f.fdValid())
        {
            text(mF.mMed, "FD", sl + w * 0.5f, ay, C_GREEN, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }

    // ---- attitude ----
    const F32 cx = sl + w * 0.47f, cy = sb + h * 0.50f;
    const F32 aw = w * 0.235f, ah = h * 0.255f;
    const F32 ppd = ah / 22.f;   // pixels per degree of pitch
    {
        LLLocalClipRect adi_clip(LLRect((S32)(cx - aw), (S32)(cy + ah), (S32)(cx + aw), (S32)(cy - ah)));
        const Rot rot(cx, cy, -d.mRoll);
        const F32 y0 = -d.mPitch * ppd;
        const F32 L = (aw + ah) * 3.f;
        F32 x1, y1, x2, y2, x3, y3, x4, y4;
        // sky: lighter at the horizon
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        auto sv = [&](F32 x, F32 y, const LLColor4& c) { F32 ox, oy; rot.p(x, y, ox, oy); vtx(ox, oy, c); };
        sv(-L, y0, C_SKY_LO); sv(L, y0, C_SKY_LO); sv(L, y0 + ah * 1.6f, C_SKY_HI);
        sv(-L, y0, C_SKY_LO); sv(L, y0 + ah * 1.6f, C_SKY_HI); sv(-L, y0 + ah * 1.6f, C_SKY_HI);
        sv(-L, y0 + ah * 1.6f, C_SKY_HI); sv(L, y0 + ah * 1.6f, C_SKY_HI); sv(L, y0 + L, C_SKY_HI);
        sv(-L, y0 + ah * 1.6f, C_SKY_HI); sv(L, y0 + L, C_SKY_HI); sv(-L, y0 + L, C_SKY_HI);
        // ground: darker away from the horizon
        sv(-L, y0, C_GND_HI); sv(L, y0, C_GND_HI); sv(L, y0 - ah * 1.6f, C_GND_LO);
        sv(-L, y0, C_GND_HI); sv(L, y0 - ah * 1.6f, C_GND_LO); sv(-L, y0 - ah * 1.6f, C_GND_LO);
        sv(-L, y0 - ah * 1.6f, C_GND_LO); sv(L, y0 - ah * 1.6f, C_GND_LO); sv(L, y0 - L, C_GND_LO);
        sv(-L, y0 - ah * 1.6f, C_GND_LO); sv(L, y0 - L, C_GND_LO); sv(-L, y0 - L, C_GND_LO);
        gGL.end();
        // horizon
        rot.p(-L, y0, x1, y1); rot.p(L, y0, x2, y2);
        lineW(x1, y1, x2, y2, 2.f, C_WHITE);
        // pitch ladder, every 2.5 degrees, numbered every 10
        for (S32 i = -36; i <= 36; ++i)
        {
            if (i == 0) continue;
            const F32 p = i * 2.5f;
            if (fabsf(p - d.mPitch) > 24.f) continue;
            const F32 y = y0 + p * ppd;
            const bool ten = (i % 4) == 0;
            const bool five = (i % 2) == 0;
            const F32 half = ten ? aw * 0.30f : five ? aw * 0.15f : aw * 0.07f;
            rot.p(-half, y, x1, y1); rot.p(half, y, x2, y2);
            lineW(x1, y1, x2, y2, ten ? 1.8f : 1.3f, C_WHITE);
            if (ten)
            {
                rot.p(-half - aw * 0.12f, y, x3, y3); rot.p(half + aw * 0.12f, y, x4, y4);
                const std::string n = fmt("%d", llabs((S32)p));
                text(mF.mSmall, n, x3, y3, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
                text(mF.mSmall, n, x4, y4, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
        }
        // the bank pointer and slip indicator, turning with the sphere, under the fixed scale
        {
            const F32 rr = ah * 0.92f;
            F32 px, py, qx, qy, ux, uy;
            rot.p(0.f, rr, px, py);
            rot.p(-aw * 0.05f, rr - aw * 0.09f, qx, qy);
            rot.p(aw * 0.05f, rr - aw * 0.09f, ux, uy);
            triF(px, py, qx, qy, ux, uy, C_WHITE);
            const F32 slip = llclamp(-d.mSlip / 10.f, -1.f, 1.f) * aw * 0.08f;
            F32 a1x, a1y, a2x, a2y, a3x, a3y, a4x, a4y;
            rot.p(slip - aw * 0.06f, rr - aw * 0.11f, a1x, a1y);
            rot.p(slip + aw * 0.06f, rr - aw * 0.11f, a2x, a2y);
            rot.p(slip + aw * 0.07f, rr - aw * 0.15f, a3x, a3y);
            rot.p(slip - aw * 0.07f, rr - aw * 0.15f, a4x, a4y);
            quadF(a1x, a1y, a2x, a2y, a3x, a3y, a4x, a4y, C_WHITE);
        }
    }
    // the ADI's frame and the fixed bank scale
    frameW(cx - aw - 1.f, cy - ah - 1.f, cx + aw + 1.f, cy + ah + 1.f, 1.f, C_BLACK);
    {
        const F32 rr = ah * 0.92f;
        arcW(cx, cy, rr, 1.5f, (90.f - 60.f) * DEG_TO_RAD, (90.f + 60.f) * DEG_TO_RAD, C_WHITE, 40);
        const F32 ticks[] = { -60.f, -45.f, -30.f, -20.f, -10.f, 10.f, 20.f, 30.f, 45.f, 60.f };
        for (F32 a : ticks)
        {
            const F32 ra = (90.f + a) * DEG_TO_RAD;
            const F32 len = (fabsf(a) == 30.f || fabsf(a) == 60.f) ? aw * 0.09f : aw * 0.05f;
            if (fabsf(a) == 45.f)
            {
                const F32 bx = cx + (rr + aw * 0.02f) * cosf(ra), by = cy + (rr + aw * 0.02f) * sinf(ra);
                triF(bx, by, bx + aw * 0.03f * cosf(ra + 0.6f), by + aw * 0.06f * sinf(ra + 0.6f),
                     bx + aw * 0.03f * cosf(ra - 0.6f), by + aw * 0.06f * sinf(ra - 0.6f), C_WHITE);
                continue;
            }
            lineW(cx + rr * cosf(ra), cy + rr * sinf(ra), cx + (rr + len) * cosf(ra), cy + (rr + len) * sinf(ra), 1.5f, C_WHITE);
        }
        // the fixed zero index
        triF(cx, cy + rr, cx - aw * 0.05f, cy + rr + aw * 0.08f, cx + aw * 0.05f, cy + rr + aw * 0.08f, C_WHITE);
        if (fabsf(d.mRoll) > 35.f)
        {
            text(mF.mMed, "BANK ANGLE", cx, cy + ah * 0.55f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
    // flight director: magenta bars toward where the guidance wants the nose
    if (f.fdValid())
    {
        const F32 dy = llclamp((f.fdPitch() - d.mPitch) * ppd, -ah * 0.7f, ah * 0.7f);
        const F32 dx = llclamp((f.fdRoll() - d.mRoll) * aw / 35.f, -aw * 0.7f, aw * 0.7f);
        lineW(cx - aw * 0.42f, cy + dy, cx + aw * 0.42f, cy + dy, 3.f, C_MAGENTA);
        if (!heli)
        {
            lineW(cx + dx, cy - ah * 0.55f, cx + dx, cy + ah * 0.55f, 3.f, C_MAGENTA);
        }
    }
    // the aircraft symbol: black with a white edge, two wings and a centre square
    {
        const F32 s = aw * 0.05f;
        auto wing = [&](F32 sgn)
        {
            const F32 x0 = cx + sgn * aw * 0.32f, x1 = cx + sgn * aw * 0.72f;
            rectF(llmin(x0, x1) - 1.5f, cy - s * 0.5f - 1.5f, llmax(x0, x1) + 1.5f, cy + s * 0.5f + 1.5f, C_WHITE);
            rectF(x0 - (sgn > 0 ? 1.5f : -1.5f) - (sgn > 0 ? 0.f : s), cy - s * 2.f - 1.5f, x0 + (sgn > 0 ? s : 0.f) + 1.5f, cy + 1.5f, C_WHITE);
            rectF(llmin(x0, x1), cy - s * 0.5f, llmax(x0, x1), cy + s * 0.5f, C_BLACK);
            rectF(x0 - (sgn > 0 ? 0.f : s), cy - s * 2.f, x0 + (sgn > 0 ? s : 0.f), cy, C_BLACK);
        };
        wing(-1.f);
        wing(1.f);
        rectF(cx - s - 1.5f, cy - s - 1.5f, cx + s + 1.5f, cy + s + 1.5f, C_WHITE);
        rectF(cx - s, cy - s, cx + s, cy + s, C_BLACK);
    }
    // radio altitude, low down
    {
        const F32 ra_ft = d.mAGL * FT;
        if (ra_ft < 2500.f)
        {
            text(mF.mSmall, "RADIO", cx + aw * 0.62f, cy - ah * 0.72f, C_WHITE, LLFontGL::HCENTER, LLFontGL::BOTTOM);
            text(mF.mMonoMed, fmt("%d", (S32)llround(llmax(ra_ft, 0.f) / (ra_ft < 100.f ? 2.f : 10.f)) * (ra_ft < 100.f ? 2 : 10)),
                 cx + aw * 0.62f, cy - ah * 0.74f, ra_ft < 200.f ? C_AMBER : C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
        }
    }

    // ---- speed tape ----
    {
        const F32 tl = sl + w * 0.04f, tr = sl + w * 0.165f;
        const F32 tb = cy - h * 0.31f, tt = cy + h * 0.31f;
        const F32 spd = d.mAirspeed * KT;
        const F32 ppk = (tt - tb) / 90.f;
        {
            LLLocalClipRect clip(LLRect((S32)tl, (S32)tt, (S32)tr + 12, (S32)tb));
            rectF(tl, tb, tr, tt, C_TAPE);
            for (S32 k = ((S32)(spd - 50.f) / 10) * 10; k <= spd + 50.f; k += 10)
            {
                if (k < 0) continue;
                const F32 y = cy + (k - spd) * ppk;
                lineW(tr - w * 0.018f, y, tr, y, 1.5f, C_WHITE);
                if (k % 20 == 0)
                {
                    text(mF.mMonoSmall, fmt("%d", k), tr - w * 0.024f, y, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
                }
            }
            // selected speed bug
            const F32 by = llclamp(cy + (f.selSpeedKt() - spd) * ppk, tb, tt);
            rectF(tr - 1.f, by - h * 0.012f, tr + w * 0.012f, by + h * 0.012f, C_MAGENTA);
            rectF(tr - 1.f, by - h * 0.004f, tr + w * 0.006f, by + h * 0.004f, C_TAPE);
        }
        // trend vector: where the speed will be in 10 s
        const F32 trend = d.mSpeedTrend * KT;
        if (fabsf(trend) > 1.f)
        {
            const F32 ty = llclamp(cy + trend * ppk, tb, tt);
            lineW(tr + w * 0.006f, cy, tr + w * 0.006f, ty, 2.f, C_GREEN);
            const F32 dir = trend > 0.f ? 1.f : -1.f;
            triF(tr + w * 0.006f, ty, tr - w * 0.002f, ty - dir * h * 0.015f, tr + w * 0.014f, ty - dir * h * 0.015f, C_GREEN);
        }
        // the readout, its units digit rolling like a drum
        const F32 bh = h * 0.045f;
        const F32 bl = tl - w * 0.005f, br = tr - w * 0.008f;
        rectF(bl, cy - bh, br, cy + bh, C_BLACK);
        triF(br, cy - bh * 0.4f, br, cy + bh * 0.4f, br + w * 0.016f, cy, C_WHITE);
        frameW(bl, cy - bh, br, cy + bh, 1.5f, C_WHITE);
        {
            const S32 whole = (S32)floorf(llmax(spd, 0.f));
            const F32 frac = llmax(spd, 0.f) - whole;
            const F32 dw = mF.mMonoLarge->getWidthF32(std::string("0"));
            text(mF.mMonoLarge, fmt("%d", whole / 10), br - dw - w * 0.008f, cy, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
            LLLocalClipRect clip(LLRect((S32)(br - dw - w * 0.008f), (S32)(cy + bh), (S32)br, (S32)(cy - bh)));
            const F32 step = bh * 1.1f;
            for (S32 k = -1; k <= 1; ++k)
            {
                const S32 digit = ((whole % 10) + k + 10) % 10;
                text(mF.mMonoLarge, fmt("%d", digit), br - w * 0.008f, cy - (k - frac) * step, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
            }
        }
        text(mF.mMonoMed, fmt("%d", (S32)llround(f.selSpeedKt())), (tl + tr) * 0.5f, tt + h * 0.012f, C_MAGENTA, LLFontGL::HCENTER, LLFontGL::BOTTOM);
        text(mF.mSmall, fmt("GS %d", (S32)llround(d.mGS * KT)), (tl + tr) * 0.5f, tb - h * 0.012f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
    }

    // ---- altitude tape and vertical speed ----
    {
        const F32 tl = sl + w * 0.735f, tr = sl + w * 0.875f;
        const F32 tb = cy - h * 0.31f, tt = cy + h * 0.31f;
        const F32 alt = d.mAltMSL * FT;
        const F32 ppf = (tt - tb) / 800.f;
        {
            LLLocalClipRect clip(LLRect((S32)tl - 12, (S32)tt, (S32)tr, (S32)tb));
            rectF(tl, tb, tr, tt, C_TAPE);
            // the ground: amber hatching below the land's height
            const F32 gnd_ft = (llmax(d.mGround, d.mWaterZ) - d.mWaterZ) * FT;
            const F32 gy = cy + (gnd_ft - alt) * ppf;
            if (gy > tb)
            {
                const F32 top_y = llmin(gy, tt);
                rectF(tl, tb, tr, top_y, LLColor4(0.30f, 0.20f, 0.05f, 0.85f));
                for (F32 k = tb - (tr - tl); k < top_y; k += h * 0.02f)
                {
                    lineW(tl, k, tr, k + (tr - tl), 1.f, C_AMBER);
                }
                rectF(tl, top_y - 1.f, tr, top_y + 1.f, C_AMBER);
            }
            for (S32 k = ((S32)(alt - 500.f) / 100) * 100; k <= alt + 500.f; k += 100)
            {
                const F32 y = cy + (k - alt) * ppf;
                lineW(tl, y, tl + w * 0.016f, y, 1.5f, C_WHITE);
                if (k % 200 == 0)
                {
                    text(mF.mMonoSmall, fmt("%d", k), tl + w * 0.022f, y, C_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
                }
            }
            const F32 by = llclamp(cy + (f.selAltFt() - alt) * ppf, tb, tt);
            rectF(tl - w * 0.012f, by - h * 0.014f, tl + 1.f, by + h * 0.014f, C_MAGENTA);
            rectF(tl - w * 0.006f, by - h * 0.005f, tl + 1.f, by + h * 0.005f, C_TAPE);
        }
        // the readout: thousands and hundreds large, the last two digits rolling in twenties
        const F32 bh = h * 0.05f;
        const F32 bl = tl + w * 0.008f, br = tr + w * 0.012f;
        rectF(bl, cy - bh, br, cy + bh, C_BLACK);
        triF(bl, cy - bh * 0.4f, bl, cy + bh * 0.4f, bl - w * 0.014f, cy, C_WHITE);
        frameW(bl, cy - bh, br, cy + bh, 1.5f, C_WHITE);
        {
            const bool neg = alt < 0.f;
            const F32 a = fabsf(alt);
            const S32 hundreds = (S32)floorf(a / 100.f);
            const F32 rem = a - hundreds * 100.f;   // 0..100, rolled in 20s
            const F32 dw2 = mF.mMonoMed->getWidthF32(std::string("00"));
            text(mF.mMonoLarge, fmt("%s%d", neg ? "-" : "", hundreds), br - dw2 - w * 0.01f, cy, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
            LLLocalClipRect clip(LLRect((S32)(br - dw2 - w * 0.01f), (S32)(cy + bh), (S32)br, (S32)(cy - bh)));
            const F32 step = bh * 0.9f;
            const S32 base = (S32)floorf(rem / 20.f);
            const F32 frac = rem / 20.f - base;
            for (S32 k = -1; k <= 1; ++k)
            {
                const S32 v = (((base + k) % 5) + 5) % 5 * 20;
                text(mF.mMonoMed, fmt("%02d", v), br - w * 0.006f, cy - (k - frac) * step, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
            }
        }
        text(mF.mMonoMed, fmt("%d", (S32)llround(f.selAltFt())), (tl + tr) * 0.5f, tt + h * 0.012f, C_MAGENTA, LLFontGL::HCENTER, LLFontGL::BOTTOM);
        // metric altitude, as the Airbus shows it
        text(mF.mSmall, fmt("%d M", (S32)llround(d.mAltMSL)), (tl + tr) * 0.5f, tb - h * 0.012f, C_CYAN, LLFontGL::HCENTER, LLFontGL::TOP);

        // VSI: 0-1000-2000-6000 ft/min on a squeezed scale, a needle from off the screen's edge
        const F32 vl = tr + w * 0.02f, vr = sl + w * 0.975f;
        const F32 vb = cy - h * 0.27f, vt = cy + h * 0.27f;
        quadF(vl, vb + h * 0.05f, vr, vb, vr, vt, vl, vt - h * 0.05f, C_TAPE);
        auto vsy = [&](F32 fpm)
        {
            const F32 a = fabsf(fpm), s = fpm < 0.f ? -1.f : 1.f;
            F32 u;
            if (a <= 1000.f) u = a / 1000.f * 0.48f;
            else if (a <= 2000.f) u = 0.48f + (a - 1000.f) / 1000.f * 0.24f;
            else u = 0.72f + llmin((a - 2000.f) / 4000.f, 1.f) * 0.26f;
            return cy + s * u * (vt - cy);
        };
        const S32 marks[] = { 500, 1000, 1500, 2000, 6000 };
        for (S32 m : marks)
        {
            for (S32 sgn = -1; sgn <= 1; sgn += 2)
            {
                const F32 y = vsy((F32)(m * sgn));
                const bool big = m == 1000 || m == 2000 || m == 6000;
                lineW(vl + w * 0.004f, y, vl + w * (big ? 0.018f : 0.011f), y, 1.3f, C_WHITE);
                if (big)
                {
                    text(mF.mTiny, fmt("%d", m / 1000), vl + w * 0.02f, y, C_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
                }
            }
        }
        lineW(vl + w * 0.004f, cy, vl + w * 0.022f, cy, 2.f, C_WHITE);
        const F32 vs_fpm = d.mVS * FPM;
        if (f.vertical() == WolfFlight::VERT_VS)
        {
            const F32 sy = vsy(f.selVsFpm());
            rectF(vl, sy - h * 0.008f, vl + w * 0.012f, sy + h * 0.008f, C_MAGENTA);
        }
        {
            LLLocalClipRect clip(LLRect((S32)vl, (S32)vt, (S32)vr, (S32)vb));
            lineW(vr + w * 0.06f, cy, vl + w * 0.005f, vsy(vs_fpm), 2.f, C_WHITE);
        }
        if (fabsf(vs_fpm) > 400.f)
        {
            text(mF.mMonoSmall, fmt("%d", (S32)(llround(vs_fpm / 50.f) * 50)), (vl + vr) * 0.5f,
                 vs_fpm > 0.f ? vt + h * 0.008f : vb - h * 0.008f, C_WHITE, LLFontGL::HCENTER, vs_fpm > 0.f ? LLFontGL::BOTTOM : LLFontGL::TOP);
        }
    }

    // ---- heading: the top of a compass rose along the bottom ----
    {
        const F32 hr = h * 0.40f;
        const F32 hcx = cx, hcy = sb - h * 0.235f;
        const F32 hdg = d.mHeading;
        LLLocalClipRect clip(LLRect((S32)sl, (S32)(sb + h * 0.19f), (S32)sr, (S32)sb));
        circleF(hcx, hcy, hr, LLColor4(0.22f, 0.24f, 0.27f, 0.9f), 64);
        for (S32 k = 0; k < 360; k += 5)
        {
            const F32 rel = wrap180((F32)k - hdg);
            if (fabsf(rel) > 60.f) continue;
            const F32 a = (90.f - rel) * DEG_TO_RAD;
            const F32 len = (k % 10 == 0) ? hr * 0.08f : hr * 0.045f;
            lineW(hcx + hr * cosf(a), hcy + hr * sinf(a), hcx + (hr - len) * cosf(a), hcy + (hr - len) * sinf(a), 1.3f, C_WHITE);
            if (k % 30 == 0)
            {
                text(mF.mSmall, headingLabel(k), hcx + (hr - hr * 0.16f) * cosf(a), hcy + (hr - hr * 0.16f) * sinf(a), C_WHITE,
                     LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
        }
        // selected heading bug and the track line
        {
            const F32 rel = wrap180(f.selHeading() - hdg);
            if (fabsf(rel) < 60.f)
            {
                const F32 a = (90.f - rel) * DEG_TO_RAD;
                const F32 bx = hcx + hr * cosf(a), by = hcy + hr * sinf(a);
                lineW(bx - 4.f * sinf(a), by + 4.f * cosf(a), bx + 4.f * sinf(a), by - 4.f * cosf(a), 4.f, C_MAGENTA);
            }
            const F32 trel = wrap180(d.mTrack - hdg);
            if (d.mGS > 2.f && fabsf(trel) < 60.f)
            {
                const F32 a = (90.f - trel) * DEG_TO_RAD;
                lineW(hcx + hr * 0.55f * cosf(a), hcy + hr * 0.55f * sinf(a), hcx + hr * 0.98f * cosf(a), hcy + hr * 0.98f * sinf(a), 1.3f, C_WHITE);
            }
        }
    }
    // the lubber line and the heading readout
    {
        const F32 ty = sb + h * 0.165f;
        triF(cx, ty + h * 0.0f, cx - w * 0.012f, ty + h * 0.025f, cx + w * 0.012f, ty + h * 0.025f, C_WHITE);
        const F32 bw = w * 0.05f;
        rectF(cx - bw, ty + h * 0.03f, cx + bw, ty + h * 0.075f, C_BLACK);
        frameW(cx - bw, ty + h * 0.03f, cx + bw, ty + h * 0.075f, 1.2f, C_WHITE);
        text(mF.mMonoMed, fmt("%03d", (S32)llround(d.mHeading) % 360), cx, ty + h * 0.0525f, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
        text(mF.mTiny, "TRU", cx + bw + w * 0.01f, ty + h * 0.0525f, C_GREEN, LLFontGL::LEFT, LLFontGL::VCENTER);
        text(mF.mTiny, "HDG", cx - bw - w * 0.01f, ty + h * 0.0525f, C_GREEN, LLFontGL::RIGHT, LLFontGL::VCENTER);
        if (f.lateral() == WolfFlight::LAT_HDG)
        {
            text(mF.mSmall, fmt("%03d H", (S32)llround(f.selHeading()) % 360), sl + w * 0.2f, sb + h * 0.03f, C_MAGENTA, LLFontGL::HCENTER, LLFontGL::BOTTOM);
        }
    }
    // G, when it is worth knowing
    if (d.mG > 1.6f || d.mG < 0.4f)
    {
        text(mF.mMed, fmt("G %.1f", d.mG), sl + w * 0.1f, sb + h * 0.12f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
    // ground proximity on the PFD, big and red
    for (const WolfFlight::Cas& c : f.cas())
    {
        if (c.mText == "PULL UP" && blink(0.6f))
        {
            text(mF.mLarge, "PULL UP", cx, cy - ah * 0.35f, C_RED, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
}

//-----------------------------------------------------------------------------
// ND
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawND(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    F32 sl, sb, sr, st;
    drawBezel(l, b, r, t, (r - l) * 0.045f, sl, sb, sr, st);
    const F32 w = sr - sl, h = st - sb;
    LLLocalClipRect screen_clip(LLRect((S32)sl, (S32)st, (S32)sr, (S32)sb));
    const bool sail = f.sailing();
    // Sailing: a paper chart — white deep water, blue shallows, buff land, black ink.
    const LLColor4 FG = sail ? C_INK : C_WHITE;
    addHit(sl, sb, sr, st, H_ND_SCREEN, "Click for the PLAN view: the whole route, north up");
    if (!d.mValid)
    {
        text(mF.mLarge, "MAP", sl + w * 0.5f, sb + h * 0.5f, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }
    if (sail)
    {
        rectF(sl, sb, sr, st, LLColor4(0.96f, 0.96f, 0.93f, 1.f));   // the chart's deep water
    }
    const F32 draft = gSavedSettings.getF32("WolfSailDraft");
    const S32 range_idx = llclamp(gSavedSettings.getS32("WolfFlightNDRange"), 0, ND_RANGE_COUNT - 1);
    const F32 range_m = ND_RANGES_NM[range_idx] * NM;
    const F32 cx = sl + w * 0.5f, cy = sb + h * 0.14f;
    const F32 R = h * 0.72f;
    const F32 ppm = R / range_m;
    const F32 hdg = d.mHeading;
    const F32 sh = sinf(hdg * DEG_TO_RAD), ch = cosf(hdg * DEG_TO_RAD);
    // world offset (east, north) -> screen, heading up
    auto toScreen = [&](F32 de, F32 dn, F32& x, F32& y)
    {
        const F32 right = de * ch - dn * sh;
        const F32 fwd = de * sh + dn * ch;
        x = cx + right * ppm;
        y = cy + fwd * ppm;
    };

    // ---- terrain: land in the loaded regions against the aircraft's altitude ----
    {
        const F32 z = (F32)d.mPosGlobal.mdV[VZ];
        const F32 key[4] = { (F32)range_idx + (sail ? 100.f : 0.f), hdg, (F32)d.mPosGlobal.mdV[VX], (F32)d.mPosGlobal.mdV[VY] };
        const bool stale = now() - mTerrainBuilt > 0.4
            || key[0] != mTerrainKey[0] || fabsf(wrap180(key[1] - mTerrainKey[1])) > 2.f
            || fabsf(key[2] - mTerrainKey[2]) > range_m * 0.02f || fabsf(key[3] - mTerrainKey[3]) > range_m * 0.02f;
        if (stale)
        {
            mTerrainBuilt = now();
            for (S32 i = 0; i < 4; ++i) mTerrainKey[i] = key[i];
            mTerrain.clear();
            const S32 N = 46;
            const F32 cell = R * 2.f / N;
            for (S32 iy = 0; iy < N; ++iy)
            {
                for (S32 ix = 0; ix < N; ++ix)
                {
                    const F32 sx = -R + (ix + 0.5f) * cell + ((iy & 1) ? cell * 0.5f : 0.f);
                    const F32 sy = (iy + 0.5f) * cell * 0.55f;
                    if (sx * sx + sy * sy > R * R) continue;
                    const F32 right = sx / ppm, fwd = sy / ppm;
                    const F32 de = right * ch + fwd * sh;
                    const F32 dn = -right * sh + fwd * ch;
                    LLVector3d p = d.mPosGlobal;
                    p.mdV[VX] += de;
                    p.mdV[VY] += dn;
                    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosGlobal(p);
                    if (!regionp) continue;
                    const F32 g = LLWorld::getInstance()->resolveLandHeightGlobal(p);
                    if (sail)
                    {
                        // a chart: land, and water too shallow for the keel; deep water stays dark
                        const F32 wz = regionp->getWaterHeight();
                        if (g >= wz) mTerrain.push_back({ sx, sy, 4 });
                        else if (g > wz - draft - 2.f) mTerrain.push_back({ sx, sy, 5 });
                        continue;
                    }
                    if (g <= regionp->getWaterHeight()) continue;   // water: no terrain
                    const F32 rel = g - z;
                    U8 lvl = 0;
                    if (rel > -15.f) lvl = 3;
                    else if (rel > -60.f) lvl = 2;
                    else if (rel > -150.f) lvl = 1;
                    if (lvl)
                    {
                        mTerrain.push_back({ sx, sy, lvl });
                    }
                }
            }
        }
        const F32 dot = llmax(1.5f, R / 95.f);
        for (const TerrainDot& td : mTerrain)
        {
            const LLColor4 c = td.mLevel == 5 ? LLColor4(0.62f, 0.80f, 0.93f, 1.f)
                             : td.mLevel == 4 ? LLColor4(0.93f, 0.83f, 0.56f, 1.f)
                             : td.mLevel == 3 ? LLColor4(0.85f, 0.10f, 0.08f, 1.f)
                             : td.mLevel == 2 ? LLColor4(0.80f, 0.70f, 0.05f, 1.f)
                                              : LLColor4(0.05f, 0.55f, 0.12f, 1.f);
            rectF(cx + td.mX - dot, cy + td.mY - dot, cx + td.mX + dot, cy + td.mY + dot, c);
        }
    }

    // ---- the compass arc ----
    arcW(cx, cy, R, 1.5f, 0.f, PI_F, FG, 96);
    for (S32 k = 0; k < 360; k += 5)
    {
        const F32 rel = wrap180((F32)k - hdg);
        if (fabsf(rel) > 80.f) continue;
        const F32 a = (90.f - rel) * DEG_TO_RAD;
        const F32 len = (k % 10 == 0) ? R * 0.045f : R * 0.025f;
        lineW(cx + R * cosf(a), cy + R * sinf(a), cx + (R - len) * cosf(a), cy + (R - len) * sinf(a), 1.3f, FG);
        if (k % 30 == 0)
        {
            text(mF.mSmall, headingLabel(k), cx + (R - R * 0.09f) * cosf(a), cy + (R - R * 0.09f) * sinf(a), FG,
                 LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
    // half-range arc, dashed, with its range
    for (S32 i = 0; i < 36; i += 2)
    {
        const F32 a0 = PI_F * i / 36.f, a1 = PI_F * (i + 1) / 36.f;
        arcW(cx, cy, R * 0.5f, 1.f, a0, a1, C_GREY, 4);
    }
    text(mF.mTiny, fmt("%g", ND_RANGES_NM[range_idx] * 0.5f), cx - R * 0.5f * 0.71f - 4.f, cy + R * 0.5f * 0.71f, FG, LLFontGL::RIGHT, LLFontGL::VCENTER);
    // the track line
    if (d.mGS > 2.f)
    {
        const F32 a = (90.f - wrap180(d.mTrack - hdg)) * DEG_TO_RAD;
        dashW(cx, cy, cx + R * cosf(a), cy + R * sinf(a), 1.f, R * 0.03f, FG);
    }
    // the selected heading bug, and its line when flying it
    {
        const F32 rel = wrap180(f.selHeading() - hdg);
        if (fabsf(rel) < 80.f)
        {
            const F32 a = (90.f - rel) * DEG_TO_RAD;
            const F32 bx = cx + R * cosf(a), by = cy + R * sinf(a);
            const F32 tx = -sinf(a), ty = cosf(a);
            quadF(bx - tx * 6.f, by - ty * 6.f, bx + tx * 6.f, by + ty * 6.f,
                  bx + tx * 6.f + cosf(a) * 7.f, by + ty * 6.f + sinf(a) * 7.f, bx - tx * 6.f + cosf(a) * 7.f, by - ty * 6.f + sinf(a) * 7.f, C_MAGENTA);
            if (f.lateral() == WolfFlight::LAT_HDG)
            {
                dashW(cx, cy, bx, by, 1.f, R * 0.02f, C_MAGENTA);
            }
        }
    }

    // ---- traffic: the other avatars about ----
    {
        std::vector<LLVector3d> positions;
        uuid_vec_t ids;
        LLWorld::getInstance()->getAvatars(&ids, &positions, d.mPosGlobal, range_m * 1.2f);
        const F32 s = llmax(4.f, R * 0.025f);
        for (size_t i = 0; i < positions.size(); ++i)
        {
            if (ids[i] == gAgentID) continue;
            const LLVector3d& p = positions[i];
            const F32 de = (F32)(p.mdV[VX] - d.mPosGlobal.mdV[VX]);
            const F32 dn = (F32)(p.mdV[VY] - d.mPosGlobal.mdV[VY]);
            const F32 dz_ft = (F32)(p.mdV[VZ] - d.mPosGlobal.mdV[VZ]) * FT;
            if (fabsf(dz_ft) > 9900.f) continue;
            F32 x, y;
            toScreen(de, dn, x, y);
            if (y < sb || (x - cx) * (x - cx) + (y - cy) * (y - cy) > R * R) continue;
            // "closeby", not "near": Windows' headers #define near (windef.h)
            const bool closeby = (de * de + dn * dn) < (range_m * 0.15f) * (range_m * 0.15f) && fabsf(dz_ft) < 1200.f;
            const LLColor4 c = closeby ? C_CYAN : FG;
            if (closeby)
            {
                quadF(x, y + s, x + s, y, x, y - s, x - s, y, c);
            }
            else
            {
                lineW(x, y + s, x + s, y, 1.3f, c); lineW(x + s, y, x, y - s, 1.3f, c);
                lineW(x, y - s, x - s, y, 1.3f, c); lineW(x - s, y, x, y + s, 1.3f, c);
            }
            const S32 hundreds = (S32)llround(dz_ft / 100.f);
            text(mF.mTiny, fmt("%c%02d", hundreds >= 0 ? '+' : '-', llabs(hundreds)), x, dz_ft >= 0.f ? y + s + 1.f : y - s - 1.f, c,
                 LLFontGL::HCENTER, dz_ft >= 0.f ? LLFontGL::BOTTOM : LLFontGL::TOP);
        }
    }

    // ---- the route ----
    const WolfFlight::Dest& dest = f.dest();
    const WolfFlight::Route& route = f.route();
    auto toS = [&](const LLVector3d& p, F32& x, F32& y)
    {
        toScreen((F32)(p.mdV[VX] - d.mPosGlobal.mdV[VX]), (F32)(p.mdV[VY] - d.mPosGlobal.mdV[VY]), x, y);
    };
    // the legs still to sail / fly: from here to the active waypoint magenta, the rest white
    if (route.mValid)
    {
        F32 px = cx, py = cy;
        for (S32 i = route.mLeg; i < (S32)route.mPts.size(); ++i)
        {
            F32 x, y;
            toS(route.mPts[i], x, y);
            lineW(px, py, x, y, i == route.mLeg ? 2.5f : 1.5f, i == route.mLeg ? C_MAGENTA : FG);
            if (i < (S32)route.mPts.size() - 1)
            {
                const F32 s = 5.f;
                const LLColor4 c = i == route.mLeg ? C_MAGENTA : FG;
                quadF(x, y + s, x + s * 0.3f, y, x, y - s, x - s * 0.3f, y, c);
                quadF(x + s, y, x, y - s * 0.3f, x - s, y, x, y + s * 0.3f, c);
                text(mF.mTiny, fmt("WP%d", i), x + s + 2.f, y, c, LLFontGL::LEFT, LLFontGL::VCENTER);
            }
            px = x; py = y;
        }
        // laylines: the two close-hauled courses that fetch the active waypoint
        if (sail && d.mTWS > 0.5f)
        {
            F32 wx, wy;
            toS(f.activeWaypoint(), wx, wy);
            const F32 A = WolfFlight::tackAngle();
            for (S32 side = -1; side <= 1; side += 2)
            {
                // from the mark, back down the course that reaches it: wind-from + 180 -/+ A
                const F32 back = (d.mTWDSteady + 180.f + side * A - hdg) * DEG_TO_RAD;   // the laylines the AP sails
                dashW(wx, wy, wx + sinf(back) * R * 1.5f, wy + cosf(back) * R * 1.5f, 1.f, R * 0.03f, LLColor4(0.0f, 0.42f, 0.60f, 1.f));
            }
        }
    }
    if (dest.mValid)
    {
        F32 x, y;
        toS(dest.mGlobal, x, y);
        if (!route.mValid)
        {
            lineW(cx, cy, x, y, 2.f, C_MAGENTA);
        }
        if (f.lateral() == WolfFlight::LAT_HOLD)
        {
            const F32 v = llmax(d.mGS, 15.f);
            const F32 rad = llclamp(v * v / (9.81f * 0.4663f) * 1.2f, 120.f, 1500.f) * ppm;
            arcW(x, y, rad, 1.5f, 0.f, 2.f * PI_F, C_MAGENTA, 48);
        }
        // the waypoint: a four-pointed star
        const F32 s = llmax(6.f, R * 0.04f);
        quadF(x, y + s, x + s * 0.25f, y, x, y - s, x - s * 0.25f, y, C_MAGENTA);
        quadF(x + s, y, x, y - s * 0.25f, x - s, y, x, y + s * 0.25f, C_MAGENTA);
        text(mF.mSmall, dest.mRegion, x + s * 1.2f, y, C_MAGENTA, LLFontGL::LEFT, LLFontGL::VCENTER);
    }

    // ---- own ship: an aircraft, or in Sailing Mode a boat (Paul: "it should be a boat") ----
    if (sail)
    {
        // a hull seen from above, bow up: pointed bow, straight sides, square stern, a mast
        const F32 s = R * 0.07f;
        const LLColor4 hull = C_INK;
        triF(cx, cy + s * 1.5f, cx - s * 0.45f, cy + s * 0.45f, cx + s * 0.45f, cy + s * 0.45f, hull);
        quadF(cx - s * 0.45f, cy + s * 0.45f, cx + s * 0.45f, cy + s * 0.45f, cx + s * 0.38f, cy - s * 1.0f, cx - s * 0.38f, cy - s * 1.0f, hull);
        circleF(cx, cy + s * 0.35f, s * 0.12f, C_CREAM_HI, 10);
    }
    else
    {
        const F32 s = R * 0.06f;
        lineW(cx, cy - s * 0.4f, cx, cy + s * 1.2f, 2.f, FG);
        lineW(cx - s * 0.75f, cy + s * 0.45f, cx + s * 0.75f, cy + s * 0.45f, 2.f, FG);
        lineW(cx - s * 0.35f, cy - s * 0.35f, cx + s * 0.35f, cy - s * 0.35f, 2.f, FG);
    }

    // ---- the data round the edge ----
    {
        // heading box at the top
        const F32 bw = w * 0.07f, by = st - h * 0.04f;
        rectF(cx - bw, by - h * 0.04f, cx + bw, by + h * 0.035f, C_BLACK);
        frameW(cx - bw, by - h * 0.04f, cx + bw, by + h * 0.035f, 1.2f, FG);
        // the box is black on the chart too: its digits stay white to be read
        text(mF.mMonoMed, fmt("%03d", (S32)llround(hdg) % 360), cx, by, sail ? C_WHITE : FG, LLFontGL::HCENTER, LLFontGL::VCENTER);
        text(mF.mSmall, "HDG", cx - bw - 4.f, by, C_GREEN, LLFontGL::RIGHT, LLFontGL::VCENTER);
        text(mF.mSmall, "TRU", cx + bw + 4.f, by, C_GREEN, LLFontGL::LEFT, LLFontGL::VCENTER);
        triF(cx, cy + R + 1.f, cx - 6.f, cy + R + 12.f, cx + 6.f, cy + R + 12.f, FG);

        // top left: speeds and wind
        const F32 lx = sl + w * 0.03f;
        F32 ly = st - h * 0.03f;
        text(mF.mSmall, "GS", lx, ly, FG, LLFontGL::LEFT, LLFontGL::TOP);
        text(mF.mMonoMed, fmt("%d", (S32)llround(d.mGS * KT)), lx + w * 0.06f, ly, FG, LLFontGL::LEFT, LLFontGL::TOP);
        text(mF.mSmall, "TAS", lx + w * 0.16f, ly, FG, LLFontGL::LEFT, LLFontGL::TOP);
        text(mF.mMonoMed, fmt("%d", (S32)llround(d.mAirspeed * KT)), lx + w * 0.235f, ly, FG, LLFontGL::LEFT, LLFontGL::TOP);
        ly -= h * 0.06f;
        const F32 wind_spd = sqrtf(d.mWind.mV[VX] * d.mWind.mV[VX] + d.mWind.mV[VY] * d.mWind.mV[VY]);
        const F32 wind_from = wrap360(atan2f(-d.mWind.mV[VX], -d.mWind.mV[VY]) * RAD_TO_DEG);
        text(mF.mMonoSmall, fmt("%03d\xC2\xB0/%d", (S32)llround(wind_from) % 360, (S32)llround(wind_spd * KT)), lx, ly, FG, LLFontGL::LEFT, LLFontGL::TOP);
        if (wind_spd > 0.3f)
        {
            // the arrow points the way the wind blows, heading up
            const F32 ax = lx + w * 0.03f, ay = ly - h * 0.09f;
            const F32 rel = (wind_from + 180.f - hdg) * DEG_TO_RAD;
            const F32 ux = sinf(rel), uy = cosf(rel), len = h * 0.04f;
            lineW(ax - ux * len, ay - uy * len, ax + ux * len, ay + uy * len, 1.5f, FG);
            triF(ax + ux * len, ay + uy * len, ax + ux * len * 0.5f - uy * len * 0.3f, ay + uy * len * 0.5f + ux * len * 0.3f,
                 ax + ux * len * 0.5f + uy * len * 0.3f, ay + uy * len * 0.5f - ux * len * 0.3f, FG);
        }

        // top right: the active waypoint
        const F32 rx = sr - w * 0.03f;
        F32 ry = st - h * 0.03f;
        if (dest.mValid)
        {
            text(mF.mMed, dest.mRegion, rx, ry, C_MAGENTA, LLFontGL::RIGHT, LLFontGL::TOP);
            ry -= h * 0.06f;
            const F32 eta = f.destEtaSeconds();
            if (eta >= 0.f)
            {
                const time_t at = time(nullptr) + (time_t)eta;
                struct tm g;
#if LL_WINDOWS
                gmtime_s(&g, &at);
#else
                gmtime_r(&at, &g);
#endif
                text(mF.mMonoSmall, fmt("%02d%02d.%dz", g.tm_hour, g.tm_min, g.tm_sec / 6), rx, ry, FG, LLFontGL::RIGHT, LLFontGL::TOP);
            }
            ry -= h * 0.055f;
            const F32 nm = f.destDistance() / NM;
            text(mF.mMonoSmall, nm < 10.f ? fmt("%.1f NM", nm) : fmt("%d NM", (S32)llround(nm)), rx, ry, FG, LLFontGL::RIGHT, LLFontGL::TOP);
        }
        else if (dest.mPending)
        {
            text(mF.mSmall, "ROUTE: LOOKING UP", rx, ry, C_CYAN, LLFontGL::RIGHT, LLFontGL::TOP);
        }
        // bottom corners
        text(mF.mTiny, "MAP", sr - w * 0.03f, sb + h * 0.02f, C_GREEN, LLFontGL::RIGHT, LLFontGL::BOTTOM);
        text(mF.mTiny, "TERR", sl + w * 0.03f, sb + h * 0.02f, C_CYAN, LLFontGL::LEFT, LLFontGL::BOTTOM);
    }
}

//-----------------------------------------------------------------------------
// EICAS: alerts, the control picture, flight data
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawEICAS(F32 l, F32 b, F32 r, F32 t)
{
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    F32 sl, sb, sr, st;
    drawBezel(l, b, r, t, (r - l) * 0.055f, sl, sb, sr, st);
    const F32 w = sr - sl, h = st - sb;
    LLLocalClipRect screen_clip(LLRect((S32)sl, (S32)st, (S32)sr, (S32)sb));
    const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;

    // ---- crew alerts, top: warnings red, cautions amber, advisories amber indented, memos white ----
    const F32 lh = h * 0.058f;
    F32 y = st - h * 0.03f;
    text(mF.mTiny, "CREW ALERTS", sl + w * 0.04f, y, C_CYAN, LLFontGL::LEFT, LLFontGL::TOP);
    y -= lh * 1.05f;
    S32 shown = 0;
    for (const WolfFlight::Cas& c : f.cas())
    {
        if (shown >= 6) break;
        LLColor4 col = C_WHITE;
        F32 indent = 0.f;
        switch (c.mLevel)
        {
        case WolfFlight::CAS_WARNING: col = C_RED; break;
        case WolfFlight::CAS_CAUTION: col = C_AMBER; break;
        case WolfFlight::CAS_ADVISORY: col = C_AMBER; indent = w * 0.04f; break;
        default: break;
        }
        text(mF.mMed, c.mText, sl + w * 0.06f + indent, y, col, LLFontGL::LEFT, LLFontGL::TOP);
        y -= lh;
        ++shown;
    }
    const F32 cas_bottom = st - h * 0.03f - lh * 7.2f;
    rectF(sl + w * 0.03f, cas_bottom, sr - w * 0.03f, cas_bottom + 1.f, C_DIM);

    // ---- flight controls: what the autopilot (green) and the pilot's keys (white) hold ----
    {
        const F32 top = cas_bottom - h * 0.02f;
        const F32 bot = top - h * 0.33f;
        text(mF.mTiny, "FLT CTRL", sl + w * 0.04f, top, C_CYAN, LLFontGL::LEFT, LLFontGL::TOP);
        auto vgauge = [&](F32 gx, const char* label, F32 ap, S32 pilot)
        {
            const F32 gb = bot + h * 0.04f, gt = top - h * 0.06f, gm = (gb + gt) * 0.5f;
            lineW(gx, gb, gx, gt, 1.5f, C_WHITE);
            lineW(gx - w * 0.02f, gm, gx + w * 0.02f, gm, 1.f, C_WHITE);
            lineW(gx - w * 0.012f, gb, gx + w * 0.012f, gb, 1.f, C_WHITE);
            lineW(gx - w * 0.012f, gt, gx + w * 0.012f, gt, 1.f, C_WHITE);
            const F32 py = gm + (gt - gm) * llclamp((F32)pilot, -1.f, 1.f);
            triF(gx - w * 0.006f, py, gx - w * 0.03f, py + h * 0.015f, gx - w * 0.03f, py - h * 0.015f, C_WHITE);
            const F32 ay = gm + (gt - gm) * llclamp(ap, -1.f, 1.f);
            triF(gx + w * 0.006f, ay, gx + w * 0.03f, ay + h * 0.015f, gx + w * 0.03f, ay - h * 0.015f, C_GREEN);
            text(mF.mTiny, label, gx, gb - 2.f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
        };
        auto hgauge = [&](F32 gy, F32 gx0, F32 gx1, const char* label, F32 ap, S32 pilot)
        {
            const F32 gm = (gx0 + gx1) * 0.5f;
            lineW(gx0, gy, gx1, gy, 1.5f, C_WHITE);
            lineW(gm, gy - h * 0.02f, gm, gy + h * 0.02f, 1.f, C_WHITE);
            const F32 px = gm + (gx1 - gm) * llclamp((F32)pilot, -1.f, 1.f);
            triF(px, gy + h * 0.006f, px - w * 0.02f, gy + h * 0.03f, px + w * 0.02f, gy + h * 0.03f, C_WHITE);
            const F32 ax = gm + (gx1 - gm) * llclamp(ap, -1.f, 1.f);
            triF(ax, gy - h * 0.006f, ax - w * 0.02f, gy - h * 0.03f, ax + w * 0.02f, gy - h * 0.03f, C_GREEN);
            text(mF.mTiny, label, gm, gy - h * 0.035f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
        };
        if (f.sailing())
        {
            hgauge((top + bot) * 0.5f + h * 0.04f, sl + w * 0.12f, sl + w * 0.64f, "RUDDER", f.outBank(), f.pilotBank());
            vgauge(sl + w * 0.84f, "MOTOR", f.outThrottle(), f.pilotThrottle());
        }
        else
        {
            vgauge(sl + w * 0.16f, heli ? "CYCLIC" : "PITCH", f.outPitch(), f.pilotPitch());
            hgauge((top + bot) * 0.5f + h * 0.04f, sl + w * 0.30f, sl + w * 0.70f, heli ? "YAW" : "ROLL", f.outBank(), f.pilotBank());
            vgauge(sl + w * 0.84f, heli ? "COLL" : "THR", f.outThrottle(), f.pilotThrottle());
        }
        text(mF.mTiny, "AP", sl + w * 0.5f, bot + h * 0.06f, C_GREEN, LLFontGL::RIGHT, LLFontGL::BOTTOM);
        text(mF.mTiny, " PILOT", sl + w * 0.5f, bot + h * 0.06f, C_WHITE, LLFontGL::LEFT, LLFontGL::BOTTOM);
        rectF(sl + w * 0.03f, bot, sr - w * 0.03f, bot + 1.f, C_DIM);

        // ---- flight data ----
        F32 yy = bot - h * 0.02f;
        const F32 dl = sl + w * 0.05f, dv = sl + w * 0.30f;
        auto row = [&](const char* label, const std::string& v, const LLColor4& c)
        {
            text(mF.mTiny, label, dl, yy, C_CYAN, LLFontGL::LEFT, LLFontGL::TOP);
            text(mF.mMonoSmall, v, dv, yy, c, LLFontGL::LEFT, LLFontGL::TOP);
            yy -= h * 0.052f;
        };
        if (d.mValid)
        {
            row("REGION", d.mRegion, C_WHITE);
            row("POS", fmt("%d/%d/%d", (S32)d.mPosRegion.mV[VX], (S32)d.mPosRegion.mV[VY], (S32)d.mPosRegion.mV[VZ]), C_WHITE);
            if (f.sailing())
            {
                row("DEPTH", fmt("%.1f M", d.mDepth), d.mDepth < gSavedSettings.getF32("WolfSailDraft") + 0.5f ? C_AMBER : C_WHITE);
                row("HEEL", fmt("%d\xC2\xB0 %s", (S32)llround(fabsf(d.mRoll)), d.mRoll >= 0.f ? "STBD" : "PORT"), fabsf(d.mRoll) > 30.f ? C_AMBER : C_WHITE);
            }
            else
            {
                row("RA", fmt("%d FT", (S32)llround(d.mAGL * FT)), d.mAGL < 15.f ? C_AMBER : C_WHITE);
                row("G / SLIP", fmt("%.2f  %+.1f\xC2\xB0", d.mG, d.mSlip), C_WHITE);
            }
            row("SIM TD", fmt("%.2f", d.mTimeDilation), d.mTimeDilation < 0.8f ? C_AMBER : C_WHITE);
            row(f.sailing() ? "VESSEL" : "AIRCRAFT", d.mSeated ? (d.mVehicle.empty() ? std::string("SEATED") : d.mVehicle) : std::string("NOT SEATED"), d.mSeated ? C_WHITE : C_AMBER);
        }
    }
}

//-----------------------------------------------------------------------------
// CDU
//-----------------------------------------------------------------------------

void WolfFlightDeck::syncRouteFromDest()
{
    const WolfFlight::Dest& dest = WolfFlight::instance().dest();
    mModified = false;
    if (dest.mValid || dest.mPending)
    {
        mModRegion = dest.mRegion;
        mModX = dest.mLocal.mV[VX];
        mModY = dest.mLocal.mV[VY];
        mModZ = dest.mLocal.mV[VZ];
        mModHasXY = true;
        mModHasZ = dest.mHasZ;
    }
    else
    {
        mModRegion.clear();
        mModHasXY = mModHasZ = false;
    }
}

void WolfFlightDeck::cduLines(std::string (&label)[6][2], std::string (&data)[6][2], LLColor4 (&col)[6][2], std::string& title)
{
    WolfFlight& f = WolfFlight::instance();
    for (S32 i = 0; i < 6; ++i)
    {
        for (S32 s = 0; s < 2; ++s)
        {
            label[i][s].clear();
            data[i][s].clear();
            col[i][s] = C_WHITE;
        }
    }
    if (mPage == PAGE_DIR)
    {
        const WolfFlight::Dest& dest = f.dest();
        title = mModified ? "MOD DIRECT TO" : (dest.mValid ? "ACT DIRECT TO" : "DIRECT TO");
        label[0][0] = "DEST REGION";
        data[0][0] = mModRegion.empty() ? std::string("----------") : mModRegion;
        if (mModRegion.empty() && !mModified)
        {
            col[0][0] = C_DIM;
        }
        label[0][1] = "STATUS";
        if (mModified) { data[0][1] = "MOD"; col[0][1] = C_WHITE; }
        else if (dest.mPending) { data[0][1] = "LOOKUP"; col[0][1] = C_CYAN; }
        else if (dest.mFailed) { data[0][1] = "NOT FOUND"; col[0][1] = C_AMBER; }
        else if (dest.mValid) { data[0][1] = "ACTIVE"; col[0][1] = C_MAGENTA; }

        label[1][0] = "POSITION X/Y/Z";
        data[1][0] = mModHasXY ? fmt("%d/%d/%s", (S32)llround(mModX), (S32)llround(mModY), mModHasZ ? fmt("%d", (S32)llround(mModZ)).c_str() : "---")
                               : std::string("---/---/---");
        label[1][1] = "DIST";
        if (dest.mValid && !mModified)
        {
            const F32 nm = f.destDistance() / NM;
            data[1][1] = nm < 10.f ? fmt("%.1fNM", nm) : fmt("%dNM", (S32)llround(nm));
        }

        if (f.mapHasDestination())
        {
            label[2][0] = "WORLD MAP";
            data[2][0] = "<MAP DEST";
            col[2][0] = C_CYAN;
        }
        label[2][1] = "ETA";
        if (dest.mValid && !mModified && f.destEtaSeconds() >= 0.f)
        {
            const time_t at = time(nullptr) + (time_t)f.destEtaSeconds();
            struct tm g;
#if LL_WINDOWS
            gmtime_s(&g, &at);
#else
            gmtime_r(&at, &g);
#endif
            data[2][1] = fmt("%02d%02d.%dZ", g.tm_hour, g.tm_min, g.tm_sec / 6);
        }

        if (f.sailing())
        {
            label[3][0] = "WATER ROUTE";
            const WolfFlight::Route& rt = f.route();
            data[3][0] = rt.mNoWay ? std::string("NO WAY ROUND")
                       : rt.mValid ? fmt("%d LEG%s%s", (S32)rt.mPts.size() - 1, rt.mPts.size() > 2 ? "S" : "", rt.mUnsurveyed ? " (PART)" : "")
                                   : std::string("-----");
            col[3][0] = rt.mNoWay ? C_AMBER : (rt.mValid ? C_MAGENTA : C_DIM);
        }
        else
        {
            label[3][0] = "CRZ ALT";
            data[3][0] = fmt("%dFT", (S32)llround(f.cruiseAltFt()));
        }
        label[3][1] = "BRG";
        if (dest.mValid && !mModified)
        {
            data[3][1] = fmt("%03d\xC2\xB0", (S32)llround(f.destBearing()) % 360);
        }

        label[4][0] = "";
        label[4][1] = "AP";
        data[4][1] = f.apEngaged() ? "CMD" : "OFF";
        col[4][1] = f.apEngaged() ? C_GREEN : C_AMBER;

        if (mModified)
        {
            data[5][0] = "<ERASE";
            data[5][1] = "ACTIVATE>";
            col[5][1] = C_CYAN;
        }
        else if (dest.mValid || dest.mPending)
        {
            data[5][0] = "<CLEAR ROUTE";
        }
    }
    else if (f.sailing())
    {
        title = "BOAT CONTROLS";
        label[0][0] = "TACK ANGLE";
        data[0][0] = fmt("%d\xC2\xB0", (S32)llround(WolfFlight::tackAngle()));
        label[0][1] = "AUTO LEARN";
        data[0][1] = gSavedSettings.getBOOL("WolfFlightAutoLearn") ? "ON>" : "OFF>";
        label[1][0] = "DRAFT";
        data[1][0] = fmt("%.1fM", gSavedSettings.getF32("WolfSailDraft"));
        label[2][0] = "RUDDER";
        data[2][0] = std::string("<") + WolfFlight::pairName(WolfFlight::bankPair());
        label[2][1] = "LEFT KEY";
        data[2][1] = WolfFlight::invert(WolfFlight::keySetting("BankInvert")) ? "STBD>" : "PORT>";
        label[3][0] = "MOTOR";
        data[3][0] = std::string("<") + WolfFlight::pairName(WolfFlight::throttlePair());
        label[3][1] = "THR TYPE";
        data[3][1] = fmt("%s%s>", gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps")) ? "STEP" : "HOLD",
                         WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert")) ? " REV" : "");
        label[4][0] = "SAIL CMD";
        data[4][0] = gSavedSettings.getString("WolfSailCommand");
        if (data[4][0].empty()) { data[4][0] = "-----"; col[4][0] = C_DIM; }
        label[4][1] = "CHANNEL";
        data[4][1] = fmt("%d", gSavedSettings.getS32("WolfSailCommandChannel"));
        label[5][0] = "ENGINE CMD";
        data[5][0] = gSavedSettings.getString("WolfFlightEngineCommand");
        if (data[5][0].empty()) { data[5][0] = "-----"; col[5][0] = C_DIM; }
        label[5][1] = "CHANNEL";
        data[5][1] = fmt("%d", gSavedSettings.getS32("WolfFlightEngineChannel"));
    }
    else
    {
        const bool heli = WolfFlight::craft() == WolfFlight::CRAFT_HELI;
        title = "CONTROLS";
        label[0][0] = "AIRCRAFT";
        data[0][0] = heli ? "<HELICOPTER" : "<AEROPLANE";
        label[0][1] = "AUTO LEARN";
        data[0][1] = gSavedSettings.getBOOL("WolfFlightAutoLearn") ? "ON>" : "OFF>";

        label[1][0] = heli ? "FWD / BACK" : "PITCH";
        data[1][0] = std::string("<") + WolfFlight::pairName(WolfFlight::pitchPair());
        label[1][1] = heli ? "FWD KEY" : "UP KEY";
        {
            const bool inv = WolfFlight::invert("WolfFlightPitchInvert");
            data[1][1] = heli ? (inv ? "BACK>" : "FWD>") : (inv ? "NOSE UP>" : "NOSE DN>");
        }
        label[2][0] = heli ? "YAW" : "BANK";
        data[2][0] = std::string("<") + WolfFlight::pairName(WolfFlight::bankPair());
        label[2][1] = "LEFT KEY";
        data[2][1] = WolfFlight::invert(WolfFlight::keySetting("BankInvert")) ? "RIGHT>" : "LEFT>";

        label[3][0] = heli ? "LIFT" : "THROTTLE";
        data[3][0] = std::string("<") + WolfFlight::pairName(WolfFlight::throttlePair());
        label[3][1] = heli ? "UP KEY" : "THR TYPE";
        {
            const bool inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
            const bool steps = gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps"));
            data[3][1] = heli ? (inv ? "DOWN>" : "UP>") : fmt("%s%s>", steps ? "STEP" : "HOLD", inv ? " REV" : "");
        }

        label[4][0] = "GEAR CMD";
        data[4][0] = gSavedSettings.getString("WolfFlightGearCommand");
        if (data[4][0].empty()) { data[4][0] = "-----"; col[4][0] = C_DIM; }
        label[4][1] = "CHANNEL";
        data[4][1] = fmt("%d", gSavedSettings.getS32("WolfFlightGearChannel"));

        label[5][0] = "ENGINE CMD";
        data[5][0] = gSavedSettings.getString("WolfFlightEngineCommand");
        if (data[5][0].empty()) { data[5][0] = "-----"; col[5][0] = C_DIM; }
        label[5][1] = "CHANNEL";
        data[5][1] = fmt("%d", gSavedSettings.getS32("WolfFlightEngineChannel"));
    }
}

void WolfFlightDeck::drawCDU(F32 l, F32 b, F32 r, F32 t)
{
    const F32 w = r - l, h = t - b;
    // the unit: a lighter grey box with its keys
    if (WolfFlight::instance().sailing())
    {
        brassFrame(l, b, r, t, w * 0.025f);
        roundRectF(l + w * 0.025f, b + w * 0.025f, r - w * 0.025f, t - w * 0.025f, w * 0.03f,
                   LLColor4(0.36f, 0.21f, 0.11f, 1.f), LLColor4(0.24f, 0.13f, 0.06f, 1.f));
    }
    else
    {
        roundRectF(l, b, r, t, w * 0.04f, LLColor4(0.33f, 0.35f, 0.37f, 1.f), LLColor4(0.24f, 0.255f, 0.27f, 1.f));
    }
    screw(l + w * 0.05f, t - w * 0.05f, w * 0.018f);
    screw(r - w * 0.05f, t - w * 0.05f, w * 0.018f);
    screw(l + w * 0.05f, b + w * 0.05f, w * 0.018f);
    screw(r - w * 0.05f, b + w * 0.05f, w * 0.018f);

    const F32 lsk_w = w * 0.085f;
    const F32 sl = l + w * 0.04f + lsk_w + w * 0.02f, sr = r - w * 0.04f - lsk_w - w * 0.02f;
    const F32 st = t - h * 0.06f, sb = b + h * 0.30f;
    rectF(sl - 3.f, sb - 3.f, sr + 3.f, st + 3.f, C_BEZEL_LO);
    rectF(sl, sb, sr, st, C_SCREEN);
    addHit(sl, sb, sr, st, H_CDU_SCREEN, "Flight computer: click, then type; the line select keys either side enter the scratchpad");

    const F32 lh = (st - sb) / 14.f;
    const LLFontGL* big = avionics("B612Mono", LLFontGL::BOLD, lh * 0.98f);
    const LLFontGL* small = avionics("B612Mono", LLFontGL::NORMAL, lh * 0.78f);
    // 24 columns must fit
    while (big != small && big->getWidthF32(std::string(24, 'W')) > (sr - sl) * 0.98f)
    {
        const LLFontGL* smaller = avionics("B612Mono", LLFontGL::BOLD, (F32)big->getLineHeight() - 1.f);
        if (smaller == big) break;
        big = smaller;
    }
    const F32 pad = (sr - sl) * 0.02f;

    std::string label[6][2], data[6][2], title;
    LLColor4 col[6][2];
    cduLines(label, data, col, title);
    {
        LLLocalClipRect clip(LLRect((S32)sl, (S32)st, (S32)sr, (S32)sb));
        text(big, title, (sl + sr) * 0.5f, st - lh * 0.5f, C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
        text(small, mPage == PAGE_DIR ? "1/1" : "1/1", sr - pad, st - lh * 0.5f, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
        for (S32 i = 0; i < 6; ++i)
        {
            const F32 ly = st - lh * (1.5f + 2 * i);
            const F32 dy = ly - lh;
            text(small, label[i][0], sl + pad, ly, C_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
            text(small, label[i][1], sr - pad, ly, C_WHITE, LLFontGL::RIGHT, LLFontGL::VCENTER);
            text(big, data[i][0], sl + pad, dy, col[i][0], LLFontGL::LEFT, LLFontGL::VCENTER);
            text(big, data[i][1], sr - pad, dy, col[i][1], LLFontGL::RIGHT, LLFontGL::VCENTER);
        }
        // the scratchpad
        const F32 spy = sb + lh * 0.5f;
        std::string sp = mDelete ? std::string("DELETE") : (!mScratchMsg.empty() ? mScratchMsg : mScratch);
        const LLColor4 spc = (!mScratchMsg.empty() && !mDelete) ? C_AMBER : C_WHITE;
        if (hasFocus() && mScratchMsg.empty() && !mDelete && blink(1.f))
        {
            sp += "_";
        }
        text(big, sp, sl + pad, spy, spc, LLFontGL::LEFT, LLFontGL::VCENTER);
        if (!hasFocus() && mScratch.empty() && mScratchMsg.empty() && !mDelete)
        {
            text(small, "CLICK HERE TO TYPE", (sl + sr) * 0.5f, spy, C_DIM, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }

    // line select keys, either side of the data lines
    for (S32 i = 0; i < 6; ++i)
    {
        const F32 dy = st - lh * (2.5f + 2 * i);
        for (S32 s = 0; s < 2; ++s)
        {
            const F32 kl = s == 0 ? l + w * 0.035f : r - w * 0.035f - lsk_w;
            const F32 kr = kl + lsk_w;
            const EHit id = (EHit)((s == 0 ? H_LSK_L1 : H_LSK_R1) + i);
            const bool pressed = mPressed == id;
            roundRectF(kl - 1.f, dy - lh * 0.42f - 2.f, kr + 1.f, dy + lh * 0.42f + 1.f, 2.f, C_BEZEL_LO, C_BEZEL_LO);
            roundRectF(kl, dy - lh * 0.42f - (pressed ? 1.f : 0.f), kr, dy + lh * 0.42f - (pressed ? 1.f : 0.f), 2.f, C_BTN_T, C_BTN_B);
            rectF(kl + lsk_w * 0.25f, dy - 1.f, kr - lsk_w * 0.25f, dy + 1.f, C_WHITE);
            // the tick joining the key to its line
            rectF(s == 0 ? kr + 2.f : sr + 4.f, dy - 0.5f, s == 0 ? sl - 4.f : kl - 2.f, dy + 0.5f, C_DIM);
            addHit(kl, dy - lh * 0.45f, kr, dy + lh * 0.45f, id, "Line select key");
        }
    }

    // the function keys below the screen
    struct Key { const char* label; EHit id; const char* tip; };
    const Key row1[] = { { "DIR TO", H_CDU_DIR, "DIRECT TO page: where to fly" },
                         { "CTL", H_CDU_CTL, "CONTROLS page: which keys your aircraft uses, gear and engine commands" },
                         { "EXEC", H_CDU_EXEC, "Execute the modified route" } };
    const Key row2[] = { { "CLR", H_CDU_CLR, "Clear the scratchpad (one character; hold for all)" },
                         { "DEL", H_CDU_DEL, "Delete: then a line select key clears that field" },
                         { "HELP", H_CDU_HELP, "How to fly with Flight Mode: the quick guide" } };
    const F32 kh = h * 0.085f;
    const F32 k1t = sb - h * 0.035f, k1b = k1t - kh;
    const F32 k2t = k1b - h * 0.03f, k2b = k2t - kh;
    const LLFontGL* kf = avionics("B612", LLFontGL::BOLD, kh * 0.42f);
    auto key = [&](F32 kl, F32 kb, F32 kr, F32 kt, const Key& k, bool light)
    {
        const bool pressed = mPressed == k.id;
        roundRectF(kl - 1.f, kb - 2.5f, kr + 1.f, kt + 1.f, 3.f, C_BEZEL_LO, C_BEZEL_LO);
        roundRectF(kl, kb - (pressed ? 1.f : 0.f), kr, kt - (pressed ? 1.f : 0.f), 3.f, C_BTN_T, C_BTN_B);
        text(kf, k.label, (kl + kr) * 0.5f, (kb + kt) * 0.5f - (pressed ? 1.f : 0.f), C_WHITE, LLFontGL::HCENTER, LLFontGL::VCENTER);
        if (k.id == H_CDU_EXEC)
        {
            // the EXEC light bar: lit while there is a modification to execute
            rectF(kl + (kr - kl) * 0.2f, kt - kh * 0.2f, kr - (kr - kl) * 0.2f, kt - kh * 0.1f, light ? C_WHITE : LLColor4(0.2f, 0.2f, 0.2f, 1.f));
        }
        addHit(kl, kb, kr, kt, k.id, k.tip);
    };
    {
        const F32 kw = (sr - sl) / 3.f;
        for (S32 i = 0; i < 3; ++i)
        {
            key(sl + kw * i + 3.f, k1b, sl + kw * (i + 1) - 3.f, k1t, row1[i], mModified && mPage == PAGE_DIR);
        }
        for (S32 i = 0; i < 3; ++i)
        {
            key(sl + kw * i + 3.f, k2b, sl + kw * (i + 1) - 3.f, k2t, row2[i], false);
        }
    }
}

//-----------------------------------------------------------------------------
// The head-up display, over the world
//-----------------------------------------------------------------------------

void WolfFlightDeck::drawHUD(F32 top_of_world_bottom)
{
    if (!gSavedSettings.getBOOL("WolfFlightHUD"))
    {
        return;
    }
    WolfFlight& f = WolfFlight::instance();
    const WolfFlight::Data& d = f.data();
    if (!d.mValid)
    {
        return;
    }
    const F32 W = (F32)getRect().getWidth(), Hv = (F32)getRect().getHeight();
    LLLocalClipRect clip(LLRect(0, (S32)Hv, (S32)W, (S32)top_of_world_bottom));
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 o = cam->getOrigin();
    const LLVector3 at = cam->getAtAxis();
    // A direction (true heading, elevation) as a point on the screen; false when behind.
    auto proj = [&](F32 hdg, F32 elev, F32& x, F32& y) -> bool
    {
        const F32 h = hdg * DEG_TO_RAD, e = elev * DEG_TO_RAD;
        const LLVector3 dir(sinf(h) * cosf(e), cosf(h) * cosf(e), sinf(e));
        if (dir * at < 0.15f)
        {
            return false;
        }
        LLCoordGL sc;
        cam->projectPosAgentToScreen(o + dir * 1000.f, sc, false);
        S32 lx = 0, ly = 0;
        screenPointToLocal(sc.mX, sc.mY, &lx, &ly);
        x = (F32)lx;
        y = (F32)ly;
        return true;
    };
    const LLFontGL* hf = mF.mMed ? mF.mMed : LLFontGL::getFontSansSerif();
    const LLFontGL* hs = mF.mSmall ? mF.mSmall : LLFontGL::getFontSansSerif();
    const F32 hdg = d.mHeading;

    // the horizon, with heading marks along it
    F32 px = 0.f, py = 0.f;
    bool have = false;
    for (F32 a = -60.f; a <= 60.f; a += 2.5f)
    {
        F32 x, y;
        if (proj(hdg + a, 0.f, x, y))
        {
            if (have)
            {
                if (fabsf(a) > 4.f)   // a gap round the centre, as real HUDs leave
                {
                    lineW(px, py, x, y, 1.5f, C_HUD);
                }
            }
            px = x; py = y; have = true;
        }
        else
        {
            have = false;
        }
    }
    for (S32 k = 0; k < 360; k += 10)
    {
        const F32 rel = wrap180((F32)k - hdg);
        if (fabsf(rel) > 55.f) continue;
        F32 x, y;
        if (proj((F32)k, 0.f, x, y))
        {
            lineW(x, y, x, y + 8.f, 1.5f, C_HUD);
            if (k % 30 == 0)
            {
                text(hs, fmt("%02d", k / 10), x, y + 10.f, C_HUD, LLFontGL::HCENTER, LLFontGL::BOTTOM);
            }
        }
    }
    // the pitch ladder at the aircraft's heading: solid above, dashed below
    for (S32 e = -20; e <= 20; e += 5)
    {
        if (e == 0) continue;
        F32 x1, y1, x2, y2, x3, y3, x4, y4;
        if (proj(hdg - 7.f, (F32)e, x1, y1) && proj(hdg - 2.5f, (F32)e, x2, y2)
            && proj(hdg + 2.5f, (F32)e, x3, y3) && proj(hdg + 7.f, (F32)e, x4, y4))
        {
            if (e > 0)
            {
                lineW(x1, y1, x2, y2, 1.5f, C_HUD);
                lineW(x3, y3, x4, y4, 1.5f, C_HUD);
            }
            else
            {
                dashW(x1, y1, x2, y2, 1.5f, 5.f, C_HUD);
                dashW(x3, y3, x4, y4, 1.5f, 5.f, C_HUD);
            }
            const F32 hook = e > 0 ? -7.f : 7.f;
            lineW(x1, y1, x1, y1 + hook, 1.5f, C_HUD);
            lineW(x4, y4, x4, y4 + hook, 1.5f, C_HUD);
            text(hs, fmt("%d", llabs(e)), x4 + 4.f, y4, C_HUD, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
    }
    // the boresight: where the nose points
    {
        F32 x, y;
        if (proj(hdg, d.mPitch, x, y))
        {
            lineW(x - 18.f, y, x - 9.f, y, 2.f, C_HUD);
            lineW(x - 9.f, y, x - 4.5f, y - 7.f, 2.f, C_HUD);
            lineW(x - 4.5f, y - 7.f, x, y, 2.f, C_HUD);
            lineW(x, y, x + 4.5f, y - 7.f, 2.f, C_HUD);
            lineW(x + 4.5f, y - 7.f, x + 9.f, y, 2.f, C_HUD);
            lineW(x + 9.f, y, x + 18.f, y, 2.f, C_HUD);
        }
    }
    // the flight path vector: where the aircraft is actually going
    F32 fx = W * 0.5f, fy = (Hv + top_of_world_bottom) * 0.5f;
    if (d.mAirspeed > 2.f && proj(d.mTrack, d.mFPA, fx, fy))
    {
        arcW(fx, fy, 8.f, 2.f, 0.f, 2.f * PI_F, C_HUD, 24);
        lineW(fx - 22.f, fy, fx - 8.f, fy, 2.f, C_HUD);
        lineW(fx + 8.f, fy, fx + 22.f, fy, 2.f, C_HUD);
        lineW(fx, fy + 8.f, fx, fy + 16.f, 2.f, C_HUD);
    }
    // speed and altitude boxes either side
    const F32 my = (Hv + top_of_world_bottom) * 0.5f;
    const F32 sx = W * 0.30f, ax = W * 0.70f;
    frameW(sx - 40.f, my - 14.f, sx + 40.f, my + 14.f, 1.5f, C_HUD);
    text(hf, fmt("%d", (S32)llround(d.mAirspeed * KT)), sx, my, C_HUD, LLFontGL::HCENTER, LLFontGL::VCENTER);
    text(hs, "KT", sx, my - 18.f, C_HUD, LLFontGL::HCENTER, LLFontGL::TOP);
    frameW(ax - 48.f, my - 14.f, ax + 48.f, my + 14.f, 1.5f, C_HUD);
    text(hf, fmt("%d", (S32)llround(d.mAltMSL * FT)), ax, my, C_HUD, LLFontGL::HCENTER, LLFontGL::VCENTER);
    text(hs, fmt("%+d FPM", (S32)(llround(d.mVS * FPM / 10.f) * 10)), ax, my - 18.f, C_HUD, LLFontGL::HCENTER, LLFontGL::TOP);
    if (f.apEngaged())
    {
        text(hs, "AP", W * 0.5f, Hv - 40.f, C_HUD, LLFontGL::HCENTER, LLFontGL::TOP);
    }
}

// PULL UP / TERRAIN over the view, where the pilot is looking.
void WolfFlightDeck::drawWarnings(F32 top)
{
    WolfFlight& f = WolfFlight::instance();
    bool pull_up = false, terrain = false;
    for (const WolfFlight::Cas& c : f.cas())
    {
        if (c.mText == "PULL UP") pull_up = true;
        if (c.mText == "TERRAIN") terrain = true;
    }
    if (!pull_up && !terrain)
    {
        return;
    }
    const F32 W = (F32)getRect().getWidth(), Hv = (F32)getRect().getHeight();
    const F32 cy = top + (Hv - top) * 0.62f;
    const LLFontGL* big = avionics("B612", LLFontGL::BOLD, (Hv - top) * 0.09f);
    if (pull_up)
    {
        if (blink(0.5f))
        {
            const F32 bw = big->getWidthF32(std::string("PULL UP")) * 0.65f;
            rectF(W * 0.5f - bw, cy - (F32)big->getLineHeight() * 0.7f, W * 0.5f + bw, cy + (F32)big->getLineHeight() * 0.7f, LLColor4(0.f, 0.f, 0.f, 0.45f));
            text(big, "PULL UP", W * 0.5f, cy, C_RED, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
    else
    {
        text(big, "TERRAIN", W * 0.5f, cy, C_AMBER, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
}

// The quick guide, over the world above the deck. Paul: "we need a help thing so you can know
// how to set stuff", "i cant work out how to set the autopilot".
void WolfFlightDeck::drawHelp(F32 top)
{
    const F32 W = (F32)getRect().getWidth(), Hv = (F32)getRect().getHeight();
    const F32 avail_h = Hv - top;
    const F32 cw = llmin(W * 0.92f, 1500.f);
    const F32 ch = avail_h * 0.92f;
    const F32 l = (W - cw) * 0.5f, r = l + cw, t = Hv - avail_h * 0.04f, b = t - ch;
    roundRectF(l - 2.f, b - 2.f, r + 2.f, t + 2.f, 10.f, C_BEZEL_HI, C_BEZEL_LO);
    roundRectF(l, b, r, t, 9.f, LLColor4(0.05f, 0.06f, 0.08f, 0.96f), LLColor4(0.02f, 0.025f, 0.035f, 0.96f));

    struct Line { S32 kind; const char* s; };   // 0 heading, 1 text, 2 indented, 3 gap
    static const Line col1[] = {
        { 0, "FLY TO A PLACE WITH THE AUTOPILOT" },
        { 1, "1  Sit in your plane or helicopter and take off." },
        { 1, "2  Click the flight computer screen (CDU, bottom right)." },
        { 1, "3  Type where to go:  REGION/X/Y/Z   then press ENTER" },
        { 2, "e.g.  Madrigal/128/128/60    (just  Madrigal  = 128/128/0;" },
        { 2, "Z 0 = as low as is safe). The map shows a magenta line to it." },
        { 2, "Or pick a place on the World Map, then <MAP DEST on the CDU." },
        { 1, "4  Set ALTITUDE (top panel) to the height to cruise at." },
        { 1, "5  Press CMD. It flies straight there: planes circle over" },
        { 2, "the spot, helicopters hover on it." },
        { 1, "Touching a flight key or the stick hands control back to you." },
        { 3, "" },
        { 0, "FLYING BY HAND" },
        { 1, "Drag the SIDESTICK (pull down = nose up, sideways = bank)" },
        { 1, "and the THROTTLE lever. Both spring back when let go." },
        { 1, "Your keyboard works as always; the stick shows what it does." },
        { 3, "" },
        { 0, "WARNINGS" },
        { 1, "TERRAIN / PULL UP: land or an object ahead. Climb!" },
        { 1, "Click the flashing MASTER WARNING / CAUTION to cancel them." },
    };
    static const Line col2[] = {
        { 0, "THE TOP PANEL (AUTOPILOT)" },
        { 1, "CMD        autopilot on / off" },
        { 1, "A/T        hold the IAS speed (uses your throttle keys)" },
        { 1, "LNAV       fly straight to the destination" },
        { 1, "VNAV       cruise at CRZ ALT, then descend to the place's Z" },
        { 1, "HDG SEL    fly the HEADING in the window" },
        { 1, "ALT HOLD   climb / descend to ALTITUDE and hold it" },
        { 1, "V/S        climb / descend at VERT SPEED" },
        { 1, "Knobs: mouse wheel over them, or click their left / right" },
        { 2, "half; hold Shift for x10. Hover anything for a tip." },
        { 3, "" },
        { 0, "YOUR AIRCRAFT'S KEYS  (CDU: press CTL)" },
        { 1, "Aircraft differ. On the CTL page, the keys beside each line" },
        { 2, "change it: which keys PITCH, BANK and THROTTLE, which way" },
        { 2, "round they work, AEROPLANE or HELICOPTER." },
        { 1, "GEAR / ENGINE: type your aircraft's chat command (e.g. g)," },
        { 2, "then press the key beside GEAR CMD. The lever sends it." },
        { 1, "AUTO LEARN turns reversed keys round by itself while the" },
        { 2, "autopilot flies." },
        { 3, "" },
        { 1, "Click anywhere above the panel to close.  HELP reopens it." },
    };
    static const Line sail1[] = {
        { 0, "SAIL TO A PLACE WITH THE AUTOPILOT" },
        { 1, "1  Sit at your boat's helm." },
        { 1, "2  Click the CDU screen (bottom right), type where to go:" },
        { 2, "REGION/X/Y   then ENTER   (just  REGION  = its middle)" },
        { 2, "Or pick a place on the World Map, then <MAP DEST on the CDU." },
        { 1, "3  The route goes ROUND THE LAND (magenta on the chart):" },
        { 2, "it is planned through water deeper than your DRAFT and" },
        { 2, "re-planned as more of the sea loads in." },
        { 1, "4  Press TRACK, then AUTO. Upwind it beats to the mark," },
        { 2, "tacking on the laylines (the blue dashed lines)." },
        { 1, "Turning the wheel or a helm key puts it to standby." },
        { 3, "" },
        { 0, "OTHER WAYS TO STEER" },
        { 1, "HDG    hold a compass heading (knob to set it)" },
        { 1, "WIND   hold the angle to the wind you have now" },
        { 1, "TACK   through the wind to the other side" },
        { 1, "MOTOR  hold the SPEED with your throttle keys" },
        { 3, "" },
        { 0, "ALARMS" },
        { 1, "SHALLOW WATER, SHOAL AHEAD, COLLISION AHEAD: the autopilot" },
        { 2, "turns away to starboard and re-plans the route." },
    };
    static const Line sail2[] = {
        { 0, "THE INSTRUMENTS" },
        { 1, "WIND: black needle = apparent wind (what the sails feel)," },
        { 2, "navy T = true wind. Green / red = close-hauled sectors." },
        { 1, "BOAT SPEED, DEPTH (metres under the hull), VMG (how fast" },
        { 2, "you are closing on the waypoint) and HEEL." },
        { 1, "CHART: course up around you. PLAN: the whole route on" },
        { 2, "the world map, where you are on it and how far to go." },
        { 2, "Click the chart, or PLAN, to switch." },
        { 1, "Wind is the region's own wind, as llWind() gives scripts." },
        { 3, "" },
        { 0, "YOUR BOAT'S KEYS  (CDU: press CTL)" },
        { 1, "Set which keys steer (RUDDER) and drive the MOTOR, and" },
        { 2, "which way round. TACK ANGLE: how close to the wind it" },
        { 2, "sails. DRAFT: how deep the keel is (shallower = land)." },
        { 1, "SAIL / ENGINE: type your boat's chat command (e.g. raise)," },
        { 2, "then the key beside it. The SAIL / ENG buttons send it." },
        { 1, "Drag the WHEEL to steer, the red lever for the motor." },
        { 3, "" },
        { 1, "Click anywhere above the panel to close.  HELP reopens it." },
    };
    const bool sail_help = WolfFlight::instance().sailing();
    const Line* c1 = sail_help ? sail1 : col1;
    const Line* c2 = sail_help ? sail2 : col2;
    const S32 n1 = sail_help ? (S32)(sizeof(sail1) / sizeof(sail1[0])) : (S32)(sizeof(col1) / sizeof(col1[0]));
    const S32 n2 = sail_help ? (S32)(sizeof(sail2) / sizeof(sail2[0])) : (S32)(sizeof(col2) / sizeof(col2[0]));
    const S32 rows = llmax(n1, n2) + 2;
    const F32 lh = llmin((ch - 20.f) / rows, 26.f);
    const LLFontGL* head = avionics("B612", LLFontGL::BOLD, lh * 0.95f);
    const LLFontGL* body = avionics("B612", LLFontGL::NORMAL, lh * 0.82f);
    text(head, sail_help ? "SAILING MODE  -  QUICK GUIDE" : "FLIGHT MODE  -  QUICK GUIDE", (l + r) * 0.5f, t - lh * 0.4f, C_WHITE, LLFontGL::HCENTER, LLFontGL::TOP);
    auto column = [&](const Line* lines, S32 n, F32 x)
    {
        F32 y = t - lh * 1.8f;
        for (S32 i = 0; i < n; ++i)
        {
            switch (lines[i].kind)
            {
            case 0: text(head, lines[i].s, x, y, C_CYAN, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 1: text(body, lines[i].s, x + lh * 0.5f, y, C_WHITE, LLFontGL::LEFT, LLFontGL::TOP); break;
            case 2: text(body, lines[i].s, x + lh * 1.6f, y, C_GREY, LLFontGL::LEFT, LLFontGL::TOP); break;
            default: break;
            }
            y -= (lines[i].kind == 3) ? lh * 0.5f : lh;
        }
    };
    column(c1, n1, l + cw * 0.03f);
    column(c2, n2, l + cw * 0.52f);
    rectF(l + cw * 0.505f, b + lh, l + cw * 0.505f + 1.f, t - lh * 1.8f, C_DIM);
}

//-----------------------------------------------------------------------------
// Input
//-----------------------------------------------------------------------------

bool WolfFlightDeck::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (WolfFlight::instance().active() && !WolfFlight::instance().deckHidden() && mShowHelp && y > mPanelTop)
    {
        mShowHelp = false;   // the guide is in the way of the world: a click puts it away
        return true;
    }
    if (!WolfFlight::instance().active() || y > mPanelTop || y < mPanelBottom)
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        if (hasFocus())
        {
            setFocus(false);
        }
        return true;   // the panel itself: not a click on the world behind it
    }
    const EHit id = hit->mId;
    if (id == H_CDU_SCREEN || (id >= H_CDU_DIR && id <= H_LSK_R6))
    {
        setFocus(true);
    }
    else if (hasFocus())
    {
        setFocus(false);
    }
    mPressed = id;
    mPressedAt = now();
    mLastRepeat = now();
    mPressStep = 0;
    // knobs: left half turns down, right half up
    if (id == H_SPD_KNOB || id == H_HDG_KNOB || id == H_ALT_KNOB || id == H_VS_WHEEL || id == H_RANGE_KNOB || id == H_TWA_KNOB)
    {
        if (id == H_VS_WHEEL)
        {
            mPressStep = y > (hit->mB + hit->mT) * 0.5f ? 1 : -1;
        }
        else
        {
            mPressStep = x > (hit->mL + hit->mR) * 0.5f ? 1 : -1;
        }
        knob(id, mPressStep);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (id == H_STICK || id == H_THROTTLE)
    {
        gFocusMgr.setMouseCapture(this);
        dragControl(x, y);
        return true;
    }
    press(id, x, y, 0);
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfFlightDeck::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    // the stick and the lever spring back when let go
    WolfFlight::instance().setStick(0.f, 0.f);
    WolfFlight::instance().setThrottleLever(0.f);
    const bool was = mPressed != H_NONE;
    // CLR held: clear the whole scratchpad
    if (mPressed == H_CDU_CLR && now() - mPressedAt > 0.6)
    {
        mScratch.clear();
        mScratchMsg.clear();
        mDelete = false;
    }
    mPressed = H_NONE;
    mPressStep = 0;
    return was || (WolfFlight::instance().active() && y <= mPanelTop && y >= mPanelBottom);
}

bool WolfFlightDeck::handleHover(S32 x, S32 y, MASK mask)
{
    if (!WolfFlight::instance().active() || ((y > mPanelTop || y < mPanelBottom) && !hasMouseCapture()))
    {
        return false;
    }
    if (hasMouseCapture() && (mPressed == H_STICK || mPressed == H_THROTTLE))
    {
        dragControl(x, y);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool WolfFlightDeck::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!WolfFlight::instance().active() || y > mPanelTop || y < mPanelBottom)
    {
        return false;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        knob(hit->mId, -clicks);   // wheel up (negative clicks) turns clockwise
    }
    return true;
}

bool WolfFlightDeck::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    // no context menu for the world through the panel
    return WolfFlight::instance().active() && y <= mPanelTop && y >= mPanelBottom;
}

bool WolfFlightDeck::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!WolfFlight::instance().active() || y > mPanelTop || y < mPanelBottom)
    {
        return false;
    }
    // a double click is two presses on the panel (knobs turn twice, as you would expect)
    return handleMouseDown(x, y, mask);
}

bool WolfFlightDeck::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (!WolfFlight::instance().active() || y > mPanelTop || y < mPanelBottom)
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

void WolfFlightDeck::onFocusLost()
{
    LLUICtrl::onFocusLost();
}

void WolfFlightDeck::onMouseCaptureLost()
{
    // dragged off and lost (alt-tab, a dialog): nothing stays held
    WolfFlight::instance().setStick(0.f, 0.f);
    WolfFlight::instance().setThrottleLever(0.f);
    mPressed = H_NONE;
    mPressStep = 0;
}

bool WolfFlightDeck::handleKeyHere(KEY key, MASK mask)
{
    if (!hasFocus() || !WolfFlight::instance().active())
    {
        return false;
    }
    switch (key)
    {
    case KEY_BACKSPACE:
        press(H_CDU_CLR, 0, 0, 0);
        return true;
    case KEY_DELETE:
        press(H_CDU_DEL, 0, 0, 0);
        return true;
    case KEY_RETURN:
        // Enter with a destination typed on DIRECT TO: put it in and fly it, no line keys
        // needed (Paul: "i cant work out how to set the autopilot").
        if (mPage == PAGE_DIR && !mScratch.empty() && !mDelete)
        {
            lsk(true, 0);
            if (mModified && mScratchMsg.empty())
            {
                execRoute();
            }
            return true;
        }
        press(H_CDU_EXEC, 0, 0, 0);
        return true;
    case KEY_ESCAPE:
        setFocus(false);
        return true;
    default:
        break;
    }
    // Everything else typed goes to the scratchpad (handleUnicodeCharHere); keep the flight
    // keys away from the aircraft while typing.
    return mask == MASK_NONE || mask == MASK_SHIFT;
}

bool WolfFlightDeck::handleUnicodeCharHere(llwchar uni_char)
{
    if (!hasFocus() || !WolfFlight::instance().active())
    {
        return false;
    }
    if (uni_char >= 32 && uni_char < 127 && mScratch.size() < 24)
    {
        if (!mScratchMsg.empty())
        {
            mScratchMsg.clear();
        }
        mDelete = false;
        mScratch += (char)uni_char;
    }
    return true;
}

void WolfFlightDeck::knob(EHit id, S32 step)
{
    WolfFlight& f = WolfFlight::instance();
    const bool big = (gKeyboard && (gKeyboard->currentMask(false) & MASK_SHIFT));
    switch (id)
    {
    case H_SPD_KNOB: f.setSelSpeedKt(f.selSpeedKt() + step * (big ? 10.f : 1.f)); break;
    case H_HDG_KNOB: f.setSelHeading(f.selHeading() + step * (big ? 10.f : 1.f)); break;
    case H_TWA_KNOB: f.setSelTWA(f.selTWA() + step * (big ? 10.f : 1.f)); break;
    case H_ALT_KNOB: f.setSelAltFt(100.f * llround(f.selAltFt() / 100.f) + step * (big ? 1000.f : 100.f)); break;
    case H_VS_WHEEL:
        f.setSelVsFpm(f.selVsFpm() + step * 100.f);
        if (f.vertical() != WolfFlight::VERT_VS)
        {
            f.pressVS();
            f.setSelVsFpm(100.f * llround(f.data().mVS * FPM / 100.f) + step * 100.f);
        }
        break;
    case H_RANGE_KNOB:
        gSavedSettings.setS32("WolfFlightNDRange", llclamp(gSavedSettings.getS32("WolfFlightNDRange") + step, 0, ND_RANGE_COUNT - 1));
        break;
    default:
        break;
    }
}

void WolfFlightDeck::press(EHit id, S32 x, S32 y, S32 step)
{
    WolfFlight& f = WolfFlight::instance();
    switch (id)
    {
    case H_MASTER_WARN:
    case H_MASTER_CAUT: f.cancelMasters(); break;
    case H_AT: f.toggleAT(); break;
    case H_LNAV: f.pressLNAV(); break;
    case H_VNAV: f.pressVNAV(); break;
    case H_HDG_SEL: f.pressHDG(); break;
    case H_ALT_HOLD: f.pressALT(); break;
    case H_VS: f.pressVS(); break;
    case H_CMD: f.engageAP(); break;
    case H_HUD: gSavedSettings.setBOOL("WolfFlightHUD", !gSavedSettings.getBOOL("WolfFlightHUD")); break;
    case H_PLAN:
    case H_ND_SCREEN: gSavedSettings.setBOOL("WolfFlightNDPlan", !gSavedSettings.getBOOL("WolfFlightNDPlan")); break;
    case H_WIND: f.pressWIND(); break;
    case H_TACK: f.pressTACK(); break;
    case H_SAIL: f.sendSailCommand(); break;
    case H_WEB:
        // Paul: the sailing club and the airports pages on the website
        LLWeb::loadURL(f.sailing() ? "https://www.wolf-grid.com/index.php?f=wtgs" : "https://www.wolf-grid.com/index.php?f=ap");
        break;
    case H_MAP:
    {
        // Paul: "overlay it on the world map ... add a map button to the controller ... and dont
        // forget to zoom". The World Map opens fitted to the trip: the boat or plane and every
        // turning point of the route (or the destination); the route itself is drawn by
        // LLWorldMapView::drawWolfRoute.
        const WolfFlight::Data& d = f.data();
        F64 x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        auto take = [&](const LLVector3d& p)
        {
            x0 = llmin(x0, p.mdV[VX]); y0 = llmin(y0, p.mdV[VY]);
            x1 = llmax(x1, p.mdV[VX]); y1 = llmax(y1, p.mdV[VY]);
        };
        if (d.mValid) take(d.mPosGlobal);
        if (f.route().mValid) { for (const LLVector3d& p : f.route().mPts) take(p); }
        else if (f.dest().mValid) take(f.dest().mGlobal);
        LLFloaterReg::showInstance("world_map");
        LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance();
        if (map && x1 >= x0)
        {
            map->wolfShowArea(LLVector3d((x0 + x1) * 0.5, (y0 + y1) * 0.5, 0.0), llmax(x1 - x0, y1 - y0));
        }
        break;
    }
    case H_HELP:
    case H_CDU_HELP: mShowHelp = !mShowHelp; break;
    case H_EXIT: f.setActive(false); break;
    case H_GEAR: f.sendGearCommand(); break;
    case H_ENGINE: f.sendEngineCommand(); break;
    case H_CDU_DIR: mPage = PAGE_DIR; break;
    case H_CDU_CTL: mPage = PAGE_CTL; break;
    case H_CDU_EXEC:
        if (mPage == PAGE_DIR && mModified)
        {
            execRoute();
        }
        break;
    case H_CDU_CLR:
        if (mDelete) mDelete = false;
        else if (!mScratchMsg.empty()) mScratchMsg.clear();
        else if (!mScratch.empty()) mScratch.pop_back();
        break;
    case H_CDU_DEL:
        if (mScratch.empty())
        {
            mDelete = !mDelete;
            mScratchMsg.clear();
        }
        break;
    default:
        if (id >= H_LSK_L1 && id <= H_LSK_L6)
        {
            lsk(true, id - H_LSK_L1);
        }
        else if (id >= H_LSK_R1 && id <= H_LSK_R6)
        {
            lsk(false, id - H_LSK_R1);
        }
        break;
    }
}

void WolfFlightDeck::execRoute()
{
    WolfFlight& f = WolfFlight::instance();
    if (!mModHasXY)
    {
        mModX = mModY = 128.f;   // a region alone: its middle
        mModHasXY = true;
    }
    f.setDestination(mModRegion, LLVector3(mModX, mModY, mModZ), mModHasZ);
    mModified = false;
    mArmRoute = true;   // LNAV + VNAV as soon as the region is known (draw())
}

void WolfFlightDeck::lsk(bool left, S32 line)
{
    WolfFlight& f = WolfFlight::instance();
    const std::string sp = mScratch;
    const bool have = !sp.empty();
    auto invalid = [&]() { mScratchMsg = "INVALID ENTRY"; };

    if (mPage == PAGE_DIR)
    {
        if (left && line == 0)
        {
            if (mDelete)
            {
                mModRegion.clear();
                mModified = true;
                mDelete = false;
                return;
            }
            if (!have)
            {
                mScratch = mModRegion;
                return;
            }
            // "REGION" or "REGION/X/Y" or "REGION/X/Y/Z"
            const std::vector<std::string> parts = splitSlash(sp);
            std::string region = parts[0];
            LLStringUtil::trim(region);
            F32 vx, vy, vz;
            if (parts.size() >= 3)
            {
                if (!parseFloat(parts[1], vx) || !parseFloat(parts[2], vy)) { invalid(); return; }
                mModX = vx; mModY = vy; mModHasXY = true;
                mModHasZ = false;
                if (parts.size() >= 4)
                {
                    if (!parseFloat(parts[3], vz)) { invalid(); return; }
                    mModZ = vz; mModHasZ = true;
                }
            }
            else if (parts.size() == 2)
            {
                invalid();
                return;
            }
            else
            {
                // Paul: "if the user types just the region assume 128/128/0". Z 0 means as low as
                // the terrain floor allows (WolfFlight::guidance never goes under it).
                mModX = 128.f; mModY = 128.f; mModZ = 0.f;
                mModHasXY = true;
                mModHasZ = true;
            }
            mModRegion = region;
            mModified = true;
            mScratch.clear();
            return;
        }
        if (left && line == 1)
        {
            if (mDelete)
            {
                mModHasXY = mModHasZ = false;
                mModified = true;
                mDelete = false;
                return;
            }
            if (!have)
            {
                if (mModHasXY) mScratch = mModHasZ ? fmt("%d/%d/%d", (S32)mModX, (S32)mModY, (S32)mModZ) : fmt("%d/%d", (S32)mModX, (S32)mModY);
                return;
            }
            const std::vector<std::string> parts = splitSlash(sp);
            F32 vx, vy, vz;
            if (parts.size() < 2 || parts.size() > 3 || !parseFloat(parts[0], vx) || !parseFloat(parts[1], vy)) { invalid(); return; }
            if (parts.size() == 3 && !parts[2].empty())
            {
                if (!parseFloat(parts[2], vz)) { invalid(); return; }
                mModZ = vz;
                mModHasZ = true;
            }
            else
            {
                mModHasZ = false;
            }
            mModX = vx; mModY = vy; mModHasXY = true;
            mModified = true;
            mScratch.clear();
            return;
        }
        if (left && line == 2)
        {
            if (f.setDestinationFromMap())
            {
                syncRouteFromDest();
                f.pressLNAV();
                f.pressVNAV();
            }
            return;
        }
        if (left && line == 3)
        {
            if (f.sailing())
            {
                return;   // the water route is planned, not typed
            }
            if (!have)
            {
                mScratch = fmt("%d", (S32)llround(f.cruiseAltFt()));
                return;
            }
            std::string s = sp;
            F32 v;
            bool fl = false;
            if (s.size() > 2 && (s[0] == 'F' || s[0] == 'f') && (s[1] == 'L' || s[1] == 'l'))
            {
                s = s.substr(2);
                fl = true;
            }
            if (!parseFloat(s, v) || v < 0.f) { invalid(); return; }
            f.setCruiseAltFt(fl ? v * 100.f : v);
            mScratch.clear();
            return;
        }
        if (left && line == 5)
        {
            if (mModified)
            {
                syncRouteFromDest();   // ERASE the modification
            }
            else
            {
                f.clearDestination();
                syncRouteFromDest();
            }
            return;
        }
        if (!left && line == 5)
        {
            if (mModified)
            {
                execRoute();
            }
            return;
        }
        return;
    }

    // CONTROLS page
    auto cyclePair = [&](const char* setting)
    {
        if (have) { invalid(); return; }
        gSavedSettings.setS32(setting, (gSavedSettings.getS32(setting) + 1) % ((S32)WolfFlight::PAIR_NONE + 1));
    };
    auto toggle = [&](const char* setting)
    {
        if (have) { invalid(); return; }
        gSavedSettings.setBOOL(setting, !gSavedSettings.getBOOL(setting));
    };
    // a key's "reversed" choice: the value in use turned round, saved, any learned flip cleared
    auto toggleInv = [&](const char* setting)
    {
        if (have) { invalid(); return; }
        WolfFlight::setInvert(setting, !WolfFlight::invert(setting));
    };
    auto setCommand = [&](const char* setting)
    {
        if (mDelete)
        {
            gSavedSettings.setString(setting, "");
            mDelete = false;
            return;
        }
        if (!have)
        {
            mScratch = gSavedSettings.getString(setting);
            return;
        }
        gSavedSettings.setString(setting, sp);
        mScratch.clear();
    };
    auto setChannel = [&](const char* setting)
    {
        if (!have)
        {
            mScratch = fmt("%d", gSavedSettings.getS32(setting));
            return;
        }
        F32 v;
        if (!parseFloat(sp, v) || v != floorf(v) || fabsf(v) > 2147483647.f) { invalid(); return; }
        gSavedSettings.setS32(setting, (S32)v);
        mScratch.clear();
    };
    auto setNumber = [&](const char* setting, F32 lo, F32 hi)
    {
        if (!have)
        {
            mScratch = fmt("%g", gSavedSettings.getF32(setting));
            return;
        }
        F32 v;
        std::string t = sp;
        if (!t.empty() && (t.back() == 'M' || t.back() == 'm')) t.pop_back();
        if (!parseFloat(t, v) || v < lo || v > hi) { invalid(); return; }
        gSavedSettings.setF32(setting, v);
        mScratch.clear();
        f.rebuildRoute();
    };
    if (f.sailing())
    {
        if (left)
        {
            switch (line)
            {
            case 0: setNumber("WolfSailTackAngle", 25.f, 80.f); break;
            case 1: setNumber("WolfSailDraft", 0.f, 20.f); break;
            case 2: cyclePair(WolfFlight::keySetting("BankPair")); break;
            case 3: cyclePair(WolfFlight::keySetting("ThrottlePair")); break;
            case 4: setCommand("WolfSailCommand"); break;
            case 5: setCommand("WolfFlightEngineCommand"); break;
            default: break;
            }
        }
        else
        {
            switch (line)
            {
            case 0: toggle("WolfFlightAutoLearn"); break;
            case 2: toggleInv(WolfFlight::keySetting("BankInvert")); break;
            case 3:
            {
                if (have) { invalid(); return; }
                const bool steps = gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps"));
                const bool inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
                const S32 next = ((steps ? 0 : 2) + (inv ? 1 : 0) + 1) % 4;
                gSavedSettings.setBOOL(WolfFlight::keySetting("ThrottleSteps"), next < 2);
                WolfFlight::setInvert(WolfFlight::keySetting("ThrottleInvert"), (next % 2) == 1);
                break;
            }
            case 4: setChannel("WolfSailCommandChannel"); break;
            case 5: setChannel("WolfFlightEngineChannel"); break;
            default: break;
            }
        }
        return;
    }
    if (left)
    {
        switch (line)
        {
        case 0:
            if (have) { invalid(); return; }
            gSavedSettings.setS32("WolfFlightCraft", WolfFlight::craft() == WolfFlight::CRAFT_HELI ? 0 : 1);
            break;
        case 1: cyclePair("WolfFlightPitchPair"); break;
        case 2: cyclePair(WolfFlight::keySetting("BankPair")); break;
        case 3: cyclePair(WolfFlight::keySetting("ThrottlePair")); break;
        case 4: setCommand("WolfFlightGearCommand"); break;
        case 5: setCommand("WolfFlightEngineCommand"); break;
        default: break;
        }
    }
    else
    {
        switch (line)
        {
        case 0: toggle("WolfFlightAutoLearn"); break;
        case 1: toggleInv("WolfFlightPitchInvert"); break;
        case 2: toggleInv(WolfFlight::keySetting("BankInvert")); break;
        case 3:
            if (have) { invalid(); return; }
            if (WolfFlight::craft() == WolfFlight::CRAFT_HELI)
            {
                toggleInv(WolfFlight::keySetting("ThrottleInvert"));
            }
            else
            {
                // STEP -> STEP REV -> HOLD -> HOLD REV
                const bool steps = gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps"));
                const bool inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
                const S32 state = (steps ? 0 : 2) + (inv ? 1 : 0);
                const S32 next = (state + 1) % 4;
                gSavedSettings.setBOOL(WolfFlight::keySetting("ThrottleSteps"), next < 2);
                WolfFlight::setInvert(WolfFlight::keySetting("ThrottleInvert"), (next % 2) == 1);
            }
            break;
        case 4: setChannel("WolfFlightGearChannel"); break;
        case 5: setChannel("WolfFlightEngineChannel"); break;
        default: break;
        }
    }
}
