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
#include <atomic>
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
#include "wolfvrui.h"
#include "wolfvraudio.h"
#include "wolfvr_openh264.h"
#include "wolfvrcodecfetch.h"

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
    // [VR OPENH264 2026-11-04] Cisco's OpenH264 binary, downloaded on the resident's own press (Cisco's binary licence:
    // "separately downloaded to an end user's device", the resident able to switch it off: WolfVROpenH264), kept in
    // the user settings folder. Codec state for the floater; the download's answer comes back on its own thread.
    std::string codecDir() { return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, ""); }
    std::atomic<bool> gCodecBusy{false};
    std::atomic<int> gCodecInstalled{-1};   // -1 not checked yet
    std::mutex gCodecMutex;
    std::string gCodecMessage;              // gCodecMutex
    void applyCodec()
    {
        const std::string dir = codecDir();
        const bool have = wolfvr::openh264Available() && wolfvr::openh264Installed(dir);
        gCodecInstalled = have ? 1 : 0;
        wolfvr::setOpenH264Path(have && gSavedSettings.getBOOL("WolfVROpenH264") ? wolfvr::openh264File(dir) : std::string());
    }

    struct ModeSize { std::string mode; U32 w, h; bool external; U32 kbps; };
    // [VR CONTROLS 2026-11-04] Quality steps the headset page picks (per-headset start, then down when its decoder or
    // the network falls behind, up again when steady; wolfvr.js adaptQuality): eye size, stereo / mono / big-screen
    // bitrate. Step 0 is the Quest 3 default; the page's own table says which step each headset starts on.
    // [VR 3D FIX 2026-11-04] Paul after the Quest 3 test: "the quality was a bit low". Sharper steps: step 0 is a
    // 1920 px eye (3840 x 1920 picture: 28,800 macroblocks a frame, 1,728,000 a second at 60 fps - inside H.264 level
    // 5.2, 36,864 and 2,073,600, ITU-T H.264 Table A-1; NVENC's H.264 limit is 4096 wide) at 100 Mbit/s; step 1, the
    // Quest 3 start, 1728 px at 80 Mbit/s (Wi-Fi 6 carries it); the old Quest 3 start (1152 at 40) is now step 3.
    struct QualityStep { U32 eye; U32 stereo_kbps; U32 flat_kbps; };
    constexpr int kSteps = 5;
    constexpr QualityStep kQuality[kSteps] = {{1920, 100000, 40000}, {1728, 80000, 32000}, {1440, 55000, 25000},
                                              {1152, 35000, 18000}, {896, 20000, 12000}};
    S32 gQuality = 1;   // gMutex

    ModeSize modeSize(const std::string& setting)
    {
        const QualityStep& q = kQuality[llclamp(gQuality, 0, kSteps - 1)];
        if (setting == "test") return {"test", 1280, 1280, false, 25000};
        if (setting == "world" && gSavedSettings.getBOOL("WolfVR3DMode"))
        {
            // [VR STEREO 2026-11-04] Paul: "do stereo now please". Two eyes side by side in ONE picture (one encode, one
            // decode, one texture on the headset), each eye a square WolfVREyeSize across, a multiple of 16 (H.264
            // macroblocks). 2 x 1152 x 1152 at 60 fps = 622,080 macroblocks/s, inside H.264 level 5.1 (983,040, ITU-T
            // H.264 Table A-1) that NVENC and the Quest decoder both take. More pixels than mono: 40 Mbit/s.
            if (gSavedSettings.getBOOL("WolfVRStereo"))
            {
                const U32 eye = llmin(q.eye, llclamp(gSavedSettings.getU32("WolfVREyeSize"), 512u, 2048u)) / 16 * 16;
                return {"world", eye * 2, eye, true, q.stereo_kbps};
            }
            const U32 side = q.eye * 5 / 4 / 16 * 16;   // 1440 at step 0, as before
            return {"world", side, side, true, q.flat_kbps};
        }
        return {"screen", 1920, 1080, true, q.flat_kbps};
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
        cfg.debug_stats = gSavedSettings.getBOOL("WolfVRDebugStats");   // [VR LEAN] read once, here
        cfg.stereo_check = gSavedSettings.getBOOL("WolfVRStereoCheck");
        if (ms.external) cfg.fps = 60;
        cfg.bitrate_kbps = ms.kbps;
        cfg.log = [](wolfvr::LogLevel level, const std::string& line) {
            // LL logging is thread-safe; these lines come from the server's own threads.
            if (level == wolfvr::LogLevel::WARN) LL_WARNS("WolfVR") << line << LL_ENDL;
            else LL_INFOS("WolfVR") << line << LL_ENDL;
        };
        applyCodec();   // [VR OPENH264] before the first encoder opens
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
            }
            else
            {
                stopLocked();
            }
        }
        // [VR CODEC 2026-11-04] Paul: "automatically download that codec, I didn't even see it". VR switched on, this
        // computer can use Cisco's OpenH264 and does not have it yet: fetched at once from Cisco's own server (as
        // Firefox fetches it), the checksums checked as before (wolfvrcodecfetch.cpp); nothing to press. The
        // resident's "Use the OpenH264 video codec" switch (WolfVROpenH264) still turns it off and then it is not
        // fetched. On Paul's Linux PC the encoder is NVENC (nvh264enc); OpenH264 is the fallback without it.
        if (value.asBoolean())
        {
            if (gSavedSettings.getBOOL("WolfVROpenH264") && WolfVRStream::codecState() == 0) WolfVRStream::downloadCodec();
        }
        else
        {
            WolfVRScreen::release();
        }
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
        WolfVRUi::pump();   // [VR CONTROLS 2026-11-04] the in-VR action bar's state to the headset
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
        gIdleCallbacks.addFunction([](void*) {
            if (gShutdown) return;
            WolfVRScreen::pumpInput();
            WolfVRAudio::pump();   // [VR AUDIO 2026-11-04] the sound to the headset, the PC speakers muted or not
        }, nullptr);
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
    // [VR OPENH264] the resident's on/off for Cisco's codec: the next encoder uses it, or not (a library already
    // loaded stays loaded until the viewer restarts - wolfvr_openh264.h)
    gSavedSettings.getControl("WolfVROpenH264")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { applyCodec(); });
    gSavedSettings.getControl("WolfVREyeSize")->getSignal()->connect(resize3d);
    // [VR OFF AT START 2026-11-04] Paul: "when someone closes the viewer we must turn it all off". VR is never on when
    // WolfViewer starts: the switch is not remembered (Persist 0), and a value an older build saved is cleared here,
    // so nothing listens, no headset tab can reconnect by itself and nothing of the desktop changes until the
    // resident chooses Wolf World > VR in this session. (WolfVRStreamMode stays remembered: only where it starts.)
    gSavedSettings.setBOOL("WolfVRStreamEnabled", false);
    gSavedSettings.getControl("WolfVRStreamEnabled")->getSignal()->connect(
        [](LLControlVariable*, const LLSD& value, const LLSD&) { onEnabledChanged(value); });
}

void shutdown()
{
    WolfVRAudio::shutdown();   // [VR AUDIO] every tap removed and the PC speakers given back first
    gShutdown = true;   // [VR OFF AT START] WolfVRStreamEnabled is not saved (Persist 0): off at the next start
    {
        std::lock_guard<std::mutex> lk(gMutex);
        stopLocked();
    }
    // The GPU objects of the big screen go with the GL context at exit; nothing to free here.
}

bool bigScreenWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gMode == "screen" && gService->immersive();   // [VR IMMERSIVE] only while the headset is in VR
}

bool worldWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gMode == "world" && gService->immersive();   // [VR IMMERSIVE] only while the headset is in VR
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
        if (gService)
        {
            m = gService->takeModeRequest();
            // [VR CONTROLS 2026-11-04] a new quality step: the same mode at the new size and bitrate
            const int q = gService->takeQualityRequest();
            if (q >= 0 && q != gQuality)
            {
                gQuality = q;
                const ModeSize ms = modeSize(gMode);
                if (ms.mode == gMode)
                {
                    gService->setMode(ms.mode, ms.w, ms.h, ms.external, ms.kbps);
                    LL_INFOS("WolfVR") << "headset asked for quality step " << q << ": " << ms.w << "x" << ms.h << " "
                                       << ms.kbps << " kbit/s" << LL_ENDL;
                }
            }
        }
    }
    // Through the setting, so the floater's choice follows the headset's (the signal above does the switch).
    if (m == "world" && !gSavedSettings.getBOOL("WolfVR3DMode")) return;   // not offered yet
    if (!m.empty()) gSavedSettings.setString("WolfVRStreamMode", m);
}

int codecState()
{
    if (!wolfvr::openh264Available()) return -1;
    if (gCodecBusy) return 2;
    if (gCodecInstalled < 0) applyCodec();
    return gCodecInstalled > 0 ? 1 : 0;
}

std::string codecMessage()
{
    std::lock_guard<std::mutex> lk(gCodecMutex);
    return gCodecMessage;
}

void downloadCodec()
{
    if (gCodecBusy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(gCodecMutex);
        gCodecMessage = "Downloading the video codec from Cisco...";
    }
    // done() runs on the download's own thread: it only stores the answer; the floater reads it (codecMessage) and
    // the library is picked up by the next encoder (setOpenH264Path is safe from any thread)
    wolfvr::fetchOpenH264Async(codecDir(), [](bool ok, std::string message) {
        {
            std::lock_guard<std::mutex> lk(gCodecMutex);
            gCodecMessage = message;
        }
        if (ok)
        {
            gCodecInstalled = 1;
            wolfvr::setOpenH264Path(wolfvr::openh264File(codecDir()));
        }
        gCodecBusy = false;
    });
}

bool audioWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gService->audioWanted() && gService->immersive();   // [VR IMMERSIVE] the PC keeps its sound otherwise
}

void submitAudio(const int16_t* pcm, U32 frames, U32 channels, U32 rate, double mix_ms)
{
    // the audio thread: never wait long for the main thread's lock (it is held only for short calls)
    std::unique_lock<std::mutex> lk(gMutex, std::try_to_lock);
    if (lk.owns_lock() && gService) gService->submitAudio(pcm, frames, channels, rate, mix_ms);
}

bool layerWanted()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gMode == "world" && gService->layerWanted() && gService->immersive();
}

bool layerIdle()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gService->layerIdle();
}

void submitLayer(std::vector<uint8_t>& rgba, U32 w, U32 h, double capture_ms)
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (gService) gService->submitLayer(rgba, w, h, capture_ms);
}

void sendText(const std::string& json)
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (gService) gService->sendText(json);
}

std::string mode()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gMode : std::string();
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

bool headsetActive()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService && gService->wantsFrames();
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
