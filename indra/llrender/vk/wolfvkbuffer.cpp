/**
 * @file wolfvkbuffer.cpp
 * @brief Device-local buffers with staging uploads and the Vulkan vertex buffer (see wolfvkbuffer.h).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "linden_common.h"
#include "wolfvkbuffer.h"
#include "wolfvkdevice.h"
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------------------------------------------
// WolfVkBuffer

WolfVkBuffer::~WolfVkBuffer()
{
    destroy();
}

bool WolfVkBuffer::create(WolfVkDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, bool host_only, std::string& error)
{
    destroy();
    mDevice = &device;
    mSize = size;
    mHostOnly = host_only;
    VkDevice dev = device.device();

    auto make = [&](VkBufferUsageFlags use, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                    VkBuffer& out_buffer, WolfVkAllocation& out_alloc) -> bool
    {
        // Source: vulkan_core.h:3887 VkBufferCreateInfo { sType, pNext, flags, size, usage, sharingMode, queueFamilyIndexCount, pQueueFamilyIndices }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = use;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;   // :2264
        VkResult res = gVk.vkCreateBuffer(dev, &info, nullptr, &out_buffer);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateBuffer: ") + WolfVkLoader::resultString(res);
            return false;
        }
        // Source: vulkan_core.h:4560 PFN_vkGetBufferMemoryRequirements(device, buffer, pMemoryRequirements)
        VkMemoryRequirements req{};
        gVk.vkGetBufferMemoryRequirements(dev, out_buffer, &req);
        if (!device.allocator().allocate(req, required, preferred, true /*linear*/, false, out_buffer, VK_NULL_HANDLE, out_alloc, error))
        {
            gVk.vkDestroyBuffer(dev, out_buffer, nullptr);
            out_buffer = VK_NULL_HANDLE;
            return false;
        }
        // Source: vulkan_core.h:4558 PFN_vkBindBufferMemory(device, buffer, memory, memoryOffset)
        res = gVk.vkBindBufferMemory(dev, out_buffer, out_alloc.mMemory, out_alloc.mOffset);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkBindBufferMemory: ") + WolfVkLoader::resultString(res);
            device.allocator().free(out_alloc);
            gVk.vkDestroyBuffer(dev, out_buffer, nullptr);
            out_buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };

    if (host_only)
    {
        // Source: vulkan_core.h:2856-2858 HOST_VISIBLE | HOST_COHERENT required (writes land without flushes), HOST_CACHED preferred for read-backs
        if (!make(usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, mBuffer, mAllocation))
        {
            // a type without HOST_COHERENT is acceptable: the allocator's flush() covers it
            if (!make(usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, mBuffer, mAllocation))
            {
                destroy();
                return false;
            }
        }
        return true;
    }
    // device-local buffer + host-visible staging
    if (!make(usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, mBuffer, mAllocation))   // :3059, :2855
    {
        destroy();
        return false;
    }
    if (!make(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mStaging, mStagingAllocation))   // :3058
    {
        destroy();
        return false;
    }
    return true;
}

void WolfVkBuffer::destroy()
{
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        VkBuffer b = mBuffer, s = mStaging;
        WolfVkAllocation a = mAllocation, sa = mStagingAllocation;
        // the last frame that could still read these finishes before they go (WolfVkDevice::deferDestroy)
        device->deferDestroy([device, b, s, a, sa]() mutable
        {
            if (b != VK_NULL_HANDLE) gVk.vkDestroyBuffer(device->device(), b, nullptr);
            if (s != VK_NULL_HANDLE) gVk.vkDestroyBuffer(device->device(), s, nullptr);
            device->allocator().free(a);
            device->allocator().free(sa);
        });
    }
    mBuffer = VK_NULL_HANDLE;
    mStaging = VK_NULL_HANDLE;
    mAllocation = WolfVkAllocation();
    mStagingAllocation = WolfVkAllocation();
    mPending.clear();
    mSize = 0;
    mDevice = nullptr;
}

void WolfVkBuffer::write(VkDeviceSize offset, const void* data, VkDeviceSize size)
{
    uint8_t* host = hostData();
    if (!host || offset + size > mSize)
    {
        LL_WARNS_ONCE("Vulkan") << "WolfVkBuffer::write outside the buffer (" << offset << "+" << size << " of " << mSize << ")" << LL_ENDL;
        return;
    }
    memcpy(host + offset, data, static_cast<size_t>(size));
    markDirty(offset, size);
}

void WolfVkBuffer::markDirty(VkDeviceSize offset, VkDeviceSize size)
{
    if (size == 0) return;
    if (mHostOnly)
    {
        mDevice->allocator().flush(mAllocation, offset, size);
        return;
    }
    mDevice->allocator().flush(mStagingAllocation, offset, size);
    // merge with a touching or overlapping range
    for (Range& r : mPending)
    {
        if (offset <= r.mOffset + r.mSize && offset + size >= r.mOffset)
        {
            const VkDeviceSize end = std::max(r.mOffset + r.mSize, offset + size);
            r.mOffset = std::min(r.mOffset, offset);
            r.mSize = end - r.mOffset;
            return;
        }
    }
    mPending.push_back({ offset, size });
}

void WolfVkBuffer::recordUploads(VkCommandBuffer cmd, VkPipelineStageFlags dst_stages, VkAccessFlags dst_access, WolfVkStagingRing* ring)
{
    if (mPending.empty() || mHostOnly || mStaging == VK_NULL_HANDLE)
    {
        mPending.clear();
        return;
    }
    // Order these copies after every earlier copy into this buffer AND after earlier draws reading it. The barrier
    // below only orders copy -> read, so an earlier frame's copy and this one were unordered (run nv-hang1 19:07:22Z:
    // SYNC-HAZARD-WRITE-AFTER-WRITE on vkCmdCopyBuffer, "must allow VK_ACCESS_2_TRANSFER_WRITE_BIT at COPY"); the
    // writes could land out of order and leave stale vertices. dst_stages in the source scope covers write-after-read
    // (an execution dependency is enough for WAR, Vulkan spec 7.1 "Execution and Memory Dependencies").
    // Source: vulkan_core.h:4013 VkBufferMemoryBarrier; :2934 vkCmdPipelineBarrier; :3116 VK_ACCESS_TRANSFER_WRITE_BIT
    {
        VkBufferMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.buffer = mBuffer;
        before.offset = 0;
        before.size = VK_WHOLE_SIZE;
        gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | dst_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &before, 0, nullptr);
    }
    // Source: vulkan_core.h:3983 VkBufferCopy { srcOffset, dstOffset, size }; :4590 PFN_vkCmdCopyBuffer(cmd, srcBuffer, dstBuffer, regionCount, pRegions)
    std::vector<VkBufferCopy> direct;   // ranges copied straight from the shadow (no ring, or ring full)
    for (const Range& r : mPending)
    {
        VkBuffer src = VK_NULL_HANDLE;
        VkDeviceSize src_offset = 0;
        uint8_t* host = nullptr;
        if (ring && ring->allocate(r.mSize, src, src_offset, host))
        {
            memcpy(host, hostData() + r.mOffset, static_cast<size_t>(r.mSize));
            ring->flush(src_offset, r.mSize);
            VkBufferCopy copy{ src_offset, r.mOffset, r.mSize };
            gVk.vkCmdCopyBuffer(cmd, src, mBuffer, 1, &copy);
        }
        else
        {
            direct.push_back({ r.mOffset, r.mOffset, r.mSize });
        }
    }
    if (!direct.empty())
    {
        gVk.vkCmdCopyBuffer(cmd, mStaging, mBuffer, static_cast<uint32_t>(direct.size()), direct.data());
    }
    // Source: vulkan_core.h:4013 VkBufferMemoryBarrier { sType, pNext, srcAccessMask, dstAccessMask, srcQueueFamilyIndex, dstQueueFamilyIndex, buffer, offset, size }
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;   // :251
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;      // :3116
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = mBuffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stages, 0, 0, nullptr, 1, &barrier, 0, nullptr);   // :2934
    mPending.clear();
}

bool WolfVkBuffer::uploadNow(VkPipelineStageFlags dst_stages, VkAccessFlags dst_access, std::string& error)
{
    if (mPending.empty() || mHostOnly)
    {
        mPending.clear();
        return true;
    }
    VkCommandBuffer cmd = mDevice->beginOneTime();
    if (cmd == VK_NULL_HANDLE)
    {
        error = "no one-time command buffer";
        return false;
    }
    recordUploads(cmd, dst_stages, dst_access);
    return mDevice->endOneTimeAndWait(cmd, error);
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkVertexBuffer

// Source: llvertexbuffer.cpp:702-717 LLVertexBuffer::sTypeSize
const uint32_t WolfVkVertexBuffer::sTypeSize[WolfVkVertexBuffer::TYPE_MAX] =
{
    16, // TYPE_VERTEX        sizeof(LLVector4)
    16, // TYPE_NORMAL        sizeof(LLVector4)
    8,  // TYPE_TEXCOORD0     sizeof(LLVector2)
    8,  // TYPE_TEXCOORD1
    8,  // TYPE_TEXCOORD2
    8,  // TYPE_TEXCOORD3
    4,  // TYPE_COLOR         sizeof(LLColor4U)
    4,  // TYPE_EMISSIVE      sizeof(LLColor4U)
    16, // TYPE_TANGENT       sizeof(LLVector4)
    4,  // TYPE_WEIGHT        sizeof(F32)
    16, // TYPE_WEIGHT4       sizeof(LLVector4)
    16, // TYPE_CLOTHWEIGHT   sizeof(LLVector4)
    8,  // TYPE_JOINT         sizeof(U64)
    16, // TYPE_TEXTURE_INDEX (position.w; stride 16)
};

// Source: llshadermgr.cpp:1353-1366 mReservedAttribs
const char* const WolfVkVertexBuffer::sAttribName[WolfVkVertexBuffer::TYPE_MAX] =
{
    "position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color", "emissive",
    "tangent", "weight", "weight4", "clothing", "joint", "texture_index",
};

WolfVkVertexBuffer::AttribFormat WolfVkVertexBuffer::attribFormat(AttributeType type)
{
    // Source: llvertexbuffer.cpp:1765-1865 LLVertexBuffer::setupVertexBuffer, each glVertexAttribPointer call:
    //   normal   3 GL_FLOAT         -> VK_FORMAT_R32G32B32_SFLOAT (vulkan_core.h:1965)
    //   texcoord 2 GL_FLOAT         -> VK_FORMAT_R32G32_SFLOAT (:1962)
    //   tangent  4 GL_FLOAT         -> VK_FORMAT_R32G32B32A32_SFLOAT (:1968)
    //   color/emissive 4 GL_UNSIGNED_BYTE normalised -> VK_FORMAT_R8G8B8A8_UNORM (:1896)
    //   weight   1 GL_FLOAT         -> VK_FORMAT_R32_SFLOAT (:1959)
    //   weight4  4 GL_FLOAT         -> R32G32B32A32_SFLOAT
    //   joint    4 GL_UNSIGNED_SHORT integer (glVertexAttribIPointer) -> VK_FORMAT_R16G16B16A16_UINT (:1954)
    //   clothing 4 GL_FLOAT normalised (GL_TRUE on floats is a no-op) -> R32G32B32A32_SFLOAT
    //   texture_index 1 GL_UNSIGNED_INT integer at position + 12 -> VK_FORMAT_R32_SINT (:1958): the shader declares it
    //                 `in int texture_index` (objects/indexedTextureV.glsl:26); GL's glVertexAttribIPointer converts the
    //                 unsigned fetch to the int input, Vulkan requires the format's numeric class to match the input's
    //                 (VUID-VkGraphicsPipelineCreateInfo-Input-08733, raised by the validation layer on 2026-11-08 with R32_UINT)
    //   position 3 GL_FLOAT         -> R32G32B32_SFLOAT
    switch (type)
    {
    case TYPE_VERTEX: return { VK_FORMAT_R32G32B32_SFLOAT, 3, false };
    case TYPE_NORMAL: return { VK_FORMAT_R32G32B32_SFLOAT, 3, false };
    case TYPE_TEXCOORD0: case TYPE_TEXCOORD1: case TYPE_TEXCOORD2: case TYPE_TEXCOORD3: return { VK_FORMAT_R32G32_SFLOAT, 2, false };
    case TYPE_COLOR: case TYPE_EMISSIVE: return { VK_FORMAT_R8G8B8A8_UNORM, 4, false };
    case TYPE_TANGENT: case TYPE_WEIGHT4: case TYPE_CLOTHWEIGHT: return { VK_FORMAT_R32G32B32A32_SFLOAT, 4, false };
    case TYPE_WEIGHT: return { VK_FORMAT_R32_SFLOAT, 1, false };
    case TYPE_JOINT: return { VK_FORMAT_R16G16B16A16_UINT, 4, true };
    case TYPE_TEXTURE_INDEX: return { VK_FORMAT_R32_SINT, 1, true };
    default: return { VK_FORMAT_UNDEFINED, 0, false };
    }
}

WolfVkVertexBuffer::~WolfVkVertexBuffer()
{
    destroy();
}

// Source: llvertexbuffer.cpp:1062-1082 LLVertexBuffer::calcOffsets
uint32_t WolfVkVertexBuffer::calcOffsets(uint32_t typemask, uint32_t* offsets, uint32_t num_vertices)
{
    uint32_t offset = 0;
    for (uint32_t i = 0; i < TYPE_TEXTURE_INDEX; i++)
    {
        const uint32_t mask = 1u << i;
        if (typemask & mask)
        {
            if (offsets && sTypeSize[i])
            {
                offsets[i] = offset;
                offset += sTypeSize[i] * num_vertices;
                offset = (offset + 0xF) & ~0xFu;
            }
        }
    }
    if (offsets)
    {
        offsets[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;
    }
    return offset;
}

bool WolfVkVertexBuffer::allocateBuffer(uint32_t nverts, uint32_t nindices, std::string& error)
{
    if (!mDevice)
    {
        error = "WolfVkVertexBuffer not initialised";
        return false;
    }
    // updateNumVerts :1244-1260
    const uint32_t needed = calcOffsets(mTypeMask, mOffsets, nverts);
    if (needed != mSize)
    {
        mVertices.destroy();
        mSize = 0;
        if (needed && !mVertices.create(*mDevice, needed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, error))   // :3065
        {
            return false;
        }
        mSize = needed;
    }
    mNumVerts = nverts;
    // updateNumIndices :1262-1278: sizeof(U16) * nindices
    const uint32_t needed_indices = 2u * nindices;
    if (needed_indices != mIndicesSize)
    {
        mIndices.destroy();
        mIndicesSize = 0;
        if (needed_indices && !mIndices.create(*mDevice, needed_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false, error))   // :3064
        {
            return false;
        }
        mIndicesSize = needed_indices;
    }
    mNumIndices = nindices;
    return true;
}

void WolfVkVertexBuffer::destroy()
{
    mVertices.destroy();
    mIndices.destroy();
    mSize = mIndicesSize = mNumVerts = mNumIndices = 0;
    mMappedVertexRegions.clear();
    mMappedIndexRegions.clear();
}

// Source: llvertexbuffer.cpp:1299-1314 expand_region
bool WolfVkVertexBuffer::expandRegion(MappedRegion& region, uint32_t start, uint32_t end)
{
    if (end < region.mStart || start > region.mEnd)
    {
        return false;
    }
    region.mStart = std::min(region.mStart, start);
    region.mEnd = std::max(region.mEnd, end);
    return true;
}

uint8_t* WolfVkVertexBuffer::mapVertexBuffer(AttributeType type, uint32_t index, int32_t count)
{
    if (!mVertices.hostData()) return nullptr;
    if (count == -1)
    {
        count = static_cast<int32_t>(mNumVerts - index);
    }
    const uint32_t start = mOffsets[type] + sTypeSize[type] * index;
    const uint32_t end = start + sTypeSize[type] * static_cast<uint32_t>(count) - 1;
    bool flagged = false;
    for (MappedRegion& region : mMappedVertexRegions)
    {
        if (expandRegion(region, start, end)) { flagged = true; break; }
    }
    if (!flagged)
    {
        mMappedVertexRegions.push_back({ start, end });
    }
    return mVertices.hostData() + mOffsets[type] + sTypeSize[type] * index;
}

uint8_t* WolfVkVertexBuffer::mapIndexBuffer(uint32_t index, int32_t count)
{
    if (!mIndices.hostData()) return nullptr;
    if (count == -1)
    {
        count = static_cast<int32_t>(mNumIndices - index);
    }
    const uint32_t start = 2u * index;
    const uint32_t end = start + 2u * static_cast<uint32_t>(count) - 1;
    bool flagged = false;
    for (MappedRegion& region : mMappedIndexRegions)
    {
        if (expandRegion(region, start, end)) { flagged = true; break; }
    }
    if (!flagged)
    {
        mMappedIndexRegions.push_back({ start, end });
    }
    return mIndices.hostData() + 2u * index;
}

void WolfVkVertexBuffer::flushRegions(std::vector<MappedRegion>& regions, WolfVkBuffer& buffer)
{
    // Source: llvertexbuffer.cpp:1515-1546: sort, merge touching regions, flush each run
    if (regions.empty()) return;
    std::sort(regions.begin(), regions.end(), [](const MappedRegion& a, const MappedRegion& b) { return a.mStart < b.mStart; });
    uint32_t start = regions[0].mStart;
    uint32_t end = regions[0].mEnd;
    for (size_t i = 1; i < regions.size(); ++i)
    {
        if (regions[i].mStart <= end + 1)
        {
            end = std::max(end, regions[i].mEnd);
        }
        else
        {
            buffer.markDirty(start, end - start + 1);
            start = regions[i].mStart;
            end = regions[i].mEnd;
        }
    }
    buffer.markDirty(start, end - start + 1);
    regions.clear();
}

void WolfVkVertexBuffer::unmapBuffer()
{
    flushRegions(mMappedVertexRegions, mVertices);
    flushRegions(mMappedIndexRegions, mIndices);
}

void WolfVkVertexBuffer::recordUploads(VkCommandBuffer cmd, WolfVkStagingRing* ring)
{
    // vertex input stage reads attributes and indices (vulkan_core.h:2924 VERTEX_INPUT_BIT; :3106 VERTEX_ATTRIBUTE_READ, :3105 INDEX_READ)
    mVertices.recordUploads(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, ring);
    mIndices.recordUploads(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, ring);
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkStagingRing

WolfVkStagingRing::~WolfVkStagingRing()
{
    destroy();
}

bool WolfVkStagingRing::init(WolfVkDevice& device, VkDeviceSize bytes_per_frame, std::string& error)
{
    destroy();
    mDevice = &device;
    mBytes = bytes_per_frame;
    for (uint32_t i = 0; i < device.framesInFlight(); ++i)
    {
        std::unique_ptr<WolfVkBuffer> b(new WolfVkBuffer());
        // Source: vulkan_core.h:3058 VK_BUFFER_USAGE_TRANSFER_SRC_BIT; host only (persistently mapped by the allocator)
        if (!b->create(device, bytes_per_frame, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error))
        {
            destroy();
            return false;
        }
        mBuffers.push_back(std::move(b));
    }
    return true;
}

void WolfVkStagingRing::destroy()
{
    mBuffers.clear();   // each buffer's destroy() defers its release
    mDevice = nullptr;
    mUsed = 0;
}

void WolfVkStagingRing::beginFrame(uint32_t frame_index)
{
    if (mBuffers.empty()) return;
    mFrame = frame_index % static_cast<uint32_t>(mBuffers.size());
    mUsed = 0;
    mWarnedFull = false;
}

void WolfVkStagingRing::flush(VkDeviceSize offset, VkDeviceSize size)
{
    if (!mBuffers.empty() && mBuffers[mFrame]->isValid()) mBuffers[mFrame]->markDirty(offset, size);   // host-only: a flush, no upload range
}

bool WolfVkStagingRing::allocate(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset, uint8_t*& host)
{
    if (mBuffers.empty() || !mBuffers[mFrame]->isValid()) return false;
    const VkDeviceSize start = (mUsed + 15) / 16 * 16;   // 16-byte alignment: any attribute array start is a multiple of 16 (LLVertexBuffer::calcOffsets)
    if (start + size > mBytes)
    {
        if (!mWarnedFull)
        {
            LL_WARNS_ONCE("Vulkan") << "Frame staging ring full (" << mBytes << " bytes); later copies this frame read the shadow directly" << LL_ENDL;
            mWarnedFull = true;
        }
        return false;
    }
    buffer = mBuffers[mFrame]->buffer();
    offset = start;
    host = mBuffers[mFrame]->hostData() + start;
    mUsed = start + size;
    return true;
}

void WolfVkVertexBuffer::bind(VkCommandBuffer cmd, uint32_t attrib_mask) const
{
    VkBuffer buffers[TYPE_MAX];
    VkDeviceSize offsets[TYPE_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; i < TYPE_MAX; ++i)
    {
        if (!(attrib_mask & (1u << i))) continue;
        buffers[n] = mVertices.buffer();
        // setupVertexBuffer: colour reads the emissive array when both are wanted (:1810-1814), texture_index is position + 12 (:1853-1856)
        uint32_t type = i;
        if (i == TYPE_COLOR && (attrib_mask & (1u << TYPE_EMISSIVE))) type = TYPE_EMISSIVE;
        offsets[n] = mOffsets[type];
        n++;
    }
    if (n && mVertices.buffer() != VK_NULL_HANDLE)
    {
        // Source: vulkan_core.h:4655 PFN_vkCmdBindVertexBuffers(cmd, firstBinding, bindingCount, pBuffers, pOffsets)
        gVk.vkCmdBindVertexBuffers(cmd, 0, n, buffers, offsets);
    }
    if (mIndices.buffer() != VK_NULL_HANDLE)
    {
        // Source: vulkan_core.h:4654 PFN_vkCmdBindIndexBuffer(cmd, buffer, offset, indexType); :2344 VK_INDEX_TYPE_UINT16 (mIndicesType GL_UNSIGNED_SHORT)
        gVk.vkCmdBindIndexBuffer(cmd, mIndices.buffer(), 0, VK_INDEX_TYPE_UINT16);
    }
}

void WolfVkVertexBuffer::drawRange(VkCommandBuffer cmd, uint32_t count, uint32_t indices_offset) const
{
    // Source: vulkan_core.h:4657 PFN_vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    // drawRange's indices_offset is in indices (glDrawRangeElements gets it * sizeof(U16), llvertexbuffer.cpp:938)
    gVk.vkCmdDrawIndexed(cmd, count, 1, indices_offset, 0, 0);
}

void WolfVkVertexBuffer::drawArrays(VkCommandBuffer cmd, uint32_t first, uint32_t count) const
{
    // Source: vulkan_core.h:4656 PFN_vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance)
    gVk.vkCmdDraw(cmd, count, 1, first, 0);
}
