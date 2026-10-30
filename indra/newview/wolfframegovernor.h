/**
 * @file wolfframegovernor.h
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

#ifndef WOLF_FRAME_GOVERNOR_H
#define WOLF_FRAME_GOVERNOR_H

#include "stdtypes.h"

/**
 * [HURRICANE 2026-10-30] Paul at The Caribbean under the storm (ticket #367): about 11-12 frames a second, "the waves are getting
 * jerky", "probably need to compensate for it i'm at 11 frame rate". His rule: never assume what the GPU or CPU can do - adapt
 * from what is MEASURED. This is that measure and the one budget derived from it:
 *
 *   the frame time       the real time between frames (LLFrameTimer::getFrameDeltaTimeF32), smoothed over SMOOTH_TAU_S
 *   the budget g         0.25 .. 1: eases DOWN (time constant DOWN_TAU_S) while the smoothed frame is slower than SLOW_FRAME_S
 *                        (25 fps), back UP (UP_TAU_S, slower) while it is faster than FAST_FRAME_S (30 fps), and holds in
 *                        between - the hysteresis that keeps it from hunting
 *
 * Read by the optional work that can be thinned without changing what the scene means: the weather particles (wolfweather.cpp,
 * the rate x g, each drop larger so the rain covers the same share of the screen) and the hurricane's second cloud lookup
 * (wolfenvironmentrender.cpp, off below g 0.9). Every change is logged (change-only, at most every LOG_MIN_S), with what the
 * frame and the particles cost, so the log shows whether the budget helped.
 */
class WolfFrameGovernor
{
public:
    /** Once a frame (WolfWeather::idle): reads the frame's real time and moves the budget. */
    static void update();
    /** The budget, 0.25 .. 1 (1 = everything at full). */
    static F32 budget();
    /** The smoothed frame time, seconds. */
    static F32 frameSeconds();
    /** CPU seconds spent this frame in the particle simulation (LLWorld::updateParticles) and in building the particles'
     *  geometry (LLParticlePartition::getGeometry): added up for the log line, so it says what the particles cost. */
    static void addParticleSimSeconds(F64 s);
    static void addParticleGeometrySeconds(F64 s);

    static constexpr F32 MIN_BUDGET = 0.25f;
    static constexpr F32 SLOW_FRAME_S = 1.f / 25.f;
    static constexpr F32 FAST_FRAME_S = 1.f / 30.f;
    static constexpr F32 SMOOTH_TAU_S = 1.5f;
    static constexpr F32 DOWN_TAU_S = 3.f;    // 1 -> 0.25 in about 4 s of slow frames
    static constexpr F32 UP_TAU_S = 12.f;     // 0.25 -> 1 in about 17 s of fast ones
    static constexpr F64 LOG_MIN_S = 5.0;
};

#endif // WOLF_FRAME_GOVERNOR_H
