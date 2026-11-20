/**
 * @file wolffloaterraildepartures.cpp
 * @brief WolfViewer: Railway Departures. Who and why: a resident at a station (35-65, maybe with weaker eyesight) wants to know when
 *        the next train to where they are going leaves - one look, no codes (DESIGN.md 3.3a). Flow: open it from Wolf World >
 *        Railways > Railway Departures...; it shows the last railway and station looked at; type another railway's name or pick
 *        another station; read; close. Data: wolfstorm's rail_board.php (the railway's public board, or a private one for its
 *        owner), read only while the window is open, at most every READ_S. Wolf Territories only.
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
// Source: region-terrain-work/wolf-rail-brain-2026-11-20/php/rail_board.php (GET ?network=&station=; the answer's fields: network,
// title, stale, stations, rows [{kind, station, platform, time, train, to|from, calls, coaches, status}]; errors {"error"} with 400,
// 404 "No departures for this station", 429); wolffloatertrainbuilder.cpp requestCoro (WolfCaves::serviceRequest / serviceReply,
// the coroutine and handle pattern); wolffloaterweatherprops.cpp draw (closes itself off the Wolf grid; problems in the caution colour).

#include "llviewerprecompiledheaders.h"

#include "wolffloaterraildepartures.h"

#include "llcombobox.h"
#include "llcoros.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llsdutil.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "lluri.h"
#include "wolfcave.h"
#include "wolfgrid.h"

namespace
{
    // Source: the staged rail_board.php; deployed beside train_builder.php on wolfstorm.app (the Train Builder's host, YARD_URL)
    const char* BOARD_URL = "https://wolfstorm.app/php/rail_board.php";

    /// rail_board.php's own boundary rule for a railway's name (RailText.CheckNetwork): 1-32 of A-Z a-z 0-9 - _
    bool networkOk(const std::string& s)
    {
        if (s.empty() || s.size() > 32) return false;
        for (char c : s) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
        return true;
    }
}

WolfFloaterRailDepartures::WolfFloaterRailDepartures(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterRailDepartures::postBuild()
{
    mNetwork = getChild<LLLineEditor>("railway");
    mStation = getChild<LLComboBox>("station");
    mDepartures = getChild<LLScrollListCtrl>("departures");
    mArrivals = getChild<LLScrollListCtrl>("arrivals");
    mTitle = getChild<LLTextBox>("title_text");
    mStatus = getChild<LLTextBox>("status");
    mNetwork->setText(gSavedSettings.getString("WolfRailDeparturesNetwork"));
    mNetwork->setCommitOnFocusLost(true);
    mNetwork->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("WolfRailDeparturesNetwork", mNetwork->getText());
        mTag.clear(); mStationsKey.clear();
        read();
    });
    mStation->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("WolfRailDeparturesStation", mStation->getValue().asString());
        mTag.clear();
        read();
    });
    return true;
}

void WolfFloaterRailDepartures::onOpen(const LLSD& key)
{
    mTag.clear();
    read();
}

void WolfFloaterRailDepartures::draw()
{
    // Wolf Territories only (paul-wolf-features-wolf-grid-only)
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    // only while open (draw runs only then), never closer than READ_S, one request at a time
    if (!mBusy && mSince.getElapsedTimeF32() >= READ_S) read();
    LLFloater::draw();
}

void WolfFloaterRailDepartures::setStatus(const std::string& text, bool problem)
{
    // the words as well as the colour (never colour alone); a problem stays until the next good read
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(problem ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterRailDepartures::read()
{
    mSince.reset();
    const std::string net = mNetwork->getText();
    if (net.empty()) { setStatus(getString("type_railway"), false); return; }
    if (!networkOk(net)) { setStatus(getString("bad_railway"), true); return; }
    std::string url = std::string(BOARD_URL) + "?network=" + LLURI::escapeQueryValue(net);
    const std::string station = gSavedSettings.getString("WolfRailDeparturesStation");
    if (!station.empty()) url += "&station=" + LLURI::escapeQueryValue(station);
    if (mTag.empty()) setStatus(getString("loading"), false);
    mBusy = true;
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfRailDepartures", [h, url]() { readCoro(h, url); });
}

// static
void WolfFloaterRailDepartures::readCoro(LLHandle<LLFloater> h, std::string url)
{
    // a GET (no body): WolfCaves::serviceRequest sends X-Wolf-Agent / X-Wolf-Session, so the owner of a private railway sees it
    LLSD result = WolfCaves::serviceRequest(url, nullptr, TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    if (WolfFloaterRailDepartures* f = dynamic_cast<WolfFloaterRailDepartures*>(h.get())) f->onReply(status, reply, transport);
}

void WolfFloaterRailDepartures::onReply(S32 status, const LLSD& reply, const std::string& error)
{
    mBusy = false;
    mSince.reset();
    if (status == 200 && reply.isMap() && reply.has("rows"))
    {
        fill(reply);
        return;
    }
    if (status == 404)
    {
        // a station the railway does not have, a railway that does not exist or a private one: told alike, on purpose
        mDepartures->deleteAllItems(); mArrivals->deleteAllItems(); mTag.clear();
        if (!gSavedSettings.getString("WolfRailDeparturesStation").empty()) gSavedSettings.setString("WolfRailDeparturesStation", "");
        setStatus(getString("none"), true);
        return;
    }
    setStatus(getString("unavailable"), true);   // the last good board stays on show under it
}

void WolfFloaterRailDepartures::fill(const LLSD& reply)
{
    mTitle->setText(reply["title"].asString());
    // the station list (the railway's own names), kept while it is the same
    std::string key;
    for (const LLSD& s : llsd::inArray(reply["stations"])) key += s.asString() + "\n";
    if (key != mStationsKey)
    {
        mStationsKey = key;
        mStation->removeall();
        mStation->add(getString("all_stations"), LLSD(""));
        for (const LLSD& s : llsd::inArray(reply["stations"])) mStation->add(s.asString(), s);
        mStation->setValue(LLSD(gSavedSettings.getString("WolfRailDeparturesStation")));
    }
    const std::string tag = reply["tag"].asString() + "|" + gSavedSettings.getString("WolfRailDeparturesStation");
    if (tag != mTag)
    {
        mTag = tag;
        mDepartures->deleteAllItems();
        mArrivals->deleteAllItems();
        for (const LLSD& r : llsd::inArray(reply["rows"]))
        {
            const bool dep = r["kind"].asString() != "arrival";
            LLSD row;
            row["columns"][0]["column"] = "time";     row["columns"][0]["value"] = r["time"].asString();
            row["columns"][1]["column"] = "place";    row["columns"][1]["value"] = dep ? r["to"].asString() : r["from"].asString();
            row["columns"][2]["column"] = "platform"; row["columns"][2]["value"] = r["platform"].asString();
            row["columns"][3]["column"] = "status";   row["columns"][3]["value"] = r["status"].asString();
            if (dep)
            {
                std::string calls;
                for (const LLSD& c : llsd::inArray(r["calls"])) calls += (calls.empty() ? "" : ", ") + c.asString();
                row["columns"][4]["column"] = "calls"; row["columns"][4]["value"] = calls;
                mDepartures->addElement(row);
            }
            else mArrivals->addElement(row);
        }
    }
    if (reply["stale"].asBoolean()) setStatus(getString("not_updating"), true);
    else if (mDepartures->getItemCount() == 0 && mArrivals->getItemCount() == 0) setStatus(getString("no_trains"), false);
    else setStatus(std::string(), false);
}
