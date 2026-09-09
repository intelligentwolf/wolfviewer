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
}

#endif // WOLF_GRID_H
