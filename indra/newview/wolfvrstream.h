/**
 * @file wolfvrstream.h
 * @brief WolfViewer: VR in the headset browser - the viewer side of the LAN pipe (wolfvrpipe.h): the setting, the
 *        certificate kept in the user settings folder, logging, start and stop.
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

#ifndef WOLF_VRSTREAM_H
#define WOLF_VRSTREAM_H

#include <string>
#include <vector>

#include "wolfvrpipe.h"

// [VR PIPE 2026-11-03] Stage 1. WolfVRStreamEnabled (OFF by default; remembered once ticked, Paul 2026-11-03) starts the
// server on the home-network address; switching it off stops it at once and closes the headset's connection.
// [VR SIMPLE 2026-11-03] No code and no pairing (Paul: "don't make it difficult"): while it is on, a headset on the home
// network connects straight in, one at a time; the viewer shows "Headset connected / disconnected" as a tip.
namespace WolfVRStream
{
    /** Hooks the setting (once). Called from the floater; the setting can only be switched on from there. */
    void init();
    /** Stops and joins the server threads. LLAppViewer::cleanup. */
    void shutdown();

    bool running();
    wolfvr::Stats stats();
    std::string lanUrl();
    /** The port in use (the next free one when WolfVRStreamPort was taken); 0 when off. */
    uint16_t port();
    std::string usbUrl();
    /** "192.168.1.9:4333": what the resident types in the headset browser. Empty when off. */
    std::string shortAddress();

    // [VR BIG SCREEN 2026-11-03] WolfVRStreamMode "screen": the whole viewer window goes to the headset
    // (wolfvrscreen.cpp captures it and replays the headset's pointer and keys).
    /** True when streaming in big-screen mode and an allowed headset wants pictures. */
    bool bigScreenWanted();
    /** The picture size the stream was started with (0 x 0 when off). */
    void frameSize(U32& width, U32& height);
    /** Hands one picture (frameSize RGBA, top row first) to the stream; `rgba` gets back a buffer to reuse. */
    void submitFrame(std::vector<uint8_t>& rgba, double capture_ms);
    /** The headset's pointer and key events since the last call. */
    std::vector<wolfvr::InputEvent> takeInput();

    // [VR 3D 2026-11-03] WolfVRStreamMode "world" (VR 3D Mode): the world drawn from the headset's pose.
    /** True when streaming in 3D mode and an allowed headset wants pictures. */
    bool worldWanted();
    /** The newest head pose from the headset (valid=false when none). */
    wolfvr::HeadPose headPose();
    /** Hands one world picture (frameSize RGBA, top row first) drawn for `pose`; `rgba` gets back a buffer to reuse. */
    void submitWorldFrame(std::vector<uint8_t>& rgba, const wolfvr::HeadPose& pose, double capture_ms);
    /** Applies a mode the headset page asked for (its in-VR switch). Main thread, every frame. */
    void pumpModeRequest();
    /** Why the last start failed (empty after a good start). */
    std::string lastError();
    /** The address chosen and why (or why none could be). */
    wolfvr::LanAddress lanAddress();
}

#endif // WOLF_VRSTREAM_H
