/**
 * @file wolfspeed.cpp
 * @brief WolfViewer: Xenon's WolfSpeed - the speed window and its link to WolfSim's WolfSpeedModule. See wolfspeed.h.
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

#include "wolfspeed.h"

#include <cmath>

#include "llagent.h"
#include "llcommandmanager.h"
#include "lldispatcher.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerregion.h"
#include "llwindow.h"
#include "wolfgrid.h"

namespace
{
    // Source: WolfSpeedModule.cs METHOD_IN / MESSAGE_OUT
    const std::string METHOD_TO_SIM("wolfspeed");
    const std::string MESSAGE_FROM_SIM("WolfSpeed");
    const std::string FLOATER("wolf_speed");
    // Paul 2026-10-17: the name people see (after the resident Xenon Darrow)
    const std::string FEATURE_NAME("Xenon's WolfSpeed");

    // "hello" again when the region has not answered: every second for the first 10 s (just after a crossing the region
    // may still hold us as a child agent and drops a hello - WolfSpeedModule.OnViewerBinary - and a fast avatar would
    // cross the next region at 1x meanwhile), every 5 s up to 30 s, then every 30 s (wolfborder.cpp HELLO_RETRY_S).
    const F64 HELLO_RETRY_QUICK_S = 1.0, HELLO_QUICK_FOR_S = 10.0, HELLO_RETRY_FAST_S = 5.0, HELLO_FAST_FOR_S = 30.0, HELLO_RETRY_S = 30.0;
    // How long without an answer before the window says the region does not offer it.
    const F64 NO_MODULE_AFTER_S = 4.0;
    // Picks go at most one a second (WolfSpeedModule.cs SET_BUCKET / SET_REFILL: 5, one back every second); a pick made
    // sooner waits and the newest one goes.
    const F64 SEND_EVERY_S = 1.0;
    const F64 ANSWER_TIMEOUT_S = 5.0;

    // The presets (design values): Walk is a brisk walk, Super is the most this region allows.
    const F32 PRESET_WALK = 1.5f, PRESET_RUN = 2.f, PRESET_FAST = 3.f;
    // ScenePresence.cs: AgentControlNormalVel 4.096 m/s running; AddNewMovement multiplies flying by 4. Shown as
    // "about" figures - the physics engine scales them a little (ubODE / BulletSim run factors).
    const F32 RUN_MS = 4.096f, FLY_MS = 4.096f * 4.f;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    std::string times(F32 x)
    {
        const S32 tenths = ll_round(x * 10.f);
        return tenths % 10 == 0 ? llformat("%dx", tenths / 10) : llformat("%d.%dx", tenths / 10, tenths % 10);
    }

    // The number on the wire: "2.50" - built from integers, so no locale can turn the point into a comma.
    std::string wire(F32 x)
    {
        const S32 h = ll_round(x * 100.f);
        return llformat("%d.%02d", h / 100, h % 100);
    }

    F32 snapHalf(F32 x) { return ll_round(x * 2.f) / 2.f; }
}

/** Source: wolfborder.cpp WolfBorderPushHandler - registered with gGenericDispatcher. */
class WolfSpeedPushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
    {
        if (WolfSpeed::instanceExists())   // never brings a deleted singleton back during shutdown
        {
            WolfSpeed::instance().receive(invoice, strings);
        }
        return true;
    }
};

namespace
{
    WolfSpeedPushHandler sSpeedHandler;
}

WolfSpeed::WolfSpeed()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sSpeedHandler);
    }
}

WolfSpeed::~WolfSpeed()
{
}

bool WolfSpeed::available() const
{
    return mModuleRegion.notNull() && mModuleRegion == mRegion;
}

void WolfSpeed::forget()
{
    mModuleRegion.setNull();
    mHelloSent = false;
    mHelloAt = mHelloFirstAt = 0.0;
    mSpeed = mForce = mMax = mGrid = 1.f;
    mRMax = 0.f;
    mAllow = mEdit = mPaused = false;
    mQueued = 0.f;
    mSentAt = 0.0;
}

void WolfSpeed::say(ELevel level, const std::string& word, const std::string& text, bool open_window)
{
    mMessage.mLevel = level;
    mMessage.mWord = word;
    mMessage.mText = text;
    // Something went against what the person asked (capped, forbidden, stopped): never only in a closed window.
    // Opened for a message it does not take the keyboard (the person may be steering with the arrow keys).
    if (open_window && WolfGrid::isOnWolfTerritories() && !LLFloaterReg::instanceVisible(FLOATER))
    {
        mQuietOpen = true;
        LLFloaterReg::showInstance(FLOATER);
        mQuietOpen = false;
    }
}

void WolfSpeed::sendSet(F32 x)
{
    mLastSendAt = now();
    mSentAt = mLastSendAt;
    send_generic_message(METHOD_TO_SIM, { "set", wire(x) });
}

void WolfSpeed::request(F32 x)
{
    if (!available() || !WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    x = llclamp(ll_round(x * 10.f) / 10.f, 1.f, CEILING);
    mWanted = x;
    dismissMessage();
    if (now() - mLastSendAt >= SEND_EVERY_S)
    {
        mQueued = 0.f;
        sendSet(x);
    }
    else
    {
        mQueued = x;   // idle() sends it
    }
}

void WolfSpeed::requestRegion(bool allow, F32 cap)
{
    if (!canEditRegion() || !WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    dismissMessage();
    mSentAt = now();
    send_generic_message(METHOD_TO_SIM, { "region", allow ? "1" : "0", wire(llmax(0.f, cap)) });
}

/**
 * [WOLF GRID GATE] Source: wolfdrive.cpp WolfDrive::refreshDashGate - a toolbar lays out hidden buttons too, so off Wolf
 * the wolf_speed button is REMOVED (where it was is kept in a per-account setting) and put back in the same place on
 * the way back.
 */
void WolfSpeed::gateToolbar(bool wolf)
{
    if (LLStartUp::getStartupState() < STATE_STARTED || !gToolBarView)
    {
        return;
    }
    const LLCommandId id(FLOATER);
    if (!wolf)
    {
        if (gToolBarView->hasCommand(id) == LLToolBarEnums::TOOLBAR_NONE)
        {
            return;
        }
        int rank = LLToolBar::RANK_NONE;
        const S32 where = gToolBarView->removeCommand(id, rank);
        if (where != LLToolBarEnums::TOOLBAR_NONE)
        {
            LLSD hidden = LLSD::emptyMap();
            hidden["toolbar"] = (LLSD::Integer)where;
            hidden["rank"] = (LLSD::Integer)rank;
            gSavedPerAccountSettings.setLLSD("WolfSpeedToolbarHidden", hidden);
        }
        return;
    }
    if (now() - mLastGate < 1.0 || !gToolBarView->getVisible())
    {
        return;
    }
    mLastGate = now();
    LLSD hidden = gSavedPerAccountSettings.getLLSD("WolfSpeedToolbarHidden");
    if (!hidden.isMap() || !hidden.has("toolbar"))
    {
        return;
    }
    const S32 where = hidden["toolbar"].asInteger();
    if (where >= LLToolBarEnums::TOOLBAR_FIRST && where <= LLToolBarEnums::TOOLBAR_LAST
        && gToolBarView->hasCommand(id) == LLToolBarEnums::TOOLBAR_NONE)
    {
        gToolBarView->addCommand(id, (LLToolBarEnums::EToolBarLocation)where, hidden["rank"].asInteger());
    }
    gSavedPerAccountSettings.setLLSD("WolfSpeedToolbarHidden", LLSD::emptyMap());
}

void WolfSpeed::idle()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    gateToolbar(wolf);
    if ((S32)wolf != mOnWolf)
    {
        mOnWolf = wolf ? 1 : 0;
        if (!wolf)
        {
            // [WOLF ONLY] left Wolf (hypergrid, or a login elsewhere): the window shuts and nothing is sent. The
            // region we left put the speed back to 1x when we left it (ScenePresence.MakeChildAgent).
            LLFloaterReg::hideInstance(FLOATER);
            forget();
            mWanted = 1.f;
            mMessage = Message();
            mRegion.setNull();
        }
    }
    if (!wolf)
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    const F64 t = now();
    if (region->getRegionID() != mRegion)
    {
        // a crossing or a teleport: the old region's answers are about somewhere else now
        mRegion = region->getRegionID();
        forget();
    }
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    // Say hello when it matters: the window is open, or a speed was picked (asked again after a crossing).
    const bool need = mWanted > 1.f || LLFloaterReg::instanceVisible(FLOATER);
    if (need && !available())
    {
        const F64 since = t - mHelloFirstAt;
        const F64 retry = since < HELLO_QUICK_FOR_S ? HELLO_RETRY_QUICK_S : since < HELLO_FAST_FOR_S ? HELLO_RETRY_FAST_S : HELLO_RETRY_S;
        if (!mHelloSent || t - mHelloAt > retry)
        {
            if (!mHelloSent)
            {
                mHelloFirstAt = t;
            }
            mHelloSent = true;
            mHelloAt = t;
            send_generic_message(METHOD_TO_SIM, { "hello" });
        }
    }
    if (mQueued > 0.f && available() && t - mLastSendAt >= SEND_EVERY_S)
    {
        const F32 x = mQueued;
        mQueued = 0.f;
        sendSet(x);
    }
    if (mSentAt > 0.0 && t - mSentAt > ANSWER_TIMEOUT_S)
    {
        mSentAt = 0.0;
        say(M_WARN, "NO ANSWER", "The region did not answer. Try again in a moment.", true);
    }
}

void WolfSpeed::receive(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // Only from a Wolf Territories region that has said so itself, and only the region we are in (wolfborder.cpp
    // receive's rule): on any other grid "WolfSpeed" is not ours.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || invoice != region->getRegionID())
    {
        return;
    }
    if (mRegion != invoice)
    {
        mRegion = invoice;
        forget();
    }
    std::string ev, why, via;
    F32 speed = mSpeed, force = mForce, maxv = mMax, grid = mGrid, rmax = mRMax;
    S32 allow = -1, edit = -1, paused = -1;
    for (const std::string& kv : strings)
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        // never trust the wire: text is shown as text, numbers are parsed and checked (wolfborder.cpp receive)
        auto num = [&v](F32& out, F32 lo, F32 hi)
        {
            try
            {
                size_t n = 0;
                const F64 d = std::stod(v, &n);
                if (n == v.size() && std::isfinite(d)) out = llclamp((F32)d, lo, hi);
            }
            catch (...) {}
        };
        if (k == "event") ev = utf8str_truncate(v, 16);
        else if (k == "why") why = utf8str_truncate(v, 220);
        else if (k == "via") via = utf8str_truncate(v, 8);
        else if (k == "speed") num(speed, 1.f, CEILING);
        else if (k == "force") num(force, 1.f, CEILING);
        else if (k == "max") num(maxv, 1.f, CEILING);
        else if (k == "grid") num(grid, 1.f, CEILING);
        else if (k == "rmax") num(rmax, 0.f, CEILING);
        else if (k == "allow") allow = v == "1" ? 1 : 0;
        else if (k == "edit") edit = v == "1" ? 1 : 0;
        else if (k == "paused") paused = v == "1" ? 1 : 0;
    }
    if (ev.empty())
    {
        return;
    }
    mModuleRegion = invoice;
    mSpeed = speed;
    mForce = force;
    mMax = maxv;
    mGrid = grid;
    mRMax = rmax;
    if (allow >= 0) mAllow = allow == 1;
    if (edit >= 0) mEdit = edit == 1;
    if (paused >= 0) mPaused = paused == 1;
    if (ev != "hello" && ev != "state" && ev != "blocked")
    {
        mSentAt = 0.0;   // the answer to what was sent
    }

    if (ev == "hello")
    {
        if (via == "cross" && mWanted > 1.f)
        {
            // a crossing: the same speed, asked of this region (which applies its own limits)
            sendSet(mWanted);
        }
        else if (via != "cross" && mWanted > 1.f)
        {
            mWanted = 1.f;
            say(M_INFO, "OFF", FEATURE_NAME + " went off with the " + std::string(via == "login" ? "login" : "teleport")
                + ". Pick a speed to go fast again.", false);
        }
    }
    else if (ev == "set")
    {
        mWanted = mSpeed;
        if (!why.empty())
        {
            say(M_INFO, mPaused ? "PAUSED" : "ON", why, false);
        }
        else if (mPaused && mSpeed > 1.f)
        {
            say(M_INFO, "PAUSED", "A script or game here is setting your speed. " + FEATURE_NAME + " waits until it lets go.", false);
        }
    }
    else if (ev == "limited")
    {
        mWanted = mSpeed;
        say(M_WARN, "LIMIT", why, true);
    }
    else if (ev == "denied")
    {
        mWanted = 1.f;
        say(M_ERROR, "NOT HERE", why, true);
    }
    else if (ev == "reset")
    {
        mWanted = 1.f;
        const bool sat = why.find("sat down") != std::string::npos;
        say(sat ? M_INFO : M_WARN, "OFF", why, !sat);
    }
    else if (ev == "state")
    {
        if (!why.empty())
        {
            say(M_INFO, mPaused ? "PAUSED" : "ON", why, false);
        }
    }
    else if (ev == "region")
    {
        say(M_OK, "SAVED", why, false);
    }
    else if (ev == "blocked")
    {
        say(M_ERROR, "NO ENTRY", why, true);
    }
    else if (ev == "busy")
    {
        mWanted = mSpeed;   // what is in force, not the pick the region refused
        say(M_WARN, "WAIT", why, true);
    }
    else if (ev == "error")
    {
        mWanted = mSpeed;
        say(M_ERROR, "ERROR", why, true);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// the window
// ═══════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Three looks (WolfSpeedTheme), each a dark panel and one accent - modelled on a sports car's instrument cluster
    // at night (a lit needle and arc over a dark face). Text is fixed light grey on the dark faces:
    // #eef2f6 on #0b1220 = 16.9:1; the dim #a9b4c2 = 8.6:1; amber #ffc24d = 11.6:1; red #ff8a80 = 7.9:1 (WCAG 2.x).
    struct Palette
    {
        const char* mName;
        LLColor4 mTop, mBottom, mFace, mAccent, mAccentDim;
    };
    const Palette PALETTES[] = {
        { "NIGHT", LLColor4(0.06f, 0.09f, 0.15f, 1.f), LLColor4(0.02f, 0.03f, 0.06f, 1.f), LLColor4(0.04f, 0.07f, 0.12f, 1.f),
          LLColor4(0.25f, 0.85f, 1.00f, 1.f), LLColor4(0.10f, 0.32f, 0.42f, 1.f) },
        { "EMBER", LLColor4(0.13f, 0.06f, 0.04f, 1.f), LLColor4(0.04f, 0.02f, 0.01f, 1.f), LLColor4(0.09f, 0.04f, 0.03f, 1.f),
          LLColor4(1.00f, 0.62f, 0.18f, 1.f), LLColor4(0.40f, 0.22f, 0.07f, 1.f) },
        { "WOLF", LLColor4(0.06f, 0.11f, 0.08f, 1.f), LLColor4(0.02f, 0.04f, 0.03f, 1.f), LLColor4(0.04f, 0.08f, 0.06f, 1.f),
          LLColor4(0.40f, 1.00f, 0.55f, 1.f), LLColor4(0.14f, 0.36f, 0.20f, 1.f) },
    };
    const S32 PALETTE_COUNT = 3;
    const LLColor4 TEXT(0.93f, 0.95f, 0.97f, 1.f);
    const LLColor4 TEXT_DIM(0.66f, 0.71f, 0.76f, 1.f);
    const LLColor4 AMBER(1.00f, 0.76f, 0.30f, 1.f);
    const LLColor4 RED(1.00f, 0.54f, 0.50f, 1.f);
    const LLColor4 GREEN(0.45f, 0.95f, 0.55f, 1.f);
    const LLColor4 LIMIT_ZONE(0.45f, 0.08f, 0.07f, 1.f);
    const LLColor4 BUTTON_TOP(0.17f, 0.19f, 0.22f, 1.f);
    const LLColor4 BUTTON_BOTTOM(0.09f, 0.10f, 0.12f, 1.f);
    const LLColor4 EDGE(0.30f, 0.33f, 0.37f, 1.f);
    const LLColor4 DARK_TEXT(0.03f, 0.04f, 0.06f, 1.f);

    const Palette& palette()
    {
        static LLCachedControl<S32> theme(gSavedSettings, "WolfSpeedTheme", 0);
        return PALETTES[llclamp((S32)theme, 0, PALETTE_COUNT - 1)];
    }

    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    void untextured() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }

    void vtx(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bottom)
    {
        untextured();   // a label drawn before leaves the font atlas bound (LLFontGL::render)
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bottom); vtx(r, t, top); vtx(l, t, top);
        vtx(l, b, bottom); vtx(r, b, bottom); vtx(r, t, top);
        gGL.end();
    }

    void frame(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectG(l, b, r, b + w, c, c);
        rectG(l, t - w, r, t, c, c);
        rectG(l, b, l + w, t, c, c);
        rectG(r - w, b, r, t, c, c);
    }

    // A band of a circle between radii r0 and r1, from angle a0 to a1 (radians, counter-clockwise from +x).
    void band(F32 cx, F32 cy, F32 r0, F32 r1, F32 a0, F32 a1, const LLColor4& c0, const LLColor4& c1)
    {
        untextured();   // a label drawn before leaves the font atlas bound (LLFontGL::render)
        const S32 n = llclamp((S32)(fabsf(a1 - a0) * r1 / 4.f), 6, 160);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 t0 = (F32)i / n, t1 = (F32)(i + 1) / n;
            const F32 p = a0 + (a1 - a0) * t0, q = a0 + (a1 - a0) * t1;
            const LLColor4 k0 = lerp(c0, c1, t0), k1 = lerp(c0, c1, t1);
            vtx(cx + r0 * cosf(p), cy + r0 * sinf(p), k0);
            vtx(cx + r1 * cosf(p), cy + r1 * sinf(p), k0);
            vtx(cx + r1 * cosf(q), cy + r1 * sinf(q), k1);
            vtx(cx + r0 * cosf(p), cy + r0 * sinf(p), k0);
            vtx(cx + r1 * cosf(q), cy + r1 * sinf(q), k1);
            vtx(cx + r0 * cosf(q), cy + r0 * sinf(q), k1);
        }
        gGL.end();
    }

    void disc(F32 cx, F32 cy, F32 r, const LLColor4& centre, const LLColor4& rim)
    {
        untextured();   // a label drawn before leaves the font atlas bound (LLFontGL::render)
        const S32 n = llclamp((S32)(r * 0.8f), 24, 120);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 a0 = F_TWO_PI * i / n, a1 = F_TWO_PI * (i + 1) / n;
            vtx(cx, cy, centre);
            vtx(cx + r * cosf(a0), cy + r * sinf(a0), rim);
            vtx(cx + r * cosf(a1), cy + r * sinf(a1), rim);
        }
        gGL.end();
    }

    void line(F32 x0, F32 y0, F32 x1, F32 y1, F32 w, const LLColor4& c)
    {
        untextured();   // a label drawn before leaves the font atlas bound (LLFontGL::render)
        const F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        gGL.begin(LLRender::TRIANGLES);
        vtx(x0 + nx, y0 + ny, c); vtx(x1 + nx, y1 + ny, c); vtx(x1 - nx, y1 - ny, c);
        vtx(x0 + nx, y0 + ny, c); vtx(x1 - nx, y1 - ny, c); vtx(x0 - nx, y0 - ny, c);
        gGL.end();
    }

    // The dial: 1x at the lower left (225 degrees), CEILING at the lower right (-45 degrees), 270 degrees of sweep -
    // the layout of a car's speedometer.
    const F32 DIAL_FROM = 225.f * DEG_TO_RAD, DIAL_SWEEP = 270.f * DEG_TO_RAD;
    F32 angleOf(F32 v) { return DIAL_FROM - (llclamp(v, 1.f, WolfSpeed::CEILING) - 1.f) / (WolfSpeed::CEILING - 1.f) * DIAL_SWEEP; }

    // Display lettering: the WolfDisplay family (fonts.xml Cinzel-Bold) at the Avionics0..11 sizes - the registration
    // wolffloatercompass.cpp displayFonts() and wolfflightdeck.cpp avionics() use.
    const LLFontGL* displayFont(F32 px)
    {
        static std::vector<const LLFontGL*> fonts;
        if (fonts.empty())
        {
            for (S32 i = 0; i < 12; ++i)
            {
                const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("WolfDisplay", llformat("Avionics%d", i), LLFontGL::NORMAL));
                if (f) fonts.push_back(f);
            }
            if (fonts.empty()) fonts.push_back(LLFontGL::getFontSansSerifBig());
        }
        const LLFontGL* best = fonts.front();
        for (const LLFontGL* f : fonts)
        {
            if ((F32)f->getLineHeight() <= px) best = f;
        }
        return best;
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c, LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        f->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // Words wrapped to width (at most max_lines; the last one ends "..." when cut).
    std::vector<std::string> wrap(const LLFontGL* f, const std::string& s, F32 width, S32 max_lines)
    {
        std::vector<std::string> out;
        std::string cur;
        size_t i = 0;
        while (i < s.size())
        {
            size_t j = s.find(' ', i);
            if (j == std::string::npos) j = s.size();
            const std::string word = s.substr(i, j - i);
            const std::string tryline = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && f->getWidthF32(tryline) > width)
            {
                out.push_back(cur);
                cur = word;
            }
            else
            {
                cur = tryline;
            }
            i = j + 1;
        }
        if (!cur.empty()) out.push_back(cur);
        if ((S32)out.size() > max_lines)
        {
            out.resize(max_lines);
            out.back() += "...";
        }
        return out;
    }
}

WolfFloaterSpeed::WolfFloaterSpeed(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterSpeed::postBuild()
{
    WolfSpeed::getInstance();
    return true;
}

void WolfFloaterSpeed::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly("Xenon's WolfSpeed");   // [WOLF GRID GATE 2026-10-17/]
        closeFloater();   // Wolf Territories only (the menu and the button are not there elsewhere either)
        return;
    }
    mNeedle = WolfSpeed::instance().inForce();
    mEditDirty = false;
    mDragging = false;
    mPressed = H_NONE;
    mFrameTimer.reset();
    if (!WolfSpeed::instance().openingQuietly())
    {
        setFocus(true);   // opened by the person: the arrow and number keys work at once
    }
}

F32 WolfFloaterSpeed::presetValue(EHit h) const
{
    switch (h)
    {
    case H_WALK: return PRESET_WALK;
    case H_RUN: return PRESET_RUN;
    case H_FAST: return PRESET_FAST;
    case H_SUPER: return llmax(1.f, WolfSpeed::instance().maxHere());
    default: return 1.f;
    }
}

F32 WolfFloaterSpeed::dialValueAt(S32 x, S32 y) const
{
    F32 a = atan2f((F32)y - mDialCY, (F32)x - mDialCX);   // -pi..pi
    if (a < DIAL_FROM - DIAL_SWEEP - 0.5f * (F_TWO_PI - DIAL_SWEEP)) a += F_TWO_PI;
    F32 v = 1.f + (DIAL_FROM - a) / DIAL_SWEEP * (WolfSpeed::CEILING - 1.f);
    v = llclamp(snapHalf(v), 1.f, WolfSpeed::CEILING);
    return llmin(v, llmax(1.f, WolfSpeed::instance().maxHere()));   // never past what the region allows
}

WolfFloaterSpeed::EHit WolfFloaterSpeed::hitAt(S32 x, S32 y) const
{
    for (S32 i = H_WALK; i < H_COUNT; ++i)
    {
        if (mHit[i].notEmpty() && mHit[i].pointInRect(x, y)) return (EHit)i;
    }
    if (mDialR > 0.f)
    {
        const F32 dx = (F32)x - mDialCX, dy = (F32)y - mDialCY;
        const F32 d = sqrtf(dx * dx + dy * dy);
        if (d <= mDialR * 1.08f && d >= mDialR * 0.55f) return H_DIAL;
    }
    return H_NONE;
}

void WolfFloaterSpeed::press(EHit h)
{
    WolfSpeed& ws = WolfSpeed::instance();
    const F32 grid = ws.gridMax();
    switch (h)
    {
    case H_WALK: case H_RUN: case H_FAST: case H_SUPER:
        // never more than the region allows (a preset above it is drawn disabled)
        ws.request(llmin(presetValue(h), llmax(1.f, ws.maxHere())));
        break;
    case H_OFF:
        ws.request(1.f);
        break;
    case H_THEME:
        gSavedSettings.setS32("WolfSpeedTheme", (gSavedSettings.getS32("WolfSpeedTheme") + 1) % PALETTE_COUNT);
        break;
    case H_DISMISS:
        ws.dismissMessage();
        break;
    case H_REGION_ALLOW:
        mEditAllow = !mEditAllow;
        mEditDirty = true;
        break;
    case H_CAP_DOWN:
        // 0 = no cap (the grid's maximum); below that in half steps down to 1x
        mEditCap = (mEditCap <= 0.f) ? llmax(1.f, grid - 0.5f) : llmax(1.f, mEditCap - 0.5f);
        mEditDirty = true;
        break;
    case H_CAP_UP:
        if (mEditCap > 0.f)
        {
            mEditCap += 0.5f;
            if (mEditCap >= grid) mEditCap = 0.f;
        }
        mEditDirty = true;
        break;
    case H_REGION_SAVE:
        if (mEditDirty)
        {
            ws.requestRegion(mEditAllow, mEditCap);
            mEditDirty = false;
        }
        break;
    default:
        break;
    }
}

bool WolfFloaterSpeed::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const EHit h = hitAt(x, y);
    if (h == H_NONE)
    {
        return LLFloater::handleMouseDown(x, y, mask);
    }
    setFocus(true);
    if (h == H_DIAL)
    {
        if (WolfSpeed::instance().available())
        {
            mDragging = true;
            mDragValue = dialValueAt(x, y);
            gFocusMgr.setMouseCapture(this);
        }
        return true;
    }
    mPressed = h;
    gFocusMgr.setMouseCapture(this);   // the release comes here even off the button, so it never sticks "pressed"
    return true;
}

void WolfFloaterSpeed::onMouseCaptureLost()
{
    // hidden, or the capture taken away: no drag or press left half done (nothing is sent)
    mDragging = false;
    mPressed = H_NONE;
}

bool WolfFloaterSpeed::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
        WolfSpeed::instance().request(dialValueAt(x, y));   // sent on release, not while dragging
        return true;
    }
    if (mPressed != H_NONE)
    {
        const EHit h = hitAt(x, y);
        const EHit pressed = mPressed;
        mPressed = H_NONE;
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        if (h == pressed)
        {
            press(h);
        }
        return true;
    }
    return LLFloater::handleMouseUp(x, y, mask);
}

bool WolfFloaterSpeed::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = hitAt(x, y);
    if (mDragging)
    {
        mDragValue = dialValueAt(x, y);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    if (mHover != H_NONE)
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}

bool WolfFloaterSpeed::handleToolTip(S32 x, S32 y, MASK mask)
{
    std::string tip;
    switch (hitAt(x, y))
    {
    case H_DIAL: tip = "Drag or click the dial to pick a speed (up to what this region allows). Keys: left / right."; break;
    case H_WALK: tip = "Walk: 1.5x your normal speed (key 1)"; break;
    case H_RUN: tip = "Run: 2x your normal speed (key 2)"; break;
    case H_FAST: tip = "Fast: 3x your normal speed (key 3)"; break;
    case H_SUPER: tip = "Super: the most this region allows (key 4)"; break;
    case H_OFF: tip = "Off: back to your normal speed (key 0)"; break;
    case H_THEME: tip = "Change the look of this window"; break;
    case H_DISMISS: tip = "Dismiss this message"; break;
    case H_REGION_ALLOW: tip = "Region owners: allow " + FEATURE_NAME + " in this region or not (saved with SAVE)"; break;
    case H_CAP_DOWN: case H_CAP_UP: tip = "Region owners: the most anyone may go here (saved with SAVE)"; break;
    case H_REGION_SAVE: tip = "Save the region's settings. Everyone here faster than the new limit is slowed down."; break;
    default: return LLFloater::handleToolTip(x, y, mask);
    }
    LLToolTipMgr::instance().show(tip);
    return true;
}

bool WolfFloaterSpeed::handleKeyHere(KEY key, MASK mask)
{
    if (mask != MASK_NONE)
    {
        return LLFloater::handleKeyHere(key, mask);
    }
    WolfSpeed& ws = WolfSpeed::instance();
    switch (key)
    {
    case KEY_LEFT: case KEY_DOWN:
        ws.request(llmax(1.f, snapHalf(ws.shownSpeed()) - 0.5f));
        return true;
    case KEY_RIGHT: case KEY_UP:
        ws.request(llmin(ws.maxHere(), snapHalf(ws.shownSpeed()) + 0.5f));
        return true;
    case '1': press(H_WALK); return true;
    case '2': press(H_RUN); return true;
    case '3': press(H_FAST); return true;
    case '4': press(H_SUPER); return true;
    case '0': case KEY_BACKSPACE: press(H_OFF); return true;
    default:
        return LLFloater::handleKeyHere(key, mask);
    }
}

void WolfFloaterSpeed::drawButton(EHit h, const std::string& label, const std::string& sub, bool lit, bool enabled)
{
    const LLRect& r = mHit[h];
    if (r.isEmpty()) return;
    const Palette& p = palette();
    const F32 l = (F32)r.mLeft, b = (F32)r.mBottom, rt = (F32)r.mRight, t = (F32)r.mTop;
    const bool hot = enabled && (mHover == h || mPressed == h);
    if (lit)
    {
        rectG(l, b, rt, t, p.mAccent, lerp(p.mAccent, p.mAccentDim, 0.45f));
    }
    else
    {
        rectG(l, b, rt, t, hot ? lerp(BUTTON_TOP, p.mAccentDim, 0.5f) : BUTTON_TOP, BUTTON_BOTTOM);
    }
    frame(l, b, rt, t, hot ? 2.f : 1.f, hot ? p.mAccent : EDGE);
    const LLColor4 ink = !enabled ? alpha(TEXT_DIM, 0.6f) : lit ? DARK_TEXT : TEXT;
    const LLFontGL* big = LLFontGL::getFontSansSerifBold();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 cx = (l + rt) * 0.5f, cy = (b + t) * 0.5f;
    if (sub.empty())
    {
        text(big, label, cx, cy, ink);
    }
    else
    {
        text(big, label, cx, cy + small->getLineHeight() * 0.45f, ink);
        text(small, sub, cx, cy - big->getLineHeight() * 0.5f, lit ? DARK_TEXT : TEXT_DIM);
    }
}

void WolfFloaterSpeed::drawDial(F32 cx, F32 cy, F32 R)
{
    WolfSpeed& ws = WolfSpeed::instance();
    const Palette& p = palette();
    const bool avail = ws.available();
    const F32 maxv = avail ? ws.maxHere() : 1.f;

    // the face
    disc(cx, cy, R * 1.06f, alpha(LLColor4::black, 0.55f), alpha(LLColor4::black, 0.f));
    disc(cx, cy, R, p.mFace, lerp(p.mFace, LLColor4::black, 0.5f));
    band(cx, cy, R * 0.97f, R, 0.f, F_TWO_PI, EDGE, EDGE);

    // the track: allowed (accent, dim), above the region's limit (dark red, labelled LIMIT below)
    const F32 r0 = R * 0.78f, r1 = R * 0.90f;
    band(cx, cy, r0, r1, angleOf(maxv), angleOf(1.f), p.mAccentDim, p.mAccentDim);
    if (maxv < WolfSpeed::CEILING)
    {
        band(cx, cy, r0, r1, angleOf(WolfSpeed::CEILING), angleOf(maxv), LIMIT_ZONE, LIMIT_ZONE);
        // hatching across the limit zone, so it is not colour alone
        for (F32 v = maxv + 0.25f; v < WolfSpeed::CEILING; v += 0.25f)
        {
            const F32 a = angleOf(v);
            line(cx + r0 * cosf(a), cy + r0 * sinf(a), cx + r1 * cosf(a - 0.04f), cy + r1 * sinf(a - 0.04f), 1.5f, alpha(RED, 0.8f));
        }
    }
    // the lit arc up to the speed shown
    const F32 shown = mDragging ? mDragValue : mNeedle;
    if (avail && shown > 1.01f)
    {
        band(cx, cy, r0, r1, angleOf(shown), angleOf(1.f), p.mAccent, lerp(p.mAccent, LLColor4::white, 0.25f));
    }
    // ticks and numerals
    const LLFontGL* nf = displayFont(llclamp(R * 0.13f, 10.f, 26.f));
    for (S32 i = 0; i <= (S32)((WolfSpeed::CEILING - 1.f) * 2.f); ++i)
    {
        const F32 v = 1.f + i * 0.5f;
        const F32 a = angleOf(v);
        const bool major = (i % 2) == 0;
        const F32 ti = major ? R * 0.68f : R * 0.73f;
        line(cx + ti * cosf(a), cy + ti * sinf(a), cx + R * 0.77f * cosf(a), cy + R * 0.77f * sinf(a), major ? 2.5f : 1.2f,
             v <= maxv ? TEXT : alpha(TEXT_DIM, 0.6f));
        if (major)
        {
            const F32 tr = R * 0.56f;
            text(nf, llformat("%d", (S32)v), cx + tr * cosf(a), cy + tr * sinf(a), v <= maxv ? TEXT : TEXT_DIM);
        }
    }
    // the needle
    if (avail)
    {
        const F32 a = angleOf(shown);
        line(cx - R * 0.12f * cosf(a), cy - R * 0.12f * sinf(a), cx + R * 0.86f * cosf(a), cy + R * 0.86f * sinf(a),
             llmax(2.f, R * 0.035f), lerp(p.mAccent, LLColor4::white, 0.35f));
        disc(cx, cy, R * 0.07f, EDGE, BUTTON_BOTTOM);
    }

    // the readout in the middle: the speed in force, what it means, and the state in a word
    const LLFontGL* bigf = displayFont(llclamp(R * 0.30f, 14.f, 64.f));
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    if (!avail)
    {
        text(bigf, "--", cx, cy - R * 0.30f, TEXT_DIM);
        return;
    }
    const F32 x = mDragging ? mDragValue : ws.inForce();
    text(bigf, times(x), cx, cy - R * 0.28f, TEXT);
    std::string word = ws.paused() && ws.speed() > 1.f ? "PAUSED" : (ws.inForce() > 1.f ? "ON" : "OFF");
    const LLColor4 wc = word == "PAUSED" ? AMBER : word == "ON" ? GREEN : TEXT_DIM;
    text(LLFontGL::getFontSansSerifBold(), word, cx, cy - R * 0.52f, wc);
    text(small, llformat("run about %d km/h  -  fly about %d km/h", ll_round(RUN_MS * x * 3.6f), ll_round(FLY_MS * x * 3.6f)),
         cx, cy - R * 0.70f, TEXT_DIM);
    if (maxv < WolfSpeed::CEILING)
    {
        const F32 a = angleOf((maxv + WolfSpeed::CEILING) * 0.5f);
        text(small, maxv <= 1.f ? "OFF HERE" : "LIMIT", cx + R * 1.0f * cosf(a) * 0.98f, cy + R * 1.0f * sinf(a) * 0.98f - small->getLineHeight(), RED);
    }
}

void WolfFloaterSpeed::draw()
{
    LLFloater::draw();
    if (isMinimized())
    {
        return;
    }
    for (S32 i = 0; i < H_COUNT; ++i) mHit[i] = LLRect();
    mDialR = 0.f;

    WolfSpeed& ws = WolfSpeed::instance();
    const Palette& p = palette();
    const bool avail = ws.available();
    if (!mEditDirty)
    {
        mEditAllow = ws.regionAllows();
        mEditCap = ws.regionCap();
    }
    // the needle eases towards the speed in force
    const F32 dt = llclamp(mFrameTimer.getElapsedTimeAndResetF32(), 0.f, 0.25f);
    mNeedle += (ws.inForce() - mNeedle) * llclamp(dt * 8.f, 0.f, 1.f);

    const LLRect local = getLocalRect();
    const S32 top = local.mTop - getHeaderHeight();
    const S32 bottom = local.mBottom + LLPANEL_BORDER_WIDTH;
    const S32 left = local.mLeft + LLPANEL_BORDER_WIDTH;
    const S32 right = local.mRight - LLPANEL_BORDER_WIDTH;
    if (top - bottom < 120 || right - left < 120)
    {
        return;
    }
    untextured();
    rectG((F32)left, (F32)bottom, (F32)right, (F32)top, p.mTop, p.mBottom);

    const S32 M = 8;                     // one spacing step
    const S32 W = right - left - 2 * M;
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const S32 btn_h = llclamp((top - bottom) / 11, 34, 52);
    const bool owner = ws.canEditRegion();

    // bottom up: the region owner's row, the message, Off, the presets; the dial takes what is left
    S32 y = bottom + M;
    if (owner)
    {
        const S32 rh = llmax(28, btn_h - 8);
        const S32 w4 = (W - 3 * M) / 4;
        mHit[H_REGION_ALLOW] = LLRect(left + M, y + rh, left + M + w4 + w4 / 2, y);
        mHit[H_CAP_DOWN] = LLRect(mHit[H_REGION_ALLOW].mRight + M, y + rh, mHit[H_REGION_ALLOW].mRight + M + rh, y);
        mHit[H_REGION_SAVE] = LLRect(right - M - w4, y + rh, right - M, y);
        mHit[H_CAP_UP] = LLRect(mHit[H_REGION_SAVE].mLeft - M - rh, y + rh, mHit[H_REGION_SAVE].mLeft - M, y);
        drawButton(H_REGION_ALLOW, mEditAllow ? "HERE: ON" : "HERE: OFF", "", mEditAllow, true);
        drawButton(H_CAP_DOWN, "-", "", false, mEditAllow);
        drawButton(H_CAP_UP, "+", "", false, mEditAllow && mEditCap > 0.f);
        drawButton(H_REGION_SAVE, "SAVE", "", mEditDirty, mEditDirty);
        const F32 capx = (mHit[H_CAP_DOWN].mRight + mHit[H_CAP_UP].mLeft) * 0.5f;
        text(small, "MOST HERE", capx, (F32)(y + rh) - small->getLineHeight() * 0.6f, TEXT_DIM);
        text(bold, mEditCap > 0.f ? times(mEditCap) : "NO CAP", capx, (F32)y + rh * 0.35f, mEditAllow ? TEXT : TEXT_DIM);
        y += rh + M / 2;
        text(small, "REGION SETTINGS (you manage this region)", (F32)(left + M), (F32)y + small->getLineHeight() * 0.5f, TEXT_DIM, LLFontGL::LEFT);
        y += small->getLineHeight() + M;
    }

    // the message panel: a coloured bar and a word for the level, the text, and a dismiss button. It stays until
    // dismissed or replaced by the next pick.
    const S32 msg_h = body->getLineHeight() * 3 + M;
    {
        const LLRect panel(left + M, y + msg_h, right - M, y);
        rectG((F32)panel.mLeft, (F32)panel.mBottom, (F32)panel.mRight, (F32)panel.mTop, alpha(BUTTON_BOTTOM, 0.9f), alpha(BUTTON_BOTTOM, 0.9f));
        WolfSpeed::Message m = ws.message();
        if (m.mLevel == WolfSpeed::M_NONE)
        {
            // no event to tell: what is going on now
            m.mLevel = WolfSpeed::M_INFO;
            if (!WolfGrid::isOnWolfTerritoriesConfirmed() || (!avail && ws.waiting()))
            {
                m.mWord = "WAIT";
                m.mText = "Asking this region about " + FEATURE_NAME + "...";
            }
            else if (!avail)
            {
                m.mWord = "NOT HERE";
                m.mText = "This region does not offer " + FEATURE_NAME + " yet. It comes with the region's next restart.";
            }
            else if (ws.maxHere() <= 1.f)
            {
                m.mLevel = WolfSpeed::M_WARN;
                m.mWord = "OFF HERE";
                m.mText = "The owner of this region has switched " + FEATURE_NAME + " off here.";
            }
            else if (ws.paused() && ws.speed() > 1.f)
            {
                m.mWord = "PAUSED";
                m.mText = "A script or game here is setting your speed. " + FEATURE_NAME + " waits until it lets go.";
            }
            else
            {
                m.mWord = ws.inForce() > 1.f ? "ON" : "READY";
                m.mText = "This region allows up to " + times(ws.maxHere()) + ". Pick a speed, or press Off.";
            }
        }
        const LLColor4 bar = m.mLevel == WolfSpeed::M_ERROR ? RED : m.mLevel == WolfSpeed::M_WARN ? AMBER
                           : m.mLevel == WolfSpeed::M_OK ? GREEN : p.mAccent;
        rectG((F32)panel.mLeft, (F32)panel.mBottom, (F32)panel.mLeft + 4.f, (F32)panel.mTop, bar, bar);
        const F32 tx = (F32)panel.mLeft + 4.f + M;
        F32 ty = (F32)panel.mTop - body->getLineHeight() * 0.6f - 2.f;
        text(bold, m.mWord, tx, ty, bar, LLFontGL::LEFT);
        const bool can_dismiss = ws.message().mLevel != WolfSpeed::M_NONE;
        if (can_dismiss)
        {
            const S32 d = body->getLineHeight() + 4;
            mHit[H_DISMISS] = LLRect(panel.mRight - d - 2, panel.mTop - 2, panel.mRight - 2, panel.mTop - 2 - d);
            drawButton(H_DISMISS, "x", "", false, true);
        }
        const F32 text_w = (F32)panel.getWidth() - 4.f - 2.f * M - (can_dismiss ? body->getLineHeight() + 6.f : 0.f);
        const F32 word_w = bold->getWidthF32(m.mWord) + M;
        // the first line follows the word, the rest use the full width
        std::vector<std::string> lines = wrap(body, m.mText, text_w - word_w, 1);
        std::string rest = m.mText.substr(llmin(m.mText.size(), lines.empty() ? 0 : lines[0].size()));
        if (!lines.empty() && lines[0].size() >= 3 && lines[0].compare(lines[0].size() - 3, 3, "...") == 0)
        {
            // the first line was cut: wrap the whole text below the word instead
            lines.clear();
            rest = m.mText;
        }
        if (!lines.empty())
        {
            text(body, lines[0], tx + word_w, ty, TEXT, LLFontGL::LEFT);
        }
        LLStringUtil::trim(rest);
        for (const std::string& l : wrap(body, rest, text_w, 2))
        {
            ty -= body->getLineHeight();
            text(body, l, tx, ty, TEXT, LLFontGL::LEFT);
        }
        y += msg_h + M;
    }

    // Off: full width, its own row, set apart from the presets (it is the way back)
    mHit[H_OFF] = LLRect(left + M, y + btn_h, right - M, y);
    drawButton(H_OFF, "OFF", "normal speed", avail && ws.inForce() <= 1.f && ws.speed() <= 1.f, avail);
    y += btn_h + M;

    // the presets
    {
        const S32 bw = (W - 3 * M) / 4;
        const EHit hs[4] = { H_WALK, H_RUN, H_FAST, H_SUPER };
        const char* names[4] = { "WALK", "RUN", "FAST", "SUPER" };
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 l = left + M + i * (bw + M);
            mHit[hs[i]] = LLRect(l, y + btn_h, l + bw, y);
            const F32 v = presetValue(hs[i]);
            const bool allowed = avail && v <= ws.maxHere() + 0.001f && (hs[i] == H_SUPER ? ws.maxHere() > 1.f : true);
            const bool lit = avail && ws.speed() > 1.f && fabsf(ws.speed() - v) < 0.05f;
            drawButton(hs[i], names[i], hs[i] == H_SUPER ? times(llmax(1.f, ws.maxHere())) : times(v), lit, allowed);
        }
        y += btn_h + M;
    }

    // the title strip with THEME, then the dial in what is left
    const S32 title_h = bold->getLineHeight() + M;
    const S32 theme_w = llmax(70, (S32)small->getWidthF32("THEME: NIGHT") + 2 * M);
    mHit[H_THEME] = LLRect(right - M - theme_w, top - M / 2, right - M, top - title_h);
    drawButton(H_THEME, std::string("THEME: ") + p.mName, "", false, true);
    text(bold, "XENON'S WOLFSPEED", (F32)(left + M), (F32)(top - title_h / 2 - M / 4), p.mAccent, LLFontGL::LEFT);

    const S32 dial_top = top - title_h - M;
    const S32 avail_h = dial_top - y;
    const F32 R = llmax(30.f, llmin((F32)W * 0.46f, (F32)avail_h * 0.52f));
    mDialCX = (F32)(left + right) * 0.5f;
    mDialCY = (F32)dial_top - R * 1.02f;
    mDialR = R;
    drawDial(mDialCX, mDialCY, R);
}
