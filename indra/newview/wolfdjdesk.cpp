/**
 * @file wolfdjdesk.cpp
 * @brief WolfViewer Wolf DJ: the mixing desk. See wolfdjdesk.h.
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

#include "wolfdjdesk.h"

#include <algorithm>
#include <cmath>

#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender.h"
#include "lltooltip.h"
#include "llui.h"
#include "llwindow.h"
#include "wolfdjplayer.h"

static LLDefaultChildRegistry::Register<WolfDJDesk> r_wolf_dj_desk("wolf_dj_desk");

using namespace WolfDJ;

namespace
{
    const F32 PI_F = 3.14159265f;

    // Desk colours: a dark charcoal surface, lighter strip edges, and the lamp colours of a
    // broadcast desk (Sonifex S2: routing lit green, CUE/PFL lit; red for mute and on air).
    const LLColor4 C_PANEL_TOP(0.17f, 0.18f, 0.20f, 1.f);
    const LLColor4 C_PANEL_BOT(0.10f, 0.10f, 0.12f, 1.f);
    const LLColor4 C_EDGE(0.30f, 0.31f, 0.34f, 1.f);
    const LLColor4 C_SHADOW(0.04f, 0.04f, 0.05f, 1.f);
    const LLColor4 C_LCD(0.05f, 0.09f, 0.10f, 1.f);
    const LLColor4 C_LCD_TEXT(0.55f, 0.95f, 0.90f, 1.f);
    const LLColor4 C_TEXT(0.86f, 0.87f, 0.89f, 1.f);
    const LLColor4 C_DIM(0.52f, 0.54f, 0.58f, 1.f);
    const LLColor4 C_SCALE(0.70f, 0.72f, 0.75f, 1.f);
    const LLColor4 C_GREEN(0.20f, 0.92f, 0.32f, 1.f);
    const LLColor4 C_AMBER(1.00f, 0.70f, 0.08f, 1.f);
    const LLColor4 C_RED(1.00f, 0.16f, 0.12f, 1.f);
    const LLColor4 C_WHITE(0.97f, 0.97f, 0.97f, 1.f);
    // Knob caps, one colour per job as on a desk's channel strip.
    const LLColor4 C_KNOB_HF(0.30f, 0.55f, 1.00f, 1.f);
    const LLColor4 C_KNOB_MF(0.25f, 0.85f, 0.40f, 1.f);
    const LLColor4 C_KNOB_LF(0.95f, 0.30f, 0.25f, 1.f);
    const LLColor4 C_KNOB_FX(0.72f, 0.45f, 0.95f, 1.f);
    // Fader caps: grey for music, a colour for the special channels, red for the master (X32).
    const LLColor4 C_CAP_MUSIC(0.78f, 0.79f, 0.81f, 1.f);
    const LLColor4 C_CAP_VOICE(0.35f, 0.75f, 0.85f, 1.f);
    const LLColor4 C_CAP_MIC(1.00f, 0.60f, 0.15f, 1.f);
    const LLColor4 C_CAP_JINGLE(0.75f, 0.50f, 0.95f, 1.f);
    const LLColor4 C_CAP_MASTER(0.92f, 0.18f, 0.15f, 1.f);

    // ---- layout (desk-local; offsets are from the desk's top) ----
    const S32 STRIP_W = 84;
    const S32 STRIP_X0 = 6;
    const S32 MASTER_L = STRIP_X0 + 7 * STRIP_W;            // after 6 channels and the jingles
    const S32 MASTER_R = WolfDJDesk::WIDTH - 6;
    const S32 BRIDGE_T = 4, BRIDGE_B = 80;
    const S32 NAME_T = 86, NAME_B = 102;
    const S32 SRC_T = 104, SRC_B = 124;
    const S32 EQ_Y[3] = { 142, 172, 202 };                  // knob centres: HF, MF, LF
    const S32 FXTYPE_T = 220, FXTYPE_B = 234;
    const S32 FX_Y = 254;
    const S32 BTN1_T = 274, BTN1_B = 290;
    const S32 BTN2_T = 294, BTN2_B = 310;
    const S32 TRAVEL_T = 326, TRAVEL_B = 456;               // fader travel (cap centre)
    const S32 READ_T = 466, READ_B = 482;
    const F32 KNOB_R = 12.f;
    const S32 CAP_W = 26, CAP_H = 14;

    const char* STRIP_NAME[WolfDJDesk::STRIP_MASTER + 1] = { "VOICE", "MIC", "MUSIC 1", "MUSIC 2", "MUSIC 3", "MUSIC 4", "JINGLES", "MASTER" };
    const char* FX_NAME[FX_COUNT] = { "NO FX", "ECHO", "REVERB", "RADIO" };

    // What counts as "something playing" for the flashing fader: -50 dBFS.
    const float SIGNAL = 0.00316f;
    // A fader at or below -60 dB is "down".
    const float FADER_DOWN = 0.0625f;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    bool blink(F32 period) { return fmod(now(), (F64)period) < period * 0.5f; }
    LLColor4 scaled(const LLColor4& c, F32 k) { return LLColor4(c.mV[VRED] * k, c.mV[VGREEN] * k, c.mV[VBLUE] * k, c.mV[VALPHA]); }

    // ---- primitives, view-local, untextured (after wolfdashboard.cpp's) ----
    void noTex() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }
    void vtx(F32 x, F32 y, const LLColor4& c) { gGL.color4fv(c.mV); gGL.vertex2f(x, y); }

    void rectF(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(r, b, c); vtx(r, t, c);
        vtx(l, b, c); vtx(r, t, c); vtx(l, t, c);
        gGL.end();
    }

    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bot)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bot); vtx(r, b, bot); vtx(r, t, top);
        vtx(l, b, bot); vtx(r, t, top); vtx(l, t, top);
        gGL.end();
    }

    void frame(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        rectF(l, b, r, b + 1, c);
        rectF(l, t - 1, r, t, c);
        rectF(l, b, l + 1, t, c);
        rectF(r - 1, b, r, t, c);
    }

    // A recessed panel: dark inside, a shadow along the top and left, a highlight bottom and right.
    void inset(F32 l, F32 b, F32 r, F32 t, const LLColor4& fill)
    {
        rectF(l, b, r, t, fill);
        rectF(l, t - 1, r, t, C_SHADOW);
        rectF(l, b, l + 1, t, C_SHADOW);
        rectF(l, b, r, b + 1, C_EDGE);
        rectF(r - 1, b, r, t, C_EDGE);
    }

    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 32)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(cx, cy, c);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }

    // A lit dome, brighter toward the upper left (wolfdashboard.cpp circleShaded).
    void dome(F32 cx, F32 cy, F32 rad, const LLColor4& hi, const LLColor4& lo, S32 seg = 32)
    {
        noTex();
        const F32 hx = cx - rad * 0.3f, hy = cy + rad * 0.3f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * i / seg, a1 = 2.f * PI_F * (i + 1) / seg;
            vtx(hx, hy, hi);
            vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), lo);
            vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), lo);
        }
        gGL.end();
    }

    // A stroked arc, angles in radians counter-clockwise from +x.
    void arc(F32 cx, F32 cy, F32 rad, F32 w, F32 a0, F32 a1, const LLColor4& c, S32 seg = 24)
    {
        noTex();
        const F32 h = w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
            const F32 c0 = cosf(u0), s0 = sinf(u0), c1 = cosf(u1), s1 = sinf(u1);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c0, cy + (rad + h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c);
            vtx(cx + (rad - h) * c0, cy + (rad - h) * s0, c); vtx(cx + (rad + h) * c1, cy + (rad + h) * s1, c); vtx(cx + (rad - h) * c1, cy + (rad - h) * s1, c);
        }
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

    void triF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
        gGL.end();
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER, LLFontGL::VAlign v = LLFontGL::VCENTER, S32 max_px = S32_MAX)
    {
        if (!f || s.empty()) return;
        f->renderUTF8(s, 0, x, y, c, h, v, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, max_px, nullptr, true);
    }

    const LLFontGL* fontSmall() { return LLFontGL::getFontSansSerifSmall(); }
    const LLFontGL* fontBold() { return LLFontGL::getFontSansSerifBold(); }
    const LLFontGL* fontBig() { return LLFontGL::getFontSansSerifBig(); }
    const LLFontGL* fontMono() { return LLFontGL::getFontMonospace(); }

    std::string clock_text(double secs)
    {
        const S32 s = (S32)std::max(0.0, secs);
        return s >= 3600 ? llformat("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) : llformat("%d:%02d", s / 60, s % 60);
    }

    std::string db_text(float db)
    {
        if (db <= -89.5f) return "-inf";
        return llformat("%+.1f", db);
    }

    // EQ knobs turn round 12 o'clock at 0 dB: -24..0 dB on the left half, 0..+12 on the right.
    float eq_to_knob(float db) { return db < 0.f ? 0.5f + 0.5f * db / -EQ_MIN_DB : 0.5f + 0.5f * db / EQ_MAX_DB; }
    float knob_to_eq(float v) { return v < 0.5f ? (0.5f - v) * 2.f * EQ_MIN_DB : (v - 0.5f) * 2.f * EQ_MAX_DB; }
    float eq_step(float db) { return roundf(db * 2.f) / 2.f + 0.f; }    // 0.5 dB steps; + 0 turns -0 into 0

    bool is_channel(int strip) { return strip >= 0 && strip < CH_COUNT; }

    WolfDJChannel* strip_channel(int strip)
    {
        return is_channel(strip) ? &WolfDJMixer::instance().channel(strip) : nullptr;
    }

    LLColor4 cap_colour(int strip)
    {
        switch (strip)
        {
        case CH_VOICE: return C_CAP_VOICE;
        case CH_MIC: return C_CAP_MIC;
        case WolfDJDesk::STRIP_JINGLE: return C_CAP_JINGLE;
        case WolfDJDesk::STRIP_MASTER: return C_CAP_MASTER;
        default: return C_CAP_MUSIC;
        }
    }

    // The strip the playlist plays on, or -1.
    int playlist_strip()
    {
        WolfDJMixer& mix = WolfDJMixer::instance();
        WolfDJChannel* out = WolfDJPlayer::instance().output();
        for (int ch = 0; ch < CH_COUNT && out; ++ch)
        {
            if (&mix.channel(ch) == out) return ch;
        }
        return -1;
    }
}

WolfDJDesk::WolfDJDesk(const Params& p)
:   LLUICtrl(p)
{
    mShownDb.fill(-120.f);
    mShownPreDb.fill(-120.f);
    mClipUntil.fill(0.f);
    mSpecDb.fill(-120.f);
    mSpecPeakDb.fill(-120.f);
    mSpecPeakUntil.fill(0.f);
}

WolfDJDesk::~WolfDJDesk()
{
}

LLRect WolfDJDesk::sourceRect(int ch) const
{
    const S32 l = STRIP_X0 + ch * STRIP_W + 4;
    return LLRect(l, HEIGHT - SRC_T, l + STRIP_W - 8, HEIGHT - SRC_B);
}

void WolfDJDesk::addHit(S32 l, S32 b, S32 r, S32 t, EKind kind, int strip, const std::string& tip, S32 travel_b, S32 travel_t)
{
    Hit h;
    h.mL = std::min(l, r);
    h.mB = std::min(b, t);
    h.mR = std::max(l, r);
    h.mT = std::max(b, t);
    h.mKind = kind;
    h.mStrip = strip;
    h.mTip = tip;
    h.mTravelB = travel_b;
    h.mTravelT = travel_t;
    mHits.push_back(h);
}

const WolfDJDesk::Hit* WolfDJDesk::hitAt(S32 x, S32 y) const
{
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (x >= it->mL && x <= it->mR && y >= it->mB && y <= it->mT)
        {
            return &*it;
        }
    }
    return nullptr;
}

// ---- values ----

float WolfDJDesk::value(EKind kind, int strip) const
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    WolfDJChannel* ch = strip_channel(strip);
    switch (kind)
    {
    case K_FADER:
        if (ch) return ch->mFader.load();
        if (strip == STRIP_JINGLE) return mix.mJingleLevel.load();
        return 0.f;
    case K_MASTER_L: return mix.mMasterFaderL.load();
    case K_MASTER_R: return mix.mMasterFaderR.load();
    case K_EQ_HIGH: return ch ? eq_to_knob(ch->mEqHighDb.load()) : 0.5f;
    case K_EQ_MID: return ch ? eq_to_knob(ch->mEqMidDb.load()) : 0.5f;
    case K_EQ_LOW: return ch ? eq_to_knob(ch->mEqLowDb.load()) : 0.5f;
    case K_FX_AMOUNT: return ch ? ch->mFxAmount.load() : 0.f;
    default: return 0.f;
    }
}

void WolfDJDesk::setControlValue(EKind kind, int strip, float v)
{
    v = llclamp(v, 0.f, 1.f);
    WolfDJMixer& mix = WolfDJMixer::instance();
    WolfDJChannel* ch = strip_channel(strip);
    switch (kind)
    {
    case K_FADER:
        if (ch) ch->mFader = v;
        else if (strip == STRIP_JINGLE) mix.mJingleLevel = v;
        break;
    case K_MASTER_L: mix.mMasterFaderL = v; break;
    case K_MASTER_R: mix.mMasterFaderR = v; break;
    // EQ steps of 0.5 dB, so a knob comes back to exactly 0 (the mixer skips the filters then).
    case K_EQ_HIGH: if (ch) ch->mEqHighDb = eq_step(knob_to_eq(v)); break;
    case K_EQ_MID: if (ch) ch->mEqMidDb = eq_step(knob_to_eq(v)); break;
    case K_EQ_LOW: if (ch) ch->mEqLowDb = eq_step(knob_to_eq(v)); break;
    case K_FX_AMOUNT: if (ch) ch->mFxAmount = v; break;
    default: break;
    }
}

float WolfDJDesk::defaultValue(EKind kind) const
{
    switch (kind)
    {
    case K_FADER: case K_MASTER_L: case K_MASTER_R: return wolfdj_fader_pos(0.f);    // 0 dB
    case K_FX_AMOUNT: return 0.35f;             // WolfDJChannel::mFxAmount's default
    default: return 0.5f;                       // EQ flat
    }
}

void WolfDJDesk::toggle(EKind kind, int strip)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    WolfDJChannel* ch = strip_channel(strip);
    switch (kind)
    {
    case K_MON:
        if (ch) ch->mMonitor = !ch->mMonitor.load();
        else if (strip == STRIP_JINGLE) mix.mJingleMonitor = !mix.mJingleMonitor.load();
        // <WolfViewer 2026-10-10/> Paul: "the mon buttons turn on but not off" - each press, and what it left
        LL_INFOS("WolfDJ") << "MON pressed on strip " << strip << ": now "
                           << ((ch ? ch->mMonitor.load() : mix.mJingleMonitor.load()) ? "on" : "off") << LL_ENDL;
        break;
    case K_CUE:
    {
        const int cue = strip == STRIP_MASTER ? CUE_MASTER : strip;
        mix.mCue = mix.mCue.load() == cue ? CUE_NONE : cue;
        break;
    }
    case K_MUTE:
        if (ch) ch->mMute = !ch->mMute.load();
        break;
    case K_LINK:
        mLinked = !mLinked;
        if (mLinked)
        {
            mix.mMasterFaderR = mix.mMasterFaderL.load();   // linking brings the sides together
        }
        break;
    case K_FX_TYPE:
        if (ch) ch->mFx = (ch->mFx.load() + 1) % FX_COUNT;
        break;
    default:
        break;
    }
}

// Scroll wheel: 1 dB a click for faders and EQ, 5% for an effect.
void WolfDJDesk::nudge(const Hit& h, S32 clicks)
{
    const float step = (float)-clicks;      // wheel up is a negative click count
    auto fader = [&](float pos)
    {
        float db = wolfdj_fader_db(pos);
        if (db <= -90.f) db = step > 0.f ? -60.f : -120.f;
        else db += step;
        return db < -60.f ? 0.f : wolfdj_fader_pos(std::min(db, 10.f));
    };
    WolfDJChannel* ch = strip_channel(h.mStrip);
    switch (h.mKind)
    {
    case K_FADER:
        setControlValue(K_FADER, h.mStrip, fader(value(K_FADER, h.mStrip)));
        break;
    case K_MASTER_L:
    case K_MASTER_R:
    {
        const float before = value(h.mKind, h.mStrip);
        const float after = fader(before);
        setControlValue(h.mKind, h.mStrip, after);
        if (mLinked)
        {
            const EKind other = h.mKind == K_MASTER_L ? K_MASTER_R : K_MASTER_L;
            setControlValue(other, h.mStrip, value(other, h.mStrip) + (after - before));
        }
        break;
    }
    case K_EQ_HIGH: if (ch) ch->mEqHighDb = llclamp(ch->mEqHighDb.load() + step, EQ_MIN_DB, EQ_MAX_DB); break;
    case K_EQ_MID: if (ch) ch->mEqMidDb = llclamp(ch->mEqMidDb.load() + step, EQ_MIN_DB, EQ_MAX_DB); break;
    case K_EQ_LOW: if (ch) ch->mEqLowDb = llclamp(ch->mEqLowDb.load() + step, EQ_MIN_DB, EQ_MAX_DB); break;
    case K_FX_AMOUNT: if (ch) ch->mFxAmount = llclamp(ch->mFxAmount.load() + step * 0.05f, 0.f, 1.f); break;
    case K_FX_TYPE: if (ch) ch->mFx = (ch->mFx.load() + (step > 0.f ? 1 : FX_COUNT - 1)) % FX_COUNT; break;
    default: return;
    }
    onCommit();
}

// ---- drawing ----

void WolfDJDesk::drawKnob(S32 cx, S32 cy, F32 r, float v, const LLColor4& cap, bool centred)
{
    // 7 o'clock to 5 o'clock, like a pot's 270 degree travel.
    const F32 a_min = PI_F * 1.25f, a_max = -PI_F * 0.25f;
    const F32 a = a_min + (a_max - a_min) * llclamp(v, 0.f, 1.f);
    arc((F32)cx, (F32)cy, r + 4.f, 2.f, a_max, a_min, scaled(C_EDGE, 1.2f));           // the scale ring
    const F32 a_from = centred ? PI_F * 0.5f : a_min;
    if (fabsf(a - a_from) > 0.01f)
    {
        arc((F32)cx, (F32)cy, r + 4.f, 2.f, std::min(a, a_from), std::max(a, a_from), cap);   // the value
    }
    if (centred)
    {
        lineW((F32)cx, (F32)cy + r + 2.f, (F32)cx, (F32)cy + r + 6.f, 1.5f, C_SCALE);   // the 0 dB detent
    }
    circleF((F32)cx, (F32)cy - 1.5f, r + 0.5f, C_SHADOW);
    dome((F32)cx, (F32)cy, r, LLColor4(0.36f, 0.37f, 0.40f, 1.f), LLColor4(0.12f, 0.12f, 0.14f, 1.f));
    circleF((F32)cx, (F32)cy, r * 0.55f, scaled(cap, 0.85f));                           // the coloured cap
    lineW((F32)cx + cosf(a) * r * 0.25f, (F32)cy + sinf(a) * r * 0.25f, (F32)cx + cosf(a) * (r - 1.f), (F32)cy + sinf(a) * (r - 1.f), 2.f, C_WHITE);
}

void WolfDJDesk::drawFader(S32 cx, S32 b, S32 t, float pos, const LLColor4& cap, bool flash, bool scale_left)
{
    // The scale: a desk's dB marks, at their real places on this fader's law.
    static const float MARKS[] = { 10.f, 5.f, 0.f, -5.f, -10.f, -20.f, -30.f, -40.f, -60.f };
    const F32 travel = (F32)(t - b);
    for (float db : MARKS)
    {
        const F32 y = b + travel * wolfdj_fader_pos(db);
        const bool unity = db == 0.f;
        const F32 x0 = scale_left ? cx - 17.f : cx + 6.f;
        const F32 x1 = scale_left ? cx - 6.f : cx + 17.f;
        rectF(x0, y - (unity ? 1.f : 0.5f), x1, y + (unity ? 1.f : 0.5f), unity ? C_WHITE : C_SCALE);
        const std::string label = db > 0.f ? llformat("+%.0f", db) : llformat("%.0f", db);
        if (scale_left)
        {
            text(fontSmall(), label, x0 - 2.f, y, unity ? C_WHITE : C_DIM, LLFontGL::RIGHT, LLFontGL::VCENTER);
        }
    }
    const F32 yb = (F32)b;
    rectF(scale_left ? cx - 17.f : cx + 6.f, yb - 0.5f, scale_left ? cx - 6.f : cx + 17.f, yb + 0.5f, C_SCALE);
    if (scale_left)
    {
        text(fontSmall(), "-inf", cx - 19.f, yb, C_DIM, LLFontGL::RIGHT, LLFontGL::VCENTER);
    }

    // The slot, glowing when the fader is down on something playing.
    const bool lit = flash && blink(0.6f);
    inset(cx - 3.f, b - 8.f, cx + 3.f, t + 8.f, lit ? scaled(C_RED, 0.6f) : LLColor4(0.02f, 0.02f, 0.03f, 1.f));

    // The cap: a ridged block with the white index line across it.
    const F32 y = b + travel * llclamp(pos, 0.f, 1.f);
    const F32 l = cx - CAP_W * 0.5f, r = cx + CAP_W * 0.5f;
    rectF(l + 2.f, y - CAP_H * 0.5f - 3.f, r + 2.f, y + CAP_H * 0.5f - 3.f, LLColor4(0.f, 0.f, 0.f, 0.5f));   // shadow
    const LLColor4 body = lit ? C_RED : cap;
    rectG(l, y - CAP_H * 0.5f, r, y + CAP_H * 0.5f, scaled(body, 1.1f), scaled(body, 0.55f));
    for (S32 i = -2; i <= 2; ++i)
    {
        if (i == 0) continue;
        rectF(l + 2.f, y + i * 2.5f - 0.5f, r - 2.f, y + i * 2.5f + 0.5f, scaled(body, 0.4f));
    }
    rectF(l, y - 1.f, r, y + 1.f, C_WHITE);
    frame(l, y - CAP_H * 0.5f, r, y + CAP_H * 0.5f, scaled(body, 0.3f));
}

// LED meter: green to -12, amber to -3, red above, and a clip LED on top for 2 s after full
// scale. The pre-fader level shows as dim LEDs above the lit ones, so a channel shows its music
// even with its fader down (Paul: "always show the eq if something is playing").
void WolfDJDesk::drawMeter(S32 l, S32 b, S32 r, S32 t, float post, float pre, int slot)
{
    static const float SEG_DB[] = { -48.f, -42.f, -36.f, -30.f, -26.f, -22.f, -18.f, -15.f, -12.f, -9.f, -6.f, -4.f, -3.f, -2.f, -1.f, -0.5f };
    const S32 levels = (S32)(sizeof(SEG_DB) / sizeof(SEG_DB[0]));
    const S32 segs = levels + 1;
    const float t_now = (float)now();
    const float dt = llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.25f);
    mShownDb[slot] = std::max(wolfdj_lin_to_db(post), mShownDb[slot] - 24.f * dt);
    mShownPreDb[slot] = std::max(wolfdj_lin_to_db(pre), mShownPreDb[slot] - 24.f * dt);
    if (post >= 0.99f) mClipUntil[slot] = t_now + 2.f;

    inset((F32)l - 2.f, (F32)b - 2.f, (F32)r + 2.f, (F32)t + 2.f, LLColor4(0.02f, 0.02f, 0.03f, 1.f));
    const F32 gap = 1.5f;
    const F32 seg_h = ((F32)(t - b) - gap * (segs - 1)) / segs;
    for (S32 i = 0; i < segs; ++i)
    {
        const F32 y0 = b + i * (seg_h + gap);
        LLColor4 on;
        F32 k;
        if (i == segs - 1)
        {
            on = C_RED;
            k = t_now < mClipUntil[slot] ? 1.f : 0.14f;
        }
        else
        {
            on = SEG_DB[i] < -12.f ? C_GREEN : (SEG_DB[i] < -3.f ? C_AMBER : C_RED);
            k = mShownDb[slot] >= SEG_DB[i] ? 1.f : (mShownPreDb[slot] >= SEG_DB[i] ? 0.38f : 0.14f);
        }
        rectF((F32)l, y0, (F32)r, y0 + seg_h, scaled(on, k));
    }
}

void WolfDJDesk::drawButton(S32 l, S32 b, S32 r, S32 t, const std::string& label, bool lit, const LLColor4& lamp)
{
    rectF((F32)l + 1.f, (F32)b - 2.f, (F32)r + 1.f, (F32)t - 2.f, LLColor4(0.f, 0.f, 0.f, 0.45f));
    if (lit)
    {
        rectG((F32)l, (F32)b, (F32)r, (F32)t, scaled(lamp, 1.05f), scaled(lamp, 0.7f));
    }
    else
    {
        rectG((F32)l, (F32)b, (F32)r, (F32)t, LLColor4(0.30f, 0.31f, 0.33f, 1.f), LLColor4(0.18f, 0.18f, 0.20f, 1.f));
        rectF((F32)l + 3.f, (F32)t - 3.f, (F32)r - 3.f, (F32)t - 2.f, scaled(lamp, 0.35f));   // the unlit lamp
    }
    frame((F32)l, (F32)b, (F32)r, (F32)t, C_SHADOW);
    text(fontSmall(), label, (l + r) * 0.5f, (b + t) * 0.5f, lit ? LLColor4(0.05f, 0.05f, 0.05f, 1.f) : C_TEXT);
}

void WolfDJDesk::drawStrip(int strip)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    WolfDJChannel* ch = strip_channel(strip);
    const S32 L = STRIP_X0 + strip * STRIP_W, R = L + STRIP_W;
    const S32 H = HEIGHT;
    const std::string name = STRIP_NAME[strip];

    // The strip's own panel, edged like a module in a frame.
    rectG((F32)L + 1.f, (F32)(H - NAME_T + 4), (F32)R - 1.f, 4.f, C_PANEL_TOP, C_PANEL_BOT);
    rectF((F32)R - 1.f, 4.f, (F32)R, (F32)(H - NAME_T + 4), C_SHADOW);
    rectF((F32)L, 4.f, (F32)L + 1.f, (F32)(H - NAME_T + 4), C_EDGE);

    // Name plate.
    inset((F32)L + 4.f, (F32)(H - NAME_B), (F32)R - 4.f, (F32)(H - NAME_T), C_LCD);
    text(fontBold(), name, (L + R) * 0.5f, H - (NAME_T + NAME_B) * 0.5f, C_LCD_TEXT);

    // Source: the music channels' lists are laid over this by the floater.
    if (strip == CH_VOICE || strip == CH_MIC || strip == STRIP_JINGLE)
    {
        inset((F32)L + 4.f, (F32)(H - SRC_B), (F32)R - 4.f, (F32)(H - SRC_T), C_LCD);
        std::string src = strip == CH_VOICE ? "voice chat" : (strip == CH_MIC ? "microphone" : "pads below");
        LLColor4 c = C_DIM;
        if (strip == CH_VOICE && ch->mPrePeak.load() <= 0.f) src = "no voice yet";
        if (strip == STRIP_JINGLE && WolfDJJingles::instance().playing() >= 0)
        {
            src = llformat("PAD %d", WolfDJJingles::instance().playing() + 1);
            c = C_LCD_TEXT;
        }
        text(fontSmall(), src, (L + R) * 0.5f, H - (SRC_T + SRC_B) * 0.5f, c);
    }

    const S32 kx = L + 54;
    if (ch)
    {
        // EQ.
        static const char* EQ_LABEL[3] = { "HF", "MF", "LF" };
        static const EKind EQ_KIND[3] = { K_EQ_HIGH, K_EQ_MID, K_EQ_LOW };
        static const char* EQ_WHAT[3] = { "Treble (8 kHz shelf)", "Mid (1 kHz)", "Bass (100 Hz shelf)" };
        const LLColor4 EQ_CAP[3] = { C_KNOB_HF, C_KNOB_MF, C_KNOB_LF };
        const float EQ_DB[3] = { ch->mEqHighDb.load(), ch->mEqMidDb.load(), ch->mEqLowDb.load() };
        for (int i = 0; i < 3; ++i)
        {
            const S32 cy = H - EQ_Y[i];
            text(fontSmall(), EQ_LABEL[i], (F32)L + 18.f, (F32)cy + 5.f, C_TEXT);
            text(fontSmall(), llformat("%+.0f", EQ_DB[i]), (F32)L + 18.f, (F32)cy - 7.f, EQ_DB[i] != 0.f ? EQ_CAP[i] : C_DIM);
            drawKnob(kx, cy, KNOB_R, eq_to_knob(EQ_DB[i]), EQ_CAP[i], true);
            addHit(L + 4, cy - 15, R - 4, cy + 15, EQ_KIND[i], strip,
                   llformat("%s %s: %+.1f dB (-24 to +12). Drag up or down, or scroll; double-click for flat.", name.c_str(), EQ_WHAT[i], EQ_DB[i]));
        }

        // Effect: its name (click to change) and how much.
        const int fx = llclamp(ch->mFx.load(), (int)FX_NONE, (int)FX_COUNT - 1);
        inset((F32)L + 4.f, (F32)(H - FXTYPE_B), (F32)R - 4.f, (F32)(H - FXTYPE_T), C_LCD);
        text(fontSmall(), FX_NAME[fx], (L + R) * 0.5f, H - (FXTYPE_T + FXTYPE_B) * 0.5f, fx == FX_NONE ? C_DIM : C_KNOB_FX);
        addHit(L + 4, H - FXTYPE_B, R - 4, H - FXTYPE_T, K_FX_TYPE, strip,
               std::string("Effect on this channel, after its EQ: click for the next (Echo, Reverb, Radio voice, none), right-click or scroll to go back."));
        const S32 fy = H - FX_Y;
        text(fontSmall(), "FX", (F32)L + 18.f, (F32)fy + 5.f, C_TEXT);
        text(fontSmall(), llformat("%d%%", (int)roundf(ch->mFxAmount.load() * 100.f)), (F32)L + 18.f, (F32)fy - 7.f, fx == FX_NONE ? C_DIM : C_KNOB_FX);
        drawKnob(kx, fy, KNOB_R, ch->mFxAmount.load(), fx == FX_NONE ? C_DIM : C_KNOB_FX, false);
        addHit(L + 4, fy - 15, R - 4, fy + 15, K_FX_AMOUNT, strip,
               std::string("How much of the effect: for Echo, louder and longer repeats; for Reverb, more room; for Radio, more grit."));
    }
    else if (strip == STRIP_JINGLE)
    {
        // The pads' lamps, each its own colour; lit while it plays. Click one to play it.
        WolfDJJingles& j = WolfDJJingles::instance();
        for (int i = 0; i < WolfDJJingles::PAD_COUNT; ++i)
        {
            const int col = i % 2, row = i / 2;
            const S32 l = L + 10 + col * 34, t = H - (130 + row * 32);
            const float* rgb = WolfDJJingles::PAD_RGB[i];
            const LLColor4 c(rgb[0], rgb[1], rgb[2], 1.f);
            const bool empty = j.pad(i).empty();
            const bool on = j.playing() == i;
            drawButton(l, t - 24, l + 30, t, llformat("%d", i + 1), on, c);
            if (!on && !empty) rectF((F32)l + 3.f, (F32)t - 6.f, (F32)l + 27.f, (F32)t - 3.f, c);
            const std::string label = WolfDJJingles::padLabel(j.pad(i));
            addHit(l, t - 24, l + 30, t, K_NONE, 100 + i, empty
                ? llformat("Jingle pad %d - empty. Put a song on it from the Playlist window.", i + 1)
                : llformat("Jingle pad %d: %s. Click to play it on air.", i + 1, label.c_str()));
        }
    }

    // Routing buttons.
    const S32 mid = (L + R) / 2;
    const bool mon = ch ? ch->mMonitor.load() : mix.mJingleMonitor.load();
    drawButton(L + 6, H - BTN1_B, ch ? mid - 2 : R - 6, H - BTN1_T, "MON", mon, C_GREEN);
    addHit(L + 6, H - BTN1_B, ch ? mid - 2 : R - 6, H - BTN1_T, K_MON, strip,
           (mon ? std::string("MON is on: you hear this channel in your own speakers or headphones. Click to stop hearing it (it still goes on air).")
                : std::string("MON is off: this channel goes on air but you do not hear it yourself. Click to hear it."))
           + (strip == CH_MIC ? std::string(" Your own voice: use headphones, or speakers will feed back into the mic.") : std::string()));
    if (ch)
    {
        const bool cue = mix.mCue.load() == strip;
        drawButton(mid + 2, H - BTN1_B, R - 6, H - BTN1_T, "CUE", cue, C_AMBER);
        addHit(mid + 2, H - BTN1_B, R - 6, H - BTN1_T, K_CUE, strip,
               std::string("CUE: listen to just this channel yourself, before its fader (it can be checked with the fader down). Click again to stop."));
        const bool mute = ch->mMute.load();
        drawButton(L + 6, H - BTN2_B, R - 6, H - BTN2_T, "MUTE", mute, C_RED);
        addHit(L + 6, H - BTN2_B, R - 6, H - BTN2_T, K_MUTE, strip, std::string("MUTE: take this channel off the stream."));
    }

    // Fader and meter.
    const float pos = value(K_FADER, strip);
    const float pre = ch ? ch->mPrePeak.load() : mix.mJinglePrePeak.load();
    const float post = ch ? ch->mPeak.load() : mix.mJinglePeak.load();
    const bool music = strip >= CH_MUSIC_A;     // the music channels and the jingles
    const bool flash = music && pre > SIGNAL && (pos <= FADER_DOWN || (ch && ch->mMute.load()));
    const S32 fx_ = L + 40;
    drawFader(fx_, H - TRAVEL_B, H - TRAVEL_T, pos, cap_colour(strip), flash && pos <= FADER_DOWN, true);
    addHit(fx_ - CAP_W / 2 - 2, H - TRAVEL_B - 10, fx_ + CAP_W / 2 + 2, H - TRAVEL_T + 10, K_FADER, strip,
           llformat("%s fader: %s dB on air. Drag, or scroll for 1 dB steps; double-click for 0 dB.", name.c_str(), db_text(wolfdj_fader_db(pos)).c_str()),
           H - TRAVEL_B, H - TRAVEL_T);
    drawMeter(L + 60, H - TRAVEL_B - 6, L + 70, H - TRAVEL_T + 6, post, pre, strip);

    // Readout.
    inset((F32)L + 4.f, (F32)(H - READ_B), (F32)R - 4.f, (F32)(H - READ_T), C_LCD);
    std::string read;
    LLColor4 rc = C_LCD_TEXT;
    if (ch && ch->mMute.load())
    {
        read = "MUTED";
        rc = C_RED;
        if (flash && blink(0.6f)) rc = C_WHITE;
    }
    else
    {
        read = db_text(wolfdj_fader_db(pos)) + " dB";
        if (flash) rc = blink(0.6f) ? C_RED : C_WHITE;
    }
    text(fontMono(), read, (L + R) * 0.5f, H - (READ_T + READ_B) * 0.5f, rc);
}

void WolfDJDesk::drawMaster()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    const S32 L = MASTER_L, R = MASTER_R, H = HEIGHT;

    rectG((F32)L + 1.f, (F32)(H - NAME_T + 4), (F32)R - 1.f, 4.f, LLColor4(0.20f, 0.20f, 0.23f, 1.f), C_PANEL_BOT);
    rectF((F32)L, 4.f, (F32)L + 2.f, (F32)(H - NAME_T + 4), C_EDGE);
    inset((F32)L + 4.f, (F32)(H - NAME_B), (F32)R - 4.f, (F32)(H - NAME_T), C_LCD);
    text(fontBold(), "MASTER  L / R", (L + R) * 0.5f, H - (NAME_T + NAME_B) * 0.5f, C_LCD_TEXT);

    // Status lamps: MIC LIVE (the mic is open on air), TALK-OVER (the music is dipping under the
    // mic), and the limiter's gain reduction.
    const WolfDJChannel& mic = mix.channel(CH_MIC);
    const bool mic_live = !mic.mMute.load() && mic.mFader.load() > FADER_DOWN && mix.capture(CH_MIC);
    const bool ducking = mix.mDucking.load();
    auto lamp = [&](S32 t, const std::string& label, bool on, const LLColor4& c, const std::string& tip)
    {
        drawButton(L + 8, H - (t + 18), R - 8, H - t, label, on, c);
        addHit(L + 8, H - (t + 18), R - 8, H - t, K_NONE, -1, tip);
    };
    lamp(SRC_T, "MIC LIVE", mic_live, C_RED, "MIC LIVE: your microphone is open on air (not muted, fader up).");
    lamp(SRC_T + 24, "TALK-OVER", ducking, C_AMBER, "TALK-OVER: the music dips while you talk (the Talk-over box below).");

    const float lim = mix.mLimiterDb.load();
    const S32 gt = H - (SRC_T + 54);
    text(fontSmall(), "LIMITER", (F32)L + 10.f, (F32)gt - 6.f, C_TEXT, LLFontGL::LEFT);
    inset((F32)L + 62.f, (F32)gt - 12.f, (F32)R - 8.f, (F32)gt, LLColor4(0.02f, 0.02f, 0.03f, 1.f));
    const F32 frac = llclamp(-lim / 12.f, 0.f, 1.f);       // 0..12 dB of gain reduction
    if (frac > 0.f)
    {
        rectF((F32)R - 9.f - frac * (R - 72 - L), (F32)gt - 11.f, (F32)R - 9.f, (F32)gt - 1.f, C_AMBER);
    }
    addHit(L + 8, gt - 12, R - 8, gt, K_NONE, -1, llformat("Limiter: %.1f dB taken off to keep the stream from clipping. Often lit = the mix is too hot.", -lim));

    // CUE the stream mix, and LINK the two sides.
    const S32 mid = (L + R) / 2;
    const bool cue = mix.mCue.load() == CUE_MASTER;
    drawButton(L + 8, H - BTN1_B, mid - 3, H - BTN1_T, "CUE", cue, C_AMBER);
    addHit(L + 8, H - BTN1_B, mid - 3, H - BTN1_T, K_CUE, STRIP_MASTER, std::string("CUE the stream: hear exactly what goes on air. Click again to stop."));
    drawButton(mid + 3, H - BTN1_B, R - 8, H - BTN1_T, "LINK", mLinked, C_GREEN);
    addHit(mid + 3, H - BTN1_B, R - 8, H - BTN1_T, K_LINK, STRIP_MASTER,
           mLinked ? std::string("LINK is on: the L and R faders move together. Click to move them one at a time (to balance left and right).")
                   : std::string("LINK is off: the L and R faders move separately. Click to link them (R jumps to L)."));

    // The two faders on one shared scale, the stereo meter beside them.
    const float pl = mix.mMasterFaderL.load(), pr = mix.mMasterFaderR.load();
    const float pre = mix.mMasterPrePeak.load();
    const S32 xl = L + 48, xr = L + 88;
    drawFader(xl, H - TRAVEL_B, H - TRAVEL_T, pl, C_CAP_MASTER, pre > SIGNAL && pl <= FADER_DOWN, true);
    drawFader(xr, H - TRAVEL_B, H - TRAVEL_T, pr, C_CAP_MASTER, pre > SIGNAL && pr <= FADER_DOWN, false);
    text(fontSmall(), "L", (F32)xl, (F32)(H - TRAVEL_T + 16), C_TEXT);
    text(fontSmall(), "R", (F32)xr, (F32)(H - TRAVEL_T + 16), C_TEXT);
    addHit(xl - CAP_W / 2 - 2, H - TRAVEL_B - 10, xl + CAP_W / 2 + 2, H - TRAVEL_T + 10, K_MASTER_L, STRIP_MASTER,
           llformat("Master left: %s dB. Drag, or scroll for 1 dB steps; double-click for 0 dB.", db_text(wolfdj_fader_db(pl)).c_str()),
           H - TRAVEL_B, H - TRAVEL_T);
    addHit(xr - CAP_W / 2 - 2, H - TRAVEL_B - 10, xr + CAP_W / 2 + 2, H - TRAVEL_T + 10, K_MASTER_R, STRIP_MASTER,
           llformat("Master right: %s dB. Drag, or scroll for 1 dB steps; double-click for 0 dB.", db_text(wolfdj_fader_db(pr)).c_str()),
           H - TRAVEL_B, H - TRAVEL_T);
    drawMeter(L + 116, H - TRAVEL_B - 6, L + 126, H - TRAVEL_T + 6, mix.mMasterPeakL.load(), mix.mMasterPeakL.load(), CH_COUNT + 1);
    drawMeter(L + 132, H - TRAVEL_B - 6, L + 142, H - TRAVEL_T + 6, mix.mMasterPeakR.load(), mix.mMasterPeakR.load(), CH_COUNT + 2);
    text(fontSmall(), "L", (F32)L + 121.f, (F32)(H - TRAVEL_T + 16), C_DIM);
    text(fontSmall(), "R", (F32)L + 137.f, (F32)(H - TRAVEL_T + 16), C_DIM);
    addHit(L + 112, H - TRAVEL_B - 6, L + 146, H - TRAVEL_T + 6, K_NONE, -1,
           std::string("What goes on air, left and right. The top LED lights when it clips."));

    inset((F32)L + 4.f, (F32)(H - READ_B), (F32)R - 4.f, (F32)(H - READ_T), C_LCD);
    const bool flash = pre > SIGNAL && (pl <= FADER_DOWN || pr <= FADER_DOWN);
    const std::string read = llformat("L %s  R %s", db_text(wolfdj_fader_db(pl)).c_str(), db_text(wolfdj_fader_db(pr)).c_str());
    text(fontMono(), read, (L + R) * 0.5f, H - (READ_T + READ_B) * 0.5f, flash ? (blink(0.6f) ? C_RED : C_WHITE) : C_LCD_TEXT);
}

// The meter bridge: the playlist deck on the left, the on-air spectrum and the ON AIR lamp on the right.
void WolfDJDesk::drawBridge()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    WolfDJPlayer& player = WolfDJPlayer::instance();
    const S32 H = HEIGHT;
    const S32 t = H - BRIDGE_T, b = H - BRIDGE_B;

    rectG((F32)STRIP_X0, (F32)b, (F32)MASTER_R, (F32)t, LLColor4(0.22f, 0.23f, 0.26f, 1.f), LLColor4(0.13f, 0.13f, 0.15f, 1.f));
    frame((F32)STRIP_X0, (F32)b, (F32)MASTER_R, (F32)t, C_SHADOW);

    // ---- the deck ----
    const S32 dl = STRIP_X0 + 6, dr = MASTER_L - 6;
    inset((F32)dl, (F32)b + 6.f, (F32)dr, (F32)t - 6.f, C_LCD);
    const bool playing = player.isPlaying();
    const bool paused = player.isPaused();
    const int strip = playlist_strip();
    std::string artist, title;
    player.nowPlaying(artist, title);
    const double pos = player.position(), len = player.duration();
    const double left = std::max(0.0, len - pos);

    // Transport at the right of the deck.
    const S32 bw = 30, bh = 22, by = t - 12;
    const S32 bx0 = dr - 6 - 4 * (bw + 4);
    struct Btn { EKind k; const char* tip; };
    const Btn btns[4] = {
        { K_PREV, "Back to the start of this song, or the one before." },
        { K_PLAY, "Play / pause the playlist. If it is on no channel it goes on a free music channel." },
        { K_NEXT, "The next song." },
        { K_STOP, "Stop the playlist." },
    };
    for (int i = 0; i < 4; ++i)
    {
        const S32 l = bx0 + i * (bw + 4), r = l + bw;
        const bool lit = (btns[i].k == K_PLAY && playing && !paused) || (btns[i].k == K_STOP && !playing);
        drawButton(l, by - bh, r, by, "", lit, btns[i].k == K_PLAY ? C_GREEN : C_DIM);
        const F32 cx = (l + r) * 0.5f, cy = by - bh * 0.5f;
        const LLColor4 ic = lit ? LLColor4(0.05f, 0.05f, 0.05f, 1.f) : C_TEXT;
        switch (btns[i].k)
        {
        case K_PREV: rectF(cx - 7.f, cy - 5.f, cx - 5.f, cy + 5.f, ic); triF(cx + 5.f, cy - 5.f, cx + 5.f, cy + 5.f, cx - 4.f, cy, ic); break;
        case K_NEXT: rectF(cx + 5.f, cy - 5.f, cx + 7.f, cy + 5.f, ic); triF(cx - 5.f, cy - 5.f, cx - 5.f, cy + 5.f, cx + 4.f, cy, ic); break;
        case K_STOP: rectF(cx - 5.f, cy - 5.f, cx + 5.f, cy + 5.f, ic); break;
        default:
            if (playing && !paused) { rectF(cx - 5.f, cy - 5.f, cx - 2.f, cy + 5.f, ic); rectF(cx + 2.f, cy - 5.f, cx + 5.f, cy + 5.f, ic); }
            else triF(cx - 4.f, cy - 6.f, cx - 4.f, cy + 6.f, cx + 6.f, cy, ic);
            break;
        }
        addHit(l, by - bh, r, by, btns[i].k, -1, btns[i].tip);
    }

    // Song line.
    const S32 tl = dl + 8;
    const std::string where = strip >= 0 ? std::string(STRIP_NAME[strip]) : std::string("NO CHANNEL");
    text(fontSmall(), "PLAYLIST > " + where, (F32)tl, (F32)t - 16.f, strip >= 0 ? C_DIM : C_AMBER, LLFontGL::LEFT);
    std::string song = playing ? (artist.empty() ? title : artist + " - " + title) : std::string("Nothing playing - press play, or open the playlist");
    if (playing && paused) song = "PAUSED  " + song;
    text(fontBold(), song, (F32)tl, (F32)t - 32.f, playing ? C_WHITE : C_DIM, LLFontGL::LEFT, LLFontGL::VCENTER, bx0 - tl - 8);
    addHit(tl, t - 40, bx0 - 8, t - 8, K_DECK, -1, std::string("The playlist. Click to open the playlist window."));

    // Times: elapsed, length, and the time left - big, and flashing red for the last 30 seconds
    // (Paul: "i need to see how long a track is as it's playing").
    const S32 ty = b + 30;
    if (playing)
    {
        text(fontMono(), clock_text(pos), (F32)tl, (F32)ty, C_LCD_TEXT, LLFontGL::LEFT);
        text(fontSmall(), len > 0.0 ? "of " + clock_text(len) : std::string("length unknown"), (F32)tl + 52.f, (F32)ty, C_DIM, LLFontGL::LEFT);
        if (len > 0.0)
        {
            const bool ending = left < 30.0 && !paused;
            const LLColor4 rc = ending ? (blink(0.5f) ? C_RED : C_WHITE) : C_AMBER;
            text(fontSmall(), "LEFT", (F32)bx0 - 64.f, (F32)ty, C_DIM, LLFontGL::RIGHT);
            text(fontBig(), "-" + clock_text(left), (F32)bx0 - 8.f, (F32)ty, rc, LLFontGL::RIGHT);
        }
    }
    // Progress.
    const F32 pl = (F32)tl, pr = (F32)bx0 - 120.f, py = (F32)ty - 1.f;
    if (pr > pl + 160.f)
    {
        inset(pl + 130.f, py - 3.f, pr, py + 3.f, LLColor4(0.02f, 0.02f, 0.03f, 1.f));
        if (playing && len > 0.0)
        {
            const F32 f = (F32)llclamp(pos / len, 0.0, 1.0);
            rectF(pl + 131.f, py - 2.f, pl + 131.f + (pr - pl - 132.f) * f, py + 2.f, left < 30.0 ? C_RED : C_LCD_TEXT);
        }
    }
    // The next song, once it is in memory.
    const std::string next = player.preloadedNext();
    if (playing)
    {
        // Kept clear of the big time-left figure on the right.
        text(fontSmall(), next.empty() ? std::string("NEXT: loading...") : "NEXT: " + next + "  (loaded)",
             (F32)tl, (F32)b + 13.f, next.empty() ? C_DIM : C_GREEN, LLFontGL::LEFT, LLFontGL::VCENTER, bx0 - 130 - tl);
    }
    else if (!player.lastError().empty())
    {
        text(fontSmall(), player.lastError(), (F32)tl, (F32)b + 13.f, C_AMBER, LLFontGL::LEFT, LLFontGL::VCENTER, dr - tl - 8);
    }

    // ---- the spectrum ----
    const S32 sl = MASTER_L + 4, sr = MASTER_R - 6;
    inset((F32)sl, (F32)b + 6.f, (F32)sr, (F32)t - 6.f, LLColor4(0.02f, 0.02f, 0.03f, 1.f));
    // ON AIR lamp across the top of it.
    const EState st = mix.state();
    const bool live = mix.isLiveRequested();
    const bool on_air = live && st == ON_AIR;
    const bool connecting = live && (st == CONNECTING || st == RETRYING || st == OFF_AIR);
    const LLColor4 lc = on_air ? C_RED : (connecting ? C_AMBER : LLColor4(0.25f, 0.08f, 0.08f, 1.f));
    rectF((F32)sl + 2.f, (F32)t - 20.f, (F32)sr - 2.f, (F32)t - 8.f, lc);
    text(fontSmall(), on_air ? "ON AIR" : (connecting ? "CONNECTING" : "OFF AIR"), (sl + sr) * 0.5f, (F32)t - 14.f,
         on_air || connecting ? LLColor4(0.05f, 0.02f, 0.02f, 1.f) : C_DIM);
    addHit(sl, b + 6, sr, t - 6, K_NONE, -1, std::string("What is on air, by pitch: bass on the left, treble on the right."));

    static const char* BAND_LABEL[SPECTRUM_BANDS] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    const float t_now = (float)now();
    const float dt = llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.25f);
    const F32 bar_b = (F32)b + 18.f, bar_t = (F32)t - 24.f;
    const F32 bw2 = ((F32)(sr - sl) - 8.f) / SPECTRUM_BANDS;
    for (int i = 0; i < SPECTRUM_BANDS; ++i)
    {
        // An RMS of 0.707 is a full-scale sine: +3 dB so a loud band reaches the top.
        const float db = wolfdj_lin_to_db(mix.mSpectrum[i].load()) + 3.f;
        mSpecDb[i] = std::max(db, mSpecDb[i] - 30.f * dt);
        if (db > mSpecPeakDb[i]) { mSpecPeakDb[i] = db; mSpecPeakUntil[i] = t_now + 1.f; }
        else if (t_now > mSpecPeakUntil[i]) mSpecPeakDb[i] = std::max(-120.f, mSpecPeakDb[i] - 20.f * dt);
        const F32 x0 = sl + 4.f + i * bw2 + 1.f, x1 = x0 + bw2 - 2.f;
        const F32 f = llclamp((mSpecDb[i] + 60.f) / 60.f, 0.f, 1.f);
        // Segmented bars: green, amber near the top, red at the top.
        const S32 segs = 12;
        const F32 sh = (bar_t - bar_b) / segs;
        for (S32 s = 0; s < segs; ++s)
        {
            const F32 y0 = bar_b + s * sh;
            const LLColor4 c = s >= 10 ? C_RED : (s >= 7 ? C_AMBER : C_GREEN);
            rectF(x0, y0, x1, y0 + sh - 1.f, (F32)(s + 1) / segs <= f ? c : scaled(c, 0.12f));
        }
        const F32 pf = llclamp((mSpecPeakDb[i] + 60.f) / 60.f, 0.f, 1.f);
        if (pf > 0.02f) rectF(x0, bar_b + (bar_t - bar_b) * pf - 1.f, x1, bar_b + (bar_t - bar_b) * pf + 1.f, C_WHITE);
        text(fontSmall(), BAND_LABEL[i], (x0 + x1) * 0.5f, (F32)b + 11.f, C_DIM);
    }
}

void WolfDJDesk::draw()
{
    mHits.clear();
    const S32 W = WIDTH, H = HEIGHT;
    // The desk's surface.
    rectG(0.f, 0.f, (F32)W, (F32)H, LLColor4(0.15f, 0.15f, 0.17f, 1.f), LLColor4(0.07f, 0.07f, 0.08f, 1.f));
    frame(0.f, 0.f, (F32)W, (F32)H, C_SHADOW);

    drawBridge();
    for (int s = 0; s <= STRIP_JINGLE; ++s)
    {
        drawStrip(s);
    }
    drawMaster();
    LLUICtrl::draw();
}

// ---- mouse ----

bool WolfDJDesk::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const Hit* h = hitAt(x, y);
    if (!h) return LLUICtrl::handleMouseDown(x, y, mask);
    WolfDJPlayer& player = WolfDJPlayer::instance();
    switch (h->mKind)
    {
    case K_FADER:
    case K_MASTER_L:
    case K_MASTER_R:
    case K_EQ_HIGH:
    case K_EQ_MID:
    case K_EQ_LOW:
    case K_FX_AMOUNT:
    {
        mDragKind = h->mKind;
        mDragStrip = h->mStrip;
        mDragY = y;
        mDragStart = value(h->mKind, h->mStrip);
        mDragB = h->mTravelB;
        mDragT = h->mTravelT;
        mGrabOffset = 0;
        mDragFine = (mask & MASK_SHIFT) != 0;
        if (h->mKind == K_FADER || h->mKind == K_MASTER_L || h->mKind == K_MASTER_R)
        {
            const EKind other = h->mKind == K_MASTER_L ? K_MASTER_R : K_MASTER_L;
            mDragOther = value(other, h->mStrip);
            const S32 cap_y = mDragB + (S32)lltrunc((mDragT - mDragB) * mDragStart);
            if (abs(y - cap_y) <= CAP_H / 2 + 2)
            {
                mGrabOffset = y - cap_y;        // held by the cap: it does not jump
            }
            else
            {
                // A click on the slot moves the cap there, as on a desk you put your finger on it.
                const float to = llclamp((float)(y - mDragB) / (float)std::max(1, mDragT - mDragB), 0.f, 1.f);
                setControlValue(h->mKind, h->mStrip, to);
                if ((h->mKind == K_MASTER_L || h->mKind == K_MASTER_R) && mLinked)
                {
                    setControlValue(other, h->mStrip, mDragOther + (to - mDragStart));
                    mDragOther = value(other, h->mStrip);
                }
                mDragStart = to;
            }
        }
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    case K_MON: case K_CUE: case K_MUTE: case K_LINK: case K_FX_TYPE:
        toggle(h->mKind, h->mStrip);
        onCommit();
        return true;
    case K_PLAY:
        if (player.files().empty())
        {
            LLFloaterReg::showInstance("wolf_dj_playlist");
        }
        else if (!player.isPlaying())
        {
            player.play(player.current() >= 0 ? player.current() : 0);
        }
        else
        {
            player.togglePause();
        }
        return true;
    case K_PREV: player.previous(); return true;
    case K_NEXT: player.next(); return true;
    case K_STOP: player.stop(); return true;
    case K_DECK: LLFloaterReg::showInstance("wolf_dj_playlist"); return true;
    case K_NONE:
        if (h->mStrip >= 100)
        {
            const int pad = h->mStrip - 100;
            if (WolfDJJingles::instance().pad(pad).empty()) LLFloaterReg::showInstance("wolf_dj_playlist");
            else WolfDJJingles::instance().trigger(pad);
        }
        return true;
    default:
        return true;
    }
}

bool WolfDJDesk::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mDragKind != K_NONE)
    {
        float v;
        const bool fine = (mask & MASK_SHIFT) != 0;
        if (fine != mDragFine)
        {
            // Shift pressed or let go mid-drag: carry on from here, without a jump.
            mDragFine = fine;
            mDragStart = value(mDragKind, mDragStrip);
            mDragY = y;
            const S32 cap_y = mDragB + (S32)lltrunc((mDragT - mDragB) * mDragStart);
            mGrabOffset = y - cap_y;
            if (mDragKind == K_MASTER_L || mDragKind == K_MASTER_R)
            {
                mDragOther = value(mDragKind == K_MASTER_L ? K_MASTER_R : K_MASTER_L, mDragStrip);
            }
        }
        if (mDragKind == K_FADER || mDragKind == K_MASTER_L || mDragKind == K_MASTER_R)
        {
            // The cap follows the mouse; with Shift it moves a fifth as far (fine trim).
            const float span = (float)std::max(1, mDragT - mDragB);
            if (mask & MASK_SHIFT)
            {
                v = mDragStart + (float)(y - mDragY) / span * 0.2f;
            }
            else
            {
                v = (float)(y - mGrabOffset - mDragB) / span;
            }
        }
        else
        {
            // Knobs: 200 px of travel turns one end to the other (Shift: 1000 px).
            v = mDragStart + (float)(y - mDragY) / ((mask & MASK_SHIFT) ? 1000.f : 200.f);
        }
        v = llclamp(v, 0.f, 1.f);
        setControlValue(mDragKind, mDragStrip, v);
        if ((mDragKind == K_MASTER_L || mDragKind == K_MASTER_R) && mLinked)
        {
            const EKind other = mDragKind == K_MASTER_L ? K_MASTER_R : K_MASTER_L;
            setControlValue(other, mDragStrip, mDragOther + (v - mDragStart));
        }
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    getWindow()->setCursor(hitAt(x, y) ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfDJDesk::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
        if (mDragKind != K_NONE)
        {
            mDragKind = K_NONE;
            onCommit();         // save the levels once, when the move is finished
        }
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

bool WolfDJDesk::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    const Hit* h = hitAt(x, y);
    if (!h) return LLUICtrl::handleDoubleClick(x, y, mask);
    switch (h->mKind)
    {
    case K_FADER: case K_EQ_HIGH: case K_EQ_MID: case K_EQ_LOW: case K_FX_AMOUNT:
        setControlValue(h->mKind, h->mStrip, defaultValue(h->mKind));
        onCommit();
        return true;
    case K_MASTER_L: case K_MASTER_R:
        setControlValue(h->mKind, h->mStrip, defaultValue(h->mKind));
        if (mLinked) setControlValue(h->mKind == K_MASTER_L ? K_MASTER_R : K_MASTER_L, h->mStrip, defaultValue(h->mKind));
        onCommit();
        return true;
    case K_PLAY: case K_DECK:
        return true;        // a double-click on play is not play-then-pause
    default:
        // Buttons: a quick second click is a second press.
        return handleMouseDown(x, y, mask);
    }
}

bool WolfDJDesk::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    const Hit* h = hitAt(x, y);
    if (h && h->mKind == K_FX_TYPE)
    {
        nudge(*h, 1);       // back one effect
        return true;
    }
    return LLUICtrl::handleRightMouseDown(x, y, mask);
}

bool WolfDJDesk::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    const Hit* h = hitAt(x, y);
    switch (h ? h->mKind : K_NONE)
    {
    case K_FADER: case K_MASTER_L: case K_MASTER_R:
    case K_EQ_HIGH: case K_EQ_MID: case K_EQ_LOW:
    case K_FX_AMOUNT: case K_FX_TYPE:
        nudge(*h, clicks);
        return true;
    default:
        return LLUICtrl::handleScrollWheel(x, y, clicks);   // elsewhere the strips scroll
    }
}

bool WolfDJDesk::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (const Hit* h = hitAt(x, y))
    {
        if (!h->mTip.empty())
        {
            LLToolTipMgr::instance().show(h->mTip);
            return true;
        }
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

void WolfDJDesk::onMouseCaptureLost()
{
    if (mDragKind != K_NONE)
    {
        mDragKind = K_NONE;
        onCommit();
    }
    LLUICtrl::onMouseCaptureLost();
}
