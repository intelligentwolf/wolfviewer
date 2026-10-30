/**
 * @file wolfframegovernor.cpp
 * @brief WolfViewer: a budget for optional per-frame work, set from the MEASURED frame time.
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

#include "wolfframegovernor.h"

#include "llframetimer.h"
#include "llviewerpartsim.h"

#include <cmath>

namespace
{
    F32 sSmoothed = 1.f / 30.f;   // seconds: starts in the hysteresis band, so nothing moves before frames are measured
    F32 sBudget = 1.f;
    bool sHaveFrame = false;
    // the particles' measured cost since the last log line (addParticle*Seconds) and the frames it covers
    F64 sSimS = 0.0, sGeomS = 0.0;
    U32 sFrames = 0;
    F64 sNextLog = 0.0;
    S32 sLastBudgetStep = -1, sLastFpsStep = -1;
}

// static
void WolfFrameGovernor::update()
{
    // Source: llframetimer.h:73 getFrameDeltaTimeF32 - the real time between this frame and the last. A single frame longer than
    // half a second (a teleport, a file dialog, a hitch) is counted as half a second, so one stall does not swing the budget.
    const F32 dt = llmin(LLFrameTimer::getFrameDeltaTimeF32(), 0.5f);
    if (!(dt > 0.f)) return;
    if (!sHaveFrame)
    {
        sHaveFrame = true;
        return;   // the first frame's delta is the time since start-up, not a frame
    }
    sSmoothed += (dt - sSmoothed) * (1.f - expf(-dt / SMOOTH_TAU_S));
    if (sSmoothed > SLOW_FRAME_S)
    {
        sBudget = llmax(MIN_BUDGET, sBudget * expf(-dt / DOWN_TAU_S));
    }
    else if (sSmoothed < FAST_FRAME_S)
    {
        sBudget = llmin(1.f, sBudget * expf(dt / UP_TAU_S));
    }
    ++sFrames;

    // the log: when the budget (to 0.05) or the frame rate (to 2 fps) changed, and at most every LOG_MIN_S
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const S32 budget_step = (S32)floorf(sBudget * 20.f + 0.5f);
    const S32 fps_step = (S32)floorf(0.5f / sSmoothed + 0.5f);
    if ((budget_step != sLastBudgetStep || fps_step != sLastFpsStep) && now >= sNextLog)
    {
        sNextLog = now + LOG_MIN_S;
        sLastBudgetStep = budget_step;
        sLastFpsStep = fps_step;
        const F64 frames = (F64)llmax(sFrames, 1u);
        LL_INFOS("WolfGovernor") << "frame governor: " << llformat("%.1f", 1.f / sSmoothed) << " fps (" << llformat("%.1f", sSmoothed * 1000.f)
                                 << " ms), budget " << llformat("%.2f", sBudget)
                                 << "; particles " << LLViewerPartSim::getPartCount() << " live (max " << LLViewerPartSim::getMaxPartCount() << ")"
                                 << ", simulation " << llformat("%.2f", sSimS * 1000.0 / frames) << " ms and geometry "
                                 << llformat("%.2f", sGeomS * 1000.0 / frames) << " ms a frame on the CPU (over " << sFrames << " frames)"
                                 << LL_ENDL;
        sSimS = sGeomS = 0.0;
        sFrames = 0;
    }
}

// static
F32 WolfFrameGovernor::budget()
{
    return sBudget;
}

// static
F32 WolfFrameGovernor::frameSeconds()
{
    return sSmoothed;
}

// static
void WolfFrameGovernor::addParticleSimSeconds(F64 s)
{
    sSimS += s;
}

// static
void WolfFrameGovernor::addParticleGeometrySeconds(F64 s)
{
    sGeomS += s;
}
