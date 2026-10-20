/**
 * @file wolfpaint.h
 * @brief WolfViewer: the small painting kit the drawn Wolf windows share - triangles, rounded plates, rings, lettering.
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

#ifndef WOLF_PAINT_H
#define WOLF_PAINT_H

#include <cmath>
#include <string>
#include <vector>

#include "llfontgl.h"
#include "llfontregistry.h"
#include "llformat.h"
#include "llframetimer.h"
#include "llmath.h"
#include "llrect.h"
#include "llrender.h"
#include "llui.h"
#include "llview.h"
#include "v2math.h"
#include "v4color.h"

/**
 * [LOOK 2026-10-20] Paul: "upgrade the mesh upload floater make it look really nice as well and our standard object ai
 * generator ... as nice as the new avatar and clothing generator but not the same". Both windows are painted in code behind
 * their XUI controls, the technique of the radio and WolfAvatars (wolffloaterradio.cpp WolfRadioBackdrop,
 * llfloaterwolfavatars.cpp WolfAvatarsBackdrop): an LLView behind every widget that paints with gGL triangles. The
 * primitives are the same ones; they live here once for the two new windows (Upload Model: wolffloatermeshupload.cpp, the
 * "Drawing Office"; Create with AI: wolfai.cpp, "The Forge") instead of a third private copy.
 * Source for each function: llfloaterwolfavatars.cpp:150-631 (which cites wolffloaterradio.cpp for most of them).
 * Angles are compass-style: radians clockwise from screen-up, point (sin a, cos a).
 */
namespace WolfPaint
{
    inline LLColor4 withAlpha(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    // Source: llfloaterwolfavatars.cpp:156 lerpColour (wolffloaterradio.cpp:105)
    inline LLColor4 lerpColour(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[VRED] + (b.mV[VRED] - a.mV[VRED]) * t,
                        a.mV[VGREEN] + (b.mV[VGREEN] - a.mV[VGREEN]) * t,
                        a.mV[VBLUE] + (b.mV[VBLUE] - a.mV[VBLUE]) * t,
                        a.mV[VALPHA] + (b.mV[VALPHA] - a.mV[VALPHA]) * t);
    }

    // Source: llfloaterwolfavatars.cpp:164 lighten
    inline LLColor4 lighten(const LLColor4& c, F32 amount)
    {
        return LLColor4(llclamp(c.mV[VRED] + amount, 0.f, 1.f), llclamp(c.mV[VGREEN] + amount, 0.f, 1.f),
                        llclamp(c.mV[VBLUE] + amount, 0.f, 1.f), c.mV[VALPHA]);
    }

    // Source: llfloaterwolfavatars.cpp:171 hash01 (wolffloaterradio.cpp:119) - a repeatable pseudo-random 0..1
    inline F32 hash01(F32 n)
    {
        const F32 s = sinf(n * 12.9898f) * 43758.5453f;
        return s - floorf(s);
    }

    // Source: llfloaterwolfavatars.cpp:177 grown
    inline LLRect grown(const LLRect& r, S32 d)
    {
        return LLRect(r.mLeft - d, r.mTop + d, r.mRight + d, r.mBottom - d);
    }

    /** The current time for animation, wrapped so it stays precise in F32. Source: llfloaterwolfavatars.cpp:628 animSeconds
     *  (llframetimer.h getElapsedSeconds: seconds since the viewer started, updated once a frame). */
    inline F32 animSeconds()
    {
        return (F32)fmod((F64)LLFrameTimer::getElapsedSeconds().value(), 3600.0);
    }

    // Source: llfloaterwolfavatars.cpp:232 text (wolffloaterradio.cpp:187) - centred vertically on y; cut with an ellipsis
    // if wider than max_w
    inline void text(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c,
                     LLFontGL::HAlign h = LLFontGL::HCENTER, F32 max_w = 1.e6f)
    {
        if (s.empty() || !font) return;
        if (font->getWidthF32(s) <= max_w)
        {
            font->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            return;
        }
        const F32 left = h == LLFontGL::HCENTER ? x - max_w * 0.5f : h == LLFontGL::RIGHT ? x - max_w : x;
        font->renderUTF8(s, 0, left, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, (S32)max_w, nullptr, true);
    }

    /** The WolfDisplay family (Cinzel Bold, fonts.xml, registered in every font set) at the Avionics0..11 sizes (6..26 pt),
     *  smallest first; the sans serif when it is missing. Source: llfloaterwolfavatars.cpp:185 displayFonts
     *  (wolffloaterradio.cpp:129-151). */
    inline const std::vector<const LLFontGL*>& displayFonts()
    {
        static std::vector<const LLFontGL*> fonts;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            for (S32 i = 0; i < 12; ++i)
            {
                const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("WolfDisplay", llformat("Avionics%d", i), LLFontGL::NORMAL));
                if (f) fonts.push_back(f);
            }
            if (fonts.empty()) fonts.push_back(LLFontGL::getFontSansSerif());
        }
        return fonts;
    }

    /** The largest display size whose line fits px and whose widest string fits max_w (the smallest from min_index when
     *  none does). Source: llfloaterwolfavatars.cpp:208 fitFont (wolffloaterradio.cpp:155-179). */
    inline const LLFontGL* fitDisplayFont(F32 px, F32 max_w, const std::vector<std::string>& strings, S32 min_index = 0)
    {
        const std::vector<const LLFontGL*>& fonts = displayFonts();
        const S32 first = llclamp(min_index, 0, (S32)fonts.size() - 1);
        const LLFontGL* best = fonts[first];
        for (S32 i = first; i < (S32)fonts.size(); ++i)
        {
            const LLFontGL* f = fonts[i];
            if ((F32)f->getLineHeight() > px) break;
            bool fits = true;
            for (const std::string& s : strings)
            {
                if (f->getWidthF32(s) > max_w)
                {
                    fits = false;
                    break;
                }
            }
            if (fits) best = f;
        }
        return best;
    }

    /** The width of s drawn letter by letter with `spacing` px between letters (spacedText). ASCII only: the windows'
     *  own titles, never resident text. */
    inline F32 spacedWidth(const LLFontGL* font, const std::string& s, F32 spacing)
    {
        if (!font || s.empty()) return 0.f;
        F32 w = 0.f;
        for (char ch : s) w += font->getWidthF32(std::string(1, ch)) + spacing;
        return w - spacing;
    }

    /** Lettering with open letter spacing (a draughtsman's title), left-aligned at x, centred vertically on y. */
    inline void spacedText(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c, F32 spacing)
    {
        if (!font) return;
        for (char ch : s)
        {
            const std::string one(1, ch);
            font->renderUTF8(one, 0, x, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            x += font->getWidthF32(one) + spacing;
        }
    }

    // ---- geometry: everything is triangles (llfloaterwolfavatars.cpp:257-386, wolffloaterradio.cpp:209-309) ----

    inline void untextured()
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    inline void vtx(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

    inline void tri(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2, const LLColor4& c)
    {
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0, y0, c);
        vtx(x1, y1, c);
        vtx(x2, y2, c);
        gGL.end();
    }

    /** A convex four-sided shape, its corners in order round the edge. */
    inline void quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0, y0, c); vtx(x1, y1, c); vtx(x2, y2, c);
        vtx(x0, y0, c); vtx(x2, y2, c); vtx(x3, y3, c);
        gGL.end();
    }

    /** An axis-aligned box, its colour running from top to bottom. */
    inline void box(F32 l, F32 t, F32 r, F32 b, const LLColor4& top, const LLColor4& bottom)
    {
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bottom);
        vtx(r, t, top);
        vtx(l, t, top);
        vtx(l, b, bottom);
        vtx(r, b, bottom);
        vtx(r, t, top);
        gGL.end();
    }

    inline S32 segmentsFor(F32 r)
    {
        return llclamp((S32)(r * 0.8f), 12, 120);
    }

    /** An ellipse, its centre colour at the middle and its rim colour at the edge; a soft glow when rim is clear. */
    inline void ellipse(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& centre, const LLColor4& rim)
    {
        const S32 n = segmentsFor(llmax(rx, ry));
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * (F32)i / (F32)n;
            const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)n;
            vtx(cx, cy, centre);
            vtx(cx + rx * sinf(a0), cy + ry * cosf(a0), rim);
            vtx(cx + rx * sinf(a1), cy + ry * cosf(a1), rim);
        }
        gGL.end();
    }

    inline void disc(F32 cx, F32 cy, F32 r, const LLColor4& centre, const LLColor4& rim)
    {
        ellipse(cx, cy, r, r, centre, rim);
    }

    /** The band r0..r1 between angles a0 and a1 (clockwise from up), its colour running from c0 at a0 to c1 at a1. */
    inline void arcBand(F32 cx, F32 cy, F32 r0, F32 r1, F32 a0, F32 a1, const LLColor4& c0, const LLColor4& c1)
    {
        if (a1 <= a0) return;
        const S32 n = llmax(2, (S32)((F32)segmentsFor(r1) * (a1 - a0) / F_TWO_PI) + 1);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 t0 = (F32)i / (F32)n, t1 = (F32)(i + 1) / (F32)n;
            const F32 b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
            const LLColor4 k0 = lerpColour(c0, c1, t0), k1 = lerpColour(c0, c1, t1);
            const F32 s0 = sinf(b0), q0 = cosf(b0), s1 = sinf(b1), q1 = cosf(b1);
            vtx(cx + r0 * s0, cy + r0 * q0, k0);
            vtx(cx + r1 * s0, cy + r1 * q0, k0);
            vtx(cx + r1 * s1, cy + r1 * q1, k1);
            vtx(cx + r0 * s0, cy + r0 * q0, k0);
            vtx(cx + r1 * s1, cy + r1 * q1, k1);
            vtx(cx + r0 * s1, cy + r0 * q1, k1);
        }
        gGL.end();
    }

    inline void ring(F32 cx, F32 cy, F32 r0, F32 r1, const LLColor4& c)
    {
        arcBand(cx, cy, r0, r1, 0.f, F_TWO_PI, c, c);
    }

    // Source: llfloaterwolfavatars.cpp:353 line (wolffloaterradio.cpp:295) - a line w px wide as two triangles
    inline void line(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    }

    /** A dashed line: dash px drawn, gap px skipped (a hidden edge on a drawing). */
    inline void dashed(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, F32 dash, F32 gap, const LLColor4& c)
    {
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f || dash <= 0.f) return;
        const F32 ux = dx / len, uy = dy / len;
        for (F32 d = 0.f; d < len; d += dash + gap)
        {
            const F32 e = llmin(len, d + dash);
            line(x0 + ux * d, y0 + uy * d, x0 + ux * e, y0 + uy * e, w, c);
        }
    }

    /** A line with round ends. */
    inline void limb(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        line(x0, y0, x1, y1, w, c);
        disc(x0, y0, w * 0.5f, c, c);
        disc(x1, y1, w * 0.5f, c, c);
    }

    /** A check mark: a state in a shape as well as a colour. Source: llfloaterwolfavatars.cpp:620 checkMark */
    inline void checkMark(F32 cx, F32 cy, F32 s, F32 w, const LLColor4& c)
    {
        limb(cx - s * 0.45f, cy + s * 0.02f, cx - s * 0.12f, cy - s * 0.30f, w, c);
        limb(cx - s * 0.12f, cy - s * 0.30f, cx + s * 0.48f, cy + s * 0.34f, w, c);
    }

    /** A small four-pointed star (a spark). Source: llfloaterwolfavatars.cpp:1964 sparkle */
    inline void sparkle(F32 x, F32 y, F32 s, const LLColor4& c)
    {
        quad(x, y + s * 1.6f, x + s * 0.5f, y, x, y - s * 1.6f, x - s * 0.5f, y, c);
        quad(x - s * 1.6f, y, x, y + s * 0.5f, x + s * 1.6f, y, x, y - s * 0.5f, c);
    }

    // ---- rounded rectangles (llfloaterwolfavatars.cpp:389-474, wolffloaterradio.cpp:311-405) ----
    struct Radii
    {
        F32 tl, tr, br, bl;
    };

    inline Radii allCorners(F32 r)
    {
        return Radii{ r, r, r, r };
    }

    /** The outline of rect shrunk by inset, corners concentric. Source: llfloaterwolfavatars.cpp:400 roundedOutline */
    inline std::vector<LLVector2> roundedOutline(const LLRect& rect, const Radii& rad, F32 inset)
    {
        constexpr S32 SEG = 10;
        const F32 l = (F32)rect.mLeft + inset, t = (F32)rect.mTop - inset, r = (F32)rect.mRight - inset, b = (F32)rect.mBottom + inset;
        const F32 max_r = llmax(0.f, llmin(r - l, t - b) * 0.5f);
        auto radius = [&](F32 v) { return llclamp(v - inset, 0.f, max_r); };
        std::vector<LLVector2> pts;
        pts.reserve(4 * (SEG + 1));
        auto corner = [&](F32 cx, F32 cy, F32 cr, F32 a0, F32 a1)
        {
            for (S32 i = 0; i <= SEG; ++i)
            {
                const F32 a = (a0 + (a1 - a0) * (F32)i / (F32)SEG) * DEG_TO_RAD;
                pts.emplace_back(cx + cr * cosf(a), cy + cr * sinf(a));
            }
        };
        const F32 rtl = radius(rad.tl), rtr = radius(rad.tr), rbr = radius(rad.br), rbl = radius(rad.bl);
        corner(l + rtl, t - rtl, rtl, 180.f, 90.f);
        corner(r - rtr, t - rtr, rtr, 90.f, 0.f);
        corner(r - rbr, b + rbr, rbr, 0.f, -90.f);
        corner(l + rbl, b + rbl, rbl, -90.f, -180.f);
        return pts;
    }

    /** Filled, its colour running from top to bottom. Source: llfloaterwolfavatars.cpp:425 fillRounded */
    inline void fillRounded(const LLRect& rect, const Radii& rad, const LLColor4& top, const LLColor4& bottom)
    {
        const std::vector<LLVector2> pts = roundedOutline(rect, rad, 0.f);
        const F32 h = llmax(1.f, (F32)rect.getHeight());
        auto colourAt = [&](F32 y) { return lerpColour(bottom, top, llclamp((y - (F32)rect.mBottom) / h, 0.f, 1.f)); };
        const F32 cx = (F32)rect.getCenterX(), cy = (F32)rect.getCenterY();
        const LLColor4 cc = colourAt(cy);
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const LLVector2& p0 = pts[i];
            const LLVector2& p1 = pts[(i + 1) % pts.size()];
            vtx(cx, cy, cc);
            vtx(p0.mV[VX], p0.mV[VY], colourAt(p0.mV[VY]));
            vtx(p1.mV[VX], p1.mV[VY], colourAt(p1.mV[VY]));
        }
        gGL.end();
    }

    /** The band between two concentric outlines, coloured top to bottom. Source: llfloaterwolfavatars.cpp:445 strokeRounded */
    inline void strokeRounded(const LLRect& rect, const Radii& rad, F32 inset0, F32 inset1, const LLColor4& top, const LLColor4& bottom)
    {
        const std::vector<LLVector2> o = roundedOutline(rect, rad, inset0);
        const std::vector<LLVector2> in = roundedOutline(rect, rad, inset1);
        const F32 h = llmax(1.f, (F32)rect.getHeight());
        auto colourAt = [&](F32 y) { return lerpColour(bottom, top, llclamp((y - (F32)rect.mBottom) / h, 0.f, 1.f)); };
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i < o.size(); ++i)
        {
            const size_t j = (i + 1) % o.size();
            vtx(o[i].mV[VX], o[i].mV[VY], colourAt(o[i].mV[VY]));
            vtx(o[j].mV[VX], o[j].mV[VY], colourAt(o[j].mV[VY]));
            vtx(in[j].mV[VX], in[j].mV[VY], colourAt(in[j].mV[VY]));
            vtx(o[i].mV[VX], o[i].mV[VY], colourAt(o[i].mV[VY]));
            vtx(in[j].mV[VX], in[j].mV[VY], colourAt(in[j].mV[VY]));
            vtx(in[i].mV[VX], in[i].mV[VY], colourAt(in[i].mV[VY]));
        }
        gGL.end();
    }

    /** A soft halo round rect: layers growing outwards, each fainter. Source: llfloaterwolfavatars.cpp:466 halo */
    inline void halo(const LLRect& rect, F32 radius, S32 spread, const LLColor4& c)
    {
        constexpr S32 LAYERS = 4;
        for (S32 i = LAYERS; i >= 1; --i)
        {
            const S32 d = spread * i / LAYERS;
            fillRounded(grown(rect, d), allCorners(radius + (F32)d), withAlpha(c, 0.30f / (F32)i), withAlpha(c, 0.30f / (F32)i));
        }
    }

    // Source: llfloaterwolfavatars.cpp:484 mouseOver (wolffloaterradio.cpp:474)
    inline bool mouseOver(LLView* v)
    {
        S32 x = 0, y = 0;
        LLUI::getInstance()->getMousePositionLocal(v, &x, &y);
        return v->pointInView(x, y);
    }

    /** v's rectangle in other's coordinates (a widget inside a step panel, for the backdrop that paints under it). */
    inline LLRect rectIn(const LLView* v, const LLView* other)
    {
        LLRect out;
        if (v && other) v->localRectToOtherView(v->getLocalRect(), &out, other);
        return out;
    }
}

#endif // WOLF_PAINT_H
