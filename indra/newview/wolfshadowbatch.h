/**
 * @file wolfshadowbatch.h
 * @brief WolfViewer: the shadow map pass sends neighbouring untextured draws of one vertex buffer as one draw call.
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

#ifndef WOLF_SHADOW_BATCH_H
#define WOLF_SHADOW_BATCH_H

#include "stdtypes.h"

/**
 * [W107 SHADOW BATCH 2026-11-14] Far draw distance (Paddington Station, 4096 m): LLPipeline::renderShadow spent ~26% of the
 * main thread in LLRenderPass::pushUntexturedBatches. Used only by renderShadow for its static (not rigged) casters, in place
 * of LLRenderPass::pushUntexturedBatches / pushUntexturedGLTFBatches: the same draw infos, in the same order, but a run of
 * neighbours that differ only in texture or material (which a depth-only shadow pass does not use) goes as one draw call.
 * The exactness argument is in wolfshadowbatch.cpp.
 */
namespace WolfShadowBatch
{
    struct Stats
    {
        U32 mInfos = 0;          // draw infos in the render maps pushed
        U32 mDraws = 0;          // draw calls sent
        U32 mBufferChanges = 0;  // draw calls whose vertex buffer differs from the previous draw call's in the same map
    };

    // Source: lldrawpool.cpp LLRenderPass::pushUntexturedBatches() - the same draws, joined where exact
    void pushUntextured(U32 type, Stats& stats);
    // Source: lldrawpool.cpp LLRenderPass::pushUntexturedGLTFBatches() - the same draws, joined where exact
    void pushUntexturedGLTF(U32 type, Stats& stats);
}

#endif // WOLF_SHADOW_BATCH_H
