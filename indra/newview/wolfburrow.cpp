/**
 * @file wolfburrow.cpp
 * @brief WolfViewer: "the burrow" - the painted look of the Tunnel tool, the Cave Builder and the Build window's Wolf strip.
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

// [WOLF BURROW 2026-10-23] See wolfburrow.h.

#include "llviewerprecompiledheaders.h"

#include "wolfburrow.h"

#include <cmath>

#include "llbutton.h"
#include "llfloater.h"
#include "lluicolortable.h"
#include "wolfpaint.h"

using namespace WolfPaint;

namespace WolfBurrow
{
    // Text colours and the plates they sit on (burrow_contrast.py: CREAM on PLATE_TOP 13.4:1, MUTED 9.1:1, DIS 6.8:1, AMBER
    // 9.6:1, ERR_INK 9.1:1; CREAM on the sheet 15.2:1; on a stone button 8.1:1 (top) to 11.7:1; DIS on a disabled button
    // 6.7:1; INK on an amber button 13.2:1 (top) to 6.7:1; AMBER on the header 8.3:1, MUTED 7.8:1).
    const LLColor4 CREAM(0.95f, 0.92f, 0.84f, 1.f);
    const LLColor4 MUTED(0.82f, 0.76f, 0.66f, 1.f);
    const LLColor4 DIS(0.70f, 0.66f, 0.60f, 1.f);
    const LLColor4 AMBER(1.f, 0.74f, 0.32f, 1.f);
    const LLColor4 AMBER_DEEP(0.86f, 0.54f, 0.16f, 1.f);
    const LLColor4 INK(0.12f, 0.07f, 0.03f, 1.f);
    const LLColor4 ERR_INK(1.f, 0.70f, 0.52f, 1.f);
    const LLColor4 SHEET_TOP(0.11f, 0.08f, 0.055f, 1.f);
    const LLColor4 SHEET_BOTTOM(0.07f, 0.05f, 0.035f, 1.f);
    const LLColor4 PLATE_TOP(0.18f, 0.12f, 0.08f, 1.f);
    const LLColor4 PLATE_BOTTOM(0.09f, 0.06f, 0.04f, 1.f);
    const LLColor4 EDGE(0.55f, 0.40f, 0.24f, 1.f);
}

namespace
{
    using namespace WolfBurrow;
    const LLColor4 HEADER_TOP(0.24f, 0.16f, 0.10f, 1.f);
    const LLColor4 HEADER_BOTTOM(0.13f, 0.09f, 0.06f, 1.f);
    const LLColor4 STRATA(0.16f, 0.11f, 0.07f, 1.f);
    const LLColor4 STONE_TOP(0.34f, 0.25f, 0.17f, 1.f);
    const LLColor4 STONE_BOTTOM(0.22f, 0.16f, 0.10f, 1.f);
    const LLColor4 STONE_OFF(0.17f, 0.13f, 0.10f, 1.f);
    const LLColor4 SEAL_TOP(1.f, 0.84f, 0.46f, 1.f);
    const LLColor4 SEAL_BOTTOM(0.86f, 0.54f, 0.16f, 1.f);
    const LLColor4 DANGER(0.85f, 0.30f, 0.25f, 1.f);
    const LLColor4 SHADOW(0.f, 0.f, 0.f, 0.45f);
    // the burrow scene
    const LLColor4 SOIL_TOP(0.40f, 0.26f, 0.14f, 1.f);
    const LLColor4 SOIL_BOTTOM(0.22f, 0.14f, 0.08f, 1.f);
    const LLColor4 GRASS(0.30f, 0.55f, 0.22f, 1.f);
    const LLColor4 HOLE(0.06f, 0.035f, 0.02f, 1.f);
    const LLColor4 PEBBLE(0.52f, 0.42f, 0.32f, 1.f);
    const LLColor4 FUR(0.20f, 0.18f, 0.21f, 1.f);
    const LLColor4 FUR_LIGHT(0.32f, 0.29f, 0.33f, 1.f);
    const LLColor4 PINK(0.95f, 0.62f, 0.66f, 1.f);
    const LLColor4 EYE(0.02f, 0.02f, 0.02f, 1.f);
    const LLColor4 FLAME(1.f, 0.85f, 0.35f, 1.f);
}

void WolfBurrow::drawMoleScene(const LLRect& pit, F32 t, bool lit)
{
    untextured();
    const F32 l = (F32)pit.mLeft, b = (F32)pit.mBottom, w = (F32)pit.getWidth(), h = (F32)pit.getHeight();
    fillRounded(pit, allCorners(4.f), SOIL_TOP, SOIL_BOTTOM);
    box(l + 2.f, b + h - 2.f, l + w - 2.f, b + h - llmax(3.f, h * 0.06f), GRASS, lighten(GRASS, -0.08f));
    for (S32 i = 0; i < 9; ++i)   // pebbles, the same every frame
    {
        const F32 px = l + 6.f + hash01((F32)i * 3.1f) * (w - 12.f), py = b + 5.f + hash01((F32)i * 7.7f) * (h - 16.f);
        disc(px, py, llmax(0.8f, h * 0.015f) + hash01((F32)i * 1.3f) * llmax(1.f, h * 0.02f), PEBBLE, lighten(PEBBLE, -0.12f));
    }
    // the tunnel the mole is digging, ahead of it
    const F32 cy = b + h * 0.42f;
    ellipse(l + w * 0.76f, cy, w * 0.22f, h * 0.24f, HOLE, lighten(HOLE, 0.05f));
    box(l + w * 0.12f, cy + h * 0.16f, l + w * 0.76f, cy - h * 0.20f, HOLE, HOLE);
    // the mole, bobbing as it digs
    const F32 bob = t > 0.f ? sinf(t * 8.f) * llmax(0.6f, h * 0.015f) : 0.f;
    const F32 mx = l + w * 0.40f, my = cy - h * 0.02f + bob;
    const F32 rx = w * 0.22f, ry = h * 0.15f;
    ellipse(mx, my, rx, ry, FUR_LIGHT, FUR);
    ellipse(mx + rx * 0.85f, my + ry * 0.15f, rx * 0.45f, ry * 0.70f, FUR_LIGHT, FUR);              // the head
    disc(mx + rx * 1.32f, my + ry * 0.10f, llmax(1.2f, ry * 0.22f), PINK, lighten(PINK, -0.1f));  // the snout
    disc(mx + rx * 0.98f, my + ry * 0.42f, llmax(0.7f, ry * 0.08f), EYE, EYE);                      // the eye
    for (S32 k = 0; k < 2; ++k)   // the front paws, scooping in turn
    {
        const F32 a = sinf(t * 10.f + (F32)k * F_PI) * 0.6f;
        const F32 sx = mx + rx * 0.80f, sy = my - ry * 0.55f;
        limb(sx, sy, sx + cosf(a) * rx * 0.55f, sy - 2.f + sinf(a) * ry * 0.6f, llmax(1.5f, ry * 0.30f), PINK);
    }
    ellipse(mx - rx * 0.95f, my - ry * 0.10f, rx * 0.18f, ry * 0.12f, PINK, PINK);                  // the tail
    if (t > 0.f)
    {
        for (S32 i = 0; i < 6; ++i)   // clods thrown back over its shoulders
        {
            const F32 p = fmodf(t * 1.3f + (F32)i / 6.f, 1.f);
            const F32 cx = mx + rx * 0.3f - p * w * 0.45f;
            const F32 cyy = my + ry * 0.6f + sinf(p * F_PI) * h * 0.28f;
            const F32 rr = llmax(0.8f, h * 0.02f) * (1.f + (1.f - p));
            disc(cx, cyy, rr, withAlpha(SOIL_TOP, 1.f - p * 0.6f), withAlpha(SOIL_BOTTOM, 1.f - p * 0.6f));
        }
    }
    if (lit)
    {
        // a miner's flame on its head, flickering
        const F32 fx = mx + rx * 0.80f, fy = my + ry * 0.95f;
        const F32 fl = 1.f + 0.25f * sinf(t * 23.f) + 0.15f * sinf(t * 37.f);
        ellipse(fx, fy + 3.f * fl, 2.6f, 4.5f * fl, FLAME, withAlpha(AMBER_DEEP, 0.f));
        disc(fx, fy + 2.f, 1.4f, LLColor4::white, FLAME);
    }
}

void WolfBurrow::drawSheet(const LLRect& client)
{
    untextured();
    box((F32)client.mLeft, (F32)client.mTop, (F32)client.mRight, (F32)client.mBottom, SHEET_TOP, SHEET_BOTTOM);
    // faint strata, the same every frame
    for (S32 i = 0; i < 14; ++i)
    {
        const F32 y = (F32)client.mBottom + hash01((F32)i * 5.3f) * (F32)client.getHeight();
        const F32 x0 = (F32)client.mLeft + hash01((F32)i * 2.9f) * (F32)client.getWidth() * 0.4f;
        const F32 x1 = x0 + (F32)client.getWidth() * (0.3f + 0.4f * hash01((F32)i * 8.1f));
        line(x0, y, llmin(x1, (F32)client.mRight), y + (hash01((F32)i * 4.4f) - 0.5f) * 6.f, 1.f, withAlpha(STRATA, 0.8f));
    }
}

void WolfBurrow::drawHeader(const LLRect& band, const std::string& title, const std::string& line_text, F32 t)
{
    untextured();
    LLRect shadow = band;
    shadow.translate(1, -2);
    fillRounded(shadow, allCorners(5.f), SHADOW, SHADOW);
    fillRounded(band, allCorners(5.f), HEADER_TOP, HEADER_BOTTOM);
    strokeRounded(band, allCorners(5.f), 0.f, 1.f, EDGE, withAlpha(EDGE, 0.6f));
    const S32 ph = band.getHeight() - 8;
    const LLRect pit(band.mLeft + 4, band.mTop - 4, band.mLeft + 4 + (S32)(ph * 1.35f), band.mBottom + 4);
    drawMoleScene(pit, t, false);
    const F32 tx = (F32)pit.mRight + 10.f, tw = (F32)band.mRight - 10.f - tx;
    const F32 mid = (F32)band.getCenterY();
    const LLFontGL* tf = fitDisplayFont((F32)band.getHeight() * 0.55f, tw, { title });
    text(tf, title, tx, mid + (F32)band.getHeight() * 0.16f, AMBER, LLFontGL::LEFT, tw);
    text(LLFontGL::getFontSansSerifSmall(), line_text, tx, mid - (F32)band.getHeight() * 0.24f, MUTED, LLFontGL::LEFT, tw);
}

void WolfBurrow::drawSection(const LLRect& plate, const std::string& heading)
{
    if (plate.isEmpty()) return;
    untextured();
    LLRect shadow = plate;
    shadow.translate(1, -2);
    fillRounded(shadow, allCorners(5.f), SHADOW, SHADOW);
    fillRounded(plate, allCorners(5.f), PLATE_TOP, PLATE_BOTTOM);
    strokeRounded(plate, allCorners(5.f), 0.f, 1.f, withAlpha(EDGE, 0.85f), withAlpha(EDGE, 0.45f));
    if (heading.empty()) return;
    const LLFontGL* hf = LLFontGL::getFontSansSerifBold();
    const F32 hy = (F32)plate.mTop - 10.f;
    // a small amber notch before the heading, then the heading in open lettering
    untextured();
    box((F32)plate.mLeft + 8.f, hy + 4.f, (F32)plate.mLeft + 11.f, hy - 4.f, AMBER, AMBER_DEEP);
    spacedText(hf, heading, (F32)plate.mLeft + 17.f, hy, AMBER, 1.2f);
}

void WolfBurrow::drawButtonFace(LLButton* b, const LLView* owner, EWeight weight)
{
    if (!b || !b->isInVisibleChain()) return;
    // Source: wolffloatermeshupload.cpp drawButtons (face): shadow, then the face by state, then its rule
    untextured();
    const LLRect r = rectIn(b, owner);
    const bool enabled = b->isInEnabledChain();
    const bool pressed = b->hasMouseCapture() && mouseOver(b);
    const bool hover = enabled && !pressed && mouseOver(b);
    const bool on = b->getToggleState();
    LLRect shadow = r;
    shadow.translate(0, -2);
    fillRounded(shadow, allCorners(4.f), SHADOW, SHADOW);
    if (!enabled)
    {
        fillRounded(r, allCorners(4.f), STONE_OFF, STONE_OFF);
        strokeRounded(r, allCorners(4.f), 0.f, 1.f, withAlpha(EDGE, 0.45f), withAlpha(EDGE, 0.45f));
        return;
    }
    if (weight == WEIGHT_PRIMARY)
    {
        halo(r, 4.f, 6, withAlpha(AMBER, 0.55f));
        LLColor4 top = SEAL_TOP, bottom = SEAL_BOTTOM;
        if (hover) bottom = lighten(bottom, 0.06f);
        if (pressed) { top = lighten(top, -0.06f); bottom = lighten(bottom, -0.06f); }
        fillRounded(r, allCorners(4.f), top, bottom);
        strokeRounded(r, allCorners(4.f), 0.f, 1.f, lighten(SEAL_TOP, 0.1f), AMBER_DEEP);
        return;
    }
    LLColor4 top = STONE_TOP, bottom = STONE_BOTTOM;
    if (on) { top = lighten(top, 0.06f); bottom = lighten(bottom, 0.04f); }
    if (hover) { top = lighten(top, 0.04f); bottom = lighten(bottom, 0.03f); }
    if (pressed) { top = lighten(top, -0.04f); bottom = lighten(bottom, -0.04f); }
    fillRounded(r, allCorners(4.f), top, bottom);
    const LLColor4 rule = on ? AMBER : weight == WEIGHT_DANGER ? DANGER : hover ? AMBER : EDGE;
    strokeRounded(r, allCorners(4.f), 0.f, on ? 2.f : 1.f, rule, rule);
}

void WolfBurrow::drawBuildPlate(const LLRect& plate)
{
    if (plate.isEmpty()) return;
    untextured();
    static LLUIColor fill = LLUIColorTable::instance().getColor("WolfBuildPlate", LLColor4(0.2f, 0.2f, 0.2f, 0.6f));
    static LLUIColor rule = LLUIColorTable::instance().getColor("WolfBuildPlateEdge", LLColor4(0.5f, 0.5f, 0.5f, 0.55f));
    fillRounded(plate, allCorners(4.f), fill.get(), fill.get());
    strokeRounded(plate, allCorners(4.f), 0.f, 1.f, rule.get(), withAlpha(rule.get(), 0.6f));
}

void WolfBurrow::drawBuildHeader(const LLRect& band)
{
    if (band.isEmpty()) return;
    untextured();
    static LLUIColor top = LLUIColorTable::instance().getColor("WolfBuildHeaderTop", LLColor4(0.3f, 0.3f, 0.3f, 0.8f));
    static LLUIColor bottom = LLUIColorTable::instance().getColor("WolfBuildHeaderBottom", LLColor4(0.15f, 0.15f, 0.15f, 0.8f));
    static LLUIColor accent = LLUIColorTable::instance().getColor("WolfBuildAccent", LLColor4(1.f, 0.74f, 0.32f, 1.f));
    static LLUIColor rule = LLUIColorTable::instance().getColor("WolfBuildPlateEdge", LLColor4(0.5f, 0.5f, 0.5f, 0.55f));
    fillRounded(band, allCorners(5.f), top.get(), bottom.get());
    strokeRounded(band, allCorners(5.f), 0.f, 1.f, rule.get(), rule.get());
    box((F32)band.mLeft + 6.f, (F32)band.mBottom + 3.f, (F32)band.mRight - 6.f, (F32)band.mBottom + 1.f, accent.get(), withAlpha(accent.get(), 0.4f));
}

LLRect WolfBurrow::around(const std::vector<LLView*>& views, const LLView* owner, S32 pad, S32 heading_room)
{
    LLRect out;
    bool any = false;
    for (LLView* v : views)
    {
        if (!v || !v->isInVisibleChain()) continue;
        const LLRect r = rectIn(v, owner);
        if (!any) { out = r; any = true; }
        else out.unionWith(r);
    }
    if (!any) return LLRect();
    return LLRect(out.mLeft - pad, out.mTop + pad + heading_room, out.mRight + pad, out.mBottom - pad);
}

WolfBurrow::Sheet* WolfBurrow::Sheet::add(LLFloater* owner, const std::function<void()>& painter)
{
    // Source: wolffloatermeshupload.cpp postBuild (llfloaterwolfavatars.cpp:1014-1024): behind every control; the caller then
    // sends its drag handle behind it, so empty paper drags the window.
    Params p;
    p.name("wolf_burrow_sheet");
    p.rect(owner->getLocalRect());
    p.follows.flags(FOLLOWS_ALL);
    p.mouse_opaque(false);
    Sheet* s = new Sheet(p);
    s->mPainter = painter;
    owner->addChild(s);
    owner->sendChildToBack(s);
    // (the window then sends its drag handle behind it: LLFloater::getDragHandle is the window's own, see wolfburrow.h)
    return s;
}

void WolfBurrow::Sheet::draw()
{
    if (mPainter) mPainter();
}
