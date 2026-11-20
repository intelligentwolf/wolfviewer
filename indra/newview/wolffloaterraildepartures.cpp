/**
 * @file wolffloaterraildepartures.cpp
 * @brief WolfViewer: Railway Departures. Who and why: a resident at a station (35-65, maybe with weaker eyesight) wants to know when
 *        the next train to where they are going leaves - one look, no codes (DESIGN.md 3.3a). Flow: open it from Wolf World >
 *        Railways > Railway Departures...; it opens on the station the avatar stands in (the railways with a station in this
 *        region, rail_board.php ?region=); another railway is picked from that list or the ones looked at before, and a name is
 *        typed only as the last resort (the coordinator, 20 Nov: older and disabled users should not have to type); read; close. Data: wolfstorm's rail_board.php (the railway's public board, or a private one for its
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
#include "llagent.h"
#include "llviewerregion.h"
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

// the railways looked at before, newest first (WolfRailDeparturesRecent: names, comma separated; at most RECENT)
std::vector<std::string> WolfFloaterRailDepartures::recent()
{
    std::vector<std::string> out;
    std::string s = gSavedSettings.getString("WolfRailDeparturesRecent");
    size_t a = 0;
    while (a <= s.size() && out.size() < 8)
    {
        size_t b = s.find(',', a);
        std::string n = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (networkOk(n) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

void WolfFloaterRailDepartures::choose(const std::string& network)
{
    if (!networkOk(network)) { setStatus(getString("bad_railway"), true); return; }
    gSavedSettings.setString("WolfRailDeparturesNetwork", network);
    std::vector<std::string> r = recent();
    r.erase(std::remove(r.begin(), r.end(), network), r.end());
    r.insert(r.begin(), network);
    if (r.size() > 8) r.resize(8);
    std::string joined;
    for (const std::string& n : r) joined += (joined.empty() ? "" : ",") + n;
    gSavedSettings.setString("WolfRailDeparturesRecent", joined);
    mTag.clear(); mStationsKey.clear();
    read();
}

// the list: the railways with a station here first (their titles), then the ones looked at before
void WolfFloaterRailDepartures::fillRailways()
{
    const std::string cur = gSavedSettings.getString("WolfRailDeparturesNetwork");
    mNetwork->removeall();
    std::vector<std::string> seen;
    for (const LLSD& n : llsd::inArray(mHere))
    {
        const std::string name = n["network"].asString();
        if (!networkOk(name)) continue;
        mNetwork->add(n["title"].asString() + " (" + name + ")", LLSD(name));
        seen.push_back(name);
    }
    for (const std::string& n : recent()) if (std::find(seen.begin(), seen.end(), n) == seen.end()) mNetwork->add(n, LLSD(n));
    if (!cur.empty()) mNetwork->setValue(LLSD(cur));
}

void WolfFloaterRailDepartures::findHere()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    mHereRegion = rgn->getRegionID();
    setStatus(getString("finding"), false);
    mBusy = true;
    LLHandle<LLFloater> h = getHandle();
    const std::string url = std::string(BOARD_URL) + "?region=" + mHereRegion.asString();
    LLCoros::instance().launch("WolfRailDeparturesHere", [h, url]() { hereCoro(h, url); });
}

// static
void WolfFloaterRailDepartures::hereCoro(LLHandle<LLFloater> h, std::string url)
{
    LLSD result = WolfCaves::serviceRequest(url, nullptr, TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    if (WolfFloaterRailDepartures* f = dynamic_cast<WolfFloaterRailDepartures*>(h.get())) f->onHere(status, reply);
}

void WolfFloaterRailDepartures::onHere(S32 status, const LLSD& reply)
{
    mBusy = false;
    mHere = status == 200 && reply.isMap() ? reply["railways"] : LLSD::emptyArray();
    // standing at a station: that railway and that station (the first of them when there are several)
    if (mHere.isArray() && mHere.size() > 0 && networkOk(mHere[0]["network"].asString()))
    {
        const std::string net = mHere[0]["network"].asString();
        if (mHere[0]["stations"].isArray() && mHere[0]["stations"].size() > 0) gSavedSettings.setString("WolfRailDeparturesStation", mHere[0]["stations"][0].asString());
        fillRailways();
        choose(net);
        return;
    }
    fillRailways();
    if (status != 200) { setStatus(getString("unavailable"), true); return; }
    read();   // nothing here: the last railway looked at, if any
}

bool WolfFloaterRailDepartures::postBuild()
{
    mNetwork = getChild<LLComboBox>("railway");
    mStation = getChild<LLComboBox>("station");
    mDepartures = getChild<LLScrollListCtrl>("departures");
    mArrivals = getChild<LLScrollListCtrl>("arrivals");
    mTitle = getChild<LLTextBox>("title_text");
    mStatus = getChild<LLTextBox>("status");
    // picked from the list, or (the last resort) typed and Enter
    mNetwork->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        std::string n = mNetwork->getValue().asString();
        if (n.empty()) n = mNetwork->getSimple();
        choose(n);
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
    fillRailways();
    findHere();   // opens on the station the avatar stands in
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
    // a new region: the railways here again; else the board, never closer than READ_S
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mBusy && rgn && rgn->getRegionID() != mHereRegion) findHere();
    else if (!mBusy && mSince.getElapsedTimeF32() >= READ_S) read();
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
    const std::string net = gSavedSettings.getString("WolfRailDeparturesNetwork");
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
