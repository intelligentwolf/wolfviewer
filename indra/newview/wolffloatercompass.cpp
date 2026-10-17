/**
 * @file wolffloatercompass.cpp
 * @brief WolfViewer: World > Compass — a Victorian mariner's compass that shows North and the
 *        direction you are actually moving. See wolffloatercompass.h for the frame it uses.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloatercompass.h"

#include "llagent.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llresizehandle.h"
#include "llui.h"
#include "lluiimage.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"

#include <cmath>
#include <iterator>

namespace
{
    // ---- behaviour (design values, not protocol values) ----
    // Below this horizontal speed (m/s) the avatar or vehicle counts as at rest: standing still
    // the avatar's reported velocity is not exactly zero, and a course from a few cm/s of drift
    // would swing the arrow about at random.
    constexpr F32 AT_REST_SPEED = 0.2f;
    // How fast the card and the arrow ease towards a new bearing (per second; 1 - e^(-rate*dt) of
    // the gap is closed each frame) - quick enough to follow a mouselook turn, slow enough that a
    // jittery camera does not shake the card.
    constexpr F32 EASE_RATE = 9.f;
    // How fast the course arrow fades in and out when starting and stopping (per second).
    constexpr F32 FADE_RATE = 4.f;
    // A frame gap longer than this (s) - the window was hidden or the viewer stalled - snaps the
    // card to the present instead of spinning it there.
    constexpr F32 MAX_EASE_GAP = 0.5f;

    // ---- palette: brass, walnut, aged parchment, red and blued steel ----
    // Contrast (WCAG 2.x relative luminance), worked for the text that carries information:
    //   readout cream #f2e3bd on the plaque #1a0f09: 14.8:1; the dim "at rest" #b39461: 6.6:1
    //   card ink #2b1c12 on parchment #f2e6c2: 13.4:1, on the aged rim #d6bd87: 9.1:1
    //   the crimson N #9e1a14 on parchment: 6.5:1
    // Fixed colours, not the skin's: this is a painted instrument and no skin may lower them.
    const LLColor4 WALNUT_TOP(0.23f, 0.14f, 0.08f, 1.f);
    const LLColor4 WALNUT_BOTTOM(0.13f, 0.08f, 0.045f, 1.f);
    const LLColor4 PLAQUE(0.10f, 0.06f, 0.035f, 1.f);
    const LLColor4 BRASS(0.78f, 0.60f, 0.27f, 1.f);
    const LLColor4 BRASS_DARK(0.40f, 0.27f, 0.10f, 1.f);
    const LLColor4 BRASS_LIGHT(0.97f, 0.86f, 0.55f, 1.f);
    const LLColor4 PARCHMENT(0.95f, 0.90f, 0.76f, 1.f);
    const LLColor4 PARCHMENT_AGED(0.84f, 0.74f, 0.53f, 1.f);
    const LLColor4 PARCHMENT_LIGHT(1.f, 0.97f, 0.88f, 0.85f);
    const LLColor4 INK(0.17f, 0.11f, 0.07f, 1.f);
    const LLColor4 SEPIA(0.42f, 0.27f, 0.14f, 1.f);
    const LLColor4 ROSE_GOLD(0.80f, 0.63f, 0.30f, 1.f);
    const LLColor4 ROSE_CREAM(0.90f, 0.81f, 0.60f, 1.f);
    const LLColor4 ROSE_RED(0.52f, 0.15f, 0.10f, 1.f);
    const LLColor4 CRIMSON(0.62f, 0.10f, 0.08f, 1.f);
    const LLColor4 CRIMSON_LIGHT(0.82f, 0.22f, 0.15f, 1.f);
    const LLColor4 STEEL_BLACK(0.09f, 0.09f, 0.10f, 1.f);
    const LLColor4 STEEL_GREY(0.34f, 0.34f, 0.37f, 1.f);
    const LLColor4 BLUED(0.10f, 0.19f, 0.42f, 1.f);
    const LLColor4 BLUED_LIGHT(0.27f, 0.43f, 0.74f, 1.f);
    const LLColor4 GOLD_LEAF(0.88f, 0.71f, 0.30f, 1.f);
    const LLColor4 TEXT_CREAM(0.95f, 0.89f, 0.74f, 1.f);
    const LLColor4 TEXT_DIM(0.70f, 0.58f, 0.38f, 1.f);

    // English names, used only if the XUI list is missing or malformed.
    const char* const DEFAULT_POINTS[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                             "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };

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

    F32 wrapDegrees(F32 d)
    {
        d = fmodf(d, 360.f);
        return d < 0.f ? d + 360.f : d;
    }

    // Shortest way round from cur to target, closing fraction k of the gap.
    F32 easeDegrees(F32 cur, F32 target, F32 k)
    {
        const F32 gap = fmodf(target - cur + 540.f, 360.f) - 180.f;   // target - cur is in (-360, 360)
        return wrapDegrees(cur + gap * k);
    }

    // Bearing of a horizontal world vector, degrees clockwise from north.
    // Source: lldefs.h gDirAxes - east is {1, 0} (+X), north is {0, 1} (+Y).
    F32 bearingOf(F32 east, F32 north)
    {
        return wrapDegrees(atan2f(east, north) * RAD_TO_DEG);
    }

    // ---- lettering: Cinzel Bold, classical Roman capitals ----
    // The WolfDisplay family (fonts.xml and every fonts_*.xml: Cinzel-Bold.ttf) at the Avionics0..11
    // sizes, smallest first - the same registration wolfgamehud.cpp display() and wolfflightdeck.cpp
    // avionics() use. Cinzel carries the degree sign (U+00B0).
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
                LL_WARNS("WolfCompass") << "font WolfDisplay missing, using the sans serif" << LL_ENDL;
                fonts.push_back(LLFontGL::getFontSansSerif());
            }
        }
        return fonts;
    }

    // The largest size whose line fits px (the smallest when none does).
    const LLFontGL* displayFont(F32 px)
    {
        const std::vector<const LLFontGL*>& fonts = displayFonts();
        const LLFontGL* best = fonts.front();
        for (const LLFontGL* f : fonts)
        {
            if ((F32)f->getLineHeight() <= px)
            {
                best = f;
            }
        }
        return best;
    }

    void text(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        font->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // Engraved into the card: a pale lip one pixel below, the ink on top.
    void engraved(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& ink)
    {
        text(font, s, x, y - 1.f, PARCHMENT_LIGHT);
        text(font, s, x, y, ink);
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

    S32 segmentsFor(F32 r)
    {
        return llclamp((S32)(r * 0.6f), 48, 160);
    }

    // A filled disc, its colour running from the centre to the rim.
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

    // Polished metal: brighter towards the light (upper left), darker away from it.
    LLColor4 sheen(const LLColor4& base, F32 a)
    {
        constexpr F32 LIGHT_FROM = -F_PI / 4.f;   // upper left
        const F32 t = 0.5f + 0.5f * cosf(a - LIGHT_FROM);
        return t > 0.5f ? lerpColour(base, BRASS_LIGHT, (t - 0.5f) * 1.1f)
                        : lerpColour(BRASS_DARK, base, t * 2.f);
    }

    // An annulus r0..r1. With metal set, each vertex takes sheen() of base.
    void ring(F32 cx, F32 cy, F32 r0, F32 r1, const LLColor4& inner, const LLColor4& outer, bool metal = false)
    {
        const S32 n = segmentsFor(r1);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n;
            const F32 a1 = F_TWO_PI * (i + 1) / n;
            const F32 s0 = sinf(a0), c0 = cosf(a0), s1 = sinf(a1), c1 = cosf(a1);
            const LLColor4 i0 = metal ? sheen(inner, a0) : inner;
            const LLColor4 i1 = metal ? sheen(inner, a1) : inner;
            const LLColor4 o0 = metal ? sheen(outer, a0) : outer;
            const LLColor4 o1 = metal ? sheen(outer, a1) : outer;
            vtx(cx + r0 * s0, cy + r0 * c0, i0);
            vtx(cx + r1 * s0, cy + r1 * c0, o0);
            vtx(cx + r1 * s1, cy + r1 * c1, o1);
            vtx(cx + r0 * s0, cy + r0 * c0, i0);
            vtx(cx + r1 * s1, cy + r1 * c1, o1);
            vtx(cx + r0 * s1, cy + r0 * c1, i1);
        }
        gGL.end();
    }

    void circleLine(F32 cx, F32 cy, F32 r, F32 w, const LLColor4& c)
    {
        ring(cx, cy, r - w * 0.5f, r + w * 0.5f, c, c);
    }

    // A straight line w pixels wide.
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

    // A frame in the plane: origin, "up" axis (u) and "right" axis (v), scaled to pixels.
    struct Frame
    {
        F32 ox, oy;
        F32 ux, uy;   // unit "up" (outward along the bearing)
        F32 vx, vy;   // unit "right"
        F32 s;        // pixels per unit
        Frame(F32 cx, F32 cy, F32 a, F32 scale)
            : ox(cx), oy(cy), ux(sinf(a)), uy(cosf(a)), vx(cosf(a)), vy(-sinf(a)), s(scale) {}
        F32 x(F32 v, F32 u) const { return ox + (ux * u + vx * v) * s; }
        F32 y(F32 v, F32 u) const { return oy + (uy * u + vy * v) * s; }
    };

    // A tapering ribbon along a centreline of n points (v, u) with half-widths hw (frame units).
    void ribbon(const Frame& f, const std::vector<LLVector2>& pts, const std::vector<F32>& hw,
                const LLColor4& c, F32 dx = 0.f, F32 dy = 0.f)
    {
        const size_t n = pts.size();
        if (n < 2) return;
        std::vector<LLVector2> left(n), right(n);
        for (size_t i = 0; i < n; ++i)
        {
            const LLVector2& a = pts[i == 0 ? 0 : i - 1];
            const LLVector2& b = pts[i == n - 1 ? n - 1 : i + 1];
            LLVector2 t(b.mV[VX] - a.mV[VX], b.mV[VY] - a.mV[VY]);
            const F32 len = t.length();
            if (len > 0.0001f) t /= len;
            const LLVector2 nrm(-t.mV[VY], t.mV[VX]);
            left[i] = pts[i] + nrm * hw[i];
            right[i] = pts[i] - nrm * hw[i];
        }
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const LLVector2& l0 = left[i];
            const LLVector2& l1 = left[i + 1];
            const LLVector2& r0 = right[i];
            const LLVector2& r1 = right[i + 1];
            vtx(f.x(l0.mV[VX], l0.mV[VY]) + dx, f.y(l0.mV[VX], l0.mV[VY]) + dy, c);
            vtx(f.x(l1.mV[VX], l1.mV[VY]) + dx, f.y(l1.mV[VX], l1.mV[VY]) + dy, c);
            vtx(f.x(r1.mV[VX], r1.mV[VY]) + dx, f.y(r1.mV[VX], r1.mV[VY]) + dy, c);
            vtx(f.x(l0.mV[VX], l0.mV[VY]) + dx, f.y(l0.mV[VX], l0.mV[VY]) + dy, c);
            vtx(f.x(r1.mV[VX], r1.mV[VY]) + dx, f.y(r1.mV[VX], r1.mV[VY]) + dy, c);
            vtx(f.x(r0.mV[VX], r0.mV[VY]) + dx, f.y(r0.mV[VX], r0.mV[VY]) + dy, c);
        }
        gGL.end();
    }

    // The fleur-de-lis on the North tip of the needle, in frame units: v across (about -0.9..0.9),
    // u outward (0 at its foot, 1.22 at the tip of the middle petal). (dx, dy) shifts the whole
    // mark - used for its shadow.
    void fleurDeLis(const Frame& f, const LLColor4& c, F32 dx, F32 dy)
    {
        constexpr S32 N = 18;
        std::vector<LLVector2> pts;
        std::vector<F32> hw;

        // middle petal: a spear, widest a little below half way, pointed at the top
        for (S32 i = 0; i <= N; ++i)
        {
            const F32 t = (F32)i / N;
            pts.emplace_back(0.f, 0.42f + 0.80f * t);
            hw.push_back(0.31f * sinf(F_PI * (0.15f + 0.85f * t)) * (1.f - 0.2f * t));
        }
        ribbon(f, pts, hw, c, dx, dy);

        // the two side petals: rising from the band, arching over and curling down and out
        for (F32 side : { -1.f, 1.f })
        {
            pts.clear();
            hw.clear();
            constexpr F32 CV = 0.50f, CU = 0.70f, RR = 0.36f;   // centre and radius of the curl
            for (S32 i = 0; i <= N; ++i)
            {
                const F32 t = (F32)i / N;
                const F32 phi = (215.f - 265.f * t) * DEG_TO_RAD;   // 215 deg round to -50 deg
                pts.emplace_back(side * (CV + RR * cosf(phi)), CU + RR * sinf(phi));
                hw.push_back(0.13f * (1.f - t) + 0.02f);
            }
            ribbon(f, pts, hw, c, dx, dy);
        }

        // the band that binds the petals
        {
            constexpr F32 BV = 0.40f, B0 = 0.36f, B1 = 0.48f;
            gGL.begin(LLRender::TRIANGLES);
            vtx(f.x(-BV, B0) + dx, f.y(-BV, B0) + dy, c);
            vtx(f.x(-BV, B1) + dx, f.y(-BV, B1) + dy, c);
            vtx(f.x(BV, B1) + dx, f.y(BV, B1) + dy, c);
            vtx(f.x(-BV, B0) + dx, f.y(-BV, B0) + dy, c);
            vtx(f.x(BV, B1) + dx, f.y(BV, B1) + dy, c);
            vtx(f.x(BV, B0) + dx, f.y(BV, B0) + dy, c);
            gGL.end();
        }

        // the foot: a tapering stem towards the centre of the card and two small spurs
        pts.clear();
        hw.clear();
        for (S32 i = 0; i <= N / 2; ++i)
        {
            const F32 t = (F32)i / (N / 2);
            pts.emplace_back(0.f, 0.37f - 0.37f * t);
            hw.push_back(0.11f * (1.f - t) + 0.01f);
        }
        ribbon(f, pts, hw, c, dx, dy);
        for (F32 side : { -1.f, 1.f })
        {
            tri(f.x(side * 0.08f, 0.37f) + dx, f.y(side * 0.08f, 0.37f) + dy,
                f.x(side * 0.34f, 0.37f) + dx, f.y(side * 0.34f, 0.37f) + dy,
                f.x(side * 0.30f, 0.20f) + dx, f.y(side * 0.30f, 0.20f) + dy, c);
        }
    }

    // One point of the rose: a kite from the centre to its tip, its two corners on either side.
    // Shaded the classical way, dark on the clockwise half and light on the other, ink-ruled.
    void rosePoint(F32 cx, F32 cy, F32 a, F32 tip_r, F32 corner_r, F32 corner_half_angle,
                   const LLColor4& dark, const LLColor4& light, F32 rule_w)
    {
        const F32 tx = cx + tip_r * sinf(a), ty = cy + tip_r * cosf(a);
        const F32 lx = cx + corner_r * sinf(a - corner_half_angle), ly = cy + corner_r * cosf(a - corner_half_angle);
        const F32 rx = cx + corner_r * sinf(a + corner_half_angle), ry = cy + corner_r * cosf(a + corner_half_angle);
        tri(cx, cy, lx, ly, tx, ty, light);
        tri(cx, cy, tx, ty, rx, ry, dark);
        line(lx, ly, tx, ty, rule_w, INK);
        line(tx, ty, rx, ry, rule_w, INK);
        line(cx, cy, tx, ty, rule_w, INK);
    }

    // The course pointer: a blued-steel wedge, its tip at (tx, ty) pointing along the unit vector
    // (dx, dy), h long and hw half-wide at its base. Paul 10-16: "the compass arrow should always turn
    // north" - so the needle is the only arrow on the card, and the way you move is this pointer on
    // the brass rim instead. With expand > 0 it draws the same wedge fattened by that many pixels in
    // gold leaf - its gilt outline.
    void coursePointer(F32 tx, F32 ty, F32 dx, F32 dy, F32 h, F32 hw, F32 alpha, F32 expand)
    {
        const F32 e = expand;
        const bool outline = e > 0.f;
        const LLColor4 left = withAlpha(outline ? GOLD_LEAF : BLUED_LIGHT, alpha);
        const LLColor4 right = withAlpha(outline ? GOLD_LEAF : BLUED, alpha);
        const F32 px = dy, py = -dx;   // perpendicular, to the right of the pointing direction
        const F32 ax = tx + dx * e * 1.5f, ay = ty + dy * e * 1.5f;           // tip
        const F32 bx = tx - dx * (h + e), by = ty - dy * (h + e);             // middle of the base
        const F32 w = hw + e;
        // a shallow notch in the base so it reads as a pointer, not a triangle
        const F32 nx = tx - dx * (h * 0.78f), ny = ty - dy * (h * 0.78f);
        tri(ax, ay, bx - px * w, by - py * w, nx, ny, left);
        tri(ax, ay, nx, ny, bx + px * w, by + py * w, right);
    }

    // The lubber mark: a black enamel wedge with a bright brass edge, its tip at (x, y) pointing
    // down; h tall, hw half-width.
    void lubberMark(F32 x, F32 y, F32 h, F32 hw)
    {
        const F32 e = llmax(1.f, h * 0.08f);
        tri(x, y - e * 1.5f, x - hw - e, y + h + e * 0.5f, x + hw + e, y + h + e * 0.5f, BRASS_LIGHT);
        tri(x, y, x - hw, y + h, x + hw, y + h, INK);
    }
}

WolfFloaterCompass::WolfFloaterCompass(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterCompass::postBuild()
{
    mHeadingLabel = getString("heading_label");
    mCourseLabel = getString("course_label");
    mAtRestLabel = getString("at_rest");

    // "N,NNE,NE,..." clockwise from North
    mPointNames.clear();
    const std::string names = getString("point_names");
    std::string::size_type start = 0;
    while (start <= names.size())
    {
        const std::string::size_type comma = names.find(',', start);
        const std::string::size_type end = (comma == std::string::npos) ? names.size() : comma;
        std::string name = names.substr(start, end - start);
        LLStringUtil::trim(name);
        mPointNames.push_back(name);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (mPointNames.size() != 16)
    {
        LL_WARNS("WolfCompass") << "point_names has " << mPointNames.size() << " names, not 16; using English" << LL_ENDL;
        mPointNames.assign(std::begin(DEFAULT_POINTS), std::end(DEFAULT_POINTS));
    }
    return true;
}

void WolfFloaterCompass::onOpen(const LLSD& key)
{
    // Start from the present, not from wherever the card stood when the window last closed.
    mPrimed = false;
    mHaveCourse = false;
    mIsMoving = false;
}

const std::string& WolfFloaterCompass::pointName(F32 bearing_deg) const
{
    // 16 points, 22.5 degrees apart, each owning +/- 11.25 degrees about itself
    const S32 i = (S32)floorf(wrapDegrees(bearing_deg) / 22.5f + 0.5f) % 16;
    return mPointNames[i];
}

F32 WolfFloaterCompass::sampleHeading() const
{
    // The way the main view looks, flattened onto the ground - the axis the mini-map turns with
    // (llnetmap.cpp draw(): atan2(camera at.x, camera at.y)). Looking straight down or up the
    // at-axis has no horizontal part; then the camera's up-axis gives the facing instead: pitched
    // down by t it is (sin t, 0, cos t) in the camera's forward plane - ahead - but pitched up it is
    // (-sin t, 0, cos t) - behind - so looking up it is reversed.
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    LLVector3 at = cam->getAtAxis();
    const bool looking_up = at.mV[VZ] > 0.f;
    at.mV[VZ] = 0.f;
    if (at.length() < 0.05f)
    {
        at = cam->getUpAxis();
        at.mV[VZ] = 0.f;
        if (looking_up)
        {
            at = -at;
        }
    }
    if (at.length() < 0.0001f)
    {
        return mHeading;
    }
    return bearingOf(at.mV[VX], at.mV[VY]);
}

bool WolfFloaterCompass::sampleCourse(F32& course_deg) const
{
    if (!isAgentAvatarValid())
    {
        return false;
    }
    LLVector3 vel;
    if (gAgentAvatarp->isSitting())
    {
        // Seated, the avatar rides its seat and reports no motion of its own: the course is the
        // vehicle's, its root prim's (the same root wolfdashboard.cpp vehicleRoot() reads).
        LLViewerObject* root = (LLViewerObject*)gAgentAvatarp->getParent();
        while (root && root->getParent())
        {
            root = (LLViewerObject*)root->getParent();
        }
        if (!root)
        {
            return false;
        }
        // A keyframed vehicle (llSetKeyframedMotion) reports one too: OpenSim KeyframeMotion.cs
        // sets RootPart.Velocity = m_currentVel on every step while it moves.
        vel = root->getVelocity();
    }
    else
    {
        vel = gAgent.getVelocity();
    }
    // Region velocities are in the region frame, whose axes are the world's (+X east, +Y north).
    const F32 east = vel.mV[VX], north = vel.mV[VY];
    if (sqrtf(east * east + north * north) < AT_REST_SPEED)
    {
        return false;
    }
    course_deg = bearingOf(east, north);
    return true;
}

void WolfFloaterCompass::updateSmoothing()
{
    const F32 dt = mFrameTimer.getElapsedTimeAndResetF32();
    const F32 heading = sampleHeading();
    F32 course = 0.f;
    mIsMoving = sampleCourse(course);

    if (!mPrimed || dt > MAX_EASE_GAP)
    {
        mPrimed = true;
        mHeading = heading;
        if (mIsMoving)
        {
            mCourse = course;
            mHaveCourse = true;
        }
        mMoving = mIsMoving ? 1.f : 0.f;
        return;
    }

    const F32 k = 1.f - expf(-dt * EASE_RATE);
    mHeading = easeDegrees(mHeading, heading, k);
    if (mIsMoving)
    {
        // the first course after rest is where the arrow starts from - no spin from a stale one
        mCourse = mHaveCourse ? easeDegrees(mCourse, course, k) : course;
        mHaveCourse = true;
    }
    const F32 kf = 1.f - expf(-dt * FADE_RATE);
    mMoving += ((mIsMoving ? 1.f : 0.f) - mMoving) * kf;
}

void WolfFloaterCompass::draw()
{
    LLFloater::draw();
    if (isMinimized())
    {
        return;
    }
    updateSmoothing();

    const LLRect local = getLocalRect();
    const S32 W = local.getWidth();
    const S32 top = local.mTop - getHeaderHeight();
    const S32 bottom = local.mBottom + LLPANEL_BORDER_WIDTH;
    const S32 left = local.mLeft + LLPANEL_BORDER_WIDTH;
    const S32 right = local.mRight - LLPANEL_BORDER_WIDTH;
    if (top - bottom < 40 || right - left < 40)
    {
        return;
    }

    // The walnut case the instrument sits in.
    untextured();
    gGL.begin(LLRender::TRIANGLES);
    vtx((F32)left, (F32)bottom, WALNUT_BOTTOM);
    vtx((F32)right, (F32)top, WALNUT_TOP);
    vtx((F32)left, (F32)top, WALNUT_TOP);
    vtx((F32)left, (F32)bottom, WALNUT_BOTTOM);
    vtx((F32)right, (F32)bottom, WALNUT_BOTTOM);
    vtx((F32)right, (F32)top, WALNUT_TOP);
    gGL.end();

    // The readout plaque: two lines, as large as the window allows and still fits across.
    constexpr S32 MARGIN = 8;
    const S32 client_w = right - left;
    const S32 client_h = top - bottom;
    const F32 want_px = llclamp(llmin(client_w * 0.075f, client_h * 0.06f), 11.f, 28.f);
    const F32 plaque_text_w = (F32)(client_w - 2 * MARGIN);
    // the longest line it can show (WSW stands for the widest point name)
    const std::string samples[] = { mHeadingLabel + "  000\xC2\xB0 WSW", mCourseLabel + "  000\xC2\xB0 WSW",
                                    mCourseLabel + "  " + mAtRestLabel };
    const std::vector<const LLFontGL*>& fonts = displayFonts();
    const LLFontGL* font = fonts.front();
    for (const LLFontGL* f : fonts)
    {
        F32 widest = 0.f;
        for (const std::string& s : samples)
        {
            widest = llmax(widest, f->getWidthF32(s));
        }
        // room for the legend glyph (drawReadout: text starts 1.75 line heights in) and padding
        const F32 need = widest + 3.f * (F32)f->getLineHeight();
        if ((F32)f->getLineHeight() <= want_px && need <= plaque_text_w)
        {
            font = f;
        }
    }
    const S32 lh = font->getLineHeight();
    const S32 pad = llmax(4, lh / 3);
    const S32 plaque_h = 2 * lh + 3 * pad;
    const LLRect plaque(left + MARGIN, bottom + MARGIN + plaque_h, right - MARGIN, bottom + MARGIN);

    // The compass fills what is left above the plaque.
    const F32 dial_top = (F32)(top - MARGIN);
    const F32 dial_bottom = (F32)(plaque.mTop + MARGIN);
    const F32 R = llmin((F32)(client_w - 2 * MARGIN) * 0.5f, (dial_top - dial_bottom) * 0.5f);
    if (R >= 24.f)
    {
        drawCompass((F32)W * 0.5f, (dial_top + dial_bottom) * 0.5f, R);
    }
    drawReadout(plaque, font);

    // The walnut covers the resize corner the floater drew underneath; put it back on top.
    if (isResizable())
    {
        LLUIImagePtr corner = LLUI::getUIImage("Resize_Corner");   // llresizehandle.cpp LLResizeHandle()
        if (corner.notNull())
        {
            // llfloater.cpp layoutResizeCtrls(): the RIGHT_BOTTOM handle's rect starts here
            corner->draw(W - RESIZE_HANDLE_WIDTH, 0);
        }
    }
}

void WolfFloaterCompass::drawCompass(F32 cx, F32 cy, F32 R) const
{
    // Card and needle turn together: card bearing b is drawn at screen angle (b - heading).
    const F32 rot = mHeading * DEG_TO_RAD;
    auto screenAngle = [rot](F32 bearing_deg) { return bearing_deg * DEG_TO_RAD - rot; };
    const F32 rule_w = llmax(1.f, R * 0.006f);

    untextured();

    // ---- the brass bowl (fixed) ----
    disc(cx + R * 0.02f, cy - R * 0.03f, R * 1.02f, LLColor4(0.f, 0.f, 0.f, 0.35f), LLColor4(0.f, 0.f, 0.f, 0.f));
    ring(cx, cy, R * 0.84f, R, BRASS, BRASS, true);
    ring(cx, cy, R * 0.975f, R, BRASS_DARK, BRASS_DARK);
    ring(cx, cy, R * 0.84f, R * 0.858f, BRASS_DARK, BRASS_DARK);
    // a ring of raised beads round the bezel, each domed (bright centre, dark rim)
    {
        const F32 bead_r = llmax(1.2f, R * 0.012f);
        for (S32 i = 0; i < 36; ++i)
        {
            const F32 a = F_TWO_PI * i / 36.f;
            const F32 bx = cx + R * 0.917f * sinf(a), by = cy + R * 0.917f * cosf(a);
            disc(bx, by, bead_r, BRASS_LIGHT, BRASS_DARK);
        }
    }

    // ---- the card (turns) ----
    disc(cx, cy, R * 0.84f, PARCHMENT, PARCHMENT_AGED);
    // double rule at the rim and degree ticks inside it, read against the lubber mark
    circleLine(cx, cy, R * 0.82f, rule_w * 1.4f, INK);
    circleLine(cx, cy, R * 0.805f, rule_w, INK);
    for (S32 d = 0; d < 360; d += 5)
    {
        const F32 a = screenAngle((F32)d);
        const bool major = (d % 45) == 0;
        const bool ten = (d % 10) == 0;
        const F32 inner = R * (major ? 0.745f : ten ? 0.765f : 0.78f);
        const F32 w = rule_w * (major ? 1.7f : ten ? 1.3f : 1.f);
        line(cx + R * 0.805f * sinf(a), cy + R * 0.805f * cosf(a), cx + inner * sinf(a), cy + inner * cosf(a), w, INK);
    }
    // Card radii, as fractions of R, from the rim inwards:
    //   0.84 card edge | 0.82 / 0.805 double rule | ticks 0.805 -> 0.745..0.78
    //   0.615..0.745 lettering band (letters centred on 0.68) | 0.60..0.615 chequered ring
    //   rose: cardinals 0.58, NE etc 0.44, NNE etc 0.33
    //   needle: South 0.56; North 0.42, then its fleur-de-lis to 0.40 + 1.22 x 0.14 = 0.57
    // chequered chart ring under the lettering band
    circleLine(cx, cy, R * 0.615f, rule_w, INK);
    circleLine(cx, cy, R * 0.60f, rule_w, INK);
    for (S32 i = 0; i < 32; i += 2)
    {
        const F32 a0 = screenAngle(i * 11.25f), a1 = screenAngle((i + 1) * 11.25f);
        gGL.begin(LLRender::TRIANGLES);
        const S32 steps = 4;
        for (S32 s = 0; s < steps; ++s)
        {
            const F32 b0 = a0 + (a1 - a0) * s / steps, b1 = a0 + (a1 - a0) * (s + 1) / steps;
            vtx(cx + R * 0.60f * sinf(b0), cy + R * 0.60f * cosf(b0), SEPIA);
            vtx(cx + R * 0.615f * sinf(b0), cy + R * 0.615f * cosf(b0), SEPIA);
            vtx(cx + R * 0.615f * sinf(b1), cy + R * 0.615f * cosf(b1), SEPIA);
            vtx(cx + R * 0.60f * sinf(b0), cy + R * 0.60f * cosf(b0), SEPIA);
            vtx(cx + R * 0.615f * sinf(b1), cy + R * 0.615f * cosf(b1), SEPIA);
            vtx(cx + R * 0.60f * sinf(b1), cy + R * 0.60f * cosf(b1), SEPIA);
        }
        gGL.end();
    }

    // the 16-point rose, smallest points first so the cardinals lie on top
    circleLine(cx, cy, R * 0.20f, rule_w, INK);
    for (S32 i = 1; i < 16; i += 2)   // by-points: NNE, ENE, ...
    {
        rosePoint(cx, cy, screenAngle(i * 22.5f), R * 0.33f, R * 0.075f, 11.25f * DEG_TO_RAD, ROSE_RED, ROSE_CREAM, rule_w * 0.8f);
    }
    for (S32 i = 2; i < 16; i += 4)   // NE, SE, SW, NW
    {
        rosePoint(cx, cy, screenAngle(i * 22.5f), R * 0.44f, R * 0.11f, 22.5f * DEG_TO_RAD, SEPIA, ROSE_CREAM, rule_w);
    }
    for (S32 i = 0; i < 16; i += 4)   // N, E, S, W
    {
        rosePoint(cx, cy, screenAngle(i * 22.5f), R * 0.58f, R * 0.155f, 45.f * DEG_TO_RAD, INK, ROSE_GOLD, rule_w);
    }

    // ---- lettering on the card: upright so it reads at any heading ----
    // [2026-10-17] Larger N/E/S/W (a resident with low vision asked for them bigger; was R * 0.115).
    // displayFont() fits the line height, which LLFontGL::getLineHeight (llfontgl.cpp:479-482) makes
    // ascender + descender: Cinzel-Bold 976 + 372 = 1.348 em, capitals 700 = 0.52 of the line. A 0.20 R
    // line gives 0.104 R capitals, centred on 0.68 R: 0.628..0.732, inside the 0.615..0.745 band.
    const LLFontGL* cardinal_font = displayFont(R * 0.20f);
    const LLFontGL* small_font = displayFont(R * 0.07f);
    const LLFontGL* numeral_font = displayFont(R * 0.06f);
    const F32 letter_r = R * 0.68f;
    for (S32 i = 0; i < 16; i += 2)
    {
        const bool cardinal = (i % 4) == 0;
        if (!cardinal && R < 70.f) continue;   // NE etc. too small to read on a little dial
        const F32 a = screenAngle(i * 22.5f);
        engraved(cardinal ? cardinal_font : small_font, mPointNames[i],
                 cx + letter_r * sinf(a), cy + letter_r * cosf(a), i == 0 ? CRIMSON : INK);
    }
    if (R >= 105.f)
    {
        for (S32 d = 30; d < 360; d += 30)
        {
            if (d % 90 == 0) continue;   // the cardinal letters stand there
            const F32 a = screenAngle((F32)d);
            engraved(numeral_font, llformat("%d", d), cx + letter_r * sinf(a), cy + letter_r * cosf(a), SEPIA);
        }
    }

    untextured();

    // ---- the needle: red to North ending in a fleur-de-lis, blackened steel to South ----
    {
        const F32 a = screenAngle(0.f);
        const F32 len_n = R * 0.42f, len_s = R * 0.56f, hw = R * 0.05f;   // inside the chequered ring (0.60 R)
        const F32 ux = sinf(a), uy = cosf(a), vx = cosf(a), vy = -sinf(a);
        const F32 nx = cx + ux * len_n, ny = cy + uy * len_n;
        const F32 sx = cx - ux * len_s, sy = cy - uy * len_s;
        const F32 lx = cx - vx * hw, ly = cy - vy * hw;
        const F32 rx = cx + vx * hw, ry = cy + vy * hw;
        const F32 sh = llmax(1.f, R * 0.012f);
        // its shadow on the card
        tri(nx + sh, ny - sh, lx + sh, ly - sh, rx + sh, ry - sh, LLColor4(0.f, 0.f, 0.f, 0.25f));
        tri(sx + sh, sy - sh, lx + sh, ly - sh, rx + sh, ry - sh, LLColor4(0.f, 0.f, 0.f, 0.25f));
        tri(cx, cy, lx, ly, nx, ny, CRIMSON_LIGHT);
        tri(cx, cy, nx, ny, rx, ry, CRIMSON);
        tri(cx, cy, sx, sy, lx, ly, STEEL_GREY);
        tri(cx, cy, rx, ry, sx, sy, STEEL_BLACK);
        // the fleur-de-lis: its foot at 0.40 R, overlapping the needle's point; unit 0.14 R
        const Frame fleur(cx + ux * R * 0.40f, cy + uy * R * 0.40f, a, R * 0.14f);
        fleurDeLis(fleur, LLColor4(0.f, 0.f, 0.f, 0.25f), sh, -sh);
        fleurDeLis(fleur, CRIMSON, 0.f, 0.f);
    }

    // ---- the course pointer on the brass rim (fixed frame: straight up = moving the way you look) ----
    // Shown only while moving; at rest it is gone (the readout says "at rest"), so nothing pale is
    // ever left on the dial and the needle alone points North.
    if (mMoving > 0.01f && mHaveCourse)
    {
        const F32 a = (mCourse - mHeading) * DEG_TO_RAD;
        const F32 ux = sinf(a), uy = cosf(a);
        // in the degree-tick band, 0.745 .. 0.86 R: below the lubber mark (whose tip is at 0.865 R), so
        // moving straight ahead shows the black mark with the blue pointer right under it
        const F32 tip_r = R * 0.745f;
        const F32 h = R * 0.115f, hw = R * 0.07f;
        coursePointer(cx + ux * tip_r, cy + uy * tip_r, -ux, -uy, h, hw, mMoving, llmax(1.f, R * 0.012f));
        coursePointer(cx + ux * tip_r, cy + uy * tip_r, -ux, -uy, h, hw, mMoving, 0.f);
    }

    // ---- the pivot: a brass cap with a jewel bearing ----
    disc(cx, cy, R * 0.07f, BRASS_LIGHT, BRASS_DARK);
    circleLine(cx, cy, R * 0.07f, rule_w, BRASS_DARK);
    disc(cx, cy, R * 0.03f, CRIMSON_LIGHT, LLColor4(0.30f, 0.04f, 0.04f, 1.f));
    disc(cx - R * 0.01f, cy + R * 0.01f, llmax(1.f, R * 0.008f), LLColor4(1.f, 1.f, 1.f, 0.8f), LLColor4(1.f, 1.f, 1.f, 0.f));

    // ---- a glint on the glass, upper left ----
    {
        const F32 a0 = -80.f * DEG_TO_RAD, a1 = -20.f * DEG_TO_RAD;
        const S32 steps = 16;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 s = 0; s < steps; ++s)
        {
            const F32 t0 = (F32)s / steps, t1 = (F32)(s + 1) / steps;
            const F32 b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
            const LLColor4 c0(1.f, 1.f, 1.f, 0.10f * sinf(F_PI * t0));
            const LLColor4 c1(1.f, 1.f, 1.f, 0.10f * sinf(F_PI * t1));
            vtx(cx + R * 0.60f * sinf(b0), cy + R * 0.60f * cosf(b0), c0);
            vtx(cx + R * 0.80f * sinf(b0), cy + R * 0.80f * cosf(b0), c0);
            vtx(cx + R * 0.80f * sinf(b1), cy + R * 0.80f * cosf(b1), c1);
            vtx(cx + R * 0.60f * sinf(b0), cy + R * 0.60f * cosf(b0), c0);
            vtx(cx + R * 0.80f * sinf(b1), cy + R * 0.80f * cosf(b1), c1);
            vtx(cx + R * 0.60f * sinf(b1), cy + R * 0.60f * cosf(b1), c1);
        }
        gGL.end();
    }

    // ---- the lubber mark (fixed): where you are looking ----
    lubberMark(cx, cy + R * 0.865f, R * 0.115f, R * 0.05f);
}

void WolfFloaterCompass::drawReadout(const LLRect& plaque, const LLFontGL* font) const
{
    untextured();
    gl_rect_2d(plaque, BRASS_DARK, true);
    const LLRect inner(plaque.mLeft + 2, plaque.mTop - 2, plaque.mRight - 2, plaque.mBottom + 2);
    gl_rect_2d(inner, PLAQUE, true);
    const LLRect rule(plaque.mLeft + 4, plaque.mTop - 4, plaque.mRight - 4, plaque.mBottom + 4);
    gl_rect_2d(rule, withAlpha(BRASS, 0.6f), false);

    const S32 lh = font->getLineHeight();
    const S32 pad = (plaque.getHeight() - 2 * lh) / 3;
    const F32 y1 = (F32)(plaque.mTop - pad) - lh * 0.5f;
    const F32 y2 = y1 - (F32)(lh + pad);
    const F32 glyph_cx = (F32)plaque.mLeft + pad + lh * 0.75f;
    const F32 text_x = glyph_cx + lh * 1.0f;

    // legend glyphs: the same marks as on the dial, so each line names what it reads
    lubberMark(glyph_cx, y1 - lh * 0.28f, lh * 0.55f, lh * 0.24f);
    {
        // the rim pointer in small, tip down like the lubber mark: always full colour, it is a legend
        const F32 h = lh * 0.55f;
        coursePointer(glyph_cx, y2 - lh * 0.28f, 0.f, -1.f, h, lh * 0.24f, 1.f, 1.f);
        coursePointer(glyph_cx, y2 - lh * 0.28f, 0.f, -1.f, h, lh * 0.24f, 1.f, 0.f);
    }

    const S32 heading_deg = ll_round(mHeading) % 360;
    const std::string heading = mHeadingLabel + "  " + llformat("%03d", heading_deg) + "\xC2\xB0 " + pointName((F32)heading_deg);
    text(font, heading, text_x, y1, TEXT_CREAM, LLFontGL::LEFT);

    std::string course;
    if (mIsMoving)
    {
        const S32 course_deg = ll_round(mCourse) % 360;
        course = mCourseLabel + "  " + llformat("%03d", course_deg) + "\xC2\xB0 " + pointName((F32)course_deg);
    }
    else
    {
        course = mCourseLabel + "  " + mAtRestLabel;
    }
    text(font, course, text_x, y2, mIsMoving ? TEXT_CREAM : TEXT_DIM, LLFontGL::LEFT);
}
