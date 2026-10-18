/**
 * @file wolffloateradventuregm.cpp
 * @brief WolfViewer: Wolf Adventure GM Console (STUDIO_AND_CONSOLE_V1.md §3) and the console desk.
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

#include "wolffloateradventuregm.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "llagent.h"
#include "llagentcamera.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llmd5.h"
#include "llrender.h"
#include "lltoolmgr.h"
#include "lltooltip.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "wolfadventure.h"
#include "wolfadventurehud.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureGmView> r_wolf_adventure_gm_view("wolf_adventure_gm_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    // The inks of the Roleplay window, the Studio and the talk panel: 4.5:1 or better on the game's cards.
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24
    // STUDIO_AND_CONSOLE_V1.md §1 Feedback: "confirmations fade, errors stay until the next action".
    const F64 STATUS_LIFE = 6.0;
    // STUDIO_AND_CONSOLE_V1.md §3: "It polls gm_state every 2 s while open."
    const F64 POLL_SEC = 2.0;
    const F32 ROW_H = 28.f;

    // Source: HUD_MESSAGES_V1.md §4 gm_move gait; ADVENTURE_PACK_V1.md §6 paths gait walk/run/fly.
    const char* const GAITS[] = { "walk", "run", "fly" };
    // Source: wolfstorm/php/wolf_env_lib.php:55 WE_TYPES (a fixed weather type the World tab may set).
    const char* const WEATHERS[] = { "clear", "fair", "cloudy", "overcast", "rain", "storm", "snow", "fog" };
    // Source: RULES_PRESETS_V1.md §4 roll outcomes; HUD_MESSAGES_V1.md §2.9 dice.outcome.
    const char* const OUTCOMES[] = { "success", "failure", "critical", "fumble" };

    std::string whenText(const LLSD& v)
    {
        if (!(v.isInteger() || v.isReal())) return cap(v.asString(), 24);
        const time_t t = (time_t)v.asReal();
        struct tm lt;
#if LL_WINDOWS
        if (localtime_s(&lt, &t) != 0) return std::string();
#else
        if (!localtime_r(&t, &lt)) return std::string();
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M:%S", &lt);
        return buf;
    }

    std::string controlWords(const LLSD& sess)
    {
        // Source: STUDIO_AND_CONSOLE_V1.md §3 top bar ("AI Director running" / "You are running it" / "Paused");
        // BRAIN_API_V1.md §2.2 sessions.control auto | live | paused.
        const std::string c = sess["control"].asString();
        if (c == "paused") return "Paused";
        if (c == "live")
        {
            if (LLUUID(sess["gm"].asString()) == gAgentID) return "You are running it";
            const std::string n = cap(sess["gmName"].asString(), 64);
            return (n.empty() ? std::string("A host") : n) + " is running it";
        }
        return "AI Director running";
    }

    // The log drawer's filters (STUDIO §3 Log: story, players, AI, Director, GM, rolls, rewards).
    const char* const LOG_FILTERS[] = { "All", "Story", "Players", "AI", "Director", "Host", "Rolls", "Rewards" };
    const S32 LOG_FILTER_COUNT = 8;

    bool logMatches(S32 filter, const LLSD& e)
    {
        if (filter <= 0) return true;
        const std::string src = e["source"].asString(), type = e["type"].asString();
        // BRAIN_API_V1.md §2.3 wolfadv_events.source region | gm | director | brain | lsl
        switch (filter)
        {
        case 1: return src == "brain" && (type == "beat" || type == "trigger" || type == "choose" || type == "publish" || type == "revision" || type.find("end") != std::string::npos);
        case 2: return src == "region";
        case 3: return type.compare(0, 3, "ai_") == 0 || type == "action_refused" || type == "talk" || type == "say";
        case 4: return src == "director";
        case 5: return src == "gm";
        case 6: return type == "roll" || type == "dice" || type == "rollask" || type == "override";
        case 7: return type.find("reward") != std::string::npos || type == "game_result";
        default: return true;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// helpers
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfAdventureDesk
{
    LLUUID npcAgent(const LLUUID& region, const std::string& handle)
    {
        // Source: REGION_HANDS_V1.md §4.1 (settled in P1): MD5 over the UTF-8 bytes of "wolfadv:" + region +
        // ":" + handle (the region lower-case and hyphenated), byte 6 = (b & 0x0f) | 0x30, byte 8 = (b & 0x3f) |
        // 0x80, the 16 bytes in order as the uuid's hex.
        const std::string text = "wolfadv:" + region.asString() + ":" + handle;
        LLMD5 md5;
        md5.update((const uint8_t*)text.data(), text.size());
        md5.finalize();
        unsigned char b[16];
        md5.raw_digest(b);
        b[6] = (unsigned char)((b[6] & 0x0f) | 0x30);
        b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
        static const char* const HEX = "0123456789abcdef";
        std::string s;
        for (S32 i = 0; i < 16; ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
            s += HEX[b[i] >> 4];
            s += HEX[b[i] & 0x0f];
        }
        return LLUUID(s);
    }

    void packPlaces(const LLSD& pack, const LLUUID& region, std::vector<WolfAdventure::GmArea>& areas, std::vector<WolfAdventure::GmPath>& paths)
    {
        // Source: ADVENTURE_PACK_V1.md §6 places (areas: circle centre/radius or polygon; paths: points [x,y,z]).
        areas.clear();
        paths.clear();
        const LLSD& places = pack["places"];
        const std::string rid = region.asString();
        const LLSD& as = places["areas"];
        for (size_t i = 0; as.isArray() && i < (size_t)as.size(); ++i)
        {
            const LLSD& a = as[(LLSD::Integer)i];
            if (a["region"].asString() != rid) continue;
            WolfAdventure::GmArea g;
            g.mId = cap(a["id"].asString(), 64);
            const LLSD& c = a["shape"]["circle"];
            if (c.isMap() && c["centre"].isArray() && c["centre"].size() >= 2)
            {
                g.mCircle = true;
                g.mCentre.set((F32)c["centre"][0].asReal(), (F32)c["centre"][1].asReal(), 0.f);
                g.mRadius = (F32)c["radius"].asReal();
            }
            else
            {
                const LLSD& poly = a["shape"]["polygon"];
                g.mCircle = false;
                for (size_t k = 0; poly.isArray() && k < (size_t)poly.size(); ++k)
                {
                    if (poly[(LLSD::Integer)k].isArray() && poly[(LLSD::Integer)k].size() >= 2)
                        g.mPoly.push_back(LLVector3((F32)poly[(LLSD::Integer)k][0].asReal(), (F32)poly[(LLSD::Integer)k][1].asReal(), 0.f));
                }
                if (g.mPoly.size() < 3) continue;
            }
            areas.push_back(g);
        }
        const LLSD& ps = places["paths"];
        for (size_t i = 0; ps.isArray() && i < (size_t)ps.size(); ++i)
        {
            const LLSD& pa = ps[(LLSD::Integer)i];
            if (pa["region"].asString() != rid) continue;
            WolfAdventure::GmPath g;
            g.mId = cap(pa["id"].asString(), 64);
            const LLSD& pts = pa["points"];
            for (size_t k = 0; pts.isArray() && k < (size_t)pts.size(); ++k)
            {
                const LLSD& pt = pts[(LLSD::Integer)k];
                if (pt.isArray() && pt.size() >= 2)
                    g.mPoints.push_back(LLVector3((F32)pt[0].asReal(), (F32)pt[1].asReal(), pt.size() >= 3 ? (F32)pt[2].asReal() : 0.f));
            }
            if (g.mPoints.size() >= 2) paths.push_back(g);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the pick tool
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfToolAdventurePick::WolfToolAdventurePick()
:   LLTool("WolfAdventurePick")
{
}

void WolfToolAdventurePick::start(pick_callback_t cb, bool keep)
{
    mCallback = cb;
    mKeep = keep;
    if (!mActive)
    {
        mActive = true;
        LLToolMgr::getInstance()->setTransientTool(this);
    }
}

void WolfToolAdventurePick::stop()
{
    if (!mActive) return;
    mActive = false;
    mCallback = nullptr;
    LLToolMgr::getInstance()->clearTransientTool();
}

bool WolfToolAdventurePick::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mActive) return false;
    // the land or an object under the pointer (llviewerwindow.h pickImmediate: LLPickInfo mPosGlobal)
    LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false);
    if (pick.mPickType == LLPickInfo::PICK_LAND || pick.mPickType == LLPickInfo::PICK_OBJECT)
    {
        pick_callback_t cb = mCallback;
        if (!mKeep) stop();
        if (cb) cb(pick.mPosGlobal);
    }
    return true;
}

bool WolfToolAdventurePick::handleMouseUp(S32 x, S32 y, MASK mask) { return mActive; }

bool WolfToolAdventurePick::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mActive) return false;
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool WolfToolAdventurePick::handleKey(KEY key, MASK mask)
{
    if (mActive && key == KEY_ESCAPE)   // STUDIO §6 2.1.1 "Escape cancels"
    {
        stop();
        return true;
    }
    return false;
}

void WolfToolAdventurePick::handleDeselect()
{
    mActive = false;
    mCallback = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the desk
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventureDeskView::WolfAdventureDeskView(const Params& p)
:   LLUICtrl(p)
{
}

void WolfAdventureDeskView::setStatus(const std::string& text, bool error)
{
    mStatus = text;
    mStatusError = error;
    mStatusUntil = error ? 0.0 : now() + STATUS_LIFE;
}

void WolfAdventureDeskView::call(const std::string& action, const LLSD& body, std::function<void(WolfAdventureDeskView*, const LLSD&)> on_ok,
                                 const std::string& doing)
{
    ++mBusy;
    if (!doing.empty()) setStatus(doing, false);
    LLHandle<LLUICtrl> handle = getHandle();
    WolfAdventure::api(action, body, [handle, on_ok](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureDeskView* self = static_cast<WolfAdventureDeskView*>(handle.get());
        self->mBusy = llmax(0, self->mBusy - 1);
        if (!ok)
        {
            self->setStatus(error, true);
            return;
        }
        if (on_ok) on_ok(self, reply);
    });
}

void WolfAdventureDeskView::ask(const std::string& title, const std::string& body, const std::string& yes, bool danger, std::function<void()> on_yes)
{
    mAsking = true;
    mAskTitle = title;
    mAskBody = body;
    mAskYes = yes;
    mAskDanger = danger;
    mAskYesFn = on_yes;
    mHits.setFocus(DH_NO, 0);   // the safe answer focused (§1)
}

void WolfAdventureDeskView::hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable, bool enabled)
{
    if (mScrollSlot >= 0)
    {
        const LLRect& c = mScrolls[mScrollSlot].mRect;
        l = llmax(l, (F32)c.mLeft);
        r = llmin(r, (F32)c.mRight);
        b = llmax(b, (F32)c.mBottom);
        t = llmin(t, (F32)c.mTop);
        if (r <= l || t <= b) return;
    }
    mHits.add(l, b, r, t, id, arg, tip, focusable, enabled);
}

U32 WolfAdventureDeskView::flagsFor(S32 id, S32 arg, bool enabled) const
{
    U32 f = 0;
    if (!enabled) f |= BTN_DISABLED;
    if (mHits.isHover(id, arg)) f |= BTN_HOVER;
    if (mHits.isPressed(id, arg)) f |= BTN_PRESSED;
    if (mKeyboardNav && hasFocus() && mHits.isFocused(id, arg)) f |= BTN_FOCUS;
    return f;
}

void WolfAdventureDeskView::btn(F32 l, F32 b, F32 r, F32 t, const std::string& label, S32 id, S32 arg, const std::string& tip,
                                const Palette& p, bool enabled, U32 extra)
{
    button(l, b, r, t, label, p, flagsFor(id, arg, enabled) | extra);
    hit(l, b, r, t, id, arg, tip, true, enabled);
}

F32 WolfAdventureDeskView::head(F32 l, F32 r, F32 top, const std::string& txt, const Palette& p)
{
    const LLFontGL* f = LLFontGL::getFontSansSerifSmallBold();
    const F32 lh = (F32)f->getLineHeight();
    textL(f, caps(txt), l, top, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    rect(l, top - lh - 3.f, r, top - lh - 2.f, fade(p.mAccent, 0.3f));
    return top - lh - 8.f;
}

F32 WolfAdventureDeskView::para(F32 l, F32 r, F32 top, const std::string& txt, const LLColor4& colour, bool small, S32 max_lines)
{
    const LLFontGL* f = small ? LLFontGL::getFontSansSerifSmall() : LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    F32 y = top;
    for (const std::string& line : wrap(f, txt, r - l, max_lines))
    {
        textL(f, line, l, y, colour);
        y -= lh;
    }
    return y;
}

void WolfAdventureDeskView::field(F32 l, F32 b, F32 r, F32 t, S32 which, const std::string& placeholder, const std::string& tip,
                                  const Palette& p, bool invalid)
{
    const bool focused = hasFocus() && mField == which;
    chamfer(l, b, r, t, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
    chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, 4.f, focused || invalid ? 2.f : 1.f, invalid ? DANGER_INK : (focused ? p.mAccent : p.mDim));
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    const std::string shown = fieldMasked(which) ? std::string(utf8str_to_wstring(mTexts[which]).size(), '*') : std::string();
    const std::string& txt = fieldMasked(which) ? shown : mTexts[which];
    const bool multi = fieldMultiline(which) && t - b > lh * 1.8f;
    F32 cx = l + 8.f, cy = (b + t) * 0.5f;
    if (txt.empty())
    {
        textL(f, placeholder, l + 8.f, multi ? t - 5.f : cy, p.mDim, LLFontGL::LEFT, multi ? LLFontGL::TOP : LLFontGL::VCENTER, r - l - 16.f);
        if (multi) cy = t - 5.f - lh * 0.5f;
    }
    else if (multi)
    {
        const S32 rows = llmax(1, (S32)((t - b - 8.f) / lh));
        const std::vector<std::string> lines = wrap(f, txt, r - l - 16.f, 0);
        const size_t first = lines.size() > (size_t)rows ? lines.size() - (size_t)rows : 0;   // the end stays in view
        F32 y = t - 5.f;
        for (size_t k = first; k < lines.size(); ++k)
        {
            cx = textL(f, lines[k], l + 8.f, y, p.mText);
            cy = y - lh * 0.5f;
            y -= lh;
        }
    }
    else
    {
        cx = textL(f, txt, l + 8.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 16.f);
    }
    if (focused && fmod(now(), 1.0) < 0.6) rect(cx + 1.f, cy - lh * 0.45f, cx + 2.f, cy + lh * 0.45f, p.mText);
    hit(l, b, r, t, DH_FIELD, which, tip);
}

F32 WolfAdventureDeskView::beginScroll(S32 slot, F32 l, F32 b, F32 r, F32 t)
{
    Scroll& s = mScrolls[slot];
    s.mRect = LLRect((S32)l, (S32)t, (S32)r, (S32)b);
    mScrollSlot = slot;
    mClipStack.push_back(slot);
    return t + s.mOffset;
}

void WolfAdventureDeskView::endScroll(S32 slot, F32 low, const Palette& p)
{
    Scroll& s = mScrolls[slot];
    const F32 t = (F32)s.mRect.mTop, b = (F32)s.mRect.mBottom, r = (F32)s.mRect.mRight;
    s.mContent = t + s.mOffset - low + 8.f;
    const F32 view_h = t - b, max_scroll = llmax(0.f, s.mContent - view_h);
    s.mOffset = llclamp(s.mOffset, 0.f, max_scroll);
    mClipStack.pop_back();
    mScrollSlot = mClipStack.empty() ? -1 : mClipStack.back();
    if (max_scroll > 0.f)
    {
        const F32 th = llmax(24.f, view_h * view_h / s.mContent), tyy = t - (view_h - th) * (s.mOffset / max_scroll);
        rect(r - 5.f, b, r, t, fade(p.mTrack, 0.8f));
        rect(r - 5.f, tyy - th, r, tyy, mDragSlot == slot ? p.mAccent : fade(p.mDim, 0.8f));
        mHits.add(r - 9.f, b, r + 2.f, t, DH_SCROLL, slot, "Drag to scroll (or the mouse wheel)", false);
    }
}

void WolfAdventureDeskView::draw()
{
    tick();
    mHits.clear();
    mScrollSlot = -1;
    mClipStack.clear();
    const Palette& p = gamePalette();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    LLGLSUIDefault gls_ui;
    frame(0.f, 0.f, W, H, p, 1.f, 14.f);
    const F32 M = 14.f;
    F32 x = M + 2.f;
    const F32 ty = H - 10.f;
    crescent(x + 7.f, ty - 9.f, 7.f, p.mText, p.mBaseTop);
    x = textL(bold, titleWord(), x + 20.f, ty - 2.f, p.mDim) + 10.f;
    textL(display(17.f), titleText(), x, ty, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - x - 60.f);
    {
        const F32 xc = W - M - 8.f, yc = ty - 9.f;
        closeX(xc, yc, 5.f, mHits.isHover(DH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(DH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
        hit(xc - 11.f, yc - 11.f, xc + 11.f, yc + 11.f, DH_CLOSE, 0, "Close (Escape)");
    }
    // the status line along the foot: a confirmation fades, an error stays (§1 Feedback); an icon AND words
    F32 bottom = 10.f;
    if (!mStatus.empty() && mStatusUntil > 0.0 && now() > mStatusUntil) mStatus.clear();
    if (!mStatus.empty() || mBusy > 0)
    {
        const F32 sh = (F32)small->getLineHeight() + 8.f;
        card(M, bottom, W - M, bottom + sh, p, 4.f);
        const F32 cy = bottom + sh * 0.5f;
        if (mStatusError)
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(WARN_INK.mV);
            gGL.vertex2f(M + 6.f, cy - 5.f); gGL.vertex2f(M + 18.f, cy - 5.f); gGL.vertex2f(M + 12.f, cy + 6.f);
            gGL.end();
        }
        else if (!mStatus.empty())
        {
            lineW(M + 7.f, cy, M + 10.f, cy - 3.f, 2.f, OK_INK);
            lineW(M + 10.f, cy - 3.f, M + 16.f, cy + 4.f, 2.f, OK_INK);
        }
        textL(small, mStatus.empty() ? std::string("Working...") : mStatus, M + 24.f, cy, mStatusError ? DANGER_INK : p.mText,
              LLFontGL::LEFT, LLFontGL::VCENTER, W - 2.f * M - 30.f);
        bottom += sh + 6.f;
    }
    face(W, H, H - 40.f, bottom, p);
    if (mAsking) drawQuestion(p);
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureDeskView::drawQuestion(const Palette& p)
{
    // Everything under it stops taking clicks: only the question counts (the Studio's way).
    mHits.clear();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    rect(0.f, 0.f, W, H, LLColor4(0.f, 0.f, 0.f, 0.55f));
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const F32 w = llmin(460.f, W - 40.f);
    const std::vector<std::string> lines = wrap(body, mAskBody, w - 32.f, 8);
    const F32 h = 24.f + (F32)display(18.f)->getLineHeight() + 8.f + (F32)lines.size() * (F32)body->getLineHeight() + 16.f + 32.f + 16.f;
    const F32 l = (W - w) * 0.5f, b = (H - h) * 0.5f, r = l + w, t = b + h;
    frame(l, b, r, t, p, 1.f, 10.f);
    F32 y = t - 16.f;
    textL(display(18.f), mAskTitle, l + 16.f, y, mAskDanger ? DANGER_INK : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, w - 32.f);
    y -= (F32)display(18.f)->getLineHeight() + 8.f;
    for (const std::string& line : lines)
    {
        textL(body, line, l + 16.f, y, p.mText);
        y -= (F32)body->getLineHeight();
    }
    // Fitts: the destructive button on the right, the safe one on the left, apart (NN/G proximity of options)
    const F32 bb = b + 16.f, bt = bb + 32.f;
    btn(l + 16.f, bb, l + 140.f, bt, "Cancel", DH_NO, 0, "Keep things as they are (Escape)", p);
    btn(r - 16.f - 150.f, bb, r - 16.f, bt, mAskYes, DH_YES, 0, mAskYes, p, true, mAskDanger ? BTN_DANGER : BTN_PRIMARY);
}

bool WolfAdventureDeskView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h)
    {
        if (mAsking) return true;   // the question is modal to this window
        mField = 0;
        return false;
    }
    setFocus(true);
    mKeyboardNav = false;
    mField = h->mId == DH_FIELD ? h->mArg : 0;
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    if (h->mId == DH_SCROLL)
    {
        mDragSlot = h->mArg;
        mDragGrabY = (F32)y;
        mDragStart = mScrolls[h->mArg].mOffset;
    }
    return true;
}

bool WolfAdventureDeskView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    const S32 pid = mHits.pressedId(), parg = mHits.pressedArg();
    const bool was_drag = mDragSlot >= 0;
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    mDragSlot = -1;
    if (was_drag) return true;
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mId != pid || h->mArg != parg || !h->mEnabled) return true;
    if (pid == DH_CLOSE) { escape(); return true; }
    if (pid == DH_FIELD) return true;
    if (pid == DH_YES || pid == DH_NO)
    {
        std::function<void()> fn = mAskYesFn;
        mAsking = false;
        mAskYesFn = nullptr;
        if (pid == DH_YES && fn) fn();
        return true;
    }
    if (mStatusError) setStatus(std::string(), false);   // the next action clears an error (§1)
    press(pid, parg);
    return true;
}

void WolfAdventureDeskView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
    mDragSlot = -1;
}

bool WolfAdventureDeskView::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (mDragSlot >= 0)
        {
            Scroll& s = mScrolls[mDragSlot];
            const F32 view_h = (F32)s.mRect.getHeight();
            const F32 max_scroll = llmax(0.f, s.mContent - view_h);
            const F32 th = llmax(24.f, view_h * view_h / llmax(1.f, s.mContent));
            const F32 per_px = (view_h - th) > 1.f ? max_scroll / (view_h - th) : 0.f;
            s.mOffset = llclamp(mDragStart + (mDragGrabY - (F32)y) * per_px, 0.f, max_scroll);
        }
        gViewerWindow->setCursor(UI_CURSOR_HAND);
        return true;
    }
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(h->mId == DH_FIELD ? UI_CURSOR_IBEAM : (h->mEnabled && h->mFocusable ? UI_CURSOR_HAND : UI_CURSOR_ARROW));
    return true;
}

bool WolfAdventureDeskView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mAsking) return true;
    for (auto& kv : mScrolls)
    {
        if (kv.second.mRect.pointInRect(x, y))
        {
            kv.second.mOffset = llmax(0.f, kv.second.mOffset + (F32)clicks * 40.f);
            return true;
        }
    }
    return false;
}

bool WolfAdventureDeskView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

void WolfAdventureDeskView::escape()
{
    if (mAsking)
    {
        mAsking = false;
        mAskYesFn = nullptr;
        return;
    }
    if (mField != 0)
    {
        mField = 0;
        return;
    }
    if (LLFloater* f = getParentByType<LLFloater>()) f->closeFloater();
}

bool WolfAdventureDeskView::handleKeyHere(KEY key, MASK mask)
{
    if (mField != 0 && !mAsking)
    {
        std::string& txt = mTexts[mField];
        if (key == KEY_BACKSPACE && mask == MASK_NONE)
        {
            LLWString w = utf8str_to_wstring(txt);
            if (!w.empty()) w.pop_back();
            txt = wstring_to_utf8str(w);
            return true;
        }
        if (key == KEY_RETURN && (mask == MASK_NONE || mask == MASK_SHIFT))
        {
            if (fieldMultiline(mField) && mask == MASK_SHIFT)
            {
                if (utf8str_to_wstring(txt).size() < fieldCap(mField)) txt += "\n";
            }
            else
            {
                fieldEnter(mField);
            }
            return true;
        }
        if (key == 'V' && (mask & MASK_CONTROL))
        {
            LLWString pasted;
            if (LLClipboard::instance().pasteFromClipboard(pasted)) txt = WolfGame::cleanText(txt + wstring_to_utf8str(pasted), fieldCap(mField), fieldMultiline(mField));
            return true;
        }
    }
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        const WolfGameHits::Hit* f = mHits.focused();
        mField = f && f->mId == DH_FIELD ? f->mArg : 0;
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_ESCAPE:
        escape();
        return true;
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled && f->mFocusable)
        {
            mKeyboardNav = true;
            if (f->mId == DH_YES || f->mId == DH_NO || f->mId == DH_CLOSE)
            {
                if (f->mId == DH_CLOSE) { escape(); return true; }
                std::function<void()> fn = mAskYesFn;
                const bool yes = f->mId == DH_YES;
                mAsking = false;
                mAskYesFn = nullptr;
                if (yes && fn) fn();
                return true;
            }
            press(f->mId, f->mArg);
        }
        return true;
    }
    case KEY_LEFT:
    case KEY_RIGHT:
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_RIGHT);
        return true;
    default:
        return false;
    }
}

bool WolfAdventureDeskView::handleUnicodeCharHere(llwchar uni_char)
{
    if (mField != 0 && !mAsking)
    {
        if (uni_char < 0x20 || uni_char == 0x7f) return true;
        std::string& txt = mTexts[mField];
        LLWString w = utf8str_to_wstring(txt);
        if (w.size() < fieldCap(mField)) w.push_back(uni_char);
        txt = wstring_to_utf8str(w);
        return true;
    }
    if (uni_char != ' ') return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (f && f->mEnabled && f->mFocusable)
    {
        mKeyboardNav = true;
        if (f->mId == DH_CLOSE) escape();
        else if (f->mId != DH_YES && f->mId != DH_NO) press(f->mId, f->mArg);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the GM Console: data
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventureGmView::WolfAdventureGmView(const Params& p)
:   WolfAdventureDeskView(p)
{
    text(F_REWARD_QTY) = "1";
}

std::string WolfAdventureGmView::titleText() const
{
    const std::string a = selectedAdventure();
    for (size_t i = 0; mAdvs.isArray() && i < (size_t)mAdvs.size(); ++i)
    {
        if (mAdvs[(LLSD::Integer)i]["id"].asString() == a) return cap(mAdvs[(LLSD::Integer)i]["title"].asString(), 64);
    }
    return "No adventure";
}

size_t WolfAdventureGmView::fieldCap(S32 f) const
{
    // Source: HUD_MESSAGES_V1.md §1.3 caps (vignette text 2000, titles 64, labels 24) and §1.2 (gm_say 720 bytes,
    // counted here in characters and cut to bytes on sending); quiz/check ids 64.
    switch (f)
    {
    case F_NARRATE_TEXT: return 2000;
    case F_NARRATE_TITLE: return 64;
    case F_NARRATE_IMAGE: return 36;
    case F_PS_SAY: return 500;
    case F_PS_ANIM: return 64;
    case F_WHISPER: return 500;
    case F_REASON: return 64;          // wolfadventure_gm_lib.php rep/honour: why cut to 64
    case F_MUSIC: return 1024;
    case F_REWARD_XP: case F_REWARD_MONEY: case F_REWARD_QTY: case F_DC: return 9;
    case F_REWARD_ITEM: return 24;     // wag_reward items: ^[a-z0-9_]{1,24}$
    case F_CHECK: case F_FLAG_TEXT: return 64;
    case F_ASSIST_TEXT: return 500;
    default: return f >= F_OPT0 ? 24 : 240;
    }
}

bool WolfAdventureGmView::fieldMultiline(S32 f) const
{
    return f == F_NARRATE_TEXT || f == F_WHISPER;
}

std::string WolfAdventureGmView::selectedAdventure() const
{
    if (!mAdvs.isArray() || mAdvs.size() == 0) return std::string();
    return mAdvs[(LLSD::Integer)llclamp(mAdvSel, 0, (S32)mAdvs.size() - 1)]["id"].asString();
}

const LLSD* WolfAdventureGmView::selectedParty() const
{
    const LLSD& ps = mState["parties"];
    for (size_t i = 0; ps.isArray() && i < (size_t)ps.size(); ++i)
    {
        if (ps[(LLSD::Integer)i]["party"].asString() == mParty) return &ps[(LLSD::Integer)i];
    }
    return nullptr;
}

const LLSD& WolfAdventureGmView::packList(const char* key) const
{
    static const LLSD EMPTY = LLSD::emptyArray();
    const LLSD& v = mPack[key];
    return v.isArray() ? v : EMPTY;
}

const LLSD* WolfAdventureGmView::packBeat(const std::string& id, std::string* scene_out) const
{
    const LLSD& acts = packList("acts");
    for (size_t a = 0; a < (size_t)acts.size(); ++a)
    {
        const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s)
        {
            const LLSD& beats = scenes[(LLSD::Integer)s]["beats"];
            for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b)
            {
                if (beats[(LLSD::Integer)b]["id"].asString() == id)
                {
                    if (scene_out) *scene_out = scenes[(LLSD::Integer)s]["id"].asString();
                    return &beats[(LLSD::Integer)b];
                }
            }
        }
    }
    return nullptr;
}

std::string WolfAdventureGmView::castName(const std::string& id) const
{
    const LLSD& cast = packList("cast");
    for (size_t i = 0; i < (size_t)cast.size(); ++i)
    {
        if (cast[(LLSD::Integer)i]["id"].asString() == id) return cap(cast[(LLSD::Integer)i]["first"].asString() + " " + cast[(LLSD::Integer)i]["last"].asString(), 64);
    }
    return id;
}

std::string WolfAdventureGmView::partyRegion() const
{
    const LLSD* party = selectedParty();
    if (!party) return std::string();
    const std::string scene = (*party)["scene"].asString();
    const LLSD& acts = packList("acts");
    for (size_t a = 0; a < (size_t)acts.size(); ++a)
    {
        const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s)
        {
            if (scenes[(LLSD::Integer)s]["id"].asString() == scene) return scenes[(LLSD::Integer)s]["region"].asString();
        }
    }
    return std::string();
}

void WolfAdventureGmView::onOpened(const LLSD& key)
{
    mOverlayDirty = true;
    loadAdventures();
}

void WolfAdventureGmView::onClosed()
{
    // §3 Overlays "on only while the Console is open"; the director camera and any pick end with it too.
    WolfToolAdventurePick::instance().stop();
    if (mDirCam) setDirectorCamera(false);
    mClickMove = false;
}

void WolfAdventureGmView::loadAdventures()
{
    // BRAIN_API_V1.md §5.1 list_mine: the caller's own adventures, plus those where they hold a grant (with role).
    call("list_mine", LLSD::emptyMap(), [](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
        self->mAdvs = LLSD::emptyArray();
        const LLSD& list = reply["adventures"];
        for (size_t i = 0; list.isArray() && i < (size_t)list.size(); ++i)
        {
            const LLSD& a = list[(LLSD::Integer)i];
            // only those that can be running: published (a draft has no session but its owner's Test Play)
            if (a["status"].asString() == "published") self->mAdvs.append(a);
        }
        self->mAdvSel = 0;
        self->mOverlayDirty = true;
        if (self->mAdvs.size() == 0) self->setStatus("No published adventure where you are the owner or a host.", false);
        self->loadSessions();
    }, "Loading your adventures...");
}

void WolfAdventureGmView::loadSessions()
{
    const std::string adv = selectedAdventure();
    mSessions = LLSD();
    mSession.clear();
    mState = LLSD();
    mParty.clear();
    mOverlayDirty = true;
    if (adv.empty()) return;
    LLSD body;
    body["adventure"] = adv;
    // BRAIN_API_V1.md §5.5 sessions (P2 GM Console Lite) - the open sessions the caller may GM.
    call("sessions", body, [](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
        self->mSessions = reply["sessions"].isArray() ? reply["sessions"] : LLSD::emptyArray();
        if (self->mSessions.size() > 0) self->mSession = self->mSessions[0]["session"].asString();
        // [WOLF ADVENTURE AUDIT 2026-10-17] never silent: none running says what to do
        else self->setStatus("No sessions are running: press Start a session, or wait for a player to start the story.", false);
        self->mOverlayDirty = true;
        self->pollState(true);
    }, "Loading the sessions...");
}

void WolfAdventureGmView::pollState(bool force)
{
    if (mSession.empty() || mPolling) return;
    if (!force && now() < mPollAt) return;
    mPolling = true;
    mPollAt = now() + POLL_SEC;
    LLSD body;
    body["session"] = mSession;
    const std::string sess = mSession;
    LLHandle<LLUICtrl> handle = getHandle();
    // BRAIN_API_V1.md §8.2 gm_state - polled every 2 s while the console is open (STUDIO §3).
    WolfAdventure::api("gm_state", body, [handle, sess](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(handle.get());
        self->mPolling = false;
        if (sess != self->mSession) return;
        if (!ok)
        {
            self->mStateError = error;
            return;
        }
        self->mStateError.clear();
        self->mState = reply;
        self->mOverlayDirty = true;
        if (self->mParty.empty() || !self->selectedParty())
        {
            const LLSD& ps = reply["parties"];
            self->mParty = ps.isArray() && ps.size() > 0 ? ps[0]["party"].asString() : std::string();
        }
        self->loadPack();
    });
}

void WolfAdventureGmView::loadPack()
{
    const std::string adv = selectedAdventure();
    const S32 rev = mState["session"]["revision"].asInteger();
    const std::string key = adv + ":" + llformat("%d", rev);
    if (adv.empty() || rev <= 0 || key == mPackFor || key == mPackFailed || mPackLoading) return;
    mPackLoading = true;
    LLSD body;
    body["adventure"] = adv;
    body["revision"] = rev;
    LLHandle<LLUICtrl> handle = getHandle();
    // BRAIN_API_V1.md §5.1 get_revision {adventure, revision} (owner, GM): the session's pack, for the
    // beats, triggers, flags, cast, encounters, spots, endings and parcels the tabs list. A refusal is
    // remembered for this revision and not asked again: a GM granted on one region only (BRAIN §5.3) may not read
    // the pack, and the console works without its lists (the tabs say so; no error stays up).
    WolfAdventure::api("get_revision", body, [handle, key](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead()) return;
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(handle.get());
        self->mPackLoading = false;
        if (!ok)
        {
            self->mPackFailed = key;
            return;
        }
        self->mPack = reply["pack"];
        self->mPackFor = key;
        self->mOverlayDirty = true;
    });
}

void WolfAdventureGmView::loadLogOlder()
{
    const LLSD& base = mOlderLog.size() > 0 ? mOlderLog : mState["log"];
    S64 oldest = 0;
    for (size_t i = 0; base.isArray() && i < (size_t)base.size(); ++i)
    {
        const S64 id = base[(LLSD::Integer)i]["id"].asInteger();
        if (oldest == 0 || id < oldest) oldest = id;
    }
    LLSD body;
    body["session"] = mSession;
    body["limit"] = 50;
    if (oldest > 0) body["before"] = (LLSD::Integer)oldest;
    // BRAIN_API_V1.md §8.2 gm_log {session, before?, limit}
    call("gm_log", body, [](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
        if (!self->mOlderLog.isArray()) self->mOlderLog = LLSD::emptyArray();
        const LLSD& log = reply["log"];
        for (size_t i = 0; log.isArray() && i < (size_t)log.size(); ++i) self->mOlderLog.append(log[(LLSD::Integer)i]);
        if (!log.isArray() || log.size() == 0) self->setStatus("No older log entries.", false);
    }, "Loading older log entries...");
}

void WolfAdventureGmView::loadConversations()
{
    LLSD body;
    body["session"] = mSession;
    // BRAIN_API_V1.md §10.3 chatlog {session, ...}: names while running it and for 7 days after.
    call("chatlog", body, [](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
        self->mConvo = reply;   // {lines, more}
    }, "Loading conversations...");
}

void WolfAdventureGmView::gmOp(const std::string& op, const LLSD& fields, const std::string& doing, std::function<void(WolfAdventureGmView*, const LLSD&)> then)
{
    if (mSession.empty())
    {
        setStatus("Choose a session first.", true);
        return;
    }
    LLSD body = fields.isMap() ? fields : LLSD::emptyMap();
    body["session"] = mSession;
    body["op"] = op;
    // BRAIN_API_V1.md §8.1: {"action":"gm","session","op", ...}; may_gm checked on every call; the reply is {log}.
    call("gm", body, [then, doing](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
        if (!doing.empty()) self->setStatus("Done: " + doing, false);
        self->pollState(true);
        if (then) then(self, reply);
    }, doing.empty() ? std::string() : doing + "...");
}

void WolfAdventureGmView::pick(const std::string& what, std::function<void(WolfAdventureGmView*, const LLVector3d&)> then)
{
    mPickWhat = what;
    setStatus("Click the ground in the world to " + what + ". Escape cancels.", false);
    mStatusUntil = 0.0;   // stays up until the click
    LLHandle<LLUICtrl> handle = getHandle();
    WolfToolAdventurePick::instance().start([handle, then](const LLVector3d& global)
    {
        if (handle.isDead()) return;
        WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(handle.get());
        self->mPickWhat.clear();
        self->setStatus(std::string(), false);
        if (then) then(self, global);
    }, false);
}

void WolfAdventureGmView::stopPossessing()
{
    if (mClickMove) WolfToolAdventurePick::instance().stop();
    mClickMove = false;
    mPossessHandle.clear();
    mPossessName.clear();
    mPossessCast.clear();
    mPsLines = LLSD();
    mPsSuggest = LLSD();
}

void WolfAdventureGmView::setDirectorCamera(bool on)
{
    // Source: STUDIO_AND_CONSOLE_V1.md §3 "Director camera unlocks the viewer's camera from the avatar, using the
    // viewer's existing free-camera controls with no distance limit": the camera leaves the avatar
    // (llagentcamera.h setFocusOnAvatar) and the limit is lifted as DisableCameraConstraints lifts it, but for this
    // session only (llagentcamera.h setConstraintsLiftedForSession): that setting is persisted (settings.xml), so
    // changing it would leave the user's constraints off for good if the viewer crashed with the camera on.
    if (on == mDirCam) return;
    mDirCam = on;
    gAgentCamera.setConstraintsLiftedForSession(on);
    if (on)
    {
        gAgentCamera.setFocusOnAvatar(false, false);
        setStatus("Director camera on: Alt-drag to move the camera anywhere. Turn it off or close the console to come back.", false);
    }
    else
    {
        gAgentCamera.setFocusOnAvatar(true, true);
        setStatus("Director camera off.", false);
    }
}

void WolfAdventureGmView::tick()
{
    if (!mSession.empty()) pollState(false);
    // the possession plate's last player lines (STUDIO §3 "The last 5 player lines to this character")
    if (!mPossessCast.empty() && now() > mPsLinesAt)
    {
        mPsLinesAt = now() + 5.0;
        LLSD body;
        body["session"] = mSession;
        body["cast"] = mPossessCast;
        const std::string cast = mPossessCast;
        LLHandle<LLUICtrl> handle = getHandle();
        WolfAdventure::api("chatlog", body, [handle, cast](bool ok, const LLSD& reply, const std::string& error)
        {
            if (handle.isDead() || !ok) return;
            WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(handle.get());
            if (self->mPossessCast == cast) self->mPsLines = reply["lines"];
        });
    }
    // The overlay is rebuilt (packPlaces, npcList) only when what it shows changed: the console's own state
    // (mOverlayDirty), the region, or WolfAdventure's gm layers (its generation goes up with every message,
    // wolfadventure.cpp apply). Otherwise it is only kept alive: drawGmOverlay stops a second after the last.
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLUUID region = rgn ? rgn->getRegionID() : LLUUID::null;
    const U32 gen = WolfAdventure::instance().generation();
    if (mOverlayDirty || region != mOverlayRegion || gen != mOverlayGen)
    {
        mOverlayDirty = false;
        mOverlayRegion = region;
        mOverlayGen = gen;
        publishOverlay();
    }
    else
    {
        WolfAdventureUI::keepGmOverlay();
    }
}

void WolfAdventureGmView::publishOverlay()
{
    WolfAdventureUI::GmOverlay o;
    o.mShowZones = mOvZones;
    o.mShowPaths = mOvPaths;
    o.mShowNpcs = mOvNpcs;
    o.mAdv = selectedAdventure();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn)
    {
        o.mRegion = rgn->getRegionID();
        WolfAdventureDesk::packPlaces(mPack, o.mRegion, o.mAreas, o.mPaths);
        const LLSD npcs = npcList();
        for (size_t i = 0; i < (size_t)npcs.size(); ++i)
        {
            const LLSD& n = npcs[(LLSD::Integer)i];
            WolfAdventure::GmNpc g;
            if (n.has("region") && LLUUID(n["region"].asString()) != o.mRegion) continue;   // only those on this region
            g.mHandle = cap(n["handle"].asString(), 128);
            g.mNpc = LLUUID::validate(n["npc"].asString()) ? LLUUID(n["npc"].asString()) : WolfAdventureDesk::npcAgent(o.mRegion, g.mHandle);
            g.mName = n["name"].asString();
            g.mPossessedBy = cap(n["possessedBy"].asString(), 36);
            o.mNpcs.push_back(g);
        }
    }
    WolfAdventureUI::setGmOverlay(o);
}

LLSD WolfAdventureGmView::npcList() const
{
    LLSD out = LLSD::emptyArray();
    const LLSD& npcs = mState["npcs"];
    if (npcs.isArray())
    {
        for (size_t i = 0; i < (size_t)npcs.size(); ++i)
        {
            // Source: wolfmind_lib.php gm_state npcs {handle, cast, name, region, regionName, npc, pos, possessedBy}
            LLSD n = npcs[(LLSD::Integer)i];
            if (n["name"].asString().empty()) n["name"] = castName(n["cast"].asString());
            out.append(n);
        }
        return out;
    }
    // Source: wolfadventure_gm_lib.php wag_overlay: the npcs layer {npc, handle, name, possessedBy}; the cast id
    // is the handle's second part ("<adventure>:<cast>:<n>", explode(':', $h)[1] there).
    const std::map<std::string, WolfAdventure::GmLayers>& layers = WolfAdventure::instance().gmLayers();
    std::map<std::string, WolfAdventure::GmLayers>::const_iterator it = layers.find(selectedAdventure());
    if (it == layers.end() || it->second.mSession != mSession) return out;
    for (const WolfAdventure::GmNpc& g : it->second.mNpcs)
    {
        LLSD n;
        n["handle"] = g.mHandle;
        const size_t a = g.mHandle.find(':'), b = a == std::string::npos ? a : g.mHandle.find(':', a + 1);
        n["cast"] = a == std::string::npos ? std::string() : g.mHandle.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        n["name"] = g.mName.empty() ? castName(n["cast"].asString()) : g.mName;
        n["possessedBy"] = g.mPossessedBy;
        out.append(n);
    }
    return out;
}

namespace
{
    // One "key: value" of a log row's text (wolfmind_lib.php wm_log_row joins the event data as "k: v, k: v").
    std::string logField(const std::string& text, const std::string& key)
    {
        for (const char* lead : { " - ", ", " })
        {
            const std::string k = std::string(lead) + key + ": ";
            const size_t at = text.find(k);
            if (at == std::string::npos) continue;
            const size_t from = at + k.size(), to = text.find(", ", from);
            return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
        }
        return std::string();
    }
}

LLSD WolfAdventureGmView::rollList() const
{
    const LLSD& rolls = mState["rolls"];
    if (rolls.isArray())
    {
        // Source: wolfmind_lib.php gm_state rolls {roll, agent, name, check, total, dc, outcome, secret, at}
        LLSD out = LLSD::emptyArray();
        for (size_t i = 0; i < (size_t)rolls.size(); ++i)
        {
            LLSD r = rolls[(LLSD::Integer)i];
            r["id"] = r["roll"].asString();
            if (r["secret"].asInteger() != 0) r["label"] = r["check"].asString() + " (secret)";
            out.append(r);
        }
        return out;
    }
    // Source: wolfadventure_lib.php doRoll / wolfadventure_gm_lib.php wag_roll log `dice` with data
    // {total, dc, outcome, roll, check, ...}; wag_override logs `override` {roll, from, to}.
    LLSD out = LLSD::emptyArray();
    const LLSD& log = mState["log"];
    for (size_t i = 0; log.isArray() && i < (size_t)log.size() && out.size() < 10; ++i)
    {
        const LLSD& e = log[(LLSD::Integer)i];
        if (e["type"].asString() != "dice") continue;
        const std::string text = e["text"].asString(), id = logField(text, "roll");
        if (!LLUUID::validate(id)) continue;
        LLSD r;
        r["id"] = id;
        r["name"] = e["name"].asString();
        r["check"] = logField(text, "check");
        r["total"] = atoi(logField(text, "total").c_str());
        r["outcome"] = logField(text, "outcome");
        out.append(r);
    }
    return out;
}

void WolfAdventureGmView::requestLayers()
{
    if (mSession.empty()) return;
    // Source: wolfadventure_gm_lib.php WAG_OPS 'overlay' (settled in P4) and wag_overlay: the gm HUD layers of the
    // region the GM stands on, sent to the GM alone.
    gmOp("overlay", LLSD::emptyMap(), std::string());
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the GM Console: drawing

// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureGmView::rowText(F32 l, F32 r, F32 y, const std::string& left, const LLColor4& c)
{
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    textL(f, left, l, y - ROW_H * 0.5f, c, LLFontGL::LEFT, LLFontGL::VCENTER, r - l);
    return y - ROW_H;
}

void WolfAdventureGmView::face(F32 W, F32 H, F32 top, F32 bottom, const Palette& p)
{
    const F32 M = 14.f;
    F32 y = drawTopBar(M, W - M, top, p);
    // the left: the parties; the right drawer: the log; the centre: the tabs
    const F32 left_r = M + 210.f, drawer_l = W - M - 270.f;
    drawParties(M, bottom, left_r, y, p);
    drawLog(drawer_l, bottom, W - M, y, p);
    const F32 l = left_r + 12.f, r = drawer_l - 12.f;
    // the tab strip
    static const char* NAMES[T_COUNT] = { "Story", "Cast", "Narrate", "Party", "Rewards", "World", "Rules", "Assist" };
    static const char* TIPS[T_COUNT] = {
        "The beat: advance, go to a beat or scene, fire triggers, flags, reputation and honour",
        "The characters present: possess, release, remove, move; place cast and fire encounters",
        "Send a narration card to the party or a player, or whisper to one player",
        "Call the party to a spot, a ready check, end for the party, remove a player",
        "XP, money, items or a parcel to a player or the party",
        "Time of day, weather and music",
        "Dice, checks, turn order and overriding a roll",
        "Ideas from the AI, for you only" };
    const F32 tw = (r - l) / (F32)T_COUNT;
    for (S32 i = 0; i < T_COUNT; ++i)
    {
        const F32 tl = l + tw * (F32)i;
        btn(tl, y - 28.f, tl + tw - 4.f, y, NAMES[i], H_TAB, i, TIPS[i], p, true, mTab == i ? BTN_ACTIVE : 0);
    }
    y -= 36.f;
    F32 t = beginScroll(1 + (S32)mTab, l, bottom, r, y);
    {
        LLLocalClipRect clip(LLRect((S32)l, (S32)y, (S32)r, (S32)bottom));
        if (!mPossessHandle.empty()) t = drawPossession(l, r - 10.f, t, p) - 10.f;
        if (mSession.empty())
        {
            t = para(l, r - 10.f, t, mAdvs.isArray() && mAdvs.size() > 0
                     ? "No open session for this adventure. A session opens by itself when the first player joins; you can also start a scheduled one with the button above."
                     : "Nothing to run yet. Publish an adventure in the Adventure Studio, or ask its owner to make you a host (Advanced editing > Publish, \"Hosts and teachers\").", p.mDim);
        }
        else if (!mStateError.empty())
        {
            t = para(l, r - 10.f, t, "The session could not be read: " + mStateError, DANGER_INK);
        }
        else
        {
            switch (mTab)
            {
            case T_STORY: t = tabStory(l, r - 10.f, t, p); break;
            case T_CAST: t = tabCast(l, r - 10.f, t, p); break;
            case T_NARRATE: t = tabNarrate(l, r - 10.f, t, p); break;
            case T_PARTY: t = tabParty(l, r - 10.f, t, p); break;
            case T_REWARDS: t = tabRewards(l, r - 10.f, t, p); break;
            case T_WORLD: t = tabWorld(l, r - 10.f, t, p); break;
            case T_RULES: t = tabRules(l, r - 10.f, t, p); break;
            default: t = tabAssist(l, r - 10.f, t, p); break;
            }
        }
    }
    endScroll(1 + (S32)mTab, t, p);
}

F32 WolfAdventureGmView::drawTopBar(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    F32 y = top;
    const bool idle = mBusy == 0;
    // row 1: the adventure and the session (§3 "Session picker (the auto session, scheduled ones, classes; the region)")
    {
        std::string adv = "Adventure: " + titleText();
        if (mAdvs.isArray() && mAdvs.size() > 1) adv += llformat("  (%d of %d)", mAdvSel + 1, (S32)mAdvs.size());
        btn(l, y - 28.f, l + 300.f, y, adv, H_ADV, 0, "The next of your adventures (owner or host)", p, mAdvs.isArray() && mAdvs.size() > 1);
        std::string sw = "Session: none open";
        S32 si = -1;
        for (size_t i = 0; mSessions.isArray() && i < (size_t)mSessions.size(); ++i)
        {
            if (mSessions[(LLSD::Integer)i]["session"].asString() == mSession) si = (S32)i;
        }
        if (si >= 0)
        {
            const LLSD& s = mSessions[si];
            const std::string kind = s["kind"].asString();
            sw = "Session: " + std::string(kind == "scheduled" ? "Scheduled" : (kind == "class" ? "Class" : "Auto"))
               + llformat(" \xC2\xB7 %d players", s["players"].asInteger());
            if (mSessions.size() > 1) sw += llformat("  (%d of %d)", si + 1, (S32)mSessions.size());
        }
        btn(l + 306.f, y - 28.f, l + 600.f, y, sw, H_SESSION, 0, "The next open session of this adventure", p, mSessions.isArray() && mSessions.size() > 1);
        btn(l + 606.f, y - 28.f, l + 690.f, y, "Refresh", H_REFRESH, 0, "Read your adventures and sessions again", p, idle);
        btn(l + 696.f, y - 28.f, l + 860.f, y, "Start a session", H_SCHEDULE, 0, "Open a session now that you run yourself", p, idle && !selectedAdventure().empty());
        if (r - (l + 866.f) > 90.f) btn(l + 866.f, y - 28.f, l + 960.f, y, "End session", H_SESSION_END, 0, "End this scheduled or class session for every party (asks first); the auto session runs while the adventure is published", p,
                 idle && !mSession.empty() && static_cast<const LLSD&>(mState)["session"]["kind"].asString() != "auto", BTN_DANGER);
    }
    y -= 34.f;
    // row 2: control state as WORDS, Take control / Hand back, Pause / Resume, AI on, the meter, overlays, camera
    {
        const LLSD& sess = static_cast<const LLSD&>(mState)["session"];
        const bool have = !mSession.empty() && sess.isMap();
        const std::string ctl = have ? controlWords(sess) : std::string("No session");
        textL(display(16.f), ctl, l, y - 14.f, have && sess["control"].asString() == "live" ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, 190.f);
        F32 x = l + 196.f;
        const bool mine = have && sess["control"].asString() == "live" && LLUUID(sess["gm"].asString()) == gAgentID;
        if (mine) btn(x, y - 28.f, x + 110.f, y, "Hand back", H_LEAVE, 0, "Give the story back to the AI Director; it carries on from exactly here", p, have);
        else btn(x, y - 28.f, x + 110.f, y, "Take control", H_TAKE, 0, "Run it yourself: the AI Director stands back (asks first)", p, have, BTN_PRIMARY);
        x += 116.f;
        const bool paused = have && sess["control"].asString() == "paused";
        if (paused) btn(x, y - 28.f, x + 90.f, y, "Resume", H_RESUME, 0, "Resume timers, hints and the Director for every party", p, have);
        else btn(x, y - 28.f, x + 90.f, y, "Pause", H_PAUSE, 0, "Pause timers, hints and the Director; players see that it is paused", p, have);
        x += 96.f;
        const bool ai_on = have && sess["aiPaused"].asInteger() == 0;
        btn(x, y - 28.f, x + 90.f, y, ai_on ? "AI: On" : "AI: Off", H_AI, 0, ai_on ? "AI characters talk. Click to pause all AI (written lines only)" : "All AI is paused. Click to let AI characters talk again", p, have, ai_on ? BTN_ACTIVE : 0);
        x += 96.f;
        // the AI meter (§3 top bar; HUD §2.7 budget layer)
        const LLSD& ai = mState["ai"];
        std::string meter = "AI meter: -";
        if (ai.isMap())
        {
            meter = cap(ai["modeText"].asString(), 64) + llformat(" \xC2\xB7 today %.2f of %d credits", ai["todayCredits"].asReal(), ai["capCredits"].asInteger());
            if (ai.has("turnsLeft")) meter += " \xC2\xB7 about " + ai["turnsLeft"].asString() + " turns left";
        }
        textL(small, meter, x, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, llmax(60.f, r - x - 330.f));
        // overlays and the director camera, at the right
        F32 rx = r - 320.f;
        btn(rx, y - 28.f, rx + 60.f, y, "Zones", H_OVERLAY, 0, mOvZones ? "Hide the zones in the world" : "Show the adventure's zones in the world while this console is open", p, true, mOvZones ? BTN_ACTIVE : 0);
        btn(rx + 64.f, y - 28.f, rx + 124.f, y, "Paths", H_OVERLAY, 1, mOvPaths ? "Hide the paths" : "Show the characters' paths in the world", p, true, mOvPaths ? BTN_ACTIVE : 0);
        btn(rx + 128.f, y - 28.f, rx + 188.f, y, "Tags", H_OVERLAY, 2, mOvNpcs ? "Hide the character tags" : "Show each character's handle above them", p, true, mOvNpcs ? BTN_ACTIVE : 0);
        btn(rx + 192.f, y - 28.f, rx + 262.f, y, "Camera", H_DIRCAM, 0, mDirCam ? "Director camera on: click to give the camera back to your avatar" : "Director camera: free the camera from your avatar with no distance limit", p, true, mDirCam ? BTN_ACTIVE : 0);
        btn(rx + 266.f, y - 28.f, r, y, "Park me", H_PARK, 0, "Teleport your avatar out of the way: the pack's gm-booth spot, or 300 m above you (asks first)", p);
        (void)body;
    }
    return y - 38.f;
}

void WolfAdventureGmView::drawParties(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    card(l, b, r, t, p, 6.f);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = beginScroll(100, l, b, r, t) - 8.f;
    {
        LLLocalClipRect clip(LLRect((S32)l, (S32)t, (S32)r, (S32)b));
        y = head(l + 8.f, r - 8.f, y, "Parties", p);
        const LLSD& ps = mState["parties"];
        if (!ps.isArray() || ps.size() == 0) y = para(l + 8.f, r - 8.f, y, mSession.empty() ? "No session." : "No party is playing.", p.mDim, true);
        for (size_t i = 0; ps.isArray() && i < (size_t)ps.size(); ++i)
        {
            const LLSD& party = ps[(LLSD::Integer)i];
            const bool sel = party["party"].asString() == mParty;
            const LLSD& mem = party["members"];
            const S32 rows = 3 + (mem.isArray() ? (S32)mem.size() : 0);
            const F32 h = (F32)rows * slh + 14.f;
            button(l + 6.f, y - h, r - 8.f, y, std::string(), p, flagsFor(H_PARTY, (S32)i, true) | (sel ? BTN_ACTIVE : 0));
            hit(l + 6.f, y - h, r - 8.f, y, H_PARTY, (S32)i, "Work with this party in the tabs");
            F32 ty = y - 6.f;
            const std::string leader = cap(party["leaderName"].asString(), 64);
            textL(bold, "Leader: " + (leader.empty() ? std::string("?") : leader), l + 12.f, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            ty -= slh;
            std::string beat = cap(party["beat"].asString(), 64);
            if (!party["ending"].asString().empty()) beat = "Ended: " + cap(party["ending"].asString(), 64);
            if (party["paused"].asInteger() != 0) beat += " \xC2\xB7 paused";
            textL(small, "Beat: " + beat, l + 12.f, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            ty -= slh;
            for (size_t k = 0; mem.isArray() && k < (size_t)mem.size(); ++k)
            {
                const LLSD& m = mem[(LLSD::Integer)k];
                // the ready state as a word, not colour alone
                std::string line = cap(m["name"].asString(), 48) + " \xC2\xB7 " + (m["region"].asString().empty() ? std::string("away") : cap(m["region"].asString(), 40))
                                 + (m["ready"].asInteger() != 0 ? " \xC2\xB7 ready" : "");
                textL(small, line, l + 16.f, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 30.f);
                ty -= slh;
            }
            y -= h + 6.f;
        }
    }
    endScroll(100, y, p);
}

void WolfAdventureGmView::drawLog(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    card(l, b, r, t, p, 6.f);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = t - 8.f;
    y = head(l + 8.f, r - 8.f, y, mConvoOn ? "Conversations" : "Session log", p);
    // filter chips (§3 Log "filters: story, players, AI, Director, GM, rolls, rewards") and Conversations
    {
        F32 x = l + 8.f;
        for (S32 i = 0; i < LOG_FILTER_COUNT; ++i)
        {
            const F32 w = small->getWidthF32(LOG_FILTERS[i]) + 14.f;
            if (x + w > r - 8.f) { x = l + 8.f; y -= 26.f; }
            btn(x, y - 22.f, x + w, y, LOG_FILTERS[i], H_LOG_FILTER, i, std::string("Show ") + LOG_FILTERS[i], p, !mConvoOn, mLogFilter == i && !mConvoOn ? BTN_ACTIVE : 0);
            x += w + 4.f;
        }
        y -= 26.f;
        btn(l + 8.f, y - 24.f, l + 130.f, y, mConvoOn ? "Show the log" : "Conversations", H_CONVO, 0,
            mConvoOn ? "Back to the session log" : "This session's conversations with the characters", p, !mSession.empty());
        btn(l + 136.f, y - 24.f, r - 8.f, y, "Older", H_LOG_OLDER, 0, "Load older log entries", p, !mSession.empty() && !mConvoOn && mBusy == 0);
        y -= 30.f;
    }
    F32 ty = beginScroll(200, l, b, r, y);
    {
        LLLocalClipRect clip(LLRect((S32)l, (S32)y, (S32)r, (S32)b));
        if (mConvoOn)
        {
            const LLSD& convo = mConvo;
            const LLSD& lines = convo["lines"];
            if (!lines.isArray() || lines.size() == 0) ty = para(l + 8.f, r - 12.f, ty, "No conversations yet.", p.mDim, true);
            for (size_t i = 0; lines.isArray() && i < (size_t)lines.size(); ++i)
            {
                const LLSD& ln = lines[(LLSD::Integer)i];
                const std::string who = ln["who"].asString() == "player" ? cap(ln["name"].asString(), 48) : cap(ln["castName"].asString(), 48);
                std::string head_line = whenText(ln["at"]) + " \xC2\xB7 " + who;
                if (!ln["flags"].asString().empty()) head_line += " \xC2\xB7 " + cap(ln["flags"].asString(), 64);
                textL(LLFontGL::getFontSansSerifSmallBold(), head_line, l + 8.f, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 20.f);
                ty -= slh;
                ty = para(l + 8.f, r - 12.f, ty, cap(ln["text"].asString(), 1000), p.mText, true, 8) - 4.f;
            }
        }
        else
        {
            S32 shown = 0;
            for (const LLSD* src : { &mState["log"], &mOlderLog })
            {
                for (size_t i = 0; src->isArray() && i < (size_t)src->size(); ++i)
                {
                    const LLSD& e = (*src)[(LLSD::Integer)i];
                    if (!logMatches(mLogFilter, e)) continue;
                    ++shown;
                    std::string who = cap(e["name"].asString(), 48);
                    const std::string line1 = whenText(e["at"]) + " \xC2\xB7 " + cap(e["source"].asString(), 12) + (who.empty() ? std::string() : " \xC2\xB7 " + who);
                    textL(LLFontGL::getFontSansSerifSmallBold(), line1, l + 8.f, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 20.f);
                    ty -= slh;
                    ty = para(l + 8.f, r - 12.f, ty, cap(e["text"].asString().empty() ? e["type"].asString() : e["text"].asString(), 500), p.mText, true, 6) - 4.f;
                }
            }
            if (shown == 0) ty = para(l + 8.f, r - 12.f, ty, "Nothing in the log for this filter yet.", p.mDim, true);
        }
    }
    endScroll(200, ty, p);
}

F32 WolfAdventureGmView::drawPossession(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 "Possession (a plate shown while possessing)": the NPC's portrait, a say box (Enter speaks;
    // gm_say), Click to move (gm_move), an animation picker (gm_anim), the last 5 player lines, Suggest a reply
    // with Use, Release.
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 slh = (F32)small->getLineHeight();
    const F32 h0 = top;
    F32 y = top - 10.f;
    const F32 portrait = 56.f;
    disc(l + 10.f + portrait * 0.5f, y - portrait * 0.5f, portrait * 0.5f, fade(p.mAccent, 0.35f));
    {
        LLWString w = utf8str_to_wstring(mPossessName);
        std::string ini;
        bool start = true;
        for (llwchar c : w) { if (c == ' ') { start = true; continue; } if (start && ini.size() < 4) ini += wstring_to_utf8str(LLWString(1, c)); start = false; }
        textL(display(22.f), ini, l + 10.f + portrait * 0.5f, y - portrait * 0.5f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }
    const F32 tl = l + portrait + 22.f;
    textL(display(18.f), "You are " + mPossessName, tl, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - tl - 110.f);
    btn(r - 100.f, y - 28.f, r, y, "Release", H_PS_RELEASE, 0, "Give the character back to its AI and written lines", p, mBusy == 0, BTN_DANGER);
    y -= (F32)display(18.f)->getLineHeight() + 6.f;
    textL(small, "Players see the character speak and move; its AI and written lines are stopped while you have it.", tl, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - tl);
    y = llmin(y - slh - 6.f, top - portrait - 16.f);
    field(l + 10.f, y - 30.f, r - 110.f, y, F_PS_SAY, "Say something as " + mPossessName + "...", "What the character says aloud (Enter speaks)", p);
    btn(r - 100.f, y - 30.f, r, y, "Speak", H_PS_USE, -1, "Speak the line (Enter)", p, !text(F_PS_SAY).empty(), BTN_PRIMARY);
    y -= 38.f;
    btn(l + 10.f, y - 28.f, l + 160.f, y, mClickMove ? "Click to move: On" : "Click to move: Off", H_PS_MOVE, 0,
        mClickMove ? "Each click on the ground walks the character there. Click to stop" : "Walk the character where you click on the ground", p, true, mClickMove ? BTN_ACTIVE : 0);
    btn(l + 166.f, y - 28.f, l + 260.f, y, std::string("Gait: ") + GAITS[mGait], H_PS_GAIT, 0, "Walk, run or fly", p);
    field(l + 270.f, y - 28.f, r - 150.f, y, F_PS_ANIM, "Animation name", "The name of the animation to play on the character", p);   // [WOLF ADVENTURE AUDIT 2026-10-17] plain words
    btn(r - 144.f, y - 28.f, r - 74.f, y, "Play", H_PS_ANIM_PLAY, 0, "Play the animation", p, !text(F_PS_ANIM).empty());
    btn(r - 70.f, y - 28.f, r, y, "Stop", H_PS_ANIM_STOP, 0, "Stop the animation", p, !text(F_PS_ANIM).empty());
    y -= 36.f;
    textL(LLFontGL::getFontSansSerifSmallBold(), "Players said to " + mPossessName + ":", l + 10.f, y, p.mText);
    y -= slh + 2.f;
    S32 n = 0;
    const LLSD& lines = mPsLines;
    for (size_t i = 0; lines.isArray() && i < (size_t)lines.size() && n < 5; ++i)
    {
        const LLSD& ln = lines[(LLSD::Integer)i];
        if (ln["who"].asString() != "player") continue;
        y = para(l + 14.f, r, y, cap(ln["name"].asString(), 48) + ": " + cap(ln["text"].asString(), 500), p.mText, true, 3);
        ++n;
    }
    if (n == 0) { textL(small, "Nothing yet.", l + 14.f, y, p.mDim); y -= slh; }
    y -= 4.f;
    btn(l + 10.f, y - 28.f, l + 180.f, y, "Suggest a reply", H_PS_SUGGEST, 0, "Ask the AI for lines this character could say (only you see them)", p, mBusy == 0);
    y -= 34.f;
    for (size_t i = 0; mPsSuggest.isArray() && i < (size_t)mPsSuggest.size(); ++i)
    {
        const LLSD& sgg = mPsSuggest[(LLSD::Integer)i];
        const F32 top_i = y;
        y = para(l + 14.f, r - 80.f, y, cap(sgg["text"].asString(), 2000), p.mText, true, 6);
        if (!sgg["warning"].asString().empty()) y = para(l + 14.f, r - 80.f, y, "Warning: " + cap(sgg["warning"].asString(), 200), WARN_INK, true, 2);
        btn(r - 70.f, top_i - 26.f, r, top_i, "Use", H_PS_USE, (S32)i, "Copy into the say box to edit or speak", p);
        y = llmin(y, top_i - 30.f) - 4.f;
    }
    // the plate around it all
    chamferEdge(l + 0.5f, y - 4.f, r - 0.5f, h0 - 0.5f, 6.f, 2.f, p.mAccent);
    return y - 8.f;
}

F32 WolfAdventureGmView::tabStory(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Story: the beat and its objective; Advance, Go to beat, Switch scene; the triggers with Fire; the
    // pack's gm buttons; flags editable in place; reputation and honour per player with +/- and a reason.
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 slh = (F32)small->getLineHeight();
    const bool idle = mBusy == 0;
    F32 y = top;
    const LLSD* party = selectedParty();
    if (!party) return para(l, r, y, "Choose a party on the left.", p.mDim);
    std::string scene;
    const LLSD* beat = packBeat((*party)["beat"].asString(), &scene);
    y = head(l, r, y, "Now", p);
    y = para(l, r, y, "Beat " + cap((*party)["beat"].asString(), 64) + (scene.empty() ? std::string() : " in scene " + scene), p.mText);
    y = para(l, r, y, "Objective: " + cap((*party)["objective"].asString(), 240), p.mDim) - 6.f;
    btn(l, y - 28.f, l + 120.f, y, "Advance", H_ADVANCE, 0, "Go to the beat's first trigger's next, or the next beat in the scene", p, idle, BTN_PRIMARY);
    y -= 36.f;
    if (mPackFor.empty()) return para(l, r, y, mPackLoading ? "Reading the adventure..." : "The story's beats, triggers and flags are shown to the owner and adventure-wide hosts only. Advance and the other tabs still work.", p.mDim);
    // the beat's triggers, each with Fire; the gm buttons are the triggers with on: gm (ADVENTURE_PACK §11.1)
    y = head(l, r, y, "Triggers of this beat", p);
    const LLSD& trig = beat ? (*beat)["triggers"] : LLSD();
    if (!trig.isArray() || trig.size() == 0) y = para(l, r, y, "This beat has no triggers.", p.mDim, true);
    for (size_t i = 0; trig.isArray() && i < (size_t)trig.size(); ++i)
    {
        const LLSD& tr = trig[(LLSD::Integer)i];
        const bool gm = tr["on"].asString() == "gm";
        std::string label = gm ? "Host button: " + cap(tr["label"].asString(), 24) : cap(tr["id"].asString(), 48) + " (on " + cap(tr["on"].asString(), 12) + ")";
        if (tr.has("next")) label += " \xE2\x86\x92 " + cap(tr["next"].isString() ? tr["next"].asString() : std::string("move"), 48);
        textL(LLFontGL::getFontSansSerif(), label, l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 90.f);
        btn(r - 80.f, y - 26.f, r, y, gm ? "Press" : "Fire", H_FIRE, (S32)i, "Fire this trigger now, as if its event happened", p, idle, gm ? BTN_PRIMARY : 0);
        y -= 30.f;
    }
    y -= 6.f;
    // Go to beat / Switch scene (a list of the pack's scenes and beats)
    y = head(l, r, y, "Go to a beat or scene", p);
    const LLSD& acts = packList("acts");
    S32 bi = 0, si = 0;
    for (size_t a = 0; a < (size_t)acts.size(); ++a)
    {
        const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s, ++si)
        {
            const LLSD& sc = scenes[(LLSD::Integer)s];
            textL(LLFontGL::getFontSansSerifSmallBold(), "Scene: " + cap(sc["title"].asString(), 64), l, y - 13.f, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 130.f);
            btn(r - 120.f, y - 26.f, r, y, "Switch scene", H_GOTO_SCENE, si, "Move the party to this scene: its whole stage is applied", p, idle);
            y -= 30.f;
            const LLSD& beats = sc["beats"];
            for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b, ++bi)
            {
                const LLSD& bt = beats[(LLSD::Integer)b];
                const bool cur = bt["id"].asString() == (*party)["beat"].asString();
                textL(small, (cur ? "Now: " : "") + cap(bt["id"].asString(), 48) + " - " + cap(bt["objective"].asString(), 160), l + 14.f, y - 12.f,
                      cur ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 110.f);
                btn(r - 80.f, y - 24.f, r, y, "Go", H_GOTO_BEAT, bi, "Move the party to this beat", p, idle && !cur);
                y -= 28.f;
            }
        }
    }
    y -= 6.f;
    // flags in a table, editable in place
    y = head(l, r, y, "Flags", p);
    const LLSD& flags = mPack["state"]["flags"];
    const LLSD& values = (*party)["flags"];
    field(l, y - 28.f, l + 220.f, y, F_REASON, "Reason (for the log)", "Why you changed it (shown in the session log)", p);
    y -= 34.f;
    for (size_t i = 0; flags.isArray() && i < (size_t)flags.size(); ++i)
    {
        const LLSD& f = flags[(LLSD::Integer)i];
        const std::string id = f["id"].asString(), type = f["type"].asString(), scope = f["scope"].asString();
        // party-scope values come with gm_state's party; player and world scope are set but not shown here
        std::string val = values.has(id) ? (values[id].isBoolean() ? (values[id].asBoolean() ? "Yes" : "No") : cap(values[id].asString(), 64)) : std::string("-");
        if (scope != "party") val = scope + " scope";
        textL(LLFontGL::getFontSansSerif(), cap(id, 48) + ": " + val, l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 200.f);
        if (type == "bool")
        {
            btn(r - 180.f, y - 26.f, r - 95.f, y, "Set Yes", H_FLAG_TOGGLE, (S32)i * 2 + 1, "Set it true", p, idle);
            btn(r - 90.f, y - 26.f, r, y, "Set No", H_FLAG_TOGGLE, (S32)i * 2, "Set it false", p, idle);
        }
        else if (type == "int")
        {
            btn(r - 90.f, y - 26.f, r - 50.f, y, "-1", H_FLAG_STEP, (S32)i * 2, "One less", p, idle);
            btn(r - 46.f, y - 26.f, r, y, "+1", H_FLAG_STEP, (S32)i * 2 + 1, "One more", p, idle);
        }
        else
        {
            btn(r - 90.f, y - 26.f, r, y, "Set text", H_FLAG_SET, (S32)i, "Set it to the text in the box below", p, idle && !text(F_FLAG_TEXT).empty());
        }
        y -= 30.f;
    }
    if (flags.isArray() && flags.size() > 0)
    {
        field(l, y - 28.f, l + 260.f, y, F_FLAG_TEXT, "Text for a text flag", "The value Set text writes", p);
        y -= 36.f;
    }
    else y = para(l, r, y, "This adventure has no flags.", p.mDim, true);
    // reputation and honour per player, with +/- and a reason
    y = head(l, r, y, "Honour and reputation", p);
    const LLSD& mem = (*party)["members"];
    const LLSD& factions = mPack["state"]["factions"];
    const LLSD& cast = packList("cast");
    for (size_t k = 0; mem.isArray() && k < (size_t)mem.size(); ++k)
    {
        const LLSD& m = mem[(LLSD::Integer)k];
        textL(LLFontGL::getFontSansSerifSmallBold(), cap(m["name"].asString(), 64), l, y, p.mText);
        y -= slh + 4.f;
        if (mPack["state"]["honour"]["enabled"].asBoolean())
        {
            textL(small, "Honour", l + 10.f, y - 12.f, p.mText);
            btn(r - 110.f, y - 24.f, r - 56.f, y, "-5", H_HONOUR_STEP, (S32)k * 2, "Honour down 5", p, idle);
            btn(r - 52.f, y - 24.f, r, y, "+5", H_HONOUR_STEP, (S32)k * 2 + 1, "Honour up 5", p, idle);
            y -= 28.f;
        }
        for (size_t f = 0; factions.isArray() && f < (size_t)factions.size(); ++f)
        {
            textL(small, cap(factions[(LLSD::Integer)f]["name"].asString(), 48), l + 10.f, y - 12.f, p.mText);
            btn(r - 110.f, y - 24.f, r - 56.f, y, "-5", H_REP_STEP, ((S32)k * 64 + (S32)f) * 2, "Reputation down 5", p, idle);
            btn(r - 52.f, y - 24.f, r, y, "+5", H_REP_STEP, ((S32)k * 64 + (S32)f) * 2 + 1, "Reputation up 5", p, idle);
            y -= 28.f;
        }
        for (size_t c = 0; c < (size_t)cast.size() && c < 64; ++c)
        {
            textL(small, castName(cast[(LLSD::Integer)c]["id"].asString()) + "'s opinion", l + 10.f, y - 12.f, p.mText);
            btn(r - 110.f, y - 24.f, r - 56.f, y, "-5", H_CASTREP_STEP, ((S32)k * 64 + (S32)c) * 2, "Their opinion down 5", p, idle);
            btn(r - 52.f, y - 24.f, r, y, "+5", H_CASTREP_STEP, ((S32)k * 64 + (S32)c) * 2 + 1, "Their opinion up 5", p, idle);
            y -= 28.f;
        }
        y -= 4.f;
    }
    return y;
}

F32 WolfAdventureGmView::tabCast(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 "Cast and spawn": the NPCs present, with Possess, Release, Remove and Move here; the spawn
    // palette: the pack's cast (Place), its encounters (Fire), and for 5E the SRD monsters.
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "Characters here", p);
    btn(l, y - 28.f, l + 220.f, y, "Find characters here", H_FIND_NPCS, 0, "Ask the story which of its characters stand on the region you are on", p, idle);
    y -= 34.f;
    const LLSD npcs = npcList();
    if (npcs.size() == 0) y = para(l, r, y, "No characters found on your region for this session. Stand on the scene's region and press Find characters here.", p.mDim, true);
    for (size_t i = 0; i < (size_t)npcs.size(); ++i)
    {
        const LLSD& n = npcs[(LLSD::Integer)i];
        const std::string handle = cap(n["handle"].asString(), 128);
        const std::string who = cap(n["possessedBy"].asString(), 36);
        std::string line = n["name"].asString() + " \xC2\xB7 " + handle;
        if (!who.empty()) line += LLUUID(who) == gAgentID ? " \xC2\xB7 you have it" : " \xC2\xB7 played by a host";
        textL(LLFontGL::getFontSansSerif(), line, l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 330.f);
        const bool mine = !who.empty() && LLUUID(who) == gAgentID;
        if (mine) btn(r - 320.f, y - 26.f, r - 240.f, y, "Release", H_RELEASE, (S32)i, "Give it back to its AI", p, idle);
        else btn(r - 320.f, y - 26.f, r - 240.f, y, "Possess", H_POSSESS, (S32)i, "Speak and move as this character; its AI pauses", p, idle && who.empty());
        btn(r - 236.f, y - 26.f, r - 140.f, y, "Move here", H_MOVEHERE, (S32)i, "Walk it to where you click on the ground", p, idle);
        btn(r - 136.f, y - 26.f, r, y, "Remove", H_REMOVE, (S32)i, "Take it out of the world (asks first)", p, idle, BTN_DANGER);
        y -= 30.f;
    }
    y -= 6.f;
    y = head(l, r, y, "Spawn palette", p);
    const LLSD& cast = packList("cast");
    for (size_t i = 0; i < (size_t)cast.size(); ++i)
    {
        const LLSD& c = cast[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), castName(c["id"].asString()) + (c["role"].asString().empty() ? std::string() : " - " + cap(c["role"].asString(), 120)),
              l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 100.f);
        btn(r - 90.f, y - 26.f, r, y, "Place", H_PLACE_CAST, (S32)i, "Click the ground to place this character there", p, idle);
        y -= 30.f;
    }
    const LLSD& enc = packList("encounters");
    for (size_t i = 0; i < (size_t)enc.size(); ++i)
    {
        const LLSD& e = enc[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), "Encounter: " + cap(e["label"].asString().empty() ? e["id"].asString() : e["label"].asString(), 64),
              l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 100.f);
        btn(r - 90.f, y - 26.f, r, y, "Fire", H_FIRE_ENC, (S32)i, "Spawn this encounter at its spot", p, idle, BTN_PRIMARY);
        y -= 30.f;
    }
    if (cast.size() == 0 && enc.size() == 0) y = para(l, r, y, "This adventure has no characters or encounters to spawn.", p.mDim, true);
    if (mPack["rules"]["preset"].asString() == "5e-srd-5.2")
    {
        // RULES_PRESETS_V1.md §6: the 5E data (monster stat blocks) is a skeleton until P5 transcribes the SRD.
        y = para(l, r, y - 4.f, "SRD monsters: the 5E preset carries no stat blocks yet (they arrive when the SRD 5.2.1 data is transcribed); "
                 "place the adventure's own characters instead.", p.mDim, true);
    }
    return y;
}

F32 WolfAdventureGmView::tabNarrate(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Narrate: title, text, image (a texture), up to 6 options, and to whom; Send. Whisper to one player.
    const bool idle = mBusy == 0;
    const LLSD* party = selectedParty();
    const LLSD mem = party ? (*party)["members"] : LLSD();
    const S32 nmem = mem.isArray() ? (S32)mem.size() : 0;
    F32 y = head(l, r, top, "Narration card", p);
    field(l, y - 30.f, r, y, F_NARRATE_TITLE, "Title (optional)", "The card's title (up to 64 characters)", p);
    y -= 36.f;
    field(l, y - 110.f, r, y, F_NARRATE_TEXT, "What the players read... (Shift+Enter for a new line)", "The card's text (up to 2000 characters)", p);
    y -= 116.f;
    field(l, y - 30.f, r, y, F_NARRATE_IMAGE, "Picture (optional): paste a texture's Asset UUID", "In your inventory, right-click a texture > Copy Asset UUID, then paste it here", p,
          !text(F_NARRATE_IMAGE).empty() && !LLUUID::validate(text(F_NARRATE_IMAGE)));
    y -= 36.f;
    for (S32 i = 0; i < mNarrateOpts; ++i)
    {
        field(l, y - 30.f, r - 90.f, y, F_OPT0 + i, llformat("Option %d label (24 characters)", i + 1), "A choice the players can press", p);
        btn(r - 84.f, y - 30.f, r, y, "Remove", H_NARRATE_DELOPT, i, "Remove this option", p);
        y -= 36.f;
    }
    if (mNarrateOpts < 6) { btn(l, y - 28.f, l + 140.f, y, "Add an option", H_NARRATE_ADDOPT, 0, "Up to 6 choices; answers come back in the log", p); y -= 34.f; }
    std::string to = "To: the whole party";
    if (mNarrateTo > 0 && mNarrateTo <= nmem) to = "To: " + cap(mem[mNarrateTo - 1]["name"].asString(), 48);
    btn(l, y - 28.f, l + 260.f, y, to, H_NARRATE_TO, 0, "The party, or one of its players (click to change)", p, party != nullptr);
    btn(r - 120.f, y - 28.f, r, y, "Send", H_NARRATE_SEND, 0, "Send the card", p, idle && party && !text(F_NARRATE_TEXT).empty(), BTN_PRIMARY);
    y -= 40.f;
    y = head(l, r, y, "Whisper", p);
    std::string wto = "To: choose a player";
    if (mWhisperTo > 0 && mWhisperTo <= nmem) wto = "To: " + cap(mem[mWhisperTo - 1]["name"].asString(), 48);
    btn(l, y - 28.f, l + 260.f, y, wto, H_WHISPER_TO, 0, "Who hears it (click to change)", p, nmem > 0);
    y -= 34.f;
    field(l, y - 60.f, r - 130.f, y, F_WHISPER, "A private line from the narrator...", "Only that player sees it", p);
    btn(r - 120.f, y - 28.f, r, y, "Whisper", H_WHISPER_SEND, 0, "Send it to that player only", p, idle && mWhisperTo > 0 && !text(F_WHISPER).empty(), BTN_PRIMARY);
    return y - 70.f;
}

F32 WolfAdventureGmView::tabParty(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Party: Call them to... (sends offers, never forced), Ready check, End for the party..., Remove player...
    const bool idle = mBusy == 0;
    const LLSD* party = selectedParty();
    if (!party) return para(l, r, top, "Choose a party on the left.", p.mDim);
    F32 y = head(l, r, top, "Call them to a spot", p);
    y = para(l, r, y, "Each player gets an offer to teleport (Go / Not now). Nobody is moved without their own click.", p.mDim, true) - 4.f;
    const LLSD& spots = mPack["places"]["spots"];
    for (size_t i = 0; spots.isArray() && i < (size_t)spots.size(); ++i)
    {
        const LLSD& s = spots[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), cap(s["label"].asString().empty() ? s["id"].asString() : s["label"].asString(), 64), l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 110.f);
        btn(r - 100.f, y - 26.f, r, y, "Call here", H_GATHER, (S32)i, "Offer the party a teleport to this spot", p, idle);
        y -= 30.f;
    }
    y -= 6.f;
    btn(l, y - 28.f, l + 160.f, y, "Ready check", H_READY_CHECK, 0, "A card asking each player if they are ready; answers come back in the log", p, idle);
    y -= 40.f;
    y = head(l, r, y, "End for the party", p);
    const LLSD& endings = packList("endings");
    std::string ew = "Ending: none (they just stop)";
    if (mEndPick > 0 && mEndPick <= (S32)endings.size()) ew = "Ending: " + cap(endings[mEndPick - 1]["title"].asString(), 64);
    btn(l, y - 28.f, l + 320.f, y, ew, H_END_PICK, 0, "Which ending (click to change)", p);
    btn(r - 150.f, y - 28.f, r, y, "End for the party", H_END_PARTY, 0, "End the story for this party (asks first)", p, idle, BTN_DANGER);
    y -= 40.f;
    y = head(l, r, y, "Remove a player", p);
    const LLSD& mem = (*party)["members"];
    for (size_t k = 0; mem.isArray() && k < (size_t)mem.size(); ++k)
    {
        textL(LLFontGL::getFontSansSerif(), cap(mem[(LLSD::Integer)k]["name"].asString(), 64), l, y - 14.f, p.mText);
        btn(r - 100.f, y - 26.f, r, y, "Remove", H_KICK, (S32)k, "Remove this player from the party and the session (asks first)", p, idle, BTN_DANGER);
        y -= 30.f;
    }
    return y;
}

F32 WolfAdventureGmView::tabRewards(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Rewards: to a player or the party, XP, money, items or a parcel, with a reason; the remaining GM
    // reward limit; fails with the brain's reason when the game is not linked (BRAIN §8.3).
    const bool idle = mBusy == 0;
    const LLSD* party = selectedParty();
    if (!party) return para(l, r, top, "Choose a party on the left.", p.mDim);
    const LLSD& mem = (*party)["members"];
    F32 y = head(l, r, top, "Give a reward", p);
    std::string to = "To: the whole party";
    if (mRewardTo > 0 && mem.isArray() && mRewardTo <= (S32)mem.size()) to = "To: " + cap(mem[mRewardTo - 1]["name"].asString(), 48);
    btn(l, y - 28.f, l + 260.f, y, to, H_REWARD_TO, 0, "The party or one player (click to change)", p);
    y -= 34.f;
    const LLSD& left = mState["session"];
    if (left.has("gmMoneyLeft") || left.has("gmXpLeft"))
        y = para(l, r, y, llformat("Left this session: %d money, %d XP (the owner's limit).", left["gmMoneyLeft"].asInteger(), left["gmXpLeft"].asInteger()), p.mDim, true);
    else
        y = para(l, r, y, "A host's rewards are capped per session by the adventure owner's limit (Studio, AI and voice).", p.mDim, true);
    y -= 4.f;
    const F32 c3 = (r - l) / 3.f;
    field(l, y - 30.f, l + c3 - 6.f, y, F_REWARD_XP, "XP", "Experience points (a whole number)", p);
    field(l + c3, y - 30.f, l + 2.f * c3 - 6.f, y, F_REWARD_MONEY, "Money", "Game money (a whole number)", p);
    y -= 36.f;
    field(l, y - 30.f, l + 2.f * c3 - 6.f, y, F_REWARD_ITEM, "Item name (the game's item id)", "A wolfGame item id, lower case", p);
    field(l + 2.f * c3, y - 30.f, r, y, F_REWARD_QTY, "Qty", "How many", p);
    y -= 36.f;
    field(l, y - 30.f, r, y, F_REASON, "Reason (shown to the player)", "Why they get it", p);
    y -= 36.f;
    btn(r - 140.f, y - 28.f, r, y, "Give", H_REWARD_SEND, 0, "Give it now", p, idle, BTN_PRIMARY);
    y -= 40.f;
    y = head(l, r, y, "Or a parcel from the adventure", p);
    const LLSD& parcels = packList("parcels");
    if (parcels.size() == 0) y = para(l, r, y, "This adventure has no reward parcels.", p.mDim, true);
    for (size_t i = 0; i < (size_t)parcels.size(); ++i)
    {
        const LLSD& pc = parcels[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), cap(pc["label"].asString().empty() ? pc["id"].asString() : pc["label"].asString(), 64), l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 100.f);
        btn(r - 90.f, y - 26.f, r, y, "Give", H_REWARD_PARCEL, (S32)i, "Give this parcel", p, idle);
        y -= 30.f;
    }
    return y;
}

F32 WolfAdventureGmView::tabWorld(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 World: time of day, weather presets and music, applied to the region (needs the region's
    // environment approval; the brain gives the reason when it is missing) or the music to the party.
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "Time of day", p);
    btn(l, y - 28.f, l + 40.f, y, "-", H_HOUR_STEP, 0, "An hour earlier", p);
    textL(display(18.f), llformat("%02d:00", mHour), l + 50.f, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
    btn(l + 120.f, y - 28.f, l + 160.f, y, "+", H_HOUR_STEP, 1, "An hour later", p);
    btn(l + 170.f, y - 28.f, l + 320.f, y, "Set the time", H_HOUR_SET, 0, "Fix the region's time of day (Wolf Environment)", p, idle && !partyRegion().empty());
    y -= 40.f;
    y = head(l, r, y, "Weather", p);
    F32 x = l;
    for (S32 i = 0; i < 8; ++i)
    {
        std::string w = WEATHERS[i];
        w[0] = (char)toupper(w[0]);
        if (x + 96.f > r) { x = l; y -= 34.f; }
        btn(x, y - 28.f, x + 92.f, y, w, H_WEATHER, i, "Set the region's weather to " + w, p, idle && !partyRegion().empty());
        x += 96.f;
    }
    y -= 40.f;
    y = para(l, r, y, "Time and weather change the scene's region for everyone there, and need the region holder's environment approval.", p.mDim, true) - 6.f;
    y = head(l, r, y, "Music for the party", p);
    field(l, y - 30.f, r, y, F_MUSIC, "https://... a music stream", "An https stream (the viewers' music rules)", p,
          !text(F_MUSIC).empty() && text(F_MUSIC).compare(0, 8, "https://") != 0);
    y -= 36.f;
    btn(l, y - 28.f, l + 120.f, y, "Play", H_MUSIC_PLAY, 0, "Play it for the party", p, idle && text(F_MUSIC).compare(0, 8, "https://") == 0);
    btn(l + 126.f, y - 28.f, l + 246.f, y, "Stop", H_MUSIC_STOP, 0, "Stop the party's music", p, idle);
    return y - 40.f;
}

F32 WolfAdventureGmView::tabRules(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Rules: ask the party (or roll in secret for the GM) a check of the rules with a DC, public or
    // private; the turn order (start, next, end); Override on the last rolls. Source: wolfadventure_gm_lib.php
    // wag_roll takes {party|agent, check, dc?, public, secret} - a check of the rules, not free dice notation.
    const bool idle = mBusy == 0;
    const LLSD* party = selectedParty();
    F32 y = head(l, r, top, "Request a check", p);
    field(l, y - 30.f, l + 200.f, y, F_CHECK, "Check id (e.g. draw)", "A check of the adventure's rules (its id)", p);
    const bool dc_bad = !text(F_DC).empty() && text(F_DC).find_first_not_of("0123456789") != std::string::npos;
    field(l + 206.f, y - 30.f, l + 286.f, y, F_DC, "DC", "Difficulty: a whole number, or empty to keep it hidden", p, dc_bad);
    btn(l + 292.f, y - 30.f, l + 402.f, y, mDicePublic ? "Public" : "Private", H_DICE_PUBLIC, 0,
        mDicePublic ? "Everyone in the party sees the dice (click for private)" : "Only the roller sees the dice (click for public)", p);
    y -= 36.f;
    const bool can = idle && party && !text(F_CHECK).empty() && !dc_bad;
    btn(l, y - 30.f, l + 160.f, y, "Ask the party", H_CHECK_ASK, 0, "Each player in the party is asked to roll this check", p, can, BTN_PRIMARY);
    btn(l + 166.f, y - 30.f, l + 326.f, y, "Roll in secret", H_DICE_ROLL, 0, "Rolled at once for each player, shown to you alone", p, can);
    y -= 36.f;
    if (dc_bad) y = para(l, r, y, "The DC is a whole number (or leave it empty).", DANGER_INK, true);
    y -= 4.f;
    y = head(l, r, y, "Turn order", p);
    btn(l, y - 28.f, l + 120.f, y, "Start", H_COMBAT, 0, "Start turn-based combat for the party", p, idle && party);
    btn(l + 126.f, y - 28.f, l + 246.f, y, "Next turn", H_COMBAT, 1, "Move to the next in the order", p, idle && party);
    btn(l + 252.f, y - 28.f, l + 372.f, y, "End", H_COMBAT, 2, "End combat", p, idle && party);
    y -= 40.f;
    // [WOLF STORY FIRST M4 2026-10-17] battles (COMBAT_V1.md §3): start one of the story's fights for the party; while one
    // runs, the party row's `fight` (wolfmind_lib.php wm_gm_fight) shows whose turn it is, and on an enemy's or a
    // companion's turn the GM may play it (combat enemy_act {move, target}) instead of waiting for the Director
    {
        const LLSD& battles = packList("battles");
        const LLSD fight = party ? (*party)["fight"] : LLSD();
        if (battles.isArray() && battles.size() > 0 && !fight.isMap())
        {
            y = head(l, r, y, "Start a battle", p);
            F32 x = l;
            for (size_t i = 0; i < (size_t)battles.size() && i < 12; ++i)
            {
                const LLSD& bt = battles[(LLSD::Integer)i];
                const std::string lab = cap(bt["label"].asString().empty() ? bt["id"].asString() : bt["label"].asString(), 28);
                const F32 bw = llmax(90.f, (F32)LLFontGL::getFontSansSerifSmall()->getWidth(lab) + 24.f);
                if (x > l && x + bw > r) { x = l; y -= 32.f; }
                btn(x, y - 28.f, x + bw, y, lab, H_BATTLE_START, (S32)i, "Start this fight for the party now", p, idle && party);
                x += bw + 6.f;
            }
            y -= 40.f;
        }
        if (fight.isMap())
        {
            y = head(l, r, y, "Battle: " + cap(fight["label"].asString(), 32), p);
            y = para(l, r, y, llformat("Round %d \xC2\xB7 ", fight["round"].asInteger()) + cap(fight["currentName"].asString(), 48) + "'s turn", p.mText, true);
            if (!fight["line"].asString().empty()) y = para(l, r, y, cap(fight["line"].asString(), 300), p.mDim, true);
            for (const LLSD& f : llsd::inArray(fight["fighters"]))
            {
                const std::string st = f["state"].asString();
                const std::string line = cap(f["name"].asString(), 40) + (f["side"].asString() == "enemy" ? " (enemy)" : "")
                                       + llformat(" \xC2\xB7 %d/%d", f["hp"][0].asInteger(), f["hp"][1].asInteger())
                                       + (st == "ok" ? std::string() : " \xC2\xB7 " + (st == "fled" ? std::string("ran off") : st));
                y = para(l, r, y, line, st == "ok" || st == "defending" ? p.mText : WARN_INK, false);
            }
            y -= 4.f;
            if (!fight["playerTurn"].asBoolean())
            {
                // whose side is playing: attack someone of the other side who is still in it
                std::string side;
                for (const LLSD& f : llsd::inArray(fight["fighters"])) if (f["key"].asString() == fight["current"].asString()) side = f["side"].asString();
                y = para(l, r, y, "Play " + cap(fight["currentName"].asString(), 48) + "'s move (or leave it: it acts by itself):", p.mText, true);
                F32 x = l;
                S32 fi = 0;
                for (const LLSD& f : llsd::inArray(fight["fighters"]))
                {
                    const std::string st = f["state"].asString();
                    if (f["side"].asString() != side && (st == "ok" || st == "defending"))
                    {
                        const std::string lab = "Attack " + cap(f["name"].asString(), 24);
                        const F32 bw = llmax(90.f, (F32)LLFontGL::getFontSansSerifSmall()->getWidth(lab) + 24.f);
                        if (x > l && x + bw > r) { x = l; y -= 32.f; }
                        btn(x, y - 28.f, x + bw, y, lab, H_ENEMY_HIT, fi, "Attack " + f["name"].asString(), p, idle);
                        x += bw + 6.f;
                    }
                    ++fi;
                }
                if (x > l && x + 186.f > r) { x = l; y -= 32.f; }
                btn(x, y - 28.f, x + 90.f, y, "Defend", H_ENEMY_MOVE, 0, "Defend: attacks on it are harder until its next turn", p, idle);
                if (side == "enemy") btn(x + 96.f, y - 28.f, x + 186.f, y, "Run off", H_ENEMY_MOVE, 1, "It runs off and is out of the fight", p, idle);
                y -= 40.f;
            }
        }
    }
    y = head(l, r, y, "The last rolls", p);
    const LLSD rolls = rollList();
    if (rolls.size() == 0) y = para(l, r, y, "No rolls yet in this session.", p.mDim, true);
    for (size_t i = 0; i < (size_t)rolls.size(); ++i)
    {
        const LLSD& rl = rolls[(LLSD::Integer)i];
        std::string line = cap(rl["name"].asString(), 48) + " \xC2\xB7 " + cap(rl["label"].asString().empty() ? rl["check"].asString() : rl["label"].asString(), 48)
                         + llformat(" \xC2\xB7 %d", rl["total"].asInteger()) + " \xC2\xB7 " + cap(rl["outcome"].asString(), 16);
        textL(LLFontGL::getFontSansSerifSmall(), line, l, y - 12.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 360.f);
        F32 x = r - 352.f;
        for (S32 o = 0; o < 4; ++o)
        {
            std::string w = OUTCOMES[o];
            w[0] = (char)toupper(w[0]);
            btn(x, y - 24.f, x + 84.f, y, w, H_OVERRIDE, (S32)i * 4 + o, "Override this roll's outcome to " + w, p, idle);
            x += 88.f;
        }
        y -= 28.f;
    }
    return y;
}

F32 WolfAdventureGmView::tabAssist(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §3 Assist: What could happen next?, Write boxed text for this scene, Suggest a reply for...; results
    // listed with Use and Copy, marked "AI suggestion; players have not seen this"; its cost shown.
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "Ask the assistant", p);
    static const char* ASKS[3] = { "What could happen next?", "Write boxed text for this scene", "Suggest a reply for..." };
    for (S32 i = 0; i < 3; ++i)
    {
        btn(l, y - 30.f, l + 280.f, y, ASKS[i], H_ASSIST, i, "Paid from the adventure owner's credits (their assistant limit applies)", p, idle && selectedParty() != nullptr);
        if (i == 2)
        {
            const LLSD& cast = packList("cast");
            std::string cw = cast.size() > 0 ? castName(cast[llclamp(mAssistCast, 0, (S32)cast.size() - 1)]["id"].asString()) : std::string("no characters");
            btn(l + 286.f, y - 30.f, l + 486.f, y, "For: " + cw, H_ASSIST_CAST, 0, "Which character (click to change)", p, cast.size() > 0);
        }
        y -= 36.f;
    }
    field(l, y - 30.f, r, y, F_ASSIST_TEXT, "The player's last line (for a reply)", "What the player said to the character", p);
    y -= 40.f;
    if (mSuggest.isArray() && mSuggest.size() > 0)
    {
        y = para(l, r, y, "AI suggestion; players have not seen this." + (mSuggestCost.empty() ? std::string() : " " + mSuggestCost), WARN_INK, true) - 4.f;
        for (size_t i = 0; i < (size_t)mSuggest.size(); ++i)
        {
            const LLSD& s = mSuggest[(LLSD::Integer)i];
            const F32 t0 = y;
            y = para(l, r - 150.f, y, cap(s["text"].asString(), 2000), p.mText, false, 12);
            if (!s["warning"].asString().empty()) y = para(l, r - 150.f, y, "Warning: " + cap(s["warning"].asString(), 200), WARN_INK, true, 2);
            btn(r - 140.f, t0 - 26.f, r - 72.f, t0, "Use", H_ASSIST_USE, (S32)i, "Put it where it goes: the narration text, or the possession say box", p);
            btn(r - 68.f, t0 - 26.f, r, t0, "Copy", H_ASSIST_COPY, (S32)i, "Copy to the clipboard", p);
            y = llmin(y, t0 - 30.f) - 8.f;
        }
    }
    return y;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the GM Console: actions
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureGmView::fieldEnter(S32 f)
{
    if (f == F_PS_SAY) press(H_PS_USE, -1);
    else if (f == F_WHISPER) press(H_WHISPER_SEND, 0);
}

void WolfAdventureGmView::press(S32 id, S32 arg)
{
    const LLSD* party = selectedParty();
    const std::string party_id = party ? (*party)["party"].asString() : std::string();
    const LLSD mem = party ? (*party)["members"] : LLSD();
    const S32 nmem = mem.isArray() ? (S32)mem.size() : 0;
    const std::string reason = text(F_REASON);
    switch (id)
    {
    case H_TAB:
        mTab = (ETab)llclamp(arg, 0, (S32)T_COUNT - 1);
        if (mTab == T_CAST) requestLayers();
        return;
    case H_ADV:
        if (mAdvs.isArray() && mAdvs.size() > 0) { mAdvSel = (mAdvSel + 1) % (S32)mAdvs.size(); stopPossessing(); mPack = LLSD(); mPackFor.clear(); mOverlayDirty = true; loadSessions(); }
        return;
    case H_SESSION:
        if (mSessions.isArray() && mSessions.size() > 1)
        {
            S32 si = 0;
            for (size_t i = 0; i < (size_t)mSessions.size(); ++i) if (mSessions[(LLSD::Integer)i]["session"].asString() == mSession) si = (S32)i;
            mSession = mSessions[(si + 1) % (S32)mSessions.size()]["session"].asString();
            stopPossessing();
            mState = LLSD();
            mOlderLog = LLSD();
            mParty.clear();
            mOverlayDirty = true;
            pollState(true);
        }
        return;
    case H_REFRESH: loadAdventures(); return;
    case H_SCHEDULE:
    {
        LLSD body;
        body["adventure"] = selectedAdventure();
        body["kind"] = "scheduled";
        // BRAIN_API_V1.md §5.5 session_start {adventure, kind:"scheduled", minutes?} (GM)
        call("session_start", body, [](WolfAdventureDeskView* d, const LLSD& reply)
        {
            WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
            self->setStatus("A scheduled session is open. Players join it from Adventures here.", false);
            self->loadSessions();
        }, "Starting a session...");
        return;
    }
    case H_SESSION_END:
        ask("End this session?", "Every party in it ends with \"session over\". The adventure itself stays published.", "End session", true, [this]()
        {
            LLSD body;
            body["session"] = mSession;
            // BRAIN_API_V1.md §5.5 session_end {session}
            call("session_end", body, [](WolfAdventureDeskView* d, const LLSD&)
            {
                WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(d);
                self->setStatus("The session has ended.", false);
                self->stopPossessing();
                self->loadSessions();
            }, "Ending the session...");
        });
        return;
    case H_TAKE:
        ask("Take control?", "The AI Director stands back while you run it. AI characters still talk unless you pause them or possess one. Hand back when you leave.",
            "Take control", false, [this]() { gmOp("take", LLSD::emptyMap(), "you took control"); });
        return;
    case H_LEAVE:
        gmOp("leave", LLSD::emptyMap(), "handed back to the AI Director", [](WolfAdventureGmView* self, const LLSD& reply)
        {
            const LLSD& since = reply["log"]["since"];
            if (since.isMap() && since.size() > 0)
            {
                std::string s;
                for (LLSD::map_const_iterator it = since.beginMap(); it != since.endMap(); ++it) s += (s.empty() ? "" : ", ") + it->first + " " + it->second.asString();
                self->setStatus("Handed back. Since you took over: " + s + ". The AI Director carries on from here.", false);
            }
        });
        return;
    case H_PAUSE: gmOp("pause", LLSD::emptyMap(), "paused"); return;
    case H_RESUME: gmOp("resume", LLSD::emptyMap(), "resumed"); return;
    case H_AI:
    {
        LLSD f;
        f["on"] = mState["session"]["aiPaused"].asInteger() != 0 ? 1 : 0;
        gmOp("ai", f, f["on"].asInteger() ? "AI back on" : "all AI paused");
        return;
    }
    case H_OVERLAY:
    {
        const bool was = mOvZones || mOvPaths || mOvNpcs;
        if (arg == 0) mOvZones = !mOvZones;
        else if (arg == 1) mOvPaths = !mOvPaths;
        else mOvNpcs = !mOvNpcs;
        mOverlayDirty = true;
        if (!was && (mOvZones || mOvPaths || mOvNpcs)) requestLayers();   // fresh layers for the region you are on
        return;
    }
    case H_DIRCAM: setDirectorCamera(!mDirCam); return;
    case H_PARK:
    {
        // STUDIO §3 "Park me offers to teleport the GM's avatar to a spot the pack marks gm-booth, or to 300 m above".
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        LLVector3d dest = gAgent.getPositionGlobal() + LLVector3d(0.0, 0.0, 300.0);
        std::string where = "300 m above where you stand";
        const LLSD& spots = mPack["places"]["spots"];
        for (size_t i = 0; spots.isArray() && i < (size_t)spots.size(); ++i)
        {
            const LLSD& s = spots[(LLSD::Integer)i];
            if (s["id"].asString() != "gm-booth" || s["region"].asString() != rgn->getRegionID().asString() || !s["pos"].isArray()) continue;
            dest = rgn->getPosGlobalFromRegion(LLVector3((F32)s["pos"][0].asReal(), (F32)s["pos"][1].asReal(), (F32)s["pos"][2].asReal()));
            where = "the adventure's host booth";
        }
        ask("Park your avatar?", "You will be teleported to " + where + ", out of the players' way.", "Teleport", false, [dest]() { gAgent.teleportViaLocation(dest); });
        return;
    }
    case H_PARTY:
    {
        const LLSD& ps = mState["parties"];
        if (ps.isArray() && arg >= 0 && arg < (S32)ps.size()) mParty = ps[arg]["party"].asString();
        mNarrateTo = mWhisperTo = mRewardTo = 0;
        return;
    }
    // ---- Story ----
    case H_ADVANCE: { LLSD f; f["party"] = party_id; gmOp("advance", f, "advanced"); return; }
    case H_FIRE:
    {
        const LLSD* beat = party ? packBeat((*party)["beat"].asString()) : nullptr;
        if (!beat || arg < 0 || arg >= (S32)(*beat)["triggers"].size()) return;
        LLSD f;
        f["party"] = party_id;
        f["trigger"] = (*beat)["triggers"][arg]["id"].asString();
        gmOp("fire", f, "fired " + f["trigger"].asString());
        return;
    }
    case H_GOTO_BEAT:
    case H_GOTO_SCENE:
    {
        const LLSD& acts = packList("acts");
        S32 bi = 0, si = 0;
        for (size_t a = 0; a < (size_t)acts.size(); ++a)
        {
            const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
            for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s, ++si)
            {
                if (id == H_GOTO_SCENE && si == arg)
                {
                    LLSD f; f["party"] = party_id; f["scene"] = scenes[(LLSD::Integer)s]["id"].asString();
                    gmOp("goto", f, "moved the party to scene " + f["scene"].asString());
                    return;
                }
                const LLSD& beats = scenes[(LLSD::Integer)s]["beats"];
                for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b, ++bi)
                {
                    if (id == H_GOTO_BEAT && bi == arg)
                    {
                        LLSD f; f["party"] = party_id; f["beat"] = beats[(LLSD::Integer)b]["id"].asString();
                        gmOp("goto", f, "moved the party to beat " + f["beat"].asString());
                        return;
                    }
                }
            }
        }
        return;
    }
    case H_FLAG_TOGGLE:
    case H_FLAG_STEP:
    case H_FLAG_SET:
    {
        const LLSD& flags = mPack["state"]["flags"];
        const S32 fi = id == H_FLAG_SET ? arg : arg / 2;
        if (!flags.isArray() || fi < 0 || fi >= (S32)flags.size()) return;
        const std::string fid = flags[fi]["id"].asString();
        LLSD f;
        f["party"] = party_id;
        f["flag"] = fid;
        if (id == H_FLAG_TOGGLE) f["value"] = (arg % 2) == 1;
        else if (id == H_FLAG_STEP)
        {
            const LLSD& values = party ? (*party)["flags"] : LLSD();
            f["value"] = (LLSD::Integer)(values[fid].asInteger() + ((arg % 2) == 1 ? 1 : -1));
        }
        else f["value"] = text(F_FLAG_TEXT);
        if (!reason.empty()) f["why"] = reason;
        gmOp("set", f, "set " + fid);
        return;
    }
    case H_HONOUR_STEP:
    case H_REP_STEP:
    case H_CASTREP_STEP:
    {
        const S32 by = (arg % 2) == 1 ? 5 : -5;
        const S32 k = id == H_HONOUR_STEP ? arg / 2 : (arg / 2) / 64;
        const S32 which = (arg / 2) % 64;
        if (k < 0 || k >= nmem) return;
        LLSD f;
        f["agent"] = mem[k]["agent"].asString();
        f["by"] = by;
        if (!reason.empty()) f["why"] = reason;
        if (id == H_HONOUR_STEP) { gmOp("honour", f, llformat("honour %+d", by)); return; }
        if (id == H_REP_STEP)
        {
            const LLSD& factions = mPack["state"]["factions"];
            if (!factions.isArray() || which >= (S32)factions.size()) return;
            f["faction"] = factions[which]["id"].asString();
        }
        else
        {
            const LLSD& cast = packList("cast");
            if (which >= (S32)cast.size()) return;
            f["cast"] = cast[which]["id"].asString();
        }
        gmOp("rep", f, llformat("reputation %+d", by));
        return;
    }
    // ---- Cast and spawn ----
    case H_POSSESS:
    case H_RELEASE:
    case H_REMOVE:
    case H_MOVEHERE:
    {
        const LLSD npcs = npcList();
        if (arg < 0 || arg >= (S32)npcs.size()) return;
        const std::string handle = npcs[arg]["handle"].asString();
        const std::string cast = npcs[arg]["cast"].asString();
        LLSD f;
        f["npc"] = handle;
        if (id == H_POSSESS)
        {
            gmOp("possess", f, "you have " + castName(cast), [handle, cast](WolfAdventureGmView* self, const LLSD&)
            {
                self->mPossessHandle = handle;
                self->mPossessCast = cast;
                self->mPossessName = self->castName(cast);
                self->mPsLinesAt = 0.0;
                self->mPsSuggest = LLSD();
            });
        }
        else if (id == H_RELEASE)
        {
            gmOp("release", f, "released " + castName(cast), [handle](WolfAdventureGmView* self, const LLSD&) { if (self->mPossessHandle == handle) self->stopPossessing(); });
        }
        else if (id == H_REMOVE)
        {
            ask("Remove " + castName(cast) + "?", "The character leaves the world for this session. It can be placed again from the spawn palette.", "Remove", true,
                [this, f]() { gmOp("despawn", f, "removed"); });
        }
        else
        {
            pick("move " + castName(cast) + " there", [f](WolfAdventureGmView* self, const LLVector3d& global)
            {
                LLViewerRegion* rgn = gAgent.getRegion();
                if (!rgn) return;
                const LLVector3 v = rgn->getPosRegionFromGlobal(global);
                LLSD g = f;
                LLSD pos = LLSD::emptyArray();
                pos.append(v.mV[VX]); pos.append(v.mV[VY]); pos.append(v.mV[VZ]);
                g["move"]["pos"] = pos;
                self->gmOp("npc", g, "the character is on its way");
            });
        }
        return;
    }
    case H_PLACE_CAST:
    {
        const LLSD& cast = packList("cast");
        if (arg < 0 || arg >= (S32)cast.size()) return;
        const std::string cid = cast[arg]["id"].asString();
        pick("place " + castName(cid), [cid](WolfAdventureGmView* self, const LLVector3d& global)
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!rgn) return;
            const LLVector3 v = rgn->getPosRegionFromGlobal(global);
            LLSD f;
            f["cast"] = cid;
            LLSD pos = LLSD::emptyArray();
            pos.append(v.mV[VX]); pos.append(v.mV[VY]); pos.append(v.mV[VZ]);
            f["pos"] = pos;
            self->gmOp("spawn", f, "placed " + self->castName(cid));
        });
        return;
    }
    case H_FIRE_ENC:
    {
        const LLSD& enc = packList("encounters");
        if (arg < 0 || arg >= (S32)enc.size()) return;
        LLSD f;
        f["encounter"] = enc[arg]["id"].asString();
        gmOp("spawn", f, "fired the encounter");
        return;
    }
    // ---- possession ----
    case H_PS_RELEASE: { LLSD f; f["npc"] = mPossessHandle; gmOp("release", f, "released " + mPossessName, [](WolfAdventureGmView* self, const LLSD&) { self->stopPossessing(); }); return; }
    case H_PS_USE:
        if (arg >= 0)
        {
            if (mPsSuggest.isArray() && arg < (S32)mPsSuggest.size()) text(F_PS_SAY) = cap(mPsSuggest[arg]["text"].asString(), 500);
            mField = F_PS_SAY;
            return;
        }
        else
        {
            // HUD_MESSAGES_V1.md §4 ["gm_say", npc, text...] straight to the region, under the possession lease
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!rgn || text(F_PS_SAY).empty()) return;
            const LLUUID npc = WolfAdventureDesk::npcAgent(rgn->getRegionID(), mPossessHandle);
            if (WolfAdventure::instance().sendGmSay(npc, text(F_PS_SAY)))
            {
                setStatus(mPossessName + " says it.", false);
                text(F_PS_SAY).clear();
            }
            else setStatus("It could not be said: you must be on the character's region (and the region must be Wolf Territories).", true);
        }
        return;
    case H_PS_MOVE:
        mClickMove = !mClickMove;
        if (!mClickMove) { WolfToolAdventurePick::instance().stop(); setStatus("Click to move off.", false); return; }
        {
            LLHandle<LLUICtrl> handle = getHandle();
            setStatus("Click to move on: click the ground and " + mPossessName + " walks there. Escape stops.", false);
            WolfToolAdventurePick::instance().start([handle](const LLVector3d& global)
            {
                if (handle.isDead()) return;
                WolfAdventureGmView* self = static_cast<WolfAdventureGmView*>(handle.get());
                LLViewerRegion* rgn = gAgent.getRegion();
                if (!rgn || self->mPossessHandle.empty()) return;
                const LLUUID npc = WolfAdventureDesk::npcAgent(rgn->getRegionID(), self->mPossessHandle);
                // §4 ["gm_move", npc, x, y, z, gait]
                if (!WolfAdventure::instance().sendGmMove(npc, rgn->getPosRegionFromGlobal(global), GAITS[self->mGait]))
                    self->setStatus("The move could not be sent: you must be on the character's region.", true);
            }, true);
        }
        return;
    case H_PS_GAIT: mGait = (mGait + 1) % 3; return;
    case H_PS_ANIM_PLAY:
    case H_PS_ANIM_STOP:
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        const LLUUID npc = WolfAdventureDesk::npcAgent(rgn->getRegionID(), mPossessHandle);
        // §4 ["gm_anim", npc, anim, "0"|"1"]
        if (!WolfAdventure::instance().sendGmAnim(npc, text(F_PS_ANIM), id == H_PS_ANIM_PLAY))
            setStatus("The animation could not be sent: you must be on the character's region.", true);
        return;
    }
    case H_PS_SUGGEST:
    {
        LLSD f;
        f["ask"] = "reply";
        f["context"]["cast"] = mPossessCast;
        f["context"]["party"] = party_id;
        for (size_t i = 0; mPsLines.isArray() && i < (size_t)mPsLines.size(); ++i)
        {
            if (mPsLines[(LLSD::Integer)i]["who"].asString() == "player") { f["context"]["text"] = mPsLines[(LLSD::Integer)i]["text"].asString(); break; }
        }
        gmOp("assist", f, "the assistant answered", [](WolfAdventureGmView* self, const LLSD& reply) { self->mPsSuggest = reply["suggestions"]; });
        return;
    }
    // ---- Narrate ----
    case H_NARRATE_ADDOPT: mNarrateOpts = llmin(6, mNarrateOpts + 1); return;
    case H_NARRATE_DELOPT:
        for (S32 i = arg; i + 1 < mNarrateOpts; ++i) text(F_OPT0 + i) = text(F_OPT0 + i + 1);
        text(F_OPT0 + mNarrateOpts - 1).clear();
        mNarrateOpts = llmax(0, mNarrateOpts - 1);
        return;
    case H_NARRATE_TO: mNarrateTo = nmem > 0 ? (mNarrateTo + 1) % (nmem + 1) : 0; return;
    case H_NARRATE_SEND:
    {
        LLSD f;
        if (mNarrateTo > 0 && mNarrateTo <= nmem) f["agent"] = mem[mNarrateTo - 1]["agent"].asString();
        else f["party"] = party_id;
        if (!text(F_NARRATE_TITLE).empty()) f["title"] = text(F_NARRATE_TITLE);
        f["text"] = text(F_NARRATE_TEXT);
        if (LLUUID::validate(text(F_NARRATE_IMAGE))) f["image"] = text(F_NARRATE_IMAGE);
        LLSD opts = LLSD::emptyArray();
        for (S32 i = 0; i < mNarrateOpts; ++i)
        {
            if (text(F_OPT0 + i).empty()) continue;
            LLSD o;
            o["id"] = llformat("o%d", i + 1);
            o["label"] = text(F_OPT0 + i);
            opts.append(o);
        }
        if (opts.size() > 0) f["options"] = opts;
        gmOp("narrate", f, "card sent", [](WolfAdventureGmView* self, const LLSD&) { self->text(F_NARRATE_TEXT).clear(); self->text(F_NARRATE_TITLE).clear(); });
        return;
    }
    case H_WHISPER_TO: mWhisperTo = nmem > 0 ? mWhisperTo % nmem + 1 : 0; return;
    case H_WHISPER_SEND:
    {
        if (mWhisperTo <= 0 || mWhisperTo > nmem || text(F_WHISPER).empty()) return;
        LLSD f;
        f["agent"] = mem[mWhisperTo - 1]["agent"].asString();
        f["text"] = text(F_WHISPER);
        gmOp("whisper", f, "whispered", [](WolfAdventureGmView* self, const LLSD&) { self->text(F_WHISPER).clear(); });
        return;
    }
    // ---- Party ----
    case H_GATHER:
    {
        const LLSD& spots = mPack["places"]["spots"];
        if (!spots.isArray() || arg < 0 || arg >= (S32)spots.size()) return;
        LLSD f;
        f["party"] = party_id;
        f["spot"] = spots[arg]["id"].asString();
        gmOp("gather", f, "offers sent");
        return;
    }
    case H_READY_CHECK:
    {
        // A narration card with two options; each answer comes back as a `choose` event (BRAIN §8.1 narrate).
        LLSD f;
        f["party"] = party_id;
        f["title"] = "Ready?";
        f["text"] = "Your host asks: are you ready to go on?";
        LLSD opts = LLSD::emptyArray();
        LLSD a; a["id"] = "ready"; a["label"] = "Ready"; opts.append(a);
        LLSD b; b["id"] = "notyet"; b["label"] = "Not yet"; opts.append(b);
        f["options"] = opts;
        gmOp("narrate", f, "ready check sent; answers appear in the log");
        return;
    }
    case H_END_PICK: { const S32 n = (S32)packList("endings").size(); mEndPick = (mEndPick + 1) % (n + 1); return; }
    case H_END_PARTY:
        ask("End the story for this party?", "They see the ending (or \"session over\") and their adventure is finished.", "End for the party", true, [this, party_id]()
        {
            LLSD f;
            f["party"] = party_id;
            const LLSD& endings = packList("endings");
            if (mEndPick > 0 && mEndPick <= (S32)endings.size()) f["ending"] = endings[mEndPick - 1]["id"].asString();
            gmOp("end", f, "ended for the party");
        });
        return;
    case H_KICK:
        if (arg < 0 || arg >= nmem) return;
        {
            const std::string agent = mem[arg]["agent"].asString(), name = cap(mem[arg]["name"].asString(), 64);
            ask("Remove " + name + "?", "They leave the party and this session. They can join again later.", "Remove", true, [this, agent]()
            {
                LLSD f;
                f["agent"] = agent;
                gmOp("kick", f, "removed from the session");
            });
        }
        return;
    // ---- Rewards ----
    case H_REWARD_TO: mRewardTo = nmem > 0 ? (mRewardTo + 1) % (nmem + 1) : 0; return;
    case H_REWARD_PARCEL:
    case H_REWARD_SEND:
    {
        LLSD f;
        if (mRewardTo > 0 && mRewardTo <= nmem) f["agent"] = mem[mRewardTo - 1]["agent"].asString();
        else f["party"] = party_id;
        if (id == H_REWARD_PARCEL)
        {
            const LLSD& parcels = packList("parcels");
            if (arg < 0 || arg >= (S32)parcels.size()) return;
            f["parcel"] = parcels[arg]["id"].asString();
        }
        else
        {
            // BRAIN §8.1 reward {items, money, stats}
            const S32 xp = atoi(text(F_REWARD_XP).c_str()), money = atoi(text(F_REWARD_MONEY).c_str()), qty = llmax(1, atoi(text(F_REWARD_QTY).c_str()));
            if (xp > 0) f["stats"]["xp"] = xp;
            if (money > 0) f["money"] = money;
            std::string item = text(F_REWARD_ITEM);
            LLStringUtil::toLower(item);
            if (!item.empty()) f["items"][item] = qty;
            if (xp <= 0 && money <= 0 && item.empty()) { setStatus("Enter XP, money or an item to give.", true); return; }
        }
        if (!reason.empty()) f["why"] = reason;
        gmOp("reward", f, "reward given");
        return;
    }
    // ---- World ----
    case H_HOUR_STEP: mHour = (mHour + (arg == 1 ? 1 : 23)) % 24; return;
    case H_HOUR_SET:
    case H_WEATHER:
    {
        LLSD f;
        f["region"] = partyRegion();
        // Source: wolfstorm/php/wolf_env_lib.php:436-456 time {mode:"fixed", hour 0..24}; :600-621 weather
        // {source:"fixed", type} with type one of WE_TYPES (:55). The brain writes it through wolf_env_lib.php.
        if (id == H_HOUR_SET) { f["environment"]["time"]["mode"] = "fixed"; f["environment"]["time"]["hour"] = mHour; }
        else { f["environment"]["weather"]["source"] = "fixed"; f["environment"]["weather"]["type"] = WEATHERS[llclamp(arg, 0, 7)]; }
        gmOp("world", f, id == H_HOUR_SET ? "time set" : "weather set");
        return;
    }
    case H_MUSIC_PLAY: { LLSD f; f["party"] = party_id; f["url"] = text(F_MUSIC); gmOp("music", f, "music playing"); return; }
    case H_MUSIC_STOP: { LLSD f; f["party"] = party_id; f["stop"] = 1; gmOp("music", f, "music stopped"); return; }
    // ---- Rules ----
    case H_DICE_PUBLIC: mDicePublic = !mDicePublic; return;
    case H_DICE_ROLL:
    case H_CHECK_ASK:
    {
        // Source: wolfadventure_gm_lib.php wag_roll {party, check, dc?, public, secret}
        LLSD f;
        f["party"] = party_id;
        f["check"] = text(F_CHECK);
        if (!text(F_DC).empty()) f["dc"] = atoi(text(F_DC).c_str());
        f["public"] = mDicePublic ? 1 : 0;
        if (id == H_DICE_ROLL) f["secret"] = 1;
        gmOp("roll", f, id == H_DICE_ROLL ? "rolled in secret (the dice show to you)" : "roll requested");
        return;
    }
    case H_COMBAT:
    {
        // Source: wolfadventure_gm_lib.php wag_combat_op: `do` start | next | end.
        static const char* const STEPS[3] = { "start", "next", "end" };
        LLSD f;
        f["party"] = party_id;
        f["do"] = STEPS[llclamp(arg, 0, 2)];
        gmOp("combat", f, std::string("combat ") + STEPS[llclamp(arg, 0, 2)]);
        return;
    }
    case H_BATTLE_START:
    {
        // Source: wolfadventure_gm_lib.php wag_combat_op -> wab_gm_op start {battle}
        const LLSD& battles = packList("battles");
        if (!battles.isArray() || arg < 0 || arg >= (S32)battles.size()) return;
        LLSD f;
        f["party"] = party_id;
        f["do"] = "start";
        f["battle"] = battles[arg]["id"].asString();
        gmOp("combat", f, "started the fight " + f["battle"].asString());
        return;
    }
    case H_ENEMY_MOVE:
    case H_ENEMY_HIT:
    {
        // Source: wolfadventure_combat_lib.php wab_gm_op enemy_act {npc, move, target?}: the fighter whose turn it is
        // (npc = its key, refused if the turn moved on); a target for attack
        const LLSD fight = party ? (*party)["fight"] : LLSD();
        if (!fight.isMap()) return;
        LLSD f;
        f["party"] = party_id;
        f["do"] = "enemy_act";
        f["npc"] = fight["current"].asString();
        if (id == H_ENEMY_MOVE) f["move"] = arg == 1 ? "flee" : "defend";
        else
        {
            if (arg < 0 || arg >= (S32)fight["fighters"].size()) return;
            f["move"] = "attack";
            f["target"] = fight["fighters"][arg]["key"].asString();
        }
        gmOp("combat", f, std::string("played ") + fight["currentName"].asString() + "'s move: " + f["move"].asString());
        return;
    }
    case H_OVERRIDE:
    {
        const LLSD rolls = rollList();
        const S32 ri = arg / 4, oi = arg % 4;
        if (ri >= (S32)rolls.size()) return;
        LLSD f;
        f["roll"] = rolls[ri]["id"].asString();
        f["outcome"] = OUTCOMES[oi];
        gmOp("override", f, std::string("set to ") + OUTCOMES[oi]);
        return;
    }
    case H_FIND_NPCS: requestLayers(); setStatus("Asked the story for the characters on your region.", false); return;
    // ---- Assist ----
    case H_ASSIST_CAST: { const S32 n = (S32)packList("cast").size(); if (n > 0) mAssistCast = (mAssistCast + 1) % n; return; }
    case H_ASSIST:
    {
        static const char* const ASKS[3] = { "next", "boxed", "reply" };
        mAssistAsk = llclamp(arg, 0, 2);
        LLSD f;
        f["ask"] = ASKS[mAssistAsk];
        f["context"]["party"] = party_id;
        if (party) f["context"]["scene"] = (*party)["scene"].asString();
        if (mAssistAsk == 2)
        {
            const LLSD& cast = packList("cast");
            if (cast.size() == 0) return;
            f["context"]["cast"] = cast[llclamp(mAssistCast, 0, (S32)cast.size() - 1)]["id"].asString();
            if (!text(F_ASSIST_TEXT).empty()) f["context"]["text"] = text(F_ASSIST_TEXT);
        }
        gmOp("assist", f, "the assistant answered", [](WolfAdventureGmView* self, const LLSD& reply)
        {
            self->mSuggest = reply["suggestions"];
            const LLSD& c = reply["cost"];
            self->mSuggestCost = c.isMap() ? llformat("Cost %.2f credits.", c["credits"].asReal()) : std::string();
        });
        return;
    }
    case H_ASSIST_COPY:
    case H_ASSIST_USE:
    {
        if (!mSuggest.isArray() || arg < 0 || arg >= (S32)mSuggest.size()) return;
        const std::string s = mSuggest[arg]["text"].asString();
        if (id == H_ASSIST_COPY)
        {
            LLClipboard::instance().copyToClipboard(utf8str_to_wstring(s), 0, (S32)utf8str_to_wstring(s).size());
            setStatus("Copied.", false);
        }
        else if (mAssistAsk == 2 && !mPossessHandle.empty()) { text(F_PS_SAY) = cap(s, 500); setStatus("In the say box - edit it, then Speak.", false); }
        else { text(F_NARRATE_TEXT) = cap(s, 2000); mTab = T_NARRATE; setStatus("In the narration card - edit it, then Send.", false); }
        return;
    }
    // ---- the log ----
    case H_LOG_FILTER: mLogFilter = llclamp(arg, 0, LOG_FILTER_COUNT - 1); return;
    case H_LOG_OLDER: loadLogOlder(); return;
    case H_CONVO:
        mConvoOn = !mConvoOn;
        if (mConvoOn) loadConversations();
        return;
    default: return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventureGm::WolfFloaterAdventureGm(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureGm::postBuild()
{
    mView = getChild<WolfAdventureGmView>("gm");
    setBackgroundVisible(false);   // in front of the drag handle, as WolfFloaterRoleplay::postBuild explains
    return true;
}

void WolfFloaterAdventureGm::onOpen(const LLSD& key)
{
    // Wolf Territories only (HUD_MESSAGES_V1.md §5.1; the service also refuses any other grid)
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    if (mView)
    {
        mView->onOpened(key);
        mView->setFocus(true);
    }
}

void WolfFloaterAdventureGm::onClose(bool app_quitting)
{
    if (mView) mView->onClosed();
    LLFloater::onClose(app_quitting);
}

void WolfFloaterAdventureGm::draw()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}
