/**
 * @file wolfviewerkey.cpp
 * @brief WolfViewer: the per-login key that tells Wolf Territories regions this login is WolfViewer's.
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

#include "wolfviewerkey.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <boost/json.hpp>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"
#include "wolfgrid.h"

// [COPYBOT EXEMPT 2026-10-19] (wolfviewerkey.h)
namespace
{
    // Source: wolfstorm/php/wolfviewer_key.php, deployed to wolfstorm.app (the host SHARE_TOKEN_URL in wolfgrid.h uses).
    const char* const KEY_URL = "https://wolfstorm.app/php/wolfviewer_key.php";
    // Source: WolfCopybotGuard.cs VIEWER_KEY_METHOD - the region's handler (lower case: LLClientView.HandleGenericMessage
    // lower-cases the method before looking it up).
    const std::string METHOD_TO_SIM("wolfviewerkey");
    // A failed fetch is tried again after this, doubling up to RETRY_MAX_S.
    const F64 RETRY_MIN_S = 60.0;
    const F64 RETRY_MAX_S = 1800.0;
    // A key this close to its end is no longer handed out (the region refuses it once its expiry has passed).
    const F64 END_MARGIN_S = 120.0;

    LLUUID sSession;                       // the login the key below belongs to
    std::string sKey;
    F64 sKeyAt = 0.0;                      // LLFrameTimer::getElapsedSeconds() when it arrived
    F64 sLifeS = 0.0;                      // its life, by the brain's own clock (expires - now in the reply)
    bool sFetching = false;
    F64 sRetryAt = 0.0;
    F64 sRetryWait = RETRY_MIN_S;
    // region id -> the region object and the key it was given (a region met again is a new object: told again)
    std::map<LLUUID, std::pair<const LLViewerRegion*, std::string>> sTold;

    // Source: wolfautoenvironment.cpp json_to_llsd (the same helper every Wolf service reader has)
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    void failed(const std::string& why)
    {
        sRetryAt = LLFrameTimer::getElapsedSeconds() + sRetryWait;
        LL_WARNS("WolfViewerKey") << "no key (" << why << "); asking again in " << sRetryWait << " s" << LL_ENDL;
        sRetryWait = std::min(sRetryWait * 2.0, RETRY_MAX_S);
    }

    void fetchCoro(LLUUID agent, LLUUID session)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfViewerKey", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        // Source: wolfautoenvironment.cpp postCoro - verified TLS, no redirects (the session id is in the headers)
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append("X-Wolf-Agent", agent.asString());
        headers->append("X-Wolf-Session", session.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());   // the headers are the whole request
        LLSD result = adapter->postRawAndSuspend(request, KEY_URL, raw, options, headers);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());

        sFetching = false;
        if (session != gAgentSessionID)
        {
            return;                         // a new login since: its own key is fetched for it
        }
        if (!status || !reply.isMap() || !reply["success"].asBoolean())
        {
            failed(reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString());
            return;
        }
        // Source: wolfviewer_key.php answer {"success":true,"key":"1.<expiry>.<hex>","expires":<unix>,"now":<unix>}
        const std::string key = reply["key"].asString();
        const F64 life = reply["expires"].asReal() - reply["now"].asReal();
        if (key.empty() || key.size() > 120 || life <= END_MARGIN_S)
        {
            failed("unreadable reply");
            return;
        }
        sKey = key;
        sKeyAt = LLFrameTimer::getElapsedSeconds();
        sLifeS = life;
        sRetryWait = RETRY_MIN_S;
        LL_INFOS("WolfViewerKey") << "key for this login, good for " << life << " s" << LL_ENDL;
    }

    // Source: llviewergenericmessage.cpp send_generic_message - the same GenericMessage, sent to this region's own
    // circuit (send_generic_message sends only to the agent's region, gAgent.sendReliableMessage). One parameter:
    // addString writes it with its NUL, which the region's Util.FieldToString removes.
    void sendKey(const LLHost& host, const std::string& key)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessage("GenericMessage");
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_TransactionID, LLUUID::null); //not used
        msg->nextBlock("MethodData");
        msg->addString("Method", METHOD_TO_SIM);
        msg->addUUID("Invoice", LLUUID::null);
        msg->nextBlock("ParamList");
        msg->addString("Parameter", key);
        msg->sendReliable(host);
    }
}

void WolfViewerKey::idle()
{
    // Source: wolfinvisible.cpp WolfInvisible::idle - logged in and in a region.
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion() || !gMessageSystem)
    {
        return;
    }
    // A region gone from the world (teleport, crossing, a hypergrid trip and back) is forgotten even while the gate below
    // is shut, so it is given the key again when it comes back (a new circuit there, a new client on the sim).
    std::set<LLUUID> present;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        if (region)
        {
            present.insert(region->getRegionID());
        }
    }
    for (auto it = sTold.begin(); it != sTold.end();)
    {
        it = present.count(it->first) ? std::next(it) : sTold.erase(it);
    }
    // [WOLF GRID GATE] Wolf Territories logins on a region that has itself said it is Wolf (wolfgrid.h
    // isOnWolfTerritoriesConfirmed, as WolfInvisible): the session goes to wolfstorm.app only when it is a Wolf session,
    // and the key only to Wolf regions (the neighbours of a Wolf region are Wolf regions: the grid's own neighbour list).
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    if (gAgentSessionID != sSession)
    {
        // a new login (llstartup.cpp sets a new session id): nothing from the last one counts
        sSession = gAgentSessionID;
        sKey.clear();
        sLifeS = 0.0;
        sRetryAt = 0.0;
        sRetryWait = RETRY_MIN_S;
        sTold.clear();
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 age = now - sKeyAt;
    const bool usable = !sKey.empty() && age < sLifeS - END_MARGIN_S;
    if (!sFetching && now >= sRetryAt && (!usable || age >= sLifeS * 0.5))
    {
        sFetching = true;
        const LLUUID agent = gAgentID;
        const LLUUID session = gAgentSessionID;
        LLCoros::instance().launch("WolfViewerKey", [agent, session]() { fetchCoro(agent, session); });
    }
    if (!usable)
    {
        return;
    }

    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        // the circuit is up and the region has answered its seed capability (asset fetches start after that)
        if (!region || !region->isAlive() || !region->capabilitiesReceived())
        {
            continue;
        }
        std::pair<const LLViewerRegion*, std::string>& told = sTold[region->getRegionID()];
        if (told.first != region || told.second != sKey)
        {
            told = { region, sKey };
            sendKey(region->getHost(), sKey);
            LL_INFOS("WolfViewerKey") << "key given to " << region->getName() << LL_ENDL;
        }
    }
}
