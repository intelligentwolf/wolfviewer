/**
 * @file wolfspanscreens.cpp
 * @brief WolfViewer: spread the viewer window across every screen. See wolfspanscreens.h.
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

#include "wolfspanscreens.h"

#include "llcallbacklist.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#if LL_LINUX
#include "llprocess.h"
#include "boost/json.hpp"
#include <cerrno>
#include <cmath>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    bool sHyprSpanned = false;      // Hyprland was asked to spread the window (it is not the window's own state)

    void tell(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("GenericAlert", args);
    }

#if LL_LINUX
    // ---- Hyprland ------------------------------------------------------------------------------
    // Source: Hyprland v0.56.2 (efb5099, the version Paul runs) hyprctl/src/main.cpp:82-89
    // getRuntimeDir() = $XDG_RUNTIME_DIR/hypr (else /run/user/<uid>/hypr), :230 the socket is
    // <runtime>/<HYPRLAND_INSTANCE_SIGNATURE>/.socket.sock; :480 a request is "<flags>/<command>"
    // ("j" = JSON); the reply is everything read until Hyprland closes the connection (:258-276).
    bool hyprlandRunning()
    {
        const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        return sig && *sig;
    }

    std::string hyprRequest(const std::string& request)
    {
        const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!sig || !*sig)
            return "";
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const std::string path = (xdg ? std::string(xdg) : "/run/user/" + std::to_string(getuid())) + "/hypr/" + sig + "/.socket.sock";

        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return "";
        timeval tv = { 2, 0 };      // never hold the viewer up for long if Hyprland is busy
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        std::string reply;
        if (connect(fd, (sockaddr*)&addr, SUN_LEN(&addr)) == 0)
        {
            size_t sent = 0;
            while (sent < request.size())
            {
                const ssize_t n = write(fd, request.data() + sent, request.size() - sent);
                if (n <= 0)
                    break;
                sent += (size_t)n;
            }
            char buf[8192];
            while (sent == request.size())
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                reply.append(buf, (size_t)n);
            }
        }
        close(fd);
        return reply;
    }

    // A Hyprland using the Lua config (0.55 and later, like Paul's) runs "dispatch X" as Lua
    // hl.dispatch(X); one using the classic config looks X up as a dispatcher name
    // (HyprCtl.cpp:1126-1158 dispatchRequest). So the Lua form is tried, then the classic one.
    bool hyprDispatch(const std::string& lua, const std::string& classic)
    {
        if (hyprRequest("/dispatch " + lua).rfind("ok", 0) == 0)
            return true;
        const std::string reply = hyprRequest("/dispatch " + classic);
        if (reply.rfind("ok", 0) == 0)
            return true;
        LL_WARNS("Window") << "Span screens: Hyprland refused \"" << classic << "\": " << reply << LL_ENDL;
        return false;
    }

    struct HyprClient
    {
        std::string address;
        bool floating = false, spanTag = false;
        int x = 0, y = 0, w = 0, h = 0, fullscreen = 0, fullscreenClient = 0;
    };

    // Our own window: the client whose pid is this process (clients JSON, HyprCtl.cpp:387-411).
    bool hyprFindSelf(HyprClient& out)
    try
    {
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(hyprRequest("j/clients"), ec);
        if (ec || !v.is_array())
            return false;
        const pid_t me = getpid();
        for (const auto& c : v.as_array())
        {
            if (!c.is_object())
                continue;
            const auto& o = c.as_object();
            const auto* pid = o.if_contains("pid");
            const auto* address = o.if_contains("address");
            if (!pid || !pid->is_int64() || pid->as_int64() != me || !address || !address->is_string())
                continue;
            out.address = std::string(address->as_string());
            if (const auto* f = o.if_contains("floating"); f && f->is_bool())
                out.floating = f->as_bool();
            if (const auto* at = o.if_contains("at"); at && at->is_array() && at->as_array().size() == 2)
            {
                out.x = (int)at->as_array()[0].to_number<double>();
                out.y = (int)at->as_array()[1].to_number<double>();
            }
            if (const auto* sz = o.if_contains("size"); sz && sz->is_array() && sz->as_array().size() == 2)
            {
                out.w = (int)sz->as_array()[0].to_number<double>();
                out.h = (int)sz->as_array()[1].to_number<double>();
            }
            if (const auto* f = o.if_contains("fullscreen"); f && f->is_int64())
                out.fullscreen = (int)f->as_int64();
            if (const auto* f = o.if_contains("fullscreenClient"); f && f->is_int64())
                out.fullscreenClient = (int)f->as_int64();
            if (const auto* tags = o.if_contains("tags"); tags && tags->is_array())
            {
                for (const auto& t : tags->as_array())
                {
                    // the tag Paul's span script and this file set (a dynamic tag may carry a "*")
                    if (t.is_string() && (t.as_string() == "span" || t.as_string() == "span*"))
                        out.spanTag = true;
                }
            }
            return true;
        }
        return false;
    }
    catch (const std::exception& e)     // to_number on a field of an unexpected type
    {
        LL_WARNS("Window") << "Span screens: Hyprland clients reply not understood: " << e.what() << LL_ENDL;
        return false;
    }

    // The desktop's box, as Paul's span script measures it: logical size per monitor is mode size
    // / scale (width and height swapped for the 90/270 degree transforms 1, 3, 5, 7), as wide as
    // the whole desktop and as tall as the shortest monitor measured from the top, so nothing the
    // window draws lands off a screen. Monitor fields: HyprCtl.cpp:229-251 getMonitorData.
    bool hyprDesktopBox(int& bx, int& by, int& bw, int& bh)
    try
    {
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(hyprRequest("j/monitors"), ec);
        if (ec || !v.is_array() || v.as_array().empty())
            return false;
        bool first = true;
        int x0 = 0, y0 = 0, right = 0, bottom = 0;
        for (const auto& m : v.as_array())
        {
            if (!m.is_object())
                return false;
            const auto& o = m.as_object();
            const auto *px = o.if_contains("x"), *py = o.if_contains("y"), *pw = o.if_contains("width"),
                       *ph = o.if_contains("height"), *ps = o.if_contains("scale"), *pt = o.if_contains("transform");
            if (!px || !py || !pw || !ph || !ps || !pt)
                return false;
            const double scale = ps->to_number<double>();
            if (!(scale > 0.0))
                return false;
            const bool turned = (pt->to_number<int64_t>() % 2) == 1;
            const int mw = (int)std::lround((turned ? ph : pw)->to_number<double>() / scale);
            const int mh = (int)std::lround((turned ? pw : ph)->to_number<double>() / scale);
            const int mx = (int)px->to_number<int64_t>(), my = (int)py->to_number<int64_t>();
            if (first)
            {
                x0 = mx; y0 = my; right = mx + mw; bottom = my + mh;
                first = false;
            }
            else
            {
                x0 = llmin(x0, mx);
                y0 = llmin(y0, my);
                right = llmax(right, mx + mw);
                bottom = llmin(bottom, my + mh);
            }
        }
        bx = x0; by = y0; bw = right - x0; bh = bottom - y0;
        return bw > 0 && bh > 0;
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Window") << "Span screens: Hyprland monitors reply not understood: " << e.what() << LL_ENDL;
        return false;
    }

    // Paul's own script (~/.local/bin, bound to SUPER+ALT+W) also hides and restores his bar, so
    // where it is installed it does the job and the key and the menu stay in step (same "span" tag).
    std::string findSpanScript()
    {
        const char* path = getenv("PATH");
        if (!path)
            return "";
        std::stringstream dirs(path);
        std::string dir;
        while (std::getline(dirs, dir, ':'))
        {
            if (dir.empty())
                continue;
            const std::string p = dir + "/omarchy-hyprland-window-span-screens";
            if (access(p.c_str(), X_OK) == 0)
                return p;
        }
        return "";
    }

    struct HyprSaved
    {
        bool valid = false, floating = false;
        int x = 0, y = 0, w = 0, h = 0, fullscreen = 0, fullscreenClient = 0;
    };
    HyprSaved sHyprSaved;

    bool hyprSpan(bool on, std::string& why)
    {
        HyprClient self;
        if (!hyprFindSelf(self))
        {
            why = "Hyprland did not list the viewer window.";
            return false;
        }
        sHyprSpanned = self.spanTag;
        if (self.spanTag == on)
            return true;

        const std::string script = findSpanScript();
        if (!script.empty())
        {
            LLProcess::Params params;
            params.executable = script;
            params.args.add("--window");
            params.args.add(self.address);
            params.autokill = false;
            if (LLProcess::create(params))
            {
                sHyprSpanned = on;
                return true;
            }
            LL_WARNS("Window") << "Span screens: could not run " << script << "; asking Hyprland directly" << LL_ENDL;
        }

        const std::string win = "address:" + self.address;
        const std::string luaWin = "window = \"" + win + "\"";
        // Lua dispatchers: LuaBindingsDispatchers.cpp:1370-1387 (float, fullscreen_state, move,
        // alter_zorder, resize; x/y absolute unless relative = true) and hlWindowTag. Classic:
        // DispatcherTranslator.cpp setfloating/settiled (:122-130), movewindowpixel/resizewindowpixel
        // "exact X Y,<window>" (:448-472, Compositor.cpp:841 "exact"), alterzorder "top,<window>"
        // (:683), tagwindow "<tag> <window>", fullscreenstate "I C" (the focused window, :188).
        if (on)
        {
            int bx = 0, by = 0, bw = 0, bh = 0;
            if (!hyprDesktopBox(bx, by, bw, bh))
            {
                why = "Hyprland did not give a screen layout one window can cover.";
                return false;
            }
            sHyprSaved = { true, self.floating, self.x, self.y, self.w, self.h, self.fullscreen, self.fullscreenClient };
            if (self.fullscreen || self.fullscreenClient)
            {
                hyprDispatch("hl.dsp.window.fullscreen_state({ " + luaWin + ", internal = 0, client = 0 })", "fullscreenstate 0 0");
            }
            const bool ok = hyprDispatch("hl.dsp.window.float({ " + luaWin + ", action = \"enable\" })", "setfloating " + win)
                && hyprDispatch(llformat("hl.dsp.window.resize({ %s, x = %d, y = %d })", luaWin.c_str(), bw, bh),
                                llformat("resizewindowpixel exact %d %d,%s", bw, bh, win.c_str()))
                && hyprDispatch(llformat("hl.dsp.window.move({ %s, x = %d, y = %d })", luaWin.c_str(), bx, by),
                                llformat("movewindowpixel exact %d %d,%s", bx, by, win.c_str()));
            if (!ok)
            {
                why = "Hyprland would not move the viewer window over every screen.";
                return false;
            }
            hyprDispatch("hl.dsp.window.alter_zorder({ " + luaWin + ", mode = \"top\" })", "alterzorder top," + win);
            hyprDispatch("hl.dsp.window.tag({ " + luaWin + ", tag = \"+span\" })", "tagwindow +span " + win);
            LL_INFOS("Window") << "Span screens: Hyprland, " << bw << "x" << bh << " at " << bx << "," << by << LL_ENDL;
        }
        else
        {
            if (sHyprSaved.valid && sHyprSaved.floating)
            {
                hyprDispatch(llformat("hl.dsp.window.resize({ %s, x = %d, y = %d })", luaWin.c_str(), sHyprSaved.w, sHyprSaved.h),
                             llformat("resizewindowpixel exact %d %d,%s", sHyprSaved.w, sHyprSaved.h, win.c_str()));
                hyprDispatch(llformat("hl.dsp.window.move({ %s, x = %d, y = %d })", luaWin.c_str(), sHyprSaved.x, sHyprSaved.y),
                             llformat("movewindowpixel exact %d %d,%s", sHyprSaved.x, sHyprSaved.y, win.c_str()));
            }
            else
            {
                hyprDispatch("hl.dsp.window.float({ " + luaWin + ", action = \"disable\" })", "settiled " + win);
            }
            hyprDispatch("hl.dsp.window.tag({ " + luaWin + ", tag = \"-span\" })", "tagwindow -span " + win);
            if (sHyprSaved.valid && (sHyprSaved.fullscreen || sHyprSaved.fullscreenClient))
            {
                hyprDispatch(llformat("hl.dsp.window.fullscreen_state({ %s, internal = %d, client = %d })", luaWin.c_str(),
                                      sHyprSaved.fullscreen, sHyprSaved.fullscreenClient),
                             llformat("fullscreenstate %d %d", sHyprSaved.fullscreen, sHyprSaved.fullscreenClient));
            }
            sHyprSaved = {};
        }
        sHyprSpanned = on;
        return true;
    }
#endif // LL_LINUX

    // Another Wayland desktop: a window cannot place itself there. Through XWayland the X11 way
    // works, so offer that (WolfNativeWayland, read at the next start - llwindowsdl2.cpp
    // createContext) and keep the choice so the next start spreads the window.
    bool onRestartInX11(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            gSavedSettings.setBOOL("WolfNativeWayland", false);
            gSavedSettings.setBOOL("WolfSpanAllScreens", true);
            tell("Done. Restart the viewer and it opens across all your screens.");
        }
        return false;
    }

    void apply(bool on, bool quiet)
    {
        if (!gViewerWindow || !gViewerWindow->getWindow())
            return;
        std::string why;
        bool done = false;
#if LL_LINUX
        if (hyprlandRunning())
        {
            done = hyprSpan(on, why);
        }
        else
#endif
        {
            const LLWindow::ESpanResult r = gViewerWindow->getWindow()->spanAllScreens(on, why);
            if (r == LLWindow::SPAN_COMPOSITOR)
            {
                if (on && !quiet)
                {
                    LLSD args;
                    args["MESSAGE"] = "This Wayland desktop does not let a window place itself across several screens. "
                                      "The viewer can do it when it runs through XWayland instead. "
                                      "Switch to XWayland? It takes effect when you restart the viewer.";
                    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), &onRestartInX11);
                }
                else if (!on)
                {
                    gSavedSettings.setBOOL("WolfSpanAllScreens", false);
                }
                return;
            }
            done = (r == LLWindow::SPAN_DONE);
        }
        if (done)
        {
            gSavedSettings.setBOOL("WolfSpanAllScreens", on);
        }
        else
        {
            LL_WARNS("Window") << "Span screens " << (on ? "on" : "off") << " failed: " << why << LL_ENDL;
            if (on)
            {
                gSavedSettings.setBOOL("WolfSpanAllScreens", false);
            }
            if (!quiet)
            {
                tell(std::string(on ? "Could not spread the viewer across your screens. " : "Could not put the viewer window back. ") + why);
            }
        }
    }

    // At start-up the window has to be on screen (and, on Hyprland, listed) before it can be
    // moved, so the spread waits two seconds into the main loop.
    F64 sStartupAt = 0.0;
    void startupIdle(void*)
    {
        if (LLTimer::getElapsedSeconds() < sStartupAt)
            return;
        gIdleCallbacks.deleteFunction(startupIdle, nullptr);
        if (gSavedSettings.getBOOL("WolfSpanAllScreens"))
        {
            // quiet: a failure here is logged, not put in a box before the login screen
            apply(true, true);
        }
    }
}

namespace WolfSpanScreens
{
    void toggle()
    {
        apply(!isOn(), false);
    }

    bool isOn()
    {
#if LL_LINUX
        // SUPER+ALT+W (Paul's script) can spread or restore the window behind the viewer's back;
        // the "span" tag says which, so the tick and the saved choice follow it. At most once a second
        // - the menu asks every time it draws.
        static F64 next_look = 0.0;
        if (hyprlandRunning() && LLTimer::getElapsedSeconds() >= next_look)
        {
            next_look = LLTimer::getElapsedSeconds() + 1.0;
            HyprClient self;
            if (hyprFindSelf(self) && self.spanTag != sHyprSpanned)
            {
                sHyprSpanned = self.spanTag;
                gSavedSettings.setBOOL("WolfSpanAllScreens", self.spanTag);
            }
        }
#endif
        return active();
    }

    bool active()
    {
        if (sHyprSpanned)
            return true;
        return gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getWindow()->getSpanningScreens();
    }

    void startup()
    {
        if (!gSavedSettings.getBOOL("WolfSpanAllScreens"))
            return;
        sStartupAt = LLTimer::getElapsedSeconds() + 2.0;
        gIdleCallbacks.addFunction(startupIdle, nullptr);
    }
}
