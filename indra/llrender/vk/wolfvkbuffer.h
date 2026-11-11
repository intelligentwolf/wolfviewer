/**
 * @file wolfvkbuffer.h
 * @brief Device-local buffers with staging uploads, and the Vulkan vertex buffer behind LLVertexBuffer's interface.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * WolfVkBuffer: one VkBuffer in device-local memory plus a host-visible staging buffer of the same size. write()
 * copies bytes into the staging buffer and remembers the range; recordUploads() turns the ranges into
 * vkCmdCopyBuffer commands (plus the barrier that makes them visible to vertex input / index read / uniform read),
 * once per frame, or uploadNow() does it on a one-time command buffer. Freed resources go through
 * WolfVkDevice::deferDestroy so a frame in flight never loses its buffer.
 *
 * WolfVkVertexBuffer: the same shape as LLVertexBuffer (llvertexbuffer.h): allocateBuffer(nverts, nindices) with a
 * type mask, mapVertexBuffer / mapIndexBuffer returning client pointers, unmapBuffer flushing only the mapped byte
 * ranges (LLVertexBuffer::MappedRegion, :1316-1390 and _unmapBuffer :1448-1583), the same structure-of-arrays
 * layout (LLVertexBuffer::calcOffsets :1062-1082: each attribute array 16-byte aligned, one after the other,
 * stride sTypeSize[type] :702-717), the same index type (U16, updateNumIndices :1262). bind() + draw() replace
 * setBuffer() + drawRange(): one vertex binding per attribute present in the shader's attribute mask, pointing into
 * the one VkBuffer at the attribute's offset.
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

#ifndef WOLF_VK_BUFFER_H
#define WOLF_VK_BUFFER_H

#include "wolfvkallocator.h"
#include <string>
#include <vector>
#include <memory>

class WolfVkDevice;
class WolfVkStagingRing;

class WolfVkBuffer
{
public:
    WolfVkBuffer() = default;
    ~WolfVkBuffer();
    WolfVkBuffer(const WolfVkBuffer&) = delete;
    WolfVkBuffer& operator=(const WolfVkBuffer&) = delete;

    // usage: VK_BUFFER_USAGE_* bits (vulkan_core.h:3058-3065); TRANSFER_DST is added for the staging copies.
    // host_only: no device-local copy, the buffer itself is host visible (read-backs, small per-frame data)
    bool create(WolfVkDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, bool host_only, std::string& error);
    void destroy();

    bool isValid() const { return mBuffer != VK_NULL_HANDLE; }
    VkBuffer buffer() const { return mBuffer; }
    VkDeviceSize size() const { return mSize; }
    // The host-visible bytes: the staging buffer's, or the buffer's own when host_only.
    uint8_t* hostData() const { return mHostOnly ? mAllocation.mMapped : mStagingAllocation.mMapped; }
    // [V14 2026-11-11] Before reading hostData() of a host_only buffer the device wrote (a read-back): makes those writes
    // visible on a non-coherent memory type (vkInvalidateMappedMemoryRanges); a no-op on coherent memory.
    void invalidateForRead();

    // Copies into the host bytes and marks [offset, offset+size) for upload (no-op range when host_only, the write
    // is the data; flushes non-coherent memory either way).
    void write(VkDeviceSize offset, const void* data, VkDeviceSize size);
    // Marks a range already written through hostData() for upload.
    void markDirty(VkDeviceSize offset, VkDeviceSize size);
    bool hasPendingUploads() const { return !mPending.empty(); }
    // Records the pending copies into cmd with a barrier for dst_stages/dst_access (e.g. VERTEX_INPUT | VERTEX_ATTRIBUTE_READ).
    // [STAGING RACE 2026-11-08] With a ring, the bytes are first copied from the host shadow into the ring's per-frame
    // memory and the GPU copies from THERE: the shadow (hostData) is then only ever touched by the CPU, so a GL flush
    // hook writing it while last frame's copy still runs on the GPU cannot corrupt a frame. Without a ring (or when it
    // is full) the copy reads the shadow directly, as the first cut did.
    void recordUploads(VkCommandBuffer cmd, VkPipelineStageFlags dst_stages, VkAccessFlags dst_access, WolfVkStagingRing* ring = nullptr);
    // Records them on a one-time command buffer and waits (start-up uploads).
    bool uploadNow(VkPipelineStageFlags dst_stages, VkAccessFlags dst_access, std::string& error);

private:
    struct Range { VkDeviceSize mOffset; VkDeviceSize mSize; };
    WolfVkDevice* mDevice = nullptr;
    VkBuffer mBuffer = VK_NULL_HANDLE;
    WolfVkAllocation mAllocation;
    VkBuffer mStaging = VK_NULL_HANDLE;
    WolfVkAllocation mStagingAllocation;
    VkDeviceSize mSize = 0;
    bool mHostOnly = false;
    std::vector<Range> mPending;
};

// [STAGING RACE 2026-11-08] Per-frame transient staging memory: a host-visible buffer per frame in flight, bump
// allocated, reset when the frame slot is reused (after its fence), as WolfVkFrameUniforms does for uniforms.
class WolfVkStagingRing
{
public:
    WolfVkStagingRing() = default;
    ~WolfVkStagingRing();
    bool init(WolfVkDevice& device, VkDeviceSize bytes_per_frame, std::string& error);
    void destroy();
    void beginFrame(uint32_t frame_index);
    // Space for size bytes this frame; false when the frame's memory is used up (the caller falls back).
    bool allocate(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset, uint8_t*& host);
    // After writing host bytes of an allocation: flushes them when the memory is not host coherent.
    void flush(VkDeviceSize offset, VkDeviceSize size);
    VkDeviceSize used() const { return mUsed; }
    VkDeviceSize bytesPerFrame() const { return mBytes; }
private:
    WolfVkDevice* mDevice = nullptr;
    std::vector<std::unique_ptr<WolfVkBuffer>> mBuffers;
    VkDeviceSize mBytes = 0;
    VkDeviceSize mUsed = 0;
    uint32_t mFrame = 0;
    bool mWarnedFull = false;
};

class WolfVkVertexBuffer
{
public:
    // LLVertexBuffer::AttributeType (llvertexbuffer.h:103-120) repeated so this header needs no llvertexbuffer.h.
    enum AttributeType : uint32_t
    {
        TYPE_VERTEX = 0, TYPE_NORMAL, TYPE_TEXCOORD0, TYPE_TEXCOORD1, TYPE_TEXCOORD2, TYPE_TEXCOORD3, TYPE_COLOR, TYPE_EMISSIVE,
        TYPE_TANGENT, TYPE_WEIGHT, TYPE_WEIGHT4, TYPE_CLOTHWEIGHT, TYPE_JOINT, TYPE_TEXTURE_INDEX, TYPE_MAX
    };
    // LLVertexBuffer::sTypeSize (llvertexbuffer.cpp:702-717): bytes per vertex of each attribute array
    static const uint32_t sTypeSize[TYPE_MAX];
    // LLShaderMgr::mReservedAttribs names (llshadermgr.cpp:1353-1366), by AttributeType: the shader input names
    static const char* const sAttribName[TYPE_MAX];

    // The vertex input description of one attribute, as glVertexAttribPointer gave it (llvertexbuffer.cpp:1765-1865)
    struct AttribFormat
    {
        VkFormat mFormat;       // vulkan_core.h VkFormat
        uint32_t mSize;         // components
        bool mInteger;          // glVertexAttribIPointer (joint, texture_index)
    };
    static AttribFormat attribFormat(AttributeType type);

    WolfVkVertexBuffer() = default;
    ~WolfVkVertexBuffer();

    void init(WolfVkDevice& device, uint32_t typemask) { mDevice = &device; mTypeMask = typemask; }
    // LLVertexBuffer::allocateBuffer :1280-1293 (createGLBuffer/createGLIndices replaced by the Vulkan buffers)
    bool allocateBuffer(uint32_t nverts, uint32_t nindices, std::string& error);
    void destroy();

    // LLVertexBuffer::mapVertexBuffer :1316-1351 / mapIndexBuffer :1353-1389: a client pointer and a marked range
    uint8_t* mapVertexBuffer(AttributeType type, uint32_t index, int32_t count = -1);
    uint8_t* mapIndexBuffer(uint32_t index, int32_t count = -1);
    // LLVertexBuffer::unmapBuffer :1434 (_unmapBuffer :1448): the marked ranges go to the device
    void unmapBuffer();
    bool hasPendingUploads() const { return mVertices.hasPendingUploads() || mIndices.hasPendingUploads(); }
    void recordUploads(VkCommandBuffer cmd, WolfVkStagingRing* ring = nullptr);

    // LLVertexBuffer::setBuffer :1710 + setupVertexBuffer :1765: one binding per attribute in attrib_mask that this
    // buffer has; binding b is the b-th set bit, so the pipeline's vertex input (WolfVkPipeline) uses the same rule
    void bind(VkCommandBuffer cmd, uint32_t attrib_mask) const;
    // LLVertexBuffer::drawRange :931 (glDrawRangeElements): count indices from indices_offset (in indices), U16
    void drawRange(VkCommandBuffer cmd, uint32_t count, uint32_t indices_offset) const;
    // LLVertexBuffer::drawArrays :956
    void drawArrays(VkCommandBuffer cmd, uint32_t first, uint32_t count) const;

    uint32_t getNumVerts() const { return mNumVerts; }
    uint32_t getNumIndices() const { return mNumIndices; }
    uint32_t getTypeMask() const { return mTypeMask; }
    uint32_t getOffset(AttributeType type) const { return mOffsets[type]; }
    uint8_t* getMappedData() const { return mVertices.hostData(); }
    uint8_t* getMappedIndices() const { return mIndices.hostData(); }
    uint32_t getSize() const { return mSize; }
    uint32_t getIndicesSize() const { return mIndicesSize; }
    VkBuffer vertexBuffer() const { return mVertices.buffer(); }
    VkBuffer indexBuffer() const { return mIndices.buffer(); }
    WolfVkBuffer& vertices() { return mVertices; }
    WolfVkBuffer& indices() { return mIndices; }

    // LLVertexBuffer::calcOffsets :1062-1082
    static uint32_t calcOffsets(uint32_t typemask, uint32_t* offsets, uint32_t num_vertices);

private:
    struct MappedRegion { uint32_t mStart; uint32_t mEnd; };
    static bool expandRegion(MappedRegion& region, uint32_t start, uint32_t end);
    void flushRegions(std::vector<MappedRegion>& regions, WolfVkBuffer& buffer);

    WolfVkDevice* mDevice = nullptr;
    uint32_t mTypeMask = 0;
    uint32_t mNumVerts = 0;
    uint32_t mNumIndices = 0;
    uint32_t mSize = 0;
    uint32_t mIndicesSize = 0;
    uint32_t mOffsets[TYPE_MAX] = {};
    WolfVkBuffer mVertices;
    WolfVkBuffer mIndices;
    std::vector<MappedRegion> mMappedVertexRegions;
    std::vector<MappedRegion> mMappedIndexRegions;
};

#endif // WOLF_VK_BUFFER_H
