/**
 * @file wolfvrscreen.cpp
 * @brief WolfViewer VR in the headset browser, big-screen mode. See wolfvrscreen.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfvrscreen.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llrendertarget.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llkeyboard.h"
#include "llstring.h"
#include "lltimer.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llvoavatarself.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfvrstream.h"
#include "wolfvrui.h"
#include "pipeline.h"

#include <cmath>
#include <cstring>
#include <vector>

extern bool gSnapshot;       // llviewerdisplay.cpp:190
extern bool gCubeSnapshot;   // llviewerdisplay.cpp:191
extern bool gDisplaySwapBuffers;   // llviewerdisplay.cpp:138 - swap() shows the frame only when true
extern bool gDepthDirty;           // llviewerdisplay.cpp:186
void render_hud_attachments();     // llviewerdisplay.cpp:209 (defined there, not in its header)
void render_ui_2d();               // llviewerdisplay.cpp:211

namespace
{
    constexpr int kSlots = 3;   // pixel buffers in flight: read into one, collect the one from two frames ago

    struct Capture
    {
        GLuint fbo = 0, tex = 0;
        GLuint pbo[kSlots] = {0, 0, 0};
        GLsync fence[kSlots] = {nullptr, nullptr, nullptr};
        F64 issued[kSlots] = {0, 0, 0};
        wolfvr::HeadPose pose[kSlots];   // 3D mode: the head pose each picture was drawn for
        U32 w = 0, h = 0;          // picture size
        int next = 0;
        std::vector<uint8_t> buf;  // handed to the stream; comes back as the stream's spare buffer
        // where the window sits in the picture (letterbox), top-row-first picture pixels: [x0, x1) x [y0, y1)
        S32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        S32 win_w = 0, win_h = 0;
        F64 last = 0;
        bool world = false;        // the GL objects are set up for 3D mode (square window crop) or big screen
        bool eye0 = false;         // [VR STEREO] this frame's left eye is in the left half; the right eye reads it back
    };
    Capture gCap;

    // [VR MOVE 2026-11-03] Paul: "left controller must move him". The headset's left stick, held now, and jump/fly
    // up/down held, applied every frame like held movement keys (llviewerinput.cpp agent_push_forwardbackward:
    // gAgent.moveAt each frame while the key is down). A stick that has not been heard from for a second (the
    // headset slept or the network dropped) counts as let go, so the avatar never walks off on its own.
    struct Stick { F32 x = 0, y = 0; S32 lift = 0; F64 heard = 0; bool ran = false; };
    Stick gStick;
    constexpr F32 kDead = 0.25f;   // WolfStorm input_manager.js _vrLocomotionFlags DEAD
    constexpr F32 kRun = 0.9f;     // pushed (nearly) all the way: run, as double-tap-and-hold does

    // [VR 3D] the head pose the camera was turned to for this frame, and the view to give back afterwards
    wolfvr::HeadPose gFramePose;
    bool gCameraOverridden = false;
    F32 gSavedView = 0.f;

    // [VR STEREO 2026-11-04] Which eye display() is drawing: -1 one view (mono / big screen), 0 left, 1 right. Both
    // eyes are drawn from ONE head pose (taken for the left eye), each from its own position: the head centre the
    // agent camera placed (gEyeBase) plus that eye's offset from the headset, turned with the head.
    S32 gEye = -1;
    LLVector3 gEyeBase;
    bool gPaused = false;       // [VR LEAN] the desktop window is not drawn this frame (displayFrame)
    bool gDebugStats = false;   // [VR LEAN] WolfVRDebugStats, read once (the first frame)
    // frame-time numbers for the log (and Paul's report): display() time per frame, mono vs stereo
    struct FrameTimes { F64 sum = 0, eye0 = 0; U32 n = 0; F64 since = 0; bool stereo = false; };
    FrameTimes gTimes;

    // [VR HUD LAYER 2026-11-04] The viewer's UI layer (HUD attachments + the 2D UI: floaters, notices, toolbars) drawn a
    // second time, into its own transparent target, for the headset - the desktop's own frame is not touched. At most
    // 30 a second; read back asynchronously like the world picture (pixel buffers + fences, collected later).
    struct Layer
    {
        LLRenderTarget rt;
        GLuint pbo[kSlots] = {0, 0, 0};
        GLsync fence[kSlots] = {nullptr, nullptr, nullptr};
        F64 issued[kSlots] = {0, 0, 0};
        U32 ww = 0, wh = 0;              // the window size the target is made for
        U32 w = 0, h = 0;                // the picture size (the window, scaled to fit 1920 x 1080) the buffers are for
        GLuint sfbo = 0, stex = 0;       // the scaled copy that is read back
        int next = 0;
        F64 last = 0;
        F64 cost = 0, cost_n = 0, since = 0;   // draw time of the layer pass, for the log
        std::vector<uint8_t> buf;        // top row first, handed to the stream
        std::vector<uint8_t> alpha;      // the last collected layer's coverage, for "is there anything to click"
        U32 aw = 0, ah = 0;
    };
    Layer gLayer;
    bool gLayerDownIgnored[3] = {false, false, false};   // a press on an empty part of the layer, and its release

    void freeLayer()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (gLayer.fence[i]) glDeleteSync(gLayer.fence[i]);
            gLayer.fence[i] = nullptr;
        }
        if (gLayer.pbo[0]) glDeleteBuffers(kSlots, gLayer.pbo);
        for (GLuint& b : gLayer.pbo) b = 0;
        if (gLayer.sfbo) glDeleteFramebuffers(1, &gLayer.sfbo);
        if (gLayer.stex) glDeleteTextures(1, &gLayer.stex);
        gLayer.sfbo = gLayer.stex = 0;
        gLayer.rt.release();
        gLayer.w = gLayer.h = gLayer.ww = gLayer.wh = 0;
        gLayer.next = 0;
    }

    // Anything drawn near window pixel (x, y) (GL coordinates, origin bottom-left) in the last layer picture?
    bool layerHasPixel(S32 x, S32 y)
    {
        if (gLayer.alpha.empty() || !gLayer.aw || !gLayer.ah || !gLayer.ww || !gLayer.wh) return true;   // not known yet
        // window pixels -> the scaled picture's pixels
        x = (S32)((F64)x * gLayer.aw / gLayer.ww);
        y = (S32)((F64)y * gLayer.ah / gLayer.wh);
        const S32 row = (S32)gLayer.ah - 1 - y;                              // the buffer is top row first
        for (S32 dy = -3; dy <= 3; ++dy)
        {
            for (S32 dx = -3; dx <= 3; ++dx)
            {
                const S32 px = x + dx, py = row + dy;
                if (px < 0 || py < 0 || px >= (S32)gLayer.aw || py >= (S32)gLayer.ah) continue;
                if (gLayer.alpha[(size_t)py * gLayer.aw + px] >= 8) return true;
            }
        }
        return false;
    }

    void freeGl()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (gCap.fence[i]) glDeleteSync(gCap.fence[i]);
            gCap.fence[i] = nullptr;
        }
        if (gCap.pbo[0]) glDeleteBuffers(kSlots, gCap.pbo);
        if (gCap.fbo) glDeleteFramebuffers(1, &gCap.fbo);
        if (gCap.tex) glDeleteTextures(1, &gCap.tex);
        gCap = Capture();
    }

    bool makeGl(U32 w, U32 h)
    {
        freeGl();
        glGenTextures(1, &gCap.tex);
        glBindTexture(GL_TEXTURE_2D, gCap.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &gCap.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, gCap.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gCap.tex, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGenBuffers(kSlots, gCap.pbo);
        for (int i = 0; i < kSlots; ++i)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (!ok)
        {
            LL_WARNS("WolfVR") << "big screen: could not make the capture framebuffer" << LL_ENDL;
            freeGl();
            return false;
        }
        gCap.w = w;
        gCap.h = h;
        return true;
    }

    // The window, fitted into the picture keeping its shape (centred, black bars).
    S32 gSrc[4] = {0, 0, 0, 0};   // the part of the window that is copied (GL pixels)
    void fitWindow(S32 ww, S32 wh)
    {
        if (gCap.world)
        {
            // 3D mode: the camera's vertical field of view spans the WORLD view (the window less the menu bar and
            // anything docked: LLViewerWindow::getWorldViewRectRaw, what the 3D projection is set up for), so its
            // centre square is exactly the square 100-degree view. Seen 2026-11-03: the whole-window square had the
            // black menu-bar strip along its top.
            const LLRect wr = gViewerWindow->getWorldViewRectRaw();
            const S32 side = llmax(1, llmin(wr.getWidth(), wr.getHeight()));
            gSrc[0] = wr.mLeft + (wr.getWidth() - side) / 2;
            gSrc[1] = wr.mBottom + (wr.getHeight() - side) / 2;
            gSrc[2] = gSrc[0] + side;
            gSrc[3] = gSrc[1] + side;
            gCap.win_w = ww;
            gCap.win_h = wh;
            gCap.x0 = gCap.y0 = 0;
            gCap.x1 = (S32)gCap.w;
            gCap.y1 = (S32)gCap.h;
            if (gEye >= 0)
            {
                // [VR STEREO] the eye's own square half of the side-by-side picture (left half = left eye)
                gCap.x0 = gEye * (S32)gCap.h;
                gCap.x1 = gCap.x0 + (S32)gCap.h;
            }
            return;
        }
        gSrc[0] = gSrc[1] = 0;
        gSrc[2] = ww;
        gSrc[3] = wh;
        gCap.win_w = ww;
        gCap.win_h = wh;
        const F64 sx = (F64)gCap.w / ww, sy = (F64)gCap.h / wh, s = llmin(sx, sy);
        const S32 fw = (S32)(ww * s + 0.5), fh = (S32)(wh * s + 0.5);
        gCap.x0 = ((S32)gCap.w - fw) / 2;
        gCap.y0 = ((S32)gCap.h - fh) / 2;
        gCap.x1 = gCap.x0 + fw;
        gCap.y1 = gCap.y0 + fh;
    }

    // DOM KeyboardEvent.key names (https://developer.mozilla.org/docs/Web/API/UI_Events/Keyboard_event_key_values)
    // to the viewer's own key codes (indra_constants.h). Printable characters go as typed characters instead.
    KEY domKey(const std::string& k)
    {
        static const std::pair<const char*, KEY> kMap[] = {
            {"Enter", KEY_RETURN}, {"Backspace", KEY_BACKSPACE}, {"Tab", KEY_TAB}, {"Escape", KEY_ESCAPE},
            {"ArrowLeft", KEY_LEFT}, {"ArrowRight", KEY_RIGHT}, {"ArrowUp", KEY_UP}, {"ArrowDown", KEY_DOWN},
            {"Delete", KEY_DELETE}, {"Home", KEY_HOME}, {"End", KEY_END}, {"PageUp", KEY_PAGE_UP},
            {"PageDown", KEY_PAGE_DOWN}, {"Insert", KEY_INSERT},
        };
        for (const auto& [name, key] : kMap)
        {
            if (k == name) return key;
        }
        return KEY_NONE;
    }

    MASK maskOf(U32 mods)
    {
        MASK m = MASK_NONE;
        if (mods & 1) m |= MASK_SHIFT;
        if (mods & 2) m |= MASK_CONTROL;
        if (mods & 4) m |= MASK_ALT;
        return m;
    }

    // A point on the picture (0..1 from its top-left) to the viewer window (GL coordinates, origin bottom-left).
    LLCoordGL toWindow(F32 u, F32 v)
    {
        const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
        F64 fx = 0.5, fy = 0.5;
        if (gCap.x1 > gCap.x0 && gCap.y1 > gCap.y0)
        {
            fx = (u * gCap.w - gCap.x0) / (F64)(gCap.x1 - gCap.x0);
            fy = (v * gCap.h - gCap.y0) / (F64)(gCap.y1 - gCap.y0);
        }
        fx = llclamp(fx, 0.0, 1.0);
        fy = llclamp(fy, 0.0, 1.0);
        const S32 x = llclamp((S32)(fx * ww), 0, ww - 1);
        const S32 y = llclamp((S32)((1.0 - fy) * wh), 0, wh - 1);
        return LLCoordGL(x, y);
    }
}

namespace WolfVRScreen
{
void release()
{
    freeGl();
    freeLayer();
}

// [VR HUD LAYER 2026-11-04] End of render_ui (the right eye's / the only pass): draw the UI layer again into our own
// target, transparent where there is no UI, and send it. Everything the desktop shows over the world is in it - HUD
// attachments (ship helms, flight decks, game HUDs, residents' own), floaters, notices - so every HUD works in the
// headset with no code of its own.
void captureLayer()
{
    if (gSnapshot || gCubeSnapshot || gEye == 0 || !gViewerWindow) return;
    if (!WolfVRStream::layerWanted())
    {
        if (gLayer.w) freeLayer();
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    // collect the oldest finished read first (every frame, so the fences never pile up)
    const int old = (gLayer.next + 1) % kSlots;
    if (gLayer.fence[old] && glClientWaitSync(gLayer.fence[old], 0, 0) != GL_TIMEOUT_EXPIRED)
    {
        glDeleteSync(gLayer.fence[old]);
        gLayer.fence[old] = nullptr;
        GLint pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[old]);
        const size_t row = (size_t)gLayer.w * 4, bytes = row * gLayer.h;
        if (const uint8_t* p = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
        {
            gLayer.buf.resize(bytes);
            gLayer.alpha.resize((size_t)gLayer.w * gLayer.h);
            for (U32 y = 0; y < gLayer.h; ++y)   // GL rows are bottom first; the stream wants top first
            {
                const uint8_t* src = p + (size_t)(gLayer.h - 1 - y) * row;
                std::memcpy(&gLayer.buf[(size_t)y * row], src, row);
                uint8_t* a = &gLayer.alpha[(size_t)y * gLayer.w];
                for (U32 x = 0; x < gLayer.w; ++x) a[x] = src[4 * x + 3];
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            gLayer.aw = gLayer.w;
            gLayer.ah = gLayer.h;
            WolfVRStream::submitLayer(gLayer.buf, gLayer.w, gLayer.h, (now - gLayer.issued[old]) * 1000.0);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    }
    // 30 a second while something on it moves; 8 a second while it stands still (a change is then seen within 1/8 s,
    // and the rate goes back up at once) - the UI drawn a second time costs the viewer about 2 ms each time
    if (now - gLayer.last < (WolfVRStream::layerIdle() ? 1.0 / 8.0 : 1.0 / 30.0)) return;
    gLayer.last = now;

    const U32 ww = (U32)gViewerWindow->getWindowWidthRaw(), wh = (U32)gViewerWindow->getWindowHeightRaw();
    if (ww < 16 || wh < 16) return;
    if (ww != gLayer.ww || wh != gLayer.wh || !gLayer.rt.isComplete())
    {
        freeLayer();
        if (!gLayer.rt.allocate(ww, wh, GL_RGBA8, true))
        {
            LL_WARNS("WolfVR") << "UI layer: could not make the target " << ww << "x" << wh << LL_ENDL;
            return;
        }
        // The picture: the window scaled to fit 1920 x 1080 (multiples of 16, H.264 macroblocks). Stacked with its
        // coverage it is at most 1920 x 2160, which every headset decoder we name takes (4K-class) beside the world
        // picture; a bigger desktop window is sharper on the PC, not in the headset.
        const F64 sc = llmin(1.0, llmin(1920.0 / ww, 1080.0 / wh));
        const U32 lw = llmax(16u, (U32)(ww * sc) / 16 * 16), lh = llmax(16u, (U32)(wh * sc) / 16 * 16);
        glGenTextures(1, &gLayer.stex);
        glBindTexture(GL_TEXTURE_2D, gLayer.stex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        GLint prev = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glGenFramebuffers(1, &gLayer.sfbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gLayer.sfbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gLayer.stex, 0);
        const bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
        if (!ok)
        {
            LL_WARNS("WolfVR") << "UI layer: could not make the scaled copy " << lw << "x" << lh << LL_ENDL;
            freeLayer();
            return;
        }
        glGenBuffers(kSlots, gLayer.pbo);
        for (int i = 0; i < kSlots; ++i)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)lw * lh * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gLayer.ww = ww;
        gLayer.wh = wh;
        gLayer.w = lw;
        gLayer.h = lh;
    }

    const F64 t0 = LLTimer::getTotalSeconds();
    gLayer.rt.bindTarget();
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    gLayer.rt.clear();
    render_hud_attachments();
    {
        LLGLSDefault gls_default;
        LLGLSUIDefault gls_ui;
        gPipeline.disableLights();
        render_ui_2d();
    }
    gGL.flush();
    // scale it down into the copy that is read (still bottom row first)
    GLint read_fb = 0, draw_fb = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gLayer.rt.getFBO());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gLayer.sfbo);
    glBlitFramebuffer(0, 0, (GLint)gLayer.ww, (GLint)gLayer.wh, 0, 0, (GLint)gLayer.w, (GLint)gLayer.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gLayer.sfbo);
    // the read, without waiting: into the slot after the one just collected
    const int slot = gLayer.next;
    gLayer.next = (gLayer.next + 1) % kSlots;
    if (gLayer.fence[slot]) { glDeleteSync(gLayer.fence[slot]); gLayer.fence[slot] = nullptr; }
    GLint pack = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)gLayer.w, (GLsizei)gLayer.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gLayer.fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gLayer.issued[slot] = now;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    gLayer.rt.flush();
    gGL.setColorMask(true, false);

    if (!gDebugStats) return;   // [VR LEAN] no measuring unless asked for
    gLayer.cost += LLTimer::getTotalSeconds() - t0;
    gLayer.cost_n++;
    if (!gLayer.since) gLayer.since = now;
    if (now - gLayer.since >= 10.0)
    {
        LL_INFOS("WolfVR") << "UI layer: " << gLayer.w << "x" << gLayer.h << ", " << gLayer.cost_n / (now - gLayer.since)
                           << " passes/s, " << gLayer.cost / llmax(1.0, gLayer.cost_n) * 1000.0 << " ms each (CPU)" << LL_ENDL;
        gLayer.cost = gLayer.cost_n = 0;
        gLayer.since = now;
    }
}

namespace
{
// Big screen: the whole window, letterboxed. 3D: the centre square, drawn for gFramePose.
void captureInto(bool world)
{
    if (!gViewerWindow) return;
    if (world ? !WolfVRStream::worldWanted() : !WolfVRStream::bigScreenWanted()) return;
    U32 w = 0, h = 0;
    WolfVRStream::frameSize(w, h);
    if (!w || !h) return;
    // [VR STEREO] The right eye only finishes a picture whose left half was drawn this frame.
    const bool stereo = world && gEye >= 0;
    if (stereo && w != 2 * h) return;   // the stream is switching size: wait for the new picture size
    if (stereo && gEye == 1 && !gCap.eye0) return;
    // At most 75 pictures a second (the stream is 60): a viewer running faster does not need to copy every frame.
    const F64 now = LLTimer::getTotalSeconds();
    if (!(stereo && gEye == 1))
    {
        if (now - gCap.last < 1.0 / 75.0) return;
        gCap.last = now;
    }
    if (w != gCap.w || h != gCap.h || world != gCap.world)
    {
        if (!makeGl(w, h)) return;
        gCap.world = world;
        gCap.last = now;
    }
    const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
    if (ww <= 0 || wh <= 0) return;
    const bool refit = ww != gCap.win_w || wh != gCap.win_h;
    if (refit || world) fitWindow(ww, wh);   // 3D: the world view can change without the window changing (docking)

    // Keep the viewer's GL state as we found it.
    GLint read_fb = 0, draw_fb = 0, pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);       // copy the pixels as they are on screen
    if (scissor) glDisable(GL_SCISSOR_TEST);

    // 1. back buffer -> picture texture, scaled, upside down (so the read below comes out top row first)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCap.fbo);
    if (refit)
    {
        GLfloat cc[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);   // the letterbox bars
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
    }
    glBlitFramebuffer(gSrc[0], gSrc[1], gSrc[2], gSrc[3], gCap.x0, gCap.y1, gCap.x1, gCap.y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (stereo && gEye == 0)
    {
        // [VR STEREO] left half done; the right eye's pass reads the whole picture back
        gCap.eye0 = true;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
        if (read_fb == 0) glReadBuffer(GL_BACK);
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        return;
    }
    gCap.eye0 = false;

    // 2. picture texture -> pixel buffer, without waiting
    const int slot = gCap.next;
    gCap.next = (gCap.next + 1) % kSlots;
    if (gCap.fence[slot])
    {
        // Still not collected after kSlots frames: the GPU is behind. Drop the old one rather than wait for it.
        glDeleteSync(gCap.fence[slot]);
        gCap.fence[slot] = nullptr;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gCap.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)gCap.w, (GLsizei)gCap.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gCap.fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gCap.issued[slot] = now;
    gCap.pose[slot] = gFramePose;

    // 3. collect the oldest picture whose copy has finished
    const int old = gCap.next;   // the slot written kSlots-1 frames ago
    if (gCap.fence[old] && glClientWaitSync(gCap.fence[old], 0, 0) != GL_TIMEOUT_EXPIRED)
    {
        glDeleteSync(gCap.fence[old]);
        gCap.fence[old] = nullptr;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[old]);
        const size_t bytes = (size_t)gCap.w * gCap.h * 4;
        if (const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
        {
            gCap.buf.resize(bytes);
            std::memcpy(gCap.buf.data(), p, bytes);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            const F64 ms = (LLTimer::getTotalSeconds() - gCap.issued[old]) * 1000.0;
            if (world) WolfVRStream::submitWorldFrame(gCap.buf, gCap.pose[old], ms);
            else WolfVRStream::submitFrame(gCap.buf, ms);
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (read_fb == 0) glReadBuffer(GL_BACK);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

}

void applyEvents(const std::vector<wolfvr::InputEvent>& events);

// [VR LEAN 2026-11-04] Paul: "If you need to pause the main viewer's screen while in VR you can do that". In VR 3D Mode
// (WolfVRPauseDesktop, on by default) the desktop window is not drawn or shown: no UI pass, no swap, except one
// ordinary frame every few seconds so the window is never a dead, frozen one. Big Screen Mode always draws it (it IS
// the stream). gPaused is decided once per frame in displayFrame.
bool uiSkipped() { return gEye == 0 || gPaused; }

// [VR STEREO 2026-11-04] The frame, mono or one pass per eye. The left eye is a whole display() whose picture goes only
// to the headset (swap() does not show it: gDisplaySwapBuffers false, the way a tiled snapshot renders, and render_ui
// stops after the world, uiSkipped); the right eye is the ordinary frame that is shown on the desktop. Each eye is a
// full render - culling, shadows, sky, water, glow - from its own position, so all of it is right for that eye.
void displayFrame()
{
    static bool s_read = false;
    if (!s_read) { s_read = true; gDebugStats = gSavedSettings.getBOOL("WolfVRDebugStats"); }
    static LLCachedControl<bool> pause_desktop(gSavedSettings, "WolfVRPauseDesktop", true);
    U32 w = 0, h = 0;
    WolfVRStream::frameSize(w, h);
    const bool world = LLStartUp::getStartupState() == STATE_STARTED && !gDisconnected && WolfVRStream::worldWanted() &&
                       WolfVRStream::headPose().valid;
    const bool stereo = world && w && w == 2 * h;
    // the desktop: paused in 3D Mode, with one real frame every 3 s; and while paused nothing waits for the screen's
    // refresh any more, so the frames are paced here to the stream's 60 a second rather than run flat out
    static F64 s_last_shown = 0, s_last_frame = 0;
    const F64 now0 = LLTimer::getTotalSeconds();
    const bool show_now = now0 - s_last_shown >= 3.0;
    gPaused = world && pause_desktop && !show_now;
    if (!gPaused) s_last_shown = now0;
    if (gPaused)
    {
        const F64 wait = 1.0 / 60.0 - (now0 - s_last_frame);
        if (wait > 0.0005) ms_sleep((U32)(wait * 1000.0));
    }
    s_last_frame = LLTimer::getTotalSeconds();
    const F64 t0 = s_last_frame;
    F64 t1 = t0;
    if (!stereo)
    {
        if (gPaused) gDisplaySwapBuffers = false;   // not shown; swap() sets it back
        display();
        gDisplaySwapBuffers = true;
    }
    else
    {
        // the head centre the agent camera placed this frame (LLAgentCamera::updateCamera, idle), taken here so an
        // early return in the left eye's display() can never leave a stale one for the right eye
        gEyeBase = LLViewerCamera::getInstance()->getOrigin();
        gEye = 0;
        gDisplaySwapBuffers = false;
        display();
        t1 = LLTimer::getTotalSeconds();
        gEye = 1;
        // set again (an early return in the left eye's display() skips swap()); paused: the right eye is not shown either
        gDisplaySwapBuffers = !gPaused;
        gDepthDirty = true;           // the depth buffer is the other eye's; do not trust it for occlusion
        display();
        gEye = -1;
        gDisplaySwapBuffers = true;
        LLViewerCamera::getInstance()->setOrigin(gEyeBase);   // the head centre again for the rest of the frame
    }
    const bool was_paused = gPaused;
    gPaused = false;
    if (!gDebugStats) return;   // [VR LEAN] no measuring unless asked for
    // Numbers every 10 s while the headset is in 3D Mode: the cost of the frame, for comparing mono and stereo.
    const F64 t2 = LLTimer::getTotalSeconds();
    if (!WolfVRStream::worldWanted()) { gTimes = FrameTimes(); return; }
    if (gTimes.n && gTimes.stereo != stereo) gTimes = FrameTimes();
    if (!gTimes.n) gTimes.since = t0;
    gTimes.stereo = stereo;
    gTimes.sum += t2 - t0;
    gTimes.eye0 += t1 - t0;
    gTimes.n++;
    if (t2 - gTimes.since >= 10.0)
    {
        const F64 span = t2 - gTimes.since;
        LL_INFOS("WolfVR") << "VR 3D " << (stereo ? "stereo" : "mono") << ": " << gTimes.n / span << " frames/s, display "
                           << gTimes.sum / gTimes.n * 1000.0 << " ms/frame"
                           << (stereo ? " (left eye " + llformat("%.2f", gTimes.eye0 / gTimes.n * 1000.0) + " ms)" : std::string())
                           << ", picture " << w << "x" << h << (was_paused ? ", desktop paused" : ", desktop drawn") << LL_ENDL;
        gTimes = FrameTimes();
    }
}

void captureFrame() { captureInto(false); }

void captureWorld()
{
    if (gSnapshot || gCubeSnapshot) return;   // a snapshot or reflection-probe pass is not the headset's view
    captureInto(true);
}

// [VR 3D 2026-11-03] Turn this frame's camera to the headset's pose. The agent camera has already placed it
// (LLAgentCamera::updateCamera, idle); this keeps its position, takes the avatar's heading (yaw only) and adds the
// head's own rotation, so turning the avatar still turns the view and the head looks around in it.
// WebXR space is right-handed, +Y up, -Z forward (wolfvrpipe_video.h Quat); the viewer's is X forward (at), Y left,
// Z up (llcoordframe.h). A vector (x, y, z) in head space is therefore -z forward, -x left, +y up.
void applyHeadCamera()
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const bool want = !gSnapshot && !gCubeSnapshot && WolfVRStream::worldWanted();
    // [VR STEREO] the right eye uses the left eye's head pose, so the two halves of one picture always match
    wolfvr::HeadPose hp = gEye == 1 ? gFramePose : want ? WolfVRStream::headPose() : wolfvr::HeadPose();
    if (!want) hp.valid = false;
    if (!hp.valid)
    {
        if (gCameraOverridden)
        {
            cam->setViewNoBroadcast(gSavedView);
            gCameraOverridden = false;
        }
        gFramePose = wolfvr::HeadPose();
        return;
    }
    if (!gCameraOverridden)
    {
        gSavedView = cam->getView();
        gCameraOverridden = true;
    }
    // The heading is the avatar's, not the camera's: if the agent camera ever skipped a frame, reading back our own
    // turned camera would add the head's yaw again every frame.
    LLVector3 fwd = gAgent.getAtAxis();
    fwd.mV[VZ] = 0.f;
    if (fwd.normVec() < 1e-3f) fwd = LLVector3::x_axis;
    const LLVector3 up = LLVector3::z_axis;
    const LLVector3 left = up % fwd;
    // rotate v by the unit quaternion q: v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
    const F32 qx = hp.q[0], qy = hp.q[1], qz = hp.q[2], qw = hp.q[3];
    auto rot = [&](F32 vx, F32 vy, F32 vz) {
        const F32 cx = qy * vz - qz * vy, cy = qz * vx - qx * vz, cz = qx * vy - qy * vx;
        const F32 ccx = qy * cz - qz * cy, ccy = qz * cx - qx * cz, ccz = qx * cy - qy * cx;
        const F32 rx = vx + 2.f * (qw * cx + ccx), ry = vy + 2.f * (qw * cy + ccy), rz = vz + 2.f * (qw * cz + ccz);
        return fwd * (-rz) + left * (-rx) + up * ry;
    };
    LLVector3 at = rot(0.f, 0.f, -1.f), lft = rot(-1.f, 0.f, 0.f), u = rot(0.f, 1.f, 0.f);
    at.normVec(); lft.normVec(); u.normVec();
    cam->setAxes(at, lft, u);
    // The picture is square and 100 degrees across both ways (or what the headset measured covers both its eyes'
    // frusta, wolfvr.js eyeFov); the viewer's FOV is vertical.
    const F32 fov = hp.fov_deg > 0.f ? hp.fov_deg : 100.f;
    cam->setViewNoBroadcast(fov * DEG_TO_RAD);
    hp.fov_deg = fov;
    hp.stereo = gEye >= 0;
    if (gEye >= 0)
    {
        // [VR STEREO 2026-11-04] this eye's own position: the head centre plus the eye's offset in head space, turned
        // with the head (the same WebXR -> viewer mapping as the axes above). Axes stay parallel: WebXR gives both
        // eyes the head's orientation on Quest, and the page shows each half from its own eye.
        const F32* e = hp.eye + 3 * gEye;
        cam->setOrigin(gEyeBase + rot(e[0], e[1], e[2]));
    }
    gFramePose = hp;
}

// [VR AUDIO 2026-11-04] The ears follow the head: audio_update_listener (llvieweraudio.cpp, idle, before display() turns
// the camera to the head) asks here for the newest head pose's axes, the same mapping as applyHeadCamera, so a sound
// stays where it is in the world when the head turns.
bool listenerAxes(LLVector3& at, LLVector3& up)
{
    if (gSnapshot || gCubeSnapshot || !WolfVRStream::worldWanted()) return false;
    const wolfvr::HeadPose hp = WolfVRStream::headPose();
    if (!hp.valid) return false;
    LLVector3 fwd = gAgent.getAtAxis();
    fwd.mV[VZ] = 0.f;
    if (fwd.normVec() < 1e-3f) fwd = LLVector3::x_axis;
    const LLVector3 zup = LLVector3::z_axis;
    const LLVector3 left = zup % fwd;
    const F32 qx = hp.q[0], qy = hp.q[1], qz = hp.q[2], qw = hp.q[3];
    auto rot = [&](F32 vx, F32 vy, F32 vz) {
        const F32 cx = qy * vz - qz * vy, cy = qz * vx - qx * vz, cz = qx * vy - qy * vx;
        const F32 ccx = qy * cz - qz * cy, ccy = qz * cx - qx * cz, ccz = qx * cy - qy * cx;
        const F32 rx = vx + 2.f * (qw * cx + ccx), ry = vy + 2.f * (qw * cy + ccy), rz = vz + 2.f * (qw * cz + ccz);
        return fwd * (-rz) + left * (-rx) + zup * ry;
    };
    at = rot(0.f, 0.f, -1.f);
    up = rot(0.f, 1.f, 0.f);
    at.normVec();
    up.normVec();
    return true;
}

void pumpInput()
{
    WolfVRStream::pumpModeRequest();
    if (!gViewerWindow) return;
    const std::vector<wolfvr::InputEvent> events = WolfVRStream::takeInput();
    applyEvents(events);
    // the held stick, every frame
    const F64 now = LLTimer::getTotalSeconds();
    if (now - gStick.heard > 1.0) { gStick.x = gStick.y = 0.f; gStick.lift = 0; }
    const bool moving = std::fabs(gStick.x) > kDead || std::fabs(gStick.y) > kDead;
    if (!gAgent.isMovementLocked() && isAgentAvatarValid())
    {
        // stick y is negative pushed forward (xr-standard); moveLeft(+1) = left, stick x is positive to the right
        if (gStick.y < -kDead) gAgent.moveAt(1);
        else if (gStick.y > kDead) gAgent.moveAt(-1);
        if (gStick.x < -kDead) gAgent.moveLeft(1);
        else if (gStick.x > kDead) gAgent.moveLeft(-1);
        if (gStick.lift) gAgent.moveUp(gStick.lift);
    }
    const bool run = moving && llmax(std::fabs(gStick.x), std::fabs(gStick.y)) > kRun;
    if (run && !gStick.ran) { gAgent.setTempRun(); gStick.ran = true; }
    else if (!run && gStick.ran) { gAgent.clearTempRun(); gStick.ran = false; }
}

void applyEvents(const std::vector<wolfvr::InputEvent>& events)
{
    if (events.empty()) return;
    LLWindow* window = gViewerWindow->getWindow();
    using T = wolfvr::InputEvent::Type;
    for (const wolfvr::InputEvent& e : events)
    {
        const MASK mask = maskOf(e.mods);
        // [VR HUD LAYER] a point on the UI layer is a point on the whole window (no letterbox)
        auto where = [&](const wolfvr::InputEvent& ev) {
            if (!ev.layer) return toWindow(ev.u, ev.v);
            const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
            return LLCoordGL(llclamp((S32)(ev.u * ww), 0, ww - 1), llclamp((S32)((1.f - ev.v) * wh), 0, wh - 1));
        };
        switch (e.type)
        {
        case T::MOVE:
            gViewerWindow->handleMouseMove(window, where(e), mask);
            break;
        case T::DOWN:
        case T::UP:
        {
            const LLCoordGL pos = where(e);
            // [VR HUD LAYER] a press where the layer shows nothing would land on the world behind the desktop's view,
            // not on anything the resident can see in the headset: ignore it (and its release)
            const int bi = e.button == 2 ? 2 : 0;
            if (e.layer && e.type == T::DOWN && !layerHasPixel(pos.mX, pos.mY)) { gLayerDownIgnored[bi] = true; break; }
            if (e.type == T::UP && gLayerDownIgnored[bi]) { gLayerDownIgnored[bi] = false; break; }
            gViewerWindow->handleMouseMove(window, pos, mask);
            if (e.button == 2)
            {
                if (e.type == T::DOWN) gViewerWindow->handleRightMouseDown(window, pos, mask);
                else gViewerWindow->handleRightMouseUp(window, pos, mask);
            }
            else
            {
                if (e.type == T::DOWN) gViewerWindow->handleMouseDown(window, pos, mask);
                else gViewerWindow->handleMouseUp(window, pos, mask);
            }
            break;
        }
        case T::WHEEL:
            gViewerWindow->handleMouseMove(window, where(e), mask);
            gViewerWindow->handleScrollWheel(window, e.wheel);
            break;
        case T::KEY_DOWN:
        case T::KEY_UP:
        {
            if (!gKeyboard) break;
            KEY key = domKey(e.key);
            if (key == KEY_NONE && e.key.size() == 1 && (mask & (MASK_CONTROL | MASK_ALT)))
            {
                // a shortcut such as Ctrl+F: the viewer's key codes for letters and digits are upper-case ASCII
                const char c = e.key[0];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                {
                    key = (KEY)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
                }
            }
            if (key == KEY_NONE)
            {
                // A printable character without Ctrl/Alt is typed text: the page also sends it as "text".
                break;
            }
            if (e.type == T::KEY_DOWN) gKeyboard->handleTranslatedKeyDown(key, mask);
            else gKeyboard->handleTranslatedKeyUp(key, mask);
            break;
        }
        case T::STICK:
            gStick.x = e.x;
            gStick.y = e.y;
            gStick.heard = LLTimer::getTotalSeconds();
            break;
        case T::LIFT:
            gStick.lift = e.wheel;
            gStick.heard = LLTimer::getTotalSeconds();
            break;
        case T::TURN:
            // One snap turn of 45 degrees (WolfStorm render_manager.js _updateVRInput, Meta's comfort default);
            // LLAgent::yaw turns the body about the up vector, + = left.
            if (!gAgent.isMovementLocked()) gAgent.yaw((F32)e.wheel * 45.f * DEG_TO_RAD);
            break;
        case T::ACTION:
            WolfVRUi::run(e.key, e.text);   // [VR CONTROLS 2026-11-04] an action-bar button (wolfvrui.cpp)
            break;
        case T::TEXT:
        {
            const LLWString w = utf8str_to_wstring(e.text);
            for (llwchar c : w)
            {
                if (c >= 32) gViewerWindow->handleUnicodeChar(c, MASK_NONE);
            }
            break;
        }
        }
    }
}
}
