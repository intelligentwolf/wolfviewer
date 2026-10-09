/**
 * @file wolfgame.cpp
 * @brief WolfViewer: Wolf Roleplay (wolfGame) state and protocol. See wolfgame.h.
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

#include "wolfgame.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <boost/json.hpp>

#include "llagent.h"
#include "llappviewer.h"
#include "lldispatcher.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llhudnametag.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "llviewercamera.h"
#include "llviewergenericmessage.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "llbutton.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"
#include "lltoolbarview.h"
#include "wolffloaterroleplay.h"
#include "wolfgamehud.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "llkeyboard.h"
#include "llmenugl.h"
#include "llselectmgr.h"
#include "llviewermenu.h"

F32 WolfGame::sDesaturate = 0.f;
F32 WolfGame::sBlur = 0.f;

namespace
{
    // Source: VIEWER_SPEC.md §1 "Drop an incomplete id after 10 s".
    constexpr F64 PENDING_LIFE = 10.0;
    // Source: VIEWER_SPEC.md §1 / §4 "send hello about 3 s after arriving on a region".
    constexpr F64 HELLO_DELAY = 3.0;
    // Source: VIEWER_SPEC.md §3.4 timings and "at most 4".
    constexpr F64 NOTICE_LIFE_SHORT = 6.0;
    constexpr F64 NOTICE_LIFE_WARNING = 10.0;
    constexpr size_t NOTICE_MAX = 4;
    // How long a Game Mode flip waits for the region's game message before going back.
    constexpr F64 GAMEMODE_ANSWER = 20.0;
    // Caps, dropping the oldest (WolfStorm review): a looping script cannot grow these forever.
    constexpr size_t MAX_MARKERS = 64;
    constexpr size_t MAX_TAGS = 256;
    // Photosensitivity: never flash faster than 3 times a second.
    constexpr F64 FLASH_MIN_GAP = 0.334;
    // A message is at most a few KB (WolfGameLimits: rules 2000, text 512, 20 panel rows);
    // 720 bytes a packet. Anything claiming more parts than this is not ours.
    constexpr S32 MAX_PARTS = 256;
    // An unresolved other-region marker asks the map server again this often.
    constexpr F64 MAP_ASK_EVERY = 15.0;
    // Camera shake at strength 100, metres either way.
    constexpr F32 SHAKE_METRES = 0.25f;

    const std::string MESSAGE_WOLF_GAME("WolfGame");     // WolfGameModule.cs MESSAGE
    const std::string METHOD_TO_SIM("wolfgame");         // WolfGameModule.cs OnNewClient

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    /** "#rrggbb" (WolfGameLimits.COLOUR) to a colour; anything else gives `fallback`. */
    LLColor4 parseColour(const std::string& s, const LLColor4& fallback)
    {
        if (s.size() != 7 || s[0] != '#') return fallback;
        for (size_t i = 1; i < 7; ++i)
        {
            if (!isxdigit((unsigned char)s[i])) return fallback;
        }
        const unsigned long v = strtoul(s.c_str() + 1, nullptr, 16);
        return LLColor4(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, 1.f);
    }

    // [SECURITY 2026-10-09] String caps, mirroring WolfGameTypes.cs WolfGameLimits (MAX_TITLE 64,
    // MAX_TEXT 512, MAX_LABEL 32, MAX_BUTTON_LABEL 24, MAX_RULES 2000, MAX_DESCRIPTION 500,
    // MAX_TAG 48, MAX_URL 1024, MAX_DATA_KEY 32, MAX_NAME 24, MAX_CURRENCY_NAME 24,
    // MAX_CURRENCY_SYMBOL 8, icons Cut(16), regions Cut(64)). The sim cuts by UTF-16 unit; this
    // cuts by code point, so it never keeps MORE than the sim allows.
    constexpr size_t CAP_TITLE = 64, CAP_TEXT = 512, CAP_SRC = 64, CAP_LABEL = 32, CAP_BUTTON = 24,
                     CAP_RULES = 2000, CAP_DESCRIPTION = 500, CAP_TAG = 48, CAP_URL = 1024, CAP_ID = 64,
                     CAP_NAME = 32, CAP_CURRENCY = 24, CAP_SYMBOL = 8, CAP_ICON = 16, CAP_REGION = 64,
                     CAP_ANY = 2000;
    // [SECURITY 2026-10-09] Count caps (the same as WolfStorm's).
    // [COMBAT 2026-10-09] WolfGameTypes.cs WolfGameLimits MAX_ABILITIES 12, MAX_BUFFS 20,
    // MAX_KEY_HINT 8; VIEWER_SPEC.md §5.6 "at most 12 numbers on screen".
    constexpr size_t MAX_ABILITIES = 12, MAX_BUFFS = 20, CAP_KEY = 8, MAX_COMBAT_TEXT = 12;
    // Source: VIEWER_SPEC.md §5.4 errors "stays for 3 s; the same text again within 1 s is not
    // repeated"; §5.3 pending equip "until the next loadout ... or a notify".
    constexpr F64 COMBAT_ERROR_LIFE = 3.0, COMBAT_ERROR_REPEAT = 1.0, EQUIP_ANSWER = 10.0, CAST_END_SHOW = 1.0;
    constexpr F64 COMBAT_TEXT_LIFE = 1.2;
    constexpr size_t MAX_STATS = 64, MAX_ITEMS = 256, MAX_PETS = 64, MAX_PET_STATS = 16,
                     MAX_PANEL_ROWS = 20, MAX_BUTTONS = 12, MAX_PENDING = 64;

    /** At most `max` code points, control characters gone (a newline kept only when `lines`). */
    std::string capText(const std::string& in, size_t max, bool lines = false)
    {
        LLWString w = utf8str_to_wstring(in.size() > max * 4 + 4 ? in.substr(0, max * 4 + 4) : in);
        LLWString out;
        out.reserve(llmin(w.size(), max));
        for (llwchar c : w)
        {
            if (out.size() >= max) break;
            if (c == '\n' && lines) { out.push_back(c); continue; }
            if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) continue;
            out.push_back(c);
        }
        return wstring_to_utf8str(out);
    }

    std::string str(const LLSD& m, const char* k, size_t max = CAP_ANY, bool lines = false)
    {
        return m.has(k) && m[k].isString() ? capText(m[k].asString(), max, lines) : std::string();
    }

    /**
     * [WOLF GAME ICONS 2026-10-09] An icon field (stat i, item i, pet icon, pet stat i, game
     * currencyIcon): a texture UUID (the game kit's own icons - Paul: "PROPER GRAPHICS", no emoji),
     * or, from other people's games, a short text glyph of at most 16 characters (WolfGameModule.cs
     * ShowStat: Cut(icon, 16)). A UUID is kept whole and lower case; anything else is capped.
     */
    std::string iconStr(const LLSD& m, const char* k)
    {
        if (!m.has(k) || !m[k].isString()) return std::string();
        const std::string raw = m[k].asString();
        if (raw.size() == 36 && LLUUID::validate(raw))
        {
            const LLUUID id(raw);
            return id.notNull() ? id.asString() : std::string();
        }
        return capText(raw, CAP_ICON);
    }

    /**
     * [SECURITY 2026-10-09] A script's object name as shown beside its notices, dialogs, buttons
     * and pages: capped, and with nothing that reads as a link or a markup token - [ ], control
     * characters, "://", "secondlife:", "www.".
     */
    std::string cleanSrc(const std::string& in)
    {
        std::string s = capText(in, CAP_SRC);
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '[' || c == ']'; }), s.end());
        for (const char* bad : { "://", "secondlife:", "www." })
        {
            for (;;)
            {
                std::string lower = s;
                LLStringUtil::toLower(lower);
                const size_t at = lower.find(bad);
                if (at == std::string::npos) break;
                s.erase(at, strlen(bad));
            }
        }
        return s;
    }

    /** The sim's JsonSerializer writes these as numbers; jsonToLLSD keeps small ints as ints.
     *  [SECURITY] Non-finite values give the fallback; others are held to +-1e15. */
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
        return fallback;
    }

    F64 real(const LLSD& m, const char* k, F64 fallback = 0.0)
    {
        if (!m.has(k)) return fallback;
        const LLSD& v = m[k];
        if (!(v.isInteger() || v.isReal())) return fallback;
        const F64 d = v.asReal();
        return std::isfinite(d) ? llclamp(d, -1e15, 1e15) : fallback;
    }

    S32 num32(const LLSD& m, const char* k, S64 fallback = 0)
    {
        return (S32)llclamp(num(m, k, fallback), (S64)std::numeric_limits<S32>::min(), (S64)std::numeric_limits<S32>::max());
    }

    /** Sim-side ints can be long (the balance, xp): LLSD integers are 32-bit, reals are not. */
    S64 bigNum(const LLSD& m, const char* k)
    {
        return num(m, k, 0);
    }

    /**
     * Source: llsdjson.cpp LlsdFromJson, with one difference: LLSD integers are 32-bit and
     * LLSD(int64) narrows by static_cast (stdtypes.h narrow, an assert only), so a balance or
     * xp past 2^31 (WolfGameModule.cs: long) would wrap. Those become reals (exact to 2^53).
     */
    LLSD jsonToLLSD(const boost::json::value& val)
    {
        LLSD result;
        switch (val.kind())
        {
        case boost::json::kind::int64:
        {
            const int64_t i = val.get_int64();
            if (i >= std::numeric_limits<S32>::min() && i <= std::numeric_limits<S32>::max()) result = LLSD((LLSD::Integer)i);
            else result = LLSD((LLSD::Real)i);
            break;
        }
        case boost::json::kind::uint64:
        {
            const uint64_t u = val.get_uint64();
            if (u <= (uint64_t)std::numeric_limits<S32>::max()) result = LLSD((LLSD::Integer)u);
            else result = LLSD((LLSD::Real)u);
            break;
        }
        case boost::json::kind::double_:
            result = LLSD(val.get_double());
            break;
        case boost::json::kind::string:
            result = LLSD(std::string(val.get_string().c_str(), val.get_string().size()));
            break;
        case boost::json::kind::bool_:
            result = LLSD(val.get_bool());
            break;
        case boost::json::kind::array:
            result = LLSD::emptyArray();
            for (const boost::json::value& e : val.get_array()) result.append(jsonToLLSD(e));
            break;
        case boost::json::kind::object:
            result = LLSD::emptyMap();
            for (const auto& kv : val.get_object()) result[std::string(kv.key())] = jsonToLLSD(kv.value());
            break;
        default:   // null
            break;
        }
        return result;
    }

    /** Header "id/part/total" (WolfGameModule.cs Send: id + "/" + part + "/" + total). */
    bool parseHeader(const std::string& h, S64& id, S32& part, S32& total)
    {
        const size_t a = h.find('/');
        if (a == std::string::npos) return false;
        const size_t b = h.find('/', a + 1);
        if (b == std::string::npos || h.find('/', b + 1) != std::string::npos) return false;
        auto digits = [](const std::string& s) { return !s.empty() && s.size() <= 10 && s.find_first_not_of("0123456789") == std::string::npos; };
        const std::string sid = h.substr(0, a), spart = h.substr(a + 1, b - a - 1), stotal = h.substr(b + 1);
        if (!digits(sid) || !digits(spart) || !digits(stotal)) return false;
        id = std::stoll(sid);
        part = (S32)std::stoll(spart);
        total = (S32)std::stoll(stotal);
        return total >= 1 && total <= MAX_PARTS && part >= 0 && part < total;
    }
}

/**
 * Source: wolfregionweather.cpp WolfWeatherPushHandler - the same shape: registered with
 * gGenericDispatcher, and only the region the agent is in may change this agent's screen.
 */
class WolfGamePushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice,
                    const sparam_t& strings) override
    {
        WolfGame::instance().receivePacket(invoice, strings);
        return true;
    }
};

namespace
{
    WolfGamePushHandler sGamePushHandler;
}

WolfGame::WolfGame()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_WOLF_GAME))
    {
        gGenericDispatcher.addHandler(MESSAGE_WOLF_GAME, &sGamePushHandler);
    }
    // Source: llagent.cpp setRegion fires this inside the region switch, before any later packet
    // is read, so the old region's screen is gone before the new region's "game" arrives.
    // [WOLF GRID GATE] off Wolf Territories nothing of the game runs; back on Wolf, idle()'s
    // ensureRegion catches the region up.
    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { if (WolfGrid::isOnWolfTerritories()) ensureRegion(); });
}

WolfGame::~WolfGame()
{
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
    clearTags();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// sending
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGame::send(const std::vector<std::string>& params)
{
    if (!gAgent.getRegion()) return;
    // [WOLF GRID GATE 2026-10-09] only to a region that has itself said it is Wolf Territories:
    // just after a hypergrid jump the previous region's answer still stands (wolfgrid.h).
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    // Source: llviewergenericmessage.cpp send_generic_message - every non-empty parameter goes
    // out with its NUL, the byte LLClientView's Util.FieldToString removes.
    send_generic_message(METHOD_TO_SIM, params);
}

void WolfGame::sendHello() { send({ "hello" }); }
void WolfGame::sendJoin() { send({ "join" }); }
void WolfGame::sendLeave() { send({ "leave" }); }
void WolfGame::sendDialog(const std::string& id, const std::string& label) { send({ "dialog", id, label }); }
void WolfGame::sendButton(const std::string& id) { send({ "button", id }); }

// ═══════════════════════════════════════════════════════════════════════════════════════
// receiving
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGame::receivePacket(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // [WOLF GRID GATE 2026-10-09] Wolf Roleplay exists only on Wolf Territories (the same gate as
    // Flight Mode, wolfgrid.h isOnWolfTerritories): elsewhere these messages are ignored.
    if (!WolfGrid::isOnWolfTerritories()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    // VIEWER_SPEC.md §1: drop any message whose Invoice is not the agent's current region.
    if (!rgn || rgn->getRegionID() != invoice)
    {
        LL_DEBUGS("WolfGame") << "ignoring a WolfGame message for another region" << LL_ENDL;
        return;
    }
    ensureRegion();
    if (strings.empty()) return;

    S64 id = 0;
    S32 part = 0, total = 0;
    if (!parseHeader(strings[0], id, part, total))
    {
        LL_WARNS("WolfGame") << "unreadable WolfGame header '" << strings[0] << "'" << LL_ENDL;
        return;
    }
    std::string piece;
    for (size_t i = 1; i < strings.size(); ++i) piece += strings[i];

    std::string json;
    if (total == 1)
    {
        json.swap(piece);   // the common case, no buffering (§1)
    }
    else
    {
        if (!mPending.count(id) && mPending.size() >= MAX_PENDING)
        {
            mPending.erase(mPending.begin());   // [SECURITY] at most MAX_PENDING half-built messages
        }
        Pending& p = mPending[id];
        if (p.mTotal == 0)
        {
            p.mTotal = total;
            p.mFirst = now();
        }
        if (p.mTotal != total)
        {
            LL_WARNS("WolfGame") << "WolfGame message " << id << " changed its part count; dropped" << LL_ENDL;
            mPending.erase(id);
            return;
        }
        p.mParts[part] = piece;
        if ((S32)p.mParts.size() < total) return;
        for (auto& kv : p.mParts) json += kv.second;   // std::map: in part order
        mPending.erase(id);
    }

    boost::system::error_code ec;
    boost::json::value v = boost::json::parse(json, ec);
    if (ec)
    {
        LL_WARNS("WolfGame") << "unreadable WolfGame JSON (" << json.size() << " bytes)" << LL_ENDL;
        return;
    }
    const LLSD sd = jsonToLLSD(v);
    if (!sd.isMap()) return;
    apply(sd);
}

void WolfGame::apply(const LLSD& m)
{
    const std::string t = str(m, "t");
    if (t == "game") applyGame(m);
    else if (t == "player") applyPlayer(m);
    else if (t == "stat") applyStat(m);
    else if (t == "hidestat")
    {
        const std::string n = str(m, "n");
        mStats.erase(std::remove_if(mStats.begin(), mStats.end(), [&](const Stat& s) { return s.mName == n; }), mStats.end());
    }
    else if (t == "currency") applyCurrency(m);
    else if (t == "level") applyLevel(m);
    else if (t == "notify") applyNotify(m);
    else if (t == "dialog") applyDialog(m);
    else if (t == "buttons")
    {
        // Source: WolfGameModule.cs SetButtons {t,buttons,src,verified}.
        mButtonsSrc = cleanSrc(str(m, "src"));
        mButtonsVerified = !m.has("verified") || num(m, "verified") != 0;
        applyButtons(m["buttons"]);
    }
    else if (t == "effect") applyEffect(m);
    else if (t == "downed") applyDowned(m);
    else if (t == "web") applyWeb(m);
    else if (t == "webclose") WolfFloaterGameWeb::closePage();
    else if (t == "marker") applyMarker(m);
    else if (t == "unmarker")
    {
        const std::string id = str(m, "id");
        if (id.empty()) mMarkers.clear();
        else mMarkers.erase(id);
    }
    else if (t == "tag") applyTag(m);
    else if (t == "item") applyItem(m);
    else if (t == "pet") applyPet(m["pet"]);
    else if (t == "unpet")
    {
        const std::string key = str(m, "key");
        mPets.erase(std::remove_if(mPets.begin(), mPets.end(), [&](const Pet& p) { return p.mKey == key; }), mPets.end());
    }
    else if (t == "panel") applyPanel(m);
    // [COMBAT] §5.2
    else if (t == "loadout") applyLoadout(m["slots"]);
    else if (t == "gear") applyGear(str(m, "slot", CAP_LABEL), m["g"]);
    else if (t == "ammo") applyAmmo(str(m, "slot", CAP_LABEL), m);
    else if (t == "ability") applyAbility(m["a"]);
    else if (t == "unability")
    {
        const std::string id = str(m, "id", CAP_ID);
        if (id.empty()) mAbilities.clear();
        else mAbilities.erase(std::remove_if(mAbilities.begin(), mAbilities.end(), [&](const Ability& a) { return a.mId == id; }), mAbilities.end());
    }
    else if (t == "cooldown") applyCooldown(m);
    else if (t == "cast") applyCast(m);
    else if (t == "combaterr") applyCombatError(m);
    else if (t == "target") applyTarget(m);
    else if (t == "targethp") applyTargetHealth(m);
    else if (t == "buff") applyBuff(m["b"]);
    else if (t == "unbuff")
    {
        const std::string id = str(m, "id", CAP_ID);
        if (id.empty()) mBuffs.clear();
        else mBuffs.erase(std::remove_if(mBuffs.begin(), mBuffs.end(), [&](const Buff& b) { return b.mId == id; }), mBuffs.end());
    }
    else if (t == "ctext") applyCombatText(m);
    else
    {
        LL_DEBUGS("WolfGame") << "unknown WolfGame message '" << t << "'" << LL_ENDL;
        return;
    }
    ++mGeneration;
}

// Source: WolfGameModule.cs SendGame - {"t":"game","playing":0|1,"gamemode":0|1,"game":null|
// {key,name,kind,description,website,rules,logo,currency,symbol,theme}}.
void WolfGame::applyGame(const LLSD& m)
{
    const bool was_playing = mPlaying;   // before this message: the Leave edge below needs it
    mPlaying = num(m, "playing") != 0;
    // §3.2: both switches show `gamemode` from the latest game message; a pending flip ends when
    // the region reports the value asked for.
    const bool asked_on = mGameModePending && mGameModeWanted;
    mGameMode = num(m, "gamemode") != 0;
    // [WOLF GAME UI 2026-10-09] The guide opens by itself the first time the PLAYER turns Game
    // Mode on (wolfflightdeck.cpp's WolfFlightHelpShown pattern; not on a login with it already
    // on, nor on a forced region); the window's Help tab reopens it.
    // [GAME HUD 2026-10-09] Paul: the help must not open over the play area - the guide is a
    // few callouts on the HUD and a small card beside it, nothing over the world, gone on "Got it".
    if (asked_on && mGameMode && !gSavedSettings.getBOOL("WolfGameHelpShown"))
    {
        gSavedSettings.setBOOL("WolfGameHelpShown", true);
        mGuideOn = true;
    }
    // "Leave this game" (WolfGameModule.cs LeaveNow) answers with a `game` only: on the edge from
    // playing to not, ask for the whole screen again so the old game's bars, bag and buttons go.
    if (was_playing && num(m, "playing") == 0)
    {
        mLeavePendingAt = 0.0;
        sendHello();
    }
    // [FORCED 2026-10-09] VIEWER_SPEC.md §6 / WolfGameModule.cs SendGame {forced, forcedText}:
    // the region requires Game Mode; gamemode is 1 whenever it does.
    const bool was_forced = mForced;
    mForced = num(m, "forced") != 0;
    // [2026-10-09] Paul: "if game mode is forced on they can still get injured" - arriving on a
    // region that requires Game Mode brings the HUD up (health in view), even though a session
    // starts with it hidden. A flight deck or car dashboard keeps the bottom until it goes.
    if (mForced && !was_forced) { mHudHidden = false; ++mGeneration; }
    mForcedText = mForced ? str(m, "forcedText", CAP_TEXT) : std::string();
    if (mForced && mForcedText.empty()) mForcedText = "This region requires Game Mode";
    // [FIX 2026-10-09] Paul: "i closed the game and this didnt disappear" - with Game Mode off
    // nothing of the game shows, the target frame included.
    if (!mGameMode && !mForced) mTarget = Target();
    if (mGameModePending && mGameMode == mGameModeWanted) mGameModePending = false;
    // [GAME HUD] the player switched it on and the region agreed: the HUD shows, and takes the
    // bottom (not before - a refused switch must not leave the deck hidden or the dashboard shut).
    if (asked_on && mGameMode) claimBottom();
    updateGameModeButtons();
    const LLSD& g = m["game"];
    if (!g.isMap())
    {
        // §2: no game here - clear the player, the stats and the panel; web pages and markers
        // belong to the script and stay.
        mGame = Game();
        clearPlayer();
        return;
    }
    mGame.mValid = true;
    mGame.mKey = str(g, "key", CAP_ID);
    mGame.mName = str(g, "name", CAP_TITLE);
    mGame.mKind = str(g, "kind", CAP_LABEL);
    mGame.mDescription = str(g, "description", CAP_DESCRIPTION, true);
    mGame.mWebsite = str(g, "website", CAP_URL);
    mGame.mRules = str(g, "rules", CAP_RULES, true);
    // The logo is fetched as a texture: only ever a well-formed, non-null UUID.
    mGame.mLogo = str(g, "logo", CAP_ID);
    if (!LLUUID::validate(mGame.mLogo) || LLUUID(mGame.mLogo).isNull()) mGame.mLogo.clear();
    mGame.mCurrency = str(g, "currency", CAP_CURRENCY);
    mGame.mSymbol = str(g, "symbol", CAP_SYMBOL);
    // [WOLF GAME ICONS 2026-10-09] The money's own icon: a texture UUID or "" (never a glyph -
    // iconStr keeps a glyph, so anything that is not a UUID is dropped here).
    mGame.mCurrencyIcon = iconStr(g, "currencyIcon");
    if (!LLUUID::validate(mGame.mCurrencyIcon)) mGame.mCurrencyIcon.clear();
    // [BAG 2026-10-09] Source: WolfGameModule.cs SendGame "items" (ItemDefsJson {n,l,i,o,k,s}) -
    // every item the game defines, sorted by o then name.
    mGame.mItemDefs.clear();
    const LLSD& defs = g["items"];
    if (defs.isArray())
    {
        for (LLSD::array_const_iterator it = defs.beginArray(); it != defs.endArray(); ++it)
        {
            if (!it->isMap()) continue;
            if (mGame.mItemDefs.size() >= MAX_ITEMS) break;   // [SECURITY] count cap
            Item d;
            d.mName = str(*it, "n", CAP_NAME);
            if (d.mName.empty()) continue;
            d.mLabel = str(*it, "l", CAP_NAME);
            if (d.mLabel.empty()) d.mLabel = d.mName;
            d.mIcon = iconStr(*it, "i");
            d.mOrder = num32(*it, "o");
            d.mKind = str(*it, "k", CAP_LABEL);
            d.mSlot = str(*it, "s", CAP_LABEL);
            mGame.mItemDefs.push_back(d);
        }
    }
    mGame.mTheme = str(g, "theme", CAP_LABEL);
}

// Source: WolfGameModule.cs SendPlayer. Replaces all previous screen state (§2 player).
void WolfGame::applyPlayer(const LLSD& m)
{
    mPlaying = num(m, "playing") != 0;
    mStats.clear();
    const LLSD& stats = m["stats"];
    if (stats.isArray())
    {
        for (LLSD::array_const_iterator it = stats.beginArray(); it != stats.endArray(); ++it)
        {
            const LLSD& s = *it;
            if (!s.isMap()) continue;
            if (mStats.size() >= MAX_STATS) break;   // [SECURITY] count cap
            Stat st;
            st.mName = str(s, "n", CAP_LABEL);
            st.mLabel = s.has("l") ? str(s, "l", CAP_LABEL) : st.mName;
            st.mValue = num32(s, "v");
            st.mMax = num32(s, "m");
            st.mColor = parseColour(str(s, "c"), LLColor4(0.518f, 0.800f, 0.086f, 1.f));   // #84cc16, ShowStat's default
            st.mIcon = iconStr(s, "i");
            st.mOrder = num32(s, "o");
            st.mShown = num(s, "shown") != 0;
            st.mArrival = ++mStatArrival;
            mStats.push_back(st);
        }
    }
    sortStats();
    mDead = num(m, "dead") != 0;
    mDowned = num(m, "downed") != 0;
    // The overlay follows the player's state: a downed overlay already up keeps its text and
    // countdown; one the sim no longer has is taken down (never leave an overlay stuck, §4).
    if (mDowned && !mDownedOn)
    {
        mDownedOn = true;
        mDownedText = "You are down";   // WolfGameModule.cs Death(): the "stay" text
        mDownedUntil = 0.0;
    }
    else if (!mDowned && mDownedOn)
    {
        mDownedOn = false;
        mDownedText.clear();
        mDownedUntil = 0.0;
    }
    mHasCurrency = false;
    mCurrencySymbol.clear();
    mCurrencyName.clear();
    mBalance = 0;
    if (m["currency"].isMap()) applyCurrency(m["currency"]);
    mHasLevel = false;
    if (m["level"].isMap()) applyLevel(m["level"]);
    mPanelTitle.clear();
    mPanelRows.clear();
    if (m["panel"].isMap()) applyPanel(m["panel"]);
    mButtons.clear();
    if (m["buttons"].isArray()) applyButtons(m["buttons"]);
    // [SECURITY 2026-10-09] SendPlayer now says who set the buttons (buttonsSrc, buttonsVerified as
    // an int, WolfGameTypes.cs Screen.ButtonsVerified). Absent (an older sim): the last "buttons"
    // message's values stand, and after a region change they are unverified.
    if (m.has("buttonsVerified"))
    {
        mButtonsVerified = num(m, "buttonsVerified") != 0;
        mButtonsSrc = cleanSrc(str(m, "buttonsSrc"));
    }
    // [FARMING] / [PETS] SendPlayer: items[] (q > 0 only) and pets[] when a player of the game.
    mItems.clear();
    const LLSD& items = m["items"];
    if (items.isArray())
    {
        for (LLSD::array_const_iterator it = items.beginArray(); it != items.endArray(); ++it)
        {
            if (it->isMap()) applyItem(*it);
        }
    }
    mPets.clear();
    const LLSD& pets = m["pets"];
    if (pets.isArray())
    {
        for (LLSD::array_const_iterator it = pets.beginArray(); it != pets.endArray(); ++it)
        {
            applyPet(*it);
        }
    }
    // [COMBAT] Source: WolfGameModule.cs SendPlayer / AddCombatState - loadout {slot: entry},
    // gear {slot: g}, ammo {slot: {a,m,r,rl,rt}}, abilities [a], cast {id,l,sec,left},
    // target {..}, buffs [b]; each only when there is something to show.
    clearCombat();
    applyLoadout(m["loadout"]);
    if (m["gear"].isMap())
    {
        for (LLSD::map_const_iterator it = m["gear"].beginMap(); it != m["gear"].endMap(); ++it) applyGear(capText(it->first, CAP_LABEL), it->second);
    }
    if (m["ammo"].isMap())
    {
        for (LLSD::map_const_iterator it = m["ammo"].beginMap(); it != m["ammo"].endMap(); ++it) applyAmmo(capText(it->first, CAP_LABEL), it->second);
    }
    if (m["abilities"].isArray())
    {
        for (LLSD::array_const_iterator it = m["abilities"].beginArray(); it != m["abilities"].endArray(); ++it) applyAbility(*it);
    }
    if (m["cast"].isMap()) applyCast(m["cast"]);
    if (m["target"].isMap()) applyTarget(m["target"]);
    if (m["buffs"].isArray())
    {
        for (LLSD::array_const_iterator it = m["buffs"].beginArray(); it != m["buffs"].endArray(); ++it) applyBuff(*it);
    }
}

// Source: WolfGameModule.cs FlushChanges {t,n,v,m} and ShowStat {t,n,l,v,m,c,i,o,shown}.
void WolfGame::applyStat(const LLSD& m)
{
    const std::string n = str(m, "n", CAP_LABEL);
    if (n.empty()) return;
    auto it = std::find_if(mStats.begin(), mStats.end(), [&](const Stat& s) { return s.mName == n; });
    if (it == mStats.end())
    {
        if (mStats.size() >= MAX_STATS) return;   // [SECURITY] count cap
        Stat st;
        st.mName = n;
        st.mLabel = n;
        st.mColor = LLColor4(0.518f, 0.800f, 0.086f, 1.f);   // #84cc16, ShowStat's default
        st.mOrder = (S32)mStats.size();
        st.mArrival = ++mStatArrival;
        mStats.push_back(st);
        it = mStats.end() - 1;
    }
    it->mValue = num32(m, "v", it->mValue);
    it->mMax = num32(m, "m", it->mMax);
    // §2: when l / c are absent, keep the label and colour already shown.
    if (m.has("l")) it->mLabel = str(m, "l", CAP_LABEL);
    if (m.has("c")) it->mColor = parseColour(str(m, "c"), it->mColor);
    if (m.has("i")) it->mIcon = iconStr(m, "i");
    if (m.has("o")) it->mOrder = num32(m, "o");
    if (m.has("shown")) it->mShown = num(m, "shown") != 0;
    sortStats();
}

void WolfGame::sortStats()
{
    std::stable_sort(mStats.begin(), mStats.end(), [](const Stat& a, const Stat& b)
    {
        if (a.mOrder != b.mOrder) return a.mOrder < b.mOrder;
        return a.mArrival < b.mArrival;
    });
}

// Source: WolfGameModule.cs Money() {t,s,name,b}, SetCurrencyDisplay {t,s,b,shown:1},
// SendPlayer currency {s,name,b} / {s,b,shown:1}.
void WolfGame::applyCurrency(const LLSD& m)
{
    const std::string s = str(m, "s", CAP_SYMBOL);
    // §2: s == "" with shown hides the money line (SetCurrencyDisplay: HasCurrency = symbol != "").
    if (s.empty() && num(m, "shown") != 0)
    {
        mHasCurrency = false;
        mCurrencySymbol.clear();
        mCurrencyName.clear();
        mBalance = 0;
        return;
    }
    mHasCurrency = true;
    mCurrencySymbol = s;
    if (m.has("name")) mCurrencyName = str(m, "name", CAP_CURRENCY);
    mBalance = bigNum(m, "b");
}

// Source: WolfGameModule.cs SetLevel {t,lv,xp,next}; §2 "lv <= 0 hides it".
void WolfGame::applyLevel(const LLSD& m)
{
    mLevel = num32(m, "lv");
    mXp = bigNum(m, "xp");
    mXpNext = bigNum(m, "next");
    mHasLevel = mLevel > 0;
}

// Source: WolfGameModule.cs Notify {t,title,text,kind,src}; OnViewerMessage's failed join
// sends {t,title,text,kind:"danger"} with no src.
// [SECURITY 2026-10-09] A few a second at most (a burst of 3, then 3 a second): a looping script
// cannot flood the screen with notices or dialogs.
bool WolfGame::takeToken(F64& tokens, F64& last)
{
    const F64 t = now();
    tokens = llmin(3.0, tokens + (t - last) * 3.0);
    last = t;
    if (tokens < 1.0) return false;
    tokens -= 1.0;
    return true;
}

// Source: WolfGameModule.cs Dialog {t,id,title,text,buttons:[label...],src,verified}, every field
// held to the sim's own limits before a floater sees it.
void WolfGame::applyDialog(const LLSD& m)
{
    if (!takeToken(mDialogTokens, mDialogTokenAt)) return;
    LLSD d;
    d["id"] = str(m, "id", CAP_ID);
    d["title"] = str(m, "title", CAP_TITLE);
    d["text"] = str(m, "text", CAP_TEXT, true);
    d["src"] = cleanSrc(str(m, "src"));
    d["verified"] = (!m.has("verified") || num(m, "verified") != 0) ? 1 : 0;
    d["buttons"] = LLSD::emptyArray();
    const LLSD& b = m["buttons"];
    if (b.isArray())
    {
        for (LLSD::array_const_iterator it = b.beginArray(); it != b.endArray() && (size_t)d["buttons"].size() < MAX_BUTTONS; ++it)
        {
            if (it->isString()) d["buttons"].append(capText(it->asString(), CAP_BUTTON));
        }
    }
    WolfFloaterGameDialog::showDialog(d);
}

void WolfGame::applyNotify(const LLSD& m)
{
    // The sim's own notices (no src - Game Mode, join failures) are not rate limited.
    if (m.has("src") && !takeToken(mNoticeTokens, mNoticeTokenAt)) return;
    Notice n;
    n.mSerial = ++mNoticeSerial;
    n.mTitle = str(m, "title", CAP_TITLE);
    n.mText = str(m, "text", CAP_TEXT, true);
    n.mSrc = cleanSrc(str(m, "src"));
    n.mVerified = !m.has("verified") || num(m, "verified") != 0;
    const std::string kind = str(m, "kind");
    n.mKind = kind == "success" ? NOTICE_SUCCESS : kind == "warning" ? NOTICE_WARNING : kind == "danger" ? NOTICE_DANGER : NOTICE_INFO;
    n.mStart = now();
    n.mExpires = n.mKind == NOTICE_DANGER ? 0.0 : n.mStart + (n.mKind == NOTICE_WARNING ? NOTICE_LIFE_WARNING : NOTICE_LIFE_SHORT);
    mNotices.push_back(n);
    while (mNotices.size() > NOTICE_MAX) mNotices.pop_front();   // §3.4 the oldest drops off
    // §3.2: "Turning Game Mode on…" lasts until the next game message, or a danger notice
    // (OnViewerMessage's "Could not turn Game Mode on/off: ..."); the switch then shows the truth.
    // [COMBAT] §5.3: a refused equip is explained by a notify; the slot stops showing "Equipping".
    if ((n.mKind == NOTICE_WARNING || n.mKind == NOTICE_DANGER) && !mEquipPending.empty())
    {
        mEquipPending.clear();
    }
    // Only the sim's own or the game's (verified) danger notice: any other script could send
    // one to make the switch look as if it had failed.
    if (n.mKind == NOTICE_DANGER && n.mVerified && mGameModePending)
    {
        mGameModePending = false;
        updateGameModeButtons();
    }
}

void WolfGame::closeNotice(U32 serial)
{
    mNotices.erase(std::remove_if(mNotices.begin(), mNotices.end(), [&](const Notice& n) { return n.mSerial == serial; }), mNotices.end());
    ++mGeneration;
}

// Source: WolfGameModule.cs SetButtons {t,buttons:[{id,label}],src}; SendPlayer buttons.
void WolfGame::applyButtons(const LLSD& buttons)
{
    mButtons.clear();
    if (!buttons.isArray()) return;
    for (LLSD::array_const_iterator it = buttons.beginArray(); it != buttons.endArray(); ++it)
    {
        if (!it->isMap()) continue;
        Button b;
        b.mId = str(*it, "id", CAP_ID);
        b.mLabel = str(*it, "label", CAP_BUTTON);
        if (!b.mId.empty()) mButtons.push_back(b);
        if (mButtons.size() >= MAX_BUTTONS) break;   // [SECURITY] WolfGameLimits.MAX_BUTTONS
    }
}

// Source: WolfGameModule.cs Effect {t,e,s,sec}; WolfGameLimits.EFFECTS.
void WolfGame::applyEffect(const LLSD& m)
{
    const std::string e = str(m, "e");
    if (e == "clear")
    {
        clearEffects();
        return;
    }
    EEffect which;
    if (e == "flash") which = FX_FLASH;
    else if (e == "pulse") which = FX_PULSE;
    else if (e == "blur") which = FX_BLUR;
    else if (e == "blackout") which = FX_BLACKOUT;
    else if (e == "desaturate") which = FX_DESATURATE;
    else if (e == "shake") which = FX_SHAKE;
    else return;
    const F32 seconds = llclamp((F32)real(m, "sec"), 0.f, 600.f);   // WolfGameModule.cs Effect: 0..600
    // [SECURITY 2026-10-09] Photosensitivity: ONE limiter for every effect that changes the
    // screen's brightness (flash, blackout, pulse, desaturate) - no start or restart within
    // 334 ms of the last, so no combination of them can strobe faster than 3 Hz; and a blackout
    // shorter than 0.33 s (a strobe frame) is ignored. 0 = held until "clear" is still allowed:
    // it cannot strobe, and the player can always clear it (Escape, "Clear screen effects").
    const bool luma = which == FX_FLASH || which == FX_BLACKOUT || which == FX_PULSE || which == FX_DESATURATE;
    if (which == FX_BLACKOUT && seconds > 0.f && seconds < 0.33f) return;
    if (luma)
    {
        const F64 t = now();
        if (t - mLastFlashAt < FLASH_MIN_GAP) return;
        mLastFlashAt = t;
    }
    Effect& fx = mEffects[which];
    fx.mOn = true;
    fx.mStrength = llclamp((F32)num(m, "s") / 100.f, 0.f, 1.f);
    fx.mSeconds = seconds;
    fx.mStart = now();
}

void WolfGame::clearEffects()
{
    for (Effect& fx : mEffects) fx = Effect();
    sDesaturate = 0.f;
    sBlur = 0.f;
    ++mGeneration;
}

F32 WolfGame::effectNow(EEffect e) const
{
    const Effect& fx = mEffects[e];
    if (!fx.mOn) return 0.f;
    if (fx.mSeconds <= 0.f) return fx.mStrength;   // held until "clear"
    const F64 t = now() - fx.mStart;
    if (t >= fx.mSeconds) return 0.f;
    return fx.mStrength * (F32)(1.0 - t / fx.mSeconds);
}

F32 WolfGame::pulseBeat() const
{
    const F64 t = now() - mEffects[FX_PULSE].mStart;
    return 0.5f + 0.5f * (F32)sin(t * 2.0 * F_PI);   // about once a second (§3.6)
}

// Source: WolfGameModule.cs SetDowned {t,on,text,sec}; Death() {t,on:1,sec,text}; DoRespawn {t,on:0}.
void WolfGame::applyDowned(const LLSD& m)
{
    if (num(m, "on") != 0)
    {
        mDownedOn = true;
        mDowned = true;
        mDownedText = str(m, "text", CAP_TITLE);
        // WolfGameModule.cs SetDowned: 0..3600 - held there, so a huge value cannot overflow.
        const S64 sec = llclamp(num(m, "sec"), (S64)0, (S64)3600);
        mDownedUntil = sec > 0 ? now() + (F64)sec : 0.0;
    }
    else
    {
        mDownedOn = false;
        mDowned = false;
        mDownedText.clear();
        mDownedUntil = 0.0;
    }
}

S32 WolfGame::downedCountdown() const
{
    if (!mDownedOn || mDownedUntil <= 0.0) return -1;
    return llmax(0, (S32)ceil(mDownedUntil - now()));
}

// Source: WolfGameModule.cs ShowWeb {t,url,mode,w,h,src}. The sim already refuses non-https;
// §3.8 says the viewer refuses it too and says so in the log.
void WolfGame::applyWeb(const LLSD& m)
{
    const std::string url = str(m, "url", CAP_URL);
    std::string host;
    if (!validWebUrl(url, host))
    {
        LL_WARNS("WolfGame") << "refused a game web page (not https, or its address is not plain): '" << url << "'" << LL_ENDL;
        return;
    }
    // [SECURITY 2026-10-09] A page from any script but the game's own (verified 0) is not loaded
    // until the player says Open; an unverified overlay (the sim refuses those) is a panel.
    const bool verified = !m.has("verified") || num(m, "verified") != 0;
    WolfFloaterGameWeb::showPage(url, str(m, "mode", CAP_LABEL), num32(m, "w"), num32(m, "h"), cleanSrc(str(m, "src")), verified);
}

// Source: WolfGameModule.cs Marker {t,id,region,x,y,z,label,c,sec}.
void WolfGame::applyMarker(const LLSD& m)
{
    const std::string id = str(m, "id", CAP_ID);
    if (id.empty()) return;
    Marker mk;
    mk.mId = id;
    mk.mRegion = str(m, "region", CAP_REGION);
    // [SECURITY 2026-10-09] Finite and within +-1e6 m, or the marker is refused.
    const F64 mx = real(m, "x"), my = real(m, "y"), mz = real(m, "z");
    if (fabs(mx) > 1e6 || fabs(my) > 1e6 || fabs(mz) > 1e6) return;
    mk.mPos.set((F32)mx, (F32)my, (F32)mz);
    // [SECURITY 2026-10-09] Which region sent it: markers from a region the player has left are
    // cleared on the next region change (onRegionChanged).
    if (LLViewerRegion* rgn = gAgent.getRegion()) mk.mSender = rgn->getRegionID();
    mk.mLabel = str(m, "label", CAP_TITLE);
    mk.mColor = parseColour(str(m, "c"), LLColor4(0.937f, 0.267f, 0.267f, 1.f));   // #ef4444, Marker()'s default
    const F64 sec = llclamp(real(m, "sec"), 0.0, 86400.0);   // WolfGameModule.cs Marker: 0..86400
    mk.mExpires = sec > 0.0 ? now() + sec : 0.0;
    mk.mSeq = ++mMarkerSeq;
    mMarkers[id] = mk;   // the same id replaces the marker
    while (mMarkers.size() > MAX_MARKERS)
    {
        auto oldest = mMarkers.begin();
        for (auto it = mMarkers.begin(); it != mMarkers.end(); ++it)
        {
            if (it->second.mSeq < oldest->second.mSeq) oldest = it;
        }
        mMarkers.erase(oldest);
    }
    resolveMarkers();
}

// Source: WolfGameModule.cs ItemMessage() {n,l,i,o,q} (sent with t:"item" by Exchange()).
// §2: q == 0 removes it from the list.
void WolfGame::applyItem(const LLSD& m)
{
    const std::string n = str(m, "n", CAP_NAME);
    if (n.empty()) return;
    const S64 q = bigNum(m, "q");
    auto it = std::find_if(mItems.begin(), mItems.end(), [&](const Item& i) { return i.mName == n; });
    if (q <= 0)
    {
        if (it != mItems.end()) mItems.erase(it);
        return;
    }
    if (it == mItems.end())
    {
        if (mItems.size() >= MAX_ITEMS) return;   // [SECURITY] count cap
        mItems.push_back(Item());
        it = mItems.end() - 1;
        it->mName = n;
    }
    it->mLabel = m.has("l") && !str(m, "l", CAP_NAME).empty() ? str(m, "l", CAP_NAME) : n;
    it->mIcon = iconStr(m, "i");
    it->mOrder = num32(m, "o");
    it->mQty = q;
    // [COMBAT] Source: WolfGameModule.cs ItemMessage {k, s}.
    it->mKind = str(m, "k", CAP_LABEL);
    it->mSlot = str(m, "s", CAP_LABEL);
    if (std::find(weaponSlots().begin(), weaponSlots().end(), it->mSlot) == weaponSlots().end()
        && std::find(armourSlots().begin(), armourSlots().end(), it->mSlot) == armourSlots().end())
    {
        it->mSlot.clear();
    }
    sortItems();
}

void WolfGame::sortItems()
{
    // Source: WolfGameModule.cs SendPlayer - by o, then the name (ordinal).
    std::sort(mItems.begin(), mItems.end(), [](const Item& a, const Item& b)
    {
        if (a.mOrder != b.mOrder) return a.mOrder < b.mOrder;
        return a.mName < b.mName;
    });
}

// Source: WolfGameModule.cs ShowPet {t:"pet",pet}; the pet object is wolfgame_lib.php
// wg_pet_payload(): key, owner, type, typeLabel, icon, name, alive, age, stats[{n,l,v,m,c,i}], data.
void WolfGame::applyPet(const LLSD& pe)
{
    if (!pe.isMap()) return;
    Pet p;
    p.mKey = str(pe, "key", CAP_ID);
    if (p.mKey.empty()) return;
    p.mOwner = str(pe, "owner", CAP_ID);
    p.mType = str(pe, "type", CAP_NAME);
    p.mTypeLabel = str(pe, "typeLabel", CAP_NAME);
    if (p.mTypeLabel.empty()) p.mTypeLabel = p.mType;
    p.mIcon = iconStr(pe, "icon");
    p.mName = str(pe, "name", CAP_NAME);
    p.mAlive = num(pe, "alive", 1) != 0;
    p.mAge = bigNum(pe, "age");
    p.mReceived = now();
    const LLSD& stats = pe["stats"];
    if (stats.isArray())
    {
        for (LLSD::array_const_iterator it = stats.beginArray(); it != stats.endArray(); ++it)
        {
            if (!it->isMap()) continue;
            Stat st;
            st.mName = str(*it, "n", CAP_LABEL);
            st.mLabel = str(*it, "l", CAP_LABEL).empty() ? st.mName : str(*it, "l", CAP_LABEL);
            st.mValue = num32(*it, "v");
            st.mMax = num32(*it, "m");
            st.mColor = parseColour(str(*it, "c"), LLColor4(0.518f, 0.800f, 0.086f, 1.f));
            st.mIcon = iconStr(*it, "i");
            p.mStats.push_back(st);
            if (p.mStats.size() >= MAX_PET_STATS) break;   // [SECURITY] count cap
        }
    }
    // §2: add or replace by key.
    auto it = std::find_if(mPets.begin(), mPets.end(), [&](const Pet& o) { return o.mKey == p.mKey; });
    if (it != mPets.end()) *it = p;
    else if (mPets.size() < MAX_PETS) mPets.push_back(p);   // [SECURITY] count cap
}

S64 WolfGame::Pet::ageNow() const
{
    return mAge + (S64)llmax(0.0, LLFrameTimer::getTotalSeconds() - mReceived);
}

// Source: WolfGameModule.cs SetTag / OnMakeRootAgent / Depart {t,agent,text,c}.
void WolfGame::applyTag(const LLSD& m)
{
    const LLUUID agent(str(m, "agent"));
    if (agent.isNull()) return;
    const std::string text = str(m, "text", CAP_TAG);
    if (text.empty())
    {
        auto it = mTags.find(agent);
        if (it != mTags.end())
        {
            if (it->second.mHud.notNull()) it->second.mHud->markDead();
            mTags.erase(it);
        }
        return;
    }
    Tag& t = mTags[agent];
    t.mSeq = ++mTagSeq;
    t.mText = text;
    t.mColor = parseColour(str(m, "c"), LLColor4::white);
    if (t.mHud.notNull()) setTagText(t.mHud, t.mText, t.mColor);
    while (mTags.size() > MAX_TAGS)
    {
        auto oldest = mTags.begin();
        for (auto it = mTags.begin(); it != mTags.end(); ++it)
        {
            if (it->second.mSeq < oldest->second.mSeq) oldest = it;
        }
        if (oldest->second.mHud.notNull()) oldest->second.mHud->markDead();
        mTags.erase(oldest);
    }
}

// Source: WolfGameModule.cs SetPanel {t,title,rows:[[label,value]...]}; §2 "Empty rows remove it".
void WolfGame::applyPanel(const LLSD& m)
{
    mPanelTitle = str(m, "title", CAP_TITLE);
    mPanelRows.clear();
    const LLSD& rows = m["rows"];
    if (rows.isArray())
    {
        for (LLSD::array_const_iterator it = rows.beginArray(); it != rows.endArray(); ++it)
        {
            if (!it->isArray() || it->size() < 2) continue;
            Row r;
            // WolfGameModule.cs SetPanel: Cut(label, MAX_LABEL), Cut(value, MAX_TITLE).
            r.mLabel = (*it)[0].isString() ? capText((*it)[0].asString(), CAP_LABEL) : std::string();
            r.mValue = (*it)[1].isString() ? capText((*it)[1].asString(), CAP_TITLE) : std::string();
            mPanelRows.push_back(r);
            if (mPanelRows.size() >= MAX_PANEL_ROWS) break;   // [SECURITY] WolfGameLimits.MAX_PANEL_ROWS
        }
    }
    if (mPanelRows.empty()) mPanelTitle.clear();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// region changes, logout, Game Mode
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGame::ensureRegion()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLUUID id = rgn ? rgn->getRegionID() : LLUUID::null;
    if (id == mRegionId) return;
    mRegionId = id;
    onRegionChanged();
}

void WolfGame::onRegionChanged()
{
    // §4: clear the stats, money, level, panel, buttons, downed state, dialogs and tags; keep
    // web pages and markers; effects clear too. Then wait for the new region's game / player,
    // and say hello about 3 s after arrival.
    clearScreen();
    mHelloAt = mRegionId.notNull() ? now() + HELLO_DELAY : 0.0;
    // [SECURITY 2026-10-09] Markers sent by another region go (scan item 11; this narrows
    // VIEWER_SPEC.md §3.9 "markers survive region changes" to markers from the region you are on).
    for (auto it = mMarkers.begin(); it != mMarkers.end();)
    {
        if (it->second.mSender != mRegionId) it = mMarkers.erase(it);
        else ++it;
    }
}

void WolfGame::clearPlayer()
{
    mPlaying = false;
    mStats.clear();
    mHasCurrency = false;
    mCurrencySymbol.clear();
    mCurrencyName.clear();
    mBalance = 0;
    mHasLevel = false;
    mLevel = 0;
    mXp = mXpNext = 0;
    mPanelTitle.clear();
    mPanelRows.clear();
    mButtons.clear();
    mButtonsSrc.clear();
    // Unknown until a "buttons" message says: the player message replays buttons without
    // src / verified (WolfGameModule.cs SendPlayer), so they are not assumed to be the game's.
    mButtonsVerified = false;
    mItems.clear();
    mPets.clear();
    clearCombat();
    mDead = false;
    mDowned = false;
    ++mGeneration;
}

void WolfGame::clearScreen()
{
    clearPlayer();
    mGame = Game();
    mDownedOn = false;
    mDownedText.clear();
    mDownedUntil = 0.0;
    mPending.clear();   // message ids are per simulator
    // [FORCED] §6: a forced region's lock ends when the avatar leaves it; the next region's
    // `game` says whether it forces too.
    mForced = false;
    mForcedText.clear();
    mLeavePendingAt = 0.0;
    updateGameModeButtons();
    closeDialogs();
    clearTags();
    clearEffects();
}

void WolfGame::clearAll()
{
    clearScreen();
    mGameModePending = false;
    mQueuedMode = -1;
    mGameMode = false;    // the region says again (the hello's answer); no stale "on" off / back on Wolf
    // [GAME HUD] Paul 2026-10-09: "when i start up i should ALWAYS start with the normal panel not
    // the roleplay one". A session (and a return to Wolf) starts with the HUD hidden even when the
    // region says Game Mode is on; switching Game Mode on, or the Game corner icon, shows it
    // (claimBottom).
    mHudHidden = true;
    mGuideOn = false;
    mNotices.clear();
    mMarkers.clear();
    mHelloAt = 0.0;
    WolfFloaterGameWeb::closePage();
    LLFloaterReg::hideInstance("wolf_roleplay");
    ++mGeneration;
}

void WolfGame::closeDialogs()
{
    WolfFloaterGameDialog::closeAll();
}

void WolfGame::clearTags()
{
    for (auto& kv : mTags)
    {
        if (kv.second.mHud.notNull()) kv.second.mHud->markDead();
    }
    mTags.clear();
}

/**
 * [WOLF GRID GATE 2026-10-09] The toolbar commands wolf_gamemode / wolf_roleplay: a toolbar lays
 * out hidden buttons too (lltoolbar.cpp), so off Wolf they are REMOVED, remembering where, and put
 * back in the same place on the way back (Paul: "the menu needs to adjust itself").
 * Same idea as lltoolbarview.cpp's login-time removal of wolf_dictate & co.
 */
void WolfGame::gateToolbar(bool on_wolf)
{
    // Only once the account's toolbars are loaded (llviewerwindow.cpp initWorldUI makes the view
    // visible after loadToolbars), and the per-account settings with them.
    if (!gToolBarView || !gToolBarView->getVisible()) return;
    // A layout saved while a button is off must not lose it (memory: "removing a toolbar command
    // ORPHANS it from saved layouts"), so where each removed button was is kept in a per-account
    // setting - across relaunches - and only cleared once it is back on a toolbar.
    LLSD hidden = gSavedPerAccountSettings.getLLSD("WolfGameToolbarHidden");
    if (!hidden.isArray()) hidden = LLSD::emptyArray();
    if (!on_wolf)
    {
        bool changed = false;
        for (const char* name : { "wolf_gamemode", "wolf_roleplay" })
        {
            int rank = LLToolBar::RANK_NONE;
            const S32 where = gToolBarView->removeCommand(LLCommandId(name), rank);
            if (where != LLToolBarEnums::TOOLBAR_NONE)
            {
                LLSD entry;
                entry["name"] = name;
                entry["toolbar"] = (LLSD::Integer)where;
                entry["rank"] = (LLSD::Integer)rank;
                hidden.append(entry);
                changed = true;
            }
        }
        if (changed) gSavedPerAccountSettings.setLLSD("WolfGameToolbarHidden", hidden);
        return;
    }
    if (hidden.size() == 0) return;
    // Back in reverse order, so each goes in at the rank it was taken from.
    for (S32 i = (S32)hidden.size() - 1; i >= 0; --i)
    {
        const LLSD& e = hidden[i];
        const std::string name = e["name"].asString();
        if (name != "wolf_gamemode" && name != "wolf_roleplay") continue;   // only ours, ever
        const S32 where = e["toolbar"].asInteger();
        if (where < LLToolBarEnums::TOOLBAR_FIRST || where > LLToolBarEnums::TOOLBAR_LAST) continue;
        const LLCommandId id(name);
        if (gToolBarView->hasCommand(id) == LLToolBarEnums::TOOLBAR_NONE)
        {
            gToolBarView->addCommand(id, (LLToolBarEnums::EToolBarLocation)where, e["rank"].asInteger());
        }
    }
    gSavedPerAccountSettings.setLLSD("WolfGameToolbarHidden", LLSD::emptyArray());
}

bool WolfGame::toggleReady() const
{
    // Source: WolfGameModule.cs VIEWER_TOGGLE_MS = 5000 - join / leave / gamemode inside that
    // window are dropped without a reply, so the switch would wait for nothing.
    return now() - mLastToggleAt >= 5.0;
}

bool WolfGame::requestGameMode(bool on)
{
    if (!WolfGrid::isOnWolfTerritories()) return false;
    if (mForced && !on) return false;
    if (setGameMode(on))
    {
        mQueuedMode = -1;
        return true;
    }
    // Not now (the region not ready, the last switch still being answered, or inside the region's
    // 5 s window): keep the wish; idle() sends it. The switches read "Connecting...".
    mQueuedMode = on ? 1 : 0;
    updateGameModeButtons();
    ++mGeneration;
    return true;
}

bool WolfGame::setGameMode(bool on)
{
    if (!WolfGrid::isOnWolfTerritories()) return false;   // [WOLF GRID GATE]
    // [FORCED] §6: the region requires it - the switch is locked on (the sim would refuse "0").
    if (mForced && !on) return false;
    if (mGameModePending || !toggleReady() || !gAgent.getRegion()) return false;
    // Not sent until this region has said it is Wolf (WolfGame::send drops it): do not go pending
    // on a message that never left - the switch would wait for an answer that cannot come.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return false;
    mGameModePending = true;
    mGameModeWanted = on;
    mGameModeAskedAt = now();
    mLastToggleAt = mGameModeAskedAt;
    // Source: WolfGameModule.cs OnViewerMessage "gamemode": on = args[1] == "1".
    send({ "gamemode", on ? "1" : "0" });
    updateGameModeButtons();
    ++mGeneration;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [GAME HUD 2026-10-09] the HUD across the bottom
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfGame::hudWanted() const
{
    if (!WolfGrid::isOnWolfTerritories()) return false;
    if (mForced || mGameMode || (mGameModePending && mGameModeWanted)) return true;
    // §5.8: a worn HUD may drive the displays with Game Mode off.
    return hasScreenStats() || !mAbilities.empty() || hasWeapons();
}

// static
bool WolfGame::bottomTaken()
{
    // Source (from the dashboard's author, wolfdrive.h): WolfDrive::dashboardOn(); wolfflight.h
    // active() / deckHidden() - the deck is on screen while active and not hidden.
    const WolfFlight& wf = WolfFlight::instance();
    return (wf.active() && !wf.deckHidden()) || WolfDrive::instance().dashboardOn();
}

bool WolfGame::hudShowing() const
{
    return hudWanted() && !mHudHidden && !bottomTaken();
}

void WolfGame::toggleHud()
{
    if (!WolfGrid::isOnWolfTerritories()) return;   // [WOLF GRID GATE]
    if (hudShowing())
    {
        mHudHidden = true;
        LL_INFOS("WolfGame") << "Game HUD hidden (Game Mode keeps running)" << LL_ENDL;
    }
    else
    {
        claimBottom();
        LL_INFOS("WolfGame") << "Game HUD shown" << LL_ENDL;
    }
    ++mGeneration;
}

void WolfGame::claimBottom()
{
    if (!WolfGrid::isOnWolfTerritories()) return;   // [WOLF GRID GATE] never touch the deck or dashboard off Wolf
    mHudHidden = false;
    // Source: wolfdrive.h requestDashboard(false) closes the dashboard, keeps WolfDashboardMode and
    // its menu tick in step, and does not reopen it for this seat.
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    // Source: wolfflight.cpp toggleDeck - on the mode that is up, it flips mDeckHidden only; the
    // mode and its autopilot keep running (the Plane / Boat icons bring the deck back).
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
    ++mGeneration;
}

void WolfGame::dismissGuide()
{
    mGuideOn = false;
    ++mGeneration;
}

bool WolfGame::leaveThisGame()
{
    if (!WolfGrid::isOnWolfTerritories()) return false;   // [WOLF GRID GATE]
    if (mForced) return false;   // [FORCED] §6: "Leave this game" is hidden and refused there
    if (!toggleReady() || !gAgent.getRegion()) return false;
    mLastToggleAt = now();
    mLeavePendingAt = mLastToggleAt;   // answered by a `game` with playing 0
    sendLeave();
    ++mGeneration;
    return true;
}

void WolfGame::clearEffectsByPlayer()
{
    // The player's own way out of any effect (a blackout or blur must never be undismissable).
    clearEffects();
    // [SECURITY 2026-10-09] ...and of the downed overlay, here only: the sim still holds them
    // (AllowMovement) and its next downed / player message is the truth.
    mDownedOn = false;
    mDownedText.clear();
    mDownedUntil = 0.0;
    ++mGeneration;
}

/**
 * The toolbar's Game Mode button (commands.xml wolf_gamemode; lltoolbar.cpp createButton names
 * it after the command) says On / Off in its label and tooltip, not only by being lit (§3.2).
 */
void WolfGame::updateGameModeButtons()
{
    if (!gToolBarView) return;
    const bool shown_on = gameModeShownOn();
    std::string label, tip;
    if (mForced)
    {
        // [FORCED] §6: ON and LOCKED - a padlock on the button, the reason in its words.
        label = "Game Mode: Locked on";
        tip = label + ". " + mForcedText + ".";
    }
    else
    {
        label = gameModeConnecting()
            ? std::string("Game Mode: connecting...")
            : std::string(shown_on ? "Game Mode: On" : "Game Mode: Off");
        tip = label + (gameModeConnecting() ? std::string()
            : std::string(shown_on ? ". Click to turn Game Mode off." : ". Click to play the region's roleplay game wherever you go."));
    }
    mButtonsShownState = label;
    for (S32 i = LLToolBarEnums::TOOLBAR_FIRST; i <= LLToolBarEnums::TOOLBAR_LAST; ++i)
    {
        LLToolBar* tb = gToolBarView->getToolbar((LLToolBarEnums::EToolBarLocation)i);
        if (!tb) continue;
        if (LLButton* b = tb->findChild<LLButton>("wolf_gamemode"))
        {
            b->setLabel(label);
            b->setToolTip(tip);
            // The command's own icon (commands.xml wolf_gamemode), or the skin's padlock
            // (textures.xml "Lock") while the region forces it; the same alignment either way.
            const std::string want = mForced ? "Lock" : "Command_WolfGameMode_Icon";
            LLPointer<LLUIImage> have = b->getImageOverlay();
            if (have.isNull() || have->getName() != want)
            {
                b->setImageOverlay(want, b->getImageOverlayHAlign());
            }
        }
    }
    // World > Game Mode says why it is greyed (§6 "the menu item checked and disabled, with the
    // same reason").
    if (gMenuBarView)
    {
        if (LLMenuItemGL* item = gMenuBarView->findChild<LLMenuItemGL>("WolfGameMode", true))
        {
            item->setLabel(mForced ? "Game Mode (" + mForcedText + ")" : std::string("Game Mode"));
        }
    }
}

bool WolfGame::handleEscape()
{
    if (!WolfGrid::isOnWolfTerritories()) return false;   // [WOLF GRID GATE 2026-10-09/]
    // The overlay page closes on Escape (§3.8) when nothing else has the keyboard, or when the
    // keyboard is its own; a game dialog closes when it has the keyboard (§3.5, sending nothing).
    if (WolfFloaterGameWeb::closeOverlayIfFocusAllows()) return true;
    if (WolfFloaterGameDialog::closeFocused()) return true;
    if (WolfFloaterGameWebPrompt::closeFocused()) return true;
    // A text field keeps its own Escape (chat, a notecard).
    LLUICtrl* focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    if (focus && focus->acceptsTextInput()) return false;
    // The screen effects and the downed overlay: the player's way out of a blackout or blur is
    // never blocked, not even by the Roleplay window having the keyboard (the review,
    // 2026-10-09: its "Clear screen effects" button says Escape does it too).
    bool fx_on = mDownedOn;   // [SECURITY 2026-10-09] the downed overlay too, locally
    for (const Effect& fx : mEffects) fx_on = fx_on || fx.mOn;
    if (fx_on)
    {
        clearEffectsByPlayer();
        return true;
    }
    // The Roleplay window, when it has the keyboard (the window itself or its console): its
    // question's safe answer, else it closes.
    if (WolfFloaterRoleplay::escapeFocused()) return true;
    // [COMBAT] §5.4 "Escape (when no text field has focus) sends cancel" while casting.
    if (mCast.mOn && mCast.mEnd.empty() && cancelCast()) return true;
    // [COMBAT] §5.5 "Escape with no other overlay ... sends target ''".
    if (mTarget.mOn && hudShowing())   // [FIX 2026-10-09/] the frame shows only with the HUD
    {
        clearTargetLocal();            // [FIX 2026-10-09/] at once, as the frame's X
        selectTarget(std::string());
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// per frame
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGame::idle()
{
    // [2026-10-09] A Game Mode switch that could not go at once goes as soon as the region can take
    // it - or is dropped when the region already says what was wanted.
    if (mQueuedMode >= 0 && !mGameModePending)
    {
        const bool want = mQueuedMode == 1;
        if (!WolfGrid::isOnWolfTerritories() || (mForced && !want) || want == mGameMode)
        {
            mQueuedMode = -1;
            updateGameModeButtons();
            ++mGeneration;
        }
        else if (setGameMode(want))
        {
            mQueuedMode = -1;
        }
    }
    // Logout: clear everything (§4), once.
    if (gDisconnected || LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent())
    {
        if (mRegionId.notNull() || !mTags.empty() || !mMarkers.empty() || !mNotices.empty())
        {
            clearAll();
            mRegionId.setNull();
        }
        sDesaturate = sBlur = 0.f;
        return;
    }
    // [WOLF GRID GATE 2026-10-09] Off Wolf Territories (another grid, or a hypergrid jump away)
    // nothing of the game shows or runs: everything is cleared once, the Game Mode / Roleplay
    // toolbar buttons come off, and the next arrival back on Wolf says hello again.
    {
        const bool wolf = WolfGrid::isOnWolfTerritories();
        if ((int)wolf != mOnWolf)
        {
            const bool was_known = mOnWolf >= 0;
            mOnWolf = wolf ? 1 : 0;
            if (!wolf)
            {
                clearAll();
            }
            // the toolbar buttons come off (or back) at once, not up to a second later
            if (LLStartUp::getStartupState() >= STATE_STARTED)
            {
                mLastGridGate = now();
                gateToolbar(wolf);
            }
            else if (was_known)
            {
                mHelloAt = now() + HELLO_DELAY;   // a message dropped while the gate was shut
            }
            ++mGeneration;
        }
        // Once a second: a layout loaded or a button dragged on while off Wolf is caught too.
        if (LLStartUp::getStartupState() >= STATE_STARTED && now() - mLastGridGate >= 1.0)
        {
            mLastGridGate = now();
            gateToolbar(wolf);
        }
        if (!wolf)
        {
            sDesaturate = sBlur = 0.f;
            return;
        }
    }
    ensureRegion();

    const F64 t = now();

    // §4 "On teleport, effects clear" - including a teleport inside the region.
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (teleporting && !mWasTeleporting) clearEffects();
    mWasTeleporting = teleporting;

    if (mHelloAt > 0.0 && t >= mHelloAt && gAgent.getRegion())
    {
        // Wait for this region's own word on its grid; a foreign one gets nothing.
        if (WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            mHelloAt = 0.0;
            sendHello();
        }
        else if (!WolfGrid::isOnWolfTerritories())
        {
            mHelloAt = 0.0;
        }
    }

    for (auto it = mPending.begin(); it != mPending.end();)
    {
        if (t - it->second.mFirst > PENDING_LIFE) it = mPending.erase(it);
        else ++it;
    }

    const size_t notices_before = mNotices.size();
    mNotices.erase(std::remove_if(mNotices.begin(), mNotices.end(),
                                  [&](const Notice& n) { return n.mExpires > 0.0 && t >= n.mExpires; }), mNotices.end());

    bool markers_changed = notices_before != mNotices.size();
    for (auto it = mMarkers.begin(); it != mMarkers.end();)
    {
        if (it->second.mExpires > 0.0 && t >= it->second.mExpires)
        {
            it = mMarkers.erase(it);
            markers_changed = true;
        }
        else ++it;
    }
    resolveMarkers();

    for (Effect& fx : mEffects)
    {
        if (fx.mOn && fx.mSeconds > 0.f && t - fx.mStart >= fx.mSeconds) fx = Effect();
    }
    sDesaturate = effectNow(FX_DESATURATE);
    sBlur = effectNow(FX_BLUR);

    // §3.2: a flip the region never answered (lost packet, a busy web service) goes back, and
    // says so, rather than waiting forever.
    if (mGameModePending && t - mGameModeAskedAt >= GAMEMODE_ANSWER)
    {
        mGameModePending = false;
        LLSD n;
        n["title"] = "Game Mode";
        n["text"] = std::string("The region did not answer, so Game Mode is still ") + (mGameMode ? "on" : "off") + ". Try again in a moment.";
        n["kind"] = "warning";
        applyNotify(n);
        ++mGeneration;
        updateGameModeButtons();
    }
    // A Leave the region never answered says so (the Game Mode flip's pattern above).
    if (mLeavePendingAt > 0.0 && t - mLeavePendingAt >= GAMEMODE_ANSWER)
    {
        mLeavePendingAt = 0.0;
        LLSD n;
        n["title"] = "Leave this game";
        n["text"] = mPlaying ? std::string("No answer from the region about leaving - you may still be playing. Try again in a moment.")
                             : std::string("You have left the game.");
        n["kind"] = mPlaying ? "warning" : "success";
        applyNotify(n);
        ++mGeneration;
    }
    // Buttons dragged onto a toolbar later get their On / Off label within a second.
    if (t - mLastButtonsUpdate >= 1.0)
    {
        mLastButtonsUpdate = t;
        updateGameModeButtons();
    }

    updateTags();
    tickCombat(t);
    if (markers_changed) ++mGeneration;
}

void WolfGame::resolveMarkers()
{
    const F64 t = now();
    for (auto& kv : mMarkers)
    {
        Marker& m = kv.second;
        // A region the viewer is connected to (this one or a neighbour) knows its own origin.
        LLViewerRegion* found = nullptr;
        for (LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
        {
            if (r && LLStringUtil::compareInsensitive(r->getName(), m.mRegion) == 0)
            {
                found = r;
                break;
            }
        }
        if (found)
        {
            m.mGlobal = found->getPosGlobalFromRegion(m.mPos);
            m.mHaveGlobal = true;
            continue;
        }
        // Any other region: the world map's lookup by name (§3.9). The reply fills LLWorldMap.
        LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromName(m.mRegion);
        if (info)
        {
            m.mGlobal = info->getGlobalOrigin() + LLVector3d(m.mPos);
            m.mHaveGlobal = true;
            continue;
        }
        m.mHaveGlobal = false;
        if (m.mRegion.empty()) continue;
        // [SECURITY 2026-10-09] One map-server question per region NAME every MAP_ASK_EVERY, however
        // many markers name it.
        std::string key = m.mRegion;
        LLStringUtil::toLower(key);
        auto asked = mMapAsked.find(key);
        if (asked != mMapAsked.end() && t - asked->second < MAP_ASK_EVERY) continue;
        mMapAsked[key] = t;
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(m.mRegion);
    }
    // Forget names no marker uses any more.
    for (auto it = mMapAsked.begin(); it != mMapAsked.end();)
    {
        bool used = false;
        for (const auto& kv : mMarkers)
        {
            if (LLStringUtil::compareInsensitive(kv.second.mRegion, it->first) == 0) { used = true; break; }
        }
        if (used) ++it;
        else it = mMapAsked.erase(it);
    }
}

/**
 * [SECURITY 2026-10-09] A game page's address, checked before anything uses it. LLURI and the
 * browser (WHATWG) can disagree about where the host is - "https://evil.com\\@wolf-grid.com/" or
 * "https://evil.com#@wolf-grid.com/" - so the authority (between "https://" and the first "/" or
 * the end) may hold none of \ # @ % ? whitespace or control characters, and must be
 * [A-Za-z0-9.-]+ with an optional :port. `host` is that authority's host, the one every label shows.
 */
bool WolfGame::validWebUrl(const std::string& url, std::string& host)
{
    host.clear();
    if (url.size() > CAP_URL || url.size() <= 8) return false;
    std::string scheme = url.substr(0, 8);
    LLStringUtil::toLower(scheme);
    if (scheme != "https://") return false;
    const size_t slash = url.find('/', 8);
    const std::string authority = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
    if (authority.empty()) return false;
    for (unsigned char c : authority)
    {
        if (c == '\\' || c == '#' || c == '@' || c == '%' || c == '?' || c <= 0x20 || c >= 0x7f) return false;
    }
    const size_t colon = authority.find(':');
    const std::string h = authority.substr(0, colon);
    if (h.empty() || h.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos) return false;
    if (colon != std::string::npos)
    {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
    }
    // [SECURITY 2026-10-09] Never a page on this computer or the player's own network (VIEWER_SPEC.md
    // §3.8; WolfStorm wolf_game.js _privateHost): localhost, *.local, *.internal, and loopback /
    // private / link-local / CGNAT IPv4. A host whose last label is a number is an IP literal - only
    // a plain dotted quad is read, and any other numeric form (2130706433, 0x7f.1, 127.1) refused.
    {
        std::string lh = h;
        LLStringUtil::toLower(lh);
        while (!lh.empty() && lh.back() == '.') lh.pop_back();
        auto ends = [&](const char* suffix)
        {
            const size_t n = strlen(suffix);
            return lh.size() >= n && lh.compare(lh.size() - n, n, suffix) == 0;
        };
        if (lh.empty() || lh == "localhost" || ends(".localhost") || ends(".local") || ends(".internal")) return false;
        const size_t last_dot = lh.rfind('.');
        const std::string last = last_dot == std::string::npos ? lh : lh.substr(last_dot + 1);
        const bool numeric_last = !last.empty() && (last.find_first_not_of("0123456789") == std::string::npos || last.rfind("0x", 0) == 0);
        if (numeric_last)
        {
            S32 part[4] = { 0, 0, 0, 0 };
            S32 n = 0;
            size_t from = 0;
            for (;;)
            {
                const size_t dot = lh.find('.', from);
                const std::string p = lh.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
                if (n >= 4 || p.empty() || p.size() > 3 || p.find_first_not_of("0123456789") != std::string::npos) return false;
                if (p.size() > 1 && p[0] == '0') return false;   // a leading 0 is octal to Chromium (012 = 10)
                part[n++] = std::stoi(p);
                if (part[n - 1] > 255) return false;
                if (dot == std::string::npos) break;
                from = dot + 1;
            }
            if (n != 4) return false;
            const S32 a = part[0], b = part[1];
            if (a == 0 || a == 10 || a == 127 || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31)
                || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127)
                || (a == 192 && b == 0 && part[2] == 0) || (a == 198 && (b == 18 || b == 19)) || a >= 224)   // reserved, benchmark, multicast, broadcast
            {
                return false;
            }
        }
    }
    host = h;
    return true;
}

bool WolfGame::markerGlobal(const Marker& m, LLVector3d& out)
{
    if (!m.mHaveGlobal) return false;
    out = m.mGlobal;
    return true;
}

/**
 * §3.10: one line under the avatar's name tag, drawn by its own LLHUDText. Nothing in the avatar
 * classes is changed (Paul, 2026-09-20): the avatar's name tag (LLVOAvatar::mNameText, public) is
 * only READ for where it is and whether it shows.
 */
/**
 * [SECURITY 2026-10-09] A game tag is drawn so it cannot pass for the viewer's own name-tag lines
 * (a display name, a group title): in italics and in parentheses.
 */
void WolfGame::setTagText(LLHUDText* hud, const std::string& text, const LLColor4& colour)
{
    hud->setColor(colour);
    hud->clearString();
    hud->addLine("(" + text + ")", colour, LLFontGL::ITALIC, LLFontGL::getFontSansSerifSmall());
}

void WolfGame::updateTags()
{
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    for (auto kit = mTags.begin(); kit != mTags.end();)
    {
        auto& kv = *kit;
        Tag& tag = kv.second;
        LLViewerObject* obj = gObjectList.findObject(kv.first);
        LLVOAvatar* av = (obj && obj->isAvatar() && !obj->isDead()) ? (LLVOAvatar*)obj : nullptr;
        // [SECURITY 2026-10-09] A tag is this region's: an avatar seen on another region (a
        // neighbour) gets none, and the tag is dropped.
        if (av && av->getRegion() != gAgent.getRegion())
        {
            if (tag.mHud.notNull()) tag.mHud->markDead();
            kit = mTags.erase(kit);
            continue;
        }
        ++kit;
        LLHUDNameTag* name_tag = av ? av->mNameText.get() : nullptr;
        // The name tag's anchor is its BOTTOM edge (llvoavatar.cpp: setVertAlignment(ALIGN_VERT_TOP),
        // llhudnametag.cpp renderText: the bubble spans 0..mHeight above mPositionAgent).
        const bool tag_shows = name_tag && name_tag->getVisible();
        if (!av)
        {
            if (tag.mHud.notNull())
            {
                tag.mHud->markDead();
                tag.mHud = nullptr;
            }
            continue;
        }
        if (tag.mHud.isNull())
        {
            tag.mHud = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            tag.mHud->setFont(font);
            tag.mHud->setTextAlignment(LLHUDText::ALIGN_TEXT_CENTER);
            tag.mHud->setVertAlignment(LLHUDText::ALIGN_VERT_CENTER);
            tag.mHud->setDoFade(false);
            tag.mHud->setMaxLines(1);
            setTagText(tag.mHud, tag.mText, tag.mColor);
        }
        if (!tag_shows)
        {
            tag.mHud->setHidden(true);
            continue;
        }
        const LLVector3 anchor = gAgent.getPosAgentFromGlobal(name_tag->getPositionGlobal());
        // No source object: an LLHUDText without one is always "visible", so behind the camera
        // it is hidden here.
        if ((anchor - cam->getOrigin()) * cam->getAtAxis() <= 0.f)
        {
            tag.mHud->setHidden(true);
            continue;
        }
        LLVector3 up, right;
        cam->getPixelVectors(anchor, up, right);
        const F32 drop_px = (F32)font->getLineHeight() * 0.5f + 3.f;   // the line's centre, just under the tag
        tag.mHud->setPositionAgent(anchor - up * drop_px);
        tag.mHud->setHidden(false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// camera shake (§3.6)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGame::unshakeCamera()
{
    if (mShakeApplied.isExactlyZero()) return;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    cam->setOrigin(cam->getOrigin() - mShakeApplied);
    mShakeApplied.clearVec();
}

void WolfGame::shakeCamera()
{
    const F32 amp = effectNow(FX_SHAKE);
    if (amp <= 0.f) return;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    // Two incommensurate wobbles a side: jittery, never a steady sway.
    const F64 t = now();
    const F32 jx = (F32)(0.6 * sin(t * 71.0) + 0.4 * sin(t * 113.0 + 1.3));
    const F32 jy = (F32)(0.6 * sin(t * 89.0 + 0.7) + 0.4 * sin(t * 131.0 + 2.1));
    mShakeApplied = (cam->getLeftAxis() * jx + cam->getUpAxis() * jy) * (SHAKE_METRES * amp);
    cam->setOrigin(cam->getOrigin() + mShakeApplied);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [COMBAT 2026-10-09] VIEWER_SPEC.md §5 - every field read from WolfGameModule.cs
// ═══════════════════════════════════════════════════════════════════════════════════════

const std::vector<std::string>& WolfGame::weaponSlots()
{
    // Source: WolfGameTypes.cs WolfGameLimits.SLOTS - the first three are the weapons.
    static const std::vector<std::string> s = { "main", "off", "ranged" };
    return s;
}

const std::vector<std::string>& WolfGame::armourSlots()
{
    // Source: WolfGameTypes.cs WolfGameLimits.SLOTS (head, body, hands, legs, feet, neck, ring),
    // in VIEWER_SPEC.md §5.3's paper-doll order.
    static const std::vector<std::string> s = { "head", "neck", "body", "hands", "legs", "feet", "ring" };
    return s;
}

std::string WolfGame::slotLabel(const std::string& slot)
{
    if (slot == "main") return "Main hand";
    if (slot == "off") return "Off hand";
    if (slot == "ranged") return "Ranged";
    if (slot == "head") return "Head";
    if (slot == "neck") return "Neck";
    if (slot == "body") return "Body";
    if (slot == "hands") return "Hands";
    if (slot == "legs") return "Legs";
    if (slot == "feet") return "Feet";
    if (slot == "ring") return "Ring";
    return slot;
}

namespace
{
    bool knownSlot(const std::string& slot)
    {
        const auto& w = WolfGame::weaponSlots();
        const auto& a = WolfGame::armourSlots();
        return std::find(w.begin(), w.end(), slot) != w.end() || std::find(a.begin(), a.end(), slot) != a.end();
    }
}

void WolfGame::clearCombat()
{
    mLoadout.clear();
    mGear.clear();
    mAmmo.clear();
    mAbilities.clear();
    mCast = Cast();
    mTarget = Target();
    mBuffs.clear();
    mCombatText.clear();
    mCombatError.clear();
    mCombatErrorUntil = 0.0;
    mActiveWeapon.clear();
    mEquipPending.clear();
}

// Source: WolfGameModule.cs LoadoutJson {slot: {n,l,i,k,two,dmg,rng,fm,mag,ammo,def}} - the WHOLE set.
void WolfGame::applyLoadout(const LLSD& slots)
{
    mLoadout.clear();
    mEquipPending.clear();   // §5.3: the next loadout ends the pending state
    if (!slots.isMap()) return;
    for (LLSD::map_const_iterator it = slots.beginMap(); it != slots.endMap(); ++it)
    {
        if (!knownSlot(it->first) || !it->second.isMap()) continue;
        const LLSD& e = it->second;
        LoadoutItem li;
        li.mName = str(e, "n", CAP_NAME);
        if (li.mName.empty()) continue;
        li.mLabel = str(e, "l", CAP_NAME);
        if (li.mLabel.empty()) li.mLabel = li.mName;
        li.mIcon = iconStr(e, "i");
        li.mKind = str(e, "k", CAP_LABEL);
        li.mTwoHand = num(e, "two") != 0;
        // WolfGameTypes.cs ItemDef: damage 24, range 16, firemode 16, defence 24, magazine 0..10000.
        li.mDamage = str(e, "dmg", 24);
        li.mRange = str(e, "rng", 16);
        li.mFireMode = str(e, "fm", 16);
        li.mMagazine = (S32)llclamp(num(e, "mag"), (S64)0, (S64)10000);
        li.mAmmo = str(e, "ammo", CAP_NAME);
        li.mDefence = str(e, "def", 24);
        mLoadout[it->first] = li;
    }
}

// Source: WolfGameModule.cs GearJson {l,i,dmg,rng,fm,def,ammoName,mag,src,verified} or null.
void WolfGame::applyGear(const std::string& slot, const LLSD& g)
{
    if (!knownSlot(slot)) return;
    if (!g.isMap())
    {
        mGear.erase(slot);
        return;
    }
    LLSD out = LLSD::emptyMap();
    // Only the fields a script set override the loadout item (§5.1 "Gear").
    auto keep = [&](const char* k, size_t cap)
    {
        const std::string v = str(g, k, cap);
        if (!v.empty()) out[k] = v;
    };
    keep("l", CAP_NAME);
    keep("dmg", 24);
    keep("rng", 16);
    keep("fm", 16);
    keep("def", 24);
    keep("ammoName", CAP_NAME);
    const std::string icon = iconStr(g, "i");
    if (!icon.empty()) out["i"] = icon;
    const S64 mag = llclamp(num(g, "mag"), (S64)0, (S64)10000);
    if (mag > 0) out["mag"] = (LLSD::Integer)mag;
    out["src"] = cleanSrc(str(g, "src"));
    out["verified"] = (!g.has("verified") || num(g, "verified") != 0) ? 1 : 0;
    mGear[slot] = out;
}

// Source: WolfGameModule.cs AmmoJson {a, m, r, rl, rt}.
void WolfGame::applyAmmo(const std::string& slot, const LLSD& m)
{
    if (!knownSlot(slot) || !m.isMap()) return;
    Ammo a;
    a.mRounds = (S32)llclamp(num(m, "a", -1), (S64)-1, (S64)1000000);
    a.mMagazine = (S32)llclamp(num(m, "m"), (S64)0, (S64)1000000);
    a.mReserve = (S32)llclamp(num(m, "r", -1), (S64)-1, (S64)100000000);
    const F64 rl = llclamp(real(m, "rl"), 0.0, 600.0);
    a.mReloadTotal = (F32)llclamp(real(m, "rt"), 0.0, 600.0);
    a.mReloadEnds = rl > 0.0 ? now() + rl : 0.0;
    // A slot with nothing to count and no reload says nothing.
    if (a.mRounds < 0 && a.mReserve < 0 && a.mReloadEnds <= 0.0) mAmmo.erase(slot);
    else mAmmo[slot] = a;
}

// Source: WolfGameModule.cs AbilityJson {id,l,i,cd,left,cost,costL,amt,cast,key,on,grp,wpn,ammo,tgt,o,src,verified}.
void WolfGame::applyAbility(const LLSD& m)
{
    if (!m.isMap()) return;
    const std::string id = str(m, "id", CAP_ID);
    if (id.empty()) return;
    auto it = std::find_if(mAbilities.begin(), mAbilities.end(), [&](const Ability& a) { return a.mId == id; });
    if (it == mAbilities.end())
    {
        if (mAbilities.size() >= MAX_ABILITIES) return;   // [SECURITY] WolfGameLimits.MAX_ABILITIES
        mAbilities.push_back(Ability());
        it = mAbilities.end() - 1;
        it->mId = id;
        it->mArrival = ++mAbilityArrival;
    }
    Ability& a = *it;
    a.mLabel = str(m, "l", CAP_LABEL);
    if (a.mLabel.empty()) a.mLabel = id;
    a.mIcon = iconStr(m, "i");
    a.mCooldown = (F32)llclamp(real(m, "cd"), 0.0, 86400.0);
    const F64 left = llclamp(real(m, "left"), 0.0, 86400.0);
    a.mCooldownEnds = left > 0.0 ? now() + left : 0.0;
    a.mCooldownTotal = (F32)llmax((F64)a.mCooldown, left);
    a.mCost = str(m, "cost", CAP_LABEL);
    a.mCostLabel = str(m, "costL", CAP_LABEL);
    a.mAmount = num32(m, "amt");
    a.mCast = (F32)llclamp(real(m, "cast"), 0.0, 600.0);
    a.mKey = str(m, "key", CAP_KEY);
    a.mOn = !m.has("on") || num(m, "on") != 0;
    a.mGroup = str(m, "grp", CAP_ID);
    a.mWeapon = str(m, "wpn", CAP_LABEL);
    if (!knownSlot(a.mWeapon)) a.mWeapon.clear();
    a.mAmmoPerUse = (S32)llclamp(num(m, "ammo"), (S64)0, (S64)1000);
    a.mNeedsTarget = num(m, "tgt") != 0;
    a.mOrder = num32(m, "o");
    a.mSrc = cleanSrc(str(m, "src"));
    a.mVerified = !m.has("verified") || num(m, "verified") != 0;
    sortAbilities();
}

void WolfGame::sortAbilities()
{
    // §5.4: sorted by o, then by arrival order.
    std::stable_sort(mAbilities.begin(), mAbilities.end(), [](const Ability& a, const Ability& b)
    {
        if (a.mOrder != b.mOrder) return a.mOrder < b.mOrder;
        return a.mArrival < b.mArrival;
    });
}

// Source: WolfGameModule.cs Cooldown {id, cd, left}: left 0 ends it.
void WolfGame::applyCooldown(const LLSD& m)
{
    const std::string id = str(m, "id", CAP_ID);
    for (Ability& a : mAbilities)
    {
        if (a.mId != id) continue;
        const F64 left = llclamp(real(m, "left"), 0.0, 86400.0);
        const F32 cd = (F32)llclamp(real(m, "cd"), 0.0, 86400.0);
        if (left > 0.0)
        {
            a.mCooldownEnds = now() + left;
            a.mCooldownTotal = (F32)llmax((F64)cd, left);
        }
        else
        {
            if (a.mCooldownEnds > 0.0) a.mReadyAt = now();
            a.mCooldownEnds = 0.0;
        }
        if (!a.mWeapon.empty()) mActiveWeapon = a.mWeapon;
    }
}

// Source: WolfGameModule.cs cast {id,l,sec,left} and {id, sec:0, end}.
void WolfGame::applyCast(const LLSD& m)
{
    const std::string end = str(m, "end", CAP_LABEL);
    if (!end.empty() || real(m, "sec") <= 0.0)
    {
        if (!mCast.mOn) return;
        mCast.mEnd = (end == "interrupted" || end == "cancelled") ? end : std::string("done");
        mCast.mEndedAt = now();
        return;
    }
    mCast = Cast();
    mCast.mOn = true;
    mCast.mId = str(m, "id", CAP_ID);
    mCast.mLabel = str(m, "l", CAP_LABEL);
    mCast.mSeconds = (F32)llclamp(real(m, "sec"), 0.0, 600.0);
    mCast.mEnds = now() + llclamp(real(m, "left"), 0.0, (F64)mCast.mSeconds);
    for (const Ability& a : mAbilities)
    {
        if (a.mId == mCast.mId && !a.mWeapon.empty()) mActiveWeapon = a.mWeapon;
    }
}

// Source: WolfGameModule.cs CombatError {text, id?}.
void WolfGame::applyCombatError(const LLSD& m)
{
    const std::string text = str(m, "text", CAP_TITLE * 2);
    if (text.empty()) return;
    const F64 t = now();
    // §5.4: "the same text again within 1 s is not repeated".
    if (text == mCombatError && t - mCombatErrorShownAt < COMBAT_ERROR_REPEAT) return;
    mCombatError = text;
    mCombatErrorShownAt = t;
    mCombatErrorUntil = t + COMBAT_ERROR_LIFE;
}

// Source: WolfGameModule.cs TargetJson {key,kind,name,hp,m,lv,host,i,sub,live,dead,src,verified}.
void WolfGame::applyTarget(const LLSD& m)
{
    const std::string key = str(m, "key", CAP_ID);
    if (key.empty())
    {
        mTarget = Target();
        return;
    }
    Target t;
    t.mOn = true;
    t.mKey = key;
    t.mKind = str(m, "kind", CAP_LABEL);
    t.mName = str(m, "name", CAP_TITLE);
    t.mHealth = (S32)llclamp(num(m, "hp", -1), (S64)-1, (S64)100000000);
    t.mMax = (S32)llclamp(num(m, "m"), (S64)0, (S64)100000000);
    t.mLevel = (S32)llclamp(num(m, "lv"), (S64)0, (S64)100000);
    const std::string host = str(m, "host", CAP_LABEL);
    // WolfGameTypes.cs WolfGameLimits.HOSTILITY.
    t.mHost = (host == "hostile" || host == "friendly") ? host : std::string("neutral");
    t.mIcon = iconStr(m, "i");
    t.mSub = str(m, "sub", CAP_TITLE);
    t.mLive = num(m, "live") != 0;
    t.mDead = num(m, "dead") != 0;
    t.mSrc = cleanSrc(str(m, "src"));
    t.mVerified = !m.has("verified") || num(m, "verified") != 0;
    mTarget = t;
}

// Source: WolfGameModule.cs targethp {key, hp, m, dead} - only for the current target.
void WolfGame::applyTargetHealth(const LLSD& m)
{
    if (!mTarget.mOn || str(m, "key", CAP_ID) != mTarget.mKey) return;
    mTarget.mHealth = (S32)llclamp(num(m, "hp", -1), (S64)-1, (S64)100000000);
    mTarget.mMax = (S32)llclamp(num(m, "m"), (S64)0, (S64)100000000);
    mTarget.mDead = num(m, "dead") != 0;
}

// Source: WolfGameModule.cs BuffJson {id,l,i,good,n,dur,left,text,src,verified}.
void WolfGame::applyBuff(const LLSD& m)
{
    if (!m.isMap()) return;
    const std::string id = str(m, "id", CAP_ID);
    if (id.empty()) return;
    auto it = std::find_if(mBuffs.begin(), mBuffs.end(), [&](const Buff& b) { return b.mId == id; });
    if (it == mBuffs.end())
    {
        if (mBuffs.size() >= MAX_BUFFS) return;   // [SECURITY] WolfGameLimits.MAX_BUFFS
        mBuffs.push_back(Buff());
        it = mBuffs.end() - 1;
        it->mId = id;
    }
    Buff& b = *it;
    b.mLabel = str(m, "l", CAP_LABEL);
    if (b.mLabel.empty()) b.mLabel = id;
    b.mIcon = iconStr(m, "i");
    b.mGood = num(m, "good", 1) != 0;
    b.mStacks = (S32)llclamp(num(m, "n", 1), (S64)1, (S64)999);
    b.mDuration = (F32)llclamp(real(m, "dur"), 0.0, 86400.0);
    const F64 left = llclamp(real(m, "left"), 0.0, 86400.0);
    b.mEnds = b.mDuration > 0.f ? now() + left : 0.0;
    b.mText = str(m, "text", CAP_TEXT, true);
    b.mSrc = cleanSrc(str(m, "src"));
    b.mVerified = !m.has("verified") || num(m, "verified") != 0;
    // §5.6 "Buffs first, then debuffs" - kept in that order, arrival order within each.
    std::stable_sort(mBuffs.begin(), mBuffs.end(), [](const Buff& x, const Buff& y) { return x.mGood && !y.mGood; });
}

// Source: WolfGameModule.cs CombatText {at, amt, k, c, self}.
void WolfGame::applyCombatText(const LLSD& m)
{
    const std::string k = str(m, "k", CAP_LABEL);
    // WolfGameTypes.cs WolfGameLimits.COMBAT_TEXT.
    static const char* KINDS[] = { "hit", "crit", "heal", "miss", "dodge", "block", "immune", "absorb" };
    if (std::none_of(std::begin(KINDS), std::end(KINDS), [&](const char* x) { return k == x; })) return;
    CombatText c;
    c.mAt.set(str(m, "at", CAP_ID), false);
    c.mAmount = llclamp(num(m, "amt"), (S64)0, (S64)100000000);
    c.mKind = k;
    const std::string col = str(m, "c");
    if (!col.empty())
    {
        c.mColor = parseColour(col, LLColor4::white);
        c.mHasColor = true;
    }
    c.mSelf = num(m, "self") != 0;
    c.mBorn = now();
    c.mSerial = ++mCombatTextSerial;
    mCombatText.push_back(c);
    while (mCombatText.size() > MAX_COMBAT_TEXT) mCombatText.pop_front();   // §5.6 the oldest goes first
    if (!c.mSelf && (k == "hit" || k == "crit"))
    {
        mHitMarkerAt = c.mBorn;
        mHitMarkerCrit = k == "crit";
    }
}

void WolfGame::tickCombat(F64 t)
{
    bool changed = false;
    // §5.6 "At left = 0 the viewer removes it itself".
    const size_t buffs = mBuffs.size();
    mBuffs.erase(std::remove_if(mBuffs.begin(), mBuffs.end(), [&](const Buff& b) { return b.mEnds > 0.0 && t >= b.mEnds; }), mBuffs.end());
    changed = changed || buffs != mBuffs.size();
    while (!mCombatText.empty() && t - mCombatText.front().mBorn > COMBAT_TEXT_LIFE)
    {
        mCombatText.pop_front();
    }
    for (Ability& a : mAbilities)
    {
        if (a.mCooldownEnds > 0.0 && t >= a.mCooldownEnds)
        {
            a.mCooldownEnds = 0.0;
            a.mReadyAt = t;   // the brief "ready" pulse
            changed = true;
        }
    }
    for (auto& kv : mAmmo)
    {
        if (kv.second.mReloadEnds > 0.0 && t >= kv.second.mReloadEnds)
        {
            kv.second.mReloadEnds = 0.0;
            changed = true;
        }
    }
    if (mCast.mOn && !mCast.mEnd.empty() && t - mCast.mEndedAt > CAST_END_SHOW)
    {
        mCast = Cast();   // "Interrupted" / "Cancelled" stays for 1 s (§5.4)
        changed = true;
    }
    if (!mCombatError.empty() && t >= mCombatErrorUntil)
    {
        mCombatError.clear();
        changed = true;
    }
    if (!mEquipPending.empty() && t - mEquipPendingAt > EQUIP_ANSWER)
    {
        mEquipPending.clear();   // no answer: the slot shows the truth again
        changed = true;
    }
    if (changed) ++mGeneration;
}

WolfGame::SlotView WolfGame::slotView(const std::string& slot) const
{
    SlotView v;
    auto li = mLoadout.find(slot);
    if (li != mLoadout.end())
    {
        const LoadoutItem& i = li->second;
        v.mFilled = true;
        v.mItem = i.mName;
        v.mLabel = i.mLabel;
        v.mIcon = i.mIcon;
        v.mDamage = i.mDamage;
        v.mRange = i.mRange;
        v.mFireMode = i.mFireMode;
        v.mDefence = i.mDefence;
        v.mAmmoName = i.mAmmo;
        v.mMagazine = i.mMagazine;
    }
    auto g = mGear.find(slot);
    if (g != mGear.end())
    {
        const LLSD& f = g->second;
        v.mFilled = true;
        v.mFromGear = true;
        if (f.has("l")) v.mLabel = f["l"].asString();
        if (f.has("i")) v.mIcon = f["i"].asString();
        if (f.has("dmg")) v.mDamage = f["dmg"].asString();
        if (f.has("rng")) v.mRange = f["rng"].asString();
        if (f.has("fm")) v.mFireMode = f["fm"].asString();
        if (f.has("def")) v.mDefence = f["def"].asString();
        if (f.has("ammoName")) v.mAmmoName = f["ammoName"].asString();
        if (f.has("mag")) v.mMagazine = f["mag"].asInteger();
        v.mSrc = f["src"].asString();
        v.mVerified = f["verified"].asInteger() != 0;
    }
    return v;
}

const WolfGame::Ammo* WolfGame::ammoFor(const std::string& slot) const
{
    auto it = mAmmo.find(slot);
    return it == mAmmo.end() ? nullptr : &it->second;
}

bool WolfGame::hasWeapons() const
{
    for (const std::string& s : weaponSlots())
    {
        if (mLoadout.count(s) || mGear.count(s)) return true;
    }
    return false;
}

// ---- viewer -> sim (§5.7) ----

bool WolfGame::useAbility(const std::string& id)
{
    if (!gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    for (Ability& a : mAbilities)
    {
        if (a.mId != id) continue;
        a.mPressedAt = now();   // §5.4 "a 100 ms pressed flash"; the sweep waits for the sim
        if (!a.mWeapon.empty()) mActiveWeapon = a.mWeapon;
    }
    send({ "use", id });
    ++mGeneration;
    return true;
}

bool WolfGame::cancelCast()
{
    if (!gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    send({ "cancel" });
    return true;
}

bool WolfGame::reload(const std::string& slot)
{
    if (!knownSlot(slot) || !gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    send({ "reload", slot });
    return true;
}

bool WolfGame::equip(const std::string& item)
{
    // §5.3 "At most one request in flight".
    if (!mEquipPending.empty() || item.empty() || !gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    for (const Item& i : mItems)
    {
        if (i.mName == item) mEquipPending = i.mSlot.empty() ? item : i.mSlot;
    }
    if (mEquipPending.empty()) mEquipPending = item;
    mEquipPendingAt = now();
    send({ "equip", item });
    ++mGeneration;
    return true;
}

bool WolfGame::unequip(const std::string& slot)
{
    if (!mEquipPending.empty() || !knownSlot(slot) || !gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    mEquipPending = slot;
    mEquipPendingAt = now();
    send({ "unequip", slot });
    ++mGeneration;
    return true;
}

bool WolfGame::selectTarget(const std::string& key)
{
    if (!gAgent.getRegion() || !WolfGrid::isOnWolfTerritories()) return false;
    // An empty key goes as an empty parameter - just its NUL (llviewergenericmessage.cpp
    // send_generic_message adds every string), which Util.FieldToString reads as "" (§5.7).
    send({ "target", key });
    return true;
}

bool WolfGame::handleActionKey(KEY key, MASK mask)
{
    // §5.4: keys 1-9, 0, -, = for the first 12 buttons; only in Game Mode, with buttons, and
    // with no text field holding the keyboard. R (reload) is NOT taken: the default key
    // bindings use it for roll_left in third person and sitting (app_settings/key_bindings.xml),
    // and the viewer's own binding wins - Reload is the button on each weapon card.
    if (mask != MASK_NONE || mAbilities.empty() || !(mGameMode || mForced)) return false;
    // A held key is one press: auto-repeat would only spam "Not ready yet" refusals.
    if (gKeyboard && gKeyboard->getKeyRepeated(key)) return false;
    if (!WolfGrid::isOnWolfTerritories()) return false;
    LLUICtrl* focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    if (focus && focus->acceptsTextInput()) return false;
    S32 index = -1;
    if (key >= '1' && key <= '9') index = key - '1';
    else if (key == '0') index = 9;
    // llkeyboardwin32.cpp maps VK_OEM_MINUS / VK_OEM_PLUS to '-' / '='; llkeyboardsdl2.cpp maps
    // SDLK_MINUS to '-' but SDLK_EQUALS to KEY_EQUALS (and llkeyboardsdl.cpp KEY_HYPHEN for minus).
    else if (key == '-' || key == KEY_HYPHEN) index = 10;
    else if (key == '=' || key == KEY_EQUALS) index = 11;
    if (index < 0 || index >= (S32)mAbilities.size()) return false;
    return useAbility(mAbilities[index].mId);
}

void WolfGame::releaseTags()
{
    clearTags();
    // ...and the game UI's textures and the background picture, before the texture list goes.
    WolfGameDraw::releaseTextures();
    WolfGameBackground::release();
}
