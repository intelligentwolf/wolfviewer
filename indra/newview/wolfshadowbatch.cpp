/**
 * @file wolfshadowbatch.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfshadowbatch.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llgl.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "pipeline.h"

// [W107 SHADOW BATCH 2026-11-14] Why this is exact.
//
// The casters drawn are exactly stock's: every draw info of the render map, in render map order; nothing is reordered and
// nothing that stock draws is left out. Only the number of calls that carry them changes.
//
// registerFace (llvovolume.cpp) starts a new LLDrawInfo whenever the texture or material changes (its mTexture, mMaterialID,
// mBump, mShiny ... tests), even when the next face's indices follow straight on in the same vertex buffer. The shadow pass
// binds no texture or material for these: LLPipeline::renderShadow binds gDeferredShadowProgram with texture unit 0 disabled,
// and LLRenderPass::pushUntexturedBatch only sets the model matrix and the buffer before glDrawRangeElements;
// pushUntexturedGLTFBatch also sets GL_CULL_FACE from the material's double-sided flag. So a run is extended only while the
// next draw info
//  - uses the same LLVertexBuffer (the same GL vertex and index buffers, index type and stride: setBuffer / drawRange),
//  - uses the same model matrix pointer (applyModelMatrix compares the pointer; the matrix it would load is the same),
//  - (GLTF) has the same double-sided flag, so GL_CULL_FACE is the same for every triangle of the run,
//  - starts at the index where the run ends (mOffset == run offset + run count),
//  - and the run and the next draw info are whole triangles (count % 3 == 0), so one glDrawRangeElements(GL_TRIANGLES) over
//    the joined index range assembles exactly the triangles the separate calls did, in the same order (independent
//    triangles carry nothing from one primitive to the next). The joined start / end are the min / max of the parts'
//    vertex ranges, which hold every index drawn (glDrawRangeElements' range is a promise, not a clip).
// Same triangles, same order, same GL state for each, through the same program and matrices: the depth (and the colour,
// where a shadow map writes one) comes out bit for bit as stock's.
// A draw info with no indices is passed over and does not break a run: pushUntexturedBatch returns at once when !mCount,
// and the zero-count draw pushUntexturedGLTFBatch sends draws nothing.
namespace
{
    struct Run
    {
        LLDrawInfo* mFirst = nullptr;
        U32 mStart = 0;
        U32 mEnd = 0;
        U32 mCount = 0;
        bool mDoubleSided = false;

        bool canAppend(const LLDrawInfo& params, bool double_sided) const
        {
            return mFirst
                && params.mVertexBuffer.get() == mFirst->mVertexBuffer.get()
                && params.mModelMatrix == mFirst->mModelMatrix
                && double_sided == mDoubleSided
                && params.mOffset == mFirst->mOffset + mCount
                && mCount % 3 == 0
                && params.mCount % 3 == 0
#if LL_DARWIN
                // Source: llvovolume.cpp:5756-5759 registerFace() - on macOS a draw info is kept within the GL's vertex and
                // index range limits (end - start + new vertices <= mGLMaxVertexRange, count + new indices <=
                // mGLMaxIndexRange); a joined run keeps to the same limits, so no call is bigger than one stock could make
                && llmax(mEnd, (U32)params.mEnd) - llmin(mStart, (U32)params.mStart) <= (U32)gGLManager.mGLMaxVertexRange
                && mCount + params.mCount <= (U32)gGLManager.mGLMaxIndexRange
#endif
                ;
        }

        void start(LLDrawInfo& params, bool double_sided)
        {
            mFirst = &params;
            mStart = params.mStart;
            mEnd = params.mEnd;
            mCount = params.mCount;
            mDoubleSided = double_sided;
        }

        void append(const LLDrawInfo& params)
        {
            mStart = llmin(mStart, (U32)params.mStart);
            mEnd = llmax(mEnd, (U32)params.mEnd);
            mCount += params.mCount;
        }

        // Source: lldrawpool.cpp LLRenderPass::pushUntexturedBatch() / pushUntexturedGLTFBatch() - the same calls in the
        // same order (the GLTF cull-face state first), once for the whole run
        void flush(WolfShadowBatch::Stats& stats, const LLVertexBuffer*& last_buffer)
        {
            if (!mFirst)
            {
                return;
            }
            LLGLDisable cull_face(mDoubleSided ? GL_CULL_FACE : 0);   // 0: LLGLState does nothing (llgl.cpp LLGLState())
            LLRenderPass::applyModelMatrix(*mFirst);
            mFirst->mVertexBuffer->setBuffer();
            mFirst->mVertexBuffer->drawRange(LLRender::TRIANGLES, mStart, mEnd, mCount, mFirst->mOffset);
            ++stats.mDraws;
            if (mFirst->mVertexBuffer.get() != last_buffer)
            {
                ++stats.mBufferChanges;
                last_buffer = mFirst->mVertexBuffer.get();
            }
            mFirst = nullptr;
        }
    };

    // Source: lldrawpool.cpp LLRenderPass::pushUntexturedBatches() / pushUntexturedGLTFBatches() - the same render map walk
    void push(U32 type, bool gltf, WolfShadowBatch::Stats& stats)
    {
        Run run;
        const LLVertexBuffer* last_buffer = nullptr;
        auto* begin = gPipeline.beginRenderMap(type);
        auto* end = gPipeline.endRenderMap(type);
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo& params = **i;
            LLCullResult::increment_iterator(i, end);
            ++stats.mInfos;

            if (!params.mCount)
            {
                continue;
            }

            // Source: lldrawpool.cpp pushUntexturedGLTFBatch() - LLGLDisable cull_face(mat->mDoubleSided ? GL_CULL_FACE : 0)
            const bool double_sided = gltf && params.mGLTFMaterial->mDoubleSided;
            if (run.canAppend(params, double_sided))
            {
                run.append(params);
            }
            else
            {
                run.flush(stats, last_buffer);
                run.start(params, double_sided);
            }
        }
        run.flush(stats, last_buffer);
    }
}

void WolfShadowBatch::pushUntextured(U32 type, Stats& stats)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    push(type, false, stats);
}

void WolfShadowBatch::pushUntexturedGLTF(U32 type, Stats& stats)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    push(type, true, stats);
}
