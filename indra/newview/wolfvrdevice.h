/**
 * @file wolfvrdevice.h
 * @brief WolfViewer VR in the headset browser: the headset page's own id and a plain name for its browser.
 *        No viewer headers: the self-test builds it on its own.
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

#ifndef WOLF_VRDEVICE_H
#define WOLF_VRDEVICE_H

// [VR SIMPLE 2026-11-03] Paul: "don't make it difficult". There is no pairing: while VR is on, a headset on the home
// network connects straight in (wolfvrpipe.cpp). The page still sends a random id of its own (localStorage), only so
// that a reloaded page takes over its own old connection instead of being told someone is already connected; and the
// viewer names the browser in its "Headset connected" notice.

#include <string>

namespace wolfvr
{
    /** 32 lower-case hex characters (what wolfvr.js makes with crypto.getRandomValues). */
    bool validDeviceId(const std::string& id);
    /** A short, plain name for the notice from the browser's user agent ("Meta Quest 3 browser", ...). */
    std::string deviceLabelFromUserAgent(const std::string& ua);
}

#endif // WOLF_VRDEVICE_H
