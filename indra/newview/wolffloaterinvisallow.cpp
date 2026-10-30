/**
 * @file wolffloaterinvisallow.cpp
 * @brief WolfViewer: Invisible Mode - the people who can still see you online. See wolffloaterinvisallow.h.
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

#include "wolffloaterinvisallow.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcallingcard.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewergenericmessage.h"
#include "wolfgrid.h"

namespace
{
    // wolfstorm/php/wolf_invisible_allow.php (WIA_MAX = 100 is its own cap; the answer's "max" is shown).
    const char* ALLOW_API = "https://wolfstorm.app/php/wolf_invisible_allow.php";
    // The WolfSim module's "my list changed" message (WolfInvisibleModule METHOD_ALLOW, parameter "reload").
    const std::string METHOD_TO_SIM("wolfinvisibleallow");

    // Source: wolffloaterdob.cpp json_to_llsd (itself from wolfviewerkey.cpp)
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    std::string json_string(const LLSD& v, const char* key)
    {
        return v.isMap() && v.has(key) ? v[key].asString() : std::string();
    }

    /** POST the JSON body with this login's agent and session. Source: wolffloaterdob.cpp post_dob (verified TLS, no
     *  redirects, the session in the headers), with a JSON body as wolfautoenvironment.cpp sends. Runs in a coroutine. */
    LLSD post_allow(const LLSD& body, LLCore::HttpStatus& status)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfInvisAllow", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        const std::string text = boost::json::serialize(LlsdToJson(body));
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(text.data(), text.size());
        LLSD result = adapter->postRawAndSuspend(request, ALLOW_API, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }
}

WolfFloaterInvisAllow::WolfFloaterInvisAllow(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterInvisAllow::postBuild()
{
    mList = getChild<LLScrollListCtrl>("allowed");
    mName = getChild<LLLineEditor>("add_name");
    mFriends = getChild<LLComboBox>("friends");
    mAddName = getChild<LLButton>("add_name_btn");
    mAddFriend = getChild<LLButton>("add_friend_btn");
    mRemove = getChild<LLButton>("remove_btn");
    mCount = getChild<LLTextBox>("count");
    mStatus = getChild<LLTextEditor>("status");

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mName->setKeystrokeCallback([this](LLLineEditor*, void*) { updateButtons(); }, nullptr);
    mName->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddName(); });   // Enter in the name box adds
    mFriends->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mAddName->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddName(); });
    mAddFriend->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddFriend(); });
    mRemove->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    updateButtons();
    return true;
}

void WolfFloaterInvisAllow::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    fillFriends();
    if (mBusy) return;
    if (!WolfGrid::isWolfTerritories())
    {
        mList->deleteAllItems();
        setStatus("This list is for Wolf Territories residents. Log in to Wolf Territories to use it.", EStatus::ERR);
        mAddName->setEnabled(false);   // nothing to send to: every control stays off
        mAddFriend->setEnabled(false);
        mRemove->setEnabled(false);
        return;
    }
    LLSD body = LLSD::emptyMap();
    body["action"] = "list";
    send(body, "Loading your list...");
}

void WolfFloaterInvisAllow::fillFriends()
{
    // The friends this viewer knows (LLAvatarTracker, the friends list). A hypergrid friend can be picked; the grid
    // answers that only Wolf Territories residents can be added, and that answer is shown.
    mFriends->removeall();
    LLAvatarTracker::buddy_map_t buddies;
    LLAvatarTracker::instance().copyBuddyList(buddies);
    for (const auto& b : buddies)
    {
        LLAvatarName av;
        std::string name;
        if (LLAvatarNameCache::get(b.first, &av)) name = av.getCompleteName();
        if (name.empty()) continue;   // not known yet: shown next time the window opens
        mFriends->add(name, LLSD(b.first.asString()));
    }
    mFriends->sortByName();
    mFriends->add("Choose a friend", LLSD(std::string()), ADD_TOP);
    mFriends->selectFirstItem();
}

void WolfFloaterInvisAllow::updateButtons()
{
    const bool full = mList->getItemCount() >= mMax;
    const bool can = !mBusy && WolfGrid::isWolfTerritories();
    mAddName->setEnabled(can && !full && !mName->getText().empty());
    mAddFriend->setEnabled(can && !full && !mFriends->getValue().asString().empty());
    mRemove->setEnabled(can && mList->getFirstSelected() != nullptr);
}

void WolfFloaterInvisAllow::setBusy(bool busy)
{
    mBusy = busy;
    updateButtons();
}

void WolfFloaterInvisAllow::setStatus(const std::string& text, EStatus kind)
{
    // Source: wolffloaterdob.cpp setStatus - light red / light green on the dark floater, and the words say it too
    static const LLUIColor err(LLColor4(1.f, 0.6f, 0.6f, 1.f));
    static const LLUIColor ok(LLColor4(0.79f, 1.f, 0.79f, 1.f));
    static const LLUIColor info(LLColor4::white);
    const LLUIColor& c = kind == EStatus::ERR ? err : kind == EStatus::OK ? ok : info;
    mStatus->setReadOnlyColor(c);
    mStatus->setColor(c);
    mStatus->setText(text);
    mStatus->setVisible(!text.empty());
}

void WolfFloaterInvisAllow::onAddName()
{
    if (mBusy) return;
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        setStatus("Type the avatar name of the person to add, for example \"Lone Wolf\".", EStatus::ERR);
        return;
    }
    LLSD body = LLSD::emptyMap();
    body["action"] = "add";
    body["name"] = name;
    send(body, "Adding " + name + "...");
}

void WolfFloaterInvisAllow::onAddFriend()
{
    if (mBusy) return;
    const std::string id = mFriends->getValue().asString();
    if (id.empty())
    {
        setStatus("Choose a friend from the list first.", EStatus::ERR);
        return;
    }
    LLSD body = LLSD::emptyMap();
    body["action"] = "add";
    body["agent"] = id;
    send(body, "Adding " + mFriends->getSimple() + "...");
}

void WolfFloaterInvisAllow::onRemove()
{
    if (mBusy) return;
    LLScrollListItem* item = mList->getFirstSelected();
    if (!item) return;
    LLSD body = LLSD::emptyMap();
    body["action"] = "remove";
    body["agent"] = item->getValue().asString();
    body["name_shown"] = item->getColumn(0) ? item->getColumn(0)->getValue().asString() : std::string();
    send(body, "Removing...");
}

void WolfFloaterInvisAllow::send(const LLSD& body, const std::string& doing)
{
    setBusy(true);
    setStatus(doing, EStatus::INFO);
    LLSD wire = body;
    wire.erase("name_shown");   // ours only, for the message afterwards
    LLHandle<LLFloater> handle = getHandle();
    LLSD both = LLSD::emptyMap();
    both["wire"] = wire;
    both["body"] = body;
    LLCoros::instance().launch("WolfInvisAllow", [handle, both]() { sendCoro(handle, both); });
}

// static
void WolfFloaterInvisAllow::sendCoro(LLHandle<LLFloater> handle, LLSD both)
{
    LLCore::HttpStatus status;
    LLSD reply = post_allow(both["wire"], status);
    std::string error;
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        error = json_string(reply, "error");
        if (error.empty())
        {
            error = "Wolf Territories did not answer properly (" + status.toString() + "). Nothing was changed. "
                    "Please try again in a moment.";
        }
        LL_WARNS("WolfInvisAllow") << both["wire"]["action"].asString() << ": " << status.toString() << " " << error << LL_ENDL;
    }
    WolfFloaterInvisAllow* self = static_cast<WolfFloaterInvisAllow*>(handle.get());
    if (!self) return;
    self->onReply(both["body"], reply, error);
}

void WolfFloaterInvisAllow::onReply(const LLSD& body, const LLSD& reply, const std::string& error)
{
    setBusy(false);
    const std::string action = body["action"].asString();
    if (!error.empty())
    {
        setStatus(error, EStatus::ERR);   // stays until the next action
        return;
    }

    if (reply.has("max") && reply["max"].asInteger() > 0) mMax = reply["max"].asInteger();
    const LLUUID selected = mList->getFirstSelected() ? mList->getFirstSelected()->getUUID() : LLUUID::null;
    mList->deleteAllItems();
    const LLSD& allowed = reply["allowed"];
    for (LLSD::array_const_iterator it = allowed.beginArray(); it != allowed.endArray(); ++it)
    {
        LLSD row = LLSD::emptyMap();
        row["id"] = (*it)["agent"].asUUID();
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = (*it)["name"].asString();
        mList->addElement(row);
    }
    if (mList->getItemCount() == 0)
    {
        mList->setCommentText("Nobody yet. While Invisible Mode is on, everyone sees you as offline.");
    }
    else if (selected.notNull())
    {
        mList->selectByID(selected);
    }
    mCount->setText(llformat("%d of %d", mList->getItemCount(), mMax));
    updateButtons();

    if (action == "list")
    {
        setStatus(std::string(), EStatus::INFO);
        return;
    }

    // The list changed: the region this viewer stands on reads it again (only a Wolf region that has said so: the same
    // gate wolfinvisible.cpp uses to send "wolfinvisible"). Other regions read it when the viewer arrives there.
    if (WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        send_generic_message(METHOD_TO_SIM, { "reload" });
    }
    if (action == "add")
    {
        mName->clear();
        mFriends->selectFirstItem();
        updateButtons();
        setStatus(json_string(reply["added"], "name") + " can now see you online while you are invisible. They have not been told.",
                  EStatus::OK);
    }
    else
    {
        const std::string who = body["name_shown"].asString();
        setStatus((who.empty() ? std::string("They") : who) + " will now see you as offline while you are invisible.", EStatus::OK);
    }
}
