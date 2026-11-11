/**
 * @file wolffloatergreedy.cpp
 * @brief WolfViewer: Greedy, the dice game window. See wolffloatergreedy.h.
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

#include "wolffloatergreedy.h"

#include <cstdlib>

#include <boost/json.hpp>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "llchat.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"   // [W104 FEATFIX review/] llcoro::suspendUntilTimeout (WolfGreedyDebugDelaySecs)
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "lliconctrl.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llstyle.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llui.h"
#include "wolfdrive.h"   // [W104 FEATFIX review/] one bottom panel at a time (bottomTakenBy / claimBottom)
#include "wolfflight.h"
#include "wolfgame.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfskis.h"
#include "wolfobjectprops.h"

namespace
{
    // Source: region-terrain-work/greedy-dice-2026-11-10/server/deploy.sh - the same file on both web hosts (one database)
    const char* API_1 = "https://wolf-grid.com/wolfstorm/php/wolfdice.php";
    const char* API_2 = "https://wolfstorm.app/php/wolfdice.php";
    // Source: lsl/wolfdice_table.lsl HELLO_CH and tellWindow() (README s3c)
    const S32 HELLO_CHANNEL = 7171;
    const char* const HELLO = "greedy hello 1";
    const std::string PREFIX("WolfGreedy1|");
    const F64 HELLO_RETRY_S = 3.0;
    const S32 HELLO_TRIES = 10;
    const size_t MAX_LINE = 200;
    // Source: wolfdice_window.php - a game going is polled every 2 s, a lobby or a finished game every 4 s (README s3)
    const F64 POLL_PLAYING_S = 2.0;
    const F64 POLL_WAITING_S = 4.0;
    // [W104 FEATFIX 2026-11-11 A6] Paul: think of 2,000 users. With nothing happening the pace slows (CHOSEN): the wait is
    // 0.15 x the seconds since the last change or press, never under the pace above and never over POLL_PLAYING_MAX_S
    // (a game going: other players' turns must still show soon) or POLL_LOBBY_MAX_S (a lobby), and after POLL_LONG_QUIET_S
    // of nothing at all, POLL_QUIET_MAX_S. A finished game is not polled at all (it never changes again); a minimised
    // window is not polled; a press, a change or opening the window out again brings back the quick pace at once.
    // An open window left alone in a lobby all day: about 1,500 requests (was 21,600).
    const F64 POLL_SLOWING = 0.15;
    const F64 POLL_PLAYING_MAX_S = 8.0;
    const F64 POLL_LOBBY_MAX_S = 15.0;
    const F64 POLL_LONG_QUIET_S = 600.0;
    const F64 POLL_QUIET_MAX_S = 60.0;
    // [W104 FEATFIX review 2026-11-11] a game going is never polled less than every 15 s (others' turns still show soon); the
    // 60 s is for a lobby only
    const F64 POLL_PLAYING_QUIET_MAX_S = 15.0;
    // Source: region-terrain-work/greedy-dice-2026-11-10/test/delivery_lonewolf.json "sounds" (our own, art/make_art.py)
    const char* SND_ROLL = "2888b686-e2c2-48c0-bc92-3ec2ad4c63fd";
    const char* SND_BUST = "db99ff28-3bc3-432a-a991-a4bf8a4adf11";
    const char* SND_BANK = "51193b59-63fe-47e6-a028-5906ffe0f628";
    const char* SND_LUCKY = "2c68cbc9-aecc-4cc8-a160-d3aba2042140";
    const char* SND_WIN = "45be77bb-2278-45a5-95a9-c0109229fe77";

    // The guide: the same nine pages as the table's (lsl/wolfdice_tutorial.lsl PAGES), each with its example dice.
    // [W104 FEATFIX 2026-11-11] Paul: "it says page 1 of 9, it looks ugly", "no fun". Each page is now a card: a title, the
    // words, and on the two scoring pages one row of dice pictures for each combination (the same scores, in the same words);
    // the page number is the dots under the card (their tool tip says "Page 2 of 9"). The words are the old pages' words.
    struct GuideRow { const char* dice; const char* label; };
    struct GuidePage { const char* title; const char* body; const char* dice; const char* kept; GuideRow rows[6]; };
    const GuidePage GUIDE[] = {
        { "How to play", "Welcome! This takes 2 minutes.\n\nGreedy is a game of luck and good sense for 1 to 8 players.\n\n"
          "Roll six dice and collect points. The first to 10,000 wins.", "000000", "000000", {} },
        { "Rolling the dice", "Press ROLL and the six dice are rolled for you - by the Greedy server, so nobody can cheat.\n\n"
          "Dice that score turn GOLD and are put aside for you.\n\nHere a 1 and a 5 scored: 100 + 50 = 150 points.", "153326", "110000", {} },
        { "What scores", "Here: 800 + 100 = 900.", "444412", "111110",
          { { "1", "A single 1: 100" }, { "5", "A single 5: 50" }, { "111", "Three 1s: 1,000" },
            { "222", "Three 2s: 200, three 3s: 300, and so on" }, { "4444", "Each extra matching die doubles it: four 4s = 800" } } },
        { "Special rolls", "In one roll.", "123456", "111111",
          { { "12345", "1 2 3 4 5 or 2 3 4 5 6: 1,000" }, { "123456", "1 2 3 4 5 6: 1,500" }, { "224466", "Three pairs: 1,500" },
            { "666622", "Four alike and a pair: 2,000 (four 1s and a pair: 2,500)" }, { "333444", "Two threes alike: 2,000" },
            { "111111", "Six 1s: 8,000" } } },
        { "Roll again or bank?", "After a roll that scores, you choose: ROLL the dice that are left to add more points, "
          "or BANK to keep this turn's points. Then it is the next player's turn.\n\nFewer dice left means more risk.", "152263", "110000", {} },
        { "Going bust", "No 1s, no 5s and nothing special means BUST: the points from that turn are lost, and the "
          "next player goes.\n\nA star * shows each bust. On your third bust your score goes back to 0 - take care!", "234623", "xxxxxx", {} },
        { "Lucky six", "When every die has scored, you may roll all six again and keep adding to your turn.\n\n"
          "Or BANK, if you would rather be safe. It is up to you.\n\nHere: two threes alike, 2,000.", "111555", "111111", {} },
        { "Winning", "The first to bank 10,000 (or that game's target) starts the LAST TURNS: everyone else has one more "
          "turn to beat them. The highest score wins.\n\nThere is no clock. Take all the time you need.", "000000", "000000", {} },
        { "Ready?", "Let's play!\n\nNew game starts a game others can join from their Greedy window or a Greedy table. "
          "Join plays in a game going now.\n\nPRACTICE is a game on your own with tips. Nothing is saved and it is not ranked.\n\n"
          "HOW TO PLAY shows this again.", "000000", "000000", {} },
    };
    const S32 GUIDE_PAGES = S32(sizeof(GUIDE) / sizeof(GUIDE[0]));

    // Source: wolffloaterdob.cpp json_to_llsd (wolfviewerkey.cpp)
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    /** [W104 FEATFIX 2026-11-11 A3] The requests that are safe to send twice (wolfdice_window.php): mine, list and poll only
     *  read (poll's last_seen touch and away-pass are the same whoever asks), and join of a game the player is already in only
     *  touches last_seen (wdw_join). new, start, roll, bank, pass and leave change the game: a second copy rolls again,
     *  starts a second game, passes the NEXT player's turn, or answers "not in that game". */
    bool safe_to_repeat(const std::string& op)
    {
        return op == "mine" || op == "list" || op == "poll" || op == "join";
    }

    /** [W104 FEATFIX 2026-11-11 A3] The request certainly never reached the server: no name lookup, no connection, or the TLS
     *  handshake failed - all before curl sends the request (curl CURLE_COULDNT_RESOLVE_HOST 6, CURLE_COULDNT_CONNECT 7,
     *  CURLE_SSL_CONNECT_ERROR 35; httpcommon.cpp isRetryable names the first two). Only then may a change go to the other host. */
    bool never_sent(const LLCore::HttpStatus& status)
    {
        return status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_COULDNT_RESOLVE_HOST)
            || status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_COULDNT_CONNECT)
            || status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_SSL_CONNECT_ERROR);
    }

    /** One request to the dice server as this login. Runs in a coroutine. Source: wolffloaterdob.cpp post_dob.
     *  [W104 FEATFIX 2026-11-11 A3] repeat = false: LLCore's own retries are off (HttpOptions default HTTP_RETRY_COUNT_DEFAULT = 5,
     *  _httppolicy.cpp stageAfterCompletion retries a timed-out or 5xx request, which for a roll would roll again). */
    LLSD post_greedy(const char* url, const std::string& body, LLCore::HttpStatus& status, bool repeat)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfGreedy", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        if (!repeat) options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }

    std::string number(S32 n)
    {
        // 10,000 style, as the server's lines (wolfdice_lib.php wd_n)
        std::string s = llformat("%d", n < 0 ? -n : n), out;
        S32 c = 0;
        for (size_t i = s.size(); i-- > 0;)
        {
            out.insert(out.begin(), s[i]);
            if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
        }
        return n < 0 ? "-" + out : out;
    }

    LLViewerObject* seat_root()
    {
        // Source: wolfskis.cpp seatRoot() (wolfhelm.cpp)
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting()) return nullptr;
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent()) p = (LLViewerObject*)p->getParent();
        return p;
    }

    const LLColor4 YELLOW(1.f, 0.86f, 0.35f, 1.f);
    const LLColor4 GREEN(0.45f, 1.f, 0.55f, 1.f);
    const LLColor4 GREY(0.65f, 0.65f, 0.7f, 1.f);
    const LLColor4 RED_TEXT(1.f, 0.6f, 0.6f, 1.f);
    const LLColor4 GOLD_DIE(1.f, 0.8f, 0.3f, 1.f);
    const LLColor4 RED_DIE(1.f, 0.42f, 0.4f, 1.f);
    // [W104 FEATFIX 2026-11-11] the game's moments (CHOSEN): the dice tumble about a second (12 rendered frames,
    // greedy-art/render_dice.py), then the banner ("Bust!", "You banked 350!") pulses for its first 0.6 s and goes after
    // 2.5 s - nothing to react to, and the event line under the dice keeps the words. A click on the window ends the
    // tumble; Reduce motion (WolfAdventureReduceMotion, the viewer's own) shows the result and a still banner at once.
    const S32 ROLL_FRAMES = 12;
    const F64 ROLL_SECS = 0.96;
    const F64 BANNER_PULSE_SECS = 0.6;
    const F64 BANNER_SECS = 2.5;
    const LLColor4 BOARD_ME(0.55f, 1.f, 0.62f, 1.f);

    bool reduce_motion()
    {
        static LLCachedControl<bool> reduce(gSavedPerAccountSettings, "WolfAdventureReduceMotion", false);
        return reduce;
    }
}

namespace
{
    void playSound(const std::string& kind)
{
    const char* id = kind == "roll" ? SND_ROLL : kind == "bust" ? SND_BUST : kind == "bank" ? SND_BANK
                   : kind == "lucky" ? SND_LUCKY : kind == "win" ? SND_WIN : nullptr;
    if (!id || !gAudiop) return;
    gAudiop->triggerSound(LLUUID(id), gAgentID, 0.8f, LLAudioEngine::AUDIO_TYPE_UI);
}
}

// ----------------------------------------------------------------------------------------------------------- the session

// [W104 FEATFIX 2026-11-11] wolffloatergreedy.h WolfGreedyGame: what the window did alone before - the requests, the polling,
// the game - now shared by the HUD and the window.
void WolfGreedyGame::viewOpened(EView v)
{
    const bool was = mViews != 0;
    mViews |= (U32)v;
    if (mHeldImages.empty())
    {
        // [W104 FEATFIX review 2026-11-11] the dice faces and the tumble's frames, loaded as a view opens (the HUD on its own as
        // well as the window), so the first roll does not flicker while they arrive
        for (S32 i = 0; i < ROLL_FRAMES; ++i) mHeldImages.push_back(LLUI::getUIImage(llformat("WolfGreedy_Roll%d", i)));
        for (S32 i = 0; i <= 6; ++i) mHeldImages.push_back(LLUI::getUIImage(llformat("WolfGreedy_Die%d", i)));
    }
    noteActivity();   // [A6] opened (again): the quick pace, and a poll at once
    mNextPoll = 0.0;
    if (!WolfGrid::isWolfTerritories())
    {
        setMessage("Greedy is for Wolf Territories logins. Log in to Wolf Territories to play.", true);
        return;
    }
    if (!was && !mGame)
    {
        send("mine");
        send("list");
    }
    changed();
}

void WolfGreedyGame::viewClosed(EView v)
{
    mViews &= ~(U32)v;
    changed();
}

// static
void WolfGreedyGame::openHud()
{
    WolfGreedyGame& g = instance();
    if (g.hudOn()) return;
    if (WolfGrid::isOnWolfTerritories()) claimBottom();   // [W104 FEATFIX review 2026-11-11/] one bottom panel at a time
    g.viewOpened(VIEW_HUD);
    // the first time: the guide's cards (as the window did, WolfGreedyGuideSeen)
    if (WolfGrid::isWolfTerritories() && !gSavedSettings.getBOOL("WolfGreedyGuideSeen"))
    {
        gSavedSettings.setBOOL("WolfGreedyGuideSeen", true);
        WolfFloaterGreedy::showGuideWindow();
    }
}

// static
std::string WolfGreedyGame::bottomTakenBy()
{
    // Source: wolfgame.cpp WolfGame::bottomTaken (the deck on screen while active and not hidden; the dashboard; the helm),
    // and the Ski HUD and the game HUD, which give way to each other the same way (wolfskis.cpp, wolfgame.cpp claimBottom)
    const WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) return wf.sailing() ? "sailing deck" : "flight deck";
    if (WolfDrive::instance().dashboardOn()) return "dashboard";
    if (WolfHelm::instance().hudShowing()) return "Captain's Helm";
    if (WolfSkis::instance().hudShowing()) return "Ski HUD";
    if (WolfGame::instance().hudShowing()) return "game HUD";
    return std::string();
}

// static
void WolfGreedyGame::claimBottom()
{
    // Source: wolfskis.cpp WolfSkis::claimBottom - the dashboard closes (requestDashboard(false) does not reopen it for this
    // seat), the deck is hidden (its mode keeps running), the Captain's Helm gives way; and here the Ski HUD and the game HUD
    // too (the game keeps running with its HUD hidden, WolfGame::toggleHud)
    std::string put_away = bottomTakenBy();
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
    if (WolfHelm::instance().hudShowing()) WolfHelm::instance().showHud(false);
    if (WolfSkis::instance().hudShowing()) WolfSkis::instance().showHud(false);
    if (WolfGame::instance().hudShowing()) WolfGame::instance().toggleHud();
    if (!put_away.empty())
    {
        instance().setMessage("The " + put_away + " was put away so Greedy can sit along the bottom of the screen "
                              "(the Wolf HUDs window brings it back).", false);
    }
}

// static
void WolfGreedyGame::closeHud()
{
    if (instanceExists()) instance().viewClosed(VIEW_HUD);
}

// static
void WolfGreedyGame::toggleHud()
{
    if (instanceExists() && instance().hudOn()) closeHud();
    else openHud();
}

void WolfGreedyGame::noteActivity()
{
    mLastActivity = LLFrameTimer::getElapsedSeconds();
}

void WolfGreedyGame::setMessage(const std::string& text, bool error)
{
    mMessage = text;
    mMessageError = error;
    if (!text.empty())
    {
        Msg m;
        m.mText = text;
        m.mError = error;
        mLog.insert(mLog.begin(), m);
        if (mLog.size() > 6) mLog.resize(6);   // (the HUD shows the newest few)
    }
    changed();
}

void WolfGreedyGame::tick(bool minimised)
{
    moments();
    // [W104 FEATFIX 2026-11-11 A6] polled only while a view shows; the window minimised (and no HUD): nothing, and opened out
    // again it polls at once
    if (minimised && !hudOn())
    {
        mWasMinimised = true;
        return;
    }
    if (mWasMinimised)
    {
        mWasMinimised = false;
        noteActivity();
        mNextPoll = 0.0;
    }
    if (!mGame || mBusy || !WolfGrid::isWolfTerritories()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!mPollNow)
    {
        if (now < mNextPoll) return;
        if (mState["game"]["state"].asString() == "finished") return;   // [A6] it never changes again
    }
    mPollNow = false;
    send("poll", "&v=" + llformat("%d", mVersion));
}

void WolfGreedyGame::roll()
{
    const std::string b = mState["game"]["buttons"].asString();
    if (!mGame) { setMessage("Start a new game, or join one, first.", true); return; }
    if (b.find('S') != std::string::npos || b.find('A') != std::string::npos) send("start");
    else if (b.find('R') != std::string::npos) send("roll");
    else setMessage("It is not your turn yet.", false);
}

void WolfGreedyGame::bank()
{
    const std::string b = mState["game"]["buttons"].asString();
    if (b.find('B') != std::string::npos) send("bank");
    else if (b.find('R') != std::string::npos) setMessage("Roll first - there is nothing to bank yet.", false);
    else if (mGame) setMessage("It is not your turn yet.", false);
}

void WolfGreedyGame::newGame(bool practice, S32 target)
{
    if (mGame && mState["game"]["in_game"].asBoolean() && mState["game"]["state"].asString() != "finished")
    {
        send("leave");   // one game at a time; the queue keeps the order
    }
    std::string region = gAgent.getRegion() ? gAgent.getRegion()->getName() : std::string();
    send("new", llformat("&target=%d", practice ? 2000 : target) + (practice ? "&practice=1" : "") + "&region=" + LLURI::escape(region));
}

void WolfGreedyGame::join(S32 id)
{
    if (id <= 0) { setMessage("Choose a game from the list first (Refresh looks again).", false); return; }
    if (mGame && mGame != id && mState["game"]["in_game"].asBoolean()) send("leave");
    mVersion = -1;
    send("join", llformat("&g=%d", id));   // the answer's game.id becomes mGame (onReply)
}

void WolfGreedyGame::leave()
{
    if (mGame) send("leave");
}

void WolfGreedyGame::pass()
{
    if (mGame) send("pass");
}

void WolfGreedyGame::refreshList()
{
    send("list");
}

void WolfGreedyGame::onTable(S32 game)
{
    if (game <= 0) return;
    noteActivity();   // [A6/]
    if (mGame != game)
    {
        // the table has already seated them in this game: this just loads it (wolfdice_window.php wdw_join). The game
        // goes in the request itself ("&g=" last wins in PHP), so a "leave" still out names the old one.
        mVersion = -1;
        send("join", llformat("&g=%d", game));
    }
}

void WolfGreedyGame::send(const std::string& op, const std::string& extra)
{
    if (mBusy)
    {
        // the newest press wins; it goes when the answer comes. [W104 FEATFIX review 2026-11-11] the same press as the one out
        // now is dropped: a second ROLL / BANK / NEW GAME pressed before the first was answered was made on an unchanged
        // screen, and sent after it would roll twice or make a second game. (A different press - "leave" then "new" - waits.)
        if (op != "poll" && op != mInFlight) mQueued = op + "\n" + extra;
        else if (op != "poll") LL_INFOS("WolfGreedy") << "a second " << op << " while the first is out: dropped" << LL_ENDL;
        return;
    }
    mBusy = true;
    mInFlight = op;
    if (op != "poll") noteActivity();   // [A6/] a press: the quick pace again
    // [A6/] DEBUG only (logcontrol tag WolfGreedy): every request, to count the polls of an idle view
    LL_DEBUGS("WolfGreedy") << "request " << op << " game " << mGame << " quiet " << (S32)(LLFrameTimer::getElapsedSeconds() - mLastActivity) << " s" << LL_ENDL;
    const std::string body = "op=" + op + "&g=" + llformat("%d", mGame) + extra;
    changed();
    LLCoros::instance().launch("WolfGreedy", [op, body]() { sendCoro(op, body); });
}

// static
void WolfGreedyGame::sendCoro(std::string op, std::string body)
{
    LLCore::HttpStatus status;
    // [W104 FEATFIX review 2026-11-11] for tests only (off: 0): every request waits this long first, a slow link
    static LLCachedControl<F32> debug_delay(gSavedSettings, "WolfGreedyDebugDelaySecs", 0.f);
    if (debug_delay() > 0.f) llcoro::suspendUntilTimeout(llmin((F32)debug_delay(), 30.f));
    // [W104 FEATFIX 2026-11-11 A3] a read is asked again (LLCore's retries, then the second host); a change goes once, and to
    // the second host only when it certainly never reached the first (never_sent): no roll, bank or new game twice
    const bool safe = safe_to_repeat(op);
    LLSD reply = post_greedy(API_1, body, status, safe && op != "poll");   // a poll is asked again soon anyway
    if (!reply.isMap() && (safe || never_sent(status)))
    {
        // the first web host did not answer: the second has the same file and the same database (README s3)
        reply = post_greedy(API_2, body, status, safe && op != "poll");
    }
    std::string error;
    bool uncertain = false;
    if (!reply.isMap() || !reply["ok"].asBoolean())
    {
        error = reply.isMap() && reply.has("error") ? reply["error"].asString() : std::string();
        // no answer to a change that may have reached the server: whether it counted is not known
        uncertain = !reply.isMap() && !safe && !never_sent(status);
        if (error.empty() && uncertain)
            error = "The Greedy server did not answer (" + status.toString() + "), so that press may or may not have counted. "
                    "The game is being checked now - look at it before you press again.";
        else if (error.empty())
            error = "The Greedy server did not answer (" + status.toString() + "). Please try again in a moment.";
        if (op != "poll") LL_WARNS("WolfGreedy") << op << ": " << status.toString() << " " << error << LL_ENDL;
    }
    if (!instanceExists() || LLApp::isExiting()) return;   // (the session is a singleton: it outlives every request but the last)
    instance().onReply(op, reply, error, uncertain);
}

void WolfGreedyGame::onReply(const std::string& op, const LLSD& reply, const std::string& error, bool uncertain)
{
    mBusy = false;
    mInFlight.clear();
    changed();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!error.empty())
    {
        if (op != "poll") setMessage(error, true);
        if (uncertain)
        {
            // [W104 FEATFIX 2026-11-11 A3] what the server has now, at once: the game if there is one (a full answer, not
            // "same"), else whether a new game was made; a press queued behind the lost one was made on the old picture
            mQueued.clear();
            if (mGame)
            {
                mVersion = -1;
                mPollNow = true;
            }
            else
            {
                mQueued = "mine\n";
            }
        }
    }
    else if (op == "list")
    {
        mGames = reply["games"];
    }
    else if (reply["left"].asBoolean())
    {
        mGame = 0;
        mVersion = -1;
        mState = LLSD();
        setMessage("You have left the game.", false);
    }
    else if (reply["gone"].asBoolean())
    {
        mGame = 0;
        mVersion = -1;
        mState = LLSD();
        setMessage("That game has ended.", false);
        send("list");
        return;
    }
    else if (reply["none"].asBoolean())
    {
        mState = LLSD();
    }
    else if (!reply["same"].asBoolean() && reply.has("game"))
    {
        const LLSD& g = reply["game"];
        const S32 v = g["version"].asInteger();
        const bool first = mVersion < 0 || mGame != g["id"].asInteger();
        mGame = g["id"].asInteger();
        if (!first && v != mVersion) playSound(g["event_kind"].asString());
        if (first || v != mVersion) noteActivity();   // [A6/] something happened: the quick pace again
        // [W104 FEATFIX 2026-11-11] the moment: a roll tumbles the dice, then "Bust!" / "You banked 350!" / "Lucky six!"
        std::string banner;
        LLColor4 banner_color = YELLOW;
        bool rolled = false;
        if (!first && v != mVersion)
        {
            const std::string kind = g["event_kind"].asString();
            const LLSD& before = mState["game"];
            rolled = kind == "roll" || kind == "bust" || kind == "lucky";
            if (kind == "bust") { banner = "Bust!"; banner_color = RED_TEXT; }
            else if (kind == "lucky") banner = "Lucky six!";
            else if (kind == "win") banner = "We have a winner!";
            else if (kind == "bank")
            {
                const S32 pts = before["turn_points"].asInteger();
                banner = before["my_turn"].asBoolean() && pts > 0 ? "You banked " + number(pts) + "!" : std::string("Banked!");
                banner_color = GREEN;
            }
        }
        mVersion = v;
        mState = reply;
        if (op != "poll") { mMessage.clear(); mMessageError = false; }   // a press that went through: its old words go
        mBanner.clear();
        mBannerStart = -1.0;
        if (rolled && !reduce_motion())
        {
            mRollStart = now;
            mPendingBanner = banner;
            mPendingBannerColor = banner_color;
        }
        else if (!banner.empty())
        {
            mRollStart = -1.0;
            mBanner = banner;
            mBannerColor = banner_color;
            mBannerStart = now;
        }
    }
    // [W104 FEATFIX 2026-11-11 A6] the pace slows while nothing happens (see POLL_SLOWING)
    const std::string st = mState["game"]["state"].asString();
    const bool going = st == "playing" || st == "final";
    const F64 quiet = now - mLastActivity;
    const F64 base = going ? POLL_PLAYING_S : POLL_WAITING_S;
    const F64 cap = going ? (quiet > POLL_LONG_QUIET_S ? POLL_PLAYING_QUIET_MAX_S : POLL_PLAYING_MAX_S)
                          : (quiet > POLL_LONG_QUIET_S ? POLL_QUIET_MAX_S : POLL_LOBBY_MAX_S);
    mNextPoll = now + llclamp(quiet * POLL_SLOWING, base, cap);
    if (!mQueued.empty())
    {
        const size_t nl = mQueued.find('\n');
        const std::string q = mQueued.substr(0, nl), extra = nl == std::string::npos ? std::string() : mQueued.substr(nl + 1);
        mQueued.clear();
        send(q, extra);
    }
}

S32 WolfGreedyGame::rollFrame(S32 die) const
{
    if (mRollStart < 0.0) return 0;
    const F64 t = LLFrameTimer::getElapsedSeconds() - mRollStart;
    const S32 frame = (S32)(llclamp(t / ROLL_SECS, 0.0, 0.999) * (F64)ROLL_FRAMES);
    return (frame + die * 5) % ROLL_FRAMES;   // each die a few frames apart, so they do not turn together
}

void WolfGreedyGame::landNow()
{
    if (mRollStart < 0.0) return;
    mRollStart = -1.0;
    if (!mPendingBanner.empty())
    {
        mBanner = mPendingBanner;
        mBannerColor = mPendingBannerColor;
        mBannerStart = LLFrameTimer::getElapsedSeconds();
    }
    mPendingBanner.clear();
    changed();
}

F32 WolfGreedyGame::bannerAge() const
{
    return mBannerStart < 0.0 ? 0.f : (F32)(LLFrameTimer::getElapsedSeconds() - mBannerStart);
}

void WolfGreedyGame::moments()
{
    if (mRollStart >= 0.0 && LLFrameTimer::getElapsedSeconds() - mRollStart >= ROLL_SECS) landNow();
    if (mBannerStart >= 0.0 && LLFrameTimer::getElapsedSeconds() - mBannerStart >= BANNER_SECS)
    {
        mBanner.clear();
        mBannerStart = -1.0;
        changed();
    }
}

// ------------------------------------------------------------------------------------------------------------- window

WolfFloaterGreedy::WolfFloaterGreedy(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterGreedy::~WolfFloaterGreedy() = default;

bool WolfFloaterGreedy::postBuild()
{
    for (const char* p : { "f_", "t_" })
    {
        const std::string pre(p);
        getChild<LLButton>(pre + "mode_full")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setMode("full", true); });
        getChild<LLButton>(pre + "mode_table")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setMode("table", true); });
        // [W104 FEATFIX 2026-11-11] the old Strip view is the Greedy HUD now (Paul: "a HUD not a floater")
        getChild<LLButton>(pre + "mode_strip")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            WolfGreedyGame::openHud();
            closeFloater();
        });
        getChild<LLButton>(pre + "roll")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRoll(); });
        getChild<LLButton>(pre + "bank")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfGreedyGame::instance().bank(); });
        getChild<LLButton>(pre + "leave")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfGreedyGame::instance().leave(); });
        getChild<LLButton>(pre + "help")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setMode("full", false); showGuide(0); });
    }
    getChild<LLButton>("f_pass")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfGreedyGame::instance().pass(); });
    getChild<LLButton>("f_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNew(false); });
    getChild<LLButton>("f_practice")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNew(true); });
    getChild<LLButton>("f_join")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onJoin(); });
    getChild<LLButton>("f_refresh")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfGreedyGame::instance().refreshList(); });
    getChild<LLButton>("f_guide_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showGuide(llmax(0, mGuidePage - 1)); });
    // [W104 FEATFIX 2026-11-11] on the last card Next reads "Start playing" and closes the guide (showGuide)
    getChild<LLButton>("f_guide_next")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        showGuide(mGuidePage >= GUIDE_PAGES - 1 ? -1 : mGuidePage + 1);
    });
    getChild<LLButton>("f_guide_done")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showGuide(-1); });
    getChild<LLButton>("f_guide_practice")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showGuide(-1); onNew(true); });
    getChild<LLButton>("f_offer_yes")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        getChild<LLUICtrl>("f_offer")->setVisible(false);
        setMode("table", true);
    });
    getChild<LLButton>("f_offer_no")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        mOfferDismissed = true;
        getChild<LLUICtrl>("f_offer")->setVisible(false);
        // back to the view they had chosen (the offer is shown in Full so it can be read)
        if (!mModeBeforeOffer.empty() && mModeBeforeOffer != mMode) setMode(mModeBeforeOffer, false);
        mModeBeforeOffer.clear();
    });
    LLComboBox* target = getChild<LLComboBox>("f_target");
    target->add("10,000", LLSD(10000));
    target->add("5,000", LLSD(5000));
    target->add("2,000", LLSD(2000));
    target->selectFirstItem();
    std::string mode = gSavedSettings.getString("WolfGreedyMode");
    if (mode != "table") mode = "full";   // ("strip", the old bar, is the HUD now)
    setMode(mode, false);
    showNoGame();
    return true;
}

void WolfFloaterGreedy::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    WolfGreedyGame::instance().viewOpened(WolfGreedyGame::VIEW_WINDOW);
    mSeen = 0;   // drawn afresh
    if (key.isMap() && key.has("table"))
    {
        onTable(key["table"].asInteger(), key["label"].asString());
    }
}

void WolfFloaterGreedy::onClose(bool app_quitting)
{
    if (!app_quitting && WolfGreedyGame::instanceExists()) WolfGreedyGame::instance().viewClosed(WolfGreedyGame::VIEW_WINDOW);
    LLFloater::onClose(app_quitting);
}

// static
void WolfFloaterGreedy::showGuideWindow()
{
    if (WolfFloaterGreedy* f = LLFloaterReg::showTypedInstance<WolfFloaterGreedy>("wolf_greedy"))
    {
        f->setMode("full", false);
        f->showGuide(0);
    }
}

// static
void WolfFloaterGreedy::showLobbyWindow()
{
    if (WolfFloaterGreedy* f = LLFloaterReg::showTypedInstance<WolfFloaterGreedy>("wolf_greedy"))
    {
        f->setMode("full", false);
        if (f->mGuidePage >= 0) f->showGuide(-1);
        WolfGreedyGame::instance().refreshList();
    }
}

void WolfFloaterGreedy::setMode(const std::string& mode, bool remember)
{
    mMode = mode;
    if (remember) gSavedSettings.setString("WolfGreedyMode", mode);
    LLView* full = getChild<LLView>("full_panel");
    LLView* table = getChild<LLView>("table_panel");
    full->setVisible(mode == "full");
    table->setVisible(mode == "table");
    LLView* shown = mode == "table" ? table : full;
    // the panels follow left|top only (floater_wolf_greedy.xml), so reshaping the floater keeps each one under the
    // title bar and leaves its children alone; the floater takes the shown panel's size plus the title bar
    const S32 header = getRect().getHeight() - full->getRect().mTop;
    const LLRect old = getRect();
    const S32 w = shown->getRect().getWidth(), h = shown->getRect().getHeight() + header;
    reshape(w, h, false);
    setOrigin(old.mLeft, old.mTop - h);   // the top-left corner stays where it was
    if (gFloaterView) gFloaterView->adjustToFitScreen(this, false);
    for (const char* p : { "f_", "t_" })
    {
        const std::string pre(p);
        getChild<LLButton>(pre + "mode_full")->setToggleState(mode == "full");
        getChild<LLButton>(pre + "mode_table")->setToggleState(mode == "table");
        getChild<LLButton>(pre + "mode_strip")->setToggleState(false);
    }
}

void WolfFloaterGreedy::draw()
{
    // the view buttons are toggles: a click flips the one pressed after our callback ran (LLButton), so the three are
    // put right here every frame (the 14:23Z screenshots showed "Full" lit in the Table view)
    for (const char* p : { "f_", "t_" })
    {
        const std::string pre(p);
        getChild<LLButton>(pre + "mode_full")->setToggleState(mMode == "full");
        getChild<LLButton>(pre + "mode_table")->setToggleState(mMode == "table");
        getChild<LLButton>(pre + "mode_strip")->setToggleState(false);
    }
    WolfGreedyGame& game = WolfGreedyGame::instance();
    game.tick(isMinimized());
    if (game.version() != mSeen)
    {
        mSeen = game.version();
        render();
    }
    animate();   // [W104 FEATFIX 2026-11-11/] the tumble and the banner
    LLFloater::draw();
}

void WolfFloaterGreedy::render()
{
    const WolfGreedyGame& game = WolfGreedyGame::instance();
    if (game.state().has("game")) show(game.state());
    else showNoGame();
    // the games to join
    LLComboBox* games = getChild<LLComboBox>("f_games");
    const LLSD sel = games->getValue();
    games->removeall();
    const LLSD& list = game.games();
    if (!list.isArray() || list.size() == 0)
    {
        games->add("Nobody is playing just now", LLSD(0));
    }
    else
    {
        games->add("Choose a game to join...", LLSD(0));
        for (const LLSD& g : llsd::inArray(list))
        {
            std::string where = g["region"].asString();
            games->add(g["label"].asString() + (where.empty() ? std::string() : " at " + where) +
                       llformat(" - %d in", g["players"].asInteger()) +
                       (g["state"].asString() == "lobby" ? std::string(", waiting") : std::string(", playing")),
                       LLSD(g["id"].asInteger()));
        }
    }
    if (!games->selectByValue(sel)) games->selectFirstItem();
    showMessage();
}

void WolfFloaterGreedy::showMessage()
{
    // Source: wolffloaterdob.cpp setStatus - light red on the dark floater, and the words say it too; it stays until the
    // next action (an error is never taken away by a timer)
    const WolfGreedyGame& game = WolfGreedyGame::instance();
    for (const char* n : { "f_message", "t_message" })
    {
        LLTextEditor* m = getChild<LLTextEditor>(n);
        const LLUIColor c = game.messageIsError() ? LLUIColor(RED_TEXT) : LLUIColor(LLColor4::white);
        m->setReadOnlyColor(c);
        m->setColor(c);
        m->setText(game.message());
        m->setVisible(!game.message().empty());
    }
}

void WolfFloaterGreedy::onTable(S32 game, const std::string& label)
{
    if (game <= 0) return;
    mTableGame = game;
    mTableLabel = label;
    WolfGreedyGame::instance().onTable(game);
    if (mMode != "table" && !mOfferDismissed)
    {
        getChild<LLTextBox>("f_offer_text")->setText("You are at " + (label.empty() ? std::string("a Greedy table") : label) + ".");
        getChild<LLUICtrl>("f_offer")->setVisible(true);
        mModeBeforeOffer = mMode;
    }
}

void WolfFloaterGreedy::onLeftTable()
{
    mTableGame = 0;
    mOfferDismissed = false;
    getChild<LLUICtrl>("f_offer")->setVisible(false);
    if (mMode == "table") WolfGreedyGame::instance().setMessage("You have left the table. The Full view shows the whole game.", false);
}

void WolfFloaterGreedy::onRoll()
{
    WolfGreedyGame::instance().landNow();
    WolfGreedyGame::instance().roll();
}

void WolfFloaterGreedy::onNew(bool practice)
{
    WolfGreedyGame::instance().newGame(practice, getChild<LLComboBox>("f_target")->getValue().asInteger());
}

void WolfFloaterGreedy::onJoin()
{
    WolfGreedyGame::instance().join(getChild<LLComboBox>("f_games")->getValue().asInteger());
}


void WolfFloaterGreedy::showNoGame()
{
    // [W104 FEATFIX 2026-11-11] the scoreboard's empty state says what to do (f_board_empty)
    getChild<LLScrollListCtrl>("f_board")->deleteAllItems();
    mBoardEmpty = true;
    getChild<LLUICtrl>("f_tips")->setVisible(false);
    for (const char* n : { "f_status", "t_status" }) getChild<LLTextBox>(n)->setText(std::string("Greedy"));
    getChild<LLTextBox>("f_turn_label")->setText(std::string());
    getChild<LLTextBox>("f_turn")->setText(std::string());
    getChild<LLTextBox>("f_dice_left")->setText(std::string());
    getChild<LLTextBox>("f_event")->setText(std::string());
    getChild<LLTextBox>("t_score")->setText(std::string("Not in a game"));
    if (mGuidePage < 0 && !WolfGreedyGame::instance().rolling()) applyDice(LLSD(), "000000");
    refreshButtons();
}

void WolfFloaterGreedy::show(const LLSD& state)
{
    const LLSD& g = state["game"];
    const std::string status = g["status"].asString();
    for (const char* n : { "f_status", "t_status" })
    {
        LLTextBox* t = getChild<LLTextBox>(n);
        t->setText(status);
        t->setColor(g["my_turn"].asBoolean() ? YELLOW : LLColor4::white);
    }
    // [W104 FEATFIX 2026-11-11] the scoreboard: one row a player - whose turn (an arrow, and the row in yellow), you (green),
    // the winner (a star), busts as stars, the score right-aligned in big figures; words as well as colour
    LLScrollListCtrl* board = getChild<LLScrollListCtrl>("f_board");
    board->deleteAllItems();
    mBoardEmpty = false;
    S32 myScore = 0, top = 0;
    const LLFontGL* big = LLFontGL::getFontSansSerifBig();   // SansSerif "Large" (llfontgl.cpp), as the XUI font="SansSerifLarge"
    for (const LLSD& p : llsd::inArray(state["players"]))
    {
        const bool away = p["away"].asBoolean(), current = p["current"].asBoolean(), me = p["me"].asBoolean(), winner = p["winner"].asBoolean();
        const LLColor4 c = away ? GREY : current ? YELLOW : (me || winner) ? BOARD_ME : LLColor4::white;
        std::string stars;
        for (S32 i = 0; i < p["busts"].asInteger() && i < 3; ++i) stars += "\xE2\x9C\xB1 ";   // U+2731
        LLScrollListItem::Params row;
        auto cell = [&](const char* col, const std::string& text, LLFontGL::HAlign align)
        {
            LLScrollListCell::Params c_p;
            c_p.column(col).value(text).font(big).font_color(c).font_halign(align);
            row.columns.add(c_p);
        };
        cell("mark", winner ? "\xE2\x98\x85" : current ? "\xE2\x96\xB6" : "", LLFontGL::HCENTER);   // U+2605 star, U+25B6 arrow
        cell("name", p["name"].asString() + (me ? "  (you)" : "") + (away ? "  (away)" : "") + (winner ? "  WINNER" : ""), LLFontGL::LEFT);
        cell("busts", stars, LLFontGL::LEFT);
        cell("score", number(p["score"].asInteger()), LLFontGL::RIGHT);
        board->addRow(row);
        if (me) myScore = p["score"].asInteger();
        top = llmax(top, p["score"].asInteger());
    }
    LLTextEditor* tips = getChild<LLTextEditor>("f_tips");
    tips->clear();
    if (state.has("tips"))
    {
        LLStyle::Params tip;
        tip.color = LLColor4(0.45f, 0.9f, 1.f, 1.f);
        bool first = true;
        for (const LLSD& t : llsd::inArray(state["tips"])) { tips->appendText(t.asString(), !first, tip); first = false; }
    }
    mHasTips = state.has("tips") && state["tips"].size() > 0;
    const std::string st = g["state"].asString();
    const bool going = st == "playing" || st == "final";
    // [W104 FEATFIX 2026-11-11] this turn's points, big and gold
    getChild<LLTextBox>("f_turn_label")->setText(going ? std::string("THIS TURN") : std::string("FIRST TO"));
    getChild<LLTextBox>("f_turn")->setText(going ? number(g["turn_points"].asInteger()) : number(g["target"].asInteger()));
    getChild<LLTextBox>("f_dice_left")->setText(going ? llformat("Dice to roll: %d", g["dice_left"].asInteger()) : std::string());
    LLTextBox* ev = getChild<LLTextBox>("f_event");
    ev->setText(g["event"].asString());
    ev->setColor(g["event_kind"].asString() == "bust" ? RED_TEXT : LLColor4::white);
    const std::string score = "Your score: " + number(myScore) + (top > myScore ? "   Top: " + number(top) : std::string());
    getChild<LLTextBox>("t_score")->setText(score);
    if (mGuidePage < 0 && !WolfGreedyGame::instance().rolling()) applyDice(g["dice"], g["kept"].asString());
    refreshButtons();
}

// The 3D dice faces (greedy-art/render_dice.py): a die that scored is gold, a bust red, the others ivory.
void WolfFloaterGreedy::applyDice(const LLSD& dice, const std::string& kept)
{
    for (S32 i = 0; i < 6; ++i)
    {
        const S32 face = dice.isArray() && i < (S32)dice.size() ? llclamp(dice[i].asInteger(), 0, 6) : 0;
        const char k = i < (S32)kept.size() ? kept[i] : '0';
        const LLColor4 c = k == '1' ? GOLD_DIE : k == 'x' ? RED_DIE : LLColor4::white;
        for (const char* p : { "f_die" })
        {
            LLIconCtrl* icon = getChild<LLIconCtrl>(llformat("%s%d", p, i));
            icon->setValue(LLSD(llformat("WolfGreedy_Die%d", face)));
            icon->setColor(c);
        }
    }
}

// [W104 FEATFIX 2026-11-11] The session's moment on the window: the dice tumble (the session says which frame), then its banner
// pulses briefly and goes (wolffloatergreedy.h WolfGreedyGame).
void WolfFloaterGreedy::animate()
{
    const WolfGreedyGame& game = WolfGreedyGame::instance();
    if (mGuidePage >= 0) return;
    if (game.rolling())
    {
        mWasRolling = true;
        const LLSD& dice = game.state()["game"]["dice"];
        for (S32 i = 0; i < 6; ++i)
        {
            if (!(dice.isArray() && i < (S32)dice.size() && dice[i].asInteger() > 0)) continue;
            LLIconCtrl* icon = getChild<LLIconCtrl>(llformat("f_die%d", i));
            icon->setValue(LLSD(llformat("WolfGreedy_Roll%d", game.rollFrame(i))));
            icon->setColor(LLColor4::white);
        }
    }
    else if (mWasRolling)
    {
        mWasRolling = false;
        const LLSD& g = game.state()["game"];
        applyDice(g["dice"], g["kept"].asString());   // landed: the server's dice
    }
    LLTextBox* b = getChild<LLTextBox>("f_banner");
    if (game.banner().empty() || game.rolling())
    {
        b->setVisible(false);
        return;
    }
    if (b->getText() != game.banner()) b->setText(game.banner());
    b->setVisible(true);
    LLColor4 c = game.bannerColor();
    const F32 t = game.bannerAge();
    if (!reduce_motion() && t < (F32)BANNER_PULSE_SECS)
    {
        // a brief flash: bright and dim three times in the first BANNER_PULSE_SECS, then steady
        const F32 pulse = (F32)(0.5 + 0.5 * cos((F64)t / BANNER_PULSE_SECS * 3.0 * F_TWO_PI));
        c = lerp(LLColor4::white, game.bannerColor(), 1.f - pulse);
    }
    b->setColor(c);
}

bool WolfFloaterGreedy::handleMouseDown(S32 x, S32 y, MASK mask)
{
    WolfGreedyGame::instance().landNow();   // [W104 FEATFIX 2026-11-11/] a click lands the dice at once
    return LLFloater::handleMouseDown(x, y, mask);
}

void WolfFloaterGreedy::refreshButtons()
{
    const WolfGreedyGame& game = WolfGreedyGame::instance();
    const LLSD& g = game.state()["game"];
    const std::string b = g["buttons"].asString();
    const bool inGame = game.inGame();
    const std::string rollLabel = b.find('S') != std::string::npos ? "Start" : b.find('A') != std::string::npos ? "Play again" : "Roll";
    for (const char* p : { "f_", "t_" })
    {
        const std::string pre(p);
        LLButton* roll = getChild<LLButton>(pre + "roll");
        roll->setLabel(rollLabel);
        // [W104 FEATFIX review 2026-11-11] off while a press that changes the game is out (changeInFlight)
        const bool free = !game.changeInFlight();
        roll->setEnabled(free && inGame && (b.find('R') != std::string::npos || b.find('S') != std::string::npos || b.find('A') != std::string::npos));
        getChild<LLButton>(pre + "bank")->setEnabled(free && inGame && b.find('B') != std::string::npos);
        getChild<LLButton>(pre + "leave")->setEnabled(free && inGame);
    }
    const bool guide = mGuidePage >= 0;
    for (const char* n : { "f_target_label", "f_target", "f_new", "f_practice", "f_games", "f_join", "f_refresh" })
        getChild<LLUICtrl>(n)->setVisible(!guide && !inGame);
    for (const char* n : { "f_leave", "f_pass" }) getChild<LLUICtrl>(n)->setVisible(!guide && inGame);
    for (const char* n : { "f_pass", "f_new", "f_practice", "f_join" }) getChild<LLUICtrl>(n)->setEnabled(!game.changeInFlight());
    getChild<LLUICtrl>("f_help")->setVisible(!guide);
    for (const char* n : { "f_board", "f_turn_label", "f_turn", "f_dice_left", "f_event", "f_roll", "f_bank", "f_status" })
        getChild<LLUICtrl>(n)->setVisible(!guide);
    getChild<LLUICtrl>("f_board_empty")->setVisible(!guide && mBoardEmpty);
    getChild<LLUICtrl>("f_tips")->setVisible(!guide && mHasTips);
    if (guide) getChild<LLUICtrl>("f_banner")->setVisible(false);
}

// [W104 FEATFIX 2026-11-11] Paul: "it says page 1 of 9, it looks ugly", "no fun". The guide is a card: the logo, a title,
// the words (or, on the scoring cards, a row of dice pictures for every combination), the example dice on the baize, and
// under the card Back (off on the first card), the page dots (gold = this card; the tool tip says "Page 2 of 9") and
// Next (green; "Start playing" on the last card). "I've got it, let's play" leaves the guide from any card; the last card
// offers "Practice game" there instead.
void WolfFloaterGreedy::showGuide(S32 page)
{
    WolfGreedyGame::instance().landNow();
    mGuidePage = page;
    const bool on = page >= 0;
    const bool last = page == GUIDE_PAGES - 1;
    const GuidePage* gp = on ? &GUIDE[page] : nullptr;
    S32 rows = 0;
    if (gp) while (rows < 6 && gp->rows[rows].dice) ++rows;
    for (const char* n : { "f_guide_card", "f_guide_logo", "f_guide_title", "f_guide_back", "f_guide_dots", "f_guide_next" })
        getChild<LLUICtrl>(n)->setVisible(on);
    getChild<LLUICtrl>("f_guide_done")->setVisible(on && !last);
    getChild<LLUICtrl>("f_guide_practice")->setVisible(on && last);
    getChild<LLUICtrl>("f_guide")->setVisible(on && rows == 0);
    getChild<LLUICtrl>("f_guide_note")->setVisible(on && rows > 0);
    for (S32 r = 0; r < 6; ++r)
    {
        const char* dice = r < rows ? gp->rows[r].dice : "";
        const S32 n = (S32)strlen(dice);
        for (S32 i = 0; i < 6; ++i)
        {
            LLIconCtrl* d = getChild<LLIconCtrl>(llformat("f_combo%d_d%d", r, i));
            d->setVisible(i < n);
            if (i < n)
            {
                d->setValue(LLSD(llformat("WolfGreedy_Die%c", dice[i])));
                d->setColor(GOLD_DIE);   // every die in a scoring combination scores
            }
        }
        LLTextBox* t = getChild<LLTextBox>(llformat("f_combo%d_t", r));
        t->setVisible(r < rows);
        t->setText(r < rows ? std::string(gp->rows[r].label) : std::string());
    }
    // the dice on the baize: the card's example, or (out of the guide) the game's
    const bool example = gp && strcmp(gp->dice, "000000") != 0;
    getChild<LLUICtrl>("f_felt")->setVisible(!on || example);
    for (S32 i = 0; i < 6; ++i) getChild<LLUICtrl>(llformat("f_die%d", i))->setVisible(!on || example);
    if (on)
    {
        getChild<LLTextBox>("f_guide_title")->setText(std::string(gp->title));
        getChild<LLTextEditor>(rows ? "f_guide_note" : "f_guide")->setText(std::string(gp->body));
        std::string dots;
        for (S32 i = 0; i < GUIDE_PAGES; ++i) dots += i == page ? "\xE2\x97\x8F " : "\xE2\x97\x8B ";   // U+25CF / U+25CB
        LLTextBox* d = getChild<LLTextBox>("f_guide_dots");
        d->setText(dots);
        d->setToolTip(llformat("Page %d of %d", page + 1, GUIDE_PAGES));
        LLButton* back = getChild<LLButton>("f_guide_back");
        back->setEnabled(page > 0);
        back->setToolTip(page > 0 ? llformat("Back to page %d of %d", page, GUIDE_PAGES) : std::string("This is the first page"));
        LLButton* next = getChild<LLButton>("f_guide_next");
        next->setLabel(last ? std::string("Start playing") : std::string("Next"));
        next->setToolTip(last ? std::string("Close the guide and play") : llformat("Page %d of %d", page + 2, GUIDE_PAGES));
        LLSD dice = LLSD::emptyArray();
        for (S32 i = 0; i < 6; ++i) dice.append(gp->dice[i] - '0');
        applyDice(dice, gp->kept);
    }
    else
    {
        const LLSD& g = WolfGreedyGame::instance().state()["game"];
        applyDice(g["dice"], g["kept"].asString());
        mSeen = 0;   // the game is drawn afresh
    }
    refreshButtons();
}

// ---------------------------------------------------------------------------------------------------------- table link

void WolfGreedy::reset(const LLUUID& seat)
{
    mSeatRoot = seat;
    mDescKnown = false;
    mIsTable = false;
    mAnswered = false;
    mHelloTries = 0;
    mHelloAt = -100.0;
}

void WolfGreedy::idle()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        if (mSeatRoot.notNull()) reset(LLUUID::null);
        return;
    }
    LLViewerObject* root = seat_root();
    const LLUUID id = root ? root->getID() : LLUUID::null;
    if (id != mSeatRoot)
    {
        if (mIsTable)
        {
            WolfFloaterGreedy* f = LLFloaterReg::findTypedInstance<WolfFloaterGreedy>("wolf_greedy");
            if (f) f->onLeftTable();
        }
        reset(id);
        if (root) WolfObjectProps::instance().want(root, false);
    }
    if (!root) return;   // not sitting: nothing to do (paul-think-of-scale)
    if (!mDescKnown)
    {
        const WolfObjectProps::Props* p = WolfObjectProps::instance().get(id);
        if (!p)
        {
            WolfObjectProps::instance().want(root, false);
            return;
        }
        mDescKnown = true;
        // Source: build_wolfdice.py TABLE_DESC "greedy-table: ..."
        mIsTable = p->mDescriptionLower.compare(0, 12, "greedy-table") == 0;
    }
    if (!mIsTable || mAnswered || mHelloTries >= HELLO_TRIES) return;
    const F64 t = LLFrameTimer::getElapsedSeconds();
    if (t - mHelloAt < HELLO_RETRY_S || !WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    mHelloAt = t;
    ++mHelloTries;
    // said, not shouted: the table is the object we sit on (as WolfSkis::say)
    FSNearbyChat::sendChatFromViewerFinal(HELLO, HELLO, CHAT_TYPE_NORMAL, false, HELLO_CHANNEL);
}

bool WolfGreedy::onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0) return false;
    if (!WolfGrid::isOnWolfTerritories() || chat_type != CHAT_TYPE_DIRECT) return false;
    // ours from here, never chat; read only from the table we sit at (as WolfSkis::onObjectChat)
    const LLViewerObject* o = gObjectList.findObject(source);
    while (o && o->getParent()) o = (const LLViewerObject*)o->getParent();
    if (!o || o->getID() != mSeatRoot || !mIsTable || text.size() > MAX_LINE) return true;
    std::vector<std::string> f;
    std::string rest = text.substr(PREFIX.size());
    size_t at = 0;
    while (f.size() < 4)
    {
        const size_t bar = rest.find('|', at);
        f.push_back(rest.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    if (f[0] == "table" && f.size() >= 2)
    {
        mAnswered = true;
        const S32 game = (S32)std::strtol(f[1].c_str(), nullptr, 10);
        LLSD key;
        key["table"] = game;
        key["label"] = f.size() >= 3 ? f[2] : std::string();
        WolfFloaterGreedy* fl = LLFloaterReg::findTypedInstance<WolfFloaterGreedy>("wolf_greedy");
        if (fl && fl->getVisible()) fl->onTable(game, key["label"].asString());
        else LLFloaterReg::showInstance("wolf_greedy", key);
    }
    else if (f[0] == "bye")
    {
        WolfFloaterGreedy* fl = LLFloaterReg::findTypedInstance<WolfFloaterGreedy>("wolf_greedy");
        if (fl) fl->onLeftTable();
    }
    return true;
}
