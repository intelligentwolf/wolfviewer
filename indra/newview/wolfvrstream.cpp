/**
 * @file wolfvrstream.cpp
 * @brief WolfViewer: VR in the headset browser, viewer side. See wolfvrstream.h and wolfvrpipe.h.
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

#include "wolfvrstream.h"

#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llnotificationsutil.h"
#include "llmenugl.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "wolfgrid.h"
#include "wolfvrscreen.h"

#if LL_LINUX || LL_DARWIN
#include <sys/stat.h>
#endif

namespace
{
    std::mutex gMutex;   // guards everything below; the server's own threads never take it
    std::unique_ptr<wolfvr::Service> gService;
    std::string gLastError;
    wolfvr::LanAddress gLan;
    bool gInit = false;

    const char* kCertFile = "wolfvr_cert.pem";
    const char* kKeyFile = "wolfvr_key.pem";

    // [VR SIMPLE 2026-11-03] "Headset connected / disconnected" from the server's network thread, shown on the main
    // thread as a notice that needs no click. A mutex of its own: the network thread must never wait for gMutex,
    // which the main thread holds while it stops (joins) that thread.
    std::mutex gNoticeMutex;
    std::deque<std::pair<std::string, bool>> gNotices;
    bool gPumpRunning = false;
    bool gShutdown = false;
    std::string gMode;                // what the running stream shows: test | screen | world (gMutex)

    // [VR 3D 2026-11-03] The picture for each mode. Big screen: the viewer window in 1920x1080, letterboxed when the
    // window is not 16:9 (wolfvrscreen.cpp). A fixed size: the window is often still small when the stream starts
    // (1024x738 at the login screen, seen 2026-11-03) and is resized freely afterwards. World (VR 3D Mode): a square
    // 1440x1440 window into the world, 100 degrees across (Config fov_deg), drawn for the newest head pose.
    struct ModeSize { std::string mode; U32 w, h; bool external; U32 kbps; };
    ModeSize modeSize(const std::string& setting)
    {
        if (setting == "test") return {"test", 1280, 1280, false, 25000};
        if (setting == "world" && gSavedSettings.getBOOL("WolfVR3DMode"))
        {
            // [VR STEREO 2026-11-04] Paul: "do stereo now please". Two eyes side by side in ONE picture (one encode, one
            // decode, one texture on the headset), each eye a square WolfVREyeSize across, a multiple of 16 (H.264
            // macroblocks). 2 x 1152 x 1152 at 60 fps = 622,080 macroblocks/s, inside H.264 level 5.1 (983,040, ITU-T
            // H.264 Table A-1) that NVENC and the Quest decoder both take. More pixels than mono: 40 Mbit/s.
            if (gSavedSettings.getBOOL("WolfVRStereo"))
            {
                const U32 eye = llclamp(gSavedSettings.getU32("WolfVREyeSize"), 512u, 2048u) / 16 * 16;
                return {"world", eye * 2, eye, true, 40000};
            }
            return {"world", 1440, 1440, true, 25000};
        }
        return {"screen", 1920, 1080, true, 25000};
    }

    bool readText(const std::string& path, std::string& out)
    {
        llifstream f(path.c_str(), std::ios::binary);
        if (!f.is_open()) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    bool writeText(const std::string& path, const std::string& text, bool secret)
    {
        llofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << text;
        f.close();
#if LL_LINUX || LL_DARWIN
        if (secret) chmod(path.c_str(), 0600);   // the private key is for this PC's user only
#endif
        return true;
    }

    // Stage-1 certificate: self-signed for this PC's address, kept so the headset browser only asks once per address.
    bool loadOrMakeCert(const std::string& ip, std::string& cert, std::string& key, std::string& error)
    {
        const std::string cert_path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kCertFile);
        const std::string key_path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kKeyFile);
        if (readText(cert_path, cert) && readText(key_path, key) && wolfvr::certCoversIp(cert, ip))
        {
            return true;
        }
        if (!wolfvr::makeSelfSignedCert(ip, cert, key, error))
        {
            return false;
        }
        if (!writeText(key_path, key, true) || !writeText(cert_path, cert, false))
        {
            LL_WARNS("WolfVR") << "could not save the headset certificate; the headset will ask again next time" << LL_ENDL;
        }
        LL_INFOS("WolfVR") << "made a new headset certificate for " << ip << LL_ENDL;
        return true;
    }

    void startLocked()
    {
        gLastError.clear();
        gLan = wolfvr::pickLanAddress();
        const std::string forced = gSavedSettings.getString("WolfVRStreamBindIP");
        if (!forced.empty())
        {
            gLan.ip = wolfvr::isPrivateIPv4(forced) ? forced : std::string();
            gLan.reason = gLan.ip.empty() ? forced + " (WolfVRStreamBindIP) is not a home-network address"
                                          : "set by WolfVRStreamBindIP";
        }
        if (gLan.ip.empty())
        {
            gLastError = "No home network found: " + gLan.reason;
            return;
        }
        wolfvr::Config cfg;
        cfg.bind_ip = gLan.ip;
        cfg.port = (uint16_t)llclamp(gSavedSettings.getU32("WolfVRStreamPort"), 1024u, 65535u);
        cfg.web_root = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolfvr");
        cfg.on_headset = [](const std::string& label, bool connected) {
            std::lock_guard<std::mutex> lk(gNoticeMutex);
            gNotices.emplace_back(label, connected);
        };
        const ModeSize ms = modeSize(gSavedSettings.getString("WolfVRStreamMode"));
        cfg.mode = ms.mode;
        cfg.width = ms.w;
        cfg.height = ms.h;
        cfg.external_frames = ms.external;
        cfg.world_allowed = gSavedSettings.getBOOL("WolfVR3DMode");
        if (ms.external) cfg.fps = 60;
        cfg.bitrate_kbps = ms.kbps;
        cfg.log = [](wolfvr::LogLevel level, const std::string& line) {
            // LL logging is thread-safe; these lines come from the server's own threads.
            if (level == wolfvr::LogLevel::WARN) LL_WARNS("WolfVR") << line << LL_ENDL;
            else LL_INFOS("WolfVR") << line << LL_ENDL;
        };
        std::string error;
        if (!loadOrMakeCert(cfg.bind_ip, cfg.cert_pem, cfg.key_pem, error))
        {
            gLastError = "Could not make the headset certificate: " + error;
            return;
        }
        auto svc = std::make_unique<wolfvr::Service>();
        if (!svc->start(cfg, error))
        {
            gLastError = error;
            LL_WARNS("WolfVR") << "headset stream did not start: " << error << LL_ENDL;
            return;
        }
        gService = std::move(svc);
        gMode = cfg.mode;
        LL_INFOS("WolfVR") << "headset stream on (" << cfg.mode << ", " << cfg.width << "x" << cfg.height << ")" << LL_ENDL;
    }

    void stopLocked()
    {
        if (!gService) return;
        gMode.clear();
        gService->stop();
        gService.reset();
    }

    void onEnabledChanged(const LLSD& value)
    {
        {
            std::lock_guard<std::mutex> lk(gMutex);
            if (value.asBoolean())
            {
                if (!gService) startLocked();
                return;
            }
            stopLocked();
        }
        WolfVRScreen::release();
    }

    // [VR MENU 2026-11-03] Wolf World > VR on Wolf Territories only. A submenu has no on_visible (llmenugl.h), so it is
    // shown or hidden here, as wolfgame.cpp gate_games_menu does for Games & Stories; touched only when it changes.
    void gateMenu()
    {
        static S32 s_applied = -1;
        static const LLView* s_bar = nullptr;
        const bool on = WolfGrid::isWolfTerritories();
        if (!gMenuBarView || (s_bar == gMenuBarView && s_applied == (on ? 1 : 0))) return;
        if (LLMenuGL* wolf_world = gMenuBarView->findChildMenuByName("WolfWorld", true))
        {
            wolf_world->setItemVisible("WolfGroupVR", on);
            s_bar = gMenuBarView;
            s_applied = on ? 1 : 0;
        }
    }

    // Main thread, 4 times a second: the menu gate and the connected / disconnected notices (WolfVRHeadsetNotice, a
    // tip that goes by itself: nothing to click, and nothing that needs a quick reaction).
    bool pump()
    {
        if (gShutdown) { gPumpRunning = false; return true; }
        gateMenu();
        std::deque<std::pair<std::string, bool>> notices;
        {
            std::lock_guard<std::mutex> lk(gNoticeMutex);
            notices.swap(gNotices);
        }
        for (const auto& [label, connected] : notices)
        {
            LLSD args;
            args["DEVICE"] = label;
            LLNotificationsUtil::add(connected ? "WolfVRHeadsetConnected" : "WolfVRHeadsetDisconnected", args);
        }
        return false;
    }
}

namespace WolfVRStream
{
void init()
{
    if (gInit) return;
    gInit = true;
    if (!gPumpRunning)
    {
        gPumpRunning = true;
        doPeriodically(pump, 0.25f);
        // [VR BIG SCREEN] the headset's pointer and keys, every frame
        gIdleCallbacks.addFunction([](void*) { if (!gShutdown) WolfVRScreen::pumpInput(); }, nullptr);
    }
    // [VR 3D] a new choice of what to show switches a running stream to it, without dropping the headset
    gSavedSettings.getControl("WolfVRStreamMode")->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&) {
        std::lock_guard<std::mutex> lk(gMutex);
        if (!gService) return;
        const ModeSize ms = modeSize(v.asString());
        if (ms.mode == gMode) return;
        gService->setMode(ms.mode, ms.w, ms.h, ms.external, ms.kbps);
        gMode = ms.mode;
        LL_INFOS("WolfVR") << "headset stream switched to " << ms.mode << " " << ms.w << "x" << ms.h << LL_ENDL;
    });
    // [VR STEREO 2026-11-04] stereo on/off or a new eye size while in 3D Mode: the new picture size at once
    auto resize3d = [](LLControlVariable*, const LLSD&, const LLSD&) {
        std::lock_guard<std::mutex> lk(gMutex);
        if (!gService || gMode != "world") return;
        const ModeSize ms = modeSize("world");
        gService->setMode(ms.mode, ms.w, ms.h, ms.external, ms.kbps);
        LL_INFOS("WolfVR") << "headset 3D picture now " << ms.w << "x" << ms.h << LL_ENDL;
    };
    gSavedSettings.getControl("WolfVRStereo")->getSignal()->connect(resize3d);
    gSavedSettings.getControl("WolfVREyeSize")->getSignal()->connect(resize3d);
    gSavedSettings.getControl("WolfVRStreamEnabled")->getSignal()->connect(
        [](LLControlVariable*, const LLSD& value, const LLSD&) { onEnabledChanged(value); });
    if (gSavedSettings.getBOOL("WolfVRStreamEnabled")) onEnabledChanged(LLSD(true));
}

void shutdown()
{
    gShutdown = true;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        stopLocked();
    }
    // The GPU objects of the big screen go with the GL context at exit; nothing to free here.
}

bool bigScreenWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gMode == "screen" && gService->wantsFrames();
}

bool worldWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gMode == "world" && gService->wantsFrames();
}

wolfvr::HeadPose headPose()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->headPose() : wolfvr::HeadPose();
}

void submitWorldFrame(std::vector<uint8_t>& rgba, const wolfvr::HeadPose& pose, double capture_ms)
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (gService) gService->submitFrame(std::move(rgba), pose, capture_ms);
}

void pumpModeRequest()
{
    std::string m;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        if (gService) m = gService->takeModeRequest();
    }
    // Through the setting, so the floater's choice follows the headset's (the signal above does the switch).
    if (m == "world" && !gSavedSettings.getBOOL("WolfVR3DMode")) return;   // not offered yet
    if (!m.empty()) gSavedSettings.setString("WolfVRStreamMode", m);
}

void frameSize(U32& width, U32& height)
{
    std::lock_guard<std::mutex> lk(gMutex);
    width = height = 0;
    if (!gService) return;
    width = gService->frameWidth();
    height = gService->frameHeight();
}

void submitFrame(std::vector<uint8_t>& rgba, double capture_ms)
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (gService) gService->submitFrame(std::move(rgba), wolfvr::HeadPose(), capture_ms);
}

std::vector<wolfvr::InputEvent> takeInput()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->takeInput() : std::vector<wolfvr::InputEvent>();
}

bool running()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService != nullptr;
}

wolfvr::Stats stats()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->stats() : wolfvr::Stats();
}

std::string lanUrl()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->lanUrl() : std::string();
}

uint16_t port()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->port() : 0;
}

std::string usbUrl()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->usbUrl() : std::string();
}

std::string shortAddress()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->shortAddress() : std::string();
}

std::string lastError()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gLastError;
}

wolfvr::LanAddress lanAddress()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gLan;
}
}
