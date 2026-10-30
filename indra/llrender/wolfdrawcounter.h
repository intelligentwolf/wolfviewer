/**
 * @file wolfdrawcounter.h
 * @brief WolfViewer: how many draw calls the viewer has issued, for the frame profile (newview/wolfframeprofile.h).
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

#ifndef WOLF_DRAW_COUNTER_H
#define WOLF_DRAW_COUNTER_H

#include "stdtypes.h"

/**
 * [bigregion-perf 2026-10-30] Every glDrawRangeElements / glDrawArrays that goes through an LLVertexBuffer
 * (llvertexbuffer.cpp drawRange, drawRangeFast, drawArrays) adds one. Main thread only, like the calls it counts.
 * It only ever goes up (wrapping after 2^32 calls); the frame profile takes the difference across each phase.
 * Kept out of llvertexbuffer.h so that adding it does not rebuild everything that includes that header.
 */
namespace WolfDrawCounter
{
    extern U32 sCalls;
}

#endif // WOLF_DRAW_COUNTER_H
