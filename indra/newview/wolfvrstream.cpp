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
#include "wolfvrpairing.h"
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
    const char* kDevicesFile = "wolfvr_devices.json";

    // [VR PAIRING 2026-11-03] Questions from the server's network thread, shown on the main thread. A mutex of its own:
    // the network thread must never wait for gMutex, which the main thread holds while it stops (joins) that thread.
    std::mutex gAskMutex;
    struct AskEvent { uint64_t generation; wolfvr::ApprovalRequest req; bool cancelled; };
    std::deque<AskEvent> gAskEvents;
    // main thread only: the questions on screen, by (service generation, ticket)
    std::map<std::pair<uint64_t, uint64_t>, LLNotificationPtr> gShown;
    uint64_t gGeneration = 0;          // a new number per start: old questions never answer a new server
    size_t gKnownWhenOff = 0;
    bool gPumpRunning = false;
    bool gShutdown = false;
    std::string gMode;                // what the running stream shows: test | screen | world (gMutex)

    // [VR 3D 2026-11-03] The picture for each mode. Big screen: the viewer window in 1920x1080, letterboxed when the
    // window is not 16:9 (wolfvrscreen.cpp). A fixed size: the window is often still small when the stream starts
    // (1024x738 at the login screen, seen 2026-11-03) and is resized freely afterwards. World (VR 3D Mode): a square
    // 1440x1440 window into the world, 100 degrees across (Config fov_deg), drawn for the newest head pose.
    struct ModeSize { std::string mode; U32 w, h; bool external; };
    ModeSize modeSize(const std::string& setting)
    {
        if (setting == "test") return {"test", 1280, 1280, false};
        if (setting == "world" && gSavedSettings.getBOOL("WolfVR3DMode")) return {"world", 1440, 1440, true};
        return {"screen", 1920, 1080, true};
    }

    std::string devicesPath() { return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kDevicesFile); }

    // main thread
    void closeQuestions()
    {
        // Take the list first: a responder must never find a question it can still answer.
        std::map<std::pair<uint64_t, uint64_t>, LLNotificationPtr> shown;
        shown.swap(gShown);
        for (auto& [key, n] : shown) LLNotificationsUtil::cancel(n);
        std::lock_guard<std::mutex> lk(gAskMutex);
        gAskEvents.clear();
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
        cfg.device_store = devicesPath();
        const uint64_t generation = ++gGeneration;
        cfg.on_approval = [generation](const wolfvr::ApprovalRequest& r) {
            std::lock_guard<std::mutex> lk(gAskMutex);
            gAskEvents.push_back(AskEvent{generation, r, false});
        };
        cfg.on_approval_cancelled = [generation](uint64_t ticket) {
            std::lock_guard<std::mutex> lk(gAskMutex);
            wolfvr::ApprovalRequest r;
            r.ticket = ticket;
            gAskEvents.push_back(AskEvent{generation, r, true});
        };
        cfg.bitrate_kbps = llclamp(gSavedSettings.getU32("WolfVRStreamBitrateKbps"), 2000u, 100000u);
        cfg.encoder = gSavedSettings.getString("WolfVRStreamEncoder");
        const ModeSize ms = modeSize(gSavedSettings.getString("WolfVRStreamMode"));
        cfg.mode = ms.mode;
        cfg.width = ms.w;
        cfg.height = ms.h;
        cfg.external_frames = ms.external;
        cfg.world_allowed = gSavedSettings.getBOOL("WolfVR3DMode");
        if (ms.external) cfg.fps = 60;
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
        gKnownWhenOff = gService->knownDevices();
        gService->stop();
        gService.reset();
        ++gGeneration;
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
        closeQuestions();
        WolfVRScreen::release();
    }

    void answer(uint64_t generation, uint64_t ticket, bool allow)
    {
        std::lock_guard<std::mutex> lk(gMutex);
        if (gService && generation == gGeneration) gService->answer(ticket, allow);
    }

    // Main thread, 4 times a second: show the server's questions, close the ones the headset gave up on.
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

    bool pumpQuestions()
    {
        if (gShutdown) { gPumpRunning = false; return true; }
        gateMenu();
        std::deque<AskEvent> events;
        {
            std::lock_guard<std::mutex> lk(gAskMutex);
            events.swap(gAskEvents);
        }
        for (const AskEvent& e : events)
        {
            const auto key = std::make_pair(e.generation, e.req.ticket);
            if (e.cancelled)
            {
                auto it = gShown.find(key);
                if (it != gShown.end())
                {
                    LLNotificationPtr n = it->second;
                    gShown.erase(it);
                    LLNotificationsUtil::cancel(n);
                }
                continue;
            }
            if (e.generation != gGeneration) continue;   // from a stream already switched off
            LLSD args;
            args["DEVICE"] = e.req.label;
            args["IP"] = e.req.ip;
            const std::string name = e.req.known ? "WolfVRHeadsetAsksAgain" : "WolfVRHeadsetAsks";
            LLNotificationPtr n = LLNotificationsUtil::add(name, args, LLSD(), [key](const LLSD& notification, const LLSD& response) {
                // Only an answer to a question still on screen counts (a closed one is erased first).
                if (gShown.erase(key) == 0) return;
                const bool allow = LLNotificationsUtil::getSelectedOption(notification, response) == 0;
                LL_INFOS("WolfVR") << (allow ? "headset allowed" : "headset denied") << LL_ENDL;
                answer(key.first, key.second, allow);
            });
            LL_INFOS("WolfVR") << "asking Allow / Deny for " << e.req.label << " at " << e.req.ip << LL_ENDL;
            if (n) gShown[key] = n;
            else answer(key.first, key.second, false);   // could not ask: the safe answer
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
    gKnownWhenOff = wolfvr::DeviceStore(devicesPath()).count();
    if (!gPumpRunning)
    {
        gPumpRunning = true;
        doPeriodically(pumpQuestions, 0.25f);
        // [VR BIG SCREEN] the headset's pointer and keys, every frame
        gIdleCallbacks.addFunction([](void*) { if (!gShutdown) WolfVRScreen::pumpInput(); }, nullptr);
    }
    // [VR 3D] a new choice of what to show switches a running stream to it, without dropping the headset
    gSavedSettings.getControl("WolfVRStreamMode")->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&) {
        std::lock_guard<std::mutex> lk(gMutex);
        if (!gService) return;
        const ModeSize ms = modeSize(v.asString());
        if (ms.mode == gMode) return;
        gService->setMode(ms.mode, ms.w, ms.h, ms.external);
        gMode = ms.mode;
        LL_INFOS("WolfVR") << "headset stream switched to " << ms.mode << LL_ENDL;
    });
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

size_t knownHeadsets()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gService ? gService->knownDevices() : gKnownWhenOff;
}

void forgetAllHeadsets()
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (gService) gService->forgetAllDevices();
    else wolfvr::DeviceStore(devicesPath()).forgetAll();
    gKnownWhenOff = 0;
    LL_INFOS("WolfVR") << "forgot every allowed headset" << LL_ENDL;
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
