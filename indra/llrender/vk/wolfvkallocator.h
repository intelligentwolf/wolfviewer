/**
 * @file wolfvkallocator.h
 * @brief Sub-allocating device memory allocator over vkAllocateMemory.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * Why not one vkAllocateMemory per resource: VkPhysicalDeviceLimits::maxMemoryAllocationCount (vulkan_core.h:3594)
 * caps the number of live allocations (4096 on many drivers; the spec guarantees only 4096,
 * https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#limits-minmax "maxMemoryAllocationCount
 * 4096"), and a scene has tens of thousands of vertex buffers and textures. So memory comes in blocks of
 * BLOCK_SIZE (64 MiB, or an eighth of a small heap) and resources are carved out of them with a first-fit free list.
 *
 * Buffer-image granularity: the spec requires linear resources (buffers, linear-tiled images) and non-linear
 * resources (optimally tiled images) that share a memory "page" of VkPhysicalDeviceLimits::bufferImageGranularity
 * (vulkan_core.h:3596) to be kept apart
 * (https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#resources-bufferimagegranularity).
 * This allocator keeps linear and non-linear resources in SEPARATE blocks, so two resources of different kinds are
 * never adjacent and only VkMemoryRequirements::alignment (vulkan_core.h:3790) has to be honoured inside a block.
 * The granularity is still read and logged, and used to size the blocks.
 *
 * Dedicated allocations: a resource larger than a quarter of a block, or one the driver says prefers or requires
 * its own allocation (VkMemoryDedicatedRequirements, vulkan_core.h:5697, Vulkan 1.1), gets its own
 * vkAllocateMemory with VkMemoryDedicatedAllocateInfo chained (vulkan_core.h:259). Images shared with OpenGL must
 * always be dedicated (the vkglshare probe of 2026-11-08: RADV requires it); callers pass dedicated = true.
 *
 * Host-visible blocks are mapped once for their whole life (persistent mapping is allowed: a VkDeviceMemory can
 * stay mapped while in use, spec "Host Access to Device Memory Objects"). Writes to a memory type without
 * VK_MEMORY_PROPERTY_HOST_COHERENT_BIT (vulkan_core.h:2857) are flushed with vkFlushMappedMemoryRanges, offsets and
 * sizes rounded to nonCoherentAtomSize (vulkan_core.h:3690) as VkMappedMemoryRange's valid usage demands.
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

#ifndef WOLF_VK_ALLOCATOR_H
#define WOLF_VK_ALLOCATOR_H

#include "wolfvkloader.h"
#include <vector>
#include <list>
#include <mutex>
#include <string>

struct WolfVkAllocation
{
    VkDeviceMemory mMemory = VK_NULL_HANDLE;   // the block's (or dedicated) memory object
    VkDeviceSize   mOffset = 0;                // byte offset of this allocation inside mMemory
    VkDeviceSize   mSize = 0;                  // bytes granted (>= requested)
    uint8_t*       mMapped = nullptr;          // host pointer to mOffset, null when not host visible
    uint32_t       mMemoryType = 0;            // index into VkPhysicalDeviceMemoryProperties::memoryTypes
    uint32_t       mBlock = 0;                 // block id inside the allocator (0 = dedicated)
    bool           mDedicated = false;
    bool           mCoherent = true;           // memory type has HOST_COHERENT: no flush needed

    bool isValid() const { return mMemory != VK_NULL_HANDLE; }
};

class WolfVkAllocator
{
public:
    // 64 MiB blocks: large enough that a scene's buffers fit in a few, small enough to give memory back.
    static constexpr VkDeviceSize BLOCK_SIZE = 64u * 1024u * 1024u;

    WolfVkAllocator() = default;
    ~WolfVkAllocator();

    void init(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceProperties& props,
              const VkPhysicalDeviceMemoryProperties& mem_props);
    void destroy();   // frees every block; everything must have been returned

    // requirements: from vkGetBufferMemoryRequirements / vkGetImageMemoryRequirements (vulkan_core.h:4560/4561)
    // required / preferred: VK_MEMORY_PROPERTY_* bits (vulkan_core.h:2855-2858); a type with all "required" bits
    //                       and the most "preferred" bits wins, within requirements.memoryTypeBits
    // linear: true for buffers and linear-tiled images, false for optimally tiled images (block separation)
    // dedicated: force a dedicated allocation; dedicated_buffer / dedicated_image fill VkMemoryDedicatedAllocateInfo
    // export_types: [SHARED IMAGES 2026-11-08] VkExternalMemoryHandleTypeFlags (vulkan_core.h:5579-5580) when the memory will be
    //               exported to OpenGL; forces a dedicated allocation (RADV requires one, vkglshare probe) with
    //               VkExportMemoryAllocateInfo (:5963) chained. 0 for ordinary memory.
    bool allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                  VkMemoryPropertyFlags preferred, bool linear, bool dedicated,
                  VkBuffer dedicated_buffer, VkImage dedicated_image, WolfVkAllocation& out, std::string& error,
                  VkExternalMemoryHandleTypeFlags export_types = 0);

    void free(WolfVkAllocation& allocation);

    // Flushes a host write to a non-coherent allocation (no-op when coherent); offset/size relative to the allocation.
    void flush(const WolfVkAllocation& allocation, VkDeviceSize offset, VkDeviceSize size);

    // Finds a memory type: all required bits present, most preferred bits, allowed by type_bits. Returns UINT32_MAX if none.
    uint32_t findMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;

    // Statistics for the log and the renderer floater.
    VkDeviceSize bytesAllocated() const { return mBytesAllocated; }   // from the driver
    VkDeviceSize bytesUsed() const { return mBytesUsed; }             // handed out
    uint32_t allocationCount() const { return mAllocationCount; }     // live vkAllocateMemory objects
    uint32_t maxAllocationCount() const { return mLimits.maxMemoryAllocationCount; }
    VkDeviceSize bufferImageGranularity() const { return mLimits.bufferImageGranularity; }

private:
    struct FreeRange
    {
        VkDeviceSize mOffset;
        VkDeviceSize mSize;
    };
    struct Block
    {
        uint32_t mId = 0;
        VkDeviceMemory mMemory = VK_NULL_HANDLE;
        VkDeviceSize mSize = 0;
        uint32_t mMemoryType = 0;
        bool mLinear = true;
        uint8_t* mMapped = nullptr;
        std::list<FreeRange> mFree;     // sorted by offset, non-overlapping, coalesced
        uint32_t mLiveAllocations = 0;
    };

    bool newBlock(uint32_t memory_type, bool linear, VkDeviceSize min_size, Block*& out, std::string& error);
    bool carve(Block& block, VkDeviceSize size, VkDeviceSize alignment, WolfVkAllocation& out);
    void giveBack(Block& block, VkDeviceSize offset, VkDeviceSize size);
    void releaseBlock(Block& block);

    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits mLimits{};
    VkPhysicalDeviceMemoryProperties mMemProps{};
    std::vector<Block> mBlocks;       // index = id - 1
    uint32_t mNextBlockId = 1;
    VkDeviceSize mBytesAllocated = 0;
    VkDeviceSize mBytesUsed = 0;
    uint32_t mAllocationCount = 0;
    std::mutex mMutex;
};

#endif // WOLF_VK_ALLOCATOR_H
