/**
 * @file wolfframeprofile.h
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

#ifndef WOLF_FRAME_PROFILE_H
#define WOLF_FRAME_PROFILE_H

#include "stdtypes.h"

/**
 * [bigregion-perf 2026-10-30] The stock render timers are Tracy zones now (LL_PROFILE_ZONE: compiled out of a release
 * build), so a resident's log says how fast the frame is but never where it went. This measures the main phases of
 * display() (llviewerdisplay.cpp), the reflection-probe update after it and two idle steps before it (llappviewer.cpp):
 *   CPU      LLTimer, the main thread's time inside the phase
 *   GPU      GL_TIMESTAMP queries at the start and end of the phase (glQueryCounter, OpenGL 3.3), read back four frames
 *            later so nothing waits; when the CPU is the limit the GPU number is mostly the gaps between commands
 *   draws    LLVertexBuffer draw calls issued in the phase (llrender/wolfdrawcounter.h)
 * and logs the averages every LOG_SECONDS while the view reaches past 1,024 m (a far draw distance or a region wider
 * than 1,024 m), with the visible octree groups and what WolfDistantCull skipped. Nothing else is changed by it.
 */
class WolfFrameProfile
{
public:
    enum Phase
    {
        MIRRORS = 0,      // hero probes (mirrors), start of display()
        GEOM_UPDATE,      // createObjects / processPartitionQ / updateGeom
        CULL,             // the world camera's octree cull (LLPipeline::updateCull)
        SHADOWS,          // generateSunShadow: four sun cascades (each its own cull, sort and draw) and spot shadows
        IMAGES,           // texture list / bump / GLTF material updates
        SORT,             // the world camera's stateSort (+ rebuildPools)
        GEOMETRY,         // renderGeomDeferred: the opaque scene into the G-buffer
        LIGHTING_POST,    // renderDeferredLighting: lights, sky, then water and alpha (renderGeomPostDeferred; the Wolf
                          // sky's composite included)
        FINALIZE_UI,      // render_ui: renderFinalize (tone map, glow, anti-aliasing) then the HUDs and 2D UI
        SWAP,             // swap (waits for the GPU when it is the limit)
        PROBES,           // LLReflectionMapManager::update, after display()
        OBJECTS,          // idle: gObjectList.update (and the GLTF scene)
        TERRAIN_VIS,      // idle: LLWorld::updateVisibilities - the terrain patches' visibility and LOD (llsurface.cpp)
        NUM_PHASES
    };

    static void begin(Phase p);
    static void end(Phase p);
    /** Once a frame, at the end of display(): reads back old GPU timings, counts the frame, logs when due. */
    static void endFrame();
    /** The GL context is going (LLPipeline::destroyGL): its query objects go with it. */
    static void releaseGL();
    /** The world camera's cull result after stateSort (llviewerdisplay.cpp). */
    static void noteVisible(U32 groups, U32 drawable_groups, U32 alpha_groups, U32 drawables);

    struct Scope
    {
        explicit Scope(Phase p) : mPhase(p) { begin(p); }
        ~Scope() { end(mPhase); }
        Phase mPhase;
    };

    static constexpr F64 LOG_SECONDS = 10.0;
    static constexpr F32 BIG_VIEW_M = 1024.f;
};

#endif // WOLF_FRAME_PROFILE_H
