/**
 * @file wolfframeprofile.cpp
 * @brief WolfViewer: where the frame goes on a big view - CPU and GPU time and draw calls per render phase, logged.
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

#include "wolfframeprofile.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llgl.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "wolfdistantcull.h"
#include "wolfdrawcounter.h"
#include "wolfframegovernor.h"

#include <sstream>

namespace
{
    constexpr S32 PHASE_COUNT = WolfFrameProfile::NUM_PHASES;
    constexpr U32 RING = 4;   // frames between issuing a GPU timestamp and reading it back

    const char* const PHASE_NAMES[] = { "mirrors", "update", "cull", "shadows", "images", "sort", "geometry",
                                        "lighting+water+alpha", "finalize+ui", "swap", "probes", "objects", "terrain-visibility" };
    static_assert(sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]) == PHASE_COUNT, "one name per WolfFrameProfile::Phase");

    struct PhaseStats
    {
        F64 cpuStart = 0.0;
        U32 drawStart = 0;
        bool open = false;
        // over the current log period
        F64 cpuSum = 0.0;
        F64 gpuSum = 0.0;
        U32 gpuFrames = 0;
        U64 draws = 0;
    };

    struct State
    {
        PhaseStats phase[PHASE_COUNT];
        GLuint q[RING][PHASE_COUNT][2] = {};
        bool   issued[RING][PHASE_COUNT][2] = {};
        U32    slot = 0;            // the ring slot of the frame being drawn
        U32    frames = 0;          // frames in the current log period
        F64    periodStart = -1.0;
        U64    visGroups = 0, visDrawableGroups = 0, visAlpha = 0, visDrawables = 0;
        U32    visSamples = 0;
        U64    skippedGroups = 0, skippedSubtrees = 0;
    };
    State sState;

    // Source: wolfenvironmentrender.cpp timersAvailable - glQueryCounter is OpenGL 3.3 core; on Windows llgl.cpp loads it
    // as a pointer (llgl.cpp:613, 1928), so fail closed when the driver did not give one.
    bool timersAvailable()
    {
#if LL_WINDOWS
        if (glQueryCounter == nullptr || glGetQueryObjectui64v == nullptr) return false;
#endif
        return gGLManager.mGLVersion >= 3.29f;
    }

    void stamp(S32 p, S32 which)
    {
        // CPU-only idle work: a GPU interval around it would only measure unrelated GL work queued before it
        if (p == WolfFrameProfile::OBJECTS || p == WolfFrameProfile::TERRAIN_VIS) return;
        if (!timersAvailable()) return;
        GLuint& id = sState.q[sState.slot][p][which];
        if (!id) glGenQueries(1, &id);
        glQueryCounter(id, GL_TIMESTAMP);
        sState.issued[sState.slot][p][which] = true;
    }

    /** The oldest slot in the ring: its queries were issued RING - 1 frames ago. */
    void collect(U32 slot)
    {
        if (!timersAvailable()) return;
        for (S32 p = 0; p < PHASE_COUNT; ++p)
        {
            bool* issued = sState.issued[slot][p];
            if (!issued[0] || !issued[1])
            {
                issued[0] = issued[1] = false;
                continue;
            }
            GLuint64 ready = 0;
            glGetQueryObjectui64v(sState.q[slot][p][1], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready)
            {
                GLuint64 a = 0, b = 0;
                glGetQueryObjectui64v(sState.q[slot][p][0], GL_QUERY_RESULT, &a);
                glGetQueryObjectui64v(sState.q[slot][p][1], GL_QUERY_RESULT, &b);
                if (b >= a)
                {
                    sState.phase[p].gpuSum += (F64)(b - a) * 1.0e-6;   // ns -> ms
                    sState.phase[p].gpuFrames++;
                }
            }
            issued[0] = issued[1] = false;   // not ready four frames on: dropped rather than waited for
        }
    }

    bool bigView()
    {
        if (LLViewerCamera::instanceExists() && LLViewerCamera::getInstance()->getFar() > WolfFrameProfile::BIG_VIEW_M)
        {
            return true;
        }
        const LLViewerRegion* region = gAgent.getRegion();
        return region && region->getWidth() > WolfFrameProfile::BIG_VIEW_M;
    }

    void resetPeriod(F64 now)
    {
        for (PhaseStats& ph : sState.phase)
        {
            ph.cpuSum = ph.gpuSum = 0.0;
            ph.gpuFrames = 0;
            ph.draws = 0;
        }
        sState.frames = 0;
        sState.periodStart = now;
        sState.visGroups = sState.visDrawableGroups = sState.visAlpha = sState.visDrawables = 0;
        sState.visSamples = 0;
        sState.skippedGroups = sState.skippedSubtrees = 0;
    }
}

// static
void WolfFrameProfile::begin(Phase p)
{
    PhaseStats& ph = sState.phase[p];
    ph.cpuStart = LLTimer::getTotalSeconds();
    ph.drawStart = WolfDrawCounter::sCalls;
    ph.open = true;
    stamp(p, 0);
}

// static
void WolfFrameProfile::end(Phase p)
{
    PhaseStats& ph = sState.phase[p];
    if (!ph.open) return;
    ph.open = false;
    ph.cpuSum += LLTimer::getTotalSeconds() - ph.cpuStart;
    ph.draws += (U32)(WolfDrawCounter::sCalls - ph.drawStart);   // unsigned difference: right across the wrap
    stamp(p, 1);
}

// static
void WolfFrameProfile::noteVisible(U32 groups, U32 drawable_groups, U32 alpha_groups, U32 drawables)
{
    sState.visGroups += groups;
    sState.visDrawableGroups += drawable_groups;
    sState.visAlpha += alpha_groups;
    sState.visDrawables += drawables;
    sState.visSamples++;
}

// static
void WolfFrameProfile::endFrame()
{
    // GPU: the slot after this one is the oldest in the ring; read it back before this frame's slot moves onto it.
    const U32 next = (sState.slot + 1) % RING;
    collect(next);
    sState.slot = next;

    U32 groups = 0, subtrees = 0;
    WolfDistantCull::takeCounts(groups, subtrees);
    sState.skippedGroups += groups;
    sState.skippedSubtrees += subtrees;

    const F64 now = LLTimer::getTotalSeconds();
    if (sState.periodStart < 0.0)
    {
        resetPeriod(now);
        return;
    }
    sState.frames++;
    const F64 elapsed = now - sState.periodStart;
    if (elapsed < LOG_SECONDS)
    {
        return;
    }
    if (!bigView() || sState.frames == 0)
    {
        resetPeriod(now);
        return;
    }

    const F64 frames = (F64)sState.frames;
    const F64 frame_ms = elapsed * 1000.0 / frames;
    F64 phases_ms = 0.0;
    std::ostringstream out;
    out << "frame profile: " << llformat("%.1f", frames / elapsed) << " fps (" << llformat("%.1f", frame_ms)
        << " ms) over " << sState.frames << " frames; per frame cpu/gpu ms and draw calls:";
    for (S32 p = 0; p < PHASE_COUNT; ++p)
    {
        const PhaseStats& ph = sState.phase[p];
        const F64 cpu = ph.cpuSum * 1000.0 / frames;
        phases_ms += cpu;
        out << " " << PHASE_NAMES[p] << " " << llformat("%.2f", cpu) << "/";
        if (ph.gpuFrames > 0)
        {
            out << llformat("%.2f", ph.gpuSum / (F64)ph.gpuFrames);
        }
        else
        {
            out << "-";
        }
        out << " " << (U64)((F64)ph.draws / frames + 0.5) << (p + 1 < PHASE_COUNT ? ";" : "");
    }
    out << " | rest of the frame (idle, network, scripts, UI logic) " << llformat("%.2f", llmax(0.0, frame_ms - phases_ms)) << " ms";
    if (sState.visSamples > 0)
    {
        const F64 n = (F64)sState.visSamples;
        out << " | world camera: " << (U64)((F64)sState.visGroups / n + 0.5) << " visible groups, "
            << (U64)((F64)sState.visDrawableGroups / n + 0.5) << " drawable groups, "
            << (U64)((F64)sState.visAlpha / n + 0.5) << " alpha groups, "
            << (U64)((F64)sState.visDrawables / n + 0.5) << " drawables";
    }
    out << " | far small prims: under " << llformat("%.1f", WolfDistantCull::thresholdPixels()) << " px beyond "
        << (S32)WolfDistantCull::FULL_DETAIL_M << " m not drawn (load " << llformat("%.2f", WolfDistantCull::load())
        << ", budget " << llformat("%.2f", WolfFrameGovernor::budget()) << "), "
        << (U64)((F64)sState.skippedGroups / frames + 0.5) << " groups and "
        << (U64)((F64)sState.skippedSubtrees / frames + 0.5) << " subtrees skipped a frame";
    if (LLViewerCamera::instanceExists())
    {
        out << " | draw distance " << (S32)LLViewerCamera::getInstance()->getFar() << " m";
    }
    LL_INFOS("WolfFrame") << out.str() << LL_ENDL;
    resetPeriod(now);
}

// static
void WolfFrameProfile::releaseGL()
{
    for (U32 s = 0; s < RING; ++s)
    {
        for (S32 p = 0; p < PHASE_COUNT; ++p)
        {
            for (S32 w = 0; w < 2; ++w)
            {
                if (sState.q[s][p][w])
                {
                    glDeleteQueries(1, &sState.q[s][p][w]);
                    sState.q[s][p][w] = 0;
                }
                sState.issued[s][p][w] = false;
            }
        }
    }
}
