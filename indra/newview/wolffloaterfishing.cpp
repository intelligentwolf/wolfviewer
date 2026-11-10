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

#include <vector>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llappearancemgr.h"
#include "llbutton.h"
#include "llchat.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "wolfcave.h"
#include "wolfgrid.h"

namespace
{
    // lsl/fish_rod.lsl VCH: the rod's channel (ticket 679); negative = a ScriptDialogReply, heard region-wide
    constexpr S32 VCH = -76790;
    const std::string PREFIX = "WolfFish1|";
    // build_fishing.py ROD_DESC: the rod's description starts with this word (wolffishing_lib.php WF_DESC_PREFIX)
    const std::string ROD_DESC = "wolffish-rod";
    // php/wolffishing.php: gives the rod ({"action":"give"}) and reads the week ({"action":"week"}) as this resident
    // (the lift kit's give: wolffloaterlifts.cpp GIVE_URL, WolfCaves::serviceRequest carries the session pair)
    const char* API_URL = "https://wolfstorm.app/php/wolffishing.php";
    constexpr F32 API_TIMEOUT_S = 30.f;
    constexpr size_t MAX_LINE = 1023;          // llRegionSayTo's longest line (LSL_Api.cs llRegionSayTo)

    bool onWolf() { return WolfGrid::isOnWolfTerritories(); }

    // An object item (not a link) whose description starts "wolffish-rod" (wolffloaterlifts.cpp KitItems, the same test).
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
}

bool WolfFishing::onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0) return false;
    // [WOLF ONLY] as WolfLifts::onObjectChat: off Wolf the line is left to chat like any other
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT) return false;
    // only one of this agent's own attachments speaks to the window
    LLViewerObject* o = gObjectList.findObject(source);
    if (!o || !isAgentAvatarValid() || o->getAvatarAncestor() != gAgentAvatarp) return false;
    // from here the line is the window's, never chat (the rod only sends these after the window said hello)
    if (text.size() > MAX_LINE) return true;
    if (WolfFloaterFishing* f = LLFloaterReg::findTypedInstance<WolfFloaterFishing>("wolf_fishing")) f->onRodLine(text);
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
    mMine = getChild<LLTextBox>("fish_mine");
    mMyBest = getChild<LLScrollListCtrl>("fish_my_best");
    mBoard = getChild<LLScrollListCtrl>("fish_board");
    mBoardNote = getChild<LLTextBox>("fish_board_note");
    mLastWeek = getChild<LLTextBox>("fish_last_week");
    mCastBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCast(); });
    mReelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReel(); });
    mStopBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStop(); });
    mAwayBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPutAway(); });
    mUpdateBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshWeek(); });
    updateControls();
    return true;
}

void WolfFloaterFishing::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!onWolf())
    {
        setStatus("Fishing is on Wolf Territories regions. Come to Wolf Territories to fish.", true);
        updateControls();
        return;
    }
    refreshWeek();
    mRod = ROD_NONE;
    needRod();
}

void WolfFloaterFishing::onClose(bool app_quitting)
{
    // the rod goes back to owner chat and its touch menu (fish_rod.lsl "bye"), so a bite is never missed
    if (!app_quitting && mRod == ROD_READY) say("WFISH|bye");
    mRod = ROD_NONE;
    LLFloater::onClose(app_quitting);
}

void WolfFloaterFishing::say(const std::string& text)
{
    // a negative channel goes as a ScriptDialogReply (fsnearbychathub.cpp sendChatFromViewerFinal; wolffloaterlifts.cpp say)
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_NORMAL, false, VCH);
}

void WolfFloaterFishing::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterFishing::updateControls()
{
    if (!mCastBtn) return;
    const bool wolf = onWolf();
    const bool ready = wolf && mRod == ROD_READY;
    // Cast also puts the rod on again when it was taken off
    mCastBtn->setEnabled(wolf && mRod != ROD_FINDING && (mRod == ROD_NONE || mFish == FISH_IDLE));
    mCastBtn->setLabel(mRod == ROD_READY || mRod == ROD_FINDING ? std::string("Cast") : std::string("Get my rod"));
    mReelBtn->setEnabled(ready && (mFish == FISH_WAITING || mFish == FISH_BITE));
    mStopBtn->setEnabled(ready && (mFish == FISH_WAITING || mFish == FISH_BITE));
    mAwayBtn->setEnabled(ready && mFish == FISH_IDLE);
    mUpdateBtn->setEnabled(wolf && !mWeekBusy);
}

void WolfFloaterFishing::onCast()
{
    if (mRod != ROD_READY) { needRod(); return; }
    if (mFish != FISH_IDLE) return;
    mFish = FISH_BUSY;
    setStatus("Casting...", false);
    updateControls();
    say("WFISH|cast");
}

void WolfFloaterFishing::onReel()
{
    if (mRod != ROD_READY || (mFish != FISH_WAITING && mFish != FISH_BITE)) return;
    mFish = FISH_BUSY;
    setStatus("Reeling in...", false);
    updateControls();
    say("WFISH|reel");
}

void WolfFloaterFishing::onStop()
{
    if (mRod != ROD_READY) return;
    say("WFISH|stop");
}

void WolfFloaterFishing::onPutAway()
{
    if (LLViewerInventoryItem* rod = findRod())
    {
        if (get_is_item_worn(rod->getUUID())) LLAppearanceMgr::instance().removeItemFromAvatar(rod->getUUID());
    }
    mRod = ROD_NONE;
    mFish = FISH_IDLE;
    setStatus("Your rod is put away. Press Get my rod to fish again.", false);
    updateControls();
}

void WolfFloaterFishing::onRodLine(const std::string& text)
{
    const std::vector<std::string> f = fields(text);
    const std::string kind = fieldAt(f, 1);
    auto fishState = [this](const std::string& s)
    {
        if (s == "waiting") mFish = FISH_WAITING;
        else if (s == "bite") mFish = FISH_BITE;
        else if (s == "busy") mFish = FISH_BUSY;
        else mFish = FISH_IDLE;
    };
    if (kind == "hi")
    {
        mRod = ROD_READY;
        fishState(fieldAt(f, 2));
        if (mFish == FISH_BITE) setStatus("A fish is biting! Reel it in whenever you are ready.", false);
        else if (mFish == FISH_WAITING) setStatus("Your line is in the water. Waiting for a bite...", false);
        else setStatus("Your rod is ready. Face the water and press Cast.", false);
    }
    else if (kind == "state")
    {
        fishState(fieldAt(f, 2));
        setStatus(fieldAt(f, 3), false);
    }
    else if (kind == "catch")
    {
        // catch | fish or junk | name | words | counted | score | fish | place | text
        mFish = FISH_IDLE;
        setStatus(fieldAt(f, 9), false);
        if (fieldAt(f, 2) == "fish" && fieldAt(f, 5) == "1") refreshWeek();
    }
    else if (kind == "error")
    {
        mFish = FISH_IDLE;
        setStatus(fieldAt(f, 2), true);
    }
    else if (kind == "info")
    {
        setStatus(fieldAt(f, 2), false);
    }
    else if (kind == "off")
    {
        mRod = ROD_NONE;
        mFish = FISH_IDLE;
        setStatus("Your rod was taken off. Press Get my rod to fish again.", false);
    }
    updateControls();
}

void WolfFloaterFishing::needRod()
{
    if (mRodCoroRunning || mRod == ROD_READY || !onWolf()) return;
    mRod = ROD_FINDING;
    mRodCoroRunning = true;
    updateControls();
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfFishingRod", [h]() { rodCoro(h); });
}

void WolfFloaterFishing::rodFailed(const std::string& why)
{
    mRodCoroRunning = false;
    mRod = ROD_NONE;
    setStatus(why, true);
    updateControls();
}

void WolfFloaterFishing::rodCoro(LLHandle<LLFloater> h)
{
    // The window is looked up again through its handle after every wait (wolffloaterlifts.cpp boxCoro): closed meanwhile,
    // the coroutine stops.
    auto self = [&h]() { return dynamic_cast<WolfFloaterFishing*>(h.get()); };
    // 1. a rod already worn answers hello
    if (WolfFloaterFishing* f = self()) { f->setStatus("Looking for your fishing rod...", false); f->say("WFISH|hello|1"); }
    llcoro::suspendUntilTimeout(2.5f);
    WolfFloaterFishing* f = self();
    if (!f) return;
    if (f->mRod == ROD_READY || !f->getVisible()) { f->mRodCoroRunning = false; return; }
    // 2. the rod in the inventory; when there is none, the grid gives one (php/wolffishing.php) and it is waited for
    LLViewerInventoryItem* rod = findRod();
    if (!rod)
    {
        f->setStatus("You have no fishing rod yet - getting one for you (it goes into Objects > Fishing)...", false);
        const std::string body("{\"action\":\"give\"}");
        LLSD result = WolfCaves::serviceRequest(API_URL, &body, API_TIMEOUT_S);
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        f = self();
        if (!f) return;
        if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
        {
            std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
                : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
            f->rodFailed("The fishing rod could not be fetched: " + why);
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
            if (!self()) return;
            rod = item_id.notNull() ? gInventory.getItem(item_id) : nullptr;
            if (rod && !rod->isFinished()) rod = nullptr;
            if (!rod) rod = findRod();
        }
        f = self();
        if (!f) return;
        if (!rod)
        {
            f->rodFailed("Your fishing rod is in your inventory (Objects > Fishing), but this viewer could not see it yet. "
                         "Close the window and open it again.");
            return;
        }
    }
    // 3. put it on (in the right hand: the rod's own attach point), added beside anything already worn there
    const LLUUID rod_id = rod->getUUID();
    if (!get_is_item_worn(rod_id))
    {
        f->setStatus("Putting your fishing rod in your hand...", false);
        LLAppearanceMgr::instance().wearItemOnAvatar(rod_id, true, false);
    }
    else
    {
        f->setStatus("Waiting for your fishing rod to answer...", false);
    }
    for (S32 t = 0; t < 24; ++t)
    {
        llcoro::suspendUntilTimeout(1.f);
        f = self();
        if (!f) return;
        if (f->mRod == ROD_READY) { f->mRodCoroRunning = false; return; }
        if (t % 3 == 1) f->say("WFISH|hello|1");
    }
    // not on after 24 s: scripts off here, or (not worn) OpenSim's appearance already lists it and ignored the wear
    // (AttachmentsModule.cs RezSingleAttachmentFromInventory "alreadyOn"), which a new login puts right
    f->rodFailed(get_is_item_worn(rod_id)
        ? "Your fishing rod is in your hand but did not answer. Scripts may be switched off here (About Land > Options) - "
          "try somewhere else, or take the rod off and put it on again."
        : "Your fishing rod could not be put on just now. Press Get my rod to try again; if it still will not go on, log out and back in.");
}

void WolfFloaterFishing::refreshWeek()
{
    if (mWeekBusy || !onWolf()) return;
    mWeekBusy = true;
    updateControls();
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfFishingWeek", [h]() { weekCoro(h); });
}

void WolfFloaterFishing::weekCoro(LLHandle<LLFloater> h)
{
    const std::string body("{\"action\":\"week\"}");
    LLSD result = WolfCaves::serviceRequest(API_URL, &body, API_TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    WolfFloaterFishing* f = dynamic_cast<WolfFloaterFishing*>(h.get());
    if (!f) return;
    f->mWeekBusy = false;
    if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
    {
        std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
            : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
        f->mBoardNote->setText("This week's board could not be read just now: " + why + " Press Update to try again.");
        f->mBoardNote->setColor(LLUIColorTable::instance().getColor("AlertCautionTextColor"));
    }
    else
    {
        f->showWeek(reply);
    }
    f->updateControls();
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
