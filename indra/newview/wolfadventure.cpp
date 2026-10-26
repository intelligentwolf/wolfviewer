/**
 * @file wolfadventure.cpp
 * @brief WolfViewer: Wolf Adventure, the player's side. See wolfadventure.h.
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

#include "wolfadventure.h"
#include "wolfai.h"   // [WOLF AI CREDITS 2026-10-24/] note_credit_use

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/json.hpp>

#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llappviewer.h"
#include "llaudioengine.h"
#include "llcorehttputil.h"
#include "lldir.h"
#include "llfile.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llparcel.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llvieweraudio.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "wolfadventurehud.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgrid.h"

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // [WOLF ADVENTURE GATE 2026-10-17] brain calls made while between Wolf regions (WolfAdventure::api): held until the
    // region confirms it is Wolf, at most API_HOLD_MAX of them for at most API_HOLD_SEC (a region's SimulatorFeatures
    // normally arrive within a few seconds of the region change; past that the caller is told and may press again)
    struct HeldCall { std::string mAction; LLSD mBody; WolfAdventure::api_callback_t mDone; F64 mAt; };
    std::vector<HeldCall> sHeldCalls;
    const size_t API_HOLD_MAX = 16;
    const F64 API_HOLD_SEC = 30.0;
    void failHeldCalls(const std::string& why)
    {
        std::vector<HeldCall> gone;
        gone.swap(sHeldCalls);   // a callback that calls api() again starts a new list
        for (HeldCall& h : gone) if (h.mDone) h.mDone(false, LLSD(), why);
    }

    // Source: HUD_MESSAGES_V1.md §1.2 - the viewer -> region method, a separate one from wolfgame
    // (the sim's AddGenericPacketHandler is TryAdd per method name, LLClientView.cs:726-731).
    const std::string METHOD_TO_SIM("wolfadventure");
    // Source: HUD_MESSAGES_V1.md §1.2 - at most 240 bytes a parameter, at most 900 bytes of
    // parameters in all (one reliable packet, MTU 1200 at net.h:65).
    constexpr size_t PARAM_BYTES = 240, VERB_BYTES = 900;
    // Source: HUD_MESSAGES_V1.md §4 hello "3 s after arriving on a region".
    constexpr F64 HELLO_DELAY = 3.0;
    // Source: HUD_MESSAGES_V1.md §1.3 text caps.
    constexpr size_t CAP_TITLE = 64, CAP_NAME = 64, CAP_LABEL = 24, CAP_OBJECTIVE = 240, CAP_VIGNETTE = 2000,
                     CAP_ENTRY = 500, CAP_HANDOUT = 4000, CAP_ID = 64, CAP_TALK = 1000;
    // Source: HUD_MESSAGES_V1.md §3.2 "the replies, as up to 6 buttons"; a cite list is held to 8.
    constexpr size_t MAX_REPLIES = 6, MAX_CITES = 8;
    // A click that the region never answers (an old region without the hands' talk): after this the
    // panel says so instead of waiting for ever (§6 4.1.3: the state is never silent).
    constexpr F64 TALK_WAIT_LIMIT = 8.0;
    // Lists are held to these (a message is at most 16 KiB, §1.1; these keep a bad one bounded).
    // §2.1 done "at most 10"; §2.2 entries "at most 40 per page"; §3.3 options "up to 6".
    constexpr size_t MAX_DONE = 10, MAX_ENTRIES = 40, MAX_OPTIONS = 6, MAX_ADVENTURES = 16, MAX_CARDS = 8,
                     MAX_LIST = 40, MAX_BANDS = 8;
    // Source: HUD_MESSAGES_V1.md §2.6 / §5.5.
    constexpr F32 FADE_MIN_SEC = 0.33f;          // a fade is never a flash (WCAG 2.3.1)
    constexpr F64 FADE_BLACK_LIMIT = 10.0;       // cleared after 10 s at black
    constexpr F64 LETTERBOX_LIMIT = 120.0;       // bars with no on:0 within 120 s clear
    constexpr F32 LETTERBOX_SLIDE = 0.6f;        // default slide
    constexpr F32 TITLE_DEFAULT = 4.f;           // a title's default seconds
    constexpr F32 CAMERA_HOLD = 2.f;             // look / pan hold 2 s
    constexpr F32 PAN_MAX = 8.f;                 // pan at most 8 s
    constexpr F64 CAMERA_REPEAT = 20.0;          // never repeated within 20 s
    // Source: adventure_pack_v1.schema.json stageCue.sec maximum 30.
    constexpr F32 CUE_MAX_SEC = 30.f;
    // Settled in P1 (HUD_MESSAGES_V1.md §2.6 notes, the same in both viewers): `look` and `pan` keep
    // the camera where it stands; a look turns it to `pos`, a pan sweeps its view from `from` to
    // `pos` (a pan in the film sense). Both take 1.5 s without `sec` (the example's value).
    constexpr F32 CUE_DEFAULT_SEC = 1.5f;

    std::string str(const LLSD& m, const char* k, size_t cap, bool lines = false)
    {
        return m.has(k) && m[k].isString() ? WolfGame::cleanText(m[k].asString(), cap, lines) : std::string();
    }

    /** Same reading as wolfgame.cpp num(): integers as they are, reals rounded, non-finite = fallback. */
    S64 num(const LLSD& m, const char* k, S64 fallback = 0)
    {
        if (!m.has(k)) return fallback;
        const LLSD& v = m[k];
        if (v.isInteger()) return v.asInteger();
        if (v.isReal())
        {
            const F64 d = v.asReal();
            if (!std::isfinite(d)) return fallback;
            return (S64)std::llround(llclamp(d, -1e15, 1e15));
        }
        if (v.isBoolean()) return v.asBoolean() ? 1 : 0;
        return fallback;
    }

    S32 num32(const LLSD& m, const char* k, S64 fallback = 0)
    {
        return (S32)llclamp(num(m, k, fallback), (S64)std::numeric_limits<S32>::min(), (S64)std::numeric_limits<S32>::max());
    }

    F64 real(const LLSD& m, const char* k, F64 fallback = 0.0)
    {
        if (!m.has(k)) return fallback;
        const LLSD& v = m[k];
        if (!(v.isInteger() || v.isReal())) return fallback;
        const F64 d = v.asReal();
        return std::isfinite(d) ? llclamp(d, -1e15, 1e15) : fallback;
    }

    /** An id field: a lower-case uuid or a pack id, capped (§1.3 ids 64). */
    std::string idOf(const LLSD& m, const char* k)
    {
        std::string s = str(m, k, CAP_ID);
        LLStringUtil::toLower(s);
        return s;
    }

    /** A texture uuid field: only a well-formed, non-null uuid, else "" (§1.3 "Texture UUIDs"). */
    std::string textureOf(const LLSD& m, const char* k)
    {
        const std::string s = str(m, k, 36);
        if (s.size() != 36 || !LLUUID::validate(s)) return std::string();
        const LLUUID id(s);
        return id.notNull() ? id.asString() : std::string();
    }

    /** [x, y, z] in region metres, finite and within +-1e6 (the wolfGame marker rule, wolfgame.cpp applyMarker). */
    bool vec3(const LLSD& v, LLVector3& out)
    {
        if (!v.isArray() || v.size() < 3) return false;
        F32 c[3];
        for (S32 i = 0; i < 3; ++i)
        {
            if (!(v[i].isReal() || v[i].isInteger())) return false;
            const F64 d = v[i].asReal();
            if (!std::isfinite(d) || fabs(d) > 1e6) return false;
            c[i] = (F32)d;
        }
        out.set(c[0], c[1], c[2]);
        return true;
    }

    /** "#rrggbb" to a colour (the wolfGame COLOUR rule, wolfgame.cpp parseColour). */
    bool colourOf(const std::string& s, LLColor4& out)
    {
        if (s.size() != 7 || s[0] != '#') return false;
        for (size_t i = 1; i < 7; ++i)
        {
            if (!isxdigit((unsigned char)s[i])) return false;
        }
        const unsigned long v = strtoul(s.c_str() + 1, nullptr, 16);
        out = LLColor4(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, 1.f);
        return true;
    }

    WolfAdventure::Band bandOf(const LLSD& b)
    {
        WolfAdventure::Band out;
        if (!b.isMap()) return out;
        out.mId = idOf(b, "id");
        out.mLabel = str(b, "label", CAP_LABEL);
        out.mFrom = num32(b, "from");
        out.mHasColour = colourOf(str(b, "colour", 7), out.mColour);
        return out;
    }

    WolfAdventure::Meter meterOf(const LLSD& m, const char* label_key)
    {
        WolfAdventure::Meter out;
        if (!m.isMap()) return out;
        out.mOn = true;
        out.mId = idOf(m, "id");
        out.mLabel = str(m, label_key, CAP_NAME);
        out.mMin = num32(m, "min", -100);
        out.mMax = num32(m, "max", 100);
        if (out.mMax <= out.mMin) { out.mMin = -100; out.mMax = 100; }
        out.mValue = llclamp(num32(m, "v"), out.mMin, out.mMax);
        out.mDelta = num32(m, "delta");
        out.mWhy = str(m, "why", CAP_OBJECTIVE);   // §1.3 note: rep.*.why as the objective (240)
        if (m["band"].isMap())
        {
            out.mHasBand = true;
            out.mBand = bandOf(m["band"]);
        }
        const LLSD& bands = m["bands"];
        if (bands.isArray())
        {
            for (LLSD::array_const_iterator it = bands.beginArray(); it != bands.endArray() && out.mBands.size() < MAX_BANDS; ++it)
            {
                if (it->isMap()) out.mBands.push_back(bandOf(*it));
            }
            std::stable_sort(out.mBands.begin(), out.mBands.end(), [](const WolfAdventure::Band& a, const WolfAdventure::Band& b) { return a.mFrom < b.mFrom; });
        }
        return out;
    }

    /** A signed number with its sign: "+5", "-3". */
    std::string signedText(S32 v)
    {
        return v > 0 ? "+" + std::to_string(v) : std::to_string(v);
    }
}

WolfAdventure::WolfAdventure()
{
}

WolfAdventure::~WolfAdventure()
{
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// receiving
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventure::apply(const std::string& t, const LLSD& m)
{
    // [WOLF ADVENTURE P4 2026-10-16] + roll, dice, turns, quiz and gm (§2.7, §2.9-2.11).
    if (t != "quest" && t != "journal" && t != "vignette" && t != "rep" && t != "stage" && t != "talk"
        && t != "roll" && t != "dice" && t != "turns" && t != "quiz" && t != "gm") return false;
    // HUD_MESSAGES_V1.md §1.3: every adventure message carries verified 1, set by the hands. The
    // hands are the only forwarder of these types (REGION_HANDS_V1.md §3 `hud`), so anything else
    // is not theirs and is dropped. (WolfGame::receivePacket already took only Wolf, this region.)
    // [WOLF ADVENTURE GATE 2026-10-17] only from a region that has itself said it is Wolf (isOnWolfTerritoriesConfirmed, the
    // WolfBorder::receive rule, wolfborder.cpp:239-246): a foreign region claiming to be Wolf Adventure is ignored. The
    // hands answer the viewer's hello (sent only once confirmed, idle()) with the state again, so nothing is lost.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        LL_DEBUGS("WolfAdventure") << "dropped an adventure message '" << t << "' from a region not confirmed as Wolf" << LL_ENDL;
        return true;
    }
    if (num(m, "verified") != 1)
    {
        LL_DEBUGS("WolfAdventure") << "dropped an unverified adventure message '" << t << "'" << LL_ENDL;
        return true;
    }
    if (t == "quest") applyQuest(m);
    else if (t == "journal") applyJournal(m);
    else if (t == "vignette") applyVignette(m);
    else if (t == "rep") applyRep(m);
    else if (t == "talk") applyTalk(m);
    else if (t == "roll") applyRoll(m);
    else if (t == "dice") applyDice(m);
    else if (t == "turns") applyTurns(m);
    else if (t == "quiz") applyQuiz(m);
    else if (t == "gm") applyGm(m);
    else applyStage(m);
    ++mGeneration;
    return true;
}

WolfAdventure::Adventure& WolfAdventure::adventure(const std::string& adv)
{
    auto it = mAdventures.find(adv);
    if (it != mAdventures.end()) return it->second;
    // Bounded: the oldest that is not the current one goes.
    while (mAdventures.size() >= MAX_ADVENTURES)
    {
        auto oldest = mAdventures.end();
        for (auto o = mAdventures.begin(); o != mAdventures.end(); ++o)
        {
            if (o->first == mCurrent) continue;
            if (oldest == mAdventures.end() || o->second.mSeq < oldest->second.mSeq) oldest = o;
        }
        if (oldest == mAdventures.end()) break;
        dropAdventure(oldest->first);
    }
    Adventure& a = mAdventures[adv];
    a.mSeq = ++mSeq;
    a.mFirst = a.mSeq;
    return a;
}

const WolfAdventure::Adventure* WolfAdventure::find(const std::string& adv) const
{
    auto it = mAdventures.find(adv);
    return it == mAdventures.end() ? nullptr : &it->second;
}

std::vector<std::string> WolfAdventure::playing() const
{
    std::vector<std::pair<U32, std::string>> order;
    for (const auto& kv : mAdventures)
    {
        if (kv.second.mHasQuest) order.emplace_back(kv.second.mFirst, kv.first);
    }
    std::sort(order.begin(), order.end());
    std::vector<std::string> out;
    for (const auto& o : order) out.push_back(o.second);
    return out;
}

const WolfAdventure::Adventure* WolfAdventure::current() const
{
    const Adventure* a = find(mCurrent);
    if (a && a->mHasQuest) return a;
    // the newest one with a quest
    const Adventure* best = nullptr;
    for (const auto& kv : mAdventures)
    {
        if (kv.second.mHasQuest && (!best || kv.second.mSeq > best->mSeq)) best = &kv.second;
    }
    return best;
}

void WolfAdventure::dropAdventure(std::string adv)
{
    // §5.4 leaving an adventure: its quest, journal, rep, sheet and cards go.
    mAdventures.erase(adv);
    mCards.erase(std::remove_if(mCards.begin(), mCards.end(), [&](const Card& c) { return c.mAdv == adv; }), mCards.end());
    if (mCurrent == adv) mCurrent.clear();
    if (mMusicAdv == adv) stopMusic();
    if (mTalk.mOn && mTalk.mAdv == adv)
    {
        mTalk = Talk();   // §5.4 leaving an adventure: its talk goes too
        ++mTalk.mSeq;
    }
    // [WOLF ADVENTURE P4 2026-10-16] and its roll, dice, turn tracker, quiz and gm layers
    if (mRoll.mAdv == adv) mRoll = Roll();
    if (mDice.mAdv == adv) mDice = Dice();
    if (mTurns.mAdv == adv) mTurns = Turns();
    if (mQuiz.mAdv == adv) mQuiz = Quiz();
    mGm.erase(adv);
    ++mGeneration;
}

// Source: HUD_MESSAGES_V1.md §2.1.
void WolfAdventure::applyQuest(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    if (adv.empty()) return;
    if (num(m, "clear") != 0)
    {
        dropAdventure(adv);
        return;
    }
    Adventure* a = nullptr;
    if (num(m, "patch") != 0)
    {
        // A patch merges into the copy; one for an adventure without a copy is ignored.
        auto it = mAdventures.find(adv);
        if (it == mAdventures.end() || !it->second.mHasQuest) return;
        a = &it->second;
        // Only the quest's own fields merge (HUD_MESSAGES_V1.md §2.1, the ones parseQuest reads, and `class`,
        // aiTagShown): anything else a patch carries would only grow the kept copy. `adv` found the copy.
        static const char* const PATCHABLE[] = { "src", "chapter", "act", "scene", "sceneTitle", "beat", "objective",
            "marker", "done", "party", "paused", "pausedText", "gm", "ai", "ended", "cast", "class", "look",
            "goal", "shape", "main" };   // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 4
        for (const char* key : PATCHABLE)
        {
            if (m.has(key)) a->mQuestRaw[key] = m[key];
        }
    }
    else
    {
        a = &adventure(adv);
        a->mQuestRaw = m;
    }
    // [WOLF ADVENTURE P2 2026-10-16] a reply that fired a trigger moves the story instead of drawing
    // a line (BRAIN_API_V1.md §4 "A talk or say no trigger takes is answered"): the quest update is
    // the answer, so the talk panel stops waiting for one.
    if (mTalk.mOn && mTalk.mAdv == adv && mTalk.mWaiting)
    {
        mTalk.mWaiting = false;
        talkChanged();
    }
    const bool had = a->mHasQuest;
    const bool was_paused = had && a->mQuest.mPaused;
    const bool was_ended = had && a->mQuest.mEnded.mOn;
    parseQuest(a->mQuestRaw, a->mQuest);
    a->mHasQuest = true;
    a->mSeq = ++mSeq;
    mCurrent = adv;
    const Quest& q = a->mQuest;
    // §6 4.1.3: "Paused" is announced through the notification area, as combaterr is.
    if (q.mPaused && !was_paused)
    {
        notice(q.mSrc.empty() ? std::string("Adventure") : q.mSrc,
               q.mPausedText.empty() ? std::string("The story is paused.") : q.mPausedText, false);
    }
    if (q.mEnded.mOn && !was_ended)
    {
        notice(q.mEnded.mTitle.empty() ? std::string("The adventure is over") : q.mEnded.mTitle,
               q.mEnded.mText.empty() ? q.mSrc : q.mEnded.mText, true);
    }
}

void WolfAdventure::parseQuest(const LLSD& m, Quest& q) const
{
    q = Quest();
    q.mAdv = idOf(m, "adv");
    q.mSrc = str(m, "src", CAP_TITLE);
    q.mChapter = str(m, "chapter", CAP_TITLE);
    q.mAct = idOf(m, "act");
    q.mScene = idOf(m, "scene");
    q.mSceneTitle = str(m, "sceneTitle", CAP_TITLE);
    q.mBeat = idOf(m, "beat");
    q.mObjective = str(m, "objective", CAP_OBJECTIVE, true);
    // [GAME CARD 2026-10-18] Source: wolfadventure_lib.php quest message 'goal' (adventure.card.goal, cut to 240), 'shape' (the
    // card's shape), 'main' (wa_main_msg: {step, of, title} or null when the pack has no main steps)
    q.mGoal = str(m, "goal", CAP_OBJECTIVE, true);
    q.mShape = str(m, "shape", CAP_LABEL);
    {
        const LLSD& mn = m["main"];
        if (mn.isMap())
        {
            q.mMainOf = llclamp(num32(mn, "of"), 0, 1000);
            q.mMainStep = llclamp(num32(mn, "step"), 0, q.mMainOf);
            q.mMainTitle = str(mn, "title", CAP_OBJECTIVE, true);
        }
    }
    // [WOLF ADVENTURE AUDIT 2026-10-17] §2.1 look: a texture uuid only (§1.3), and both colours or neither
    const LLSD& lk = m["look"];
    if (lk.isMap())
    {
        const std::string logo = lk["logo"].asString();
        if (logo.size() == 36 && LLUUID::validate(logo) && LLUUID(logo).notNull()) q.mLook.mLogo = logo;
        q.mLook.mColours = WolfAdvStudio::parseColour(lk["accent"].asString(), q.mLook.mAccent)
                        && WolfAdvStudio::parseColour(lk["panel"].asString(), q.mLook.mPanel);
    }
    const LLSD& mk = m["marker"];
    if (mk.isMap())
    {
        q.mMarker.mId = str(mk, "id", CAP_ID);
        q.mMarker.mRegion = str(mk, "region", CAP_NAME);
        q.mMarker.mLabel = str(mk, "label", CAP_TITLE);
        const F64 x = real(mk, "x"), y = real(mk, "y"), z = real(mk, "z");
        q.mMarker.mOn = !q.mMarker.mId.empty() && fabs(x) <= 1e6 && fabs(y) <= 1e6 && fabs(z) <= 1e6;
        q.mMarker.mPos.set((F32)x, (F32)y, (F32)z);
    }
    const LLSD& done = m["done"];
    if (done.isArray())
    {
        for (LLSD::array_const_iterator it = done.beginArray(); it != done.endArray() && q.mDone.size() < MAX_DONE; ++it)
        {
            if (it->isString()) q.mDone.push_back(WolfGame::cleanText(it->asString(), CAP_OBJECTIVE));
        }
    }
    const LLSD& party = m["party"];
    if (party.isMap())
    {
        q.mParty.mOn = true;
        q.mParty.mId = idOf(party, "id");
        q.mParty.mLeader = idOf(party, "leader");
        q.mParty.mSize = llclamp(num32(party, "size"), 0, 64);
        q.mParty.mReady = num(party, "ready") != 0;
        q.mParty.mWaiting = llclamp(num32(party, "waiting"), 0, 64);
    }
    q.mPaused = num(m, "paused") != 0;
    q.mPausedText = str(m, "pausedText", CAP_OBJECTIVE);
    q.mGm = num(m, "gm") != 0;
    q.mAi = num(m, "ai", 1) != 0;
    const LLSD& ended = m["ended"];
    if (ended.isMap())
    {
        q.mEnded.mOn = true;
        q.mEnded.mId = idOf(ended, "id");
        q.mEnded.mTitle = str(ended, "title", CAP_TITLE);
        q.mEnded.mText = str(ended, "text", CAP_VIGNETTE, true);
        q.mEnded.mKind = str(ended, "kind", CAP_LABEL);   // [WORDING 2026-10-18] the ending's kind, when the brain sends it
    }
    // [WOLF ADVENTURE P2 2026-10-16] `cast` (§2.1): the story characters on the player's region,
    // the talk panel's click-to-talk list (§3.2). Only well-formed, non-null agent keys.
    const LLSD& cast = m["cast"];
    if (cast.isArray())
    {
        for (LLSD::array_const_iterator it = cast.beginArray(); it != cast.endArray() && q.mCast.size() < MAX_LIST; ++it)
        {
            if (!it->isMap()) continue;
            const std::string npc = str(*it, "npc", 36);
            if (npc.size() != 36 || !LLUUID::validate(npc)) continue;
            CastRef c;
            c.mNpc = LLUUID(npc);
            if (c.mNpc.isNull()) continue;
            c.mName = str(*it, "name", CAP_NAME);
            q.mCast.push_back(c);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [WOLF ADVENTURE P2 2026-10-16] the talk panel (HUD_MESSAGES_V1.md §2.4, §3.2, §4)
// ═══════════════════════════════════════════════════════════════════════════════════════

// Source: HUD_MESSAGES_V1.md §4 - "say and gm_say at most one per 1.5 s" (the hands drop faster ones).
const F64 WolfAdventure::SAY_GAP = 1.5;

void WolfAdventure::applyTalk(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    const std::string npc_s = str(m, "npc", 36);
    if (adv.empty() || npc_s.size() != 36 || !LLUUID::validate(npc_s)) return;
    const LLUUID npc(npc_s);
    if (npc.isNull()) return;
    const std::string state = str(m, "state", CAP_LABEL);
    if (state == "close")
    {
        // §2.4 close: the character ends the talk (or end_talk); only the conversation it names.
        if (mTalk.mOn && mTalk.mNpc == npc)
        {
            mTalk = Talk();
            talkChanged();
        }
        return;
    }
    if (state != "open" && state != "typing" && state != "line") return;
    // A message about another character than the one on the panel starts a new conversation.
    if (!mTalk.mOn || mTalk.mNpc != npc || mTalk.mAdv != adv)
    {
        mTalk = Talk();
        mTalk.mOn = true;
        mTalk.mNpc = npc;
        mTalk.mAdv = adv;
        mTalk.mSince = now();
    }
    Talk& t = mTalk;
    t.mWaiting = false;
    t.mError.clear();
    const std::string src = str(m, "src", CAP_TITLE);
    if (!src.empty()) t.mSrc = src;
    const std::string name = str(m, "name", CAP_NAME);
    if (!name.empty()) t.mName = name;
    if (m.has("title")) t.mTitle = str(m, "title", CAP_TITLE);
    if (m.has("portrait")) t.mPortrait = textureOf(m, "portrait");
    if (state == "typing")
    {
        // §2.4 typing: "<name> is thinking..." replaces the line until `line`.
        t.mTyping = true;
        talkChanged();
        return;
    }
    t.mTyping = false;
    const LLSD& line = m["line"];
    if (line.isMap())
    {
        t.mHasLine = true;
        t.mText = str(line, "text", CAP_TALK, true);
        t.mEmote = str(line, "emote", CAP_OBJECTIVE);
        t.mAi = num(line, "ai") != 0;
        t.mCites.clear();
        const LLSD& cites = line["cites"];
        if (cites.isArray())
        {
            for (LLSD::array_const_iterator it = cites.beginArray(); it != cites.endArray() && t.mCites.size() < MAX_CITES; ++it)
            {
                if (it->isMap()) t.mCites.push_back({ str(*it, "title", CAP_TITLE), str(*it, "where", CAP_OBJECTIVE) });
            }
        }
    }
    else if (state == "open")
    {
        // an `open` with no line: the panel shows with the character's name and the replies only
        t.mHasLine = false;
        t.mText.clear();
        t.mEmote.clear();
        t.mCites.clear();
    }
    if (m.has("replies"))
    {
        t.mReplies.clear();
        const LLSD& rs = m["replies"];
        if (rs.isArray())
        {
            for (LLSD::array_const_iterator it = rs.beginArray(); it != rs.endArray() && t.mReplies.size() < MAX_REPLIES; ++it)
            {
                if (!it->isMap()) continue;
                TalkReply r{ idOf(*it, "id"), str(*it, "label", CAP_LABEL) };
                if (!r.mId.empty() && !r.mLabel.empty()) t.mReplies.push_back(r);
            }
        }
    }
    if (m.has("free")) t.mFree = num(m, "free") != 0;
    if (m.has("give")) t.mGive = num(m, "give") != 0;
    t.mVoiced = num(m, "voiced") != 0;
    const LLSD& mood = m["mood"];
    if (mood.isMap())
    {
        t.mHasMood = true;
        t.mMoodLabel = str(mood, "label", CAP_LABEL);
        t.mMoodBand = idOf(mood, "band");
    }
    else if (m.has("mood"))
    {
        t.mHasMood = false;   // mood null: no mood shown
    }
    // p2brief: `hint` - a give_hint action's one sentence, shown under the line
    t.mHint = str(m, "hint", CAP_OBJECTIVE);
    const std::string notice = str(m, "notice", CAP_ENTRY);
    if (!notice.empty()) t.mNotice = notice;   // §2.4 "present only the first time": kept for this talk
    talkChanged();
}

bool WolfAdventure::isStoryCharacter(const LLUUID& avatar) const
{
    if (avatar.isNull()) return false;
    for (const auto& kv : mAdventures)
    {
        if (!kv.second.mHasQuest || kv.second.mQuest.mEnded.mOn) continue;
        for (const CastRef& c : kv.second.mQuest.mCast)
        {
            if (c.mNpc == avatar) return true;
        }
    }
    return false;
}

bool WolfAdventure::talkTo(const LLUUID& avatar)
{
    if (!WolfGrid::isOnWolfTerritories() || avatar.isNull()) return false;
    // Source: HUD_MESSAGES_V1.md §3.2 Opening - only the `quest` message's cast; "a click on any
    // other avatar does nothing new".
    const CastRef* who = nullptr;
    std::string adv, src;
    for (const auto& kv : mAdventures)
    {
        if (!kv.second.mHasQuest || kv.second.mQuest.mEnded.mOn) continue;
        for (const CastRef& c : kv.second.mQuest.mCast)
        {
            if (c.mNpc == avatar) { who = &c; adv = kv.first; src = kv.second.mQuest.mSrc; break; }
        }
        if (who) break;
    }
    if (!who) return false;
    // A second click on the character already talking just brings the panel back.
    if (mTalk.mOn && mTalk.mNpc == avatar && !mTalk.mError.empty()) mTalk.mError.clear();
    if (!(mTalk.mOn && mTalk.mNpc == avatar))
    {
        if (mTalk.mOn && mTalk.mNpc.notNull()) sendTalkEnd(mTalk.mNpc);   // leaving one conversation for another
        mTalk = Talk();
        mTalk.mOn = true;
        mTalk.mNpc = avatar;
        mTalk.mAdv = adv;
        mTalk.mSrc = src;
        mTalk.mName = who->mName;
        mTalk.mSince = now();
    }
    if (sendTalk(avatar))
    {
        mTalk.mWaiting = !mTalk.mHasLine;
        mTalk.mSince = now();
    }
    else
    {
        mTalk.mError = "Not sent: the viewer is not connected to a Wolf Territories region.";
    }
    talkChanged();
    WolfAdventureUI::syncTalk();
    return true;
}

bool WolfAdventure::talkReply(S32 index)
{
    if (!mTalk.mOn || index < 0 || index >= (S32)mTalk.mReplies.size()) return false;
    const TalkReply r = mTalk.mReplies[index];
    if (!sendReply(mTalk.mNpc, r.mId))
    {
        mTalk.mError = "Not sent: the viewer is not connected to a Wolf Territories region.";
        talkChanged();
        return false;
    }
    mTalk.mError.clear();
    mTalk.mYouSaid = r.mLabel;
    mTalk.mWaiting = true;
    mTalk.mSince = now();
    talkChanged();
    return true;
}

F64 WolfAdventure::talkSayWait() const
{
    return llmax(0.0, mLastSayAt + SAY_GAP - now());
}

bool WolfAdventure::talkSay(const std::string& text_in)
{
    if (!mTalk.mOn) return false;
    std::string text = cutUtf8(text_in, MAX_TEXT_BYTES);
    LLStringUtil::trim(text);
    if (text.empty())
    {
        mTalk.mError = "Type something to say first.";
        talkChanged();
        return false;
    }
    if (talkSayWait() > 0.0)
    {
        mTalk.mError = "Wait a moment before saying the next thing.";
        talkChanged();
        return false;
    }
    if (!sendSay(mTalk.mNpc, text))
    {
        mTalk.mError = "Not sent: the viewer is not connected to a Wolf Territories region.";
        talkChanged();
        return false;
    }
    mLastSayAt = now();
    mTalk.mError.clear();
    mTalk.mYouSaid = text;
    // §2.4: the hands send `typing` at once; shown here at once too, so there is never a gap.
    mTalk.mTyping = true;
    mTalk.mSince = now();
    talkChanged();
    return true;
}

bool WolfAdventure::talkGive(const std::string& item, S32 qty)
{
    if (!mTalk.mOn || item.empty()) return false;
    if (!sendGive(mTalk.mNpc, item, llmax(1, qty)))
    {
        mTalk.mError = "Not given: the viewer is not connected to a Wolf Territories region.";
        talkChanged();
        return false;
    }
    mTalk.mError.clear();
    mTalk.mYouSaid = qty > 1 ? llformat("(You offer %d %s.)", qty, item.c_str()) : "(You offer " + item + ".)";
    mTalk.mWaiting = true;
    mTalk.mSince = now();
    talkChanged();
    return true;
}

void WolfAdventure::talkClose()
{
    if (!mTalk.mOn) return;
    if (mTalk.mNpc.notNull()) sendTalkEnd(mTalk.mNpc);   // §4 talkend: X, Escape or walking away
    mTalk = Talk();
    talkChanged();
}

namespace
{
    // Source: BRAIN_API_V1.md §1 / WOLF_MIND_V1.md §8.3 - a clip is served by the web host that made
    // it, wolfstorm.app or wolf-grid.com, from php/wolfadventure_media.php. Nothing else is fetched.
    bool clipUrlAllowed(const std::string& url)
    {
        static const char* const PREFIXES[] = {
            "https://wolfstorm.app/php/wolfadventure_media.php?", "https://www.wolfstorm.app/php/wolfadventure_media.php?",
            "https://wolf-grid.com/php/wolfadventure_media.php?", "https://www.wolf-grid.com/php/wolfadventure_media.php?",
            // wolf-grid.com serves WolfStorm (and its php/) under /wolfstorm/: the brain's mediaBase there is
            // https://wolf-grid.com/wolfstorm/php (/etc/wolfstorm/wolfadv.json on legolas, 2026-10-16).
            "https://wolf-grid.com/wolfstorm/php/wolfadventure_media.php?", "https://www.wolf-grid.com/wolfstorm/php/wolfadventure_media.php?",
        };
        if (url.size() > 1024) return false;
        for (const char* p : PREFIXES)
        {
            if (url.rfind(p, 0) == 0) return true;
        }
        return false;
    }

    struct PlayedClip { std::string mPath; LLUUID mId; F64 mUntil; };
    std::vector<PlayedClip> sPlayedClips;

    /** Clips played more than a minute ago: their file and the engine's entry go. */
    void sweepClips(bool all)
    {
        const F64 t = LLFrameTimer::getElapsedSeconds();
        for (auto it = sPlayedClips.begin(); it != sPlayedClips.end();)
        {
            if (all || t > it->mUntil)
            {
                if (gAudiop) gAudiop->removeAudioData(it->mId);
                LLFile::remove(it->mPath);
                it = sPlayedClips.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

// static
void WolfAdventure::playClip(const std::string& url, std::function<void(const std::string& error)> done)
{
    if (!clipUrlAllowed(url))
    {
        if (done) done("the voice clip did not come from the adventure service");
        return;
    }
    // [WOLF ADVENTURE GATE 2026-10-17] a fetch from the Wolf service: only while on a confirmed Wolf region (as api())
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (done) done("Adventures work on Wolf Territories only.");
        return;
    }
    if (!gAudiop)
    {
        if (done) done("sound is off in this viewer (no audio engine)");
        return;
    }
    LLCoros::instance().launch("WolfAdventure clip", [url, done]()
    {
        // Source: wolfspeech.cpp ttsCoro / postRaw - a host-verified, non-redirecting request.
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAdventure clip", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "audio/wav");
        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        LLSD::Binary wav;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            wav = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        }
        // a RIFF/WAVE body over the 44-byte header (wolfspeech.cpp's own test), at most 2 MB
        // (WOLF_MIND_V1.md §8.3 "at most 2 MB")
        if (!st || wav.size() <= 44 || wav.size() > 2 * 1024 * 1024 || memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0)
        {
            if (done) done(st ? std::string("the voice clip was not a sound file") : "the voice clip could not be fetched (" + st.toString() + ")");
            return;
        }
        if (!gAudiop)
        {
            if (done) done("sound is off in this viewer (no audio engine)");
            return;
        }
        sweepClips(false);
        // Source: wolfspeech.cpp onSynthesised - into the sound cache under a fresh UUID as
        // <uuid>.dsf (LLAudioData looks there), then triggerSound as a UI sound.
        const LLUUID id = LLUUID::generateNewID();
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, id.asString()) + ".dsf";
        {
            llofstream out(path, std::ios::binary);
            if (!out.is_open())
            {
                if (done) done("the voice clip could not be written to the sound cache");
                return;
            }
            out.write(reinterpret_cast<const char*>(wav.data()), wav.size());
        }
        // [WOLF ADVENTURE P4 2026-10-16] §5.6 "Character voices" volume
        gAudiop->triggerSound(id, gAgentID, WolfAdventure::voiceVolume(), LLAudioEngine::AUDIO_TYPE_UI);
        // the file stays a minute, well past any line's playback, so the engine never reads a deleted file
        sPlayedClips.push_back({ path, id, LLFrameTimer::getElapsedSeconds() + 60.0 });
        if (done) done(std::string());
    });
}

// Source: HUD_MESSAGES_V1.md §2.2.
void WolfAdventure::applyJournal(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    if (adv.empty()) return;
    Journal j;
    j.mValid = true;
    j.mPages = llclamp(num32(m, "pages", 1), 1, 1000);
    j.mPage = llclamp(num32(m, "page"), 0, j.mPages - 1);
    j.mTitle = str(m, "title", CAP_TITLE);
    j.mPurpose = str(m, "purpose", CAP_LABEL);
    j.mRating = str(m, "rating", CAP_LABEL);
    j.mObjective = str(m, "objective", CAP_OBJECTIVE, true);
    auto each = [](const LLSD& list, size_t max, const std::function<void(const LLSD&)>& fn)
    {
        if (!list.isArray()) return;
        size_t n = 0;
        for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray() && n < max; ++it)
        {
            if (!it->isMap()) continue;
            fn(*it);
            ++n;
        }
    };
    each(m["chapters"], MAX_LIST, [&](const LLSD& c)
    {
        Chapter ch;
        ch.mAct = idOf(c, "act");
        ch.mTitle = str(c, "title", CAP_TITLE);
        ch.mState = str(c, "state", CAP_LABEL);
        if (ch.mState != "done" && ch.mState != "current") ch.mState = "locked";
        j.mChapters.push_back(ch);
    });
    each(m["entries"], MAX_ENTRIES, [&](const LLSD& e)
    {
        Entry en;
        en.mKind = str(e, "kind", CAP_LABEL);
        en.mText = str(e, "text", CAP_ENTRY, true);
        en.mAct = idOf(e, "act");
        en.mAt = num(e, "at");
        if (!en.mText.empty()) j.mEntries.push_back(en);
    });
    // newest first (§3.1 Entries)
    std::stable_sort(j.mEntries.begin(), j.mEntries.end(), [](const Entry& a, const Entry& b) { return a.mAt > b.mAt; });
    each(m["handouts"], MAX_LIST, [&](const LLSD& h)
    {
        Handout ho;
        ho.mId = idOf(h, "id");
        ho.mKind = str(h, "kind", CAP_LABEL);
        ho.mTitle = str(h, "title", CAP_TITLE);
        ho.mTexture = textureOf(h, "texture");
        ho.mText = str(h, "text", CAP_HANDOUT, true);
        j.mHandouts.push_back(ho);
    });
    // §1.3 note: flags[].label 24 (labels), flags[].value 64
    each(m["flags"], MAX_LIST, [&](const LLSD& f) { j.mFlags.push_back({ str(f, "label", CAP_LABEL), str(f, "value", CAP_TITLE) }); });
    each(m["quizzes"], MAX_LIST, [&](const LLSD& q)
    {
        j.mQuizzes.push_back({ idOf(q, "id"), str(q, "title", CAP_TITLE), str(q, "state", CAP_LABEL), str(q, "score", CAP_LABEL) });
    });
    each(m["credits"], MAX_LIST, [&](const LLSD& c) { j.mCredits.push_back({ str(c, "role", CAP_TITLE), str(c, "name", CAP_NAME) }); });
    each(m["attribution"], MAX_LIST, [&](const LLSD& a) { j.mAttribution.push_back({ str(a, "work", CAP_TITLE), str(a, "text", CAP_VIGNETTE, true) }); });   // §1.3 note: 2000
    j.mConversations = num(m, "conversations") != 0;
    // [GAME CARD 2026-10-18] Source: wolfadventure_lib.php wa_journal_card: goal, main ({step, of, title} | null), card ({youAre, goal,
    // win, lose, shape} | null)
    j.mGoal = str(m, "goal", CAP_OBJECTIVE, true);
    {
        const LLSD& mn = m["main"];
        if (mn.isMap())
        {
            j.mMainOf = llclamp(num32(mn, "of"), 0, 1000);
            j.mMainStep = llclamp(num32(mn, "step"), 0, j.mMainOf);
            j.mMainTitle = str(mn, "title", CAP_OBJECTIVE, true);
        }
        const LLSD& cd = m["card"];
        if (cd.isMap())
        {
            j.mHasCard = true;
            j.mYouAre = str(cd, "youAre", CAP_OBJECTIVE, true);
            j.mWin = str(cd, "win", CAP_OBJECTIVE, true);
            j.mLose = str(cd, "lose", CAP_OBJECTIVE, true);
            j.mShape = str(cd, "shape", CAP_LABEL);
            if (j.mGoal.empty()) j.mGoal = str(cd, "goal", CAP_OBJECTIVE, true);
        }
        // Source: GAME_CARD_SPINE_SPEC.md "Change (brain, 2026-10-18, for the viewer agent)": journal `ended` (null while playing)
        if (m["ended"].isMap())
        {
            j.mEndedOn = true;
            j.mEndedKind = str(m["ended"], "kind", CAP_LABEL);
            j.mEndedTitle = str(m["ended"], "title", CAP_TITLE);
            j.mEndedText = str(m["ended"], "text", CAP_VIGNETTE, true);
        }
    }
    adventure(adv).mJournal = j;
}

// Source: HUD_MESSAGES_V1.md §2.3.
void WolfAdventure::applyVignette(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    const std::string id = str(m, "id", CAP_ID);
    if (adv.empty() || id.empty()) return;
    // [WOLF ADVENTURE P2 2026-10-16] a card (an offered choice, narration) answers a waiting talk too
    if (num(m, "close") == 0 && mTalk.mOn && mTalk.mAdv == adv && mTalk.mWaiting)
    {
        mTalk.mWaiting = false;
        talkChanged();
    }
    if (num(m, "close") != 0)
    {
        mCards.erase(std::remove_if(mCards.begin(), mCards.end(), [&](const Card& c) { return c.mAdv == adv && c.mId == id; }), mCards.end());
        return;
    }
    // a card is its adventure's: the same id from another adventure is another card
    Card* existing = findCard(adv, id);
    Card c;
    if (existing)
    {
        // a resend (the tally, §2.3): the player's own state on the card stays
        c.mChosen = existing->mChosen;
        c.mMoreAsked = existing->mMoreAsked;
        c.mWaiting = existing->mWaiting;
        c.mSeq = existing->mSeq;
    }
    else
    {
        c.mSeq = ++mSeq;
    }
    c.mAdv = adv;
    c.mSrc = str(m, "src", CAP_TITLE);
    c.mId = id;
    c.mTitle = str(m, "title", CAP_TITLE);
    c.mText = str(m, "text", CAP_VIGNETTE, true);
    c.mImage = textureOf(m, "image");
    const LLSD& sp = m["speaker"];
    if (sp.isMap())
    {
        c.mSpeaker = str(sp, "name", CAP_NAME);
        c.mPortrait = textureOf(sp, "portrait");
    }
    const LLSD& tally = m["tally"];
    const LLSD& opts = m["options"];
    if (opts.isArray())
    {
        for (LLSD::array_const_iterator it = opts.beginArray(); it != opts.endArray() && c.mOptions.size() < MAX_OPTIONS; ++it)
        {
            if (!it->isMap()) continue;
            Option o;
            o.mId = str(*it, "id", CAP_ID);
            o.mLabel = str(*it, "label", CAP_LABEL);
            o.mDetail = str(*it, "detail", CAP_OBJECTIVE);
            if (o.mId.empty() || o.mLabel.empty()) continue;
            if (tally.isMap() && tally.has(o.mId)) o.mTally = llclamp(num32(tally, o.mId.c_str()), 0, 9999);
            c.mOptions.push_back(o);
        }
    }
    c.mWho = str(m, "who", CAP_LABEL);
    if (c.mWho != "leader" && c.mWho != "vote") c.mWho = "player";
    c.mMayChoose = num(m, "mayChoose", 1) != 0;
    // §1.3 time: `left` is "remaining when sent"; the card ends at arrival + left.
    const F64 left = llclamp(real(m, "left", real(m, "sec")), 0.0, 3600.0);
    c.mSec = (F32)llclamp(real(m, "sec", left), 0.0, 3600.0);
    c.mEndsAt = left > 0.0 ? now() + left : 0.0;
    if (c.mSec < (F32)left) c.mSec = (F32)left;
    if (existing)
    {
        *existing = c;
        return;
    }
    // Bounded: past MAX_CARDS the oldest card without options goes first, else the oldest.
    if (mCards.size() >= MAX_CARDS)
    {
        auto victim = std::find_if(mCards.begin(), mCards.end(), [](const Card& k) { return k.mOptions.empty(); });
        if (victim == mCards.end()) victim = mCards.begin();
        mCards.erase(victim);
    }
    mCards.push_back(c);
}

WolfAdventure::Card* WolfAdventure::findCard(const std::string& adv, const std::string& id)
{
    for (Card& c : mCards)
    {
        if (c.mAdv == adv && c.mId == id) return &c;
    }
    return nullptr;
}

const WolfAdventure::Card* WolfAdventure::topCard() const
{
    // the newest card not put away (§3.3 "oldest underneath")
    for (auto it = mCards.rbegin(); it != mCards.rend(); ++it)
    {
        if (!it->mWaiting) return &*it;
    }
    return nullptr;
}

std::vector<const WolfAdventure::Card*> WolfAdventure::waitingCards() const
{
    std::vector<const Card*> out;
    for (const Card& c : mCards)
    {
        if (!c.mOptions.empty() && c.mChosen.empty() && c.mMayChoose) out.push_back(&c);
    }
    return out;
}

void WolfAdventure::choose(const std::string& adv, const std::string& card, const std::string& option)
{
    Card* c = findCard(adv, card);
    if (!c || !c->mChosen.empty()) return;
    auto o = std::find_if(c->mOptions.begin(), c->mOptions.end(), [&](const Option& x) { return x.mId == option; });
    if (o == c->mOptions.end()) return;
    if (!sendChoose(card, option))
    {
        // [WOLF ADVENTURE AUDIT 2026-10-17] a failure stays until read (NOTICE_DANGER has no expiry; a warning goes after 10 s)
        WolfGame::instance().addNotice(c->mTitle.empty() ? c->mSrc : c->mTitle, "Your choice was not sent: the viewer is not connected to a Wolf Territories region.",
                                       WolfGame::NOTICE_DANGER);
        return;
    }
    // §3.3 "the card shows Chosen: <label> until close"
    c->mChosen = o->mLabel;
    c->mWaiting = false;
    ++mGeneration;
}

// static
bool WolfAdventure::moreTimeAllowed(const Card& c)
{
    // Source: HUD_MESSAGES_V1.md §3.3 - "adds 60 s, up to 10 times ... in a vote, any member may ask once".
    if (c.mEndsAt <= 0.0 || c.mOptions.empty() || !c.mChosen.empty()) return false;
    return c.mMoreAsked < (c.mWho == "vote" ? 1 : 10);
}

bool WolfAdventure::askMoreTime(const std::string& adv, const std::string& card)
{
    Card* c = findCard(adv, card);
    if (!c || !moreTimeAllowed(*c)) return false;
    if (!sendMore(card)) return false;
    ++c->mMoreAsked;
    ++mGeneration;
    return true;
}

void WolfAdventure::putAway(const std::string& adv, const std::string& card)
{
    Card* c = findCard(adv, card);
    if (!c) return;
    // §3.3: a card with no options is dismissed by the player; one with options (unanswered)
    // waits in the Journal's "Waiting for your choice". A chosen card or one the player may not
    // answer waits for its close the same way, out of sight.
    if (c->mOptions.empty())
    {
        const std::string id = c->mId, cadv = c->mAdv;
        mCards.erase(std::remove_if(mCards.begin(), mCards.end(), [&](const Card& k) { return k.mId == id && k.mAdv == cadv; }), mCards.end());
    }
    else
    {
        c->mWaiting = true;
    }
    ++mGeneration;
}

void WolfAdventure::bringBack(const std::string& adv_in, const std::string& card)
{
    Card* c = findCard(adv_in, card);
    if (!c) return;
    c->mWaiting = false;
    // to the top of the stack: the newest place
    Card copy = *c;
    copy.mSeq = ++mSeq;
    const std::string id = copy.mId, adv = copy.mAdv;   // `card` may name an element about to move
    mCards.erase(std::remove_if(mCards.begin(), mCards.end(), [&](const Card& k) { return k.mId == id && k.mAdv == adv; }), mCards.end());
    mCards.push_back(copy);
    ++mGeneration;
}

// Source: HUD_MESSAGES_V1.md §2.5, §3.5.
void WolfAdventure::applyRep(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    if (adv.empty()) return;
    Rep r;
    r.mValid = true;
    r.mHonour = meterOf(m["honour"], "label");
    if (r.mHonour.mOn && r.mHonour.mLabel.empty()) r.mHonour.mLabel = "Honour";
    const LLSD& factions = m["factions"];
    if (factions.isArray())
    {
        for (LLSD::array_const_iterator it = factions.beginArray(); it != factions.endArray() && r.mFactions.size() < MAX_LIST; ++it)
        {
            Meter f = meterOf(*it, "name");
            if (f.mOn && !f.mLabel.empty()) r.mFactions.push_back(f);
        }
    }
    const LLSD& cast = m["cast"];
    if (cast.isArray())
    {
        for (LLSD::array_const_iterator it = cast.beginArray(); it != cast.endArray() && r.mCast.size() < MAX_LIST; ++it)
        {
            if (!it->isMap()) continue;
            Opinion o;
            o.mId = idOf(*it, "id");
            o.mName = str(*it, "name", CAP_NAME);
            o.mValue = llclamp(num32(*it, "v"), -100, 100);
            if (!o.mName.empty()) r.mCast.push_back(o);
        }
    }
    Adventure& a = adventure(adv);
    const Rep before = a.mRep;
    a.mRep = r;
    // §2.5 / §3.5: a non-zero delta is a toast "Honour +5 · Took the badge", success or warning -
    // once: a resend with the value unchanged (a hello's answer) does not toast again.
    auto previous = [&](const Meter& mt) -> const Meter*
    {
        if (!before.mValid) return nullptr;
        if (&mt == &r.mHonour) return before.mHonour.mOn ? &before.mHonour : nullptr;
        for (const Meter& f : before.mFactions) if (f.mId == mt.mId && f.mLabel == mt.mLabel) return &f;
        return nullptr;
    };
    auto toast = [&](const Meter& mt)
    {
        if (!mt.mOn || mt.mDelta == 0) return;
        const Meter* old = previous(mt);
        if (old && old->mValue == mt.mValue) return;
        // "Honour +5" as the notice's title, "Took the badge" under it: the title line is capped at
        // 64, and the reason may be 240 (§1.3 note)
        notice(mt.mLabel + " " + signedText(mt.mDelta), mt.mWhy, mt.mDelta > 0);
    };
    toast(r.mHonour);
    for (const Meter& f : r.mFactions) toast(f);
}

// Source: HUD_MESSAGES_V1.md §2.6.
void WolfAdventure::applyStage(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    const std::string cue = str(m, "cue", CAP_LABEL);
    const F64 t = now();
    const bool has_sec = m.has("sec");
    const F32 sec = (F32)llclamp(real(m, "sec"), 0.0, (F64)CUE_MAX_SEC);
    const bool on = num(m, "on", 1) != 0;
    // Only the adventures this player is in may stage this screen (§2.6 look: "only when the player is
    // in this adventure's session" - held for every cue); `clear` is always taken.
    const Adventure* in = find(adv);
    if (cue != "clear" && (!in || !in->mHasQuest)) return;
    if (cue == "letterbox")
    {
        if (on != mBoxOn)
        {
            // sliding from where the bars are now, so a quick on/off never jumps
            mBoxFrom = letterboxNow();
            mBoxChanged = t;
        }
        mBoxSec = has_sec ? sec : LETTERBOX_SLIDE;
        mBoxOn = on;
        if (on) mBoxSince = t;
    }
    else if (cue == "fade")
    {
        const std::string to = str(m, "to", CAP_LABEL);
        if (to != "black" && to != "clear") return;
        mFadeFrom = fadeNow();
        mFadeTo = to == "black" ? 1.f : 0.f;
        // WCAG 2.3.1: never quicker than 0.33 s, the wolfGame limiter's 334 ms (VIEWER_SPEC.md §3.6).
        mFadeSec = llmax(FADE_MIN_SEC, has_sec ? sec : 1.f);
        mFadeStart = t;
        mBlackSince = 0.0;
    }
    else if (cue == "title")
    {
        mTitleText = str(m, "text", CAP_TITLE);
        mTitleSub = str(m, "sub", CAP_TITLE);
        // [WOLF ADVENTURE AUDIT 2026-10-17] §2.6 `image`: the adventure's title card picture (a texture uuid only, §1.3)
        mTitleImage = m["image"].asString();
        if (mTitleImage.size() != 36 || !LLUUID::validate(mTitleImage) || LLUUID(mTitleImage).isNull()) mTitleImage.clear();
        mTitleSec = has_sec && sec > 0.f ? sec : TITLE_DEFAULT;
        mTitleStart = mTitleText.empty() ? 0.0 : t;
    }
    else if (cue == "look" || cue == "pan")
    {
        LLVector3 pos, from;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || !vec3(m["pos"], pos)) return;
        if (!cameraAllowed(adv)) return;
        const LLVector3d target = rgn->getPosGlobalFromRegion(pos);
        if (cue == "look")
        {
            startCamera(CAM_LOOK, target, LLVector3d(), has_sec ? sec : CUE_DEFAULT_SEC);
        }
        else
        {
            if (!vec3(m["from"], from)) return;
            startCamera(CAM_PAN, target, rgn->getPosGlobalFromRegion(from), llmin(PAN_MAX, has_sec ? sec : CUE_DEFAULT_SEC));
        }
    }
    else if (cue == "music")
    {
        if (!on) stopMusic();
        else if (startMusic(str(m, "url", 1024))) mMusicAdv = adv;
    }
    else if (cue == "clear")
    {
        clearStage();
    }
}

F32 WolfAdventure::letterboxNow() const
{
    const F64 t = now();
    const F32 k = (reduceMotion() || mBoxSec <= 0.f) ? 1.f : (F32)llclamp((t - mBoxChanged) / (F64)mBoxSec, 0.0, 1.0);
    const F32 to = mBoxOn ? 1.f : 0.f;
    return mBoxFrom + (to - mBoxFrom) * k;
}

F32 WolfAdventure::fadeNow() const
{
    if (mFadeSec <= 0.f) return mFadeTo;
    const F32 k = (F32)llclamp((now() - mFadeStart) / (F64)mFadeSec, 0.0, 1.0);
    return mFadeFrom + (mFadeTo - mFadeFrom) * k;
}

bool WolfAdventure::titleOn() const
{
    return mTitleStart > 0.0 && now() - mTitleStart < (F64)mTitleSec;
}

F32 WolfAdventure::titleAlpha() const
{
    if (!titleOn()) return 0.f;
    // half a second in and out - an opacity change, never a slide (§2.6 reduced motion)
    const F64 e = now() - mTitleStart, rest = (F64)mTitleSec - e;
    return (F32)llclamp(llmin(e, rest) / 0.5, 0.0, 1.0);
}

void WolfAdventure::skipTitle()
{
    mTitleStart = 0.0;
    mTitleText.clear();
    mTitleSub.clear();
    mTitleImage.clear();
    ++mGeneration;
}

void WolfAdventure::clearStage()
{
    mBoxOn = false;
    mBoxFrom = 0.f;
    mBoxChanged = -100.0;
    mFadeFrom = mFadeTo = 0.f;
    mFadeSec = 0.f;
    mBlackSince = 0.0;
    mTitleStart = 0.0;
    mTitleText.clear();
    mTitleSub.clear();
    mTitleImage.clear();
    if (mCamMode != CAM_NONE) endCamera();
    // Music is not a stage effect `clear` names (bars, fade, title, camera): it plays on through a
    // clear and a region change, and stops on on:0, on leaving its adventure, on logout and off Wolf
    // (HUD_MESSAGES_V1.md §2.6 note).
    ++mGeneration;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the camera cue (§2.6 look / pan; §3.6 "P1 must read llagentcamera.h for the exact call")
// ═══════════════════════════════════════════════════════════════════════════════════════

// static
bool WolfAdventure::storyCamera()
{
    return gSavedPerAccountSettings.getBOOL("WolfAdventureStoryCamera");
}

// static
bool WolfAdventure::reduceMotion()
{
    return gSavedPerAccountSettings.getBOOL("WolfAdventureReduceMotion");
}

bool WolfAdventure::cameraAllowed(const std::string& adv) const
{
    // Source: HUD_MESSAGES_V1.md §2.6 look: only in this adventure's session, not driving, flying
    // or sailing (no vehicle deck shown: WolfGame::bottomTaken), Story camera on, never within 20 s.
    if (!storyCamera()) return false;
    const Adventure* a = find(adv);
    if (!a || !a->mHasQuest || a->mQuest.mEnded.mOn) return false;
    if (WolfGame::bottomTaken()) return false;
    if (now() - mLastCameraCueAt < CAMERA_REPEAT) return false;
    // Settled in P1: the player's own camera is theirs - not in mouselook, not while they have
    // the camera off the avatar themselves (alt-cam, a focus tool), not mid-teleport.
    if (gAgentCamera.cameraMouselook() || !gAgentCamera.getFocusOnAvatar()) return false;
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) return false;
    return true;
}

void WolfAdventure::startCamera(ECamera mode, const LLVector3d& target, const LLVector3d& from, F32 sec)
{
    // Both cues keep the eye where it is (HUD_MESSAGES_V1.md §2.6 notes): a look turns the view from
    // what it looks at now to the target, a pan sweeps it from `from` to the target.
    mCamMode = mode;
    mCamPos = gAgentCamera.getCameraPositionGlobal();
    mCamFocus = target;
    if (mode == CAM_LOOK)
    {
        mCamFromFocus = gAgentCamera.getFocusGlobal();
        if ((mCamFromFocus - mCamPos).magVecSquared() < 0.01) mCamFromFocus = target;
    }
    else
    {
        mCamFromFocus = from;
    }
    // §2.6 reduced motion: jump straight to the view, hold, give back.
    mCamSec = reduceMotion() ? 0.f : llmax(0.f, sec);
    mCamStart = now();
    mLastCameraCueAt = mCamStart;
    mLastDriveAt = mCamStart;
    // Source: llagentcamera.h setFocusOnAvatar(false, false) unlocks the camera from the avatar
    // without an animation (llagentcamera.cpp:3068-3118); driveCamera then places it each frame.
    gAgentCamera.setFocusOnAvatar(false, false);
    driveCamera(mCamStart);
}

void WolfAdventure::driveCamera(F64 t)
{
    if (mCamMode == CAM_NONE) return;
    // §2.6 "Any mouse or key input ends it at once". LLViewerWindow resets gAwayTriggerTimer on
    // every mouse move and key press (llviewerwindow.cpp:1535-1541 handleMouseMove, :1683-1689
    // handleTranslatedKeyDown; LLAgent::clearAFK resets it too, llagent.cpp:1857-1859). Without
    // input it has run at least as long as since the last frame; shorter means it was reset since
    // (continuous input included, each frame). A held mouse button counts as well.
    const F64 since_last = t - mLastDriveAt;
    mLastDriveAt = t;
    const F32 e = gAwayTriggerTimer.getElapsedTimeF32();
    const bool input = (F64)e + 0.002 < since_last || gViewerWindow->getLeftMouseDown() || gViewerWindow->getRightMouseDown()
                       || gViewerWindow->getMiddleMouseDown();
    const bool stopped = gAgentCamera.cameraMouselook() || WolfGame::bottomTaken() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (input || stopped || t - mCamStart > (F64)mCamSec + CAMERA_HOLD)
    {
        endCamera();
        return;
    }
    F32 k = mCamSec > 0.f ? (F32)llclamp((t - mCamStart) / (F64)mCamSec, 0.0, 1.0) : 1.f;
    k = k * k * (3.f - 2.f * k);   // ease in and out
    const LLVector3d focus = mCamFromFocus + (mCamFocus - mCamFromFocus) * (F64)k;
    // Source: llagentcamera.cpp:2990-3035 setCameraPosAndFocusGlobal sets the focus target and the
    // camera offset at once and starts its own animation when the focus moves; this cue is its own
    // animation, so that one is stopped (llagentcamera.h stopCameraAnimation) and the camera is
    // where this frame says.
    gAgentCamera.setCameraPosAndFocusGlobal(mCamPos, focus, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    mCamLastFocus = focus;
}

void WolfAdventure::endCamera()
{
    const bool ours = mCamMode != CAM_NONE;
    mCamMode = CAM_NONE;
    if (!ours) return;
    // Give the camera back - unless the player has already taken it (an alt-click, a focus tool):
    // then the focus is no longer the one this cue set last, and it is left where they put it.
    // setCameraPosAndFocusGlobal stores the focus it was given as the focus target
    // (llagentcamera.cpp:3003), so that is what is compared.
    const bool still_ours = !gAgentCamera.getFocusOnAvatar() && (gAgentCamera.getFocusTargetGlobal() - mCamLastFocus).magVecSquared() < 0.01;
    // reset_axes false: setFocusOnAvatar's default turns the avatar to the camera's heading in third
    // person (llagentcamera.cpp:3085-3108); a story cue never turns the player's avatar.
    if (still_ours) gAgentCamera.setFocusOnAvatar(true, !reduceMotion(), false);
    ++mGeneration;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// music (§2.6 music)
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventure::startMusic(const std::string& url)
{
    // Source: HUD_MESSAGES_V1.md §2.6 "https, the VIEWER_SPEC.md §3.8 host rules" - the game's own
    // web-page check (WolfGame::validWebUrl).
    std::string host;
    if (!WolfGame::validWebUrl(url, host))
    {
        LL_WARNS("WolfAdventure") << "refused a stage music address that is not https or not a plain public host" << LL_ENDL;
        return false;
    }
    // The player's own switch for streamed music (llviewerparcelmgr.cpp optionallyStartMusic reads
    // the same setting before any parcel stream plays).
    if (!gSavedSettings.getBOOL("AudioStreamingMusic")) return false;
    // Source: wolfradiostations.cpp preview() - the viewer's music stream; its gain is the player's
    // Music volume and mute every frame (llvieweraudio.cpp:506-530 audio_update_volume), which is
    // "the viewer's own media volume and mute" (§2.6). Settled in P1: the cue's `volume` is not a
    // second gain on top of it in WolfViewer.
    LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);
    mMusicUrl = url;
    return true;
}

void WolfAdventure::stopMusic(bool restore)
{
    if (mMusicUrl.empty()) return;
    mMusicUrl.clear();
    mMusicAdv.clear();
    if (!restore)
    {
        // logout: stop it, and start nothing else while the viewer goes
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(LLStringUtil::null);
        return;
    }
    // Give back what the agent's own parcel plays, under the player's music settings - the path
    // wolfradiostations.cpp preview("") takes (LLViewerParcelMgr::optionallyStartMusic).
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    LLViewerRegion* region = gAgent.getRegion();
    if (parcel && region)
    {
        LLViewerParcelMgr::optionallyStartMusic(parcel->getMusicURL(), parcel->getLocalID(), region->getRegionID(), true);
    }
    else
    {
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(LLStringUtil::null);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// per frame, region changes, logout
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventure::idle()
{
    // §5.3 logout: everything goes, once.
    if (gDisconnected || LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent())
    {
        if (!sHeldCalls.empty()) failHeldCalls("You are logging out.");   // [WOLF ADVENTURE GATE 2026-10-17] never sent
        if (mRegionId.notNull() || !mAdventures.empty() || !mCards.empty() || mCamMode != CAM_NONE || !mMusicUrl.empty())
        {
            clearAll(true);
            mRegionId.setNull();
        }
        return;
    }
    // §5.1 Wolf Territories only: off Wolf nothing is kept, sent or shown.
    const bool wolf = WolfGrid::isOnWolfTerritories();
    if ((int)wolf != mOnWolf)
    {
        mOnWolf = wolf ? 1 : 0;
        if (!wolf) clearAll();
        else mHelloAt = now() + HELLO_DELAY;
    }
    if (!wolf) return;
    ensureRegion();   // local only: a new region's leftovers are cleared (onRegionChanged sends nothing)
    const F64 t = now();
    // [WOLF GRID GATE 2026-10-17] the timers below run only on a region that has CONFIRMED it is Wolf
    // (WolfGrid::isOnWolfTerritoriesConfirmed, wolfgrid.h - the predicate for anything that sends or acts): between regions
    // (or on a hypergrid region still answering as the last one) only a held call's time-out is checked, nothing is sent
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (!sHeldCalls.empty() && LLFrameTimer::getElapsedSeconds() - sHeldCalls.front().mAt > API_HOLD_SEC)
            failHeldCalls("The region did not answer in time - try again.");
        return;
    }
    // [review] a move whose next `turns` message never came (dropped mid-crossing): the buttons come back after 20 s
    if (mTurns.mActSending && t - mTurns.mActAt > 20.0) { mTurns.mActSending = false; ++mTurns.mSeq; ++mGeneration; }
    // [WOLF ADVENTURE GATE 2026-10-17] calls held while between Wolf regions go once this region confirms, or are refused
    if (!sHeldCalls.empty())
    {
        if (WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            std::vector<HeldCall> go;
            go.swap(sHeldCalls);
            for (HeldCall& h : go) api(h.mAction, h.mBody, h.mDone);
        }
        else if (LLFrameTimer::getElapsedSeconds() - sHeldCalls.front().mAt > API_HOLD_SEC)
            failHeldCalls("The region did not answer in time - try again.");
    }

    if (mHelloAt > 0.0 && t >= mHelloAt && gAgent.getRegion() && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mHelloAt = 0.0;
        sendHello();
    }

    // §5.5 never stuck: 10 s at black clears the fade; bars with no on:0 within 120 s go.
    const F32 fade = fadeNow();
    if (fade >= 0.999f)
    {
        if (mBlackSince <= 0.0) mBlackSince = t;
        else if (t - mBlackSince >= FADE_BLACK_LIMIT)
        {
            mFadeFrom = mFadeTo = 0.f;
            mFadeSec = 0.f;
            mBlackSince = 0.0;
            ++mGeneration;
        }
    }
    else
    {
        mBlackSince = 0.0;
    }
    if (mBoxOn && t - mBoxSince >= LETTERBOX_LIMIT)
    {
        mBoxFrom = letterboxNow();
        mBoxOn = false;
        mBoxChanged = t;
        ++mGeneration;
    }
    if (mTitleStart > 0.0 && !titleOn()) skipTitle();
    driveCamera(t);
    // [WOLF ADVENTURE P2 2026-10-16] a talk the region never answered says so (never silent): the
    // first click after TALK_WAIT_LIMIT, an AI line after the brain's own 12 s timeout plus the
    // hops (WOLF_MIND_V1.md §3.6) - then the written fallback should already have come.
    if (mTalk.mOn && mTalk.mError.empty() && (mTalk.mWaiting || mTalk.mTyping)
        && t - mTalk.mSince > (mTalk.mTyping ? 20.0 : TALK_WAIT_LIMIT))
    {
        mTalk.mWaiting = mTalk.mTyping = false;
        mTalk.mError = mTalk.mName.empty() ? std::string("No answer came. The story may be paused - try again in a moment.")
                                           : mTalk.mName + " did not answer. The story may be paused - try again in a moment.";
        talkChanged();
    }
    WolfAdventureUI::syncCards();
    WolfAdventureUI::syncTalk();
    // [WOLF ADVENTURE P4 2026-10-16] §3.9-3.11: the roll prompt / dice card, the turn tracker, the quiz.
    if (mDice.mOn && t - mDice.mShownAt > 4.0) diceDone();
    if (mRoll.mOn && mRoll.mEndsAt > 0.0 && t > mRoll.mEndsAt + 30.0) { mRoll = Roll(); ++mGeneration; }   // a lost roll never stays up
    WolfAdventureUI::syncPlay();
}

void WolfAdventure::ensureRegion()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLUUID id = rgn ? rgn->getRegionID() : LLUUID::null;
    if (id == mRegionId) return;
    mRegionId = id;
    onRegionChanged();
}

void WolfAdventure::onRegionChanged()
{
    // Source: HUD_MESSAGES_V1.md §5.2. Quests, journals and reputation stay (adventures span
    // regions); the talk panel closes (P2); cards with options stay, the others go; the stage
    // clears; rolls, turns and gm overlays clear (P4); hello 3 s after arrival.
    mCards.erase(std::remove_if(mCards.begin(), mCards.end(), [](const Card& c) { return c.mOptions.empty(); }), mCards.end());
    // [WOLF ADVENTURE P2 2026-10-16] §5.2 "Close the talk panel." (the old region's hands already
    // ended the talk when the player left - REGION_HANDS_V1.md §7 20 m rule - so nothing is sent)
    mTalk = Talk();
    ++mTalk.mSeq;
    clearStage();
    // [WOLF ADVENTURE P4 2026-10-16] §5.2 "Clear rolls and the turn tracker. Clear gm overlays."
    mRoll = Roll();
    mDice = Dice();
    mTurns = Turns();
    mGm.clear();
    mHelloAt = mRegionId.notNull() ? now() + HELLO_DELAY : 0.0;
    ++mHereSerial;   // a list on its way is for the old region
    mHereLoading = false;
    mHereError.clear();   // the Journal asks again for the new region
    ++mGeneration;
}

void WolfAdventure::clearAll(bool logout)
{
    // [WOLF ADVENTURE GATE 2026-10-17] calls held for a region to confirm Wolf are refused, never sent (off Wolf / logout)
    failHeldCalls(logout ? "You are logging out." : "Adventures work on Wolf Territories only.");
    mAdventures.clear();
    mCards.clear();
    mCurrent.clear();
    mTalk = Talk();
    ++mTalk.mSeq;
    mRoll = Roll();   // [WOLF ADVENTURE P4 2026-10-16/] §5.3 everything
    mDice = Dice();
    mTurns = Turns();
    mQuiz = Quiz();
    mGm.clear();
    if (logout) sweepClips(true);
    clearStage();
    stopMusic(!logout);
    mHere = LLSD();
    mHereRegion.setNull();
    mHereError.clear();
    mHereLoading = false;
    ++mHereSerial;
    mHelloAt = 0.0;
    WolfAdventureUI::closeWindows();
    ++mGeneration;
}

bool WolfAdventure::skipStage()
{
    if (!(titleOn() || mBoxOn || fadeNow() > 0.f || mCamMode != CAM_NONE)) return false;
    // §6 2.2.2: titles, letterbox and camera cues can be skipped with Escape. Music stays: it has
    // its own stop (the viewer's music control) and is not touched here.
    mBoxOn = false;
    mBoxFrom = 0.f;
    mBoxChanged = -100.0;
    mFadeFrom = mFadeTo = 0.f;
    mFadeSec = 0.f;
    skipTitle();
    if (mCamMode != CAM_NONE) endCamera();
    ++mGeneration;
    return true;
}

bool WolfAdventure::handleEscape(EEscape step)
{
    if (!WolfGrid::isOnWolfTerritories()) return false;
    switch (step)
    {
    case ESC_FOCUSED_CARD:
        // [WOLF ADVENTURE P2 2026-10-16] then the talk panel when it has the keyboard (§3.2:
        // "Escape closes the panel, when no text is in the box"; with text, it empties the box).
        return WolfAdventureUI::escapeFocusedCard() || WolfAdventureUI::escapeFocusedTalk();
    case ESC_STAGE_FIRST:
        // §5.5 "the stage effects (never blocked)": not even by a text field having the keyboard -
        // but a card on screen, then the talk panel, come first in that order, so then this waits.
        return !topCard() && !mTalk.mOn && skipStage();
    case ESC_CARD_THEN_STAGE:
    default:
        if (const Card* c = topCard())
        {
            putAway(c->mAdv, c->mId);
            return true;
        }
        // §5.5: a vignette, then the talk panel, then the stage effects
        if (mTalk.mOn)
        {
            talkClose();
            return true;
        }
        return skipStage();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// sending (§4)
// ═══════════════════════════════════════════════════════════════════════════════════════

// static
std::string WolfAdventure::cutUtf8(const std::string& text, size_t max_bytes)
{
    if (text.size() <= max_bytes) return text;
    size_t n = max_bytes;
    // never end inside a character: step back over continuation bytes (10xxxxxx)
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;
    return text.substr(0, n);
}

bool WolfAdventure::send(const std::vector<std::string>& params)
{
    if (!gAgent.getRegion()) return false;
    // §1.2: gated exactly as WolfGame::send - only to a region that has itself said it is Wolf.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return false;
    size_t total = 0;
    for (const std::string& p : params)
    {
        if (p.size() > PARAM_BYTES) return false;   // every caller cuts first; never a cut-off packet
        total += p.size() + 1;                      // + its NUL (LLTemplateMessageBuilder::addString)
    }
    if (total > VERB_BYTES) return false;
    // Source: llviewergenericmessage.cpp:43-70 send_generic_message - one reliable packet.
    send_generic_message(METHOD_TO_SIM, params);
    return true;
}

bool WolfAdventure::sendWithText(std::vector<std::string> params, const std::string& text_in)
{
    // §1.2: text in consecutive parameters of at most 240 bytes, never splitting a UTF-8
    // character, at most 720 bytes in all; the hands join them in order.
    const std::string text = cutUtf8(text_in, MAX_TEXT_BYTES);
    if (text.empty()) return false;
    size_t at = 0;
    while (at < text.size() && params.size() < 64)
    {
        const std::string piece = cutUtf8(text.substr(at), PARAM_BYTES);
        if (piece.empty()) break;
        params.push_back(piece);
        at += piece.size();
    }
    return send(params);
}

bool WolfAdventure::sendHello() { return send({ "hello" }); }
bool WolfAdventure::sendTalk(const LLUUID& npc) { return send({ "talk", npc.asString() }); }
bool WolfAdventure::sendReply(const LLUUID& npc, const std::string& reply_id) { return send({ "reply", npc.asString(), cutUtf8(reply_id, CAP_ID) }); }
bool WolfAdventure::sendSay(const LLUUID& npc, const std::string& text) { return sendWithText({ "say", npc.asString() }, text); }
bool WolfAdventure::sendTalkEnd(const LLUUID& npc) { return send({ "talkend", npc.asString() }); }
bool WolfAdventure::sendGive(const LLUUID& npc, const std::string& item, S32 qty)
{
    return send({ "give", npc.asString(), cutUtf8(item, CAP_ID), std::to_string(llmax(1, qty)) });
}
bool WolfAdventure::sendChoose(const std::string& card, const std::string& option) { return send({ "choose", cutUtf8(card, CAP_ID), cutUtf8(option, CAP_ID) }); }
bool WolfAdventure::sendMore(const std::string& card) { return send({ "more", cutUtf8(card, CAP_ID) }); }
bool WolfAdventure::sendJournalPage(const std::string& adv, S32 page) { return send({ "journal", cutUtf8(adv, CAP_ID), std::to_string(llmax(0, page)) }); }
bool WolfAdventure::sendReady(const std::string& adv, bool ready) { return send({ "ready", cutUtf8(adv, CAP_ID), ready ? "1" : "0" }); }
bool WolfAdventure::sendRoll(const std::string& roll) { return send({ "roll", cutUtf8(roll, CAP_ID) }); }
bool WolfAdventure::sendEndTurn(const std::string& adv) { return send({ "endturn", cutUtf8(adv, CAP_ID) }); }
bool WolfAdventure::sendCallGm(const std::string& adv, const std::string& text) { return sendWithText({ "callgm", cutUtf8(adv, CAP_ID) }, text); }
bool WolfAdventure::sendGmSay(const LLUUID& npc, const std::string& text) { return sendWithText({ "gm_say", npc.asString() }, text); }
bool WolfAdventure::sendGmMove(const LLUUID& npc, const LLVector3& pos, const std::string& gait)
{
    return send({ "gm_move", npc.asString(), llformat("%.2f", pos.mV[VX]), llformat("%.2f", pos.mV[VY]), llformat("%.2f", pos.mV[VZ]),
                  cutUtf8(gait, CAP_LABEL) });
}
bool WolfAdventure::sendGmAnim(const LLUUID& npc, const std::string& anim, bool on)
{
    return send({ "gm_anim", npc.asString(), cutUtf8(anim, CAP_ID), on ? "1" : "0" });
}

void WolfAdventure::notice(const std::string& title, const std::string& text, bool good)
{
    WolfGame::instance().addNotice(title, text, good ? WolfGame::NOTICE_SUCCESS : WolfGame::NOTICE_WARNING);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// markers
// ═══════════════════════════════════════════════════════════════════════════════════════

// static
bool WolfAdventure::markerGlobal(const Quest& q, LLVector3d& out)
{
    if (!q.mMarker.mOn) return false;
    // §2.1: the marker is also sent as a wolfGame `marker` of the same id, whose region lookup
    // (WolfGame::resolveMarkers) already finds where it is.
    const auto& markers = WolfGame::instance().markers();
    auto it = markers.find(q.mMarker.mId);
    if (it != markers.end() && WolfGame::markerGlobal(it->second, out)) return true;
    // Before that one arrives: a marker on the agent's own region is placed directly.
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && LLStringUtil::compareInsensitive(rgn->getName(), q.mMarker.mRegion) == 0)
    {
        out = rgn->getPosGlobalFromRegion(q.mMarker.mPos);
        return true;
    }
    return false;
}

// static
F32 WolfAdventure::markerDistance(const Quest& q)
{
    LLVector3d g;
    if (!markerGlobal(q, g)) return -1.f;
    return (F32)(g - gAgent.getPositionGlobal()).magVec();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the grid's adventure service
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: BRAIN_API_V1.md §1 - php/wolfadventure_api.php on the WolfStorm web hosts, for
    // viewers (X-Wolf-Agent + X-Wolf-Session); the host is the one every WolfViewer call to a
    // wolfstorm/php service uses (wolfcave.cpp WolfCaves::API_URL).
    const char* const API_URL = "https://wolfstorm.app/php/wolfadventure_api.php";

    /** The words for a refused call when the body did not come through (the raw adapter drops a
     *  4xx body, wolfcave.cpp caveServiceError): the statuses of BRAIN_API_V1.md §3. */
    std::string statusWords(S32 status, const std::string& transport)
    {
        switch (status)
        {
            case 400: return "the adventure service could not read the request - update WolfViewer and try again";
            case 401: return "the grid could not confirm you are signed in - log out and back in, then try again";
            case 403: return "the adventure service refused it: you do not have the rights for this (if you do, your session may have expired - log out and back in)";
            case 404: return "the adventure service does not know that, or is not available on this grid yet";
            case 409: return "it was changed in another window - reload it and try again";
            case 413: return "that is too large for the adventure service";
            case 429: return "too many requests - wait a moment and try again";
            case 503: return "the adventure service is busy or down - try again in a moment";
            default:  return transport.empty() ? llformat("the adventure service answered HTTP %d", status) : transport;
        }
    }
}

namespace
{
    // [WOLF AI CREDITS 2026-10-24] Paul: "use credits has to update the viewer". Every WolfAvatars, Adventure Studio and
    // import call comes through api(), so its replies tell the status bar's "AI n" when credits were spent.
    // Source: wolfmind_lib.php wm_job_out (:3607) and the jobs list (:3902) - {job, cost: {credits}} per job, the cost
    // so far, sent again on every poll; was_cost (wolfadventure_story_lib.php:1184) and wm_suggest (:2188) - cost {credits}
    // of that one call. A job counts only when its cost has grown since the last reply that showed it.
    std::map<std::string, F64> sSeenJobCost;
    const size_t SEEN_JOB_COST_MAX = 500;   // a long session's jobs; past it the map starts again (one extra ask at most)

    bool cost_grew(const LLSD& item)
    {
        if (!item.isMap() || !item["cost"].isMap()) return false;
        const F64 credits = item["cost"]["credits"].asReal();
        if (!item["job"].isString() || item["job"].asString().empty()) return credits > 0.0;
        if (sSeenJobCost.size() >= SEEN_JOB_COST_MAX) sSeenJobCost.clear();
        F64& last = sSeenJobCost[item["job"].asString()];
        const bool grew = credits > last;
        last = credits;
        return grew;
    }

    void note_credit_use(const LLSD& reply)
    {
        bool used = cost_grew(reply);
        for (const LLSD& j : llsd::inArray(reply["jobs"])) used = cost_grew(j) || used;
        if (used) WolfAI::instance().creditsUsed();
    }
}

// static
void WolfAdventure::api(const std::string& action, const LLSD& body, api_callback_t done)
{
    // §5.1 Wolf Territories only: the service needs a live Wolf presence (wt_require_presence);
    // off Wolf the session id is never sent anywhere.
    // [WOLF ADVENTURE GATE 2026-10-17] Paul: "Things that only work on Wolf don't work anywhere else, or try to." A request
    // carries the session pair, so it goes only while the region underfoot has itself said it is Wolf
    // (WolfGrid::isOnWolfTerritoriesConfirmed, wolfgrid.h - the predicate for SENDING, as send() and WolfBorder::receive).
    // Not Wolf at all (isOnWolfTerritories false): refused at once, nothing sent. Still on Wolf but between regions (the
    // new region's features not in yet): held, and sent from idle() once the region confirms - or refused if it turns out
    // not to be Wolf (clearAll) or after API_HOLD_SEC.
    if (!WolfGrid::isOnWolfTerritories())
    {
        if (done) done(false, LLSD(), "Adventures work on Wolf Territories only.");
        return;
    }
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (sHeldCalls.size() >= API_HOLD_MAX)
        {
            if (done) done(false, LLSD(), "Moving between regions - try again in a moment.");
            return;
        }
        sHeldCalls.push_back({ action, body, done, LLFrameTimer::getElapsedSeconds() });
        return;
    }
    // [review] calls held while between regions go first, in the order they were made (a newer call never overtakes one
    // still held: a publish after a held save_draft)
    if (!sHeldCalls.empty())
    {
        std::vector<HeldCall> go;
        go.swap(sHeldCalls);
        for (HeldCall& h : go) api(h.mAction, h.mBody, h.mDone);
    }
    LLSD req = body.isMap() ? body : LLSD::emptyMap();
    req["action"] = action;
    // A pack travels as its own JSON text ("pack_json"), never through LLSD: LLSD integers are
    // 32-bit and a round trip would change numbers the brain validates (BRAIN_API_V1.md §5.1 packs).
    std::string pack_json;
    if (req.has("pack_json"))
    {
        pack_json = req["pack_json"].asString();
        req.erase("pack_json");
    }
    boost::json::value jv = LlsdToJson(req);
    if (!pack_json.empty())
    {
        boost::system::error_code pec;
        boost::json::value pv = boost::json::parse(pack_json, pec);
        if (pec || !pv.is_object())
        {
            if (done) done(false, LLSD(), "the pack is not a JSON object");
            return;
        }
        jv.as_object()["pack"] = std::move(pv);
    }
    const std::string text = boost::json::serialize(jv);
    // [WOLF STORY FIRST 2026-10-17] the helper's turns wait for the AI: the brain allows up to 600 s for a drafting turn and
    // 90 s for an edit (wolfadventure_story_lib.php:52-53), job_apply merges a whole pack; every other call keeps 60 s
    // [GAME CARD 2026-10-18] import_start carries the whole file (at most 20 MB, as base64): 300 s for the upload
    // [WOLF PETS 2026-10-24] wolfpet_make waits for wolfpet.py: WPET_WAIT 120 s (wolfstorm/php/wolfpet_lib.php:32), so 150 s here
    // [WOLF 3D TEXT 2026-10-26] text_make / text_preview build the sign on the server: 120 s (wolf-text3d/PROTOCOL.md "Transport"), 150 s here as wolfpet_make
    const S32 timeout_sec = action == "helper_chat" ? 660 : action == "import_start" ? 300 : (action == "wolfpet_make" || action == "text_make" || action == "text_preview") ? 150 : (action == "job_apply" || action == "helper_undo"
                                                                      || action == "update_preview" || action == "update_live" || action == "update_undo") ? 120 : 60;   // [M7] the preview may ask the helper (WAL_HELPER_TIMEOUT 45 s)
    LLCoros::instance().launch("WolfAdventure " + action, [text, done, timeout_sec]()
    {
        // Source: wolfcave.cpp wolfRequest - the session pair on a host-verified, non-redirecting
        // request (wolfgrid.h makeVerifiedHttpOptions), and no automatic retry of a POST.
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAdventure", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(timeout_sec);
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(text.data(), text.size());
        LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
        LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
            if (!ec)
            {
                reply = LlsdFromJson(v);
                // the pack as its own exact JSON text as well (see pack_json above)
                if (v.is_object() && v.as_object().contains("pack") && v.as_object().at("pack").is_object())
                {
                    reply["pack_json"] = boost::json::serialize(v.as_object().at("pack"));
                }
                // [WOLF ADVENTURE P2 2026-10-16] a drafting job's result is a pack too (job {job} output)
                if (v.is_object() && v.as_object().contains("output") && v.as_object().at("output").is_object())
                {
                    reply["output_json"] = boost::json::serialize(v.as_object().at("output"));
                }
            }
        }
        // [WOLF ADVENTURE P3 2026-10-16] a refusal with a 4xx status (e.g. save_draft's 409 {error, etag}) carries its
        // JSON in the error body (llcorehttputil.cpp:321 httpStatus["error_body"]), not in the raw result
        if (!reply.isMap() && http.has("error_body"))
        {
            boost::system::error_code eec;
            boost::json::value ev = boost::json::parse(http["error_body"].asString(), eec);
            if (!eec && ev.is_object()) reply = LlsdFromJson(ev);
        }
        const S32 status = st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
        std::string error;
        bool ok = false;
        if (status == 200 && reply.isMap())
        {
            // BRAIN_API_V1.md §3: {"success": true, ...} or {"success": false, "error": "..."}.
            ok = reply["success"].asBoolean();
            if (!ok)
            {
                error = WolfGame::cleanText(reply["error"].asString(), 500);
                if (error.empty()) error = "the adventure service refused it without saying why";
            }
        }
        else if (reply.isMap() && reply.has("error"))
        {
            error = WolfGame::cleanText(reply["error"].asString(), 500);
        }
        else
        {
            error = statusWords(status, st ? std::string() : st.toString());
        }
        if (ok) note_credit_use(reply);   // [WOLF AI CREDITS 2026-10-24/] before done(): it may close the caller
        if (done) done(ok, reply, error);
    });
}

bool WolfAdventure::hereCurrent() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn && rgn->getRegionID() == mHereRegion;
}

void WolfAdventure::refreshHere()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const LLUUID region = rgn->getRegionID();
    const U32 serial = ++mHereSerial;
    mHereLoading = true;
    mHereError.clear();
    ++mGeneration;
    LLSD body;
    body["region"] = region.asString();
    // Source: BRAIN_API_V1.md §10.1 adventures_here {region}.
    api("adventures_here", body, [serial, region](bool ok, const LLSD& reply, const std::string& error)
    {
        if (!WolfAdventure::instanceExists()) return;
        WolfAdventure& self = WolfAdventure::instance();
        if (serial != self.mHereSerial) return;   // a newer list was asked for
        self.mHereLoading = false;
        self.mHereRegion = region;
        if (ok)
        {
            self.mHere = reply["adventures"].isArray() ? reply["adventures"] : LLSD::emptyArray();
            self.mHereError.clear();
        }
        else
        {
            self.mHere = LLSD::emptyArray();
            self.mHereError = error;
        }
        ++self.mGeneration;
    });
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [WOLF ADVENTURE P4 2026-10-16] rolls and dice (§2.9), turns (§2.10), quizzes (§2.11), gm layers
// (§2.7), and the §5.6 settings "Character voices" and "Show AI tag"
// ═══════════════════════════════════════════════════════════════════════════════════════

// static
F32 WolfAdventure::voiceVolume()
{
    return llclamp(gSavedPerAccountSettings.getF32("WolfAdventureVoiceVolume"), 0.f, 1.f);
}

bool WolfAdventure::aiTagShown(const std::string& adv) const
{
    if (gSavedPerAccountSettings.getBOOL("WolfAdventureShowAiTag")) return true;
    // Source: HUD_MESSAGES_V1.md §5.6 "Show AI tag (default on, cannot be hidden in classes)". The brain's
    // `quest` message carries class:1 in a class session (lead, P4); patches merge into mQuestRaw. Unknown
    // (no quest yet) counts as a class: shown.
    const Adventure* a = find(adv);
    if (!a || !a->mHasQuest) return true;
    return num(a->mQuestRaw, "class") != 0;
}

// static
std::string WolfAdventure::agentName(const LLUUID& id)
{
    if (id.isNull()) return std::string();
    LLAvatarName av;
    if (LLAvatarNameCache::get(id, &av)) return av.getCompleteName();
    LLAvatarNameCache::get(id, [](const LLUUID&, const LLAvatarName&) { if (WolfAdventure::instanceExists()) ++WolfAdventure::instance().mGeneration; });
    return std::string();
}

// Source: HUD_MESSAGES_V1.md §2.9 roll (`advantage`, settled in P1).
void WolfAdventure::applyRoll(const LLSD& m)
{
    const std::string id = idOf(m, "id");
    if (id.empty()) return;
    Roll r;
    r.mOn = true;
    r.mAdv = idOf(m, "adv");
    r.mSrc = str(m, "src", CAP_TITLE);
    r.mId = id;
    r.mLabel = str(m, "label", CAP_TITLE);
    r.mCheck = idOf(m, "check");
    r.mDice = str(m, "dice", 16);
    r.mAdvantage = llclamp(num32(m, "advantage"), -1, 1);
    r.mHasDc = m.has("dc") && (m["dc"].isInteger() || m["dc"].isReal());
    r.mDc = num32(m, "dc");
    r.mPublic = num(m, "public", 1) != 0;
    r.mSec = (F32)llclamp(real(m, "sec"), 0.0, 3600.0);
    const F64 left = llclamp(real(m, "left", r.mSec), 0.0, 3600.0);
    r.mEndsAt = left > 0.0 ? now() + left : 0.0;
    const std::string who = str(m, "who", 36);
    if (who.size() == 36 && LLUUID::validate(who)) r.mWho = LLUUID(who);
    if (mRoll.mOn && mRoll.mId == id) r.mSent = mRoll.mSent;   // the same request again keeps the press
    r.mSeq = mRoll.mSeq + 1;
    mRoll = r;
}

// Source: HUD_MESSAGES_V1.md §2.9 dice; §3.9 "a card in the same place for 4 s".
void WolfAdventure::applyDice(const LLSD& m)
{
    Dice d;
    d.mOn = true;
    d.mAdv = idOf(m, "adv");
    d.mSrc = str(m, "src", CAP_TITLE);
    d.mId = idOf(m, "id");
    d.mLabel = str(m, "label", CAP_TITLE);
    const LLSD& by = m["by"];
    if (by.isMap())
    {
        d.mByName = str(by, "name", CAP_NAME);
        const std::string a = str(by, "agent", 36);
        if (a.size() == 36 && LLUUID::validate(a)) d.mBy = LLUUID(a);
    }
    const LLSD& dice = m["dice"];
    for (size_t i = 0; dice.isArray() && i < (size_t)dice.size() && d.mDice.size() < 20; ++i)
    {
        const LLSD& x = dice[(LLSD::Integer)i];
        if (!x.isMap()) continue;
        Die die;
        die.mSides = llclamp(num32(x, "s"), 2, 1000);
        die.mValue = llclamp(num32(x, "v"), 0, 1000);
        die.mKept = num(x, "kept", 1) != 0;
        d.mDice.push_back(die);
    }
    d.mMod = num32(m, "mod");
    d.mTotal = num32(m, "total");
    d.mHasDc = m.has("dc") && (m["dc"].isInteger() || m["dc"].isReal());
    d.mDc = num32(m, "dc");
    d.mOutcome = idOf(m, "outcome");
    d.mPublic = num(m, "public", 1) != 0;
    d.mNote = str(m, "note", CAP_OBJECTIVE);
    d.mShownAt = now();
    d.mSeq = mDice.mSeq + 1;
    mDice = d;
    if (mRoll.mOn && mRoll.mId == d.mId) mRoll = Roll();   // the request is answered
    // §3.9 "every result also goes to the Journal": the brain logs it there; the Journal's next page read shows it.
}

// Source: HUD_MESSAGES_V1.md §2.10.
void WolfAdventure::applyTurns(const LLSD& m)
{
    if (num(m, "end") != 0)
    {
        // [M4 review] the fight's closing line (wab_stop: who won, or why it ended) is read, not lost with the panel
        const std::string last = str(m, "line", CAP_OBJECTIVE, true);
        if (!last.empty() && !mTurns.mLabel.empty()) WolfGame::instance().addNotice(mTurns.mLabel, last, WolfGame::NOTICE_INFO);
        mTurns = Turns();
        return;
    }
    Turns t;
    t.mOn = true;
    t.mAdv = idOf(m, "adv");
    t.mSrc = str(m, "src", CAP_TITLE);
    t.mRound = num32(m, "round");
    t.mCurrent = idOf(m, "current");
    t.mSec = (F32)llclamp(real(m, "sec"), 0.0, 600.0);   // HUD §6 2.2.1: turn time at most 600 s
    const F64 left = llclamp(real(m, "left", t.mSec), 0.0, 600.0);
    t.mEndsAt = left > 0.0 ? now() + left : 0.0;
    const LLSD& order = m["order"];
    for (size_t i = 0; order.isArray() && i < (size_t)order.size() && t.mOrder.size() < 24; ++i)
    {
        const LLSD& x = order[(LLSD::Integer)i];
        if (!x.isMap()) continue;
        TurnRow row;
        row.mKey = idOf(x, "key");
        row.mName = str(x, "name", CAP_NAME);
        row.mInit = num32(x, "init");
        row.mYou = num(x, "you") != 0;
        row.mSide = idOf(x, "side");
        const LLSD& hp = x["hp"];
        if (hp.isArray() && hp.size() >= 2)
        {
            row.mHasHp = true;
            row.mHp = hp[0].asInteger();
            row.mHpMax = llmax(1, hp[1].asInteger());
        }
        row.mState = idOf(x, "state");
        const LLSD& cond = x["cond"];
        for (size_t k = 0; cond.isArray() && k < (size_t)cond.size() && row.mCond.size() < 8; ++k)
        {
            if (cond[(LLSD::Integer)k].isString()) row.mCond.push_back(WolfGame::cleanText(cond[(LLSD::Integer)k].asString(), CAP_LABEL));
        }
        t.mOrder.push_back(row);
    }
    // [WOLF STORY FIRST M4 2026-10-17] the battle's fields (wolfadventure_combat_lib.php wab_show)
    t.mLabel = str(m, "label", CAP_TITLE);
    t.mLine = str(m, "line", CAP_OBJECTIVE, true);
    t.mDraughts = llclamp(num32(m, "draughts"), 0, 99);
    for (const LLSD& a : llsd::inArray(m["actions"]))
        if (a.isString() && (a.asString() == "attack" || a.asString() == "defend" || a.asString() == "item" || a.asString() == "flee")) t.mActions.push_back(a.asString());
    for (const LLSD& x : llsd::inArray(m["targets"]))
    {
        if (!x.isMap() || t.mTargets.size() >= 24) continue;
        TurnTarget tg;
        tg.mKey = idOf(x, "key");
        tg.mName = str(x, "name", CAP_NAME);
        if (x["hp"].isArray() && x["hp"].size() >= 2) { tg.mHp = x["hp"][0].asInteger(); tg.mHpMax = llmax(1, x["hp"][1].asInteger()); }
        if (!tg.mKey.empty()) t.mTargets.push_back(tg);
    }
    t.mSeq = mTurns.mSeq + 1;
    mTurns = t;
}

void WolfAdventure::combatAct(const std::string& move, const std::string& target)
{
    if (!mTurns.mOn || mTurns.mActSending || mTurns.mAdv.empty()) return;
    LLSD body;
    body["adventure"] = mTurns.mAdv;
    body["move"] = move;
    if (!target.empty()) body["target"] = target;
    mTurns.mActSending = true;
    mTurns.mActAt = now();
    ++mGeneration;
    const U32 seq = mTurns.mSeq;   // the turn this move is for
    // wolfadventure_combat_lib.php wa_api_combat_act {adventure, move, target?} -> {done, on, line}; 409 when not your turn
    api("combat_act", body, [seq](bool ok, const LLSD& reply, const std::string& error)
    {
        if (!WolfAdventure::instanceExists()) return;
        WolfAdventure& self = WolfAdventure::instance();
        // [review] a newer `turns` message came first: it is the truth (its own line, its own buttons); only a refusal is said
        if (self.mTurns.mSeq != seq)
        {
            if (!ok) WolfGame::instance().addNotice("Fight", "Your move was not made: " + error, WolfGame::NOTICE_DANGER);
            return;
        }
        // [M4 review] a move that was made keeps the buttons off (mActSending) until the brain's next `turns` message,
        // which applyTurns writes as a fresh Turns (mActSending false); a refused move frees them at once
        if (ok) self.mTurns.mLine = WolfGame::cleanText(reply["line"].asString(), CAP_OBJECTIVE, true);
        else
        {
            self.mTurns.mActSending = false;
            WolfGame::instance().addNotice("Fight", "Your move was not made: " + error, WolfGame::NOTICE_DANGER);
        }
        ++self.mTurns.mSeq;
        ++self.mGeneration;
    });
}

// Source: HUD_MESSAGES_V1.md §2.11, §3.11.
void WolfAdventure::applyQuiz(const LLSD& m)
{
    const std::string id = idOf(m, "id");
    if (id.empty()) return;
    const std::string adv = idOf(m, "adv");
    const bool same = mQuiz.mOn && mQuiz.mId == id && mQuiz.mAdv == adv;
    Quiz q = same ? mQuiz : Quiz();
    q.mOn = true;
    q.mAdv = adv;
    q.mSrc = str(m, "src", CAP_TITLE);
    q.mId = id;
    q.mTitle = str(m, "title", CAP_TITLE);
    const LLSD& items = m["items"];
    if (items.isArray())
    {
        q.mItems.clear();
        for (size_t i = 0; i < (size_t)items.size() && q.mItems.size() < 20; ++i)   // ADVENTURE_PACK §9: 1 to 20 items
        {
            const LLSD& x = items[(LLSD::Integer)i];
            if (!x.isMap()) continue;
            QuizItem it;
            it.mId = idOf(x, "id");
            it.mType = idOf(x, "type");
            if (it.mType != "choice" && it.mType != "text" && it.mType != "essay") it.mType = "text";
            it.mPrompt = str(x, "prompt", CAP_VIGNETTE, true);
            it.mMax = llclamp(num32(x, "max", 1), 1, 6);
            const LLSD& ch = x["choices"];
            for (size_t k = 0; ch.isArray() && k < (size_t)ch.size() && it.mChoices.size() < 6; ++k)
            {
                const LLSD& c = ch[(LLSD::Integer)k];
                if (c.isMap()) it.mChoices.push_back({ idOf(c, "id"), str(c, "label", CAP_OBJECTIVE) });
            }
            if (!it.mId.empty()) q.mItems.push_back(it);
        }
    }
    const LLSD& res = m["result"];
    if (res.isMap())
    {
        q.mHasResult = true;
        q.mScore = num32(res, "score");
        q.mScoreMax = num32(res, "max");
        q.mPassed = num(res, "passed") != 0;
        q.mMarked.clear();
        const LLSD& mk = res["marked"];
        for (size_t k = 0; mk.isArray() && k < (size_t)mk.size(); ++k)
        {
            const LLSD& x = mk[(LLSD::Integer)k];
            if (x.isMap()) q.mMarked[idOf(x, "id")] = (S32)num(x, "correct");
        }
        q.mSending = false;
    }
    q.mIndex = llclamp(q.mIndex, 0, llmax(0, (S32)q.mItems.size() - 1));
    q.mSeq = mQuiz.mSeq + 1;
    mQuiz = q;
}

// Source: HUD_MESSAGES_V1.md §2.7 (sent only to agents with GM rights; drawn only while the GM Console is open).
void WolfAdventure::applyGm(const LLSD& m)
{
    const std::string adv = idOf(m, "adv");
    if (adv.empty()) return;
    const std::string layer = idOf(m, "layer");
    if (layer == "clear")
    {
        mGm.erase(adv);
        return;
    }
    // Bounded as the adventures are (MAX_ADVENTURES): a new one pushes out the one updated longest ago.
    if (mGm.find(adv) == mGm.end() && mGm.size() >= MAX_ADVENTURES)
    {
        auto oldest = mGm.begin();
        for (auto o = mGm.begin(); o != mGm.end(); ++o)
        {
            if (o->second.mAt < oldest->second.mAt) oldest = o;
        }
        mGm.erase(oldest);
    }
    GmLayers& g = mGm[adv];
    g.mAt = now();
    g.mAdv = adv;
    g.mSession = idOf(m, "session");
    if (m["areas"].isArray())
    {
        g.mHasAreas = true;
        g.mAreas.clear();
        const LLSD& a = m["areas"];
        for (size_t i = 0; i < (size_t)a.size() && g.mAreas.size() < 128; ++i)
        {
            const LLSD& x = a[(LLSD::Integer)i];
            if (!x.isMap()) continue;
            GmArea area;
            area.mId = idOf(x, "id");
            const LLSD& c = x["circle"];
            if (c.isArray() && c.size() >= 3)
            {
                area.mCircle = true;
                area.mCentre.set((F32)c[0].asReal(), (F32)c[1].asReal(), 0.f);
                area.mRadius = (F32)llclamp(c[2].asReal(), 0.0, 512.0);
            }
            else if (x["polygon"].isArray())
            {
                area.mCircle = false;
                const LLSD& poly = x["polygon"];
                for (size_t k = 0; k < (size_t)poly.size() && area.mPoly.size() < 64; ++k)
                {
                    const LLSD& pt = poly[(LLSD::Integer)k];
                    if (pt.isArray() && pt.size() >= 2) area.mPoly.push_back(LLVector3((F32)pt[0].asReal(), (F32)pt[1].asReal(), 0.f));
                }
                if (area.mPoly.size() < 3) continue;
            }
            else continue;
            g.mAreas.push_back(area);
        }
    }
    if (m["paths"].isArray())
    {
        g.mHasPaths = true;
        g.mPaths.clear();
        const LLSD& a = m["paths"];
        for (size_t i = 0; i < (size_t)a.size() && g.mPaths.size() < 64; ++i)
        {
            const LLSD& x = a[(LLSD::Integer)i];
            if (!x.isMap()) continue;
            GmPath path;
            path.mId = idOf(x, "id");
            const LLSD& pts = x["points"];
            for (size_t k = 0; pts.isArray() && k < (size_t)pts.size() && path.mPoints.size() < 500; ++k)
            {
                LLVector3 v;
                const LLSD& pt = pts[(LLSD::Integer)k];
                if (vec3(pt, v)) path.mPoints.push_back(v);
                else if (pt.isArray() && pt.size() >= 2) path.mPoints.push_back(LLVector3((F32)pt[0].asReal(), (F32)pt[1].asReal(), 0.f));
            }
            if (path.mPoints.size() >= 2) g.mPaths.push_back(path);
        }
    }
    if (m["npcs"].isArray())
    {
        g.mHasNpcs = true;
        g.mNpcs.clear();
        const LLSD& a = m["npcs"];
        for (size_t i = 0; i < (size_t)a.size() && g.mNpcs.size() < 128; ++i)
        {
            const LLSD& x = a[(LLSD::Integer)i];
            if (!x.isMap()) continue;
            GmNpc n;
            const std::string id = str(x, "npc", 36);
            if (id.size() == 36 && LLUUID::validate(id)) n.mNpc = LLUUID(id);
            n.mHandle = str(x, "handle", 128);
            n.mName = str(x, "name", CAP_NAME);
            n.mPossessedBy = str(x, "possessedBy", 36);
            g.mNpcs.push_back(n);
        }
    }
    if (m["budget"].isMap())
    {
        g.mHasBudget = true;
        g.mBudget = m["budget"];
    }
    ++g.mSeq;
}

bool WolfAdventure::pressRoll()
{
    if (!mRoll.mOn || mRoll.mSent || mRoll.mWho != gAgentID) return false;
    if (!sendRoll(mRoll.mId))
    {
        // [WOLF ADVENTURE AUDIT 2026-10-17] never silent: the roll stays to press again
        WolfGame::instance().addNotice("Roll", "Your roll was not sent: the viewer is not connected to a Wolf Territories region. Press Roll again.",
                                       WolfGame::NOTICE_DANGER);
        return false;
    }
    mRoll.mSent = true;   // §2.9: the brain rolls once per request; a second press would only be ignored
    ++mRoll.mSeq;
    ++mGeneration;
    return true;
}

bool WolfAdventure::pressEndTurn()
{
    if (!mTurns.mOn) return false;
    bool mine = false;
    for (const TurnRow& r : mTurns.mOrder) if (r.mKey == mTurns.mCurrent && r.mYou) mine = true;
    if (!mine) return false;
    if (sendEndTurn(mTurns.mAdv)) return true;
    // [WOLF ADVENTURE AUDIT 2026-10-17] never silent
    WolfGame::instance().addNotice("End turn", "Your turn was not ended: the viewer is not connected to a Wolf Territories region. Press End turn again.",
                                   WolfGame::NOTICE_DANGER);
    return false;
}

void WolfAdventure::setQuizAnswer(const std::string& item, const std::string& answer)
{
    if (!mQuiz.mOn || mQuiz.mHasResult) return;
    // §4: "answer: choice ids comma-separated, or text up to 2000 characters"
    mQuiz.mAnswers[item] = WolfGame::cleanText(answer, 2000, true);
    mQuiz.mSentOk[item] = false;
    if (mQuiz.mError.rfind("Question ", 0) == 0) mQuiz.mError.clear();   // [review] the "no answer yet" note goes once answered
    ++mQuiz.mSeq;
    ++mGeneration;
}

void WolfAdventure::sendQuizItem(const std::string& item)
{
    auto it = mQuiz.mAnswers.find(item);
    if (it == mQuiz.mAnswers.end() || it->second.empty() || mQuiz.mSentOk[item]) return;
    LLSD body;
    // Source: BRAIN_API_V1.md §5.5 quiz_answer {adventure, quiz, item, answer}; HUD_MESSAGES_V1.md §4.
    body["adventure"] = mQuiz.mAdv;
    body["quiz"] = mQuiz.mId;
    body["item"] = item;
    body["answer"] = it->second;
    const std::string quiz = mQuiz.mId, adv = mQuiz.mAdv, sent = it->second;
    // The reply's `correct` is not used: the brain gives null until the whole quiz is complete; the marks come
    // with the quiz message's result (§2.11).
    api("quiz_answer", body, [quiz, adv, item, sent](bool ok, const LLSD& reply, const std::string& error)
    {
        if (!WolfAdventure::instanceExists()) return;
        WolfAdventure& self = WolfAdventure::instance();
        if (self.mQuiz.mId != quiz || self.mQuiz.mAdv != adv) return;
        // a late reply for an older answer must not mark a newer, unsent answer as saved
        if (self.mQuiz.mAnswers[item] != sent) return;
        if (ok)
        {
            self.mQuiz.mSentOk[item] = true;
            self.mQuiz.mError.clear();
        }
        else
        {
            self.mQuiz.mError = "Your answer was not saved: " + error + " It stays here - press Next or Submit to send it again.";
            self.mQuiz.mSending = false;
        }
        ++self.mQuiz.mSeq;
        ++self.mGeneration;
    });
}

void WolfAdventure::quizGo(S32 index)
{
    if (!mQuiz.mOn || mQuiz.mItems.empty()) return;
    const S32 cur = llclamp(mQuiz.mIndex, 0, (S32)mQuiz.mItems.size() - 1);
    sendQuizItem(mQuiz.mItems[cur].mId);
    mQuiz.mIndex = llclamp(index, 0, (S32)mQuiz.mItems.size() - 1);
    ++mQuiz.mSeq;
    ++mGeneration;
}

void WolfAdventure::quizSubmit()
{
    if (!mQuiz.mOn || mQuiz.mItems.empty()) return;
    // [WOLF ADVENTURE AUDIT 2026-10-17] the brain marks a quiz only once every question has an answer
    // (wolfadventure_gm_lib.php wag_quiz_finish `complete`), and sendQuizItem skips a blank one: so a blank question is
    // shown and named first, instead of "Waiting for your result" for ever
    for (size_t i = 0; i < mQuiz.mItems.size(); ++i)
    {
        const auto a = mQuiz.mAnswers.find(mQuiz.mItems[i].mId);
        std::string ans = a == mQuiz.mAnswers.end() ? std::string() : a->second;
        LLStringUtil::trim(ans);
        if (ans.empty())
        {
            mQuiz.mIndex = (S32)i;
            mQuiz.mError = llformat("Question %d has no answer yet. Answer it, then press Submit again.", (S32)i + 1);
            ++mQuiz.mSeq;
            ++mGeneration;
            return;
        }
    }
    for (const QuizItem& it : mQuiz.mItems) sendQuizItem(it.mId);   // anything not sent yet goes now
    mQuiz.mSending = true;
    ++mQuiz.mSeq;
    ++mGeneration;
}

void WolfAdventure::quizReopen(const std::string& adv, const std::string& quiz)
{
    if (quiz.empty()) return;
    if (mQuiz.mId == quiz && mQuiz.mAdv == adv && !mQuiz.mItems.empty() && !mQuiz.mHasResult)
    {
        mQuiz.mOn = true;   // the play window follows mOn (wolfadventureplay.cpp wanted())
        ++mQuiz.mSeq;
        ++mGeneration;
        return;
    }
    LLSD body;
    body["adventure"] = adv;
    body["quiz"] = quiz;
    // BRAIN_API_V1.md quiz_get {adventure, quiz} -> {quiz: <the HUD quiz message>, answers: {item: answer}}
    api("quiz_get", body, [adv, quiz](bool ok, const LLSD& reply, const std::string& error)
    {
        if (!WolfAdventure::instanceExists()) return;
        WolfAdventure& self = WolfAdventure::instance();
        if (!ok)
        {
            WolfGame::instance().addNotice("Quiz", "The quiz could not be opened: " + error, WolfGame::NOTICE_DANGER);
            return;
        }
        LLSD m = reply["quiz"];
        if (!m.isMap()) return;
        if (!m.has("adv")) m["adv"] = adv;
        self.applyQuiz(m);
        if (self.mQuiz.mId != quiz) return;
        for (const auto& kv : llsd::inMap(reply["answers"]))
        {
            // saved already: shown as the player left them, and not sent again
            self.mQuiz.mAnswers[kv.first] = cutUtf8(kv.second.asString(), 2000);
            self.mQuiz.mSentOk[kv.first] = true;
        }
        ++self.mQuiz.mSeq;
        ++self.mGeneration;
    });
}

void WolfAdventure::quizClose()
{
    // §3.11 "nothing is lost if the window closes": answers already went per item; an open quiz stays in
    // memory until the adventure is left, and the window comes back with the next quiz message.
    mQuiz.mOn = false;
    ++mGeneration;
}

