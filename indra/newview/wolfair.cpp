/**
 * @file wolfair.cpp
 * @brief WolfViewer: Wolf Air - the WolfAir1 link to a warplane, and the "WolfAir" kill feed. See wolfair.h and
 *        region-terrain-work/aircraft-dogfight-2026-11-01/a-research/DESIGN.md s2 and s3.8 (the protocol, both sides).
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

#include "wolfair.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llchat.h"
#include "lldispatcher.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

namespace
{
    // Source: DESIGN.md s2 "llRegionSayTo(pilot, 0, "WolfAir1|<kind>|k=v|...")".
    const std::string PREFIX("WolfAir1|");
    // Source: DESIGN.md s2 "hud hello 1".
    const S32 PROTOCOL_VERSION = 1;
    // Source: DESIGN.md s2 "The root's description starts with wolfvehicle:warplane, so the viewer says hello by itself".
    const std::string PLANE_DESC("wolfvehicle:warplane");
    // Source: DESIGN.md s2 "shouted on channel 7 until the plane's hi names its own channel".
    const S32 HELLO_CHANNEL = 7;
    // Source: DESIGN.md s3.8 - the region's GenericMessage method.
    const std::string MESSAGE_FROM_SIM("WolfAir");
    // As wolfhelm.cpp: a hello unanswered this long is asked again, at most this many times per seat.
    const F64 HELLO_RETRY = 4.0;
    const S32 HELLO_TRIES = 3;
    // As wolfhelm.cpp RESUBSCRIBE: a long silence (a helm script reset forgets its HUDs) says hello again. A plane's "st"
    // goes at most 4 a second while she flies (DESIGN.md s2), so silence is shorter than a ship's.
    const F64 RESUBSCRIBE = 30.0;
    const F64 ANSWER_WAIT = 3.0;
    const F64 ANSWER_SHOW = 10.0;
    // While FIRE is held the order is said again every second, so a plane that missed the first line (or a script that
    // stops firing when it hears nothing, as a dead-man's grip) keeps firing only while the pilot really holds it.
    const F64 FIRE_REPEAT = 1.0;
    // Source: llRegionSayTo truncates at 1023 characters (LSL_Api.cs llRegionSayTo msg[..1023]); as wolfhelm.cpp.
    const size_t MAX_LINE = 1023;
    const size_t MAX_FIELDS = 32;
    // The kill feed: the last few, each shown this long (game convention: a short-lived feed in a corner).
    const size_t MAX_KILLS = 6;
    const F64 KILL_SHOW = 20.0;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // Source: wolfhelm.cpp seatRoot (itself wolfdrive.cpp's) - the root of what the agent sits on, or null.
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

    // Source: wolfhelm.cpp parseNumber - plain decimals only, never trust the wire.
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

    S32 inum(const std::string& s, S32 lo, S32 hi, S32 fallback)
    {
        F64 v = 0.0;
        // rounded in double: a channel such as -1234567890 does not survive a float (24-bit mantissa)
        return parseNumber(s, v) ? (S32)std::llround(llclamp(v, (F64)lo, (F64)hi)) : fallback;
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

    // Source: wolfhelm.cpp cleanText - printable, at most n characters, never cut inside a UTF-8 character.
    std::string cleanText(const std::string& s, size_t max_chars = 200)
    {
        size_t n = llmin(s.size(), max_chars);
        while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
        std::string out = s.substr(0, n);
        for (char& c : out)
        {
            if ((unsigned char)c < 0x20) c = ' ';
        }
        return out;
    }

    bool flag(const std::string& v) { return v == "1"; }

    // Source: wolffx.cpp WolfFXHandler - the region's GenericMessage, only on a Wolf region that said so itself.
    class WolfAirHandler : public LLDispatchHandler
    {
    public:
        bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
        {
            if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return true;
            WolfAir::instance().receive(invoice, strings);
            return true;
        }
    };
    WolfAirHandler sHandler;
}

WolfAir::WolfAir()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sHandler);
    }
}

WolfAir::~WolfAir()
{
}

// static
bool WolfAir::onWolf()
{
    return WolfGrid::isOnWolfTerritories();
}

// static
S32 WolfAir::fireKey()
{
    static LLCachedControl<S32> key(gSavedSettings, "WolfAirFireKey", 0);
    return llclamp((S32)key, 0, 2);
}

// static
const char* WolfAir::fireKeyName(S32 key)
{
    switch (key)
    {
    case 1: return "SPACE";
    case 2: return "INSERT";
    default: return "NONE";
    }
}

// static
const char* WolfAir::partName(S32 part)
{
    // Source: DESIGN.md s3.4 the eight parts
    // short, for the COMBAT display's narrow column
    static const char* NAMES[PART_COUNT] = { "ENG", "FUEL", "CKPT", "FUS", "WING L", "WING R", "TAIL", "FIN" };
    return part >= 0 && part < PART_COUNT ? NAMES[part] : "";
}

bool WolfAir::active() const
{
    return onWolf() && mSeatRoot.notNull() && mState.mHi;
}

bool WolfAir::inDogfight() const
{
    return active() && !mState.mDogfight.empty() && mState.mDogfight != "off";
}

bool WolfAir::isDown() const
{
    return active() && (mState.mDogfight == "down" || mState.mDogfight.compare(0, 6, "wreck:") == 0);
}

bool WolfAir::hailing() const
{
    return mSeatRoot.notNull() && !mState.mHi && mError.empty()
        && (mHelloTries < HELLO_TRIES || now() - mHelloAt < HELLO_RETRY);
}

bool WolfAir::noAnswer() const
{
    const F64 since = now() - mLastCommandAt;
    return mLastCommandAt > 0.0 && mLastLineAt < mLastCommandAt && since > ANSWER_WAIT && since < ANSWER_SHOW;
}

bool WolfAir::noseAxis(LLVector3& origin, LLVector3& axis) const
{
    const LLViewerObject* root = gObjectList.findObject(mSeatRoot);
    if (!root)
    {
        return false;
    }
    // Source: DESIGN.md s14 "+X is the nose" (the class file's plane frame is the root's frame); a kit plane whose nose is
    // another axis tells the plugin its axis, not the viewer, so its gunsight follows the root's +X (the docs say so).
    origin = root->getPositionAgent();
    axis = LLVector3::x_axis * root->getRotation();
    axis.normalize();
    return true;
}

void WolfAir::reset(const LLUUID& seat)
{
    if (mFiring && mState.mHi)
    {
        send("hud fire off");
    }
    mSeatRoot = seat;
    mState = State();
    mPlaneDesc = false;
    mDescKnown = false;
    mHelloTries = 0;
    mHelloAt = 0.0;
    mLastLineAt = 0.0;
    mLastCommandAt = 0.0;
    mStateAsked = false;
    mDeckOpened = false;
    mFiring = false;
    mKeyFiring = false;
    mError.clear();
}

void WolfAir::send(const std::string& text)
{
    // As WolfHelm::send: to the region only once it has itself said it is Wolf, never on a foreign grid.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || mSeatRoot.isNull())
    {
        return;
    }
    const S32 ch = mState.mHi ? mState.mChannel : HELLO_CHANNEL;
    // Shouted, as the ship's: a plane's helm is in the root, and a big aircraft's root can be over 20 m from her cockpit.
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_SHOUT, false, ch);
}

void WolfAir::sayHello()
{
    mHelloAt = now();
    ++mHelloTries;
    send(llformat("hud hello %d", PROTOCOL_VERSION));
}

void WolfAir::order(const std::string& words)
{
    if (!onWolf() || !mState.mHi) return;
    mLastCommandAt = now();
    send(words);
}

void WolfAir::fire(bool on)
{
    if (!active())
    {
        mFiring = false;
        return;
    }
    if (on == mFiring)
    {
        return;
    }
    mFiring = on;
    mFireSentAt = now();
    order(on ? "hud fire on" : "hud fire off");
}

void WolfAir::dogfight(bool on)
{
    order(on ? "hud dogfight on" : "hud dogfight off");
}

void WolfAir::bail()
{
    order("hud bail");
}

// ════════════════════════════════════════════════════════════════════════════════════════
// from the plane
// ════════════════════════════════════════════════════════════════════════════════════════

bool WolfAir::onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0)
    {
        return false;
    }
    // [WOLF ONLY] as WolfHelm::onObjectChat: off Wolf the line is left to chat like any other.
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT)
    {
        return false;
    }
    // From here the line is the band's, never chat: read only from the object the agent sits on.
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
    else if (kind == "hit") parseHit(f);
    else if (kind == "kill") parseKill(f);
    else if (kind == "down") parseDown(f);
    else if (kind == "respawn") { mState.mRespawnAt = now(); mState.mDownBy.clear(); }
    // anything else: a later version's - ignored (as WolfHelm)
    return true;
}

void WolfAir::parseHi(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s2 hi|ver=1|plane=<title>|class=<id>|ch=<n>|vs=<stall m/s>|vne=<m/s>|gmax=<n>|guns=<n>|cannon=<n>|wolfair=<0/1>
    State& s = mState;
    S32 ver = 0;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "ver") ver = inum(v, 0, 1000, 0);
        else if (k == "plane") s.mPlane = cleanText(v, 64);
        else if (k == "class") s.mClass = cleanText(v, 48);
        else if (k == "ch") s.mChannel = inum(v, -2147483647, 2147483647, HELLO_CHANNEL);
        else if (k == "vs") s.mStall = num(v, 0.f, 500.f, 0.f);
        else if (k == "vne") s.mVne = num(v, 0.f, 2000.f, 0.f);
        else if (k == "gmax") s.mGmax = num(v, 0.f, 20.f, 0.f);
        else if (k == "guns") s.mGuns = inum(v, 0, 64, 0);
        else if (k == "cannon") s.mCannon = inum(v, 0, 64, 0);
        else if (k == "wolfair") s.mWolfAir = flag(v);
    }
    if (ver < PROTOCOL_VERSION)
    {
        mError = "This aircraft speaks an older dogfight protocol.";
        return;
    }
    if (s.mChannel == 0)
    {
        s.mChannel = HELLO_CHANNEL;   // never shout the HUD's orders into open chat
    }
    s.mHi = true;
    mError.clear();
}

void WolfAir::parseState(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s2 st|thr=|eng=|gear=|flaps=|g=|aoa=|stall=|fuel=|ammo=<guns>/<cannon>|heat=<guns %>/<cannon %>|
    //   dmg=<engine>,<fuel>,<cockpit>,<fuselage>,<wing_left>,<wing_right>,<tail>,<fin>|fire=|smoke=|df=<state word>|safe=|say=
    State& s = mState;
    s.mHaveState = true;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "thr") s.mThrottle = inum(v, 0, 100, s.mThrottle);
        else if (k == "eng") s.mEngine = flag(v);
        else if (k == "gear") s.mGear = flag(v);
        else if (k == "flaps") s.mFlaps = flag(v);
        else if (k == "g") s.mG = num(v, -20.f, 20.f, s.mG);
        else if (k == "aoa") s.mAoa = num(v, -90.f, 90.f, s.mAoa);
        else if (k == "stall") s.mStalled = flag(v);
        else if (k == "fuel") s.mFuel = inum(v, 0, 100, s.mFuel);
        else if (k == "ammo" || k == "heat")
        {
            const std::vector<std::string> p = split(v, '/', 2);
            const S32 a = inum(p[0], 0, 1000000, 0), b = p.size() > 1 ? inum(p[1], 0, 1000000, 0) : 0;
            if (k == "ammo") { s.mAmmoGuns = a; s.mAmmoCannon = b; }
            else { s.mHeatGuns = llclamp(a, 0, 100); s.mHeatCannon = llclamp(b, 0, 100); }
        }
        else if (k == "dmg")
        {
            const std::vector<std::string> p = split(v, ',', PART_COUNT);
            for (size_t j = 0; j < p.size() && j < (size_t)PART_COUNT; ++j)
            {
                s.mDmg[j] = inum(p[j], 0, 100, s.mDmg[j]);
            }
        }
        else if (k == "fire") s.mFire = flag(v);
        else if (k == "smoke") s.mSmoke = flag(v);
        else if (k == "df") s.mDogfight = cleanText(v, 32);
        else if (k == "safe") s.mSafe = flag(v);
        else if (k == "say") s.mSay = cleanText(v);
    }
}

void WolfAir::parseHit(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s2 hit|by=<name>|part=<section>|dmg=<n>
    State& s = mState;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "by") s.mHitBy = cleanText(v, 64);
        else if (k == "part") s.mHitPart = cleanText(v, 24);
        else if (k == "dmg") s.mHitDmg = inum(v, 0, 100000, 0);
    }
    s.mHitAt = now();
}

void WolfAir::parseKill(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s2 kill|killer=<name>|victim=<name>|how=<text>
    std::string killer, victim, how;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "killer") killer = v;
        else if (k == "victim") victim = v;
        else if (k == "how") how = v;
    }
    addKill(killer, victim, how);
}

void WolfAir::parseDown(const std::vector<std::string>& f)
{
    // Source: DESIGN.md s2 down|by=<name>
    for (size_t i = 2; i < f.size(); ++i)
    {
        if (f[i].compare(0, 3, "by=") == 0) mState.mDownBy = cleanText(f[i].substr(3), 64);
    }
    mState.mDownAt = now();
}

void WolfAir::addKill(const std::string& killer, const std::string& victim, const std::string& how)
{
    Kill k;
    k.mKiller = cleanText(killer, 48);
    k.mVictim = cleanText(victim, 48);
    k.mHow = cleanText(how, 48);
    k.mAt = now();
    if (k.mVictim.empty())
    {
        return;
    }
    // The same kill can arrive twice (the plane's line and the region's message): once in the feed.
    for (const Kill& o : mKills)
    {
        if (o.mKiller == k.mKiller && o.mVictim == k.mVictim && k.mAt - o.mAt < 5.0)
        {
            return;
        }
    }
    mKills.push_front(k);
    while (mKills.size() > MAX_KILLS)
    {
        mKills.pop_back();
    }
}

void WolfAir::receive(const LLUUID& region_id, const std::vector<std::string>& strings)
{
    // Source: DESIGN.md s3.8 "e=kill|k=<killer>|v=<victim>|w=<weapon>" - one parameter per key=value, as WolfFX's wire.
    if (!onWolf()) return;
    std::string e, killer, victim, weapon;
    LLUUID owner;
    for (const std::string& kv : strings)
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "e") e = v;
        else if (k == "k") killer = v;
        else if (k == "v") victim = v;
        else if (k == "w") weapon = v;
    }
    if (e == "kill")
    {
        addKill(killer, victim, weapon);
    }
    // another kind: a later plugin's - ignored
}

// ════════════════════════════════════════════════════════════════════════════════════════
// every frame
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfAir::pollFireKey()
{
    const S32 key = fireKey();
    bool down = false;
    // Only while nothing that takes typing has the keyboard (chat bar, a floater's field): a Space typed into chat
    // must never fire. The flight deck itself takes focus only while the CDU is used.
    if (key != 0 && gKeyboard && gFocusMgr.getKeyboardFocus() == nullptr)
    {
        down = gKeyboard->getKeyDown(key == 1 ? ' ' : KEY_INSERT);
    }
    if (down && !mFiring)
    {
        mKeyFiring = true;
        fire(true);
    }
    else if (!down && mKeyFiring)
    {
        mKeyFiring = false;
        fire(false);
    }
}

void WolfAir::idle()
{
    const bool wolf = onWolf();
    if ((S32)wolf != mOnWolf)
    {
        mOnWolf = wolf ? 1 : 0;
        if (!wolf)
        {
            // [WOLF ONLY] left Wolf: nothing more is said or read, the feed goes.
            mKills.clear();
            reset(LLUUID::null);
        }
    }
    if (!wolf)
    {
        return;
    }
    const F64 t = now();
    while (!mKills.empty() && t - mKills.back().mAt > KILL_SHOW)
    {
        mKills.pop_back();
    }

    LLViewerObject* root = seatRoot();
    const LLUUID id = root ? root->getID() : LLUUID::null;
    if (id != mSeatRoot)
    {
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
            // Source: wolfhelm.cpp - the word ends there ("wolfvehicle:warplane", not "...warplanes2")
            mPlaneDesc = d.compare(0, PLANE_DESC.size(), PLANE_DESC) == 0
                      && (d.size() == PLANE_DESC.size() || (!isalnum((unsigned char)d[PLANE_DESC.size()]) && d[PLANE_DESC.size()] != '_'));
        }
        else
        {
            WolfObjectProps::instance().want(root, false);
        }
    }
    // Hello: to a warplane by her description by itself, at most HELLO_TRIES times per seat.
    if (!mState.mHi && mPlaneDesc && WolfGrid::isOnWolfTerritoriesConfirmed()
        && mHelloTries < HELLO_TRIES && t - mHelloAt >= HELLO_RETRY && mError.empty())
    {
        sayHello();
    }
    if (mState.mHi && t - mLastLineAt > RESUBSCRIBE && t - mHelloAt > RESUBSCRIBE)
    {
        mHelloAt = t;
        send(llformat("hud hello %d", PROTOCOL_VERSION));
    }
    if (mState.mHi && !mState.mHaveState && !mStateAsked && t - mHelloAt > ANSWER_WAIT)
    {
        mStateAsked = true;
        mHelloAt = t;
        send(llformat("hud hello %d", PROTOCOL_VERSION));
    }
    // The band lives in the flight deck: Flight Mode comes on by itself once per seat (WolfAirAutoDeck), as the captain's
    // helm HUD opens itself (WolfHelmHudAuto).
    if (mState.mHi && !mDeckOpened)
    {
        mDeckOpened = true;
        static LLCachedControl<bool> auto_deck(gSavedSettings, "WolfAirAutoDeck", true);
        WolfFlight& wf = WolfFlight::instance();
        if (auto_deck && !wf.active())
        {
            wf.requestMode(false, true);
        }
    }
    if (mState.mHi)
    {
        pollFireKey();
        if (mFiring && t - mFireSentAt >= FIRE_REPEAT)
        {
            mFireSentAt = t;
            send("hud fire on");
        }
    }
}
