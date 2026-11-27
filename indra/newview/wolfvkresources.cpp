/**
 * @file wolfvkresources.cpp
 * @brief The Vulkan-owned store of vertex and index buffers, read by OpenGL through an import (see wolfvkresources.h).
 *
 * [VK S6a 2026-11-26]
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "wolfvkresources.h"
#include "wolfvkshare.h"
#include "vk/wolfvkdevice.h"
#include "llvertexbuffer.h"
#include "llgl.h"
#include "llviewercontrol.h"   // WolfVulkanDebugStoreFailAfterFrames
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // One exportable block. 32 MiB: a busy scene's buffers fit in a few dozen, and an emptied block gives a useful
    // amount back. A buffer larger than half a block gets a block of its own size.
    const VkDeviceSize BLOCK_BYTES = 32ull * 1024ull * 1024ull;
    // Every range starts on a multiple of this and has a multiple of it as its size. LLVertexBuffer::calcOffsets lays
    // each attribute array out on 16 bytes inside the range, so any multiple of 16 keeps that; 256 is used so that the
    // offset given to glBufferStorageMemEXT is also a multiple of what vkGetBufferMemoryRequirements reports for the
    // block's buffer on the drivers measured (init() refuses to start if the driver reports a larger alignment than
    // this, rather than hand OpenGL an offset the driver did not allow).
    const VkDeviceSize RANGE_ALIGN = 256;
    // Host-visible staging memory comes in chunks of this size (a larger single write gets a chunk of its own size).
    const VkDeviceSize STAGING_CHUNK_BYTES = 8ull * 1024ull * 1024ull;
    // A freed range is handed out again only after this many device frames beyond the frames in flight: command
    // buffers recorded before the free may still draw from it.
    const uint64_t FREE_DELAY_EXTRA_FRAMES = 2;
    // An empty block, and an idle staging chunk, is given back to the driver after this many device frames.
    const uint64_t IDLE_FRAMES = 600;

    struct Block
    {
        uint32_t mId = 0;
        VkBuffer mBuffer = VK_NULL_HANDLE;      // spans the block
        WolfVkAllocation mAllocation;           // its own exportable VkDeviceMemory
        uint32_t mGlMemory = 0;                 // the GL memory object imported from it
        VkDeviceSize mSize = 0;
        std::map<VkDeviceSize, VkDeviceSize> mFree;   // offset -> size, coalesced
        uint32_t mLive = 0;                     // ranges handed out or waiting in sPendingFree
        uint64_t mEmptySinceFrame = 0;
    };

    struct Interval { uint32_t mStart; uint32_t mEnd; };   // [start, end) bytes of a range written in the open batch

    struct Range
    {
        Block* mBlock = nullptr;
        VkDeviceSize mOffset = 0;      // in the block
        VkDeviceSize mBytes = 0;       // reserved (a multiple of RANGE_ALIGN)
        uint32_t mSize = 0;            // asked for by LLVertexBuffer
        uint32_t mGlName = 0;
        bool mIndices = false;
        uint64_t mSerial = 0;          // never re-used: tells a view its range was replaced
        uint64_t mTouchedBatch = 0;    // the open batch id when this range last joined sTouched
        std::vector<Interval> mPending;
    };

    struct PendingFree
    {
        Range* mRange;
        uint64_t mFrame;
    };

    struct Chunk
    {
        std::unique_ptr<WolfVkBuffer> mBuffer;
        VkDeviceSize mUsed = 0;
        uint64_t mLastBatch = 0;       // the newest batch whose copies read from it
        uint64_t mLastUseFrame = 0;
    };

    struct Copy
    {
        VkBuffer mSrc;
        VkBuffer mDst;
        VkBufferCopy mRegion;
        bool mBreak;                   // must start a new vkCmdCopyBuffer (it overlaps bytes an earlier copy of this batch wrote)
    };

    struct Batch
    {
        uint64_t mId = 0;
        VkFence mFence = VK_NULL_HANDLE;
        VkCommandBuffer mCmd = VK_NULL_HANDLE;
    };

    struct View
    {
        std::unique_ptr<WolfVkVertexBuffer> mView;
        uint64_t mVertexSerial = 0, mIndexSerial = 0;
        uint32_t mNumVerts = 0, mNumIndices = 0, mTypeMask = 0, mIndicesType = 0;
    };

    class Store;
    Store* sStore = nullptr;
    WolfVkDevice* sDevice = nullptr;
    bool sFailed = false;
    std::string sFailure;

    std::vector<std::unique_ptr<Block>> sBlocks;
    uint32_t sNextBlockId = 1;
    uint32_t sMemoryType = UINT32_MAX;
    uint64_t sNextSerial = 1;
    std::deque<PendingFree> sPendingFree;
    uint64_t sBytesUsed = 0;
    uint32_t sRangeCount = 0;

    VkCommandPool sPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> sFreeCmds;
    std::vector<VkFence> sFreeFences;
    std::deque<Batch> sInFlight;       // submitted, oldest first
    uint64_t sBatchId = 1;             // the open batch
    uint64_t sCompletedBatch = 0;      // every batch up to this id has run

    std::vector<Chunk> sChunks;
    size_t sCurrentChunk = SIZE_MAX;
    std::vector<Copy> sCopies;         // the open batch
    std::vector<Range*> sTouched;      // ranges written in the open batch
    bool sBreakNext = false;

    VkSemaphore sVkSemGl = VK_NULL_HANDLE;   // OpenGL signals, the batch's submit waits
    VkSemaphore sVkSemVk = VK_NULL_HANDLE;   // the batch's submit signals, OpenGL waits
    uint32_t sGlSemGl = 0, sGlSemVk = 0;

    std::unordered_map<LLVertexBuffer*, View> sViews;
    void (*sPrevOnDestroy)(LLVertexBuffer*) = nullptr;

    // [POSE CHECK] serials of the ranges a Vulkan pass drew from this frame, and writes into them afterwards
    std::unordered_set<uint64_t> sPoseDrawn;
    uint64_t sPoseLateFlushes = 0;
    uint32_t sPoseLateLogged = 0;

    WolfVkResources::Stats sStats;
    uint64_t sStatsFrame = 0;
    uint64_t sStartFrame = 0;          // the device frame the store was installed in (the forced-failure debug setting counts from it)
    uint32_t sBatchesThisFrame = 0;

    VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

    void fail(const std::string& why)
    {
        if (sFailed) return;
        sFailed = true;
        sFailure = why;
        LL_WARNS("Vulkan") << "Resource store failed: " << why << " - the renderer falls back to OpenGL and the buffers move to OpenGL's own" << LL_ENDL;
    }

    // ---- blocks ---------------------------------------------------------------------------------------------------

    Block* newBlock(VkDeviceSize min_bytes, std::string& error)
    {
        const VkDeviceSize size = std::max(BLOCK_BYTES, alignUp(min_bytes, RANGE_ALIGN));
        VkDevice dev = sDevice->device();
        std::unique_ptr<Block> block(new Block());
        // Source: vulkan_core.h:5957 VkExternalMemoryBufferCreateInfo { sType, pNext, handleTypes }; :292 sType;
        // :3887 VkBufferCreateInfo. Usage: vertex + index data (:3064-3065), the destination of the staging copies and
        // the source of a debug read-back (:3058-3059).
        VkExternalMemoryBufferCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = sDevice->externalHandleType();
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.pNext = &external;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult res = gVk.vkCreateBuffer(dev, &info, nullptr, &block->mBuffer);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateBuffer (block): ") + WolfVkLoader::resultString(res);
            return nullptr;
        }
        VkMemoryRequirements req{};
        gVk.vkGetBufferMemoryRequirements(dev, block->mBuffer, &req);
        if (req.alignment > RANGE_ALIGN || (RANGE_ALIGN % std::max<VkDeviceSize>(req.alignment, 1)) != 0)
        {
            error = llformat("the driver wants buffer memory aligned to %llu bytes, the store lays ranges out on %llu",
                             static_cast<unsigned long long>(req.alignment), static_cast<unsigned long long>(RANGE_ALIGN));
            gVk.vkDestroyBuffer(dev, block->mBuffer, nullptr);
            return nullptr;
        }
        // an exportable allocation of its own, NOT a dedicated one in Vulkan's sense (no VkMemoryDedicatedAllocateInfo:
        // no resource is named), so OpenGL may bind many buffers into it; WolfVkAllocator chains
        // VkExportMemoryAllocateInfo when export types are given
        if (!sDevice->allocator().allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true, true, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                           block->mAllocation, error, sDevice->externalHandleType()))
        {
            gVk.vkDestroyBuffer(dev, block->mBuffer, nullptr);
            return nullptr;
        }
        res = gVk.vkBindBufferMemory(dev, block->mBuffer, block->mAllocation.mMemory, block->mAllocation.mOffset);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkBindBufferMemory (block): ") + WolfVkLoader::resultString(res);
            sDevice->allocator().free(block->mAllocation);
            gVk.vkDestroyBuffer(dev, block->mBuffer, nullptr);
            return nullptr;
        }
        // glImportMemory takes the allocation's full size ("<size> ... the size of the memory object", the driver may
        // have granted more than the buffer asked for)
        block->mGlMemory = WolfVkShare::glImportMemory(*sDevice, block->mAllocation.mMemory, block->mAllocation.mSize, false, error);
        if (!block->mGlMemory)
        {
            sDevice->allocator().free(block->mAllocation);
            gVk.vkDestroyBuffer(dev, block->mBuffer, nullptr);
            return nullptr;
        }
        block->mId = sNextBlockId++;
        block->mSize = size;
        block->mFree[0] = size;
        block->mEmptySinceFrame = sDevice->frameCounter();
        LL_INFOS("Vulkan") << "Resource store: block " << block->mId << " of " << (size >> 20) << " MiB, OpenGL memory object " << block->mGlMemory << LL_ENDL;
        sBlocks.push_back(std::move(block));
        return sBlocks.back().get();
    }

    bool carve(Block& block, VkDeviceSize bytes, VkDeviceSize& offset)
    {
        for (auto it = block.mFree.begin(); it != block.mFree.end(); ++it)
        {
            if (it->second >= bytes)
            {
                offset = it->first;
                const VkDeviceSize rest = it->second - bytes;
                block.mFree.erase(it);
                if (rest) block.mFree[offset + bytes] = rest;
                return true;
            }
        }
        return false;
    }

    void giveBack(Block& block, VkDeviceSize offset, VkDeviceSize bytes)
    {
        auto next = block.mFree.lower_bound(offset);
        if (next != block.mFree.end() && offset + bytes == next->first)
        {
            bytes += next->second;
            next = block.mFree.erase(next);
        }
        if (next != block.mFree.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset)
            {
                prev->second += bytes;
                return;
            }
        }
        block.mFree[offset] = bytes;
    }

    // The Vulkan and OpenGL objects of a block. hung: the device cannot be waited for, so the Vulkan objects stay with it.
    void destroyBlock(Block& block, bool deferred, bool hung)
    {
        WolfVkShare::glDeleteMemory(block.mGlMemory);
        block.mGlMemory = 0;
        if (hung || !sDevice || !sDevice->hasDevice()) return;
        const VkBuffer buffer = block.mBuffer;
        WolfVkAllocation allocation = block.mAllocation;
        WolfVkDevice* device = sDevice;
        auto destroy = [device, buffer, allocation]() mutable
        {
            if (buffer != VK_NULL_HANDLE) gVk.vkDestroyBuffer(device->device(), buffer, nullptr);
            device->allocator().free(allocation);
        };
        if (deferred) sDevice->deferDestroy(destroy);
        else destroy();
        block.mBuffer = VK_NULL_HANDLE;
    }

    // ---- batches --------------------------------------------------------------------------------------------------

    void pollBatches()
    {
        while (!sInFlight.empty())
        {
            Batch& batch = sInFlight.front();
            // Source: vulkan_core.h:4568 PFN_vkGetFenceStatus(device, fence): VK_SUCCESS signalled, VK_NOT_READY not yet
            const VkResult res = gVk.vkGetFenceStatus(sDevice->device(), batch.mFence);
            if (res == VK_NOT_READY) break;
            if (res != VK_SUCCESS)
            {
                sDevice->lost(res, "vkGetFenceStatus (resource store)");
                fail(std::string("vkGetFenceStatus: ") + WolfVkLoader::resultString(res));
                break;
            }
            sCompletedBatch = batch.mId;
            gVk.vkResetFences(sDevice->device(), 1, &batch.mFence);
            // the pool was made with RESET_COMMAND_BUFFER: vkBeginCommandBuffer resets the buffer when it is used again
            sFreeFences.push_back(batch.mFence);
            sFreeCmds.push_back(batch.mCmd);
            sInFlight.pop_front();
        }
    }

    // Per device frame: ranges whose delay is over go back to their blocks; empty blocks and idle staging are released.
    void tick()
    {
        if (!sDevice) return;
        const uint64_t frame = sDevice->frameCounter();
        pollBatches();
        if (frame == sStatsFrame) return;
        sStatsFrame = frame;
        sStats.mBatchesLastFrame = sBatchesThisFrame;
        sBatchesThisFrame = 0;
        const uint64_t delay = sDevice->framesInFlight() + FREE_DELAY_EXTRA_FRAMES;
        while (!sPendingFree.empty() && sPendingFree.front().mFrame + delay <= frame)
        {
            Range* range = sPendingFree.front().mRange;
            sPendingFree.pop_front();
            Block* block = range->mBlock;
            giveBack(*block, range->mOffset, range->mBytes);
            if (--block->mLive == 0) block->mEmptySinceFrame = frame;
            delete range;
        }
        if (sFailed || sDevice->hung()) return;
        // empty blocks (always keeping one) and idle staging chunks (always keeping one)
        for (size_t i = 0; i < sBlocks.size() && sBlocks.size() > 1; )
        {
            Block& block = *sBlocks[i];
            if (block.mLive == 0 && block.mEmptySinceFrame + IDLE_FRAMES <= frame)
            {
                LL_INFOS("Vulkan") << "Resource store: block " << block.mId << " given back (empty for " << IDLE_FRAMES << " frames)" << LL_ENDL;
                destroyBlock(block, true, false);
                sBlocks.erase(sBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else ++i;
        }
        for (size_t i = 0; i < sChunks.size() && sChunks.size() > 1; )
        {
            Chunk& chunk = sChunks[i];
            if (i != sCurrentChunk && chunk.mLastBatch <= sCompletedBatch && chunk.mLastUseFrame + IDLE_FRAMES <= frame)
            {
                sStats.mStagingBytes -= chunk.mBuffer->size();
                sChunks.erase(sChunks.begin() + static_cast<std::ptrdiff_t>(i));   // WolfVkBuffer::destroy defers the Vulkan release
                if (sCurrentChunk != SIZE_MAX && sCurrentChunk > i) sCurrentChunk--;
            }
            else ++i;
        }
    }

    // size bytes of staging memory for the open batch: the buffer, the offset in it and the host pointer.
    bool stagingSpace(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset, Chunk*& chunk_out, std::string& error)
    {
        const VkDeviceSize need = alignUp(size, 16);
        if (sCurrentChunk != SIZE_MAX)
        {
            Chunk& chunk = sChunks[sCurrentChunk];
            if (chunk.mUsed + need <= chunk.mBuffer->size())
            {
                buffer = chunk.mBuffer->buffer();
                offset = chunk.mUsed;
                chunk.mUsed += need;
                chunk_out = &chunk;
                return true;
            }
        }
        // a chunk no submitted batch still reads
        for (size_t i = 0; i < sChunks.size(); ++i)
        {
            Chunk& chunk = sChunks[i];
            if (i != sCurrentChunk && chunk.mLastBatch <= sCompletedBatch && chunk.mBuffer->size() >= need)
            {
                chunk.mUsed = need;
                sCurrentChunk = i;
                buffer = chunk.mBuffer->buffer();
                offset = 0;
                chunk_out = &chunk;
                return true;
            }
        }
        Chunk chunk;
        chunk.mBuffer.reset(new WolfVkBuffer());
        const VkDeviceSize bytes = std::max(STAGING_CHUNK_BYTES, need);
        // host_only: the buffer itself is host visible; it is only ever the source of copies (vulkan_core.h:3058)
        if (!chunk.mBuffer->create(*sDevice, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error)) return false;
        sStats.mStagingBytes += bytes;
        chunk.mUsed = need;
        sChunks.push_back(std::move(chunk));
        sCurrentChunk = sChunks.size() - 1;
        buffer = sChunks.back().mBuffer->buffer();
        offset = 0;
        chunk_out = &sChunks.back();
        return true;
    }

    void forgetOpenBatch()
    {
        for (Range* r : sTouched) { r->mPending.clear(); r->mTouchedBatch = 0; }
        sTouched.clear();
        sCopies.clear();
        sBreakNext = false;
    }

    // ---- the store LLVertexBuffer talks to ------------------------------------------------------------------------------

    class Store final : public LLVertexBuffer::WolfVkStore
    {
    public:
        bool allocate(bool indices, U32 size, U32& gl_name, void*& handle) override
        {
            if (sFailed || !sDevice || sDevice->hung() || size == 0) return false;
            tick();
            const VkDeviceSize bytes = alignUp(size, RANGE_ALIGN);
            Block* block = nullptr;
            VkDeviceSize offset = 0;
            // the newest block first: it is the one with room while a scene loads
            for (auto it = sBlocks.rbegin(); it != sBlocks.rend() && !block; ++it)
            {
                if (carve(**it, bytes, offset)) block = it->get();
            }
            std::string error;
            if (!block)
            {
                block = newBlock(bytes, error);
                if (!block || !carve(*block, bytes, offset))
                {
                    fail(error.empty() ? "a new block could not hold the buffer" : error);
                    return false;
                }
            }
            const uint32_t name = WolfVkShare::glBufferOverMemory(block->mGlMemory, offset, size, error);
            if (!name)
            {
                giveBack(*block, offset, bytes);
                fail(error);
                return false;
            }
            Range* range = new Range();
            range->mBlock = block;
            range->mOffset = offset;
            range->mBytes = bytes;
            range->mSize = size;
            range->mGlName = name;
            range->mIndices = indices;
            range->mSerial = sNextSerial++;
            block->mLive++;
            sBytesUsed += bytes;
            sRangeCount++;
            gl_name = name;
            handle = range;
            return true;
        }

        void free(void* handle) override
        {
            Range* range = static_cast<Range*>(handle);
            if (!range) return;
            // writes of the open batch into it are dropped with it: nobody may draw them any more
            if (!range->mPending.empty() || range->mTouchedBatch == sBatchId)
            {
                const VkDeviceSize first = range->mOffset, last = range->mOffset + range->mBytes;
                const VkBuffer dst = range->mBlock->mBuffer;
                bool removed_break = false;
                sCopies.erase(std::remove_if(sCopies.begin(), sCopies.end(), [&](const Copy& c)
                {
                    const bool hit = c.mDst == dst && c.mRegion.dstOffset >= first && c.mRegion.dstOffset < last;
                    if (hit && c.mBreak) removed_break = true;
                    return hit;
                }), sCopies.end());
                // a removed copy may have carried the "new call" mark for those after it: keep the split
                if (removed_break && !sCopies.empty()) sBreakNext = true;
                sTouched.erase(std::remove(sTouched.begin(), sTouched.end(), range), sTouched.end());
                range->mPending.clear();
            }
            // OpenGL's own later commands no longer name the buffer; its earlier ones are ahead of this in its stream
            if (range->mGlName && gGLManager.mInited)
            {
                const GLuint name = range->mGlName;
                glDeleteBuffers(1, &name);
            }
            range->mGlName = 0;
            sBytesUsed -= range->mBytes;
            sRangeCount--;
            sPendingFree.push_back({ range, sDevice ? sDevice->frameCounter() : 0 });
        }

        void write(void* handle, U32 start, U32 size, const U8* data) override
        {
            Range* range = static_cast<Range*>(handle);
            if (!range || sFailed || !sDevice || sDevice->hung() || size == 0) return;
            if (static_cast<uint64_t>(start) + size > range->mSize)
            {
                // LLVertexBuffer never asks for this (its regions lie inside mSize); a write outside the range would
                // land in another buffer's bytes
                LL_WARNS_ONCE("Vulkan") << "Resource store: write of " << size << " bytes at " << start << " outside a range of " << range->mSize << " bytes ignored" << LL_ENDL;
                return;
            }
            if (!sPoseDrawn.empty() && sPoseDrawn.count(range->mSerial))
            {
                sPoseLateFlushes++;
                if (sPoseLateLogged < 10)
                {
                    sPoseLateLogged++;
                    LL_WARNS("WolfVulkanPose") << "write into a " << (range->mIndices ? "index" : "vertex") << " range (bytes " << start << "-" << (start + size - 1)
                                               << ") after a Vulkan pass recorded draws from it this frame" << LL_ENDL;
                }
            }
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceSize staging_offset = 0;
            Chunk* chunk = nullptr;
            std::string error;
            if (!stagingSpace(size, staging, staging_offset, chunk, error))
            {
                fail("staging memory: " + error);
                return;
            }
            chunk->mBuffer->write(staging_offset, data, size);   // copies, and flushes a non-coherent memory type
            chunk->mLastBatch = sBatchId;
            chunk->mLastUseFrame = sDevice->frameCounter();

            // bytes this batch already writes: the copies must then run in order, which one vkCmdCopyBuffer with both
            // regions would not promise, so the later one starts a new call
            const uint32_t end = start + size;
            bool overlaps = false;
            for (const Interval& iv : range->mPending) if (start < iv.mEnd && iv.mStart < end) { overlaps = true; break; }
            if (overlaps)
            {
                sBreakNext = true;
                for (Range* r : sTouched) r->mPending.clear();
            }
            if (range->mTouchedBatch != sBatchId)
            {
                range->mTouchedBatch = sBatchId;
                sTouched.push_back(range);
            }
            range->mPending.push_back({ start, end });
            Copy copy;
            copy.mSrc = staging;
            copy.mDst = range->mBlock->mBuffer;
            copy.mRegion.srcOffset = staging_offset;
            copy.mRegion.dstOffset = range->mOffset + start;
            copy.mRegion.size = size;
            copy.mBreak = sBreakNext;
            sBreakNext = false;
            sCopies.push_back(copy);
            sStats.mBytesUploaded += size;
        }

        void batchEnd() override
        {
            if (sCopies.empty())
            {
                forgetOpenBatch();
                return;
            }
            if (sFailed || !sDevice || sDevice->hung())
            {
                forgetOpenBatch();
                return;
            }
            tick();
            VkDevice dev = sDevice->device();
            std::string error;

            // a command buffer and a fence of our own (recycled once their batch has run)
            Batch batch;
            batch.mId = sBatchId;
            if (!sFreeCmds.empty()) { batch.mCmd = sFreeCmds.back(); sFreeCmds.pop_back(); }
            else
            {
                VkCommandBufferAllocateInfo alloc{};
                alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                alloc.commandPool = sPool;
                alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc.commandBufferCount = 1;
                const VkResult res = gVk.vkAllocateCommandBuffers(dev, &alloc, &batch.mCmd);
                if (res != VK_SUCCESS) { fail(std::string("vkAllocateCommandBuffers: ") + WolfVkLoader::resultString(res)); forgetOpenBatch(); return; }
            }
            if (!sFreeFences.empty()) { batch.mFence = sFreeFences.back(); sFreeFences.pop_back(); }
            else
            {
                VkFenceCreateInfo fi{};
                fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                const VkResult res = gVk.vkCreateFence(dev, &fi, nullptr, &batch.mFence);
                if (res != VK_SUCCESS) { sFreeCmds.push_back(batch.mCmd); fail(std::string("vkCreateFence: ") + WolfVkLoader::resultString(res)); forgetOpenBatch(); return; }
            }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VkResult res = gVk.vkBeginCommandBuffer(batch.mCmd, &begin);
            if (res != VK_SUCCESS)
            {
                sFreeCmds.push_back(batch.mCmd); sFreeFences.push_back(batch.mFence);
                fail(std::string("vkBeginCommandBuffer: ") + WolfVkLoader::resultString(res)); forgetOpenBatch(); return;
            }
            // Source: vulkan_core.h:4590 PFN_vkCmdCopyBuffer(cmd, srcBuffer, dstBuffer, regionCount, pRegions); consecutive
            // copies between the same two buffers share a call, except across a break (see write())
            std::vector<VkBufferCopy> regions;
            for (size_t i = 0; i < sCopies.size(); )
            {
                regions.clear();
                const VkBuffer src = sCopies[i].mSrc, dst = sCopies[i].mDst;
                size_t j = i;
                do { regions.push_back(sCopies[j].mRegion); ++j; }
                while (j < sCopies.size() && sCopies[j].mSrc == src && sCopies[j].mDst == dst && !sCopies[j].mBreak);
                gVk.vkCmdCopyBuffer(batch.mCmd, src, dst, static_cast<uint32_t>(regions.size()), regions.data());
                i = j;
            }
            // the bytes are read as vertex and index data by later command buffers on this queue
            // (vulkan_core.h VkMemoryBarrier { sType, pNext, srcAccessMask, dstAccessMask })
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            gVk.vkCmdPipelineBarrier(batch.mCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            res = gVk.vkEndCommandBuffer(batch.mCmd);
            if (res != VK_SUCCESS)
            {
                sFreeCmds.push_back(batch.mCmd); sFreeFences.push_back(batch.mFence);
                fail(std::string("vkEndCommandBuffer: ") + WolfVkLoader::resultString(res)); forgetOpenBatch(); return;
            }

            // the OpenGL names whose memory this batch writes
            std::vector<uint32_t> names;
            names.reserve(sTouched.size());
            for (Range* r : sTouched) if (r->mGlName) names.push_back(r->mGlName);

            // 1. OpenGL's commands so far (draws that read the old bytes) are ahead of the copy
            if (!WolfVkShare::glSignalBuffers(sGlSemGl, static_cast<uint32_t>(names.size()), names.data(), error))
            {
                sFreeCmds.push_back(batch.mCmd); sFreeFences.push_back(batch.mFence);
                fail(error); forgetOpenBatch(); return;
            }
            // 2. the copy, after that signal, signalling OpenGL when it has run
            WolfVkDevice::ExtraSync sync;
            sync.mWaits.push_back(sVkSemGl);
            sync.mWaitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
            sync.mSignals.push_back(sVkSemVk);
            if (!sDevice->submitTracked(batch.mCmd, sync, batch.mFence, error))
            {
                // OpenGL's signal stays unconsumed and its wait below is never issued: the pair is not used again
                // (fail() ends the store); the command buffer and fence were not accepted by the queue
                sFreeCmds.push_back(batch.mCmd); sFreeFences.push_back(batch.mFence);
                fail(error); forgetOpenBatch(); return;
            }
            sInFlight.push_back(batch);
            sBatchId++;
            sStats.mBatches++;
            sBatchesThisFrame++;
            // 3. OpenGL's commands from here on (draws that read the new bytes) are behind the copy
            if (!WolfVkShare::glWaitBuffers(sGlSemVk, static_cast<uint32_t>(names.size()), names.data(), error))
            {
                fail(error);
            }
            forgetOpenBatch();
        }

        U64 bytesUsed() override { return sBytesUsed; }
    };

    void onBufferDestroy(LLVertexBuffer* buffer)
    {
        sViews.erase(buffer);
        if (sPrevOnDestroy) sPrevOnDestroy(buffer);
    }

    // Waits (bounded) for every submitted batch; false when the device is hung.
    bool waitBatches()
    {
        if (!sDevice || !sDevice->hasDevice() || sDevice->hung()) return false;
        while (!sInFlight.empty())
        {
            Batch& batch = sInFlight.front();
            // vulkan_core.h:4569 PFN_vkWaitForFences(device, fenceCount, pFences, waitAll, timeout)
            const VkResult res = gVk.vkWaitForFences(sDevice->device(), 1, &batch.mFence, VK_TRUE, WolfVkDevice::FRAME_WAIT_LIMIT_NS);
            if (res == VK_TIMEOUT)
            {
                sDevice->declareHung("a resource store upload has not finished after 8 s");
                return false;
            }
            if (res != VK_SUCCESS)
            {
                sDevice->lost(res, "vkWaitForFences (resource store)");
                return false;
            }
            pollBatches();
        }
        return true;
    }

    // The first bytes of an OpenGL buffer as a draw fetches them: each vertex is 4 unsigned bytes read as an integer
    // attribute (glVertexAttribIPointer) and captured unchanged by transform feedback (OpenGL 4.6 core 13.3 "Transform
    // Feedback"; GLSL 3.30: the viewer's context is a 3.3+ core profile). bytes must be a multiple of 4. Every binding
    // and switch it touches is put back, so LLVertexBuffer's and LLGLSLShader's tracking still holds.
    bool glReadByDrawing(GLuint buffer, U32 bytes, std::vector<U8>& out, std::string& error)
    {
        out.clear();
        if (bytes == 0 || (bytes & 3u) != 0) { error = "size not a multiple of 4"; return false; }
        while (glGetError() != GL_NO_ERROR) {}
        static const char* const vs_source =
            "#version 330 core\n"
            "in uvec4 a;\n"
            "flat out uvec4 o;\n"
            "void main()\n"
            "{\n"
            "    o = a;\n"
            "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "}\n";
        // a fragment stage that is never run (rasterizer discard): present so the program is complete on every driver
        static const char* const fs_source =
            "#version 330 core\n"
            "out vec4 c;\n"
            "void main()\n"
            "{\n"
            "    c = vec4(0.0);\n"
            "}\n";
        GLint saved_program = 0, saved_vao = 0, saved_array = 0, saved_tf = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array);
        glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, &saved_tf);
        const GLboolean saved_discard = glIsEnabled(GL_RASTERIZER_DISCARD);

        bool ok = true;
        GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), program = glCreateProgram();
        glShaderSource(vs, 1, &vs_source, nullptr);
        glCompileShader(vs);
        glShaderSource(fs, 1, &fs_source, nullptr);
        glCompileShader(fs);
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "a");
        const char* varyings[1] = { "o" };
        glTransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) { error = "the read-by-draw program did not link"; ok = false; }

        GLuint vao = 0, capture = 0;
        const GLsizei count = static_cast<GLsizei>(bytes / 4u);
        std::vector<GLuint> captured(static_cast<size_t>(bytes), 0u);   // 4 unsigned ints a vertex
        if (ok)
        {
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glEnableVertexAttribArray(0);
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, 4, nullptr);
            glGenBuffers(1, &capture);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, capture);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(captured.size() * sizeof(GLuint)), nullptr, GL_STREAM_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, capture);
            glUseProgram(program);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, count);
            glEndTransformFeedback();
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(captured.size() * sizeof(GLuint)), captured.data());
            if (glGetError() != GL_NO_ERROR) { error = "the read-by-draw raised an OpenGL error"; ok = false; }
        }
        if (!saved_discard) glDisable(GL_RASTERIZER_DISCARD);
        glUseProgram(static_cast<GLuint>(saved_program));
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(saved_tf));
        glBindVertexArray(static_cast<GLuint>(saved_vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(saved_array));
        if (capture) glDeleteBuffers(1, &capture);
        if (vao) glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        while (glGetError() != GL_NO_ERROR) {}
        if (!ok) return false;
        out.resize(bytes);
        for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<U8>(captured[i] & 0xFFu);
        return true;
    }

    // Vulkan writes, OpenGL reads: three ranges at different offsets, then a partial rewrite of the middle one.
    bool roundTrip(Store& store, std::string& why)
    {
        const U32 sizes[3] = { 100, 4096, 70000 };
        U32 names[3] = { 0, 0, 0 };
        void* handles[3] = { nullptr, nullptr, nullptr };
        std::vector<U8> expect[3];
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i)
        {
            if (!store.allocate(i == 2, sizes[i], names[i], handles[i])) { why = "self-test allocation: " + sFailure; ok = false; break; }
            expect[i].resize(sizes[i]);
            for (U32 b = 0; b < sizes[i]; ++b) expect[i][b] = static_cast<U8>((b * 7u + 13u * static_cast<U32>(i + 1)) & 0xFFu);
            store.write(handles[i], 0, sizes[i], expect[i].data());
        }
        auto check = [&](const char* round) -> bool
        {
            store.batchEnd();
            if (sFailed) { why = std::string("self-test ") + round + ": " + sFailure; return false; }
            // OpenGL's queue now waits on the store's semaphore: bounded, so a signal that never comes is a failure
            // and not a frozen viewer
            if (!WolfVkShare::glFinishBounded(sDevice, "resource store self-test")) { why = std::string("self-test ") + round + ": OpenGL did not get past the store's semaphore"; return false; }
            // read back by DRAWING, not with glGetBufferSubData: the renderer's use of these buffers is drawing, and on
            // NVIDIA 615.78 glGetBufferSubData keeps returning the bytes of its first read after Vulkan rewrites the
            // memory, while draws see the new bytes (probe of 2026-11-26, vk-s6/probe/RESULTS.md)
            std::vector<U8> got;
            for (int i = 0; i < 3; ++i)
            {
                std::string draw_error;
                if (!glReadByDrawing(names[i], sizes[i], got, draw_error)) { why = llformat("self-test %s: range %d: ", round, i) + draw_error; return false; }
                if (got != expect[i])
                {
                    size_t at = 0;
                    while (at < got.size() && got[at] == expect[i][at]) ++at;
                    why = llformat("self-test %s: range %d (%u bytes) drawn by OpenGL differs from byte %zu (0x%02x, wrote 0x%02x)",
                                   round, i, sizes[i], at, got[at], expect[i][at]);
                    return false;
                }
            }
            return true;
        };
        if (ok) ok = check("first write");
        if (ok)
        {
            // bytes 1000..1999 of the middle range, the rest must stay
            for (U32 b = 1000; b < 2000; ++b) expect[1][b] = static_cast<U8>(0xFFu - (b & 0xFFu));
            store.write(handles[1], 1000, 1000, expect[1].data() + 1000);
            ok = check("partial rewrite");
        }
        for (int i = 0; i < 3; ++i) if (handles[i]) store.free(handles[i]);
        return ok;
    }

    void destroyVulkanObjects(bool hung)
    {
        for (auto& block : sBlocks) destroyBlock(*block, false, hung);
        sBlocks.clear();
        for (const PendingFree& p : sPendingFree) delete p.mRange;
        sPendingFree.clear();
        WolfVkShare::glDeleteSemaphore(sGlSemGl);
        WolfVkShare::glDeleteSemaphore(sGlSemVk);
        sGlSemGl = sGlSemVk = 0;
        if (hung)
        {
            // a stuck submit may still read the staging memory and name the semaphores, fences and command buffers:
            // all of it stays with the abandoned device (the chunks are released without destruction)
            for (Chunk& chunk : sChunks) (void)chunk.mBuffer.release();
        }
        else if (sDevice && sDevice->hasDevice())
        {
            VkDevice dev = sDevice->device();
            for (VkFence f : sFreeFences) gVk.vkDestroyFence(dev, f, nullptr);
            if (sPool != VK_NULL_HANDLE) gVk.vkDestroyCommandPool(dev, sPool, nullptr);   // frees its command buffers
            if (sVkSemGl != VK_NULL_HANDLE) gVk.vkDestroySemaphore(dev, sVkSemGl, nullptr);
            if (sVkSemVk != VK_NULL_HANDLE) gVk.vkDestroySemaphore(dev, sVkSemVk, nullptr);
        }
        sChunks.clear();
        sCurrentChunk = SIZE_MAX;
        sFreeFences.clear();
        sFreeCmds.clear();
        sInFlight.clear();
        sPool = VK_NULL_HANDLE;
        sVkSemGl = sVkSemVk = VK_NULL_HANDLE;
        sCopies.clear();
        sTouched.clear();
        sBytesUsed = 0;
        sRangeCount = 0;
        sStats = WolfVkResources::Stats();
    }
}

namespace WolfVkResources
{

bool init(WolfVkDevice& device, std::string& why)
{
    if (sStore) return true;
#if LL_DARWIN
    (void)device;
    why = "Apple's OpenGL has no GL_EXT_memory_object: buffers stay OpenGL's until the whole frame is drawn by Vulkan";
    return false;
#else
    if (!WolfVkShare::available(device, why)) return false;
    if (!WolfVkShare::glBufferImportAvailable()) { why = "OpenGL has no glBufferStorageMemEXT"; return false; }

    // Can a buffer of this use have exportable memory, and may that memory hold more than one resource?
    // Source: vulkan_core.h:6187 PFN_vkGetPhysicalDeviceExternalBufferProperties (Vulkan 1.1, an instance command);
    // :5927 VkPhysicalDeviceExternalBufferInfo { sType, pNext, flags, usage, handleType }; :5935 VkExternalBufferProperties
    // { sType, pNext, externalMemoryProperties }; :5609-5610 DEDICATED_ONLY 0x1, EXPORTABLE 0x2.
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
        gVk.vkGetInstanceProcAddr ? gVk.vkGetInstanceProcAddr(device.instance(), "vkGetPhysicalDeviceExternalBufferProperties") : nullptr);
    if (!query) { why = "vkGetPhysicalDeviceExternalBufferProperties is missing (Vulkan 1.1 needed)"; return false; }
    VkPhysicalDeviceExternalBufferInfo ext_info{};
    ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    ext_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ext_info.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(device.externalHandleType());
    VkExternalBufferProperties ext_props{};
    ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    query(device.physical(), &ext_info, &ext_props);
    const VkExternalMemoryFeatureFlags features = ext_props.externalMemoryProperties.externalMemoryFeatures;
    if (!(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) { why = "the driver cannot export vertex buffer memory"; return false; }
    if (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
    {
        why = "the driver exports vertex buffer memory as dedicated allocations only (one per buffer): the store needs shared blocks";
        return false;
    }

    sDevice = &device;
    sFailed = false;
    sFailure.clear();
    VkDevice dev = device.device();
    // Source: vulkan_core.h VkCommandPoolCreateInfo { sType, pNext, flags, queueFamilyIndex };
    // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT: each buffer is reset by its next vkBeginCommandBuffer
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = device.queueFamily();
    const VkResult res = gVk.vkCreateCommandPool(dev, &pool, nullptr, &sPool);
    if (res != VK_SUCCESS)
    {
        why = std::string("vkCreateCommandPool: ") + WolfVkLoader::resultString(res);
        sDevice = nullptr;
        return false;
    }
    if (!WolfVkShare::glCreateSemaphore(device, sVkSemGl, sGlSemGl, why) || !WolfVkShare::glCreateSemaphore(device, sVkSemVk, sGlSemVk, why))
    {
        destroyVulkanObjects(false);
        sDevice = nullptr;
        return false;
    }
    std::unique_ptr<Store> store(new Store());
    const bool passed = roundTrip(*store, why);
    if (!passed || sFailed)
    {
        if (why.empty()) why = sFailure;
        const bool idle = waitBatches();
        destroyVulkanObjects(!idle);
        sDevice = nullptr;
        sFailed = false;   // nothing was installed: there is nothing for the owner to fall back from
        sFailure.clear();
        return false;
    }
    sStore = store.release();
    sStartFrame = device.frameCounter();
    sPrevOnDestroy = LLVertexBuffer::sWolfVkOnDestroy;
    LLVertexBuffer::sWolfVkOnDestroy = onBufferDestroy;
    LLVertexBuffer::setWolfVkStore(sStore);
    LL_INFOS("Vulkan") << "Resource store started: vertex and index buffers live in Vulkan memory that OpenGL imports (self-test passed: "
                       << "3 ranges written by Vulkan and drawn by OpenGL, and a partial rewrite)" << LL_ENDL;
    return true;
#endif
}

void shutdown()
{
    if (!sStore && !sDevice) return;
    if (sStore)
    {
        // every buffer back into OpenGL's own pool, refilled from its client copy; each free() only notes the range
        if (gGLManager.mInited) LLVertexBuffer::wolfVkReleaseAllToGL();
        LLVertexBuffer::setWolfVkStore(nullptr);
        if (LLVertexBuffer::sWolfVkOnDestroy == onBufferDestroy) LLVertexBuffer::sWolfVkOnDestroy = sPrevOnDestroy;
        sPrevOnDestroy = nullptr;
    }
    sViews.clear();
    sPoseDrawn.clear();
    forgetOpenBatch();
    // OpenGL may still be queued behind the last batch's semaphore and Vulkan may still run a batch: both are waited
    // for (bounded) before anything they use is destroyed; if either wait runs out the Vulkan side is left with the device
    bool idle = waitBatches();
    if (idle && sDevice) idle = sDevice->waitIdle();
    if (idle && gGLManager.mInited) idle = WolfVkShare::glFinishBounded(sDevice, "resource store shutdown");
    destroyVulkanObjects(!idle);
    delete sStore;
    sStore = nullptr;
    sDevice = nullptr;
    // sFailed / sFailure stay readable for the owner's log line until the next init()
}

bool active() { return sStore != nullptr && !sFailed; }
bool failed() { return sFailed; }
const std::string& failure() { return sFailure; }

WolfVkVertexBuffer* vertexBuffer(LLVertexBuffer* buffer)
{
    if (!sStore || sFailed || !buffer) return nullptr;
    // what LLVertexBuffer::setBuffer does before OpenGL draws: nothing mapped may be pending, and the store's batch
    // must have been submitted, so the draws recorded after this read the bytes OpenGL would
    LLVertexBuffer::flushBuffers();
    const Range* vertices = static_cast<const Range*>(buffer->getWolfVkVertexHandle());
    const Range* indices = static_cast<const Range*>(buffer->getWolfVkIndexHandle());
    if (!vertices) return nullptr;
    View& view = sViews[buffer];
    const uint64_t vserial = vertices->mSerial, iserial = indices ? indices->mSerial : 0;
    if (!view.mView || view.mVertexSerial != vserial || view.mIndexSerial != iserial || view.mNumVerts != buffer->getNumVerts()
        || view.mNumIndices != buffer->getNumIndices() || view.mTypeMask != buffer->getTypeMask() || view.mIndicesType != buffer->getIndicesType())
    {
        view.mView.reset(new WolfVkVertexBuffer());
        // LLVertexBuffer::mIndicesType is GL_UNSIGNED_SHORT, or GL_UNSIGNED_INT after setIndexData(const U32*)
        // (llvertexbuffer.cpp); vulkan_core.h:2344-2345 VK_INDEX_TYPE_UINT16 / VK_INDEX_TYPE_UINT32
        const VkIndexType index_type = buffer->getIndicesType() == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        view.mView->initExternal(buffer->getTypeMask(), buffer->getNumVerts(), indices ? buffer->getNumIndices() : 0,
                                 vertices->mBlock->mBuffer, vertices->mOffset,
                                 indices ? indices->mBlock->mBuffer : VK_NULL_HANDLE, indices ? indices->mOffset : 0, index_type);
        view.mVertexSerial = vserial;
        view.mIndexSerial = iserial;
        view.mNumVerts = buffer->getNumVerts();
        view.mNumIndices = buffer->getNumIndices();
        view.mTypeMask = buffer->getTypeMask();
        view.mIndicesType = buffer->getIndicesType();
    }
    return view.mView.get();
}

void frameBegin()
{
    // WolfVulkanDebugStoreFailAfterFrames (debug, 0 = off): the store fails by itself that many device frames after it
    // started, as it would after a failed block import or semaphore call, so the fall-back below can be tested with a
    // scene's worth of buffers in it.
    static LLCachedControl<S32> fail_after(gSavedSettings, "WolfVulkanDebugStoreFailAfterFrames", 0);
    if (sStore && !sFailed && sDevice && fail_after > 0 && sDevice->frameCounter() >= sStartFrame + static_cast<uint64_t>(fail_after))
    {
        fail("forced by the debug setting WolfVulkanDebugStoreFailAfterFrames");
    }
    if (sStore && sFailed)
    {
        LL_WARNS("Vulkan") << "Resource store: shut down after a failure (" << sFailure << "); every buffer is OpenGL's own again and OpenGL draws the geometry" << LL_ENDL;
        shutdown();
    }
}

void poseNoteDrawn(LLVertexBuffer* buffer)
{
    if (!buffer) return;
    if (const Range* r = static_cast<const Range*>(buffer->getWolfVkVertexHandle())) sPoseDrawn.insert(r->mSerial);
    if (const Range* r = static_cast<const Range*>(buffer->getWolfVkIndexHandle())) sPoseDrawn.insert(r->mSerial);
}
void poseFrameEnd() { sPoseDrawn.clear(); }
uint64_t poseLateFlushes() { return sPoseLateFlushes; }

Stats stats()
{
    Stats s = sStats;
    s.mBlocks = static_cast<uint32_t>(sBlocks.size());
    s.mBlockBytes = 0;
    for (const auto& block : sBlocks) s.mBlockBytes += block->mSize;
    s.mBytesUsed = sBytesUsed;
    s.mRanges = sRangeCount;
    return s;
}

} // namespace WolfVkResources
