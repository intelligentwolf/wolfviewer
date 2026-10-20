/**
 * @file llfloaterwolfavatars.cpp
 * @brief WolfViewer: Wolf World > WolfAvatars... - describe a character, pick one of four pictures, and a wearable rigged
 *        avatar arrives in your inventory.
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

#include "llfloaterwolfavatars.h"

#include <cmath>
#include <vector>

#include "apr_base64.h"
#include "llbutton.h"
#include "lldraghandle.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llimage.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdutil.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llui.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewerinventory.h"
#include "llviewertexture.h"
#include "llweb.h"
#include "wolfadventure.h"
#include "wolfgame.h"
#include "wolfgrid.h"

// [WOLFAVATARS 2026-10-20] Source for every call, field and word below: firestorm/region-terrain-work/character-builder/
// WOLFAVATARS_API.md and wolfstorm/php/wolfavatars_lib.php (cited by function). The service is reached through
// WolfAdventure::api, the Adventure Studio's helper (same identity headers, same off-Wolf refusal, same error words).

namespace
{
    // Source: WOLFAVATARS_API.md Flow 3 "Poll `job {job}` every 3 s"
    const F64 POLL_SEC = 3.0;
    // Source: wolfavatars_lib.php WCH_PROMPT_MAX 600 - checked with strlen (bytes) after control characters become spaces
    const size_t PROMPT_MAX_BYTES = 600;
    // Source: wolfavatars_lib.php wa_api_char_quote - "the name is at most 48 characters" (mb_strlen, UTF-8 characters)
    const size_t NAME_MAX_CHARS = 48;
    // Source: wolfavatars_lib.php wa_api_char_quote - "the height is from 0.3 to 4 metres"
    const F64 HEIGHT_MIN = 0.3, HEIGHT_MAX = 4.0;
    // Source: wolfavatars_lib.php WCH_PICTURES 4 (pictures in one round) and WCH_PIC_SIDE 512 (the JPEG's longest side). A
    // picture's base64 is refused above 4 MB and its sides above 2048 px: far beyond what the brain sends, never a huge allocation.
    const size_t PICTURE_B64_MAX = 4u * 1024u * 1024u;
    const S32 PICTURE_SIDE_MAX = 2048;
    // [CLOTHING 2026-10-20] Source: WOLFAVATARS_API.md Flow 1 "Garment types" - top, dress, trousers, skirt, shoes, hat, cape,
    // gown (the char_quote garment values, in the order the chips show them; the floater string garment_words in the same order)
    const char* const GARMENT_IDS[8] = { "top", "dress", "trousers", "skirt", "shoes", "hat", "cape", "gown" };

    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    // ---- palette: the moonlit den ----
    // Fixed colours, not the skin's: this is a painted scene (as the radio, wolffloaterradio.cpp:41 "no skin may lower them"),
    // and every line of text sits on a fixed plate drawn under it. Contrast (WCAG 2.x relative luminance) of each text pair:
    //   CREAM #f2ebd6 on PLATE #0f0d1f: 16.0:1; on SLATE (top #212438): 12.8:1; on the den floor (#120d21 at its lightest): 15:1
    //   MUTED #c7c7db on SLATE top: 9.2:1; on PLATE: 11.5:1
    //   ERR_INK #ffb385 on ERR_PLATE #2e0d0f: 10.2:1 (the error line, the form and picture errors, a picture that failed)
    //   AMBER #ffbd52 on PLATE: 11.5:1 (the current step's word, the stage words); on the chosen tile (top #573d24): 6.0:1
    //   SILVER #ccd4e6 on PLATE: 12.8:1 (a step done); DIM #9999b8 on PLATE: 6.9:1 (a step still to come)
    //   SEAL_INK #291405 on the seal's amber (top #ffd675 12.6:1, bottom #db8a29 6.4:1)
    //   CREAM on a moonstone button (top #3d4261 8.2:1, bottom #212438 12.8:1)
    //   INK #1f1208 on the brass price plaque (top #edcf8a 12.1:1, bottom #c2944d 6.7:1)
    //   a disabled button's DIS #b8b8cc on its dim stone (#333347): 6.3:1
    const LLColor4 CREAM(0.95f, 0.92f, 0.84f, 1.f);
    const LLColor4 MUTED(0.78f, 0.78f, 0.86f, 1.f);
    const LLColor4 ERR_INK(1.f, 0.70f, 0.52f, 1.f);
    const LLColor4 ERR_PLATE(0.18f, 0.05f, 0.06f, 1.f);
    const LLColor4 AMBER(1.f, 0.74f, 0.32f, 1.f);
    const LLColor4 SILVER(0.80f, 0.83f, 0.90f, 1.f);
    const LLColor4 DIM(0.60f, 0.60f, 0.72f, 1.f);
    const LLColor4 INK(0.12f, 0.07f, 0.03f, 1.f);

    const LLColor4 PLATE_TOP(0.08f, 0.07f, 0.15f, 1.f);
    const LLColor4 PLATE(0.06f, 0.05f, 0.12f, 1.f);
    const LLColor4 SLATE_TOP(0.13f, 0.14f, 0.22f, 1.f);
    const LLColor4 SLATE_BOTTOM(0.08f, 0.09f, 0.15f, 1.f);
    const LLColor4 SEAL_TOP(1.f, 0.84f, 0.46f, 1.f);
    const LLColor4 SEAL_BOTTOM(0.86f, 0.54f, 0.16f, 1.f);
    const LLColor4 SEAL_RIM(0.55f, 0.30f, 0.06f, 1.f);
    const LLColor4 STONE_TOP(0.24f, 0.26f, 0.38f, 1.f);
    const LLColor4 STONE_BOTTOM(0.13f, 0.14f, 0.22f, 1.f);
    const LLColor4 STONE_OFF(0.20f, 0.20f, 0.28f, 1.f);
    const LLColor4 BRASS_TOP(0.93f, 0.81f, 0.54f, 1.f);
    const LLColor4 BRASS_BOTTOM(0.76f, 0.58f, 0.30f, 1.f);
    const LLColor4 BRASS_DARK(0.40f, 0.27f, 0.10f, 1.f);
    const LLColor4 BRASS_LIGHT(0.97f, 0.86f, 0.55f, 1.f);
    const LLColor4 GOLD_LIGHT(1.f, 0.88f, 0.52f, 1.f);
    const LLColor4 GOLD(0.82f, 0.62f, 0.26f, 1.f);
    const LLColor4 GOLD_DARK(0.42f, 0.28f, 0.08f, 1.f);
    const LLColor4 TILE_TOP(0.16f, 0.17f, 0.28f, 1.f);
    const LLColor4 TILE_BOTTOM(0.09f, 0.09f, 0.16f, 1.f);
    const LLColor4 TILE_ON_TOP(0.34f, 0.24f, 0.14f, 1.f);
    const LLColor4 TILE_ON_BOTTOM(0.20f, 0.12f, 0.08f, 1.f);
    const LLColor4 MAT(0.05f, 0.04f, 0.10f, 1.f);

    const LLColor4 SKY_TOP(0.02f, 0.03f, 0.10f, 1.f);
    const LLColor4 SKY_MID(0.07f, 0.06f, 0.20f, 1.f);
    const LLColor4 SKY_HORIZON(0.20f, 0.11f, 0.32f, 1.f);
    const LLColor4 FLOOR_TOP(0.07f, 0.05f, 0.13f, 1.f);
    const LLColor4 FLOOR_BOTTOM(0.03f, 0.025f, 0.07f, 1.f);
    const LLColor4 MOUNT_FAR(0.13f, 0.10f, 0.25f, 1.f);
    const LLColor4 MOUNT_NEAR(0.07f, 0.05f, 0.14f, 1.f);
    const LLColor4 SILHOUETTE(0.025f, 0.02f, 0.06f, 1.f);
    const LLColor4 MOON_LIGHT(0.98f, 0.97f, 0.90f, 1.f);
    const LLColor4 MOON_RIM(0.84f, 0.85f, 0.80f, 1.f);
    const LLColor4 MOON_GLOW(0.70f, 0.76f, 1.f, 1.f);
    const LLColor4 VIOLET(0.45f, 0.30f, 0.80f, 1.f);
    const LLColor4 RING_STONE(0.10f, 0.09f, 0.18f, 1.f);
    const LLColor4 SPIRIT(0.82f, 0.86f, 0.98f, 1.f);       // a silhouette in the circle, moonlit
    const LLColor4 SHADOW(0.f, 0.f, 0.f, 0.40f);
    const LLColor4 CLEAR(0.f, 0.f, 0.f, 0.f);

    // ---- behaviour (design values, not protocol values) ----
    constexpr S32 STONES = 5;              // Describe, Pictures, Pick, Build, Done
    constexpr F32 RUNE_SPIN = 0.12f;       // radians per second the rune marks turn
    constexpr F32 WAIT_SPIN = 1.4f;        // the arc that turns while there is no % to show
    constexpr S32 SPARKS = 16;
    constexpr S32 MIN_BUTTON_FONT = 2;     // Avionics2 (8 pt): the smallest a button label is drawn in
    // a clothes chip, left to right: CHIP_PAD, the check badge (CHIP_BADGE across), CHIP_SPACE, the label, CHIP_SPACE,
    // "chosen" on the chosen one, CHIP_PAD
    constexpr S32 CHIP_PAD = 6;
    constexpr S32 CHIP_BADGE = 14;
    constexpr S32 CHIP_SPACE = 5;

    LLColor4 withAlpha(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    // Source: wolffloaterradio.cpp:105 lerpColour
    LLColor4 lerpColour(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[VRED] + (b.mV[VRED] - a.mV[VRED]) * t,
                        a.mV[VGREEN] + (b.mV[VGREEN] - a.mV[VGREEN]) * t,
                        a.mV[VBLUE] + (b.mV[VBLUE] - a.mV[VBLUE]) * t,
                        a.mV[VALPHA] + (b.mV[VALPHA] - a.mV[VALPHA]) * t);
    }

    LLColor4 lighten(const LLColor4& c, F32 amount)
    {
        return LLColor4(llclamp(c.mV[VRED] + amount, 0.f, 1.f), llclamp(c.mV[VGREEN] + amount, 0.f, 1.f),
                        llclamp(c.mV[VBLUE] + amount, 0.f, 1.f), c.mV[VALPHA]);
    }

    // Source: wolffloaterradio.cpp:119 hash01 - a repeatable pseudo-random 0..1, the same every frame
    F32 hash01(F32 n)
    {
        const F32 s = sinf(n * 12.9898f) * 43758.5453f;
        return s - floorf(s);
    }

    LLRect grown(const LLRect& r, S32 d)
    {
        return LLRect(r.mLeft - d, r.mTop + d, r.mRight + d, r.mBottom - d);
    }

    // ---- lettering: Cinzel Bold (WolfDisplay), as the radio and Tessa's Compass ----
    // Source: wolffloaterradio.cpp:129-151 displayFonts - the WolfDisplay family (fonts.xml, registered in every font set:
    // fonts/fonts*.xml "WolfDisplay") at the Avionics0..11 sizes (6..26 pt), smallest first.
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
                if (f) fonts.push_back(f);
            }
            if (fonts.empty())
            {
                LL_WARNS("WolfAvatars") << "font WolfDisplay missing, using the sans serif" << LL_ENDL;
                fonts.push_back(LLFontGL::getFontSansSerif());
            }
        }
        return fonts;
    }

    // Source: wolffloaterradio.cpp:155-179 fitFont - the largest size whose line fits px and whose widest string fits max_w
    // (the smallest from min_index when none does)
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
            if (fits) best = f;
        }
        return best;
    }

    // Source: wolffloaterradio.cpp:187-199 text - centred vertically on y; cut with an ellipsis if wider than max_w
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

    // Source: wolffloaterradio.cpp:202-207 engraved - a dark lip one pixel below, the colour on top
    void engraved(const LLFontGL* font, const std::string& s, F32 x, F32 y, const LLColor4& c,
                  LLFontGL::HAlign h = LLFontGL::HCENTER, F32 max_w = 1.e6f)
    {
        text(font, s, x + 0.5f, y - 1.f, LLColor4(0.f, 0.f, 0.f, 0.55f), h, max_w);
        text(font, s, x, y, c, h, max_w);
    }

    // ---- geometry: everything is triangles (wolffloaterradio.cpp:209-309) ----
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

    // A convex four-sided shape, its corners in order round the edge.
    void quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0, y0, c); vtx(x1, y1, c); vtx(x2, y2, c);
        vtx(x0, y0, c); vtx(x2, y2, c); vtx(x3, y3, c);
        gGL.end();
    }

    // An axis-aligned box, its colour running from top to bottom (wolffloaterradio.cpp:233 box)
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
        return llclamp((S32)(r * 0.8f), 12, 120);
    }

    // An ellipse, bright at the centre and its rim colour at the edge (a disc when rx == ry); a soft glow when rim is clear
    void ellipse(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& centre, const LLColor4& rim)
    {
        const S32 n = segmentsFor(llmax(rx, ry));
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n;
            const F32 a1 = F_TWO_PI * (i + 1) / n;
            vtx(cx, cy, centre);
            vtx(cx + rx * sinf(a0), cy + ry * cosf(a0), rim);
            vtx(cx + rx * sinf(a1), cy + ry * cosf(a1), rim);
        }
        gGL.end();
    }

    void disc(F32 cx, F32 cy, F32 r, const LLColor4& centre, const LLColor4& rim)
    {
        ellipse(cx, cy, r, r, centre, rim);
    }

    // The band r0..r1 between angles a0 and a1 (clockwise from up), its colour running from c0 at a0 to c1 at a1.
    void arcBand(F32 cx, F32 cy, F32 r0, F32 r1, F32 a0, F32 a1, const LLColor4& c0, const LLColor4& c1)
    {
        if (a1 <= a0) return;
        const S32 n = llmax(2, (S32)(segmentsFor(r1) * (a1 - a0) / F_TWO_PI) + 1);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 t0 = (F32)i / n, t1 = (F32)(i + 1) / n;
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

    void ring(F32 cx, F32 cy, F32 r0, F32 r1, const LLColor4& c)
    {
        arcBand(cx, cy, r0, r1, 0.f, F_TWO_PI, c, c);
    }

    // Source: wolffloaterradio.cpp:295 line - a line w px wide as two triangles
    void line(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    }

    // A limb: a line with round ends, so joints meet cleanly in a silhouette.
    void limb(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        line(x0, y0, x1, y1, w, c);
        disc(x0, y0, w * 0.5f, c, c);
        disc(x1, y1, w * 0.5f, c, c);
    }

    // A skyline: every point joined to the next and filled down to base (any outline, convex or not).
    void fillProfile(const std::vector<LLVector2>& pts, F32 base, const LLColor4& top, const LLColor4& bottom)
    {
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const LLVector2& a = pts[i];
            const LLVector2& b = pts[i + 1];
            vtx(a.mV[VX], base, bottom);
            vtx(b.mV[VX], b.mV[VY], top);
            vtx(a.mV[VX], a.mV[VY], top);
            vtx(a.mV[VX], base, bottom);
            vtx(b.mV[VX], base, bottom);
            vtx(b.mV[VX], b.mV[VY], top);
        }
        gGL.end();
    }

    // ---- rounded rectangles (wolffloaterradio.cpp:311-405) ----
    struct Radii
    {
        F32 tl, tr, br, bl;
    };

    Radii allCorners(F32 r)
    {
        return Radii{ r, r, r, r };
    }

    // Source: wolffloaterradio.cpp:324 roundedOutline - the outline of rect shrunk by inset, corners concentric
    std::vector<LLVector2> roundedOutline(const LLRect& rect, const Radii& rad, F32 inset)
    {
        constexpr S32 SEG = 10;
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

    // Source: wolffloaterradio.cpp:350 fillRounded - filled, its colour running from top to bottom
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

    // Source: wolffloaterradio.cpp:387 strokeRounded - the band between two concentric outlines, coloured top to bottom
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

    // A soft halo round rect: layers growing outwards, each fainter (the glow behind a lit tile, frame or seal).
    void halo(const LLRect& rect, F32 radius, S32 spread, const LLColor4& c)
    {
        constexpr S32 LAYERS = 4;
        for (S32 i = LAYERS; i >= 1; --i)
        {
            const S32 d = spread * i / LAYERS;
            fillRounded(grown(rect, d), allCorners(radius + d), withAlpha(c, 0.30f / i), withAlpha(c, 0.30f / i));
        }
    }

    // Source: wolffloaterradio.cpp:468 screw - a small domed screw head with its slot
    void screw(F32 x, F32 y, F32 r)
    {
        disc(x, y, r, BRASS_LIGHT, BRASS_DARK);
        line(x - r * 0.7f, y + r * 0.35f, x + r * 0.7f, y - r * 0.35f, llmax(1.f, r * 0.3f), withAlpha(BRASS_DARK, 0.9f));
    }

    // Source: wolffloaterradio.cpp:474 mouseOver
    bool mouseOver(LLView* v)
    {
        S32 x = 0, y = 0;
        LLUI::getInstance()->getMousePositionLocal(v, &x, &y);
        return v->pointInView(x, y);
    }

    // Source: wolffloaterradio.cpp:481 splitList - a comma separated floater string
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

    // ---- silhouettes, built from convex pieces in one opaque colour (so where they overlap nothing shows) ----

    /** A person standing, h px from the feet (base) to the top of the head, arms a little out (the T-pose's cousin). */
    void drawPerson(F32 cx, F32 base, F32 h, const LLColor4& c)
    {
        const F32 limb_w = h * 0.07f;
        disc(cx, base + h * 0.91f, h * 0.085f, c, c);                                   // head
        box(cx - h * 0.03f, base + h * 0.84f, cx + h * 0.03f, base + h * 0.78f, c, c);  // neck
        quad(cx - h * 0.14f, base + h * 0.79f, cx + h * 0.14f, base + h * 0.79f,        // torso, shoulders to hips
             cx + h * 0.10f, base + h * 0.49f, cx - h * 0.10f, base + h * 0.49f, c);
        disc(cx - h * 0.125f, base + h * 0.765f, h * 0.035f, c, c);                     // shoulders
        disc(cx + h * 0.125f, base + h * 0.765f, h * 0.035f, c, c);
        limb(cx - h * 0.13f, base + h * 0.76f, cx - h * 0.24f, base + h * 0.50f, limb_w * 0.85f, c);   // arms
        limb(cx + h * 0.13f, base + h * 0.76f, cx + h * 0.24f, base + h * 0.50f, limb_w * 0.85f, c);
        limb(cx - h * 0.06f, base + h * 0.50f, cx - h * 0.08f, base + h * 0.04f, limb_w, c);          // legs
        limb(cx + h * 0.06f, base + h * 0.50f, cx + h * 0.08f, base + h * 0.04f, limb_w, c);
        box(cx - h * 0.13f, base + h * 0.04f, cx - h * 0.04f, base, c, c);              // feet
        box(cx + h * 0.04f, base + h * 0.04f, cx + h * 0.13f, base, c, c);
    }

    /** [CLOTHING 2026-10-20] A shirt hanging on a coat hanger, h px from the hem (base) to the top of the hook, about 0.74 h
     *  across the sleeves: the hook and the hanger's arms in thin limbs, the shirt in convex pieces over them. */
    void drawShirtOnHanger(F32 cx, F32 base, F32 h, const LLColor4& c)
    {
        const F32 w = llmax(1.5f, h * 0.03f);           // the hanger's wire
        const F32 apex = base + h * 0.82f;              // where the hook meets the arms
        const F32 ys = base + h * 0.66f;                // the hanger's shoulder line
        limb(cx, apex, cx - h * 0.33f, ys, w, c);       // the hanger's arms
        limb(cx, apex, cx + h * 0.33f, ys, w, c);
        limb(cx, apex, cx, base + h * 0.87f, w, c);     // the hook: a short stem, then curling over to the right and down
        const F32 hr = h * 0.045f;
        arcBand(cx + hr, base + h * 0.87f, hr - w * 0.5f, hr + w * 0.5f, -F_PI * 0.5f, F_PI * 0.8f, c, c);
        // the shirt: the yoke from the collar to the shoulders, the body down to the hem, a sleeve each side
        quad(cx - h * 0.08f, ys + h * 0.10f, cx + h * 0.08f, ys + h * 0.10f,
             cx + h * 0.21f, ys + h * 0.04f, cx - h * 0.21f, ys + h * 0.04f, c);
        quad(cx - h * 0.21f, ys + h * 0.04f, cx + h * 0.21f, ys + h * 0.04f,
             cx + h * 0.19f, base, cx - h * 0.19f, base, c);
        quad(cx - h * 0.21f, ys + h * 0.04f, cx - h * 0.20f, ys - h * 0.12f,
             cx - h * 0.30f, ys - h * 0.20f, cx - h * 0.37f, ys - h * 0.09f, c);
        quad(cx + h * 0.21f, ys + h * 0.04f, cx + h * 0.20f, ys - h * 0.12f,
             cx + h * 0.30f, ys - h * 0.20f, cx + h * 0.37f, ys - h * 0.09f, c);
    }

    /** A wolf standing side-on, facing right; u is its length nose to tail (about 0.62 u tall to the ear tips). */
    void drawWolf(F32 cx, F32 base, F32 u, const LLColor4& c)
    {
        const F32 leg_w = u * 0.05f;
        ellipse(cx - u * 0.03f, base + u * 0.36f, u * 0.25f, u * 0.10f, c, c);         // body
        disc(cx + u * 0.17f, base + u * 0.37f, u * 0.115f, c, c);                       // chest
        quad(cx + u * 0.12f, base + u * 0.44f, cx + u * 0.27f, base + u * 0.40f,        // neck
             cx + u * 0.37f, base + u * 0.52f, cx + u * 0.27f, base + u * 0.60f, c);
        disc(cx + u * 0.33f, base + u * 0.55f, u * 0.07f, c, c);                        // head
        quad(cx + u * 0.35f, base + u * 0.60f, cx + u * 0.50f, base + u * 0.53f,        // muzzle
             cx + u * 0.50f, base + u * 0.505f, cx + u * 0.36f, base + u * 0.49f, c);
        tri(cx + u * 0.285f, base + u * 0.59f, cx + u * 0.31f, base + u * 0.68f, cx + u * 0.345f, base + u * 0.60f, c);   // ear
        limb(cx + u * 0.19f, base + u * 0.33f, cx + u * 0.21f, base + u * 0.02f, leg_w, c);           // forelegs
        limb(cx + u * 0.11f, base + u * 0.31f, cx + u * 0.10f, base + u * 0.02f, leg_w, c);
        limb(cx - u * 0.18f, base + u * 0.34f, cx - u * 0.23f, base + u * 0.15f, leg_w * 1.2f, c);   // hind legs: thigh,
        limb(cx - u * 0.23f, base + u * 0.15f, cx - u * 0.20f, base + u * 0.02f, leg_w, c);          // then the bent hock
        limb(cx - u * 0.10f, base + u * 0.32f, cx - u * 0.14f, base + u * 0.15f, leg_w * 1.2f, c);
        limb(cx - u * 0.14f, base + u * 0.15f, cx - u * 0.11f, base + u * 0.02f, leg_w, c);
        quad(cx - u * 0.25f, base + u * 0.42f, cx - u * 0.24f, base + u * 0.36f,        // tail, hanging in a curve
             cx - u * 0.36f, base + u * 0.24f, cx - u * 0.38f, base + u * 0.32f, c);
        quad(cx - u * 0.38f, base + u * 0.32f, cx - u * 0.36f, base + u * 0.24f,
             cx - u * 0.45f, base + u * 0.17f, cx - u * 0.44f, base + u * 0.24f, c);
    }

    /** A wolf sitting with its head thrown back, howling up to the right; u is its height from the ground to the nose. */
    void drawHowlingWolf(F32 x, F32 base, F32 u, const LLColor4& c)
    {
        ellipse(x - u * 0.06f, base + u * 0.20f, u * 0.20f, u * 0.20f, c, c);          // haunch
        quad(x - u * 0.16f, base + u * 0.28f, x + u * 0.12f, base + u * 0.22f,          // chest, leaning up
             x + u * 0.17f, base + u * 0.60f, x - u * 0.02f, base + u * 0.68f, c);
        limb(x + u * 0.10f, base + u * 0.40f, x + u * 0.14f, base + u * 0.02f, u * 0.065f, c);   // foreleg
        limb(x + u * 0.03f, base + u * 0.38f, x + u * 0.06f, base + u * 0.02f, u * 0.06f, c);
        quad(x + u * 0.00f, base + u * 0.62f, x + u * 0.16f, base + u * 0.56f,          // neck
             x + u * 0.24f, base + u * 0.82f, x + u * 0.10f, base + u * 0.88f, c);
        disc(x + u * 0.17f, base + u * 0.86f, u * 0.075f, c, c);                         // head
        quad(x + u * 0.15f, base + u * 0.92f, x + u * 0.22f, base + u * 0.82f,          // muzzle, pointing at the moon
             x + u * 0.34f, base + u * 0.97f, x + u * 0.31f, base + u * 1.00f, c);
        tri(x + u * 0.09f, base + u * 0.88f, x + u * 0.06f, base + u * 0.99f, x + u * 0.15f, base + u * 0.92f, c);   // ear
        quad(x - u * 0.20f, base + u * 0.08f, x - u * 0.18f, base,                      // tail along the rock
             x - u * 0.46f, base, x - u * 0.44f, base + u * 0.05f, c);
    }

    /** A gilt picture frame round rect (fw px of moulding), with a dark mat inside it. */
    void giltFrame(const LLRect& r, F32 fw)
    {
        LLRect shadow = r;
        shadow.translate(3, -4);
        fillRounded(shadow, allCorners(4.f), SHADOW, SHADOW);
        fillRounded(r, allCorners(4.f), GOLD, GOLD_DARK);
        strokeRounded(r, allCorners(4.f), 0.f, 1.5f, GOLD_DARK, GOLD_DARK);
        strokeRounded(r, allCorners(4.f), 1.5f, fw * 0.45f, GOLD_LIGHT, GOLD);                // the lit outer moulding
        strokeRounded(r, allCorners(4.f), fw * 0.45f, fw * 0.6f, GOLD_DARK, GOLD_DARK);       // a groove
        strokeRounded(r, allCorners(4.f), fw * 0.6f, fw, GOLD, GOLD_LIGHT);                   // the inner bead, lit from below
        const LLRect mat(r.mLeft + (S32)fw, r.mTop - (S32)fw, r.mRight - (S32)fw, r.mBottom + (S32)fw);
        fillRounded(mat, allCorners(2.f), MAT, MAT);
    }

    /** A dark plate with a silver bead: the ground every line of light text sits on. */
    void plate(const LLRect& r, F32 radius, const LLColor4& rim)
    {
        LLRect shadow = r;
        shadow.translate(2, -3);
        fillRounded(shadow, allCorners(radius), SHADOW, SHADOW);
        fillRounded(r, allCorners(radius), PLATE_TOP, PLATE);
        strokeRounded(r, allCorners(radius), 0.f, 1.5f, rim, withAlpha(rim, 0.6f));
    }

    /** A check mark (a done step, a chosen tile, a picked picture): the state in a shape as well as a colour. */
    void checkMark(F32 cx, F32 cy, F32 s, F32 w, const LLColor4& c)
    {
        limb(cx - s * 0.45f, cy + s * 0.02f, cx - s * 0.12f, cy - s * 0.30f, w, c);
        limb(cx - s * 0.12f, cy - s * 0.30f, cx + s * 0.48f, cy + s * 0.34f, w, c);
    }

    /** The current time for animation, wrapped so it stays precise in F32. Source: llframetimer.h:46 getElapsedSeconds
     *  (sFrameTime, seconds since the viewer started, updated once a frame). */
    F32 animSeconds()
    {
        return (F32)fmod((F64)LLFrameTimer::getElapsedSeconds().value(), 3600.0);
    }

    /** The percent in a progress line, or -1. Source: wolfavatars_lib.php wch_wait - progress is "<stage> (<n>%)" while a
     *  provider task runs ("Drawing pictures (40%)", "Making the body (63%)", "Fitting the skeleton (20%)"); the other lines
     *  ("Waiting to start", "Drawing pictures", "Putting it in your inventory") carry none. */
    S32 progressPercent(const std::string& progress)
    {
        const size_t close = progress.rfind("%)");
        if (close == std::string::npos) return -1;
        const size_t open = progress.rfind('(', close);
        if (open == std::string::npos || open + 1 >= close) return -1;
        S32 pct = 0;
        for (size_t i = open + 1; i < close; ++i)
        {
            if (progress[i] < '0' || progress[i] > '9') return -1;
            pct = pct * 10 + (progress[i] - '0');
            if (pct > 100) return -1;
        }
        return pct;
    }

    /** Characters in a UTF-8 string (bytes that do not continue a sequence). */
    size_t utf8Chars(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    /** Source: wolfavatars_lib.php wa_api_char_quote - control characters become one space, then the words are trimmed. */
    std::string cleanWords(const std::string& in)
    {
        std::string out;
        bool space = false;
        for (unsigned char c : in)
        {
            if (c < 0x20 || c == 0x7F) { if (!space) out += ' '; space = true; continue; }
            space = false;
            out += (char)c;
        }
        LLStringUtil::trim(out);
        return out;
    }

    /** Source: wolfimportflow.cpp creditsShort (the brain's refusals, wolfavatars_lib.php wch_need_credits): only code
     *  credits_short carries the buy link (buyurl = WM_BUY_URL); credits_unread has none. */
    std::string buyUrlOf(const LLSD& reply)
    {
        if (!reply.isMap() || reply["code"].asString() != "credits_short") return std::string();
        return reply["buyurl"].asString();
    }

    /**
     * char_picture's data -> a texture. Source: wolfavatars_lib.php wa_api_char_picture {mime: "image/jpeg", data: base64}
     * (wch_picture_jpeg: PHP base64_encode of imagejpeg). The base64 is read with apr_base64_decode_binary as fslslbridge.cpp
     * getScriptInfo does (LLBase64::decodeAsString stops at the first zero byte, so it cannot carry a JPEG); the JPEG with
     * LLImageFormatted::loadFromMemory + decode as LLViewerTextureList::getRawImageFromMemory does; the texture with
     * LLViewerTextureManager::getLocalTexture as wolfai.cpp WolfPanelAI::loadPresentation does for its PNG previews.
     */
    bool pictureTexture(const std::string& mime, const std::string& b64, LLPointer<LLViewerTexture>& out, std::string& why)
    {
        out = nullptr;
        if (mime != "image/jpeg") { why = "the picture came in a format WolfViewer does not show (" + cap(mime, 40) + ")"; return false; }
        if (b64.empty() || b64.size() > PICTURE_B64_MAX) { why = "the picture was empty or too large"; return false; }
        const int max_len = apr_base64_decode_len(b64.c_str());
        if (max_len <= 0) { why = "the picture could not be read"; return false; }
        std::vector<U8> bytes((size_t)max_len);
        const int len = apr_base64_decode_binary(bytes.data(), b64.c_str());
        if (len <= 0 || len > max_len) { why = "the picture could not be read"; return false; }
        bytes.resize((size_t)len);
        LLPointer<LLImageFormatted> img = LLImageFormatted::loadFromMemory(bytes.data(), (U32)bytes.size(), mime);
        if (img.isNull()) { why = "the picture is not a JPEG WolfViewer can read"; return false; }
        if (img->getWidth() <= 0 || img->getHeight() <= 0 || img->getWidth() > PICTURE_SIDE_MAX || img->getHeight() > PICTURE_SIDE_MAX)
        {
            why = "the picture has an unexpected size";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!img->decode(raw, 0.f) || raw->isBufferInvalid() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        {
            why = "the picture could not be decoded";
            return false;
        }
        out = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        if (out.isNull()) { why = "the picture could not be put on screen"; return false; }
        return true;
    }

    /** The picture scaled to fit inside rect, centred (as wolfai.cpp WolfAIPreview::draw: gl_draw_scaled_image). */
    void drawPictureIn(const LLRect& rect, LLViewerTexture* tex)
    {
        if (!tex || tex->getWidth() <= 0 || tex->getHeight() <= 0 || rect.getWidth() <= 0 || rect.getHeight() <= 0) return;
        const F32 scale = llmin((F32)rect.getWidth() / tex->getWidth(), (F32)rect.getHeight() / tex->getHeight());
        const S32 tw = ll_round(tex->getWidth() * scale), th = ll_round(tex->getHeight() * scale);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gl_draw_scaled_image(rect.getCenterX() - tw / 2, rect.getCenterY() - th / 2, tw, th, tex);
        untextured();
    }
}

/**
 * One of the four pictures, in a gilt frame: the picture scaled to fit, its number on a small plaque (with a check mark and
 * "picked" when it is the one picked), the words while it loads or why it did not. A click (or Enter / Space with the
 * keyboard on it: a tab stop) picks it. The amber halo and the beam to the circle are drawn by the den behind it.
 */
class WolfAvatarPicture : public LLUICtrl
{
public:
    enum EState { S_EMPTY = 0, S_LOADING, S_SHOWN, S_FAILED };

    explicit WolfAvatarPicture(const LLUICtrl::Params& p) : LLUICtrl(p) {}

    std::function<void()> mOnPick;
    LLPointer<LLViewerTexture> mTexture;
    EState mState = S_EMPTY;
    std::string mWhy;
    bool mSelected = false;
    S32 mNumber = 1;                    // 1..4, on its plaque
    std::string mPickedWord;            // "picked" (floater string)

    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        if (!getEnabled()) return true;
        setFocus(true);
        if (mOnPick) mOnPick();
        return true;
    }
    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (mask == MASK_NONE && (key == KEY_RETURN || key == ' '))
        {
            if (getEnabled() && mOnPick) mOnPick();
            return true;
        }
        return LLUICtrl::handleKeyHere(key, mask);
    }
    void draw() override
    {
        const S32 w = getRect().getWidth(), h = getRect().getHeight();
        if (w < 24 || h < 24) return;
        LLGLSUIDefault ui;
        untextured();
        const F32 fw = llclamp(llmin(w, h) * 0.055f, 5.f, 10.f);
        const LLRect outer(0, h, w, 0);
        giltFrame(outer, fw);
        const LLRect mat(outer.mLeft + (S32)fw + 2, outer.mTop - (S32)fw - 2, outer.mRight - (S32)fw - 2, outer.mBottom + (S32)fw + 2);
        const F32 t = animSeconds();
        if (mState == S_SHOWN && mTexture.notNull())
        {
            drawPictureIn(mat, mTexture.get());
        }
        else if (mState == S_LOADING || mState == S_FAILED)
        {
            const bool failed = mState == S_FAILED;
            if (!failed)
            {
                // a small turning arc in the mat while it loads (the words say it too)
                const F32 cx = (F32)mat.getCenterX(), cy = mat.mBottom + mat.getHeight() * 0.66f, r = llmin(mat.getWidth(), mat.getHeight()) * 0.12f;
                const F32 a = t * WAIT_SPIN * 1.5f;
                arcBand(cx, cy, r * 0.75f, r, a, a + 2.2f, withAlpha(AMBER, 0.f), AMBER);
            }
            const std::string words = failed ? std::string("Not shown: ") + mWhy + ". Click to try again."
                                             : std::string("Loading the picture...");
            const LLColor4 ink = failed ? ERR_INK : CREAM;   // on MAT: ERR_INK 10:1, CREAM 16:1 (MAT is as dark as PLATE)
            LLFontGL::getFontSansSerifSmall()->renderUTF8(words, 0, (F32)mat.mLeft + 6.f, failed ? (F32)mat.getCenterY() : mat.mBottom + mat.getHeight() * 0.36f,
                                                         ink, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                                         S32_MAX, mat.getWidth() - 12, nullptr, true);
        }
        // the number plaque, top left over the moulding; the picked one says so in a word and a check mark, not only amber
        {
            const LLFontGL* f = fitFont(llclamp(h * 0.11f, 12.f, 20.f), (F32)w * 0.6f, { "4", mPickedWord }, 1);
            const std::string num = llformat("%d", mNumber);
            const F32 lh = (F32)f->getLineHeight();
            const F32 pw = mSelected ? f->getWidthF32(num) + f->getWidthF32(mPickedWord) + lh * 1.9f : lh * 1.5f;
            const LLRect pr(4, h - 4, 4 + (S32)pw, h - 4 - (S32)(lh + 6.f));
            untextured();
            fillRounded(pr, allCorners(4.f), mSelected ? lighten(AMBER, 0.08f) : PLATE_TOP, mSelected ? AMBER : PLATE);
            strokeRounded(pr, allCorners(4.f), 0.f, 1.f, GOLD_LIGHT, GOLD_DARK);
            const F32 cy = (F32)pr.getCenterY();
            if (mSelected)
            {
                // INK on AMBER: 11:1
                text(f, num, pr.mLeft + lh * 0.45f, cy, INK, LLFontGL::LEFT);
                untextured();
                checkMark(pr.mLeft + lh * 0.45f + f->getWidthF32(num) + lh * 0.5f, cy, lh * 0.55f, llmax(1.5f, lh * 0.12f), INK);
                text(f, mPickedWord, pr.mLeft + lh * 0.45f + f->getWidthF32(num) + lh * 0.95f, cy, INK, LLFontGL::LEFT);
            }
            else
            {
                text(f, num, (F32)pr.getCenterX(), cy, CREAM, LLFontGL::HCENTER);
            }
        }
        untextured();
        if (mSelected)
        {
            strokeRounded(LLRect((S32)fw - 1, h - (S32)fw + 1, w - (S32)fw + 1, (S32)fw - 1), allCorners(3.f), 0.f, 2.5f, AMBER, AMBER);
        }
        else if (hasFocus())
        {
            strokeRounded(LLRect((S32)fw - 1, h - (S32)fw + 1, w - (S32)fw + 1, (S32)fw - 1), allCorners(3.f), 0.f, 1.5f, SILVER, SILVER);
        }
        else if (getEnabled() && mState == S_SHOWN && mouseOver(this))
        {
            strokeRounded(LLRect((S32)fw - 1, h - (S32)fw + 1, w - (S32)fw + 1, (S32)fw - 1), allCorners(3.f), 0.f, 1.5f, withAlpha(AMBER, 0.6f), withAlpha(AMBER, 0.6f));
        }
        LLUICtrl::draw();
    }
};

/**
 * The view behind every control that paints the den. It covers the whole floater (so its coordinates are the floater's),
 * sits just in front of the floater's drag handle and behind everything else, and is not mouse-opaque: clicks fall through
 * to the drag handle, so the window still drags by its sky. Source: wolffloaterradio.cpp:499-591 WolfRadioBackdrop.
 */
class WolfAvatarsBackdrop : public LLView
{
public:
    // Its own Params, never shared with another widget (it is created in code, not registered).
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };

    WolfAvatarsBackdrop(const Params& p, LLFloaterWolfAvatars* owner) : LLView(p), mOwner(owner) {}

    void draw() override
    {
        mOwner->drawDen();
    }

private:
    LLFloaterWolfAvatars* mOwner;
};

LLFloaterWolfAvatars::LLFloaterWolfAvatars(const LLSD& key)
:   LLFloater(key)
{
}

LLFloaterWolfAvatars::~LLFloaterWolfAvatars()
{
}

bool LLFloaterWolfAvatars::postBuild()
{
    static const char* PANELS[6] = { "loading_panel", "describe_panel", "price_panel", "working_panel", "pictures_panel", "done_panel" };
    for (S32 i = 0; i < 6; ++i) mPanels[i] = getChild<LLPanel>(PANELS[i]);

    // Person / Animal: two drawn tiles (toggle buttons, so the keyboard reaches them), one always chosen
    mPersonBtn = getChild<LLButton>("person_btn");
    mPersonBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setKind(KIND_PERSON); });
    mAnimalBtn = getChild<LLButton>("animal_btn");
    mAnimalBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setKind(KIND_ANIMAL); });
    // [BASE BODY 2026-10-20] people only: dressed as described / base body, two drawn chips (toggle buttons, as the tiles)
    mDressedBtn = getChild<LLButton>("dressed_btn");
    mDressedBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setClothes(false); });
    mBaseBtn = getChild<LLButton>("base_btn");
    mBaseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setClothes(true); });
    mClothesHint = getChild<LLTextBox>("clothes_hint");
    // [CLOTHING 2026-10-20] the third tile, and the garment chips (toggle buttons, as the tiles; one always chosen)
    mClothingBtn = getChild<LLButton>("clothing_btn");
    mClothingBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setKind(KIND_CLOTHING); });
    for (S32 i = 0; i < GARMENTS; ++i)
    {
        mGarmentBtn[i] = getChild<LLButton>(llformat("garment_%s_btn", GARMENT_IDS[i]));
        mGarmentBtn[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { setGarment(i); });
    }
    mGarmentLabel = getChild<LLTextBox>("garment_label");
    mGarmentHint = getChild<LLTextBox>("garment_hint");
    // [FIGURE 2026-10-20] Woman / Man
    mFigureLabel = getChild<LLTextBox>("figure_label");
    mWomanBtn = getChild<LLButton>("woman_btn");
    mWomanBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setFigure(false); });
    mManBtn = getChild<LLButton>("man_btn");
    mManBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { setFigure(true); });
    mLoadingText = getChild<LLTextBox>("loading_text");
    mIntro = getChild<LLTextBox>("intro");
    mKindLabel = getChild<LLTextBox>("kind_label");
    mNameLabel = getChild<LLTextBox>("name_label");
    mPromptLabel = getChild<LLTextBox>("prompt_label");
    mHeightLabel = getChild<LLTextBox>("height_label");
    mTips = getChild<LLTextBox>("tips");
    mPriceTitle = getChild<LLTextBox>("price_title");
    mPriceHow = getChild<LLTextBox>("price_how");
    mWorkNote = getChild<LLTextBox>("work_note");
    mDoneTitle = getChild<LLTextBox>("done_title");
    mDoneNote = getChild<LLTextBox>("done_note");
    mStopText = getChild<LLTextBox>("stop_text");
    mName = getChild<LLLineEditor>("name");
    mPrompt = getChild<LLTextEditor>("prompt");
    mHeight = getChild<LLLineEditor>("height");
    mHeightNote = getChild<LLTextBox>("height_note");
    mFormError = getChild<LLTextBox>("form_error");
    mPriceBtn = getChild<LLButton>("price_btn");
    mPriceBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGetPrice(); });

    mPriceText = getChild<LLTextBox>("price_text");
    mBackBtn = getChild<LLButton>("back_btn");
    mBackBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBack(); });
    mStartBtn = getChild<LLButton>("start_btn");
    mStartBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStart(); });

    mWorkTitle = getChild<LLTextBox>("work_title");
    mWorkWords = getChild<LLTextBox>("work_words");
    mWorkStop = getChild<LLButton>("work_stop_btn");
    mWorkStop->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAskStop(); });

    mPicTitle = getChild<LLTextBox>("pic_title");
    mPicError = getChild<LLTextBox>("pic_error");
    for (S32 i = 0; i < PICTURES; ++i)
    {
        // Source: wolfai.cpp WolfPanelAI::postBuild - the drawn child is made with LLUICtrlFactory::create and put in a
        // placeholder panel of the layout, filling it
        LLPanel* slot = getChild<LLPanel>(llformat("pic_%d", i));
        mPicSlot[i] = slot;
        LLUICtrl::Params pp;
        pp.name = llformat("pic_view_%d", i);
        pp.rect = slot->getLocalRect();
        pp.follows.flags = FOLLOWS_ALL;
        pp.tab_stop = true;
        pp.mouse_opaque = true;
        pp.tool_tip = llformat("Picture %d: click to pick it", i + 1);
        mPic[i] = LLUICtrlFactory::create<WolfAvatarPicture>(pp);
        slot->addChild(mPic[i]);
        const S32 n = i;
        mPic[i]->mOnPick = [this, n]() { onPictureClicked(n); };
        mPic[i]->mNumber = i + 1;
        mPic[i]->mPickedWord = getString("picked");
    }
    mPickBtn = getChild<LLButton>("pick_btn");
    mPickBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPick(); });
    mMoreBtn = getChild<LLButton>("more_btn");
    mMoreBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMore(); });
    mPicStop = getChild<LLButton>("pic_stop_btn");
    mPicStop->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAskStop(); });

    mReport = getChild<LLTextEditor>("report");
    mInvBtn = getChild<LLButton>("inventory_btn");
    mInvBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOpenInventory(); });
    mAnotherBtn = getChild<LLButton>("another_btn");
    mAnotherBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMakeAnother(); });

    mError = getChild<LLTextBox>("error");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mBuyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuyCredits(); });
    mStopPanel = getChild<LLPanel>("stop_panel");
    mStopYes = getChild<LLButton>("stop_yes_btn");
    mStopYes->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStopAnswer(true); });
    mStopNo = getChild<LLButton>("stop_no_btn");
    mStopNo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStopAnswer(false); });

    // the XUI's words for a person or an animal, put back when Clothing is let go of (onKindChanged)
    mIntroText = mIntro->getText();
    mPromptLabelText = mPromptLabel->getText();
    mPromptTip = mPrompt->getToolTip();
    mTipsText = mTips->getText();
    mDoneTitleText = mDoneTitle->getText();

    // The painted den goes behind every control, then the drag handle behind it, so empty sky drags the window.
    // Source: wolffloaterradio.cpp:630-643 (llfloater.cpp layoutDragHandle(): the drag handle covers the whole floater).
    WolfAvatarsBackdrop::Params bp;
    bp.name("wolfavatars_backdrop");
    bp.rect(getLocalRect());
    bp.follows.flags(FOLLOWS_ALL);
    bp.mouse_opaque(false);
    mBackdrop = new WolfAvatarsBackdrop(bp, this);
    addChild(mBackdrop);
    sendChildToBack(mBackdrop);
    if (getDragHandle()) sendChildToBack(getDragHandle());

    onKindChanged();
    showStep(STEP_LOADING);
    return true;
}

void LLFloaterWolfAvatars::reshape(S32 width, S32 height, bool called_from_parent)
{
    // Source: wolffloaterradio.cpp:671 reshape - the drawing and every control follow the window's size
    LLFloater::reshape(width, height, called_from_parent);
    layoutWidgets();
}

void LLFloaterWolfAvatars::setKind(EKind kind)
{
    mKind = kind;
    onKindChanged();
}

void LLFloaterWolfAvatars::setClothes(bool base)
{
    mBase = base;
    refreshClothes();
    layoutWidgets();   // Woman / Man comes and goes with Base body, and takes its room from the description
}

void LLFloaterWolfAvatars::setGarment(S32 n)
{
    mGarment = llclamp(n, 0, GARMENTS - 1);
    refreshClothes();
}

void LLFloaterWolfAvatars::setFigure(bool male)
{
    mMale = male;
    refreshClothes();
}

void LLFloaterWolfAvatars::refreshClothes()
{
    // Source: WOLFAVATARS_API.md Flow 1 - clothes is for people only (wolfavatars_lib.php wa_api_char_quote: an animal is
    // always "dressed"), so the choice shows only while Person is chosen. One chip is always pressed in, as the tiles.
    const bool person = isPerson();
    mDressedBtn->setToggleState(!mBase);
    mBaseBtn->setToggleState(mBase);
    mDressedBtn->setVisible(person);
    mBaseBtn->setVisible(person);
    mClothesHint->setVisible(person);
    // [CLOTHING 2026-10-20] Flow 1 "Clothing" - a garment is required, and there is no height and no clothes choice
    const bool clothing = isClothing();
    for (S32 i = 0; i < GARMENTS; ++i)
    {
        mGarmentBtn[i]->setToggleState(i == mGarment);
        mGarmentBtn[i]->setVisible(clothing);
    }
    // the Garment line names the chosen one, with what it covers where the floater has words for that (Top, Trousers)
    const std::vector<std::string> garments = splitList(getString("garment_words"));
    const std::string word = mGarment < (S32)garments.size() ? garments[mGarment] : std::string(GARMENT_IDS[mGarment]);
    const std::string detail_name = std::string("garment_detail_") + GARMENT_IDS[mGarment];
    mGarmentLabel->setText(getString("garment_label") + ": " + word + (hasString(detail_name) ? " (" + getString(detail_name) + ")" : std::string()));
    mGarmentLabel->setVisible(clothing);
    mGarmentHint->setVisible(clothing);
    mHeightLabel->setVisible(!clothing);
    mHeight->setVisible(!clothing);
    mHeightNote->setVisible(!clothing);
    // [FIGURE 2026-10-20] Flow 1 "Figure" - a base body's standard ("Figure") or the body clothing is made for ("Made for");
    // ignored for animals and dressed people, so it is not shown for them
    const bool figure = figureShown();
    mFigureLabel->setText(getString(clothing ? "made_for_label" : "figure_label"));
    mWomanBtn->setToggleState(!mMale);
    mManBtn->setToggleState(mMale);
    mFigureLabel->setVisible(figure);
    mWomanBtn->setVisible(figure);
    mManBtn->setVisible(figure);
}

void LLFloaterWolfAvatars::onOpen(const LLSD& key)
{
    // Wolf Territories only (paul-wolf-features-wolf-grid-only; WOLFAVATARS_API.md "Wolf-grid only"): the menu item is hidden
    // elsewhere, and this closes the window if it is opened another way. Source: wolffloaterimportworlds.cpp onOpen.
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    // a job already on screen (the window was only hidden) carries on; otherwise look for one to take up again
    if (mJob.empty() && mStep != STEP_DESCRIBE) resume();
}

void LLFloaterWolfAvatars::onClose(bool app_quitting)
{
    // A price asked for and not started is dropped: nothing was charged (wolfmind_lib.php wa_api_job_cancel: quoting ->
    // cancelled), as the Studio's "No" does (wolffloateradventurestudioedit.cpp meshNo). Anything started carries on
    // without the window (the brain's worker runs it) and is taken up again when the window opens (resume).
    if (!app_quitting && mStep == STEP_PRICE && !mJob.empty())
    {
        LLSD body;
        body["job"] = mJob;
        WolfAdventure::api("job_cancel", body, nullptr);
        mJob.clear();
        ++mSerial;
        showStep(STEP_DESCRIBE);
    }
    mConfirmStop = false;
    // a window opened again finds what is going on by asking (resume) rather than trusting what it last showed
    if (!app_quitting && (mStep == STEP_WORKING || mStep == STEP_PICTURES || mStep == STEP_LOADING))
    {
        mJob.clear();
        ++mSerial;
        mStep = STEP_LOADING;
    }
}

void LLFloaterWolfAvatars::draw()
{
    if (mStep == STEP_WORKING && !mPollOut && !mJob.empty() && LLFrameTimer::getTotalSeconds() >= mNextPoll) poll();
    LLFloater::draw();
}

// ─── steps and messages ─────────────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::showStep(EStep step)
{
    mStep = step;
    for (S32 i = 0; i < 6; ++i) mPanels[i]->setVisible(i == (S32)step);
    mStopPanel->setVisible(mConfirmStop && (step == STEP_WORKING || step == STEP_PICTURES));
    setDefaultBtn(step == STEP_PRICE ? mStartBtn : step == STEP_PICTURES ? mPickBtn : step == STEP_DONE ? mInvBtn : mPriceBtn);
    layoutWidgets();   // each step has its own arrangement (and the Stop question takes room from it)
}

void LLFloaterWolfAvatars::setError(const std::string& error, const LLSD& reply)
{
    // Every failure shows here, red-amber on its own dark plate, and stays until the next action (WOLFAVATARS_API.md "Every
    // failure visible in the window"); a credits_short refusal keeps its Buy credits link (Flow 2)
    mError->setText(error);
    mError->setColor(ERR_INK);
    mBuyUrl = buyUrlOf(reply);
    mBuyBtn->setVisible(!mBuyUrl.empty());
    layoutWidgets();   // the line makes room for Buy credits... when it shows
}

void LLFloaterWolfAvatars::clearError()
{
    mError->setText(LLStringUtil::null);
    mBuyUrl.clear();
    mBuyBtn->setVisible(false);
    mFormError->setText(LLStringUtil::null);
    layoutWidgets();
}

void LLFloaterWolfAvatars::setBusy(bool busy)
{
    mBusy = busy;
    mPriceBtn->setEnabled(!busy);
    mStartBtn->setEnabled(!busy);
    mBackBtn->setEnabled(!busy);
    mWorkStop->setEnabled(!busy);
    mPicStop->setEnabled(!busy);
    mAnotherBtn->setEnabled(!busy);
    refreshPictures();
}

void LLFloaterWolfAvatars::onKindChanged()
{
    // Source: WOLFAVATARS_API.md Flow 1 - height defaults to 1.8 for a person; an animal with no height uses the model's own
    // estimated size (wolfavatars_lib.php wa_api_char_quote: person + no height -> 1.8; wch_build: auto_size for an animal)
    const bool person = isPerson();
    const bool clothing = isClothing();
    // the three tiles show the choice: one is always pressed in (a toggle button pressed again would let go of it)
    mPersonBtn->setToggleState(person);
    mAnimalBtn->setToggleState(mKind == KIND_ANIMAL);
    mClothingBtn->setToggleState(clothing);
    // [HEIGHT 2026-10-20] no 1.8 filled in: 1.8 m to the top of the model counted ears, horns and hats as height and made
    // animal-headed people short (Paul's Wolf King). Left empty, the brain stands a person's head where a standard SL
    // avatar's is (build_avatar.py, sl_skeleton.json mHead 1.683).
    if (mHeight->getText() == "1.8") mHeight->setText(LLStringUtil::null);
    mHeightNote->setText(person ? std::string("Optional. Metres, 0.3 to 4. Left empty, as tall as a standard avatar (ears and hats not counted).")
                                : std::string("Optional. Metres, 0.3 to 4. Left empty, the animal keeps its own size."));
    // the name the brain gives one left empty (wolfavatars_lib.php wa_api_char_quote: My character / My animal / My clothes)
    mName->setLabel(person ? std::string("My character") : clothing ? getString("name_clothing") : std::string("My animal"));
    // [CLOTHING 2026-10-20] a garment's own words in place of a character's
    mIntro->setText(clothing ? getString("intro_clothing") : mIntroText);
    mPromptLabel->setText(clothing ? getString("prompt_clothing") : mPromptLabelText);
    mPrompt->setToolTip(clothing ? getString("prompt_tip_clothing") : mPromptTip);
    mTips->setText(clothing ? getString("tips_clothing") : mTipsText);
    refreshClothes();
    layoutWidgets();   // the clothes choice takes its room from the description only while it shows
}

// ─── 1-2: the price, then Start ─────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::onGetPrice()
{
    if (mBusy) return;
    clearError();
    const bool person = isPerson();
    const std::string prompt = cleanWords(mPrompt->getText());
    const std::string name = cleanWords(mName->getText());
    std::string height = mHeight->getText();
    LLStringUtil::trim(height);
    // the brain's own checks (wa_api_char_quote), said here by the field before anything is sent
    std::string bad;
    const bool clothing = isClothing();
    if (prompt.empty()) bad = clothing ? "Describe the garment in words." : "Describe your character in words.";
    else if (prompt.size() > PROMPT_MAX_BYTES) bad = llformat("The description is at most %d characters (it is %d now).", (S32)PROMPT_MAX_BYTES, (S32)prompt.size());
    else if (utf8Chars(name) > NAME_MAX_CHARS) bad = llformat("The name is at most %d characters.", (S32)NAME_MAX_CHARS);
    F64 metres = 0.0;
    if (clothing) height.clear();   // Flow 1 "Clothing": no height (the brain gives a garment the standard body's size)
    if (bad.empty() && !height.empty())
    {
        if (!LLStringUtil::convertToF64(height, metres)) bad = "The height is a number of metres, like 1.8.";
        else if (metres < HEIGHT_MIN || metres > HEIGHT_MAX) bad = "The height is from 0.3 to 4 metres.";
    }
    if (!bad.empty())
    {
        mFormError->setText(bad);
        mFormError->setColor(ERR_INK);   // on its own dark plate (drawPlates)
        return;
    }
    // Source: WOLFAVATARS_API.md Flow 1 - char_quote {kind, prompt, name?, height?, clothes?: "dressed"|"base"}
    LLSD body;
    body["kind"] = person ? "person" : clothing ? "clothing" : "animal";
    body["prompt"] = prompt;
    if (!name.empty()) body["name"] = name;
    if (!height.empty()) body["height"] = metres;
    // [BASE BODY 2026-10-20] people only; an animal says "dressed", which the brain stores for it anyway
    // (wolfavatars_lib.php wa_api_char_quote: if ($kind !== 'person') $clothes = 'dressed')
    if (!clothing) body["clothes"] = (person && mBase) ? "base" : "dressed";
    // [CLOTHING 2026-10-20] Flow 1 - garment top | dress | trousers | skirt | shoes | hat | cape | gown, required for clothing
    if (clothing) body["garment"] = GARMENT_IDS[mGarment];
    // [FIGURE 2026-10-20] Flow 1 "Figure" - female | male, sent for a base body and for clothing only
    if (figureShown()) body["figure"] = mMale ? "male" : "female";
    setBusy(true);
    mPriceBtn->setLabel(std::string("Working out the price..."));
    const U32 serial = ++mSerial;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_quote", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        self->setBusy(false);
        self->mPriceBtn->setLabel(std::string("Get the price"));
        if (!ok) { self->setError("The price could not be worked out: " + error, reply); return; }
        // -> {job, kind, name, height, clothes, quote: {credits, pictures, build, providerCredits}}. Nothing charged.
        const std::string job = reply["job"].asString();
        const LLSD& q = reply["quote"];
        if (job.empty() || !q.isMap())
        {
            self->setError("The price could not be worked out: the WolfAvatars service gave no price. Try again in a moment.");
            return;
        }
        self->mJob = job;
        self->mQuoteCredits = q["credits"].asInteger();
        self->mQuotePictures = q["pictures"].asInteger();
        self->mQuoteBuild = q["build"].asInteger();
        // Source: WOLFAVATARS_API.md Flow 2 - "about N credits: N1 for 4 pictures, N2 to build the one you pick"; a base body
        // says so on the line (the reply's clothes, wolfavatars_lib.php wa_api_char_quote: what the job was quoted for)
        // [CLOTHING / FIGURE 2026-10-20] the garment and the figure the job was quoted for, from the reply (wa_api_char_quote
        // returns garment and figure, null when they do not apply)
        std::vector<std::string> what;
        if (reply["clothes"].asString() == "base") what.push_back(self->getString("base_short"));
        const std::string garment = reply["garment"].asString();
        for (S32 i = 0; i < GARMENTS; ++i)
        {
            if (garment != GARMENT_IDS[i]) continue;
            const std::vector<std::string> words = splitList(self->getString("garment_words"));
            what.push_back(i < (S32)words.size() ? words[i] : garment);
        }
        const std::string figure = reply["figure"].asString();
        if (figure == "female") what.push_back(self->getString("woman_word"));
        else if (figure == "male") what.push_back(self->getString("man_word"));
        std::string base;
        for (const std::string& w : what) base += (base.empty() ? " (" : ", ") + w;
        if (!base.empty()) base += ")";
        self->mPriceText->setText(llformat("\"%s\"%s costs about %d credits: %d for 4 pictures, %d to build the one you pick.",
                                           cap(reply["name"].asString(), 48).c_str(), base.c_str(), self->mQuoteCredits, self->mQuotePictures, self->mQuoteBuild));
        self->showStep(STEP_PRICE);
    });
}

void LLFloaterWolfAvatars::onBack()
{
    if (mBusy) return;
    clearError();
    // the quoted job is dropped: nothing was charged (wolfmind_lib.php wa_api_job_cancel: quoting -> cancelled)
    if (!mJob.empty())
    {
        LLSD body;
        body["job"] = mJob;
        WolfAdventure::api("job_cancel", body, nullptr);
    }
    mJob.clear();
    ++mSerial;
    showStep(STEP_DESCRIBE);
}

void LLFloaterWolfAvatars::onStart()
{
    if (mBusy || mJob.empty()) return;
    clearError();
    // Source: WOLFAVATARS_API.md Flow 2 - char_start {job, confirm: true} -> {job, state: "queued", maxCredits}; refusals shown as
    // they are (credits_short + buyurl, credits_unread, the daily limit)
    LLSD body;
    body["job"] = mJob;
    body["confirm"] = true;
    setBusy(true);
    const U32 serial = mSerial;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_start", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        self->setBusy(false);
        if (!ok) { self->setError("It did not start: " + error, reply); return; }
        LLSD job;
        job["job"] = self->mJob;
        job["state"] = reply["state"].asString().empty() ? std::string("queued") : reply["state"].asString();
        job["phase"] = "pictures";
        job["progress"] = "Waiting to start";
        self->applyJob(job);
    });
}

// ─── 3: polling ─────────────────────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::poll()
{
    if (mJob.empty() || mPollOut) return;
    mPollOut = true;
    const U32 serial = mSerial;
    LLSD body;
    body["job"] = mJob;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("job", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self) return;
        self->mPollOut = false;
        self->mNextPoll = LLFrameTimer::getTotalSeconds() + POLL_SEC;
        if (serial != self->mSerial) return;
        if (!ok)
        {
            // a poll that did not get through is said, and the next one is tried (the job carries on at the brain)
            self->setError("Could not read how it is going: " + error + " Trying again...", reply);
            return;
        }
        self->clearError();   // a poll error said before is over
        self->applyJob(reply);
    });
}

void LLFloaterWolfAvatars::applyJob(const LLSD& job)
{
    // Source: WOLFAVATARS_API.md Flow 3 - common fields {job, kind:"character", state, progress, error, cost{credits},
    // maxCredits, quote} plus {characterKind, name, prompt, height, clothes, phase, round, rounds, pictures, pick}
    mJobLast = job;
    const std::string state = job["state"].asString();
    const std::string error = cap(job["error"].asString(), 500);
    if (!job["job"].asString().empty()) mJob = job["job"].asString();

    // keep what the job was made from, so a failed one goes back to the form with its words (and a resumed one has them)
    if (job.has("prompt") && !job["prompt"].asString().empty()) mPrompt->setText(job["prompt"].asString());
    if (job.has("name") && !job["name"].asString().empty()) mName->setText(job["name"].asString());
    // [BASE BODY 2026-10-20] the clothes it was quoted with (wolfavatars_lib.php wch_job_out: clothes, "dressed" when unset)
    if (job.has("clothes")) mBase = job["clothes"].asString() == "base";
    // [CLOTHING / FIGURE 2026-10-20] the garment and figure it was quoted with (wch_job_out: garment, figure; null when unset)
    if (job["garment"].isString())
    {
        for (S32 i = 0; i < GARMENTS; ++i) if (job["garment"].asString() == GARMENT_IDS[i]) mGarment = i;
    }
    if (job["figure"].isString()) mMale = job["figure"].asString() == "male";
    if (job.has("characterKind"))
    {
        const std::string ck = job["characterKind"].asString();
        mKind = ck == "animal" ? KIND_ANIMAL : ck == "clothing" ? KIND_CLOTHING : KIND_PERSON;
        if (job["height"].isReal() || job["height"].isInteger()) mHeight->setText(llformat("%.2f", job["height"].asReal()));
        else if (job["characterKind"].asString() == "animal") mHeight->setText(LLStringUtil::null);
        onKindChanged();
    }
    else if (job.has("clothes") || job.has("garment") || job.has("figure"))
    {
        refreshClothes();
        layoutWidgets();
    }

    if (state == "queued" || state == "running")
    {
        // Source: WOLFAVATARS_API.md Flow 3 - show `progress` ("Drawing pictures (40%)", "Making the body (63%)", ...);
        // Flow 5 - the build "takes 3-6 minutes"
        const bool build = job["phase"].asString() == "build";
        const bool clothing = isClothing();
        mWorkTitle->setText(build ? (clothing ? std::string("Making your clothes. This takes 3 to 6 minutes.")
                                              : std::string("Making your avatar. This takes 3 to 6 minutes."))
                                  : (clothing ? std::string("Drawing 4 pictures of your garment.")
                                              : std::string("Drawing 4 pictures of your character.")));
        const std::string progress = cap(job["progress"].asString(), 120);
        mWorkWords->setText(progress.empty() ? std::string("Waiting to start") : progress);
        const S32 pct = progressPercent(progress);
        // the rune ring's glowing arc is the bar (drawCircle); with no % in the words it turns instead
        mWorkPct = pct;
        // the step stone lit: Build for the build, Pictures otherwise (the same test as the title above)
        mWorkPhase = build ? "build" : "pictures";
        if (mStep != STEP_WORKING) clearError();
        showStep(STEP_WORKING);
        return;
    }
    if (state == "asking")
    {
        // Flow 3 - `asking` = pictures ready; `error` may be set while asking (a build that failed went back to the pictures:
        // shown above them)
        const S32 count = llclamp(job["pictures"].asInteger(), 0, PICTURES);
        const S32 round = job["round"].asInteger(), rounds = job["rounds"].asInteger();
        mPicTitle->setText(llformat("Pick the picture to make into your avatar (round %d of %d).", round, rounds));
        mPicError->setText(error);
        mPicError->setColor(ERR_INK);    // on its own dark plate (drawPlates)
        if (mStep != STEP_PICTURES) clearError();
        showStep(STEP_PICTURES);
        loadPictures(count, round);
        // Flow 4 - "More pictures (N credits)" while round < rounds
        mMoreBtn->setVisible(round < rounds);
        if (round < rounds) askMorePrice();
        refreshPictures();
        return;
    }
    if (state == "done")
    {
        // Flow 6 - output {folder, folderName, object, faces, triangles}, report [lines]: the lines are shown; the avatar is
        // in Objects > folderName
        mReport->clear();
        mDoneTitle->setText(isClothing() ? getString("done_clothing") : mDoneTitleText);
        LLStyle::Params text;
        text.color = LLUIColorTable::instance().getColor("TextFgReadOnlyColor");
        bool first = true;
        for (const LLSD& line : llsd::inArray(job["report"]))
        {
            mReport->appendText(cap(line.asString(), 400), !first, text);
            first = false;
        }
        const std::string folder = cap(job["output"]["folderName"].asString(), 120);
        if (first) mReport->appendText(folder.empty() ? std::string("Your avatar is in your inventory, under Objects.")
                                                      : "Your avatar is in your inventory: Objects > " + folder + ".", false, text);
        // the folder was written straight into the inventory service: ask for the Objects folder again so it shows up
        // (as wolfai.cpp does after a delivery: VERSION_UNKNOWN + fetchDescendentsOf)
        const LLUUID objects = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
        if (LLViewerInventoryCategory* cat = objects.notNull() ? gInventory.getCategory(objects) : nullptr)
        {
            cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
            gInventory.fetchDescendentsOf(objects);
        }
        clearError();
        showStep(STEP_DONE);
        return;
    }
    if (state == "failed" || state == "cancelled")
    {
        // Flow 3 - `failed` / `cancelled` -> show `error`; the form comes back with the words, to change them or try again
        mJob.clear();
        ++mSerial;
        showStep(STEP_DESCRIBE);
        setError(!error.empty() ? error : state == "cancelled" ? std::string("This character was stopped before it was finished.")
                                                                : std::string("This character could not be made."));
        return;
    }
    if (state == "quoting")
    {
        // a price asked for and not started (only a reply to char_start's own job ever says this): back to the price
        showStep(STEP_PRICE);
        return;
    }
    mJob.clear();
    ++mSerial;
    showStep(STEP_DESCRIBE);
    setError("The WolfAvatars service said something WolfViewer does not understand (state \"" + cap(state, 20) + "\"). Update WolfViewer and try again.");
}

// ─── resume ─────────────────────────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::resume()
{
    showStep(STEP_LOADING);
    clearError();
    const U32 serial = ++mSerial;
    // Source: WOLFAVATARS_API.md Flow "Any time" - jobs {kind: "character"} -> recent jobs {job, state, progress, name,
    // characterKind, created, cost}, newest first (wolfmind_lib.php wa_api_jobs: ORDER BY created_at DESC)
    LLSD body;
    body["kind"] = "character";
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("jobs", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        if (!ok)
        {
            self->showStep(STEP_DESCRIBE);
            self->setError("Could not check for a character you were already making: " + error, reply);
            return;
        }
        // the newest unfinished one: queued or running (being made) or asking (waiting for a picture to be picked)
        for (const LLSD& j : llsd::inArray(reply["jobs"]))
        {
            const std::string state = j["state"].asString();
            if (state != "queued" && state != "running" && state != "asking") continue;
            self->mJob = j["job"].asString();
            if (self->mJob.empty()) continue;
            // the full job (pictures, round, phase) comes from job {job}
            self->mWorkTitle->setText(std::string("Taking up \"") + cap(j["name"].asString(), 48) + "\" where it was.");
            self->mWorkWords->setText(cap(j["progress"].asString(), 120));
            self->mWorkPct = -1;
            self->mWorkPhase.clear();   // not known until job {job} answers (the next poll): no step stone lit till then
            self->showStep(STEP_WORKING);
            self->mNextPoll = 0.0;
            if (state == "asking") self->poll();
            return;
        }
        self->showStep(STEP_DESCRIBE);
    });
}

// ─── 4-5: pictures, pick, more ──────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::loadPictures(S32 count, S32 round)
{
    if (mPicJob == mJob && mPicRound == round && mPicCount == count) return;   // the same pictures as shown (a failed build came back)
    mPicJob = mJob;
    mPicRound = round;
    mPicCount = count;
    mPicked = -1;
    for (S32 i = 0; i < PICTURES; ++i)
    {
        mPic[i]->mTexture = nullptr;
        mPic[i]->mSelected = false;
        mPic[i]->mWhy.clear();
        mPic[i]->mState = WolfAvatarPicture::S_EMPTY;
    }
    for (S32 i = 0; i < count; ++i) loadPicture(i);
}

void LLFloaterWolfAvatars::loadPicture(S32 n)
{
    if (n < 0 || n >= mPicCount) return;
    mPic[n]->mState = WolfAvatarPicture::S_LOADING;
    mPic[n]->mTexture = nullptr;
    // Source: WOLFAVATARS_API.md Flow 4 - char_picture {job, n} -> {n, round, mime: "image/jpeg", data: <base64>} (512 px)
    LLSD body;
    body["job"] = mJob;
    body["n"] = n;
    const std::string job = mJob;
    const S32 round = mPicRound;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_picture", body, [h, job, round, n](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || self->mPicJob != job || self->mPicRound != round) return;   // another job or round is on screen now
        WolfAvatarPicture* pic = self->mPic[n];
        std::string why = error;
        LLPointer<LLViewerTexture> tex;
        if (ok && reply["round"].asInteger() != round) { ok = false; why = "a newer round of pictures is being made"; }
        if (ok) ok = pictureTexture(reply["mime"].asString(), reply["data"].asString(), tex, why);
        pic->mTexture = ok ? tex : LLPointer<LLViewerTexture>();
        pic->mState = ok ? WolfAvatarPicture::S_SHOWN : WolfAvatarPicture::S_FAILED;
        pic->mWhy = ok ? std::string() : cap(why, 160);
        // the square may be too narrow for the whole reason: its tool tip carries it
        pic->setToolTip(ok ? llformat("Picture %d: click to pick it", n + 1) : "Picture " + std::to_string(n + 1) + " not shown: " + pic->mWhy + ". Click to try again.");
        self->refreshPictures();
    });
}

void LLFloaterWolfAvatars::askMorePrice()
{
    const std::string key = mJob + "#" + std::to_string(mPicRound);
    if (mMoreFor == key) return;
    mMoreFor = key;
    mMoreCredits = -1;
    // Source: WOLFAVATARS_API.md Flow 4 - char_more {job} -> {quote:{credits}} (the price of 4 more pictures; nothing charged)
    LLSD body;
    body["job"] = mJob;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_more", body, [h, key](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || self->mMoreFor != key) return;
        if (!ok)
        {
            self->mMoreFor.clear();   // asked again when the button is pressed
            self->setError("The price of more pictures could not be read: " + error, reply);
        }
        else self->mMoreCredits = reply["quote"]["credits"].asInteger();
        self->refreshPictures();
    });
}

void LLFloaterWolfAvatars::refreshPictures()
{
    if (!mPickBtn) return;
    for (S32 i = 0; i < PICTURES; ++i)
    {
        mPic[i]->mSelected = i == mPicked;
        mPic[i]->setEnabled(!mBusy && i < mPicCount);
        mPic[i]->setVisible(i < mPicCount);
    }
    const bool shown = mPicked >= 0 && mPicked < mPicCount && mPic[mPicked]->mState == WolfAvatarPicture::S_SHOWN;
    mPickBtn->setEnabled(!mBusy && shown);
    mPickBtn->setLabel(mPicked >= 0 ? llformat("Make picture %d", mPicked + 1) : std::string("Pick a picture first"));
    mMoreBtn->setEnabled(!mBusy && mMoreCredits >= 0);
    mMoreBtn->setLabel(mMoreCredits >= 0 ? llformat("More pictures (%d credits)", mMoreCredits) : std::string("More pictures"));
}

void LLFloaterWolfAvatars::onPictureClicked(S32 n)
{
    if (mBusy || n < 0 || n >= mPicCount) return;
    if (mPic[n]->mState == WolfAvatarPicture::S_FAILED) { loadPicture(n); refreshPictures(); return; }
    if (mPic[n]->mState != WolfAvatarPicture::S_SHOWN) return;
    mPicked = n;
    refreshPictures();
}

void LLFloaterWolfAvatars::onPick()
{
    if (mBusy || mJob.empty() || mPicked < 0) return;
    clearError();
    // Source: WOLFAVATARS_API.md Flow 5 - char_pick {job, n} -> {job, state: "queued"}; back to polling (phase build)
    LLSD body;
    body["job"] = mJob;
    body["n"] = mPicked;
    setBusy(true);
    const U32 serial = mSerial;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_pick", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        self->setBusy(false);
        if (!ok) { self->setError("Your avatar could not be started: " + error, reply); return; }
        LLSD job;
        job["job"] = self->mJob;
        job["state"] = "queued";
        job["phase"] = "build";
        job["progress"] = "Waiting to start";
        self->mNextPoll = LLFrameTimer::getTotalSeconds() + POLL_SEC;
        self->applyJob(job);
    });
}

void LLFloaterWolfAvatars::onMore()
{
    if (mBusy || mJob.empty()) return;
    if (mMoreCredits < 0) { askMorePrice(); return; }
    clearError();
    // Source: WOLFAVATARS_API.md Flow 4 - char_more {job, confirm:true} -> queued again (a new round of pictures); the price
    // was on the button (wolfavatars_lib.php wa_api_char_more checks the live balance for it: credits_short + buyurl)
    LLSD body;
    body["job"] = mJob;
    body["confirm"] = true;
    setBusy(true);
    const U32 serial = mSerial;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("char_more", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        self->setBusy(false);
        if (!ok) { self->setError("More pictures could not be started: " + error, reply); return; }
        LLSD job;
        job["job"] = self->mJob;
        job["state"] = "queued";
        job["phase"] = "pictures";
        job["progress"] = "Waiting to start";
        self->mNextPoll = LLFrameTimer::getTotalSeconds() + POLL_SEC;
        self->applyJob(job);
    });
}

// ─── stop (job_cancel), asked first ─────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::onAskStop()
{
    if (mBusy || mJob.empty()) return;
    mConfirmStop = true;
    showStep(mStep);
}

void LLFloaterWolfAvatars::onStopAnswer(bool stop)
{
    mConfirmStop = false;
    showStep(mStep);
    if (!stop || mJob.empty()) return;
    clearError();
    // Source: WOLFAVATARS_API.md "Any time: job_cancel {job}"; wolfmind_lib.php wa_api_job_cancel takes quoting, queued, running
    // and asking. The next read says cancelled and the form comes back.
    LLSD body;
    body["job"] = mJob;
    setBusy(true);
    const U32 serial = mSerial;
    LLHandle<LLFloater> h = handle();
    WolfAdventure::api("job_cancel", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        LLFloaterWolfAvatars* self = dynamic_cast<LLFloaterWolfAvatars*>(h.get());
        if (!self || serial != self->mSerial) return;
        self->setBusy(false);
        if (!ok) { self->setError("It could not be stopped: " + error, reply); return; }
        // read it back so the window shows what the brain now says (cancelled -> the form, with its words)
        self->mWorkTitle->setText(std::string("Stopping it..."));
        self->mWorkWords->setText(LLStringUtil::null);
        self->mWorkPct = -1;
        self->showStep(STEP_WORKING);
        self->mNextPoll = 0.0;
        self->poll();
    });
}

// ─── 6: done ────────────────────────────────────────────────────────────────────────────────────────

void LLFloaterWolfAvatars::onOpenInventory()
{
    // Flow 6 - the avatar is in Objects > folderName; "Open my inventory". The folder is selected when the viewer has it;
    // otherwise the inventory opens and the note under the report says to relog or refresh (the window's done step).
    LLUUID folder;
    folder.set(mJobLast["output"]["folder"].asString(), false);
    if (folder.notNull() && gInventory.getCategory(folder))
    {
        // Source: llmeshrepository.cpp - an uploaded item shown with openInventoryPanelAndSetSelection(true, id, true, ...)
        LLInventoryPanel::openInventoryPanelAndSetSelection(true, folder, true);
        return;
    }
    const LLUUID objects = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    if (objects.notNull() && gInventory.getCategory(objects)) LLInventoryPanel::openInventoryPanelAndSetSelection(true, objects, true);
    else LLFloaterReg::showInstance("inventory");
    setError("The folder has not reached your inventory list yet. Look in Objects in a moment; if it is still not there, relog or refresh your inventory.");
}

void LLFloaterWolfAvatars::onMakeAnother()
{
    if (mBusy) return;
    mJob.clear();
    ++mSerial;
    mPicJob.clear();
    mPicRound = -1;
    mPicCount = 0;
    mPicked = -1;
    mMoreFor.clear();
    mMoreCredits = -1;
    mPrompt->setText(LLStringUtil::null);
    mName->setText(LLStringUtil::null);
    clearError();
    showStep(STEP_DESCRIBE);
}

void LLFloaterWolfAvatars::onBuyCredits()
{
    // Source: wolfimportflow.cpp openBuyPage - only a Wolf Territories page is opened (WM_BUY_URL =
    // https://www.wolf-grid.com/index.php?f=credits, wolfmind_lib.php:81)
    if (mBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuyUrl);
    else setError("The credits page address did not come from Wolf Territories, so it was not opened.");
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// The Summoning Circle: where everything sits, and the den drawn behind it
// ════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    /** The slate panel fields and lines of text sit on. */
    void slate(const LLRect& r)
    {
        LLRect shadow = r;
        shadow.translate(3, -4);
        fillRounded(shadow, allCorners(10.f), SHADOW, SHADOW);
        fillRounded(r, allCorners(10.f), SLATE_TOP, SLATE_BOTTOM);
        strokeRounded(r, allCorners(10.f), 0.f, 1.5f, withAlpha(SILVER, 0.65f), withAlpha(SILVER, 0.30f));
        strokeRounded(r, allCorners(10.f), 4.f, 5.f, withAlpha(AMBER, 0.18f), withAlpha(AMBER, 0.06f));
    }

    /** The plate an error line sits on: dark red with a red-amber rim and a warning sign, so it reads as an error in shape
     *  as well as colour. */
    void errorPlate(const LLRect& r)
    {
        LLRect shadow = r;
        shadow.translate(2, -3);
        fillRounded(shadow, allCorners(6.f), SHADOW, SHADOW);
        fillRounded(r, allCorners(6.f), lighten(ERR_PLATE, 0.02f), ERR_PLATE);
        strokeRounded(r, allCorners(6.f), 0.f, 1.5f, ERR_INK, withAlpha(ERR_INK, 0.7f));
        const F32 x = r.mLeft + 19.f, y = r.mTop - 19.f, s = 10.f;
        tri(x - s, y - s * 0.8f, x + s, y - s * 0.8f, x, y + s * 0.95f, ERR_INK);
        line(x, y + s * 0.45f, x, y - s * 0.25f, 2.2f, ERR_PLATE);
        disc(x, y - s * 0.52f, 1.4f, ERR_PLATE, ERR_PLATE);
    }

    /** The brass plaque the price is engraved on. */
    void brassPlaque(const LLRect& r)
    {
        LLRect shadow = r;
        shadow.translate(3, -4);
        fillRounded(shadow, allCorners(8.f), SHADOW, SHADOW);
        fillRounded(r, allCorners(8.f), BRASS_TOP, BRASS_BOTTOM);
        strokeRounded(r, allCorners(8.f), 0.f, 1.5f, BRASS_DARK, BRASS_DARK);
        strokeRounded(r, allCorners(8.f), 4.f, 5.f, withAlpha(BRASS_LIGHT, 0.8f), withAlpha(BRASS_DARK, 0.5f));
        const F32 sr = 3.f;
        screw(r.mLeft + 9.f, r.mTop - 9.f, sr);
        screw(r.mRight - 9.f, r.mTop - 9.f, sr);
        screw(r.mLeft + 9.f, r.mBottom + 9.f, sr);
        screw(r.mRight - 9.f, r.mBottom + 9.f, sr);
    }

    /** A choice chip on the slate (Dressed / Base, Woman / Man, a garment), drawn as a small tile: the chosen one lit amber
     *  with a check in its badge and the word "chosen", the other dark with an empty ring - the choice in a shape and a word,
     *  not only a colour. keep_word: "chosen" always shows and the label is cut to make room; otherwise "chosen" shows only
     *  where it fits beside the whole label (the eight narrow garment chips), and the badge and amber say it alone.
     *  Contrast (WCAG 2.x, the chip's lightest point, hovered): AMBER on the chosen chip (TILE_ON_TOP +0.03) 5.3:1, 6.0:1
     *  unhovered; CREAM on the other (TILE_TOP +0.03) 10.3:1; INK on the badge (AMBER +0.1) 13.2:1. */
    void drawChip(const LLRect& r, LLButton* b, bool chosen, const std::string& label, const std::string& chosen_word,
                  const LLFontGL* f, bool keep_word)
    {
        if (!f || !b || r.getWidth() < 30 || r.getHeight() < 12 || !b->getVisible()) return;
        const bool hover = b->isInEnabledChain() && mouseOver(b);
        const F32 lift = hover ? 0.03f : 0.f;
        const F32 rad = 6.f;
        untextured();
        LLRect shadow = r;
        shadow.translate(2, -2);
        fillRounded(shadow, allCorners(rad), SHADOW, SHADOW);
        fillRounded(r, allCorners(rad), lighten(chosen ? TILE_ON_TOP : TILE_TOP, lift), lighten(chosen ? TILE_ON_BOTTOM : TILE_BOTTOM, lift));
        strokeRounded(r, allCorners(rad), 0.f, chosen ? 2.f : 1.f, chosen ? AMBER : withAlpha(SILVER, 0.55f), chosen ? SEAL_BOTTOM : withAlpha(SILVER, 0.30f));

        const F32 cy = (F32)r.getCenterY();
        const F32 bx = r.mLeft + CHIP_PAD + CHIP_BADGE * 0.5f, br = CHIP_BADGE * 0.5f;
        if (chosen)
        {
            disc(bx, cy, br, lighten(AMBER, 0.1f), SEAL_BOTTOM);
            checkMark(bx, cy, br * 1.1f, 1.6f, INK);
        }
        else
        {
            ring(bx, cy, br - 1.5f, br, withAlpha(SILVER, 0.7f));
        }
        const F32 lx = (F32)(r.mLeft + CHIP_PAD + CHIP_BADGE + CHIP_SPACE);
        F32 right = (F32)(r.mRight - CHIP_PAD);
        const bool word = chosen && (keep_word || f->getWidthF32(label) + CHIP_SPACE + f->getWidthF32(chosen_word) <= right - lx);
        if (word)
        {
            text(f, chosen_word, right, cy, AMBER, LLFontGL::RIGHT);
            right -= f->getWidthF32(chosen_word) + CHIP_SPACE;
        }
        text(f, label, lx, cy, chosen ? AMBER : CREAM, LLFontGL::LEFT, llmax(10.f, right - lx));
    }

    /** A small four-pointed star (a star in the sky, a spark on the ring). */
    void sparkle(F32 x, F32 y, F32 s, const LLColor4& c)
    {
        quad(x, y + s * 1.6f, x + s * 0.5f, y, x, y - s * 1.6f, x - s * 0.5f, y, c);
        quad(x - s * 1.6f, y, x, y + s * 0.5f, x + s * 1.6f, y, x, y - s * 0.5f, c);
    }
}

void LLFloaterWolfAvatars::fitButton(LLButton* b, const std::vector<std::string>& samples, bool display) const
{
    // one size for the label and every label the button can be given (refreshPictures, onGetPrice), the largest that fits;
    // a label still too wide is cut with an ellipsis (use_ellipses in the XUI) and the tooltip says what it does
    std::vector<std::string> all = samples;
    all.push_back(b->getLabelUnselected());
    const LLRect r = b->getRect();
    b->setFont(fitFont((F32)r.getHeight() - (display ? 14.f : 12.f), (F32)r.getWidth() - (display ? 32.f : 22.f), all, MIN_BUTTON_FONT));
}

void LLFloaterWolfAvatars::layoutWidgets()
{
    if (!mBackdrop || !mBuyBtn || !mStopNo || !mStopYes || !mPicSlot[PICTURES - 1] || !mPanels[5] || !mStopPanel)
    {
        return;   // reshape() during construction, before postBuild() is done
    }
    if (isMinimized())
    {
        // Source: wolffloaterradio.cpp:871-877 - a minimized floater is reshaped to the bare title bar (llfloater.cpp
        // setMinimized): nothing to lay out, and the controls keep their places for when it is restored
        return;
    }
    Layout& L = mLayout;
    const S32 W = getRect().getWidth();
    const S32 H = getRect().getHeight();
    const LLRect local = getLocalRect();
    // the den and every step panel cover the whole window, so a panel's coordinates are the window's
    mBackdrop->setShape(local);
    for (LLPanel* p : mPanels) p->setShape(local);
    mStopPanel->setShape(local);

    // Source: wolffloaterradio.cpp:885 - the client area below the title bar
    L.client = LLRect(LLPANEL_BORDER_WIDTH, H - getHeaderHeight(), W - LLPANEL_BORDER_WIDTH, LLPANEL_BORDER_WIDTH);
    const S32 cw = L.client.getWidth();
    const S32 ch = L.client.getHeight();
    constexpr S32 M = 16;   // side margin

    // ---- the sky: title plate, moon, ridge with the wolf ----
    const S32 banner_h = llclamp(ll_round(ch * 0.19f), 104, 150);
    L.banner = LLRect(L.client.mLeft, L.client.mTop, L.client.mRight, L.client.mTop - banner_h);
    {
        const S32 tw = llclamp(ll_round(cw * 0.36f), 230, 340);
        const S32 th = llclamp(ll_round(banner_h * 0.56f), 58, 78);
        L.title_plate = LLRect(L.client.mLeft + M, L.banner.mTop - 10, L.client.mLeft + M + tw, L.banner.mTop - 10 - th);
    }
    L.moon_r = llclamp(banner_h * 0.26f, 26.f, 40.f);
    L.moon_x = L.client.mRight - M - L.moon_r * 1.8f;
    L.moon_y = L.banner.mTop - 10.f - L.moon_r * 1.1f;
    L.ridge_x = llmax(L.client.mRight - cw * 0.31f, L.title_plate.mRight + cw * 0.10f);

    // ---- the step stones, on the ground under the mountains ----
    constexpr S32 STONES_H = 56;
    L.stones = LLRect(L.client.mLeft, L.banner.mBottom, L.client.mRight, L.banner.mBottom - STONES_H);

    // ---- bottom up: the button row, the error plate, the Stop question while it is asked ----
    constexpr S32 FOOTER_H = 50, ERROR_H = 48, STOP_H = 78;
    L.footer = LLRect(L.client.mLeft + M, L.client.mBottom + FOOTER_H, L.client.mRight - M, L.client.mBottom + 4);
    L.error_plate = LLRect(L.client.mLeft + M, L.footer.mTop + ERROR_H, L.client.mRight - M, L.footer.mTop + 2);
    S32 content_bottom = L.error_plate.mTop + 8;
    const bool stop_up = mConfirmStop && (mStep == STEP_WORKING || mStep == STEP_PICTURES);
    L.stop_plate = stop_up ? LLRect(L.client.mLeft + M, content_bottom + STOP_H, L.client.mRight - M, content_bottom) : LLRect();
    if (stop_up) content_bottom = L.stop_plate.mTop + 8;
    L.content = LLRect(L.client.mLeft + M, L.stones.mBottom - 6, L.client.mRight - M, content_bottom);
    const LLRect& C = L.content;

    // ---- the buttons: the step's main action bottom right as a big seal; Back / Stop / Make another bottom left, away
    // from it; More pictures beside the seal ----
    {
        const S32 fy = L.footer.getCenterY();
        const S32 seal_w = llclamp(ll_round(cw * 0.30f), 200, 280);
        const LLRect primary(L.footer.mRight - seal_w, fy + 20, L.footer.mRight, fy - 20);
        const S32 stone_w = llclamp(ll_round(cw * 0.18f), 120, 170);
        const LLRect left_btn(L.footer.mLeft, fy + 17, L.footer.mLeft + stone_w, fy - 17);
        for (LLButton* b : { mPriceBtn, mStartBtn, mPickBtn, mInvBtn }) b->setShape(primary);
        for (LLButton* b : { mBackBtn, mWorkStop, mPicStop, mAnotherBtn }) b->setShape(left_btn);
        const S32 more_w = llclamp(ll_round(cw * 0.30f), 190, 270);
        mMoreBtn->setShape(LLRect(primary.mLeft - 16 - more_w, fy + 17, primary.mLeft - 16, fy - 17));
        // every label each button can show (onGetPrice, refreshPictures)
        fitButton(mPriceBtn, { "Get the price", "Working out the price..." }, true);
        fitButton(mStartBtn, { "Start" }, true);
        fitButton(mPickBtn, { "Pick a picture first", "Make picture 4" }, true);
        fitButton(mInvBtn, { "Open my inventory" }, true);
        fitButton(mBackBtn, { "Back" }, false);
        fitButton(mWorkStop, { "Stop..." }, false);
        fitButton(mPicStop, { "Stop..." }, false);
        fitButton(mAnotherBtn, { "Make another" }, false);
        fitButton(mMoreBtn, { "More pictures", "More pictures (999 credits)" }, false);
    }

    // ---- the error line, and Buy credits... beside it when a refusal carries the link ----
    {
        const LLRect& e = L.error_plate;
        constexpr S32 BUY_W = 140;
        const bool buy = mBuyBtn->getVisible();
        mError->setShape(LLRect(e.mLeft + 38, e.mTop - 4, e.mRight - (buy ? BUY_W + 18 : 10), e.mBottom + 2));
        mBuyBtn->setShape(LLRect(e.mRight - 8 - BUY_W, e.getCenterY() + 14, e.mRight - 8, e.getCenterY() - 14));
        fitButton(mBuyBtn, { "Buy credits..." }, true);
    }

    // ---- the Stop question ----
    if (stop_up)
    {
        const LLRect& s = L.stop_plate;
        constexpr S32 BW = 150;
        mStopText->setShape(LLRect(s.mLeft + 12, s.mTop - 6, s.mRight - 12, s.mTop - 40));
        mStopNo->setShape(LLRect(s.mLeft + 12, s.mBottom + 36, s.mLeft + 12 + BW, s.mBottom + 8));
        mStopYes->setShape(LLRect(s.mRight - 12 - BW, s.mBottom + 36, s.mRight - 12, s.mBottom + 8));
        fitButton(mStopNo, { "Keep going" }, false);
        fitButton(mStopYes, { "Stop it" }, false);
    }

    // ---- 1. describe: the three tiles on the left, the slate with the fields on the right ----
    {
        mIntro->setShape(LLRect(C.mLeft, C.mTop, C.mRight, C.mTop - 34));
        const S32 below = C.mTop - 40;
        const S32 tw = llclamp(ll_round(cw * 0.26f), 170, 240);
        mKindLabel->setShape(LLRect(C.mLeft, below, C.mLeft + tw, below - 18));
        // [CLOTHING 2026-10-20] Person, Animal, Clothing stacked; drawDescribe lays a tile out side-on (figure left, words
        // right) when it is too short for the figure above its words
        const S32 tiles_top = below - 22;
        constexpr S32 TILE_GAP = 10;
        const S32 tile_h = llmax(40, (tiles_top - C.mBottom - 2 * TILE_GAP) / 3);
        LLButton* tiles[3] = { mPersonBtn, mAnimalBtn, mClothingBtn };
        for (S32 k = 0; k < 3; ++k)
        {
            const S32 top = tiles_top - k * (tile_h + TILE_GAP);
            L.tile[k] = LLRect(C.mLeft, top, C.mLeft + tw, top - tile_h);
            tiles[k]->setShape(L.tile[k]);
        }

        L.slate = LLRect(C.mLeft + tw + 16, below, C.mRight, C.mBottom);
        const S32 x0 = L.slate.mLeft + 12, x1 = L.slate.mRight - 12;
        // top down: name, then the description's label
        S32 y = L.slate.mTop - 10;
        mNameLabel->setShape(LLRect(x0, y, x1, y - 16));
        y -= 18;
        mName->setShape(LLRect(x0, y, x1, y - 22));
        y -= 30;
        mPromptLabel->setShape(LLRect(x0, y, x1, y - 16));
        y -= 18;
        const S32 prompt_top = y;
        // bottom up: the form's error, the tips, the height row (not for clothing), the choices; the description fills what
        // is left. At the window's smallest, 720 x 720, the slate is 354 px: 76 above the description, and below it 190 for
        // a person (dressed), 218 for a base body (Woman / Man as well), 150 for an animal, 214 for clothing - so the
        // description keeps 88 / 60 / 128 / 64 px, above its 40 px floor. (At the old 660 px clothing left it 16: hence 720.)
        S32 yb = L.slate.mBottom + 10;
        L.form_error_plate = LLRect(x0 - 2, yb + 36, x1 + 2, yb - 2);
        mFormError->setShape(LLRect(x0 + 36, yb + 34, x1 - 6, yb));
        yb += 42;
        mTips->setShape(LLRect(x0, yb + 34, x1, yb));
        yb += 38;
        const bool person = isPerson();
        const bool clothing = isClothing();
        if (!clothing)
        {
            const S32 row_t = yb + 34;
            mHeight->setShape(LLRect(x0, row_t, x0 + 70, row_t - 22));
            mHeightNote->setShape(LLRect(x0 + 80, row_t - 2, x1, yb));
            yb = row_t + 4;
            mHeightLabel->setShape(LLRect(x0, yb + 16, x1, yb));
            yb += 16;   // the height label's top
        }
        constexpr S32 HINT_H = 14, CHIP_H = 22, CHIP_GAP = 8, ROW_GAP = 6;   // the hint is one SansSerifSmall line (13 px)
        L.clothes[0] = L.clothes[1] = L.figure[0] = L.figure[1] = LLRect();
        for (LLRect& r : L.garment) r = LLRect();
        L.clothes_font = L.garment_font = L.figure_font = nullptr;
        const std::string chosen = getString("chosen");
        // a chip's width for its label: the badge, the label and "chosen" (drawChip)
        auto need = [&](const LLFontGL* f, const std::string& label) { return 2 * CHIP_PAD + CHIP_BADGE + 2 * CHIP_SPACE + f->getWidth(label) + f->getWidth(chosen); };
        // the line under the choices: the base body's (people) or the garment's (clothing)
        if (person || clothing)
        {
            (clothing ? mGarmentHint : mClothesHint)->setShape(LLRect(x0, yb + 5 + HINT_H, x1, yb + 5));
            yb += 5 + HINT_H + 1;
        }
        // [FIGURE 2026-10-20] Woman / Man, after its label ("Figure" or "Made for"), for a base body or clothing
        if (figureShown())
        {
            const LLFontGL* lf = LLFontGL::getFontSansSerifBold();   // the label's font (XUI figure_label)
            const S32 lw = llmax(lf->getWidth(getString("figure_label")), lf->getWidth(getString("made_for_label"))) + 10;
            mFigureLabel->setShape(LLRect(x0, yb + CHIP_H - 3, x0 + lw, yb + 3));
            const std::string words[2] = { getString("woman_word"), getString("man_word") };
            const S32 cl = x0 + lw;
            const S32 half = llmax(30, ((x1 - cl) - CHIP_GAP) / 2);
            const LLFontGL* f = LLFontGL::getFontSansSerif();
            if (llmax(need(f, words[0]), need(f, words[1])) > half) f = LLFontGL::getFontSansSerifSmall();
            L.figure_font = f;
            const S32 w = llmin(half, llmax(need(f, words[0]), need(f, words[1])) + 16);
            L.figure[0] = LLRect(cl, yb + CHIP_H, cl + w, yb);
            L.figure[1] = LLRect(cl + w + CHIP_GAP, yb + CHIP_H, cl + 2 * w + CHIP_GAP, yb);
            mWomanBtn->setShape(L.figure[0]);
            mManBtn->setShape(L.figure[1]);
            yb += CHIP_H + ROW_GAP;
        }
        // [BASE BODY 2026-10-20] people only: the two clothes chips side by side
        if (person)
        {
            // each chip as wide as its own label needs (the badge, the label, "chosen"), the room left shared out; the smaller
            // font when the window is too narrow for both in SansSerif (drawChip cuts a label with an ellipsis if even
            // that does not fit, and the tooltips carry the whole words)
            const std::string labels[2] = { getString("dressed_word"), getString("base_word") };
            const S32 avail = (x1 - x0) - CHIP_GAP;
            const LLFontGL* f = LLFontGL::getFontSansSerif();
            if (need(f, labels[0]) + need(f, labels[1]) > avail) f = LLFontGL::getFontSansSerifSmall();
            L.clothes_font = f;
            const S32 n0 = need(f, labels[0]), n1 = need(f, labels[1]);
            const S32 w0 = llclamp(avail * n0 / llmax(1, n0 + n1), 40, llmax(40, avail - 40));
            L.clothes[0] = LLRect(x0, yb + CHIP_H, x0 + w0, yb);
            L.clothes[1] = LLRect(x0 + w0 + CHIP_GAP, yb + CHIP_H, x1, yb);
            mDressedBtn->setShape(L.clothes[0]);
            mBaseBtn->setShape(L.clothes[1]);
            yb += CHIP_H + 4;
        }
        // [CLOTHING 2026-10-20] the eight garments, two rows of four equal chips (Top .. Skirt above, Shoes .. Gown below),
        // then the Garment line naming the chosen one. A chip shows "chosen" only where the word fits beside its label;
        // where it does not, the lit check badge and the amber say it (drawChip)
        if (clothing)
        {
            const std::vector<std::string> labels = splitList(getString("garment_words"));
            const S32 gw = llmax(30, ((x1 - x0) - 3 * CHIP_GAP) / 4);
            const LLFontGL* f = LLFontGL::getFontSansSerif();
            for (const std::string& lbl : labels)
            {
                if (2 * CHIP_PAD + CHIP_BADGE + CHIP_SPACE + f->getWidth(lbl) > gw) { f = LLFontGL::getFontSansSerifSmall(); break; }
            }
            L.garment_font = f;
            for (S32 i = 0; i < GARMENTS; ++i)
            {
                const S32 col = i % 4;
                const S32 bottom = i < 4 ? yb + CHIP_H + ROW_GAP : yb;
                const S32 left = x0 + col * (gw + CHIP_GAP);
                L.garment[i] = LLRect(left, bottom + CHIP_H, col == 3 ? x1 : left + gw, bottom);
                mGarmentBtn[i]->setShape(L.garment[i]);
            }
            yb += 2 * CHIP_H + ROW_GAP + 4;
            mGarmentLabel->setShape(LLRect(x0, yb + 16, x1, yb));
            yb += 16 + 6;
        }
        else if (!person)
        {
            yb += 6;
        }
        mPrompt->setShape(LLRect(x0, prompt_top, x1, llmin(yb, prompt_top - 40)));
    }

    // ---- 2. the price: the circle on the left, the brass plaque and how it is charged on the right ----
    F32 price_x, price_y, price_r;
    LLRect price_how_plate;
    {
        price_r = llmax(30.f, llmin(C.getHeight() * 0.42f, C.getWidth() * 0.22f));
        price_x = C.mLeft + C.getWidth() * 0.26f;
        price_y = (F32)C.getCenterY();
        const S32 px0 = C.mLeft + ll_round(C.getWidth() * 0.52f);
        L.plaque = LLRect(px0, C.mTop - 8, C.mRight, C.mTop - 8 - 112);
        mPriceTitle->setShape(LLRect(px0 + 20, L.plaque.mTop - 14, C.mRight - 20, L.plaque.mTop - 38));
        mPriceText->setShape(LLRect(px0 + 20, L.plaque.mTop - 42, C.mRight - 20, L.plaque.mBottom + 10));
        price_how_plate = LLRect(px0, L.plaque.mBottom - 14, C.mRight, llmax(C.mBottom, L.plaque.mBottom - 14 - 112));
        mPriceHow->setShape(LLRect(px0 + 14, price_how_plate.mTop - 10, C.mRight - 14, price_how_plate.mBottom + 6));
    }

    // ---- 3. working (and the loading check): the title, the ring, the stage words on a plate below ----
    F32 work_x, work_y, work_r;
    LLRect work_plate;
    {
        mWorkTitle->setShape(LLRect(C.mLeft, C.mTop, C.mRight, C.mTop - 38));
        const S32 pw = llmin(C.getWidth(), 620);
        const S32 pl = C.getCenterX() - pw / 2;
        work_plate = LLRect(pl, C.mBottom + 92, pl + pw, C.mBottom);
        mWorkWords->setShape(LLRect(pl + 12, work_plate.mTop - 8, pl + pw - 12, work_plate.mTop - 46));
        mWorkNote->setShape(LLRect(pl + 12, work_plate.mTop - 50, pl + pw - 12, work_plate.mBottom + 4));
        mLoadingText->setShape(LLRect(pl + 12, work_plate.mTop - 26, pl + pw - 12, work_plate.mBottom + 20));
        const F32 top = C.mTop - 44.f, bottom = work_plate.mTop + 10.f;
        work_r = llmax(24.f, llmin((top - bottom) * 0.5f, C.getWidth() * 0.5f) * 0.94f);
        work_x = (F32)C.getCenterX();
        work_y = (top + bottom) * 0.5f;
    }

    // ---- 4-5. the pictures: pictures 1 and 3 left of the circle, 2 and 4 right of it (the 2x2 grid's reading order) ----
    F32 pic_x, pic_y, pic_r;
    {
        mPicTitle->setShape(LLRect(C.mLeft, C.mTop, C.mRight, C.mTop - 20));
        S32 top = C.mTop - 26;
        L.pic_error_plate = LLRect(C.mLeft, top, C.mRight, top - 44);
        mPicError->setShape(LLRect(C.mLeft + 38, top - 4, C.mRight - 10, top - 42));
        if (!mPicError->getText().empty()) top -= 52;   // room for the plate only while there is something on it
        const S32 area_h = llmax(60, top - C.mBottom);
        constexpr S32 GAP_V = 14, GAP_H = 22;
        const S32 s = llmax(48, llmin((area_h - GAP_V) / 2, ll_round(C.getWidth() * 0.24f)));
        const F32 D = llmax(40.f, llmin((F32)(C.getWidth() - 2 * s - 2 * GAP_H), (F32)area_h));
        pic_r = D * 0.5f * 0.92f;
        pic_x = (F32)C.getCenterX();
        pic_y = C.mBottom + area_h * 0.5f;
        const S32 bt = ll_round(pic_y + (2 * s + GAP_V) * 0.5f);
        const S32 lx = ll_round(pic_x - D * 0.5f) - GAP_H - s;
        const S32 rx = ll_round(pic_x + D * 0.5f) + GAP_H;
        L.pic[0] = LLRect(lx, bt, lx + s, bt - s);
        L.pic[1] = LLRect(rx, bt, rx + s, bt - s);
        L.pic[2] = LLRect(lx, bt - s - GAP_V, lx + s, bt - 2 * s - GAP_V);
        L.pic[3] = LLRect(rx, bt - s - GAP_V, rx + s, bt - 2 * s - GAP_V);
        for (S32 i = 0; i < PICTURES; ++i) mPicSlot[i]->setShape(L.pic[i]);
    }

    // ---- 6. done: the picture in its light burst on the left, the report on a slate on the right ----
    {
        const S32 half = C.mLeft + ll_round(C.getWidth() * 0.46f);
        L.done_slate = LLRect(half + 8, C.mTop, C.mRight, C.mBottom);
        const S32 s = llmax(60, llmin(C.getHeight() - 60, half - C.mLeft - 70));
        const S32 cx = (C.mLeft + half) / 2, cy = C.getCenterY();
        L.done_picture = LLRect(cx - s / 2, cy + s / 2, cx + s / 2, cy - s / 2);
        const S32 x0 = L.done_slate.mLeft + 14, x1 = L.done_slate.mRight - 14;
        mDoneTitle->setShape(LLRect(x0, L.done_slate.mTop - 12, x1, L.done_slate.mTop - 36));
        mReport->setShape(LLRect(x0, L.done_slate.mTop - 42, x1, L.done_slate.mBottom + 56));
        mDoneNote->setShape(LLRect(x0, L.done_slate.mBottom + 50, x1, L.done_slate.mBottom + 8));
    }

    // the circle and the plate under it belong to the step on screen
    L.ring_x = L.ring_y = L.ring_r = 0.f;
    L.words_plate = LLRect();
    switch (mStep)
    {
    case STEP_PRICE:
        L.ring_x = price_x; L.ring_y = price_y; L.ring_r = price_r;
        L.words_plate = price_how_plate;
        break;
    case STEP_LOADING:
    case STEP_WORKING:
        L.ring_x = work_x; L.ring_y = work_y; L.ring_r = work_r;
        L.words_plate = work_plate;
        break;
    case STEP_PICTURES:
        L.ring_x = pic_x; L.ring_y = pic_y; L.ring_r = pic_r;
        break;
    default:
        break;
    }
}

S32 LLFloaterWolfAvatars::currentStone() const
{
    // Describe - Pictures - Pick - Build - Done (the floater string "stones")
    switch (mStep)
    {
    case STEP_DESCRIBE:
    case STEP_PRICE:    return 0;
    case STEP_WORKING:  return mWorkPhase == "build" ? 3 : mWorkPhase == "pictures" ? 1 : -1;
    case STEP_PICTURES: return 2;
    case STEP_DONE:     return 4;
    default:            return -1;   // still checking for a character already being made
    }
}

void LLFloaterWolfAvatars::drawDen()
{
    const Layout& L = mLayout;
    if (isMinimized() || L.client.getWidth() < 120 || L.client.getHeight() < 120)
    {
        return;
    }
    const F32 t = animSeconds();
    untextured();
    drawSky(t);
    drawTitle();
    drawStones();
    switch (mStep)
    {
    case STEP_LOADING:
        drawCircle(L.ring_x, L.ring_y, L.ring_r, t, -1, true, 0.6f, false);
        break;
    case STEP_DESCRIBE:
        drawDescribe(t);
        break;
    case STEP_PRICE:
        drawCircle(L.ring_x, L.ring_y, L.ring_r, t, -1, false, 0.8f, true);
        break;
    case STEP_WORKING:
        drawCircle(L.ring_x, L.ring_y, L.ring_r, t, mWorkPct, mWorkPct < 0, 1.f, true);
        break;
    case STEP_PICTURES:
        drawPictures(t);
        break;
    case STEP_DONE:
        drawDone(t);
        break;
    }
    drawPlates();
    if (mStep == STEP_DESCRIBE) drawChips();   // on the slate, so after it
    drawButtons(t);
    untextured();
}

void LLFloaterWolfAvatars::drawSky(F32 t) const
{
    const Layout& L = mLayout;
    const LLRect& c = L.client;
    const LLRect& b = L.banner;
    const F32 bh = (F32)b.getHeight();
    const F32 cw = (F32)c.getWidth();
    untextured();

    // the den floor under everything, then the sky: deep blue at the top through indigo to violet at the horizon
    box((F32)c.mLeft, (F32)b.mBottom, (F32)c.mRight, (F32)c.mBottom, FLOOR_TOP, FLOOR_BOTTOM);
    const F32 mid = b.mBottom + bh * 0.5f;
    box((F32)c.mLeft, (F32)b.mTop, (F32)c.mRight, mid, SKY_TOP, SKY_MID);
    box((F32)c.mLeft, mid, (F32)c.mRight, (F32)b.mBottom, SKY_MID, SKY_HORIZON);

    // stars drifting slowly east and twinkling (one batch: two quads each)
    {
        const S32 n = llclamp((S32)(cw / 11.f), 30, 100);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 h1 = hash01(i * 1.37f + 0.5f), h2 = hash01(i * 4.11f + 1.3f), h3 = hash01(i * 7.93f + 2.1f);
            const F32 x = c.mLeft + fmodf(h1 * cw + t * (1.5f + 3.f * h3), cw);
            const F32 y = b.mBottom + bh * 0.28f + bh * 0.70f * h2;
            const F32 tw = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * (0.7f + 1.6f * h3) + h1 * 20.f));
            const F32 s = 0.6f + 1.3f * h3;
            const LLColor4 col(0.92f, 0.93f, 1.f, 0.85f * tw);
            vtx(x, y + s * 1.6f, col); vtx(x + s * 0.5f, y, col); vtx(x, y - s * 1.6f, col);
            vtx(x, y + s * 1.6f, col); vtx(x, y - s * 1.6f, col); vtx(x - s * 0.5f, y, col);
            vtx(x - s * 1.6f, y, col); vtx(x, y + s * 0.5f, col); vtx(x + s * 1.6f, y, col);
            vtx(x - s * 1.6f, y, col); vtx(x + s * 1.6f, y, col); vtx(x, y - s * 0.5f, col);
        }
        gGL.end();
    }

    // the moon: a wide soft halo, a nearer glow, the pale disc and its seas
    {
        const F32 mx = L.moon_x, my = L.moon_y, R = L.moon_r;
        const F32 breathe = 0.9f + 0.1f * sinf(t * 0.6f);
        disc(mx, my, R * 3.4f, withAlpha(MOON_GLOW, 0.16f * breathe), CLEAR);
        disc(mx, my, R * 1.9f, withAlpha(MOON_GLOW, 0.24f * breathe), CLEAR);
        disc(mx, my, R, MOON_LIGHT, MOON_RIM);
        disc(mx - R * 0.30f, my + R * 0.22f, R * 0.24f, withAlpha(MOON_RIM, 0.75f), withAlpha(MOON_RIM, 0.25f));
        disc(mx + R * 0.28f, my - R * 0.30f, R * 0.17f, withAlpha(MOON_RIM, 0.70f), withAlpha(MOON_RIM, 0.20f));
        disc(mx + R * 0.36f, my + R * 0.36f, R * 0.10f, withAlpha(MOON_RIM, 0.60f), withAlpha(MOON_RIM, 0.15f));
        disc(mx - R * 0.05f, my - R * 0.55f, R * 0.08f, withAlpha(MOON_RIM, 0.55f), withAlpha(MOON_RIM, 0.10f));
    }

    // the far mountains, peaks and saddles in turn
    {
        constexpr S32 N = 14;
        std::vector<LLVector2> pts;
        pts.reserve(N + 1);
        for (S32 i = 0; i <= N; ++i)
        {
            const F32 h = (i & 1) ? 0.18f + 0.30f * hash01(i * 3.7f + 1.f) : 0.07f + 0.10f * hash01(i * 2.3f + 4.f);
            pts.emplace_back(c.mLeft + cw * i / N, b.mBottom + bh * h);
        }
        fillProfile(pts, (F32)b.mBottom, MOUNT_FAR, lerpColour(MOUNT_FAR, MOUNT_NEAR, 0.6f));
    }

    // pines along the foot of the mountains (the near hills cover their roots)
    {
        const S32 n = llclamp((S32)(cw / 24.f), 12, 60);
        const F32 base = b.mBottom + bh * 0.03f;
        for (S32 i = 0; i < n; ++i)
        {
            const F32 x = c.mLeft + cw * hash01(i * 5.3f + 0.7f);
            const F32 h = bh * (0.10f + 0.14f * hash01(i * 2.9f + 3.1f));
            box(x - 1.f, base + h * 0.25f, x + 1.f, base, MOUNT_NEAR, MOUNT_NEAR);
            for (S32 k = 0; k < 3; ++k)
            {
                const F32 w = h * (0.30f - 0.07f * k);
                const F32 y0 = base + h * (0.15f + 0.26f * k);
                tri(x - w, y0, x + w, y0, x, y0 + h * 0.42f, MOUNT_NEAR);
            }
        }
    }

    // the near hills
    {
        constexpr S32 N = 22;
        std::vector<LLVector2> pts;
        pts.reserve(N + 1);
        for (S32 i = 0; i <= N; ++i)
        {
            pts.emplace_back(c.mLeft + cw * i / N, b.mBottom + bh * (0.03f + 0.07f * hash01(i * 1.9f + 5.f)));
        }
        fillProfile(pts, b.mBottom - 1.f, MOUNT_NEAR, MOUNT_NEAR);
    }

    // the ridge, and on it the wolf howling at the moon
    {
        const F32 rx = L.ridge_x, rw = cw * 0.09f;
        const F32 rt = b.mBottom + bh * 0.42f;
        const F32 base = (F32)b.mBottom - 1.f;
        std::vector<LLVector2> pts = {
            LLVector2(rx - rw * 1.9f, base), LLVector2(rx - rw * 1.1f, b.mBottom + bh * 0.18f), LLVector2(rx - rw * 0.6f, rt - 6.f),
            LLVector2(rx - rw * 0.25f, rt), LLVector2(rx + rw * 0.45f, rt - 2.f), LLVector2(rx + rw * 0.8f, rt - bh * 0.10f),
            LLVector2(rx + rw * 1.5f, b.mBottom + bh * 0.08f), LLVector2(rx + rw * 2.2f, base) };
        fillProfile(pts, base, MOUNT_NEAR, MOUNT_NEAR);
        // moonlight catching the top of the rock
        line(rx - rw * 0.6f, rt - 6.f, rx - rw * 0.25f, rt, 1.2f, withAlpha(MOON_GLOW, 0.35f));
        line(rx - rw * 0.25f, rt, rx + rw * 0.45f, rt - 2.f, 1.2f, withAlpha(MOON_GLOW, 0.45f));
        line(rx + rw * 0.45f, rt - 2.f, rx + rw * 0.8f, rt - bh * 0.10f, 1.2f, withAlpha(MOON_GLOW, 0.30f));
        drawHowlingWolf(rx, rt - 1.f, bh * 0.30f, SILHOUETTE);
    }

    // a faint violet mist where the hills meet the den floor
    box((F32)c.mLeft, (F32)b.mBottom, (F32)c.mRight, b.mBottom - 26.f, withAlpha(VIOLET, 0.14f), withAlpha(VIOLET, 0.f));
}

void LLFloaterWolfAvatars::drawTitle() const
{
    const LLRect& p = mLayout.title_plate;
    const std::string title = getString("title");
    const std::string sub = getString("subtitle");
    untextured();
    halo(p, 10.f, 8, withAlpha(AMBER, 0.35f));
    plate(p, 10.f, SILVER);
    strokeRounded(p, allCorners(10.f), 4.f, 5.f, withAlpha(AMBER, 0.45f), withAlpha(AMBER, 0.25f));

    const F32 cx = (F32)p.getCenterX();
    // the name glows: a ring of faint amber copies round it, then cut into the plate in cream (CREAM on PLATE 16:1)
    const LLFontGL* tf = fitFont(p.getHeight() * 0.52f, (F32)p.getWidth() - 36.f, { title }, 4);
    const F32 ty = p.mTop - p.getHeight() * 0.40f;
    const LLColor4 glow = withAlpha(AMBER, 0.20f);
    static const F32 OFFSETS[8][2] = { { -2.f, 0.f }, { 2.f, 0.f }, { 0.f, -2.f }, { 0.f, 2.f }, { -1.5f, -1.5f }, { 1.5f, 1.5f }, { -1.5f, 1.5f }, { 1.5f, -1.5f } };
    for (const auto& o : OFFSETS) text(tf, title, cx + o[0], ty + o[1], glow);
    engraved(tf, title, cx, ty, CREAM);

    // "wolf-grid.com" small under it, between two little diamonds (MUTED on PLATE 11.5:1)
    const LLFontGL* sf = fitFont(p.getHeight() * 0.22f, (F32)p.getWidth() - 60.f, { sub }, 0);
    const F32 sy = p.mBottom + p.getHeight() * 0.20f;
    text(sf, sub, cx, sy, MUTED);
    untextured();
    const F32 sw = sf->getWidthF32(sub) * 0.5f + 12.f;
    sparkle(cx - sw, sy, 2.f, withAlpha(AMBER, 0.8f));
    sparkle(cx + sw, sy, 2.f, withAlpha(AMBER, 0.8f));
}

void LLFloaterWolfAvatars::drawStones() const
{
    const Layout& L = mLayout;
    const std::vector<std::string> words = splitList(getString("stones"));
    if ((S32)words.size() < STONES) return;
    const S32 cur = currentStone();
    const F32 t = animSeconds();
    const F32 l = L.stones.mLeft + 40.f, r = L.stones.mRight - 40.f;
    const F32 slot = (r - l) / STONES;
    const F32 sy = L.stones.mTop - 18.f;
    const F32 wy = L.stones.mBottom + 13.f;
    const F32 rx = llclamp(slot * 0.16f, 12.f, 20.f), ry = rx * 0.62f;
    const LLFontGL* f = fitFont(17.f, slot - 8.f, words, 1);
    untextured();

    // the path from stone to stone: small diamonds, silver where it has been walked
    for (S32 i = 0; i + 1 < STONES; ++i)
    {
        const F32 x0 = l + slot * (i + 0.5f) + rx + 8.f, x1 = l + slot * (i + 1.5f) - rx - 8.f;
        const bool walked = cur >= 0 && i < cur;
        for (F32 x = x0; x <= x1; x += 10.f) sparkle(x, sy, 1.2f, walked ? withAlpha(SILVER, 0.7f) : withAlpha(DIM, 0.45f));
    }
    for (S32 i = 0; i < STONES; ++i)
    {
        const F32 x = l + slot * (i + 0.5f);
        const bool done = cur >= 0 && i < cur;
        const bool now = i == cur;
        untextured();
        ellipse(x + 2.f, sy - 3.f, rx * 1.1f, ry * 1.1f, SHADOW, CLEAR);
        if (now)
        {
            const F32 pulse = 0.75f + 0.25f * sinf(t * 2.4f);
            ellipse(x, sy, rx * 2.0f, ry * 2.2f, withAlpha(AMBER, 0.40f * pulse), CLEAR);
            ellipse(x, sy, rx, ry, lighten(AMBER, 0.12f), SEAL_BOTTOM);
            sparkle(x, sy, ry * 0.32f, INK);   // a rune on the lit stone
        }
        else if (done)
        {
            ellipse(x, sy, rx, ry, lighten(SILVER, 0.12f), lerpColour(SILVER, STONE_BOTTOM, 0.35f));
            checkMark(x, sy, ry * 1.0f, llmax(1.5f, ry * 0.22f), INK);
        }
        else
        {
            ellipse(x, sy, rx, ry, STONE_TOP, STONE_BOTTOM);
            disc(x, sy, 1.6f, withAlpha(DIM, 0.8f), withAlpha(DIM, 0.8f));
        }
        // its word (on the den floor: AMBER 11.4:1, SILVER 12.8:1, DIM 6.9:1); the current one underlined as well as amber
        const LLColor4 ink = now ? AMBER : done ? SILVER : DIM;
        text(f, words[i], x, wy, ink, LLFontGL::HCENTER, slot - 4.f);
        if (now)
        {
            untextured();
            const F32 uw = llmin(f->getWidthF32(words[i]), slot - 4.f) * 0.5f;
            line(x - uw, wy - f->getLineHeight() * 0.55f, x + uw, wy - f->getLineHeight() * 0.55f, 1.5f, AMBER);
        }
    }
}

void LLFloaterWolfAvatars::drawFigure(F32 cx, F32 cy, F32 size, const LLColor4& colour) const
{
    // the chosen kind's silhouette centred on (cx, cy), size across
    if (mKind == KIND_PERSON)
    {
        drawPerson(cx, cy - size * 0.5f, size, colour);
    }
    else if (mKind == KIND_CLOTHING)
    {
        drawShirtOnHanger(cx, cy - size * 0.46f, size * 0.92f, colour);   // the hook's top at 0.92 size: centred like the person
    }
    else
    {
        const F32 u = size * 1.15f;   // the wolf is about 0.68 u tall (drawWolf), its body centred 0.025 u right of cx
        drawWolf(cx - u * 0.025f, cy - u * 0.34f, u, colour);
    }
}

void LLFloaterWolfAvatars::drawDescribe(F32 t) const
{
    const Layout& L = mLayout;
    const std::string words[3] = { getString("person_word"), getString("animal_word"), getString("clothing_word") };
    const std::string details[3] = { getString("person_detail"), getString("animal_detail"), getString("clothing_detail") };
    const std::string chosen_word = getString("chosen");
    LLButton* buttons[3] = { mPersonBtn, mAnimalBtn, mClothingBtn };
    // [CLOTHING 2026-10-20] a tile at least TALL px high has its figure above its words (the tiles as they were with two);
    // a shorter one - three tiles stacked in all but a very tall window - is laid out side-on: the figure on the left, the
    // word, what it means and "chosen" on the right. 136 px is the two-tile height at the old smallest window, where the
    // figure and the "chosen" line were last seen clear of each other.
    constexpr S32 TALL = 136;
    for (S32 k = 0; k < 3; ++k)
    {
        const LLRect& r = L.tile[k];
        if (r.getWidth() < 40 || r.getHeight() < 40) continue;
        const bool chosen = k == (S32)mKind;
        const bool hover = buttons[k]->isInEnabledChain() && mouseOver(buttons[k]);
        untextured();
        if (chosen) halo(r, 12.f, 14, withAlpha(AMBER, 0.8f + 0.2f * sinf(t * 2.f)));
        LLRect shadow = r;
        shadow.translate(3, -4);
        fillRounded(shadow, allCorners(12.f), SHADOW, SHADOW);
        const F32 lift = hover ? 0.03f : 0.f;
        fillRounded(r, allCorners(12.f), lighten(chosen ? TILE_ON_TOP : TILE_TOP, lift), lighten(chosen ? TILE_ON_BOTTOM : TILE_BOTTOM, lift));
        strokeRounded(r, allCorners(12.f), 0.f, chosen ? 2.5f : 1.5f, chosen ? AMBER : withAlpha(SILVER, 0.55f), chosen ? SEAL_BOTTOM : withAlpha(SILVER, 0.30f));

        const bool tall = r.getHeight() >= TALL;
        // where the figure stands: centred above the words on a tall tile, in a square box on the left of a short one
        const F32 box_w = tall ? (F32)r.getWidth() : llmin((F32)r.getHeight() - 12.f, r.getWidth() * 0.36f);
        const F32 fx = tall ? (F32)r.getCenterX() : r.mLeft + 8.f + box_w * 0.5f;
        const F32 base = tall ? r.mBottom + r.getHeight() * 0.30f : r.mBottom + r.getHeight() * 0.14f;
        const F32 fig_h = tall ? r.getHeight() * 0.55f : r.getHeight() * 0.72f;

        // the figure in a pool of light, standing on its shadow
        disc(fx, base + fig_h * 0.55f, llmin(box_w, (F32)r.getHeight()) * (tall ? 0.32f : 0.42f), withAlpha(chosen ? AMBER : MOON_GLOW, chosen ? 0.24f : 0.12f), CLEAR);
        ellipse(fx, base, box_w * (tall ? 0.24f : 0.36f), 3.5f, withAlpha(LLColor4::black, 0.45f), CLEAR);
        const LLColor4 fig = chosen ? lerpColour(TILE_ON_TOP, MOON_LIGHT, 0.92f) : lerpColour(TILE_TOP, SPIRIT, 0.75f);
        if (k == 0)
        {
            drawPerson(fx, base, fig_h, fig);
        }
        else if (k == 1)
        {
            // the wolf is 0.95 u from tail to nose and about 0.68 u tall (drawWolf)
            const F32 u = llmin(box_w * (tall ? 0.80f : 1.f / 0.95f), fig_h / 0.68f);
            drawWolf(fx - u * 0.025f, base, u, fig);
        }
        else
        {
            drawShirtOnHanger(fx, base, fig_h, fig);
        }

        // its word and what it means; the chosen one says "chosen" and carries a check mark, not only the amber
        // (AMBER on the chosen tile 6.4:1 at its top, 8.7:1 by the word; CREAM on the other 14:1; MUTED on both 9.4:1 or more)
        const F32 bx = r.mRight - (tall ? 18.f : 15.f), by = r.mTop - (tall ? 18.f : 15.f), badge = tall ? 11.f : 9.f;
        if (tall)
        {
            const F32 cx = (F32)r.getCenterX();
            const LLFontGL* wf = fitFont(r.getHeight() * 0.14f, (F32)r.getWidth() - 20.f, { words[0], words[1], words[2] }, 2);
            engraved(wf, words[k], cx, r.mBottom + r.getHeight() * 0.19f, chosen ? AMBER : CREAM, LLFontGL::HCENTER, (F32)r.getWidth() - 12.f);
            const LLFontGL* df = fitFont(llmax(10.f, r.getHeight() * 0.09f), (F32)r.getWidth() - 20.f, { details[0], details[1], details[2], chosen_word }, 0);
            text(df, details[k], cx, r.mBottom + r.getHeight() * 0.08f, MUTED, LLFontGL::HCENTER, (F32)r.getWidth() - 12.f);
            if (chosen) text(df, chosen_word, r.mLeft + 12.f, by, AMBER, LLFontGL::LEFT, (F32)r.getWidth() - 50.f);
        }
        else
        {
            // the words column, clear of the badge in the top right corner
            const F32 tx = r.mLeft + 8.f + box_w + 8.f;
            const F32 tw = llmax(10.f, r.mRight - 8.f - tx);
            const LLFontGL* wf = fitFont(r.getHeight() * 0.26f, tw - badge * 2.f, { words[0], words[1], words[2] }, 2);
            engraved(wf, words[k], tx, r.mBottom + r.getHeight() * 0.66f, chosen ? AMBER : CREAM, LLFontGL::LEFT, tw - badge * 2.f);
            const LLFontGL* df = fitFont(llmax(10.f, r.getHeight() * 0.17f), tw, { details[0], details[1], details[2], chosen_word }, 0);
            text(df, details[k], tx, r.mBottom + r.getHeight() * 0.40f, MUTED, LLFontGL::LEFT, tw);
            if (chosen) text(df, chosen_word, tx, r.mBottom + r.getHeight() * 0.17f, AMBER, LLFontGL::LEFT, tw);
        }
        if (chosen)
        {
            untextured();
            disc(bx, by, badge, lighten(AMBER, 0.1f), SEAL_BOTTOM);
            checkMark(bx, by, badge, 2.f, INK);
        }
    }
}

void LLFloaterWolfAvatars::drawChips() const
{
    // [BASE BODY / CLOTHING / FIGURE 2026-10-20] each set only while its buttons show (refreshClothes)
    const Layout& L = mLayout;
    const std::string chosen_word = getString("chosen");
    if (isPerson() && L.clothes_font)
    {
        drawChip(L.clothes[0], mDressedBtn, !mBase, getString("dressed_word"), chosen_word, L.clothes_font, true);
        drawChip(L.clothes[1], mBaseBtn, mBase, getString("base_word"), chosen_word, L.clothes_font, true);
    }
    if (figureShown() && L.figure_font)
    {
        drawChip(L.figure[0], mWomanBtn, !mMale, getString("woman_word"), chosen_word, L.figure_font, true);
        drawChip(L.figure[1], mManBtn, mMale, getString("man_word"), chosen_word, L.figure_font, true);
    }
    if (isClothing() && L.garment_font)
    {
        const std::vector<std::string> labels = splitList(getString("garment_words"));
        for (S32 i = 0; i < GARMENTS; ++i)
        {
            const std::string label = i < (S32)labels.size() ? labels[i] : std::string(GARMENT_IDS[i]);
            drawChip(L.garment[i], mGarmentBtn[i], i == mGarment, label, chosen_word, L.garment_font, false);
        }
    }
    untextured();
}

void LLFloaterWolfAvatars::drawCircle(F32 cx, F32 cy, F32 r, F32 t, S32 pct, bool spinning, F32 strength, bool figure) const
{
    if (r < 12.f) return;
    untextured();
    const F32 rule = llmax(1.f, r * 0.012f);
    const LLColor4 inner_centre(0.10f, 0.08f, 0.20f, 1.f);

    // the glow on the den floor, the stone ring, the dark well inside it
    disc(cx, cy, r * 1.35f, withAlpha(VIOLET, 0.22f * strength), CLEAR);
    ring(cx, cy, r * 0.80f, r, RING_STONE);
    ring(cx, cy, r * 0.985f, r * 1.005f, withAlpha(SILVER, 0.55f));
    ring(cx, cy, r * 0.80f, r * 0.815f, withAlpha(SILVER, 0.40f));
    disc(cx, cy, r * 0.80f, inner_centre, MAT);

    // a six-pointed star turning slowly the other way, inside a fine circle
    {
        const F32 rr = r * 0.70f, rot = -t * 0.05f;
        const LLColor4 c = withAlpha(VIOLET, 0.55f * strength);
        for (S32 tri_i = 0; tri_i < 2; ++tri_i)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                const F32 a0 = rot + tri_i * F_PI / 3.f + k * F_TWO_PI / 3.f;
                const F32 a1 = a0 + F_TWO_PI / 3.f;
                line(cx + rr * sinf(a0), cy + rr * cosf(a0), cx + rr * sinf(a1), cy + rr * cosf(a1), rule, c);
            }
        }
        ring(cx, cy, rr, rr + rule, withAlpha(SILVER, 0.25f));
    }

    // the runes cut round the stone, turning
    {
        constexpr S32 RUNES = 24;
        const F32 rot = t * RUNE_SPIN;
        const F32 r0 = r * 0.845f, r1 = r * 0.955f, rm = (r0 + r1) * 0.5f, hw = (r1 - r0) * 0.35f;
        const LLColor4 c = withAlpha(SILVER, 0.30f + 0.30f * strength);
        for (S32 k = 0; k < RUNES; ++k)
        {
            const F32 a = rot + F_TWO_PI * k / RUNES;
            const F32 s = sinf(a), q = cosf(a);   // outward
            const F32 ts = q, tq = -s;            // along the ring
            switch (k % 4)
            {
            case 0:   // a stave
                line(cx + r0 * s, cy + r0 * q, cx + r1 * s, cy + r1 * q, rule, c);
                break;
            case 1:   // an arrow
                line(cx + r0 * s, cy + r0 * q, cx + r1 * s, cy + r1 * q, rule, c);
                line(cx + r1 * s, cy + r1 * q, cx + rm * s + hw * ts, cy + rm * q + hw * tq, rule, c);
                line(cx + r1 * s, cy + r1 * q, cx + rm * s - hw * ts, cy + rm * q - hw * tq, rule, c);
                break;
            case 2:   // a cross
                line(cx + r0 * s, cy + r0 * q, cx + r1 * s, cy + r1 * q, rule, c);
                line(cx + rm * s - hw * ts, cy + rm * q - hw * tq, cx + rm * s + hw * ts, cy + rm * q + hw * tq, rule, c);
                break;
            default:  // a diamond
                quad(cx + r0 * s, cy + r0 * q, cx + rm * s + hw * ts, cy + rm * q + hw * tq,
                     cx + r1 * s, cy + r1 * q, cx + rm * s - hw * ts, cy + rm * q - hw * tq, withAlpha(c, 0.8f));
                break;
            }
        }
    }

    // the progress: the glowing arc IS the bar, clockwise from the top; a turning arc while there is no number
    const F32 lit_to = pct >= 0 ? F_TWO_PI * llclamp(pct, 0, 100) / 100.f : 0.f;
    if (pct > 0)
    {
        arcBand(cx, cy, r * 0.78f, r * 1.04f, 0.f, lit_to, withAlpha(AMBER, 0.14f), withAlpha(AMBER, 0.30f));
        arcBand(cx, cy, r * 0.86f, r * 0.95f, 0.f, lit_to, withAlpha(SEAL_BOTTOM, 0.95f), lighten(AMBER, 0.12f));
        const F32 hx = cx + r * 0.905f * sinf(lit_to), hy = cy + r * 0.905f * cosf(lit_to);
        disc(hx, hy, r * 0.10f, withAlpha(lighten(AMBER, 0.25f), 0.85f), CLEAR);
    }
    if (pct >= 0)
    {
        line(cx, cy + r * 0.80f, cx, cy + r * 1.0f, llmax(1.5f, rule * 1.5f), withAlpha(AMBER, 0.9f));   // where it starts
    }
    if (spinning)
    {
        const F32 a0 = fmodf(t * WAIT_SPIN, F_TWO_PI);
        arcBand(cx, cy, r * 0.86f, r * 0.95f, a0, a0 + 1.3f, withAlpha(AMBER, 0.f), withAlpha(AMBER, 0.85f * llmax(0.5f, strength)));
        disc(cx + r * 0.905f * sinf(a0 + 1.3f), cy + r * 0.905f * cosf(a0 + 1.3f), r * 0.08f, withAlpha(lighten(AMBER, 0.25f), 0.8f), CLEAR);
    }

    // sparks drifting round the ring: amber over the lit part, pale elsewhere
    for (S32 i = 0; i < SPARKS; ++i)
    {
        const F32 h1 = hash01(i * 3.17f + 0.3f), h2 = hash01(i * 6.71f + 2.9f), h3 = hash01(i * 9.13f + 5.5f);
        const F32 a = fmodf(h1 * F_TWO_PI + t * (0.20f + 0.45f * h2), F_TWO_PI);
        const F32 rr = r * (0.905f + 0.05f * sinf(t * 1.7f + h3 * 9.f));
        const bool lit = pct > 0 && a < lit_to;
        const F32 twinkle = 0.5f + 0.5f * sinf(t * (3.f + 4.f * h3) + h1 * 30.f);
        const F32 s = r * (0.012f + 0.014f * h3) * (lit ? 1.4f : 1.f);
        const F32 alpha = (lit ? 0.95f : 0.50f) * (0.35f + 0.65f * twinkle) * llmax(0.4f, strength);
        const LLColor4 col = lit ? lighten(AMBER, 0.15f) : SPIRIT;
        const F32 x = cx + rr * sinf(a), y = cy + rr * cosf(a);
        disc(x, y, s * 3.f, withAlpha(col, alpha * 0.35f), CLEAR);
        sparkle(x, y, llmax(0.8f, s), withAlpha(col, alpha));
    }

    // the middle: the number while there is one, otherwise the chosen kind standing in a pool of moonlight
    if (pct >= 0)
    {
        const LLFontGL* f = fitFont(r * 0.40f, r * 1.1f, { "100%" }, 3);
        engraved(f, llformat("%d%%", llclamp(pct, 0, 100)), cx, cy, CREAM);   // CREAM on the dark well: 15:1
        untextured();
    }
    else if (figure)
    {
        disc(cx, cy, r * 0.55f, withAlpha(MOON_GLOW, 0.10f + 0.06f * strength), CLEAR);
        drawFigure(cx, cy, r * 1.0f, lerpColour(inner_centre, SPIRIT, 0.45f + 0.35f * strength));
    }
}

void LLFloaterWolfAvatars::drawPictures(F32 t) const
{
    const Layout& L = mLayout;
    const bool picked = mPicked >= 0 && mPicked < mPicCount;
    drawCircle(L.ring_x, L.ring_y, L.ring_r, t, -1, false, picked ? 1.f : 0.7f, true);
    if (!picked) return;

    // the chosen frame glows, and a beam of light runs from it into the circle with a pulse travelling along it
    const LLRect& f = L.pic[mPicked];
    untextured();
    halo(f, 6.f, 14, withAlpha(AMBER, 0.8f + 0.2f * sinf(t * 2.2f)));
    const bool left = mPicked == 0 || mPicked == 2;
    const F32 ex = left ? (F32)f.mRight : (F32)f.mLeft, ey = (F32)f.getCenterY();
    F32 dx = L.ring_x - ex, dy = L.ring_y - ey;
    const F32 len = sqrtf(dx * dx + dy * dy);
    if (len < L.ring_r + 4.f) return;
    dx /= len;
    dy /= len;
    const F32 end_x = L.ring_x - dx * L.ring_r * 0.80f, end_y = L.ring_y - dy * L.ring_r * 0.80f;
    const F32 nx = -dy, ny = dx;
    const F32 w0 = f.getHeight() * 0.18f, w1 = L.ring_r * 0.14f;
    const LLColor4 near_c = withAlpha(AMBER, 0.30f), far_c = withAlpha(AMBER, 0.12f);
    gGL.begin(LLRender::TRIANGLES);
    vtx(ex + nx * w0, ey + ny * w0, near_c);
    vtx(end_x + nx * w1, end_y + ny * w1, far_c);
    vtx(end_x - nx * w1, end_y - ny * w1, far_c);
    vtx(ex + nx * w0, ey + ny * w0, near_c);
    vtx(end_x - nx * w1, end_y - ny * w1, far_c);
    vtx(ex - nx * w0, ey - ny * w0, near_c);
    gGL.end();
    line(ex, ey, end_x, end_y, 2.5f, withAlpha(lighten(AMBER, 0.15f), 0.75f));
    const F32 k = fmodf(t * 0.7f, 1.f);
    const F32 px = ex + (end_x - ex) * k, py = ey + (end_y - ey) * k;
    disc(px, py, 14.f, withAlpha(AMBER, 0.35f * (1.f - k * 0.5f)), CLEAR);
    disc(px, py, 5.f, withAlpha(lighten(AMBER, 0.3f), 0.95f * (1.f - k * 0.5f)), CLEAR);
}

void LLFloaterWolfAvatars::drawDone(F32 t) const
{
    const Layout& L = mLayout;
    const LLRect& p = L.done_picture;
    if (p.getWidth() < 40) return;
    const F32 cx = (F32)p.getCenterX(), cy = (F32)p.getCenterY();
    const F32 R = p.getWidth() * 0.95f;
    untextured();

    // the light burst: long and short rays turning slowly, a bright core, sparkles round it
    {
        constexpr S32 RAYS = 18;
        const F32 rot = t * 0.10f;
        const F32 half = F_PI / RAYS * 0.45f;
        const LLColor4 core = withAlpha(lighten(AMBER, 0.1f), 0.38f);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 k = 0; k < RAYS; ++k)
        {
            const F32 a = rot + F_TWO_PI * k / RAYS;
            const F32 len = R * ((k & 1) ? 0.72f : 1.f);
            vtx(cx, cy, core);
            vtx(cx + len * sinf(a - half), cy + len * cosf(a - half), CLEAR);
            vtx(cx + len * sinf(a + half), cy + len * cosf(a + half), CLEAR);
        }
        gGL.end();
        disc(cx, cy, R * 0.75f, withAlpha(lighten(AMBER, 0.15f), 0.32f), CLEAR);
        for (S32 i = 0; i < 12; ++i)
        {
            const F32 h1 = hash01(i * 2.71f + 0.9f), h2 = hash01(i * 8.31f + 1.7f);
            const F32 a = h1 * F_TWO_PI + t * 0.15f;
            const F32 rr = R * (0.60f + 0.35f * h2);
            const F32 tw = 0.5f + 0.5f * sinf(t * (2.f + 3.f * h2) + h1 * 25.f);
            sparkle(cx + rr * sinf(a), cy + rr * cosf(a), 1.5f + 1.5f * h2, withAlpha(lighten(AMBER, 0.25f), 0.3f + 0.6f * tw));
        }
    }

    // the picture it was made from, in its gilt frame; after a relog (no picture in this window) the kind in the circle
    const bool have = mPicked >= 0 && mPicked < mPicCount && mPic[mPicked] && mPic[mPicked]->mState == WolfAvatarPicture::S_SHOWN
                      && mPic[mPicked]->mTexture.notNull();
    if (have)
    {
        const F32 fw = llclamp(p.getWidth() * 0.05f, 6.f, 11.f);
        giltFrame(p, fw);
        const LLRect mat(p.mLeft + (S32)fw + 2, p.mTop - (S32)fw - 2, p.mRight - (S32)fw - 2, p.mBottom + (S32)fw + 2);
        drawPictureIn(mat, mPic[mPicked]->mTexture.get());
    }
    else
    {
        drawCircle(cx, cy, p.getWidth() * 0.42f, t, -1, false, 1.f, true);
    }
}

void LLFloaterWolfAvatars::drawPlates() const
{
    const Layout& L = mLayout;
    untextured();
    switch (mStep)
    {
    case STEP_DESCRIBE:
        slate(L.slate);
        if (!mFormError->getText().empty()) errorPlate(L.form_error_plate);
        break;
    case STEP_PRICE:
        brassPlaque(L.plaque);
        slate(L.words_plate);
        break;
    case STEP_LOADING:
    case STEP_WORKING:
        plate(L.words_plate, 8.f, SILVER);
        break;
    case STEP_PICTURES:
        if (!mPicError->getText().empty()) errorPlate(L.pic_error_plate);
        break;
    case STEP_DONE:
        slate(L.done_slate);
        break;
    }
    if (mStopPanel->getVisible() && L.stop_plate.getHeight() > 0)
    {
        halo(L.stop_plate, 8.f, 8, withAlpha(ERR_INK, 0.6f));
        plate(L.stop_plate, 8.f, ERR_INK);
    }
    if (!mError->getText().empty()) errorPlate(L.error_plate);
}

void LLFloaterWolfAvatars::drawButtons(F32 t) const
{
    untextured();
    // the step's main action: a glowing amber seal (SEAL_INK on it 6.4:1 or more); dim stone while it cannot be pressed
    auto seal = [t](LLButton* b)   // not const: LLView::hasMouseCapture() is not (wolffloaterradio.cpp:1329)
    {
        if (!b->isInVisibleChain()) return;
        const LLRect r = b->getRect();
        const bool enabled = b->isInEnabledChain();
        const bool pressed = b->hasMouseCapture() && mouseOver(b);
        const bool hover = enabled && !pressed && mouseOver(b);
        const F32 rad = r.getHeight() * 0.5f;
        if (enabled) halo(r, rad, 10, withAlpha(AMBER, 0.75f + 0.25f * sinf(t * 2.f)));
        LLRect shadow = r;
        shadow.translate(2, -3);
        fillRounded(shadow, allCorners(rad), SHADOW, SHADOW);
        LLColor4 top = enabled ? SEAL_TOP : STONE_OFF, bottom = enabled ? SEAL_BOTTOM : lighten(STONE_OFF, -0.04f);
        if (hover) { top = lighten(top, 0.04f); bottom = lighten(bottom, 0.05f); }
        if (pressed) { top = lighten(top, -0.08f); bottom = lighten(bottom, -0.06f); }
        fillRounded(r, allCorners(rad), top, bottom);
        strokeRounded(r, allCorners(rad), 0.f, 1.5f, enabled ? SEAL_RIM : withAlpha(SILVER, 0.4f), enabled ? SEAL_RIM : withAlpha(SILVER, 0.3f));
        if (enabled)
        {
            strokeRounded(r, allCorners(rad), 3.f, 4.f, withAlpha(LLColor4::white, 0.40f), withAlpha(SEAL_RIM, 0.35f));
            // a sheen across the upper half
            const LLRect sheen(r.mLeft + 5, r.mTop - 3, r.mRight - 5, r.getCenterY());
            fillRounded(sheen, Radii{ rad - 3.f, rad - 3.f, 0.f, 0.f }, withAlpha(LLColor4::white, 0.28f), withAlpha(LLColor4::white, 0.f));
        }
    };
    // the others: moonstone (CREAM on it 8.2:1 or more); Stop is the same stone with a red-amber rim
    auto stone = [](LLButton* b, bool stop)
    {
        if (!b->isInVisibleChain()) return;
        const LLRect r = b->getRect();
        const bool enabled = b->isInEnabledChain();
        const bool pressed = b->hasMouseCapture() && mouseOver(b);
        const bool hover = enabled && !pressed && mouseOver(b);
        LLRect shadow = r;
        shadow.translate(2, -3);
        fillRounded(shadow, allCorners(8.f), SHADOW, SHADOW);
        LLColor4 top = enabled ? STONE_TOP : STONE_OFF, bottom = enabled ? STONE_BOTTOM : lighten(STONE_OFF, -0.04f);
        if (hover) { top = lighten(top, 0.05f); bottom = lighten(bottom, 0.04f); }
        if (pressed) { top = lighten(top, -0.06f); bottom = lighten(bottom, -0.03f); }
        fillRounded(r, allCorners(8.f), top, bottom);
        const LLColor4 rim = stop ? ERR_INK : SILVER;
        strokeRounded(r, allCorners(8.f), 0.f, 1.5f, withAlpha(rim, enabled ? 0.85f : 0.4f), withAlpha(rim, enabled ? 0.55f : 0.3f));
        box(r.mLeft + 8.f, r.mTop - 2.f, r.mRight - 8.f, r.mTop - 3.f, withAlpha(LLColor4::white, 0.22f), withAlpha(LLColor4::white, 0.10f));
    };
    for (LLButton* b : { mPriceBtn, mStartBtn, mPickBtn, mInvBtn, mBuyBtn }) seal(b);
    for (LLButton* b : { mBackBtn, mMoreBtn, mAnotherBtn, mStopNo }) stone(b, false);
    for (LLButton* b : { mWorkStop, mPicStop, mStopYes }) stone(b, true);
}
