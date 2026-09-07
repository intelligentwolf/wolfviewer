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

    // The WolfStorm Rust proxy's HTTP API: POST /stt (wolfstorm js/ui/speech_to_text.js STT_URL)
    // and POST /tts (js/ui/text_to_speech.js TTS_URL), both on port 8080 of the host serving the
    // viewer; the primary host is wolfstorm.app (the same box runs the whisper and piper engines).
    const char* const SPEECH_API_BASE = "https://wolfstorm.app:8080";

    // Screen sharing: the token endpoint (wolfstorm js/voice/screen_share.js TOKEN_URL, host
    // SHARE_HOST = 'wolfstorm.app') and the publisher page (wolfstorm/publish.php).
    const char* const SHARE_TOKEN_URL = "https://wolfstorm.app/php/screen_token.php";
    const char* const SHARE_PUBLISH_URL = "https://wolfstorm.app/publish.php";
}

#endif // WOLF_GRID_H
