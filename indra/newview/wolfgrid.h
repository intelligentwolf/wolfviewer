/**
 * @file wolfgrid.h
 * @brief WolfViewer: is this session on Wolf Territories, and where are its own services?
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

#ifndef WOLF_GRID_H
#define WOLF_GRID_H

#include "llviewernetwork.h"

#include "llfloater.h"
#include "llpanel.h"

// The features that talk to Wolf Territories' OWN servers — speech to text, text to speech,
// screen sharing — are offered on Wolf Territories only. Every one of them checks here, so a
// user of this viewer on another grid never sees a button that would send their audio or
// their screen to a server that is not that grid's.
namespace WolfGrid
{
    // Source: app_settings/grids.xml — the shipped grid entry has key
    // "grid.wolfterritories.org:8002" and grid_login_id "wolfterritories"
    // (llviewernetwork.cpp:42 GRID_ID_VALUE = "grid_login_id"; LLGridManager::getGridId()
    // returns that value for the current grid, getGrid() the key).
    inline bool isWolfTerritories()
    {
        LLGridManager* gm = LLGridManager::getInstance();
        if (!gm)
        {
            return false;
        }
        if (gm->getGridId() == "wolfterritories")
        {
            return true;
        }
        return gm->getGrid().find("wolfterritories.org") != std::string::npos;
    }

    // The WolfStorm Rust proxy's HTTP API, on port 8080 of the host serving the viewer; the
    // primary host is wolfstorm.app (rust_proxy/src/main.rs:46-92 binds 8080 with TLS, and the
    // same box runs the whisper and piper engines).
    const char* const PROXY_API_BASE = "https://wolfstorm.app:8080";

    // Speech: POST /stt (wolfstorm js/ui/speech_to_text.js STT_URL) and POST /tts
    // (js/ui/text_to_speech.js TTS_URL).
    const char* const SPEECH_API_BASE = PROXY_API_BASE;

    // Model upload: POST /upload_mesh (wolfstorm js/ui/floaters/floater_mesh_upload.js PROXY;
    // handler rust_proxy/src/main.rs:1993 handle_upload_mesh). That endpoint writes the mesh
    // asset, the object asset and the inventory item straight into Wolf Territories' own ROBUST
    // services — GRID_ROBUST_BASE is hard-coded at main.rs:1638 — so it is meaningless, and
    // would be wrong, on any other grid. Every caller checks isWolfTerritories() first.
    const char* const MESH_API_BASE = PROXY_API_BASE;

    // The grid's Patreon, shown on the login screen. Same link WolfStorm uses
    // (wolfstorm/index.php:564).
    const char* const PATREON_URL = "https://www.patreon.com/15362110/join";

    // Screen sharing: the token endpoint (wolfstorm js/voice/screen_share.js TOKEN_URL, host
    // SHARE_HOST = 'wolfstorm.app') and the publisher page (wolfstorm/publish.php).
    const char* const SHARE_TOKEN_URL = "https://wolfstorm.app/php/screen_token.php";
    const char* const SHARE_PUBLISH_URL = "https://wolfstorm.app/publish.php";

    /**
     * Stop a floater being resized smaller than the panel it contains.
     *
     * [2026-09-11] WHY THIS EXISTS. floater_wolf_ai and floater_wolf_terrain_paint were split out
     * of the build floater and became resizable windows with min_height="320" — far below what
     * their contents need (414 and 404). Dragging either one smaller left the lower controls
     * PERFECTLY VISIBLE and completely dead to the mouse, because LLUI draws a child against the
     * SCREEN (llview.cpp:1310-1313 tests the ROOT's rect) but hit-tests it against its PARENT
     * (llview.cpp:799-806 `visibleEnabledAndContains`). A control below the shrunken panel's
     * bottom edge is therefore still painted and can never be clicked. It was reported as "the
     * Build button does nothing", and no amount of checking enabled state, commit callbacks or
     * overlapping widgets would ever have found it.
     *
     * Fixing it by editing min_height in the XML works exactly until the next layout change, so
     * the minimum is computed from the real contents instead: the bottom-most child decides it.
     * Only ever RAISES the limit, so a floater that is already generous keeps its own numbers.
     *
     * Not applied globally to LLFloater on purpose — a scrolling list is meant to shrink, and
     * forcing every floater to fit its contents would make some of them unusably large.
     */
    inline void fitFloaterToContents(LLPanel* panel, S32 margin = 16)
    {
        if (!panel) return;
        LLFloater* floater = panel->getParentByType<LLFloater>();
        if (!floater || !floater->isResizable()) return;

        S32 needed_w = 0;
        S32 needed_h = 0;
        for (const LLView* child : *panel->getChildList())
        {
            if (!child) continue;
            const LLRect& r = child->getRect();
            needed_w = llmax(needed_w, r.mRight);
            // Child rects are bottom-up; the panel's own height minus the child's bottom is how
            // far down the panel that child reaches.
            needed_h = llmax(needed_h, panel->getRect().getHeight() - r.mBottom);
        }
        if (needed_w <= 0 || needed_h <= 0) return;

        // The floater has to carry the panel plus its own chrome, so ask for the difference
        // between the two rather than assuming a header height.
        const S32 chrome_h = llmax(0, floater->getRect().getHeight() - panel->getRect().getHeight());
        const S32 chrome_w = llmax(0, floater->getRect().getWidth()  - panel->getRect().getWidth());

        const S32 min_w = needed_w + chrome_w + margin;
        const S32 min_h = needed_h + chrome_h + margin;

        LLRect cur = floater->getRect();
        floater->setResizeLimits(llmax(min_w, floater->getMinWidth()),
                                 llmax(min_h, floater->getMinHeight()));
        // A rect saved from before this guard existed can still be too small, so grow it now.
        if (cur.getWidth() < min_w || cur.getHeight() < min_h)
        {
            floater->reshape(llmax(cur.getWidth(), min_w), llmax(cur.getHeight(), min_h), true);
        }
    }
}

#endif // WOLF_GRID_H
