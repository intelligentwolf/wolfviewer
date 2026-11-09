/**
 * @file wolfvkbridge.h
 * @brief The viewer's side of the Vulkan backend: settings, start-up, the window swapchain, presenting, fall-back.
 *
 * [VULKAN BACKEND 2026-11-08] Paul: "build the vulkan backend stop taking shortcuts and not doing stuff."
 *
 * Settings (settings.xml):
 *   WolfRenderBackend   "opengl" (default) | "vulkan": read once at start-up, so a change takes effect at the next start
 *   WolfVulkanValidation  the Khronos validation layer on (debugging; messages go to the log as "VulkanValidation")
 *   WolfVulkanDevice      part of a device name to prefer ("radeon", "nvidia"); empty = automatic (wolfvkdevice.h)
 *   WolfVulkanStartOK     the first-run marker: false while a Vulkan start is under way, true once the login screen
 *                         has been drawn. A Vulkan start that never got there leaves it false, and the next start
 *                         goes back to OpenGL with the WolfVulkanFellBack notice.
 *
 * What runs in Vulkan mode today (stage 4, first cut): the swapchain owns the window; OpenGL still draws the frame
 * into its (never swapped) default framebuffer, the bridge reads that back and composites it in Vulkan, and the
 * Vulkan pass draws the scene's fullbright batches with the real fullbright shader on top (wolfvkscene.cpp). When
 * Vulkan cannot be set up the viewer carries on in OpenGL and says so (WolfVulkanFellBack).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_VK_BRIDGE_H
#define WOLF_VK_BRIDGE_H

#include "llsd.h"
#include <string>

class LLWindow;

class LLDrawInfo;
class LLMatrix4;

namespace WolfVkBridge
{
    // True when WolfRenderBackend was "vulkan" at start-up and the first-run marker allowed it (decided once, in
    // decideBackend(), called from LLAppViewer::initConfiguration before the window exists).
    void decideBackend();
    bool vulkanRequested();

    // After the OpenGL context and gGL exist (LLViewerWindow::LLViewerWindow after gGL.init): opens the Vulkan loader,
    // enumerates devices (always, for About and the floater), and in Vulkan mode creates the surface, device and
    // swapchain. On failure in Vulkan mode: falls back to OpenGL for this run and remembers the notice.
    void initAfterGL(LLWindow* window);

    // Vulkan is presenting the window (device + swapchain live).
    bool isActive();

    // Called where the viewer would call LLWindow::swapBuffers(). Returns true when Vulkan presented the frame
    // (the GL swap must then be skipped); false when Vulkan is not active and GL should swap as before.
    bool present();

    // Called from render_ui() after renderFinalize(), where the finished world (no HUD, no UI) is in the default
    // framebuffer: in Vulkan mode the world layer is read back and GL's colour is cleared so the UI lands on a
    // transparent layer of its own.
    void captureWorld();

    // The window changed size (LLViewerWindow::reshape): the swapchain is rebuilt at the next present.
    void onResize();

    // The login screen has been drawn: the first-run marker is set, and a pending fall-back notice is shown.
    void onLoginScreenShown();

    // Before the GL context goes (LLViewerWindow::shutdownGL).
    void shutdown();

    // Help > About: "OpenGL: <GL_RENDERER>" or "Vulkan: <device>".
    std::string rendererLine();
    // Help > About: "<device>, driver <driver>, Vulkan <api>" or the "not available on this computer" string.
    std::string vulkanLine();
    // For the grid's hardware statistics and the crash/bug reports: renderer, vulkan_available, vulkan_device,
    // vulkan_driver, vulkan_api, vulkan_loader (strings; absent values are empty strings so the keys are stable).
    LLSD reportFields();
    // A Vulkan device was found at this start (the loader opened and enumerated one).
    bool vulkanAvailable();
    // [V BUTTON 2026-11-08] The resident chose a renderer for the next start ("vulkan" or "opengl"): records it, clears
    // the first-run marker so a failed earlier start does not block the try, and shows the notice that nothing changes
    // until the next start (WolfVulkanChosen / WolfVulkanOpenGLChosen). Shared by Wolf World > Vulkan Renderer and the
    // navigation bar's V button. Nothing happens when the choice is the current setting.
    void chooseBackend(const std::string& choice);
    // Plain-words status for the Vulkan Renderer floater.
    std::string statusLine();
    // The reason the last Vulkan start failed (empty when none).
    const std::string& lastError();
    // Frame timing for the floater/log: milliseconds spent in present() averaged over the last second.
    F32 presentMillis();
    // How many batches the Vulkan pass drew last frame, and the validation layer's message count this session.
    U32 sceneDrawsLastFrame();
    // [VULKAN G-BUFFER 2026-11-08] LLDrawPoolSimple::renderDeferred asks before drawing its static PASS_SIMPLE batches: true =
    // Vulkan drew them into the shared G-buffer (GL skips them), false = GL draws them as always.
    bool drawPassSimple();
    // True when Vulkan drew this draw info this frame AND GL is drawing the leftovers of that pass (inside a
    // ScopedSkipDrawn); LLRenderPass::pushBatches then leaves it to Vulkan. Everywhere else (shadow maps, probes,
    // impostors) GL draws every batch as always.
    bool batchDrawnByVulkan(const LLDrawInfo* params);
    // [AMD PERF 2026-11-08] WolfLogFrameRate: an OpenGL GL_TIMESTAMP at every swap/present (llviewerdisplay.cpp swap()), in BOTH
    // renderers; the interval between consecutive stamps is how long OpenGL's GPU work of a frame took to get through
    // (when GPU-bound). One "GL GPU frame" line a second, log tag WolfVulkanFrame.
    void glFrameStamp();
    // [AMD PERF 2026-11-09] The timing instruments are on: WolfLogFrameRate, and not WolfLogFrameRateOnly. They are the
    // Vulkan timestamps and GL time/timestamp queries, the CPU step timers, the main-loop timers and the depth rewrite's
    // GL_TIME_ELAPSED query. WolfLogFrameRateOnly leaves just the frame-rate line, so fps is measured without their cost.
    bool timingOn();
    // [AMD PERF 2026-11-09] WolfLogFrameRate, both renderers: the main loop's stages (LLAppViewer::doFrame), each timed as
    // wall time AND as the main thread's own CPU time; wall minus CPU is where the thread was blocked or asleep. One
    // "main loop per frame" line a second (log tag WolfVulkanFrame), written by loopFrameEnd() at the end of doFrame.
    // kSwap (swap(): glFrameStamp + present / swapBuffers) runs inside kDisplay and is shown on its own and taken out of it.
    enum class LoopStage { kInput, kIdle, kDisplay, kSwap, kSleep, kCount };   // [CI 2026-11-09] prefixed, like NativeHandles::Kind: no clash with platform macros
    struct LoopTimer
    {
        explicit LoopTimer(LoopStage stage);
        ~LoopTimer();
        LoopStage mStage; bool mOn; double mWall0; double mCpu0;
    };
    void loopFrameEnd();
    // [POSE CHECK 2026-11-08] WolfVulkanDebugPoseCheck: a model matrix GL applies (LLRenderPass::applyModelMatrix); compared
    // with what the Vulkan pass used for the same matrix this frame while the main G-buffer is bound
    void poseCheckModelMatrix(const LLMatrix4* model_matrix);
    struct ScopedSkipDrawn { ScopedSkipDrawn(); ~ScopedSkipDrawn(); };
    // After the simple pass: writes the shared G-buffer to the logs folder when WolfVulkanDebugDumpGBuffer is set.
    void dumpGBufferIfAsked();
    U32 gbufferDrawsLastFrame();
    bool gbufferShared();
    // [SHARED IMAGES 2026-11-08] how the picture is handed from OpenGL to Vulkan, in plain words ("through shared images ..." or "by copying ...")
    std::string handOverLine();
    U32 validationMessageCount();
}

#endif // WOLF_VK_BRIDGE_H
