/**
 * @file wolfdjcapture.h
 * @brief WolfViewer Wolf DJ: capturing the microphone and other programs' audio into a
 *        WolfDJChannel. One implementation per platform:
 *          wolfdjcapture_linux.cpp - PulseAudio API (PipeWire's pulse server too), loaded at run time
 *          wolfdjcapture_win.cpp   - WASAPI (process loopback per program, Windows 10 build 20348+)
 *          wolfdjcapture_mac.mm    - AudioQueue (mic) + ScreenCaptureKit (programs, macOS 13+)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_DJCAPTURE_H
#define WOLF_DJCAPTURE_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

class WolfDJChannel;

// A program that is playing (or can play) sound. mId is opaque to the floater.
struct WolfDJApp
{
    std::string mId;
    std::string mName;
};

// The id of "Everything you hear" (the default output, as heard by the speakers).
#define WOLFDJ_EVERYTHING_ID "everything"

class WolfDJCaptureStream
{
public:
    virtual ~WolfDJCaptureStream() = default;
    // False once the source has gone (program closed, device unplugged); error() says why.
    virtual bool ok() const = 0;
    virtual std::string error() const = 0;
};

namespace WolfDJCapture
{
    // Programs that are playing sound now. False + why when per-program capture is not
    // available on this system (the list may still hold "everything").
    bool listApps(std::vector<WolfDJApp>& apps, std::string& why);

    // Opened streams push audio into ch until destroyed. Null + err on failure.
    std::unique_ptr<WolfDJCaptureStream> openMic(WolfDJChannel* ch, std::string& err);
    std::unique_ptr<WolfDJCaptureStream> openApp(const std::string& id, WolfDJChannel* ch, std::string& err);
}

#endif // WOLF_DJCAPTURE_H
