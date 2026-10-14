/**
 * @file wolfhudscale.cpp
 * @brief WolfViewer: the HUDs drawn at a size the driver sets. See wolfhudscale.h.
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

#include <cmath>
#include <map>
#include <vector>

#include "llfloaterreg.h"
#include "llfontregistry.h"
#include "llnotificationsutil.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "rlvactions.h"
#include "v4color.h"

namespace
{
    F32 sCurrent = 1.f;

    // The handle: this far below and above the top edge, in the view's units. Over the top edge so it is
    // found by eye, mostly above it so the buttons along the edge keep their clicks.
    const F32 GRIP_BELOW = 3.f;
    const F32 GRIP_ABOVE = 7.f;
    // The tab on the middle of the edge, always shown, so the handle is seen without hunting for it: this
    // wide either side of the middle and this tall above the edge (it takes clicks over all of it).
    const F32 TAB_HALF_W = 46.f;
    const F32 TAB_H = 14.f;

    // Source: wolfdashboard.cpp face() / wolfflightdeck.cpp avionics() - the B612 faces in their twelve sizes,
    // fonts.xml Avionics0..11 (6 to 26 pt, smallest first). Each face's own list, found from any of its sizes.
    const std::vector<const LLFontGL*>* familyOf(const LLFontGL* f)
    {
        static std::map<const LLFontGL*, std::vector<const LLFontGL*>> families;   // keyed by the smallest size
        static std::map<const LLFontGL*, const std::vector<const LLFontGL*>*> of;
        static bool built = false;
        if (!built)
        {
            built = true;
            const char* const names[2] = { "B612", "B612Mono" };
            const U8 styles[2] = { LLFontGL::NORMAL, LLFontGL::BOLD };
            for (const char* name : names)
            {
                for (U8 style : styles)
                {
                    std::vector<const LLFontGL*> v;
                    for (S32 i = 0; i < 12; ++i)
                    {
                        if (const LLFontGL* sz = LLFontGL::getFont(LLFontDescriptor(name, llformat("Avionics%d", i), style)))
                        {
                            v.push_back(sz);
                        }
                    }
                    if (v.empty())
                    {
                        continue;   // the face is missing: its text is drawn as the picture scales it
                    }
                    const std::vector<const LLFontGL*>& kept = families.emplace(v.front(), v).first->second;
                    for (const LLFontGL* sz : kept)
                    {
                        of.emplace(sz, &kept);
                    }
                }
            }
        }
        auto it = of.find(f);
        return it == of.end() ? nullptr : it->second;
    }

    void rectF(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(c.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(l, b); gGL.vertex2f(r, b); gGL.vertex2f(r, t);
        gGL.vertex2f(l, b); gGL.vertex2f(r, t); gGL.vertex2f(l, t);
        gGL.end();
    }
}

namespace WolfHudScale
{
    Scope::Scope(F32 scale)
    :   mPrev(sCurrent),
        mPushed(fabsf(scale - 1.f) > 0.0005f)
    {
        if (mPushed)
        {
            // Source: LLRender::transform() - a UI vertex is drawn at (v + UI offset) * UI scale, and LLFontGL::render()
            // puts text at x * sScaleX + floor(sCurOrigin * sScaleX), the same point. Both then go through the
            // modelview matrix, so one scale about the view's corner (its UI offset, in those pixels) moves both.
            const LLVector3 off = gGL.getUITranslation();
            const LLVector3 ui = gGL.getUIScale();
            const F32 ox = off.mV[VX] * ui.mV[VX], oy = off.mV[VY] * ui.mV[VY];
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            gGL.translatef(ox, oy, 0.f);
            gGL.scalef(scale, scale, 1.f);
            gGL.translatef(-ox, -oy, 0.f);
        }
        sCurrent = mPrev * scale;
    }

    Scope::~Scope()
    {
        if (mPushed)
        {
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.popMatrix();
        }
        sCurrent = mPrev;
    }

    F32 current()
    {
        return sCurrent;
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h, LLFontGL::VAlign v)
    {
        if (!f || s.empty())
        {
            return;
        }
        const F32 sc = sCurrent;
        const LLFontGL* use = f;
        if (fabsf(sc - 1.f) > 0.0005f)
        {
            if (const std::vector<const LLFontGL*>* fam = familyOf(f))
            {
                // the largest size no more than a tenth bigger than the text really comes out (else the smallest)
                const F32 want = (F32)f->getLineHeight() * sc * 1.1f;
                use = fam->front();
                for (const LLFontGL* sz : *fam)
                {
                    if ((F32)sz->getLineHeight() <= want)
                    {
                        use = sz;
                    }
                }
            }
        }
        if (use == f)
        {
            f->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            return;
        }
        // `use` drawn at f's height in the picture: scaled by f / use about the point the text hangs from
        // (Source: LLFontGL::render() - cur_x = x * sScaleX + floor(sCurOrigin.mX * sScaleX), and y alike).
        const F32 k = (F32)f->getLineHeight() / (F32)llmax(1, use->getLineHeight());
        const F32 px = x * LLFontGL::sScaleX + floorf((F32)LLFontGL::sCurOrigin.mX * LLFontGL::sScaleX);
        const F32 py = y * LLFontGL::sScaleY + floorf((F32)LLFontGL::sCurOrigin.mY * LLFontGL::sScaleY);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.translatef(px, py, 0.f);
        gGL.scalef(k, k, 1.f);
        gGL.translatef(-px, -py, 0.f);
        use->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        gGL.popMatrix();
    }

    LLRect clip(const LLRect& r)
    {
        const F32 sc = sCurrent;
        return LLRect((S32)floorf(r.mLeft * sc), (S32)ceilf(r.mTop * sc), (S32)ceilf(r.mRight * sc), (S32)floorf(r.mBottom * sc));
    }
}

//-----------------------------------------------------------------------------

namespace WolfHud
{
    void toggleInventory()
    {
        if (RlvActions::isRlvEnabled() && RlvActions::hasBehaviour(RLV_BHVR_SHOWINV))
        {
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                std::string("Your inventory can't be opened now: an RLV restriction (showinv) is on.")));
            return;
        }
        LLFloaterReg::toggleInstance("inventory");
    }

    bool inventoryOpen()
    {
        return LLFloaterReg::instanceVisible("inventory");
    }

    const std::string& inventoryTip()
    {
        static const std::string TIP("Your inventory (the toolbar's Inventory): click to open or close it");
        return TIP;
    }
}

//-----------------------------------------------------------------------------

WolfHudResize::WolfHudResize(const char* setting)
:   mSetting(setting),
    mScale(gSavedSettings, setting)
{
}

F32 WolfHudResize::scale() const
{
    const F32 s = mScale;
    // a hand-edited NaN would pass llclamp: the default size
    return std::isfinite(s) ? llclamp(s, WolfHudScale::MIN_SCALE, WolfHudScale::MAX_SCALE) : WolfHudScale::DEFAULT_SCALE;
}

void WolfHudResize::place(F32 bottom, F32 top, F32 width)
{
    mShown = true;
    mBottom = bottom;
    mTop = top;
    mWidth = width;
    mPlacedScale = scale();   // the size this frame was laid out at (the HUD read scale() for it)
}

void WolfHudResize::hide()
{
    mShown = false;
    mDragging = false;
}

bool WolfHudResize::onGrip(S32 x, S32 y) const
{
    if (!mShown || (F32)y < mTop - GRIP_BELOW)
    {
        return false;
    }
    if ((F32)y <= mTop + GRIP_ABOVE)
    {
        return true;    // the band along the whole edge
    }
    return (F32)y <= mTop + TAB_H && fabsf((F32)x - mWidth * 0.5f) <= TAB_HALF_W;   // the tab
}

void WolfHudResize::hovered()
{
    mHoverFrame = LLFrameTimer::getFrameCount();
    mHoverSeen = true;
}

void WolfHudResize::begin(S32 y)
{
    mDragging = true;
    mGrabOffset = mTop - (F32)y;
}

void WolfHudResize::drag(S32 y)
{
    if (!mDragging)
    {
        return;
    }
    // The top edge follows the mouse. The new size comes from the height the HUD was really drawn at this
    // frame (place()) at the size it was drawn at, not from the height when the drag began: the dashboard's
    // height is not always in proportion to its size (a narrow window lays it out by width, wolfdashboard.cpp
    // draw() `one`), so each frame corrects what the last one missed and the edge settles under the mouse.
    // Several moves within one frame all start from that same drawn frame, so they never compound.
    const F32 h = llmax(1.f, mTop - mBottom);
    const F32 want = llmax(1.f, (F32)y + mGrabOffset - mBottom);
    const F32 s = llclamp(mPlacedScale * want / h, WolfHudScale::MIN_SCALE, WolfHudScale::MAX_SCALE);
    if (fabsf(s - scale()) > 0.001f)
    {
        gSavedSettings.setF32(mSetting, s);
    }
}

void WolfHudResize::end()
{
    mDragging = false;
}

void WolfHudResize::reset()
{
    mDragging = false;
    if (LLControlVariablePtr control = gSavedSettings.getControl(mSetting))
    {
        control->resetToDefault(true);
    }
}

void WolfHudResize::draw() const
{
    if (!mShown)
    {
        return;
    }
    const bool hot = mDragging || (mHoverSeen && LLFrameTimer::getFrameCount() - mHoverFrame <= 1);
    const F32 a = hot ? 1.f : 0.8f;
    const LLColor4 light(0.85f, 0.87f, 0.89f, a);
    // lit: a line along the whole edge, the HUD's size handle end to end
    if (hot)
    {
        rectF(0.f, mTop - 1.f, mWidth, mTop + 1.f, LLColor4(1.f, 1.f, 1.f, mDragging ? 0.6f : 0.45f));
    }
    // the tab: a dark plate with a light rim, three ridges to take hold of, and an up and a down arrow at
    // each end (the HUD gets bigger or smaller)
    const F32 cx = mWidth * 0.5f, l = cx - TAB_HALF_W, r = cx + TAB_HALF_W, b = mTop, t = mTop + TAB_H;
    rectF(l, b, r, t, LLColor4(0.06f, 0.07f, 0.08f, hot ? 0.92f : 0.78f));
    rectF(l, t - 1.f, r, t, light);
    rectF(l, b, l + 1.f, t, light);
    rectF(r - 1.f, b, r, t, light);
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 y = b + 3.5f + 3.f * i;
        rectF(cx - 20.f, y, cx + 20.f, y + 1.f, light);
    }
    for (const F32 ax : { l + 12.f, r - 12.f })
    {
        const F32 m = b + TAB_H * 0.5f;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(light.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(ax - 4.f, m + 1.f); gGL.vertex2f(ax + 4.f, m + 1.f); gGL.vertex2f(ax, m + 5.f);   // up
        gGL.vertex2f(ax - 4.f, m - 1.f); gGL.vertex2f(ax, m - 5.f); gGL.vertex2f(ax + 4.f, m - 1.f);   // down
        gGL.end();
    }
}

// static
const std::string& WolfHudResize::tip()
{
    static const std::string TIP("Drag this edge down to make it smaller, up to make it bigger. Double-click for the default size.");
    return TIP;
}
