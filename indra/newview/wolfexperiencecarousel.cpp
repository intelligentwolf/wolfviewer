/**
 * @file wolfexperiencecarousel.cpp
 * @brief WolfViewer: Wolf Experiences' 3D carousel. See wolfexperiencecarousel.h.
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

#include "wolfexperiencecarousel.h"

#include <algorithm>
#include <cmath>

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llimage.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "lltooltip.h"
#include "llwindow.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "wolfgamehud.h"        // WolfGameDraw: the flight deck family's shapes, buttons and lettering
#include "wolfspacesky.h"

static LLDefaultChildRegistry::Register<WolfExperienceCarousel> r_wolf_experience_carousel("wolf_experience_carousel");

using namespace WolfGameDraw;

namespace
{
    // ---- the ring (Source: floater_wolf_experiences.js ExperienceRing, 2026-11-21 - the same numbers) ----
    const F32 MIN_SLOTS = 8.f;          // N cards share the ring as if it held at least 8 and at most 10
    const F32 MAX_SLOTS = 10.f;
    const F32 VISIBLE_SLOTS = 3.6f;     // cards further than this from the front are not drawn
    const F32 EASE_S = 0.11f;           // the easing's time constant
    const S32 DRAG_SLOP_PX = 6;         // a press that moves less than this is a click
    const F32 CARD_ASPECT = 1.05f;      // card height per width: the 4:3 picture plus its words
    const F32 CHORD = 0.62f;            // neighbouring cards' gap: radius = CHORD * cw / tan(step / 2)
    const F32 CAMERA_RADII = 2.2f;      // the camera this many radii back (CSS perspective = 2.2 * radius)
    const F32 TURN = 0.8f;              // each card turned 0.8 of its angle round the ring
    const F32 LIFT = 0.10f;             // far cards rise by (1 - cos) * 0.10 of a card's height
    const S32 STRIPS = 8;               // a picture drawn in this many strips, so the perspective does not shear it

    // ---- the window's layout (view-local, y up; the floater's own widgets sit at these places too:
    //      floater_wolf_experiences.xml) ----
    const F32 MARGIN = 16.f;
    const F32 PILL_TOP = 58.f;          // from the window's top: under the Experiences / What's on row
    const F32 PILL_H = 48.f;
    const F32 FOOT_H = 48.f;            // the footer (Don't show / Add your experience / Close) and its gap
    const F32 CAP_H = 78.f;
    const F32 GAP = 8.f;
    const F32 SPIN_R = 28.f;            // the big round arrows: 56 across
    const F32 SKY_MARGIN = 48.f;        // painted this far past each side, for the drift

    // ---- colours: the wolves palette (wolfgamehud.cpp wolvesPalette) and the WolfStorm window's (2026-11-21) ----
    // #f5a524, the same value as wolfgamehud.cpp:353 WOLF_AMBER - written out, not copied from it: a namespace-scope copy
    // of another file's constant is made before that constant exists when this file is initialised first (it came out
    // transparent in the first rig run).
    const LLColor4 AMBER(245.f / 255.f, 165.f / 255.f, 36.f / 255.f, 1.f);
    const LLColor4 INK_ON_AMBER(17.f / 255.f, 17.f / 255.f, 17.f / 255.f, 1.f);   // #111, 9.6:1 on amber
    const LLColor4 TEXT(1.f, 1.f, 1.f, 1.f);
    const LLColor4 TEXT_DIM(201.f / 255.f, 211.f / 255.f, 230.f / 255.f, 1.f);    // #c9d3e6, 12.6:1 on the sky
    const LLColor4 PLATE(10.f / 255.f, 16.f / 255.f, 34.f / 255.f, 0.80f);        // rgba(10,16,34,.8)
    const LLColor4 EDGE(201.f / 255.f, 211.f / 255.f, 230.f / 255.f, 0.35f);
    const LLColor4 BAND_TOP(15.f / 255.f, 24.f / 255.f, 48.f / 255.f, 1.f);       // #0f1830
    const LLColor4 BAND_BOT(10.f / 255.f, 16.f / 255.f, 34.f / 255.f, 1.f);       // #0a1022
    const LLColor4 SPACE(0.010f, 0.015f, 0.040f, 1.f);                           // wolfspacesky.cpp SPACE_RGB
    const LLColor4 FLOOR_WARM(245.f / 255.f, 165.f / 255.f, 36.f / 255.f, 0.42f);
    const LLColor4 FLOOR_COOL(120.f / 255.f, 150.f / 255.f, 1.f, 0.20f);

    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    const LLFontGL* fontBig()   { return LLFontGL::getFont(LLFontDescriptor("SansSerif", "Large", LLFontGL::BOLD)); }
    const LLFontGL* fontHuge()  { return LLFontGL::getFont(LLFontDescriptor("SansSerif", "Huge", LLFontGL::BOLD)); }
    const LLFontGL* fontBody()  { return LLFontGL::getFont(LLFontDescriptor("SansSerif", "Large", LLFontGL::NORMAL)); }
    const LLFontGL* fontSmall() { return LLFontGL::getFontSansSerif(); }

    // Lines of `text` that fit `width` (words kept whole where they can be), at most `max_lines`, the last cut with
    // "..." when there is more. Source: llfontgl.h maxDrawableChars(..., WORD_BOUNDARY_IF_POSSIBLE).
    std::vector<std::string> wrapLines(const LLFontGL* f, const std::string& text, F32 width, S32 max_lines)
    {
        std::vector<std::string> out;
        LLWString w = utf8str_to_wstring(text);
        size_t start = 0;
        while (start < w.size() && (S32)out.size() < max_lines)
        {
            while (start < w.size() && w[start] == ' ') ++start;
            if (start >= w.size()) break;
            const llwchar* p = w.c_str() + start;
            S32 n = f->maxDrawableChars(p, width, (S32)(w.size() - start), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
            if (n <= 0) n = 1;
            std::string line = wstring_to_utf8str(w.substr(start, n));
            start += n;
            if ((S32)out.size() == max_lines - 1 && start < w.size())
            {
                // the last line: cut to fit with "..."
                std::string rest = wstring_to_utf8str(w.substr(start - n));
                LLStringUtil::trim(rest);
                const LLWString rw = utf8str_to_wstring(rest);
                const S32 m = f->maxDrawableChars(rw.c_str(), width - f->getWidthF32("..."), (S32)rw.size(), LLFontGL::ANYWHERE);
                line = wstring_to_utf8str(rw.substr(0, llmax(0, m))) + "...";
                start = w.size();
            }
            LLStringUtil::trim(line);
            out.push_back(line);
        }
        return out;
    }

    void noTexture() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }

    // A filled quad (corners in order round it) with a colour per corner.
    void quad(const LLVector2 c[4], const LLColor4 col[4])
    {
        noTexture();
        gGL.begin(LLRender::TRIANGLES);
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i : order)
        {
            gGL.color4fv(col[i].mV);
            gGL.vertex2f(c[i].mV[VX], c[i].mV[VY]);
        }
        gGL.end();
    }

    // A pill: a rectangle with round ends (WolfStorm's .wx-cat, border-radius half its height).
    void pill(F32 l, F32 b, F32 r, F32 t, const LLColor4& fill, const LLColor4& edge, F32 edge_w)
    {
        const F32 rad = (t - b) * 0.5f, cy = (b + t) * 0.5f;
        rect(l + rad, b, r - rad, t, fill);
        // the round ends: half discs as fans
        noTexture();
        gGL.begin(LLRender::TRIANGLES);
        const S32 seg = 16;
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = F_PI * 0.5f + F_PI * i / seg, a1 = F_PI * 0.5f + F_PI * (i + 1) / seg;
            gGL.color4fv(fill.mV);
            gGL.vertex2f(l + rad, cy);
            gGL.vertex2f(l + rad + rad * cosf(a0), cy + rad * sinf(a0));
            gGL.vertex2f(l + rad + rad * cosf(a1), cy + rad * sinf(a1));
            gGL.vertex2f(r - rad, cy);
            gGL.vertex2f(r - rad - rad * cosf(a0), cy + rad * sinf(a0));
            gGL.vertex2f(r - rad - rad * cosf(a1), cy + rad * sinf(a1));
        }
        gGL.end();
        if (edge_w > 0.f)
        {
            lineW(l + rad, t - edge_w * 0.5f, r - rad, t - edge_w * 0.5f, edge_w, edge);
            lineW(l + rad, b + edge_w * 0.5f, r - rad, b + edge_w * 0.5f, edge_w, edge);
            arc(l + rad, cy, rad - edge_w * 0.5f, edge_w, F_PI * 0.5f, F_PI * 1.5f, edge, 20);
            arc(r - rad, cy, rad - edge_w * 0.5f, edge_w, -F_PI * 0.5f, F_PI * 0.5f, edge, 20);
        }
    }

    // A big round arrow button (WolfStorm's .wx-spin): a dark disc, a light ring (amber on hover), a chevron.
    void spinButton(F32 cx, F32 cy, bool left, bool hover)
    {
        disc(cx, cy, SPIN_R, PLATE, 40);
        arc(cx, cy, SPIN_R - 1.f, 2.f, 0.f, 2.f * F_PI, hover ? AMBER : alpha(TEXT_DIM, 0.6f), 48);
        const F32 s = 9.f, d = left ? 1.f : -1.f;
        const LLColor4 ink = hover ? AMBER : TEXT;
        lineW(cx + d * s * 0.5f, cy + s, cx - d * s * 0.5f, cy, 3.5f, ink);
        lineW(cx - d * s * 0.5f, cy, cx + d * s * 0.5f, cy - s, 3.5f, ink);
    }

    // The sky's one texture, shared by every open window (there is one Wolf Experiences window): painted for the
    // size it is drawn at, again only when that changes.
    LLPointer<LLViewerTexture> sSky;
    S32 sSkyW = 0, sSkyH = 0;           // the painted size in pixels
    F32 sSkyU = 1.f, sSkyV = 1.f;       // how much of the (power-of-two) texture that is

    S32 nextPow2(S32 v) { S32 p = 1; while (p < v) p <<= 1; return p; }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfExperienceCarousel::WolfExperienceCarousel(const Params& p)
:   LLUICtrl(p)
{
    setTabStop(true);
}

WolfExperienceCarousel::~WolfExperienceCarousel()
{
    if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
}

void WolfExperienceCarousel::setData(const std::vector<Category>& categories, const std::vector<Entry>& entries)
{
    // Keep the chosen category (by id) and the pictures already asked for, when the same list comes again.
    const std::string chosen = (mPill >= 0 && mPill < (S32)mCategories.size()) ? mCategories[mPill].mId : std::string();
    std::vector<Entry> fresh = entries;
    for (Entry& e : fresh)
    {
        for (const Entry& old : mEntries)
        {
            if (old.mId == e.mId && old.mImage == e.mImage) { e.mPicture = old.mPicture; break; }
        }
    }
    const bool same = mHaveData && fresh.size() == mEntries.size() && categories.size() == mCategories.size()
        && std::equal(fresh.begin(), fresh.end(), mEntries.begin(), [](const Entry& a, const Entry& b) { return a.mId == b.mId; });
    mCategories = categories;
    mEntries = fresh;
    mHaveData = true;
    S32 pill = 0;
    for (S32 i = 0; i < (S32)mCategories.size(); ++i)
    {
        if (mCategories[i].mId == chosen) { pill = i; break; }
    }
    if (same && pill == mPill) return;   // the same list again (a refresh): the ring stays where it is
    mPill = -1;
    choose(pill);
}

void WolfExperienceCarousel::clearData()
{
    mHaveData = false;
    mCategories.clear();
    mEntries.clear();
    mShown.clear();
    mDetail = -1;
    mPill = 0;
}

void WolfExperienceCarousel::setSkyOnly(bool sky_only)
{
    mSkyOnly = sky_only;
    if (sky_only) mDetail = -1;
}

void WolfExperienceCarousel::choose(S32 pill)
{
    if (pill < 0 || pill >= (S32)mCategories.size() || pill == mPill) return;
    mPill = pill;
    mPillFocus = pill;
    mDetail = -1;
    mShown.clear();
    const std::string& id = mCategories[pill].mId;
    for (S32 i = 0; i < (S32)mEntries.size(); ++i)
    {
        if (id.empty() || mEntries[i].mCategory == id) mShown.push_back(i);
    }
    mTarget = 0.f;
    // Dealt in with a short spin (not with reduce motion).
    mPos = (reduceMotion() || count() < 2) ? 0.f : -(F32)llmin(2, count() - 1);
    mLast = LLFrameTimer::getElapsedSeconds();
    mMoving = mPos != mTarget;
}

LLSD WolfExperienceCarousel::getValue() const
{
    LLSD v = LLSD::emptyMap();
    v["data"] = mHaveData;
    v["sky_only"] = mSkyOnly;
    if (mPill >= 0 && mPill < (S32)mCategories.size())
    {
        v["category"] = mCategories[mPill].mId;
        v["category_title"] = mCategories[mPill].mTitle;
    }
    v["pills"] = (S32)mCategories.size();
    v["count"] = count();
    const S32 f = frontIndex();
    v["front"] = f;
    if (f >= 0) v["title"] = mEntries[mShown[f]].mTitle;
    v["detail"] = mDetail >= 0;
    v["moving"] = mMoving;
    return v;
}

S32 WolfExperienceCarousel::frontIndex() const
{
    const S32 n = count();
    if (!n) return -1;
    return ((ll_round(mTarget) % n) + n) % n;
}

void WolfExperienceCarousel::step(S32 delta)
{
    if (!count()) return;
    mTarget = (F32)ll_round(mTarget) + (F32)delta;
    if (reduceMotion()) mPos = mTarget;
    mLast = LLFrameTimer::getElapsedSeconds();
    mMoving = mPos != mTarget;
}

void WolfExperienceCarousel::goTo(S32 index)
{
    const S32 n = count();
    if (!n) return;
    S32 d = ((index - frontIndex()) % n + n) % n;
    if (d > n / 2) d -= n;
    step(d);
}

void WolfExperienceCarousel::openDetail(S32 index)
{
    if (index < 0 || index >= count()) return;
    mDetail = index;
    mDetailVisitFocus = true;
}

void WolfExperienceCarousel::closeDetail()
{
    mDetail = -1;
    mZone = Z_RING;
}

void WolfExperienceCarousel::visit(S32 index)
{
    if (index < 0 || index >= count() || !mOnVisit) return;
    mOnVisit(mEntries[mShown[index]].mId);   // the window teleports and closes (it may delete this control)
}

bool WolfExperienceCarousel::reduceMotion() const
{
    // The viewer's Wolf reduce-motion setting (settings_per_account.xml WolfAdventureReduceMotion).
    static LLCachedControl<bool> reduce(gSavedPerAccountSettings, "WolfAdventureReduceMotion", false);
    return reduce;
}

void WolfExperienceCarousel::animate()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLast), 0.f, 0.05f);
    mLast = now;
    if (!mMoving || mDragging) return;
    if (reduceMotion())
    {
        mPos = mTarget;
    }
    else
    {
        mPos += (mTarget - mPos) * (1.f - expf(-dt / EASE_S));
        if (fabsf(mTarget - mPos) < 0.002f) mPos = mTarget;
    }
    mMoving = mPos != mTarget;
}

// ── drawing ───────────────────────────────────────────────────────────────────────────────────────────

void WolfExperienceCarousel::layout()
{
    const F32 H = (F32)getRect().getHeight();
    mPillT = H - PILL_TOP;
    mPillB = mPillT - PILL_H;
    mCapB = FOOT_H;
    mCapT = mCapB + CAP_H;
    mStageB = mCapT + GAP;
    mStageT = mPillB - GAP;
    const F32 W = (F32)getRect().getWidth();
    const F32 stage_h = llmax(120.f, mStageT - mStageB);
    // Source: floater_wolf_experiences.js ExperienceRing._geometry - as big as the stage allows with room for a card
    // each side, never so small its title cannot be read.
    mCardW = (F32)ll_round(llclamp(llmin(W * (W < 560.f ? 0.56f : 0.34f), stage_h * 0.84f / CARD_ASPECT), 150.f, 360.f));
    mCardH = (F32)ll_round(mCardW * CARD_ASPECT);
}

void WolfExperienceCarousel::draw()
{
    mHits.clear();
    animate();
    layout();
    drawSky();
    if (!mSkyOnly && mHaveData && !mCategories.empty())
    {
        drawPills();
        drawRing();
        drawCaption();
        if (mDetail >= 0) drawDetail();
    }
    LLUICtrl::draw();
}

void WolfExperienceCarousel::drawSky()
{
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const F32 scale = LLUI::getScaleFactor().mV[VX];
    const S32 pw = ll_round((W + 2.f * SKY_MARGIN) * scale), ph = ll_round(H * scale);
    if (sSky.isNull() || pw != sSkyW || ph != sSkyH)
    {
        // Painted once for this size (wolfspacesky.h): the map's sky through Wolf Experiences' camera. GL textures here
        // must be a power of two each way (llimagegl.cpp:624 checkSize - the first rig run drew a plain grey quad), so
        // the painting goes into the corner of one and only that part is drawn.
        LLPointer<LLImageRaw> raw = WolfSpaceSky::paint(ll_round(W + 2.f * SKY_MARGIN), ll_round(H), scale, WolfSpaceSky::experiencesView());
        if (raw.notNull())
        {
            const S32 rw = raw->getWidth(), rh = raw->getHeight();
            const S32 tw = llmin(nextPow2(rw), 4096), th = llmin(nextPow2(rh), 4096);
            LLPointer<LLImageRaw> pot = new LLImageRaw(tw, th, 3);
            U8* dst = pot->getData();
            const U8* src = raw->getData();
            if (dst && src)
            {
                memset(dst, 0, (size_t)tw * th * 3);
                const S32 cw = llmin(rw, tw), chh = llmin(rh, th);
                for (S32 y = 0; y < chh; ++y)
                {
                    memcpy(dst + (size_t)y * tw * 3, src + (size_t)y * rw * 3, (size_t)cw * 3);
                }
                sSky = LLViewerTextureManager::getLocalTexture(pot.get(), false);
                sSkyU = (F32)cw / tw;
                sSkyV = (F32)chh / th;
            }
            sSkyW = pw;
            sSkyH = ph;
        }
    }
    if (sSky.isNull())
    {
        rect(0.f, 0.f, W, H, SPACE);
        return;
    }
    // The gentle drift with the ring's turn, never more than the painted margin (none with reduce motion).
    const F32 off = reduceMotion() ? 0.f : sinf(mPos * 0.22f) * SKY_MARGIN * 0.8f;
    const F32 l = -SKY_MARGIN + off;
    const F32 r = l + W + 2.f * SKY_MARGIN;
    LLLocalClipRect clip(getLocalRect());   // the margin painted for the drift stays inside the window
    gGL.getTexUnit(0)->bind(sSky);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.begin(LLRender::TRIANGLES);
    gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(l, 0.f);
    gGL.texCoord2f(sSkyU, 0.f); gGL.vertex2f(r, 0.f);
    gGL.texCoord2f(sSkyU, sSkyV); gGL.vertex2f(r, H);
    gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(l, 0.f);
    gGL.texCoord2f(sSkyU, sSkyV); gGL.vertex2f(r, H);
    gGL.texCoord2f(0.f, sSkyV); gGL.vertex2f(l, H);
    gGL.end();
    noTexture();
}

void WolfExperienceCarousel::drawPills()
{
    const F32 W = (F32)getRect().getWidth();
    const LLFontGL* name_font = fontBig();
    const LLFontGL* count_font = fontSmall();
    const F32 icon = 26.f;
    // Widths: icon, name or count (the wider), padding.
    std::vector<F32> widths;
    F32 total = 0.f;
    for (const Category& c : mCategories)
    {
        const std::string n = c.mCount == 1 ? std::string("1 place") : llformat("%d places", c.mCount);
        const F32 w = 14.f + icon + 10.f + llmax(name_font->getWidthF32(c.mTitle), count_font->getWidthF32(n)) + 22.f;
        widths.push_back(w);
        total += w + GAP;
    }
    total -= GAP;
    const F32 left0 = MARGIN, right0 = W - MARGIN;
    const bool overflow = total > right0 - left0;
    const F32 arrows = overflow ? 44.f : 0.f;   // room for the small scroll arrows at each end
    const F32 l_edge = left0 + arrows, r_edge = right0 - arrows;
    mPillScrollMax = llmax(0.f, total - (r_edge - l_edge));
    // The pill the keyboard just moved to, scrolled into view (once: the wheel and the arrows scroll freely after).
    if (mPillFollow)
    {
        mPillFollow = false;
        F32 x = 0.f;
        for (S32 i = 0; i < (S32)widths.size(); ++i)
        {
            if (i == mPillFocus)
            {
                if (x < mPillScroll) mPillScroll = x;
                if (x + widths[i] > mPillScroll + (r_edge - l_edge)) mPillScroll = x + widths[i] - (r_edge - l_edge);
            }
            x += widths[i] + GAP;
        }
    }
    mPillScroll = llclamp(mPillScroll, 0.f, mPillScrollMax);

    {
        LLLocalClipRect clip(LLRect(ll_round(l_edge), ll_round(mPillT + 4.f), ll_round(r_edge), ll_round(mPillB - 4.f)));
        F32 x = l_edge - mPillScroll;
        for (S32 i = 0; i < (S32)mCategories.size(); ++i)
        {
            const Category& c = mCategories[i];
            const F32 l = x, r = x + widths[i];
            x = r + GAP;
            if (r < l_edge || l > r_edge) continue;
            const bool on = i == mPill;
            const bool hover = mHoverHit == H_PILL && mHoverIndex == i;
            pill(l, mPillB, r, mPillT, on ? AMBER : PLATE, on ? AMBER : (hover ? AMBER : EDGE), 1.f);
            const LLColor4 ink = on ? INK_ON_AMBER : TEXT;
            LLPointer<LLUIImage> img = LLUI::getUIImage(c.mIcon);
            if (img.notNull())
            {
                img->draw(ll_round(l + 14.f), ll_round((mPillB + mPillT) * 0.5f - icon * 0.5f), ll_round(icon), ll_round(icon),
                          on ? INK_ON_AMBER : AMBER);
            }
            const F32 tx = l + 14.f + icon + 10.f;
            textL(name_font, c.mTitle, tx, mPillT - 6.f, ink, LLFontGL::LEFT, LLFontGL::TOP);
            textL(count_font, c.mCount == 1 ? std::string("1 place") : llformat("%d places", c.mCount), tx, mPillB + 7.f,
                  on ? INK_ON_AMBER : TEXT_DIM, LLFontGL::LEFT, LLFontGL::BOTTOM);
            if (hasFocus() && mZone == Z_PILLS && mPillFocus == i)
            {
                pill(l - 3.f, mPillB - 3.f, r + 3.f, mPillT + 3.f, LLColor4(0.f, 0.f, 0.f, 0.f), LLColor4(1.f, 0.70f, 0.28f, 1.f), 2.f);
            }
            addHit(llmax(l, l_edge), mPillB, llmin(r, r_edge), mPillT, H_PILL, i,
                   llformat("%s: %d %s", c.mTitle.c_str(), c.mCount, c.mCount == 1 ? "place" : "places"));
        }
    }
    if (overflow)
    {
        const F32 cy = (mPillB + mPillT) * 0.5f;
        const bool can_l = mPillScroll > 0.5f, can_r = mPillScroll < mPillScrollMax - 0.5f;
        auto small = [&](F32 cx, bool left, bool enabled, EHit id)
        {
            const bool hover = mHoverHit == id;
            disc(cx, cy, 17.f, PLATE, 32);
            arc(cx, cy, 16.f, 1.5f, 0.f, 2.f * F_PI, enabled ? (hover ? AMBER : EDGE) : alpha(EDGE, 0.4f), 32);
            const LLColor4 ink = enabled ? (hover ? AMBER : TEXT) : alpha(TEXT_DIM, 0.4f);
            const F32 d = left ? 1.f : -1.f;
            lineW(cx + d * 3.f, cy + 6.f, cx - d * 3.f, cy, 2.5f, ink);
            lineW(cx - d * 3.f, cy, cx + d * 3.f, cy - 6.f, 2.5f, ink);
            if (enabled) addHit(cx - 18.f, cy - 18.f, cx + 18.f, cy + 18.f, id, -1, left ? "More categories to the left" : "More categories to the right");
        };
        small(left0 + 18.f, true, can_l, H_PILL_LEFT);
        small(right0 - 18.f, false, can_r, H_PILL_RIGHT);
    }
}

void WolfExperienceCarousel::drawRing()
{
    const S32 n = count();
    if (!n) return;
    const F32 W = (F32)getRect().getWidth();
    const F32 stage_h = mStageT - mStageB;
    const F32 slots = llclamp((F32)n, MIN_SLOTS, MAX_SLOTS);
    const F32 step_rad = 2.f * F_PI / slots;
    const F32 cw = mCardW, ch = mCardH;
    const F32 radius = (cw * CHORD) / tanf(step_rad * 0.5f);
    const F32 P = llmax(700.f, radius * CAMERA_RADII);
    // The vanishing point at the stage's middle; the ring's centre a little above it (WolfStorm: top 46% of the stage).
    const F32 ox = W * 0.5f, oy = mStageB + stage_h * 0.5f;
    const F32 ring_dy = -stage_h * 0.04f;   // CSS y (down): the ring 4% of the stage above the middle
    // A point of the card plane: CSS-style coordinates about the ring's centre (y down, z towards the eye).
    auto project = [&](F32 X, F32 Y, F32 Z) -> LLVector2
    {
        const F32 s = P / (P - Z);
        return LLVector2(ox + X * s, oy - (Y + ring_dy) * s);
    };

    struct Card { S32 i; F32 k; F32 th; };
    std::vector<Card> cards;
    const F32 vis = llmin(VISIBLE_SLOTS, llmin((F32)n * 0.5f, slots * 0.5f - 0.6f));
    for (S32 i = 0; i < n; ++i)
    {
        F32 k = fmodf((F32)i - mPos, (F32)n);
        if (k < 0.f) k += (F32)n;
        if (k >= (F32)n * 0.5f) k -= (F32)n;
        if (fabsf(k) > vis + 0.5f) continue;
        cards.push_back({ i, k, k * step_rad });
    }
    // Back to front: the most distant first.
    std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) { return cosf(a.th) < cosf(b.th); });

    // The floor's light under the cards (WolfStorm .wx-floor): a soft amber-to-blue ellipse.
    {
        const LLVector2 c = project(0.f, ch * 0.5f + 18.f, 0.f);
        const F32 rx = radius * 1.3f, ry = radius * 1.3f * 0.16f;
        noTexture();
        gGL.begin(LLRender::TRIANGLES);
        const S32 seg = 48;
        for (S32 j = 0; j < seg; ++j)
        {
            const F32 a0 = 2.f * F_PI * j / seg, a1 = 2.f * F_PI * (j + 1) / seg;
            const LLVector2 m0(c.mV[0] + rx * 0.55f * cosf(a0), c.mV[1] + ry * 0.55f * sinf(a0));
            const LLVector2 m1(c.mV[0] + rx * 0.55f * cosf(a1), c.mV[1] + ry * 0.55f * sinf(a1));
            const LLVector2 e0(c.mV[0] + rx * cosf(a0), c.mV[1] + ry * sinf(a0));
            const LLVector2 e1(c.mV[0] + rx * cosf(a1), c.mV[1] + ry * sinf(a1));
            gGL.color4fv(FLOOR_WARM.mV); gGL.vertex2f(c.mV[0], c.mV[1]);
            gGL.color4fv(FLOOR_COOL.mV); gGL.vertex2f(m0.mV[0], m0.mV[1]);
            gGL.color4fv(FLOOR_COOL.mV); gGL.vertex2f(m1.mV[0], m1.mV[1]);
            gGL.color4fv(FLOOR_COOL.mV); gGL.vertex2f(m0.mV[0], m0.mV[1]);
            gGL.color4fv(alpha(FLOOR_COOL, 0.f).mV); gGL.vertex2f(e0.mV[0], e0.mV[1]);
            gGL.color4fv(alpha(FLOOR_COOL, 0.f).mV); gGL.vertex2f(e1.mV[0], e1.mV[1]);
            gGL.color4fv(FLOOR_COOL.mV); gGL.vertex2f(m0.mV[0], m0.mV[1]);
            gGL.color4fv(alpha(FLOOR_COOL, 0.f).mV); gGL.vertex2f(e1.mV[0], e1.mV[1]);
            gGL.color4fv(FLOOR_COOL.mV); gGL.vertex2f(m1.mV[0], m1.mV[1]);
        }
        gGL.end();
    }

    // The stage itself: a press anywhere on it may become a drag (added first, so every card is on top of it).
    addHit(0.f, mStageB, W, mStageT, H_STAGE);
    LLLocalClipRect clip(LLRect(0, ll_round(mStageT), getRect().getWidth(), ll_round(mStageB)));
    const LLFontGL* title_big = fontBig();
    const LLFontGL* small = fontSmall();
    const F32 pic_frac = (cw * 0.75f) / ch;   // the picture's share of the card's height
    for (const Card& cd : cards)
    {
        Entry& e = mEntries[mShown[cd.i]];
        const F32 a = fabsf(cd.k);
        const F32 fade = llclamp((vis + 0.5f - a) / 0.8f, 0.f, 1.f);
        if (fade <= 0.01f) continue;
        const bool front = a < 0.5f;
        const F32 Xc = sinf(cd.th) * radius;
        const F32 Zc = (cosf(cd.th) - 1.f) * radius;
        const F32 Yc = -(1.f - cosf(cd.th)) * ch * LIFT;
        const F32 phi = cd.th * TURN;
        const F32 cphi = cosf(phi), sphi = sinf(phi);
        // (u, v) on the card: u 0..1 left to right, v 0..1 top to bottom (and past 1 for the reflection).
        auto at = [&](F32 u, F32 v) -> LLVector2
        {
            const F32 lx = (u - 0.5f) * cw, ly = (v - 0.5f) * ch;
            return project(Xc + lx * cphi, Yc + ly, Zc - lx * sphi);
        };

        // The reflection on the floor (WolfStorm: -webkit-box-reflect below 10px, fading): the words' band mirrored.
        {
            const F32 gap = 10.f / ch, len = 0.28f;
            const LLVector2 c4[4] = { at(0.f, 1.f + gap), at(1.f, 1.f + gap), at(1.f, 1.f + gap + len), at(0.f, 1.f + gap + len) };
            const LLColor4 top = alpha(front ? AMBER : BAND_TOP, (front ? 0.10f : 0.14f) * fade), bot(0.f, 0.f, 0.f, 0.f);
            const LLColor4 c4c[4] = { top, top, bot, bot };
            quad(c4, c4c);
        }

        // The picture, in strips: the texture once loaded, else a dark gradient and the category's icon.
        if (e.mPicture.isNull() && e.mImage.notNull())
        {
            e.mPicture = LLUI::getUIImageByID(e.mImage);   // asked for now: the first time this card is in view
        }
        LLViewerTexture* tex = e.mPicture.notNull() ? dynamic_cast<LLViewerTexture*>(e.mPicture->getImage().get()) : nullptr;
        const bool have_pic = tex && tex->hasGLTexture();
        if (have_pic)
        {
            gGL.getTexUnit(0)->bind(tex);
            gGL.color4f(1.f, 1.f, 1.f, fade);
            gGL.begin(LLRender::TRIANGLES);
            for (S32 s = 0; s < STRIPS; ++s)
            {
                const F32 u0 = (F32)s / STRIPS, u1 = (F32)(s + 1) / STRIPS;
                const LLVector2 p0 = at(u0, 0.f), p1 = at(u1, 0.f), p2 = at(u1, pic_frac), p3 = at(u0, pic_frac);
                gGL.texCoord2f(u0, 1.f); gGL.vertex2f(p0.mV[0], p0.mV[1]);
                gGL.texCoord2f(u1, 1.f); gGL.vertex2f(p1.mV[0], p1.mV[1]);
                gGL.texCoord2f(u1, 0.f); gGL.vertex2f(p2.mV[0], p2.mV[1]);
                gGL.texCoord2f(u0, 1.f); gGL.vertex2f(p0.mV[0], p0.mV[1]);
                gGL.texCoord2f(u1, 0.f); gGL.vertex2f(p2.mV[0], p2.mV[1]);
                gGL.texCoord2f(u0, 0.f); gGL.vertex2f(p3.mV[0], p3.mV[1]);
            }
            gGL.end();
            noTexture();
        }
        else
        {
            const LLVector2 c4[4] = { at(0.f, 0.f), at(1.f, 0.f), at(1.f, pic_frac), at(0.f, pic_frac) };
            const LLColor4 top = alpha(LLColor4(29.f / 255.f, 42.f / 255.f, 82.f / 255.f, 1.f), fade);   // #1d2a52
            const LLColor4 bot = alpha(LLColor4(11.f / 255.f, 18.f / 255.f, 38.f / 255.f, 1.f), fade);   // #0b1226
            const LLColor4 c4c[4] = { top, top, bot, bot };
            quad(c4, c4c);
            LLPointer<LLUIImage> img = LLUI::getUIImage(e.mIcon);
            if (img.notNull())
            {
                const LLVector2 mid = at(0.5f, pic_frac * 0.5f);
                const F32 sz = cw * 0.28f * (P / (P - Zc));
                img->draw(ll_round(mid.mV[0] - sz * 0.5f), ll_round(mid.mV[1] - sz * 0.5f), ll_round(sz), ll_round(sz), alpha(AMBER, 0.85f * fade));
            }
        }
        // The words' band.
        {
            const LLVector2 c4[4] = { at(0.f, pic_frac), at(1.f, pic_frac), at(1.f, 1.f), at(0.f, 1.f) };
            const LLColor4 c4c[4] = { alpha(BAND_TOP, fade), alpha(BAND_TOP, fade), alpha(BAND_BOT, fade), alpha(BAND_BOT, fade) };
            quad(c4, c4c);
        }
        // The words: flat lettering where the band is (the front card is face on; side cards get a short title).
        if (a < 1.6f)
        {
            const LLVector2 tl = at(0.f, pic_frac), tr = at(1.f, pic_frac);
            const F32 band_w = (tr.mV[0] - tl.mV[0]) - 24.f;
            if (band_w > 40.f)
            {
                const F32 tx = tl.mV[0] + 12.f;
                F32 ty = llmin(tl.mV[1], tr.mV[1]) - 8.f;
                const LLFontGL* tf = front ? title_big : small;
                const std::vector<std::string> lines = wrapLines(tf, e.mTitle, band_w, front ? 2 : 1);
                for (const std::string& line : lines)
                {
                    textL(tf, line, tx, ty, alpha(TEXT, fade), LLFontGL::LEFT, LLFontGL::TOP, band_w);
                    ty -= tf->getLineHeight();
                }
                textL(small, e.mRegion, tx, ty - 2.f, alpha(TEXT_DIM, fade), LLFontGL::LEFT, LLFontGL::TOP, band_w);
            }
        }
        // Featured: the amber badge, top left (flat).
        if (e.mFeatured && a < 1.6f)
        {
            const LLVector2 tl = at(0.f, 0.f);
            const LLFontGL* bf = LLFontGL::getFontSansSerifSmallBold();
            const std::string label = "\xE2\x98\x85 Featured";   // U+2605
            const F32 bw = bf->getWidthF32(label) + 16.f, bh = 20.f;
            const F32 bl = tl.mV[0] + 10.f, bt = tl.mV[1] - 10.f;
            pill(bl, bt - bh, bl + bw, bt, alpha(AMBER, fade), alpha(AMBER, fade), 0.f);
            textL(bf, label, bl + 8.f, bt - bh * 0.5f, INK_ON_AMBER, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        // Dim with distance, then the edge (amber and glowing at the front).
        const LLVector2 q[4] = { at(0.f, 0.f), at(1.f, 0.f), at(1.f, 1.f), at(0.f, 1.f) };
        const F32 dim = llmin(0.7f, a * 0.24f);
        if (dim > 0.01f)
        {
            const LLColor4 d = alpha(SPACE, dim * fade);
            const LLColor4 dc[4] = { d, d, d, d };
            quad(q, dc);
        }
        for (int j = 0; j < 4; ++j)
        {
            const LLVector2& p0 = q[j];
            const LLVector2& p1 = q[(j + 1) % 4];
            if (front)
            {
                lineW(p0.mV[0], p0.mV[1], p1.mV[0], p1.mV[1], 14.f, alpha(AMBER, 0.10f));
                lineW(p0.mV[0], p0.mV[1], p1.mV[0], p1.mV[1], 6.f, alpha(AMBER, 0.20f));
                lineW(p0.mV[0], p0.mV[1], p1.mV[0], p1.mV[1], 2.f, AMBER);
            }
            else
            {
                lineW(p0.mV[0], p0.mV[1], p1.mV[0], p1.mV[1], 1.f, alpha(EDGE, 0.8f * fade));
            }
        }
        if (front && hasFocus() && mZone == Z_RING && mDetail < 0)
        {
            for (int j = 0; j < 4; ++j)
            {
                const LLVector2 c0 = q[j], c1 = q[(j + 1) % 4];
                const LLVector2 mid((q[0].mV[0] + q[2].mV[0]) * 0.5f, (q[0].mV[1] + q[2].mV[1]) * 0.5f);
                auto out = [&](const LLVector2& p) { LLVector2 d = p - mid; d.normalize(); return p + d * 6.f; };
                const LLVector2 o0 = out(c0), o1 = out(c1);
                lineW(o0.mV[0], o0.mV[1], o1.mV[0], o1.mV[1], 3.f, LLColor4(1.f, 0.70f, 0.28f, 1.f));   // #ffb347
            }
        }
        // The hit: the card's screen box (front-most drawn last, so hitAt finds it first - see hitAt).
        F32 l = q[0].mV[0], r = q[0].mV[0], b = q[0].mV[1], t = q[0].mV[1];
        for (const LLVector2& p : q) { l = llmin(l, p.mV[0]); r = llmax(r, p.mV[0]); b = llmin(b, p.mV[1]); t = llmax(t, p.mV[1]); }
        addHit(l, llmax(b, mStageB), r, llmin(t, mStageT), H_CARD, cd.i, front ? e.mTitle + " - click for the details" : e.mTitle);
    }

    if (n > 1)
    {
        const F32 cy = oy;
        spinButton(MARGIN + SPIN_R, cy, true, mHoverHit == H_PREV);
        addHit(MARGIN, cy - SPIN_R, MARGIN + 2.f * SPIN_R, cy + SPIN_R, H_PREV, -1, "Previous experience (Left arrow)");
        spinButton(W - MARGIN - SPIN_R, cy, false, mHoverHit == H_NEXT);
        addHit(W - MARGIN - 2.f * SPIN_R, cy - SPIN_R, W - MARGIN, cy + SPIN_R, H_NEXT, -1, "Next experience (Right arrow)");
    }
}

void WolfExperienceCarousel::drawCaption()
{
    const S32 n = count();
    const S32 i = n ? ((ll_round(mPos) % n) + n) % n : -1;
    if (i < 0) return;
    const Entry& e = mEntries[mShown[i]];
    const F32 W = (F32)getRect().getWidth();
    const F32 l = MARGIN, r = W - MARGIN;
    chamfer(l, mCapB, r, mCapT, 8.f, PLATE, PLATE);
    chamferEdge(l + 0.5f, mCapB + 0.5f, r - 0.5f, mCapT - 0.5f, 8.f, 1.f, alpha(EDGE, 0.7f));
    const Palette& pal = wolvesPalette();
    const F32 bw = 128.f, bh = 48.f, by = (mCapB + mCapT) * 0.5f - bh * 0.5f;
    const F32 visit_l = r - 14.f - bw, details_l = visit_l - 12.f - bw;
    const F32 text_w = details_l - (l + 14.f) - 12.f;
    textL(fontHuge(), e.mTitle, l + 14.f, mCapT - 8.f, TEXT, LLFontGL::LEFT, LLFontGL::TOP, text_w);
    std::string where = e.mRegion;
    if (!e.mBy.empty()) where += "  \xC2\xB7  by " + e.mBy;
    textL(fontBody(), where, l + 14.f, mCapB + 26.f, TEXT_DIM, LLFontGL::LEFT, LLFontGL::BOTTOM, text_w);
    textL(fontSmall(), llformat("%d of %d", i + 1, n), l + 14.f, mCapB + 8.f, TEXT_DIM, LLFontGL::LEFT, LLFontGL::BOTTOM);
    if (mDetail >= 0) return;   // the details panel covers the buttons
    U32 fd = (mHoverHit == H_DETAILS ? BTN_HOVER : 0) | (hasFocus() && mZone == Z_DETAILS ? BTN_FOCUS : 0);
    button(details_l, by, details_l + bw, by + bh, "Details", pal, fd, fontBig());
    addHit(details_l, by, details_l + bw, by + bh, H_DETAILS, i, "What it is and what to do when you arrive");
    U32 fv = BTN_PRIMARY | (mHoverHit == H_VISIT ? BTN_HOVER : 0) | (hasFocus() && mZone == Z_VISIT ? BTN_FOCUS : 0);
    button(visit_l, by, visit_l + bw, by + bh, "Visit", pal, fv, fontBig());
    addHit(visit_l, by, visit_l + bw, by + bh, H_VISIT, i, "Teleport to " + e.mRegion);
}

void WolfExperienceCarousel::drawDetail()
{
    if (mDetail < 0 || mDetail >= count()) return;
    Entry& e = mEntries[mShown[mDetail]];
    const F32 W = (F32)getRect().getWidth();
    // The scrim over the ring and the caption.
    rect(0.f, mCapB, W, mStageT, alpha(SPACE, 0.72f));
    addHit(0.f, mCapB, W, mStageT, H_D_SCRIM);
    const F32 avail_h = mStageT - mCapB - 16.f;
    const F32 pw = llmin(760.f, W - 2.f * MARGIN), ph = llmin(avail_h, 420.f);
    const F32 l = (W - pw) * 0.5f, r = l + pw, b = mCapB + (mStageT - mCapB - ph) * 0.5f, t = b + ph;
    // The panel: dark gradient, amber edge with a glow (WolfStorm .wx-dbox).
    chamferEdge(l - 4.f, b - 4.f, r + 4.f, t + 4.f, 14.f, 8.f, alpha(AMBER, 0.12f));
    chamfer(l, b, r, t, 12.f, LLColor4(17.f / 255.f, 27.f / 255.f, 54.f / 255.f, 1.f), BAND_BOT);
    chamferEdge(l + 1.f, b + 1.f, r - 1.f, t - 1.f, 12.f, 2.f, AMBER);
    addHit(l, b, r, t, H_D_PANEL);
    // The picture, 4:3, on the left (above on a narrow window there is no room for both side by side, so it shrinks).
    const F32 pad = 16.f;
    const F32 pic_w = llmin((pw - 3.f * pad) * 0.48f, (ph - 2.f * pad) * 4.f / 3.f), pic_h = pic_w * 0.75f;
    const F32 pl = l + pad, pt = t - pad, pb = pt - pic_h;
    if (e.mPicture.isNull() && e.mImage.notNull()) e.mPicture = LLUI::getUIImageByID(e.mImage);
    LLViewerTexture* tex = e.mPicture.notNull() ? dynamic_cast<LLViewerTexture*>(e.mPicture->getImage().get()) : nullptr;
    if (tex && tex->hasGLTexture())
    {
        e.mPicture->draw(ll_round(pl), ll_round(pb), ll_round(pic_w), ll_round(pic_h), LLColor4::white);
    }
    else
    {
        rectG(pl, pb, pl + pic_w, pt, LLColor4(29.f / 255.f, 42.f / 255.f, 82.f / 255.f, 1.f), LLColor4(11.f / 255.f, 18.f / 255.f, 38.f / 255.f, 1.f));
        LLPointer<LLUIImage> img = LLUI::getUIImage(e.mIcon);
        if (img.notNull())
        {
            img->draw(ll_round(pl + pic_w * 0.5f - 48.f), ll_round(pb + pic_h * 0.5f - 48.f), 96, 96, alpha(AMBER, 0.85f));
        }
    }
    // The words on the right.
    const F32 tl = pl + pic_w + pad, tw = r - pad - tl;
    F32 y = t - pad;
    std::string cat;
    for (const Category& c : mCategories) if (!c.mId.empty() && c.mId == e.mCategory) cat = c.mTitle;
    textL(LLFontGL::getFontSansSerifBold(), caps((e.mFeatured ? std::string("\xE2\x98\x85 Featured  \xC2\xB7  ") : std::string()) + cat), tl, y, AMBER,
          LLFontGL::LEFT, LLFontGL::TOP, tw);
    y -= 22.f;
    for (const std::string& line : wrapLines(fontHuge(), e.mTitle, tw, 2))
    {
        textL(fontHuge(), line, tl, y, TEXT, LLFontGL::LEFT, LLFontGL::TOP, tw);
        y -= fontHuge()->getLineHeight();
    }
    if (!e.mBy.empty())
    {
        textL(fontSmall(), "by " + e.mBy, tl, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP, tw);
        y -= fontSmall()->getLineHeight() + 2.f;
    }
    y -= 6.f;
    const LLFontGL* body = fontBody();
    for (const std::string& line : wrapLines(body, e.mBlurb, tw, 4))
    {
        textL(body, line, tl, y, LLColor4(230.f / 255.f, 235.f / 255.f, 245.f / 255.f, 1.f), LLFontGL::LEFT, LLFontGL::TOP, tw);
        y -= body->getLineHeight();
    }
    y -= 8.f;
    if (!e.mArrive.empty())
    {
        const std::vector<std::string> lines = wrapLines(body, "When you arrive: " + e.mArrive, tw - 22.f, 3);
        const F32 box_h = lines.size() * body->getLineHeight() + 14.f;
        rect(tl, y - box_h, tl + tw, y, alpha(AMBER, 0.12f));
        rect(tl, y - box_h, tl + 4.f, y, AMBER);
        F32 ly = y - 7.f;
        for (const std::string& line : lines)
        {
            textL(body, line, tl + 14.f, ly, LLColor4(230.f / 255.f, 235.f / 255.f, 245.f / 255.f, 1.f), LLFontGL::LEFT, LLFontGL::TOP, tw - 22.f);
            ly -= body->getLineHeight();
        }
        y -= box_h + 8.f;
    }
    textL(body, "In " + e.mRegion, tl, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP, tw);
    // Back and Visit, bottom right.
    const Palette& pal = wolvesPalette();
    const F32 bh = 48.f, vw = 160.f, bkw = 120.f;
    const F32 vl = r - pad - vw, bl = vl - 12.f - bkw, bb = b + pad;
    button(bl, bb, bl + bkw, bb + bh, "Back", pal, (mHoverHit == H_D_BACK ? BTN_HOVER : 0) | (hasFocus() && !mDetailVisitFocus ? BTN_FOCUS : 0), fontBig());
    addHit(bl, bb, bl + bkw, bb + bh, H_D_BACK, mDetail, "Back to the carousel (Escape)");
    button(vl, bb, vl + vw, bb + bh, "Visit", pal, BTN_PRIMARY | (mHoverHit == H_D_VISIT ? BTN_HOVER : 0) | (hasFocus() && mDetailVisitFocus ? BTN_FOCUS : 0),
           fontHuge());
    addHit(vl, bb, vl + vw, bb + bh, H_D_VISIT, mDetail, "Teleport to " + e.mRegion);
}

// ── hits and input ────────────────────────────────────────────────────────────────────────────────────

void WolfExperienceCarousel::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, S32 index, const std::string& tip)
{
    mHits.push_back({ l, b, r, t, id, index, tip });
}

const WolfExperienceCarousel::Hit* WolfExperienceCarousel::hitAt(S32 x, S32 y) const
{
    // The last added is on top (cards are added back to front, the panel after everything).
    const bool detail = mDetail >= 0;
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (x >= it->mL && x <= it->mR && y >= it->mB && y <= it->mT)
        {
            // With the details open only the panel's own hits (and the scrim) count.
            if (detail && it->mId != H_D_BACK && it->mId != H_D_VISIT && it->mId != H_D_PANEL && it->mId != H_D_SCRIM) continue;
            return &*it;
        }
    }
    return nullptr;
}

bool WolfExperienceCarousel::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const Hit* h = hitAt(x, y);
    if (!h) return false;   // empty space: nothing of ours (the window's own widgets sit on top anyway)
    setFocus(true);
    mDownHit = h->mId;
    mDownIndex = h->mIndex;
    mDownX = x;
    mDownPos = mPos;
    mDragMoved = false;
    mSamples.clear();
    mSamples.push_back({ x, LLFrameTimer::getElapsedSeconds() });
    // A press on the ring or a card may become a drag; anything else is a button.
    mDragging = mDetail < 0 && (h->mId == H_CARD || h->mId == H_STAGE) && count() > 1;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfExperienceCarousel::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mDragging)
    {
        const S32 dx = x - mDownX;
        if (!mDragMoved && abs(dx) < DRAG_SLOP_PX) return true;
        mDragMoved = true;
        mPos = mDownPos - (F32)dx / (mCardW * 0.9f);
        mMoving = true;
        mSamples.push_back({ x, LLFrameTimer::getElapsedSeconds() });
        if (mSamples.size() > 6) mSamples.erase(mSamples.begin());
        return true;
    }
    const Hit* h = hitAt(x, y);
    mHoverHit = h ? h->mId : H_NONE;
    mHoverIndex = h ? h->mIndex : -1;
    if (h && h->mId != H_D_PANEL && h->mId != H_D_SCRIM && h->mId != H_STAGE) getWindow()->setCursor(UI_CURSOR_HAND);
    return h != nullptr;
}

bool WolfExperienceCarousel::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    gFocusMgr.setMouseCapture(nullptr);
    const bool dragged = mDragging && mDragMoved;
    mDragging = false;
    if (dragged)
    {
        // A flick carries on: the release speed (cards per second) for about 0.18 s more, then the nearest card.
        const Sample& a = mSamples.front();
        const Sample& b = mSamples.back();
        const F32 v = (b.t > a.t) ? -((F32)(b.x - a.x) / (mCardW * 0.9f)) / (F32)(b.t - a.t) : 0.f;
        mTarget = (F32)ll_round(mPos + llclamp(v * 0.18f, -4.f, 4.f));
        if (reduceMotion()) mPos = mTarget;
        mLast = LLFrameTimer::getElapsedSeconds();
        mMoving = mPos != mTarget;
        return true;
    }
    // A click: only when released on what was pressed.
    const Hit* h = hitAt(x, y);
    if (!h || h->mId != mDownHit || h->mIndex != mDownIndex) return true;
    switch (h->mId)
    {
    case H_PILL: choose(h->mIndex); mZone = Z_PILLS; mPillFocus = h->mIndex; break;
    case H_PILL_LEFT: mPillScroll -= ((F32)getRect().getWidth() - 2.f * MARGIN) * 0.6f; break;
    case H_PILL_RIGHT: mPillScroll += ((F32)getRect().getWidth() - 2.f * MARGIN) * 0.6f; break;
    case H_PREV: step(-1); mZone = Z_RING; break;
    case H_NEXT: step(1); mZone = Z_RING; break;
    case H_CARD:
        mZone = Z_RING;
        if (h->mIndex == frontIndex()) openDetail(h->mIndex); else goTo(h->mIndex);
        break;
    case H_DETAILS: openDetail(h->mIndex); break;
    case H_VISIT: visit(h->mIndex); return true;     // may close the window
    case H_D_BACK: closeDetail(); break;
    case H_D_VISIT: visit(h->mIndex); return true;
    case H_D_SCRIM: closeDetail(); break;
    default: break;
    }
    return true;
}

void WolfExperienceCarousel::onMouseCaptureLost()
{
    if (mDragging && mDragMoved)
    {
        mTarget = (F32)ll_round(mPos);
        mMoving = mPos != mTarget;
    }
    mDragging = false;
}

bool WolfExperienceCarousel::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mSkyOnly || !mHaveData || mDetail >= 0 || clicks == 0) return mDetail >= 0;
    if (y >= mPillB && y <= mPillT)
    {
        mPillScroll += clicks * 60.f;   // over the pills: scroll them sideways
        return true;
    }
    if (y >= mStageB && y <= mStageT && count())
    {
        step(clicks > 0 ? 1 : -1);     // one card a notch
        return true;
    }
    return false;
}

bool WolfExperienceCarousel::handleToolTip(S32 x, S32 y, MASK mask)
{
    const Hit* h = hitAt(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

void WolfExperienceCarousel::onFocusReceived()
{
    LLUICtrl::onFocusReceived();
    if (mDetail < 0) mZone = Z_RING;
}

bool WolfExperienceCarousel::handleKeyHere(KEY key, MASK mask)
{
    if (mSkyOnly || !mHaveData) return false;
    // The details panel: Back / Visit, Escape goes back.
    if (mDetail >= 0)
    {
        if (key == KEY_ESCAPE && mask == MASK_NONE) { closeDetail(); return true; }
        if (key == KEY_TAB || key == KEY_LEFT || key == KEY_RIGHT) { mDetailVisitFocus = !mDetailVisitFocus; return true; }
        if ((key == KEY_RETURN || key == ' ') && mask == MASK_NONE)
        {
            if (mDetailVisitFocus) visit(mDetail); else closeDetail();
            return true;
        }
        return key != KEY_ESCAPE;   // keys stay in the panel; Escape with a modifier goes on
    }
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        // pills -> ring -> Details -> Visit, then out of the control (and back the other way).
        const bool fwd = mask == MASK_NONE;
        const S32 z = (S32)mZone + (fwd ? 1 : -1);
        if (z < (S32)Z_PILLS || z > (S32)Z_VISIT || (z >= (S32)Z_DETAILS && !count())) return false;
        mZone = (EZone)z;
        if (mZone == Z_PILLS) { mPillFocus = mPill; mPillFollow = true; }
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (mZone)
    {
    case Z_PILLS:
        if (key == KEY_LEFT) { mPillFocus = llmax(0, mPillFocus - 1); mPillFollow = true; return true; }
        if (key == KEY_RIGHT) { mPillFocus = llmin((S32)mCategories.size() - 1, mPillFocus + 1); mPillFollow = true; return true; }
        if (key == KEY_DOWN) { mZone = Z_RING; return true; }
        if (key == KEY_RETURN || key == ' ') { choose(mPillFocus); return true; }
        return false;
    case Z_RING:
        if (!count()) return false;
        if (key == KEY_RIGHT || key == KEY_DOWN) { step(1); return true; }
        if (key == KEY_LEFT) { step(-1); return true; }
        if (key == KEY_UP) { mZone = Z_PILLS; mPillFocus = mPill; mPillFollow = true; return true; }
        if (key == KEY_PAGE_DOWN) { step(3); return true; }
        if (key == KEY_PAGE_UP) { step(-3); return true; }
        if (key == KEY_HOME) { goTo(0); return true; }
        if (key == KEY_END) { goTo(count() - 1); return true; }
        if (key == KEY_RETURN || key == ' ') { openDetail(frontIndex()); return true; }
        return false;
    case Z_DETAILS:
    case Z_VISIT:
        if (key == KEY_LEFT || key == KEY_RIGHT) { mZone = mZone == Z_DETAILS ? Z_VISIT : Z_DETAILS; return true; }
        if (key == KEY_RETURN || key == ' ')
        {
            if (mZone == Z_DETAILS) openDetail(frontIndex()); else visit(frontIndex());
            return true;
        }
        return false;
    }
    return false;
}
