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
#include "wolfenvironmentrender.h"   // WolfEnvironmentRender::skyPassGpuMs
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
    // A raise is sized (RAISE_AIM below) to land in the middle of the band between CUT_BELOW and RAISE_AT, so one raise
    // cannot by itself bring the next cut. Between 0.9 and 1.4 x the target nothing moves - the hysteresis band.
    constexpr F32 RAISE_AT = 1.4f;

    // ---- Does a cut help? [ADAPTIVE DD CHECKS ITS CUTS 2026-11-18] ---------------------------------------------------
    // Paul at Tafelberg, 18 Nov 16:56-16:57 (W106 beta log): cut 2085 -> 1876 -> 1303 -> 914 -> 639 -> 431 -> 284 -> 187 m,
    // every step at 8.7-9.8 fps; then 187 -> 136 m at 17.8 fps, the same ~20 fps it had at 4096 m. The frame rate did not
    // follow the draw distance (his GPU was throttling), so every cut took world away and bought nothing. Now each cut is a
    // trial: the frame time after it is compared with the frame time before it, and a cut that bought nothing is undone.
    //
    // After a cut (or the trial's step back) the frames of EVAL_SETTLE_SECS are not judged: in Paul's log the land and the
    // regions answered a cut within two seconds (16:56:14-16:56:19 terrain "tex built" for the neighbours after each step,
    // 16:57:09 a region dropped two seconds after the 16:57:06 cut). Then EVAL_SECS whole seconds, each on its own median,
    // and the median of those: one slow second (a mesh arriving) cannot decide a trial.
    constexpr F64 EVAL_SETTLE_SECS = 2.0;
    constexpr size_t EVAL_SECS = 3;
    // "Before" is the median of up to the last PRE_SECS whole seconds at the old distance, not the second that set the cut
    // off: that second was picked for being slow, so the next ones look better by chance alone (in Paul's log the trigger
    // seconds read 17.8 fps among seconds of 20-21 fps, which would have "proved" every cut).
    constexpr size_t PRE_SECS = 3;
    // A cut helps when it takes at least GAIN_MIN off the frame time. Paul's brief (18 Nov): "frame time within a few
    // percent ... stop cutting". Measured noise (Xvfb runs of 18 Nov, adaptive off, still camera, outside the GPU's slow
    // spells): a 3-second median of per-second medians moved by 1-5% from one window to the next at Tafelberg and Paddington,
    // so 5% is at the top of the noise; a cut wrongly called useless is caught by the probe nearer in (below), one wrongly
    // called useful is judged again by the next trial, which compares with this cut's own unselected seconds.
    constexpr F32 GAIN_MIN = 0.05f;
    // A cut that did not clearly help is not called useless on that alone: a probe much nearer in (PROBE_FACTOR, below) is
    // measured too, and only when that is no faster than the cut by GAIN_MIN is cutting useless here.
    //
    // Then no cut for HOLD_SECS, doubling up to HOLD_MAX_SECS while trials keep failing, unless frames get CLEARLY_WORSE
    // than the frame time measured at the distance kept (sustained: the median of PRE_SECS seconds). 1.25 is more than
    // twice the 10% band either side of the target (llperfstats.cpp:410 time_buf): a change that size is not noise; it
    // is new work, which may be new objects in range.
    //
    // The GPU's own speed. A laptop GPU slowed by heat changes every frame time with no change in the scene: in Paul's
    // session the fixed-size sky dome pass (WolfEnvironmentRender::skyPassGpuMs) took 1.67 ms a frame at 16:50, 3.62 at
    // 16:51, 5.60 at 16:52, 6.87 at 16:53 (WolfEnvGPU lines) while his frames fell from 92 to 31 fps with the same 331-341
    // objects in range; on this machine in the 18 Nov Xvfb test nvidia-smi showed "SW Thermal Slowdown: Active" and the clock
    // swinging between 390 and 950 MHz (of 3090), and with it the frame time between 28 and 66 ms at one spot and one
    // distance. A trial whose sky pass time moved by more than GPU_SPEED_CHANGE between "before" and "after" compared two
    // speeds of GPU, not two distances: it is void (the old distance goes back) and no cut is tried for HOLD_SECS, unless
    // frames get CLEARLY_WORSE. Twice GAIN_MIN: a GPU that changed speed by that much can by itself move a GPU-bound frame
    // by twice the gain a cut must show. The 3-second sky pass median is steady enough for it: 0.49-0.50 ms at Tafelberg,
    // 0.71-0.73 ms at Paddington between trials of the third Xvfb run (18 Nov), and 0.81 -> 0.95 ms when the clock fell.
    constexpr F32 GPU_SPEED_CHANGE = 2.f * GAIN_MIN;
    // The probe nearer in after a cut that showed nothing: three of the strongest cuts (CUT_FACTOR_MIN^3 = 1/8), which
    // reaches inside the station at Paddington (4096 -> 3686 m, probe 461 m; the frames there: 65 ms at 1024 m, 37 ms at
    // 384 m) and is never under the floor.
    constexpr F32 PROBE_FACTOR = 0.125f;
    constexpr F64 HOLD_SECS = 120.0;
    constexpr F64 HOLD_MAX_SECS = 480.0;
    constexpr F32 CLEARLY_WORSE = 1.25f;

    // ---- How far it moves ---------------------------------------------------------------------------------------------
    // Drawing cost follows what is in range, and on a world spread over the ground that grows with the area, d^2; so the
    // distance that would bring the frame rate to the target is d * sqrt(fps / target). Never more than halved in one step
    // (one bad second cannot take a 4096 m view straight to the floor), never less than the 10% band above.
    constexpr F32 CUT_FACTOR_MIN = 0.5f;
    constexpr F32 CUT_FACTOR_MAX = 0.9f;
    // Raising, by the same d^2 model the other way: from fps f the distance may grow by sqrt(f / aim), aim being the
    // geometric middle of the hysteresis band, sqrt(CUT_BELOW * RAISE_AT) = 1.12 x the target. The model counts every
    // millisecond as distance work, so it overstates what a raise costs and the landing is above the aim, not below. At
    // least RAISE_STEP (10%, the old fixed step); at most RAISE_FACTOR_MAX, the inverse of the strongest cut (x 2 = 1 / 0.5).
    const F32 RAISE_AIM = std::sqrt(CUT_BELOW * RAISE_AT);
    constexpr F32 RAISE_STEP = 0.1f;
    constexpr F32 RAISE_FACTOR_MAX = 1.f / CUT_FACTOR_MIN;
    // Source: panel_preferences_graphics1.xml:548 - the Preferences draw distance slider moves in steps of 8 m.
    constexpr F32 RAISE_MIN_M = 8.f;
    // Raised only after this many whole seconds, every one of them smooth on its own median (stricter than one median over
    // the span): the same 5 s as SETTLE_SECS. When every one of the last RAISE_FAST_SECS seconds runs at RAISE_FAST_AT x the
    // target or better, that is wait enough: at twice the target a frame has more than the whole hysteresis band (1.4 x)
    // to spare. Paul, 18 Nov: recovery at 10% every 5 s was too slow; from 136 m back to 4096 m (x 30) at three times the
    // target the steps are now x 1.64 every 2 s, 7 steps, about 15 s (it was 36 steps, 3 minutes).
    constexpr F32 RAISE_HOLD_SECS = 5.f;
    constexpr F32 RAISE_FAST_SECS = 2.f;
    constexpr F32 RAISE_FAST_AT = 2.f;
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
    std::vector<F32> sSkyBucket;               // this whole second's sky pass GPU times (ms)
    std::deque<F32> sSkySeconds;               // each whole second's median sky pass GPU time, -1 = none; beside sSeconds
    const char* sAside = nullptr;              // why it stands aside now (nullptr = it does not); string literals only

    // the trial of a cut
    enum class Phase { WATCH, JUDGE_CUT, JUDGE_PROBE };
    Phase sPhase = Phase::WATCH;
    F32 sTrialFrom = 0.f, sTrialTo = 0.f;      // the distance in use before and after the cut on trial
    F32 sTrialPre = 0.f, sTrialPost = 0.f;     // frame seconds before the cut, and with it
    F32 sTrialProbe = 0.f;                     // the distance of the probe nearer in (PROBE_FACTOR)
    bool sHelped = false;                      // a cut has helped since the cap was last let go
    F64 sHoldUntil = 0.0;                      // no cut before this, unless frames get CLEARLY_WORSE than sPlateau
    F64 sHoldLen = HOLD_SECS;
    F32 sPlateau = 0.f;                        // frame seconds measured at the distance kept when a trial failed
    F32 sTrialPreSky = -1.f, sTrialPostSky = -1.f;   // the sky pass GPU ms over the same seconds (-1 = not known)

    F32 median(std::vector<F32>& v)
    {
        const size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)mid, v.end());
        return v[mid];
    }

    /** The median of the last n whole seconds' medians (n <= sSeconds.size()). */
    F32 lastSeconds(size_t n)
    {
        std::vector<F32> v(sSeconds.end() - (std::ptrdiff_t)n, sSeconds.end());
        return median(v);
    }

    /** The median sky pass GPU time (ms) over the last n whole seconds that have one; -1 when none of them has. */
    F32 lastSky(size_t n)
    {
        std::vector<F32> v;
        for (size_t i = sSkySeconds.size() - llmin(n, sSkySeconds.size()); i < sSkySeconds.size(); ++i)
        {
            if (sSkySeconds[i] > 0.f)
            {
                v.push_back(sSkySeconds[i]);
            }
        }
        return v.empty() ? -1.f : median(v);
    }

    /** True when both sky pass times are known and differ by more than GPU_SPEED_CHANGE. */
    bool gpuSpeedChanged(F32 sky_a, F32 sky_b)
    {
        return sky_a > 0.f && sky_b > 0.f && std::fabs(sky_b / sky_a - 1.f) > GPU_SPEED_CHANGE;
    }

    std::string skyText(F32 sky) { return sky > 0.f ? llformat(", sky pass %.2f ms", sky) : std::string(); }

    std::string msText(F32 secs) { return llformat("%.1f ms (%.1f fps)", secs * 1000.f, 1.f / llmax(secs, 0.001f)); }

    void forgetFrames(F64 now, F64 quiet)
    {
        sRecent.clear();
        sBucket.clear();
        sSeconds.clear();
        sSkyBucket.clear();
        sSkySeconds.clear();
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
        sPhase = Phase::WATCH;
        sHelped = false;
        forgetFrames(now, 0.0);
    }

    /** A trial cut was useless (or void): no cut for a while (unless frames get clearly worse than now). A useless trial
     *  doubles the next hold; a void one does not (it learnt nothing about the distance). */
    void holdOff(F64 now, F32 plateau, bool escalate)
    {
        sPlateau = plateau;
        sHoldUntil = now + sHoldLen;
        LL_INFOS("WolfAdaptiveDD") << "no further cuts for " << llformat("%.0f", sHoldLen) << " s unless frames get "
                                   << llformat("%.0f", (CLEARLY_WORSE - 1.f) * 100.f) << "% slower than " << msText(plateau) << LL_ENDL;
        if (escalate)
        {
            sHoldLen = llmin(sHoldLen * 2.0, HOLD_MAX_SECS);
        }
    }

    void endHold()
    {
        sHoldUntil = 0.0;
        sHoldLen = HOLD_SECS;
    }

    void applyCap(F32 cap);   // below, beside targetFps

    /** The trial showed the distance does not count: back to the distance before the cut when an earlier cut of this
     *  stretch helped, else to the resident's own; then no cut for a while (each useless trial doubles the next wait). */
    void endTrialUseless(F64 now)
    {
        sPhase = Phase::WATCH;
        if (sHelped && sTrialFrom < sBase)
        {
            LL_INFOS("WolfAdaptiveDD") << "back to " << llformat("%.0f", sTrialFrom) << " m, the distance before the useless cut"
                                       << LL_ENDL;
            applyCap(sTrialFrom);
            sLastChange = now;
            forgetFrames(now, EVAL_SETTLE_SECS);
        }
        else
        {
            release(now, "cutting did not help");
        }
        holdOff(now, sTrialPre, true);
    }

    /** The GPU changed speed during the trial: back to the distance before the cut (or the resident's own), no verdict. */
    void endTrialVoid(F64 now)
    {
        sPhase = Phase::WATCH;
        if (sHelped && sTrialFrom < sBase)
        {
            applyCap(sTrialFrom);
            sLastChange = now;
            forgetFrames(now, EVAL_SETTLE_SECS);
        }
        else
        {
            release(now, "the trial was void");
        }
        holdOff(now, sTrialPost, false);
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

    /** Puts a cap in use (min of it and the resident's own), as every frame of step 3 does. */
    void applyCap(F32 cap)
    {
        sCap = cap;
        putInUse(llmin(cap, sBase));
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
        if (sBase != gAgentCamera.mDrawDistance && sPhase != Phase::WATCH)
        {
            // the resident (or Auto-tune, or the region) chose a new distance in the middle of a trial: the trial is void
            LL_INFOS("WolfAdaptiveDD") << "trial dropped: the resident's own draw distance changed" << LL_ENDL;
            sPhase = Phase::WATCH;
        }
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
        endHold();
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
    if (sCap > 0.f && gAgentCamera.mDrawDistance != llmin(sCap, sBase))
    {
        putInUse(llmin(sCap, sBase));
    }

    // 4. Frames that say nothing about the scene are not measured; the cap stays as it is, and a trial under way is dropped
    //    (its frames would compare one scene with another).
    LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    const bool quiet = gDisconnected
        || LLAppViewer::instance()->logoutRequestSent()
        || teleporting
        || !gViewerWindow || !window
        || gViewerWindow->getShowProgress()
        // Source: llappviewer.cpp:1966-1985 - not visible or not in front, every frame sleeps BackgroundYieldTime
        || window->getMinimized() || !window->getVisible() || !gFocusMgr.getAppHasFocus()
        // Source: llviewerdisplay.cpp:875 - Firestorm's draw distance stepping after a teleport is running
        || gSavedDrawDistance > 0.f;
    const F32 frame = (F32)gFrameIntervalSeconds.value();
    if (quiet || !(frame > 0.f))
    {
        if (sPhase != Phase::WATCH)
        {
            LL_INFOS("WolfAdaptiveDD") << "trial dropped: frames stopped saying anything about the scene; "
                                       << llformat("%.0f", llmin(sCap > 0.f ? sCap : sBase, sBase)) << " m stays in use" << LL_ENDL;
            sPhase = Phase::WATCH;
        }
        if (teleporting)
        {
            endHold();   // somewhere else: what was learnt about this scene says nothing there
        }
        forgetFrames(now, SETTLE_SECS);
        return;
    }
    if (now < sSettleUntil)
    {
        return;
    }

    F32 ceiling = 0.f;
    const F32 target = targetFps(ceiling);

    // 5. The frames: the last second for lowering, whole seconds for judging trials and for raising.
    sRecent.emplace_back(now, frame);
    while (sRecent.size() > MIN_FRAMES && sRecent.front().first < now - CUT_WINDOW_SECS)
    {
        sRecent.pop_front();
    }
    if (sBucket.empty())
    {
        sBucketFrom = now;
    }
    sBucket.push_back(frame);
    const F32 sky = WolfEnvironmentRender::skyPassGpuMs();
    if (sky > 0.f)
    {
        sSkyBucket.push_back(sky);
    }
    if (now - sBucketFrom >= 1.0)
    {
        sSeconds.push_back(median(sBucket));
        sSkySeconds.push_back(sSkyBucket.empty() ? -1.f : median(sSkyBucket));
        sBucket.clear();
        sSkyBucket.clear();
        while (sSeconds.size() > (size_t)RAISE_HOLD_MAX_SECS)
        {
            sSeconds.pop_front();
            sSkySeconds.pop_front();
        }
    }

    // 6. A cut on trial: did it take frame time off?
    if (sPhase == Phase::JUDGE_CUT)
    {
        if (sSeconds.size() < EVAL_SECS)
        {
            return;
        }
        sTrialPost = lastSeconds(EVAL_SECS);
        sTrialPostSky = lastSky(EVAL_SECS);
        if (gpuSpeedChanged(sTrialPreSky, sTrialPostSky))
        {
            LL_INFOS("WolfAdaptiveDD") << "trial void: the GPU's own speed changed during it (sky pass "
                                       << llformat("%.2f -> %.2f", sTrialPreSky, sTrialPostSky) << " ms; frames " << msText(sTrialPre)
                                       << " -> " << msText(sTrialPost) << "): " << llformat("%.0f", sTrialFrom) << " m back" << LL_ENDL;
            endTrialVoid(now);
            return;
        }
        if (sTrialPost <= sTrialPre * (1.f - GAIN_MIN))
        {
            LL_INFOS("WolfAdaptiveDD") << "the cut to " << llformat("%.0f", sTrialTo) << " m helped: " << msText(sTrialPre) << " -> "
                                       << msText(sTrialPost) << skyText(sTrialPostSky) << LL_ENDL;
            sHelped = true;
            sHoldLen = HOLD_SECS;   // the frames follow the distance here: a later useless trial starts the holds afresh
            sPhase = Phase::WATCH;
            return;
        }
        // Not clearly better. The world is not spread evenly: at Paddington Station (Xvfb test, 18 Nov) the frame took 58-73
        // ms at 4096 m, 65 ms at 1024 m and 37 ms at 384 m - nearly all the cost was the station itself, so a 10% cut
        // from 4096 m showed nothing although a deep one helps a lot. Before calling the distance useless, a probe much
        // nearer in (PROBE_FACTOR) is measured; only when that is no better either is the frame time taken not to follow
        // the distance at all.
        const F32 probe = llmax(floor_m, sTrialTo * PROBE_FACTOR);
        if (probe < sTrialTo)
        {
            LL_INFOS("WolfAdaptiveDD") << "the cut to " << llformat("%.0f", sTrialTo) << " m did not clearly help (" << msText(sTrialPre)
                                       << skyText(sTrialPreSky) << " -> " << msText(sTrialPost) << skyText(sTrialPostSky)
                                       << "): measuring " << llformat("%.0f", probe) << " m for a few seconds" << LL_ENDL;
            sTrialProbe = probe;
            sPhase = Phase::JUDGE_PROBE;
            applyCap(probe);
            sLastChange = now;
            forgetFrames(now, EVAL_SETTLE_SECS);
            return;
        }
        LL_INFOS("WolfAdaptiveDD") << "cutting does not help here: " << llformat("%.0f", sTrialFrom) << " m was " << msText(sTrialPre)
                                   << skyText(sTrialPreSky) << ", " << llformat("%.0f", sTrialTo) << " m is " << msText(sTrialPost)
                                   << skyText(sTrialPostSky) << " (at the floor: nothing nearer to try)" << LL_ENDL;
        endTrialUseless(now);
        return;
    }
    // 7. ... and the probe nearer in
    if (sPhase == Phase::JUDGE_PROBE)
    {
        if (sSeconds.size() < EVAL_SECS)
        {
            return;
        }
        const F32 at_probe = lastSeconds(EVAL_SECS);
        const F32 probe_sky = lastSky(EVAL_SECS);
        sPhase = Phase::WATCH;
        if (gpuSpeedChanged(sTrialPostSky, probe_sky))
        {
            LL_INFOS("WolfAdaptiveDD") << "trial void: the GPU's own speed changed during it (sky pass "
                                       << llformat("%.2f -> %.2f", sTrialPostSky, probe_sky) << " ms; " << llformat("%.0f", sTrialTo)
                                       << " m was " << msText(sTrialPost) << ", " << llformat("%.0f", sTrialProbe) << " m is "
                                       << msText(at_probe) << "): " << llformat("%.0f", sTrialFrom) << " m back" << LL_ENDL;
            endTrialVoid(now);
            return;
        }
        if (at_probe <= sTrialPost * (1.f - GAIN_MIN))
        {
            // the cost lies nearer in: the probe is kept; raising (step 9) takes it out again while frames allow
            LL_INFOS("WolfAdaptiveDD") << "the distance does count, nearer in: " << llformat("%.0f", sTrialProbe) << " m is "
                                       << msText(at_probe) << skyText(probe_sky) << ", " << llformat("%.0f", sTrialTo) << " m was "
                                       << msText(sTrialPost) << "; keeping " << llformat("%.0f", sTrialProbe) << " m" << LL_ENDL;
            sHelped = true;
            sHoldLen = HOLD_SECS;
            sLastCut = sLastChange = now;
            return;
        }
        // The frame time does not follow the draw distance here: something else holds the frames (a GPU or CPU slowed by
        // heat, a heavy shader or light near the camera). Shrinking the world cannot fix that, so it stops.
        LL_INFOS("WolfAdaptiveDD") << "cutting does not help here: " << llformat("%.0f", sTrialFrom) << " m was " << msText(sTrialPre)
                                   << ", " << llformat("%.0f", sTrialTo) << " m " << msText(sTrialPost) << ", "
                                   << llformat("%.0f", sTrialProbe) << " m " << msText(at_probe) << skyText(probe_sky) << LL_ENDL;
        endTrialUseless(now);
        return;
    }

    // 8. Lowering: the last second's median.
    const F32 in_use = sCap > 0.f ? llmin(sBase, sCap) : sBase;
    if (now - sMeasureFrom >= CUT_WINDOW_SECS && sRecent.size() >= MIN_FRAMES && in_use > floor_m)
    {
        std::vector<F32> times;
        times.reserve(sRecent.size());
        for (const auto& s : sRecent)
        {
            times.push_back(s.second);
        }
        const F32 window_secs = median(times);
        const F32 fps = 1.f / llmax(window_secs, 0.001f);
        bool cut = fps < CUT_BELOW * target;
        if (cut && now < sHoldUntil)
        {
            // held off after a useless trial: only frames that stay clearly slower than they were then
            const size_t n = llmin(sSeconds.size(), PRE_SECS);
            cut = n >= PRE_SECS && lastSeconds(n) >= sPlateau * CLEARLY_WORSE && window_secs >= sPlateau * CLEARLY_WORSE;
            if (cut)
            {
                LL_INFOS("WolfAdaptiveDD") << "frames clearly slower than when cuts were held off (" << msText(sPlateau) << " -> "
                                           << msText(lastSeconds(n)) << "): cuts are tried again" << LL_ENDL;
                sHoldUntil = 0.0;   // the hold's length stays doubled: a second useless trial holds off longer
            }
        }
        if (cut)
        {
            const F32 factor = llclamp(std::sqrt(fps / target), CUT_FACTOR_MIN, CUT_FACTOR_MAX);
            const F32 cap = llmax(floor_m, in_use * factor);
            // before: the last whole seconds at this distance (see PRE_SECS), or this second when there are none
            const size_t n = llmin(sSeconds.size(), PRE_SECS);
            sTrialPre = n > 0 ? lastSeconds(n) : window_secs;
            sTrialPreSky = n > 0 ? lastSky(n) : WolfEnvironmentRender::skyPassGpuMs();
            sTrialFrom = in_use;
            sTrialTo = cap;
            LL_INFOS("WolfAdaptiveDD") << "lowered " << llformat("%.0f", in_use) << " -> " << llformat("%.0f", cap)
                                       << " m (median " << llformat("%.1f", fps) << " fps over " << times.size()
                                       << " frames, target " << llformat("%.1f", target) << ", floor "
                                       << llformat("%.0f", floor_m) << ", resident's own " << llformat("%.0f", sBase)
                                       << " m; before: " << msText(sTrialPre) << skyText(sTrialPreSky) << ")" << LL_ENDL;
            if (now - sLastRaise < OVERSHOOT_SECS)
            {
                sRaiseHold = llmin(sRaiseHold * 2.f, RAISE_HOLD_MAX_SECS);
                LL_INFOS("WolfAdaptiveDD") << "the last raise went too far: the next waits "
                                           << llformat("%.0f", sRaiseHold) << " s" << LL_ENDL;
            }
            applyCap(cap);
            sPhase = Phase::JUDGE_CUT;
            sLastCut = sLastChange = now;
            forgetFrames(now, EVAL_SETTLE_SECS);
            return;
        }
    }

    // 9. Raising: whole seconds, each smooth on its own; sooner, and by more, the faster they are.
    if (sCap <= 0.f)
    {
        return;
    }
    if (now - sLastCut > RELAX_SECS)
    {
        sRaiseHold = RAISE_HOLD_SECS;
    }
    // the slowest of the last n seconds, as frames a second (0 when there are not n yet)
    auto slowest_fps = [](size_t n) -> F32
    {
        if (n == 0 || sSeconds.size() < n)
        {
            return 0.f;
        }
        F32 worst = 0.f;
        for (size_t i = sSeconds.size() - n; i < sSeconds.size(); ++i)
        {
            worst = llmax(worst, sSeconds[i]);
        }
        return 1.f / llmax(worst, 0.001f);
    };
    const size_t need = (size_t)sRaiseHold;
    const size_t need_fast = (size_t)std::ceil(sRaiseHold * (RAISE_FAST_SECS / RAISE_HOLD_SECS));
    size_t waited = 0;
    F32 f = 0.f;
    if (now - sLastChange >= (F64)need_fast && (f = slowest_fps(need_fast)) >= RAISE_FAST_AT * target)
    {
        waited = need_fast;
    }
    else if (now - sLastChange >= (F64)need && (f = slowest_fps(need)) >= RAISE_AT * target)
    {
        waited = need;
    }
    if (waited == 0)
    {
        return;
    }
    const F32 factor = llclamp(std::sqrt(f / (RAISE_AIM * target)), 1.f + RAISE_STEP, RAISE_FACTOR_MAX);
    const F32 cap = llmax(sCap * factor, sCap + RAISE_MIN_M);
    if (cap >= sBase)
    {
        sLastRaise = sLastChange = now;
        release(now, "frames smooth");
        return;
    }
    LL_INFOS("WolfAdaptiveDD") << "raised " << llformat("%.0f", sCap) << " -> " << llformat("%.0f", cap) << " m (x"
                               << llformat("%.2f", factor) << ": " << waited << " s each at " << llformat("%.0f", f)
                               << " fps or better, target " << llformat("%.1f", target) << ", resident's own "
                               << llformat("%.0f", sBase) << " m)" << LL_ENDL;
    applyCap(cap);
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
