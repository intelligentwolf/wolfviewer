/**
 * @file wolfrecordprivacy.cpp
 * @brief WolfViewer: "Don't include me in other people's recordings". See wolfrecordprivacy.h.
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

#include "wolfrecordprivacy.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <boost/json.hpp>

#include "llagent.h"
#include "llcharacter.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llhudnametag.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "lluri.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "wolfgrid.h"
#include "wolfrecorder.h"

namespace
{
    // gridmanager/recordingprivacy.php on the grid website (the same host and headers as viewerdob.php, which
    // wolffloaterdob.cpp DOB_API calls)
    const char* API = "https://www.wolf-grid.com/recordingprivacy.php";
    constexpr size_t kBatchMax = 100;       // recordingprivacy.php RP_LOOKUP_MAX: ids a lookup may carry
    constexpr F64 kKeepAnswerS = 300.0;     // an answer is asked again after 5 minutes (a choice changed meanwhile)
    constexpr F64 kRequestGapS = 2.0;       // at most one lookup every 2 s
    constexpr F64 kRetryAfterFailS = 10.0;  // after a failed lookup
    constexpr F64 kScanEveryS = 1.0;        // the avatars around are listed once a second
    constexpr F64 kAwakeS = 5.0;            // keepLookingUp keeps the lookups going this long after the window's draw
    // Someone not known yet (just arrived, the answer on its way) is left out of the video until the answer comes -
    // the safe side for the person who asked - but only this long: if the website cannot be reached, a recording is
    // not greyed out for everyone around (the window says the check is failing, lookupTrouble).
    constexpr F64 kUnknownHiddenS = 20.0;

    struct Entry
    {
        bool known = false;      // an answer has come
        bool opted_out = false;  // known: their answer
        bool asking = false;     // in the request in flight
        F64 first_seen = 0.0;    // when first wanted (for kUnknownHiddenS)
        F64 answered = 0.0;      // when the answer came
    };
    std::unordered_map<LLUUID, Entry> sCache;
    bool sInFlight = false;
    F64 sNextRequest = 0.0;
    F64 sAwakeUntil = 0.0;
    F64 sNextScan = 0.0;
    F64 sFailingSince = 0.0;     // 0 = the last lookup worked
    S32 sNearby = 0;

    // own choice
    WolfRecordPrivacy::Own sOwn = WolfRecordPrivacy::Own::UNAVAILABLE;
    bool sOwnOptedOut = false;
    bool sOwnAsked = false;
    F64 sOwnRetryAt = 0.0;       // a failed read is asked again from then (never every frame)
    LLUUID sOwnSession;          // the login sOwn belongs to
    std::string sOwnError;

    // Source: wolfviewerkey.cpp json_to_llsd (the helper every Wolf service reader has; wolffloaterdob.cpp's copy)
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    /** POST `body` to recordingprivacy.php; with the login's agent and session headers when `signed_in`. Returns the
     *  JSON reply as LLSD (empty when there was none) and the HTTP status. Runs in a coroutine.
     *  Source: wolffloaterdob.cpp post_dob (verified TLS, no redirects, form-encoded body). */
    LLSD post(const std::string& body, bool signed_in, LLCore::HttpStatus& status)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfRecordPrivacy", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
        if (signed_in)
        {
            headers->append("X-Wolf-Agent", gAgentID.asString());
            headers->append("X-Wolf-Session", gAgentSessionID.asString());
        }
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, API, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }

    // Source: httpcommon.h - an HTTP answer's code is the status type (100-999, isHttpStatus); wolffloaterdob.cpp statusCoro
    int httpCode(const LLCore::HttpStatus& status)
    {
        return status.isHttpStatus() ? static_cast<int>(status.getType()) : 0;
    }

    void lookupCoro(std::vector<LLUUID> ids)
    {
        std::string body = "action=lookup&ids=";
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i) body += ",";
            body += ids[i].asString();
        }
        LLCore::HttpStatus status;
        LLSD reply = post(body, false, status);
        sInFlight = false;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const bool ok = reply.isMap() && reply["success"].asBoolean() && reply["opted_out"].isArray();
        if (!ok)
        {
            for (const LLUUID& id : ids)
            {
                auto it = sCache.find(id);
                if (it != sCache.end()) it->second.asking = false;
            }
            if (sFailingSince == 0.0) sFailingSince = now;
            sNextRequest = now + kRetryAfterFailS;
            LL_WARNS("WolfRecordPrivacy") << "lookup of " << ids.size() << " avatars failed (" << status.toString() << " "
                                          << (reply.isMap() ? reply["error"].asString() : std::string())
                                          << "); trying again in " << kRetryAfterFailS << " s" << LL_ENDL;
            return;
        }
        sFailingSince = 0.0;
        std::unordered_map<LLUUID, bool> out;
        for (const LLSD& v : llsd::inArray(reply["opted_out"]))
        {
            LLUUID id;
            if (LLUUID::parseUUID(v.asString(), &id)) out[id] = true;
        }
        for (const LLUUID& id : ids)
        {
            Entry& e = sCache[id];
            e.asking = false;
            e.known = true;
            e.opted_out = out.count(id) > 0;
            e.answered = now;
        }
        LL_DEBUGS("WolfRecordPrivacy") << "looked up " << ids.size() << " avatars, " << out.size() << " opted out" << LL_ENDL;
    }

    bool recorderActive()
    {
        return WolfRecorder::isRecording() || LLFrameTimer::getElapsedSeconds() < sAwakeUntil;
    }

    /** Is this avatar to be left out of the video now? */
    bool hidden(const LLUUID& id, F64 now)
    {
        auto it = sCache.find(id);
        if (it == sCache.end()) return false;   // not wanted yet: the next scan adds it (at most a second)
        const Entry& e = it->second;
        if (e.known) return e.opted_out;
        return now - e.first_seen < kUnknownHiddenS;
    }

    /** The avatars around that the recorder may see: real residents, not this viewer's own, not dead. */
    template <typename F> void forEachOther(F f)
    {
        for (LLCharacter* c : LLCharacter::sInstances)
        {
            LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(c);
            if (!av || av->isDead() || av->isSelf() || av->isControlAvatar() || av->isUIAvatar()) continue;
            f(av);
        }
    }

    void scan(F64 now)
    {
        sNextScan = now + kScanEveryS;
        // only on a Wolf region (a visitor too), this region's own word (wolfgrid.h isOnWolfRegionConfirmed: a read
        // from a Wolf service for what is drawn); the answers already kept still count anywhere
        const bool ask = WolfGrid::isOnWolfRegionConfirmed();
        S32 count = 0;
        std::vector<LLUUID> want;
        forEachOther([&](LLVOAvatar* av) {
            const LLUUID& id = av->getID();
            if (!ask)
            {
                // off our regions nobody new is added (no unknown-means-hidden shapes on another grid)
                auto it = sCache.find(id);
                if (it != sCache.end() && it->second.known && it->second.opted_out) ++count;
                return;
            }
            Entry& e = sCache[id];
            if (e.first_seen == 0.0) e.first_seen = now;
            if (e.known && e.opted_out) ++count;
            if (!e.asking && (!e.known || now - e.answered >= kKeepAnswerS) && want.size() < kBatchMax)
            {
                want.push_back(id);
            }
        });
        sNearby = count;
        // forget the ones long gone (they are asked again if they come back)
        for (auto it = sCache.begin(); it != sCache.end();)
        {
            const Entry& e = it->second;
            if (!e.asking && now - std::max(e.first_seen, e.answered) > 2.0 * kKeepAnswerS) it = sCache.erase(it);
            else ++it;
        }
        if (want.empty() || sInFlight || now < sNextRequest) return;
        for (const LLUUID& id : want) sCache[id].asking = true;
        sInFlight = true;
        sNextRequest = now + kRequestGapS;
        LLCoros::instance().launch("WolfRecordPrivacyLookup", [want]() { lookupCoro(want); });
    }

    void ownStatusCoro(LLUUID session)
    {
        LLCore::HttpStatus status;
        LLSD reply = post("action=status", true, status);
        if (session != gAgentSessionID) return;   // a new login since: it asks for itself
        if (reply.isMap() && reply["success"].asBoolean())
        {
            sOwnOptedOut = reply["opted_out"].asBoolean();
            sOwn = WolfRecordPrivacy::Own::KNOWN;
            sOwnError.clear();
            return;
        }
        const int code = httpCode(status);
        // 401/403: not a Wolf Territories resident (a visitor's login, a closed account): nothing to choose here
        sOwn = (code == 401 || code == 403) ? WolfRecordPrivacy::Own::UNAVAILABLE : WolfRecordPrivacy::Own::LOADING;
        sOwnError = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                        : std::string("Your choice could not be read from the website just now.");
        if (sOwn == WolfRecordPrivacy::Own::LOADING)
        {
            sOwnAsked = false;   // asked again the next time it is needed, a minute from now at the soonest
            sOwnRetryAt = LLFrameTimer::getElapsedSeconds() + 60.0;
        }
        LL_WARNS("WolfRecordPrivacy") << "own status failed (" << status.toString() << ")" << LL_ENDL;
    }

    void ownSetCoro(LLUUID session, bool want)
    {
        LLCore::HttpStatus status;
        LLSD reply = post(std::string("action=set&opted_out=") + (want ? "1" : "0"), true, status);
        if (session != gAgentSessionID) return;
        sOwn = WolfRecordPrivacy::Own::KNOWN;
        if (reply.isMap() && reply["success"].asBoolean())
        {
            sOwnOptedOut = reply["opted_out"].asBoolean();
            sOwnError.clear();
            LL_INFOS("WolfRecordPrivacy") << "own choice saved: " << (sOwnOptedOut ? "left out of recordings" : "may be recorded")
                                          << LL_ENDL;
            return;
        }
        // the old choice stands
        sOwnError = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                        : std::string("Your choice could not be saved just now. Please try again in a minute.");
        LL_WARNS("WolfRecordPrivacy") << "own choice not saved (" << status.toString() << ")" << LL_ENDL;
    }

    void project(const glm::mat4& mvp, const LLVector4a* ext, const LLRect& view, bool& any, bool& behind,
                 F32& lx, F32& by, F32& rx, F32& ty)
    {
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec4 p(ext[(i & 1) ? 1 : 0][0], ext[(i & 2) ? 1 : 0][1], ext[(i & 4) ? 1 : 0][2], 1.f);
            const glm::vec4 c = mvp * p;
            if (c.w <= 0.01f)
            {
                behind = true;   // a corner at or behind the camera: the shape cannot be bounded on screen
                continue;
            }
            // Source: llviewercamera.cpp projectPosAgentToScreen - glm::project into the world view's raw viewport
            const F32 sx = (F32)view.mLeft + (c.x / c.w * 0.5f + 0.5f) * (F32)view.getWidth();
            const F32 sy = (F32)view.mBottom + (c.y / c.w * 0.5f + 0.5f) * (F32)view.getHeight();
            if (!any)
            {
                lx = rx = sx;
                by = ty = sy;
                any = true;
            }
            lx = std::min(lx, sx);
            rx = std::max(rx, sx);
            by = std::min(by, sy);
            ty = std::max(ty, sy);
        }
    }
}

namespace WolfRecordPrivacy
{
void idle()
{
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion()) return;
    if (gAgentSessionID != sOwnSession)
    {
        // a new login: the own choice is asked again; other people's answers from before still stand (same grid ids)
        sOwnSession = gAgentSessionID;
        sOwn = WolfRecordPrivacy::Own::UNAVAILABLE;
        sOwnAsked = false;
        sOwnRetryAt = 0.0;
        sOwnError.clear();
    }
    if (!recorderActive())
    {
        sNearby = 0;
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= sNextScan) scan(now);
}

void keepLookingUp()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= sAwakeUntil) sNextScan = 0.0;   // just opened: list the avatars at once
    sAwakeUntil = now + kAwakeS;
}

S32 nearbyCount()
{
    return sNearby;
}

bool lookupTrouble()
{
    return sFailingSince != 0.0 && LLFrameTimer::getElapsedSeconds() - sFailingSince >= kRetryAfterFailS;
}

void recordingMasks(bool name_tags, std::vector<LLRectf>& out)
{
    out.clear();
    if (sCache.empty() || !gViewerWindow) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // the world's own camera for this frame: pipeline.cpp copies gGLModelView / gGLProjection into gGLLastModelView /
    // gGLLastProjection at the end of the 3D scene render, before render_ui (where the recorder copies the picture)
    const glm::mat4 mvp = get_last_projection() * get_last_modelview();
    const LLRect view = gViewerWindow->getWorldViewRectRaw();
    const LLVector2& scale = gViewerWindow->getDisplayScale();
    forEachOther([&](LLVOAvatar* av) {
        if (!hidden(av->getID(), now)) return;
        // drawn this frame: llvoavatar.cpp LLVOAvatar::isVisible (the drawable on this frame's visible list - frustum and
        // occlusion culling - and not hidden by RenderAvatarFriendsOnly). Someone behind a wall needs no shape.
        if (av->isVisible() && av->mDrawable.notNull())
        {
            // the avatar and everything it wears: llvoavatar.cpp calculateSpatialExtents (attachments, rigged meshes)
            const LLVector4a* ext = av->mDrawable->getSpatialExtents();
            bool any = false, behind = false;
            F32 lx = 0.f, by = 0.f, rx = 0.f, ty = 0.f;
            project(mvp, ext, view, any, behind, lx, by, rx, ty);
            if (behind)
            {
                // the camera is inside or right against them: they fill the view
                out.emplace_back((F32)view.mLeft, (F32)view.mTop, (F32)view.mRight, (F32)view.mBottom);
            }
            else if (any && rx >= (F32)view.mLeft && lx <= (F32)view.mRight && ty >= (F32)view.mBottom && by <= (F32)view.mTop)
            {
                // a margin all round: the shape's rounded corners (wolfrecorder.cpp) must not uncover a hand or a hat
                const F32 m = 0.06f * std::max(rx - lx, ty - by) + 4.f;
                out.emplace_back(lx - m, ty + m, rx + m, by - m);
            }
        }
        // the name tag (and its chat bubble), where llhudnametag.cpp updateScreenPos put it this frame (UI pixels)
        if (name_tags && av->mNameText.notNull() && av->mNameText->getVisible() && !av->mNameText->getHidden())
        {
            const LLRectf& r = av->mNameText->getSoftScreenRect();
            if (r.getWidth() > 0.f && r.getHeight() > 0.f)
            {
                out.emplace_back(r.mLeft * scale.mV[VX] - 4.f, r.mTop * scale.mV[VY] + 4.f,
                                 r.mRight * scale.mV[VX] + 4.f, r.mBottom * scale.mV[VY] - 4.f);
            }
        }
    });
}

Own ownState()
{
    if (LLStartUp::getStartupState() < STATE_STARTED || !WolfGrid::isWolfTerritories()) return Own::UNAVAILABLE;
    if (!sOwnAsked && sOwn != Own::SAVING && LLFrameTimer::getElapsedSeconds() >= sOwnRetryAt)
    {
        sOwnAsked = true;
        sOwn = Own::LOADING;
        const LLUUID session = gAgentSessionID;
        LLCoros::instance().launch("WolfRecordPrivacyOwn", [session]() { ownStatusCoro(session); });
    }
    return sOwn;
}

bool ownOptedOut()
{
    return sOwnOptedOut;
}

std::string ownError()
{
    return sOwnError;
}

void setOwn(bool opted_out)
{
    if (ownState() != Own::KNOWN) return;
    sOwn = Own::SAVING;
    sOwnError.clear();
    const LLUUID session = gAgentSessionID;
    LLCoros::instance().launch("WolfRecordPrivacySet", [session, opted_out]() { ownSetCoro(session, opted_out); });
}
}
