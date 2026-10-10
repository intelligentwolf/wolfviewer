/**
 * @file wolffloaterwatchwolf.cpp
 * @brief WolfViewer: WatchWolf. See wolffloaterwatchwolf.h.
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

#include "wolffloaterwatchwolf.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llavataractions.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"

namespace
{
    const char* API_URL = "https://www.wolf-grid.com/wolfstorm/php/watchwolf.php";
    const F64 REFRESH_SECONDS = 20.0;

    LLCore::HttpHeaders::ptr_t identity_headers(bool form)
    {
        LLCore::HttpHeaders::ptr_t h = std::make_shared<LLCore::HttpHeaders>();
        h->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        if (form) h->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
        h->append("X-Wolf-Agent", gAgentID.asString());
        h->append("X-Wolf-Session", gAgentSessionID.asString());
        return h;
    }

    LLSD reply_of(const LLSD& result)
    {
        if (!result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW)) return LLSD();
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(raw.begin(), raw.end()), ec);
        return ec ? LLSD() : LlsdFromJson(v);
    }
}

WolfFloaterWatchWolf::WolfFloaterWatchWolf(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterWatchWolf::postBuild()
{
    mList = getChild<LLScrollListCtrl>("people");
    mFilter = getChild<LLLineEditor>("filter");
    mNpc = getChild<LLCheckBoxCtrl>("show_npcs");
    mMine = getChild<LLCheckBoxCtrl>("mine_only");
    mMine->set(gSavedSettings.getBOOL("WolfWatchWolfMineOnly"));
    mMine->setCommitCallback([this](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL("WolfWatchWolfMineOnly", c->getValue().asBoolean());
        mPage = 1;
        fetch();
    });
    mPer = getChild<LLComboBox>("per_page");
    mPrev = getChild<LLButton>("prev");
    mNext = getChild<LLButton>("next");
    mPageText = getChild<LLTextBox>("page_text");
    mSummary = getChild<LLTextBox>("summary");
    mStatus = getChild<LLTextBox>("status");
    mIm = getChild<LLButton>("im");
    mProfile = getChild<LLButton>("profile");
    mEject = getChild<LLButton>("eject");
    mBan = getChild<LLButton>("ban");

    mList->setSortChangedCallback([this]() { onSortChanged(); });
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelection(); });
    mList->setDoubleClickCallback([this]() { act("im"); });
    mFilter->setCommitCallback([this](LLUICtrl*, const LLSD&) { mPage = 1; fetch(); });
    mFilter->setCommitOnFocusLost(true);
    mNpc->setCommitCallback([this](LLUICtrl*, const LLSD&) { mPage = 1; fetch(); });
    mPer->setCommitCallback([this](LLUICtrl*, const LLSD&) { mPage = 1; fetch(); });
    mPrev->setCommitCallback([this](LLUICtrl*, const LLSD&) { if (mPage > 1) { --mPage; fetch(); } });
    mNext->setCommitCallback([this](LLUICtrl*, const LLSD&) { if (mPage < mPages) { ++mPage; fetch(); } });
    getChild<LLButton>("refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { fetch(); });
    mIm->setCommitCallback([this](LLUICtrl*, const LLSD&) { act("im"); });
    mProfile->setCommitCallback([this](LLUICtrl*, const LLSD&) { act("profile"); });
    mEject->setCommitCallback([this](LLUICtrl*, const LLSD&) { act("eject"); });
    mBan->setCommitCallback([this](LLUICtrl*, const LLSD&) { act("ban"); });
    onSelection();
    return true;
}

void WolfFloaterWatchWolf::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mNextFetch = 0.0;
}

void WolfFloaterWatchWolf::draw()
{
    if (!mBusy && LLFrameTimer::getTotalSeconds() >= mNextFetch)
    {
        fetch();
    }
    LLFloater::draw();
}

void WolfFloaterWatchWolf::setStatus(const std::string& text, bool error)
{
    mStatus->setText(text);
    mStatus->setColor(error ? LLColor4(1.f, 0.45f, 0.4f, 1.f) : LLColor4(0.75f, 0.77f, 0.8f, 1.f));
}

void WolfFloaterWatchWolf::fetch()
{
    mNextFetch = LLFrameTimer::getTotalSeconds() + REFRESH_SECONDS;
    if (!WolfGrid::isWolfTerritories())
    {
        // [WOLF ONLY] the request carries this login's agent and session: only ever to Wolf's own site.
        setStatus("WatchWolf is for Wolf Territories.", true);
        return;
    }
    mBusy = true;
    const std::string query = llformat("action=list&page=%d&per=%d&sort=%s&dir=%s&npc=%d&mine=%d&q=", mPage,
                                       mPer->getValue().asInteger() > 0 ? mPer->getValue().asInteger() : 50,
                                       mSort.c_str(), mDesc ? "desc" : "asc", mNpc->get() ? 1 : 0, mMine->get() ? 1 : 0)
                              + LLURI::escape(mFilter->getText());
    const LLHandle<LLFloater> h = getHandle();
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfWatchWolf", [h, query, serial]() { fetchCoro(h, query, serial); });
}

// static
void WolfFloaterWatchWolf::fetchCoro(LLHandle<LLFloater> handle, std::string query, U64 serial)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfWatchWolf", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLSD result = adapter->getRawAndSuspend(request, std::string(API_URL) + "?" + query, options, identity_headers(false));
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    const LLSD reply = reply_of(result);
    WolfFloaterWatchWolf* self = static_cast<WolfFloaterWatchWolf*>(handle.get());
    if (!self) return;
    if (serial != self->mSerial) return;     // a newer request is on its way
    self->mBusy = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        self->setStatus("Could not read who is on your regions: " + (reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString()), true);
        return;
    }
    self->show(reply);
}

void WolfFloaterWatchWolf::show(const LLSD& reply)
{
    const LLUUID keep = mList->getCurrentID();
    mRows = reply["rows"];
    mPage = reply["page"].asInteger();
    mPages = llmax(1, reply["pages"].asInteger());
    mList->deleteAllItems();
    for (const LLSD& r : llsd::inArray(mRows))
    {
        LLSD row;
        row["value"] = r["id"];
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = r["name"].asString() + (r["self"].asBoolean() ? " (you)" : "");
        row["columns"][1]["column"] = "region";
        row["columns"][1]["value"] = r["region"];
        row["columns"][2]["column"] = "estate";
        row["columns"][2]["value"] = r["estate"];
        row["columns"][3]["column"] = "status";
        row["columns"][3]["value"] = r["npc"].asBoolean() ? std::string("NPC") : (r["sitting"].asBoolean() ? std::string("sitting") : std::string("here"));
        row["columns"][4]["column"] = "position";
        row["columns"][4]["value"] = llformat("%d, %d, %d", r["x"].asInteger(), r["y"].asInteger(), r["z"].asInteger());
        mList->addElement(row);
    }
    // the list shows the site's order (it sorted the whole list, not just this page)
    mList->clearSortOrder();
    if (keep.notNull()) mList->selectByID(keep);
    mPageText->setText(llformat("Page %d of %d", mPage, mPages));
    mMine->setVisible(reply["isGod"].asBoolean());
    mPrev->setEnabled(mPage > 1);
    mNext->setEnabled(mPage < mPages);
    mSummary->setText(llformat("%d people on the %d regions you own or manage%s", reply["peopleOnThem"].asInteger(),
                               reply["regionsHeld"].asInteger(), mNpc->get() ? llformat(" (%d shown with NPCs)", reply["total"].asInteger()).c_str() : ""));
    setStatus(mRows.size() == 0 ? std::string("Nobody here matches.") : std::string(), false);
    onSelection();
}

// A click on a column heading: the site sorts the whole list by it, from the first page.
void WolfFloaterWatchWolf::onSortChanged()
{
    const std::string col = mList->getSortColumnName();
    const bool asc = mList->getSortAscending();
    if (col.empty() || col == "position") return;
    if (col == mSort && asc == !mDesc) return;
    mSort = col;
    mDesc = !asc;
    mPage = 1;
    fetch();
}

LLSD WolfFloaterWatchWolf::selectedRow() const
{
    const LLUUID id = mList->getCurrentID();
    if (id.isNull()) return LLSD();
    for (const LLSD& r : llsd::inArray(mRows))
    {
        if (r["id"].asUUID() == id) return r;
    }
    return LLSD();
}

void WolfFloaterWatchWolf::onSelection()
{
    const LLSD r = selectedRow();
    const bool person = r.isMap() && !r["npc"].asBoolean();
    const bool other = person && !r["self"].asBoolean();
    mIm->setEnabled(other);
    mProfile->setEnabled(person);
    mEject->setEnabled(other);
    mBan->setEnabled(other);
}

void WolfFloaterWatchWolf::act(const std::string& action)
{
    const LLSD r = selectedRow();
    if (!r.isMap() || r["npc"].asBoolean()) return;
    const LLUUID avatar = r["id"].asUUID();
    if (action == "im")
    {
        if (!r["self"].asBoolean()) LLAvatarActions::startIM(avatar);
        return;
    }
    if (action == "profile")
    {
        LLAvatarActions::showProfile(avatar);
        return;
    }
    if (r["self"].asBoolean()) return;
    const LLUUID region = r["regionId"].asUUID();
    const std::string name = r["name"].asString();
    const std::string msg = action == "ban"
        ? "Ban " + name + " from the estate \"" + r["estate"].asString() + "\" (every region in it) and send them home now?"
        : "Eject " + name + " from " + r["region"].asString() + " - send them home now?";
    const LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", msg), LLSD(),
        [h, action, avatar, region, name](const LLSD& n, const LLSD& resp)
        {
            if (LLNotificationsUtil::getSelectedOption(n, resp) != 0) return false;
            LLCoros::instance().launch("WolfWatchWolfAction", [h, action, avatar, region, name]() { actionCoro(h, action, avatar, region, name); });
            return false;
        });
}

// static
void WolfFloaterWatchWolf::actionCoro(LLHandle<LLFloater> handle, std::string action, LLUUID avatar, LLUUID region, std::string name)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfWatchWolf", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(60);    // a ban reloads every region of the estate
    options->setRetries(0);     // never sent twice
    const std::string body = "action=" + action + "&avatar=" + avatar.asString() + "&region=" + region.asString();
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());
    LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, identity_headers(true));
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    const LLSD reply = reply_of(result);
    const bool ok = status && reply.isMap() && reply["success"].asBoolean();
    std::string text;
    if (!ok)
        text = "Could not " + action + " " + name + ": " + (reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString());
    else if (action == "ban")
        text = llformat("%s is banned from the estate (%d of %d region servers reloaded it)%s.", name.c_str(),
                        reply["serversReloaded"].asInteger(), reply["servers"].asInteger(), reply["sentHome"].asBoolean() ? " and sent home" : "");
    else
        text = name + " was sent home.";
    WolfFloaterWatchWolf* self = static_cast<WolfFloaterWatchWolf*>(handle.get());
    if (self)
    {
        self->setStatus(text, !ok);
        self->fetch();
    }
    else if (!ok)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", text));
    }
}
