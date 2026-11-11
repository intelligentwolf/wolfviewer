/**
 * @file wolffishingoverlay.cpp
 * @brief WolfViewer: Fishing on the screen (ticket 679, 2026-11-11). Design: wolffishingoverlay.h.
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

#include "wolffishingoverlay.h"

#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llappearancemgr.h"
#include "llchat.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llrand.h"
#include "llrender.h"
#include "lluiimage.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llwindow.h"
#include "wolfcave.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfFishingHud> r_wolf_fishing_hud("wolf_fishing_hud");

namespace
{
    // Source: lsl/fish_rod.lsl VCH = -76790 (the window's channel; negative = a ScriptDialogReply, heard region-wide from the
    // avatar: OpenSim LLClientView.cs HandlerScriptDialogReply)
    constexpr S32 VCH = -76790;
    // Source: wolffloaterfishing.cpp ROD_DESC / build_fishing.py ROD_DESC / wolffishing_lib.php WF_DESC_PREFIX
    const std::string ROD_DESC = "wolffish-rod";
    // Source: wolffloaterfishing.cpp API_URL (php/wolffishing.php: {"action":"give"}, {"action":"week"})
    const char* API_URL = "https://wolfstorm.app/php/wolffishing.php";
    constexpr F32 API_TIMEOUT_S = 30.f;
    // [REVIEW 11-11 item 1] after Cast / Reel / Stop the buttons wait for the rod's answer; with none in this time the rod is
    // asked "hello" again (it answers "hi" with its real state: fish_rod.lsl listen "hello"), and after HELLO_TRIES unanswered
    // hellos it is treated as gone. CHOSEN: the rod's own server request can take a while (llHTTPRequest, then its second URL),
    // and a busy rod answers "hi|busy", which starts the wait again, so a slow server is never mistaken for a lost rod.
    constexpr F64 BUSY_HELLO_S = 15.0;
    constexpr S32 HELLO_TRIES = 2;
    // Source: fish_rod.lsl reel(): the rod's text when a reel comes before the bite, and the server's line kinds
    const std::string RARE_WORDS = "A rare catch!";              // wolffishing.php op=reel: ($fish['rare'] ? ' A rare catch!' : '')
    const std::string BIGGEST_WORDS = "Your biggest this week!";  // wolffishing.php op=reel: if ($res['best']) ...

    // CHOSEN game tuning (Challenge mode; Paul 11-11 "yes challenge mode"): a short game of about five seconds' holding.
    constexpr F32 T_START = 0.25f;      // the line's tension when the game starts (0..1)
    constexpr F32 T_RISE = 0.28f;       // a second of holding Reel in raises it this much ...
    constexpr F32 T_PULL_MAX = 0.42f;   // ... plus the fish's pull, 0 to this, changing every PULL_MIN_S..PULL_MAX_S
    constexpr F32 T_FALL = 0.45f;       // a second of letting go lowers it this much
    constexpr F32 LAND_RATE = 0.22f;    // a second of holding brings in this much of the line (about 4.5 s in all)
    constexpr F32 PULL_MIN_S = 0.6f, PULL_MAX_S = 1.6f;
    constexpr F32 PULL_GRACE_S = 0.6f;  // the fish does not pull in the first moment of the game

    bool onWolf() { return WolfGrid::isOnWolfTerritories(); }

    // Source: wolffloaterfishing.cpp RodItems (the window's test, kept as it was): an object item (not a link) whose
    // description starts "wolffish-rod".
    class RodItems : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_OBJECT
                || item->getInventoryType() != LLInventoryType::IT_OBJECT) return false;
            std::string d = item->getDescription();
            LLStringUtil::trimHead(d);
            LLStringUtil::toLower(d);
            if (d.compare(0, ROD_DESC.size(), ROD_DESC) != 0) return false;
            return d.size() == ROD_DESC.size() || !(isalnum((unsigned char)d[ROD_DESC.size()]) || d[ROD_DESC.size()] == '_' || d[ROD_DESC.size()] == '-');
        }
    };

    /** The rod in the inventory (outside the Trash): one being worn first, else the first found. */
    LLViewerInventoryItem* findRod()
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        RodItems match;
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
        LLViewerInventoryItem* first = nullptr;
        for (const LLPointer<LLViewerInventoryItem>& p : items)
        {
            if (p.isNull() || !p->isFinished()) continue;
            if (get_is_item_worn(p->getUUID())) return p.get();
            if (!first) first = p.get();
        }
        return first;
    }

    /** "a|b||c" -> {"a", "b", "", "c"} (empty fields kept). */
    std::vector<std::string> fields(const std::string& s)
    {
        std::vector<std::string> out;
        size_t at = 0;
        for (;;)
        {
            size_t bar = s.find('|', at);
            out.push_back(s.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        return out;
    }

    std::string fieldAt(const std::vector<std::string>& f, size_t i) { return i < f.size() ? f[i] : std::string(); }

    /** Everything from field i on, joined again (a text that held a "|" stays whole). */
    std::string restFrom(const std::vector<std::string>& f, size_t i)
    {
        std::string out;
        for (size_t k = i; k < f.size(); ++k) { if (k > i) out += "|"; out += f[k]; }
        return out;
    }

    // Source: wolffishing_lib.php wf_ordinal(): 1st 2nd 3rd 4th ... 11th 12th 13th ... 21st
    std::string ordinal(S32 n)
    {
        const char* s = "th";
        if (n % 100 < 11 || n % 100 > 13)
        {
            if (n % 10 == 1) s = "st";
            else if (n % 10 == 2) s = "nd";
            else if (n % 10 == 3) s = "rd";
        }
        return llformat("%d%s", n, s);
    }

    /** The bracketed part of a server text ("(You have caught 300 fish today - more today are just for fun.)"), or "". */
    std::string bracketed(const std::string& text)
    {
        const size_t a = text.rfind('(');
        const size_t b = text.rfind(')');
        if (a == std::string::npos || b == std::string::npos || b < a) return std::string();
        return text.substr(a + 1, b - a - 1);
    }
    // the message area keeps this many messages this session (Paul 11-11: a history to scroll back through; bounded)
    constexpr size_t LOG_KEPT = 50;

    // Source: wolffishing_lib.php wf_article(): "an" before a vowel, else "a"
    std::string articled(const std::string& name)
    {
        const char c = name.empty() ? 'x' : (char)tolower((unsigned char)name[0]);
        return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' ? "an " : "a ") + name;
    }

    // ── today's catch: how each fish looks on the pile (Paul 11-11: "a pile of fish as they catch them") ──
    // A few looks by family, matched on the server's fish names (wolffishing_lib.php WF_FISH), first match wins. CHOSEN
    // colours, our own; the shape: an ordinary fish, a long one (pike, eel, garfish), a deep one (bream, snapper,
    // triggerfish), a flat one (plaice, turbot, ray).
    enum EShape { SHAPE_FISH = 0, SHAPE_LONG, SHAPE_DEEP, SHAPE_FLAT };
    enum EPattern { PATTERN_NONE = 0, PATTERN_STRIPES, PATTERN_SPOTS };
    enum EJunk { JUNK_BOOT = 0, JUNK_HAT, JUNK_WEED, JUNK_WOOD };
    struct FishLook
    {
        const char* mKey;            // a word of the fish's name, lower case ("" = any: the last row)
        S32 mShape;
        S32 mPattern;
        LLColor4 mBody, mFin;
    };
    const FishLook FISH_LOOKS[] =
    {
        { "mackerel",    SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.20f, 0.58f, 0.62f, 1.f), LLColor4(0.06f, 0.22f, 0.32f, 1.f) },
        { "tuna",        SHAPE_FISH, PATTERN_NONE,    LLColor4(0.22f, 0.32f, 0.58f, 1.f), LLColor4(1.00f, 0.82f, 0.25f, 1.f) },
        { "wahoo",       SHAPE_LONG, PATTERN_STRIPES, LLColor4(0.25f, 0.45f, 0.68f, 1.f), LLColor4(0.08f, 0.18f, 0.36f, 1.f) },
        { "barracuda",   SHAPE_LONG, PATTERN_SPOTS,   LLColor4(0.62f, 0.68f, 0.72f, 1.f), LLColor4(0.18f, 0.22f, 0.26f, 1.f) },
        { "garfish",     SHAPE_LONG, PATTERN_NONE,    LLColor4(0.30f, 0.66f, 0.56f, 1.f), LLColor4(0.12f, 0.36f, 0.30f, 1.f) },
        { "pike",        SHAPE_LONG, PATTERN_SPOTS,   LLColor4(0.42f, 0.56f, 0.26f, 1.f), LLColor4(0.86f, 0.88f, 0.60f, 1.f) },
        { "eel",         SHAPE_LONG, PATTERN_NONE,    LLColor4(0.32f, 0.32f, 0.28f, 1.f), LLColor4(0.20f, 0.20f, 0.16f, 1.f) },
        { "snakehead",   SHAPE_LONG, PATTERN_SPOTS,   LLColor4(0.36f, 0.34f, 0.26f, 1.f), LLColor4(0.16f, 0.14f, 0.10f, 1.f) },
        { "arowana",     SHAPE_LONG, PATTERN_NONE,    LLColor4(0.78f, 0.80f, 0.82f, 1.f), LLColor4(0.55f, 0.60f, 0.66f, 1.f) },
        { "arapaima",    SHAPE_LONG, PATTERN_NONE,    LLColor4(0.46f, 0.42f, 0.30f, 1.f), LLColor4(0.80f, 0.25f, 0.20f, 1.f) },
        { "catfish",     SHAPE_LONG, PATTERN_NONE,    LLColor4(0.50f, 0.50f, 0.52f, 1.f), LLColor4(0.90f, 0.32f, 0.20f, 1.f) },
        { "dogfish",     SHAPE_LONG, PATTERN_SPOTS,   LLColor4(0.62f, 0.56f, 0.48f, 1.f), LLColor4(0.30f, 0.24f, 0.18f, 1.f) },
        { "tope",        SHAPE_LONG, PATTERN_NONE,    LLColor4(0.56f, 0.60f, 0.64f, 1.f), LLColor4(0.32f, 0.36f, 0.40f, 1.f) },
        { "ling",        SHAPE_LONG, PATTERN_NONE,    LLColor4(0.50f, 0.46f, 0.36f, 1.f), LLColor4(0.30f, 0.26f, 0.18f, 1.f) },
        { "burbot",      SHAPE_LONG, PATTERN_SPOTS,   LLColor4(0.46f, 0.42f, 0.30f, 1.f), LLColor4(0.22f, 0.18f, 0.10f, 1.f) },
        { "plaice",      SHAPE_FLAT, PATTERN_SPOTS,   LLColor4(0.56f, 0.46f, 0.32f, 1.f), LLColor4(1.00f, 0.50f, 0.12f, 1.f) },
        { "dab",         SHAPE_FLAT, PATTERN_NONE,    LLColor4(0.62f, 0.52f, 0.36f, 1.f), LLColor4(0.40f, 0.32f, 0.20f, 1.f) },
        { "turbot",      SHAPE_FLAT, PATTERN_SPOTS,   LLColor4(0.48f, 0.42f, 0.32f, 1.f), LLColor4(0.26f, 0.22f, 0.16f, 1.f) },
        { "halibut",     SHAPE_FLAT, PATTERN_NONE,    LLColor4(0.40f, 0.38f, 0.32f, 1.f), LLColor4(0.22f, 0.20f, 0.16f, 1.f) },
        { "ray",         SHAPE_FLAT, PATTERN_SPOTS,   LLColor4(0.62f, 0.54f, 0.40f, 1.f), LLColor4(0.92f, 0.85f, 0.70f, 1.f) },
        { "salmon",      SHAPE_FISH, PATTERN_SPOTS,   LLColor4(0.80f, 0.72f, 0.74f, 1.f), LLColor4(0.30f, 0.32f, 0.38f, 1.f) },
        { "trout",       SHAPE_FISH, PATTERN_SPOTS,   LLColor4(0.56f, 0.62f, 0.46f, 1.f), LLColor4(0.92f, 0.42f, 0.48f, 1.f) },
        { "char",        SHAPE_FISH, PATTERN_SPOTS,   LLColor4(0.46f, 0.52f, 0.46f, 1.f), LLColor4(1.00f, 0.55f, 0.20f, 1.f) },
        { "carp",        SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.86f, 0.62f, 0.26f, 1.f), LLColor4(0.55f, 0.36f, 0.12f, 1.f) },
        { "mahseer",     SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.90f, 0.72f, 0.30f, 1.f), LLColor4(0.95f, 0.45f, 0.20f, 1.f) },
        { "bream",       SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.76f, 0.66f, 0.42f, 1.f), LLColor4(0.40f, 0.36f, 0.30f, 1.f) },
        { "tench",       SHAPE_FISH, PATTERN_NONE,    LLColor4(0.36f, 0.46f, 0.20f, 1.f), LLColor4(0.20f, 0.26f, 0.10f, 1.f) },
        { "roach",       SHAPE_FISH, PATTERN_NONE,    LLColor4(0.74f, 0.78f, 0.82f, 1.f), LLColor4(0.90f, 0.25f, 0.20f, 1.f) },
        { "rudd",        SHAPE_FISH, PATTERN_NONE,    LLColor4(0.88f, 0.76f, 0.40f, 1.f), LLColor4(0.92f, 0.22f, 0.16f, 1.f) },
        { "barbel",      SHAPE_FISH, PATTERN_NONE,    LLColor4(0.70f, 0.56f, 0.34f, 1.f), LLColor4(0.50f, 0.30f, 0.20f, 1.f) },
        { "peacock",     SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.78f, 0.72f, 0.22f, 1.f), LLColor4(0.15f, 0.20f, 0.10f, 1.f) },
        { "perch",       SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.56f, 0.66f, 0.26f, 1.f), LLColor4(0.90f, 0.35f, 0.18f, 1.f) },
        { "zander",      SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.60f, 0.63f, 0.56f, 1.f), LLColor4(0.28f, 0.30f, 0.26f, 1.f) },
        { "haddock",     SHAPE_FISH, PATTERN_NONE,    LLColor4(0.56f, 0.58f, 0.62f, 1.f), LLColor4(0.18f, 0.20f, 0.24f, 1.f) },
        { "cod",         SHAPE_FISH, PATTERN_SPOTS,   LLColor4(0.62f, 0.56f, 0.36f, 1.f), LLColor4(0.36f, 0.30f, 0.18f, 1.f) },
        { "pollack",     SHAPE_FISH, PATTERN_NONE,    LLColor4(0.48f, 0.44f, 0.30f, 1.f), LLColor4(0.26f, 0.24f, 0.14f, 1.f) },
        { "saithe",      SHAPE_FISH, PATTERN_NONE,    LLColor4(0.32f, 0.35f, 0.38f, 1.f), LLColor4(0.16f, 0.18f, 0.20f, 1.f) },
        { "wolffish",    SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.46f, 0.46f, 0.52f, 1.f), LLColor4(0.20f, 0.20f, 0.26f, 1.f) },
        { "snapper",     SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.88f, 0.32f, 0.26f, 1.f), LLColor4(1.00f, 0.80f, 0.30f, 1.f) },
        { "redfish",     SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.92f, 0.36f, 0.20f, 1.f), LLColor4(0.60f, 0.16f, 0.10f, 1.f) },
        { "grouper",     SHAPE_DEEP, PATTERN_SPOTS,   LLColor4(0.56f, 0.42f, 0.30f, 1.f), LLColor4(0.30f, 0.20f, 0.14f, 1.f) },
        { "trevally",    SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.56f, 0.66f, 0.76f, 1.f), LLColor4(0.20f, 0.45f, 0.90f, 1.f) },
        { "triggerfish", SHAPE_DEEP, PATTERN_STRIPES, LLColor4(0.30f, 0.50f, 0.78f, 1.f), LLColor4(1.00f, 0.80f, 0.20f, 1.f) },
        { "parrotfish",  SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.30f, 0.76f, 0.56f, 1.f), LLColor4(0.95f, 0.40f, 0.62f, 1.f) },
        { "hogfish",     SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.92f, 0.62f, 0.50f, 1.f), LLColor4(0.70f, 0.30f, 0.22f, 1.f) },
        { "mahi",        SHAPE_DEEP, PATTERN_SPOTS,   LLColor4(0.30f, 0.76f, 0.40f, 1.f), LLColor4(1.00f, 0.84f, 0.20f, 1.f) },
        { "tilapia",     SHAPE_DEEP, PATTERN_STRIPES, LLColor4(0.62f, 0.62f, 0.56f, 1.f), LLColor4(0.36f, 0.36f, 0.32f, 1.f) },
        { "piranha",     SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.62f, 0.64f, 0.68f, 1.f), LLColor4(0.92f, 0.24f, 0.18f, 1.f) },
        { "pacu",        SHAPE_DEEP, PATTERN_NONE,    LLColor4(0.42f, 0.44f, 0.48f, 1.f), LLColor4(0.96f, 0.55f, 0.20f, 1.f) },
        { "lumpsucker",  SHAPE_DEEP, PATTERN_SPOTS,   LLColor4(0.42f, 0.52f, 0.36f, 1.f), LLColor4(0.24f, 0.30f, 0.18f, 1.f) },
        { "wrasse",      SHAPE_FISH, PATTERN_SPOTS,   LLColor4(0.42f, 0.62f, 0.36f, 1.f), LLColor4(0.90f, 0.50f, 0.25f, 1.f) },
        { "mullet",      SHAPE_FISH, PATTERN_STRIPES, LLColor4(0.62f, 0.66f, 0.70f, 1.f), LLColor4(0.34f, 0.38f, 0.42f, 1.f) },
        { "",            SHAPE_FISH, PATTERN_NONE,    LLColor4(0.62f, 0.72f, 0.82f, 1.f), LLColor4(0.26f, 0.40f, 0.56f, 1.f) },
    };
    constexpr S32 FISH_LOOK_COUNT = (S32)(sizeof(FISH_LOOKS) / sizeof(FISH_LOOKS[0]));

    S32 fishLook(std::string name)
    {
        LLStringUtil::toLower(name);
        for (S32 i = 0; i < FISH_LOOK_COUNT; ++i)
        {
            if (!*FISH_LOOKS[i].mKey || name.find(FISH_LOOKS[i].mKey) != std::string::npos) return i;
        }
        return FISH_LOOK_COUNT - 1;
    }

    // Source: wolffishing_lib.php WF_JUNK: an old boot, a clump of seaweed / a tangle of pondweed, a lost sun hat,
    // a piece of driftwood / a sunken branch
    S32 junkLook(std::string name)
    {
        LLStringUtil::toLower(name);
        if (name.find("boot") != std::string::npos) return JUNK_BOOT;
        if (name.find("hat") != std::string::npos) return JUNK_HAT;
        if (name.find("weed") != std::string::npos) return JUNK_WEED;
        return JUNK_WOOD;
    }
}



const F32 WolfFishingSession::RED_AT = 0.85f;     // CHOSEN: the top 15% of the bar is red
const F32 WolfFishingSession::AMBER_AT = 0.65f;   // CHOSEN: "careful" from here

// ───────────────────────────── the session ─────────────────────────────

void WolfFishingSession::viewOpened(EView v)
{
    const bool was = active();
    mViews |= (U32)v;
    if (was) { changed(); return; }
    // a new session: everything as a fresh window had it (wolffloaterfishing.cpp onOpen before 11-11)
    mRod = ROD_NONE;
    mFish = FISH_IDLE;
    mLog.clear();
    mLostShown = false;
    mPlaying = false;
    mHelloTries = 0;
    if (!onWolf())
    {
        setStatus("Fishing is on Wolf Territories regions. Come to Wolf Territories to fish.", true);
        return;
    }
    refreshWeek();
    needRod();
}

void WolfFishingSession::viewClosed(EView v)
{
    if (!active()) return;
    mViews &= ~(U32)v;
    if (active()) { changed(); return; }
    // the rod goes back to owner chat and its touch menu (fish_rod.lsl "bye"), so a bite is never missed
    if (mRod == ROD_READY) say("WFISH|bye");
    // [REVIEW 11-11 items 2, 3] a rod search still waiting belongs to this opening: it stops at its next step (live())
    ++mRodGen;
    mRodCoroRunning = false;
    mRod = ROD_NONE;
    mFish = FISH_IDLE;
    mPlaying = false;
    // today's catch is this session's: it goes when Fishing closes (Paul 11-11)
    mPile.clear();
    mPileCount = 0;
    changed();
}

void WolfFishingSession::say(const std::string& text)
{
    // a negative channel goes as a ScriptDialogReply (fsnearbychathub.cpp sendChatFromViewerFinal; wolffloaterlifts.cpp say)
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_NORMAL, false, VCH);
}

void WolfFishingSession::setStatus(const std::string& text, bool error, bool transient, bool isCatch)
{
    mStatus = text;
    mStatusError = error;
    // the message area: newest first; a "just a moment" line gives way to the next; the same line twice is said once
    if (!text.empty())
    {
        if (!mLog.empty() && mLog.front().mText == text && mLog.front().mError == error)
        {
            mLog.front().mTransient = transient;
        }
        else
        {
            if (!mLog.empty() && mLog.front().mTransient) { mLog.erase(mLog.begin()); --mLogShift; }
            Msg m;
            m.mText = text;
            m.mError = error;
            m.mTransient = transient;
            m.mCatch = isCatch;
            mLog.insert(mLog.begin(), m);
            ++mLogShift;
            if (mLog.size() > LOG_KEPT) mLog.resize(LOG_KEPT);
        }
    }
    changed();
}

void WolfFishingSession::addToPile(bool junk, const std::string& name)
{
    PileItem it;
    it.mJunk = junk;
    it.mLook = junk ? junkLook(name) : fishLook(name);
    mPile.push_back(it);
    if ((S32)mPile.size() > PILE_SHOWN) mPile.erase(mPile.begin());
    ++mPileCount;
}

void WolfFishingSession::tipBack()
{
    if (mPile.empty()) return;
    mPile.clear();
    mPileCount = 0;
    setStatus("You tipped today's catch back into the water.", false);
}

void WolfFishingSession::setBusy()
{
    mFish = FISH_BUSY;
    mBusySince = LLFrameTimer::getElapsedSeconds();
    mHelloTries = 0;
}

void WolfFishingSession::cast()
{
    if (!active() || !onWolf()) return;
    if (mRod != ROD_READY) { needRod(); return; }
    if (mFish != FISH_IDLE) return;
    mLostShown = false;
    setBusy();
    setStatus("Casting...", false, true);
    say("WFISH|cast");
}

void WolfFishingSession::reel()
{
    if (!active() || mRod != ROD_READY || (mFish != FISH_WAITING && mFish != FISH_BITE)) return;
    if (mPlaying) return;       // the game is played by holding Reel in (challengeStep)
    if (challengeOn() && mFish == FISH_BITE)
    {
        // Challenge mode starts at a bite; before a bite there is no fish to play, and Reel in is the default early reel
        mPlaying = true;
        mTension = T_START;
        mLanded = 0.f;
        mPull = mPullTarget = 0.f;
        mPullLeft = PULL_GRACE_S;
        if (!hudOn()) openHud();     // the game is played on the HUD (Reel in pressed on the Board window)
        setStatus("Hold Reel in to bring the fish in. Let go before the line reaches the red, then hold again.", false, true);
        return;
    }
    setBusy();
    setStatus("Reeling in...", false, true);
    say("WFISH|reel");
}

void WolfFishingSession::stop()
{
    if (!active() || mRod != ROD_READY) return;
    endChallenge();
    if (mFish != FISH_WAITING && mFish != FISH_BITE) return;
    setBusy();
    say("WFISH|stop");
    changed();
}

void WolfFishingSession::putAway()
{
    if (!active()) return;
    if (LLViewerInventoryItem* rod = findRod())
    {
        if (get_is_item_worn(rod->getUUID())) LLAppearanceMgr::instance().removeItemFromAvatar(rod->getUUID());
    }
    ++mRodGen;
    mRodCoroRunning = false;
    mRod = ROD_NONE;
    mFish = FISH_IDLE;
    endChallenge();
    setStatus("Your rod is put away. Press Get my rod to fish again.", false);
}

bool WolfFishingSession::challengeOn() const
{
    // per resident (settings_per_account.xml WolfFishingChallenge), off by default
    return gSavedPerAccountSettings.controlExists("WolfFishingChallenge") && gSavedPerAccountSettings.getBOOL("WolfFishingChallenge");
}

void WolfFishingSession::setChallenge(bool on)
{
    if (gSavedPerAccountSettings.controlExists("WolfFishingChallenge")) gSavedPerAccountSettings.setBOOL("WolfFishingChallenge", on);
    if (!on && mPlaying)
    {
        // switched off mid-game: the default mode's single reel
        endChallenge();
        setStatus("A fish is biting! Reel it in whenever you are ready.", false);
    }
    changed();
}

void WolfFishingSession::endChallenge()
{
    if (!mPlaying) return;
    mPlaying = false;
    changed();
}

void WolfFishingSession::challengeStep(F32 dt, bool holding)
{
    if (!mPlaying) return;
    if (mRod != ROD_READY || mFish != FISH_BITE) { endChallenge(); return; }
    if (holding)
    {
        // the fish pulls in bursts while it is being reeled
        mPullLeft -= dt;
        if (mPullLeft <= 0.f)
        {
            mPullTarget = ll_frand(T_PULL_MAX);
            mPullLeft = PULL_MIN_S + ll_frand(PULL_MAX_S - PULL_MIN_S);
        }
        mPull += (mPullTarget - mPull) * llmin(1.f, dt * 4.f);
        mTension += (T_RISE + mPull) * dt;
        mLanded = llmin(1.f, mLanded + LAND_RATE * dt);
    }
    else
    {
        mTension = llmax(0.f, mTension - T_FALL * dt);
    }
    if (mTension >= RED_AT)
    {
        // lost: the cast is taken out (the rod posts op=cancel, the server deletes it): nothing is rolled or counted
        mPlaying = false;
        mTension = RED_AT;
        mLostShown = true;
        setBusy();
        say("WFISH|stop");
        setStatus("It got away! The line went into the red and the fish swam off.", false, false, true);
        return;
    }
    if (mLanded >= 1.f)
    {
        // landed: the reel the default mode sends; the server picks the fish and its weight
        mPlaying = false;
        setBusy();
        setStatus("Reeling in...", false, true);
        say("WFISH|reel");
    }
    // (no changed() while playing: the bars and their words are drawn from tension() / landed() every frame)
}

void WolfFishingSession::tick()
{
    if (!active() || mRod != ROD_READY || mFish != FISH_BUSY) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now - mBusySince < BUSY_HELLO_S) return;
    if (mHelloTries < HELLO_TRIES)
    {
        ++mHelloTries;
        mBusySince = now;
        setStatus("Still waiting for your fishing rod to answer...", false, true);
        say("WFISH|hello|1");
        return;
    }
    // [REVIEW 11-11 item 1] no answer at all: the buttons are freed and the resident is told what to do
    mRod = ROD_NONE;
    mFish = FISH_IDLE;
    mPlaying = false;
    setStatus("Your fishing rod did not answer. Press Get my rod to look for it again.", true);
}

void WolfFishingSession::onRodLine(const std::string& text)
{
    if (!active()) return;
    const std::vector<std::string> f = fields(text);
    const std::string kind = fieldAt(f, 1);
    auto fishState = [this](const std::string& s)
    {
        if (s == "waiting") mFish = FISH_WAITING;
        else if (s == "bite") mFish = FISH_BITE;
        else if (s == "busy") { mFish = FISH_BUSY; mBusySince = LLFrameTimer::getElapsedSeconds(); }
        else mFish = FISH_IDLE;
        if (mFish != FISH_BITE) mPlaying = false;
    };
    // Source: fish_rod.lsl tell() / listen "hello": WolfFish1|hi|<state>|, |state|<state>|<text>,
    // |catch|<fish|junk>|<name>|<words>|<counted>|<score>|<fish>|<place>|<text>, |error|<text>, |info|<text>, |off|
    if (kind == "hi")
    {
        mRod = ROD_READY;
        mHelloTries = 0;
        fishState(fieldAt(f, 2));
        if (mFish == FISH_BITE) setStatus("A fish is biting! Reel it in whenever you are ready.", false);
        else if (mFish == FISH_WAITING) setStatus("Your line is in the water. Waiting for a bite...", false);
        else if (mFish == FISH_BUSY) setStatus("Your rod is busy - just a moment...", false, true);
        else setStatus("Your rod is ready. Face the water and press Cast.", false);
    }
    else if (kind == "state")
    {
        fishState(fieldAt(f, 2));
        // after a lost fish the rod says "Your line is out of the water." - the message area already says what happened
        if (mLostShown && mFish == FISH_IDLE) { mLostShown = false; changed(); }
        else setStatus(restFrom(f, 3), false);
    }
    else if (kind == "catch")
    {
        mFish = FISH_IDLE;
        mPlaying = false;
        const std::string what = fieldAt(f, 2);
        const std::string words = restFrom(f, 9);
        bool counted = false;
        if (what == "junk")
        {
            // Source: wolffishing.php op=reel junk: "You reeled in <name>! Not a fish - but it makes a good story. Cast again."
            setStatus("You reeled in " + fieldAt(f, 3) + "! Not a fish - but it makes a good story.", false, false, true);
            addToPile(true, fieldAt(f, 3));
        }
        else
        {
            // one line for the catch: what, how heavy, rare / biggest (the server's own words), the week
            counted = fieldAt(f, 5) == "1";
            std::string line = "You caught " + articled(fieldAt(f, 3)) + ": " + fieldAt(f, 4) + "!";
            if (words.find(RARE_WORDS) != std::string::npos) line += " " + RARE_WORDS;
            if (words.find(BIGGEST_WORDS) != std::string::npos) line += " " + BIGGEST_WORDS;
            if (counted)
            {
                const S32 place = atoi(fieldAt(f, 8).c_str());
                line += " Your week: " + fieldAt(f, 6) + (place > 0 ? ", " + ordinal(place) + " place." : std::string("."));
                mMe.mKnown = true;
                mMe.mScore = fieldAt(f, 6);
                mMe.mFish = atoi(fieldAt(f, 7).c_str());
                mMe.mPlace = place;
            }
            else
            {
                // a visitor, or past today's counted fish: the server says which in brackets
                const std::string why = bracketed(words);
                line += " " + (why.empty() ? std::string("This one is just for fun.") : "(" + why + ")");
            }
            setStatus(line, false, false, true);
            addToPile(false, fieldAt(f, 3));
        }
        if (what == "fish" && counted) refreshWeek();
    }
    else if (kind == "error")
    {
        mFish = FISH_IDLE;
        mPlaying = false;
        setStatus(restFrom(f, 2), true);
    }
    else if (kind == "info")
    {
        setStatus(restFrom(f, 2), false);
    }
    else if (kind == "off")
    {
        mRod = ROD_NONE;
        mFish = FISH_IDLE;
        mPlaying = false;
        setStatus("Your rod was taken off. Press Get my rod to fish again.", false);
    }
    changed();
}

void WolfFishingSession::needRod()
{
    if (mRodCoroRunning || mRod == ROD_READY || !onWolf() || !active()) return;
    mRod = ROD_FINDING;
    mRodCoroRunning = true;
    const U32 gen = ++mRodGen;
    changed();
    LLCoros::instance().launch("WolfFishingRod", [gen]() { rodCoro(gen); });
}

void WolfFishingSession::rodFailed(const std::string& why)
{
    mRodCoroRunning = false;
    mRod = ROD_NONE;
    setStatus(why, true);
}

WolfFishingSession* WolfFishingSession::live(U32 gen)
{
    // the session still exists, is still open, and this search is still its current one
    if (!instanceExists()) return nullptr;
    WolfFishingSession& s = instance();
    return s.mRodGen == gen && s.active() ? &s : nullptr;
}

void WolfFishingSession::rodCoro(U32 gen)
{
    // Source: wolffloaterfishing.cpp rodCoro (the window's search, kept step for step); every step after a wait first checks
    // that this search is still wanted (live()): closed, put away or started again meanwhile, it stops (reviewer 11-11)
    // 1. a rod already worn answers hello
    if (WolfFishingSession* s = live(gen)) { s->setStatus("Looking for your fishing rod...", false, true); s->say("WFISH|hello|1"); }
    else return;
    llcoro::suspendUntilTimeout(2.5f);
    WolfFishingSession* s = live(gen);
    if (!s) return;
    if (s->mRod == ROD_READY) { s->mRodCoroRunning = false; return; }
    // 2. the rod in the inventory; when there is none, the grid gives one (php/wolffishing.php) and it is waited for
    LLViewerInventoryItem* rod = findRod();
    if (!rod)
    {
        s->setStatus("You have no fishing rod yet - getting one for you (it goes into Objects > Fishing)...", false);
        const std::string body("{\"action\":\"give\"}");
        LLSD result = WolfCaves::serviceRequest(API_URL, &body, API_TIMEOUT_S);
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        s = live(gen);
        if (!s) return;
        if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
        {
            std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
                : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
            s->rodFailed("The fishing rod could not be fetched: " + why);
            return;
        }
        LLUUID folder = reply["folderId"].asUUID();
        LLUUID item_id = reply["given"].isMap() ? reply["given"]["item"].asUUID() : LLUUID::null;
        for (S32 i = 0; i < 30 && !rod; ++i)
        {
            if (folder.notNull())
            {
                if (LLViewerInventoryCategory* cat = gInventory.getCategory(folder)) cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
                gInventory.fetchDescendentsOf(folder);
            }
            llcoro::suspendUntilTimeout(1.f);
            if (!live(gen)) return;
            rod = item_id.notNull() ? gInventory.getItem(item_id) : nullptr;
            if (rod && !rod->isFinished()) rod = nullptr;
            if (!rod) rod = findRod();
        }
        s = live(gen);
        if (!s) return;
        if (!rod)
        {
            s->rodFailed("Your fishing rod is in your inventory (Objects > Fishing), but this viewer could not see it yet. "
                         "Close Fishing and open it again.");
            return;
        }
    }
    // 3. put it on (in the right hand: the rod's own attach point), added beside anything already worn there - only for a
    // session still open (live() above, after the last wait)
    const LLUUID rod_id = rod->getUUID();
    if (!get_is_item_worn(rod_id))
    {
        s->setStatus("Putting your fishing rod in your hand...", false, true);
        LLAppearanceMgr::instance().wearItemOnAvatar(rod_id, true, false);
    }
    else
    {
        s->setStatus("Waiting for your fishing rod to answer...", false, true);
    }
    for (S32 t = 0; t < 24; ++t)
    {
        llcoro::suspendUntilTimeout(1.f);
        s = live(gen);
        if (!s) return;
        if (s->mRod == ROD_READY) { s->mRodCoroRunning = false; return; }
        if (t % 3 == 1) s->say("WFISH|hello|1");
    }
    // not on after 24 s: scripts off here, or (not worn) OpenSim's appearance already lists it and ignored the wear
    // (AttachmentsModule.cs RezSingleAttachmentFromInventory "alreadyOn"), which a new login puts right
    s->rodFailed(get_is_item_worn(rod_id)
        ? "Your fishing rod is in your hand but did not answer. Scripts may be switched off here (About Land > Options) - "
          "try somewhere else, or take the rod off and put it on again."
        : "Your fishing rod could not be put on just now. Press Get my rod to try again; if it still will not go on, log out and back in.");
}

void WolfFishingSession::refreshWeek()
{
    if (mWeekBusy || !onWolf()) return;
    mWeekBusy = true;
    changed();
    LLCoros::instance().launch("WolfFishingWeek", []() { weekCoro(); });
}

void WolfFishingSession::weekCoro()
{
    const std::string body("{\"action\":\"week\"}");
    LLSD result = WolfCaves::serviceRequest(API_URL, &body, API_TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    if (!instanceExists()) return;
    WolfFishingSession& s = instance();
    s.mWeekBusy = false;
    if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
    {
        std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
            : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
        s.mWeekError = "This week's board could not be read just now: " + why + " Press Update to try again.";
    }
    else
    {
        s.mWeek = reply;
        s.mWeekError.clear();
        // Source: wolffishing.php wf_week_payload: me = {place, placeText, score, fish, best}
        const LLSD& me = reply["me"];
        s.mMe.mKnown = true;
        s.mMe.mScore = me["score"].asString();
        s.mMe.mFish = me["fish"].asInteger();
        s.mMe.mPlace = me["place"].asInteger();
    }
    s.changed();
}


void WolfFishingSession::openHud()
{
    WolfFishingSession& s = instance();
    if (s.hudOn()) return;
    s.viewOpened(VIEW_HUD);
}

void WolfFishingSession::closeAll()
{
    // the Board first (the session stays open for the HUD), then the HUD: the session ends and says "bye"
    LLFloaterReg::hideInstance("wolf_fishing");
    if (instanceExists() && instance().hudOn()) instance().viewClosed(VIEW_HUD);
}

// ───────────────────────────── the HUD ─────────────────────────────

namespace
{
    const F32 PI_F = 3.14159265f;

    // Source: wolfskihud.cpp - the Ski HUD's night blue and ice, its status colours (amber caution, red act now) and its
    // two insets (band on / off). Text is white or ice on navy (contrast above 10:1).
    const LLColor4 NAVY_HI(0.09f, 0.17f, 0.28f, 0.94f), NAVY_LO(0.02f, 0.05f, 0.09f, 0.94f);
    const LLColor4 INSET_ON(0.f, 0.f, 0.f, 0.42f);
    // [FISHING HUD 2026-11-11] Paul: "allow users to turn off the background": off, nothing is drawn behind the read-outs at
    // all (the Ski HUD keeps a dark backing for each); the words are outlined instead (OUTLINE), so they read over any sky
    const LLColor4 INSET_OFF(0.f, 0.f, 0.f, 0.f);
    LLColor4 INSET = INSET_ON;                         // this frame's (draw() sets it from WolfFishingHudBackground)
    bool OUTLINE = false;                              // this frame's: background off
    const LLColor4 ICE(0.62f, 0.88f, 1.00f, 1.f), ICE_DIM(0.38f, 0.55f, 0.68f, 1.f);
    const LLColor4 SNOW(0.97f, 0.98f, 1.00f, 1.f);
    const LLColor4 GREY(0.66f, 0.72f, 0.78f, 1.f);
    const LLColor4 GOLD(1.00f, 0.82f, 0.25f, 1.f);
    const LLColor4 AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 RED(1.00f, 0.25f, 0.20f, 1.f);
    const LLColor4 GREEN(0.30f, 0.90f, 0.45f, 1.f);
    // Source: wolfskihud.cpp button faces (TAKE THEM OFF: the main button; HELP / HIDE: the small ones; PANEL lit)
    const LLColor4 BTN_MAIN_HI(0.20f, 0.42f, 0.66f, 0.97f), BTN_MAIN_LO(0.08f, 0.18f, 0.32f, 0.97f);
    const LLColor4 BTN_HI(0.16f, 0.26f, 0.38f, 0.97f), BTN_LO(0.06f, 0.11f, 0.18f, 0.97f);
    // REEL IN at a bite: green, the "go" of the family's status colours (ISO 2575 as wolfdashboard.cpp)
    const LLColor4 BTN_GO_HI(0.16f, 0.52f, 0.30f, 0.97f), BTN_GO_LO(0.05f, 0.24f, 0.12f, 0.97f);
    const LLColor4 BTN_OFF_HI(0.12f, 0.16f, 0.21f, 0.80f), BTN_OFF_LO(0.05f, 0.07f, 0.10f, 0.80f);

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    // ---- primitives, view-local, untextured (Source: wolfskihud.cpp's, which are wolfhelmhud.cpp's) ----
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

    void triF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
        gGL.end();
    }

    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c);
        rectF(l, t - w, r, t, c);
        rectF(l, b, l + w, t, c);
        rectF(r - w, b, r, t, c);
    }

    void roundRectF(F32 l, F32 b, F32 r, F32 t, F32 rad, const LLColor4& top, const LLColor4& bot)
    {
        // Source: wolfskihud.cpp roundRectF (wolfhelmhud.cpp roundRectF)
        rad = llmin(rad, (r - l) * 0.5f, (t - b) * 0.5f);
        const F32 h = llmax(0.001f, t - b);
        auto col = [&](F32 y) { return lerp(bot, top, llclamp((y - b) / h, 0.f, 1.f)); };
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b + rad, col(b + rad)); vtx(r, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad));
        vtx(l, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad)); vtx(l, t - rad, col(t - rad));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t)); vtx(l + rad, t, col(t));
        vtx(l + rad, b, col(b)); vtx(r - rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad));
        vtx(l + rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad)); vtx(l + rad, b + rad, col(b + rad));
        const F32 cxs[4] = { r - rad, l + rad, l + rad, r - rad };
        const F32 cys[4] = { t - rad, t - rad, b + rad, b + rad };
        for (S32 k = 0; k < 4; ++k)
        {
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 a0 = PI_F * 0.5f * ((F32)k + (F32)i / 6.f), a1 = PI_F * 0.5f * ((F32)k + (F32)(i + 1) / 6.f);
                const F32 x0 = cxs[k] + rad * cosf(a0), y0 = cys[k] + rad * sinf(a0);
                const F32 x1 = cxs[k] + rad * cosf(a1), y1 = cys[k] + rad * sinf(a1);
                vtx(cxs[k], cys[k], col(cys[k])); vtx(x0, y0, col(y0)); vtx(x1, y1, col(y1));
            }
        }
        gGL.end();
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER, LLFontGL::VAlign v = LLFontGL::VCENTER)
    {
        if (OUTLINE)
        {
            // a dark outline, about a pixel round each letter, so white and gold words read over snow, sky or sea
            const F32 o = 1.f / llmax(0.1f, WolfHudScale::current());
            const LLColor4 dark(0.01f, 0.03f, 0.06f, 0.85f * c.mV[VALPHA]);
            WolfHudScale::text(f, s, x - o, y, dark, h, v);
            WolfHudScale::text(f, s, x + o, y, dark, h, v);
            WolfHudScale::text(f, s, x, y - o, dark, h, v);
            WolfHudScale::text(f, s, x, y + o, dark, h, v);
        }
        WolfHudScale::text(f, s, x, y, c, h, v);   // sharp at the size the resident set (wolfhudscale.h)
    }

    // Source: wolfskihud.cpp face() - the B612 face (fonts.xml Avionics0..11) at the largest size whose line fits px.
    const LLFontGL* face(const char* name, U8 style, F32 px)
    {
        static std::map<std::string, std::vector<const LLFontGL*>> cache;
        const std::string key = std::string(name) + (style ? "B" : "R");
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                if (const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, llformat("Avionics%d", i), style)))
                {
                    v.push_back(f);
                }
            }
            if (v.empty())
            {
                LL_WARNS("WolfFishingHud") << "font " << name << " missing, using the default" << LL_ENDL;
                v.push_back(LLFontGL::getFontSansSerif());
            }
            it = cache.emplace(key, v).first;
        }
        const LLFontGL* best = it->second.front();
        for (const LLFontGL* f : it->second)
        {
            if ((F32)f->getLineHeight() <= px) best = f;
        }
        return best;
    }

    // THE LAYOUT, in the dashboards' frame (Source: wolfskihud.cpp "THE LAYOUT"): 1600 x 900 with the panel from y 596 to
    // 900 (y down), scaled by u; the week keeps its distance from the left edge (L), the state stays centred (C), the
    // buttons keep theirs from the right edge (R). Always one row (wolfskihud.cpp: two rows put a panel over the avatar).
    struct Anchor { F32 ox = 0.f, top0 = 900.f, u = 1.f; };
    struct Geo
    {
        F32 W = 1600.f;
        Anchor L, C, R, cur;
        F32 bandB = 0.f;
        F32 panelTop = 304.f;
    } g;
    const F32 C_L = 600.f, C_R = 1000.f, R_L = 1040.f;   // the centre and right parts' edges, mock units (left: 0..560)
    void use(const Anchor& a) { g.cur = a; }
    F32 X(F32 x) { return g.cur.ox + x * g.cur.u; }
    F32 YM(F32 y) { return g.cur.top0 - y * g.cur.u; }
    F32 S(F32 v) { return v * g.cur.u; }

    // [FISHING HUD 2026-11-11] Paul: "can you make the text slightly bigger please, it's difficult to read" - every word on the
    // HUD a step bigger than the Ski HUD's sizes (CHOSEN: 22 %); a word that would not fit its box comes down by itself (fitT)
    constexpr F32 TEXT_UP = 1.22f;

    const LLFontGL* font(F32 mock_px, bool bold = false, bool mono = false)
    {
        return face(mono ? "B612Mono" : "B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(mock_px) * 1.25f * TEXT_UP));
    }

    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, bool mono = false,
           LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        text(font(px, bold, mono), s, x, y, c, h, LLFontGL::VCENTER);
    }

    // Centred text that must fit `maxw` (picture units): smaller a size at a time down to `min_px`, then cut with "...".
    // Source: wolfskihud.cpp fitT.
    void fitT(F32 x, F32 y, std::string str, F32 px, F32 min_px, F32 maxw, const LLColor4& c, bool bold = false)
    {
        F32 size = px;
        const LLFontGL* f = font(size, bold);
        while (f && f->getWidthF32(str) > maxw && size > min_px)
        {
            size -= 1.f;
            f = font(size, bold);
        }
        if (f && f->getWidthF32(str) > maxw)
        {
            while (str.size() > 4 && f->getWidthF32(str + "...") > maxw)
            {
                str.pop_back();
                while (!str.empty() && ((unsigned char)str.back() & 0xC0) == 0x80) str.pop_back();
                if (!str.empty() && (unsigned char)str.back() >= 0xC0) str.pop_back();
            }
            str += "...";
        }
        text(f, str, x, y, c, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    /** A message: its words in lines no wider than `maxw` (at most `max_lines`, the last cut with "..."), centred one under
     *  another from `top` down. Returns the height it took. */
    F32 msgT(F32 x, F32 top, const std::string& str, F32 px, F32 maxw, S32 max_lines, const LLColor4& c, bool draw = true)
    {
        const LLFontGL* f = font(px);
        std::vector<std::string> lines;
        std::string cur;
        size_t at = 0;
        while (at < str.size())
        {
            size_t sp = str.find(' ', at);
            if (sp == std::string::npos) sp = str.size();
            const std::string w = str.substr(at, sp - at);
            at = sp + 1;
            if (w.empty()) continue;
            const std::string next = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && f->getWidthF32(next) > maxw) { lines.push_back(cur); cur = w; }
            else cur = next;
        }
        if (!cur.empty()) lines.push_back(cur);
        if ((S32)lines.size() > max_lines)
        {
            // the rest goes on the last line, cut with "..." by fitT
            std::string last;
            for (size_t i = (size_t)max_lines - 1; i < lines.size(); ++i) last += (last.empty() ? "" : " ") + lines[i];
            lines.resize((size_t)max_lines);
            lines.back() = last;
        }
        const F32 lh = S(px * 1.45f * TEXT_UP);
        if (draw) for (size_t i = 0; i < lines.size(); ++i) fitT(x, top - lh * ((F32)i + 0.5f), lines[i], px, px, maxw, c);
        return lh * (F32)lines.size();
    }

    struct Ctx
    {
        std::function<void(F32, F32, F32, F32, WolfFishingHud::EHit, const std::string&)> hit;
        S32 pressed;
    };

    // Source: wolfskihud.cpp button() - one rounded button, its face lit from above, a pale rim, B612 bold; `enabled` false:
    // drawn dim with grey words and no hit, so only the buttons usable now can be pressed (Paul 11-11)
    void button(Ctx& c, F32 l, F32 b, F32 r, F32 t, const std::string& label, WolfFishingHud::EHit id, const std::string& tip,
                F32 px, const LLColor4& face_hi, const LLColor4& face_lo, bool enabled = true)
    {
        if (!enabled)
        {
            roundRectF(l, b, r, t, (t - b) * 0.25f, BTN_OFF_HI, BTN_OFF_LO);
            frameW(l, b, r, t, 1.f, alpha(SNOW, 0.18f));
            fitT((l + r) * 0.5f, (b + t) * 0.5f, label, px, 8.f, (r - l) - S(10.f), alpha(GREY, 0.75f), true);
            return;
        }
        const bool down = c.pressed == id;
        roundRectF(l, b, r, t, (t - b) * 0.25f, down ? face_lo : face_hi, down ? face_hi : face_lo);
        frameW(l, b, r, t, 1.2f, alpha(SNOW, 0.55f));
        fitT((l + r) * 0.5f, (b + t) * 0.5f, label, px, 8.f, (r - l) - S(10.f), SNOW, true);   // never wider than the button
        c.hit(l, b, r, t, id, tip);
    }

    /** The state in a few capital words, for the centre's top line. */
    std::string stateWords(const WolfFishingSession& s, LLColor4& col)
    {
        col = ICE;
        if (!WolfGrid::isOnWolfTerritories()) { col = AMBER; return "WOLF TERRITORIES ONLY"; }
        if (s.statusIsError()) { col = AMBER; return "SOMETHING NEEDS YOU"; }
        if (s.rod() == WolfFishingSession::ROD_FINDING) return "GETTING YOUR ROD READY";
        if (s.rod() == WolfFishingSession::ROD_NONE) { col = SNOW; return "NO ROD IN YOUR HAND"; }
        if (s.challengePlaying()) { col = GOLD; return "PLAY THE FISH!"; }
        switch (s.fish())
        {
            case WolfFishingSession::FISH_WAITING: return "YOUR LINE IS IN THE WATER";
            case WolfFishingSession::FISH_BITE:    col = GOLD; return "A FISH IS BITING!";
            case WolfFishingSession::FISH_BUSY:    return "JUST A MOMENT...";
            default:                               col = SNOW; return "READY - FACE THE WATER";
        }
    }
}

WolfFishingHud::WolfFishingHud(const Params& p) : LLUICtrl(p) {}
WolfFishingHud::~WolfFishingHud() {}

void WolfFishingHud::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ llmin(l, r), llmin(b, t), llmax(l, r), llmax(b, t), id, tip });
}

const WolfFishingHud::Hit* WolfFishingHud::hitAt(S32 x, S32 y) const
{
    const F32 px = (F32)x / mHitScale, py = (F32)y / mHitScale;
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (px >= it->mL && px <= it->mR && py >= it->mB && py <= it->mT)
        {
            return &*it;
        }
    }
    return nullptr;
}

bool WolfFishingHud::onPanel(S32 x, S32 y) const
{
    if (!mDrawn) return false;
    if (hitAt(x, y)) return true;
    // Source: wolfskihud.cpp onPanel - with the background on the whole band is solid; off, only the controls
    if (!gSavedPerAccountSettings.getBOOL("WolfFishingHudBackground")) return false;
    return (F32)y >= mPanelB && (F32)y <= mPanelT;
}

void WolfFishingHud::press(EHit id)
{
    WolfFishingSession& s = WolfFishingSession::instance();
    switch (id)
    {
    case H_CAST:      s.cast(); break;
    // the reel goes as it is pressed; in Challenge mode at a bite this starts the game, which is played while it is held
    case H_REEL:      s.reel(); break;
    case H_STOP:      s.stop(); break;
    case H_BOARD:     LLFloaterReg::showInstance("wolf_fishing"); break;
    case H_CHALLENGE: s.setChallenge(!s.challengeOn()); break;
    case H_PANEL:     gSavedPerAccountSettings.setBOOL("WolfFishingHudBackground", !gSavedPerAccountSettings.getBOOL("WolfFishingHudBackground")); break;
    case H_CLOSE:     WolfFishingSession::closeAll(); break;
    case H_TIP:       s.tipBack(); break;
    case H_SCROLL_UP:   mScroll = llmax(0, mScroll - 1); break;
    case H_SCROLL_DOWN: mScroll = llmin(llmax(0, mMsgTotal - 1), mScroll + 1); break;
    default: break;
    }
}

void WolfFishingHud::draw()
{
    mHits.clear();
    mDrawn = false;
    if (!WolfFishingSession::instanceExists() || !WolfFishingSession::instance().hudOn())
    {
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        mPressed = H_NONE;
        mResize.hide();
        mLastFrame = 0.0;
        return;
    }
    WolfFishingSession& s = WolfFishingSession::instance();
    const F64 t = now();
    const F32 dt = mLastFrame > 0.0 ? (F32)llclamp(t - mLastFrame, 0.0, 0.1) : 0.f;
    mLastFrame = t;
    s.tick();
    // Challenge mode: REEL IN held down (the mouse, or a finger, still on it) is the fish being reeled
    s.challengeStep(dt, mPressed == H_REEL && hasMouseCapture());

    // the band, as wolfskihud.cpp draw(): above the bottom toolbar, one row, its height a share of the window times UI Size
    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    F32 screen_bottom = 0.f;
    if (gToolBarView)
    {
        if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
        {
            if (bp->isInVisibleChain())
            {
                const LLRect sr = bp->calcScreenRect();
                S32 lx = 0, ly = 0;
                screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                screen_bottom = llclamp((F32)ly, 0.f, Hv * 0.4f);
            }
        }
    }
    const F32 zoom = mResize.scale();
    const F32 W = screen_w / zoom;
    const F32 bottom = screen_bottom / zoom;
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    // Source: wolfskihud.cpp - 26 % of the window, 120..330 px, never more than 30 % of what is above the toolbar
    const F32 want_h = llclamp(Hv * ui * 0.26f, 120.f, 330.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.30f);
    const F32 uH = llmin(want_h, max_h) / 304.f;
    const F32 one = llmin(uH, W / 1600.f);
    g.W = W;
    g.bandB = bottom;
    g.L.u = g.C.u = g.R.u = one;
    g.L.top0 = g.C.top0 = g.R.top0 = bottom + 900.f * one;
    g.L.ox = 0.f;
    g.C.ox = W * 0.5f - 800.f * one;
    g.R.ox = W - 1600.f * one;
    g.panelTop = g.C.top0 - 596.f * g.C.u;
    mPanelB = screen_bottom;
    mPanelT = g.panelTop * zoom;
    mDrawn = true;

    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;
    zoomed.emplace(zoom);
    Ctx c{ [this](F32 l, F32 b, F32 r, F32 tp, EHit id, const std::string& tip) { addHit(l, b, r, tp, id, tip); }, mPressed };
    const bool bg = gSavedPerAccountSettings.getBOOL("WolfFishingHudBackground");
    INSET = bg ? INSET_ON : INSET_OFF;
    OUTLINE = !bg;
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool ready = wolf && s.rod() == WolfFishingSession::ROD_READY;
    const bool lineOut = s.fish() == WolfFishingSession::FISH_WAITING || s.fish() == WolfFishingSession::FISH_BITE;
    const bool bite = s.fish() == WolfFishingSession::FISH_BITE;

    // the band: night blue, a frosted top edge - or nothing at all, the read-outs floating over the world (PANEL)
    use(g.L);
    if (bg)
    {
        rectG(0.f, g.bandB, W, g.panelTop, NAVY_HI, NAVY_LO);
        rectF(0.f, g.panelTop - S(4.f), W, g.panelTop, alpha(ICE, 0.35f));
        rectF(0.f, g.panelTop - S(2.f), W, g.panelTop - S(1.f), alpha(SNOW, 0.8f));
    }

    // ── LEFT: your week ──
    use(g.L);
    {
        const WolfFishingSession::Me& me = s.me();
        if (bg) roundRectF(X(20.f), YM(760.f), X(540.f), YM(606.f), S(10.f), alpha(GOLD, 0.14f), INSET);
        T(X(34.f), YM(622.f), "YOUR WEEK", 11.f, GOLD, true, false, LLFontGL::LEFT);
        const std::string score = !me.mKnown ? std::string("--") : (me.mFish > 0 ? me.mScore : std::string("0.00 kg"));
        T(X(280.f), YM(680.f), score, 34.f, SNOW, true);   // B612, not Mono: Mono set "4.87" as "4. 87" (test shot b06)
        T(X(34.f), YM(740.f), !me.mKnown ? (s.weekError().empty() ? "reading your week..." : "your week could not be read just now")
                                         : "your score: the weight of your ten biggest fish", 11.f, GREY, false, false, LLFontGL::LEFT);
        roundRectF(X(20.f), YM(860.f), X(270.f), YM(770.f), S(8.f), INSET, INSET);
        T(X(145.f), YM(788.f), "PLACE", 11.f, GREY, true);
        T(X(145.f), YM(824.f), me.mKnown && me.mPlace > 0 ? ordinal(me.mPlace) : std::string("--"), 22.f, ICE, true, true);
        roundRectF(X(290.f), YM(860.f), X(540.f), YM(770.f), S(8.f), INSET, INSET);
        T(X(415.f), YM(788.f), "FISH CAUGHT", 11.f, GREY, true);
        T(X(415.f), YM(824.f), me.mKnown ? llformat("%d", me.mFish) : std::string("--"), 22.f, SNOW, true, true);
        if (!bg) roundRectF(X(20.f), YM(898.f), X(540.f), YM(876.f), S(6.f), INSET, INSET);
        T(X(280.f), YM(888.f), "THE BOARD STARTS AGAIN EVERY MONDAY", 11.f, GREY, true);
    }

    // ── CENTRE: the message area - what has happened, newest at the top, older lines fading (Paul 11-11: "stop the big
    // message box ... have a message area on the HUD that shows what's happened instead") ──
    use(g.C);
    const F32 beat = 0.5f + 0.5f * sinf((F32)fmod(t, 10.0) * PI_F);   // a gentle beat, two seconds long (no flashing)
    {
        LLColor4 sc;
        const std::string sw = stateWords(s, sc);
        roundRectF(X(C_L), YM(642.f), X(C_R), YM(606.f), S(8.f), INSET, INSET);
        LLColor4 swc = sc;
        if (bite && !s.challengePlaying()) swc = lerp(GOLD, SNOW, beat);
        fitT(X((C_L + C_R) * 0.5f), YM(624.f), sw, 13.f, 9.f, S(C_R - C_L - 20.f), swc, true);
        if (s.challengePlaying())
        {
            // the line's tension: green, amber ("careful"), red (the line snaps); the marker is where it is now
            const F32 tn = s.tension();
            const bool careful = tn >= WolfFishingSession::AMBER_AT;
            fitT(X((C_L + C_R) * 0.5f), YM(672.f), careful ? "CAREFUL - LET GO BEFORE THE RED!" : (mPressed == H_REEL ? "REELING IN - KEEP OUT OF THE RED" : "HOLD REEL IN TO BRING IT IN"),
                 12.f, 9.f, S(C_R - C_L - 20.f), careful ? GOLD : SNOW, true);
            const F32 l = X(C_L + 24.f), r = X(C_R - 24.f), w = r - l;
            const F32 b1 = YM(736.f), t1 = YM(700.f);
            const F32 xa = l + w * WolfFishingSession::AMBER_AT, xr = l + w * WolfFishingSession::RED_AT;
            rectF(l, b1, xa, t1, alpha(GREEN, 0.75f));
            rectF(xa, b1, xr, t1, alpha(AMBER, 0.8f));
            rectF(xr, b1, r, t1, alpha(RED, 0.85f));
            frameW(l, b1, r, t1, 1.2f, alpha(SNOW, 0.7f));
            const F32 mx = l + w * llclamp(tn, 0.f, 1.f);
            rectF(mx - S(4.f), b1 - S(6.f), mx + S(4.f), t1 + S(6.f), SNOW);
            frameW(mx - S(4.f), b1 - S(6.f), mx + S(4.f), t1 + S(6.f), 1.f, NAVY_LO);
            T(l, YM(750.f), "LINE TENSION", 11.f, GREY, true, false, LLFontGL::LEFT);
            T(r, YM(750.f), "RED: IT GETS AWAY", 11.f, RED, true, false, LLFontGL::RIGHT);
            const F32 b2 = YM(800.f), t2 = YM(780.f);
            rectF(l, b2, r, t2, alpha(NAVY_LO, 0.7f));
            rectF(l, b2, l + w * llclamp(s.landed(), 0.f, 1.f), t2, GOLD);
            frameW(l, b2, r, t2, 1.2f, alpha(SNOW, 0.7f));
            T(l, YM(814.f), "LINE IN", 11.f, GREY, true, false, LLFontGL::LEFT);
            T(r, YM(814.f), llformat("%d%%", (S32)llround(llclamp(s.landed(), 0.f, 1.f) * 100.f)), 11.f, SNOW, true, true, LLFontGL::RIGHT);
        }
        else
        {
            // the messages, newest first, each at most two lines. Paul 11-11: "don't shade the background of the message box
            // please, so you can see the history of what's happening, and it would be nice if it had a scrollbar" - no
            // backing whatever the BACKGROUND switch says, the words outlined, older ones only gently dimmed, and a scroll
            // bar (arrows, the bar, the thumb, the mouse wheel). A problem is amber with a bar beside it and stays until
            // something newer happens (no timer takes it away).
            const std::vector<WolfFishingSession::Msg>& log = s.messages();
            // new lines come in at the top: reading further back, the view stays on the same lines (it is not yanked)
            const S32 shift = s.logShift();
            if (mScroll > 0) mScroll += shift - mSeenShift;
            mSeenShift = shift;
            mScroll = llclamp(mScroll, 0, llmax(0, (S32)log.size() - 1));
            const bool outline_was = OUTLINE;
            OUTLINE = true;
            const F32 textL = X(C_L + 6.f), textR = X(C_R - 34.f);
            const F32 cx = (textL + textR) * 0.5f, maxw = textR - textL - S(16.f);
            F32 top = YM(656.f);
            const F32 bottom = YM(886.f);
            mMsgL = X(C_L); mMsgR = X(C_R); mMsgT = top; mMsgB = bottom;
            if (log.empty()) msgT(cx, top, "Looking for your fishing rod...", 12.f, maxw, 2, SNOW);
            S32 shown = 0;
            for (size_t i = (size_t)mScroll; i < log.size(); ++i)
            {
                const WolfFishingSession::Msg& m = log[i];
                const F32 px = i == 0 ? 13.f : 12.f;
                const F32 need = msgT(cx, top, m.mText, px, maxw, 2, SNOW, false);
                if (top - need < bottom && shown > 0) break;
                const F32 a = i == 0 ? 1.f : 0.86f;   // the newest bright, the rest gently dimmed (they stay readable)
                const LLColor4 col = m.mError ? AMBER : (m.mCatch ? GOLD : SNOW);
                const F32 used = msgT(cx, top, m.mText, px, maxw, 2, alpha(col, a));
                if (m.mError) rectF(textL, top - used + S(2.f), textL + S(5.f), top - S(2.f), AMBER);
                top -= used + S(6.f);
                ++shown;
            }
            OUTLINE = outline_was;
            mMsgShown = shown;
            mMsgTotal = (S32)log.size();
            // the scroll bar, at the right of the message area, when there is more than shows
            if (mMsgTotal > shown || mScroll > 0)
            {
                const F32 bl = X(C_R - 26.f), br = X(C_R - 4.f);
                const F32 upB = YM(682.f), upT = YM(656.f), dnB = YM(886.f), dnT = YM(860.f);
                Ctx sc{ [this](F32 a, F32 bb, F32 r, F32 tt, EHit id, const std::string& tip) { addHit(a, bb, r, tt, id, tip); }, mPressed };
                auto arrow = [&](F32 b0, F32 t0, bool up, EHit id, const std::string& tip, bool on)
                {
                    roundRectF(bl, b0, br, t0, S(5.f), on ? BTN_MAIN_HI : BTN_OFF_HI, on ? BTN_MAIN_LO : BTN_OFF_LO);
                    frameW(bl, b0, br, t0, 1.f, alpha(SNOW, on ? 0.55f : 0.2f));
                    const F32 mx = (bl + br) * 0.5f, my = (b0 + t0) * 0.5f, h = (t0 - b0) * 0.22f, w = (br - bl) * 0.3f;
                    if (up) triF(mx - w, my - h, mx + w, my - h, mx, my + h, on ? SNOW : GREY);
                    else triF(mx - w, my + h, mx + w, my + h, mx, my - h, on ? SNOW : GREY);
                    if (on) sc.hit(bl, b0, br, t0, id, tip);
                };
                arrow(upB, upT, true, H_SCROLL_UP, "Newer messages", mScroll > 0);
                arrow(dnB, dnT, false, H_SCROLL_DOWN, "Older messages", mScroll + shown < mMsgTotal);
                // the track and its thumb: how much of the history shows, and where
                const F32 tT = upB - S(4.f), tB = dnT + S(4.f), th = tT - tB;
                rectF(bl + S(6.f), tB, br - S(6.f), tT, alpha(NAVY_LO, 0.6f));
                const F32 n = (F32)llmax(1, mMsgTotal);
                const F32 thumbH = llmax(S(18.f), th * (F32)llmax(1, shown) / n);
                const F32 thumbT = tT - (th - thumbH) * ((F32)mScroll / llmax(1.f, n - (F32)shown));
                roundRectF(bl + S(3.f), thumbT - thumbH, br - S(3.f), thumbT, S(4.f), SNOW, ICE_DIM);
                mTrackT = tT; mTrackB = tB; mThumbT = thumbT; mThumbB = thumbT - thumbH;
                sc.hit(bl, tB, br, tT, H_SCROLL_TRACK, "The message history: drag, click, or use the mouse wheel to scroll back");
                sc.hit(bl, thumbT - thumbH, br, thumbT, H_SCROLL_THUMB, "Drag to scroll through the messages");
            }
        }
    }

    // ── RIGHT: the buttons, big and clear (memory paul-simple-for-older-disabled-users) ──
    use(g.R);
    {
        const bool castOk = wolf && s.rod() != WolfFishingSession::ROD_FINDING
            && (s.rod() == WolfFishingSession::ROD_NONE || s.fish() == WolfFishingSession::FISH_IDLE);
        const bool noRod = s.rod() == WolfFishingSession::ROD_NONE;
        button(c, X(R_L), YM(700.f), X(R_L + 268.f), YM(612.f), noRod ? "GET MY ROD" : "CAST", H_CAST,
               noRod ? "Put your fishing rod in your hand (you get one if you have none)"
                     : "Cast your line into the water in front of you. Face the water first.", 16.f,
               BTN_MAIN_HI, BTN_MAIN_LO, castOk);
        // at a bite REEL IN glows green and breathes with the state line (Paul 11-11: the bite noticeable, no big box)
        const LLColor4 go_hi = bite ? lerp(BTN_GO_HI, LLColor4(0.30f, 0.80f, 0.48f, 0.97f), beat) : BTN_MAIN_HI;
        button(c, X(R_L + 280.f), YM(700.f), X(1588.f), YM(612.f), "REEL IN", H_REEL,
               s.challengeOn() ? "Challenge mode: hold this down to bring the fish in, and let go before the line goes into the red."
                               : "Reel in your line and see what you have caught. There is no hurry - a fish that bites waits for you.", 16.f,
               go_hi, bite ? BTN_GO_LO : BTN_MAIN_LO, ready && lineOut);
        button(c, X(R_L), YM(780.f), X(R_L + 92.f), YM(716.f), "STOP", H_STOP, "Take your line out of the water without reeling in", 12.f,
               BTN_HI, BTN_LO, ready && lineOut);
        button(c, X(R_L + 97.f), YM(780.f), X(R_L + 189.f), YM(716.f), "BOARD", H_BOARD,
               "Your week and this week's top 10 on Wolf Territories (the Fishing window)", 12.f, BTN_HI, BTN_LO, wolf);
        const bool ch = s.challengeOn();
        // the word as well as the light: ON / OFF (never colour alone)
        button(c, X(R_L + 194.f), YM(780.f), X(R_L + 344.f), YM(716.f), ch ? "CHALLENGE: ON" : "CHALLENGE: OFF", H_CHALLENGE,
               ch ? "Challenge mode is on: hold REEL IN and keep the line out of the red. Click to switch it off (one press reels in, no hurry)."
                  : "Challenge mode is off: one press of REEL IN brings the fish in, no hurry. Click to switch it on: hold REEL IN and keep the line out of the red.",
               11.f, ch ? BTN_MAIN_HI : BTN_HI, BTN_LO, wolf);
        // Source: wolfskihud.cpp PANEL - here BACKGROUND (Paul 11-11: "allow users to turn off the background"), per resident
        button(c, X(R_L + 349.f), YM(780.f), X(R_L + 489.f), YM(716.f), bg ? "BACKGROUND: ON" : "BACKGROUND: OFF", H_PANEL,
               bg ? "The night-blue background is on. Click to take it away and see more of the world: just the buttons and words stay."
                  : "The background is off. Click to put the night-blue background back behind the HUD.", 10.f,
               bg ? BTN_MAIN_HI : BTN_HI, BTN_LO);
        button(c, X(R_L + 494.f), YM(780.f), X(1588.f), YM(716.f), "CLOSE", H_CLOSE,
               "Close Fishing. Your rod stays in your hand: touch it to fish from its menu (Wolf World > Fishing brings this back).", 11.f,
               BTN_HI, BTN_LO);
        // the Wolf Fishing logo (our own art: region-terrain-work/fishing-t679-2026-11-10/art/make_fishing_art.py)
        if (LLUIImagePtr logo = LLUI::getUIImage("WolfFishing_Logo"))
        {
            gGL.color4fv(LLColor4::white.mV);
            logo->draw((S32)llround(X(1500.f)), (S32)llround(YM(892.f)), (S32)llround(S(84.f)), (S32)llround(S(84.f)));
        }
        T(X(1490.f), YM(838.f), "WOLF FISHING", 12.f, bg ? ICE_DIM : ICE, true, false, LLFontGL::RIGHT);
    }

    drawPile(g.panelTop);

    zoomed.reset();
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mResize.draw();
}

namespace
{
    // ── today's catch: the pictures (all drawn here, our own: a body, a tail, a fin, an eye, a pattern) ──
    void ellipseF(F32 cx, F32 cy, F32 rx, F32 ry, const LLColor4& c, S32 seg = 24)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * (F32)i / (F32)seg, a1 = 2.f * PI_F * (F32)(i + 1) / (F32)seg;
            vtx(cx, cy, c);
            vtx(cx + rx * cosf(a0), cy + ry * sinf(a0), c);
            vtx(cx + rx * cosf(a1), cy + ry * sinf(a1), c);
        }
        gGL.end();
    }

    /** One fish, `w` long, its nose toward +x (or -x when `flip`), centred on (cx, cy). */
    void drawFish(F32 cx, F32 cy, F32 w, const FishLook& k, bool flip)
    {
        const F32 d = flip ? -1.f : 1.f;
        F32 rx = w * 0.40f, ry = w * 0.17f;
        if (k.mShape == SHAPE_LONG) { rx = w * 0.46f; ry = w * 0.10f; }
        else if (k.mShape == SHAPE_DEEP) { rx = w * 0.34f; ry = w * 0.24f; }
        else if (k.mShape == SHAPE_FLAT) { rx = w * 0.34f; ry = w * 0.26f; }
        const LLColor4 dark = lerp(k.mBody, LLColor4(0.02f, 0.05f, 0.09f, 1.f), 0.45f);
        const LLColor4 belly = lerp(k.mBody, LLColor4(0.97f, 0.98f, 1.f, 1.f), 0.45f);
        // the tail, the fins, then the body over them
        const F32 tx = cx - d * rx * 0.92f;
        const F32 tail = k.mShape == SHAPE_FLAT ? 0.6f : 1.f;
        triF(tx, cy, tx - d * w * 0.20f * tail, cy + ry * 1.25f * tail, tx - d * w * 0.20f * tail, cy - ry * 1.25f * tail, k.mFin);
        triF(cx - d * rx * 0.3f, cy + ry * 0.8f, cx + d * rx * 0.25f, cy + ry * 0.8f, cx - d * rx * 0.15f, cy + ry * 1.55f, k.mFin);
        ellipseF(cx, cy, rx * 1.04f, ry * 1.12f, dark);
        ellipseF(cx, cy, rx, ry, k.mBody);
        ellipseF(cx + d * rx * 0.05f, cy - ry * 0.38f, rx * 0.82f, ry * 0.5f, belly);
        if (k.mPattern == PATTERN_STRIPES)
        {
            for (S32 i = 0; i < 3; ++i)
            {
                const F32 x = cx - d * rx * (0.35f - 0.3f * (F32)i);
                rectF(x - w * 0.012f, cy, x + w * 0.012f, cy + ry * 0.85f, alpha(k.mFin, 0.85f));
            }
        }
        else if (k.mPattern == PATTERN_SPOTS)
        {
            static const F32 P[5][2] = { { -0.4f, 0.35f }, { -0.1f, 0.55f }, { 0.2f, 0.3f }, { 0.05f, 0.05f }, { -0.3f, -0.05f } };
            for (const auto& p : P) ellipseF(cx + d * rx * p[0], cy + ry * p[1], w * 0.025f, w * 0.025f, alpha(k.mFin, 0.9f), 8);
        }
        // the eye: white, dark, a glint - a happy fish
        const F32 ex = cx + d * rx * 0.62f, ey = cy + ry * 0.25f, er = llmax(w * 0.055f, ry * 0.28f);
        ellipseF(ex, ey, er, er, LLColor4(0.98f, 0.99f, 1.f, 1.f), 12);
        ellipseF(ex + d * er * 0.25f, ey, er * 0.55f, er * 0.55f, LLColor4(0.03f, 0.08f, 0.13f, 1.f), 10);
        ellipseF(ex + d * er * 0.4f, ey + er * 0.25f, er * 0.18f, er * 0.18f, LLColor4::white, 6);
    }

    /** Something that is not a fish: a boot, a sun hat, weed, a piece of wood. */
    void drawJunk(F32 cx, F32 cy, F32 w, S32 look)
    {
        const LLColor4 brown(0.42f, 0.27f, 0.14f, 1.f), dark(0.18f, 0.11f, 0.05f, 1.f);
        switch (look)
        {
        case JUNK_BOOT:
            roundRectF(cx - w * 0.12f, cy - w * 0.12f, cx + w * 0.10f, cy + w * 0.22f, w * 0.04f, brown, dark);
            roundRectF(cx - w * 0.12f, cy - w * 0.16f, cx + w * 0.34f, cy, w * 0.07f, brown, dark);
            rectF(cx - w * 0.14f, cy - w * 0.19f, cx + w * 0.36f, cy - w * 0.14f, dark);
            break;
        case JUNK_HAT:
            ellipseF(cx, cy - w * 0.04f, w * 0.38f, w * 0.10f, LLColor4(0.93f, 0.80f, 0.45f, 1.f));
            ellipseF(cx, cy + w * 0.05f, w * 0.19f, w * 0.15f, LLColor4(0.96f, 0.85f, 0.52f, 1.f));
            rectF(cx - w * 0.19f, cy - w * 0.01f, cx + w * 0.19f, cy + w * 0.04f, LLColor4(0.85f, 0.25f, 0.30f, 1.f));
            break;
        case JUNK_WEED:
            for (S32 i = 0; i < 5; ++i)
            {
                const F32 x = cx - w * 0.2f + w * 0.1f * (F32)i;
                ellipseF(x, cy, w * 0.035f, w * (0.16f + 0.05f * (F32)(i % 3)), LLColor4(0.20f, 0.55f + 0.06f * (F32)(i % 2), 0.25f, 1.f), 10);
            }
            break;
        default:   // JUNK_WOOD
            roundRectF(cx - w * 0.38f, cy - w * 0.08f, cx + w * 0.38f, cy + w * 0.08f, w * 0.08f, LLColor4(0.62f, 0.50f, 0.36f, 1.f), brown);
            ellipseF(cx + w * 0.36f, cy, w * 0.05f, w * 0.075f, LLColor4(0.80f, 0.68f, 0.50f, 1.f), 10);
            break;
        }
    }
}

// Above the band at the left: the pile of today's catch, growing as they catch (Paul 11-11: "a pile of fish as they catch
// them lols"). A heap of up to PILE_SHOWN pictures, the newest on top; past that a bigger heap and the count. It is this
// session's (it goes when Fishing closes) or TIP THEM BACK empties it - the fish were weighed and let go in the game anyway.
void WolfFishingHud::drawPile(F32 band_top)
{
    const WolfFishingSession& s = WolfFishingSession::instance();
    const std::vector<WolfFishingSession::PileItem>& pile = s.pile();
    if (pile.empty()) return;
    use(g.L);
    const F32 fw = S(90.f);            // one fish's length
    const F32 rowH = S(28.f);
    const F32 base = band_top + S(20.f);
    const F32 left = X(30.f);
    // rows of 6, 5, 4: a pyramid, the oldest at the bottom
    static const S32 ROW[] = { 6, 5, 4 };
    const bool heap = s.pileCount() > WolfFishingSession::PILE_SHOWN;
    if (heap)
    {
        // more than the pile shows: a heap of shadow-fish under it
        ellipseF(left + fw * 3.f, base - S(4.f), fw * 3.4f, S(26.f), alpha(NAVY_LO, 0.55f), 32);
    }
    size_t n = 0;
    for (S32 r = 0; r < 3 && n < pile.size(); ++r)
    {
        for (S32 i = 0; i < ROW[r] && n < pile.size(); ++i, ++n)
        {
            const F32 cx = left + fw * 0.5f + fw * (F32)i + fw * 0.5f * (F32)r + (F32)((n * 7) % 5) * S(1.5f);
            const F32 cy = base + rowH * (F32)r + (F32)((n * 3) % 4) * S(1.5f);
            const WolfFishingSession::PileItem& it = pile[n];
            if (it.mJunk) drawJunk(cx, cy, fw, it.mLook);
            else drawFish(cx, cy, fw, FISH_LOOKS[llclamp(it.mLook, 0, FISH_LOOK_COUNT - 1)], (n % 2) == 1);
        }
    }
    // the label, the count and the button, above the heap
    const S32 rows = pile.size() > 11 ? 3 : (pile.size() > 6 ? 2 : 1);
    const F32 ly = base + rowH * (F32)rows + S(28.f);
    const std::string label = s.pileCount() == 1 ? std::string("TODAY'S CATCH: 1") : llformat("TODAY'S CATCH: %d", s.pileCount());
    T(left, ly, label, 13.f, GOLD, true, false, LLFontGL::LEFT);
    Ctx c{ [this](F32 a, F32 bb, F32 r, F32 tt, EHit id, const std::string& tip) { addHit(a, bb, r, tt, id, tip); }, mPressed };
    const F32 bl = left + S(290.f);
    button(c, bl, ly - S(18.f), bl + S(200.f), ly + S(18.f), "TIP THEM BACK", H_TIP,
           "Tip today's catch back into the water (the pile starts again; your week's score stays)", 11.f, BTN_HI, BTN_LO);
}

bool WolfFishingHud::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return true;   // the panel itself: not a click on the world behind it
    }
    mPressed = hit->mId;
    gFocusMgr.setMouseCapture(this);
    const F32 py = (F32)y / mHitScale;
    if (hit->mId == H_SCROLL_TRACK)
    {
        // a page of history up or down, toward where the track was clicked
        const S32 page = llmax(1, mMsgShown);
        if (py > mThumbT) mScroll = llmax(0, mScroll - page);
        else if (py < mThumbB) mScroll = llmin(llmax(0, mMsgTotal - 1), mScroll + page);
        return true;
    }
    if (hit->mId == H_SCROLL_THUMB)
    {
        mDragY = py;
        mDragScroll = mScroll;
        return true;
    }
    press(hit->mId);
    return true;
}

bool WolfFishingHud::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    // the mouse wheel over the message area scrolls its history (down: older)
    if (!mDrawn) return false;
    const F32 px = (F32)x / mHitScale, py = (F32)y / mHitScale;
    if (px < mMsgL || px > mMsgR || py > mMsgT || py < mMsgB) return false;
    mScroll = llclamp(mScroll + clicks, 0, llmax(0, mMsgTotal - 1));
    return true;
}

bool WolfFishingHud::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE || mResize.dragging() || mResize.onGrip(x, y);
    mResize.end();
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    mPressed = H_NONE;
    return was || onPanel(x, y);
}

bool WolfFishingHud::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mPressed == H_SCROLL_THUMB)
    {
        // dragging the thumb: the history follows it (down the track = older)
        const F32 th = llmax(1.f, (mTrackT - mTrackB) - (mThumbT - mThumbB));
        const F32 per = (F32)llmax(1, mMsgTotal - mMsgShown) / th;
        mScroll = llclamp(mDragScroll + (S32)llround((mDragY - (F32)y / mHitScale) * per), 0, llmax(0, mMsgTotal - 1));
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    if (hasMouseCapture() && mResize.dragging())
    {
        mResize.drag(y);
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && mDrawn && mResize.onGrip(x, y))
    {
        mResize.hovered();
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && !onPanel(x, y))
    {
        return false;
    }
    getWindow()->setCursor(hitAt(x, y) ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfFishingHud::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfFishingHud::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    // a double-click on a switch would switch it twice (back where it was): the second click of a switch is ignored
    // (many residents double-click buttons); every other button takes it as one more press, which does nothing more
    if (const Hit* hit = hitAt(x, y))
    {
        if (hit->mId == H_CHALLENGE || hit->mId == H_PANEL || hit->mId == H_CLOSE || hit->mId == H_BOARD || hit->mId == H_TIP) return true;
    }
    return handleMouseDown(x, y, mask);
}

bool WolfFishingHud::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        if (!hit->mTip.empty()) LLToolTipMgr::instance().show(hit->mTip);
    }
    return true;
}

void WolfFishingHud::onMouseCaptureLost()
{
    mResize.end();
    mPressed = H_NONE;
    LLUICtrl::onMouseCaptureLost();
}
