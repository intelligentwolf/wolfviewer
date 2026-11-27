/**
 * @file wolfstaticshadowbatch.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfstaticshadowbatch.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "wolfvkbridge.h"   // [W114-VULKAN S5 2026-11-26] shadowPassOnVulkan
#include "wolfvknative.h"
#include <functional>
#include <map>
#include <memory>

// [W107 STATIC BATCH 2026-11-14] Why the shadow maps come out the same.
//
// WHAT IS DRAWN. For a group that takeGroup() answers true for, LLPipeline::postSort leaves the group's draw infos of the
// batched pass types out of the render maps, and drawPass() draws the triangles of exactly those draw infos:
//  - the 13 legacy types (the `types` table of LLPipeline::renderShadow), which stock sends through
//    LLRenderPass::pushUntexturedBatch with GL_CULL_FACE on: here every index range of every draw info, with ranges of
//    one vertex buffer that touch or overlap made one range (a face registered for two of the types, e.g. PASS_SIMPLE
//    and PASS_BUMP, is drawn twice by stock; the second draw writes the depths the first already wrote);
//  - PASS_GLTF_PBR, which stock sends through pushUntexturedGLTFBatch with GL_CULL_FACE off when the material is
//    double sided: here one command per draw info, and the material's flag is read at draw time as stock reads it.
// The group must be in a region's static volume partition and every one of those draw infos must use the group's own
// model matrix (registerFace's static branch, &group->mWolfRenderMatrix); anything else leaves the whole group to stock.
// Which groups are drawn is unchanged: it is the caster cull's visible group list, group for group.
//
// WITH WHAT DATA. The vertex positions are the bytes of the LLVertexBuffer's own copy (mMappedData, where
// LLFace::getGeometryVolume writes through the striders; flush sends the same bytes to GL), the indices the bytes of
// mMappedIndexData, drawn with a base vertex so they need no change. The copy is used only while group->mWolfGeomVersion
// is unchanged: LLSpatialGroup::clearDrawMap (every rebuild, destroyGLState, the destructor) and
// LLVolumeGeometryManager::rebuildMesh (vertices rewritten in place) change it, and drawPass() checks it again after
// postSort has run its delayed rebuildMesh calls.
//
// WITH WHAT MATRIX. deferred/wolfShadowBatchV.glsl is deferred/shadowV.glsl with the matrix as an instance attribute;
// groupMatrix() forms it as LLRenderPass::applyModelMatrix and LLRender::syncMatrices form the uniform.
//
// IN WHAT ORDER. Not stock's - and it cannot matter: the pass tests GL_LESS and writes depth (LLGLDepthTest in
// renderShadow), so a texel ends at the smallest depth of the fragments that cover it whatever the order, and the colour
// the pass writes when a variance map is on is the constant of shadowF.glsl.
//
// The self test (WolfStaticShadowBatchSelfTest, generateSunShadow) renders a cascade both ways and compares the depth
// maps texel by texel; DESIGN.md has the result on the test machine.

#if !LL_DARWIN

namespace
{
    // Source: OpenGL 4.3 core specification, section 10.5 (glDrawElementsIndirect): the layout
    // glMultiDrawElementsIndirect reads from the GL_DRAW_INDIRECT_BUFFER
    struct Command
    {
        GLuint mCount = 0;
        GLuint mInstanceCount = 1;
        GLuint mFirstIndex = 0;
        GLint mBaseVertex = 0;
        GLuint mBaseInstance = 0;
    };
    static_assert(sizeof(Command) == 20, "DrawElementsIndirectCommand is five 32-bit words");

    struct GLTFCommand
    {
        Command mCommand;
        // Source: lldrawpool.cpp LLRenderPass::pushUntexturedGLTFBatch() - mat->mDoubleSided is read when the draw is
        // sent; the draw info holds the same pointer for as long as it lives, this keeps it for as long as the copy does
        LLPointer<LLFetchedGLTFMaterial> mMaterial;
    };

    // Source: pipeline.cpp LLPipeline::renderShadow() `types` - the pass types the shadow pass draws with
    // renderObjects(type, false, false, rigged), i.e. LLRenderPass::pushUntexturedBatches
    const U32 LEGACY_TYPES[] = {
        LLRenderPass::PASS_SIMPLE,
        LLRenderPass::PASS_FULLBRIGHT,
        LLRenderPass::PASS_SHINY,
        LLRenderPass::PASS_BUMP,
        LLRenderPass::PASS_FULLBRIGHT_SHINY,
        LLRenderPass::PASS_MATERIAL,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP,
        LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,
        LLRenderPass::PASS_NORMMAP_EMISSIVE,
        LLRenderPass::PASS_NORMSPEC,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE
    };
    // Source: pipeline.cpp LLPipeline::renderShadow() - renderGLTFObjects(LLRenderPass::PASS_GLTF_PBR, false, rigged)
    const U32 GLTF_TYPE = LLRenderPass::PASS_GLTF_PBR;

    // Source: llvertexbuffer.cpp LLVertexBuffer::sTypeSize[TYPE_VERTEX] = sizeof(LLVector4): a position is 16 bytes in
    // the LLVertexBuffer (x, y, z and the texture index); the shared buffer keeps the three floats shadowV.glsl reads
    const U32 SOURCE_POSITION_STRIDE = 16;
    const U32 POSITION_BYTES = 3 * sizeof(F32);
    // Source: llvertexbuffer.cpp LLVertexBuffer::updateNumIndices() (sizeof(U16) * nindices) and llface.cpp
    // LLFace::getGeometryVolume() (getIndexStrider(LLStrider<U16>&)): a volume group's index buffer is 16-bit. The
    // 32-bit LLVertexBuffer::setIndexData is called only by gltf/primitive.cpp and llterrainpaintmap.cpp, whose buffers
    // never reach a spatial group's draw map.
    const U32 INDEX_BYTES = sizeof(U16);

    // A page of the shared buffers. Sizes are a choice, not a limit: a group that needs more gets a page of its own.
    // 2^19 positions (6 MB) and 2^20 indices (2 MB): the test scene (Paddington Station at 4096 m, 14 Nov 2026) used
    // 1.9 indices a position (24.4 million to 13.0 million, logLine()), so the two fill at about the same rate.
    const U32 PAGE_VERTICES = 1 << 19;
    const U32 PAGE_INDICES = 1 << 20;

    // Copies are made when a shadow pass first wants a group, at most this many bytes of buffer data a frame (a group
    // waiting its turn takes the stock path meanwhile): arriving in a busy region must not add a long frame.
    const U32 BUILD_BYTES_PER_FRAME = 4 * 1024 * 1024;

    // First fit over a free list keyed by offset; neighbours are joined when a range comes back.
    class RangeAllocator
    {
    public:
        void init(U32 capacity)
        {
            mFree.clear();
            mFree[0] = capacity;
            mCapacity = capacity;
            mUsed = 0;
        }

        bool allocate(U32 count, U32& offset)
        {
            for (auto it = mFree.begin(); it != mFree.end(); ++it)
            {
                if (it->second >= count)
                {
                    offset = it->first;
                    const U32 rest = it->second - count;
                    mFree.erase(it);
                    if (rest)
                    {
                        mFree[offset + count] = rest;
                    }
                    mUsed += count;
                    return true;
                }
            }
            return false;
        }

        void release(U32 offset, U32 count)
        {
            mUsed -= llmin(mUsed, count);
            auto next = mFree.lower_bound(offset);
            if (next != mFree.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first;
                    count += prev->second;
                    mFree.erase(prev);
                }
            }
            if (next != mFree.end() && offset + count == next->first)
            {
                count += next->second;
                mFree.erase(next);
            }
            mFree[offset] = count;
        }

        U32 used() const { return mUsed; }
        U32 capacity() const { return mCapacity; }

    private:
        std::map<U32, U32> mFree;   // offset -> length
        U32 mCapacity = 0;
        U32 mUsed = 0;
    };

    struct Page
    {
        GLuint mVertexBuffer = 0;
        GLuint mIndexBuffer = 0;
        GLuint mVao = 0;
        GLuint mVaoProgram = 0;      // the program object whose attribute locations mVao was set up for
        S32 mVkPage = -1;
        RangeAllocator mVertices;
        RangeAllocator mIndices;
        U32 mGroups = 0;             // records with storage in this page
        // this pass's commands: GL_CULL_FACE on, then (double-sided GLTF) off
        std::vector<Command> mCullOn;
        std::vector<Command> mCullOff;
        bool mQueued = false;
    };

    // What LLSpatialGroup::mWolfStaticBatch points at.
    struct Record
    {
        U32 mEpoch = 0;             // sEpoch when the storage was taken (releaseAll() starts a new epoch)
        U32 mVersion = 0;           // group->mWolfGeomVersion the copy was made from
        bool mBatchable = false;    // false: this version of the group is left to the stock path
        S32 mPage = -1;             // -1: no storage (nothing to draw, or not batchable)
        U32 mVertexOffset = 0;
        U32 mVertexCount = 0;
        U32 mIndexOffset = 0;
        U32 mIndexCount = 0;
        std::vector<Command> mCommands;       // legacy types: GL_CULL_FACE on
        std::vector<GLTFCommand> mGLTF;       // PASS_GLTF_PBR: by the material's double-sided flag
    };

    // One index range of one vertex buffer, as a draw info gives it.
    struct Span
    {
        const LLVertexBuffer* mBuffer = nullptr;
        U32 mOffset = 0;    // first index
        U32 mCount = 0;     // indices
        U32 mStart = 0;     // lowest vertex the indices may name
        U32 mEnd = 0;       // highest
        LLFetchedGLTFMaterial* mMaterial = nullptr;
    };

    std::vector<std::unique_ptr<Page>> sPages;   // a freed page leaves an empty slot, reused by the next new page
    U32 sEpoch = 1;
    GLuint sMatrixBuffer = 0;
    GLuint sCommandBuffer = 0;
    GLuint sLocationProgram = 0;   // the program object sPositionLocation / sMatrixLocation were read from
    GLint sPositionLocation = -1;
    GLint sMatrixLocation = -1;

    bool sPassActive = false;
    bool sForceStock = false;
    std::vector<LLSpatialGroup*> sTaken;
    std::vector<glm::mat4> sMatrices;
    std::vector<Page*> sDrawPages;
    std::vector<Span> sSpans;
    std::vector<F32> sPositionScratch;
    std::vector<U16> sIndexScratch;

    U32 sBudgetFrame = 0;
    U32 sBudgetBytes = 0;

    // for logLine()
    U32 sRecords = 0;
    U64 sLogBuilds = 0;
    U64 sLogBuildBytes = 0;
    U64 sLogNotBatchable = 0;
    U64 sLogWaiting = 0;
    U64 sLogOverLimit = 0;

    bool isLegacyType(U32 type)
    {
        for (U32 t : LEGACY_TYPES)
        {
            if (t == type)
            {
                return true;
            }
        }
        return false;
    }

    void deletePage(S32 index)
    {
        Page* page = sPages[index].get();
        if (gGLManager.mIsNativeVulkan)
        {
            if (page->mVkPage >= 0)
            {
                WolfVkNative::destroyShadowBatchPage(page->mVkPage);
            }
            sPages[index].reset();
            return;
        }
        if (page->mVao)
        {
            glDeleteVertexArrays(1, &page->mVao);
        }
        glDeleteBuffers(1, &page->mVertexBuffer);
        glDeleteBuffers(1, &page->mIndexBuffer);
        sPages[index].reset();
    }

    // Gives a record's part of its page back. A page whose last group has gone is deleted, except that one empty page
    // of the standard size is kept: a lone group whose copy is remade must not delete and create a page each time.
    void freeStorage(Record& rec)
    {
        if (rec.mPage >= 0 && rec.mEpoch == sEpoch)
        {
            Page* page = sPages[rec.mPage].get();
            page->mVertices.release(rec.mVertexOffset, rec.mVertexCount);
            page->mIndices.release(rec.mIndexOffset, rec.mIndexCount);
            if (--page->mGroups == 0)
            {
                bool keep = page->mVertices.capacity() == PAGE_VERTICES && page->mIndices.capacity() == PAGE_INDICES;
                for (S32 i = 0; keep && i < (S32)sPages.size(); ++i)
                {
                    if (i != rec.mPage && sPages[i] && sPages[i]->mGroups == 0)
                    {
                        keep = false;   // there is an empty page already
                    }
                }
                if (!keep)
                {
                    deletePage(rec.mPage);
                }
            }
        }
        rec.mPage = -1;
        rec.mVertexCount = 0;
        rec.mIndexCount = 0;
        rec.mCommands.clear();
        rec.mGLTF.clear();
    }

    // A page with vertices and indices free for the counts given (a new one when no page has), and the ranges in it.
    S32 allocate(U32 vertices, U32 indices, U32& vertex_offset, U32& index_offset)
    {
        for (S32 i = 0; i < (S32)sPages.size(); ++i)
        {
            Page* page = sPages[i].get();
            if (page && page->mVertices.allocate(vertices, vertex_offset))
            {
                if (page->mIndices.allocate(indices, index_offset))
                {
                    return i;
                }
                page->mVertices.release(vertex_offset, vertices);
            }
        }

        std::unique_ptr<Page> page(new Page());
        const U32 vertex_capacity = llmax(vertices, PAGE_VERTICES);
        const U32 index_capacity = llmax(indices, PAGE_INDICES);

        // WolfStaticShadowBatchMaxMB: the video memory the pages may take in all (0: no limit). A group that would need
        // a page beyond it stays on the stock path until its geometry next changes.
        static LLCachedControl<S32> max_mb(gSavedSettings, "WolfStaticShadowBatchMaxMB", 384);
        if (max_mb > 0)
        {
            U64 bytes = (U64)vertex_capacity * POSITION_BYTES + (U64)index_capacity * INDEX_BYTES;
            for (const std::unique_ptr<Page>& other : sPages)
            {
                if (other)
                {
                    bytes += (U64)other->mVertices.capacity() * POSITION_BYTES + (U64)other->mIndices.capacity() * INDEX_BYTES;
                }
            }
            if (bytes > (U64)max_mb * 1024 * 1024)
            {
                ++sLogOverLimit;
                return -1;
            }
        }
        if (gGLManager.mIsNativeVulkan)
        {
            page->mVkPage = WolfVkNative::createShadowBatchPage(vertex_capacity * POSITION_BYTES, index_capacity * INDEX_BYTES);
            if (page->mVkPage < 0)
            {
                LL_WARNS_ONCE("WolfShadows") << "static shadow batch: a Vulkan buffer page could not be allocated; those shadow casters are drawn the stock way" << LL_ENDL;
                return -1;
            }
        }
        else
        {
            while (glGetError() != GL_NO_ERROR) { }   // only the two allocations below are judged
            glGenBuffers(1, &page->mVertexBuffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, page->mVertexBuffer);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)vertex_capacity * POSITION_BYTES, nullptr, GL_DYNAMIC_DRAW);
            glGenBuffers(1, &page->mIndexBuffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, page->mIndexBuffer);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)index_capacity * INDEX_BYTES, nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            if (glGetError() != GL_NO_ERROR)
            {   // no video memory for a page (GL_OUT_OF_MEMORY): nothing is kept, the group stays on the stock path
                glDeleteBuffers(1, &page->mVertexBuffer);
                glDeleteBuffers(1, &page->mIndexBuffer);
                LL_WARNS_ONCE("WolfShadows") << "static shadow batch: a buffer page could not be allocated; those shadow casters are drawn the stock way" << LL_ENDL;
                return -1;
            }
        }
        page->mVertices.init(vertex_capacity);
        page->mIndices.init(index_capacity);
        page->mVertices.allocate(vertices, vertex_offset);
        page->mIndices.allocate(indices, index_offset);

        for (S32 i = 0; i < (S32)sPages.size(); ++i)
        {
            if (!sPages[i])
            {
                sPages[i] = std::move(page);
                return i;
            }
        }
        sPages.push_back(std::move(page));
        return (S32)sPages.size() - 1;
    }

    // A draw info of a batched type as a Span. False: the group cannot be drawn from the shared buffers (it is then
    // left to the stock path whole).
    bool addSpan(const LLDrawInfo& info, const LLMatrix4* group_matrix, LLFetchedGLTFMaterial* material)
    {
        if (!info.mCount)
        {   // Source: lldrawpool.cpp pushUntexturedBatch() returns at once when !mCount; pushUntexturedGLTFBatch()
            // sends a draw of no indices
            return true;
        }
        const LLVertexBuffer* buffer = info.mVertexBuffer.get();
        if (!buffer
            || info.mModelMatrix != group_matrix
            || info.mCount % 3 != 0
            || !buffer->getMappedData()
            || !buffer->getMappedIndices()
            || !buffer->hasDataType(LLVertexBuffer::TYPE_VERTEX)
            // Source: llvertexbuffer.cpp LLVertexBuffer::setIndexData(const U32*) - a buffer switched to 32-bit indices
            // keeps its byte size and halves mNumIndices; a 16-bit one has exactly two bytes an index
            || buffer->getIndicesSize() != buffer->getNumIndices() * INDEX_BYTES
            || info.mStart > info.mEnd
            || info.mEnd >= buffer->getNumVerts()
            || info.mOffset > buffer->getNumIndices()
            || info.mCount > buffer->getNumIndices() - info.mOffset)
        {
            return false;
        }
        Span span;
        span.mBuffer = buffer;
        span.mOffset = info.mOffset;
        span.mCount = info.mCount;
        span.mStart = info.mStart;
        span.mEnd = info.mEnd;
        span.mMaterial = material;
        sSpans.push_back(span);
        return true;
    }

    // Makes the group's copy for its present mWolfGeomVersion. rec.mBatchable says whether there is one.
    void build(LLSpatialGroup* group, Record& rec)
    {
        freeStorage(rec);
        rec.mEpoch = sEpoch;
        rec.mVersion = group->mWolfGeomVersion;
        rec.mBatchable = false;
        ++sLogBuilds;

        const LLMatrix4* group_matrix = &group->mWolfRenderMatrix;

        // legacy types: all their index ranges, the ones that touch or overlap in one buffer joined
        sSpans.clear();
        for (U32 type : LEGACY_TYPES)
        {
            auto it = group->mDrawMap.find(type);
            if (it == group->mDrawMap.end())
            {
                continue;
            }
            for (const LLPointer<LLDrawInfo>& info : it->second)
            {
                if (info.isNull() || !addSpan(*info, group_matrix, nullptr))
                {
                    ++sLogNotBatchable;
                    return;
                }
            }
        }
        std::sort(sSpans.begin(), sSpans.end(), [](const Span& a, const Span& b)
        {
            return a.mBuffer != b.mBuffer ? std::less<const LLVertexBuffer*>()(a.mBuffer, b.mBuffer) : a.mOffset < b.mOffset;
        });
        size_t legacy_count = 0;
        for (size_t i = 0; i < sSpans.size(); ++i)
        {
            const Span span = sSpans[i];
            if (legacy_count > 0)
            {
                Span& last = sSpans[legacy_count - 1];
                if (last.mBuffer == span.mBuffer && span.mOffset <= last.mOffset + last.mCount)
                {
                    if ((span.mOffset - last.mOffset) % 3 != 0)
                    {   // two ranges that cut the same indices into different triangles: not ours to join
                        ++sLogNotBatchable;
                        return;
                    }
                    last.mCount = llmax(last.mOffset + last.mCount, span.mOffset + span.mCount) - last.mOffset;
                    last.mStart = llmin(last.mStart, span.mStart);
                    last.mEnd = llmax(last.mEnd, span.mEnd);
                    continue;
                }
            }
            sSpans[legacy_count++] = span;
        }
        sSpans.resize(legacy_count);

        // PASS_GLTF_PBR: one range per draw info, with its material
        {
            auto it = group->mDrawMap.find(GLTF_TYPE);
            if (it != group->mDrawMap.end())
            {
                for (const LLPointer<LLDrawInfo>& info : it->second)
                {
                    // (stock dereferences the material without a test; a draw info without one is left to stock)
                    if (info.isNull() || info->mGLTFMaterial.isNull() || !addSpan(*info, group_matrix, info->mGLTFMaterial.get()))
                    {
                        ++sLogNotBatchable;
                        return;
                    }
                }
            }
        }

        U32 vertices = 0;
        U32 indices = 0;
        for (const Span& span : sSpans)
        {
            vertices += span.mEnd - span.mStart + 1;
            indices += span.mCount;
        }
        if (sSpans.empty())
        {   // nothing of the batched types to draw: the group is ours and costs nothing
            rec.mBatchable = true;
            return;
        }

        sPositionScratch.resize((size_t)vertices * 3);
        sIndexScratch.resize(indices);
        U32 vertex_at = 0;
        U32 index_at = 0;
        for (const Span& span : sSpans)
        {
            const U32 span_vertices = span.mEnd - span.mStart + 1;
            const U8* src = span.mBuffer->getMappedData() + span.mBuffer->getOffset(LLVertexBuffer::TYPE_VERTEX)
                            + (size_t)span.mStart * SOURCE_POSITION_STRIDE;
            F32* dst = &sPositionScratch[(size_t)vertex_at * 3];
            for (U32 v = 0; v < span_vertices; ++v)
            {
                memcpy(dst, src, POSITION_BYTES);
                dst += 3;
                src += SOURCE_POSITION_STRIDE;
            }

            const U16* src_index = (const U16*)span.mBuffer->getMappedIndices() + span.mOffset;
            for (U32 k = 0; k < span.mCount; ++k)
            {
                if (src_index[k] < span.mStart || src_index[k] > span.mEnd)
                {   // an index outside the range the draw info promises (glDrawRangeElements): leave it to stock
                    rec.mCommands.clear();
                    rec.mGLTF.clear();
                    ++sLogNotBatchable;
                    return;
                }
            }
            memcpy(&sIndexScratch[index_at], src_index, (size_t)span.mCount * INDEX_BYTES);

            Command command;
            command.mCount = span.mCount;
            command.mFirstIndex = index_at;                                   // + the page's index offset, below
            command.mBaseVertex = (GLint)vertex_at - (GLint)span.mStart;      // + the page's vertex offset, below
            if (span.mMaterial)
            {
                GLTFCommand gltf;
                gltf.mCommand = command;
                gltf.mMaterial = span.mMaterial;
                rec.mGLTF.push_back(gltf);
            }
            else
            {
                rec.mCommands.push_back(command);
            }
            vertex_at += span_vertices;
            index_at += span.mCount;
        }

        rec.mPage = allocate(vertices, indices, rec.mVertexOffset, rec.mIndexOffset);
        if (rec.mPage < 0)
        {   // no page to be had (the memory limit, or the GL refused): stock path for this version of the group
            rec.mCommands.clear();
            rec.mGLTF.clear();
            return;
        }
        rec.mVertexCount = vertices;
        rec.mIndexCount = indices;
        Page* page = sPages[rec.mPage].get();
        ++page->mGroups;
        for (Command& command : rec.mCommands)
        {
            command.mFirstIndex += rec.mIndexOffset;
            command.mBaseVertex += (GLint)rec.mVertexOffset;
        }
        for (GLTFCommand& gltf : rec.mGLTF)
        {
            gltf.mCommand.mFirstIndex += rec.mIndexOffset;
            gltf.mCommand.mBaseVertex += (GLint)rec.mVertexOffset;
        }

        if (gGLManager.mIsNativeVulkan)
        {
            WolfVkNative::writeShadowBatchPage(
                page->mVkPage,
                rec.mVertexOffset * POSITION_BYTES, sPositionScratch.data(), vertices * POSITION_BYTES,
                rec.mIndexOffset * INDEX_BYTES, sIndexScratch.data(), indices * INDEX_BYTES);
        }
        else
        {
            // GL_COPY_WRITE_BUFFER: a binding point nothing else in the viewer uses, so no cached binding goes stale
            glBindBuffer(GL_COPY_WRITE_BUFFER, page->mVertexBuffer);
            glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)rec.mVertexOffset * POSITION_BYTES,
                            (GLsizeiptr)vertices * POSITION_BYTES, sPositionScratch.data());
            glBindBuffer(GL_COPY_WRITE_BUFFER, page->mIndexBuffer);
            glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)rec.mIndexOffset * INDEX_BYTES,
                            (GLsizeiptr)indices * INDEX_BYTES, sIndexScratch.data());
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        const U32 bytes = vertices * POSITION_BYTES + indices * INDEX_BYTES;
        sBudgetBytes += bytes;
        sLogBuildBytes += bytes;
        rec.mBatchable = true;
    }

    bool usable(const LLSpatialGroup* group, const Record* rec)
    {
        return rec && rec->mEpoch == sEpoch && rec->mVersion == group->mWolfGeomVersion;
    }

    // Source: lldrawpool.cpp LLRenderPass::applyModelMatrix() - gGL.loadMatrix(gGLModelView); gGL.multMatrix(model)
    // (llrender.cpp: glm::make_mat4, then `*=`), and llrender.cpp LLRender::syncMatrices() - cached_mvp = modelview;
    // cached_mvp = projection * cached_mvp. The same operations on the same values, in the same order.
    glm::mat4 groupMatrix(const glm::mat4& projection, const LLSpatialGroup* group)
    {
        glm::mat4 modelview = glm::make_mat4((GLfloat*)gGLModelView);
        modelview *= glm::make_mat4((GLfloat*)group->mWolfRenderMatrix.mMatrix);
        glm::mat4 mvp = modelview;
        mvp = projection * mvp;
        return mvp;
    }

    // A taken group whose copy went out of date between postSort and the draw (postSort's delayed rebuildMesh calls
    // come after it has built the render maps): its draw infos of the batched types, sent as stock sends them.
    // Source: lldrawpool.cpp LLRenderPass::pushUntexturedBatch() / pushUntexturedGLTFBatch()
    void drawStock(LLSpatialGroup* group)
    {
        for (U32 type : LEGACY_TYPES)
        {
            auto it = group->mDrawMap.find(type);
            if (it == group->mDrawMap.end())
            {
                continue;
            }
            for (const LLPointer<LLDrawInfo>& info : it->second)
            {
                LLDrawInfo* params = info.get();
                if (!params || !params->mCount)
                {
                    continue;
                }
                LLRenderPass::applyModelMatrix(*params);
                params->mVertexBuffer->setBuffer();
                params->mVertexBuffer->drawRange(LLRender::TRIANGLES, params->mStart, params->mEnd, params->mCount, params->mOffset);
            }
        }
        auto it = group->mDrawMap.find(GLTF_TYPE);
        if (it != group->mDrawMap.end())
        {
            for (const LLPointer<LLDrawInfo>& info : it->second)
            {
                LLDrawInfo* params = info.get();
                if (!params)
                {
                    continue;
                }
                LLGLDisable cull_face(params->mGLTFMaterial->mDoubleSided ? GL_CULL_FACE : 0);
                LLRenderPass::applyModelMatrix(*params);
                params->mVertexBuffer->setBuffer();
                params->mVertexBuffer->drawRange(LLRender::TRIANGLES, params->mStart, params->mEnd, params->mCount, params->mOffset);
            }
        }
    }

    // The attribute locations of the program as it is now linked (a shader reload makes a new program object).
    bool programReady()
    {
        if (gGLManager.mIsNativeVulkan)
        {
            return WolfVkNative::shadowBatchProgramReady(&gWolfShadowBatchProgram);
        }
        const GLuint program = gWolfShadowBatchProgram.mProgramObject;
        if (!program)
        {
            return false;
        }
        if (program != sLocationProgram)
        {
            sLocationProgram = program;
            sPositionLocation = glGetAttribLocation(program, "position");
            sMatrixLocation = glGetAttribLocation(program, "wolf_mvp");
        }
        return sPositionLocation >= 0 && sMatrixLocation >= 0;
    }

    // The page's vertex array object: positions from the page, the matrix columns one per instance from sMatrixBuffer,
    // indices from the page. Left bound.
    void bindPageArray(Page& page)
    {
        if (page.mVao && page.mVaoProgram == sLocationProgram)
        {
            glBindVertexArray(page.mVao);
            return;
        }
        if (page.mVao)
        {
            glDeleteVertexArrays(1, &page.mVao);
        }
        glGenVertexArrays(1, &page.mVao);
        page.mVaoProgram = sLocationProgram;
        glBindVertexArray(page.mVao);
        glBindBuffer(GL_ARRAY_BUFFER, page.mVertexBuffer);
        glEnableVertexAttribArray((GLuint)sPositionLocation);
        glVertexAttribPointer((GLuint)sPositionLocation, 3, GL_FLOAT, GL_FALSE, (GLsizei)POSITION_BYTES, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, sMatrixBuffer);
        for (GLuint column = 0; column < 4; ++column)
        {   // a mat4 attribute takes four consecutive locations, one vec4 column each
            const GLuint location = (GLuint)sMatrixLocation + column;
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(glm::mat4),
                                  (const void*)(size_t)(column * 4 * sizeof(F32)));
            glVertexAttribDivisor(location, 1);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, page.mIndexBuffer);
    }
}

bool WolfStaticShadowBatch::supported()
{
    if (gGLManager.mIsNativeVulkan)
    {
        return WolfVkNative::shadowBatchSupported();
    }
    // Source: llgl.cpp LLGLManager::initExtensions() - glMultiDrawElementsIndirect is loaded in the GL_VERSION_4_3
    // section, reached only when mGLVersion >= 4.29f (base instance: the GL_VERSION_4_2 section before it);
    // glVertexAttribDivisor in the GL_VERSION_3_3 section
#if LL_WINDOWS
    // Source: llglheaders.h - on Windows the entry points are pointers filled by wglGetProcAddress and may be null
    if (glMultiDrawElementsIndirect == nullptr || glVertexAttribDivisor == nullptr || glGenVertexArrays == nullptr)
    {
        return false;
    }
#endif
    return gGLManager.mGLVersion >= 4.29f;
}

void WolfStaticShadowBatch::beginPass()
{
    sPassActive = false;
    sTaken.clear();

    static LLCachedControl<bool> enabled(gSavedSettings, "WolfStaticShadowBatch", true);
    if (!enabled)
    {
        releaseAll();
        return;
    }
    // [W114-VULKAN S5 2026-11-26] Vulkan is recording this pass's draw calls (wolfvkbridge.h shadowPassOnVulkan): the
    // batch's glMultiDrawElementsIndirect calls (drawPass below) are not LLVertexBuffer draws, so the casters take the
    // stock path, which Vulkan records - the same triangles (the batch's self test compares exactly these two paths)
    if (sForceStock || !supported() || !programReady() || WolfVkBridge::shadowPassOnVulkan())
    {
        return;
    }
    // LLPipeline::postSort pushes a draw info only when its pass type is switched on (hasRenderType); the copies hold
    // all the batched types together, so the pass uses them only while every one of the types is on
    for (U32 type : LEGACY_TYPES)
    {
        if (!gPipeline.hasRenderType(type))
        {
            return;
        }
    }
    if (!gPipeline.hasRenderType(GLTF_TYPE))
    {
        return;
    }
    sPassActive = true;
}

void WolfStaticShadowBatch::endPass()
{
    sPassActive = false;
    sTaken.clear();
}

bool WolfStaticShadowBatch::isBatchedType(U32 type)
{
    return type == GLTF_TYPE || isLegacyType(type);
}

bool WolfStaticShadowBatch::takeGroup(LLSpatialGroup* group)
{
    if (!sPassActive)
    {
        return false;
    }
    // Source: llvovolume.cpp registerFace() - only a static face of a region's volume partition is drawn with
    // &group->mWolfRenderMatrix; llvovolume.cpp LLVolumePartition (PARTITION_VOLUME), LLVolumeBridge (a bridge)
    LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || part->isBridge() || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
    {
        return false;
    }

    Record* rec = (Record*)group->mWolfStaticBatch;
    if (!usable(group, rec))
    {
        // not while the group's geometry is waiting to be rebuilt or rewritten: the copy would be thrown away at once
        if (group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::MESH_DIRTY))
        {
            return false;
        }
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame != sBudgetFrame)
        {
            sBudgetFrame = frame;
            sBudgetBytes = 0;
        }
        if (sBudgetBytes >= BUILD_BYTES_PER_FRAME)
        {
            ++sLogWaiting;
            return false;
        }
        if (!rec)
        {
            rec = new Record();
            group->mWolfStaticBatch = rec;
            ++sRecords;
        }
        build(group, *rec);
    }
    if (!rec->mBatchable)
    {
        return false;
    }
    if (rec->mPage >= 0)
    {
        sTaken.push_back(group);
    }
    return true;
}

void WolfStaticShadowBatch::drawPass(PassCounts& counts)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (!sPassActive || sTaken.empty())
    {
        return;
    }

    sMatrices.clear();
    sDrawPages.clear();
    glm::mat4 projection = gGL.getProjectionMatrix();
    if (gGLManager.mIsNativeVulkan)
    {
        WolfVkNative::shadowBatchAdjustProjection(glm::value_ptr(projection));
    }
    bool drew_stock = false;
    for (LLSpatialGroup* group : sTaken)
    {
        Record* rec = (Record*)group->mWolfStaticBatch;
        if (!usable(group, rec) || !rec->mBatchable || rec->mPage < 0)
        {
            if (!drew_stock)
            {   // Source: pipeline.cpp renderObjects() - before its pushes
                gGL.loadMatrix(gGLModelView);
                gGLLastMatrix = NULL;
            }
            drawStock(group);
            drew_stock = true;
            ++counts.mStockGroups;
            continue;
        }
        const GLuint slot = (GLuint)sMatrices.size();
        sMatrices.push_back(groupMatrix(projection, group));
        Page* page = sPages[rec->mPage].get();
        if (!page->mQueued)
        {
            page->mQueued = true;
            sDrawPages.push_back(page);
        }
        for (const Command& source : rec->mCommands)
        {
            Command command = source;
            command.mBaseInstance = slot;
            page->mCullOn.push_back(command);
        }
        for (const GLTFCommand& gltf : rec->mGLTF)
        {
            Command command = gltf.mCommand;
            command.mBaseInstance = slot;
            (gltf.mMaterial->mDoubleSided ? page->mCullOff : page->mCullOn).push_back(command);
        }
        ++counts.mGroups;
        counts.mCommands += (U32)(rec->mCommands.size() + rec->mGLTF.size());
    }
    if (drew_stock)
    {   // Source: pipeline.cpp renderObjects() - after its pushes
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;
    }
    if (sDrawPages.empty())
    {
        return;
    }

    if (gGLManager.mIsNativeVulkan)
    {
        std::vector<S32> vk_pages;
        vk_pages.reserve(sDrawPages.size());
        for (Page* page : sDrawPages) vk_pages.push_back(page->mVkPage);
        WolfVkNative::flushShadowBatchPages(vk_pages.data(), static_cast<U32>(vk_pages.size()));

        U64 matrix_token = UINT64_MAX;
        if (WolfVkNative::uploadShadowBatchMatrices(sMatrices.data(), static_cast<U32>(sMatrices.size()), matrix_token))
        {
            for (Page* page : sDrawPages)
            {
                const U32 cull_on = static_cast<U32>(page->mCullOn.size());
                const U32 cull_off = static_cast<U32>(page->mCullOff.size());
                page->mCullOn.insert(page->mCullOn.end(), page->mCullOff.begin(), page->mCullOff.end());
                counts.mCalls += WolfVkNative::drawShadowBatchPage(
                    &gWolfShadowBatchProgram, page->mVkPage, matrix_token,
                    page->mCullOn.data(), cull_on, cull_off);
                page->mCullOn.clear();
                page->mCullOff.clear();
                page->mQueued = false;
            }
        }
        else
        {
            for (Page* page : sDrawPages)
            {
                page->mCullOn.clear();
                page->mCullOff.clear();
                page->mQueued = false;
            }
        }
        sDrawPages.clear();
        return;
    }

    gWolfShadowBatchProgram.bind();   // (flushes gGL, unbinds LLVertexBuffer's buffers)

    GLint default_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &default_vao);

    if (!sMatrixBuffer)
    {
        glGenBuffers(1, &sMatrixBuffer);
        glGenBuffers(1, &sCommandBuffer);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, sMatrixBuffer);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(sMatrices.size() * sizeof(glm::mat4)), sMatrices.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sCommandBuffer);
    for (Page* page : sDrawPages)
    {
        bindPageArray(*page);
        const size_t cull_on = page->mCullOn.size();
        const size_t cull_off = page->mCullOff.size();
        page->mCullOn.insert(page->mCullOn.end(), page->mCullOff.begin(), page->mCullOff.end());
        glBufferData(GL_DRAW_INDIRECT_BUFFER, (GLsizeiptr)(page->mCullOn.size() * sizeof(Command)), page->mCullOn.data(), GL_STREAM_DRAW);
        if (cull_on)
        {   // GL_CULL_FACE is on: LLGLEnable cull(GL_CULL_FACE) in LLPipeline::renderShadow
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, nullptr, (GLsizei)cull_on, (GLsizei)sizeof(Command));
            ++counts.mCalls;
        }
        if (cull_off)
        {   // Source: lldrawpool.cpp pushUntexturedGLTFBatch() - LLGLDisable cull_face(mat->mDoubleSided ? GL_CULL_FACE : 0)
            LLGLDisable cull_face(GL_CULL_FACE);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void*)(cull_on * sizeof(Command)),
                                        (GLsizei)cull_off, (GLsizei)sizeof(Command));
            ++counts.mCalls;
        }
        page->mCullOn.clear();
        page->mCullOff.clear();
        page->mQueued = false;
    }
    sDrawPages.clear();
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray((GLuint)default_vao);
    // bindPageArray() bound buffers to GL_ARRAY_BUFFER behind LLVertexBuffer's back: back to "nothing bound", which is
    // what its cache says after the bind() above
    LLVertexBuffer::unbind();

    gDeferredShadowProgram.bind();
}

void WolfStaticShadowBatch::releaseGroup(LLSpatialGroup* group)
{
    Record* rec = (Record*)group->mWolfStaticBatch;
    if (!rec)
    {
        return;
    }
    freeStorage(*rec);
    delete rec;
    group->mWolfStaticBatch = nullptr;
    --sRecords;
    if (sPassActive)
    {
        sTaken.erase(std::remove(sTaken.begin(), sTaken.end(), group), sTaken.end());
    }
}

void WolfStaticShadowBatch::dropCopy(LLSpatialGroup* group)
{
    Record* rec = (Record*)group->mWolfStaticBatch;
    if (rec)
    {
        freeStorage(*rec);   // mPage < 0 from here: drawPass() sends a group taken earlier in this pass the stock way
    }
}

void WolfStaticShadowBatch::releaseAll()
{
    if (sPages.empty() && !sMatrixBuffer)
    {
        return;
    }
    for (S32 i = 0; i < (S32)sPages.size(); ++i)
    {
        if (sPages[i])
        {
            deletePage(i);
        }
    }
    sPages.clear();
    if (sMatrixBuffer && !gGLManager.mIsNativeVulkan)
    {
        glDeleteBuffers(1, &sMatrixBuffer);
        glDeleteBuffers(1, &sCommandBuffer);
    }
    sMatrixBuffer = 0;
    sCommandBuffer = 0;
    // every record's storage is gone with the pages: a record of an older epoch is not usable() and frees nothing
    ++sEpoch;
    sPassActive = false;
    sTaken.clear();
}

std::string WolfStaticShadowBatch::logLine()
{
    U32 pages = 0;
    U64 vertices_used = 0;
    U64 vertices_capacity = 0;
    U64 indices_used = 0;
    U64 indices_capacity = 0;
    U32 groups = 0;
    for (const std::unique_ptr<Page>& page : sPages)
    {
        if (page)
        {
            ++pages;
            groups += page->mGroups;
            vertices_used += page->mVertices.used();
            vertices_capacity += page->mVertices.capacity();
            indices_used += page->mIndices.used();
            indices_capacity += page->mIndices.capacity();
        }
    }
    std::string line = llformat("static shadow batch: %u pages, %u groups with storage of %u records, positions %.1f of %.1f MB, indices %.1f of %.1f MB;"
                                " since last line: %llu copies made (%.1f MB), %llu left to stock (not batchable), %llu left to stock (memory limit), %llu waited for the frame budget",
                                pages, groups, sRecords,
                                (F64)vertices_used * POSITION_BYTES / 1048576.0, (F64)vertices_capacity * POSITION_BYTES / 1048576.0,
                                (F64)indices_used * INDEX_BYTES / 1048576.0, (F64)indices_capacity * INDEX_BYTES / 1048576.0,
                                (unsigned long long)sLogBuilds, (F64)sLogBuildBytes / 1048576.0,
                                (unsigned long long)sLogNotBatchable, (unsigned long long)sLogOverLimit, (unsigned long long)sLogWaiting);
    sLogOverLimit = 0;
    sLogBuilds = 0;
    sLogBuildBytes = 0;
    sLogNotBatchable = 0;
    sLogWaiting = 0;
    return line;
}

bool WolfStaticShadowBatch::selfTestWanted()
{
    static LLCachedControl<S32> frames(gSavedSettings, "WolfStaticShadowBatchSelfTest", 0);
    return frames > 0 && supported();
}

void WolfStaticShadowBatch::selfTestForceStock(bool force)
{
    sForceStock = force;
}

bool WolfStaticShadowBatch::selfTestReadDepth(U32 width, U32 height, std::vector<F32>& out)
{
    if (gGLManager.mIsNativeVulkan)
    {
        return false;
    }
    out.assign((size_t)width * height, 0.f);
    if (out.empty())
    {
        return false;
    }
    while (glGetError() != GL_NO_ERROR) { }   // an older error must not be taken for this read's
    // glReadPixels writes into a pixel pack buffer when one is bound, and packs rows by GL_PACK_ALIGNMENT
    GLint pack_buffer = 0;
    GLint pack_alignment = 4;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT, GL_FLOAT, out.data());
    const GLenum error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    return error == GL_NO_ERROR;
}

void WolfStaticShadowBatch::selfTestReport(S32 cascade, const std::vector<F32>& batched, const std::vector<F32>& stock)
{
    size_t differ = 0;
    size_t covered = 0;
    F32 worst = 0.f;
    const size_t count = llmin(batched.size(), stock.size());
    for (size_t i = 0; i < count; ++i)
    {
        if (stock[i] < 1.f)
        {
            ++covered;
        }
        // compared as bit patterns: the claim is the same depth map, not a close one
        if (memcmp(&batched[i], &stock[i], sizeof(F32)) != 0)
        {
            ++differ;
            worst = llmax(worst, fabsf(batched[i] - stock[i]));
        }
    }
    LL_INFOS("WolfShadows") << "static shadow batch self test: cascade " << cascade << ", " << count << " texels ("
                            << covered << " with a caster), " << differ << " differ from the stock path"
                            << (differ ? llformat(", largest difference %g", worst) : std::string()) << LL_ENDL;
}

void WolfStaticShadowBatch::selfTestFrameDone()
{
    const S32 frames = gSavedSettings.getS32("WolfStaticShadowBatchSelfTest");
    if (frames > 0)
    {
        gSavedSettings.setS32("WolfStaticShadowBatchSelfTest", frames - 1);
    }
}

#else // LL_DARWIN

// macOS: OpenGL 4.1 core has neither glMultiDrawElementsIndirect (4.3) nor base instance (4.2), and the system gl3.h
// does not declare them. Every caster takes the stock path.
bool WolfStaticShadowBatch::supported() { return false; }
void WolfStaticShadowBatch::beginPass() {}
void WolfStaticShadowBatch::endPass() {}
bool WolfStaticShadowBatch::takeGroup(LLSpatialGroup*) { return false; }
bool WolfStaticShadowBatch::isBatchedType(U32) { return false; }
void WolfStaticShadowBatch::drawPass(PassCounts&) {}
void WolfStaticShadowBatch::releaseGroup(LLSpatialGroup*) {}
void WolfStaticShadowBatch::dropCopy(LLSpatialGroup*) {}
void WolfStaticShadowBatch::releaseAll() {}
std::string WolfStaticShadowBatch::logLine() { return "static shadow batch: not available on this OpenGL"; }
bool WolfStaticShadowBatch::selfTestWanted() { return false; }
void WolfStaticShadowBatch::selfTestForceStock(bool) {}
bool WolfStaticShadowBatch::selfTestReadDepth(U32, U32, std::vector<F32>&) { return false; }
void WolfStaticShadowBatch::selfTestReport(S32, const std::vector<F32>&, const std::vector<F32>&) {}
void WolfStaticShadowBatch::selfTestFrameDone() {}

#endif // LL_DARWIN
