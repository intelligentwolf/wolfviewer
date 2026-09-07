/**
 * @file wolfscreenshare.h
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

#ifndef WOLF_SCREENSHARE_H
#define WOLF_SCREENSHARE_H

#include <string>

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

// Source: wolfstorm/js/voice/screen_share.js ScreenShare — the same share, the same media
// entry on the face, the same token endpoint, the same room on the same proxy.
//
// A share has three parts there: (1) the browser's own picker captures a screen or window,
// (2) php/screen_token.php mints a subscriber token bound to `screen:<id>` and a publisher
// token, (3) the prim face is pointed at screen.php?r=<room>&t=<token> through the ObjectMedia
// capability so everyone looking at it joins the room and sees the picture.
//
// A NATIVE viewer can do (2) and (3) but not (1): it has no display-capture API on any platform
// and its embedded browser (CEF via Dullahan) has no screen picker. So the capture happens in
// the user's default browser: this class mints, sets the face media, and opens
// wolfstorm/publish.php with the publisher token — a page that runs the very same ScreenRTC
// transport WolfStorm publishes with. Watching needs nothing new: media on a prim already
// plays in this viewer.
//
// IDENTITY. WolfStorm proves who it is with its voice token; the viewer has none, so
// screen_token.php accepts agent id + session id and checks the pair against the grid's own
// presence service. That check only succeeds for a Wolf Territories session, which is also
// what keeps the feature on this grid alone (see WolfGrid::isWolfTerritories for the UI gate).
class WolfScreenShare : public LLSingleton<WolfScreenShare>
{
    LLSINGLETON(WolfScreenShare);
    ~WolfScreenShare();

public:
    /** Offered on Wolf Territories only, once logged in. */
    static bool isAvailable();

    /** Every frame from LLAppViewer::idle(): focuses and zooms the shared face once its
     *  media has been created (LLViewerMedia creates it on the next update after the face
     *  is set; it does not NAVIGATE for a change we made ourselves until the server copy
     *  comes back — llviewermedia.cpp updateMedia, `!update_from_self` — so the sharer's own
     *  view stayed blank for up to a minute unless the face was clicked). */
    void idle();

    /** The toolbar / menu action: start with a face selected, stop when sharing. */
    void toggle();
    bool isSharing() const { return mSharing; }
    void start();
    void stop();

    // Source: screen_rtc.js MAX_WIDTH / MAX_HEIGHT — the media entry's size, as WolfStorm's.
    static constexpr S32 MAX_WIDTH = 1280;
    static constexpr S32 MAX_HEIGHT = 720;

private:
    struct Target
    {
        LLUUID mObjectId;
        S32    mFace = -1;
    };

    /** Exactly one selected face on a modifiable object, or a reason why not. */
    static bool findSelectedFace(Target& out, std::string& why);
    /** Source: screen_share.js _mediaEntry(url). */
    static LLSD mediaEntryFor(const std::string& url);
    /** Coroutine: mint the share, then hand the reply back on the main thread. */
    static void mintCoro(Target target, std::string share_id);
    static void revokeCoro(std::string share_id);
    void onMinted(const Target& target, const std::string& share_id, bool ok, const LLSD& reply, const std::string& error);
    void applyFaceMedia(const Target& target, const LLSD& media);
    static void alert(const std::string& message);
    static void tip(const std::string& message);

    bool        mSharing = false;
    bool        mMinting = false;
    Target      mTarget;
    std::string mShareId;
    std::string mRoom;
    std::string mUrl;
    /** While > 0: keep trying to focus + zoom the shared face until this time. */
    F64         mFocusUntil = 0.0;
};

#endif // WOLF_SCREENSHARE_H
