/**
 * @file wolfscreenshare.cpp
 * @brief WolfViewer: share your screen onto a prim face (Wolf Territories only).
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

#include "wolfscreenshare.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llagentdata.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llmediaentry.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llselectmgr.h"
#include "lltextureentry.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llframetimer.h"
#include "llviewermediafocus.h"
#include "llvovolume.h"
#include "llvolume.h"
#include "llweb.h"
#include "wolfgrid.h"

// Source: wolfstorm/js/voice/screen_share.js ScreenShare.start() / stop().

namespace
{
    /**
     * Inside a coroutine: POST a JSON document and return the JSON reply as LLSD. `error` is
     * filled from the reply's "error" field when the server refused, or from the HTTP status.
     * Source: llcorehttputil.cpp trivialPostCoro (adapter + request + options), fsdata.cpp
     * (HTTP_RESULTS_RAW is the raw body as LLSD::Binary).
     */
    bool postJson(const std::string& url, const LLSD& body, LLSD& reply, std::string& error)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfScreenShare", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        options->setTimeout(20);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        const std::string json = boost::json::serialize(LlsdToJson(body));
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());   // intrusive pointer (fsprimfeedconnect.cpp:72)
        raw->append(json.data(), json.size());

        LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);
        LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

        reply = LLSD();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            std::string text(bytes.begin(), bytes.end());
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(text, ec);
            if (!ec)
            {
                reply = LlsdFromJson(v);
            }
        }
        if (!status)
        {
            error = reply.has("error") ? reply["error"].asString() : status.toString();
            return false;
        }
        if (!reply.has("success") || !reply["success"].asBoolean())
        {
            error = reply.has("error") ? reply["error"].asString() : "the screen-share service refused the request";
            return false;
        }
        return true;
    }
}

WolfScreenShare::WolfScreenShare()
{
}

WolfScreenShare::~WolfScreenShare()
{
}

// static
bool WolfScreenShare::isAvailable()
{
    return WolfGrid::isWolfTerritories() && gAgentID.notNull() && gAgentSessionID.notNull();
}

void WolfScreenShare::toggle()
{
    if (mSharing)
    {
        stop();
    }
    else
    {
        start();
    }
}

// Source: screen_share.js start(obj, faceIndex) and toolbar_manager.js 'screen-btn': a share
// needs a TARGET — the selected face — before anything else happens.
void WolfScreenShare::start()
{
    if (!isAvailable())
    {
        alert("Screen sharing is available on Wolf Territories only.");
        return;
    }
    if (mSharing || mMinting)
    {
        return;
    }
    Target target;
    std::string why;
    if (!findSelectedFace(target, why))
    {
        alert(why);
        return;
    }
    // crypto.randomUUID() with the dashes removed: 32 hex characters, inside screen_token.php's
    // [A-Za-z0-9_-]{8,64}.
    std::string share_id = LLUUID::generateNewID().asString();
    share_id.erase(std::remove(share_id.begin(), share_id.end(), '-'), share_id.end());

    mMinting = true;
    LLCoros::instance().launch("WolfScreenShare mint", [target, share_id]() { mintCoro(target, share_id); });
}

// Source: screen_share.js stop() — de-register the share server-side so the link stops
// working, then clear the face.
void WolfScreenShare::stop()
{
    mFocusUntil = 0.0;
    if (!mSharing)
    {
        return;
    }
    const std::string share_id = mShareId;
    const Target target = mTarget;
    mSharing = false;
    mShareId.clear();
    mRoom.clear();
    mUrl.clear();
    LLCoros::instance().launch("WolfScreenShare revoke", [share_id]() { revokeCoro(share_id); });
    // Media off the face: selectionSetMedia(0, LLSD()) removes it, the same call llpanelface
    // makes for "remove media" (llpanelface.cpp / fspanelface.cpp).
    applyFaceMedia(target, LLSD());
    tip("Screen share ended. Close the sharing tab in your browser if it is still open.");
}

// Source: llviewermediafocus.cpp setFocusFace() (what a click on a media face does: navigate
// if not yet loaded, focus) and LLPanelPrimMediaControls' zoom — setCameraZoom at ZOOM_MEDIUM
// padding 1.1 (panel_prim_media_controls.xml), the same numbers WolfStorm's
// zoomToMediaFace() transcribed. The face normal comes from the volume face in object space,
// rotated by the render rotation — the pick normal a click would have supplied.
void WolfScreenShare::idle()
{
    if (mFocusUntil <= 0.0 || !mSharing)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now > mFocusUntil)
    {
        mFocusUntil = 0.0;
        LL_WARNS("WolfScreenShare") << "the shared face never got a media impl; not focusing" << LL_ENDL;
        return;
    }
    LLViewerObject* objectp = gObjectList.findObject(mTarget.mObjectId);
    LLVOVolume* vo = objectp ? dynamic_cast<LLVOVolume*>(objectp) : nullptr;
    if (!vo || vo->isDead())
    {
        mFocusUntil = 0.0;
        return;
    }
    viewer_media_t impl = vo->getMediaImpl((U8)mTarget.mFace);
    if (impl.isNull())
    {
        return;   // not created yet, try next frame
    }
    LLVector3 normal(0.f, 0.f, 1.f);
    const LLVolume* volume = vo->getVolume();
    if (volume && mTarget.mFace < volume->getNumVolumeFaces())
    {
        const LLVolumeFace& vf = volume->getVolumeFace(mTarget.mFace);
        if (vf.mNormals && vf.mNumVertices > 0)
        {
            normal.set(vf.mNormals[0].getF32ptr());
            normal = normal * vo->getRenderRotation();
            normal.normalize();
        }
    }
    LLViewerMediaFocus::getInstance()->setFocusFace(objectp, mTarget.mFace, impl, normal);
    LLViewerMediaFocus::setCameraZoom(objectp, normal, 1.1f, false);
    mFocusUntil = 0.0;
    LL_INFOS("WolfScreenShare") << "focused and zoomed the shared face" << LL_ENDL;
}

// static
bool WolfScreenShare::findSelectedFace(Target& out, std::string& why)
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    S32 selected_faces = 0;
    for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* objectp = node ? node->getObject() : nullptr;
        if (!objectp)
        {
            continue;
        }
        const S32 num_tes = objectp->getNumTEs();
        for (S32 te = 0; te < num_tes; ++te)
        {
            if (!node->isTESelected(te))
            {
                continue;
            }
            ++selected_faces;
            if (selected_faces == 1)
            {
                if (!objectp->permModify())
                {
                    why = "You cannot modify that object, so the share cannot be put on it.";
                    return false;
                }
                out.mObjectId = objectp->getID();
                out.mFace = te;
            }
        }
    }
    if (selected_faces == 0)
    {
        why = "Select the face to share onto first: Build, switch to Select Face, and click the face.";
        return false;
    }
    if (selected_faces > 1)
    {
        why = "Select exactly ONE face to share onto (Build > Select Face). A whole object selects every face.";
        return false;
    }
    return true;
}

// Source: screen_share.js _mediaEntry(url) — key for key. The key names are LLMediaEntry's
// own (llmediaentry.cpp), which is what the ObjectMedia capability carries.
LLSD WolfScreenShare::mediaEntryFor(const std::string& url)
{
    LLSD m;
    m[LLMediaEntry::ALT_IMAGE_ENABLE_KEY] = false;
    m[LLMediaEntry::CONTROLS_KEY] = 0;
    m[LLMediaEntry::CURRENT_URL_KEY] = url;
    m[LLMediaEntry::HOME_URL_KEY] = url;
    m[LLMediaEntry::AUTO_LOOP_KEY] = false;
    m[LLMediaEntry::AUTO_PLAY_KEY] = true;
    m[LLMediaEntry::AUTO_SCALE_KEY] = true;
    m[LLMediaEntry::AUTO_ZOOM_KEY] = true;
    m[LLMediaEntry::FIRST_CLICK_INTERACT_KEY] = false;
    m[LLMediaEntry::WIDTH_PIXELS_KEY] = MAX_WIDTH;
    m[LLMediaEntry::HEIGHT_PIXELS_KEY] = MAX_HEIGHT;
    m[LLMediaEntry::WHITELIST_ENABLE_KEY] = false;
    m[LLMediaEntry::WHITELIST_KEY] = LLSD::emptyArray();
    m[LLMediaEntry::PERMS_INTERACT_KEY] = 7;
    m[LLMediaEntry::PERMS_CONTROL_KEY] = 7;
    return m;
}

// static
void WolfScreenShare::mintCoro(Target target, std::string share_id)
{
    // Source: screen_share.js start(): POST {token, share_id}; here the viewer's identity
    // fields instead of a token (screen_token.php, [WOLFVIEWER 2026-09-06]).
    LLSD body;
    body["agent_id"] = gAgentID.asString();
    body["session_id"] = gAgentSessionID.asString();
    body["share_id"] = share_id;
    LLSD reply;
    std::string error;
    const bool ok = postJson(WolfGrid::SHARE_TOKEN_URL, body, reply, error);
    WolfScreenShare::instance().onMinted(target, share_id, ok, reply, error);
}

// static
void WolfScreenShare::revokeCoro(std::string share_id)
{
    // Source: screen_share.js stop(): POST {action:'revoke', share_id, token}.
    LLSD body;
    body["action"] = "revoke";
    body["agent_id"] = gAgentID.asString();
    body["session_id"] = gAgentSessionID.asString();
    body["share_id"] = share_id;
    LLSD reply;
    std::string error;
    if (!postJson(WolfGrid::SHARE_TOKEN_URL, body, reply, error))
    {
        LL_WARNS("WolfScreenShare") << "revoke of " << share_id << " failed: " << error << LL_ENDL;
    }
}

void WolfScreenShare::onMinted(const Target& target, const std::string& share_id, bool ok, const LLSD& reply, const std::string& error)
{
    mMinting = false;
    if (!ok)
    {
        alert("Could not start the screen share: " + error);
        return;
    }
    const std::string url = reply["url"].asString();
    const std::string room = reply["room"].asString();
    const std::string publisher = reply.has("publisher_token") ? reply["publisher_token"].asString()
                                                               : reply["token"].asString();
    if (url.empty() || room.empty() || publisher.empty())
    {
        alert("Could not start the screen share: the service sent an incomplete reply.");
        return;
    }
    LLViewerObject* objectp = gObjectList.findObject(target.mObjectId);
    if (!objectp || objectp->isDead())
    {
        alert("Could not start the screen share: the selected object is gone.");
        return;
    }

    mSharing = true;
    mTarget = target;
    mShareId = share_id;
    mRoom = room;
    mUrl = url;

    // (3) the face: screen.php?r=<room>&t=<subscriber token>, via the ObjectMedia capability.
    applyFaceMedia(target, mediaEntryFor(url));

    // Play it and zoom to it in the sharer's own view, as WolfStorm does (screen_share.js
    // start() → renderManager.zoomToMediaFace) — done in idle() once the media impl exists.
    mFocusUntil = LLFrameTimer::getElapsedSeconds() + 10.0;

    // (1) the capture, in the user's browser: publish.php?r=<room>&t=<publisher token>.
    const std::string publish = std::string(WolfGrid::SHARE_PUBLISH_URL)
        + "?r=" + LLURI::escape(room) + "&t=" + LLURI::escape(publisher);
    LLWeb::loadURLExternal(publish);
    tip("Your browser has opened the screen-share page: choose what to share there. "
        "Press Share Screen again to stop.");
    LL_INFOS("WolfScreenShare") << "sharing " << share_id << " onto " << target.mObjectId
                                << " face " << target.mFace << LL_ENDL;
}

// Put a media entry on (or take it off) the remembered face. selectionSetMedia works on the
// current selection (llselectmgr.cpp: applyToTEs over the selected TEs, then sendTEUpdate and
// sendMediaDataUpdate per object), so the target face is selected on its own first — the
// user's own selection may have moved on while the mint was in flight, and a stale one could
// put the share on the wrong prim.
void WolfScreenShare::applyFaceMedia(const Target& target, const LLSD& media)
{
    LLViewerObject* objectp = gObjectList.findObject(target.mObjectId);
    if (!objectp || objectp->isDead() || target.mFace < 0)
    {
        return;
    }
    LLSelectMgr* mgr = LLSelectMgr::getInstance();
    mgr->deselectAll();
    mgr->selectObjectOnly(objectp, target.mFace);
    if (media.isMap())
    {
        mgr->selectionSetMedia(LLTextureEntry::MF_HAS_MEDIA, media);
    }
    else
    {
        mgr->selectionSetMedia(0, LLSD());
    }
    mgr->deselectAll();
}

// static
void WolfScreenShare::alert(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("GenericAlertOK", args);
}

// static
void WolfScreenShare::tip(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("SystemMessageTip", args);
}
