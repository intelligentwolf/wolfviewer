/**
 * @file wolfhudkit.cpp
 * @brief WolfViewer: the Wolf HUDs' shared parts kit (see wolfhudkit.h).
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

#include "wolfhudkit.h"
#include <cmath>

#include "llgltexture.h"
#include "llrender.h"
#include "llui.h"
#include "lluiimage.h"

namespace WolfHudKit
{
    namespace
    {
        // Source: textures.xml "WolfHudKit_Gunmetal" (wolfhud/hudkit_gunmetal.png, WolfHudKitMap::SHEET square)
        LLTexture* sheet()
        {
            static LLUIImagePtr img;
            if (img.isNull()) img = LLUI::getUIImage("WolfHudKit_Gunmetal");
            if (img.isNull()) return nullptr;
            LLTexture* t = img->getImage().get();
            if (!t) return nullptr;
            // Source: llgltexture.h hasGLTexture() - the GL image exists once the file has been decoded
            LLGLTexture* gt = dynamic_cast<LLGLTexture*>(t);
            if (!gt || !gt->hasGLTexture()) return nullptr;
            // the sheet must be the size the map was made for (a smaller one is still loading a coarser level)
            if (t->getWidth(0) != WolfHudKitMap::SHEET || t->getHeight(0) != WolfHudKitMap::SHEET) return nullptr;
            return t;
        }

        // A quad from sheet pixels (x0, y0)-(x1, y1), y down from the sheet's top, onto view (l, b)-(r, t), y up.
        // Source: llrender2dutils.cpp gl_draw_scaled_image_with_border - a UI image's top row is v = 1 (uv rect top 1).
        void quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 l, F32 b, F32 r, F32 t)
        {
            if (r - l <= 0.f || t - b <= 0.f) return;
            const F32 S = (F32)WolfHudKitMap::SHEET;
            const F32 u0 = x0 / S, u1 = x1 / S, vt = 1.f - y0 / S, vb = 1.f - y1 / S;
            gGL.texCoord2f(u0, vb); gGL.vertex2f(l, b);
            gGL.texCoord2f(u1, vb); gGL.vertex2f(r, b);
            gGL.texCoord2f(u1, vt); gGL.vertex2f(r, t);
            gGL.texCoord2f(u0, vb); gGL.vertex2f(l, b);
            gGL.texCoord2f(u1, vt); gGL.vertex2f(r, t);
            gGL.texCoord2f(u0, vt); gGL.vertex2f(l, t);
        }
    }

    bool ready()
    {
        return sheet() != nullptr;
    }

    bool part(EPart p, F32 l, F32 b, F32 r, F32 t, const LLColor4& tint, F32 k)
    {
        LLTexture* tex = sheet();
        if (!tex || p < 0 || p >= WolfHudKitMap::PART_COUNT) return false;
        if (l > r) std::swap(l, r);
        if (b > t) std::swap(b, t);
        const WolfHudKitMap::Part& s = WolfHudKitMap::PARTS[p];
        gGL.getTexUnit(0)->bind(tex);
        gGL.color4fv(tint.mV);
        gGL.begin(LLRender::TRIANGLES);
        const F32 X0 = (F32)s.x, Y0 = (F32)s.y, X3 = (F32)(s.x + s.w), Y3 = (F32)(s.y + s.h);
        if (s.l + s.t + s.r + s.b == 0)
        {
            quad(X0, Y0, X3, Y3, l, b, r, t);
        }
        else
        {
            const F32 w = r - l, h = t - b;
            if (k <= 0.f) k = llmin(w / (F32)s.w, h / (F32)s.h);
            // the borders on screen, never more than the rectangle has room for (both sides of each axis shrunk alike)
            F32 L = (F32)s.l * k, R = (F32)s.r * k, B = (F32)s.b * k, T = (F32)s.t * k;
            if (L + R > w) { const F32 f = w / (L + R); L *= f; R *= f; }
            if (B + T > h) { const F32 f = h / (B + T); B *= f; T *= f; }
            const F32 X1 = X0 + (F32)s.l, X2 = X3 - (F32)s.r, Y1 = Y0 + (F32)s.t, Y2 = Y3 - (F32)s.b;
            const F32 xs[4] = { l, l + L, r - R, r };
            const F32 ys[4] = { t, t - T, b + B, b };   // top to bottom, as the sheet's rows
            const F32 us[4] = { X0, X1, X2, X3 };
            const F32 vs[4] = { Y0, Y1, Y2, Y3 };
            for (S32 j = 0; j < 3; ++j)
            {
                for (S32 i = 0; i < 3; ++i)
                {
                    quad(us[i], vs[j], us[i + 1], vs[j + 1], xs[i], ys[j + 1], xs[i + 1], ys[j]);
                }
            }
        }
        gGL.end();
        return true;
    }

    bool key(F32 l, F32 b, F32 r, F32 t, ECap state, F32 k)
    {
        const EPart p = state == CAP_LIT ? WolfHudKitMap::P_KEY_LIT : state == CAP_DOWN ? WolfHudKitMap::P_KEY_DOWN : WolfHudKitMap::P_KEY;
        return part(p, l, b, r, t, LLColor4::white, k);
    }

    bool glow(F32 cx, F32 cy, F32 rad, const LLColor4& c)
    {
        // glow_round (parts.py) is a lit disc of radius 0.22 of the sprite's width, blurred 12 px (pack.py): it has faded out
        // inside the sprite's edge, so the sprite drawn 2 x rad square fades out within rad
        return part(WolfHudKitMap::P_GLOW_ROUND, cx - rad, cy - rad, cx + rad, cy + rad, c);
    }

    bool gaugeFace(F32 cx, F32 cy, F32 r, bool enamel)
    {
        return part(enamel ? WolfHudKitMap::P_FACE_ENAMEL : WolfHudKitMap::P_FACE_BLACK, cx - r, cy - r, cx + r, cy + r);
    }

    void markBadge(F32 cx, F32 cy, F32 r, EMark mark, const LLColor4& c)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // a disc: the light rim, then the dark face inside it
        auto disc = [](F32 x, F32 y, F32 rad, const LLColor4& col)
        {
            gGL.color4fv(col.mV);
            gGL.begin(LLRender::TRIANGLES);
            for (S32 i = 0; i < 20; ++i)
            {
                const F32 a0 = F_TWO_PI * (F32)i / 20.f, a1 = F_TWO_PI * (F32)(i + 1) / 20.f;
                gGL.vertex2f(x, y);
                gGL.vertex2f(x + rad * cosf(a0), y + rad * sinf(a0));
                gGL.vertex2f(x + rad * cosf(a1), y + rad * sinf(a1));
            }
            gGL.end();
        };
        disc(cx, cy, r, LLColor4(0.95f, 0.95f, 0.95f, 0.9f));
        disc(cx, cy, r * 0.86f, LLColor4(0.08f, 0.07f, 0.06f, 0.92f));
        // a stroke of width w from (x0, y0) to (x1, y1), as two triangles
        const F32 w = llmax(1.5f, r * 0.24f);
        auto seg = [w](F32 x0, F32 y0, F32 x1, F32 y1)
        {
            const F32 dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
            if (len <= 0.f) return;
            const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
            gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x0 - nx, y0 - ny); gGL.vertex2f(x1 - nx, y1 - ny);
            gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x1 - nx, y1 - ny); gGL.vertex2f(x1 + nx, y1 + ny);
        };
        const F32 k = r * 0.5f;
        gGL.color4fv(c.mV);
        gGL.begin(LLRender::TRIANGLES);
        if (mark == MARK_TICK)
        {
            seg(cx - k, cy, cx - k * 0.25f, cy - k * 0.75f);
            seg(cx - k * 0.25f, cy - k * 0.75f, cx + k, cy + k * 0.8f);
        }
        else
        {
            seg(cx - k * 0.8f, cy - k * 0.8f, cx + k * 0.8f, cy + k * 0.8f);
            seg(cx - k * 0.8f, cy + k * 0.8f, cx + k * 0.8f, cy - k * 0.8f);
        }
        gGL.end();
    }

    bool gaugeBezel(F32 cx, F32 cy, F32 r, bool brass)
    {
        // the bezel's inner edge (parts.py: outer W/2 - 2, inner W/2 - 22 of a 256 sprite) sits on the face's edge: drawn so
        // its inner radius is r, its ring reaches r * 126 / 106
        const F32 R = r * 128.f / 106.f;
        if (!part(WolfHudKitMap::P_GLARE_ROUND, cx - r, cy - r, cx + r, cy + r)) return false;
        return part(brass ? WolfHudKitMap::P_BEZEL_BRASS : WolfHudKitMap::P_BEZEL_CHROME, cx - R, cy - R, cx + R, cy + R);
    }
}
