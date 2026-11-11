/**
 * @file wolfvkallocator.cpp
 * @brief Sub-allocating device memory allocator (see wolfvkallocator.h).
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
#include "wolfvkallocator.h"

namespace
{
    VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
    }

    uint32_t countBits(uint32_t v)
    {
        uint32_t n = 0;
        while (v) { n += v & 1u; v >>= 1; }
        return n;
    }
}

WolfVkAllocator::~WolfVkAllocator()
{
    destroy();
}

void WolfVkAllocator::init(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceProperties& props,
                           const VkPhysicalDeviceMemoryProperties& mem_props)
{
    mPhysical = physical;
    mDevice = device;
    mLimits = props.limits;
    mMemProps = mem_props;
    LL_INFOS("Vulkan") << "Allocator: maxMemoryAllocationCount " << mLimits.maxMemoryAllocationCount
                       << ", bufferImageGranularity " << mLimits.bufferImageGranularity
                       << ", nonCoherentAtomSize " << mLimits.nonCoherentAtomSize
                       << ", " << mMemProps.memoryTypeCount << " memory types, " << mMemProps.memoryHeapCount << " heaps" << LL_ENDL;
    for (uint32_t i = 0; i < mMemProps.memoryHeapCount; ++i)
    {
        // Source: vulkan_core.h:3516 VkMemoryHeap { size, flags }
        LL_INFOS("Vulkan") << "  heap " << i << ": " << (mMemProps.memoryHeaps[i].size >> 20) << " MiB"
                           << ((mMemProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "") << LL_ENDL;
    }
}

void WolfVkAllocator::destroy()
{
    std::lock_guard<std::mutex> lock(mMutex);
    for (Block& block : mBlocks)
    {
        if (block.mMemory != VK_NULL_HANDLE)
        {
            if (block.mLiveAllocations)
            {
                LL_WARNS("Vulkan") << "Allocator block " << block.mId << " destroyed with " << block.mLiveAllocations << " live allocations" << LL_ENDL;
            }
            releaseBlock(block);
        }
    }
    mBlocks.clear();
    mDevice = VK_NULL_HANDLE;
}

uint32_t WolfVkAllocator::findMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const
{
    uint32_t best = UINT32_MAX;
    uint32_t best_score = 0;
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i)
    {
        if (!(type_bits & (1u << i)))
        {
            continue;
        }
        // Source: vulkan_core.h:3521 VkMemoryType { propertyFlags, heapIndex }
        const VkMemoryPropertyFlags flags = mMemProps.memoryTypes[i].propertyFlags;
        if ((flags & required) != required)
        {
            continue;
        }
        // +1 so a type with no preferred bits still beats "none found"
        const uint32_t score = countBits(flags & preferred) + 1;
        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }
    return best;
}

bool WolfVkAllocator::newBlock(uint32_t memory_type, bool linear, VkDeviceSize min_size, Block*& out, std::string& error)
{
    // Block size: BLOCK_SIZE, but never more than an eighth of the heap (small integrated heaps), and at least
    // the requested size rounded up to the granularity.
    const uint32_t heap = mMemProps.memoryTypes[memory_type].heapIndex;
    VkDeviceSize size = BLOCK_SIZE;
    const VkDeviceSize heap_eighth = mMemProps.memoryHeaps[heap].size / 8;
    if (heap_eighth > 0 && size > heap_eighth)
    {
        size = heap_eighth;
    }
    if (size < min_size)
    {
        size = alignUp(min_size, mLimits.bufferImageGranularity);
    }

    if (mAllocationCount >= mLimits.maxMemoryAllocationCount)
    {
        error = llformat("maxMemoryAllocationCount (%u) reached", mLimits.maxMemoryAllocationCount);
        return false;
    }

    // Source: vulkan_core.h:212 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; VkMemoryAllocateInfo { sType, pNext, allocationSize, memoryTypeIndex }
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = size;
    info.memoryTypeIndex = memory_type;

    Block block;
    VkResult res = gVk.vkAllocateMemory(mDevice, &info, nullptr, &block.mMemory);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkAllocateMemory: ") + WolfVkLoader::resultString(res);
        return false;
    }
    mAllocationCount++;
    mBytesAllocated += size;

    block.mSize = size;
    block.mMemoryType = memory_type;
    block.mLinear = linear;
    block.mFree.push_back({ 0, size });

    if (mMemProps.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        // Source: vulkan_core.h:4553 PFN_vkMapMemory(device, memory, offset, size, flags, ppData); mapped for the block's life
        void* mapped = nullptr;
        res = gVk.vkMapMemory(mDevice, block.mMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkMapMemory: ") + WolfVkLoader::resultString(res);
            gVk.vkFreeMemory(mDevice, block.mMemory, nullptr);
            mAllocationCount--;
            mBytesAllocated -= size;
            return false;
        }
        block.mMapped = static_cast<uint8_t*>(mapped);
    }

    block.mId = mNextBlockId++;
    mBlocks.push_back(block);
    out = &mBlocks.back();
    LL_DEBUGS("Vulkan") << "Allocator: new " << (linear ? "linear" : "optimal") << " block " << block.mId << " of "
                        << (size >> 20) << " MiB in memory type " << memory_type << LL_ENDL;
    return true;
}

bool WolfVkAllocator::carve(Block& block, VkDeviceSize size, VkDeviceSize alignment, WolfVkAllocation& out)
{
    for (auto it = block.mFree.begin(); it != block.mFree.end(); ++it)
    {
        const VkDeviceSize start = alignUp(it->mOffset, alignment);
        const VkDeviceSize end = start + size;
        if (end > it->mOffset + it->mSize)
        {
            continue;
        }
        // split: [it->mOffset, start) stays free, [start, end) taken, [end, range end) stays free
        const VkDeviceSize range_end = it->mOffset + it->mSize;
        if (start > it->mOffset)
        {
            it->mSize = start - it->mOffset;
            if (end < range_end)
            {
                block.mFree.insert(std::next(it), { end, range_end - end });
            }
        }
        else if (end < range_end)
        {
            it->mOffset = end;
            it->mSize = range_end - end;
        }
        else
        {
            block.mFree.erase(it);
        }

        out.mMemory = block.mMemory;
        out.mOffset = start;
        out.mSize = size;
        out.mMapped = block.mMapped ? block.mMapped + start : nullptr;
        out.mMemoryType = block.mMemoryType;
        out.mBlock = block.mId;
        out.mDedicated = false;
        out.mCoherent = (mMemProps.memoryTypes[block.mMemoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        block.mLiveAllocations++;
        mBytesUsed += size;
        return true;
    }
    return false;
}

void WolfVkAllocator::giveBack(Block& block, VkDeviceSize offset, VkDeviceSize size)
{
    // insert keeping the list sorted, then coalesce with neighbours
    auto it = block.mFree.begin();
    while (it != block.mFree.end() && it->mOffset < offset)
    {
        ++it;
    }
    it = block.mFree.insert(it, { offset, size });
    if (it != block.mFree.begin())
    {
        auto prev = std::prev(it);
        if (prev->mOffset + prev->mSize == it->mOffset)
        {
            prev->mSize += it->mSize;
            block.mFree.erase(it);
            it = prev;
        }
    }
    auto next = std::next(it);
    if (next != block.mFree.end() && it->mOffset + it->mSize == next->mOffset)
    {
        it->mSize += next->mSize;
        block.mFree.erase(next);
    }
    block.mLiveAllocations--;
    mBytesUsed -= size;
}

void WolfVkAllocator::releaseBlock(Block& block)
{
    if (block.mMapped)
    {
        gVk.vkUnmapMemory(mDevice, block.mMemory);
        block.mMapped = nullptr;
    }
    gVk.vkFreeMemory(mDevice, block.mMemory, nullptr);
    block.mMemory = VK_NULL_HANDLE;
    mAllocationCount--;
    mBytesAllocated -= block.mSize;
    block.mFree.clear();
}

bool WolfVkAllocator::allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, bool linear, bool dedicated,
                               VkBuffer dedicated_buffer, VkImage dedicated_image, WolfVkAllocation& out, std::string& error,
                               VkExternalMemoryHandleTypeFlags export_types)
{
    std::lock_guard<std::mutex> lock(mMutex);
    out = WolfVkAllocation();
    if (export_types != 0)
    {
        dedicated = true;   // an exported image gets its own memory object: the whole object is what GL imports
    }

    uint32_t memory_type = findMemoryType(requirements.memoryTypeBits, required, preferred);
    if (memory_type == UINT32_MAX && (required & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
    {
        // a cached type is a wish for read-backs, not a need
        memory_type = findMemoryType(requirements.memoryTypeBits, required & ~static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_CACHED_BIT), preferred);
    }
    if (memory_type == UINT32_MAX)
    {
        error = llformat("no memory type with properties 0x%x in type bits 0x%x", required, requirements.memoryTypeBits);
        return false;
    }

    const VkDeviceSize heap_eighth = mMemProps.memoryHeaps[mMemProps.memoryTypes[memory_type].heapIndex].size / 8;
    const VkDeviceSize block_size = (heap_eighth > 0 && BLOCK_SIZE > heap_eighth) ? heap_eighth : BLOCK_SIZE;
    if (requirements.size >= block_size / 4)
    {
        dedicated = true;
    }

    if (dedicated)
    {
        if (mAllocationCount >= mLimits.maxMemoryAllocationCount)
        {
            error = llformat("maxMemoryAllocationCount (%u) reached", mLimits.maxMemoryAllocationCount);
            return false;
        }
        // Source: vulkan_core.h:259 VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; VkMemoryDedicatedAllocateInfo { sType, pNext, image, buffer }
        VkMemoryDedicatedAllocateInfo dedicated_info{};
        dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_info.image = dedicated_image;
        dedicated_info.buffer = dedicated_buffer;

        // Source: vulkan_core.h:5963 VkExportMemoryAllocateInfo { sType, pNext, handleTypes }; :294 sType. Chain: allocate -> export -> dedicated
        VkExportMemoryAllocateInfo export_info{};
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_info.handleTypes = export_types;
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        // the dedicated chain is Vulkan 1.1 (VK_KHR_dedicated_allocation, vulkan_core.h:11291); only when a resource is named
        const void* chain = (dedicated_image != VK_NULL_HANDLE || dedicated_buffer != VK_NULL_HANDLE) ? static_cast<const void*>(&dedicated_info) : nullptr;
        if (export_types != 0)
        {
            export_info.pNext = chain;
            chain = &export_info;
        }
        info.pNext = chain;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memory_type;

        VkResult res = gVk.vkAllocateMemory(mDevice, &info, nullptr, &out.mMemory);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkAllocateMemory (dedicated): ") + WolfVkLoader::resultString(res);
            out = WolfVkAllocation();
            return false;
        }
        mAllocationCount++;
        mBytesAllocated += requirements.size;
        mBytesUsed += requirements.size;
        out.mOffset = 0;
        out.mSize = requirements.size;
        out.mMemoryType = memory_type;
        out.mBlock = 0;
        out.mDedicated = true;
        out.mCoherent = (mMemProps.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (mMemProps.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        {
            void* mapped = nullptr;
            res = gVk.vkMapMemory(mDevice, out.mMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
            if (res != VK_SUCCESS)
            {
                error = std::string("vkMapMemory (dedicated): ") + WolfVkLoader::resultString(res);
                gVk.vkFreeMemory(mDevice, out.mMemory, nullptr);
                mAllocationCount--;
                mBytesAllocated -= requirements.size;
                mBytesUsed -= requirements.size;
                out = WolfVkAllocation();
                return false;
            }
            out.mMapped = static_cast<uint8_t*>(mapped);
        }
        return true;
    }

    // sub-allocate: an existing block of the same type and linearity first
    for (Block& block : mBlocks)
    {
        if (block.mMemory != VK_NULL_HANDLE && block.mMemoryType == memory_type && block.mLinear == linear)
        {
            if (carve(block, requirements.size, requirements.alignment, out))
            {
                return true;
            }
        }
    }
    Block* block = nullptr;
    if (!newBlock(memory_type, linear, requirements.size + requirements.alignment, block, error))
    {
        return false;
    }
    if (!carve(*block, requirements.size, requirements.alignment, out))
    {
        error = "fresh block could not hold the allocation";   // cannot happen: the block is at least size + alignment
        return false;
    }
    return true;
}

void WolfVkAllocator::free(WolfVkAllocation& allocation)
{
    if (!allocation.isValid())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    if (allocation.mDedicated)
    {
        if (allocation.mMapped)
        {
            gVk.vkUnmapMemory(mDevice, allocation.mMemory);
        }
        gVk.vkFreeMemory(mDevice, allocation.mMemory, nullptr);
        mAllocationCount--;
        mBytesAllocated -= allocation.mSize;
        mBytesUsed -= allocation.mSize;
    }
    else if (allocation.mBlock >= 1 && allocation.mBlock <= mBlocks.size())
    {
        Block& block = mBlocks[allocation.mBlock - 1];
        giveBack(block, allocation.mOffset, allocation.mSize);
        // an empty block goes back to the driver when it is not the only one of its kind (keeps one warm)
        if (block.mLiveAllocations == 0)
        {
            uint32_t same_kind = 0;
            for (const Block& other : mBlocks)
            {
                if (other.mMemory != VK_NULL_HANDLE && other.mMemoryType == block.mMemoryType && other.mLinear == block.mLinear)
                {
                    same_kind++;
                }
            }
            if (same_kind > 1)
            {
                releaseBlock(block);
            }
        }
    }
    else
    {
        LL_WARNS("Vulkan") << "Allocator: free of an allocation from unknown block " << allocation.mBlock << LL_ENDL;
    }
    allocation = WolfVkAllocation();
}

VkMappedMemoryRange WolfVkAllocator::mappedRange(const WolfVkAllocation& allocation, VkDeviceSize offset, VkDeviceSize size) const
{
    // Source: vulkan_core.h:213 VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; VkMappedMemoryRange { sType, pNext, memory, offset, size }.
    // Valid usage: offset a multiple of nonCoherentAtomSize (vulkan_core.h:3690); size a multiple of it, or VK_WHOLE_SIZE,
    // or reaching the end of the memory object.
    const VkDeviceSize atom = mLimits.nonCoherentAtomSize ? mLimits.nonCoherentAtomSize : 1;
    const VkDeviceSize begin = (allocation.mOffset + offset) / atom * atom;
    VkDeviceSize end = alignUp(allocation.mOffset + offset + size, atom);
    VkDeviceSize memory_size = allocation.mDedicated ? allocation.mSize : 0;
    if (!allocation.mDedicated && allocation.mBlock >= 1 && allocation.mBlock <= mBlocks.size())
    {
        memory_size = mBlocks[allocation.mBlock - 1].mSize;
    }
    if (memory_size && end > memory_size)
    {
        end = memory_size;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = allocation.mMemory;
    range.offset = begin;
    range.size = end - begin;
    return range;
}

void WolfVkAllocator::flush(const WolfVkAllocation& allocation, VkDeviceSize offset, VkDeviceSize size)
{
    if (!allocation.isValid() || allocation.mCoherent || !allocation.mMapped || size == 0)
    {
        return;
    }
    const VkMappedMemoryRange range = mappedRange(allocation, offset, size);
    VkResult res = gVk.vkFlushMappedMemoryRanges(mDevice, 1, &range);
    if (res != VK_SUCCESS)
    {
        LL_WARNS_ONCE("Vulkan") << "vkFlushMappedMemoryRanges: " << WolfVkLoader::resultString(res) << LL_ENDL;
    }
}

void WolfVkAllocator::invalidate(const WolfVkAllocation& allocation, VkDeviceSize offset, VkDeviceSize size)
{
    // [V14 2026-11-11] device writes (a copy into a read-back buffer) become visible to the host only through this on a
    // memory type without HOST_COHERENT: "Mapping non-coherent memory does not implicitly invalidate that memory", and
    // vkInvalidateMappedMemoryRanges "guarantees that device writes to the memory ranges ... are made visible to the host"
    // (vkInvalidateMappedMemoryRanges refpage). The same range rules as flush (VkMappedMemoryRange, nonCoherentAtomSize).
    if (!allocation.isValid() || allocation.mCoherent || !allocation.mMapped || size == 0)
    {
        return;
    }
    const VkMappedMemoryRange range = mappedRange(allocation, offset, size);
    // Source: vulkan_core.h:4556 PFN_vkInvalidateMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges)
    VkResult res = gVk.vkInvalidateMappedMemoryRanges(mDevice, 1, &range);
    if (res != VK_SUCCESS)
    {
        LL_WARNS_ONCE("Vulkan") << "vkInvalidateMappedMemoryRanges: " << WolfVkLoader::resultString(res) << LL_ENDL;
    }
}
