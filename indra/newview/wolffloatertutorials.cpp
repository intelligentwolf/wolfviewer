/**
 * @file wolffloatertutorials.cpp
 * @brief WolfViewer: Tutorial Videos - "How to ..." cards that open our YouTube tutorials in the web browser (ticket 847).
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

#include "wolffloatertutorials.h"

#include "llbutton.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llpanel.h"
#include "llscrollcontainer.h"
#include "llsdutil.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llweb.h"
#include "wolfcave.h"
#include "wolfgrid.h"

namespace
{
    // Source: php/wolftutorials.php (both web hosts; the same host as the other Wolf services, wolffishingoverlay.cpp API_URL)
    const char* LIST_URL = "https://wolfstorm.app/php/wolftutorials.php";
    constexpr F32 LIST_TIMEOUT_S = 30.f;
    constexpr F64 LIST_MAX_AGE_S = 6.0 * 3600.0;   // CHOSEN: the list is fetched again only when this old (DESIGN.md s2)
    // Source: menu_viewer.xml "Wolf YouTube" (Wolf Grid > YouTube) - the channel's own address
    const char* CHANNEL_URL = "https://www.youtube.com/@wolfterritoriesgrid";
    const char* WATCH_URL = "https://www.youtube.com/watch?v=";
    // the card's size (panel_wolf_tutorial_card.xml) and the gap between cards
    constexpr S32 CARD_W = 520, CARD_H = 96, CARD_GAP = 8;

    // the list for this session: kept between openings of the window
    LLSD sVideos;               // [{id, title, about}] as the server sent them (checked again when the cards are built)
    F64 sListAt = -1.0;         // when it arrived (LLFrameTimer total seconds), -1 = never
    bool sFetching = false;
    std::string sError;         // the last fetch's failure, in words

    /** A panel that lets Escape through to the floater (wolffloaterexperiences.cpp PassEscapePanel). */
    class PassEscapePanel : public LLPanel
    {
    public:
        PassEscapePanel() : LLPanel() {}
        bool handleKeyHere(KEY key, MASK mask) override
        {
            if (key == KEY_ESCAPE) return false;
            return LLPanel::handleKeyHere(key, mask);
        }
    };

    /** A rect from the top-left of a parent of the given height (wolffloaterexperiences.cpp topLeft). */
    LLRect topLeft(S32 parent_h, S32 left, S32 top, S32 w, S32 h)
    {
        return LLRect(left, parent_h - top, left + w, parent_h - top - h);
    }
}

WolfFloaterTutorials::WolfFloaterTutorials(const LLSD& key)
:   LLFloater(key)
{
}

// static
bool WolfFloaterTutorials::validId(const std::string& id)
{
    if (id.size() != 11) return false;
    for (const char c : id)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool WolfFloaterTutorials::postBuild()
{
    mScroll = getChild<LLScrollContainer>("cards_scroll");
    mStatus = getChild<LLTextBox>("status");
    mRetry = getChild<LLButton>("retry");
    mCards = new PassEscapePanel();
    mCards->setRect(LLRect(0, 0, CARD_W, 0));
    mCards->setFollows(FOLLOWS_LEFT | FOLLOWS_TOP);
    mScroll->addChild(mCards);
    mRetry->setCommitCallback([](LLUICtrl*, const LLSD&) { fetch(); });
    getChild<LLButton>("channel")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(CHANNEL_URL); });
    return true;
}

void WolfFloaterTutorials::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // read when the window opens; kept for the session (DESIGN.md s2: no timer, no polling)
    if (!sFetching && (sListAt < 0.0 || LLFrameTimer::getTotalSeconds() - sListAt >= LIST_MAX_AGE_S)) fetch();
    refreshView();
}

void WolfFloaterTutorials::draw()
{
    // Wolf Territories only (paul-wolf-features-wolf-grid-only): no window reachable elsewhere
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

// static
void WolfFloaterTutorials::fetch()
{
    if (sFetching) return;
    sFetching = true;
    sError.clear();
    if (WolfFloaterTutorials* f = LLFloaterReg::findTypedInstance<WolfFloaterTutorials>("wolf_tutorials")) f->refreshView();
    LLCoros::instance().launch("WolfTutorialsList", []() { fetchCoro(); });
}

// static
void WolfFloaterTutorials::fetchCoro()
{
    // a GET (WolfCaves::serviceRequest with no body: wolfcave.h); the list is public, the session headers it adds are ignored
    LLSD result = WolfCaves::serviceRequest(LIST_URL, nullptr, LIST_TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    sFetching = false;
    // Source: php/wolftutorials.php -> {success: true, channel, videos: [{id, title, about}]} | {success: false, error}
    if (status != 200 || !reply.isMap() || !reply["success"].asBoolean() || !reply["videos"].isArray())
    {
        sError = reply.isMap() && reply.has("error") ? reply["error"].asString()
            : (transport.empty() ? llformat("the grid answered HTTP %d.", status) : transport);
        LL_INFOS("WolfTutorials") << "the list could not be read (" << status << "): " << sError << LL_ENDL;
    }
    else
    {
        sVideos = reply["videos"];
        sListAt = LLFrameTimer::getTotalSeconds();
        LL_INFOS("WolfTutorials") << "the list: " << sVideos.size() << " videos" << LL_ENDL;
    }
    if (WolfFloaterTutorials* f = LLFloaterReg::findTypedInstance<WolfFloaterTutorials>("wolf_tutorials")) f->refreshView();
}

void WolfFloaterTutorials::refreshView()
{
    if (!mStatus || !mRetry || !mScroll) return;
    if (sListAt >= 0.0 && mBuiltFor != sListAt) buildCards();
    const bool have = sListAt >= 0.0 && mCards->getChildCount() > 0;
    std::string msg;
    bool error = false;
    if (!sError.empty())
    {
        // a failed fetch shows only the error and Try again, an older list too (WolfStorm floater_wolf_tutorials.js the same)
        msg = "The list of videos could not be fetched: " + sError + " Press Try again.";
        error = true;
    }
    else if (sFetching && !have)
    {
        msg = "Finding the videos...";
    }
    else if (sListAt >= 0.0 && !have)
    {
        msg = "There are no videos to show just now. Press \"All our videos on YouTube\" below to see them all.";
    }
    mStatus->setText(msg);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
    mStatus->setVisible(!msg.empty());
    mRetry->setVisible(error && !sFetching);
    // the cards go below the message while there is one
    mScroll->setVisible(have && msg.empty());
}

void WolfFloaterTutorials::buildCards()
{
    mBuiltFor = sListAt;
    mCards->deleteAllChildren();
    std::vector<LLSD> shown;
    for (const LLSD& v : llsd::inArray(sVideos))
    {
        if (!v.isMap() || !validId(v["id"].asString()) || v["title"].asString().empty()) continue;
        shown.push_back(v);
    }
    const S32 total_h = (S32)shown.size() * (CARD_H + CARD_GAP);
    mCards->setRect(LLRect(0, total_h, CARD_W, 0));
    S32 top = 0;
    for (const LLSD& v : shown)
    {
        PassEscapePanel* card = new PassEscapePanel();
        card->buildFromFile("panel_wolf_tutorial_card.xml");
        card->setRect(topLeft(total_h, 0, top, CARD_W, CARD_H));
        mCards->addChild(card);   // in order: Tab reaches the Watch buttons top to bottom
        const std::string id = v["id"].asString();
        const std::string title = v["title"].asString();
        card->getChild<LLTextBox>("title")->setText(title);
        card->getChild<LLTextBox>("about")->setText(v["about"].asString());
        LLButton* watch = card->getChild<LLButton>("watch");
        watch->setToolTip("Watch \"" + title + "\" in your web browser.");
        watch->setCommitCallback([id](LLUICtrl*, const LLSD&)
        {
            // built here from the checked id, never a link from the reply (DESIGN.md s2 "Safety")
            if (validId(id)) LLWeb::loadURLExternal(std::string(WATCH_URL) + id);
        });
        top += CARD_H + CARD_GAP;
    }
    mScroll->goToTop();
}
