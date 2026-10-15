/**
 * @file wolfaurora.h
 * @brief WolfViewer: the northern lights as the real thing - the CPU half of wolfAuroraF.glsl.
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

#ifndef WOLF_AURORA_H
#define WOLF_AURORA_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;

/**
 * [WOLF AURORA 2026-10-12] Paul: "can you make the northern lights look more like RL". Design and
 * sources: region-terrain-work/wolf-aurora/DESIGN.md (firestorm repo). Source: WolfStorm
 * js/rendering/wolf_aurora.js state() - the same computation, constant for constant; keep them in step.
 *
 * Once a frame (frameUpdate, from LLDrawPoolWLSky::render) it works out the aurora from the active
 * weather profile (WolfWeather: the owner's amount and colour, or the live space weather at the
 * region's real place), the sun and moon, and the graphics level, and ray-marches a quarter of the
 * aurora's direction table (gWolfAuroraLUTProgram). A sky shader that draws the aurora attaches
 * deferred/wolfAuroraF.glsl and calls bindUniforms() before it draws.
 */
class WolfAurora
{
public:
    static void frameUpdate();
    /** wolf_aurora this frame: 0..1, the night and the fog; 0 = no aurora (nothing to draw). */
    static F32  gate();
    /** For a sky shader that attached wolfAuroraF.glsl: wolf_aurora, wolf_aurora_eye, wolf_aurora_arc_w
     *  and the table (LLShaderMgr::WOLF_AURORA_LUT). 0 in a reflection-probe capture. */
    static void bindUniforms(LLGLSLShader* shader);
    static void releaseGL();
    /** [WolfViewer 2026-10-15] WolfForceAurora (a test switch): the aurora whatever the time or weather. */
    static bool forced();
    /** [WolfViewer 2026-10-15] Once a frame from LLAppViewer::idle: while WolfForceAurora is on, this viewer's own sky is
     *  World > Environment > Midnight (so the aurora is seen at night, WE on or off); off again, the shared sky. */
    static void idle();
};

#endif
