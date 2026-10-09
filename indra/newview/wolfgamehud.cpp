/**
 * @file wolfgamehud.cpp
 * @brief WolfViewer: Wolf Roleplay on the world view, and the game UI's drawing kit.
 *        See wolfgamehud.h.
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

#include "wolfgamehud.h"

#include <cmath>
#include <map>

#include "llagent.h"
#include "llavatarnamecache.h"
#include "lllocalcliprect.h"
#include "llnotificationsutil.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llscriptfloater.h"
#include "llstartup.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llviewercamera.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfGameHUD> r_wolf_game_hud("wolf_game_hud");

namespace
{
    const F32 PI_F = 3.14159265f;
    const S32 MARGIN = 6;
    const S32 NOTICE_W = 300;

    // §3.4 kind colours: info blue, success green, warning amber, danger red. The stripe and the
    // icon use the full colour; the kind WORD a lighter one that passes 4.5:1 on either card
    // (#60a5fa 5.1:1, #4ade80 7.3:1, #fbbf24 7.2:1, #f87171 4.7:1 - worst case, over white).
    const LLColor4 KIND_COLOUR[4] = {
        LLColor4(0.231f, 0.510f, 0.965f, 1.f),   // #3b82f6
        LLColor4(0.133f, 0.773f, 0.369f, 1.f),   // #22c55e
        LLColor4(0.961f, 0.620f, 0.043f, 1.f),   // #f59e0b
        LLColor4(0.937f, 0.267f, 0.267f, 1.f),   // #ef4444
    };
    const LLColor4 KIND_TEXT[4] = {
        LLColor4(0.376f, 0.647f, 0.980f, 1.f),   // #60a5fa
        LLColor4(0.290f, 0.871f, 0.502f, 1.f),   // #4ade80
        LLColor4(0.984f, 0.749f, 0.141f, 1.f),   // #fbbf24
        LLColor4(0.973f, 0.443f, 0.443f, 1.f),   // #f87171
    };
    const char* KIND_WORD[4] = { "Info", "Success", "Warning", "Danger" };
    // Danger text on a button: #fca5a5, 7.5:1 on the wolves button, 6.1:1 on the plain one.
    const LLColor4 DANGER_TEXT(0.988f, 0.647f, 0.647f, 1.f);
    const LLColor4 DANGER_EDGE(0.937f, 0.267f, 0.267f, 0.85f);

    void noTex() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }

    void vtx(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

    LLColor4 withAlpha(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    void triF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
        gGL.end();
    }

    void polyF(F32 cx, F32 cy, F32 rad, S32 sides, F32 rot, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < sides; ++i)
        {
            const F32 a0 = rot + 2.f * PI_F * i / sides, a1 = rot + 2.f * PI_F * (i + 1) / sides;
            vtx(cx, cy, c);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }

    void ellipseF(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        const S32 seg = 20;
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + rx * cosf(a0), cy + ry * sinf(a0), c);
            vtx(cx + rx * cosf(a1), cy + ry * sinf(a1), c);
        }
        gGL.end();
    }

    /** A soft glow: a disc fading from `c` at the centre to clear at `r`. */
    void glow(F32 cx, F32 cy, F32 r, const LLColor4& c)
    {
        const LLColor4 clear = withAlpha(c, 0.f);
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        const S32 seg = 40;
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + r * cosf(a0), cy + r * sinf(a0), clear);
            vtx(cx + r * cosf(a1), cy + r * sinf(a1), clear);
        }
        gGL.end();
    }

    /**
     * An edge-to-centre fade: four trapezoids between the outer rectangle and one inset by
     * `depth`, opaque `c` at the edge and clear inside. No overlap at the corners.
     */
    void vignette(const LLRect& r, F32 depth, const LLColor4& c)
    {
        const F32 l = (F32)r.mLeft, b = (F32)r.mBottom, rr = (F32)r.mRight, t = (F32)r.mTop;
        const F32 il = l + depth, ib = b + depth, ir = rr - depth, it = t - depth;
        const LLColor4 clear = withAlpha(c, 0.f);
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(rr, b, c); vtx(ir, ib, clear);
        vtx(l, b, c); vtx(ir, ib, clear); vtx(il, ib, clear);
        vtx(l, t, c); vtx(il, it, clear); vtx(ir, it, clear);
        vtx(l, t, c); vtx(ir, it, clear); vtx(rr, t, c);
        vtx(l, b, c); vtx(il, ib, clear); vtx(il, it, clear);
        vtx(l, b, c); vtx(il, it, clear); vtx(l, t, c);
        vtx(rr, b, c); vtx(rr, t, c); vtx(ir, it, clear);
        vtx(rr, b, c); vtx(ir, it, clear); vtx(ir, ib, clear);
        gGL.end();
    }

    F32 relLum(const LLColor4& c)
    {
        auto lin = [](F32 v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * lin(c.mV[VRED]) + 0.7152f * lin(c.mV[VGREEN]) + 0.0722f * lin(c.mV[VBLUE]);
    }

    /** The eight corners of the chamfered rectangle, counter-clockwise from the bottom edge. */
    void chamferPoints(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 (&x)[8], F32 (&y)[8])
    {
        cut = llmax(0.f, llmin(cut, (r - l) * 0.5f, (t - b) * 0.5f));
        const F32 px[8] = { l + cut, r - cut, r, r, r - cut, l + cut, l, l };
        const F32 py[8] = { b, b, b + cut, t - cut, t, t, t - cut, b + cut };
        for (S32 i = 0; i < 8; ++i) { x[i] = px[i]; y[i] = py[i]; }
    }

    /** The kind's icon in a 16 px square centred on (cx, cy) - shape AND word carry the kind. */
    void kindIcon(WolfGame::ENoticeKind k, F32 cx, F32 cy)
    {
        const LLColor4& c = KIND_COLOUR[k];
        const LLColor4 ink(0.05f, 0.05f, 0.05f, 1.f);
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        switch (k)
        {
        case WolfGame::NOTICE_SUCCESS:
            polyF(cx, cy, 8.f, 24, 0.f, c);
            WolfGameDraw::lineW(cx - 4.f, cy, cx - 1.f, cy - 3.f, 2.f, ink);
            WolfGameDraw::lineW(cx - 1.f, cy - 3.f, cx + 4.f, cy + 3.5f, 2.f, ink);
            break;
        case WolfGame::NOTICE_WARNING:
            triF(cx - 8.f, cy - 7.f, cx + 8.f, cy - 7.f, cx, cy + 8.f, c);
            bold->renderUTF8("!", 0, cx, cy - 6.f, ink, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            break;
        case WolfGame::NOTICE_DANGER:
            polyF(cx, cy, 8.5f, 8, PI_F / 8.f, c);
            bold->renderUTF8("!", 0, cx, cy, LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            break;
        default:
            polyF(cx, cy, 8.f, 24, 0.f, c);
            bold->renderUTF8("i", 0, cx, cy, LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            break;
        }
    }

    /** A cheap, repeatable hash for the stars (the same sky every frame). */
    F32 hash01(U32 n)
    {
        n = (n << 13) ^ n;
        n = n * (n * n * 15731u + 789221u) + 1376312589u;
        return (F32)(n & 0x7fffffffu) / 2147483647.f;
    }

    // ---- the howling wolf ----
    // A sitting wolf, facing left, muzzle raised. Points on a 100 x 100 grid, y up, traced by
    // hand and checked as a picture before use (the same outline is WolfStorm's SVG path,
    // js/ui/wolf_game_hud.js WOLF_PATH).
    const F32 WOLF_PTS[][2] = {
        {30,0},{41,0},{42,4},{41,20},{46,14},{52,8},{58,3},{84,1},{97,3},{99,8},{90,9},{82,12},{84,24},
        {81,38},{73,50},{62,58},{55,64},{51,70},{50,78},{55,90},{48,85},{47,92},{43,86},{36,88},{27,94},
        {17,100},{15,98},{22,90},{14,91},{16,88},{26,82},{32,78},{31,70},{28,66},{31,62},{28,56},{32,50},
        {33,36},{31,20},{30,6}
    };
    const S32 WOLF_N = (S32)(sizeof(WOLF_PTS) / sizeof(WOLF_PTS[0]));

    /** Ear clipping, once: the outline is concave (legs, ears, the open jaw). */
    const std::vector<S32>& wolfTriangles()
    {
        static std::vector<S32> tris;
        if (!tris.empty()) return tris;
        std::vector<S32> idx;
        F32 area = 0.f;
        for (S32 i = 0; i < WOLF_N; ++i)
        {
            const S32 j = (i + 1) % WOLF_N;
            area += WOLF_PTS[i][0] * WOLF_PTS[j][1] - WOLF_PTS[j][0] * WOLF_PTS[i][1];
        }
        for (S32 i = 0; i < WOLF_N; ++i) idx.push_back(area > 0.f ? i : WOLF_N - 1 - i);   // counter-clockwise
        auto cross = [](const F32* a, const F32* b, const F32* c)
        {
            return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        };
        S32 guard = 0;
        while (idx.size() > 3 && guard++ < 10000)
        {
            bool clipped = false;
            const S32 n = (S32)idx.size();
            for (S32 i = 0; i < n; ++i)
            {
                const F32* a = WOLF_PTS[idx[(i + n - 1) % n]];
                const F32* b = WOLF_PTS[idx[i]];
                const F32* c = WOLF_PTS[idx[(i + 1) % n]];
                if (cross(a, b, c) <= 0.f) continue;   // reflex: not an ear
                bool inside = false;
                for (S32 k = 0; k < n && !inside; ++k)
                {
                    const S32 v = idx[k];
                    if (v == idx[(i + n - 1) % n] || v == idx[i] || v == idx[(i + 1) % n]) continue;
                    const F32* p = WOLF_PTS[v];
                    inside = cross(a, b, p) >= 0.f && cross(b, c, p) >= 0.f && cross(c, a, p) >= 0.f;
                }
                if (inside) continue;
                tris.push_back(idx[(i + n - 1) % n]);
                tris.push_back(idx[i]);
                tris.push_back(idx[(i + 1) % n]);
                idx.erase(idx.begin() + i);
                clipped = true;
                break;
            }
            if (!clipped) break;   // degenerate: draw what was found
        }
        if (idx.size() == 3)
        {
            tris.push_back(idx[0]); tris.push_back(idx[1]); tris.push_back(idx[2]);
        }
        return tris;
    }

    // The textures the game UI holds: released before the texture list goes at shutdown
    // (WolfGameDraw::releaseTextures, from WolfGame::releaseTags in llappviewer.cpp cleanup).
    LLPointer<LLViewerFetchedTexture> s_logo_tex;
    std::string s_logo_id;
    std::map<std::string, LLPointer<LLViewerFetchedTexture>> s_icons;
    std::map<std::string, LLPointer<LLUIImage>> s_photos;

    // ---- the game's logo, fetched once per id ----
    LLViewerTexture* gameLogo()
    {
        LLPointer<LLViewerFetchedTexture>& s_tex = s_logo_tex;
        std::string& s_id = s_logo_id;
        const std::string& logo = WolfGame::instance().game().mLogo;   // a valid, non-null UUID or "" (wolfgame.cpp)
        if (logo.empty())
        {
            return nullptr;
        }
        if (logo != s_id || s_tex.isNull())
        {
            s_id = logo;
            // Source: wolfmapoverlays.cpp - how a texture by asset id is fetched for UI drawing.
            s_tex = LLViewerTextureManager::getFetchedTexture(LLUUID(logo), FTT_DEFAULT, MIPMAP_TRUE,
                                                              LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE);
        }
        return s_tex.get();
    }
}

namespace WolfGameDraw
{
    /**
     * [WOLF GAME ICONS 2026-10-09] An icon texture by UUID, fetched once and kept. Bounded: past
     * 256 different icons the cache starts again (a game has a few dozen; a script cannot grow
     * this without limit).
     */
    LLViewerTexture* iconTexture(const std::string& id)
    {
        auto it = s_icons.find(id);
        if (it != s_icons.end()) return it->second.get();
        if (s_icons.size() >= 256) s_icons.clear();
        // Source: wolfmapoverlays.cpp - how a texture by asset id is fetched for UI drawing.
        LLPointer<LLViewerFetchedTexture> tex = LLViewerTextureManager::getFetchedTexture(LLUUID(id), FTT_DEFAULT, MIPMAP_TRUE,
                                                                                          LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE);
        s_icons[id] = tex;
        return tex.get();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// palettes
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfGameDraw
{
    // CONTRAST (VIEWER_SPEC.md §3, at least 4.5:1), worked against the WORST case: a white sky or
    // a white picture showing through the translucent plates (WCAG 2.x relative luminance).
    //   wolves card (#0f1626 at 0.86): silver #e6ebf5 10.0, dim #aab4c8 5.7, amber #f5a524 5.9
    //   wolves base (#0f1626 at 0.94): silver 13.0, dim 7.4, amber 7.6
    //   plain card (0.10 grey at 0.88): text 0.95 grey 10.9, dim 0.70 grey 5.8, ice #7cc4ff 6.5
    //   plain base (0.125 grey at 0.92): text 11.5, dim 6.1, ice 6.9
    //   buttons: silver on #1e2a44 12.0, amber 7.0; plain text on 0.22 grey 10.4, ice 6.2
    //   on an accent fill: night on amber 8.8, near-black on ice 9.8; white on the Down chip 6.5
    //   the Unverified word #fbbf24: 7.2 on the wolves card, 7.3 on the plain one
    // Fixed colours, not the skin's, so no skin can lower them.
    const LLColor4 WOLF_NIGHT(15.f / 255.f, 22.f / 255.f, 38.f / 255.f, 0.94f);
    const LLColor4 WOLF_NIGHT_2(20.f / 255.f, 29.f / 255.f, 49.f / 255.f, 1.f);
    const LLColor4 WOLF_SILVER(230.f / 255.f, 235.f / 255.f, 245.f / 255.f, 1.f);
    const LLColor4 WOLF_SILVER_DIM(170.f / 255.f, 180.f / 255.f, 200.f / 255.f, 1.f);
    const LLColor4 WOLF_AMBER(245.f / 255.f, 165.f / 255.f, 36.f / 255.f, 1.f);
    const LLColor4 CHIP_FILL(0.725f, 0.110f, 0.110f, 1.f);

    const Palette& wolvesPalette()
    {
        static Palette p;
        static bool init = false;
        if (!init)
        {
            init = true;
            p.mWolves = true;
            p.mBaseTop = LLColor4(20.f / 255.f, 29.f / 255.f, 49.f / 255.f, 0.94f);
            p.mBaseBottom = LLColor4(9.f / 255.f, 13.f / 255.f, 24.f / 255.f, 0.96f);
            p.mCard = LLColor4(15.f / 255.f, 22.f / 255.f, 38.f / 255.f, 0.86f);
            p.mText = WOLF_SILVER;
            p.mDim = WOLF_SILVER_DIM;
            p.mAccent = WOLF_AMBER;
            p.mEdge = LLColor4(WOLF_AMBER.mV[VRED], WOLF_AMBER.mV[VGREEN], WOLF_AMBER.mV[VBLUE], 0.55f);
            p.mTrack = LLColor4(0.17f, 0.22f, 0.32f, 1.f);
            p.mButtonTop = LLColor4(38.f / 255.f, 52.f / 255.f, 82.f / 255.f, 1.f);
            p.mButtonBottom = LLColor4(30.f / 255.f, 42.f / 255.f, 68.f / 255.f, 1.f);   // #1e2a44
            p.mButtonEdge = LLColor4(170.f / 255.f, 180.f / 255.f, 200.f / 255.f, 0.45f);
            p.mOnAccent = LLColor4(15.f / 255.f, 22.f / 255.f, 38.f / 255.f, 1.f);
        }
        return p;
    }

    const Palette& plainPalette()
    {
        static Palette p;
        static bool init = false;
        if (!init)
        {
            init = true;
            p.mWolves = false;
            p.mBaseTop = LLColor4(0.150f, 0.155f, 0.165f, 0.92f);
            p.mBaseBottom = LLColor4(0.090f, 0.092f, 0.100f, 0.94f);
            p.mCard = LLColor4(0.10f, 0.10f, 0.11f, 0.88f);
            p.mText = LLColor4(0.95f, 0.95f, 0.95f, 1.f);
            p.mDim = LLColor4(0.70f, 0.70f, 0.70f, 1.f);
            p.mAccent = LLColor4(124.f / 255.f, 196.f / 255.f, 1.f, 1.f);   // #7cc4ff
            p.mEdge = LLColor4(124.f / 255.f, 196.f / 255.f, 1.f, 0.45f);
            p.mTrack = LLColor4(0.25f, 0.25f, 0.27f, 1.f);
            p.mButtonTop = LLColor4(0.27f, 0.27f, 0.29f, 1.f);
            p.mButtonBottom = LLColor4(0.22f, 0.22f, 0.24f, 1.f);
            p.mButtonEdge = LLColor4(0.70f, 0.70f, 0.72f, 0.45f);
            p.mOnAccent = LLColor4(0.06f, 0.08f, 0.11f, 1.f);
        }
        return p;
    }

    const Palette& gamePalette()
    {
        return WolfGame::instance().wolvesTheme() ? wolvesPalette() : plainPalette();
    }

    const Palette& palette(bool themed)
    {
        return themed && WolfGame::instance().wolvesTheme() ? wolvesPalette() : plainPalette();
    }

    // ---- lettering ----

    const LLFontGL* display(F32 px)
    {
        // Source: wolfflightdeck.cpp avionics() - a family registered at the Avionics0..11 sizes
        // (fonts.xml), the largest whose line fits.
        static std::vector<const LLFontGL*> sizes;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            for (S32 i = 0; i < 12; ++i)
            {
                const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("WolfDisplay", llformat("Avionics%d", i), LLFontGL::NORMAL));
                if (f) sizes.push_back(f);
            }
            if (sizes.empty())
            {
                LL_WARNS("WolfGame") << "font WolfDisplay missing, using the bold sans" << LL_ENDL;
            }
        }
        if (sizes.empty()) return LLFontGL::getFontSansSerifBold();
        const LLFontGL* best = sizes.front();
        for (const LLFontGL* f : sizes)
        {
            if ((F32)f->getLineHeight() <= px) best = f;
        }
        return best;
    }

    F32 textL(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h, LLFontGL::VAlign v, F32 max_w)
    {
        if (!f || s.empty()) return x;
        F32 right = x;
        const S32 max_px = max_w > 0.f ? llmax(1, (S32)max_w) : S32_MAX;
        f->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, max_px, &right, max_w > 0.f);
        return right;
    }

    std::string caps(const std::string& s)
    {
        std::string out = s;
        for (char& ch : out)
        {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        }
        return out;
    }

    // ---- shapes ----

    void rect(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        if (r <= l || t <= b) return;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(r, b, c); vtx(r, t, c);
        vtx(l, b, c); vtx(r, t, c); vtx(l, t, c);
        gGL.end();
    }

    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bottom)
    {
        if (r <= l || t <= b) return;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bottom); vtx(r, b, bottom); vtx(r, t, top);
        vtx(l, b, bottom); vtx(r, t, top); vtx(l, t, top);
        gGL.end();
    }

    void lineW(F32 x1, F32 y1, F32 x2, F32 y2, F32 w, const LLColor4& c)
    {
        const F32 dx = x2 - x1, dy = y2 - y1;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1 + nx, y1 + ny, c); vtx(x1 - nx, y1 - ny, c); vtx(x2 - nx, y2 - ny, c);
        vtx(x1 + nx, y1 + ny, c); vtx(x2 - nx, y2 - ny, c); vtx(x2 + nx, y2 + ny, c);
        gGL.end();
    }

    void disc(F32 cx, F32 cy, F32 r, const LLColor4& c, S32 seg)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + r * cosf(a0), cy + r * sinf(a0), c);
            vtx(cx + r * cosf(a1), cy + r * sinf(a1), c);
        }
        gGL.end();
    }

    void arc(F32 cx, F32 cy, F32 r, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg)
    {
        if (w <= 0.f) return;
        noTex();
        const F32 ri = r - w * 0.5f, ro = r + w * 0.5f;
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

    void chamfer(F32 l, F32 b, F32 r, F32 t, F32 cut, const LLColor4& top, const LLColor4& bottom)
    {
        if (r <= l || t <= b) return;
        F32 x[8], y[8];
        chamferPoints(l, b, r, t, cut, x, y);
        const F32 cx = (l + r) * 0.5f, cy = (b + t) * 0.5f;
        auto col = [&](F32 py) { return lerp(bottom, top, llclamp((py - b) / (t - b), 0.f, 1.f)); };
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < 8; ++i)
        {
            const S32 j = (i + 1) % 8;
            vtx(cx, cy, col(cy)); vtx(x[i], y[i], col(y[i])); vtx(x[j], y[j], col(y[j]));
        }
        gGL.end();
    }

    void chamferEdge(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 w, const LLColor4& c)
    {
        F32 x[8], y[8];
        chamferPoints(l, b, r, t, cut, x, y);
        for (S32 i = 0; i < 8; ++i)
        {
            const S32 j = (i + 1) % 8;
            lineW(x[i], y[i], x[j], y[j], w, c);
        }
    }

    void frame(F32 l, F32 b, F32 r, F32 t, const Palette& p, F32 alpha, F32 cut, bool fill)
    {
        // A soft outer glow in the accent, then the plate, then the hairline and the brackets.
        chamferEdge(l - 2.f, b - 2.f, r + 2.f, t + 2.f, cut + 1.f, 4.f, withAlpha(p.mAccent, 0.10f * alpha));
        if (fill)
        {
            chamfer(l, b, r, t, cut, withAlpha(p.mBaseTop, alpha), withAlpha(p.mBaseBottom, alpha));
        }
        chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, cut, 1.f, withAlpha(p.mEdge, alpha));
        // A faint lit line just inside the top edge, as light on a bevel.
        rect(l + cut, t - 2.f, r - cut, t - 1.f, LLColor4(1.f, 1.f, 1.f, 0.07f * alpha));
        // Corner brackets: the cut corners drawn bright, with short arms along each edge.
        const F32 arm = llmin(14.f, (r - l) * 0.12f);
        const LLColor4 br = withAlpha(p.mAccent, 0.95f * alpha);
        const F32 w = 2.f;
        // top-left
        lineW(l + 1.f, t - cut, l + cut, t - 1.f, w, br);
        lineW(l + 1.f, t - cut, l + 1.f, t - cut - arm, w, br);
        lineW(l + cut, t - 1.f, l + cut + arm, t - 1.f, w, br);
        // top-right
        lineW(r - 1.f, t - cut, r - cut, t - 1.f, w, br);
        lineW(r - 1.f, t - cut, r - 1.f, t - cut - arm, w, br);
        lineW(r - cut, t - 1.f, r - cut - arm, t - 1.f, w, br);
        // bottom-left
        lineW(l + 1.f, b + cut, l + cut, b + 1.f, w, br);
        lineW(l + 1.f, b + cut, l + 1.f, b + cut + arm, w, br);
        lineW(l + cut, b + 1.f, l + cut + arm, b + 1.f, w, br);
        // bottom-right
        lineW(r - 1.f, b + cut, r - cut, b + 1.f, w, br);
        lineW(r - 1.f, b + cut, r - 1.f, b + cut + arm, w, br);
        lineW(r - cut, b + 1.f, r - cut - arm, b + 1.f, w, br);
    }

    void card(F32 l, F32 b, F32 r, F32 t, const Palette& p, F32 cut)
    {
        chamfer(l, b, r, t, cut, p.mCard, p.mCard);
        chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, cut, 1.f, withAlpha(p.mDim, 0.22f));
    }

    void gauge(F32 cx, F32 cy, F32 r, F32 w, F32 frac, const LLColor4& colour, const LLColor4& track)
    {
        // 270 degrees, open at the bottom: from the lower left (225) clockwise to the lower right (-45).
        const F32 a0 = 1.25f * PI_F, sweep = 1.5f * PI_F;
        arc(cx, cy, r, w, a0, a0 - sweep, track, 54);
        frac = llclamp(frac, 0.f, 1.f);
        if (frac <= 0.f) return;
        LLColor4 c = colour;
        c.mV[VALPHA] = 1.f;
        const F32 a1 = a0 - sweep * frac;
        const S32 seg = llmax(4, (S32)(54 * frac));
        arc(cx, cy, r, w + 3.f, a0, a1, withAlpha(c, 0.18f), seg);   // the glow under it
        arc(cx, cy, r, w, a0, a1, c, seg);
        // the bright leading tick
        const LLColor4 tip = readable(c, 0.6f);
        lineW(cx + (r - w * 0.7f) * cosf(a1), cy + (r - w * 0.7f) * sinf(a1),
              cx + (r + w * 0.7f) * cosf(a1), cy + (r + w * 0.7f) * sinf(a1), 2.f, tip);
    }

    void button(F32 l, F32 b, F32 r, F32 t, const std::string& label, const Palette& p, U32 flags, const LLFontGL* font)
    {
        if (!font) font = LLFontGL::getFontSansSerifSmallBold();
        const bool disabled = (flags & BTN_DISABLED) != 0;
        const bool primary = (flags & BTN_PRIMARY) != 0;
        const bool danger = (flags & BTN_DANGER) != 0;
        const bool hover = !disabled && (flags & BTN_HOVER) != 0;
        const bool pressed = !disabled && (flags & BTN_PRESSED) != 0;
        const F32 cut = llmin(6.f, (t - b) * 0.3f);
        const F32 fade = disabled ? 0.5f : 1.f;
        LLColor4 top = primary ? readable(p.mAccent, 0.5f) : p.mButtonTop;
        LLColor4 bot = primary ? p.mAccent : p.mButtonBottom;
        if (pressed) std::swap(top, bot);
        chamfer(l, b, r, t, cut, withAlpha(top, fade), withAlpha(bot, fade));
        LLColor4 edge = danger ? DANGER_EDGE : (hover || (flags & BTN_ACTIVE) ? p.mAccent : p.mButtonEdge);
        chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, cut, hover ? 1.5f : 1.f, withAlpha(edge, fade));
        if (hover && !primary)
        {
            rect(l + cut, t - 2.f, r - cut, t - 1.f, withAlpha(p.mAccent, 0.5f));
        }
        if (flags & BTN_ACTIVE)
        {
            rect(l + cut, b + 1.f, r - cut, b + 3.f, p.mAccent);
        }
        // Disabled text keeps 3:1 (XAG 102 for disabled elements); everything else 4.5:1 or more.
        LLColor4 ink = primary ? p.mOnAccent : (danger ? DANGER_TEXT : p.mText);
        if (disabled) ink = p.mDim;
        textL(font, label, (l + r) * 0.5f, (b + t) * 0.5f + 1.f, ink, LLFontGL::HCENTER, LLFontGL::VCENTER,
              llmax(8.f, r - l - 10.f));
        if (flags & BTN_FOCUS) focusRing(l, b, r, t, p);
    }

    void focusRing(F32 l, F32 b, F32 r, F32 t, const Palette& p)
    {
        chamferEdge(l - 3.f, b - 3.f, r + 3.f, t + 3.f, 7.f, 2.f, p.mAccent);
    }

    void closeX(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        lineW(cx - s, cy - s, cx + s, cy + s, 1.8f, c);
        lineW(cx - s, cy + s, cx + s, cy - s, 1.8f, c);
    }

    void chevron(F32 cx, F32 cy, bool down, const LLColor4& c)
    {
        if (down)
        {
            lineW(cx - 5.f, cy + 2.f, cx, cy - 3.f, 1.6f, c);
            lineW(cx, cy - 3.f, cx + 5.f, cy + 2.f, 1.6f, c);
        }
        else
        {
            lineW(cx - 5.f, cy - 2.f, cx, cy + 3.f, 1.6f, c);
            lineW(cx, cy + 3.f, cx + 5.f, cy - 2.f, 1.6f, c);
        }
    }

    // ---- emblems ----

    void paw(F32 cx, F32 cy, F32 size, const LLColor4& c)
    {
        // The same paw as the toolbar icon (toolbar_icons/wolf_gamemode.png, drawn on an 18 px grid).
        const F32 u = size / 18.f;
        auto at = [&](F32 gx, F32 gy, F32 rx, F32 ry) { ellipseF(cx + (gx - 9.f) * u, cy - (gy - 9.f) * u, rx * u, ry * u, c); };
        at(9.f, 12.8f, 4.0f, 3.4f);
        at(3.4f, 8.0f, 1.5f, 1.9f);
        at(6.9f, 4.4f, 1.55f, 2.0f);
        at(11.1f, 4.4f, 1.55f, 2.0f);
        at(14.6f, 8.0f, 1.5f, 1.9f);
    }

    void crescent(F32 cx, F32 cy, F32 r, const LLColor4& c, const LLColor4& bg)
    {
        LLColor4 cut = bg;
        cut.mV[VALPHA] = 1.f;
        ellipseF(cx, cy, r, r, c);
        ellipseF(cx + r * 0.45f, cy + r * 0.25f, r * 0.85f, r * 0.85f, cut);
    }

    void wolf(F32 l, F32 b, F32 size, const LLColor4& c)
    {
        const std::vector<S32>& tris = wolfTriangles();
        const F32 u = size / 100.f;
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                const F32* pt = WOLF_PTS[tris[i + k]];
                vtx(l + pt[0] * u, b + pt[1] * u, c);
            }
        }
        gGL.end();
    }

    void coin(F32 cx, F32 cy, F32 r, const Palette& p)
    {
        // A gold coin whatever the theme: currency reads as gold in every game players know.
        const LLColor4 gold_hi(1.0f, 0.86f, 0.45f, 1.f), gold(0.92f, 0.66f, 0.16f, 1.f), gold_lo(0.55f, 0.36f, 0.06f, 1.f);
        disc(cx, cy - 1.f, r, withAlpha(LLColor4::black, 0.45f), 24);
        disc(cx, cy, r, gold_lo, 24);
        disc(cx - r * 0.08f, cy + r * 0.08f, r * 0.86f, gold, 24);
        arc(cx, cy, r * 0.70f, llmax(1.f, r * 0.10f), 0.f, 2.f * PI_F, gold_hi, 24);
        paw(cx, cy, r * 1.0f, gold_lo);
    }

    void bag(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        // A drawstring pouch: a round body, a pinched neck, the tied top.
        const F32 u = s / 16.f;
        ellipseF(cx, cy - 2.f * u, 6.5f * u, 5.5f * u, c);
        triF(cx - 3.f * u, cy + 3.f * u, cx + 3.f * u, cy + 3.f * u, cx, cy - 1.f * u, c);
        ellipseF(cx, cy + 4.f * u, 3.2f * u, 1.3f * u, c);
        triF(cx - 1.f * u, cy + 5.f * u, cx - 4.f * u, cy + 8.f * u, cx - 2.f * u, cy + 5.f * u, c);
        triF(cx + 1.f * u, cy + 5.f * u, cx + 4.f * u, cy + 8.f * u, cx + 2.f * u, cy + 5.f * u, c);
    }

    bool isTextureIcon(const std::string& icon)
    {
        return icon.size() == 36 && LLUUID::validate(icon);
    }

    void icon(const std::string& ic, F32 l, F32 b, F32 size, const Palette& p, const std::string& fallback, bool framed)
    {
        const F32 cut = framed ? llmin(5.f, size * 0.15f) : 0.f;
        if (framed)
        {
            // A recessed slot: dark well, a lit lower edge, the palette's hairline.
            chamfer(l, b, l + size, b + size, cut, LLColor4(0.f, 0.f, 0.f, 0.55f), LLColor4(0.04f, 0.05f, 0.08f, 0.75f));
            chamferEdge(l + 0.5f, b + 0.5f, l + size - 0.5f, b + size - 0.5f, cut, 1.f, withAlpha(p.mDim, 0.35f));
        }
        const F32 in = framed ? 3.f : 0.f;
        if (isTextureIcon(ic))
        {
            if (!imageCover(iconTexture(ic), l + in, b + in, l + size - in, b + size - in, cut * 0.6f, LLColor4::white) && !framed)
            {
                // Loading, with no slot round it: the neutral frame stands in.
                chamferEdge(l + 0.5f, b + 0.5f, l + size - 0.5f, b + size - 0.5f, llmin(4.f, size * 0.15f), 1.f, withAlpha(p.mDim, 0.5f));
            }
            return;
        }
        const std::string& glyph = !ic.empty() ? ic : fallback;
        if (glyph.empty()) return;
        // The largest face whose line and width fit the box.
        const LLFontGL* f = display(size * 0.8f);
        for (F32 px = size * 0.8f; px > 6.f && f->getWidthF32(glyph) > size - 2.f * in - 2.f; px -= 2.f) f = display(px);
        textL(f, glyph, l + size * 0.5f, b + size * 0.5f, ic.empty() ? p.mDim : p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER,
              size - 2.f * in);
    }

    void releaseTextures()
    {
        s_logo_tex = nullptr;
        s_logo_id.clear();
        s_icons.clear();
        s_photos.clear();
    }

    LLViewerTexture* photo(const std::string& name)
    {
        // Source: llui.h LLUI::getUIImage - a skin texture by its textures.xml name; the UI image
        // keeps the texture, so it is held here for the session.
        auto it = s_photos.find(name);
        if (it == s_photos.end()) it = s_photos.emplace(name, LLUI::getUIImage(name)).first;
        if (it->second.isNull()) return nullptr;
        return dynamic_cast<LLViewerTexture*>(it->second->getImage().get());
    }

    void walletIcon(F32 cx, F32 cy, F32 r, const Palette& p)
    {
        const std::string& ci = WolfGame::instance().game().mCurrencyIcon;
        if (isTextureIcon(ci) && imageCover(iconTexture(ci), cx - r, cy - r, cx + r, cy + r, 0.f, LLColor4::white)) return;
        coin(cx, cy, r, p);
    }

    void unverifiedMark(F32 left, F32 top, const LLFontGL* font, F32* right_out)
    {
        // An amber caution triangle with "!" and the word "Unverified" (#fbbf24, 7.2:1 or better).
        const F32 lh = (F32)font->getLineHeight();
        const F32 cy = top - lh * 0.5f, s = lh * 0.5f;
        triF(left, cy - s, left + 2.f * s, cy - s, left + s, cy + s, KIND_COLOUR[WolfGame::NOTICE_WARNING]);
        LLFontGL::getFontSansSerifSmallBold()->renderUTF8("!", 0, left + s, cy - s + 1.f, LLColor4(0.05f, 0.05f, 0.05f, 1.f),
                                                           LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        F32 right = left + 2.f * s + 4.f;
        LLFontGL::getFontSansSerifSmallBold()->renderUTF8("Unverified", 0, right, top, KIND_TEXT[WolfGame::NOTICE_WARNING],
                                                           LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                                           S32_MAX, S32_MAX, &right);
        if (right_out) *right_out = right;
    }

    // ---- backgrounds ----

    void nightScene(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 alpha)
    {
        if (alpha <= 0.f || r <= l || t <= b) return;
        const F32 w = r - l, h = t - b, m = llmin(w, h);
        // the sky, deep at the top, a little lighter toward the horizon
        chamfer(l, b, r, t, cut, LLColor4(0.020f, 0.035f, 0.075f, alpha), LLColor4(0.075f, 0.110f, 0.200f, alpha));
        // stars: the same sky every frame (hash of the index), kept off the cut corners
        for (U32 i = 0; i < 90; ++i)
        {
            const F32 sx = l + cut + (w - 2.f * cut) * hash01(i * 3u + 1u);
            const F32 sy = b + h * (0.35f + 0.65f * hash01(i * 3u + 2u));
            if (sy > t - 2.f) continue;
            const F32 k = hash01(i * 3u + 3u);
            const F32 size = 0.6f + 1.2f * k * k;
            disc(sx, sy, size, LLColor4(0.85f, 0.90f, 1.f, alpha * (0.35f + 0.6f * k)), 8);
        }
        // the moon, upper left, the wolf howls at it
        const F32 mr = m * 0.13f, mx = l + w * 0.28f, my = t - h * 0.26f;
        glow(mx, my, mr * 3.2f, LLColor4(0.70f, 0.78f, 0.95f, 0.22f * alpha));
        glow(mx, my, mr * 1.7f, LLColor4(0.85f, 0.90f, 1.00f, 0.30f * alpha));
        disc(mx, my, mr, LLColor4(0.92f, 0.94f, 0.98f, alpha), 48);
        // the moon's seas, a few soft greys
        disc(mx - mr * 0.30f, my + mr * 0.20f, mr * 0.28f, LLColor4(0.78f, 0.81f, 0.88f, alpha), 20);
        disc(mx + mr * 0.25f, my - mr * 0.25f, mr * 0.20f, LLColor4(0.80f, 0.83f, 0.90f, alpha), 20);
        disc(mx + mr * 0.30f, my + mr * 0.35f, mr * 0.12f, LLColor4(0.82f, 0.85f, 0.91f, alpha), 16);
        // a low mist band over the far hills
        rectG(l, b + h * 0.18f, r, b + h * 0.36f, LLColor4(0.45f, 0.55f, 0.75f, 0.f), LLColor4(0.45f, 0.55f, 0.75f, 0.10f * alpha));
        // the far ridge of pines, then the near one, darker
        const LLColor4 far_c(0.040f, 0.060f, 0.110f, alpha), near_c(0.012f, 0.018f, 0.035f, alpha);
        for (S32 layer = 0; layer < 2; ++layer)
        {
            const LLColor4& c = layer == 0 ? far_c : near_c;
            const F32 base = b + h * (layer == 0 ? 0.20f : 0.08f);
            const F32 tall = h * (layer == 0 ? 0.12f : 0.17f);
            rect(l + cut * 0.5f, b + cut * 0.5f, r - cut * 0.5f, base, c);
            const S32 trees = llmax(6, (S32)(w / (layer == 0 ? 18.f : 26.f)));
            for (S32 i = 0; i <= trees; ++i)
            {
                const F32 tx = l + w * ((F32)i + 0.5f * hash01(i * 7u + 11u + layer * 97u)) / (F32)trees;
                const F32 th = tall * (0.55f + 0.45f * hash01(i * 5u + 3u + layer * 31u));
                const F32 tw = th * 0.42f;
                if (tx - tw < l + cut * 0.5f || tx + tw > r - cut * 0.5f) continue;
                // a pine: three stacked tiers
                for (S32 k = 0; k < 3; ++k)
                {
                    const F32 tb = base + th * (0.05f + 0.28f * k);
                    const F32 tt = base + th * (0.45f + 0.28f * k);
                    const F32 half = tw * (1.f - 0.25f * k);
                    triF(tx - half, tb, tx + half, tb, tx, llmin(tt, base + th), c);
                }
            }
        }
        // the rock and the wolf on it, lower right, facing the moon
        const F32 ws = m * 0.42f;
        const F32 wl = r - ws * 1.15f - cut * 0.5f, wb = b + h * 0.10f;
        ellipseF(wl + ws * 0.55f, wb, ws * 0.72f, ws * 0.12f, near_c);
        // a rim of moonlight down the wolf's front (a lighter copy one pixel toward the moon),
        // then the wolf over it
        wolf(wl - 1.f, wb + ws * 0.04f + 1.f, ws, LLColor4(0.55f, 0.62f, 0.80f, 0.18f * alpha));
        wolf(wl, wb + ws * 0.04f, ws, near_c);
    }

    void steelScene(F32 l, F32 b, F32 r, F32 t, F32 cut, F32 alpha)
    {
        if (alpha <= 0.f || r <= l || t <= b) return;
        chamfer(l, b, r, t, cut, LLColor4(0.20f, 0.21f, 0.23f, alpha), LLColor4(0.08f, 0.085f, 0.095f, alpha));
        // a faint engineering grid
        const LLColor4 line(1.f, 1.f, 1.f, 0.035f * alpha);
        for (F32 x = l + cut; x < r - cut; x += 24.f) rect(x, b + cut, x + 1.f, t - cut, line);
        for (F32 y = b + cut; y < t - cut; y += 24.f) rect(l + cut, y, r - cut, y + 1.f, line);
        // a diagonal sheen, as on brushed metal
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        const LLColor4 a(1.f, 1.f, 1.f, 0.f), s(1.f, 1.f, 1.f, 0.05f * alpha);
        const F32 x0 = l + (r - l) * 0.25f, x1 = l + (r - l) * 0.55f;
        vtx(x0, t - cut, a); vtx(x1, t - cut, s); vtx(x1 - (t - b) * 0.4f, b + cut, a);
        gGL.end();
    }

    /** Source: wolfmapoverlays.cpp - tell the fetcher the size it is drawn at, or it may never
     *  decode past a low discard level. */
    void knownSize(LLViewerTexture* tex, F32 w, F32 h)
    {
        if (!tex) return;
        tex->setKnownDrawSize(llclamp(ll_round(w * LLUI::getScaleFactor().mV[VX]), 1, 2048),
                              llclamp(ll_round(h * LLUI::getScaleFactor().mV[VY]), 1, 2048));
    }

    bool imageCover(LLViewerTexture* tex, F32 l, F32 b, F32 r, F32 t, F32 cut, const LLColor4& tint)
    {
        knownSize(tex, r - l, t - b);
        if (!tex || !tex->hasGLTexture() || r <= l || t <= b) return false;
        // Cover: the picture's shape kept, cropped to the box, centred.
        const F32 iw = (F32)llmax(1, tex->getFullWidth()), ih = (F32)llmax(1, tex->getFullHeight());
        const F32 ia = iw / ih, ba = (r - l) / (t - b);
        F32 u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
        if (ia > ba) { const F32 span = ba / ia; u0 = 0.5f - span * 0.5f; u1 = 0.5f + span * 0.5f; }
        else         { const F32 span = ia / ba; v0 = 0.5f - span * 0.5f; v1 = 0.5f + span * 0.5f; }
        F32 x[8], y[8];
        chamferPoints(l, b, r, t, cut, x, y);
        auto uv = [&](F32 px, F32 py)
        {
            // Source: wolfmapoverlays.cpp - texcoord v 1 at the top.
            gGL.texCoord2f(u0 + (px - l) / (r - l) * (u1 - u0), v0 + (py - b) / (t - b) * (v1 - v0));
            gGL.vertex2f(px, py);
        };
        const F32 cx = (l + r) * 0.5f, cy = (b + t) * 0.5f;
        gGL.getTexUnit(0)->bind(tex);
        gGL.color4fv(tint.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < 8; ++i)
        {
            const S32 j = (i + 1) % 8;
            uv(cx, cy); uv(x[i], y[i]); uv(x[j], y[j]);
        }
        gGL.end();
        noTex();
        return true;
    }

    bool imageDisc(LLViewerTexture* tex, F32 cx, F32 cy, F32 r, const LLColor4& tint)
    {
        knownSize(tex, 2.f * r, 2.f * r);
        if (!tex || !tex->hasGLTexture() || r <= 0.f) return false;
        const F32 iw = (F32)llmax(1, tex->getFullWidth()), ih = (F32)llmax(1, tex->getFullHeight());
        // the square in the middle of the picture
        const F32 su = iw > ih ? ih / iw : 1.f, sv = ih > iw ? iw / ih : 1.f;
        gGL.getTexUnit(0)->bind(tex);
        gGL.color4fv(tint.mV);
        gGL.begin(LLRender::TRIANGLES);
        const S32 seg = 40;
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            gGL.texCoord2f(0.5f, 0.5f); gGL.vertex2f(cx, cy);
            gGL.texCoord2f(0.5f + 0.5f * su * cosf(a0), 0.5f + 0.5f * sv * sinf(a0)); gGL.vertex2f(cx + r * cosf(a0), cy + r * sinf(a0));
            gGL.texCoord2f(0.5f + 0.5f * su * cosf(a1), 0.5f + 0.5f * sv * sinf(a1)); gGL.vertex2f(cx + r * cosf(a1), cy + r * sinf(a1));
        }
        gGL.end();
        noTex();
        return true;
    }

    // ---- numbers and text ----

    std::string thousands(S64 v)
    {
        const bool neg = v < 0;
        U64 u = neg ? (U64)(-(v + 1)) + 1 : (U64)v;
        std::string digits = std::to_string(u);
        std::string out;
        const S32 n = (S32)digits.size();
        for (S32 i = 0; i < n; ++i)
        {
            out += digits[i];
            const S32 left = n - 1 - i;
            if (left > 0 && left % 3 == 0) out += ',';
        }
        return neg ? "-" + out : out;
    }
}

namespace
{
    F32 digitCell(const LLFontGL* font)
    {
        F32 w = 0.f;
        for (char d = '0'; d <= '9'; ++d) w = llmax(w, font->getWidthF32(std::string(1, d)));
        return w;
    }
}

namespace WolfGameDraw
{
    F32 tabularWidth(const LLFontGL* font, const std::string& text)
    {
        const F32 cell = digitCell(font);
        F32 w = 0.f;
        std::string run;
        for (char ch : text)
        {
            if (ch >= '0' && ch <= '9')
            {
                if (!run.empty()) { w += font->getWidthF32(run); run.clear(); }
                w += cell;
            }
            else run += ch;
        }
        if (!run.empty()) w += font->getWidthF32(run);
        return w;
    }

    F32 tabularRight(const LLFontGL* font, const std::string& text, F32 right, F32 y, const LLColor4& c)
    {
        const F32 cell = digitCell(font);
        const F32 left = right - tabularWidth(font, text);
        F32 x = left;
        std::string run;
        auto flush = [&]()
        {
            if (run.empty()) return;
            font->renderUTF8(run, 0, x, y, c, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            x += font->getWidthF32(run);
            run.clear();
        };
        for (char ch : text)
        {
            if (ch >= '0' && ch <= '9')
            {
                flush();
                const std::string d(1, ch);
                font->renderUTF8(d, 0, x + cell * 0.5f, y, c, LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
                x += cell;
            }
            else run += ch;
        }
        flush();
        return left;
    }

    std::vector<std::string> wrap(const LLFontGL* font, const std::string& text, F32 width, S32 max_lines)
    {
        // [SECURITY 2026-10-09] Linear and bounded: one pass over at most 2048 characters with an
        // index (no erase-from-front), stopping as soon as there are more lines than are shown. The
        // texts are capped at the sim's limits before they get here (wolfgame.cpp capText).
        std::vector<std::string> lines;
        if (width < 1.f) width = 1.f;
        LLWString w = utf8str_to_wstring(text.size() > 8192 ? text.substr(0, 8192) : text);
        if (w.size() > 2048) w.resize(2048);
        const size_t keep = max_lines > 0 ? (size_t)max_lines + 1 : (size_t)-1;
        size_t pos = 0;
        while (pos <= w.size() && lines.size() < keep)
        {
            size_t nl = w.find('\n', pos);
            const size_t end = nl == LLWString::npos ? w.size() : nl;
            if (end == pos) lines.push_back(std::string());
            while (pos < end && lines.size() < keep)
            {
                S32 n = font->maxDrawableChars(w.c_str() + pos, width, (S32)(end - pos), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
                if (n <= 0) n = 1;
                lines.push_back(wstring_to_utf8str(w.substr(pos, n)));
                pos += n;
                while (pos < end && w[pos] == ' ') ++pos;
            }
            if (nl == LLWString::npos) break;
            pos = nl + 1;
        }
        if (max_lines > 0 && (S32)lines.size() > max_lines)
        {
            lines.resize(max_lines);
            LLWString last = utf8str_to_wstring(lines.back());
            const F32 room = llmax(1.f, width - font->getWidthF32("..."));
            const S32 fit = font->maxDrawableChars(last.c_str(), room, (S32)last.size(), LLFontGL::ANYWHERE);
            last.resize(llclamp(fit, 0, (S32)last.size()));
            lines.back() = wstring_to_utf8str(last) + "...";
        }
        return lines;
    }

    LLColor4 readable(const LLColor4& c, F32 min_lum)
    {
        LLColor4 out = c;
        out.mV[VALPHA] = 1.f;
        for (S32 i = 0; i < 20 && relLum(out) < min_lum; ++i)
        {
            out = lerp(out, LLColor4::white, 0.12f);
        }
        return out;
    }

    S32 statRowHeight(S32 bar_h)
    {
        return LLFontGL::getFontSansSerifSmall()->getLineHeight() + 2 + bar_h + 6;
    }

    void drawStatRow(const WolfGame::Stat& s, F32 left, F32 top, F32 width, const Palette& p, S32 bar_h)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const F32 lh = (F32)font->getLineHeight();
        // §3: every bar shows its number - colour never carries the meaning alone.
        const std::string value = thousands(s.mValue) + " / " + thousands(s.mMax);
        const F32 value_left = tabularRight(font, value, left + width, top, p.mText);
        F32 label_l = left;
        if (!s.mIcon.empty())
        {
            const F32 is = lh;
            icon(s.mIcon, left, top - is, is, p, std::string(), false);
            label_l += is + 4.f;
        }
        textL(font, s.mLabel, label_l, top, p.mText, LLFontGL::LEFT, LLFontGL::TOP, llmax(1.f, value_left - label_l - 6.f));
        const F32 bar_t = top - lh - 2.f, bar_b = bar_t - (F32)bar_h;
        rect(left, bar_b, left + width, bar_t, p.mTrack);
        const F32 frac = s.mMax > 0 ? llclamp((F32)s.mValue / (F32)s.mMax, 0.f, 1.f) : 0.f;
        LLColor4 bar = s.mColor;
        bar.mV[VALPHA] = 1.f;
        // A lit top half, as on a glass tube.
        rectG(left, bar_b, left + width * frac, bar_t, readable(bar, 0.45f), bar);
        // quarter ticks over the bar, so a glance reads a fraction
        for (S32 q = 1; q < 4; ++q)
        {
            const F32 x = left + width * q / 4.f;
            rect(x, bar_b, x + 1.f, bar_t, LLColor4(0.f, 0.f, 0.f, 0.35f));
        }
    }

    std::string walletText(S64 balance, const std::string& symbol, const std::string& name)
    {
        if (!name.empty()) return thousands(balance) + " " + name;
        return symbol.empty() ? thousands(balance) : symbol + " " + thousands(balance);
    }

    std::string currentWallet()
    {
        WolfGame& g = WolfGame::instance();
        // The player's balance, from the player / currency messages; the money's name from them,
        // else from the game's own record (SendGame "currency" / "symbol").
        if (g.hasCurrency())
        {
            const std::string name = !g.currencyName().empty() ? g.currencyName() : g.game().mCurrency;
            const std::string sym = !g.currencySymbol().empty() ? g.currencySymbol() : g.game().mSymbol;
            return walletText(g.balance(), sym, name);
        }
        return std::string();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the portrait
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    using namespace WolfGameDraw;

    /** The portrait's face: the game's logo, else (wolves) the bundled photograph, else the game's
     *  initial or the paw - the same choice as wolfGamePortrait. */
    void portraitFace(F32 cx, F32 cy, F32 r, const Palette& p)
    {
        WolfGame& g = WolfGame::instance();
        disc(cx, cy, r + 1.5f, p.mEdge, 48);
        if (imageDisc(gameLogo(), cx, cy, r, LLColor4::white)) return;
        if (p.mWolves)
        {
            if (!imageDisc(photo("WolfGame_Portrait"), cx, cy, r, LLColor4::white)) disc(cx, cy, r, LLColor4(0.05f, 0.08f, 0.16f, 1.f), 48);
            return;
        }
        disc(cx, cy, r, LLColor4(0.16f, 0.17f, 0.19f, 1.f), 48);
        const std::string& name = g.game().mName;
        if (!name.empty())
        {
            // The game's initial, whole (a multi-byte first letter stays whole).
            const LLWString w = utf8str_to_wstring(name);
            textL(display(r * 1.1f), wstring_to_utf8str(w.substr(0, 1)), cx, cy, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
        else paw(cx, cy, r * 1.1f, p.mDim);
    }

    /** The level badge, centred at (cx, cy) (WoW's level badge). */
    void levelBadge(F32 cx, F32 cy, F32 r, const Palette& p)
    {
        const std::string lv = thousands(WolfGame::instance().level());
        const LLFontGL* f = display(r * 0.55f);
        const F32 br = llmax(r * 0.36f, f->getWidthF32(lv) * 0.5f + 4.f);
        disc(cx, cy, br + 1.5f, p.mAccent, 32);
        disc(cx, cy, br, p.mBaseBottom, 32);
        textL(f, lv, cx, cy, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

}

void wolfGamePortrait(F32 cx, F32 cy, F32 r, const WolfGameDraw::Palette& p)
{
    using namespace WolfGameDraw;
    WolfGame& g = WolfGame::instance();
    // The rings: the first three stats, outermost first (EVE: shield, armour, hull).
    const F32 ring_w = llmax(2.f, r * 0.13f);
    const F32 ring_gap = llmax(1.f, r * 0.045f);
    const size_t n = llmin((size_t)3, g.stats().size());
    disc(cx, cy, r + 3.f * (ring_w + ring_gap) + 2.f, withAlpha(p.mBaseBottom, 0.85f), 48);
    for (size_t i = 0; i < n; ++i)
    {
        const WolfGame::Stat& s = g.stats()[i];
        const F32 rr = r + ring_gap + ring_w * 0.5f + (F32)(n - 1 - i) * (ring_w + ring_gap);
        const F32 frac = s.mMax > 0 ? (F32)s.mValue / (F32)s.mMax : 0.f;
        gauge(cx, cy, rr, ring_w, frac, s.mColor, p.mTrack);
    }
    // The portrait: the game's logo, else (wolves) the photograph - Paul: "make sure that the
    // wolves look like wolves, use photographs" (textures.xml WolfGame_Portrait) - else the
    // game's initial.
    portraitFace(cx, cy, r, p);
    if (g.dead() || g.downed())
    {
        disc(cx, cy, r, LLColor4(0.35f, 0.f, 0.f, 0.55f), 48);
    }
    // The level badge in the rings' gap at the bottom (WoW's level badge).
    if (g.hasLevel()) levelBadge(cx, cy - r - ring_w, r, p);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the HUD
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfGameHUD::WolfGameHUD(const Params& p)
:   LLUICtrl(p)
{
    // Built with the main view, before login: the "WolfGame" GenericMessage handler is in place
    // before the first region can send its "game" (WolfGame's constructor registers it).
    WolfGame::instance();
}

void WolfGameHUD::draw()
{
    mNoticeHits.clear();
    mCombatHits.clear();
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion()) return;
    if (!WolfGrid::isOnWolfTerritories()) return;   // [WOLF GRID GATE 2026-10-09] nothing off Wolf

    WolfGame& g = WolfGame::instance();
    const LLRect r = getLocalRect();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    drawEffects(r);
    drawDowned(r);
    drawMarkers(r);

    // Clear of the navigation bar (llscriptfloater.cpp LLScriptFloaterManager::getTopPad, the
    // same pad script dialogs use) and of the right-hand toolbar when it has buttons.
    const S32 top_pad = LLScriptFloaterManager::instance().getTopPad();
    S32 right_pad = 0;
    if (gToolBarView)
    {
        LLToolBar* rt = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_RIGHT);
        if (rt && rt->hasButtons()) right_pad = rt->getRect().getWidth();
    }
    // A plain click on the world, now finished (see handleMouseDown).
    if (mClickAt > 0.0 && !gViewerWindow->getLeftMouseDown())
    {
        const LLCoordGL m = gViewerWindow->getCurrentMouse();
        const bool still = abs(m.mX - mClickMouse.mX) <= 4 && abs(m.mY - mClickMouse.mY) <= 4;
        if (still && LLFrameTimer::getTotalSeconds() - mClickAt < 0.6) pickTarget(mClickX, mClickY);
        mClickAt = 0.0;
    }
    drawCombatText(r);
    // [GAME HUD 2026-10-09] the game's screen is the HUD across the bottom (with the cast bar and
    // buffs above its action bar); hidden by the player, or while the flight deck or the car
    // dashboard has the bottom, it is not drawn and the game keeps running. §5.5 the target frame
    // top-centre and §5.4 a refused press in the upper third stay where they were.
    if (g.hudShowing()) drawBottomHud(r);
    // [FIX 2026-10-09] Paul: a target frame appeared (and would not go) while the game HUD was not up
    // - Game Mode stays on at the region from the last session while the HUD starts hidden. The
    // target is part of the game's screen: shown only with the HUD.
    if (g.target().mOn && g.hudShowing()) drawTarget(r, top_pad);
    if (!g.combatError().empty()) drawCombatError(r);
    drawNotices(r, top_pad, right_pad);
    LLUICtrl::draw();
}

void WolfGameHUD::drawEffects(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    const bool real_pass = gWolfPhotoFilterProgram.isComplete();
    const F32 blur = g.effectNow(WolfGame::FX_BLUR);
    const F32 desat = g.effectNow(WolfGame::FX_DESATURATE);
    // §3.6: blur and desaturate are real post passes (pipeline.cpp wolfPhotoPass, modes 16 and 2)
    // when the photo filter shader built. Without it: a frosted / grey overlay instead.
    if (!real_pass && blur > 0.f)
    {
        WolfGameDraw::rect((F32)r.mLeft, (F32)r.mBottom, (F32)r.mRight, (F32)r.mTop, LLColor4(0.86f, 0.88f, 0.92f, 0.45f * blur));
    }
    if (!real_pass && desat > 0.f)
    {
        WolfGameDraw::rect((F32)r.mLeft, (F32)r.mBottom, (F32)r.mRight, (F32)r.mTop, LLColor4(0.5f, 0.5f, 0.5f, 0.6f * desat));
    }
    const F32 pulse = g.effectNow(WolfGame::FX_PULSE);
    if (pulse > 0.f)
    {
        const F32 a = pulse * (0.25f + 0.55f * g.pulseBeat());
        vignette(r, 0.22f * (F32)llmin(r.getWidth(), r.getHeight()), LLColor4(0.85f, 0.f, 0.f, a));
    }
    const F32 flash = g.effectNow(WolfGame::FX_FLASH);
    if (flash > 0.f)
    {
        // §3.6 "A red full-screen tint, opacity s% x 0.6".
        WolfGameDraw::rect((F32)r.mLeft, (F32)r.mBottom, (F32)r.mRight, (F32)r.mTop, LLColor4(1.f, 0.f, 0.f, 0.6f * flash));
    }
    const F32 black = g.effectNow(WolfGame::FX_BLACKOUT);
    if (black > 0.f)
    {
        WolfGameDraw::rect((F32)r.mLeft, (F32)r.mBottom, (F32)r.mRight, (F32)r.mTop, LLColor4(0.f, 0.f, 0.f, black));
    }
}

void WolfGameHUD::drawDowned(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    if (!g.downedOverlay()) return;
    // §3.7: a dark overlay with a red vignette, the text large and centred, a countdown.
    WolfGameDraw::rect((F32)r.mLeft, (F32)r.mBottom, (F32)r.mRight, (F32)r.mTop, LLColor4(0.f, 0.f, 0.f, 0.55f));
    vignette(r, 0.25f * (F32)llmin(r.getWidth(), r.getHeight()), LLColor4(0.7f, 0.f, 0.f, 0.7f));
    const LLFontGL* huge = WolfGameDraw::display(44.f);
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
    const std::string text = g.downedText().empty() ? std::string("You are down") : g.downedText();
    huge->renderUTF8(text, 0, cx, cy + 10.f, WolfGameDraw::WOLF_SILVER, LLFontGL::HCENTER, LLFontGL::BOTTOM,
                     LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    const S32 n = g.downedCountdown();
    if (n > 0)
    {
        // §3.7 '"Respawning in N" (or just N)': just N - a script's own down may not respawn.
        huge->renderUTF8(std::to_string(n), 0, cx, cy - 10.f, WolfGameDraw::WOLF_SILVER, LLFontGL::HCENTER, LLFontGL::TOP,
                         LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}

void WolfGameHUD::drawMarkers(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    if (g.markers().empty()) return;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLVector3d me = gAgent.getPositionGlobal();
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
    for (const auto& kv : g.markers())
    {
        const WolfGame::Marker& m = kv.second;
        LLVector3d global;
        if (!WolfGame::markerGlobal(m, global)) continue;
        const LLVector3 agent = gAgent.getPosAgentFromGlobal(global);
        // Source: lltracker.cpp LLTracker::drawMarker - on screen, or pinned to the edge.
        LLCoordGL screen;
        const bool on_screen = cam->projectPosAgentToScreen(agent, screen, true);
        if (!on_screen && !cam->projectPosAgentToScreenEdge(agent, screen)) continue;
        S32 lx = 0, ly = 0;
        screenPointToLocal(screen.mX, screen.mY, &lx, &ly);
        F32 x = (F32)lx, y = (F32)ly;
        const F32 metres = (F32)(global - me).magVec();
        // §3.9: the label and the distance in m, in the marker colour - lifted to a readable tint
        // on its dark pill (§3 contrast); the raw colour is the dot / arrow.
        const std::string label = (m.mLabel.empty() ? m.mId : m.mLabel) + "  " + WolfGameDraw::thousands((S64)llround(metres)) + " m";
        const LLColor4 text_col = WolfGameDraw::readable(m.mColor);
        const F32 tw = font->getWidthF32(label), lh = (F32)font->getLineHeight();
        if (on_screen)
        {
            polyF(x, y, 6.f, 4, 0.f, LLColor4(0.f, 0.f, 0.f, 0.8f));
            polyF(x, y, 4.5f, 4, 0.f, m.mColor);
            const F32 pl = x - tw * 0.5f - 4.f, pb = y + 9.f;
            WolfGameDraw::rect(pl, pb, pl + tw + 8.f, pb + lh + 4.f, LLColor4(0.f, 0.f, 0.f, 0.8f));
            font->renderUTF8(label, 0, x, pb + 2.f, text_col, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
        else
        {
            // Off screen: an arrow at the edge pointing at it, the label beside it inside the view.
            F32 dx = x - cx, dy = y - cy;
            const F32 len = sqrtf(dx * dx + dy * dy);
            if (len < 1.f) continue;
            dx /= len; dy /= len;
            x = llclamp(x, (F32)r.mLeft + 14.f, (F32)r.mRight - 14.f);
            y = llclamp(y, (F32)r.mBottom + 14.f, (F32)r.mTop - 14.f);
            const F32 px = -dy, py = dx;
            const F32 tipx = x + dx * 10.f, tipy = y + dy * 10.f;
            triF(tipx + dx * 2.f, tipy + dy * 2.f, x - dx * 6.f + px * 10.f, y - dy * 6.f + py * 10.f,
                 x - dx * 6.f - px * 10.f, y - dy * 6.f - py * 10.f, LLColor4(0.f, 0.f, 0.f, 0.8f));
            triF(tipx, tipy, x - dx * 4.f + px * 7.f, y - dy * 4.f + py * 7.f, x - dx * 4.f - px * 7.f, y - dy * 4.f - py * 7.f, m.mColor);
            F32 lxp = x - dx * 22.f - tw * 0.5f, lyp = y - dy * 22.f - lh * 0.5f;
            lxp = llclamp(lxp, (F32)r.mLeft + 4.f, (F32)r.mRight - tw - 8.f);
            lyp = llclamp(lyp, (F32)r.mBottom + 4.f, (F32)r.mTop - lh - 8.f);
            WolfGameDraw::rect(lxp - 4.f, lyp - 2.f, lxp + tw + 4.f, lyp + lh + 2.f, LLColor4(0.f, 0.f, 0.f, 0.8f));
            font->renderUTF8(label, 0, lxp, lyp, text_col, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }
}

void WolfGameHUD::drawNotices(const LLRect& r, S32 top_pad, S32 right_pad)
{
    using namespace WolfGameDraw;
    WolfGame& g = WolfGame::instance();
    if (g.notices().empty()) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* title_font = display(16.f);
    const F32 lh = (F32)font->getLineHeight(), tlh = (F32)title_font->getLineHeight();
    const F32 PAD = 10.f;
    const F32 right = (F32)(r.mRight - right_pad - MARGIN);
    const F32 left = right - NOTICE_W;
    const F32 text_l = left + 6.f + PAD, text_w = right - PAD - text_l;
    F32 top = (F32)(r.mTop - top_pad - MARGIN);
    // Newest on top, stacked down (§3.4).
    for (auto it = g.notices().rbegin(); it != g.notices().rend(); ++it)
    {
        const WolfGame::Notice& n = *it;
        // [SECURITY 2026-10-09] Only the game's own script (verified) gets the game's themed look;
        // any other script's notice is plain and says "Unverified" beside its source.
        const Palette& p = palette(n.mVerified);
        const std::vector<std::string> title = wrap(title_font, n.mTitle, text_w, 2);
        const std::vector<std::string> text = wrap(font, n.mText, text_w, 6);
        F32 h = PAD + 18.f + 4.f;
        if (!n.mTitle.empty()) h += tlh * title.size();
        if (!n.mText.empty()) h += lh * text.size() + 2.f;
        if (!n.mSrc.empty() || !n.mVerified) h += lh + 2.f;
        h += PAD;
        const F32 bottom = top - h;
        frame(left, bottom, right, top, p, 1.f, 8.f);
        // the kind's stripe down the left, with its glow
        rect(left + 3.f, bottom + 8.f, left + 7.f, top - 8.f, KIND_COLOUR[n.mKind]);
        rect(left + 7.f, bottom + 8.f, left + 11.f, top - 8.f, withAlpha(KIND_COLOUR[n.mKind], 0.25f));
        // icon + the kind as a word (§3: colour never alone)
        F32 y = top - PAD;
        kindIcon(n.mKind, text_l + 8.f, y - 9.f);
        bold->renderUTF8(caps(KIND_WORD[n.mKind]), 0, text_l + 22.f, y - 9.f, KIND_TEXT[n.mKind], LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        NoticeHit hit;
        hit.mSerial = n.mSerial;
        hit.mClose.setLeftTopAndSize((S32)(right - 26.f), (S32)(top - 3.f), 22, 22);
        closeX((F32)hit.mClose.getCenterX(), (F32)hit.mClose.getCenterY(), 4.f, p.mText);
        y -= 18.f + 4.f;
        for (const std::string& line : title)
        {
            if (n.mTitle.empty()) break;
            textL(title_font, line, text_l, y, p.mText);
            y -= tlh;
        }
        if (!n.mText.empty())
        {
            y -= 2.f;
            for (const std::string& line : text)
            {
                textL(font, line, text_l, y, p.mText);
                y -= lh;
            }
        }
        if (!n.mSrc.empty() || !n.mVerified)
        {
            y -= 2.f;
            F32 src_l = text_l;
            if (!n.mVerified)
            {
                // An icon AND the word (§3: never colour alone).
                unverifiedMark(src_l, y, font, &src_l);
                src_l += 6.f;
            }
            textL(font, n.mSrc, src_l, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, llmax(1.f, text_l + text_w - src_l));
        }
        hit.mRect.set((S32)left, (S32)top, (S32)right, (S32)bottom);
        mNoticeHits.push_back(hit);
        top = bottom - 8.f;
    }
}

bool WolfGameHUD::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!WolfGrid::isOnWolfTerritories()) return false;   // [WOLF GRID GATE] stock clicks only
    // [COMBAT] / [GAME HUD] everything the HUD draws: the action and weapon slots, the cast bar's
    // X, the target frame's X, the portrait, orbs, bag, wallet, the page buttons, PANEL, the guide.
    if (const WolfGameHits::Hit* h = mCombatHits.at(x, y))
    {
        WolfGame& g = WolfGame::instance();
        mCombatHits.setPressed(h);
        bool sent = true;
        switch (h->mId)
        {
        case H_ACTION:
            if (h->mArg >= 0 && h->mArg < (S32)g.abilities().size()) sent = g.useAbility(g.abilities()[h->mArg].mId);
            break;
        case H_CAST_CANCEL: sent = g.cancelCast(); break;
        case H_TARGET_CLEAR:
            // [FIX 2026-10-09] Paul: the X "doesnt work" - it waited for the region's answer.
            // Clear here at once; tell the region when we can (nothing to report when we can't).
            g.clearTargetLocal();
            g.selectTarget(std::string());
            break;
        case H_WEAPON:
        {
            // arg: the slot's index * 2, + 1 when a click reloads it (drawWeaponSlot)
            const S32 idx = h->mArg / 2;
            if ((h->mArg % 2) && idx >= 0 && idx < (S32)WolfGame::weaponSlots().size()) sent = g.reload(WolfGame::weaponSlots()[idx]);
            else WolfFloaterRoleplay::showPage(WolfGameConsole::PAGE_COMBAT, false);
            break;
        }
        case H_PORTRAIT:
        case H_ORB:
            WolfFloaterRoleplay::showPage(WolfGameConsole::PAGE_STATS, true);
            break;
        case H_BAG:
        case H_WALLET:
            WolfFloaterRoleplay::showPage(WolfGameConsole::PAGE_BAG, true);
            break;
        case H_PAGE:
            if (h->mArg >= 0 && h->mArg < WolfGameConsole::PAGE_COUNT)
            {
                WolfFloaterRoleplay::showPage((WolfGameConsole::EPage)h->mArg, true);
            }
            break;
        case H_PANEL_BG:
            gSavedSettings.setBOOL("WolfGameHudBackground", !gSavedSettings.getBOOL("WolfGameHudBackground"));
            break;
        case H_GAMEMODE_SW:
            // The same command as World > Game Mode and the console's switch (llviewermenu.cpp
            // WolfGame.ToggleGameMode), so its refusals say why; a forced region refuses it there.
            if (LLUICtrl::CommitCallbackRegistry::ptr_value_t cb = LLUICtrl::CommitCallbackRegistry::getValue("WolfGame.ToggleGameMode"))
            {
                (*cb)(nullptr, LLSD());
            }
            break;
        case H_GUIDE_OK:
            g.dismissGuide();
            break;
        case H_GUIDE_HELP:
            g.dismissGuide();
            WolfFloaterRoleplay::showPage(WolfGameConsole::PAGE_HELP, false);
            break;
        default: break;   // buffs, empty slots, the target frame, the panel, the guide's card: tooltips only
        }
        if (!sent)
        {
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                std::string("Not sent: the viewer is not connected to a Wolf Territories region.")));
        }
        return true;
    }
    for (const NoticeHit& h : mNoticeHits)
    {
        if (h.mClose.pointInRect(x, y))
        {
            WolfGame::instance().closeNotice(h.mSerial);
            return true;
        }
        if (h.mRect.pointInRect(x, y)) return true;   // a notice is not a hole to the world
    }
    // [COMBAT] §5.5: a plain left CLICK on the world while Game Mode is on picks the target - the
    // press is remembered here, and draw() picks once the button is up with the mouse unmoved (a
    // camera drag is not a click). The click still goes on to the world (return false).
    if (mask == MASK_NONE)
    {
        localPointToScreen(x, y, &mClickX, &mClickY);
        mClickMouse = gViewerWindow->getCurrentMouse();
        mClickAt = LLFrameTimer::getTotalSeconds();
    }
    return false;
}

bool WolfGameHUD::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mCombatHits.at(x, y))
    {
        mCombatHits.setPressed(nullptr);
        return true;
    }
    for (const NoticeHit& h : mNoticeHits)
    {
        if (h.mRect.pointInRect(x, y)) return true;
    }
    return false;
}

bool WolfGameHUD::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* ch = mCombatHits.at(x, y);
    mCombatHits.setHover(ch);
    if (ch)
    {
        bool clickable = false;
        switch (ch->mId)
        {
        case H_ACTION: case H_CAST_CANCEL: case H_TARGET_CLEAR: case H_WEAPON: case H_PORTRAIT: case H_ORB:
        case H_BAG: case H_WALLET: case H_PAGE: case H_PANEL_BG: case H_GUIDE_OK: case H_GUIDE_HELP: case H_GAMEMODE_SW:
            clickable = true;
            break;
        default: break;
        }
        gViewerWindow->setCursor(clickable ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
        return true;
    }
    bool hand = false, over = false;
    for (const NoticeHit& h : mNoticeHits)
    {
        if (h.mClose.pointInRect(x, y)) { hand = over = true; break; }
        if (h.mRect.pointInRect(x, y)) over = true;
    }
    if (!over) return false;
    gViewerWindow->setCursor(hand ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfGameHUD::handleToolTip(S32 x, S32 y, MASK mask)
{
    std::string tip;
    if (const WolfGameHits::Hit* ch = mCombatHits.at(x, y)) tip = ch->mTip;
    for (const NoticeHit& h : mNoticeHits)
    {
        if (tip.empty() && h.mClose.pointInRect(x, y)) { tip = "Close this notice"; break; }
    }
    if (tip.empty()) return false;
    LLToolTipMgr::instance().show(tip);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [COMBAT 2026-10-09] VIEWER_SPEC.md §5.3-5.6 on the world view
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    using namespace WolfGameDraw;

    F64 nowSecs() { return LLFrameTimer::getTotalSeconds(); }

    // Hostility (§5.5): a rim colour AND a word. Words in the lighter tints (4.5:1 or better on a card).
    const LLColor4 HOSTILE(0.937f, 0.267f, 0.267f, 1.f), NEUTRAL(0.961f, 0.620f, 0.043f, 1.f), FRIENDLY(0.133f, 0.773f, 0.369f, 1.f);

    /** "12s", "3m", "1h" (§5.6). */
    std::string shortTime(F64 secs)
    {
        if (secs < 60.0) return llformat("%ds", (S32)ceil(secs));
        if (secs < 3600.0) return llformat("%dm", (S32)ceil(secs / 60.0));
        return llformat("%dh", (S32)ceil(secs / 3600.0));
    }

    /** The first letters of a label, for an ability with no icon (§5.4). */
    std::string initials(const std::string& label)
    {
        LLWString w = utf8str_to_wstring(label);
        LLWString out;
        bool start = true;
        for (llwchar c : w)
        {
            if (c == ' ') { start = true; continue; }
            if (start) out.push_back(c);
            start = false;
            if (out.size() >= 2) break;
        }
        return wstring_to_utf8str(out);
    }

    /** The first letter, whole (a multi-byte letter is never cut). */
    std::string firstLetter(const std::string& s)
    {
        LLWString w = utf8str_to_wstring(s);
        return w.empty() ? std::string() : wstring_to_utf8str(w.substr(0, 1));
    }

    // ---- glyphs, drawn (no emoji) ----
    void lockGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        rect(cx - s * 0.45f, cy - s * 0.5f, cx + s * 0.45f, cy + s * 0.1f, c);
        arc(cx, cy + s * 0.1f, s * 0.3f, llmax(1.5f, s * 0.12f), 0.f, F_PI, c, 12);
    }
    void reloadGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        arc(cx, cy, s * 0.4f, llmax(1.5f, s * 0.12f), 0.3f, 2.f * F_PI - 0.6f, c, 20);
        const F32 ax = cx + s * 0.4f * cosf(0.3f), ay = cy + s * 0.4f * sinf(0.3f);
        triF(ax - s * 0.2f, ay, ax + s * 0.2f, ay, ax, ay + s * 0.25f, c);
    }
    void crosshairGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        arc(cx, cy, s * 0.35f, 1.5f, 0.f, 2.f * F_PI, c, 20);
        lineW(cx - s * 0.5f, cy, cx - s * 0.15f, cy, 1.5f, c);
        lineW(cx + s * 0.15f, cy, cx + s * 0.5f, cy, 1.5f, c);
        lineW(cx, cy - s * 0.5f, cx, cy - s * 0.15f, 1.5f, c);
        lineW(cx, cy + s * 0.15f, cx, cy + s * 0.5f, 1.5f, c);
    }
    void swordsGlyph(F32 cx, F32 cy, F32 s, const LLColor4& c)
    {
        lineW(cx - s * 0.45f, cy - s * 0.45f, cx + s * 0.45f, cy + s * 0.45f, 2.f, c);
        lineW(cx + s * 0.45f, cy - s * 0.45f, cx - s * 0.45f, cy + s * 0.45f, 2.f, c);
        lineW(cx - s * 0.45f, cy - s * 0.15f, cx - s * 0.15f, cy - s * 0.45f, 2.f, c);
        lineW(cx + s * 0.45f, cy - s * 0.15f, cx + s * 0.15f, cy - s * 0.45f, 2.f, c);
    }
    void arrowGlyph(F32 cx, F32 cy, F32 s, bool down, const LLColor4& c)
    {
        if (down) triF(cx - s * 0.5f, cy + s * 0.35f, cx + s * 0.5f, cy + s * 0.35f, cx, cy - s * 0.4f, c);
        else triF(cx - s * 0.5f, cy - s * 0.35f, cx + s * 0.5f, cy - s * 0.35f, cx, cy + s * 0.4f, c);
    }

    /** The colour of the stat named `n` (a cost's colour), and its value; false when not shown. */
    bool statOf(const std::string& n, LLColor4& colour, S32& value)
    {
        for (const WolfGame::Stat& s : WolfGame::instance().stats())
        {
            if (s.mName == n) { colour = s.mColor; value = s.mValue; return true; }
        }
        return false;
    }

    /** A dark wedge over a square, covering the part of a sweep still to run: revealed
     *  clockwise from 12 o'clock as `elapsed` goes 0 -> 1 (§5.4, WoW style). */
    void sweep(F32 l, F32 b, F32 r, F32 t, F32 elapsed, const LLColor4& c)
    {
        elapsed = llclamp(elapsed, 0.f, 1.f);
        if (elapsed >= 1.f) return;
        const F32 cx = (l + r) * 0.5f, cy = (b + t) * 0.5f, rad = (r - l) + (t - b);
        // Clockwise angle θ from 12 o'clock is the maths angle π/2 - θ.
        const F32 a0 = F_PI * 0.5f - 2.f * F_PI * elapsed, a1 = F_PI * 0.5f - 2.f * F_PI;
        const S32 seg = llmax(3, (S32)(48 * (1.f - elapsed)));
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
            gGL.color4fv(c.mV);
            gGL.vertex2f(cx, cy);
            gGL.vertex2f(cx + rad * cosf(u0), cy + rad * sinf(u0));
            gGL.vertex2f(cx + rad * cosf(u1), cy + rad * sinf(u1));
        }
        gGL.end();
    }

    /** The key that presses action button `i` (§5.4): 1-9, 0, -, =. */
    std::string actionKey(S32 i)
    {
        static const char* KEYS[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
        return i >= 0 && i < 12 ? KEYS[i] : std::string();
    }
}

F32 WolfGameHUD::bottomPad() const
{
    // Source: wolfflightdeck.cpp layoutAndDraw - sit on whatever the bottom toolbar leaves.
    if (!gToolBarView) return 0.f;
    LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true);
    if (!bp || !bp->isInVisibleChain()) return 0.f;
    const LLRect sr = bp->calcScreenRect();
    S32 lx = 0, ly = 0;
    screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
    return llclamp((F32)ly, 0.f, (F32)getRect().getHeight() * 0.4f);
}

void WolfGameHUD::drawTarget(const LLRect& r, S32 top_pad)
{
    WolfGame& g = WolfGame::instance();
    const WolfGame::Target& tg = g.target();
    const Palette& p = palette(tg.mVerified);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* small_bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* name_font = display(17.f);
    const F32 slh = (F32)small->getLineHeight();
    const F32 W = 300.f, PAD = 10.f, PS = 54.f;
    const LLColor4& rim = tg.mHost == "hostile" ? HOSTILE : tg.mHost == "friendly" ? FRIENDLY : NEUTRAL;
    const char* word = tg.mHost == "hostile" ? "Hostile" : tg.mHost == "friendly" ? "Friendly" : "Neutral";
    const F32 top = (F32)(r.mTop - top_pad - MARGIN);
    const F32 l = (F32)r.getCenterX() - W * 0.5f, rr = l + W;
    const bool bar = tg.mHealth >= 0 || tg.mDead;
    F32 h = PAD + slh + 2.f + (F32)name_font->getLineHeight() + 2.f + (tg.mSub.empty() ? 0.f : slh) + (bar ? 16.f : 0.f) + PAD;
    if (!tg.mVerified) h += slh + 2.f;
    h = llmax(h, PS + 2.f * PAD);
    const F32 b = top - h;
    frame(l, b, rr, top, p, 1.f, 10.f);
    // the rim: the frame's edge in the hostility colour, and a band down the left
    chamferEdge(l + 1.5f, b + 1.5f, rr - 1.5f, top - 1.5f, 9.f, 1.5f, LLColor4(rim.mV[VRED], rim.mV[VGREEN], rim.mV[VBLUE], 0.85f));
    // the portrait (icon or initials) with the rim round it, the level on it
    const F32 pl = rr - PAD - PS, pb = top - PAD - PS;
    chamferEdge(pl - 2.f, pb - 2.f, pl + PS + 2.f, pb + PS + 2.f, 6.f, 2.5f, rim);
    icon(tg.mIcon, pl, pb, PS, p, initials(tg.mName));
    if (tg.mLevel > 0)
    {
        const std::string lv = thousands(tg.mLevel);
        const LLFontGL* lf = display(13.f);
        const F32 br = llmax(10.f, lf->getWidthF32(lv) * 0.5f + 4.f);
        disc(pl + PS, pb, br + 1.5f, rim, 24);
        disc(pl + PS, pb, br, p.mBaseBottom, 24);
        textL(lf, lv, pl + PS, pb, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
    // the X that clears it (§5.5)
    const F32 xcx = pl - 12.f, xcy = top - 12.f;
    closeX(xcx, xcy, 4.f, p.mText);
    mCombatHits.add(xcx - 10.f, xcy - 10.f, xcx + 10.f, xcy + 10.f, H_TARGET_CLEAR, 0, "Clear the target (Escape)");
    // the words
    const F32 tl = l + PAD + 4.f, tw = pl - 26.f - tl;
    F32 y = top - PAD + 1.f;
    F32 gx = tl;
    if (tg.mHost == "hostile")
    {
        swordsGlyph(gx + 6.f, y - slh * 0.5f, 11.f, rim);
        gx += 16.f;
    }
    textL(small_bold, caps(std::string("Target - ") + word), gx, y, readable(rim, 0.35f), LLFontGL::LEFT, LLFontGL::TOP, tw - (gx - tl));
    y -= slh + 2.f;
    textL(name_font, tg.mName, tl, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, tw);
    y -= (F32)name_font->getLineHeight() + 2.f;
    if (!tg.mVerified)
    {
        F32 ur = tl;
        unverifiedMark(tl, y, small, &ur);
        y -= slh + 2.f;
    }
    if (!tg.mSub.empty())
    {
        textL(small, tg.mSub, tl, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, tw);
        y -= slh;
    }
    if (bar)
    {
        // health with its numbers; "Dead" as a word (§5.5)
        const F32 bt = y - 4.f, bb = bt - 9.f, br = pl - 10.f;
        rect(tl, bb, br, bt, p.mTrack);
        const F32 frac = tg.mDead ? 0.f : (tg.mMax > 0 ? llclamp((F32)tg.mHealth / (F32)tg.mMax, 0.f, 1.f) : 0.f);
        rectG(tl, bb, tl + (br - tl) * frac, bt, readable(HOSTILE, 0.4f), HOSTILE);
        const std::string hp = tg.mDead ? std::string("Dead") : thousands(tg.mHealth) + " / " + thousands(tg.mMax);
        // The numbers on their own dark pill (white on the bar's red would be under 4.5:1).
        const F32 hw = small_bold->getWidthF32(hp) * 0.5f + 5.f, hcx = (tl + br) * 0.5f, hcy = (bb + bt) * 0.5f;
        rect(hcx - hw, hcy - slh * 0.5f, hcx + hw, hcy + slh * 0.5f, LLColor4(0.f, 0.f, 0.f, 0.8f));
        small_bold->renderUTF8(hp, 0, (tl + br) * 0.5f, (bb + bt) * 0.5f, LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER,
                               LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
    std::string tip = tg.mName + " - " + word;
    if (tg.mLevel > 0) tip += ", level " + std::to_string(tg.mLevel);
    if (tg.mHealth >= 0 && !tg.mDead) tip += ", health " + thousands(tg.mHealth) + " / " + thousands(tg.mMax);
    if (tg.mDead) tip += ", dead";
    if (!tg.mSrc.empty()) tip += "\nFrom " + tg.mSrc;
    if (!tg.mVerified) tip += " - Unverified: not from this region's game";
    mCombatHits.add(l, b, rr, top, H_TARGET, 0, tip, false);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [GAME HUD 2026-10-09] the HUD across the bottom
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    /** The key labels on a slot: larger with the larger slots (so they grow with the window). */
    const LLFontGL* slotFont(F32 S)
    {
        return S >= 56.f ? LLFontGL::getFontSansSerifBold() : LLFontGL::getFontSansSerifSmallBold();
    }

    /** A tab on the HUD's top edge: its top corners cut, the palette's plate, a hairline edge. */
    void tabShape(F32 l, F32 b, F32 r, F32 t, F32 cut, const Palette& p, bool hover, bool active)
    {
        const LLColor4 top = withAlpha(p.mBaseTop, 0.96f), bot = withAlpha(p.mBaseBottom, 0.96f);
        rectG(l, b, r, t - cut, top, bot);
        rect(l + cut, t - cut, r - cut, t, top);
        triF(l, t - cut, l + cut, t - cut, l + cut, t, top);
        triF(r - cut, t - cut, r, t - cut, r - cut, t, top);
        const LLColor4 edge = hover || active ? p.mAccent : p.mEdge;
        lineW(l + 0.5f, b, l + 0.5f, t - cut, 1.f, edge);
        lineW(l + 0.5f, t - cut, l + cut, t - 0.5f, 1.f, edge);
        lineW(l + cut, t - 0.5f, r - cut, t - 0.5f, 1.f, edge);
        lineW(r - cut, t - 0.5f, r - 0.5f, t - cut, 1.f, edge);
        lineW(r - 0.5f, t - cut, r - 0.5f, b, 1.f, edge);
        if (active) rect(l + cut, t - 3.f, r - cut, t - 1.f, p.mAccent);   // open now: a bar AND the word's colour
    }

    /** Words on a dark pill, so they read over any colour under them (white on a bright orb's
     *  liquid would be under 4.5:1; on black at 0.66 over the brightest liquid it is about 5:1). */
    void pillText(const LLFontGL* f, const std::string& s, F32 cx, F32 cy, const LLColor4& c)
    {
        const F32 w = f->getWidthF32(s) * 0.5f + 5.f, h = (F32)f->getLineHeight() * 0.5f + 1.f;
        rect(cx - w, cy - h, cx + w, cy + h, LLColor4(0.f, 0.f, 0.f, 0.66f));
        f->renderUTF8(s, 0, cx, cy, c, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    /** The colour a fraction of the way from `a` to `b`. */
    LLColor4 mix(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[VRED] + (b.mV[VRED] - a.mV[VRED]) * t, a.mV[VGREEN] + (b.mV[VGREEN] - a.mV[VGREEN]) * t,
                        a.mV[VBLUE] + (b.mV[VBLUE] - a.mV[VBLUE]) * t, a.mV[VALPHA] + (b.mV[VALPHA] - a.mV[VALPHA]) * t);
    }
}

void WolfGameHUD::drawBottomHud(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    const Palette& p = gamePalette();
    static LLCachedControl<bool> show_bg(gSavedSettings, "WolfGameHudBackground", true);
    const F32 W = (F32)r.getWidth();
    const F32 bottom = bottomPad();

    // What it holds: up to three orbs (the first three stats, by o); the weapon slots - Main hand
    // and Ranged always, Off hand when something is in it.
    const S32 n_orbs = (S32)llmin((size_t)3, g.stats().size());
    std::vector<S32> weapons;
    const std::vector<std::string>& ws = WolfGame::weaponSlots();
    for (S32 i = 0; i < (S32)ws.size(); ++i)
    {
        if (ws[i] != "off" || g.slotView(ws[i]).mFilled) weapons.push_back(i);
    }
    const S32 n_w = (S32)weapons.size();

    // ---- the layout, in action slots (S). The numbers are the mockup's (game-hud.html) and
    //      VIEWER_SPEC.md §3.12: centre = 12 slots + the weapons; left = the portrait and the
    //      orbs; right = 8 bag slots of 0.8 S. One row when all three fit, else two. ----
    const F32 cg_u = 12.f + 11.f * 0.1f + 0.35f + (F32)n_w + (F32)(n_w - 1) * 0.1f;
    const F32 left_u = 1.62f + (F32)n_orbs * 1.5f + (F32)(n_orbs + 1) * 0.12f;
    const F32 right_u = 8.f * 0.8f + 7.f * 0.08f;
    F32 S = llclamp(W / 31.f, 40.f, 64.f);
    const bool wide = (left_u + cg_u + right_u + 1.6f) * S <= W;
    if (!wide) S = llmin(S, (W - 0.6f * S) / cg_u);   // under 40 only in a very small window
    S = floorf(llmax(S, 8.f));
    const F32 gap = floorf(0.1f * S), m = floorf(0.3f * S);
    const F32 Hp = floorf(wide ? 1.8f * S : 3.25f * S);
    const F32 top = bottom + Hp;
    const F32 ab = 12.f * S + 11.f * gap;
    const F32 cg = ab + floorf(0.35f * S) + (F32)n_w * S + (F32)(n_w - 1) * gap;
    const F32 P = 1.62f * S, D = 1.5f * S, og = 0.12f * S;
    const F32 left_w = P + (F32)n_orbs * D + (F32)(n_orbs + 1) * og;
    const F32 q = floorf(0.8f * S), qg = floorf(0.08f * S), right_w = 8.f * q + 7.f * qg;
    F32 cx, cb;
    if (wide)
    {
        cx = floorf(llclamp(W * 0.5f - cg * 0.5f, m + left_w + 0.5f * S, W - m - right_w - 0.5f * S - cg));
        cb = bottom + floorf((Hp - S) * 0.5f - 0.05f * S);
    }
    else
    {
        cx = floorf((W - cg) * 0.5f);
        cb = top - S - floorf(0.45f * S);
    }
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* small_bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();

    // ---- the panel: a plate the width of the screen, raised behind the action bar (WoW's main
    //      bar), a lit lip along its top. PANEL turns it off: then only the pieces are drawn. ----
    const F32 raise_t = top + floorf(0.22f * S);
    const F32 rl = cx - floorf(0.35f * S), rr = cx + cg + floorf(0.35f * S), rcut = floorf(0.3f * S);
    if (show_bg)
    {
        rectG(0.f, bottom, W, top, withAlpha(p.mBaseTop, 0.94f), withAlpha(p.mBaseBottom, 0.96f));
        rect(0.f, top - 1.f, W, top, p.mEdge);
        rect(W * 0.2f, top - 2.f, W * 0.8f, top - 1.f, withAlpha(p.mAccent, 0.35f));
        chamfer(rl, bottom, rr, raise_t, rcut, withAlpha(p.mBaseTop, 0.97f), withAlpha(p.mBaseBottom, 0.97f));
        chamferEdge(rl + 0.5f, bottom + 0.5f, rr - 0.5f, raise_t - 0.5f, rcut, 1.f, p.mEdge);
        // not a hole to the world (added first: everything on it is on top)
        mCombatHits.add(0.f, bottom, W, top, H_PLATE, 0, std::string(), false);
        mCombatHits.add(rl, top, rr, raise_t, H_PLATE, 1, std::string(), false);
    }

    // ---- centre: the 12 action slots, always - the empty ones drawn - then the weapons ----
    const S32 n_ab = (S32)g.abilities().size();
    for (S32 i = 0; i < 12; ++i)
    {
        const F32 x = cx + (F32)i * (S + gap);
        if (i < n_ab)
        {
            drawActionSlot(i, x, cb, S);
        }
        else
        {
            drawEmptySlot(x, cb, S, actionKey(i));
            mCombatHits.add(x, cb, x + S, cb + S, H_EMPTY, i,
                            "Key " + actionKey(i) + ": empty - the game puts its abilities on this bar", false);
        }
    }
    const F32 wx = cx + ab + floorf(0.35f * S);
    textL(small_bold, "WEAPONS", wx + ((F32)n_w * S + (F32)(n_w - 1) * gap) * 0.5f, cb + S + 2.f + slh, p.mDim,
          LLFontGL::HCENTER, LLFontGL::TOP);
    for (S32 k = 0; k < n_w; ++k) drawWeaponSlot(weapons[k], wx + (F32)k * (S + gap), cb, S);

    // ---- left: the portrait with the XP ring and the level, then the orbs ----
    const F32 oy = wide ? bottom + Hp * 0.5f : bottom + floorf(0.12f * S) + D * 0.5f;
    drawPortraitXP(m + P * 0.5f, oy, P * 0.5f * 0.78f);
    F32 ox = m + P + og;
    for (S32 i = 0; i < n_orbs; ++i)
    {
        drawOrb(g.stats()[i], i, ox + D * 0.5f, oy, D * 0.5f - 3.5f);
        ox += D + og;
    }

    // ---- right: the first 8 bag slots, the wallet under them ----
    const F32 rx = W - m - right_w;
    const F32 qb = wide ? top - q - floorf(0.22f * S) : bottom + floorf(0.75f * S);
    const std::vector<WolfGame::Item>& items = g.items();
    const LLFontGL* count_font = slotFont(q);
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 x = rx + (F32)i * (q + qg);
        if (i < (S32)items.size())
        {
            const WolfGame::Item& it = items[i];
            const std::string& label = it.mLabel.empty() ? it.mName : it.mLabel;
            icon(it.mIcon, x, qb, q, p, initials(label));
            count_font->renderUTF8(thousands(it.mQty), 0, x + q - 3.f, qb + 2.f, LLColor4::white, LLFontGL::RIGHT, LLFontGL::BOTTOM,
                                   LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            if (mCombatHits.isHover(H_BAG, i)) chamferEdge(x + 0.5f, qb + 0.5f, x + q - 0.5f, qb + q - 0.5f, 4.f, 1.5f, p.mAccent);
            mCombatHits.add(x, qb, x + q, qb + q, H_BAG, i, label + " x" + thousands(it.mQty) + "\nClick for your bag");
        }
        else
        {
            drawEmptySlot(x, qb, q, std::string());
            mCombatHits.add(x, qb, x + q, qb + q, H_EMPTY, 100 + i, "An empty bag slot - the game's objects in the world give you items", false);
        }
    }
    {
        const F32 wy = bottom + (qb - bottom) * 0.5f;
        const std::string wallet = currentWallet();
        F32 wr = rx;
        if (!wallet.empty())
        {
            const F32 cr = floorf(0.2f * S);
            walletIcon(rx + cr, wy, cr, p);
            wr = textL(display(0.36f * S), wallet, rx + 2.f * cr + 6.f, wy, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER,
                       right_w - 2.f * cr - 6.f);
            mCombatHits.add(rx, wy - cr - 2.f, wr, wy + cr + 2.f, H_WALLET, 0, "Your money: " + wallet + "\nClick for your bag");
        }
        const std::string held = items.empty() ? std::string("Bag empty") : "Bag: " + thousands((S64)items.size()) + (items.size() == 1 ? " kind" : " kinds");
        if (small->getWidthF32(held) < rx + right_w - wr - 10.f)
        {
            textL(small, held, rx + right_w, wy, p.mDim, LLFontGL::RIGHT, LLFontGL::VCENTER);
        }
    }

    // ---- the top edge: the console's pages and PANEL on the right, the name plate on the left ----
    const LLFontGL* tf = slotFont(S);
    const F32 th = llmax(22.f, floorf(0.5f * S)), tpad = 9.f, tcut = 6.f;
    struct Tab { const char* label; S32 page; const char* tip; };
    static const Tab TABS[] = {
        { "Character", WolfGameConsole::PAGE_STATS,  "Your character: every stat, your level and the game's panel" },
        { "Combat",    WolfGameConsole::PAGE_COMBAT, "Your weapons, armour and abilities" },
        { "Bag",       WolfGameConsole::PAGE_BAG,    "Everything you carry, and your money" },
        { "Pets",      WolfGameConsole::PAGE_PETS,   "Your pets" },
        { "Game",      WolfGameConsole::PAGE_GAME,   "This game: who runs it, the rules, Leave this game" },
        { "Look",      WolfGameConsole::PAGE_LOOK,   "The Roleplay window's background picture" },
        { "Help",      WolfGameConsole::PAGE_HELP,   "How to play, and the credits" },
        { "Panel",     -1,                           "" },
        // Paul 2026-10-09: "add a game on off button on the panel to show whether it's on or off"
        { "Game Mode", -2,                           "" },
    };
    F32 tx = W - m;
    for (S32 i = (S32)(sizeof(TABS) / sizeof(TABS[0])) - 1; i >= 0; --i)
    {
        const Tab& t = TABS[i];
        std::string label = caps(t.label);
        if (t.page == -2)
        {
            const bool on_now = g.gameModeShownOn();
            label = g.forced() ? "GAME: LOCKED ON" : g.gameModeConnecting() ? "GAME: CONNECTING..." : on_now ? "GAME: ON" : "GAME: OFF";
        }
        const F32 w = tf->getWidthF32(label) + 2.f * tpad;
        const F32 l = tx - w;
        const bool sw = t.page == -2;
        const bool panel = t.page == -1;
        const bool mode_on = g.gameModeShownOn();
        const S32 id = sw ? H_GAMEMODE_SW : panel ? H_PANEL_BG : H_PAGE;
        const bool hover = mCombatHits.isHover(id, t.page);
        const bool active = sw ? mode_on : panel ? (bool)show_bg : WolfFloaterRoleplay::showingPage((WolfGameConsole::EPage)t.page);
        tabShape(l, top, tx, top + th, tcut, p, hover, (!panel || sw) && active);
        // PANEL off: the word dims and the tooltip says so (never colour alone)
        textL(tf, label, (l + tx) * 0.5f, top + th * 0.5f, panel && !active ? p.mDim : (hover || active ? p.mAccent : p.mText),
              LLFontGL::HCENTER, LLFontGL::VCENTER);
        std::string tip = t.tip;
        if (sw) tip = g.forced() ? "Game Mode: LOCKED ON - " + g.forcedText()
                                 : (active ? "Game Mode: ON - click to switch it off (the game HUD goes)" : "Game Mode: OFF - click to switch it on");
        else if (panel) tip = show_bg ? "Panel: on - click to hide the panel behind the HUD (the pieces stay)" : "Panel: off - click to show the panel behind the HUD";
        else tip += active ? "\nOpen now - click to close it" : "\nOpens the Roleplay window on this page";
        mCombatHits.add(l, top, tx, top + th, id, t.page, tip);
        tx = l - floorf(0.08f * S);
    }
    if (wide)
    {
        // Name, level and XP (wide only: a narrow window has no room beside the buttons; the
        // portrait's tooltip has the same).
        std::string name;
        LLAvatarName av;
        if (LLAvatarNameCache::get(gAgentID, &av)) name = av.getDisplayName();
        if (name.empty()) name = g.game().mValid && !g.game().mName.empty() ? g.game().mName : std::string("Roleplay");
        const LLFontGL* nf = display(th * 0.62f);
        std::string sub;
        if (g.hasLevel())
        {
            sub = "Level " + thousands(g.level());
            if (g.xpNext() > 0) sub += "  -  " + thousands(g.xp()) + " / " + thousands(g.xpNext()) + " XP";
        }
        const F32 w = tpad * 2.f + nf->getWidthF32(name) + (sub.empty() ? 0.f : 12.f + small->getWidthF32(sub));
        if (m + w < tx - floorf(0.3f * S))
        {
            tabShape(m, top, m + w, top + th, tcut, p, false, false);
            const F32 after = textL(nf, name, m + tpad, top + th * 0.5f, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER);
            if (!sub.empty()) textL(small, sub, after + 12.f, top + th * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
            mCombatHits.add(m, top, m + w, top + th, H_PORTRAIT, 1, name + (sub.empty() ? std::string() : " - " + sub) + "\nClick for your character");
        }
    }

    // ---- above the action bar, in the world: the cast bar, and the buffs above it (§5.4, §5.6) ----
    const F32 ccx = cx + ab * 0.5f;
    const F32 cw = llmin(6.f * S, ab * 0.5f), ch = llmax(14.f, floorf(0.3f * S));
    const F32 cast_b = raise_t + floorf(0.3f * S);
    if (g.castBar().mOn) drawCastBar(ccx, cast_b, cw, ch);
    if (!g.buffs().empty()) drawBuffs(ccx, cast_b + ch + slh + floorf(0.35f * S), llmax(28.f, floorf(0.62f * S)));

    // ---- the first-time guide: callouts at the HUD's pieces and a small card above its right
    //      end - nothing over the middle of the world, clicks pass round it, "Got it" ends it ----
    if (g.guideOn())
    {
        const LLFontGL* gf = LLFontGL::getFontSansSerif();
        const F32 glh = (F32)gf->getLineHeight();
        auto callout = [&](F32 l, F32 b, F32 w, F32 ax, const std::string& text)
        {
            const std::vector<std::string> lines = wrap(gf, text, w - 18.f, 4);
            const F32 h = (F32)lines.size() * glh + 12.f;
            card(l, b, l + w, b + h, p, 4.f);
            chamferEdge(l + 0.5f, b + 0.5f, l + w - 0.5f, b + h - 0.5f, 4.f, 1.f, p.mAccent);
            triF(ax - 7.f, b, ax + 7.f, b, ax, b - 8.f, p.mAccent);
            F32 y = b + h - 6.f;
            for (const std::string& line : lines)
            {
                textL(gf, line, l + 9.f, y, p.mText);
                y -= glh;
            }
            return b + h;
        };
        const F32 gb = top + th + 14.f;
        const F32 lw = llmin(260.f, llmax(180.f, left_w));
        callout(m, gb, lw, m + P * 0.5f, "You. The orbs are your first three stats; the ring round the picture is your XP.");
        // above the cast bar and the buffs' row, so it covers neither
        const F32 buffs_top = cast_b + ch + slh + floorf(0.35f * S) + llmax(28.f, floorf(0.62f * S)) + slh + 2.f;
        callout(cx, buffs_top + 14.f, llmin(280.f, ab * 0.45f), cx + S * 0.5f,
                "Keys 1 to 9, 0, - and = use your abilities. Click a creature in the world to target it.");
        const F32 right_top = callout(rx, gb, right_w, rx + q * 0.5f, "Your bag and your money. The buttons above open the full Roleplay window.");
        // the card
        const LLFontGL* title_font = display(17.f);
        const F32 cw2 = llmax(280.f, right_w), cl = W - m - cw2, cbt = right_top + 16.f;
        const std::vector<std::string> body = wrap(gf, "Game Mode is on and the game is along the bottom. Keep playing while this is up.", cw2 - 24.f, 4);
        const F32 bh = 26.f;
        const F32 chh = 12.f + (F32)title_font->getLineHeight() + 4.f + (F32)body.size() * glh + 10.f + bh + 12.f;
        frame(cl, cbt, cl + cw2, cbt + chh, p, 1.f, 8.f);
        mCombatHits.add(cl, cbt, cl + cw2, cbt + chh, H_GUIDE_CARD, 0, std::string(), false);
        F32 y = cbt + chh - 12.f;
        textL(title_font, "Game Mode is on", cl + 12.f, y, p.mAccent);
        y -= (F32)title_font->getLineHeight() + 4.f;
        for (const std::string& line : body)
        {
            textL(gf, line, cl + 12.f, y, p.mText);
            y -= glh;
        }
        const F32 b1l = cl + 12.f, b1r = b1l + 90.f, b2l = b1r + 10.f, b2r = b2l + 110.f, bb = cbt + 12.f;
        button(b1l, bb, b1r, bb + bh, "Got it", p, BTN_PRIMARY | (mCombatHits.isHover(H_GUIDE_OK, 0) ? BTN_HOVER : 0), small_bold);
        button(b2l, bb, b2r, bb + bh, "Full help", p, mCombatHits.isHover(H_GUIDE_HELP, 0) ? BTN_HOVER : 0, small_bold);
        mCombatHits.add(b1l, bb, b1r, bb + bh, H_GUIDE_OK, 0, "Close this guide");
        mCombatHits.add(b2l, bb, b2r, bb + bh, H_GUIDE_HELP, 0, "Open the Roleplay window's Help page");
    }
}

void WolfGameHUD::drawEmptySlot(F32 x, F32 b, F32 S, const std::string& key)
{
    const Palette& p = gamePalette();
    const F32 cut = llmin(5.f, S * 0.15f);
    chamfer(x, b, x + S, b + S, cut, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.03f, 0.04f, 0.07f, 0.6f));
    {
        // faint hatching: an empty slot, not a missing one
        LLLocalClipRect clip(LLRect((S32)x + 2, (S32)(b + S) - 2, (S32)(x + S) - 2, (S32)b + 2));
        for (F32 d = -S; d < S; d += S * 0.18f) lineW(x + d, b, x + d + S, b + S, 1.f, LLColor4(1.f, 1.f, 1.f, 0.035f));
    }
    chamferEdge(x + 0.5f, b + 0.5f, x + S - 0.5f, b + S - 0.5f, cut, 1.f, withAlpha(p.mDim, 0.35f));
    if (!key.empty())
    {
        slotFont(S)->renderUTF8(key, 0, x + 3.f, b + S - 2.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}

void WolfGameHUD::drawActionSlot(S32 i, F32 x, F32 sb, F32 S)
{
    WolfGame& g = WolfGame::instance();
    const Palette& p = gamePalette();
    const WolfGame::Ability& a = g.abilities()[i];
    const Palette& ap = palette(a.mVerified);
    const LLFontGL* key_font = slotFont(S);
    const F32 st = sb + S;
    const F64 now = nowSecs();
    const WolfGame::Cast& cast = g.castBar();
    icon(a.mIcon, x, sb, S, ap, initials(a.mLabel));
    std::vector<std::string> state;
    // casting: the button glows
    if (cast.mOn && cast.mEnd.empty() && cast.mId == a.mId)
    {
        chamferEdge(x - 2.f, sb - 2.f, x + S + 2.f, st + 2.f, 6.f, 3.f, p.mAccent);
        state.push_back("casting");
    }
    // the cooldown sweep (§5.4), drawn from endsAt, never waiting for the sim
    if (a.mCooldownEnds > now && a.mCooldownTotal > 0.f)
    {
        const F64 left = a.mCooldownEnds - now;
        {
            LLLocalClipRect clip(LLRect((S32)x + 2, (S32)st - 2, (S32)(x + S) - 2, (S32)sb + 2));
            sweep(x, sb, x + S, st, 1.f - (F32)(left / a.mCooldownTotal), LLColor4(0.02f, 0.03f, 0.06f, 0.62f));
        }
        if (left >= 1.0)
        {
            const std::string secs = left < 3.0 ? llformat("%.1f", left) : llformat("%d", (S32)ceil(left));
            display(S * 0.42f)->renderUTF8(secs, 0, x + S * 0.5f, sb + S * 0.5f, LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER,
                                           LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
        state.push_back(llformat("cooling down, %d s", (S32)ceil(left)));
    }
    else if (a.mReadyAt > 0.0 && now - a.mReadyAt < 0.4)
    {
        // the brief "ready" pulse
        chamferEdge(x - 1.f, sb - 1.f, x + S + 1.f, st + 1.f, 6.f, 2.f, withAlpha(p.mAccent, (F32)(1.0 - (now - a.mReadyAt) / 0.4)));
    }
    if (now - a.mPressedAt < 0.1) rect(x + 2.f, sb + 2.f, x + S - 2.f, st - 2.f, LLColor4(1.f, 1.f, 1.f, 0.22f));
    if (!a.mOn)
    {
        rect(x + 2.f, sb + 2.f, x + S - 2.f, st - 2.f, LLColor4(0.f, 0.f, 0.f, 0.55f));
        lockGlyph(x + S * 0.5f, sb + S * 0.5f, S * 0.32f, p.mText);
        state.push_back("not available");
    }
    // the key, top-left
    key_font->renderUTF8(actionKey(i), 0, x + 3.f, st - 2.f, LLColor4::white, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    // the cost, bottom-right: the amount in the stat's colour and its initial; "LOW" when short
    if (a.mAmount > 0 && !a.mCost.empty())
    {
        LLColor4 sc = p.mText;
        S32 have = 0;
        const bool known = statOf(a.mCost, sc, have);
        const bool low = known && have < a.mAmount;
        const std::string cost = std::to_string(a.mAmount) + firstLetter(a.mCostLabel.empty() ? a.mCost : a.mCostLabel);
        key_font->renderUTF8(cost, 0, x + S - 2.f, sb + 2.f, low ? DANGER_TEXT : readable(sc, 0.4f), LLFontGL::RIGHT, LLFontGL::BOTTOM,
                             LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        if (low)
        {
            key_font->renderUTF8("LOW", 0, x + S - 2.f, st - 2.f, DANGER_TEXT, LLFontGL::RIGHT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            state.push_back("not enough " + (a.mCostLabel.empty() ? a.mCost : a.mCostLabel));
        }
    }
    // the ammo left, bottom-left, when the weapon's ammo is tracked
    if (!a.mWeapon.empty())
    {
        if (const WolfGame::Ammo* am = g.ammoFor(a.mWeapon))
        {
            if (am->mRounds >= 0)
            {
                const bool out = am->mRounds == 0;
                key_font->renderUTF8(std::to_string(am->mRounds), 0, x + 3.f, sb + 2.f, out ? DANGER_TEXT : LLColor4::white,
                                     LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
                if (out)
                {
                    reloadGlyph(x + S * 0.5f, sb + S * 0.5f, S * 0.45f, DANGER_TEXT);
                    state.push_back("out of ammo - reload");
                }
            }
            if (am->mReloadEnds > now && am->mReloadTotal > 0.f)
            {
                LLLocalClipRect clip(LLRect((S32)x + 2, (S32)st - 2, (S32)(x + S) - 2, (S32)sb + 2));
                sweep(x, sb, x + S, st, 1.f - (F32)((am->mReloadEnds - now) / am->mReloadTotal), LLColor4(0.02f, 0.03f, 0.06f, 0.62f));
                state.push_back("reloading");
            }
        }
    }
    // needs a target and there is none: a small crosshair mark (the tgt field)
    if (a.mNeedsTarget && !g.target().mOn)
    {
        crosshairGlyph(x + S - 8.f, st - 16.f, 10.f, p.mDim);
        state.push_back("needs a target");
    }
    if (mCombatHits.isHover(H_ACTION, i)) chamferEdge(x + 0.5f, sb + 0.5f, x + S - 0.5f, st - 0.5f, 5.f, 1.5f, p.mAccent);
    // the accessible name: "Fireball, 20 Mana, key 2, ready" (§5.4)
    std::string tip = a.mLabel;
    if (a.mAmount > 0 && !a.mCost.empty()) tip += ", " + std::to_string(a.mAmount) + " " + (a.mCostLabel.empty() ? a.mCost : a.mCostLabel);
    if (a.mCast > 0.f) tip += llformat(", %.1f s cast", a.mCast);
    if (a.mCooldown > 0.f) tip += llformat(", %.0f s cooldown", a.mCooldown);
    tip += ", key " + actionKey(i);
    if (!a.mKey.empty() && a.mKey != actionKey(i)) tip += " (the game calls it " + a.mKey + ")";
    if (state.empty()) tip += ", ready";
    for (const std::string& w : state) tip += ", " + w;
    if (!a.mSrc.empty()) tip += "\nFrom " + a.mSrc;
    if (!a.mVerified) tip += " - Unverified: not from this region's game";
    mCombatHits.add(x, sb, x + S, st, H_ACTION, i, tip);
}

void WolfGameHUD::drawWeaponSlot(S32 idx, F32 x, F32 b, F32 S)
{
    WolfGame& g = WolfGame::instance();
    const std::string& slot = WolfGame::weaponSlots()[idx];
    const WolfGame::SlotView v = g.slotView(slot);
    const Palette& p = palette(v.mVerified);
    const LLFontGL* f = slotFont(S);
    const F32 t = b + S;
    const F64 now = nowSecs();
    if (v.mFilled)
    {
        icon(v.mIcon, x, b, S, p, initials(v.mLabel));
    }
    else
    {
        drawEmptySlot(x, b, S, std::string());
        textL(LLFontGL::getFontSansSerifSmall(), slot == "main" ? "Main" : slot == "off" ? "Off" : "Ranged", x + S * 0.5f, b + S * 0.5f,
              p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER, S - 4.f);
    }
    const bool active = v.mFilled && g.activeWeapon() == slot;
    if (active) chamferEdge(x - 1.5f, b - 1.5f, x + S + 1.5f, t + 1.5f, 6.f, 2.f, p.mAccent);
    std::string tip = WolfGame::slotLabel(slot) + ": " + (v.mFilled ? v.mLabel : std::string("nothing equipped"));
    std::string second = v.mFireMode;
    if (!v.mDamage.empty()) second += (second.empty() ? "" : " - ") + v.mDamage;
    if (!v.mRange.empty()) second += (second.empty() ? "" : " - ") + v.mRange;
    if (!second.empty()) tip += " (" + second + ")";
    if (active) tip += ", the active weapon";
    // ammo (§5.3): the rounds bottom-right, red with a reload glyph at 0; the reload as a sweep
    bool can_reload = false;
    const WolfGame::Ammo* am = g.ammoFor(slot);
    if (v.mFilled && am && am->mRounds >= 0)
    {
        const bool out = am->mRounds == 0;
        const std::string count = thousands(am->mRounds) + (am->mMagazine > 0 ? " / " + thousands(am->mMagazine) : std::string());
        f->renderUTF8(thousands(am->mRounds), 0, x + S - 3.f, b + 2.f, out ? DANGER_TEXT : LLColor4::white, LLFontGL::RIGHT, LLFontGL::BOTTOM,
                      LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        tip += ", " + count + (am->mReserve >= 0 ? ", " + thousands(am->mReserve) + " in reserve" : std::string());
        if (am->mReloadEnds > now && am->mReloadTotal > 0.f)
        {
            LLLocalClipRect clip(LLRect((S32)x + 2, (S32)t - 2, (S32)(x + S) - 2, (S32)b + 2));
            sweep(x, b, x + S, t, 1.f - (F32)((am->mReloadEnds - now) / am->mReloadTotal), LLColor4(0.02f, 0.03f, 0.06f, 0.62f));
            tip += ", reloading";
        }
        else
        {
            can_reload = true;
            if (out)
            {
                reloadGlyph(x + S * 0.5f, b + S * 0.5f, S * 0.45f, DANGER_TEXT);
                tip += ", empty";
            }
        }
    }
    tip += can_reload ? "\nClick to reload" : "\nClick for your weapons and armour";
    if (!v.mSrc.empty()) tip += "\nFrom " + v.mSrc;
    if (!v.mVerified)
    {
        // a caution mark AND the word in the tooltip (§3: never colour alone)
        tip += " - Unverified: not from this region's game";
        triF(x + S - 13.f, t - 13.f, x + S - 3.f, t - 13.f, x + S - 8.f, t - 3.f, KIND_COLOUR[WolfGame::NOTICE_WARNING]);
    }
    if (mCombatHits.isHover(H_WEAPON, idx * 2 + (can_reload ? 1 : 0))) chamferEdge(x + 0.5f, b + 0.5f, x + S - 0.5f, t - 0.5f, 5.f, 1.5f, p.mAccent);
    mCombatHits.add(x, b, x + S, t, H_WEAPON, idx * 2 + (can_reload ? 1 : 0), tip);
}

void WolfGameHUD::drawOrb(const WolfGame::Stat& s, S32 i, F32 cx, F32 cy, F32 R)
{
    const Palette& p = gamePalette();
    const F32 frac = s.mMax > 0 ? llclamp((F32)s.mValue / (F32)s.mMax, 0.f, 1.f) : 0.f;
    // the bezel, then the glass's dark inside (Diablo's globes)
    disc(cx, cy, R + 3.5f, LLColor4(0.07f, 0.05f, 0.02f, 0.95f), 48);
    arc(cx, cy, R + 2.f, 1.5f, 0.f, 2.f * F_PI, withAlpha(p.mAccent, 0.6f), 48);
    disc(cx, cy, R, LLColor4(0.02f, 0.03f, 0.05f, 0.96f), 48);
    // the liquid, in the stat's own colour, up to its level: chords from the bottom up
    const LLColor4 hi(s.mColor.mV[VRED], s.mColor.mV[VGREEN], s.mColor.mV[VBLUE], 1.f);
    const LLColor4 lo(hi.mV[VRED] * 0.3f, hi.mV[VGREEN] * 0.3f, hi.mV[VBLUE] * 0.3f, 1.f);
    const F32 y_lo = cy - R, level = cy - R + 2.f * R * frac;
    auto half = [&](F32 y) { const F32 d = y - cy; return sqrtf(llmax(0.f, R * R - d * d)) * 0.99f; };
    if (frac > 0.f)
    {
        const S32 steps = llmax(4, (S32)(R * 0.6f));
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 k = 0; k < steps; ++k)
        {
            const F32 y0 = y_lo + (level - y_lo) * (F32)k / (F32)steps, y1 = y_lo + (level - y_lo) * (F32)(k + 1) / (F32)steps;
            const F32 h0 = half(y0), h1 = half(y1);
            const LLColor4 c0 = mix(lo, hi, (y0 - y_lo) / (2.f * R)), c1 = mix(lo, hi, (y1 - y_lo) / (2.f * R));
            gGL.color4fv(c0.mV); gGL.vertex2f(cx - h0, y0);
            gGL.color4fv(c0.mV); gGL.vertex2f(cx + h0, y0);
            gGL.color4fv(c1.mV); gGL.vertex2f(cx + h1, y1);
            gGL.color4fv(c0.mV); gGL.vertex2f(cx - h0, y0);
            gGL.color4fv(c1.mV); gGL.vertex2f(cx + h1, y1);
            gGL.color4fv(c1.mV); gGL.vertex2f(cx - h1, y1);
        }
        gGL.end();
        if (frac < 0.98f) lineW(cx - half(level), level, cx + half(level), level, 1.5f, LLColor4(1.f, 1.f, 1.f, 0.35f));
    }
    // the glass's shine
    ellipseF(cx - R * 0.22f, cy + R * 0.45f, R * 0.46f, R * 0.2f, LLColor4(1.f, 1.f, 1.f, 0.16f));
    // the number and the stat's name, each on a dark pill (never the colour alone)
    pillText(display(R * 0.5f), thousands(s.mValue), cx, cy + R * 0.08f, LLColor4::white);
    pillText(LLFontGL::getFontSansSerifSmallBold(), caps(s.mLabel.empty() ? s.mName : s.mLabel), cx, cy - R * 0.45f, LLColor4::white);
    if (mCombatHits.isHover(H_ORB, i)) arc(cx, cy, R + 2.f, 2.f, 0.f, 2.f * F_PI, p.mAccent, 48);
    mCombatHits.add(cx - R, cy - R, cx + R, cy + R, H_ORB, i,
                    (s.mLabel.empty() ? s.mName : s.mLabel) + " " + thousands(s.mValue) + " / " + thousands(s.mMax) + "\nClick for your character");
}

void WolfGameHUD::drawPortraitXP(F32 cx, F32 cy, F32 R)
{
    WolfGame& g = WolfGame::instance();
    const Palette& p = gamePalette();
    const F32 rw = llmax(3.f, R * 0.14f);
    const F32 rr = R + 2.f + rw * 0.5f;
    disc(cx, cy, rr + rw * 0.5f + 2.f, withAlpha(p.mBaseBottom, 0.9f), 48);
    // the XP ring: filled clockwise from 12 o'clock
    arc(cx, cy, rr, rw, 0.f, 2.f * F_PI, p.mTrack, 48);
    const bool has_xp = g.hasLevel() && g.xpNext() > 0;
    const F32 frac = has_xp ? llclamp((F32)g.xp() / (F32)g.xpNext(), 0.f, 1.f) : 0.f;
    if (frac > 0.f) arc(cx, cy, rr, rw, F_PI * 0.5f - 2.f * F_PI * frac, F_PI * 0.5f, p.mAccent, 48);
    portraitFace(cx, cy, R, p);
    if (g.dead() || g.downed()) disc(cx, cy, R, LLColor4(0.35f, 0.f, 0.f, 0.55f), 48);
    if (g.hasLevel()) levelBadge(cx, cy - R - rw * 0.5f, R, p);
    if (mCombatHits.isHover(H_PORTRAIT, 0)) arc(cx, cy, rr + rw * 0.5f + 1.f, 1.5f, 0.f, 2.f * F_PI, p.mAccent, 48);
    std::string name;
    LLAvatarName av;
    if (LLAvatarNameCache::get(gAgentID, &av)) name = av.getDisplayName();
    std::string tip = name.empty() ? std::string("You") : name;
    if (g.hasLevel()) tip += ", level " + thousands(g.level());
    if (has_xp) tip += std::string(" - XP ") + thousands(g.xp()) + " / " + thousands(g.xpNext()) + llformat(" (%d%%)", (S32)(frac * 100.f));
    if (g.dead()) tip += " - dead";
    else if (g.downed()) tip += " - down";
    tip += "\nClick for your character";
    mCombatHits.add(cx - rr, cy - rr, cx + rr, cy + rr, H_PORTRAIT, 0, tip);
}

void WolfGameHUD::drawCastBar(F32 cx, F32 bottom, F32 W, F32 H)
{
    WolfGame& g = WolfGame::instance();
    const WolfGame::Cast& c = g.castBar();
    const Palette& p = gamePalette();
    const LLFontGL* small_bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small_bold->getLineHeight();
    // §5.4: centred above the action bar
    const F32 l = cx - W * 0.5f, b = bottom, t = b + H;
    card(l - 6.f, b - 5.f, l + W + 30.f, t + slh + 6.f, p, 5.f);
    const F64 now = nowSecs();
    rect(l, b, l + W, t, p.mTrack);
    std::string right;
    if (c.mEnd.empty())
    {
        const F64 left = llmax(0.0, c.mEnds - now);
        const F32 frac = c.mSeconds > 0.f ? llclamp(1.f - (F32)(left / c.mSeconds), 0.f, 1.f) : 1.f;
        rectG(l, b, l + W * frac, t, readable(p.mAccent, 0.6f), p.mAccent);
        right = llformat("%.1f s", left);
        // the X that cancels (and Escape) - §5.4
        const F32 xc = l + W + 14.f, yc = (b + t) * 0.5f;
        closeX(xc, yc, 4.f, mCombatHits.isHover(H_CAST_CANCEL, 0) ? p.mAccent : p.mText);
        mCombatHits.add(xc - 11.f, yc - 11.f, xc + 11.f, yc + 11.f, H_CAST_CANCEL, 0, "Cancel the cast (Escape)");
    }
    else if (c.mEnd == "done")
    {
        const F32 fade = llclamp(1.f - (F32)(now - c.mEndedAt), 0.f, 1.f);
        rectG(l, b, l + W, t, withAlpha(readable(p.mAccent, 0.6f), fade), withAlpha(p.mAccent, fade));
        right = "Done";
    }
    else
    {
        // "Interrupted" (red, with the word) or "Cancelled" stays for 1 s
        right = c.mEnd == "interrupted" ? "Interrupted" : "Cancelled";
        if (c.mEnd == "interrupted") rect(l, b, l + W, t, withAlpha(HOSTILE, 0.6f));
    }
    textL(small_bold, c.mLabel, l, t + slh + 2.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, W * 0.65f);
    textL(small_bold, right, l + W, t + slh + 2.f, c.mEnd == "interrupted" ? DANGER_TEXT : p.mText, LLFontGL::RIGHT, LLFontGL::TOP);
}

void WolfGameHUD::drawBuffs(F32 cx, F32 bottom, F32 S)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = slotFont(S);
    const F32 GAP = floorf(S * 0.2f), lh = (F32)small->getLineHeight();
    const F64 t = nowSecs();
    const std::vector<WolfGame::Buff>& bs = g.buffs();
    const S32 n = (S32)bs.size(), PER = 10;
    // rows of 10, centred over the action bar, the first row lowest
    for (S32 i = 0; i < n; ++i)
    {
        const WolfGame::Buff& b = bs[i];
        const Palette& p = palette(b.mVerified);
        const S32 row = i / PER, col = i % PER, in_row = llmin(PER, n - row * PER);
        const F32 row_w = (F32)in_row * S + (F32)(in_row - 1) * GAP;
        const F32 l = cx - row_w * 0.5f + (F32)col * (S + GAP);
        const F32 bt = bottom + lh + 2.f + (F32)row * (S + lh + 8.f), y = bt + S;
        icon(b.mIcon, l, bt, S, p, initials(b.mLabel));
        if (!b.mGood)
        {
            // Debuffs: a red border AND a down-arrow (§5.6: never colour alone).
            chamferEdge(l + 0.5f, bt + 0.5f, l + S - 0.5f, y - 0.5f, 4.f, 2.f, HOSTILE);
            arrowGlyph(l + S * 0.22f, y - S * 0.22f, S * 0.28f, true, HOSTILE);
        }
        if (b.mDuration > 0.f)
        {
            // the time used, as a thin bar along the foot of the icon
            const F32 used = llclamp(1.f - (F32)((b.mEnds - t) / b.mDuration), 0.f, 1.f);
            rect(l + 2.f, bt + 2.f, l + 2.f + (S - 4.f) * used, bt + 4.f, LLColor4(0.f, 0.f, 0.f, 0.7f));
            // the time left on a dark pill, readable over any sky
            const std::string left = shortTime(llmax(0.0, b.mEnds - t));
            const F32 tw2 = small->getWidthF32(left) * 0.5f + 3.f;
            rect(l + S * 0.5f - tw2, bt - 1.f - lh, l + S * 0.5f + tw2, bt - 1.f, LLColor4(0.f, 0.f, 0.f, 0.75f));
            textL(small, left, l + S * 0.5f, bt - 1.f, b.mGood ? p.mText : DANGER_TEXT, LLFontGL::HCENTER, LLFontGL::TOP);
        }
        if (b.mStacks > 1)
        {
            bold->renderUTF8(std::to_string(b.mStacks), 0, l + S - 2.f, bt + 2.f, LLColor4::white, LLFontGL::RIGHT, LLFontGL::BOTTOM,
                             LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
        std::string tip = b.mLabel + (b.mGood ? " (helpful)" : " (harmful)");
        if (b.mDuration > 0.f) tip += ", " + shortTime(llmax(0.0, b.mEnds - t)) + " left";
        if (b.mStacks > 1) tip += ", " + std::to_string(b.mStacks) + " stacks";
        if (!b.mText.empty()) tip += "\n" + b.mText;
        if (!b.mSrc.empty()) tip += "\nFrom " + b.mSrc;
        if (!b.mVerified) tip += " - Unverified: not from this region's game";
        mCombatHits.add(l, bt - lh, l + S, y, H_BUFF, i, tip, false);
    }
}

void WolfGameHUD::drawCombatText(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    const F64 now = nowSecs();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    for (const WolfGame::CombatText& c : g.combatText())
    {
        const F32 age = (F32)(now - c.mBorn);
        if (age < 0.f || age > 1.2f) continue;
        // where it rises from: the avatar or object it is about, else the screen centre (§5.6)
        F32 x = (F32)r.getCenterX(), y = (F32)r.getCenterY();
        LLViewerObject* obj = c.mSelf ? (LLViewerObject*)gAgentAvatarp : gObjectList.findObject(c.mAt);
        if (obj && !obj->isDead())
        {
            LLVector3 at = obj->getPositionAgent();
            at.mV[VZ] += obj->isAvatar() ? 1.0f : obj->getScale().mV[VZ] * 0.5f;
            LLCoordGL screen;
            if (cam->projectPosAgentToScreen(at, screen, false))
            {
                S32 lx = 0, ly = 0;
                screenPointToLocal(screen.mX, screen.mY, &lx, &ly);
                x = (F32)lx;
                y = (F32)ly;
            }
        }
        x += (F32)((S32)(c.mSerial * 37 % 61) - 30);   // a little spread, so numbers do not stack
        y += 40.f * (age / 1.2f);                      // rises about 40 px over 1.2 s
        const F32 alpha = llclamp(1.f - age / 1.2f, 0.f, 1.f);
        std::string text;
        LLColor4 col(0.95f, 0.96f, 0.98f, 1.f);        // hit: white
        F32 size = 22.f;
        const std::string& k = c.mKind;
        if (k == "crit") { text = thousands(c.mAmount) + "!"; col = LLColor4(1.f, 0.82f, 0.48f, 1.f); size = 34.f * (age < 0.12f ? 1.f + (0.12f - age) * 2.f : 1.f); }
        else if (k == "heal") { text = "+" + thousands(c.mAmount); col = LLColor4(0.290f, 0.871f, 0.502f, 1.f); }
        else if (k == "absorb") { text = "(absorbed " + thousands(c.mAmount) + ")"; col = LLColor4(0.576f, 0.773f, 0.992f, 1.f); size = 18.f; }
        else if (k == "miss" || k == "dodge" || k == "block" || k == "immune") { text = caps(k); col = WOLF_SILVER_DIM; size = 18.f; }
        else text = thousands(c.mAmount);
        if (c.mSelf && (k == "hit" || k == "crit")) { text = "-" + text; col = LLColor4(0.973f, 0.443f, 0.443f, 1.f); }
        if (c.mHasColor) col = c.mColor;   // the shape and word are kept (§5.6)
        col.mV[VALPHA] = alpha;
        display(size)->renderUTF8(text, 0, x, y, col, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
    // the hit marker at the screen centre, 150 ms (§5.6)
    if (now - g.hitMarkerAt() < 0.15)
    {
        const bool crit = g.hitMarkerCrit();
        const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
        const F32 i = crit ? 7.f : 5.f, o = crit ? 16.f : 11.f;
        const LLColor4 c = crit ? LLColor4(1.f, 0.82f, 0.48f, 0.95f) : LLColor4(1.f, 1.f, 1.f, 0.9f);
        lineW(cx - o, cy - o, cx - i, cy - i, 2.f, c);
        lineW(cx + o, cy - o, cx + i, cy - i, 2.f, c);
        lineW(cx - o, cy + o, cx - i, cy + i, 2.f, c);
        lineW(cx + o, cy + o, cx + i, cy + i, 2.f, c);
    }
}

void WolfGameHUD::drawCombatError(const LLRect& r)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* f = display(19.f);
    const std::string& text = g.combatError();
    const F32 w = f->getWidthF32(text) + 40.f, h = (F32)f->getLineHeight() + 10.f;
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.mTop - (F32)r.getHeight() / 3.f;
    // On its own dark plate, so it reads over any sky; a warning triangle AND the words.
    card(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, gamePalette(), 5.f);
    const F32 tx = cx - w * 0.5f + 12.f;
    triF(tx - 7.f, cy - 6.f, tx + 7.f, cy - 6.f, tx, cy + 7.f, KIND_COLOUR[WolfGame::NOTICE_WARNING]);
    textL(f, text, tx + 12.f, cy, DANGER_TEXT, LLFontGL::LEFT, LLFontGL::VCENTER);
}

void WolfGameHUD::pickTarget(S32 sx, S32 sy)
{
    WolfGame& g = WolfGame::instance();
    // [FIX 2026-10-09] only while the game HUD shows (see draw): a click on a car with the HUD
    // hidden is a click on a car, not a target.
    if (!g.hudShowing() || !g.game().mValid || !WolfGrid::isOnWolfTerritories()) return;
    // Source: lltoolpie.cpp handleMouseDown picks at the click in root-view coordinates (the
    // press's point, turned to the root's coordinates in handleMouseDown).
    LLPickInfo pick = gViewerWindow->pickImmediate(sx, sy, false, false, false, true);
    LLPointer<LLViewerObject> obj = pick.getObject();
    if (obj.isNull() || obj->isDead()) return;   // land, sky: the target stays (Escape or X clears it)
    // An avatar's attachments count as the avatar (WolfGameModule.cs: "its attachments count as it").
    LLViewerObject* who = obj->isAttachment() ? (LLViewerObject*)obj->getAvatar() : obj->getRootEdit();
    if (!who || who == (LLViewerObject*)gAgentAvatarp) return;   // not yourself
    const std::string key = who->getID().asString();
    if (key == g.target().mKey) return;
    g.selectTarget(key);
}
