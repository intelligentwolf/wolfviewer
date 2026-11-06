/**
 * @file wolffloaterskis.cpp
 * [SKIS 2026-11-06] See wolffloaterskis.h.
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

#include "wolffloaterskis.h"

#include "llagentdata.h"
#include "llbutton.h"
#include "llcoros.h"
#include "llnotificationsutil.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "wolfcave.h"
#include "wolfskis.h"
#include "wolfvehiclerez.h"

namespace
{
    // [Wolf 2026-11-06] wolfstorm/php/wolfskis.php: POST {"action": "give"}. The same host and transport as the Wolf
    // Animator (wolffloateranimator.cpp:57 SERVICE_URL, :818 WolfCaves::serviceRequest: X-Wolf-Agent / X-Wolf-Session,
    // the host verified, no automatic retry of a POST - wolfcave.cpp:141-169 wolfRequest).
    const char* GIVE_URL = "https://wolfstorm.app/php/wolfskis.php";
    const char* GIVE_BODY = "{\"action\":\"give\"}";
    // A resident who has them is answered in one indexed SELECT; a give is a few Robust calls (wolfskis_lib.php wsk_give).
    constexpr F32 GIVE_TIMEOUT_S = 30.f;
    // wolfstorm/php/wolfskis.php: HTTP 429 = wa_bucket (5 at once, then one a minute per resident)
    constexpr S32 HTTP_TOO_MANY = 429;

    // What a failed give says (wolffloateranimator.cpp:78-96 serviceError: the service's own words first; the raw adapter
    // drops a 4xx body, wolfcave.cpp:185-188, so each status gets a line of its own).
    std::string giveError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 401:
            case 403: return "The grid could not confirm you are signed in. Log out and back in, then open the Skis and Snowboards window again.";
            case 404: return "The free skis service is not open on this grid yet.";
            case 500:
            case 503: return "The free skis service is not answering just now. Open the Skis and Snowboards window again in a minute.";
            default:
                return transport.empty() ? llformat("The free skis service answered HTTP %d. Open the Skis and Snowboards window again in a minute.", status)
                                         : "Could not reach the free skis service (" + transport + "). Open the Skis and Snowboards window again in a minute.";
        }
    }
}

// static
LLUUID WolfFloaterSkis::sAskedSession;

WolfFloaterSkis::WolfFloaterSkis(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterSkis::postBuild()
{
    mSkisBtn = getChild<LLButton>("skis_btn");
    mBoardBtn = getChild<LLButton>("board_btn");
    mPutOnBtn = getChild<LLButton>("put_on_btn");
    mTakeOffBtn = getChild<LLButton>("take_off_btn");
    mShowHudBtn = getChild<LLButton>("show_hud_btn");
    mStatus = getChild<LLTextBox>("status");
    mSkisBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { pick(false); });
    mBoardBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { pick(true); });
    mPutOnBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPutOn(); });
    mTakeOffBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTakeOff(); });
    mShowHudBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShowHud(); });
    return true;
}

void WolfFloaterSkis::onOpen(const LLSD& key)
{
    // [WOLF ONLY] the menu item is gone off Wolf (menu_viewer.xml); a window opened some other way says why and goes
    if (!WolfSkis::onWolf())
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
            std::string("Skis and snowboards are for Wolf Territories regions: they need the Wolf grid's ski runs and scripts.")));
        closeFloater();
        return;
    }
    pick(gSavedSettings.getString("WolfSkiKind") == "snowboard");
    say("");
    askForSkis();
}

void WolfFloaterSkis::askForSkis()
{
    // [Wolf 2026-11-06] Once per session (a give answered for this session is not asked again; a failed one is asked at the
    // next open), never polled. The server reads the inventory afresh each time (wolfskis_lib.php wsk_give), so nothing
    // here is remembered about what the resident has.
    if (mAsking || gAgentSessionID.isNull() || sAskedSession == gAgentSessionID) return;
    mAsking = true;
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfSkisGive", [h]() { giveCoro(h); });
}

// static
void WolfFloaterSkis::giveCoro(LLHandle<LLFloater> h)
{
    // Source: wolffloateranimator.cpp:811-828 requestCoro. The floater is looked up again through its handle when the
    // reply comes: closed and destroyed in the meantime, the reply is dropped and the next open asks again.
    const LLUUID session = gAgentSessionID;
    const std::string text(GIVE_BODY);
    LLSD result = WolfCaves::serviceRequest(GIVE_URL, &text, GIVE_TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    if (WolfFloaterSkis* f = dynamic_cast<WolfFloaterSkis*>(h.get()))
    {
        if (session == gAgentSessionID)
        {
            f->onGiveReply(status, ok, reply, transport);
        }
        else
        {
            f->mAsking = false;   // a different session now: its own open asks
        }
    }
}

void WolfFloaterSkis::onGiveReply(S32 status, bool ok, const LLSD& reply, const std::string& transport)
{
    mAsking = false;
    const std::string code = reply.isMap() ? reply["code"].asString() : std::string();
    if (ok)
    {
        sAskedSession = gAgentSessionID;
        // they had both: nothing is said (Paul: simple; the region's offer says the rest when something was given)
        const LLSD& given = reply["given"];
        if (!given.isArray() || given.size() == 0) return;
        std::string names;
        for (LLSD::array_const_iterator it = given.beginArray(); it != given.endArray(); ++it)
        {
            if (!names.empty()) names += " and ";
            names += (*it)["name"].asString();
        }
        LLStringUtil::format_map_t a;
        a["[NAMES]"] = names;
        a["[FOLDER]"] = reply["folder"].asString();
        say(getString(given.size() == 1 ? "given_one" : "given_both", a));
        return;
    }
    if (status == HTTP_TOO_MANY)
    {
        return;   // asked too often (several opens in a minute): the next open asks again, nothing to tell
    }
    const std::string error = giveError(status, reply, transport);
    if (code == "not_member")
    {
        sAskedSession = gAgentSessionID;   // a visitor: asking again this session gives the same answer
        say(error);
        return;
    }
    // a failure a resident must be able to read and act on: an alert stays until OK is pressed (~/.claude/CLAUDE.md
    // "Error toasts do not auto-dismiss"; the same GenericAlertOK this window uses off the Wolf grid)
    LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", error));
}

void WolfFloaterSkis::pick(bool snowboard)
{
    mSnowboard = snowboard;
    mSkisBtn->setToggleState(!snowboard);
    mBoardBtn->setToggleState(snowboard);
    gSavedSettings.setString("WolfSkiKind", snowboard ? "snowboard" : "skis");
    mPutOnBtn->setLabel(getString(snowboard ? "put_on_board" : "put_on_skis"));
}

void WolfFloaterSkis::onPutOn()
{
    WolfSkis& sk = WolfSkis::instance();
    const WolfSkis::EKind want = mSnowboard ? WolfSkis::KIND_SNOWBOARD : WolfSkis::KIND_SKIS;
    if (mAsking)
    {
        say(getString("asking"));   // [Wolf 2026-11-06] the free pair may be on its way into the inventory: wait for it
        return;
    }
    if (sk.kind() == want)
    {
        say(getString(mSnowboard ? "already_board" : "already_skis"));
        return;
    }
    if (WolfVehicleRez::pending())
    {
        say(WolfVehicleRez::tip(WolfSkis::kindWord(want)));   // one at a time: the one under way says where it is
        return;
    }
    sk.putOn(want);
    mWasPending = true;
    say(WolfVehicleRez::tip(WolfSkis::kindWord(want)));
}

void WolfFloaterSkis::onTakeOff()
{
    WolfSkis& sk = WolfSkis::instance();
    if (!sk.riding())
    {
        say(getString("not_riding"));
        return;
    }
    sk.takeOff();
    say(getString("taken_off"));
}

void WolfFloaterSkis::onShowHud()
{
    WolfSkis::instance().showHud(true);
}

void WolfFloaterSkis::draw()
{
    WolfSkis& sk = WolfSkis::instance();
    // live: what the put-on is doing (WolfVehicleRez says every failure itself, in a visible alert)
    if (WolfVehicleRez::pending())
    {
        say(WolfVehicleRez::tip(WolfSkis::kindWord(mSnowboard ? WolfSkis::KIND_SNOWBOARD : WolfSkis::KIND_SKIS)));
    }
    else if (mWasPending)
    {
        mWasPending = false;
        say(sk.riding() ? getString(sk.kind() == WolfSkis::KIND_SNOWBOARD ? "on_board" : "on_skis") : "");
    }
    mTakeOffBtn->setEnabled(sk.riding());
    mShowHudBtn->setVisible(sk.riding() && !sk.hudShowing());
    LLFloater::draw();
}

void WolfFloaterSkis::say(const std::string& text)
{
    if (mStatus && mStatus->getText() != text) mStatus->setText(text);
}
