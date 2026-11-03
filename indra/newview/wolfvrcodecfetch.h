/**
 * @file wolfvrcodecfetch.h
 * @brief WolfViewer VR pipe: downloads Cisco's prebuilt OpenH264 library on the resident's computer, on first use.
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

#ifndef WOLF_VR_CODEC_FETCH_H
#define WOLF_VR_CODEC_FETCH_H

#include <functional>
#include <string>

namespace wolfvr
{
    // [VR OPENH264 2026-11-03] Cisco's patent licence covers only Cisco's own binary, "separately downloaded to an end
    // user's device" (openh264/BINARY_LICENSE.txt, condition 1), so the viewer never ships it: the resident presses
    // a button and this downloads it from Cisco (http://ciscobinary.openh264.org/, plain HTTP: the only way Cisco
    // publishes it), checks it against SHA-256 values pinned in wolfvrcodecfetch.cpp, unpacks the bzip2 file and
    // writes it into the folder the caller names (the viewer's user settings folder).

    /** True when Cisco publishes OpenH264 2.6.0 for this computer (Windows x64/ARM64, macOS ARM64/x64,
     *  Linux x64/ARM64). */
    bool openh264Available();

    /** The full path the library has (or will have) in `dir`: dir + "/" + Cisco's own file name, such as
     *  "openh264-2.6.0-win64.dll". Empty when openh264Available() is false. UTF-8. */
    std::string openh264File(const std::string& dir);

    /** True when openh264File(dir) exists and is byte-for-byte Cisco's file (its SHA-256 is the pinned one).
     *  Reads about 1-2 MB; call it off the UI thread if that matters. */
    bool openh264Installed(const std::string& dir);

    /** Downloads, checks and installs the library into `dir` (made if missing). Blocks for up to about two
     *  minutes. `message` says what happened in plain words, for the resident. */
    bool fetchOpenH264(const std::string& dir, std::string& message);

    /** fetchOpenH264 on its own thread. `done` is called ONCE, on that thread (not the UI thread), when it ends.
     *  A second call while one is running gets done(false, "...already downloading...") at once. */
    void fetchOpenH264Async(const std::string& dir, std::function<void(bool ok, std::string message)> done);

    /** The sentence Cisco's licence (condition 3) says must be shown where the resident controls the codec:
     *  "OpenH264 Video Codec provided by Cisco Systems, Inc." */
    const char* openh264AttributionText();

    /** The whole of Cisco's BINARY_LICENSE.txt (v1.0), which condition 4 says must be reproduced where licensing
     *  information is shown. UTF-8, as published at http://www.openh264.org/BINARY_LICENSE.txt. */
    const char* openh264BinaryLicenseText();
}

#endif // WOLF_VR_CODEC_FETCH_H
