/**
 * @file wolffloaterradio.cpp
 * @brief WolfViewer: the Radio Stations floater. See wolffloaterradio.h.
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterradio.h"

#include "llbutton.h"
#include "lldraghandle.h"
#include "llfloaterland.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llwindow.h"
#include "wolfradiostations.h"

#include <cmath>

namespace
{
    // ---- palette: walnut, ivory, bakelite, chrome, the amber dial and the green magic eye ----
    // Contrast (WCAG 2.x relative luminance) for the text that carries information:
    //   status cream #f2e0b8 on the maker's plate #1a0f09: 14.5:1; status error #ff8c73 there: 8.3:1
    //   dial ink #2b1c12 on the glass at its dimmest (cold rim #dbb87a): 8.7:1; warm rim #edb861: 9.1:1
    //   the current genre in dial red #8c0f0a on the same glass: 5.1:1 (and underlined: not colour alone)
    //   key ink #2b1c12 on ivory #dbcca8 (bottom of a key): 10.3:1; on a pushed-in key #d9b273: 8.3:1
    //   button cream #f2e3bd on bakelite #3d2112: 11.6:1; on the lit "Stop" bakelite #731c12: 8.7:1
    //   card ink #2b1c12 on paper #ebdbb2: 12.0:1; on the selected row #d99e4c: 7.0:1
    //   knob labels cream #f2e3bd on the walnut top #5c361c: 8.3:1, on the bottom #382112: 11.8:1
    //   "ON AIR" cream on the plate: 14.8:1; "OFF AIR" #b29461 on the plate: 6.5:1
    // Fixed colours, not the skin's: this is a painted instrument and no skin may lower them.
    const LLColor4 STATUS_OK(0.95f, 0.88f, 0.72f, 1.f);      // cream on the maker's plate
    const LLColor4 STATUS_ERROR(1.f, 0.55f, 0.45f, 1.f);     // light red, readable on the same

    const LLColor4 BACKDROP(0.06f, 0.04f, 0.025f, 1.f);       // the wall behind the cabinet's shoulders
    const LLColor4 WALNUT_TOP(0.36f, 0.21f, 0.11f, 1.f);
    const LLColor4 WALNUT_BOTTOM(0.22f, 0.13f, 0.07f, 1.f);
    const LLColor4 WALNUT_DARK(0.10f, 0.06f, 0.03f, 1.f);
    const LLColor4 WALNUT_EDGE(0.62f, 0.40f, 0.22f, 0.55f);
    const LLColor4 GRAIN_DARK(0.08f, 0.04f, 0.02f, 1.f);
    const LLColor4 GRAIN_LIGHT(0.66f, 0.43f, 0.24f, 1.f);
    const LLColor4 CLOTH_TOP(0.52f, 0.42f, 0.28f, 1.f);
    const LLColor4 CLOTH_BOTTOM(0.37f, 0.29f, 0.19f, 1.f);
    const LLColor4 FRET_LIGHT(0.50f, 0.31f, 0.16f, 1.f);
    const LLColor4 FRET_DARK(0.24f, 0.14f, 0.07f, 1.f);
    const LLColor4 CHROME_LIGHT(0.95f, 0.95f, 0.93f, 1.f);
    const LLColor4 CHROME(0.70f, 0.70f, 0.70f, 1.f);
    const LLColor4 CHROME_DARK(0.30f, 0.30f, 0.32f, 1.f);
    const LLColor4 GLASS_COLD_CENTRE(0.93f, 0.82f, 0.60f, 1.f);
    const LLColor4 GLASS_COLD_RIM(0.86f, 0.72f, 0.48f, 1.f);
    const LLColor4 GLASS_WARM_CENTRE(1.f, 0.90f, 0.62f, 1.f);
    const LLColor4 GLASS_WARM_RIM(0.93f, 0.72f, 0.38f, 1.f);
    const LLColor4 INK(0.17f, 0.11f, 0.07f, 1.f);
    const LLColor4 DIAL_RED(0.55f, 0.06f, 0.04f, 1.f);
    const LLColor4 NEEDLE_RED(0.80f, 0.08f, 0.05f, 1.f);
    const LLColor4 NEEDLE_LIGHT(1.f, 0.40f, 0.30f, 1.f);
    const LLColor4 IVORY_TOP(0.99f, 0.96f, 0.87f, 1.f);
    const LLColor4 IVORY_BOTTOM(0.86f, 0.80f, 0.66f, 1.f);
    const LLColor4 IVORY_SIDE(0.66f, 0.58f, 0.44f, 1.f);
    const LLColor4 IVORY_PRESSED_TOP(0.97f, 0.86f, 0.62f, 1.f);
    const LLColor4 IVORY_PRESSED_BOTTOM(0.85f, 0.70f, 0.45f, 1.f);
    const LLColor4 LAMP_AMBER(1.f, 0.62f, 0.18f, 1.f);
    const LLColor4 BAKELITE_TOP(0.24f, 0.13f, 0.07f, 1.f);
    const LLColor4 BAKELITE_BOTTOM(0.11f, 0.065f, 0.04f, 1.f);
    const LLColor4 BAKELITE_SIDE(0.05f, 0.03f, 0.02f, 1.f);
    const LLColor4 BAKELITE_LIT_TOP(0.45f, 0.11f, 0.07f, 1.f);
    const LLColor4 BAKELITE_LIT_BOTTOM(0.30f, 0.06f, 0.04f, 1.f);
    const LLColor4 BAKELITE_LIGHT(0.55f, 0.36f, 0.24f, 1.f);
    const LLColor4 PAPER_TOP(0.97f, 0.93f, 0.81f, 1.f);
    const LLColor4 PAPER_BOTTOM(0.92f, 0.86f, 0.70f, 1.f);
    const LLColor4 PLATE(0.10f, 0.06f, 0.035f, 1.f);
    const LLColor4 BRASS(0.78f, 0.60f, 0.27f, 1.f);
    const LLColor4 BRASS_DARK(0.40f, 0.27f, 0.10f, 1.f);
    const LLColor4 BRASS_LIGHT(0.97f, 0.86f, 0.55f, 1.f);
    const LLColor4 TEXT_CREAM(0.95f, 0.89f, 0.74f, 1.f);
    const LLColor4 TEXT_DIM(0.70f, 0.58f, 0.38f, 1.f);
    const LLColor4 EYE_OFF(0.05f, 0.16f, 0.08f, 1.f);
    const LLColor4 EYE_ON(0.40f, 1.f, 0.55f, 1.f);
    const LLColor4 EYE_GLASS(0.02f, 0.05f, 0.03f, 1.f);
    const LLColor4 SHADOW(0.f, 0.f, 0.f, 0.40f);

    // ---- behaviour (design values, not protocol values) ----
    constexpr F32 NEEDLE_RATE = 7.f;     // per second: 1 - e^(-rate*dt) of the gap closes each frame (~0.4 s glide)
    constexpr F32 EYE_RATE = 3.f;        // the eye opens and the dial warms over about a second, like a valve
    constexpr F32 MAX_EASE_GAP = 0.5f;   // a longer frame gap (window hidden, viewer stalled) snaps instead
    constexpr F32 VOLUME_STEP = 0.05f;   // AudioLevelMusic is 0..1 (settings.xml, default 0.3)
    constexpr F32 KNOB_SWEEP = 135.f * DEG_TO_RAD;   // the Volume knob turns -135..+135 degrees
    constexpr S32 MIN_KEY_FONT = 1;      // Avionics1 (7 pt) - smaller than that is not readable on a key

    LLColor4 withAlpha(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    LLColor4 lerpColour(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[VRED] + (b.mV[VRED] - a.mV[VRED]) * t,
                        a.mV[VGREEN] + (b.mV[VGREEN] - a.mV[VGREEN]) * t,
                        a.mV[VBLUE] + (b.mV[VBLUE] - a.mV[VBLUE]) * t,
                        a.mV[VALPHA] + (b.mV[VALPHA] - a.mV[VALPHA]) * t);
    }

    LLColor4 lighten(const LLColor4& c, F32 amount)
    {
        return LLColor4(llmin(1.f, c.mV[VRED] + amount), llmin(1.f, c.mV[VGREEN] + amount), llmin(1.f, c.mV[VBLUE] + amount), c.mV[VALPHA]);
    }

    // A repeatable pseudo-random 0..1 for the wood grain, so it is the same every frame.
    F32 hash01(F32 n)
    {
        const F32 s = sinf(n * 12.9898f) * 43758.5453f;
        return s - floorf(s);
    }

    // ---- lettering: Cinzel Bold (WolfDisplay), as Tessa's Compass ----
    // The WolfDisplay family (fonts.xml: Cinzel-Bold.ttf, then the "default" font's files as
    // fallbacks, llfontregistry.cpp:511-523, so station names in other scripts still draw) at the
    // Avionics0..11 sizes (6..26 pt), smallest first - the registration wolffloatercompass.cpp uses.
    const std::vector<const LLFontGL*>& displayFonts()
    {
        static std::vector<const LLFontGL*> fonts;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            for (S32 i = 0; i < 12; ++i)
            {
                const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("WolfDisplay", llformat("Avionics%d", i), LLFontGL::NORMAL));
                if (f)
                {
                    fonts.push_back(f);
                }
            }
            if (fonts.empty())
            {
                LL_WARNS("WolfRadio") << "font WolfDisplay missing, using the sans serif" << LL_ENDL;
                fonts.push_back(LLFontGL::getFontSansSerif());
            }
        }
        return fonts;
    }

    // The largest size whose line fits px and whose widest string fits max_w (the smallest from
    // min_index when none does).
    const LLFontGL* fitFont(F32 px, F32 max_w, const std::vector<std::string>& strings, S32 min_index = 0)
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
            if (fits)
            {
                best = f;
            }
        }
        return best;
    }

    const LLFontGL* heightFont(F32 px)
    {
        return fitFont(px, 1.e6f, {});
    }

    // Text centred vertically on y; cut with an ellipsis if it is wider than max_w.
    void text(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER, F32 max_w = 1.e6f)
    {
        if (s.empty()) return;
        if (font->getWidthF32(s) <= max_w)
        {
            font->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            return;
        }
        const F32 left = h == LLFontGL::HCENTER ? x - max_w * 0.5f : h == LLFontGL::RIGHT ? x - max_w : x;
        font->renderUTF8(s, 0, left, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, (S32)max_w, nullptr, true);
    }

    // Cut into wood or metal: a dark lip one pixel below, the colour on top.
    void engraved(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c,
                  LLFontGL::HAlign h = LLFontGL::HCENTER, F32 max_w = 1.e6f)
    {
        text(font, s, x + 0.5f, y - 1.f, LLColor4(0.f, 0.f, 0.f, 0.55f), h, max_w);
        text(font, s, x, y, c, h, max_w);
    }

    // ---- geometry: everything is triangles, so line weight is the same on every GL driver ----
    // Angles are compass-style: a, radians clockwise from screen-up; point (sin a, cos a).

    void untextured()
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    void vtx(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

    void tri(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2, const LLColor4& c)
    {
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0, y0, c);
        vtx(x1, y1, c);
        vtx(x2, y2, c);
        gGL.end();
    }

    // An axis-aligned box, its colour running from top to bottom.
    void box(F32 l, F32 t, F32 r, F32 b, const LLColor4& top, const LLColor4& bottom)
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

    S32 segmentsFor(F32 r)
    {
        return llclamp((S32)(r * 0.8f), 24, 120);
    }

    void disc(F32 cx, F32 cy, F32 r, const LLColor4& centre, const LLColor4& rim)
    {
        const S32 n = segmentsFor(r);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n;
            const F32 a1 = F_TWO_PI * (i + 1) / n;
            vtx(cx, cy, centre);
            vtx(cx + r * sinf(a0), cy + r * cosf(a0), rim);
            vtx(cx + r * sinf(a1), cy + r * cosf(a1), rim);
        }
        gGL.end();
    }

    // Polished metal or bakelite: brighter towards the light (upper left), darker away from it.
    LLColor4 lit(const LLColor4& dark, const LLColor4& base, const LLColor4& light, F32 a)
    {
        constexpr F32 LIGHT_FROM = -F_PI / 4.f;   // upper left
        const F32 t = 0.5f + 0.5f * cosf(a - LIGHT_FROM);
        return t > 0.5f ? lerpColour(base, light, (t - 0.5f) * 1.8f) : lerpColour(dark, base, t * 2.f);
    }

    // An annulus r0..r1, each vertex lit() from (dark, base, light).
    void metalRing(F32 cx, F32 cy, F32 r0, F32 r1, const LLColor4& dark, const LLColor4& base, const LLColor4& light)
    {
        const S32 n = segmentsFor(r1);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n;
            const F32 a1 = F_TWO_PI * (i + 1) / n;
            const F32 s0 = sinf(a0), c0 = cosf(a0), s1 = sinf(a1), c1 = cosf(a1);
            const LLColor4 k0 = lit(dark, base, light, a0);
            const LLColor4 k1 = lit(dark, base, light, a1);
            vtx(cx + r0 * s0, cy + r0 * c0, k0);
            vtx(cx + r1 * s0, cy + r1 * c0, k0);
            vtx(cx + r1 * s1, cy + r1 * c1, k1);
            vtx(cx + r0 * s0, cy + r0 * c0, k0);
            vtx(cx + r1 * s1, cy + r1 * c1, k1);
            vtx(cx + r0 * s1, cy + r0 * c1, k1);
        }
        gGL.end();
    }

    void line(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0 + nx, y0 + ny, c);
        vtx(x1 + nx, y1 + ny, c);
        vtx(x1 - nx, y1 - ny, c);
        vtx(x0 + nx, y0 + ny, c);
        vtx(x1 - nx, y1 - ny, c);
        vtx(x0 - nx, y0 - ny, c);
        gGL.end();
    }

    // ---- rounded rectangles (the cabinet, grille, dial, keys, card, plates) ----
    struct Radii
    {
        F32 tl, tr, br, bl;
    };

    Radii allCorners(F32 r)
    {
        return Radii{ r, r, r, r };
    }

    // The outline of rect shrunk by inset, its corners' radii shrunk with it (concentric), as a
    // fixed number of points going clockwise from the top-left corner.
    std::vector<LLVector2> roundedOutline(const LLRect& rect, const Radii& rad, F32 inset)
    {
        constexpr S32 SEG = 12;
        const F32 l = rect.mLeft + inset, t = rect.mTop - inset, r = rect.mRight - inset, b = rect.mBottom + inset;
        const F32 max_r = llmax(0.f, llmin(r - l, t - b) * 0.5f);
        auto radius = [&](F32 v) { return llclamp(v - inset, 0.f, max_r); };
        std::vector<LLVector2> pts;
        pts.reserve(4 * (SEG + 1));
        auto corner = [&](F32 cx, F32 cy, F32 cr, F32 a0, F32 a1)
        {
            for (S32 i = 0; i <= SEG; ++i)
            {
                const F32 a = (a0 + (a1 - a0) * i / SEG) * DEG_TO_RAD;
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

    // Filled, its colour running from top to bottom (a fan from the centre: the shape is convex,
    // and a colour linear in y interpolates exactly across each triangle).
    void fillRounded(const LLRect& rect, const Radii& rad, const LLColor4& top, const LLColor4& bottom)
    {
        const std::vector<LLVector2> pts = roundedOutline(rect, rad, 0.f);
        const F32 h = llmax(1.f, (F32)rect.getHeight());
        auto colourAt = [&](F32 y) { return lerpColour(bottom, top, llclamp((y - rect.mBottom) / h, 0.f, 1.f)); };
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

    // Filled, bright at the centre and dimmer at the rim: a lamp behind glass.
    void fillRoundedGlow(const LLRect& rect, const Radii& rad, const LLColor4& centre, const LLColor4& rim)
    {
        const std::vector<LLVector2> pts = roundedOutline(rect, rad, 0.f);
        const F32 cx = (F32)rect.getCenterX(), cy = (F32)rect.getCenterY();
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const LLVector2& p0 = pts[i];
            const LLVector2& p1 = pts[(i + 1) % pts.size()];
            vtx(cx, cy, centre);
            vtx(p0.mV[VX], p0.mV[VY], rim);
            vtx(p1.mV[VX], p1.mV[VY], rim);
        }
        gGL.end();
    }

    // The band between two concentric outlines, inset0 (outer) .. inset1 (inner), coloured top to bottom.
    void strokeRounded(const LLRect& rect, const Radii& rad, F32 inset0, F32 inset1, const LLColor4& top, const LLColor4& bottom)
    {
        const std::vector<LLVector2> o = roundedOutline(rect, rad, inset0);
        const std::vector<LLVector2> in = roundedOutline(rect, rad, inset1);
        const F32 h = llmax(1.f, (F32)rect.getHeight());
        auto colourAt = [&](F32 y) { return lerpColour(bottom, top, llclamp((y - rect.mBottom) / h, 0.f, 1.f)); };
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

    // Where a horizontal line at y crosses the rounded rectangle.
    void spanAt(const LLRect& rect, const Radii& rad, F32 y, F32& x0, F32& x1)
    {
        auto inset = [](F32 r, F32 d) { return r - sqrtf(llmax(0.f, r * r - d * d)); };
        x0 = (F32)rect.mLeft;
        x1 = (F32)rect.mRight;
        if (rad.tl > 0.f && y > rect.mTop - rad.tl) x0 += inset(rad.tl, y - (rect.mTop - rad.tl));
        else if (rad.bl > 0.f && y < rect.mBottom + rad.bl) x0 += inset(rad.bl, rect.mBottom + rad.bl - y);
        if (rad.tr > 0.f && y > rect.mTop - rad.tr) x1 -= inset(rad.tr, y - (rect.mTop - rad.tr));
        else if (rad.br > 0.f && y < rect.mBottom + rad.br) x1 -= inset(rad.br, rect.mBottom + rad.br - y);
    }

    // Where a vertical line at x crosses it.
    void vspanAt(const LLRect& rect, const Radii& rad, F32 x, F32& y0, F32& y1)
    {
        auto inset = [](F32 r, F32 d) { return r - sqrtf(llmax(0.f, r * r - d * d)); };
        y0 = (F32)rect.mBottom;
        y1 = (F32)rect.mTop;
        if (rad.tl > 0.f && x < rect.mLeft + rad.tl) y1 -= inset(rad.tl, rect.mLeft + rad.tl - x);
        else if (rad.tr > 0.f && x > rect.mRight - rad.tr) y1 -= inset(rad.tr, x - (rect.mRight - rad.tr));
        if (rad.bl > 0.f && x < rect.mLeft + rad.bl) y0 += inset(rad.bl, rect.mLeft + rad.bl - x);
        else if (rad.br > 0.f && x > rect.mRight - rad.br) y0 += inset(rad.br, x - (rect.mRight - rad.br));
    }

    // Walnut grain: long wavering lines, a few lighter figure lines among the dark ones, kept
    // inside the rounded shape. One batch of triangles.
    void woodGrain(const LLRect& rect, const Radii& rad, F32 spacing)
    {
        constexpr F32 STEP = 16.f;   // px between the points of each grain line
        gGL.begin(LLRender::TRIANGLES);
        S32 j = 0;
        for (F32 base = rect.mBottom + spacing * 0.5f; base < rect.mTop; base += spacing, ++j)
        {
            const F32 h1 = hash01((F32)j + 0.13f), h2 = hash01((F32)j + 7.71f), h3 = hash01((F32)j + 3.37f);
            const F32 amp = spacing * (0.3f + 0.6f * h1);
            const F32 freq = 0.008f + 0.018f * h2;
            const F32 phase = h3 * F_TWO_PI;
            const bool figure = (j % 4) == 1;
            const LLColor4 c = figure ? withAlpha(GRAIN_LIGHT, 0.10f + 0.08f * h2) : withAlpha(GRAIN_DARK, 0.14f + 0.16f * h1);
            const F32 hw = figure ? 0.5f : 0.4f + 0.5f * h2;
            F32 ax0, ax1, bx0, bx1;
            spanAt(rect, rad, llmin((F32)rect.mTop, base + amp + 2.f), ax0, ax1);
            spanAt(rect, rad, llmax((F32)rect.mBottom, base - amp - 2.f), bx0, bx1);
            const F32 x0 = llmax(ax0, bx0) + 2.f, x1 = llmin(ax1, bx1) - 2.f;
            auto yAt = [&](F32 x) { return base + amp * sinf(x * freq + phase) + amp * 0.35f * sinf(x * freq * 2.7f + phase * 1.9f); };
            for (F32 x = x0; x < x1; x += STEP)
            {
                const F32 xe = llmin(x + STEP, x1);
                const F32 ya = yAt(x), yb = yAt(xe);
                vtx(x, ya - hw, c);
                vtx(xe, yb - hw, c);
                vtx(xe, yb + hw, c);
                vtx(x, ya - hw, c);
                vtx(xe, yb + hw, c);
                vtx(x, ya + hw, c);
            }
        }
        gGL.end();
    }

    // A small domed screw head with its slot.
    void screw(F32 x, F32 y, F32 r)
    {
        disc(x, y, r, BRASS_LIGHT, BRASS_DARK);
        line(x - r * 0.7f, y + r * 0.35f, x + r * 0.7f, y - r * 0.35f, llmax(1.f, r * 0.3f), withAlpha(BRASS_DARK, 0.9f));
    }

    bool mouseOver(LLView* v)
    {
        S32 x = 0, y = 0;
        LLUI::getInstance()->getMousePositionLocal(v, &x, &y);
        return v->pointInView(x, y);
    }

    std::vector<std::string> splitList(const std::string& s)
    {
        std::vector<std::string> out;
        std::string::size_type start = 0;
        while (start <= s.size())
        {
            const std::string::size_type comma = s.find(',', start);
            const std::string::size_type end = comma == std::string::npos ? s.size() : comma;
            std::string item = s.substr(start, end - start);
            LLStringUtil::trim(item);
            out.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }
}

// The view behind every control that paints the radio and turns the knobs. It covers the whole
// floater (so its coordinates are the floater's), sits just in front of the floater's drag handle
// and behind everything else, and is not mouse-opaque: off the knobs, clicks fall through to the
// drag handle, so the window still drags by its woodwork.
class WolfRadioBackdrop : public LLView
{
public:
    // Its own Params, never shared with another widget (it is created in code, not registered).
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };

    WolfRadioBackdrop(const Params& p, WolfFloaterRadio* radio) : LLView(p), mRadio(radio) {}

    void draw() override
    {
        mRadio->drawRadio();
    }

    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        return turnAt(x, y);
    }

    // The second click of a quick double click arrives here instead of handleMouseDown.
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override
    {
        return turnAt(x, y);
    }

    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
    {
        const WolfFloaterRadio::Knob knob = mRadio->knobAt(x, y);
        if (knob == WolfFloaterRadio::Knob::NONE) return false;
        mRadio->turnKnob(knob, -clicks);   // wheel up (negative clicks) turns clockwise
        return true;
    }

    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        if (mRadio->knobAt(x, y) == WolfFloaterRadio::Knob::NONE) return false;
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }

    bool handleToolTip(S32 x, S32 y, MASK mask) override
    {
        const WolfFloaterRadio::Knob knob = mRadio->knobAt(x, y);
        std::string tip;
        LLRect sticky;
        if (knob != WolfFloaterRadio::Knob::NONE)
        {
            tip = mRadio->knobToolTip(knob);
            sticky = mRadio->knobRect(knob);
        }
        else
        {
            const WolfFloaterRadio::Layout& l = mRadio->mLayout;
            const F32 dx = x - l.eye_x, dy = y - l.eye_y;
            if (dx * dx + dy * dy <= l.eye_r * l.eye_r * 2.1f || l.eye_plate.pointInRect(x, y))
            {
                tip = mRadio->getString(mRadio->onAir() ? "eye_tip_on" : "eye_tip_off");
                const S32 er = (S32)(l.eye_r * 1.45f);
                sticky = LLRect((S32)l.eye_x - er, (S32)l.eye_y + er, (S32)l.eye_x + er, l.eye_plate.mBottom);
            }
        }
        if (tip.empty()) return false;
        if (!gFocusMgr.getAppHasFocus()) return true;
        // as LLView::handleToolTip (llview.cpp:1014-1023), but sticky to the part under the mouse
        static LLCachedControl<bool> allow_ui_tooltips(*LLUI::getInstance()->mSettingGroups["config"], "BasicUITooltips", true);
        if (allow_ui_tooltips)
        {
            LLRect screen;
            localRectToScreen(sticky, &screen);
            LLToolTipMgr::instance().show(LLToolTip::Params().message(tip).sticky_rect(screen).delay_time(getTooltipTimeout()));
        }
        return true;
    }

private:
    bool turnAt(S32 x, S32 y)
    {
        const WolfFloaterRadio::Knob knob = mRadio->knobAt(x, y);
        if (knob == WolfFloaterRadio::Knob::NONE) return false;
        // the right half of a knob turns it clockwise, the left half anticlockwise
        const F32 cx = knob == WolfFloaterRadio::Knob::VOLUME ? mRadio->mLayout.vol_x : mRadio->mLayout.tune_x;
        mRadio->turnKnob(knob, (F32)x >= cx ? 1 : -1);
        return true;
    }

    WolfFloaterRadio* mRadio;
};

WolfFloaterRadio::WolfFloaterRadio(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterRadio::~WolfFloaterRadio()
{
}

bool WolfFloaterRadio::postBuild()
{
    for (S32 i = 0; i < GENRE_BUTTONS; ++i)
    {
        mGenre[i] = getChild<LLButton>(llformat("genre_%d", i));
        mGenre[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { showGenre(i); });
    }
    mStations = getChild<LLScrollListCtrl>("stations");
    mStations->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectStation(); });
    mStations->setDoubleClickCallback([this]() { if (!mListen->getToggleState()) { mListen->setToggleState(true); } onListen(); });
    mStatus = getChild<LLTextBox>("status");
    mListen = getChild<LLButton>("listen_btn");
    mListen->setCommitCallback([this](LLUICtrl*, const LLSD&) { onListen(); });
    mSetLand = getChild<LLButton>("set_land_btn");
    mSetLand->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetLand(); });
    mAddPreset = getChild<LLButton>("add_preset_btn");
    mAddPreset->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddPreset(); });

    mOnAirLabel = getString("on_air");
    mOffAirLabel = getString("off_air");
    mLogTitle = getString("log_title");
    mVolumeLabel = getString("volume_label");
    mTuningLabel = getString("tuning_label");
    mBands = splitList(getString("bands"));
    mScaleNumbers = splitList(getString("scale_numbers"));
    mScaleUnit = getString("scale_unit");
    mDialName = getString("idle_name");
    mDialDetail = getString("idle_detail");

    // The painted radio goes behind every control, then the drag handle behind it (llfloater.cpp
    // layoutDragHandle(): the top drag handle covers the whole floater, so empty woodwork drags).
    WolfRadioBackdrop::Params bp;
    bp.name("radio_backdrop");
    bp.rect(getLocalRect());
    bp.follows.flags(FOLLOWS_ALL);
    bp.mouse_opaque(false);
    mBackdrop = new WolfRadioBackdrop(bp, this);
    addChild(mBackdrop);
    sendChildToBack(mBackdrop);
    if (getDragHandle())
    {
        sendChildToBack(getDragHandle());
    }

    mChanged = WolfRadioStations::instance().onChanged([this]()
    {
        refreshGenres();
        if (!WolfRadioStations::instance().isLoaded()) setStatus(WolfRadioStations::instance().lastError(), true);
    });
    layoutRadio();
    return true;
}

void WolfFloaterRadio::onOpen(const LLSD& key)
{
    mPrimed = false;   // the needle starts where the station is, not gliding from where it was
    WolfRadioStations& st = WolfRadioStations::instance();
    st.load();
    refreshGenres();
    if (st.isLoading()) setStatus("Tuning in: loading the stations...");
    // [WOLF GRID GATE 2026-10-17] off a Wolf login load() asks nothing (wolfradiostations.cpp); say why it is empty
    else if (!st.isLoaded()) setStatus(st.lastError(), true);
}

void WolfFloaterRadio::onClose(bool app_quitting)
{
    // A preview is for while the radio is open: give the land's music back.
    if (!app_quitting && !WolfRadioStations::previewing().empty()) WolfRadioStations::preview(LLStringUtil::null);
}

void WolfFloaterRadio::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);
    layoutRadio();
}

const LLSD& WolfFloaterRadio::stationsOf(S32 index) const
{
    static const LLSD none = LLSD::emptyArray();
    WolfRadioStations& st = WolfRadioStations::instance();
    if (index == 0) return st.featured();
    const LLSD& genres = st.genres();
    if (index - 1 < (S32)genres.size()) return genres[index - 1]["stations"];
    return none;
}

void WolfFloaterRadio::refreshGenres()
{
    WolfRadioStations& st = WolfRadioStations::instance();
    const LLSD& genres = st.genres();
    for (S32 i = 0; i < GENRE_BUTTONS; ++i)
    {
        const bool have = st.isLoaded() && (i == 0 || i - 1 < (S32)genres.size());
        mGenre[i]->setVisible(have);
        if (have) mGenre[i]->setLabel(i == 0 ? std::string("Wolf") : genres[i - 1]["name"].asString());
        if (have && i > 0)
        {
            // the key may be too narrow for a long genre name; the tooltip always has all of it
            LLStringUtil::format_map_t args;
            args["[GENRE]"] = genres[i - 1]["name"].asString();
            mGenre[i]->setToolTip(getString("genre_tip", args));
        }
    }
    mGenre[0]->setToolTip(LLStringExplicit("The Wolf Territories stations, WTGR first"));
    layoutRadio();   // the keys are laid out by how many there are and how long their names are
    if (!st.isLoaded()) return;

    // The genre picked last time (setting WolfRadioGenre, a radio_stations.php genre id).
    const std::string last = gSavedSettings.getString("WolfRadioGenre");
    S32 index = 0;
    for (S32 i = 0; i < (S32)genres.size() && i + 1 < GENRE_BUTTONS; ++i)
    {
        if (genres[i]["id"].asString() == last) index = i + 1;
    }
    showGenre(index);
}

void WolfFloaterRadio::showGenre(S32 index)
{
    mGenreIndex = index;
    for (S32 i = 0; i < GENRE_BUTTONS; ++i) mGenre[i]->setToggleState(i == index);
    const LLSD& genres = WolfRadioStations::instance().genres();
    gSavedSettings.setString("WolfRadioGenre", index == 0 ? std::string("featured") : genres[index - 1]["id"].asString());

    mStations->deleteAllItems();
    const LLSD& list = stationsOf(index);
    for (S32 i = 0; i < (S32)list.size(); ++i)
    {
        const LLSD& s = list[i];
        LLSD row;
        row["value"] = i;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = s["name"].asString();
        row["columns"][1]["column"] = "country";
        row["columns"][1]["value"] = s["country"].asString();
        row["columns"][2]["column"] = "kbps";
        row["columns"][2]["value"] = s["bitrate"].asInteger() > 0 ? llformat("%d", s["bitrate"].asInteger()) : std::string();
        mStations->addElement(row);
    }
    if (list.size() > 0) mStations->selectFirstItem();
    onSelectStation();
}

const LLSD* WolfFloaterRadio::selectedStation() const
{
    LLScrollListItem* item = mStations->getFirstSelected();
    if (!item) return nullptr;
    const LLSD& list = stationsOf(mGenreIndex);
    const S32 i = item->getValue().asInteger();
    return (i >= 0 && i < (S32)list.size()) ? &list[i] : nullptr;
}

void WolfFloaterRadio::updateDial()
{
    const LLSD* s = selectedStation();
    if (!s)
    {
        mDialName = getString("idle_name");
        mDialDetail = getString("idle_detail");
        return;
    }
    const bool on_air = !WolfRadioStations::previewing().empty() && WolfRadioStations::previewing() == (*s)["url"].asString();
    mDialName = (on_air ? std::string("\xE2\x99\xAA ") : std::string()) + (*s)["name"].asString();
    std::string genre = mGenreIndex == 0 ? (*s)["genre"].asString() : mGenre[mGenreIndex]->getLabelUnselected();
    std::string detail = genre.empty() ? std::string("Wolf Territories") : genre;
    if (!(*s)["country"].asString().empty()) detail += "  \xC2\xB7  " + (*s)["country"].asString();
    if ((*s)["bitrate"].asInteger() > 0) detail += llformat("  \xC2\xB7  %d kbps", (*s)["bitrate"].asInteger());
    mDialDetail = detail;
}

void WolfFloaterRadio::onSelectStation()
{
    const LLSD* s = selectedStation();
    mListen->setEnabled(s != nullptr);
    mSetLand->setEnabled(s != nullptr && WolfRadioStations::editableParcel() != nullptr);
    mAddPreset->setEnabled(s != nullptr);
    // Listening follows the selection: pick another station while listening and it plays.
    if (s && mListen->getToggleState()) WolfRadioStations::preview((*s)["url"].asString());
    if (s && !WolfRadioStations::editableParcel()) setStatus("Only someone who may change this land's media can set its music.");
    updateDial();
}

void WolfFloaterRadio::onListen()
{
    const LLSD* s = selectedStation();
    if (mListen->getToggleState() && s)
    {
        WolfRadioStations::preview((*s)["url"].asString());
        setStatus("Listening (only you hear this). Press Stop to get the land's music back.");
    }
    else
    {
        mListen->setToggleState(false);
        WolfRadioStations::preview(LLStringUtil::null);
        setStatus("Stopped. The land's own music is back.");
    }
    updateDial();
}

void WolfFloaterRadio::onSetLand()
{
    const LLSD* s = selectedStation();
    if (!s) return;
    if (!WolfRadioStations::setLandMusic((*s)["url"].asString()))
    {
        setStatus("You can't change the music on this land.", true);
        return;
    }
    // The land now plays it for everyone; stop the private preview so it is not heard twice.
    if (mListen->getToggleState())
    {
        mListen->setToggleState(false);
        WolfRadioStations::preview(LLStringUtil::null);
    }
    setStatus("Now playing on this land: " + (*s)["name"].asString());
    updateDial();
}

void WolfFloaterRadio::onAddPreset()
{
    const LLSD* s = selectedStation();
    if (!s) return;
    bool already = false;
    if (WolfRadioStations::addPreset((*s)["url"].asString(), &already))
    {
        LLFloaterLand::refreshAll();
        setStatus("Added to My presets on the Sound tab: " + (*s)["name"].asString());
    }
    else if (already)
    {
        setStatus("That station is already in My presets.");
    }
    else
    {
        setStatus("My presets are full. Clear one on the Sound tab first.", true);
    }
}

void WolfFloaterRadio::setStatus(const std::string& text, bool error)
{
    mStatus->setText(text);
    mStatus->setColor(error ? STATUS_ERROR : STATUS_OK);
}

// ============================================================================================
// The drawn radio
// ============================================================================================

bool WolfFloaterRadio::onAir() const
{
    // the private preview is playing (wolfradiostations.cpp preview(): empty when the land's own music is back)
    return !WolfRadioStations::previewing().empty();
}

S32 WolfFloaterRadio::visibleGenres() const
{
    S32 n = 0;
    for (S32 i = 0; i < GENRE_BUTTONS; ++i)
    {
        if (mGenre[i] && mGenre[i]->getVisible()) n = i + 1;   // refreshGenres() shows keys 0..n-1
    }
    return n;
}

void WolfFloaterRadio::layoutRadio()
{
    if (!mStations || !mStatus || !mListen || !mSetLand || !mAddPreset || !mGenre[GENRE_BUTTONS - 1])
    {
        return;   // reshape() during construction, before postBuild()
    }
    if (isMinimized())
    {
        // LLFloater::setMinimized() sets mMinimized and then reshapes to the bare title bar
        // (llfloater.cpp:1399, 1458): nothing to lay out in that, and the controls keep their
        // places for when it is restored
        return;
    }
    Layout& L = mLayout;
    const S32 W = getRect().getWidth();
    const S32 H = getRect().getHeight();
    if (mBackdrop)
    {
        mBackdrop->setShape(getLocalRect());
    }
    L.client = LLRect(LLPANEL_BORDER_WIDTH, H - getHeaderHeight(), W - LLPANEL_BORDER_WIDTH, LLPANEL_BORDER_WIDTH);
    const S32 ch = L.client.getHeight();

    constexpr S32 PLINTH = 10;
    L.cabinet = LLRect(L.client.mLeft + 6, L.client.mTop - 6, L.client.mRight - 6, L.client.mBottom + PLINTH + 4);
    constexpr S32 PAD = 14;
    const S32 band_h = llclamp(ll_round(ch * 0.32f), 140, 250);   // the grille and dial band (below)
    // The shoulders: as round as the cabinet allows, but no rounder than a fifth of the band plus
    // the wood border, so the grille and dial corners can follow them concentrically (inner_r)
    // without the curve cutting into the dial's scales; and no more than a sixth of the height,
    // the most the varnish highlight (a third of the height) can round to (roundedOutline()).
    L.arch_r = llclamp(llmin(llmin(L.cabinet.getWidth() * 0.16f, L.cabinet.getHeight() / 6.f), band_h * 0.2f + PAD), 24.f, 110.f);
    const LLRect inner(L.cabinet.mLeft + PAD, L.cabinet.mTop - PAD, L.cabinet.mRight - PAD, L.cabinet.mBottom + PAD);
    const S32 iw = inner.getWidth();

    // top band: the speaker grille (left) and the dial (right); their outer top corners are
    // concentric with the shoulders, so they stay inside the cabinet (at most 0.2 x 250 = 50 px)
    L.inner_r = llmax(6.f, L.arch_r - PAD);
    const S32 grille_w = llclamp(ll_round(iw * 0.26f), 90, 200);
    L.grille = LLRect(inner.mLeft, inner.mTop, inner.mLeft + grille_w, inner.mTop - band_h);
    L.dial = LLRect(L.grille.mRight + 18, inner.mTop - 5, inner.mRight - 5, inner.mTop - band_h + 5);   // 5 px left for the bezel
    L.eye_r = llclamp(grille_w * 0.15f, 11.f, 24.f);
    L.eye_x = (F32)L.grille.getCenterX();
    L.eye_y = L.grille.mTop - llmax(L.inner_r * 0.45f, 10.f) - L.eye_r * 1.45f;
    {
        const S32 pw = llmin(grille_w - 16, 100);
        const S32 ptop = ll_round(L.eye_y - L.eye_r * 1.45f - 6.f);
        L.eye_plate = LLRect(ll_round(L.eye_x) - pw / 2, ptop, ll_round(L.eye_x) + pw / 2, ptop - 20);
    }
    {
        // inset on the right past the glass's rounded top-right corner (radius inner_r): at 8 px
        // below the top that corner takes r - sqrt(r^2 - (r - 8)^2) px off the width - at most 0.5 r + 2 for r up to 60 px (inner_r is at most 50)
        const S32 sp = 8;
        const S32 sp_right = llmax(sp, ll_round(L.inner_r * 0.5f) + 2);
        L.scale = LLRect(L.dial.mLeft + sp, L.dial.mTop - sp, L.dial.mRight - sp_right, L.dial.mTop - ll_round(L.dial.getHeight() * 0.56f));
    }

    // the genre keys: two rows under the band
    const S32 key_h = llclamp(ll_round(ch * 0.05f), 24, 32);
    const LLRect keys(inner.mLeft, L.grille.mBottom - 14, inner.mRight, L.grille.mBottom - 14 - (2 * key_h + 6));
    layoutKeys(keys);

    // the bottom strip: knobs at the ends, the buttons and the status plate between them
    L.knob_r = (F32)llclamp(ll_round(ch * 0.045f), 20, 30);
    const S32 label_h = 16;
    const S32 strip_h = ll_round(L.knob_r * 2.5f) + label_h + 2;
    const LLRect strip(inner.mLeft, inner.mBottom + strip_h, inner.mRight, inner.mBottom);
    L.vol_x = inner.mLeft + L.knob_r * 1.3f;
    L.tune_x = inner.mRight - L.knob_r * 1.3f;
    L.vol_y = L.tune_y = strip.mTop - L.knob_r * 1.25f;
    L.knob_label_y = strip.mBottom + label_h * 0.5f;

    const S32 bx0 = ll_round(L.vol_x + L.knob_r * 1.3f + 12.f);
    const S32 bx1 = ll_round(L.tune_x - L.knob_r * 1.3f - 12.f);
    {
        constexpr S32 BUTTON_H = 26, GAP = 8;
        const S32 base_w[3] = { 96, 120, 150 };
        const S32 total = base_w[0] + base_w[1] + base_w[2] + 2 * GAP;
        const F32 k = llmin(1.f, (F32)(bx1 - bx0) / total);
        S32 w[3];
        S32 sum = 2 * GAP;
        for (S32 i = 0; i < 3; ++i)
        {
            w[i] = ll_round(base_w[i] * k);
            sum += w[i];
        }
        S32 x = (bx0 + bx1) / 2 - sum / 2;
        LLButton* buttons[3] = { mListen, mSetLand, mAddPreset };
        std::vector<std::string> labels;
        for (S32 i = 0; i < 3; ++i)
        {
            buttons[i]->setShape(LLRect(x, strip.mTop, x + w[i], strip.mTop - BUTTON_H));
            x += w[i] + GAP;
            labels.push_back(buttons[i]->getLabelUnselected());
            labels.push_back(buttons[i]->getLabelSelected());
        }
        const LLFontGL* bf = fitFont((F32)BUTTON_H - 8.f, (F32)llmin(w[0], llmin(w[1], w[2])) - 12.f, labels, MIN_KEY_FONT);
        for (LLButton* b : buttons) b->setFont(bf);
        L.status_plate = LLRect(bx0, strip.mTop - BUTTON_H - 6, bx1, strip.mBottom);
        mStatus->setShape(LLRect(L.status_plate.mLeft + 10, L.status_plate.mTop - 4, L.status_plate.mRight - 10, L.status_plate.mBottom + 4));
    }

    // the station log card fills the rest
    L.card = LLRect(inner.mLeft, keys.mBottom - 14, inner.mRight, strip.mTop + 10);
    L.card_title = LLRect(L.card.mLeft + 10, L.card.mTop - 6, L.card.mRight - 10, L.card.mTop - 28);
    mStations->setShape(LLRect(L.card.mLeft + 10, L.card_title.mBottom - 4, L.card.mRight - 10, L.card.mBottom + 8));
}

void WolfFloaterRadio::layoutKeys(const LLRect& area)
{
    const S32 n = visibleGenres();
    if (n <= 0) return;
    constexpr S32 GAP = 6;
    const S32 cols = (n + 1) / 2;
    const S32 row_h = (area.getHeight() - GAP) / 2;
    const S32 key_w = (area.getWidth() - (cols - 1) * GAP) / cols;
    std::vector<std::string> labels;
    for (S32 i = 0; i < n; ++i)
    {
        const S32 row = i < cols ? 0 : 1;
        const S32 col = row == 0 ? i : i - cols;
        const S32 in_row = row == 0 ? cols : n - cols;
        // the second row is centred under the first
        const S32 offset = (cols - in_row) * (key_w + GAP) / 2;
        const S32 x = area.mLeft + offset + col * (key_w + GAP);
        const S32 top = area.mTop - row * (row_h + GAP);
        mGenre[i]->setShape(LLRect(x, top, x + key_w, top - row_h));
        labels.push_back(mGenre[i]->getLabelUnselected());
    }
    // one size for every key, the largest at which every name fits (long ones get an ellipsis
    // below the smallest readable size; the tooltip has the whole name)
    const LLFontGL* f = fitFont((F32)row_h - 8.f, (F32)key_w - 12.f, labels, MIN_KEY_FONT);
    for (S32 i = 0; i < n; ++i) mGenre[i]->setFont(f);
}

F32 WolfFloaterRadio::needleTarget() const
{
    const S32 n = visibleGenres();
    if (n <= 0) return 0.03f;   // not loaded yet: resting at the low end of the dial
    const S32 count = mStations->getItemCount();
    const S32 sel = mStations->getFirstSelectedIndex();   // the row as shown, so a re-sorted list still reads left to right
    const F32 within = (count > 0 && sel >= 0) ? (sel + 0.5f) / count : 0.5f;
    return llclamp((mGenreIndex + within) / n, 0.f, 1.f);
}

void WolfFloaterRadio::updateAnimation()
{
    const F32 dt = mFrameTimer.getElapsedTimeAndResetF32();
    const F32 target = needleTarget();
    const F32 eye = onAir() ? 1.f : 0.f;
    if (!mPrimed || dt > MAX_EASE_GAP)
    {
        mPrimed = true;
        mNeedle = target;
        mEye = eye;
        return;
    }
    mNeedle += (target - mNeedle) * (1.f - expf(-dt * NEEDLE_RATE));
    mEye += (eye - mEye) * (1.f - expf(-dt * EYE_RATE));
}

void WolfFloaterRadio::drawRadio()
{
    if (isMinimized() || mLayout.client.getWidth() < 80 || mLayout.client.getHeight() < 80)
    {
        return;
    }
    updateAnimation();
    untextured();
    drawCabinet();
    drawGrille();
    drawMagicEye();
    drawDial();
    drawCard();
    drawStatusPlate();
    drawButtons();

    static LLCachedControl<F32> music_level(gSavedSettings, "AudioLevelMusic");
    const F32 vol = llclamp((F32)music_level, 0.f, 1.f);
    drawKnob(mLayout.vol_x, mLayout.vol_y, -KNOB_SWEEP + 2.f * KNOB_SWEEP * vol,
             mVolumeLabel + llformat(" %d%%", ll_round(vol * 100.f)));
    // the tuning knob turns with the needle, a little under a full turn end to end
    drawKnob(mLayout.tune_x, mLayout.tune_y, (-150.f + 300.f * mNeedle) * DEG_TO_RAD, mTuningLabel);
}

void WolfFloaterRadio::drawCabinet() const
{
    const Layout& L = mLayout;
    untextured();
    // the wall behind the radio, seen past its rounded shoulders
    box((F32)L.client.mLeft, (F32)L.client.mTop, (F32)L.client.mRight, (F32)L.client.mBottom, BACKDROP, lighten(BACKDROP, 0.03f));

    const Radii body{ L.arch_r, L.arch_r, 8.f, 8.f };
    // plinth: a darker base the cabinet stands on, with two feet
    {
        const F32 l = L.cabinet.mLeft + 6.f, r = L.cabinet.mRight - 6.f;
        const F32 t = L.cabinet.mBottom + 6.f, b = L.client.mBottom + 4.f;
        fillRounded(LLRect((S32)l, (S32)t, (S32)r, (S32)b), allCorners(3.f), lighten(WALNUT_DARK, 0.06f), WALNUT_DARK);
        const F32 fw = llmax(18.f, (r - l) * 0.08f);
        box(l + 10.f, b + 1.f, l + 10.f + fw, (F32)L.client.mBottom + 1.f, WALNUT_DARK, BACKDROP);
        box(r - 10.f - fw, b + 1.f, r - 10.f, (F32)L.client.mBottom + 1.f, WALNUT_DARK, BACKDROP);
    }
    // drop shadow, then the polished walnut body
    {
        LLRect shadow = L.cabinet;
        shadow.translate(3, -4);
        fillRounded(shadow, body, SHADOW, SHADOW);
    }
    fillRounded(L.cabinet, body, WALNUT_TOP, WALNUT_BOTTOM);
    woodGrain(L.cabinet, body, llmax(5.f, L.cabinet.getHeight() / 90.f));
    // varnish: a soft highlight across the top, a bright bead round the edge, a dark outer line
    {
        const LLRect gloss(L.cabinet.mLeft, L.cabinet.mTop, L.cabinet.mRight, L.cabinet.mTop - L.cabinet.getHeight() / 3);
        fillRounded(gloss, Radii{ L.arch_r, L.arch_r, 0.f, 0.f }, LLColor4(1.f, 0.95f, 0.85f, 0.08f), LLColor4(1.f, 0.95f, 0.85f, 0.f));
    }
    strokeRounded(L.cabinet, body, 3.f, 5.f, WALNUT_EDGE, withAlpha(WALNUT_EDGE, 0.35f));
    strokeRounded(L.cabinet, body, 0.f, 1.5f, WALNUT_DARK, WALNUT_DARK);
}

void WolfFloaterRadio::drawGrille() const
{
    const Layout& L = mLayout;
    const LLRect& g = L.grille;
    const Radii rad{ L.inner_r, 6.f, 6.f, 6.f };
    untextured();
    // the opening cut in the cabinet, then the cloth behind it
    {
        LLRect recess(g.mLeft - 3, g.mTop + 3, g.mRight + 3, g.mBottom - 3);
        fillRounded(recess, Radii{ L.inner_r + 3.f, 9.f, 9.f, 9.f }, WALNUT_DARK, lighten(WALNUT_DARK, 0.05f));
    }
    fillRounded(g, rad, CLOTH_TOP, CLOTH_BOTTOM);
    // the weave: fine threads both ways, alternately darker and lighter
    gGL.begin(LLRender::TRIANGLES);
    {
        S32 k = 0;
        for (F32 y = g.mBottom + 1.5f; y < g.mTop; y += 3.f, ++k)
        {
            F32 x0, x1;
            spanAt(g, rad, y, x0, x1);
            const LLColor4 c = (k & 1) ? LLColor4(0.f, 0.f, 0.f, 0.16f) : LLColor4(1.f, 0.92f, 0.75f, 0.07f);
            vtx(x0, y - 0.5f, c); vtx(x1, y - 0.5f, c); vtx(x1, y + 0.5f, c);
            vtx(x0, y - 0.5f, c); vtx(x1, y + 0.5f, c); vtx(x0, y + 0.5f, c);
        }
        k = 0;
        for (F32 x = g.mLeft + 1.5f; x < g.mRight; x += 3.f, ++k)
        {
            F32 y0, y1;
            vspanAt(g, rad, x, y0, y1);
            const LLColor4 c = (k & 1) ? LLColor4(0.f, 0.f, 0.f, 0.12f) : LLColor4(1.f, 0.92f, 0.75f, 0.05f);
            vtx(x - 0.5f, y0, c); vtx(x + 0.5f, y0, c); vtx(x + 0.5f, y1, c);
            vtx(x - 0.5f, y0, c); vtx(x + 0.5f, y1, c); vtx(x - 0.5f, y1, c);
        }
    }
    gGL.end();

    // the fretwork: upright wooden bars over the cloth, each lit on its left edge, and a cross rail
    const S32 bars = llclamp(g.getWidth() / 24, 3, 7);
    const F32 bw = llmax(3.f, g.getWidth() * 0.045f);
    const F32 rail_y = g.mBottom + g.getHeight() * 0.22f;
    for (S32 i = 1; i <= bars; ++i)
    {
        const F32 x = g.mLeft + g.getWidth() * (F32)i / (bars + 1);
        F32 y0, y1a, y1b, dummy;
        vspanAt(g, rad, x - bw * 0.5f, y0, y1a);
        vspanAt(g, rad, x + bw * 0.5f, dummy, y1b);
        const F32 y1 = llmin(y1a, y1b);
        box(x - bw * 0.5f + 1.f, y1, x + bw * 0.5f + 1.f, y0, SHADOW, SHADOW);   // its shadow on the cloth
        box(x - bw * 0.5f, y1, x + bw * 0.5f, y0, FRET_LIGHT, FRET_DARK);
        box(x - bw * 0.5f, y1, x - bw * 0.5f + 1.f, y0, withAlpha(GRAIN_LIGHT, 0.7f), withAlpha(GRAIN_LIGHT, 0.4f));
    }
    box((F32)g.mLeft, rail_y + bw * 0.5f, (F32)g.mRight, rail_y - bw * 0.5f, FRET_LIGHT, FRET_DARK);
    box((F32)g.mLeft, rail_y + bw * 0.5f, (F32)g.mRight, rail_y + bw * 0.5f - 1.f, withAlpha(GRAIN_LIGHT, 0.7f), withAlpha(GRAIN_LIGHT, 0.7f));
    // the frame round the opening
    strokeRounded(g, rad, 0.f, 2.f, WALNUT_DARK, WALNUT_DARK);
}

void WolfFloaterRadio::drawMagicEye() const
{
    const Layout& L = mLayout;
    const F32 cx = L.eye_x, cy = L.eye_y, r = L.eye_r;
    untextured();
    // the wooden boss it is mounted in, the chrome bezel, the dark glass
    disc(cx + 1.5f, cy - 2.f, r * 1.5f, SHADOW, LLColor4(0.f, 0.f, 0.f, 0.f));
    disc(cx, cy, r * 1.45f, FRET_LIGHT, FRET_DARK);
    metalRing(cx, cy, r * 1.08f, r * 1.30f, CHROME_DARK, CHROME, CHROME_LIGHT);
    disc(cx, cy, r * 1.08f, EYE_GLASS, EYE_GLASS);

    // The phosphor fan of a 6E5-style tuning eye: a glowing disc with a dark shadow wedge at the
    // top. The wedge closes as the signal grows - here, as the radio comes on air - and breathes a
    // little while it plays, as a real eye does with the music.
    const F64 t = LLTimer::getElapsedSeconds();
    const F32 breathe = mEye > 0.5f ? 2.5f * sinf((F32)fmod(t * 2.3, F_TWO_PI)) * mEye : 0.f;
    const F32 half_wedge = (70.f + (12.f - 70.f) * mEye + breathe) * DEG_TO_RAD;
    const LLColor4 glow = lerpColour(EYE_OFF, EYE_ON, mEye);
    const LLColor4 glow_inner = lerpColour(EYE_OFF, lerpColour(EYE_ON, EYE_GLASS, 0.45f), mEye);
    const S32 n = 48;
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < n; ++i)
    {
        const F32 a0 = half_wedge + (F_TWO_PI - 2.f * half_wedge) * i / n;
        const F32 a1 = half_wedge + (F_TWO_PI - 2.f * half_wedge) * (i + 1) / n;
        vtx(cx, cy, glow_inner);
        vtx(cx + r * sinf(a0), cy + r * cosf(a0), glow);
        vtx(cx + r * sinf(a1), cy + r * cosf(a1), glow);
    }
    gGL.end();
    // the edges of the wedge burn brightest
    if (mEye > 0.05f)
    {
        const LLColor4 edge = withAlpha(lighten(EYE_ON, 0.3f), mEye);
        line(cx, cy, cx + r * sinf(half_wedge), cy + r * cosf(half_wedge), llmax(1.f, r * 0.08f), edge);
        line(cx, cy, cx + r * sinf(-half_wedge), cy + r * cosf(-half_wedge), llmax(1.f, r * 0.08f), edge);
    }
    // the cathode cap in the middle, and a glint on the glass
    disc(cx, cy, r * 0.34f, LLColor4(0.18f, 0.18f, 0.17f, 1.f), LLColor4(0.04f, 0.04f, 0.04f, 1.f));
    disc(cx - r * 0.45f, cy + r * 0.45f, r * 0.22f, LLColor4(1.f, 1.f, 1.f, 0.25f), LLColor4(1.f, 1.f, 1.f, 0.f));

    // the plate under it says it in words, so the state never rests on colour alone
    const LLRect& p = L.eye_plate;
    fillRounded(p, allCorners(3.f), PLATE, PLATE);
    strokeRounded(p, allCorners(3.f), 0.f, 1.f, BRASS, BRASS_DARK);
    const bool on = onAir();
    const std::string& label = on ? mOnAirLabel : mOffAirLabel;
    const LLFontGL* f = fitFont((F32)p.getHeight() - 4.f, (F32)p.getWidth() - 8.f, { mOnAirLabel, mOffAirLabel });
    text(f, label, (F32)p.getCenterX(), (F32)p.getCenterY(), on ? TEXT_CREAM : TEXT_DIM, LLFontGL::HCENTER, (F32)p.getWidth() - 6.f);
}

void WolfFloaterRadio::drawDial() const
{
    const Layout& L = mLayout;
    const LLRect& d = L.dial;
    const Radii rad{ 6.f, L.inner_r, 6.f, 6.f };
    untextured();

    // chrome bezel, then the glass lit from behind; the lamp warms while on air
    {
        const LLRect bezel(d.mLeft - 5, d.mTop + 5, d.mRight + 5, d.mBottom - 5);
        const Radii brad{ rad.tl + 5.f, rad.tr + 5.f, rad.br + 5.f, rad.bl + 5.f };
        LLRect shadow = bezel;
        shadow.translate(2, -3);
        fillRounded(shadow, brad, SHADOW, SHADOW);
        strokeRounded(bezel, brad, 0.f, 5.f, CHROME_LIGHT, CHROME_DARK);
        strokeRounded(bezel, brad, 4.f, 5.f, CHROME_DARK, CHROME_DARK);
    }
    fillRoundedGlow(d, rad, lerpColour(GLASS_COLD_CENTRE, GLASS_WARM_CENTRE, mEye), lerpColour(GLASS_COLD_RIM, GLASS_WARM_RIM, mEye));

    // ---- the scales ----
    const LLRect& s = L.scale;
    const S32 rows = 3;
    const F32 nums_h = s.getHeight() * 0.22f;
    const F32 row_h = (s.getHeight() - nums_h) / rows;
    const LLFontGL* row_font = heightFont(llmax(9.f, row_h * 0.62f));
    F32 label_w = 0.f;
    for (const std::string& b : mBands) label_w = llmax(label_w, row_font->getWidthF32(b));
    label_w += 8.f;
    const F32 track_l = s.mLeft + label_w, track_r = (F32)s.mRight - 4.f;
    const F32 track_w = llmax(1.f, track_r - track_l);
    const S32 n = visibleGenres();
    const F32 rule_w = llmax(1.f, s.getHeight() * 0.01f);

    // frequency numerals along the top, like the kHz figures on a medium-wave dial
    if (!mScaleNumbers.empty())
    {
        const LLFontGL* nf = heightFont(llmax(8.f, nums_h * 0.9f));
        const F32 slot = track_w / mScaleNumbers.size();
        const F32 ny = s.mTop - nums_h * 0.5f;
        F32 widest = 0.f;
        for (const std::string& num : mScaleNumbers) widest = llmax(widest, nf->getWidthF32(num));
        if (widest < slot - 4.f)
        {
            text(nf, mScaleUnit, (F32)s.mLeft, ny, INK, LLFontGL::LEFT, label_w - 2.f);
            for (size_t i = 0; i < mScaleNumbers.size(); ++i)
            {
                text(nf, mScaleNumbers[i], track_l + slot * (i + 0.5f), ny, INK);
            }
        }
    }
    untextured();
    // three band rows, each a printed rule with ticks
    for (S32 r = 0; r < rows; ++r)
    {
        const F32 base = s.mTop - nums_h - row_h * (r + 1) + row_h * 0.18f;
        line(track_l, base, track_r, base, rule_w, withAlpha(INK, 0.85f));
        const S32 minor = llmax(1, n) * 4;
        for (S32 k = 0; k <= minor; ++k)
        {
            const F32 x = track_l + track_w * k / minor;
            const F32 len = (k % 4) == 2 ? row_h * 0.22f : row_h * 0.10f;   // the genre's own slot centre is the long tick
            line(x, base, x, base + len, rule_w, withAlpha(INK, 0.7f));
        }
    }
    // the band names in the left column
    for (S32 r = 0; r < rows && r < (S32)mBands.size(); ++r)
    {
        const F32 base = s.mTop - nums_h - row_h * (r + 1) + row_h * 0.18f;
        text(row_font, mBands[r], (F32)s.mLeft, base + row_h * 0.42f, INK, LLFontGL::LEFT, label_w - 4.f);
    }
    // the genres printed on the scales like station names, every third on the same band
    if (n > 0)
    {
        const F32 slot = track_w / n;
        const LLFontGL* gf = heightFont(llmax(8.f, row_h * 0.58f));
        for (S32 i = 0; i < n; ++i)
        {
            const S32 r = i % rows;
            const F32 base = s.mTop - nums_h - row_h * (r + 1) + row_h * 0.18f;
            const F32 x = track_l + slot * (i + 0.5f);
            const F32 max_w = slot * rows - 4.f;   // the next name on this band is three slots on
            const bool current = i == mGenreIndex;
            const std::string& name = mGenre[i]->getLabelUnselected();
            const F32 y = base + row_h * 0.55f;
            text(gf, name, x, y, current ? DIAL_RED : INK, LLFontGL::HCENTER, max_w);
            if (current)
            {
                // underlined as well as red: the current genre does not rest on colour alone
                const F32 uw = llmin(gf->getWidthF32(name), max_w) * 0.5f;
                untextured();
                line(x - uw, y - gf->getLineHeight() * 0.42f, x + uw, y - gf->getLineHeight() * 0.42f, llmax(1.f, rule_w * 1.5f), DIAL_RED);
            }
        }
    }
    untextured();
    // a rule under the scales, between them and the station
    line((F32)s.mLeft, (F32)s.mBottom, (F32)s.mRight, (F32)s.mBottom, rule_w, withAlpha(INK, 0.5f));

    // ---- the needle ----
    {
        const F32 x = track_l + track_w * mNeedle;
        const F32 top = (F32)s.mTop - 1.f, bottom = (F32)s.mBottom + 2.f;
        const F32 w = llmax(2.f, s.getHeight() * 0.022f);
        box(x - w * 0.5f + 2.f, top - 2.f, x + w * 0.5f + 2.f, bottom - 2.f, LLColor4(0.f, 0.f, 0.f, 0.22f), LLColor4(0.f, 0.f, 0.f, 0.22f));
        box(x - w * 0.5f, top, x + w * 0.5f, bottom, NEEDLE_RED, NEEDLE_RED);
        box(x - w * 0.15f, top, x + w * 0.15f, bottom, NEEDLE_LIGHT, NEEDLE_LIGHT);
        tri(x - w * 2.f, top, x + w * 2.f, top, x, top - w * 3.f, NEEDLE_RED);
    }

    // ---- the station, under the scales ----
    {
        const F32 lower_h = (F32)(s.mBottom - d.mBottom) - 6.f;
        const F32 text_w = (F32)d.getWidth() - 20.f;
        const LLFontGL* name_font = heightFont(llmax(10.f, lower_h * 0.52f));
        const LLFontGL* detail_font = heightFont(llmax(8.f, lower_h * 0.28f));
        const F32 name_y = s.mBottom - 4.f - lower_h * 0.36f;
        const F32 detail_y = d.mBottom + 6.f + lower_h * 0.16f;
        text(name_font, mDialName, (F32)d.getCenterX(), name_y, INK, LLFontGL::HCENTER, text_w);
        text(detail_font, mDialDetail, (F32)d.getCenterX(), detail_y, INK, LLFontGL::HCENTER, text_w);
    }

    // a glint across the glass, upper left
    untextured();
    {
        const F32 gx = d.mLeft + d.getWidth() * 0.08f, gt = d.mTop - 3.f, gb = d.mTop - d.getHeight() * 0.45f;
        const F32 gw = d.getWidth() * 0.05f, slant = (gt - gb) * 0.35f;
        gGL.begin(LLRender::TRIANGLES);
        const LLColor4 c0(1.f, 1.f, 1.f, 0.14f), c1(1.f, 1.f, 1.f, 0.f);
        vtx(gx, gb, c1); vtx(gx + slant + gw, gt, c0); vtx(gx + slant, gt, c0);
        vtx(gx, gb, c1); vtx(gx + gw, gb, c1); vtx(gx + slant + gw, gt, c0);
        gGL.end();
    }
}

void WolfFloaterRadio::drawButtons() const
{
    untextured();
    auto key = [](LLButton* b, bool ivory)   // not const: LLView::hasMouseCapture() is not
    {
        if (!b->getVisible()) return;
        const LLRect r = b->getRect();
        const bool enabled = b->isInEnabledChain();
        const bool selected = b->getToggleState();
        const bool pressed = b->hasMouseCapture() && mouseOver(b);
        const bool hover = enabled && !pressed && mouseOver(b);
        const F32 depth = llmax(2.f, r.getHeight() * 0.14f);
        // a pushed-in key (the genre playing, or Listen while on air) sits lower, with no front face showing
        const F32 sink = (selected || pressed) ? depth * 0.7f : 0.f;
        const F32 alpha = enabled ? 1.f : 0.55f;

        LLRect shadow = r;
        shadow.translate(1, -2);
        fillRounded(shadow, allCorners(4.f), withAlpha(SHADOW, alpha), withAlpha(SHADOW, alpha));
        // the key's front face below its top
        fillRounded(r, allCorners(4.f), withAlpha(ivory ? IVORY_SIDE : BAKELITE_SIDE, alpha),
                    withAlpha(ivory ? lighten(IVORY_SIDE, -0.15f) : BAKELITE_SIDE, alpha));
        const LLRect face(r.mLeft, r.mTop - (S32)sink, r.mRight, r.mBottom + (S32)depth);
        LLColor4 top = ivory ? (selected ? IVORY_PRESSED_TOP : IVORY_TOP) : (selected ? BAKELITE_LIT_TOP : BAKELITE_TOP);
        LLColor4 bottom = ivory ? (selected ? IVORY_PRESSED_BOTTOM : IVORY_BOTTOM) : (selected ? BAKELITE_LIT_BOTTOM : BAKELITE_BOTTOM);
        if (hover)
        {
            top = lighten(top, ivory ? 0.03f : 0.06f);
            bottom = lighten(bottom, ivory ? 0.03f : 0.05f);
        }
        fillRounded(face, allCorners(4.f), withAlpha(top, alpha), withAlpha(bottom, alpha));
        // a bright lip along the top edge; a lamp line on a key that is pushed in
        const LLColor4 lip = selected ? LAMP_AMBER : (ivory ? LLColor4(1.f, 1.f, 1.f, 0.7f) : withAlpha(BAKELITE_LIGHT, 0.8f));
        box(face.mLeft + 4.f, (F32)face.mTop - 1.f, face.mRight - 4.f, (F32)face.mTop - (selected ? 3.f : 2.f), withAlpha(lip, alpha), withAlpha(lip, alpha * 0.6f));
        strokeRounded(face, allCorners(4.f), 0.f, 1.f, withAlpha(ivory ? IVORY_SIDE : BAKELITE_SIDE, alpha), withAlpha(ivory ? IVORY_SIDE : BAKELITE_SIDE, alpha));
    };
    for (LLButton* b : mGenre)
    {
        key(b, true);
    }
    key(mListen, false);
    key(mSetLand, false);
    key(mAddPreset, false);
}

void WolfFloaterRadio::drawCard() const
{
    const Layout& L = mLayout;
    const LLRect& c = L.card;
    untextured();
    LLRect shadow = c;
    shadow.translate(2, -3);
    fillRounded(shadow, allCorners(4.f), SHADOW, SHADOW);
    fillRounded(c, allCorners(4.f), PAPER_TOP, PAPER_BOTTOM);
    // a printed double rule round the card
    strokeRounded(c, allCorners(4.f), 3.f, 4.f, withAlpha(INK, 0.55f), withAlpha(INK, 0.55f));
    strokeRounded(c, allCorners(4.f), 5.5f, 6.f, withAlpha(INK, 0.35f), withAlpha(INK, 0.35f));

    // the heading: "STATION LOG · ROCK"
    const LLRect& t = L.card_title;
    std::string title = mLogTitle;
    if (mGenreIndex >= 0 && mGenreIndex < GENRE_BUTTONS && mGenre[mGenreIndex]->getVisible())
    {
        title += "  \xC2\xB7  " + mGenre[mGenreIndex]->getLabelUnselected();
    }
    const LLFontGL* f = heightFont((F32)t.getHeight() - 2.f);
    text(f, title, (F32)t.getCenterX(), (F32)t.getCenterY(), INK, LLFontGL::HCENTER, (F32)t.getWidth());
    untextured();
    line((F32)t.mLeft + 20.f, (F32)t.mBottom, (F32)t.mRight - 20.f, (F32)t.mBottom, 1.f, withAlpha(INK, 0.5f));
}

void WolfFloaterRadio::drawStatusPlate() const
{
    const LLRect& p = mLayout.status_plate;
    untextured();
    LLRect shadow = p;
    shadow.translate(1, -2);
    fillRounded(shadow, allCorners(4.f), SHADOW, SHADOW);
    fillRounded(p, allCorners(4.f), PLATE, PLATE);
    strokeRounded(p, allCorners(4.f), 0.f, 1.5f, BRASS, BRASS_DARK);
    const F32 sr = 2.5f;
    screw(p.mLeft + 5.f, p.mTop - 5.f, sr);
    screw(p.mRight - 5.f, p.mTop - 5.f, sr);
    screw(p.mLeft + 5.f, p.mBottom + 5.f, sr);
    screw(p.mRight - 5.f, p.mBottom + 5.f, sr);
}

void WolfFloaterRadio::drawKnob(F32 cx, F32 cy, F32 angle, const std::string& label) const
{
    const F32 R = mLayout.knob_r;
    untextured();
    // the scale of dots on the cabinet round it
    for (S32 i = 0; i <= 10; ++i)
    {
        const F32 a = -KNOB_SWEEP + 2.f * KNOB_SWEEP * i / 10.f;
        disc(cx + R * 1.2f * sinf(a), cy + R * 1.2f * cosf(a), (i % 5) == 0 ? 1.8f : 1.2f, withAlpha(TEXT_CREAM, 0.85f), withAlpha(TEXT_CREAM, 0.6f));
    }
    disc(cx + 2.f, cy - 3.f, R * 1.08f, SHADOW, LLColor4(0.f, 0.f, 0.f, 0.f));
    // the fluted bakelite body
    {
        const S32 n = 96;
        const F32 flutes = 18.f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n, a1 = F_TWO_PI * (i + 1) / n;
            const F32 r0 = R * (1.f - 0.05f * (0.5f - 0.5f * cosf((a0 + angle) * flutes)));
            const F32 r1 = R * (1.f - 0.05f * (0.5f - 0.5f * cosf((a1 + angle) * flutes)));
            vtx(cx, cy, BAKELITE_TOP);
            vtx(cx + r0 * sinf(a0), cy + r0 * cosf(a0), lit(BAKELITE_SIDE, BAKELITE_BOTTOM, BAKELITE_LIGHT, a0));
            vtx(cx + r1 * sinf(a1), cy + r1 * cosf(a1), lit(BAKELITE_SIDE, BAKELITE_BOTTOM, BAKELITE_LIGHT, a1));
        }
        gGL.end();
    }
    // the cream pointer notch on the skirt, then the chrome cap with its engraved line
    const F32 sa = sinf(angle), ca = cosf(angle);
    line(cx + sa * R * 0.66f, cy + ca * R * 0.66f, cx + sa * R * 0.95f, cy + ca * R * 0.95f, llmax(2.f, R * 0.09f), TEXT_CREAM);
    metalRing(cx, cy, R * 0.56f, R * 0.62f, CHROME_DARK, CHROME, CHROME_LIGHT);
    disc(cx, cy, R * 0.56f, CHROME_LIGHT, CHROME);
    line(cx + sa * R * 0.12f, cy + ca * R * 0.12f, cx + sa * R * 0.50f, cy + ca * R * 0.50f, llmax(1.5f, R * 0.06f), INK);
    disc(cx - R * 0.2f, cy + R * 0.2f, R * 0.18f, LLColor4(1.f, 1.f, 1.f, 0.45f), LLColor4(1.f, 1.f, 1.f, 0.f));

    // its name, cut into the wood below
    const F32 max_w = R * 3.2f;
    const LLFontGL* f = fitFont(15.f, max_w, { label }, 0);
    engraved(f, label, cx, mLayout.knob_label_y, TEXT_CREAM, LLFontGL::HCENTER, max_w);
}

WolfFloaterRadio::Knob WolfFloaterRadio::knobAt(S32 x, S32 y) const
{
    const F32 r2 = mLayout.knob_r * mLayout.knob_r * 1.25f;
    F32 dx = x - mLayout.vol_x, dy = y - mLayout.vol_y;
    if (dx * dx + dy * dy <= r2) return Knob::VOLUME;
    dx = x - mLayout.tune_x;
    dy = y - mLayout.tune_y;
    if (dx * dx + dy * dy <= r2) return Knob::TUNING;
    return Knob::NONE;
}

LLRect WolfFloaterRadio::knobRect(Knob knob) const
{
    const F32 cx = knob == Knob::VOLUME ? mLayout.vol_x : mLayout.tune_x;
    const F32 cy = knob == Knob::VOLUME ? mLayout.vol_y : mLayout.tune_y;
    const S32 r = ll_round(mLayout.knob_r * 1.15f);
    return LLRect(ll_round(cx) - r, ll_round(cy) + r, ll_round(cx) + r, ll_round(cy) - r);
}

void WolfFloaterRadio::turnKnob(Knob knob, S32 steps)
{
    if (steps == 0) return;
    if (knob == Knob::VOLUME)
    {
        // the viewer's streaming-music volume (llvieweraudio.cpp: AudioLevelMusic is the music's
        // secondary gain), which is what the preview and the land's music both play through
        const F32 v = llclamp(gSavedSettings.getF32("AudioLevelMusic") + VOLUME_STEP * steps, 0.f, 1.f);
        gSavedSettings.setF32("AudioLevelMusic", v);
    }
    else if (knob == Knob::TUNING)
    {
        // the previous / next station in the log, as the arrow keys in the list do; then the same
        // path as picking it (so Listen follows the tuning)
        const S32 count = mStations->getItemCount();
        if (count <= 0) return;
        const S32 cur = mStations->getFirstSelectedIndex();
        const S32 next = cur < 0 ? (steps > 0 ? 0 : count - 1) : llclamp(cur + steps, 0, count - 1);
        if (next == cur) return;
        mStations->selectNthItem(next);   // does not commit (commit_on_selection_change is off)
        mStations->scrollToShowSelected();
        onSelectStation();
    }
}

std::string WolfFloaterRadio::knobToolTip(Knob knob) const
{
    if (knob == Knob::VOLUME)
    {
        LLStringUtil::format_map_t args;
        args["[PERCENT]"] = llformat("%d", ll_round(llclamp(gSavedSettings.getF32("AudioLevelMusic"), 0.f, 1.f) * 100.f));
        std::string tip = getString("volume_tip", args);
        if (gSavedSettings.getBOOL("MuteMusic"))
        {
            tip += "\n" + getString("volume_muted");
        }
        return tip;
    }
    if (knob == Knob::TUNING)
    {
        return getString("tuning_tip");
    }
    return std::string();
}
