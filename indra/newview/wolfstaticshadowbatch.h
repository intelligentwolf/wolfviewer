/**
 * @file wolfstaticshadowbatch.h
 * @brief WolfViewer: the shadow map passes draw the static opaque casters of many spatial groups from shared buffers,
 * one multi-draw call per buffer page instead of one draw call (and one buffer bind) per draw info.
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

#ifndef WOLF_STATIC_SHADOW_BATCH_H
#define WOLF_STATIC_SHADOW_BATCH_H

#include "stdtypes.h"

#include <string>
#include <vector>

class LLSpatialGroup;

/**
 * [W107 STATIC BATCH 2026-11-14] Far draw distance (Paddington Station, 4096 m): LLPipeline::renderShadow spent ~24% of
 * the main thread sending the static shadow casters, about 14,000 draw calls a frame, nearly every one with a vertex
 * buffer of its own (LLVertexBuffer::setBuffer alone was ~17%). Design, measurements and the exactness argument:
 * region-terrain-work/w107-static-batching-2026-11-14/DESIGN.md (firestorm repository) and wolfstaticshadowbatch.cpp.
 *
 * For each spatial group of a region's static volume partition, the positions and indices of the draw infos the shadow
 * pass draws without a texture (the 13 legacy pass types of LLPipeline::renderShadow and PASS_GLTF_PBR) are copied into
 * a page of shared GL buffers. A shadow pass then sends every such group the caster cull let in with one
 * glMultiDrawElementsIndirect per page; each group's model matrix reaches the shader as a per-instance attribute.
 * The copy is used only while the group's mWolfGeomVersion is the one it was made from; any other group (bridges,
 * rigged, a copy not made yet or out of date) goes down the stock path in the same pass.
 *
 * Setting WolfStaticShadowBatch (default on); off, or where the GL cannot do it (macOS: OpenGL 4.1), every caster takes
 * the stock path.
 */
namespace WolfStaticShadowBatch
{
    // OpenGL 4.3 (glMultiDrawElementsIndirect, base instance) and not macOS. Checked before the shader is built.
    bool supported();

    // LLPipeline::renderShadow, before its updateCull / stateSort: decides whether this pass uses the shared buffers.
    void beginPass();
    // LLPipeline::renderShadow, at its end.
    void endPass();

    // LLPipeline::postSort, once per visible group while it builds the render maps. True: this group's draw infos of the
    // batched pass types are drawn by drawPass() in this pass, and the caller must not push them into the render maps.
    // Always false outside a pass that uses the shared buffers.
    bool takeGroup(LLSpatialGroup* group);
    // The pass types takeGroup() answers for (LLRenderPass::PASS_*).
    bool isBatchedType(U32 type);

    struct PassCounts
    {
        U32 mGroups = 0;          // groups drawn from the shared buffers
        U32 mCommands = 0;        // their draw commands (each was at least one stock draw call)
        U32 mCalls = 0;           // glMultiDrawElementsIndirect calls that carried them
        U32 mStockGroups = 0;     // groups taken whose copy went out of date before the draw: sent the stock way
    };
    // LLPipeline::renderShadow, static casters, with gDeferredShadowProgram bound (it is bound again on return):
    // draws the groups takeGroup() took.
    void drawPass(PassCounts& counts);

    // LLSpatialGroup::~LLSpatialGroup: gives the group's part of the shared buffers back.
    void releaseGroup(LLSpatialGroup* group);
    // LLSpatialGroup::clearDrawMap / LLVolumeGeometryManager::rebuildMesh, for a group with mWolfStaticBatch set: the
    // group's geometry changed (mWolfGeomVersion), so its copy is out of date; its part of the shared buffers is freed
    // now rather than when a shadow pass next wants the group.
    void dropCopy(LLSpatialGroup* group);
    // Frees every shared buffer (shadows switched off, the setting switched off, LLPipeline::cleanup). The groups'
    // copies are made again when a pass next wants them.
    void releaseAll();

    // WolfLogFrameRate: memory in use and the work done since the last call, for the "sun shadow cascades" log line.
    std::string logLine();

    // [SELF TEST] Setting WolfStaticShadowBatchSelfTest > 0: LLPipeline::generateSunShadow renders each sun cascade a
    // second time down the stock path and compares the two depth maps texel by texel (see generateSunShadow).
    bool selfTestWanted();
    void selfTestForceStock(bool force);
    // Reads the depth of the bound framebuffer (width x height) into out. False if it could not be read.
    bool selfTestReadDepth(U32 width, U32 height, std::vector<F32>& out);
    void selfTestReport(S32 cascade, const std::vector<F32>& batched, const std::vector<F32>& stock);
    // After the last cascade of a frame that was tested: one test fewer to go.
    void selfTestFrameDone();
}

#endif // WOLF_STATIC_SHADOW_BATCH_H
