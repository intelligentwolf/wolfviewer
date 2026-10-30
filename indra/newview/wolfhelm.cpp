/**
 * @file wolfhelm.cpp
 * @brief WolfViewer: the Captain's Helm link to a ship's helm script. See wolfhelm.h and
 *        region-terrain-work/pirate-ship/DESIGN.md s20 (the protocol, both sides).
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

#include "wolfhelm.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llchat.h"
#include "llframetimer.h"
#include "llstartup.h"
#include "lltoolbarview.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

namespace
{
    // Source: DESIGN.md s20.2 / hms_wolfy_helm_template.lsl HUD = "WolfHelm1|".
    const std::string PREFIX("WolfHelm1|");
    // Source: hms_wolfy_helm_template.lsl HudOrder "hello" answers with ver=1.
    const S32 PROTOCOL_VERSION = 1;
    // Source: build_ship.py object_desc "wolfvehicle:warship - <name>, <class>" (the delivered root's description).
    const std::string SHIP_DESC("wolfvehicle:warship");
    // Source: make_sailing_lsl.py CMD_CH = 7 - the channel a hello goes on before the ship has named its own ("ch").
    const S32 HELLO_CHANNEL = 7;
    // A hello unanswered this long is asked again, at most this many times per seat (a ship's helm script that is
    // not running, or a vessel that does not speak the protocol, then gets no more chat from this viewer).
    const F64 HELLO_RETRY = 4.0;
    const S32 HELLO_TRIES = 3;
    // The ship sends on change only; this long without a line (a helm script reset forgets its HUDs) says hello again.
    const F64 RESUBSCRIBE = 60.0;
    // A command with no line back within this long: the HUD says the ship is not answering.
    const F64 ANSWER_WAIT = 3.0;
    const F64 ANSWER_SHOW = 10.0;
    // Dragging the wheel: "hud helm" at most 5 a second; the wind steppers: sent once they rest this long.
    const F64 HELM_GAP = 0.2;
    const F64 WIND_SETTLE = 0.6;
    // Source: llRegionSayTo truncates at 1023 characters (LSL_Api.cs llRegionSayTo msg[..1023]).
    const size_t MAX_LINE = 1023;
    const size_t MAX_FIELDS = 48;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // Source: wolfdrive.cpp seatRoot - the root of what the agent sits on, or null.
    LLViewerObject* seatRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            return nullptr;
        }
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent())
        {
            p = (LLViewerObject*)p->getParent();
        }
        return p;
    }

    // Source: wolfdrive.cpp parseNumber - plain decimals only, never trust the wire.
    bool parseNumber(const std::string& s, F64& out)
    {
        if (s.empty() || s.size() > 24) return false;
        for (char c : s)
        {
            if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        }
        errno = 0;
        char* end = nullptr;
        const double v = strtod(s.c_str(), &end);
        if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    F32 num(const std::string& s, F32 lo, F32 hi, F32 fallback)
    {
        F64 v = 0.0;
        return parseNumber(s, v) ? (F32)llclamp(v, (F64)lo, (F64)hi) : fallback;
    }

    std::vector<std::string> split(const std::string& s, char sep, size_t max_fields)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (out.size() < max_fields)
        {
            const size_t b = s.find(sep, a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }

    // Free text from the ship, for drawing: printable, at most 200 characters.
    std::string cleanText(const std::string& s)
    {
        size_t n = llmin(s.size(), (size_t)200);
        while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) --n;   // not inside a UTF-8 character
        std::string out = s.substr(0, n);
        for (char& c : out)
        {
            if ((unsigned char)c < 0x20) c = ' ';
        }
        return out;
    }

    F32 wrap360(F32 a)
    {
        while (a >= 360.f) a -= 360.f;
        while (a < 0.f) a += 360.f;
        return a;
    }
}

WolfHelm::WolfHelm()
{
}

WolfHelm::~WolfHelm()
{
}

// static
bool WolfHelm::onWolf()
{
    return WolfGrid::isOnWolfTerritories();
}

bool WolfHelm::isCaptain() const
{
    return mState.mHaveState && mState.mCaptain.notNull() && mState.mCaptain == gAgentID;
}

void WolfHelm::reset(const LLUUID& seat)
{
    mSeatRoot = seat;
    mState = State();
    mShipDesc = false;
    mDescKnown = false;
    mHelloTries = 0;
    mHelloAt = 0.0;
    mLastLineAt = 0.0;
    mLastCommandAt = 0.0;
    mAutoDone = false;
    mHelmPending = false;
    mWindPending = false;
    mStateAsked = false;
    mError.clear();
}

void WolfHelm::send(const std::string& text)
{
    // Sending to the region only once it has itself said it is Wolf (wolfgrid.h isOnWolfTerritoriesConfirmed), as
    // WolfDrive::canSend does: a ship's channel is never shouted on a foreign grid.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || mSeatRoot.isNull())
    {
        return;
    }
    const S32 ch = mState.mHi ? mState.mChannel : HELLO_CHANNEL;
    // Shouted: the helm listens in the root prim, about 26 m from the wheel, beyond say's 20 m (DESIGN.md s20.3).
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_SHOUT, false, ch);
}

void WolfHelm::sayHello()
{
    mHelloAt = now();
    ++mHelloTries;
    send(llformat("hud hello %d", PROTOCOL_VERSION));
}

void WolfHelm::order(const std::string& words)
{
    if (!onWolf() || !mState.mHi) return;
    mLastCommandAt = now();
    send(words);
}

void WolfHelm::setHelm(F32 degrees)
{
    if (!onWolf() || !mState.mHi) return;
    mHelmWant = llclamp(degrees, -mState.mHelmMax, mState.mHelmMax);
    mHelmPending = true;
}

void WolfHelm::battle(bool on)
{
    order(on ? "hud battle on" : "hud battle off");
}

void WolfHelm::fire(bool starboard)
{
    order(starboard ? "hud fire starboard" : "hud fire port");
}

void WolfHelm::setFixedWind(F32 knots, F32 from_deg)
{
    if (!onWolf() || !mState.mHi) return;
    // Source: hms_wolfy_helm_template.lsl Order "wind <knots 0-60> <degrees it blows from>".
    mWindKn = llclamp(knots, 0.f, 60.f);
    mWindFrom = wrap360(from_deg);
    mWindPending = true;
    mWindChangedAt = now();
    // Shown at once as the steppers move; the ship's next "st" confirms it.
    mState.mWind = 2;
    mState.mTws = mWindKn;
    mState.mTwf = mWindFrom;
}

F32 WolfHelm::heading() const
{
    if (const LLViewerObject* root = gObjectList.findObject(mSeatRoot))
    {
        // Source: hms_wolfy_helm_template.lsl ReadOut - compass = 90 - yaw of the root's +X (the bow).
        const LLVector3 fwd = LLVector3::x_axis * root->getRotation();
        if (fabsf(fwd.mV[VX]) + fabsf(fwd.mV[VY]) > 0.01f)
        {
            return wrap360(90.f - atan2f(fwd.mV[VY], fwd.mV[VX]) * RAD_TO_DEG);
        }
    }
    return mState.mHdg;
}

F32 WolfHelm::reloadLeft(bool starboard) const
{
    return (F32)llmax(0.0, (starboard ? mState.mStbdReadyAt : mState.mPortReadyAt) - now());
}

F32 WolfHelm::reloadFraction(bool starboard) const
{
    const F32 left = reloadLeft(starboard);
    const F32 full = starboard ? mState.mStbdFull : mState.mPortFull;
    if (left <= 0.f) return 1.f;
    if (full <= 0.f) return 0.f;
    return llclamp(1.f - left / full, 0.f, 1.f);
}

F32 WolfHelm::lockLeft() const
{
    return (F32)llmax(0.0, mState.mLockUntil - now());
}

F32 WolfHelm::protectedLeft() const
{
    return (F32)llmax(0.0, mState.mProtectedUntil - now());
}

F32 WolfHelm::helmShown() const
{
    return mHelmPending ? mHelmWant : mState.mHelm;   // the wheel follows a drag at once
}

bool WolfHelm::hailing() const
{
    return mSeatRoot.notNull() && !mState.mHi && mError.empty()
        && (mHelloTries < HELLO_TRIES || now() - mHelloAt < HELLO_RETRY);
}

bool WolfHelm::noAnswer() const
{
    // only for a while: an order that changed nothing may get no line back (the ship sends on change)
    const F64 since = now() - mLastCommandAt;
    return mLastCommandAt > 0.0 && mLastLineAt < mLastCommandAt && since > ANSWER_WAIT && since < ANSWER_SHOW;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// from the ship
// ════════════════════════════════════════════════════════════════════════════════════════

bool WolfHelm::onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0)
    {
        return false;
    }
    // [WOLF ONLY] off Wolf the line is left to chat like any other (no ship sends one to a viewer that did not say
    // hello, and this viewer says nothing off Wolf).
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT)
    {
        return false;
    }
    // From here the line is the HUD's protocol, never chat: taken out even when it is not for this seat (a line that
    // crossed a stand-up), and read only from the object the agent sits on.
    const LLViewerObject* o = gObjectList.findObject(source);
    while (o && o->getParent())
    {
        o = (const LLViewerObject*)o->getParent();
    }
    LLViewerObject* seat = seatRoot();
    if (!o || !seat || o != seat || seat->getID() != mSeatRoot || text.size() > MAX_LINE)
    {
        return true;
    }
    const std::vector<std::string> f = split(text, '|', MAX_FIELDS);
    if (f.size() < 2)
    {
        return true;
    }
    mLastLineAt = now();
    const std::string& kind = f[1];
    if (kind == "hi") parseHi(f);
    else if (kind == "st") parseState(f);
    else if (kind == "bt") parseBattle(f);
    else if (kind == "fired") parseFired(f);
    else if (kind == "booty") parseBooty(f);
    // anything else: a later version's - ignored (DESIGN.md s20.2)
    return true;
}

void WolfHelm::parseHi(const std::vector<std::string>& f)
{
    State& s = mState;
    S32 ver = 0;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "ver") ver = (S32)num(v, 0.f, 1000.f, 0.f);
        else if (k == "ship") s.mShip = cleanText(v).substr(0, 63);
        // a channel the viewer may talk on: not 0 (public chat), not DEBUG_CHANNEL, positive
        else if (k == "ch") s.mChannel = (S32)num(v, 1.f, 2147483000.f, (F32)HELLO_CHANNEL);
        else if (k == "nogo") s.mNoGo = num(v, 0.f, 180.f, 67.5f);
        else if (k == "helmmax") s.mHelmMax = num(v, 1.f, 90.f, 35.f);
        else if (k == "hull") s.mHullKn = num(v, 0.f, 100.f, 0.f);
        else if (k == "naval") s.mNaval = v == "1";
    }
    if (ver != PROTOCOL_VERSION)
    {
        mError = "This ship speaks a newer Captain's Helm than this viewer: update WolfViewer to steer her from the HUD.";
        return;
    }
    s.mHi = true;
    mError.clear();
}

void WolfHelm::parseState(const std::vector<std::string>& f)
{
    if (!mState.mHi) return;
    State& s = mState;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "cap") { LLUUID id; s.mCaptain = id.set(v, false) ? id : LLUUID::null; }
        else if (k == "sail") s.mSail = (S32)num(v, 0.f, 3.f, 0.f);
        else if (k == "anchor") s.mAnchor = v == "1";
        else if (k == "astern") s.mAstern = v == "1";
        else if (k == "kn") s.mKn = num(v, -100.f, 100.f, 0.f);
        else if (k == "hdg") s.mHdg = num(v, 0.f, 360.f, 0.f);
        else if (k == "helm") s.mHelm = num(v, -90.f, 90.f, 0.f);
        else if (k == "heel") s.mHeel = num(v, -90.f, 90.f, 0.f);
        else if (k == "wind") s.mWind = v == "region" ? 1 : v == "fixed" ? 2 : 0;
        else if (k == "light") s.mLight = v == "1";
        else if (k == "tws") s.mTws = num(v, 0.f, 300.f, 0.f);
        else if (k == "eff") s.mEff = num(v, 0.f, 300.f, 0.f);
        else if (k == "twf") s.mTwf = wrap360(num(v, -360.f, 720.f, 0.f));
        else if (k == "twa") s.mTwa = num(v, 0.f, 180.f, 180.f);
        else if (k == "tack") s.mPortTack = v == "port";
        else if (k == "pos") s.mPos = v.substr(0, 8);
        else if (k == "stand") s.mStanding = (S32)num(v, 0.f, 1000.f, 0.f);
        else if (k == "src") s.mSrc = cleanText(v);
        else if (k == "stop") s.mStop = cleanText(v);
        else if (k == "say") s.mSay = cleanText(v);
    }
    s.mHaveState = true;
    if (!mHelmPending) mHelmWant = s.mHelm;
    if (mWindPending)
    {
        // what the captain is dialling stays on the picture until it has been sent
        s.mWind = 2;
        s.mTws = mWindKn;
        s.mTwf = mWindFrom;
    }
}

void WolfHelm::parseBattle(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s20.2 bt, the naval script's 7201 "state|<mode>|<sections>|<port ready s>|<starboard ready s>|
    // <sail factor>|<turn factor>|<struck 0/1>|<protected s>" with "state" replaced by "bt".
    if (!mState.mHi || f.size() < 10) return;
    State& s = mState;
    const F64 t = now();
    const std::string& mode = f[2];
    s.mLockUntil = 0.0;
    if (mode.compare(0, 14, "switching_off:") == 0)
    {
        s.mMode = "switching_off";
        s.mLockUntil = t + num(mode.substr(14), 0.f, 3600.f, 0.f);
    }
    else if (mode.compare(0, 9, "silenced:") == 0)
    {
        // NavalEngine.cs StateWord "silenced:<s>": the guns may not fire until then (a battery knocked out)
        s.mMode = "silenced";
        s.mLockUntil = t + num(mode.substr(9), 0.f, 3600.f, 0.f);
    }
    else if (mode.compare(0, 5, "sunk:") == 0)
    {
        s.mMode = "sunk";   // the naval script sends plain "sunk"; NavalEngine.cs's own word carries the restore time
    }
    else if (mode == "off" || mode == "on" || mode == "sinking" || mode == "sunk")
    {
        s.mMode = mode;
    }
    else
    {
        return;   // not a state this viewer knows: keep the last
    }
    // Health: "<section>=<now>/<max>,..." (naval/src/NavalEngine.cs HealthText: x.Name + "=" + now + "/" + max), by name -
    // a class may have fewer, more or other sections (the shore cannon's "battery"); the HUD draws these seven.
    static const char* const NAMES[SECTION_COUNT] = { "bow", "stern", "port", "starboard", "waterline", "rudder", "rigging" };
    s.mNow.fill(0.f);
    s.mMax.fill(0.f);
    for (const std::string& tok : split(f[3], ',', 32))
    {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = tok.substr(0, eq);
        const std::vector<std::string> nm = split(tok.substr(eq + 1), '/', 2);
        if (nm.size() != 2) continue;
        for (S32 i = 0; i < SECTION_COUNT; ++i)
        {
            if (name != NAMES[i]) continue;
            s.mMax[i] = num(nm[1], 0.f, 1000000.f, 0.f);
            s.mNow[i] = llclamp(num(nm[0], 0.f, 1000000.f, 0.f), 0.f, s.mMax[i]);
        }
    }
    auto ready = [t](F64& at, F32& full, F32 secs_left, F32 left_now)
    {
        if (secs_left > 0.f)
        {
            // a new reload (the side was loaded): its length is the ring's full circle; later updates only lengthen it
            full = left_now <= 0.05f ? secs_left : llmax(full, secs_left);
        }
        at = t + secs_left;
    };
    ready(s.mPortReadyAt, s.mPortFull, num(f[4], 0.f, 3600.f, 0.f), reloadLeft(false));
    ready(s.mStbdReadyAt, s.mStbdFull, num(f[5], 0.f, 3600.f, 0.f), reloadLeft(true));
    s.mSailFactor = num(f[6], 0.f, 10.f, 1.f);
    s.mTurnFactor = num(f[7], 0.f, 10.f, 1.f);
    s.mStruck = f[8] == "1";
    s.mProtectedUntil = t + num(f[9], 0.f, 3600.f, 0.f);
    s.mHaveBattle = true;
}

void WolfHelm::parseFired(const std::vector<std::string>& f)
{
    if (!mState.mHi || f.size() < 3) return;
    // Source: DESIGN.md s20.2 fired - "port" / "starboard", or one gun's prim name. The ship file's gun prims are
    // <deck>_<P|S><n>: gun_P01, gun_S13, fc_P1, qd_S4 (frigate_pallas_ship.json "guns"), P port and S starboard.
    const std::string& w = f[2];
    auto side = [&w](char c) -> bool
    {
        const size_t u = w.find('_');
        return u != std::string::npos && u + 2 < w.size() && w[u + 1] == c && isdigit((unsigned char)w[u + 2]);
    };
    if (w == "port" || side('P')) mState.mPortFiredAt = now();
    else if (w == "starboard" || side('S')) mState.mStbdFiredAt = now();
}

void WolfHelm::parseBooty(const std::vector<std::string>& f)
{
    // Source: lsl/hms_wolfy_helm_template.lsl HudBooty (region-terrain-work/naval-booty-2026-10-30): to this avatar
    // only, "booty|got=<won now>|hoard=<doubloons>|rank=<rank>|from=<ship>"; got 0 = just the hoard (when the HUD said
    // hello). The numbers come from the wolf-naval plugin's ledger (BootyLedger.cs), never from the viewer.
    if (!mState.mHi) return;
    State& s = mState;
    F64 got = 0.0, hoard = -1.0, rank = 0.0;
    std::string from;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        F64 x = 0.0;
        if (k == "got" && parseNumber(v, x)) got = llclamp(x, 0.0, 1.0e6);
        else if (k == "hoard" && parseNumber(v, x)) hoard = llclamp(x, 0.0, 1.0e15);
        else if (k == "rank" && parseNumber(v, x)) rank = llclamp(x, 0.0, 1.0e9);
        else if (k == "from") from = cleanText(v).substr(0, 63);
    }
    if (hoard < 0.0) return;
    s.mHaveBooty = true;
    s.mHoard = (S64)hoard;
    s.mRank = (S32)rank;
    if (got >= 1.0)
    {
        s.mGot = (S32)got;
        s.mGotFrom = from;
        s.mGotAt = now();
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the HUD on / off, one bottom panel at a time
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfHelm::claimBottom()
{
    // Source: wolfgame.cpp WolfGame::claimBottom - the dashboard closes (requestDashboard(false) keeps its menu tick
    // in step and does not reopen it for this seat); the flight / sailing deck is hidden and its mode keeps running.
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
}

void WolfHelm::showHud(bool on)
{
    if (on && !onWolf())
    {
        on = false;   // [WOLF ONLY]
    }
    if (on == mShown)
    {
        return;
    }
    mShown = on;
    if (on)
    {
        claimBottom();
        if (mSeatRoot.notNull() && !mState.mHi && mHelloTries >= HELLO_TRIES)
        {
            mHelloTries = 0;   // opened by hand: ask once more
        }
    }
    else
    {
        mAutoOpened = false;
        mAutoDone = true;   // closed (by hand, or given way): not reopened for this seat
        if (mState.mHi)
        {
            send("hud bye");
            mState.mHi = false;   // a hello again when it is opened again
            mHelloTries = 0;
        }
    }
}

void WolfHelm::toggleHud()
{
    if (!onWolf()) return;
    showHud(!mShown);
}

void WolfHelm::refreshGate()
{
    // Source: wolfdrive.cpp refreshDashGate - off Wolf the toolbar button is gated (kept in the saved layout in its
    // place, Paul 2026-10-20 "DO NOT CHANGE PEOPLES BUTTONS"), every frame so it never shows there even for a moment.
    if (LLStartUp::getStartupState() < STATE_STARTED || !gToolBarView) return;
    static const LLCommandId CMD("wolf_helm");
    if (!onWolf())
    {
        gToolBarView->setCommandGated(CMD, true);
        return;
    }
    if (now() - mLastGate < 1.0 || !gToolBarView->getVisible()) return;
    mLastGate = now();
    gToolBarView->setCommandGated(CMD, false);
}

void WolfHelm::idle()
{
    refreshGate();
    const bool wolf = onWolf();
    if ((S32)wolf != mOnWolf)
    {
        mOnWolf = wolf ? 1 : 0;
        if (!wolf)
        {
            // [WOLF ONLY] left Wolf: the HUD goes, nothing more is said or read (the ship forgets this viewer when
            // the avatar stands or leaves her region).
            mShown = false;
            mAutoOpened = false;
            reset(LLUUID::null);
        }
    }
    if (!wolf)
    {
        return;
    }

    LLViewerObject* root = seatRoot();
    const LLUUID id = root ? root->getID() : LLUUID::null;
    if (id != mSeatRoot)
    {
        // stood up, or sat on something else: a HUD that opened by itself goes; one opened by hand stays (and shows
        // "sit at a ship's helm")
        if (mShown && mAutoOpened)
        {
            mShown = false;
            mAutoOpened = false;
        }
        reset(id);
        if (id.notNull())
        {
            WolfObjectProps::instance().want(root, false);
        }
    }
    if (!root)
    {
        return;
    }
    if (!mDescKnown)
    {
        if (const WolfObjectProps::Props* p = WolfObjectProps::instance().get(id))
        {
            mDescKnown = true;
            std::string d = p->mDescriptionLower;
            LLStringUtil::trimHead(d);
            // Source: wolfvehiclerez.cpp descIs - the word ends there ("wolfvehicle:warship", not "...warships2")
            mShipDesc = d.compare(0, SHIP_DESC.size(), SHIP_DESC) == 0
                     && (d.size() == SHIP_DESC.size() || (!isalnum((unsigned char)d[SHIP_DESC.size()]) && d[SHIP_DESC.size()] != '_'));
        }
        else
        {
            WolfObjectProps::instance().want(root, false);
        }
    }

    const F64 t = now();
    // Hello: to a ship by her description by itself (unless the HUD was closed on this seat), to anything sat on
    // while the HUD is open.
    if (!mState.mHi && ((mShipDesc && !mAutoDone) || mShown) && WolfGrid::isOnWolfTerritoriesConfirmed()
        && mHelloTries < (mShown ? HELLO_TRIES : 1) && t - mHelloAt >= HELLO_RETRY && mError.empty())
    {
        sayHello();
    }
    // Still subscribed? A helm script that was reset forgot its HUDs: hello again after a long silence.
    // (subscribed with the HUD closed too: a passenger who takes the wheel later must still hear "cap")
    if (mState.mHi && t - mLastLineAt > RESUBSCRIBE && t - mHelloAt > RESUBSCRIBE)
    {
        mHelloAt = t;
        send(llformat("hud hello %d", PROTOCOL_VERSION));
    }
    // She answered "hi" but no state followed (lines out of order): ask once more.
    if (mState.mHi && !mState.mHaveState && !mStateAsked && t - mHelloAt > ANSWER_WAIT)
    {
        mStateAsked = true;
        mHelloAt = t;
        send(llformat("hud hello %d", PROTOCOL_VERSION));
    }
    // The captain's HUD opens by itself once per seat (Wolf World > Captain's Helm > the HUD's auto switch).
    if (mState.mHi && isCaptain() && !mShown && !mAutoDone && gSavedSettings.getBOOL("WolfHelmHudAuto"))
    {
        showHud(true);
        mAutoOpened = true;
    }
    // One bottom panel: the dashboard or the deck coming back takes the bottom from the helm.
    if (mShown)
    {
        const WolfFlight& wf = WolfFlight::instance();
        if (WolfDrive::instance().dashboardOn() || (wf.active() && !wf.deckHidden()))
        {
            showHud(false);
        }
    }
    // Wheel dragged: the helm order, at most every HELM_GAP.
    if (mHelmPending && t - mHelmSentAt >= HELM_GAP)
    {
        mHelmPending = false;
        mHelmSentAt = t;
        order(llformat("hud helm %d", (S32)llround(mHelmWant)));
    }
    // Wind steppers: once they rest.
    if (mWindPending && t - mWindChangedAt >= WIND_SETTLE)
    {
        mWindPending = false;
        order(llformat("wind %d %d", (S32)llround(mWindKn), (S32)llround(mWindFrom)));
    }
}
