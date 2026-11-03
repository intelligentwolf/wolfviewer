/**
 * @file wolfvrdevice.cpp
 * @brief WolfViewer VR in the headset browser: device id check and browser name. See wolfvrdevice.h.
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

#include "wolfvrdevice.h"

namespace wolfvr
{
bool validDeviceId(const std::string& id)
{
    if (id.size() != 32) return false;
    for (char c : id)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string deviceLabelFromUserAgent(const std::string& ua)
{
    // Only a friendly name for the "Headset connected" notice, never used to decide anything (any page can send any user
    // agent). Quest Browser user agents name the browser and the model ("...; Quest 3) ... OculusBrowser/..."), so
    // the longer model names are tried first; anything not recognised gets a plain description.
    auto has = [&](const char* s) { return ua.find(s) != std::string::npos; };
    if (has("OculusBrowser") || has("Quest"))
    {
        for (const char* q : {"Quest 3S", "Quest 3", "Quest Pro", "Quest 2"})
        {
            if (has(q)) return std::string("Meta ") + q + " browser";
        }
        return "Meta Quest browser";
    }
    if (has("PicoBrowser") || has("Pico")) return "Pico browser";
    if (has("Android")) return "Android browser";
    if (has("Firefox")) return "Firefox";
    if (has("Chrome")) return "Chrome-type browser";
    if (has("Safari")) return "Safari";
    return "a web browser";
}
}
