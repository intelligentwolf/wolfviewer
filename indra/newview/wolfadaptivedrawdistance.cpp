/**
 * @file wolfadaptivedrawdistance.cpp
 * @brief [ADAPTIVE DRAW DISTANCE 2026-11-18] the draw distance in use follows the frame rate (wolfadaptivedrawdistance.h).
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

#include "wolfadaptivedrawdistance.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"       // gDisconnected, LLAppViewer::logoutRequestSent
#include "llfocusmgr.h"
#include "llfloater.h"   // gFloaterView
#include "llfontgl.h"
#include "llrender2dutils.h"   // gl_rect_2d
#include "llframetimer.h"
#include "llperfstats.h"       // LLPerfStats::tunables (Auto-tune)
#include "llstartup.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"   // gSavedDrawDistance (Firestorm's draw distance stepping)
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llworld.h"
#include "wolfvrstream.h"      // WolfVRStream::headsetInVR

namespace
{
    // ---- How frames are judged ----------------------------------------------------------------------------------------
    // Source: llperfstats.cpp:371-379 StatsRecorder::updateAvatarParams - after the viewer regains focus Auto-tune waits
    // target_fps * 5 frames, 5 s at its target, before it tunes again. The same quiet time here after anything that makes
    // frames say nothing about the scene (log-in, a teleport, the progress screen, the window minimised or not in front).
    constexpr F64 SETTLE_SECS = 5.0;
    // The draw distance is lowered on the median frame time of the last second: "within about a second of a collapse"
    // (Paul's brief, 18 Nov). A median, not a mean, so a few long frames (a texture or mesh arriving) do not count.
    constexpr F64 CUT_WINDOW_SECS = 1.0;
    // A median of fewer frames than this is one frame's word: at 2 frames a second the cut waits for the third.
    constexpr size_t MIN_FRAMES = 3;
    // After a cut the next second is measured afresh: the far plane changes on the very next frame
    // (llviewerdisplay.cpp display_update_camera: final_far = gAgentCamera.mDrawDistance).
    constexpr F64 CUT_SETTLE_SECS = 1.0;
    // Source: llperfstats.cpp:410-415 - Auto-tune acts only outside its target frame time +- 10% (time_buf = target * 0.1).
    // The same 10%: frames are "slow" only below 0.9 x the target.
    constexpr F32 CUT_BELOW = 0.9f;
    // A raise goes up RAISE_STEP (10%) and so draws about 1.1^2 = 1.21 x the ground area; from 1.4 x the target that
    // leaves about 1.4 / 1.21 = 1.16 x, still well above CUT_BELOW: one raise cannot by itself bring the next cut. Between
    // 0.9 and 1.4 x the target nothing moves - the hysteresis band.
    constexpr F32 RAISE_AT = 1.4f;

    // ---- How far it moves ---------------------------------------------------------------------------------------------
    // Drawing cost follows what is in range, and on a world spread over the ground that grows with the area, d^2; so the
    // distance that would bring the frame rate to the target is d * sqrt(fps / target). Never more than halved in one step
    // (one bad second cannot take a 4096 m view straight to the floor), never less than the 10% band above.
    constexpr F32 CUT_FACTOR_MIN = 0.5f;
    constexpr F32 CUT_FACTOR_MAX = 0.9f;
    constexpr F32 RAISE_STEP = 0.1f;
    // Source: panel_preferences_graphics1.xml:548 - the Preferences draw distance slider moves in steps of 8 m.
    constexpr F32 RAISE_MIN_M = 8.f;
    // Raised only after this many whole seconds, every one of them smooth on its own median (stricter than one median over
    // the span): the same 5 s as SETTLE_SECS. From 256 m back to 4096 m is 1.1^n = 16, n = 29 steps, about 2.5 min
    // - "raises it again slowly".
    constexpr F32 RAISE_HOLD_SECS = 5.f;
    // A cut that comes within OVERSHOOT_SECS of a raise means that raise went too far: the wait before the next raise
    // doubles, up to RAISE_HOLD_MAX_SECS, and goes back to RAISE_HOLD_SECS after RELAX_SECS without a cut. 30 s is six
    // holds: long enough for the objects the region sends for the larger distance to arrive and show their cost.
    constexpr F64 OVERSHOOT_SECS = 30.0;
    constexpr F32 RAISE_HOLD_MAX_SECS = 80.f;
    constexpr F64 RELAX_SECS = 180.0;

    // ---- The settings' bounds -----------------------------------------------------------------------------------------
    // Source: panel_preferences_graphics1.xml:554-555 - the draw distance slider runs from 32 m to 4096 m. The floor's default, 128 m:
    // featuretable.txt:199-328 - the draw distance the viewer gives its Mid, MidHigh, High and HighUltra graphics levels.
    constexpr F32 FLOOR_MIN_M = 32.f, FLOOR_MAX_M = 4096.f;
    constexpr F32 TARGET_MIN_FPS = 5.f, TARGET_MAX_FPS = 120.f;

    // ---- State --------------------------------------------------------------------------------------------------------
    F32 sBase = -1.f;          // the resident's own draw distance (what gAgentCamera.mDrawDistance would be without us)
    F32 sApplied = -1.f;       // what we last wrote to gAgentCamera.mDrawDistance; -1 = we hold nothing
    F32 sCap = 0.f;            // 0 = no cap
    F32 sLastSetting = -1.f;   // RenderFarClip as last seen
    F64 sSettleUntil = 0.0;    // no measuring before this
    F64 sMeasureFrom = 0.0;    // when this measuring stretch began
    F64 sLastChange = 0.0, sLastCut = -1.0e9, sLastRaise = -1.0e9;
    F32 sRaiseHold = RAISE_HOLD_SECS;
    std::deque<std::pair<F64, F32>> sRecent;   // (when, frame seconds) of the last CUT_WINDOW_SECS
    std::vector<F32> sBucket;                  // this whole second's frame times
    F64 sBucketFrom = 0.0;
    std::deque<F32> sSeconds;                  // each whole second's median frame time, newest last
    const char* sAside = nullptr;              // why it stands aside now (nullptr = it does not); string literals only

    F32 median(std::vector<F32>& v)
    {
        const size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)mid, v.end());
        return v[mid];
    }

    void forgetFrames(F64 now, F64 quiet)
    {
        sRecent.clear();
        sBucket.clear();
        sSeconds.clear();
        sSettleUntil = llmax(sSettleUntil, now + quiet);
        sMeasureFrom = sSettleUntil;
        sBucketFrom = sSettleUntil;
    }

    // Source: llviewercontrol.cpp:195-205 handleRenderFarClipChanged - the two writes the stock viewer makes when the
    // draw distance changes.
    void putInUse(F32 metres)
    {
        gAgentCamera.mDrawDistance = metres;
        LLWorld::getInstance()->setLandFarClip(metres);
        sApplied = metres;
    }

    void release(F64 now, const char* why)
    {
        if (sCap > 0.f)
        {
            putInUse(sBase);
            LL_INFOS("WolfAdaptiveDD") << "released (" << why << "): draw distance back to the resident's own "
                                       << llformat("%.0f", sBase) << " m" << LL_ENDL;
        }
        sCap = 0.f;
        sApplied = -1.f;
        sRaiseHold = RAISE_HOLD_SECS;
        forgetFrames(now, 0.0);
    }

    /** The frame rate aimed for, and the cap the frame limiter or vsync puts on the frame rate (0 = none). */
    F32 targetFps(F32& ceiling)
    {
        static LLCachedControl<F32> target_setting(gSavedSettings, "WolfAdaptiveDrawDistanceTargetFPS", 20.f);
        // Source: llappviewer.cpp:2048-2058 (FIRE-22297) - with FSLimitFramerate on, frames are held to FramePerSecondLimit.
        static LLCachedControl<bool> limit_on(gSavedSettings, "FSLimitFramerate", true);
        static LLCachedControl<U32> limit_fps(gSavedSettings, "FramePerSecondLimit", 60);
        // Source: llperfstats.cpp:164, 370 - Auto-tune's target is held to the display's refresh rate while vsync is on.
        static LLCachedControl<bool> vsync(gSavedSettings, "RenderVSyncEnable", false);
        ceiling = 0.f;
        if (limit_on && limit_fps > 0)
        {
            ceiling = (F32)(U32)limit_fps;
        }
        if (vsync && gViewerWindow && gViewerWindow->getWindow())
        {
            // LLWindow::mRefreshRate starts at 0 (llwindow.cpp:131) and only some platforms fill it in
            const S32 hz = gViewerWindow->getWindow()->getRefreshRate();
            if (hz > 0)
            {
                ceiling = ceiling > 0.f ? llmin(ceiling, (F32)hz) : (F32)hz;
            }
        }
        F32 target = llclamp((F32)target_setting, TARGET_MIN_FPS, TARGET_MAX_FPS);
        if (ceiling > 0.f)
        {
            // at most 2/3 of the ceiling, so the raise level (RAISE_AT = 1.4 x, 0.93 of the ceiling) can still be reached by
            // a viewer running at its limit; else a limited viewer could be lowered and never raised again
            target = llmin(target, ceiling * (2.f / 3.f));
        }
        return target;
    }
}

namespace WolfAdaptiveDrawDistance
{

F32 userDrawDistance()
{
    return sCap > 0.f ? sBase : gAgentCamera.mDrawDistance;
}

bool holding()
{
    return sCap > 0.f && sCap < sBase;
}

void idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();

    // 1. The resident's own draw distance. Anything else that sets the draw distance in use (the Preferences slider through
    //    RenderFarClip, Auto-tune, Firestorm's stepping, a region's OpenSim draw distance limit in
    //    llpanelopenregionsettings.cpp, the chat bar's "dd" command) writes gAgentCamera.mDrawDistance; when it no longer
    //    holds what we wrote, or RenderFarClip moved, that value is the new own one.
    static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 256.f);
    const F32 setting = far_clip;
    if (sApplied < 0.f || gAgentCamera.mDrawDistance != sApplied || setting != sLastSetting)
    {
        sBase = gAgentCamera.mDrawDistance;
    }
    sLastSetting = setting;

    // 2. Standing aside: the cap goes at once.
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfAdaptiveDrawDistance", true);
    const char* aside = nullptr;
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        aside = "not logged in";
    }
    else if (!enabled)
    {
        aside = "switched off";
    }
    else if (LLPerfStats::tunables.userAutoTuneEnabled || LLPerfStats::tunables.userAutoTuneLock)
    {
        // Source: llperfstats.cpp:84 Tunables::applyUpdates - Auto-tune writes RenderFarClip itself while it runs
        aside = "Auto-tune is on";
    }
    else if (WolfVRStream::headsetInVR())
    {
        // Source: llviewerdisplay.cpp display_update_camera - in VR the eyes use WolfVRDrawDistance, not this
        aside = "VR headset";
    }
    if (aside != sAside)   // the same literal is the same pointer: only a change of reason is logged
    {
        if (aside)
        {
            LL_INFOS("WolfAdaptiveDD") << "standing aside: " << aside << LL_ENDL;
        }
        else
        {
            LL_INFOS("WolfAdaptiveDD") << "watching the frame rate" << LL_ENDL;
        }
        sAside = aside;
    }
    if (aside)
    {
        release(now, aside);
        return;
    }

    // 3. The cap, written every frame it is held (min of the resident's own and the cap).
    static LLCachedControl<F32> floor_setting(gSavedSettings, "WolfAdaptiveDrawDistanceFloor", 128.f);
    const F32 floor_m = llclamp((F32)floor_setting, FLOOR_MIN_M, FLOOR_MAX_M);
    if (sCap > 0.f && sCap >= sBase)
    {
        release(now, "the resident's own draw distance is no further than the cap");
    }
    if (sCap > 0.f && sBase <= floor_m)
    {
        release(now, "the resident's own draw distance is at or under the floor");
    }
    if (sCap > 0.f && gAgentCamera.mDrawDistance != sCap)
    {
        putInUse(sCap);
    }

    // 4. Frames that say nothing about the scene are not measured; the cap stays as it is.
    LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    const bool quiet = gDisconnected
        || LLAppViewer::instance()->logoutRequestSent()
        || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE
        || !gViewerWindow || !window
        || gViewerWindow->getShowProgress()
        // Source: llappviewer.cpp:1966-1985 - not visible or not in front, every frame sleeps BackgroundYieldTime
        || window->getMinimized() || !window->getVisible() || !gFocusMgr.getAppHasFocus()
        // Source: llviewerdisplay.cpp:875 - Firestorm's draw distance stepping after a teleport is running
        || gSavedDrawDistance > 0.f;
    const F32 frame = (F32)gFrameIntervalSeconds.value();
    if (quiet || !(frame > 0.f))
    {
        forgetFrames(now, SETTLE_SECS);
        return;
    }
    if (now < sSettleUntil)
    {
        return;
    }

    F32 ceiling = 0.f;
    const F32 target = targetFps(ceiling);

    // 5. Lowering: the last second's median.
    sRecent.emplace_back(now, frame);
    while (sRecent.size() > MIN_FRAMES && sRecent.front().first < now - CUT_WINDOW_SECS)
    {
        sRecent.pop_front();
    }
    const F32 in_use = sCap > 0.f ? llmin(sBase, sCap) : sBase;
    if (now - sMeasureFrom >= CUT_WINDOW_SECS && sRecent.size() >= MIN_FRAMES && in_use > floor_m)
    {
        std::vector<F32> times;
        times.reserve(sRecent.size());
        for (const auto& s : sRecent)
        {
            times.push_back(s.second);
        }
        const F32 fps = 1.f / llmax(median(times), 0.001f);
        if (fps < CUT_BELOW * target)
        {
            const F32 factor = llclamp(std::sqrt(fps / target), CUT_FACTOR_MIN, CUT_FACTOR_MAX);
            const F32 cap = llmax(floor_m, in_use * factor);
            LL_INFOS("WolfAdaptiveDD") << "lowered " << llformat("%.0f", in_use) << " -> " << llformat("%.0f", cap)
                                       << " m (median " << llformat("%.1f", fps) << " fps over " << times.size()
                                       << " frames, target " << llformat("%.1f", target) << ", floor "
                                       << llformat("%.0f", floor_m) << ", resident's own " << llformat("%.0f", sBase)
                                       << " m)" << LL_ENDL;
            if (now - sLastRaise < OVERSHOOT_SECS)
            {
                sRaiseHold = llmin(sRaiseHold * 2.f, RAISE_HOLD_MAX_SECS);
                LL_INFOS("WolfAdaptiveDD") << "the last raise went too far: the next waits "
                                           << llformat("%.0f", sRaiseHold) << " s" << LL_ENDL;
            }
            sCap = cap;
            putInUse(cap);
            sLastCut = sLastChange = now;
            forgetFrames(now, CUT_SETTLE_SECS);
            return;
        }
    }

    // 6. Raising: whole seconds, each smooth on its own.
    if (sBucket.empty())
    {
        sBucketFrom = now;
    }
    sBucket.push_back(frame);
    if (now - sBucketFrom >= 1.0)
    {
        sSeconds.push_back(median(sBucket));
        sBucket.clear();
        while (sSeconds.size() > (size_t)RAISE_HOLD_MAX_SECS)
        {
            sSeconds.pop_front();
        }
    }
    if (sCap <= 0.f)
    {
        return;
    }
    if (now - sLastCut > RELAX_SECS)
    {
        sRaiseHold = RAISE_HOLD_SECS;
    }
    const size_t need = (size_t)sRaiseHold;
    if (now - sLastChange < (F64)sRaiseHold || sSeconds.size() < need)
    {
        return;
    }
    const F32 raise_fps = RAISE_AT * target;
    for (size_t i = sSeconds.size() - need; i < sSeconds.size(); ++i)
    {
        if (1.f / llmax(sSeconds[i], 0.001f) < raise_fps)
        {
            return;
        }
    }
    const F32 cap = llmax(sCap * (1.f + RAISE_STEP), sCap + RAISE_MIN_M);
    if (cap >= sBase)
    {
        sLastRaise = sLastChange = now;
        release(now, "frames smooth");
        return;
    }
    LL_INFOS("WolfAdaptiveDD") << "raised " << llformat("%.0f", sCap) << " -> " << llformat("%.0f", cap) << " m (" << need
                               << " s each at " << llformat("%.0f", raise_fps) << " fps or better, resident's own "
                               << llformat("%.0f", sBase) << " m)" << LL_ENDL;
    sCap = cap;
    putInUse(cap);
    sLastRaise = sLastChange = now;
    forgetFrames(now, 0.0);
}

void drawNote()
{
    if (!holding())
    {
        return;
    }
    LLStringUtil::format_map_t args;
    args["NOW"] = llformat("%.0f", llmin(sCap, sBase));
    args["YOURS"] = llformat("%.0f", sBase);
    const std::string text = LLTrans::getString("WolfAdaptiveDrawNote", args);
    // Just above the bottom toolbar, centred: the bottom of the floaters' area (LLFloaterView::getSnapRect, the toolbars'
    // floater_snap_region, lltoolbarview.cpp) clears the bottom toolbar. Not at the top: the navigation and favourites
    // bars (main_view.xml navigation_bar) lie over that area's top, where the first two in-world runs (18 Nov) drew the
    // note hidden.
    LLRect view = gViewerWindow->getWorldViewRectScaled();
    if (gFloaterView)
    {
        gFloaterView->localRectToScreen(gFloaterView->getSnapRect(), &view);
    }
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    // white on black at 0.6 over the brightest sky is about #666 behind the text: 5.7:1, over the 4.5:1 floor
    const S32 pad = 6, bottom = view.mBottom + 8, top = bottom + font->getLineHeight() + 4;
    const S32 half = font->getWidth(text) / 2 + pad, cx = view.getCenterX();
    gl_rect_2d(cx - half, top, cx + half, bottom, LLColor4(0.f, 0.f, 0.f, 0.6f));
    // Source: llviewerwindow.cpp drawMouselookInstructions - the same kind of note, centred over the world view
    font->renderUTF8(text, 0, cx, top - 2, LLColor4::white, LLFontGL::HCENTER, LLFontGL::TOP);
}

}
