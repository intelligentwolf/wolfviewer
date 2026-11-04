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

class LLVector3;

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
    /** [VR STEREO 2026-11-04] The main loop's display(): one pass, or one per eye when the headset is in VR 3D Mode
     *  with stereo on (llappviewer.cpp, in place of display()). */
    void displayFrame();
    /** [VR STEREO] True while drawing the left eye: render_ui stops after the world (the headset gets no UI there and
     *  the right eye's pass draws the desktop's). */
    bool uiSkipped();
    /** [VR FPS 2026-11-04] True while drawing the right eye of a stereo frame: what is the same for both eyes (geometry
     *  and texture updates, mirrors, impostors) has been done in the left eye's pass and is skipped. */
    bool secondEye();
    /** [VR SHARED CULL 2026-11-04] The right eye's pass reuses the left eye's cull and state sort (WolfVRShareCull). */
    bool shareCull();
    /** [VR SHADOWS 2026-11-04] The left eye keeps last frame's shadow maps this frame (WolfVRShadowEvery). */
    bool reuseShadowsThisFrame();
    /** [VR SIT 2026-11-04] The object (root) the headset's pointer was on in the last 8 s, for the bar's Sit. */
    bool aimedObject(LLUUID& id);
    /** [VR HUD LAYER 2026-11-04] End of render_ui: the UI layer, drawn again into its own transparent target, to the
     *  headset (VR 3D Mode, while the page shows it). */
    void captureLayer();
    /** [VR AUDIO 2026-11-04] The listener's axes from the newest head pose (VR 3D Mode); false when not in it. */
    bool listenerAxes(LLVector3& at, LLVector3& up);
    /** [VR 3D FIX 2026-11-04] While an eye is drawn in VR 3D Mode stereo: the scale and offset (clip space) that turn
     *  the camera's symmetric projection into that eye's own off-axis window (LLViewerCamera::setPerspective applies
     *  it as it applies its zoom tiles). False when not drawing an eye. */
    bool eyeCrop(float& ax, float& bx, float& ay, float& by);
    /** [VR DEPTH 2026-11-04] The near and far planes the eye's projection was just made with (setPerspective, when
     *  eyeCrop is on): the depth the headset is sent is turned back into metres with them. */
    void noteDepthRange(float znear, float zfar);
    /** [VR WORLD POINTER 2026-11-04] The headset's pointer was used in the last 2 s (the window then counts the mouse as
     *  inside it, whatever the PC's own mouse is doing: LLViewerWindow::updateMouseDelta). */
    bool pointerActive();
    /** Frees the GPU objects (stream switched off, or the viewer closing). Main thread, GL context current. */
    void release();
}

#endif // WOLF_VRSCREEN_H
