/**
 * @file wolfvrscreen.h
 * @brief WolfViewer VR in the headset browser, big-screen mode: the whole viewer window to the headset, and the
 *        headset's pointer and keys back into the viewer window.
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

#ifndef WOLF_VRSCREEN_H
#define WOLF_VRSCREEN_H

// [VR BIG SCREEN 2026-11-03] Paul: "I'm dying to go in-world with it". Before stereo: WolfViewer's normal window, UI
// and all, on a big curved screen in the headset, with the controllers as the mouse.
//  - captureFrame() runs at the end of each drawn frame, just before the buffers swap (llviewerdisplay.cpp swap()),
//    only while an allowed headset is connected: one GPU blit of the back buffer into a picture-sized texture
//    (scaled, flipped to top-row-first, letterboxed to keep the window's shape) and an asynchronous read into a pixel
//    buffer. The pixels are collected two frames later, when the GPU fence says they are there, so the viewer never
//    waits for the GPU. Cost on the viewer: one blit and one ~8 MB copy per frame.
//  - pumpInput() runs every frame from the idle loop and replays the headset's pointer, buttons, wheel and keys as
//    the same LLViewerWindow / LLKeyboard calls the real mouse and keyboard make.
namespace WolfVRScreen
{
    void captureFrame();
    /** [VR 3D 2026-11-03] VR 3D Mode: the camera follows the headset (display(), before culling) and the finished
     *  world, without the UI, goes to the headset (render_ui(), right after gPipeline.renderFinalize()). */
    void applyHeadCamera();
    void captureWorld();
    void pumpInput();
    /** Frees the GPU objects (stream switched off, or the viewer closing). Main thread, GL context current. */
    void release();
}

#endif // WOLF_VRSCREEN_H
