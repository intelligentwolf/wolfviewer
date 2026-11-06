/**
 * @file wolfskis.cpp
 * @brief WolfViewer: skis and snowboards. See wolfskis.h.
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

#include "wolfskis.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llchat.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfobjectprops.h"
#include "wolfterrainlevels.h"
#include "wolfvehiclerez.h"

namespace
{
    // Source: region-terrain-work/skis-2026-11-06/lsl/ski_template.lsl hud() "WolfSki1|" and CMD_CH (build_skis.py CMD_CH 7,
    // the Wolf vehicles' channel); the hello is the script's listen() "ski hello 1".
    const std::string PREFIX("WolfSki1|");
    const S32 CHANNEL = 7;
    const char* const HELLO = "ski hello 1";
    const size_t MAX_LINE = 255;            // the script's longest line is "hi|snowboard|<title>|1"
    const size_t MAX_FIELDS = 8;
    const S32 HELLO_TRIES = 5;
    const F64 HELLO_RETRY = 3.0;            // s between hellos until the skis answer (the script answers at once)
    const std::string DESC_PREFIX("wolfvehicle:");

    // The run (MODEL choices, so the clock does what a rider expects).
    const F32 START_SPEED = 1.0f;           // m/s: faster than this from a standstill = pushed off, the clock starts
    const F32 STILL_SPEED = 0.3f;           // m/s: slower than this ...
    const F64 STILL_SECS = 2.0;             // ... for this long = stopped, the run is over
    const F32 MIN_RUN_SECS = 3.f;           // a shorter "run" is a shuffle, not counted
    const F64 POPUP_SECS = 3.0;
    const size_t MAX_POPUPS = 4;
    // Style points (MODEL, our own game - nothing here is a real competition's scoring)
    const F32 AIR_POINTS_PER_S = 100.f;     // a clean landing: 100 a second in the air
    const F32 SHOW_AIR = 0.6f;              // s: shorter hops are not called out
    const F32 BIG_AIR = 1.5f;               // s: "Big air!"
    const F64 CARVE_MIN = 0.6;              // s held for a carved turn to count
    const F32 CARVE_SPEED = 8.f;            // m/s
    const F64 COMBO_GAP = 4.0;              // s: turns further apart than this start the combo again
    const S32 CARVE_POINTS = 25;            // times the combo count
    const S32 MAX_COMBO = 10;
    const F32 FAST_SPEED = 20.f;            // m/s (72 km/h): every second faster than this ...
    const S32 FAST_POINTS = 5;              // ... is worth this
    const size_t MAX_BEST_REGIONS = 60;     // the bests kept on this computer (the oldest goes first)

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    LLViewerObject* seatRoot()
    {
        // Source: wolfhelm.cpp seatRoot()
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

    // Source: wolfhelm.cpp split()
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

    // Source: wolfhelm.cpp parseNumber / num - plain decimals only, never trust the wire.
    F32 num(const std::string& s, F32 lo, F32 hi, F32 fallback)
    {
        if (s.empty() || s.size() > 24) return fallback;
        for (char c : s)
        {
            if (!(isdigit((unsigned char)c) || c == '.' || c == '-' || c == '+')) return fallback;
        }
        errno = 0;
        char* end = nullptr;
        const double v = strtod(s.c_str(), &end);
        if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return fallback;
        return (F32)llclamp(v, (double)lo, (double)hi);
    }

    // "wolfvehicle:<type>" at the start, the word ending there (Source: wolfvehiclerez.cpp descIs).
    bool descIs(const std::string& lower, const std::string& type)
    {
        std::string d = lower;
        LLStringUtil::trimHead(d);
        const std::string want = DESC_PREFIX + type;
        if (d.compare(0, want.size(), want) != 0) return false;
        return d.size() == want.size() || (!isalnum((unsigned char)d[want.size()]) && d[want.size()] != '_');
    }

    std::string clockText(F32 secs)
    {
        const S32 tenths = (S32)llround(secs * 10.f);
        return llformat("%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
    }

    // The ground (or sky land) height at a global point: the highest surface at or below the point's own height, so
    // standing on sky land the level counts and the ground far below does not (wolfterrainlevels.h resolveSupportHeight:
    // "The ground counts everywhere; a level only where it has ground").
    F32 supportHeight(LLViewerRegion* region, const LLVector3d& p)
    {
        if (region)
        {
            const LLVector3 r = region->getPosRegionFromGlobal(p);
            const F32 w = region->getWidth();
            if (r.mV[VX] >= 0.f && r.mV[VY] >= 0.f && r.mV[VX] < w && r.mV[VY] < w)
            {
                if (WolfTerrainLevels* sky = region->getWolfSkyLevels(); sky && sky->hasLevels())
                {
                    return sky->resolveSupportHeight(r.mV[VX], r.mV[VY], r.mV[VZ], 1.f)
                        + (F32)region->getOriginGlobal().mdV[VZ];
                }
            }
        }
        return LLWorld::getInstance()->resolveLandHeightGlobal(p);
    }
}

WolfSkis::WolfSkis() {}
WolfSkis::~WolfSkis() {}

// static
bool WolfSkis::onWolf()
{
    return WolfGrid::isOnWolfTerritories();
}

// static
bool WolfSkis::seatIsSkis(const LLUUID& root)
{
    const WolfObjectProps::Props* p = root.notNull() ? WolfObjectProps::instance().get(root) : nullptr;
    return p && (descIs(p->mDescriptionLower, "skis") || descIs(p->mDescriptionLower, "snowboard"));
}

// static
const char* WolfSkis::kindWord(EKind k)
{
    return k == KIND_SNOWBOARD ? "snowboard" : "skis";
}

bool WolfSkis::mph() const
{
    return gSavedSettings.getBOOL("WolfSkiHudMph");
}

void WolfSkis::setMph(bool on)
{
    gSavedSettings.setBOOL("WolfSkiHudMph", on);
}

void WolfSkis::say(const std::string& text)
{
    // Only to a region that has said it is Wolf (wolfgrid.h isOnWolfTerritoriesConfirmed), as WolfHelm::send.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || mSeatRoot.isNull()) return;
    // Said, not shouted: the script is in the very object the rider sits on (20 m reaches it).
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_NORMAL, false, CHANNEL);
}

void WolfSkis::reset(const LLUUID& seat)
{
    mSeatRoot = seat;
    mKind = KIND_NONE;
    mDescKnown = false;
    mAnswered = false;
    mHelloTries = 0;
    mHelloAt = -100.0;
    mShown = false;
    mClosedByHand = false;
    mShownData = Shown();
    mPopups.clear();
    mLastTick = 0.0;
    mStillSince = 0.0;
    mAirStart = 0.0;
    mHavePos = false;
    mLastCarve.clear();
    mCarveSince = 0.0;
    mLastTurnAt = 0.0;
    mFastSince = 0.0;
}

void WolfSkis::putOn(EKind k)
{
    if (!onWolf() || k == KIND_NONE) return;
    gSavedSettings.setString("WolfSkiKind", kindWord(k));
    if (mKind == k) return;   // already on them
    WolfVehicleRez::rez(kindWord(k));
}

void WolfSkis::takeOff()
{
    if (mKind == KIND_NONE) return;
    gAgent.standUp();
}

void WolfSkis::claimBottom()
{
    // Source: wolfhelm.cpp WolfHelm::claimBottom - one bottom panel at a time (memory paul-game-ui-like-flightdeck-mmo):
    // the dashboard closes (requestDashboard(false) does not reopen it for this seat), the deck is hidden (its mode
    // keeps running), and the Captain's Helm gives way.
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
    if (WolfHelm::instance().hudShowing()) WolfHelm::instance().showHud(false);
}

void WolfSkis::showHud(bool on)
{
    if (on && (!onWolf() || mKind == KIND_NONE)) on = false;
    mClosedByHand = !on;
    if (on == mShown) return;
    mShown = on;
    if (on) claimBottom();
}

const std::vector<WolfSkis::Popup>& WolfSkis::popups()
{
    const F64 t = now();
    while (!mPopups.empty() && t - mPopups.front().mAt > POPUP_SECS)
    {
        mPopups.erase(mPopups.begin());
    }
    return mPopups;
}

void WolfSkis::popup(const std::string& text, const std::string& sub, S32 tone)
{
    if (mPopups.size() >= MAX_POPUPS) mPopups.erase(mPopups.begin());
    Popup p;
    p.mText = text;
    p.mSub = sub;
    p.mTone = tone;
    p.mAt = now();
    mPopups.push_back(p);
}

void WolfSkis::addScore(S32 points)
{
    if (points <= 0 || !mShownData.mRunning) return;
    mShownData.mScore = llmin(mShownData.mScore + points, 9999999);
}

void WolfSkis::loadBests()
{
    Shown& s = mShownData;
    s.mBestScore = 0;
    s.mBestTop = 0.f;
    s.mBestAir = 0.f;
    const LLSD all = gSavedSettings.getLLSD("WolfSkiBests");
    if (!all.isMap() || mRegionName.empty() || !all.has(mRegionName)) return;
    const LLSD& b = all[mRegionName];
    if (!b.isMap()) return;
    // never trust what is on disk either: clamped to what a run can make
    s.mBestScore = llclamp((S32)b["score"].asInteger(), 0, 9999999);
    s.mBestTop = llclamp((F32)b["top"].asReal(), 0.f, 100.f);
    s.mBestAir = llclamp((F32)b["air"].asReal(), 0.f, 60.f);
}

void WolfSkis::saveBests()
{
    if (mRegionName.empty()) return;
    LLSD all = gSavedSettings.getLLSD("WolfSkiBests");
    if (!all.isMap()) all = LLSD::emptyMap();
    LLSD b = LLSD::emptyMap();
    b["score"] = mShownData.mBestScore;
    b["top"] = mShownData.mBestTop;
    b["air"] = mShownData.mBestAir;
    b["at"] = (LLSD::Integer)time(nullptr);
    all[mRegionName] = b;
    // bounded (memory paul-no-eternal-logs): the region not ridden for longest goes first
    while (all.size() > MAX_BEST_REGIONS)
    {
        std::string oldest;
        LLSD::Integer oldest_at = 0;
        for (LLSD::map_const_iterator it = all.beginMap(); it != all.endMap(); ++it)
        {
            const LLSD::Integer at = it->second.isMap() ? it->second["at"].asInteger() : 0;
            if (oldest.empty() || at < oldest_at)
            {
                oldest = it->first;
                oldest_at = at;
            }
        }
        all.erase(oldest);
    }
    gSavedSettings.setLLSD("WolfSkiBests", all);
}

void WolfSkis::endRun(bool keep)
{
    Shown& s = mShownData;
    if (!s.mRunning) return;
    s.mRunning = false;
    s.mInAir = false;
    mStillSince = 0.0;
    if (!keep || s.mRunSecs < MIN_RUN_SECS) return;
    s.mHaveLast = true;
    s.mLastSecs = s.mRunSecs;
    s.mLastVertical = s.mVertical;
    s.mLastTop = s.mTop;
    s.mLastScore = s.mScore;
    s.mLastBest = false;
    bool best = false;
    if (s.mScore > s.mBestScore) { s.mBestScore = s.mScore; best = s.mScore > 0; }
    if (s.mTop > s.mBestTop + 0.05f) { s.mBestTop = s.mTop; best = true; }
    if (s.mBigAir > s.mBestAir + 0.01f) { s.mBestAir = s.mBigAir; best = true; }
    if (best)
    {
        s.mLastBest = true;
        saveBests();
    }
    const F32 top = mph() ? s.mTop * 2.23694f : s.mTop * 3.6f;   // m/s -> mph (1 mph = 0.44704 m/s) / km/h
    popup("Run over: " + clockText(s.mRunSecs), llformat("%d points, top speed %.0f %s", s.mScore, top, mph() ? "mph" : "km/h"), 0);
    if (best) popup("New personal best!", "Kept for this region on this computer", 1);
}

void WolfSkis::onMode(const std::string& m)
{
    Shown& s = mShownData;
    const std::string was = s.mMode;
    s.mMode = m;
    const F64 t = now();
    // a carved turn ends: count it (left, right, left ... in a row is a combo)
    if ((was == "carve_l" || was == "carve_r") && m != was)
    {
        if (t - mCarveSince >= CARVE_MIN && s.mSpeed >= CARVE_SPEED && s.mRunning)
        {
            if (!mLastCarve.empty() && mLastCarve != was && t - mLastTurnAt < COMBO_GAP)
            {
                s.mCombo = llmin(s.mCombo + 1, MAX_COMBO);
            }
            else
            {
                s.mCombo = 1;
            }
            mLastCarve = was;
            mLastTurnAt = t;
            addScore(CARVE_POINTS * s.mCombo);
            if (s.mCombo >= 3)
            {
                static const char* const CHEER[] = { "Lovely carving!", "Smooth!", "Carving combo!", "Look at those edges!" };
                popup(llformat("%s x%d", CHEER[s.mCombo % 4], s.mCombo), llformat("+%d", CARVE_POINTS * s.mCombo), 0);
            }
        }
    }
    if (m == "carve_l" || m == "carve_r")
    {
        if (was != m) mCarveSince = t;
    }
    if (m == "brake" || m == "stop")
    {
        s.mCombo = 0;
        mLastCarve.clear();
    }
    if (m == "air" && !s.mInAir)
    {
        s.mInAir = true;
        mAirStart = t;
    }
    else if (m != "air" && m != "wipe" && s.mInAir)
    {
        // back on the snow (or the water) without a "land" line - a hop shorter than the script's AIR_MIN, or a splash:
        // the air clock stops (session s720: it ran on for 27 s after the skis came down in a lake)
        s.mInAir = false;
        s.mAirNow = 0.f;
    }
    if (m == "water" && was != "water")
    {
        popup("Splash!", "Skis and boards stop on the water", 2);
    }
}

void WolfSkis::onLand(F32 air, S32 spin, bool clean, F32 impact)
{
    Shown& s = mShownData;
    s.mInAir = false;
    s.mAirNow = 0.f;
    if (!clean) return;   // the wipe line says what happened
    s.mBigAir = llmax(s.mBigAir, air);
    S32 points = (S32)llround(air * AIR_POINTS_PER_S);
    std::string trick;
    // spins (MODEL bonuses): a half turn and more, in halves
    if (spin >= 720) { points += 800; trick = llformat("%d spin!", spin); }
    else if (spin >= 540) { points += 500; trick = "540!"; }
    else if (spin >= 360) { points += 250; trick = "360!"; }
    else if (spin >= 180) { points += 100; trick = "180 - riding backwards!"; }
    addScore(points);
    if (air >= BIG_AIR) popup(llformat("Big air! %.1f s", air), trick.empty() ? llformat("Clean landing  +%d", points) : trick + llformat("  +%d", points), 1);
    else if (air >= SHOW_AIR || !trick.empty()) popup(trick.empty() ? llformat("Nice jump! %.1f s", air) : trick,
                                                     llformat("Clean landing  +%d", points), trick.empty() ? 0 : 1);
    (void)impact;   // shown only on a wipe out
}

bool WolfSkis::onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0)
    {
        return false;
    }
    // [WOLF ONLY] off Wolf the line is left to chat like any other (this viewer says no hello there, so none comes).
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT)
    {
        return false;
    }
    // From here it is the HUD's line, never chat (as WolfHelm::onObjectChat): read only from the object sat on.
    const LLViewerObject* o = gObjectList.findObject(source);
    while (o && o->getParent())
    {
        o = (const LLViewerObject*)o->getParent();
    }
    LLViewerObject* seat = seatRoot();
    if (!o || !seat || o != seat || seat->getID() != mSeatRoot || text.size() > MAX_LINE || mKind == KIND_NONE)
    {
        return true;
    }
    const std::vector<std::string> f = split(text.substr(PREFIX.size()), '|', MAX_FIELDS);
    const std::string& k = f[0];
    Shown& s = mShownData;
    if (k == "hi")
    {
        mAnswered = true;
    }
    else if (k == "bye")
    {
        mAnswered = false;
    }
    else if (k == "st" && f.size() >= 2 && f[1].size() <= 12)
    {
        onMode(f[1]);
    }
    else if (k == "jump")
    {
        s.mInAir = true;
        mAirStart = now();
    }
    else if (k == "land" && f.size() >= 5)
    {
        onLand(num(f[1], 0.f, 60.f, 0.f), (S32)num(f[2], 0.f, 7200.f, 0.f), f[3] == "1", num(f[4], -100.f, 100.f, 0.f));
    }
    else if (k == "wipe" && f.size() >= 2)
    {
        s.mWiped = true;
        s.mInAir = false;
        s.mCombo = 0;
        mLastCarve.clear();
        std::string why = "Up you get - off you go again.";
        if (f[1] == "landing") why = "Too hard a landing. Up you get!";
        else if (f[1] == "edge") why = "Caught an edge landing sideways. Up you get!";
        else if (f[1] == "crash") why = "Ouch, you hit something. Up you get!";
        popup("Wipe out!", why, 2);
    }
    else if (k == "up")
    {
        s.mWiped = false;
    }
    return true;
}

void WolfSkis::measure(F32 dt)
{
    LLViewerObject* root = seatRoot();
    if (!root) return;
    Shown& s = mShownData;
    const F64 t = now();
    const LLVector3 vel = root->getVelocity();
    s.mSpeed = vel.length();
    const LLVector3 fwd = LLVector3::x_axis * root->getRotation();
    // compass heading: 0 north (+Y), clockwise (east is +X)
    F32 hdg = atan2f(fwd.mV[VX], fwd.mV[VY]) * RAD_TO_DEG;
    if (hdg < 0.f) hdg += 360.f;
    s.mHeading = hdg;
    // the steepness under the skis: the support height 1 m either side, both ways (the fall line's gradient)
    const LLVector3d p = root->getPositionGlobal();
    LLViewerRegion* region = root->getRegion();
    const F32 hx0 = supportHeight(region, p + LLVector3d(-1.0, 0.0, 0.0));
    const F32 hx1 = supportHeight(region, p + LLVector3d(1.0, 0.0, 0.0));
    const F32 hy0 = supportHeight(region, p + LLVector3d(0.0, -1.0, 0.0));
    const F32 hy1 = supportHeight(region, p + LLVector3d(0.0, 1.0, 0.0));
    const F32 g = sqrtf((hx1 - hx0) * (hx1 - hx0) + (hy1 - hy0) * (hy1 - hy0)) * 0.5f;
    s.mSlopeDeg = atanf(g) * RAD_TO_DEG;
    s.mSlopeKnown = true;

    if (s.mInAir) s.mAirNow = (F32)(t - mAirStart);

    // the run's clock: pushed off -> running; still for STILL_SECS -> over
    if (!s.mRunning)
    {
        if (s.mSpeed > START_SPEED && !s.mWiped)
        {
            s.mRunning = true;
            s.mRunSecs = 0.f;
            s.mVertical = 0.f;
            s.mTop = 0.f;
            s.mBigAir = 0.f;
            s.mScore = 0;
            s.mCombo = 0;
            mLastCarve.clear();
            mStillSince = 0.0;
            mFastSince = 0.0;
            s.mHaveLast = false;
        }
    }
    if (s.mRunning)
    {
        s.mRunSecs += dt;
        s.mTop = llmax(s.mTop, s.mSpeed);
        if (mHavePos)
        {
            const F64 down = mLastPos.mdV[VZ] - p.mdV[VZ];
            if (down > 0.0 && down < 50.0) s.mVertical += (F32)down;   // (a teleport or a region change is no descent)
        }
        // speed points: each whole second faster than FAST_SPEED
        if (s.mSpeed > FAST_SPEED)
        {
            if (mFastSince == 0.0) mFastSince = t;
            else if (t - mFastSince >= 1.0)
            {
                mFastSince = t;
                addScore(FAST_POINTS);
            }
        }
        else
        {
            mFastSince = 0.0;
        }
        if (s.mSpeed < STILL_SPEED && !s.mInAir)
        {
            if (mStillSince == 0.0) mStillSince = t;
            else if (t - mStillSince >= STILL_SECS) endRun(true);
        }
        else
        {
            mStillSince = 0.0;
        }
    }
    mLastPos = p;
    mHavePos = true;
}

void WolfSkis::idle()
{
    if (!onWolf())
    {
        // [WOLF ONLY] nothing is said, read or shown off Wolf; a ride in progress is simply forgotten
        if (mSeatRoot.notNull()) reset(LLUUID::null);
        return;
    }
    LLViewerObject* root = seatRoot();
    const LLUUID id = root ? root->getID() : LLUUID::null;
    if (id != mSeatRoot)
    {
        endRun(true);   // stood up mid-run: a run long enough still counts
        reset(id);
        if (root) WolfObjectProps::instance().want(root, false);
    }
    if (!root)
    {
        return;   // not sitting: nothing at all to do (memory paul-think-of-scale)
    }
    if (!mDescKnown)
    {
        const WolfObjectProps::Props* p = WolfObjectProps::instance().get(id);
        if (!p)
        {
            WolfObjectProps::instance().want(root, false);
            return;
        }
        mDescKnown = true;
        if (descIs(p->mDescriptionLower, "skis")) mKind = KIND_SKIS;
        else if (descIs(p->mDescriptionLower, "snowboard")) mKind = KIND_SNOWBOARD;
        if (mKind == KIND_NONE) return;
        mRegionName = root->getRegion() ? root->getRegion()->getName() : std::string();
        loadBests();
        if (gSavedSettings.getBOOL("WolfSkiHudAuto"))
        {
            mShown = true;
            claimBottom();
        }
        LL_INFOS("WolfSkis") << "on " << kindWord(mKind) << " '" << p->mName << "' in " << mRegionName << LL_ENDL;
    }
    if (mKind == KIND_NONE)
    {
        return;
    }
    const F64 t = now();
    if (!mAnswered && mHelloTries < HELLO_TRIES && t - mHelloAt >= HELLO_RETRY && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mHelloAt = t;
        ++mHelloTries;
        say(HELLO);
    }
    // a region change (they ride across a border): the bests are the new region's
    if (root->getRegion() && root->getRegion()->getName() != mRegionName)
    {
        mRegionName = root->getRegion()->getName();
        loadBests();
    }
    const F32 dt = mLastTick > 0.0 ? (F32)llclamp(t - mLastTick, 0.0, 0.5) : 0.f;
    mLastTick = t;
    measure(dt);
    // one bottom panel: a dashboard, deck or helm opened by hand after ours takes the bottom
    if (mShown)
    {
        const WolfFlight& wf = WolfFlight::instance();
        if (WolfDrive::instance().dashboardOn() || (wf.active() && !wf.deckHidden()) || WolfHelm::instance().hudShowing())
        {
            mShown = false;
            mClosedByHand = true;
        }
    }
}
