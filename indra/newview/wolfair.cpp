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
#include "llaudioengine.h"
#include "lldir.h"
#include "llfile.h"
#include "llstartup.h"
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
    const size_t MAX_FIELDS = 48;          // a jet's st is about 30 already (helm + kit fields); room to grow
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

    F32 wrap180deg(F32 a)
    {
        a = fmodf(a + 180.f, 360.f);
        if (a < 0.f) a += 360.f;
        return a - 180.f;
    }

    // Source: wolfhelm.cpp - the word ends there ("wolfvehicle:warplane", not "...warplanes2"). The fleet's roots are
    // described "wolfvehicle:warplane - <title>" (d-planes/generator/deliver_fleet.py:414), the drones' too.
    bool isWarplaneDesc(std::string d)
    {
        LLStringUtil::trimHead(d);
        return d.compare(0, PLANE_DESC.size(), PLANE_DESC) == 0
            && (d.size() == PLANE_DESC.size() || (!isalnum((unsigned char)d[PLANE_DESC.size()]) && d[PLANE_DESC.size()] != '_'));
    }

    // ---- [WOLF AIR JETS 2026-11-02] ----
    // Source: aircraft-dogfight-2026-11-01/d-planes/generator/jet_weapons.py TYPES (title, seeker, range_m, cone_deg).
    const WolfAir::MissileType MISSILES[] = {
        { "aim9",   "AIM-9 SIDEWINDER", 30.f, 16100.f },
        { "aim120", "AIM-120 AMRAAM",   60.f, 32200.f },
        { "asraam", "ASRAAM",           45.f, 25000.f },
    };
    // Source: d-planes/classes/<id>.json "guns": f16c_blk50 2 x aim9 (group missile) + 2 x aim120 (radar); harrier_gr3 2 x
    // aim9; typhoon_fgr4 2 x asraam + 4 x aim120. No other class carries a missile.
    struct ClassStores { const char* mClass; const char* mIr; const char* mRadar; bool mReheat; };
    // mReheat: Source: the same class files, engine.jet thrust_ab_n > thrust_dry_n (f16c_blk50 128,900 > 76,300 N;
    // typhoon_fgr4 180,000 > 120,000 N; every other jet has thrust_ab_n 0).
    const ClassStores STORES[] = {
        { "f16c_blk50",   "aim9",   "aim120", true  },
        { "typhoon_fgr4", "asraam", "aim120", true  },
        { "harrier_gr3",  "aim9",   nullptr,  false },
    };
    // Source: d-planes/classes/harrier_gr3.json engine.jet.thrust_dry_n 82,000 N (the installed WET rating, its _src) and
    // engine.vtol rcs_loss 0.03 (the bleed, full from rcs_full_deg 36, so all of it in the hover), dry_frac 0.92; the helm's
    // JetThrust (wolf_plane_helm_template.lsl:200-203) applies exactly those. g = 9.80665 (AirRules.cs G). At sea level this
    // gives 8,111 kg wet and 7,462 kg dry, the b-flight-model README's "Hover limit 8,111 kg wet / 7,462 kg dry".
    const F32 HARRIER_THRUST_N = 82000.f, HARRIER_RCS_LOSS = 0.03f, HARRIER_DRY_FRAC = 0.92f, STANDARD_G = 9.80665f;

    // Source: wolf_dogfight.lsl Happened: "gWarnUntil = llGetTime() + 6.0" - the kit holds warn=1 this long after a warning.
    const F64 WARN_HOLD = 6.0;
    // Our own two-tone warble (no recording, nothing to licence): four 90 ms pips alternating 1,250 / 950 Hz with 10 ms
    // raised-cosine edges, again every 1.6 s while the warning lasts - plain enough to be heard, quiet enough not to nag.
    const char* WARN_TONE_UUID = "d76d1c11-9449-41ab-8f09-266c13782a60";
    const F64 WARN_TONE_EVERY = 1.6;
    const F32 WARN_TONE_GAIN = 0.55f;
    // Source: AirRules.cs REACH_M - a round is spent after 1,000 m, so the gunner's target cue looks no further.
    const F32 TURRET_REACH_M = 1000.f;
    const F64 SEEKER_EVERY = 0.25;
    const S32 SEEKER_ASKS = 6;          // descriptions asked for per look (WolfObjectProps keeps the shared budget)
    // An order that should change a count (a missile) - the band waits this long for it before saying it did not.
    const F64 LAUNCH_WAIT = 2.5;
    const F64 LAUNCH_SHOW = 6.0;

    void put_u32(std::vector<U8>& out, U32 v) { for (int i = 0; i < 4; ++i) out.push_back((U8)((v >> (8 * i)) & 0xff)); }
    void put_u16(std::vector<U8>& out, U16 v) { out.push_back((U8)(v & 0xff)); out.push_back((U8)((v >> 8) & 0xff)); }

    // The tone as a 16-bit mono WAV where the audio engine looks for a decoded sound. Source: wolfweathersound.cpp
    // soundPath/write_pcm_wav (LLAudioEngine::hasDecodedFile, llaudioengine.cpp:999, builds exactly "<uuid>.dsf" there).
    bool writeWarnTone()
    {
        const U32 rate = 22050;
        std::vector<F32> pcm;
        const F32 freq[4] = { 1250.f, 950.f, 1250.f, 950.f };
        const U32 pip = rate * 90 / 1000, gap = rate * 25 / 1000, edge = rate * 10 / 1000;
        for (S32 k = 0; k < 4; ++k)
        {
            for (U32 i = 0; i < pip; ++i)
            {
                F32 env = 1.f;
                if (i < edge) env = 0.5f - 0.5f * cosf(F_PI * (F32)i / (F32)edge);
                else if (i > pip - edge) env = 0.5f - 0.5f * cosf(F_PI * (F32)(pip - i) / (F32)edge);
                pcm.push_back(0.5f * env * sinf(F_TWO_PI * freq[k] * (F32)i / (F32)rate));
            }
            pcm.insert(pcm.end(), gap, 0.f);
        }
        std::vector<U8> wav;
        const U32 data_bytes = (U32)pcm.size() * 2;
        const char* riff = "RIFF"; const char* wave = "WAVEfmt "; const char* data = "data";
        wav.insert(wav.end(), riff, riff + 4); put_u32(wav, 36 + data_bytes);
        wav.insert(wav.end(), wave, wave + 8); put_u32(wav, 16);
        put_u16(wav, 1); put_u16(wav, 1); put_u32(wav, rate); put_u32(wav, rate * 2); put_u16(wav, 2); put_u16(wav, 16);
        wav.insert(wav.end(), data, data + 4); put_u32(wav, data_bytes);
        for (F32 f : pcm) put_u16(wav, (U16)(S16)llclamp((S32)std::lround(f * 32767.f), -32768, 32767));
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, std::string(WARN_TONE_UUID)) + ".dsf";
        const std::string tmp = path + ".tmp";
        LLFILE* fp = LLFile::fopen(tmp, "wb");
        if (!fp) return false;
        const size_t wrote = fwrite(wav.data(), 1, wav.size(), fp);
        fclose(fp);
        if (wrote != wav.size()) { LLFile::remove(tmp); return false; }
        LLFile::remove(path, ENOENT);
        if (LLFile::rename(tmp, path) != 0) { LLFile::remove(tmp); return false; }
        return true;
    }

    // The seat the line is about: "pilot" or a crew prim's name (DESIGN_BOMBERS.md s1), printable, short.
    std::string seatName(const std::string& v) { return cleanText(v, 64); }   // an OpenSim prim name is at most 63

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
    // [WOLF AIR PREVIEW] at the login screen only, from WolfAirPreviewFile: drawn, never sent (send() needs a Wolf region)
    return (onWolf() && mSeatRoot.notNull() && mState.mHi) || mPreview;
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
    // [WOLF AIR JETS / BOMBERS] a new seat starts clean
    mWeapon = WPN_GUN;
    mTriggerDown = false;
    mSeeker = Seeker();
    mTurretCue = Seeker();
    mLaunchAt = -100.0;
    mWarnWas = false;
    mCrew.clear();
    mMySeat.clear();
    mTurret = Turret();
    mBomb = Bombsight();
    mEng = Engines();
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

bool WolfAir::order(const std::string& words)
{
    if (!onWolf() || !mState.mHi || mSeatRoot.isNull() || !WolfGrid::isOnWolfTerritoriesConfirmed()) return false;
    mLastCommandAt = now();
    send(words);
    return true;
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
    handleLine(text);
    return true;
}

void WolfAir::handleLine(const std::string& text)
{
    const std::vector<std::string> f = split(text, '|', MAX_FIELDS);
    if (f.size() < 2)
    {
        return;
    }
    mLastLineAt = now();
    const std::string& kind = f[1];
    if (kind == "hi") parseHi(f);
    else if (kind == "st") parseState(f);
    else if (kind == "hit") parseHit(f);
    else if (kind == "kill") parseKill(f);
    else if (kind == "down") parseDown(f);
    else if (kind == "respawn") { mState.mRespawnAt = now(); mState.mDownBy.clear(); }
    else if (kind == "warn")
    {
        // Source: wolf_dogfight.lsl Happened: Line("warn|" + text) - the text is everything after "warn|"
        const size_t at = text.find('|', PREFIX.size());
        mState.mWarnText = at == std::string::npos ? std::string() : cleanText(text.substr(at + 1), 120);
        mState.mWarnAt = now();
    }
    else if (kind == "crew") parseCrew(f);          // DESIGN_BOMBERS.md s9
    else if (kind == "tur") parseTurret(f);
    else if (kind == "bmb") parseBomb(f);
    else if (kind == "bomb")
    {
        for (size_t i = 2; i < f.size(); ++i)
        {
            if (f[i].compare(0, 4, "txt=") == 0) mBomb.mLastImpact = cleanText(f[i].substr(4), 120);
        }
        mBomb.mImpactAt = now();
    }
    else if (kind == "eng") parseEngines(f);        // DESIGN_BOMBERS.md s10
    else if (kind == "preview" && mPreview) parsePreview(f);
    // anything else: a later version's - ignored (as WolfHelm)
}

void WolfAir::parseCrew(const std::vector<std::string>& f)
{
    // Source: DESIGN_BOMBERS.md s9 crew|me=<seat>|role=<words>|s=<seat>,<role words>,<avatar name or empty>...
    std::vector<CrewSeat> seats;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "me")
        {
            const std::string me = seatName(v);
            if (me != mMySeat)
            {
                // a new seat (s9 "hud seat"): the old station's turret, sight and engines are not this one's
                mTurret = Turret();
                mTurretCue = Seeker();
                mBomb = Bombsight();
                mEng = Engines();
                if (mFiring) fire(false);
            }
            mMySeat = me;
        }
        else if (k == "s" && seats.size() < 16)
        {
            const std::vector<std::string> p = split(v, ',', 3);
            CrewSeat c;
            c.mSeat = seatName(p[0]);
            c.mRole = p.size() > 1 ? cleanText(p[1], 32) : std::string();
            c.mName = p.size() > 2 ? cleanText(p[2], 64) : std::string();
            if (!c.mSeat.empty()) seats.push_back(c);
        }
    }
    mCrew.swap(seats);
}

void WolfAir::parseTurret(const std::vector<std::string>& f)
{
    // Source: DESIGN_BOMBERS.md s9 tur|seat=|zaz=|az=|el=|azmin=|azmax=|elmin=|elmax=|rate=|ammo=|heat=|fire=|stop=
    Turret& t = mTurret;
    t.mHave = true;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "seat") t.mSeat = seatName(v);
        else if (k == "zaz") t.mZeroAz = num(v, -360.f, 360.f, t.mZeroAz);
        else if (k == "az") t.mAz = num(v, -360.f, 360.f, t.mAz);
        else if (k == "el") t.mEl = num(v, -90.f, 90.f, t.mEl);
        else if (k == "azmin") t.mAzMin = num(v, -360.f, 360.f, t.mAzMin);
        else if (k == "azmax") t.mAzMax = num(v, -360.f, 360.f, t.mAzMax);
        else if (k == "elmin") t.mElMin = num(v, -90.f, 90.f, t.mElMin);
        else if (k == "elmax") t.mElMax = num(v, -90.f, 90.f, t.mElMax);
        else if (k == "rate") t.mRate = num(v, 0.f, 1000.f, t.mRate);
        else if (k == "ammo") t.mAmmo = inum(v, 0, 1000000, t.mAmmo);
        else if (k == "heat") t.mHeat = inum(v, 0, 100, t.mHeat);
        else if (k == "fire") t.mFire = flag(v);
        else if (k == "stop") t.mStop = cleanText(v, 24);
    }
}

void WolfAir::parseBomb(const std::vector<std::string>& f)
{
    // Source: DESIGN_BOMBERS.md s9 bmb|doors=0/1/2|open=|left=|load=|stick=|sp=|ttr=|drift=|steer=L/R/S|tgt=|tr=|safe=
    Bombsight& b = mBomb;
    b.mHave = true;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "doors") b.mDoors = inum(v, 0, 2, b.mDoors);
        else if (k == "open") b.mOpenS = num(v, 0.f, 600.f, b.mOpenS);
        else if (k == "left") b.mLeft = inum(v, 0, 1000, b.mLeft);
        else if (k == "load") b.mLoad = inum(v, 0, 1000, b.mLoad);
        else if (k == "stick") b.mStick = v == "all" ? 0 : inum(v, 0, 1000, b.mStick);   // s9 "hud stick <n|all>"
        else if (k == "sp") b.mSpacingMs = inum(v, 0, 1000, b.mSpacingMs);
        else if (k == "ttr") b.mTtr = num(v, -1.f, 3600.f, -1.f);
        else if (k == "drift") b.mDrift = num(v, -90.f, 90.f, b.mDrift);
        else if (k == "steer") b.mSteer = (v == "L" || v == "R" || v == "S") ? v : std::string();
        else if (k == "tgt") b.mTarget = cleanText(v, 48);
        else if (k == "tr") b.mTargetM = num(v, 0.f, 1.0e6f, b.mTargetM);
        else if (k == "safe") b.mSafe = flag(v);
    }
}

void WolfAir::parseEngines(const std::vector<std::string>& f)
{
    // Source: DESIGN_BOMBERS.md s10 eng|n=<count>|e=<i>,<rpm %>,<run>,<fea>,<pw %>,<fire>,<btl>...|ap=|trim=|asym=
    //   Concorde adds rh=<0/1 each, joined by />|nose=<deg>|cg=<%>
    Engines& e = mEng;
    e.mHave = true;
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "n") e.mCount = inum(v, 0, MAX_ENGINES, e.mCount);
        else if (k == "e")
        {
            const std::vector<std::string> p = split(v, ',', 7);
            const S32 n = p.empty() ? 0 : inum(p[0], 0, MAX_ENGINES, 0);
            if (n < 1 || p.size() < 7) continue;
            Engine& x = e.mE[n - 1];
            x.mRpm = inum(p[1], 0, 200, x.mRpm);
            x.mRun = flag(p[2]);
            x.mFeathered = flag(p[3]);
            x.mPower = inum(p[4], 0, 100, x.mPower);
            x.mFire = flag(p[5]);
            x.mBottle = flag(p[6]);
        }
        else if (k == "ap") e.mAP = flag(v);
        else if (k == "trim") e.mTrim = inum(v, -100, 100, e.mTrim);
        else if (k == "asym") e.mAsym = flag(v);
        else if (k == "rh")
        {
            e.mConcorde = true;
            const std::vector<std::string> p = split(v, '/', MAX_ENGINES);
            for (size_t j = 0; j < p.size(); ++j) e.mE[j].mReheat = flag(p[j]);
        }
        else if (k == "nose") { e.mConcorde = true; e.mNose = num(v, 0.f, 20.f, e.mNose); }
        else if (k == "cg") { e.mConcorde = true; e.mCg = num(v, -100.f, 100.f, e.mCg); }
    }
}

void WolfAir::parsePreview(const std::vector<std::string>& f)
{
    // [WOLF AIR PREVIEW] only from the preview file: what the world would tell the deck (the seeker's and the gunner's
    // target, the flight data the hover cue reads)
    for (size_t i = 2; i < f.size(); ++i)
    {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        if (k == "seeker" || k == "tcue")
        {
            Seeker& sk = k == "seeker" ? mSeeker : mTurretCue;
            const std::vector<std::string> p = split(v, ',', 3);
            sk.mTarget = p.size() == 3;
            if (sk.mTarget)
            {
                sk.mOffDeg = num(p[0], 0.f, 180.f, 0.f);
                sk.mRangeM = num(p[1], 0.f, 1.0e6f, 0.f);
                sk.mName = cleanText(p[2], 48);
            }
        }
        else if (k == "vs") mPreviewVS = num(v, -500.f, 500.f, 0.f);
        else if (k == "gs") mPreviewGS = num(v, 0.f, 2000.f, 0.f);
        else if (k == "agl") mPreviewAGL = num(v, 0.f, 100000.f, 0.f);
        else if (k == "weapon") mWeapon = v == "ir" ? WPN_IR : v == "radar" ? WPN_RADAR : WPN_GUN;
    }
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
        // [WOLF AIR JETS 2026-11-02] helm 1.5.x (wolf_plane_helm_template.lsl HudState) and kit 1.3.0 (HudFields)
        else if (k == "mach") { s.mJet = true; s.mMach = num(v, 0.f, 10.f, s.mMach); }
        else if (k == "rpm") s.mRpm = inum(v, 0, 200, s.mRpm);
        else if (k == "rh") s.mReheat = flag(v);
        else if (k == "ab") s.mAirbrake = flag(v);
        else if (k == "noz") { s.mVtol = true; s.mNozzle = inum(v, 0, 180, s.mNozzle); }
        else if (k == "h2o") s.mWaterS = inum(v, 0, 100000, s.mWaterS);
        else if (k == "wet") s.mWet = flag(v);
        else if (k == "kg") s.mMassKg = inum(v, 0, 10000000, s.mMassKg);
        else if (k == "msl" || k == "cm")
        {
            const std::vector<std::string> p = split(v, '/', 2);
            const S32 a = inum(p[0], 0, 100000, 0), b = p.size() > 1 ? inum(p[1], 0, 100000, 0) : 0;
            if (k == "msl") { s.mIr = a; s.mRadar = b; }
            else { s.mFlares = a; s.mChaff = b; }
        }
        else if (k == "warn") s.mWarn = flag(v);
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
// [WOLF AIR JETS 2026-11-02] stores, the jet's switches, the seeker, the missile warning
// ════════════════════════════════════════════════════════════════════════════════════════

// static
const WolfAir::MissileType* WolfAir::missileOf(const std::string& cls, bool radar)
{
    for (const ClassStores& c : STORES)
    {
        if (cls != c.mClass) continue;
        const char* id = radar ? c.mRadar : c.mIr;
        if (!id) return nullptr;
        for (const MissileType& m : MISSILES)
        {
            if (std::string(m.mId) == id) return &m;
        }
    }
    return nullptr;
}

// static
bool WolfAir::hasReheat(const std::string& cls)
{
    for (const ClassStores& c : STORES)
    {
        if (cls == c.mClass) return c.mReheat;
    }
    return false;
}

// static
F32 WolfAir::hoverLimitKg(const std::string& cls, bool wet)
{
    if (cls != "harrier_gr3") return 0.f;
    // rounded to the kilogram, as the README quotes it (8,110.8 -> 8,111)
    return (F32)std::lround(HARRIER_THRUST_N * (wet ? 1.f : HARRIER_DRY_FRAC) * (1.f - HARRIER_RCS_LOSS) / STANDARD_G);
}

bool WolfAir::weaponCarried(EWeapon w) const
{
    const State& s = mState;
    switch (w)
    {
    case WPN_IR:    return missileOf(s.mClass, false) != nullptr || s.mIr > 0;
    case WPN_RADAR: return missileOf(s.mClass, true) != nullptr || s.mRadar > 0;
    default:        return true;
    }
}

void WolfAir::selectWeapon(EWeapon w)
{
    if (!weaponCarried(w) || isCrew()) return;
    if (mWeapon != w && mFiring) fire(false);     // guns let go when the pilot switches away
    if (mWeapon != w) mTriggerDown = false;       // the next press starts the new weapon
    mWeapon = w;
    mSeeker = Seeker();
    mSeekerAt = 0.0;
}

void WolfAir::trigger(bool down)
{
    if (down == mTriggerDown) return;
    mTriggerDown = down;
    if (mWeapon == WPN_GUN || isCrew())
    {
        fire(down);     // guns (or the gunner's turret, s9 "hud fire on|off") while held
        return;
    }
    if (!down) return;
    // one missile a press. Source: wolf_dogfight.lsl listen: "hud missile" (infra-red) / "hud radar" -> Special()
    const bool radar = mWeapon == WPN_RADAR;
    const S32 had = radar ? mState.mRadar : mState.mIr;
    if (order(radar ? "hud radar" : "hud missile"))
    {
        mLaunchRadar = radar;
        mLaunchCount = had;
        mLaunchAt = now();
    }
}

std::string WolfAir::launchNote(LLColor4& col) const
{
    const F64 since = now() - mLaunchAt;
    if (since > LAUNCH_SHOW) return std::string();
    const S32 have = mLaunchRadar ? mState.mRadar : mState.mIr;
    if (have < mLaunchCount)
    {
        col = LLColor4(0.30f, 0.95f, 0.40f, 1.f);
        // the brevity words the kit itself says (wolf_dogfight.lsl Special: "Fox two" / "Fox three")
        return mLaunchRadar ? "FOX THREE - MISSILE AWAY" : "FOX TWO - MISSILE AWAY";
    }
    if (since < LAUNCH_WAIT) { col = LLColor4(0.85f, 0.85f, 0.85f, 1.f); return "LAUNCHING..."; }
    col = LLColor4(1.f, 0.72f, 0.f, 1.f);
    if (!inDogfight()) return "NO LAUNCH - SWITCH DOGFIGHT ON";
    if (have <= 0) return "NO LAUNCH - NONE LEFT";
    // the plugin's refusal (no lock, the one-a-second rule, too many in the air) is told in nearby chat by the kit
    return "NO LAUNCH - THE REASON IS IN NEARBY CHAT";
}

void WolfAir::decoy(bool flare)
{
    // Source: wolf_dogfight.lsl listen: "hud flare" / "hud chaff" -> Special()
    order(flare ? "hud flare" : "hud chaff");
}

// The helm's own words on her channel (wolf_plane_helm_template.lsl Command, heard from the pilot on CH)
void WolfAir::reheat() { order("reheat"); }
void WolfAir::airbrake() { order("brakes"); }
void WolfAir::nozzles(S32 deg)
{
    // Source: helm Command: "hover" = 90 deg, "nozzles <deg>" clamped to 0..NOZ_MAX (98.5)
    if (deg == 90) order("hover");
    else order(llformat("nozzles %d", llclamp(deg, 0, 98)));
}
void WolfAir::water() { order("water"); }

bool WolfAir::warning() const
{
    return active() && (mState.mWarn || now() - mState.mWarnAt < WARN_HOLD);
}

// [WOLF AIR BOMBERS] DESIGN_BOMBERS.md s9 and s10 orders
void WolfAir::changeSeat(const std::string& seat) { order("hud seat " + seat); }

bool WolfAir::enginePanel() const
{
    if (!mEng.mHave) return false;
    if (mMySeat.empty() || mMySeat == "pilot") return true;
    // the flight engineer: s1 names his seat "seat_engineer"; the role words (s5/s9) say it too
    if (mMySeat == "seat_engineer") return true;
    for (const CrewSeat& c : mCrew)
    {
        if (c.mSeat == mMySeat)
        {
            std::string role = c.mRole;
            LLStringUtil::toLower(role);
            return role.find("engineer") != std::string::npos;
        }
    }
    return false;
}
void WolfAir::bombDoors() { order("hud doors"); }
void WolfAir::bombRelease() { order("hud release"); }
void WolfAir::bombStick(S32 n) { order(n <= 0 ? std::string("hud stick all") : llformat("hud stick %d", n)); }
void WolfAir::engineOrder(S32 i, const char* what)
{
    const std::string w(what);
    if (w == "start" || w == "stop") order(llformat("hud engine %d %s", i, what));
    else order(llformat("hud %s %d", what, i));       // feather / unfeather / extinguish
}
void WolfAir::autopilot(bool on) { order(on ? "hud ap on" : "hud ap off"); }
void WolfAir::rudderTrim(bool on) { order(on ? "hud trim" : "hud trim off"); }
void WolfAir::droop(F32 deg) { order(deg >= 12.f ? "hud droop 12.5" : deg >= 5.f ? "hud droop 5" : "hud droop 0"); }
void WolfAir::concordeReheat(bool on) { order(on ? "hud reheat on" : "hud reheat off"); }
void WolfAir::cgTrim(const char* dir) { order(std::string("hud cgtrim ") + dir); }

bool WolfAir::turretAxis(LLVector3& origin, LLVector3& axis) const
{
    if (!mTurret.mHave) return false;
    const LLViewerObject* root = gObjectList.findObject(mSeatRoot);
    if (!root) return false;
    // Source: DESIGN_BOMBERS.md s2 "zero_az_deg ... in the aircraft frame (+X nose = 0, +Y port = 90)"; az/el in the turret
    // frame relative to it (s3.1). So the bore in her frame is (cos el cos A, cos el sin A, sin el), A = zaz + az.
    const F32 a = (mTurret.mZeroAz + mTurret.mAz) * DEG_TO_RAD, e = mTurret.mEl * DEG_TO_RAD;
    origin = root->getPositionAgent();
    axis = LLVector3(cosf(e) * cosf(a), cosf(e) * sinf(a), sinf(e)) * root->getRotation();
    axis.normalize();
    return true;
}

// The seeker (a missile selected) and the gunner's cue: the warplane the plugin would pick. Source: AirMissiles.cs Launch -
// of the aircraft within the cone and the reach, the one nearest the nose. The viewer cannot see dogfight mode, the era
// rule or the safe airspace (TargetsFor), so this is the seeker's view; the plugin still decides the lock at launch.
// Only moving roots are looked at (getActiveObjects), and only warplanes by their root's description.
void WolfAir::scanSeeker()
{
    if (mPreview) return;           // the preview file says what the seeker sees
    const F64 t = now();
    const bool crew = isCrew();
    const bool look_missile = !crew && mWeapon != WPN_GUN;
    const bool look_turret = crew && mTurret.mHave;
    if (!look_missile) mSeeker = Seeker();
    if (!look_turret) mTurretCue = Seeker();
    if ((!look_missile && !look_turret) || t - mSeekerAt < SEEKER_EVERY) return;
    mSeekerAt = t;
    const LLViewerObject* me = gObjectList.findObject(mSeatRoot);
    if (!me) return;
    const MissileType* mt = look_missile ? missileOf(mState.mClass, mWeapon == WPN_RADAR) : nullptr;
    if (look_missile && !mt) { mSeeker = Seeker(); return; }
    LLVector3 origin, axis;
    if (look_missile) { if (!noseAxis(origin, axis)) return; }
    else origin = me->getPositionAgent();
    const F32 reach = look_missile ? mt->mRangeM : TURRET_REACH_M;
    const F32 cos_cone = look_missile ? cosf(mt->mConeDeg * DEG_TO_RAD) : -1.f;
    const LLQuaternion inv = ~me->getRotation();
    Seeker best;
    F32 best_score = -2.f;
    S32 asks = 0;
    for (const LLPointer<LLViewerObject>& p : gObjectList.getActiveObjects())
    {
        LLViewerObject* o = p.get();
        if (!o || o->isDead() || o->getParent() || o->isAvatar() || o->isAttachment() || o == me) continue;
        const LLVector3 r = o->getPositionAgent() - origin;
        const F32 d = r.length();
        if (d < 1.f || d > reach) continue;
        const LLVector3 u = r * (1.f / d);
        F32 score = 0.f;
        if (look_missile)
        {
            score = u * axis;
            if (score < cos_cone) continue;
        }
        else
        {
            // inside the turret's arcs (s2 az/el relative to zero_az), nearest first
            const LLVector3 l = u * inv;
            const F32 az = wrap180deg(atan2f(l.mV[VY], l.mV[VX]) * RAD_TO_DEG - mTurret.mZeroAz);
            const F32 el = asinf(llclamp(l.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
            if (az < mTurret.mAzMin || az > mTurret.mAzMax || el < mTurret.mElMin || el > mTurret.mElMax) continue;
            score = 1.f - d / reach;
        }
        if (score <= best_score) continue;
        const WolfObjectProps::Props* pr = WolfObjectProps::instance().get(o->getID());
        if (!pr)
        {
            if (asks++ < SEEKER_ASKS) WolfObjectProps::instance().want(o, false);
            continue;
        }
        if (!isWarplaneDesc(pr->mDescriptionLower)) continue;
        best_score = score;
        best.mTarget = true;
        best.mName = cleanText(pr->mName, 48);
        best.mRangeM = d;
        best.mOffDeg = look_missile ? acosf(llclamp(score, -1.f, 1.f)) * RAD_TO_DEG : 0.f;
        best.mPosAgent = o->getPositionAgent();
    }
    if (look_missile) mSeeker = best; else mTurretCue = best;
}

void WolfAir::warnTone()
{
    const bool live = warning();
    const F64 t = now();
    const bool rising = live && !mWarnWas;
    mWarnWas = live;
    static LLCachedControl<bool> tone_on(gSavedSettings, "WolfAirWarnTone", true);
    if (rising)
    {
        // once a warning: what the pilot was shown, and whether the tone could play (a flea report says which)
        LL_INFOS("WolfAir") << "missile warning: \"" << mState.mWarnText << "\" tone " << (tone_on ? "on" : "off (WolfAirWarnTone)")
                            << (gAudiop ? "" : ", no audio engine") << LL_ENDL;
    }
    if (!live || !tone_on || !gAudiop) return;
    if (!rising && t - mToneAt < WARN_TONE_EVERY) return;
    if (mToneState == 0)
    {
        mToneState = writeWarnTone() ? 1 : -1;
        if (mToneState < 0) LL_WARNS("WolfAir") << "the missile-warning tone could not be written to the sound cache" << LL_ENDL;
    }
    if (mToneState > 0 && !LLFile::isfile(gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, std::string(WARN_TONE_UUID)) + ".dsf"))
    {
        mToneState = writeWarnTone() ? 1 : -1;     // the sound cache was cleared under us: write it again
    }
    if (mToneState < 0) return;     // the band's MISSILE cue still shows
    mToneAt = t;
    // a UI sound: the UI volume slider governs it (llappviewer.cpp ui_audio_callback uses the same type)
    SoundData sd(LLUUID(WARN_TONE_UUID), gAgentID, WARN_TONE_GAIN, LLAudioEngine::AUDIO_TYPE_UI);
    gAudiop->triggerSound(sd);
}

// ════════════════════════════════════════════════════════════════════════════════════════
// [WOLF AIR PREVIEW] recorded lines at the login screen (WolfAirPreviewFile), for testing the deck off the grid
// ════════════════════════════════════════════════════════════════════════════════════════

bool WolfAir::previewWanted()
{
    // only before a login starts: once in-world nothing here can run
    return LLStartUp::getStartupState() <= STATE_LOGIN_WAIT;
}

void WolfAir::previewTick()
{
    if (mPreviewChecked == 0)
    {
        mPreviewChecked = 1;
        const std::string path = gSavedSettings.getString("WolfAirPreviewFile");
        if (!path.empty() && previewWanted())
        {
            llifstream in(path.c_str());
            std::string line;
            while (in.good() && std::getline(in, line) && mPreviewLines.size() < 2000)
            {
                // "<seconds> WolfAir1|..." ; '#' starts a comment
                if (line.empty() || line[0] == '#') continue;
                const size_t sp = line.find(' ');
                F64 at = 0.0;
                if (sp == std::string::npos || !parseNumber(line.substr(0, sp), at)) continue;
                std::string text = line.substr(sp + 1);
                LLStringUtil::trim(text);
                if (text.compare(0, PREFIX.size(), PREFIX) == 0) mPreviewLines.emplace_back(at, text);
            }
            mPreview = !mPreviewLines.empty();
            LL_INFOS("WolfAir") << "preview file " << path << ": " << mPreviewLines.size() << " lines" << LL_ENDL;
            mPreviewStart = now();
        }
    }
    if (!mPreview) return;
    if (!previewWanted())
    {
        // a login started: the preview ends and leaves nothing behind
        mPreview = false;
        mPreviewLines.clear();
        reset(LLUUID::null);
        mKills.clear();
        return;
    }
    const F64 t = now() - mPreviewStart;
    while (mPreviewNext < mPreviewLines.size() && mPreviewLines[mPreviewNext].first <= t)
    {
        handleLine(mPreviewLines[mPreviewNext].second);
        ++mPreviewNext;
    }
    while (!mKills.empty() && now() - mKills.back().mAt > KILL_SHOW) mKills.pop_back();
    warnTone();
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
    // [WOLF AIR JETS] the selected weapon: guns while held, one missile a press (trigger)
    if (down && !mKeyFiring && !mTriggerDown)
    {
        mKeyFiring = true;
        trigger(true);
    }
    else if (!down && mKeyFiring)
    {
        mKeyFiring = false;
        trigger(false);
    }
}

void WolfAir::idle()
{
    // [WOLF AIR PREVIEW] in-world now: a preview left over from the login screen ends here too
    if (mPreview) previewTick();
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
            mPlaneDesc = isWarplaneDesc(p->mDescriptionLower);
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
        scanSeeker();
        warnTone();
        pollFireKey();
        if (mFiring && t - mFireSentAt >= FIRE_REPEAT)
        {
            mFireSentAt = t;
            send("hud fire on");
        }
    }
}
