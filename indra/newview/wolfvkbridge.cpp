/**
 * @file wolfvkbridge.cpp
 * @brief The viewer's side of the Vulkan backend (see wolfvkbridge.h).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "llviewerprecompiledheaders.h"
#include "wolfvkbridge.h"
#include "wolfvkscene.h"
#include "wolfvkgbuffer.h"   // [VULKAN G-BUFFER 2026-11-08]
#include "wolfvkmirror.h"
#include "lldir.h"
#include "llviewerdisplay.h"  // gResizeScreenTexture
#include "pipeline.h"

#include "vk/wolfvkloader.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvksurface.h"
#include "vk/wolfvkswapchain.h"

#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"     // every program's Vulkan mirror goes before the device
#include "vk/wolfvkshader.h"
#include "llwindow.h"
#include "llappviewer.h"   // [RUN-TIME STOP 2026-11-09] requestQuit
#include "llstartup.h"     // [BLACK WORLD 2026-11-10] the world check starts in-world (STATE_STARTED)
#include "llviewercontrol.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "lltimer.h"
#include <chrono>   // [AMD PERF] frame step timing
#include <cstring>  // [VULKAN WAYLAND 2026-11-09] memcmp
#if LL_WINDOWS
#include <windows.h>          // GetThreadTimes (main loop timing)
#else
#include <time.h>             // clock_gettime(CLOCK_THREAD_CPUTIME_ID) (main loop timing)
#endif

#if LL_SDL
#include "SDL2/SDL_video.h"   // SDL_GL_GetProcAddress, as llgl.cpp:64 GLH_EXT_GET_PROC_ADDRESS
#elif LL_WINDOWS
#include <windows.h>          // wglGetProcAddress
#endif

namespace
{
    // STOPPED [RUN-TIME STOP 2026-11-09]: Vulkan failed after the first frame; nothing more is presented (by either API)
    // until the viewer has quit (stopAtRunTime)
    enum class State { OPENGL, VULKAN_STARTING, VULKAN_ACTIVE, FELL_BACK, STOPPED };

    State sState = State::OPENGL;
    bool sRequested = false;            // WolfRenderBackend == "vulkan" at start, after the first-run check
    std::string sPendingNotice;         // notification name to show at the login screen
    std::string sPendingReason;         // its [REASON]
    std::string sLastError;
    bool sLoginShown = false;
    bool sResizePending = false;
    bool sInfoProbed = false;           // devices enumerated for About / the floater
    std::string sVulkanDeviceLine;      // "<name>, driver <d>, Vulkan <v>" of the device that would be / is used
    std::string sLoaderName;
    uint32_t sDeviceCount = 0;
    // [A1 2026-11-09] WolfVulkanShareGBuffer decided the G-buffer stays OpenGL's own (8.2o): why, for the log/report,
    // and whether it was the automatic AMD-integrated choice (the Vulkan window then says so in plain words)
    std::string sGBufferNotSharedWhy;
    bool sGBufferAutoAMD = false;

    // [BLACK WORLD 2026-11-10] WolfVulkanBlackWorldCheck (wolfvkscene.h WorldProbe): the check's own state. Paul's W102
    // report (Omniwright Mooncastle, Windows, AVX2): the whole 3D view black at every region with Vulkan on, UI and
    // mini-map fine; the viewer did not notice. It is CONSERVATIVE by construction - it never fires on a dark scene:
    //   A. OpenGL's own frame has light (max R/G/B > 0) but Vulkan's view of the shared world layer is black, three
    //      probes running: the import does not carry the picture -> stop (OpenGL next start).
    //   B. OpenGL's own frame is black three probes running -> a two-stage experiment with the shared G-buffer:
    //      stage 1 OpenGL draws the simple pass itself and skips the depth rewrite (Vulkan touches the G-buffer no
    //      more); stage 2 the G-buffer is given back to OpenGL (its own textures). Only when a stage brings light back
    //      is Vulkan to blame -> stop with the stage's reason. Black through both stages = a dark scene (a black sky
    //      box, a cave at night): nothing fires, the normal mode is restored, and the log says so.
    //   The check runs once a second for the first 90 s in-world and ends early after 10 lit probes.
    struct BlackCheck
    {
        enum Stage { OFF, WATCH, STAGE1_SETTLE, STAGE1, STAGE2_SETTLE, STAGE2, DONE };
        Stage mStage = OFF;
        F64 mStarted = 0.0;
        uint32_t mLastGlSeq = 0;     // the last GL sample judged
        uint32_t mProbes = 0;
        uint32_t mGlBlackRun = 0;    // consecutive GL-black probes in the current stage
        uint32_t mGlLitRun = 0;      // consecutive GL-lit probes in the current stage
        uint32_t mImportFaultRun = 0;
        uint32_t mGoodRun = 0;
        uint32_t mSettle = 0;        // probes to let pass after a stage change (the G-buffer is remade at the next frame)
        bool mGBufferWasShared = false;
        std::string mSummary;        // the outcome, for the log and the reports
    };
    BlackCheck sBlack;
    bool sGBufferGLOnly = false;         // [BLACK WORLD] stage 1: Vulkan draws no pass and no depth rewrite runs
    bool sGBufferUnsharePending = false; // [BLACK WORLD] stage 2: the target gives the shared textures back at its next allocate
    void stopAtRunTime(const std::string& reason);
    bool shareGBuffer(const char* why);
    void unshareGBuffer(const std::string& why);

    LLWindow* sWindow = nullptr;
    VkSurfaceKHR sSurface = VK_NULL_HANDLE;
    WolfVkSwapchain sSwapchain;
    WolfVkScene* sScene = nullptr;
    WolfVkGBuffer* sGBuffer = nullptr;   // [VULKAN G-BUFFER 2026-11-08] the shared G-buffer, null when not shareable
    bool sFrameBegun = false;            // the device frame slot is open (begun mid-frame by a pass, or at present)
    uint32_t sGBufferDraws = 0;
    int sSkipDrawnDepth = 0;             // > 0 while GL draws the leftovers of a pass Vulkan drew (ScopedSkipDrawn)

    // present() timing: wall-clock milliseconds, averaged per second
    F64 sPresentAccum = 0.0;
    U32 sPresentCount = 0;
    F64 sPresentWindowStart = 0.0;
    F32 sPresentMillis = 0.f;

    // [AMD PERF 2026-11-08] Where a Vulkan-mode frame spends its time (AMD Radeon 890M: Vulkan mode ~11 ms a frame slower than
    // OpenGL, runs amd-r1/r2). Only with WolfLogFrameRate on; one summary line a second in the WolfVulkanFrame log, per frame:
    //  - CPU ms of each step (steady_clock): the G-buffer hand-over and pass, the slot's fence/timeline wait, the world capture,
    //    the present's acquire / record / submit / present, and the one-time submits (WolfVkDevice::takeOneTimeStats);
    //  - GPU ms of the Vulkan pass and of the Vulkan frame (vkCmdWriteTimestamp, vulkan_core.h:4600, read without waiting once
    //    the slot's fence has been waited; ticks x VkPhysicalDeviceLimits::timestampPeriod, :3677) and of OpenGL's world and
    //    UI captures (GL_TIME_ELAPSED queries, read a frame later only when available).
    namespace prof
    {
        enum Step { PASS_HAND, PASS_RECORD, PASS_SUBMIT, PASS_TAKEBACK, PASS_REWRITE, BEGIN_WAIT, CAPTURE,
                    PRESENT_ACQUIRE, PRESENT_RECORD, PRESENT_SUBMIT, PRESENT_PRESENT, STEP_COUNT };
        const char* const sStepName[STEP_COUNT] = { "hand", "pass record", "pass submit", "take back", "depth rewrite", "slot wait",
                                                    "world capture", "acquire", "frame record", "frame submit", "present" };
        double sCpu[STEP_COUNT] = {};
        uint32_t sFrames = 0;
        bool on() { return WolfVkBridge::timingOn(); }
        struct Scope
        {
            Step mStep; bool mOn; std::chrono::steady_clock::time_point mT0;
            explicit Scope(Step s) : mStep(s), mOn(on()), mT0(std::chrono::steady_clock::now()) {}
            ~Scope() { if (mOn) sCpu[mStep] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mT0).count(); }
        };

        // Vulkan timestamps: per frame slot, queries 0-1 around the first G-buffer pass of the frame, 2-3 around the frame's
        // own command buffer
        VkQueryPool sPool = VK_NULL_HANDLE;
        double sPeriodNs = 0.0;
        std::vector<uint8_t> sPassWritten, sFrameWritten;
        bool sPassThisFrame = false;
        double sGpuPass = 0.0, sGpuFrame = 0.0;
        uint32_t sGpuPassN = 0, sGpuFrameN = 0;

        void initStamps()
        {
            const uint32_t family = gWolfVkDevice.queueFamily();
            const auto& families = gWolfVkDevice.chosen().mQueueFamilies;
            if (family >= families.size() || families[family].timestampValidBits == 0) return;   // vulkan_core.h:3723
            const uint32_t slots = gWolfVkDevice.framesInFlight();
            // Source: vulkan_core.h:3878 VkQueryPoolCreateInfo { sType, pNext, flags, queryType, queryCount, pipelineStatistics }
            VkQueryPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;   // :218
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;                // :2244
            info.queryCount = slots * 4;
            if (gVk.vkCreateQueryPool(gWolfVkDevice.device(), &info, nullptr, &sPool) != VK_SUCCESS) { sPool = VK_NULL_HANDLE; return; }
            sPeriodNs = gWolfVkDevice.limits().timestampPeriod;
            sPassWritten.assign(slots, 0);
            sFrameWritten.assign(slots, 0);
        }
        void destroyStamps(bool device_usable)
        {
            if (sPool != VK_NULL_HANDLE && device_usable) gVk.vkDestroyQueryPool(gWolfVkDevice.device(), sPool, nullptr);
            sPool = VK_NULL_HANDLE;
            sPassWritten.clear();
            sFrameWritten.clear();
        }
        void stamp(VkCommandBuffer cmd, uint32_t first, bool begin)
        {
            const uint32_t slot = gWolfVkDevice.frameIndex();
            if (begin) gVk.vkCmdResetQueryPool(cmd, sPool, slot * 4 + first, 2);
            gVk.vkCmdWriteTimestamp(cmd, begin ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sPool, slot * 4 + first + (begin ? 0 : 1));
        }
        // after beginFrame waited the slot: its timestamps are final (VK_QUERY_RESULT_64_BIT :3034, no WAIT flag: never blocks)
        void readSlot()
        {
            if (sPool == VK_NULL_HANDLE) return;
            const uint32_t slot = gWolfVkDevice.frameIndex();
            if (slot >= sPassWritten.size()) return;
            uint64_t t[2] = { 0, 0 };
            if (sPassWritten[slot] && gVk.vkGetQueryPoolResults(gWolfVkDevice.device(), sPool, slot * 4, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                sGpuPass += static_cast<double>(t[1] - t[0]) * sPeriodNs / 1.0e6; sGpuPassN++;
            }
            if (sFrameWritten[slot] && gVk.vkGetQueryPoolResults(gWolfVkDevice.device(), sPool, slot * 4 + 2, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                sGpuFrame += static_cast<double>(t[1] - t[0]) * sPeriodNs / 1.0e6; sGpuFrameN++;
            }
            sPassWritten[slot] = sFrameWritten[slot] = 0;
        }

        // OpenGL GPU time: two GL_TIME_ELAPSED queries in turn (OpenGL 4.6 4.3), results read when available
        struct GLTimer
        {
            GLuint mIds[2] = { 0, 0 }; bool mBusy[2] = { false, false }; int mNext = 0; bool mOpen = false;
            double mSum = 0.0; uint32_t mN = 0;
            void begin()
            {
                if (!mIds[0]) glGenQueries(2, mIds);
                const int q = mNext;
                if (mBusy[q])
                {
                    GLuint avail = 0;
                    glGetQueryObjectuiv(mIds[q], GL_QUERY_RESULT_AVAILABLE, &avail);
                    if (!avail) return;
                    GLuint64 ns = 0;
                    glGetQueryObjectui64v(mIds[q], GL_QUERY_RESULT, &ns);
                    mSum += static_cast<double>(ns) / 1.0e6; mN++; mBusy[q] = false;
                }
                glBeginQuery(GL_TIME_ELAPSED, mIds[q]);
                mOpen = true;
            }
            void end()
            {
                if (!mOpen) return;
                glEndQuery(GL_TIME_ELAPSED);
                mBusy[mNext] = true; mNext = 1 - mNext; mOpen = false;
            }
            void destroy() { if (mIds[0] && gGLManager.mInited) glDeleteQueries(2, mIds); *this = GLTimer(); }
        };
        GLTimer sGlCapture, sGlFrameRecord;
        // OpenGL's GPU time from its signal of gl_pass_done to the end of its wait on vk_pass_done: the Vulkan pass plus
        // whatever the switch between the two APIs costs on the GPU (GL cannot draw meanwhile).
        // [AMD PERF 2026-11-09] Measured with two GL_TIMESTAMP queries, not GL_TIME_ELAPSED: r37 gave the elapsed timer
        // 0.00 ms on radeonsi against a 0.81 ms Vulkan pass between, so that driver does not count the semaphore wait
        // as GL GPU time. glQueryCounter records the time once all previous commands are fully realised (OpenGL 4.6
        // core 4.3 "Time Queries"), and the second counter is issued after glWaitSemaphoreEXT, whose wait holds back
        // the commands after it (EXT_external_objects 4.2.3), so the difference holds the whole span on any driver.
        struct GLSpan
        {
            GLuint mIds[2][2] = { { 0, 0 }, { 0, 0 } }; bool mBusy[2] = { false, false }; int mNext = 0; bool mOpen = false;
            double mSum = 0.0; uint32_t mN = 0;
            void begin()
            {
                if (!mIds[0][0]) glGenQueries(4, &mIds[0][0]);
                const int q = mNext;
                if (mBusy[q])
                {
                    // the end counter completes after the begin one, so its availability covers both
                    GLuint avail = 0;
                    glGetQueryObjectuiv(mIds[q][1], GL_QUERY_RESULT_AVAILABLE, &avail);
                    if (!avail) return;
                    GLuint64 t0 = 0, t1 = 0;
                    glGetQueryObjectui64v(mIds[q][0], GL_QUERY_RESULT, &t0);
                    glGetQueryObjectui64v(mIds[q][1], GL_QUERY_RESULT, &t1);
                    if (t1 >= t0) { mSum += static_cast<double>(t1 - t0) / 1.0e6; mN++; }
                    mBusy[q] = false;
                }
                glQueryCounter(mIds[q][0], GL_TIMESTAMP);
                mOpen = true;
            }
            void end()
            {
                if (!mOpen) return;
                glQueryCounter(mIds[mNext][1], GL_TIMESTAMP);
                mBusy[mNext] = true; mNext = 1 - mNext; mOpen = false;
            }
            // no hand-over happened: the begin counter's result is simply never read (the slot is reissued next time)
            void cancel() { mOpen = false; }
            void destroy() { if (mIds[0][0] && gGLManager.mInited) glDeleteQueries(4, &mIds[0][0]); *this = GLSpan(); }
        };
        GLSpan sGlHandOver;
        // [AMD PERF 2026-11-09] the world capture again, between two GL_TIMESTAMP counters: its glWaitSemaphoreEXT(vk_done)
        // is inside, which GL_TIME_ELAPSED (sGlCapture) does not count on radeonsi (8.2o r37), so span minus elapsed is
        // the GPU time that wait held OpenGL back - the stall double-buffered layers would remove
        GLSpan sGlCaptureSpan;
    }

    void rememberNotice(const char* name, const std::string& reason)
    {
        sPendingNotice = name;
        sPendingReason = reason;
        if (sLoginShown)
        {
            LLSD args;
            args["REASON"] = reason;
            LLNotificationsUtil::add(name, args);
            sPendingNotice.clear();
        }
    }

    // The OpenGL context's device UUID (GL_EXT_memory_object, glext.h:7736 GL_DEVICE_UUID_EXT, :7759 glGetUnsignedBytevEXT),
    // so the Vulkan device chosen is the card OpenGL draws with. The extension is Windows/Linux only; Apple's GL lacks it.
    bool glDeviceUUID(uint8_t out[VK_UUID_SIZE], uint32_t& vendor_id)
    {
        vendor_id = gGLManager.mIsNVIDIA ? WolfVkDevice::VENDOR_NVIDIA
                  : gGLManager.mIsAMD ? WolfVkDevice::VENDOR_AMD
                  : gGLManager.mIsIntel ? WolfVkDevice::VENDOR_INTEL
                  : gGLManager.mIsApple ? WolfVkDevice::VENDOR_APPLE : 0;
        memset(out, 0, VK_UUID_SIZE);
#if LL_DARWIN
        return false;
#else
#if LL_WINDOWS
        // llglheaders.h:354 - on Windows glGetStringi is a function pointer looked up at run time (llgl.cpp:1086) and can be null;
        // on Linux it is the real function from GL/gl.h (llglheaders.h:1044), so testing it there is meaningless (-Waddress)
        if (!glGetStringi)
        {
            return false;
        }
#endif
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        bool have = false;
        for (GLint i = 0; i < count && !have; ++i)
        {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            have = ext && strcmp(ext, "GL_EXT_memory_object") == 0;
        }
        if (!have)
        {
            return false;
        }
        PFNGLGETUNSIGNEDBYTEVEXTPROC get_bytes = nullptr;
#if LL_SDL
        get_bytes = reinterpret_cast<PFNGLGETUNSIGNEDBYTEVEXTPROC>(SDL_GL_GetProcAddress("glGetUnsignedBytevEXT"));
#elif LL_WINDOWS
        get_bytes = reinterpret_cast<PFNGLGETUNSIGNEDBYTEVEXTPROC>(wglGetProcAddress("glGetUnsignedBytevEXT"));
#endif
        if (!get_bytes)
        {
            return false;
        }
        // GL_UUID_SIZE_EXT is 16 (glext.h:7738) = VK_UUID_SIZE (vulkan_core.h:133)
        get_bytes(GL_DEVICE_UUID_EXT, out);
        return glGetError() == GL_NO_ERROR;
#endif
    }

    // The Vulkan side of every GL program (WolfVkShaderMirror) owns device objects: destroyed before the device.
    void destroyShaderMirrors()
    {
        for (LLGLSLShader* shader : LLGLSLShader::sInstances)
        {
            if (shader && shader->mWolfVk)
            {
                delete shader->mWolfVk;
                shader->mWolfVk = nullptr;
            }
        }
    }

    // Everything Vulkan made goes, in the order the hand-over needs (both fall-back paths below).
    void logSessionSummary(const char* when)
    {
        // [BLACK WORLD 2026-11-10] one line with everything the next report needs
        LL_INFOS("Vulkan") << "Vulkan session summary (" << when << "): SPIR-V " << (sScene ? sScene->spirvSummary() : std::string("no scene"))
                           << "; hand-over " << (sScene ? sScene->handOverLine() : std::string("-"))
                           << "; G-buffer " << (sGBuffer ? (sGBuffer->available() ? "shared" : "sharing ended: " + sGBuffer->disabledReason()) : "not shared" + (sGBufferNotSharedWhy.empty() ? std::string() : " (" + sGBufferNotSharedWhy + ")"))
                           << "; world check " << (sBlack.mSummary.empty() ? std::string("not run") : sBlack.mSummary) << LL_ENDL;
    }

    void tearDown()
    {
        logSessionSummary("teardown");
        sBlack.mStage = BlackCheck::DONE;
        sGBufferGLOnly = false;
        sGBufferUnsharePending = false;
        // the G-buffer first: the render target stops being shared and gets ordinary GL textures at its next allocation
        if (gPipeline.mRT) gPipeline.mMainRT.deferredScreen.setWolfVkShared(false);
        LLRenderTarget::sWolfVkProvider = nullptr;
        // [HANG GUARD 2026-11-08 8.2k] Each teardown step waits (bounded) for the card and may itself find it hung; from
        // the step that does, everything left is abandoned (leaked) with the device instead of destroyed, because a stuck
        // submit may still use it (review: hung() was tested once, then e.g. the scene reset its descriptor pools and the
        // surface was destroyed under a swapchain that could not be - VUID-vkDestroySurfaceKHR-surface-01266).
        // WolfVkDevice::destroy() drops a hung device without waiting. The mirror's LLImageGL/LLVertexBuffer hooks come off
        // in every case (WolfVkMirror::shutdown: its deletes only file deferred destroys, which a hung device never runs).
        if (!gWolfVkDevice.hung() && sGBuffer)
        {
            delete sGBuffer;   // waits for both APIs (bounded) before its images go
            gResizeScreenTexture = true;   // the G-buffer is re-allocated with GL's own textures
        }
        sGBuffer = nullptr;
        if (!gWolfVkDevice.hung()) destroyShaderMirrors();
        if (!gWolfVkDevice.hung() && sScene) delete sScene;   // WolfVkScene::destroy abandons in turn if its own wait runs out
        else WolfVkMirror::shutdown();                        // the scene owns the mirror's shutdown when it is deleted
        sScene = nullptr;
        prof::destroyStamps(!gWolfVkDevice.hung() && gWolfVkDevice.waitIdle());   // [AMD PERF] no submit may still write them
        prof::sGlCapture.destroy();
        prof::sGlFrameRecord.destroy();
        prof::sGlHandOver.destroy();
        prof::sGlCaptureSpan.destroy();
        if (!gWolfVkDevice.hung()) sSwapchain.destroy();
        if (sSurface != VK_NULL_HANDLE)
        {
            if (!gWolfVkDevice.hung()) WolfVkSurface::destroy(gWolfVkDevice.instance(), sSurface);   // only after its swapchain went
            sSurface = VK_NULL_HANDLE;
        }
        gWolfVkDevice.destroy();
    }

    // START-UP fall-back: Vulkan could not start before its first frame. OpenGL draws this very start in the same window
    // (nothing of Vulkan has reached the window yet) and the notice says why.
    void fallBack(const std::string& reason, const char* notice)
    {
        LL_WARNS("Vulkan") << "Falling back to OpenGL: " << reason << LL_ENDL;
        sLastError = reason;
        sState = State::FELL_BACK;
        tearDown();
        // the setting follows what is running, so the floater and About tell the truth; the resident can ask again
        gSavedSettings.setString("WolfRenderBackend", "opengl");
        gSavedSettings.setBOOL("WolfVulkanStartOK", true);
        rememberNotice(notice, reason);
    }

    // [RUN-TIME STOP 2026-11-09] RUN-TIME failure (after Vulkan has drawn the window): Paul, "we force the user to relog if
    // they switch to opengl". Handing a window Vulkan has been presenting to OpenGL in the same session made the screen
    // flash on a native Wayland window (chain36 amd-wl-forced, 12:55Z), so it is not attempted: Vulkan is torn down, the
    // next start is set to OpenGL (saved at once, in case the quit itself fails), one plain-English message box says so,
    // and the viewer quits by the normal path after OK. The message box is the operating system's own
    // (OSMessageBox, llwindow.cpp:72 -> SDL_ShowMessageBox / MessageBox / NSAlert), which needs no renderer. From here
    // present() shows nothing more, Vulkan or OpenGL (State::STOPPED), until the viewer has gone.
    void stopAtRunTime(const std::string& reason)
    {
        if (sState == State::STOPPED) return;
        LL_WARNS("Vulkan") << "Vulkan renderer stopped while running; next start in OpenGL, quitting: " << reason << LL_ENDL;
        sLastError = reason;
        sState = State::STOPPED;
        tearDown();
        gSavedSettings.setString("WolfRenderBackend", "opengl");
        gSavedSettings.setBOOL("WolfVulkanStartOK", true);
        // The reason is saved with the rest BEFORE the box, so a viewer that never comes back from it (killed, or ended
        // from the desktop's "not responding" dialog while the box blocks the main thread: chain42 amd-r49-wlstop) still
        // tells the resident at the next start (decideBackend, WolfVulkanStoppedLastRun). It is cleared once the box has
        // been seen.
        gSavedSettings.setString("WolfVulkanStoppedReason", reason);
        gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
        LLStringUtil::format_map_t args;
        args["[REASON]"] = reason;
        // A one-button box answers OSBTN_CANCEL only when it could not be shown: OSMessageBoxSDL when SDL_ShowMessageBox
        // fails (llwindowsdl2.cpp; e.g. no zenity on a Wayland desktop, SDL_waylandmessagebox.c), OSMessageBoxWin32 when
        // MessageBoxW fails (its default case). Shown and closed without OK is -1 (SDL) or NSAlertFirstButtonReturn
        // (macOS showAlert), not OSBTN_CANCEL.
        if (OSMessageBox(LLTrans::getString("WolfVulkanStoppedRestart", args), LLTrans::getString("WolfVulkanStoppedTitle"), OSMB_OK) != OSBTN_CANCEL)
        {
            gSavedSettings.setString("WolfVulkanStoppedReason", "");
            gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
        }
        LLAppViewer::instance()->requestQuit();
    }

    bool createSwapchain(std::string& error)
    {
        LLCoordWindow size;
        sWindow->getSize(&size);
        const uint32_t w = size.mX > 0 ? static_cast<uint32_t>(size.mX) : 1u;
        const uint32_t h = size.mY > 0 ? static_cast<uint32_t>(size.mY) : 1u;
        const bool vsync = gSavedSettings.getBOOL("RenderVSyncEnable");
        // [AMD PERF 2026-11-08] WolfVulkanPresentMode (testing): "immediate" / "mailbox" / "fifo" in place of the vsync rule
        // when the surface offers it; anything else = the rule. VkPresentModeKHR values: vulkan_core.h:8944-8946
        const std::string pm = gSavedSettings.getString("WolfVulkanPresentMode");
        sSwapchain.setPresentModeOverride(pm == "immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR : pm == "mailbox" ? VK_PRESENT_MODE_MAILBOX_KHR
                                          : pm == "fifo" ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAX_ENUM_KHR);
        // [T724 2026-11-10] the X11 child window the surface is on follows the viewer's window first (wolfvksurface.h),
        // so the swapchain's currentExtent is this size; a no-op on every other surface
        WolfVkSurface::resize(w, h);
        if (sSwapchain.isValid())
        {
            return sSwapchain.recreate(w, h, vsync, error);
        }
        return sSwapchain.create(gWolfVkDevice, sSurface, w, h, vsync, error);
    }

    // Enumerates devices for About and the floater without touching the window: an instance on its own, destroyed
    // again unless Vulkan mode keeps it.
    void probeInfo(bool keep_instance)
    {
        if (sInfoProbed && gWolfVkDevice.hasInstance())
        {
            return;
        }
        WolfVkDevice::Config config;
        config.mValidation = keep_instance && gSavedSettings.getBOOL("WolfVulkanValidation");
        config.mDeviceOverride = gSavedSettings.getString("WolfVulkanDevice");
        config.mHaveGLDeviceUUID = glDeviceUUID(config.mGLDeviceUUID, config.mGLVendorID);
        config.mApplicationName = "WolfViewer";
        std::string error;
        if (!gWolfVkDevice.createInstance(config, error))
        {
            sLastError = error;
            sVulkanDeviceLine.clear();
            sDeviceCount = 0;
            LL_INFOS("Vulkan") << "Vulkan not available: " << error << LL_ENDL;
        }
        else
        {
            sLoaderName = WolfVkLoader::libraryName();
            sDeviceCount = static_cast<uint32_t>(gWolfVkDevice.physicalDevices().size());
            // the device the chooser would pick, with the same rules but no surface (a.-e. in wolfvkdevice.h)
            std::string pick_error;
            if (gWolfVkDevice.chooseOnly(VK_NULL_HANDLE, pick_error))
            {
                sVulkanDeviceLine = gWolfVkDevice.aboutLine();
            }
            else
            {
                sLastError = pick_error;
            }
            if (!keep_instance)
            {
                gWolfVkDevice.destroy();
            }
        }
        sInfoProbed = true;
    }

    // [VULKAN G-BUFFER 2026-11-08] the main G-buffer becomes shared: LLRenderTarget asks this provider for its textures
    // from the next allocation, which gResizeScreenTexture forces at the next frame (LLPipeline::resizeScreenTexture).
    // [BLACK WORLD 2026-11-10] a function: the world check gives the G-buffer back to OpenGL (unshareGBuffer) and, when
    // that proved nothing, shares it again. why: for the log ("start", "restored after the world check").
    bool shareGBuffer(const char* why)
    {
        if (sGBuffer) return true;
        static LLCachedControl<bool> no_gbuffer(gSavedSettings, "WolfVulkanDebugNoGBuffer", false);
        std::string error;
        // [A1 2026-11-09] WolfVulkanShareGBuffer: "on" (default) shares, "off" does not, "auto" shares except on AMD
        // integrated graphics. Default "on" by the coordinator's decision for Paul ("whichever is best", 8.2o chain34):
        // after the end-of-frame flush (r41) Vulkan matches OpenGL on the 890M either way (shared 25.6 fps, not shared
        // 26.9, OpenGL 26.6 - one valid round, within noise), and not sharing leaves Vulkan drawing none of the world.
        // "auto" keeps the measured AMD rule for whoever chooses it. There, images shared between the APIs lose DCC/HTILE compression on RADV + radeonsi
        // (8.2o r38: every shared image reports VK_IMAGE_COMPRESSION_DISABLED_EXT, the drivers' dumps agree) and OpenGL
        // draws the whole deferred geometry into them, ~6 ms a frame on the Radeon 890M (r37: 49-50 ms shared, 43.5-44
        // not); GL drawing PASS_SIMPLE itself gives the same picture (OpenGL mode). AMD integrated = the chosen Vulkan
        // device's VkPhysicalDeviceProperties deviceType VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU (vulkan_core.h) and
        // vendorID WolfVkDevice::VENDOR_AMD (0x1002, wolfvkdevice.h:101), and OpenGL on AMD too (gGLManager.mIsAMD,
        // llgl.cpp: GL_VENDOR names AMD/ATI) - the device is normally matched to GL's by its UUID (choosePhysicalDevice b.).
        const std::string share_mode = gSavedSettings.getString("WolfVulkanShareGBuffer");
        const WolfVkPhysicalDeviceInfo& dev = gWolfVkDevice.chosen();
        // Only on Mesa's RADV (VkPhysicalDeviceDriverProperties driverID VK_DRIVER_ID_MESA_RADV, vulkan_core.h:6357):
        // the compression loss was measured with RADV + radeonsi; AMD's own Windows driver has not been measured, so
        // "auto" keeps sharing there.
        const bool amd_integrated = dev.mProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                    && dev.mProperties.vendorID == WolfVkDevice::VENDOR_AMD
                                    && dev.mDriver.driverID == VK_DRIVER_ID_MESA_RADV && gGLManager.mIsAMD;
        sGBufferNotSharedWhy.clear();
        sGBufferAutoAMD = false;
        if (no_gbuffer)
        {
            sGBufferNotSharedWhy = "WolfVulkanDebugNoGBuffer";
        }
        else if (share_mode == "off")
        {
            sGBufferNotSharedWhy = "WolfVulkanShareGBuffer off";
        }
        else if (share_mode == "auto" && amd_integrated)
        {
            sGBufferNotSharedWhy = "WolfVulkanShareGBuffer auto: AMD integrated graphics on RADV (" + dev.name() + ")";
            sGBufferAutoAMD = true;
        }
        else if (sScene && !sScene->sharedHandOver())
        {
            // [IMPORT SELF-TEST 2026-11-10] the layers' round trip failed (or sharing is off): the same import would carry
            // the G-buffer, so it is not shared either - OpenGL draws every pass into its own textures
            sGBufferNotSharedWhy = "the shared layers are not in use (" + sScene->handOverLine() + ")";
        }
        if (!sGBufferNotSharedWhy.empty())
        {
            LL_INFOS("Vulkan") << "G-buffer sharing off (" << sGBufferNotSharedWhy << "): OpenGL draws the geometry passes" << LL_ENDL;
            return false;
        }
        sGBuffer = new WolfVkGBuffer();
        if (sGBuffer->init(gWolfVkDevice, gSavedSettings.getBOOL("WolfVulkanShareSelfTest"), error))
        {
            LLRenderTarget::sWolfVkProvider = sGBuffer;
            gPipeline.mMainRT.deferredScreen.setWolfVkShared(true);
            gResizeScreenTexture = true;
            LL_INFOS("Vulkan") << "G-buffer sharing on (" << why << "): the main G-buffer is remade over Vulkan memory at the next frame" << LL_ENDL;
            return true;
        }
        LL_INFOS("Vulkan") << "G-buffer sharing unavailable, geometry passes stay on OpenGL: " << error << LL_ENDL;
        sGBufferNotSharedWhy = error;
        delete sGBuffer;
        sGBuffer = nullptr;
        return false;
    }

    // [BLACK WORLD 2026-11-10] The G-buffer goes back to OpenGL's own textures: the target is marked unshared and remade at
    // the next frame (gResizeScreenTexture -> LLPipeline::resizeScreenTexture -> release(): the provider hears of it and
    // the shared images go); the provider object itself goes once the target has given them back (finishUnshare,
    // at the next present), never while the target's framebuffer still names its textures.
    void unshareGBuffer(const std::string& why)
    {
        if (!sGBuffer) return;
        LL_INFOS("WolfVulkanCheck") << "G-buffer given back to OpenGL: " << why << LL_ENDL;
        sGBufferGLOnly = true;   // no Vulkan pass into it from this moment, whatever the target still holds
        if (gPipeline.mRT) gPipeline.mMainRT.deferredScreen.setWolfVkShared(false);
        gResizeScreenTexture = true;
        sGBufferUnsharePending = true;
        if (sGBufferNotSharedWhy.empty()) sGBufferNotSharedWhy = why;
    }

    void finishUnshare()
    {
        if (!sGBufferUnsharePending || !sGBuffer) return;
        if (sGBuffer->hasImages()) return;   // the target has not released them yet (it does at its next allocate)
        if (gWolfVkDevice.hung()) return;    // tearDown abandons it
        LLRenderTarget::sWolfVkProvider = nullptr;
        delete sGBuffer;
        sGBuffer = nullptr;
        sGBufferUnsharePending = false;
        LL_INFOS("WolfVulkanCheck") << "G-buffer sharing ended: OpenGL draws every pass into its own textures" << LL_ENDL;
    }

    // [BLACK WORLD 2026-11-10] the world check, once per new GL probe sample (presentFrame, after the frame went out)
    void blackCheck()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, "WolfVulkanBlackWorldCheck", true);
        if (!enabled || sState != State::VULKAN_ACTIVE || !sScene) return;
        if (sBlack.mStage == BlackCheck::DONE) return;
        if (sBlack.mStage == BlackCheck::OFF)
        {
            // armed in-world, at the first frame that captured a world
            if (LLStartUp::getStartupState() < STATE_STARTED || !sScene->worldCapturedThisFrame()) return;
            sBlack.mStage = BlackCheck::WATCH;
            sBlack.mStarted = LLTimer::getTotalSeconds();
            sBlack.mGBufferWasShared = sGBuffer != nullptr;
            sScene->setProbeArmed(true);
            LL_INFOS("WolfVulkanCheck") << "World check started: once a second for up to 90 s; GL samples its own frame"
                                        << (sScene->sharedHandOver() ? ", Vulkan samples the shared world layer" : " (read-back hand-over: no Vulkan sample)")
                                        << "; G-buffer " << (sGBuffer ? "shared" : "not shared") << LL_ENDL;
            return;
        }
        const WolfVkScene::WorldProbe& p = sScene->probe();
        if (!p.mGlValid || p.mGlSeq == sBlack.mLastGlSeq) return;   // no new GL sample yet
        sBlack.mLastGlSeq = p.mGlSeq;
        sBlack.mProbes++;
        const bool gl_black = p.mGlMax == 0;
        // Vulkan's sample of the same frame (the GL one is read two probes after it was taken, the Vulkan one a few
        // frames after: looked up by the probe number in the scene's ring)
        uint32_t vk_max = 0, vk_lit = 0;
        const bool vk_known = p.mVkPossible && sScene->probe().vkSample(p.mGlSeq, vk_max, vk_lit);
        const bool vk_black = vk_known && vk_max == 0;
        LL_INFOS("WolfVulkanCheck") << "World probe " << p.mGlSeq << ": GL max R/G/B " << p.mGlMax << " (" << p.mGlLit << " of 1024 points lit)"
                                    << (vk_known ? llformat(", Vulkan's view max %u (%u lit)", vk_max, vk_lit) : std::string(p.mVkPossible ? ", no Vulkan sample for this probe" : ""))
                                    << ", stage " << static_cast<int>(sBlack.mStage) << LL_ENDL;
        // A. the import does not carry the picture
        if (!gl_black && vk_black) sBlack.mImportFaultRun++; else if (vk_known) sBlack.mImportFaultRun = 0;
        if (sBlack.mImportFaultRun >= 3)
        {
            sBlack.mSummary = llformat("FAILED: OpenGL's frame had light (max %u) but Vulkan read the shared world layer black, 3 probes running", p.mGlMax);
            LL_WARNS("WolfVulkanCheck") << "World check " << sBlack.mSummary << LL_ENDL;
            sScene->setProbeArmed(false);
            stopAtRunTime("Vulkan drew nothing on this computer: the picture OpenGL drew did not reach Vulkan through the shared image");
            return;
        }
        auto end_check = [&](const std::string& summary, bool warn)
        {
            sBlack.mSummary = summary;
            sBlack.mStage = BlackCheck::DONE;
            sScene->setProbeArmed(false);
            if (warn) LL_WARNS("WolfVulkanCheck") << "World check " << summary << LL_ENDL;
            else LL_INFOS("WolfVulkanCheck") << "World check " << summary << LL_ENDL;
        };
        switch (sBlack.mStage)
        {
        case BlackCheck::WATCH:
            if (gl_black) { sBlack.mGlBlackRun++; sBlack.mGoodRun = 0; }
            else { sBlack.mGlBlackRun = 0; sBlack.mGoodRun++; }
            if (sBlack.mGoodRun >= 10)
            {
                end_check(llformat("passed: OpenGL's frame lit in 10 probes running (last max %u)%s", p.mGlMax, vk_known ? ", Vulkan sees it too" : ""), false);
                return;
            }
            if (sBlack.mGlBlackRun >= 3)
            {
                if (!sGBuffer)
                {
                    // nothing of Vulkan's is in OpenGL's frame when the G-buffer is OpenGL's own: a black frame here is a dark
                    // scene or something no renderer switch would change
                    end_check("ended: OpenGL's own frame black 3 probes running with the G-buffer not shared - a dark scene, not a Vulkan fault", false);
                    return;
                }
                LL_WARNS("WolfVulkanCheck") << "OpenGL's frame black 3 probes running: stage 1, OpenGL draws the simple pass itself and no depth rewrite runs" << LL_ENDL;
                sGBufferGLOnly = true;
                sBlack.mStage = BlackCheck::STAGE1_SETTLE;
                sBlack.mSettle = 2;
                sBlack.mGlBlackRun = sBlack.mGlLitRun = 0;
            }
            else if (LLTimer::getTotalSeconds() - sBlack.mStarted > 90.0)
            {
                end_check(llformat("ended after 90 s: %u probes, last GL max %u", sBlack.mProbes, p.mGlMax), false);
            }
            break;
        case BlackCheck::STAGE1_SETTLE:
        case BlackCheck::STAGE2_SETTLE:
            if (sBlack.mSettle > 0) sBlack.mSettle--;
            if (sBlack.mSettle == 0)
            {
                sBlack.mStage = (sBlack.mStage == BlackCheck::STAGE1_SETTLE) ? BlackCheck::STAGE1 : BlackCheck::STAGE2;
                sBlack.mGlBlackRun = sBlack.mGlLitRun = 0;
            }
            break;
        case BlackCheck::STAGE1:
            if (gl_black) { sBlack.mGlBlackRun++; sBlack.mGlLitRun = 0; } else { sBlack.mGlLitRun++; sBlack.mGlBlackRun = 0; }
            if (sBlack.mGlLitRun >= 3)
            {
                end_check(llformat("FAILED: the world came back (max %u) once OpenGL drew the simple pass itself: Vulkan's pass into the shared G-buffer (or the depth rewrite) blanked it", p.mGlMax), true);
                stopAtRunTime("Vulkan drew nothing on this computer: its pass into the G-buffer shared with OpenGL blanked the picture");
                return;
            }
            if (sBlack.mGlBlackRun >= 3)
            {
                LL_WARNS("WolfVulkanCheck") << "Still black with OpenGL drawing every pass: stage 2, the G-buffer goes back to OpenGL's own textures" << LL_ENDL;
                unshareGBuffer("world check stage 2");
                sBlack.mStage = BlackCheck::STAGE2_SETTLE;
                sBlack.mSettle = 3;
            }
            break;
        case BlackCheck::STAGE2:
            if (gl_black) { sBlack.mGlBlackRun++; sBlack.mGlLitRun = 0; } else { sBlack.mGlLitRun++; sBlack.mGlBlackRun = 0; }
            if (sBlack.mGlLitRun >= 3)
            {
                end_check(llformat("FAILED: the world came back (max %u) once the G-buffer was OpenGL's own: the G-buffer shared with Vulkan came back black", p.mGlMax), true);
                stopAtRunTime("Vulkan drew nothing on this computer: the G-buffer shared between OpenGL and Vulkan came back black");
                return;
            }
            if (sBlack.mGlBlackRun >= 3)
            {
                // black with and without Vulkan's part: a dark scene. The normal mode comes back.
                sGBufferGLOnly = false;
                finishUnshare();
                if (sBlack.mGBufferWasShared && !sGBuffer) shareGBuffer("restored after the world check");
                end_check("ended: OpenGL's frame black with and without the shared G-buffer - a dark scene, not a Vulkan fault; sharing restored", false);
            }
            break;
        default:
            break;
        }
    }
}

extern bool gCubeSnapshot;   // pipeline.cpp:269 (a reflection probe / mirror render is in progress: its G-buffer is not the shared one)

namespace WolfVkBridge
{

void decideBackend()
{
    // [RUN-TIME STOP 2026-11-09] the last run's Vulkan stop when its message box could not be shown (stopAtRunTime)
    const std::string stopped = gSavedSettings.getString("WolfVulkanStoppedReason");
    if (!stopped.empty())
    {
        gSavedSettings.setString("WolfVulkanStoppedReason", "");
        rememberNotice("WolfVulkanStoppedLastRun", stopped);
    }
    // [W102 2026-11-10] Once, at the first start of a viewer with this code, a saved "opengl" goes back to Vulkan.
    // W101 shipped without its SPIR-V (no glslang on CI, indra/cmake/VulkanShaders.cmake), so every W101 Vulkan start
    // that reached the scene fell back (WolfVkScene::init, wolfvkscene.cpp: the composite pair is missing) and fallBack()
    // saved "opengl". A saved "opengl" can only come from W101 or later: WolfRenderBackend first appeared in W101
    // (W100's settings.xml has no such key), and settings are saved with nondefault_only (llappviewer.cpp:2237 at quit,
    // decideBackend/stopAtRunTime saveToFile(..., true); llcontrol.cpp LLControlVariable::shouldSave, Persist 1 =
    // PERSIST_NONDFT), so "opengl" is written only because it differs
    // from the default "vulkan". Within W101 it means that fall-back for nearly everyone on Windows and Linux, or a V
    // click in W101's few hours (rare; the V button and Wolf World > Vulkan Renderer still undo it). W101 recorded the
    // fall-back reason in no setting, so the reason cannot be told apart (Paul's coordinator, 2026-11-10: retry them all).
    // A driver that then crashes before the login screen is still caught by WolfVulkanStartOK at the next start.
    // Not on macOS: there Vulkan falls back by design (no MoltenVK in the bundle, vk/wolfvkloader.cpp), so a Mac retry
    // would only show the fall-back message again. WolfVulkanW101SpvRetryDone is set whatever is found, so nobody is
    // switched twice. A second instance leaves it to the first (WolfCrashReport::checkDriverCrash does the same).
    if (!gSavedSettings.getBOOL("WolfVulkanW101SpvRetryDone") && !LLAppViewer::instance()->isSecondInstance())
    {
        gSavedSettings.setBOOL("WolfVulkanW101SpvRetryDone", true);
#if !LL_DARWIN
        if (gSavedSettings.getString("WolfRenderBackend") == "opengl")
        {
            LL_INFOS("Vulkan") << "WolfRenderBackend was saved as opengl by W101, whose download had no Vulkan shaders; "
                                  "trying Vulkan again once" << LL_ENDL;
            gSavedSettings.setString("WolfRenderBackend", "vulkan");
            gSavedSettings.setBOOL("WolfVulkanStartOK", true);   // as chooseBackend: the first-run marker must not stop this try
        }
#endif
    }
    const std::string backend = gSavedSettings.getString("WolfRenderBackend");
    sRequested = false;
    if (backend != "vulkan")
    {
        return;
    }
    if (!gSavedSettings.getBOOL("WolfVulkanStartOK"))
    {
        // The last Vulkan start never drew the login screen (crash or hang before it): OpenGL this time, and say so.
        LL_WARNS("Vulkan") << "The previous Vulkan start did not reach the login screen; starting in OpenGL" << LL_ENDL;
        gSavedSettings.setString("WolfRenderBackend", "opengl");
        gSavedSettings.setBOOL("WolfVulkanStartOK", true);
        sState = State::FELL_BACK;
        sLastError = "the previous start with the Vulkan renderer did not get as far as the login screen";
        rememberNotice("WolfVulkanFellBackLastStart", sLastError);
        return;
    }
    sRequested = true;
    sState = State::VULKAN_STARTING;
    // [VULKAN WAYLAND 2026-11-09] the window this start makes goes through XWayland, not native Wayland (llwindow.h)
    LLWindow::sWolfAvoidNativeWayland = true;
    // Like RenderInitError (llappviewer.cpp:3952): the marker is false until the login screen has been drawn, and
    // written to disk at once in case this start crashes.
    gSavedSettings.setBOOL("WolfVulkanStartOK", false);
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
}

bool vulkanRequested()
{
    return sRequested;
}

void initAfterGL(LLWindow* window)
{
    sWindow = window;
    if (!sRequested || sState != State::VULKAN_STARTING)
    {
        probeInfo(false);
        return;
    }

    probeInfo(true);
    std::string error;
    if (!gWolfVkDevice.hasInstance())
    {
        fallBack(sLastError.empty() ? std::string("Vulkan is not available on this computer") : sLastError, "WolfVulkanFellBack");
        return;
    }
    // [VULKAN WAYLAND 2026-11-09] A native Wayland window (only when SDL_VIDEODRIVER forced it: a Vulkan start otherwise
    // runs through XWayland, llwindow.h sWolfAvoidNativeWayland): its wl_surface already carries SDL's EGL window surface
    // (SDL 2.28.0 SDL_waylandwindow.c:2082-2087). Only Mesa's RADV has been seen to share it (amd-wl-vk, 8.2o); NVIDIA
    // failed vkCreateSwapchainKHR with VK_ERROR_SURFACE_LOST_KHR. Proven by the protocol trace (chain39 nv-paul-repro,
    // WAYLAND_DEBUG=client): the compositor answered NVIDIA's Vulkan swapchain with `wl_display#1.error(nil, 0, "Surface
    // already has a fifo")` - NVIDIA's EGL window surface had already given the wl_surface its wp_fifo_v1 object, a
    // second one is a protocol error, and a protocol error ends the client's connection. SDL then either quits
    // (SDL_waylandevents.c:372-385, reconnect compiled out at SDL_waylandvideo.c:1118 - Paul's 12:06Z run) or spins
    // for ever in Wayland_ShowWindow waiting for a configure on the dead connection (SDL_waylandwindow.c:1422-1426 - the
    // frozen splash of nv-paul-repro). Mesa's EGL takes no fifo object (platform_wayland.c), which is why RADV shares the
    // surface. Any driver but RADV therefore stays on OpenGL here, before a Vulkan surface
    // touches the window - the window and its connection are then untouched and OpenGL carries on in it.
    {
        LLWindow::NativeHandles nh;
        if (window && window->getNativeHandles(nh) && nh.mKind == LLWindow::NativeHandles::KIND_WAYLAND)
        {
            const WolfVkDevice::Config& cfg = gWolfVkDevice.config();
            bool radv = false;
            for (const WolfVkPhysicalDeviceInfo& d : gWolfVkDevice.physicalDevices())
            {
                // the device OpenGL runs on (VkPhysicalDeviceIDProperties::deviceUUID = GL_DEVICE_UUID_EXT), and its
                // driver (VkPhysicalDeviceDriverProperties::driverID, VK_DRIVER_ID_MESA_RADV vulkan_core.h:6357)
                if (cfg.mHaveGLDeviceUUID && memcmp(d.mIds.deviceUUID, cfg.mGLDeviceUUID, VK_UUID_SIZE) == 0)
                {
                    radv = d.mDriver.driverID == VK_DRIVER_ID_MESA_RADV;
                }
            }
            // test only: WolfVulkanDebugWaylandAnyDriver lets the rig reproduce the failure (with WAYLAND_DEBUG=client)
            static LLCachedControl<bool> any_driver(gSavedSettings, "WolfVulkanDebugWaylandAnyDriver", false);
            if (!radv && !any_driver)
            {
                fallBack("on a native Wayland window this graphics driver cannot show Vulkan and OpenGL in the same window "
                         "(start WolfViewer without SDL_VIDEODRIVER=wayland to use Vulkan)", "WolfVulkanFellBack");
                return;
            }
        }
    }
    // [T724 2026-11-10] on X11 the surface is made on a child window of the viewer's window, which OpenGL's GLX drawable
    // never touches (wolfvksurface.h: NVIDIA on a pure X11 session refused a swapchain on the GL window itself with
    // VK_ERROR_INITIALIZATION_FAILED). WolfVulkanDebugSharedWindow (hidden test setting) keeps the W101/W102 way.
    sSurface = WolfVkSurface::create(gWolfVkDevice.instance(), window, gSavedSettings.getBOOL("WolfVulkanDebugSharedWindow"), error);
    if (sSurface == VK_NULL_HANDLE)
    {
        fallBack(error, "WolfVulkanFellBack");
        return;
    }
    // [AMD PERF 2026-11-09] debug: log each attachment image's VkImageCompressionPropertiesEXT (WolfVkImage)
    WolfVkDevice::setCompressionQueryWanted(gSavedSettings.getBOOL("WolfVulkanDebugCompressionQuery"));
    if (!gWolfVkDevice.createDevice(sSurface, error))
    {
        fallBack(error, "WolfVulkanFellBack");
        return;
    }
    sVulkanDeviceLine = gWolfVkDevice.aboutLine();
    if (!createSwapchain(error))
    {
        // [T724] with WolfVkSwapchain's own block (what was asked, what the surface offered): the windows the surface sits on
        const std::string where = WolfVkSurface::describe();
        if (!where.empty()) LL_WARNS("Vulkan") << "Swapchain refused on: " << where << LL_ENDL;
        fallBack(error, "WolfVulkanFellBack");
        return;
    }
    sScene = new WolfVkScene();
    if (!sScene->init(gWolfVkDevice, sSwapchain, error))
    {
        fallBack(error, "WolfVulkanFellBack");
        return;
    }
    sState = State::VULKAN_ACTIVE;
    LL_INFOS("Vulkan") << "Vulkan renderer active: " << sVulkanDeviceLine << ", surface " << WolfVkSurface::kind() << LL_ENDL;
    prof::initStamps();   // [AMD PERF 2026-11-08] Vulkan timestamps for the WolfLogFrameRate breakdown (none when the queue has no timestamps)

    shareGBuffer("start");
}

bool isActive()
{
    return sState == State::VULKAN_ACTIVE;
}

void onResize()
{
    sResizePending = true;
}

void captureWorld()
{
    if (sState == State::VULKAN_ACTIVE && sScene)
    {
        sScene->poseCheckVertices();   // [POSE CHECK 2026-11-08] (WolfVulkanDebugPoseCheck) every pass of the frame is drawn now
        prof::Scope t(prof::CAPTURE);
        const bool timed = prof::on();
        if (timed) { prof::sGlCaptureSpan.begin(); prof::sGlCapture.begin(); }
        sScene->captureWorld();
        if (timed) { prof::sGlCapture.end(); prof::sGlCaptureSpan.end(); }
    }
}

// The frame slot is opened once per frame: by the first mid-frame pass (drawPassSimple) or, failing that, at present().
// Opening it twice would reset this slot's descriptor pools under a submit already on the queue.
bool ensureFrameBegun()
{
    // [HANG GUARD 2026-11-08 8.2k] a wait elsewhere (one-time upload, shared-image resize, GL finish, swapchain acquire)
    // may have marked the device hung since the last frame began: the same fall-back as a frame that never finished
    if (gWolfVkDevice.hung())
    {
        stopAtRunTime("the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")");
        return false;
    }
    if (sFrameBegun) return true;
    {
        prof::Scope t(prof::BEGIN_WAIT);
        gWolfVkDevice.beginFrame();
    }
    if (gWolfVkDevice.hung())
    {
        stopAtRunTime("the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")");
        return false;
    }
    prof::readSlot();                // [AMD PERF] this slot's timestamps from its last use are final now
    prof::sPassThisFrame = false;
    sScene->beginFrame();            // this slot's descriptor pools reset
    gWolfVkDevice.flushDeferred();   // then its deferred destructions (wolfvkdevice.h flushDeferred)
    sScene->clearDrawnByVulkan();    // a new frame: nothing drawn by Vulkan yet
    sFrameBegun = true;
    return true;
}

// [MAIN PASS ONLY 2026-11-08] Vulkan takes a pass over only while GL is drawing the main camera into the very buffers that
// are shared: the main render target's deferredScreen is the bound target. LLPipeline::renderGeomDeferred also runs for
// the reflection probes and mirrors (gCubeSnapshot, mRT switched to mAuxillaryRT / mHeroProbeRT: pipeline.cpp:922-941),
// for the avatar impostors (generateImpostor, pipeline.cpp:12291, sImpostorRender: mRT stays mMainRT but the draws go to
// the avatar's impostor target with the impostor camera) and for the shadow maps (sShadowRender). Found by the
// same-session G-buffer dump of 18:02Z: it caught an impostor pass - palm trees from the impostor camera in the shared
// attachments, and the impostor itself missing every PASS_SIMPLE batch that Vulkan had taken.
static bool mainGBufferBound()
{
    return gPipeline.mRT == &gPipeline.mMainRT && !gCubeSnapshot && !LLPipeline::sImpostorRender && !LLPipeline::sShadowRender
        && LLRenderTarget::getCurrentBoundTarget() == &gPipeline.mRT->deferredScreen;
}

void dumpGBufferIfAsked()
{
    // [G-BUFFER COMPARE 2026-11-08] WolfVulkanDebugDumpGBuffer: after the simple pass (whichever renderer drew it), the five
    // shared attachments to the logs folder, once; the setting clears itself. The read-back waits for the card.
    if (sState != State::VULKAN_ACTIVE || !sScene || !sGBuffer) return;
    if (!gSavedSettings.getBOOL("WolfVulkanDebugDumpGBuffer")) return;
    if (!mainGBufferBound()) return;   // not consumed: the next main pass dumps
    gSavedSettings.setBOOL("WolfVulkanDebugDumpGBuffer", false);
    if (!sGBuffer->prepare()) return;
    if (!ensureFrameBegun()) return;
    VkCommandBuffer cmd = gWolfVkDevice.beginMidFrame();
    if (cmd == VK_NULL_HANDLE) return;
    std::string error;
    // [HAND-OVER 2026-11-08 8.2k] the same rules as drawPassSimple: nothing recorded or submitted unless GL really
    // signalled; a failed submit or hand-back falls back (before, a failed submit returned with sync() having marked
    // vk_pass_done "signalled" - the next glTakeBack would have made GL wait on a signal that was never submitted)
    if (!sGBuffer->glHandToVulkan())
    {
        gWolfVkDevice.discardMidFrame(cmd);
        if (sGBuffer->broken()) stopAtRunTime("G-buffer hand-over: " + sGBuffer->brokenReason());
        return;
    }
    const bool recorded = sGBuffer->recordDump(cmd, error);   // releases the images back to GL even when it fails
    if (!gWolfVkDevice.submitMidFrame(cmd, sGBuffer->sync(), error))
    {
        stopAtRunTime("G-buffer dump submit: " + error);
        return;
    }
    if (!sGBuffer->glTakeBack())
    {
        stopAtRunTime("G-buffer hand-back: " + sGBuffer->brokenReason());
        return;
    }
    if (!gWolfVkDevice.waitIdle()) return;   // debug: the host buffers are read now (bounded; hung -> next frame falls back)
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "");
    if (recorded && sGBuffer->writeDump(dir, error)) LL_INFOS("Vulkan") << "G-buffer dumped to " << dir << " (gbuf_0..4 raw, " << (gSavedSettings.getBOOL("WolfVulkanDebugGBufferGLOnly") ? "GL drew the simple pass" : "Vulkan drew the simple pass") << ")" << LL_ENDL;
    else LL_WARNS("Vulkan") << "G-buffer dump: " << error << LL_ENDL;
    // [BLUE DASHES 2026-11-08] and the same attachments as GL reads them, same frame, same memory (wolfvkgbuffer.h)
    const std::string prefix = gSavedSettings.getBOOL("WolfVulkanDebugGBufferGLOnly") ? "glpass" : "vkpass";
    if (sGBuffer->writeGLViewDump(dir, prefix, error)) LL_INFOS("Vulkan") << "G-buffer as GL sees it dumped (gbufglview_" << prefix << "_0..4)" << LL_ENDL;
    else LL_WARNS("Vulkan") << "G-buffer GL-view dump: " << error << LL_ENDL;
}

bool batchDrawnByVulkan(const LLDrawInfo* params)
{
    // only while GL draws the leftovers of the very pass Vulkan drew (ScopedSkipDrawn): the same draw infos go through
    // pushBatches again for the shadow maps, reflection probes and impostors, where GL must draw them all
    return sSkipDrawnDepth > 0 && sState == State::VULKAN_ACTIVE && sScene && params && sScene->drawnByVulkan(params);
}

void poseCheckModelMatrix(const LLMatrix4* model_matrix)
{
    static LLCachedControl<bool> pose_check(gSavedSettings, "WolfVulkanDebugPoseCheck", false);
    if (!pose_check || sState != State::VULKAN_ACTIVE || !sScene || !mainGBufferBound()) return;
    sScene->poseCheckMatrix(model_matrix);
}

bool timingOn()
{
    static LLCachedControl<bool> log_rate(gSavedSettings, "WolfLogFrameRate", false);
    static LLCachedControl<bool> rate_only(gSavedSettings, "WolfLogFrameRateOnly", false);
    return log_rate && !rate_only;
}

void glFrameStamp()
{
    if (!timingOn() || !gGLManager.mInited) return;
    // a ring of GL_TIMESTAMP queries (glQueryCounter, OpenGL 4.6 4.3 "Time Queries"); each read only once
    // GL_QUERY_RESULT_AVAILABLE says so, oldest first, so nothing here waits for the GPU
    constexpr int N = 8;
    static GLuint ids[N] = {};
    static bool pending[N] = {};
    static int head = 0, tail = 0;   // issue at head, read at tail
    static GLuint64 last = 0;
    static double sum = 0.0;
    static uint32_t count = 0;
    static F64 window = 0.0;
    if (!ids[0]) glGenQueries(N, ids);
    while (pending[tail])
    {
        GLuint avail = 0;
        glGetQueryObjectuiv(ids[tail], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;
        GLuint64 t = 0;
        glGetQueryObjectui64v(ids[tail], GL_QUERY_RESULT, &t);
        if (last != 0 && t > last) { sum += static_cast<double>(t - last) / 1.0e6; count++; }
        last = t;
        pending[tail] = false;
        tail = (tail + 1) % N;
    }
    if (!pending[head])
    {
        glQueryCounter(ids[head], GL_TIMESTAMP);
        pending[head] = true;
        head = (head + 1) % N;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (now - window >= 1.0)
    {
        window = now;
        if (count)
        {
            LL_INFOS("WolfVulkanFrame") << "GL GPU frame interval " << llformat("%.2f", sum / count) << " ms (" << count << " frames, "
                                        << (sState == State::VULKAN_ACTIVE ? (sGBuffer && sGBuffer->ready() ? "Vulkan, shared G-buffer" : "Vulkan, G-buffer not shared") : "OpenGL")
                                        << ")" << LL_ENDL;
        }
        sum = 0.0;
        count = 0;
    }
}

namespace
{
    // [AMD PERF 2026-11-09] the main loop's stages (LoopTimer): wall and main-thread CPU, summed over a second
    double loopWallMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // the calling thread's CPU time in ms. POSIX clock_gettime(CLOCK_THREAD_CPUTIME_ID) (Linux, macOS 10.12+); Windows
    // GetThreadTimes kernel + user time in 100-ns units (its resolution is the scheduler tick, ~15.6 ms, so per-stage
    // figures there are only meaningful summed over many frames)
    double loopCpuMs()
    {
#if LL_WINDOWS
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
        const ULONGLONG k = (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        const ULONGLONG u = (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        return static_cast<double>(k + u) / 1.0e4;
#else
        timespec ts{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
        return static_cast<double>(ts.tv_sec) * 1.0e3 + static_cast<double>(ts.tv_nsec) / 1.0e6;
#endif
    }
    constexpr int kLoopStages = static_cast<int>(LoopStage::kCount);
    const char* const kLoopStageName[kLoopStages] = { "input", "idle", "display", "swap/present", "sleep block" };
    double sLoopWall[kLoopStages] = {}, sLoopCpu[kLoopStages] = {};
    double sLoopFrameWall = 0.0, sLoopFrameCpu = 0.0;   // whole frames, end to end
    double sLoopLastWall = -1.0, sLoopLastCpu = 0.0, sLoopWindow = 0.0;
    uint32_t sLoopFrames = 0;
    bool loopOn() { return timingOn(); }
}

LoopTimer::LoopTimer(LoopStage stage) : mStage(stage), mOn(loopOn()), mWall0(0.0), mCpu0(0.0)
{
    if (mOn) { mWall0 = loopWallMs(); mCpu0 = loopCpuMs(); }
}

LoopTimer::~LoopTimer()
{
    if (!mOn) return;
    const int k = static_cast<int>(mStage);
    sLoopWall[k] += loopWallMs() - mWall0;
    sLoopCpu[k] += loopCpuMs() - mCpu0;
}

void loopFrameEnd()
{
    if (!loopOn()) { sLoopLastWall = -1.0; return; }
    const double wall = loopWallMs(), cpu = loopCpuMs();
    if (sLoopLastWall < 0.0)
    {
        // first frame with logging on: start clean
        sLoopLastWall = wall; sLoopLastCpu = cpu; sLoopWindow = wall;
        for (int k = 0; k < kLoopStages; ++k) sLoopWall[k] = sLoopCpu[k] = 0.0;
        sLoopFrameWall = sLoopFrameCpu = 0.0; sLoopFrames = 0;
        return;
    }
    sLoopFrameWall += wall - sLoopLastWall;
    sLoopFrameCpu += cpu - sLoopLastCpu;
    sLoopLastWall = wall; sLoopLastCpu = cpu;
    sLoopFrames++;
    if (wall - sLoopWindow < 1000.0) return;
    sLoopWindow = wall;
    const double n = static_cast<double>(sLoopFrames);
    const int disp = static_cast<int>(LoopStage::kDisplay), swp = static_cast<int>(LoopStage::kSwap);
    // DISPLAY without the swap inside it
    double wall_k[kLoopStages], cpu_k[kLoopStages];
    for (int k = 0; k < kLoopStages; ++k) { wall_k[k] = sLoopWall[k]; cpu_k[k] = sLoopCpu[k]; }
    wall_k[disp] -= wall_k[swp];
    cpu_k[disp] -= cpu_k[swp];
    double wall_sum = 0.0, cpu_sum = 0.0;
    std::string wall_s, blocked_s;
    for (int k = 0; k < kLoopStages; ++k)
    {
        wall_sum += wall_k[k]; cpu_sum += cpu_k[k];
        wall_s += llformat("%s %.2f, ", kLoopStageName[k], wall_k[k] / n);
        blocked_s += llformat("%s %.2f, ", kLoopStageName[k], (wall_k[k] - cpu_k[k]) / n);
    }
    const double other_wall = sLoopFrameWall - wall_sum, other_cpu = sLoopFrameCpu - cpu_sum;
    LL_INFOS("WolfVulkanFrame") << "main loop per frame (" << sLoopFrames << " frames, "
                                << (sState == State::VULKAN_ACTIVE ? "Vulkan" : "OpenGL") << "): wall ms " << wall_s
                                << "other " << llformat("%.2f", other_wall / n) << ", frame " << llformat("%.2f", sLoopFrameWall / n)
                                << "; blocked (wall - main thread CPU) ms " << blocked_s << "other " << llformat("%.2f", (other_wall - other_cpu) / n)
                                << ", frame " << llformat("%.2f", (sLoopFrameWall - sLoopFrameCpu) / n) << LL_ENDL;
    for (int k = 0; k < kLoopStages; ++k) sLoopWall[k] = sLoopCpu[k] = 0.0;
    sLoopFrameWall = sLoopFrameCpu = 0.0;
    sLoopFrames = 0;
}

ScopedSkipDrawn::ScopedSkipDrawn() { sSkipDrawnDepth++; }
ScopedSkipDrawn::~ScopedSkipDrawn() { if (sSkipDrawnDepth > 0) sSkipDrawnDepth--; }

bool drawPassSimple()
{
    if (sState != State::VULKAN_ACTIVE || !sScene || !sGBuffer) return false;
    static LLCachedControl<bool> no_gbuffer(gSavedSettings, "WolfVulkanDebugNoGBuffer", false);
    if (no_gbuffer) return false;
    // only the main G-buffer is shared (reflection probes, mirrors, impostors and shadow maps render into other targets)
    if (!mainGBufferBound() || !sGBuffer->prepare()) return false;
    // [G-BUFFER COMPARE 2026-11-08] GL draws the pass into the shared buffers instead (the dump then shows GL's result)
    static LLCachedControl<bool> gl_only(gSavedSettings, "WolfVulkanDebugGBufferGLOnly", false);
    if (gl_only) return false;
    if (sGBufferGLOnly) return false;   // [BLACK WORLD 2026-11-10] world check stage 1/2: no Vulkan pass, no depth rewrite
    if (!ensureFrameBegun()) return false;
    VkCommandBuffer cmd = gWolfVkDevice.beginMidFrame();
    if (cmd == VK_NULL_HANDLE) return false;
    // GL -> Vulkan: the five attachments with everything GL drew so far.
    // [HAND-OVER 2026-11-08 8.2k] Only a real signal lets the pass go on: on false nothing is recorded, the command buffer
    // is discarded unsubmitted and GL draws the whole pass (this function returns false), so the Vulkan queue never
    // waits on a gl_pass_done that was not signalled and GL never waits on a vk_pass_done that was not submitted. The
    // GPU-side waits (the submit's wait on gl_pass_done, GL's glWaitSemaphoreEXT on vk_pass_done) take no timeout; they
    // complete by this construction alone: one signal, then one wait, each submitted before its wait.
    bool handed = false;
    const bool time_hand_over = prof::on();
    {
        prof::Scope t(prof::PASS_HAND);   // [AMD PERF]
        if (time_hand_over) prof::sGlHandOver.begin();
        handed = sGBuffer->glHandToVulkan();
    }
    if (!handed)
    {
        if (time_hand_over) prof::sGlHandOver.cancel();
        gWolfVkDevice.discardMidFrame(cmd);
        if (sGBuffer->broken())
        {
            stopAtRunTime("G-buffer hand-over: " + sGBuffer->brokenReason());
        }
        return false;
    }
    // [AMD PERF] the first pass of the frame is timed on the GPU (a VR frame has two)
    const bool stamp_pass = prof::on() && prof::sPool != VK_NULL_HANDLE && !prof::sPassThisFrame;
    {
        prof::Scope t(prof::PASS_RECORD);
        if (stamp_pass) prof::stamp(cmd, 0, true);
        sGBuffer->acquire(cmd);
        sGBufferDraws = sScene->drawGBufferSimple(cmd, *sGBuffer->target());
        sGBuffer->release(cmd);
        if (stamp_pass)
        {
            prof::stamp(cmd, 0, false);
            prof::sPassWritten[gWolfVkDevice.frameIndex()] = 1;
            prof::sPassThisFrame = true;
        }
    }
    std::string error;
    bool submitted = false;
    {
        prof::Scope t(prof::PASS_SUBMIT);
        submitted = gWolfVkDevice.submitMidFrame(cmd, sGBuffer->sync(), error);
    }
    if (!submitted)
    {
        stopAtRunTime("G-buffer pass: " + error);
        return false;
    }
    // Vulkan -> GL: GL's queue waits for the pass before the next pool draws
    bool taken = false;
    {
        prof::Scope t(prof::PASS_TAKEBACK);
        taken = sGBuffer->glTakeBack();
        if (time_hand_over) prof::sGlHandOver.end();
    }
    if (!taken)
    {
        // GL did not wait: it must not draw into images Vulkan may still be writing, and the semaphore state is unknown
        stopAtRunTime("G-buffer hand-back: " + sGBuffer->brokenReason());
        return false;
    }
    // [BLUE DASHES 2026-11-08] OpenGL rewrites the shared depth itself before its later passes (wolfvkgbuffer.h glRewriteDepth).
    // Proven necessary by run nv-dz1: GL's reads of the depth were byte-identical to what Vulkan wrote (viewcheck, nv-fz2),
    // yet GL's later depth-TESTED passes let the water and sky through the ships in short runs ("blue dashes"); with this
    // rewrite the first Vulkan grab was clean and switching it off brought the dashes back. EXT_external_objects 4.2.3 offers
    // only the layout passed to the wait (already DEPTH_STENCIL_ATTACHMENT <-> VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    // Table 4.4) to "initialize internal GL state"; that did not reset the driver's depth-test state here.
    static LLCachedControl<bool> gl_depth_rewrite(gSavedSettings, "WolfVulkanGLDepthRewrite", true);
    if (gl_depth_rewrite)
    {
        prof::Scope t(prof::PASS_REWRITE);
        sGBuffer->glRewriteDepth();
    }

    // an incomplete pass (a batch skipped: its buffer copy still uploading, the uniform ring full) is drawn again by GL in
    // full - the same geometry into the same attachments, so nothing is missing and nothing shows twice
    return sScene->gbufferPassComplete();
}

// [HAND-OVER / HANG GUARD 2026-11-08 8.2k] A frame that reaches no swapchain image (acquire out of date, or a swapchain
// that could not be rebuilt) still ends its slot with a real submit of the slot's own command buffer and fence, carrying
// the hand-over's semaphores. Before, the slot was "abandoned": its command buffer ended unsubmitted and a separate
// semaphore-only submit (no fence, its failure only logged) balanced the GL semaphores. That left three holes:
//   - after a mid-frame G-buffer pass the slot was marked submitted while its frame fence (reset in beginFrame) was never
//     handed to the queue, so the slot's next beginFrame waited 8 s on that fence and declared a hang that was none
//     (wolfvkdevice.cpp beginFrame, mFenceSubmitted);
//   - a failed semaphore submit left vk_done marked signalled (WolfVkShare::sync) though it never was, and GL's next
//     glWaitSemaphoreEXT on it would have waited on the card for ever;
//   - commands already recorded on the slot's buffer (none today, but nothing stopped it) were silently dropped.
// The submit waits on gl_done only if GL signalled it (WolfVkShare::sync) and signals vk_done only if GL is owed one.
static bool closeFrameWithoutImage(std::string& error)
{
    const WolfVkDevice::ExtraSync sync = sScene->frameSync();
    return gWolfVkDevice.endFrame(false, VK_NULL_HANDLE, sync, error);
}

namespace
{
bool presentFrame();
}

void endUnshownFrame()
{
    // [VR 3D POOL 2026-11-09] the VR 3D Mode crash (Paul, 2026-11-09; the VR agent's finding, reproduced twice at
    // Community Hub): with the desktop paused (WolfVRScreen::displayFrame, WolfVRPauseDesktop) swap() never reaches
    // present(), so the slot that the eye passes opened (drawPassSimple -> ensureFrameBegun) was never submitted or
    // closed and its descriptor pool (8192 sets, wolfvkshader.cpp WolfVkFrameUniforms::create) was never reset: about
    // 3 s of eye passes later VK_ERROR_OUT_OF_POOL_MEMORY, and the composite bound the failed (null) set. The slot is
    // closed the way a frame without a swapchain image is (closeFrameWithoutImage: the GL semaphores balanced, the frame
    // fence handed to the queue).
    if (sState != State::VULKAN_ACTIVE || !sScene || !sFrameBegun) return;
    sFrameBegun = false;
    sScene->poseCheckFrameEnd();   // the frame's records end here, as in presentFrame
    std::string error;
    if (!closeFrameWithoutImage(error))
    {
        stopAtRunTime(gWolfVkDevice.hung() ? "the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")"
                                           : error);
    }
}

bool present()
{
    // [RUN-TIME STOP 2026-11-09] after a run-time failure nothing more reaches the window, by Vulkan or by OpenGL's own
    // swap (the caller swaps only on false), until the viewer has quit (stopAtRunTime) - also for the frame in which the
    // failure happened: every path out of presentFrame that stopped Vulkan returns false, which here becomes true
    if (sState == State::STOPPED)
    {
        return true;
    }
    const bool presented = presentFrame();
    return presented || sState == State::STOPPED;
}

namespace
{
bool presentFrame()
{
    if (sState != State::VULKAN_ACTIVE || !sScene)
    {
        return false;
    }
    // [AMD PERF 2026-11-09 8.2o] OpenGL's end-of-frame flush, which GL's skipped SwapBuffers would have made. Mesa 26.2.5
    // (tag mesa-26.2.5): with the gallium threaded context (radeonsi), glGetQueryObject*(GL_QUERY_RESULT_AVAILABLE)
    // reaches tc_get_query_result (src/gallium/auxiliary/util/u_threaded_context.c:1300-1310), which makes the calling
    // thread wait for the driver thread (tc_sync) for every query not yet marked flushed. Queries are marked flushed only
    // by tc_flush_queries (:3625), called from a tc_flush that goes through the context's flush (:3660 tc_call_flush,
    // :3728 the synchronous path); swapBuffers does that once a frame (frontends/dri/dri_drawable.c:584/594
    // st_context_flush -> st_flush, state_tracker/st_manager.c:787-809). glSignalSemaphoreEXT does not: it goes
    // tc_fence_server_signal (:3530-3540), straight to the driver. So in Vulkan mode, where GL never swaps, every occlusion
    // query the viewer polls (llvieweroctree.cpp:1134, llreflectionmap.cpp:381) made the main thread wait for the driver
    // thread: measured on the 890M as ~15% of the main thread in __futex_wait and 5.7-8 ms blocked a frame in display.
    // glFlush (main/context.c:1617-1622 _mesa_flush -> st_glFlush -> st_flush, state_tracker/st_cb_flush.c:51-63, 90-99)
    // is that flush. It is issued once per present, after this frame's gl_done signal (recordFrame below), on every
    // path out of here; it only submits GL's queued work, so the Vulkan/GL semaphore protocol is unchanged.
    struct EndOfFrameFlush { ~EndOfFrameFlush() { if (gGLManager.mInited && !gWolfVkDevice.hung()) glFlush(); } } end_of_frame_flush;
    const F64 t0 = LLTimer::getTotalSeconds();
    std::string error;
    finishUnshare();   // [BLACK WORLD 2026-11-10] a G-buffer given back last frame: the provider goes once the target released it

    // [VULKAN WAYLAND 2026-11-09] test only (WolfVulkanDebugFallBack, switches itself off): the run-time stop, as a lost
    // device takes it (stopAtRunTime: message box, next start in OpenGL, quit), so the rig can prove it on every window
    static LLCachedControl<bool> test_fallback(gSavedSettings, "WolfVulkanDebugFallBack", false);
    if (test_fallback)
    {
        gSavedSettings.setBOOL("WolfVulkanDebugFallBack", false);
        stopAtRunTime("test: WolfVulkanDebugFallBack");
        return false;
    }
    // [HANG GUARD / HAND-OVER 2026-11-08 8.2k] a device marked hung or a hand-over broken earlier in this frame (GL
    // capture, G-buffer pass, a bounded wait): fall back before anything else is waited on or submitted
    if (gWolfVkDevice.hung())
    {
        stopAtRunTime("the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")");
        return false;
    }
    if (sScene->handOverBroken())
    {
        stopAtRunTime("hand-over: " + sScene->handOverBrokenReason());
        return false;
    }

    if (sResizePending)
    {
        sResizePending = false;
        if (!createSwapchain(error))
        {
            if (gWolfVkDevice.hung())
            {
                stopAtRunTime("the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")");
                return false;
            }
            // a minimised window has no size: skip this frame, try again next time. A slot opened mid-frame by a G-buffer
            // pass is closed with a real submit, so every frame still ends its slot (one slot per frame).
            LL_DEBUGS("Vulkan") << "swapchain not rebuilt: " << error << LL_ENDL;
            sResizePending = true;
            if (sFrameBegun)
            {
                sFrameBegun = false;
                std::string close_error;
                if (!closeFrameWithoutImage(close_error))
                {
                    stopAtRunTime(close_error);
                    return false;
                }
            }
            return true;
        }
        sScene->onSwapchainChanged(sSwapchain);
        if (gWolfVkDevice.hung() || sScene->handOverBroken())   // the shared layers' resize ran out of time / failed
        {
            stopAtRunTime(gWolfVkDevice.hung() ? "the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")"
                                          : "hand-over: " + sScene->handOverBrokenReason());
            return false;
        }
    }

    if (!ensureFrameBegun())
    {
        return false;
    }
    sFrameBegun = false;   // this frame's slot closes with the submit below (with or without a swapchain image)
    sScene->poseCheckFrameEnd();   // [POSE CHECK 2026-11-08] the frame's records end here
    WolfVkDevice::Frame& frame = gWolfVkDevice.currentFrame();
    uint32_t image_index = 0;
    bool out_of_date = false;
    bool timed_out = false;
    bool acquired = false;
    {
        prof::Scope t(prof::PRESENT_ACQUIRE);   // [AMD PERF] any CPU wait for a swapchain image lands here
        acquired = sSwapchain.acquire(frame.mAcquire, image_index, out_of_date, timed_out, error);
    }
    if (!acquired)
    {
        if (gWolfVkDevice.hung())   // VK_ERROR_DEVICE_LOST from the acquire: nothing more may be submitted
        {
            stopAtRunTime("the graphics card did not finish a Vulkan frame (" + gWolfVkDevice.hungReason() + ")");
            return false;
        }
        std::string close_error;
        if (!closeFrameWithoutImage(close_error))
        {
            stopAtRunTime(close_error);
            return false;
        }
        if (out_of_date)
        {
            sResizePending = true;
            return true;
        }
        if (timed_out)
        {
            // [HANG GUARD 2026-11-08 8.2k] no image yet (a withheld/hidden window, WolfVkSwapchain::acquire): this frame
            // is skipped, its slot closed above; a GPU that has really stopped shows at the next beginFrame's fence wait
            LL_DEBUGS("Vulkan") << "frame skipped: " << error << LL_ENDL;
            return true;
        }
        stopAtRunTime(error);
        return false;
    }

    // The frame: the GL layers composited and the Vulkan scene pass, into this swapchain image.
    bool recorded = false;
    {
        prof::Scope t(prof::PRESENT_RECORD);   // includes OpenGL's UI capture + gl_done signal (readBackUI)
        const bool stamp_frame = prof::on() && prof::sPool != VK_NULL_HANDLE;
        if (stamp_frame) prof::stamp(frame.mCmd, 2, true);
        if (prof::on()) prof::sGlFrameRecord.begin();
        recorded = sScene->recordFrame(frame.mCmd, sSwapchain, image_index);
        if (prof::on()) prof::sGlFrameRecord.end();
        if (stamp_frame)
        {
            prof::stamp(frame.mCmd, 2, false);
            prof::sFrameWritten[gWolfVkDevice.frameIndex()] = 1;
        }
    }
    if (!recorded)
    {
        // GL's UI capture did not signal gl_done: this frame's submit would wait on a signal that never happened
        stopAtRunTime("hand-over: " + sScene->handOverBrokenReason());
        return false;
    }

    // [SHARED IMAGES 2026-11-08] wait for GL's gl_done (the layers are written), signal vk_done (they may be rewritten)
    const WolfVkDevice::ExtraSync sync = sScene->frameSync();
    bool ended = false;
    {
        prof::Scope t(prof::PRESENT_SUBMIT);
        ended = gWolfVkDevice.endFrame(true, sSwapchain.renderDone(image_index), sync, error);
    }
    if (!ended)
    {
        stopAtRunTime(error);
        return false;
    }
    bool present_out_of_date = false;
    bool presented = false;
    {
        prof::Scope t(prof::PRESENT_PRESENT);
        presented = sSwapchain.present(gWolfVkDevice.queue(), sSwapchain.renderDone(image_index), image_index, present_out_of_date, error);
    }
    if (!presented)
    {
        if (present_out_of_date)
        {
            sResizePending = true;
        }
        else
        {
            stopAtRunTime(error);
            return false;
        }
    }
    else if (present_out_of_date || out_of_date)
    {
        sResizePending = true;
    }

    // [BLACK WORLD 2026-11-10] the world check judges the newest probe sample now that the frame is out; a verdict against
    // Vulkan stops the renderer (stopAtRunTime), and this frame counts as shown either way
    blackCheck();
    if (sState == State::STOPPED) return false;

    const F64 t1 = LLTimer::getTotalSeconds();
    sPresentAccum += (t1 - t0) * 1000.0;
    sPresentCount++;
    if (prof::on()) prof::sFrames++;   // [AMD PERF 2026-11-09] no breakdown line under WolfLogFrameRateOnly
    if (t1 - sPresentWindowStart >= 1.0)
    {
        sPresentMillis = sPresentCount ? static_cast<F32>(sPresentAccum / sPresentCount) : 0.f;
        sPresentAccum = 0.0;
        sPresentCount = 0;
        sPresentWindowStart = t1;
        // [AMD PERF 2026-11-09] with the timing instruments only (WolfLogFrameRateOnly keeps just the frame-rate line)
        if (timingOn())
        {
            // [FLASH EVIDENCE 2026-11-08] what Vulkan drew and skipped last frame, and the mirror churn (buffers and textures
            // made or remade), once a second alongside WolfFrameRate
            const WolfVkMirror::Stats& st = WolfVkMirror::stats();
            const double rewrite_ms = sGBuffer ? sGBuffer->rewriteMsAndReset() : -1.0;
            LL_INFOS("WolfVulkanFrame") << "GL depth rewrite " << (rewrite_ms < 0.0 ? std::string("not timed") : llformat("%.3f ms (GPU)", rewrite_ms)) << "; "
                                        << "gbuffer draws " << sGBufferDraws << ", overlay draws " << sScene->drawsLastFrame()
                                        << ", skipped " << sScene->skippedLastFrameTotal()
                                        << "; mirrors: buffers " << st.mBuffersMade << " made " << st.mBuffersRemade << " remade " << st.mBufferBytesWritten / 1024 << " KB written, textures "
                                        << st.mTexturesMade << " made " << st.mTexturesRemade << " remade " << st.mTexturesDropped << " dropped (totals)" << LL_ENDL;
            // [AMD PERF 2026-11-08] the per-frame breakdown of the last second (see prof above): CPU ms per step, GPU ms
            if (prof::sFrames > 0)
            {
                const double n = static_cast<double>(prof::sFrames);
                std::string cpu;
                double cpu_total = 0.0;
                for (int k = 0; k < prof::STEP_COUNT; ++k)
                {
                    cpu += llformat("%s %.2f, ", prof::sStepName[k], prof::sCpu[k] / n);
                    cpu_total += prof::sCpu[k];
                    prof::sCpu[k] = 0.0;
                }
                uint32_t one_time = 0;
                double one_time_ms = 0.0;
                gWolfVkDevice.takeOneTimeStats(one_time, one_time_ms);
                auto mean = [](double sum, uint32_t count) { return count ? llformat("%.2f", sum / count) : std::string("-"); };
                LL_INFOS("WolfVulkanFrame") << "breakdown per frame (" << prof::sFrames << " frames): CPU ms " << cpu << "sum " << llformat("%.2f", cpu_total / n)
                                            << "; one-time submits " << llformat("%.1f", one_time / n) << " taking " << llformat("%.2f", one_time_ms / n)
                                            << " ms; GPU ms: Vulkan pass " << mean(prof::sGpuPass, prof::sGpuPassN) << ", Vulkan frame " << mean(prof::sGpuFrame, prof::sGpuFrameN)
                                            << ", GL world capture " << mean(prof::sGlCapture.mSum, prof::sGlCapture.mN)
                                            << ", GL UI capture " << mean(prof::sGlFrameRecord.mSum, prof::sGlFrameRecord.mN)
                                            << ", GL hand-over span " << mean(prof::sGlHandOver.mSum, prof::sGlHandOver.mN) << " ms (GL timestamps)"
                                            << ", GL world capture span " << mean(prof::sGlCaptureSpan.mSum, prof::sGlCaptureSpan.mN) << " ms (GL timestamps, incl. the vk_done wait)"
                                            << "; swapchain " << sSwapchain.presentModeString() << " " << sSwapchain.imageCount() << " images "
                                            << sSwapchain.extent().width << "x" << sSwapchain.extent().height << " " << sSwapchain.formatString() << LL_ENDL;
                prof::sFrames = 0;
                prof::sGpuPass = prof::sGpuFrame = 0.0;
                prof::sGpuPassN = prof::sGpuFrameN = 0;
                prof::sGlCapture.mSum = prof::sGlFrameRecord.mSum = 0.0;
                prof::sGlCapture.mN = prof::sGlFrameRecord.mN = 0;
                prof::sGlHandOver.mSum = 0.0;
                prof::sGlHandOver.mN = 0;
                prof::sGlCaptureSpan.mSum = 0.0;
                prof::sGlCaptureSpan.mN = 0;
            }
        }
    }
    return true;
}
} // namespace (presentFrame)

void onLoginScreenShown()
{
    if (sLoginShown)
    {
        return;
    }
    sLoginShown = true;
    if (sRequested)
    {
        // the login screen was drawn: this Vulkan start is a good one
        gSavedSettings.setBOOL("WolfVulkanStartOK", true);
        gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    }
    if (!sPendingNotice.empty())
    {
        LLSD args;
        args["REASON"] = sPendingReason;
        LLNotificationsUtil::add(sPendingNotice, args);
        sPendingNotice.clear();
    }
}

void shutdown()
{
    if (sState == State::VULKAN_ACTIVE) logSessionSummary("shutdown");   // [BLACK WORLD 2026-11-10]
    LLRenderTarget::sWolfVkProvider = nullptr;
    // [HANG GUARD 2026-11-08 8.2k] as fallBack: from the first step that finds the card hung, the rest is abandoned
    if (sGBuffer && !gWolfVkDevice.hung()) delete sGBuffer;
    sGBuffer = nullptr;
    if (!gWolfVkDevice.hung()) destroyShaderMirrors();
    if (sScene && !gWolfVkDevice.hung()) delete sScene;
    else WolfVkMirror::shutdown();
    sScene = nullptr;
    prof::destroyStamps(gWolfVkDevice.hasDevice() && !gWolfVkDevice.hung() && gWolfVkDevice.waitIdle());   // [AMD PERF]
    prof::sGlCapture.destroy();
    prof::sGlFrameRecord.destroy();
    prof::sGlHandOver.destroy();
    prof::sGlCaptureSpan.destroy();
    if (!gWolfVkDevice.hung()) sSwapchain.destroy();
    if (sSurface != VK_NULL_HANDLE && gWolfVkDevice.hasInstance() && !gWolfVkDevice.hung())
    {
        WolfVkSurface::destroy(gWolfVkDevice.instance(), sSurface);
    }
    sSurface = VK_NULL_HANDLE;
    gWolfVkDevice.destroy();
    if (sState == State::VULKAN_ACTIVE)
    {
        sState = State::OPENGL;
    }
    sWindow = nullptr;
}

std::string rendererLine()
{
    if (sState == State::VULKAN_ACTIVE)
    {
        return "Vulkan on " + gWolfVkDevice.chosen().name();
    }
    return "OpenGL on " + gGLManager.mGLRenderer;
}

std::string vulkanLine()
{
    if (!sVulkanDeviceLine.empty())
    {
        return sVulkanDeviceLine;
    }
    return LLTrans::getString("WolfVulkanNotAvailable");
}

LLSD reportFields()
{
    LLSD f;
    f["renderer"] = (sState == State::VULKAN_ACTIVE) ? "vulkan" : "opengl";
    f["vulkan_available"] = !sVulkanDeviceLine.empty();
    f["vulkan_device"] = sVulkanDeviceLine.empty() ? "" : gWolfVkDevice.hasInstance() ? gWolfVkDevice.chosen().name() : sVulkanDeviceLine;
    f["vulkan_driver"] = gWolfVkDevice.hasInstance() && gWolfVkDevice.chosen().mHandle ? gWolfVkDevice.chosen().driverString() : "";
    f["vulkan_api"] = gWolfVkDevice.hasInstance() && gWolfVkDevice.chosen().mHandle ? gWolfVkDevice.chosen().apiVersionString() : "";
    f["vulkan_loader"] = sLoaderName;
    f["vulkan_device_count"] = LLSD::Integer(sDeviceCount);
    f["vulkan_last_error"] = sLastError;
    f["vulkan_hand_over"] = sScene ? sScene->handOverLine() : "";
    f["vulkan_gbuffer"] = gbufferShared() ? std::string("shared") : sGBufferNotSharedWhy.empty() ? std::string("not shared")
                                                                   : "not shared: " + sGBufferNotSharedWhy;   // [A1 2026-11-09]
    // [BLACK WORLD 2026-11-10] the self-tests and the world check, so the grid's statistics and the reports carry them
    f["vulkan_self_test"] = (sScene && sScene->sharedHandOver() ? "layers: " + sScene->shareSelfTestResult() : std::string("layers: not shared"))
                          + (sGBuffer ? "; G-buffer: " + sGBuffer->selfTestResult() : std::string());
    f["vulkan_world_check"] = sBlack.mSummary.empty() ? (sBlack.mStage == BlackCheck::OFF ? std::string("not started") : std::string("running")) : sBlack.mSummary;
    f["vulkan_spirv"] = sScene ? sScene->spirvSummary() : std::string();
    return f;
}

bool vulkanAvailable()
{
    return !sVulkanDeviceLine.empty();
}

void chooseBackend(const std::string& choice)
{
    // [V BUTTON 2026-11-08] was WolfFloaterVulkan::onChoice; one path for the floater and the navigation bar's V button
    if (choice == gSavedSettings.getString("WolfRenderBackend"))
    {
        return;
    }
    gSavedSettings.setString("WolfRenderBackend", choice);
    // asking again after a failed start: the first-run marker must not stop the next try
    gSavedSettings.setBOOL("WolfVulkanStartOK", true);
    LLNotificationsUtil::add(choice == "vulkan" ? "WolfVulkanChosen" : "WolfVulkanOpenGLChosen");
}

std::string statusLine()
{
    switch (sState)
    {
    case State::VULKAN_ACTIVE:
        // [A1 2026-11-09] the automatic AMD choice is said in plain words; a setting someone chose is not explained
        return LLTrans::getString(sGBufferAutoAMD ? "WolfVulkanStatusActiveAMD" : "WolfVulkanStatusActive");
    case State::FELL_BACK:
    case State::STOPPED:
    {
        LLStringUtil::format_map_t args;
        args["[REASON]"] = sLastError;
        return LLTrans::getString("WolfVulkanStatusFellBack", args);
    }
    case State::VULKAN_STARTING:
        return LLTrans::getString("WolfVulkanStatusStarting");
    case State::OPENGL:
    default:
        if (gSavedSettings.getString("WolfRenderBackend") == "vulkan")
        {
            return LLTrans::getString("WolfVulkanStatusRestart");
        }
        return sVulkanDeviceLine.empty() ? LLTrans::getString("WolfVulkanStatusNoVulkan") : LLTrans::getString("WolfVulkanStatusOpenGL");
    }
}

const std::string& lastError()
{
    return sLastError;
}

F32 presentMillis()
{
    return sPresentMillis;
}

U32 sceneDrawsLastFrame()
{
    return sScene ? sScene->drawsLastFrame() : 0;
}

std::string handOverLine()
{
    return sScene ? sScene->handOverLine() : std::string();
}

U32 gbufferDrawsLastFrame()
{
    return sGBuffer ? sGBufferDraws : 0;
}

bool gbufferShared()
{
    return sGBuffer != nullptr && sGBuffer->ready();
}

U32 validationMessageCount()
{
    return gWolfVkDevice.validationMessageCount();
}

} // namespace WolfVkBridge
