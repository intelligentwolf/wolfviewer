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

#include "llgl.h"
#include "llglheaders.h"
#include "llkeyboard.h"
#include "llstring.h"
#include "lltimer.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfvrstream.h"

#include <cstring>
#include <vector>

namespace
{
    constexpr int kSlots = 3;   // pixel buffers in flight: read into one, collect the one from two frames ago

    struct Capture
    {
        GLuint fbo = 0, tex = 0;
        GLuint pbo[kSlots] = {0, 0, 0};
        GLsync fence[kSlots] = {nullptr, nullptr, nullptr};
        F64 issued[kSlots] = {0, 0, 0};
        U32 w = 0, h = 0;          // picture size
        int next = 0;
        std::vector<uint8_t> buf;  // handed to the stream; comes back as the stream's spare buffer
        // where the window sits in the picture (letterbox), top-row-first picture pixels: [x0, x1) x [y0, y1)
        S32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        S32 win_w = 0, win_h = 0;
        F64 last = 0;
    };
    Capture gCap;

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
    void fitWindow(S32 ww, S32 wh)
    {
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
}

void captureFrame()
{
    if (!gViewerWindow || !WolfVRStream::bigScreenWanted()) return;
    U32 w = 0, h = 0;
    WolfVRStream::frameSize(w, h);
    if (!w || !h) return;
    // At most 75 pictures a second (the stream is 60): a viewer running faster does not need to copy every frame.
    const F64 now = LLTimer::getTotalSeconds();
    if (now - gCap.last < 1.0 / 75.0) return;
    gCap.last = now;
    if ((w != gCap.w || h != gCap.h) && !makeGl(w, h)) return;
    const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
    if (ww <= 0 || wh <= 0) return;
    const bool refit = ww != gCap.win_w || wh != gCap.win_h;
    if (refit) fitWindow(ww, wh);

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
    glBlitFramebuffer(0, 0, ww, wh, gCap.x0, gCap.y1, gCap.x1, gCap.y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

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
            WolfVRStream::submitFrame(gCap.buf, (LLTimer::getTotalSeconds() - gCap.issued[old]) * 1000.0);
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (read_fb == 0) glReadBuffer(GL_BACK);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void pumpInput()
{
    if (!gViewerWindow) return;
    const std::vector<wolfvr::InputEvent> events = WolfVRStream::takeInput();
    if (events.empty()) return;
    LLWindow* window = gViewerWindow->getWindow();
    using T = wolfvr::InputEvent::Type;
    for (const wolfvr::InputEvent& e : events)
    {
        const MASK mask = maskOf(e.mods);
        switch (e.type)
        {
        case T::MOVE:
            gViewerWindow->handleMouseMove(window, toWindow(e.u, e.v), mask);
            break;
        case T::DOWN:
        case T::UP:
        {
            const LLCoordGL pos = toWindow(e.u, e.v);
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
            gViewerWindow->handleMouseMove(window, toWindow(e.u, e.v), mask);
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
