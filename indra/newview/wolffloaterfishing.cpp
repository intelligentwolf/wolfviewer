/**
 * @file wolffloaterfishing.cpp
 * @brief WolfViewer: Fishing (ticket 679). Design: wolffloaterfishing.h.
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

#include "wolffloaterfishing.h"

#include "llbutton.h"
#include "llchat.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llagent.h"
#include "wolffishingoverlay.h"
#include "wolfgrid.h"

namespace
{
    // Source: lsl/fish_rod.lsl PREFIX "WolfFish1|" (tell())
    const std::string PREFIX = "WolfFish1|";
    constexpr size_t MAX_LINE = 1023;          // llRegionSayTo's longest line (LSL_Api.cs llRegionSayTo)

    bool onWolf() { return WolfGrid::isOnWolfTerritories(); }
}

bool WolfFishing::onObjectChat(const LLUUID& source, const LLUUID& owner, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0) return false;
    // [WOLF ONLY] as WolfLifts::onObjectChat: off Wolf the line is left to chat like any other
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT) return false;
    // only an object this agent owns speaks to the window: the region writes the OwnerID, so this holds even before the
    // rod's object has reached the viewer (WolfStorm 2026-11-10: a login's rod answered before its object arrived)
    if (source.isNull() || owner != gAgent.getID()) return false;
    // from here the line is Fishing's, never chat (the rod only sends these after the HUD or the window said hello)
    if (text.size() > MAX_LINE) return true;
    // [FISHING HUD 2026-11-11] to the session the HUD and the Board window share
    if (WolfFishingSession::instanceExists()) WolfFishingSession::instance().onRodLine(text);
    return true;
}

WolfFloaterFishing::WolfFloaterFishing(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterFishing::~WolfFloaterFishing() = default;

bool WolfFloaterFishing::postBuild()
{
    mStatus = getChild<LLTextBox>("fish_status");
    mCastBtn = getChild<LLButton>("fish_cast");
    mReelBtn = getChild<LLButton>("fish_reel");
    mStopBtn = getChild<LLButton>("fish_stop");
    mAwayBtn = getChild<LLButton>("fish_away");
    mUpdateBtn = getChild<LLButton>("fish_update");
    mChallenge = getChild<LLCheckBoxCtrl>("fish_challenge");
    mMine = getChild<LLTextBox>("fish_mine");
    mMyBest = getChild<LLScrollListCtrl>("fish_my_best");
    mBoard = getChild<LLScrollListCtrl>("fish_board");
    mBoardNote = getChild<LLTextBox>("fish_board_note");
    mLastWeek = getChild<LLTextBox>("fish_last_week");
    mCastBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().cast(); });
    mReelBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().reel(); });
    mStopBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().stop(); });
    mAwayBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().putAway(); });
    mUpdateBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().refreshWeek(); });
    mChallenge->setCommitCallback([this](LLUICtrl*, const LLSD&) { WolfFishingSession::instance().setChallenge(mChallenge->get()); });
    updateControls();
    return true;
}

void WolfFloaterFishing::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    WolfFishingSession& s = WolfFishingSession::instance();
    const bool was = s.active();
    if (!mOpen)
    {
        mOpen = true;
        s.viewOpened(WolfFishingSession::VIEW_BOARD);
    }
    // the board is read when the window opens (a new session reads it itself), after each catch and on Update - never polled
    if (was) s.refreshWeek();
    mShownVersion = 0;
}

void WolfFloaterFishing::onClose(bool app_quitting)
{
    if (mOpen)
    {
        mOpen = false;
        // the last view to close ends the session: the rod goes back to owner chat and its touch menu ("bye")
        if (!app_quitting && WolfFishingSession::instanceExists()) WolfFishingSession::instance().viewClosed(WolfFishingSession::VIEW_BOARD);
    }
    LLFloater::onClose(app_quitting);
}

void WolfFloaterFishing::draw()
{
    WolfFishingSession& s = WolfFishingSession::instance();
    s.tick();
    updateControls();
    if (s.version() != mShownVersion)
    {
        mShownVersion = s.version();
        refreshWords();
    }
    LLFloater::draw();
}

void WolfFloaterFishing::updateControls()
{
    if (!mCastBtn) return;
    const WolfFishingSession& s = WolfFishingSession::instance();
    const bool wolf = onWolf();
    const bool ready = wolf && s.rod() == WolfFishingSession::ROD_READY;
    const bool lineOut = s.fish() == WolfFishingSession::FISH_WAITING || s.fish() == WolfFishingSession::FISH_BITE;
    // Cast also puts the rod on again when it was taken off
    mCastBtn->setEnabled(wolf && s.rod() != WolfFishingSession::ROD_FINDING && (s.rod() == WolfFishingSession::ROD_NONE || s.fish() == WolfFishingSession::FISH_IDLE));
    mCastBtn->setLabel(s.rod() == WolfFishingSession::ROD_NONE ? std::string("Get my rod") : std::string("Cast"));
    mReelBtn->setEnabled(ready && lineOut);
    mStopBtn->setEnabled(ready && lineOut);
    mAwayBtn->setEnabled(ready && s.fish() == WolfFishingSession::FISH_IDLE);
    mUpdateBtn->setEnabled(wolf && !s.weekBusy());
    mChallenge->setEnabled(wolf);
    if (mChallenge->get() != s.challengeOn()) mChallenge->set(s.challengeOn());
}

void WolfFloaterFishing::refreshWords()
{
    const WolfFishingSession& s = WolfFishingSession::instance();
    mStatus->setText(s.status().empty() ? std::string("Looking for your fishing rod...") : s.status());
    mStatus->setColor(LLUIColorTable::instance().getColor(s.statusIsError() ? "AlertCautionTextColor" : "LabelTextColor"));
    if (!s.weekError().empty())
    {
        mBoardNote->setText(s.weekError());
        mBoardNote->setColor(LLUIColorTable::instance().getColor("AlertCautionTextColor"));
    }
    else if (s.week().isMap())
    {
        showWeek(s.week());
    }
}

void WolfFloaterFishing::showWeek(const LLSD& w)
{
    const LLSD& me = w["me"];
    if (me["fish"].asInteger() > 0)
    {
        std::string t = llformat("You have caught %d fish this week. Your score: %s", me["fish"].asInteger(), me["score"].asString().c_str());
        if (me["place"].asInteger() > 0) t += " - " + me["placeText"].asString() + " place.";
        else t += ".";
        mMine->setText(t);
    }
    else
    {
        mMine->setText(std::string("You have not caught anything this week yet."));
    }
    mMyBest->deleteAllItems();
    for (const LLSD& b : llsd::inArray(me["best"]))
    {
        LLSD row;
        row["columns"][0]["column"] = "fish";
        row["columns"][0]["value"] = b["text"].asString();
        row["columns"][1]["column"] = "where";
        row["columns"][1]["value"] = b["region"].asString();
        mMyBest->addElement(row);
    }
    mBoard->deleteAllItems();
    for (const LLSD& r : llsd::inArray(w["board"]))
    {
        LLSD row;
        row["columns"][0]["column"] = "place";
        row["columns"][0]["value"] = llformat("%d", r["place"].asInteger());
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = r["name"].asString();
        row["columns"][2]["column"] = "score";
        row["columns"][2]["value"] = r["score"].asString();
        row["columns"][3]["column"] = "fish";
        row["columns"][3]["value"] = llformat("%d", r["fish"].asInteger());
        row["columns"][4]["column"] = "best";
        row["columns"][4]["value"] = r["best"].asString();
        mBoard->addElement(row);
    }
    if (w["board"].size() == 0) mBoard->setCommentText("Nobody has caught anything yet this week. Be the first!");
    std::string note;
    if (w["biggest"].isMap()) note = "Biggest fish this week: " + w["biggest"]["text"].asString() + ", caught by " + w["biggest"]["name"].asString() + ". ";
    note += w["rule"].asString();
    mBoardNote->setText(note);
    mBoardNote->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
    const LLSD& last = w["last"];
    if (last.isMap() && last["board"].size() > 0)
    {
        const LLSD& win = last["board"][0];
        std::string t = "Last week's winner (week of " + last["week"].asString() + "): " + win["name"].asString() + ", " + win["score"].asString() + ".";
        if (last["biggest"].isMap()) t += " Biggest fish: " + last["biggest"]["text"].asString() + ", by " + last["biggest"]["name"].asString() + ".";
        mLastWeek->setText(t);
    }
    else
    {
        mLastWeek->setText(std::string());
    }
}
